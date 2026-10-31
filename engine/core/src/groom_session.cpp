// SPDX-License-Identifier: Apache-2.0
#include "groomlab/groom_session.h"
#include "groomlab/preview_core.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "groomlab/distance_field.h"
#include "groomlab/export_view.h"
#include "groomlab/flow.h"
#include "groomlab/follow_scalp.h"
#include "groomlab/frame_time.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/hair_color.h"
#include "groomlab/instances.h"
#include "groomlab/operators.h"
#include "groomlab/operator_state.h"
#include "groomlab/parts.h"
#include "groomlab/presets.h"
#include "groomlab/paths.h"
#include "groomlab/random.h"
#include "groomlab/scalp_paint.h"
#include "groomlab/subdivision.h"
#include "groomlab/strand_set.h"
#include "groomlab/texture.h"

namespace groomlab {

namespace {

// A double's bits, for a key (std::hash is the standard library's own).
std::uint64_t doubleBits(double d) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof bits);
    return bits;
}

bool blank(const std::string& text) { return text.find_first_not_of(" \t\r\n") == std::string::npos; }

// The first node of `type` making the output (Graph::forEachActiveUpstream: not bypassed, on
// a Switch's picked input, the output's first-input chain first), with its path; null without.
const Operator* activeNode(const GraphDocument& doc, const char* type, std::string* path) {
    const Operator* found = nullptr;
    doc.graph.forEachActiveUpstream(doc.output, [&](const std::string& at, const Operator& op) {
        if (op.typeName() != type) return true;
        found = &op;
        if (path) *path = at;
        return false;
    });
    return found;
}
const Operator* activeHairColor(const GraphDocument& doc, std::string* path) { return activeNode(doc, "HairColor", path); }

// Whether a node type colors the strands (HairColor, Bands, FeatherColor).
bool colorNode(const std::string& type) { return type == "HairColor" || type == "Bands" || type == "FeatherColor"; }

// The Blend nodes making the output that mix two colors (shapeAndLook, with other color nodes
// upstream of a than of b). Blends inside compounds are not looked at.
std::vector<std::string> colorBlendNodes(const GraphDocument& doc) {
    std::vector<std::string> blends, out;
    doc.graph.forEachActiveUpstream(doc.output, [&](const std::string& at, const Operator& op) {
        if (op.typeName() == "Blend" && op.params().getString("include") != "shape") blends.push_back(at);
        return true;
    });
    for (const std::string& at : blends) {
        const auto it = doc.nodesByName.find(at);
        if (it == doc.nodesByName.end()) continue;  // inside a compound
        auto colorsOf = [&](std::size_t slot) {
            std::set<std::string> found;
            NodeId source = 0;
            if (doc.graph.inputSource(it->second, slot, &source)) {
                doc.graph.forEachActiveUpstream(source, [&](const std::string& path, const Operator& op) {
                    if (colorNode(op.typeName())) found.insert(path);
                    return true;
                });
            }
            return found;
        };
        if (colorsOf(0) != colorsOf(1)) out.push_back(at);
    }
    return out;
}

// What to say when the output's strands hold a point that is not a finite number: the first node
// on the output's first-input chain whose cached strands do (the one making them), when found.
std::string nonFiniteMessage(const GraphDocument* doc) {
    std::string first;
    if (doc && doc->graph.hasNode(doc->output)) {
        NodeId id = doc->output;
        for (int guard = 0; guard < 4096 && id; ++guard) {
            const StrandsPtr strands = valueAs<StrandsPtr>(doc->graph.cachedValue(id));
            if (strands && !strands->extent().finite) first = doc->nameOf(id);
            NodeId source = 0;
            if (doc->graph.inputSpecs(id).empty() || !doc->graph.inputSource(id, 0, &source)) break;
            id = source;
        }
    }
    const std::string what = "the strands have points that are not numbers (NaN or infinite)";
    return first.empty() ? what + ": a node before the output made them" : "node '" + first + "' makes " + what;
}

}  // namespace

std::map<std::string, std::string> parseCardMaps(const std::string& maps) {
    std::map<std::string, std::string> entries;
    std::size_t start = 0;
    while (start < maps.size()) {
        const std::size_t end = std::min(maps.find(';', start), maps.size());
        const std::string entry = maps.substr(start, end - start);
        const std::size_t eq = entry.find('=');
        if (eq != std::string::npos) entries[entry.substr(0, eq)] = entry.substr(eq + 1);
        start = end + 1;
    }
    return entries;
}

std::string cardsBakeStamp(const std::vector<CardNodeSummary>& nodes) {
    std::string stamp;
    for (const CardNodeSummary& n : nodes) {
        stamp += (stamp.empty() ? "" : ",") + n.node + ":" + std::to_string(n.bakeHash);
    }
    return stamp;
}

std::map<std::string, std::string> parseCardsBakeStamp(const std::string& stamp) {
    std::map<std::string, std::string> out;
    std::size_t start = 0;
    while (start < stamp.size()) {
        const std::size_t end = std::min(stamp.find(',', start), stamp.size());
        const std::string entry = stamp.substr(start, end - start);
        const std::size_t colon = entry.rfind(':');
        if (colon != std::string::npos) out[entry.substr(0, colon)] = entry.substr(colon + 1);
        start = end + 1;
    }
    return out;
}

void useLiveWeights(Correctives& correctives, const std::function<float(int slot)>& weightOf) {
    for (CorrectiveLayer& l : correctives.layers) {
        if (l.slot < 0) continue;
        l.weight = weightOf(l.slot);
        l.holdBefore = l.holdAfter = true;
        l.weightCurve.clear();
    }
}

std::string GroomStats::toJson() const {
    nlohmann::json out;
    out["view"] = view;
    out["strands"] = strands;
    out["points"] = points;
    out["guides"] = guides;
    out["cards"] = cards;
    out["cardStrands"] = cardStrands;
    out["cardTriangles"] = cardTriangles;
    out["cardTextures"] = cardTextures;
    out["cardFeathers"] = cardFeathers;
    out["cardsBaked"] = cardsBaked;
    out["cardsStale"] = cardsStale;
    nlohmann::json nodes = nlohmann::json::object();
    for (const auto& [name, n] : cardNodes) {
        nodes[name] = {{"cards", n.cards}, {"triangles", n.triangles}, {"textures", n.textures},
                       {"feathers", n.feathers}, {"stale", n.stale}};
    }
    out["cardNodes"] = std::move(nodes);
    out["hairColor"] = hairColor;
    out["renderData"] = renderData;
    out["colorNodes"] = colorNodes;
    out["colorBlends"] = colorBlends;
    nlohmann::json t = nlohmann::json::object();
    for (const auto& [name, ms] : times) t[name] = ms;
    out["times"] = std::move(t);
    nlohmann::json r = nlohmann::json::object();
    for (const auto& [name, kind] : runs) r[name] = kind;
    out["runs"] = std::move(r);
    nlohmann::json c = nlohmann::json::object();
    for (const auto& [name, list] : channels) c[name] = list;
    out["channels"] = std::move(c);
    nlohmann::json n = nlohmann::json::object();
    for (const auto& [name, note] : notes) n[name] = note;
    out["notes"] = std::move(n);
    out["scalpHistory"] = {{"frames", scalpHistoryFrames}, {"reads", scalpHistoryReads}};
    out["instances"] = instances;
    out["restEvaluations"] = restEvaluations;
    out["distanceFieldsMade"] = distanceFieldsMade;
    out["cacheBytes"] = cacheBytes;
    out["cacheEvictions"] = cacheEvictions;
    out["strokeDocument"] = strokeDocument;
    nlohmann::json sims = nlohmann::json::object();
    for (const SimStatus& s : simulations) {
        nlohmann::json ranges = nlohmann::json::array();
        for (const auto& [first, last] : s.ranges) ranges.push_back({first, last});
        sims[s.node] = {{"startFrame", s.startFrame}, {"ranges", ranges}, {"frames", s.frames}, {"live", s.live},
                        {"baked", s.baked}, {"bakeStale", s.bakeStale}, {"bakedFirst", s.bakedFirst},
                        {"bakedLast", s.bakedLast}, {"bakedFile", s.bakedFile}, {"stepMs", s.stepMs}, {"bytes", s.bytes}};
    }
    out["simulations"] = std::move(sims);
    return out.dump();
}

// ---- Construction and stored inputs -------------------------------------------------------------

GroomSession::GroomSession() {
    parsed_ = parseScaled(data_.graphJson, &flat_);
    doc_ = std::make_unique<GraphDocument>(parsed_->clone());
    graphSet_ = true;
}

GroomSession::GroomSession(GroomData data) : data_(std::move(data)) {
    // Every text parses or the session is not made (the parsers' own messages).
    parsed_ = parseScaled(data_.graphJson, &flat_);  // as stored: its errors are its own
    doc_ = std::make_unique<GraphDocument>(parsed_->clone());
    graphSet_ = true;
    if (!blank(data_.hierarchyJson)) {
        rawHierarchy_ = std::make_shared<const GuideHierarchy>(GuideHierarchy::fromJson(data_.hierarchyJson));
        parsedHierarchy_ = rawHierarchy_;
    }
    refreshScale();  // the scalp's size: the graph and guides at it
    hierarchySet_ = true;
    hierarchyFromText_ = true;
    ++parsedVersion_;
    partSet_ = PartSet::fromJson(data_.partsJson);
    try {
        flowSet_ = FlowSet::fromJson(data_.flowJson);
    } catch (const std::invalid_argument& e) {
        throw std::runtime_error(e.what());
    }
    try {
        correctives_ = Correctives::fromJson(data_.correctivesJson);
    } catch (const std::invalid_argument& e) {
        throw std::runtime_error(e.what());
    }
    ++correctivesVersion_;
    try {
        sculpts_ = SculptSet::fromJson(data_.sculptsJson);
    } catch (const std::invalid_argument& e) {
        throw std::runtime_error(e.what());
    }
    for (const auto& [name, edits] : sculpts_.nodes) {
        sculptExternals_[name] = {std::make_shared<const SculptEdits>(edits), edits.contentHash()};
    }
    posesHash_ = data_.guidePoses ? data_.guidePoses->contentHash() : 0;
    time_ = data_.time;
}

GroomSession::~GroomSession() = default;

void GroomSession::setScalpFingerprint(std::uint64_t fingerprint) {
    if (fingerprint == data_.scalpFingerprint) return;
    data_.scalpFingerprint = fingerprint;
    fingerprintKey_ = 0;
}

namespace {

// A graph's text with its compounds as their nodes inline (GraphEdit::flattenCompounds); the text
// as it is when none flattens or it does not edit (it then loads, or fails to, as stored).
std::string flattened(const std::string& graphJson, GraphEdit::FlatCompounds* flat) {
    *flat = {};
    try {
        GraphEdit edit(graphJson);
        GraphEdit::FlatCompounds made = edit.flattenCompounds();
        if (made.output.empty()) return graphJson;
        std::string text = edit.toJson();
        loadGraphJson(text);  // (it loads, or the graph is taken as it is)
        *flat = std::move(made);
        return text;
    } catch (const std::exception&) {
        return graphJson;
    }
}

// Its compounds' entries as hosts know them (each compound one node, its nodes inline): each its
// nodes' times summed (a compound inside it counted by its nodes), their notes joined.
void foldCompoundTimes(const GraphEdit::FlatCompounds& flat, std::map<std::string, double>& times) {
    std::map<std::string, double> sums;
    for (const auto& [compound, output] : flat.output) {
        const std::string prefix = compound + "/";
        for (const auto& [name, ms] : times) {
            if (name.rfind(prefix, 0) == 0 && !flat.output.count(name)) sums[compound] += ms;
        }
    }
    for (const auto& [compound, ms] : sums) times[compound] = ms;
}
void foldCompoundNotes(const GraphEdit::FlatCompounds& flat, std::map<std::string, std::string>& notes) {
    std::map<std::string, std::string> joined;
    for (const auto& [compound, output] : flat.output) {
        const std::string prefix = compound + "/";
        for (const auto& [name, note] : notes) {
            if (name.rfind(prefix, 0) != 0 || flat.output.count(name)) continue;
            std::string& j = joined[compound];
            j += (j.empty() ? "" : "\n") + name.substr(prefix.size()) + ": " + note;
        }
    }
    for (const auto& [compound, note] : joined) notes[compound] = note;
}

}  // namespace

std::unique_ptr<GraphDocument> GroomSession::parseScaled(const std::string& graphJson, GraphEdit::FlatCompounds* flat) const {
    if (groomScale_ == 1.0) return std::make_unique<GraphDocument>(loadGraphJson(flattened(graphJson, flat)));
    loadGraphJson(graphJson);  // a graph that does not load says why as it is stored, not scaled
    return std::make_unique<GraphDocument>(loadGraphJson(flattened(scaleGraphJson(graphJson, groomScale_), flat)));
}

void GroomSession::setGraphJson(const std::string& graphJson) {
    if (graphSet_ && graphJson == data_.graphJson) return;
    GraphEdit::FlatCompounds flat;
    installGraph(parseScaled(graphJson, &flat));  // throws before anything changes
    flat_ = std::move(flat);
    controlFactors_ = controlScaleFactors(graphJson, groomScale_);
    data_.graphJson = graphJson;
    graphSet_ = true;
    graphError_.clear();
}

void GroomSession::installGraph(std::unique_ptr<GraphDocument> parsed) {
    // Each document is a clone of the one parse (no cached results), taking its old one's cache.
    auto adopt = [&](std::unique_ptr<GraphDocument>& old) {
        auto doc = std::make_unique<GraphDocument>(parsed->clone());
        doc->graph.setCacheIntermediates(cacheIntermediates_);
        if (old) doc->graph.adoptCache(old->graph);
        old = std::move(doc);
    };
    std::unique_ptr<RestPose> rest = restPose_ ? std::make_unique<RestPose>(parsed->clone(), restPose_.get()) : nullptr;
    adopt(doc_);
    ++docGeneration_;
    ++graphGeneration_;
    if (sculptDoc_) adopt(sculptDoc_);
    ++strokeDocGeneration_;
    warmEvaluations_ = warmIncrementals_ = releasedEvaluations_ = releasedIncrementals_ = 0;  // counted since the graph was set
    // (What was shown stays: the nodes the new graph takes on from the old keep their results'
    // serials, Graph::adoptCache, so the strands a host holds go on incrementally across a param
    // edit, a Width edit's changes saying the points stayed. Another node's serial never matches.)
    if (cardDoc_) adopt(cardDoc_);
    if (fullDoc_) adopt(fullDoc_);
    for (auto& [density, doc] : meshDocs_) adopt(doc);
    restPose_ = std::move(rest);
    parsed_ = std::move(parsed);
}

// ---- Scaling with the scalp -----------------------------------------------------------------------

void GroomSession::setScaleSource(ScaleSource source) {
    if (source == data_.scaleSource) return;
    data_.scaleSource = source;
    refreshScale();
}

void GroomSession::setScaleWithScalp(bool on) {
    if (on == scaleWithScalp()) return;
    setScaleSource(on ? ScaleSource::Area : ScaleSource::Off);
}

void GroomSession::setScaleTransform(double scale) {
    if (!(scale > 0.0) || !std::isfinite(scale)) scale = 0.0;
    if (scale == scaleTransform_) return;
    scaleTransform_ = scale;
    if (firstTransform_ <= 0.0) firstTransform_ = scale;
    // Surface Area: taken with the scalp handed in next (setScalp), so a scaled transform and the
    // area it gives arrive together (scaleNotice tells the two apart).
    if (data_.scaleSource == ScaleSource::Transform) refreshScale();
}

void GroomSession::setScaleTransformReference(double scale) {
    if (!(scale > 0.0) || !std::isfinite(scale)) scale = 0.0;
    if (scale == data_.scaleTransformReference) return;
    data_.scaleTransformReference = scale;
    refreshScale();
}

void GroomSession::setScaleReference(double area) {
    if (!(area > 0.0) || !std::isfinite(area)) area = 0.0;
    if (area == data_.scaleReference) return;
    data_.scaleReference = area;
    refreshScale();
}

void GroomSession::refreshScale() {
    // The rest pose's area (the scalp's own without one): the same on every frame of a deforming
    // scalp, and for a transform's scale and the same scale frozen into the points.
    scaleArea_ = data_.scalp ? restSurfaceArea(*data_.scalp) : 0.0;
    if (!(scaleArea_ > 0.0) || !std::isfinite(scaleArea_)) scaleArea_ = 0.0;
    if (firstArea_ <= 0.0) firstArea_ = scaleArea_;
    const double reference = scaleReference();
    const bool areaKnown = scaleArea_ > 0.0 && reference > 0.0;
    double k = 1.0;
    switch (data_.scaleSource) {
        case ScaleSource::Off: break;
        case ScaleSource::Area:
            if (!areaKnown) return;  // no scalp (for now): k as it was
            k = std::sqrt(scaleArea_ / reference);
            break;
        case ScaleSource::Transform:
            if (scaleTransform_ > 0.0) {
                const double from = scaleTransformReference();
                if (!(from > 0.0)) return;
                k = scaleTransform_ / from;
            } else {
                // A host without transforms: k as the area gives it on the first measure, then kept
                // (a remodel, another scalp leave it).
                if (scaleMeasured_ || !areaKnown) return;
                k = std::sqrt(scaleArea_ / reference);
            }
            break;
    }
    if (!std::isfinite(k) || k <= 0.0) k = 1.0;
    if (std::abs(k - 1.0) < 1e-4) k = 1.0;
    // What k was worked out from before: k changing with the area while the transform scale stands
    // is the scalp's own size changing (an edit, a remodel, another scalp), said once (scaleNotice).
    const double areaBefore = measuredArea_, transformBefore = measuredTransform_, kBefore = groomScale_;
    const bool measuredBefore = scaleMeasured_;
    if (data_.scaleSource != ScaleSource::Off) {
        measuredArea_ = scaleArea_;
        measuredTransform_ = scaleTransform_;
        scaleMeasured_ = true;
    }
    if (std::abs(k / groomScale_ - 1.0) < 1e-4) return;  // the same k, or a moving scalp's float noise
    if (data_.scaleSource == ScaleSource::Area && measuredBefore && areaBefore > 0.0 &&
        std::abs(scaleArea_ / areaBefore - 1.0) > 1e-4 &&
        std::abs(scaleTransform_ - transformBefore) <= 1e-4 * std::max(std::abs(transformBefore), 1e-12)) {
        char line[200];
        std::snprintf(line, sizeof line, "GroomLab: the scalp's area changed %.0f%%: the groom scaled x%.2f. Set Scale Reference to keep its sizes.",
                      std::abs(scaleArea_ / areaBefore - 1.0) * 100.0, k / kBefore);
        scaleNotice_ = line;
        ++scaleNoticeSerial_;
    }
    groomScale_ = k;
    controlFactors_ = controlScaleFactors(data_.graphJson, k);
    if (parsed_ && graphError_.empty()) {
        try {
            GraphEdit::FlatCompounds flat;
            installGraph(parseScaled(data_.graphJson, &flat));
            flat_ = std::move(flat);
        } catch (const std::exception&) {
            // It loaded at the last k: the scaled text loads too (only numbers change).
        }
    }
    scaleHierarchy();
}

void GroomSession::scaleHierarchy() {
    std::shared_ptr<const GuideHierarchy> scaled = rawHierarchy_;
    if (rawHierarchy_ && rawHierarchy_->scale() != static_cast<float>(groomScale_)) {
        auto copy = std::make_shared<GuideHierarchy>(*rawHierarchy_);
        copy->setScale(static_cast<float>(groomScale_));
        scaled = std::move(copy);
    }
    if (scaled == parsedHierarchy_) return;
    parsedHierarchy_ = std::move(scaled);
    ++parsedVersion_;  // rooted again (and everything rooted on it)
}

std::map<std::string, float> GroomSession::scaledControls() const {
    if (controlFactors_.empty()) return data_.controls;
    std::map<std::string, float> out = data_.controls;
    for (auto& [name, value] : out) {
        const auto f = controlFactors_.find(name);
        if (f != controlFactors_.end()) value = static_cast<float>(value * f->second);
    }
    return out;
}

void GroomSession::dropGraph(const std::string& graphJson, const std::string& error) {
    parsed_.reset();
    doc_.reset();
    ++docGeneration_;
    ++graphGeneration_;
    sculptDoc_.reset();
    ++strokeDocGeneration_;
    warmEvaluations_ = warmIncrementals_ = releasedEvaluations_ = releasedIncrementals_ = 0;
    shown_[0] = shown_[1] = Shown{};
    cardDoc_.reset();
    fullDoc_.reset();
    meshDocs_.clear();
    meshMemos_.clear();
    restPose_.reset();
    data_.graphJson = graphJson;
    graphSet_ = true;
    graphError_ = error;
}

void GroomSession::setHierarchyJson(const std::string& json) {
    if (hierarchySet_ && hierarchyFromText_ && json == data_.hierarchyJson) return;
    data_.hierarchyJson = json;
    hierarchySet_ = true;
    hierarchyFromText_ = true;
    hierarchyError_.clear();
    try {
        rawHierarchy_ = std::make_shared<const GuideHierarchy>(GuideHierarchy::fromJson(json));
    } catch (const std::exception& e) {
        rawHierarchy_.reset();
        hierarchyError_ = std::string("guideData: ") + e.what();
    }
    parsedHierarchy_ = rawHierarchy_;
    scaleHierarchy();
    ++parsedVersion_;
}

void GroomSession::setHierarchy(std::shared_ptr<const GuideHierarchy> hierarchy) {
    if (hierarchySet_ && !hierarchyFromText_ && hierarchy == rawHierarchy_) return;
    data_.hierarchyJson.clear();
    hierarchySet_ = true;
    hierarchyFromText_ = false;
    hierarchyError_.clear();
    rawHierarchy_ = std::move(hierarchy);
    parsedHierarchy_ = rawHierarchy_;
    scaleHierarchy();
    ++parsedVersion_;
}

void GroomSession::setPartsJson(const std::string& json) {
    if (json == data_.partsJson) return;
    data_.partsJson = json;
    partError_.clear();
    try {
        partSet_ = PartSet::fromJson(json);
    } catch (const std::exception& e) {
        partSet_ = PartSet{};
        partError_ = std::string("partData: ") + e.what();
    }
}

void GroomSession::setFlowJson(const std::string& json) {
    if (json == data_.flowJson) return;
    data_.flowJson = json;
    flowError_.clear();
    try {
        flowSet_ = FlowSet::fromJson(json);
    } catch (const std::exception& e) {
        flowSet_ = FlowSet{};
        flowError_ = std::string("flowData: ") + e.what();
    }
}

void GroomSession::setCorrectivesJson(const std::string& json) {
    if (json == data_.correctivesJson) return;
    data_.correctivesJson = json;
    correctiveError_.clear();
    try {
        correctives_ = Correctives::fromJson(json);
    } catch (const std::exception& e) {
        correctives_ = Correctives{};
        correctiveError_ = std::string("correctiveData: ") + e.what();
    }
    ++correctivesVersion_;
}

void GroomSession::setSculptsJson(const std::string& json) {
    if (json == data_.sculptsJson) return;
    data_.sculptsJson = json;
    sculptError_.clear();
    try {
        sculpts_ = SculptSet::fromJson(json);
    } catch (const std::exception& e) {
        sculpts_ = SculptSet{};
        sculptError_ = e.what();  // "sculptData: <why>"
    }
    ++sculptsVersion_;
    // Each node's edits shared again only where they changed (the others keep their pointer).
    std::map<std::string, std::pair<SculptPtr, std::uint64_t>> externals;
    for (const auto& [name, edits] : sculpts_.nodes) {
        const std::uint64_t hash = edits.contentHash();
        const auto it = sculptExternals_.find(name);
        if (it != sculptExternals_.end() && it->second.second == hash && *it->second.first == edits) {
            externals[name] = it->second;
        } else {
            externals[name] = {std::make_shared<const SculptEdits>(edits), hash};
        }
    }
    sculptExternals_ = std::move(externals);
}

void GroomSession::setCorrectiveWeights(std::map<int, float> weights) {
    if (liveWeights_ && weights == weights_) return;
    liveWeights_ = true;
    weights_ = std::move(weights);
}

void GroomSession::clearCorrectiveWeights() {
    liveWeights_ = false;
    weights_.clear();
}

std::vector<std::string> GroomSession::inputErrors() const {
    std::vector<std::string> out;
    for (const std::string* e : {&hierarchyError_, &graphError_, &partError_, &flowError_, &correctiveError_, &scalpMapsError_,
                                 &sculptError_}) {
        if (!e->empty()) out.push_back(*e);
    }
    return out;
}

// ---- Scalp and maps -------------------------------------------------------------------------

void GroomSession::setScalp(MeshPtr scalp) {
    if (scalp == data_.scalp || (scalp && data_.scalp && scalp->contentHash() == data_.scalp->contentHash())) {
        absorbScaleTransform();
        return;  // the same scalp: keep the old one, with its closest-point tree built
    }
    const bool sameSize = scalp && data_.scalp && scalp->restTotalArea() == data_.scalp->restTotalArea();
    data_.scalp = std::move(scalp);
    // (A deforming scalp's next frame keeps its rest pose: nothing to do. Its points moved with the
    // rest standing, a rig's scale say, whose rest the host sizes next: the transform is not taken
    // as measured with the old area.)
    if (!sameSize) refreshScale();
}

void GroomSession::absorbScaleTransform() {
    if (scaleMeasured_) measuredTransform_ = scaleTransform_;
}

void GroomSession::updateScalpPoints(std::vector<Vec3f> points, const std::vector<Vec3f>* restPoints) {
    if (!data_.scalp) throw std::runtime_error("updateScalpPoints: the session has no scalp");
    if (points.size() != data_.scalp->pointCount()) {
        throw std::invalid_argument("updateScalpPoints: " + std::to_string(points.size()) + " points for a scalp of " +
                                    std::to_string(data_.scalp->pointCount()));
    }
    const bool newRest = restPoints && *restPoints != data_.scalp->restPoints();
    if (points == data_.scalp->points() && !newRest) return;
    auto moved = std::make_shared<ScalpMesh>(data_.scalp->withPoints(std::move(points)));
    if (newRest) moved->setRestPoints(*restPoints);  // throws for another count
    setScalp(std::move(moved));
}

void GroomSession::setScalps(NamedScalps scalps) {
    scalps.erase(std::remove_if(scalps.begin(), scalps.end(), [](const auto& s) { return !s.second; }), scalps.end());
    if (scalps.size() == data_.scalps.size()) {
        bool same = true;
        for (std::size_t i = 0; same && i < scalps.size(); ++i) {
            const auto& [name, mesh] = scalps[i];
            const auto& [oldName, oldMesh] = data_.scalps[i];
            same = name == oldName && (mesh == oldMesh || mesh->contentHash() == oldMesh->contentHash());
        }
        if (same) return;  // the same scalps: the old ones kept, with what was built on them
    }
    data_.scalps = std::move(scalps);
}

MeshPtr GroomSession::joinedScalp() {
    if (data_.scalps.empty() || !data_.scalp) {
        joined_.reset();
        joinedMain_.reset();
        joinedExtras_.clear();
        return data_.scalp;
    }
    if (joined_ && joinedMain_ == data_.scalp && joinedExtras_ == data_.scalps) return joined_;
    // Only points moved (a deforming scalp's next frame): the joined mesh's points move.
    bool moved = joined_ && joinedMain_ && joinedExtras_.size() == data_.scalps.size() &&
                 data_.scalp->samePointsLayout(*joinedMain_);
    for (std::size_t i = 0; moved && i < data_.scalps.size(); ++i) {
        moved = data_.scalps[i].first == joinedExtras_[i].first &&
                data_.scalps[i].second->samePointsLayout(*joinedExtras_[i].second);
    }
    if (moved) {
        std::vector<Vec3f> points(data_.scalp->points());
        points.reserve(joined_->pointCount());
        for (const auto& [name, mesh] : data_.scalps) {
            if (joined_->piece(name)) points.insert(points.end(), mesh->points().begin(), mesh->points().end());
        }
        moved = points.size() == joined_->pointCount();
        if (moved) joined_ = std::make_shared<const ScalpMesh>(joined_->withPoints(std::move(points)));
    }
    if (!moved) joined_ = combineScalps(data_.scalp, data_.scalps);
    joinedMain_ = data_.scalp;
    joinedExtras_ = data_.scalps;
    return joined_;
}

ScalpHistoryPtr GroomSession::historyJoined() {
    const MeshPtr joined = joinedScalp();
    if (!data_.scalpHistory || !joined || !data_.scalp || joined == data_.scalp) return data_.scalpHistory;
    if (data_.scalpHistory != joinedHistoryIn_ || joined != joinedHistoryOn_) {
        // Frames of every scalp's points (the host read each one's own earlier frames) are the
        // joined scalp's as they are; a frame of the groom's own scalp alone gets the other
        // scalps' points now after it (they keep still).
        const std::size_t own = data_.scalp->pointCount(), whole = joined->pointCount();
        const ScalpHistory& in = *data_.scalpHistory;
        bool joinedFrames = true;
        for (const std::vector<Vec3f>& f : in.frames) joinedFrames = joinedFrames && (f.empty() || f.size() == whole);
        if (joinedFrames) {
            joinedHistory_ = data_.scalpHistory;
        } else {
            std::vector<std::vector<Vec3f>> frames;
            for (const std::vector<Vec3f>& f : in.frames) {
                if (f.size() == whole) {
                    frames.push_back(f);
                } else if (f.size() == own) {
                    std::vector<Vec3f> all(f);
                    all.insert(all.end(), joined->points().begin() + static_cast<std::ptrdiff_t>(own), joined->points().end());
                    frames.push_back(std::move(all));
                } else {
                    frames.emplace_back();
                }
            }
            joinedHistory_ = ScalpHistory::make(in.step, std::move(frames), in.loopSeconds);
        }
        joinedHistoryIn_ = data_.scalpHistory;
        joinedHistoryOn_ = joined;
    }
    return joinedHistory_;
}

void GroomSession::setScalpMapsJson(const std::string& json) {
    if (scalpMapsSet_ && scalpMapsFromText_ && json == scalpMapsText_) return;
    scalpMapsText_ = json;
    scalpMapsSet_ = true;
    scalpMapsFromText_ = true;
    scalpMapsError_.clear();
    try {
        scalpMaps_ = ScalpMaps::fromJson(json);
    } catch (const std::exception& e) {
        scalpMaps_ = ScalpMaps{};
        scalpMapsError_ = e.what();
    }
    scalpMapsHash_ = scalpMaps_.hash();
    data_.maps = scalpMaps_.catalog();
}

void GroomSession::setScalpMaps(ScalpMaps maps) {
    const std::uint64_t hash = maps.hash();
    if (scalpMapsSet_ && !scalpMapsFromText_ && hash == scalpMapsHash_) return;
    scalpMapsText_.clear();
    scalpMapsSet_ = true;
    scalpMapsFromText_ = false;
    scalpMapsError_.clear();
    scalpMaps_ = std::move(maps);
    scalpMapsHash_ = hash;
    data_.maps = scalpMaps_.catalog();
}

void GroomSession::setMapCatalog(MapCatalog maps) {
    if (scalpMapsSet_) return;  // the scalp maps' own catalog
    data_.maps = std::move(maps);
}

MeshPtr GroomSession::scalp() {
    std::vector<std::string> errors;
    bindScalp(errors);
    return scalp_;
}

MeshPtr GroomSession::cageScalp() {
    std::vector<std::string> errors;
    bindScalp(errors);
    return cageScalp_;
}

std::map<std::string, int> GroomSession::scalpSubdivisions() const {
    const int matchRender = data_.scalp ? data_.scalp->renderSubdivision() : 0;
    // The other scalps' render levels by name (the first of a name, as combineScalps joins them).
    std::map<std::string, int> renders;
    for (const auto& [name, mesh] : data_.scalps) {
        if (mesh && validScalpName(name)) renders.emplace(name, mesh->renderSubdivision());
    }
    // Each Scalp node asks for the scalp it reads (match render: that scalp's render level); one
    // naming no other scalp asks for the groom's own.
    int own = -1;
    std::map<std::string, int> asked;
    if (parsed_) {
        for (const NodeId id : parsed_->graph.nodeIds()) {
            const Operator& op = parsed_->graph.node(id);
            if (op.typeName() != op::kScalp) continue;
            const std::string param = op.params().getString("subdivision");
            const auto it = renders.find(op.params().getString("input"));
            if (it == renders.end()) {
                own = std::max(own, subdivisionLevels(param, matchRender));
            } else {
                const auto [at, added] = asked.emplace(it->first, -1);
                at->second = std::max(at->second, subdivisionLevels(param, it->second));
            }
        }
    }
    if (own < 0) own = subdivisionLevels("matchRender", matchRender);
    std::map<std::string, int> out{{kMainScalpName, own}};
    for (const auto& [name, render] : renders) {
        const auto it = asked.find(name);
        out[name] = it == asked.end() ? own : it->second;  // no Scalp node reads it: the groom's own scalp's
    }
    return out;
}

int GroomSession::scalpSubdivision() const {
    int levels = 0;
    for (const auto& [name, l] : scalpSubdivisions()) levels = std::max(levels, l);
    return levels;
}

int GroomSession::scalpLevelsOn(const MeshPtr& joined, MeshPtr* levelled) const {
    if (levelled) *levelled = joined;
    if (!joined) return 0;
    if (joined->pieces().empty()) return scalpSubdivision();
    const std::map<std::string, int> byScalp = scalpSubdivisions();
    int levels = 0;
    for (const auto& [name, l] : byScalp) levels = std::max(levels, l);
    if (levelled) *levelled = withScalpLevels(joined, byScalp, levels);
    return levels;
}

MeshPtr GroomSession::refineAsScalp(const MeshPtr& joined) const {
    MeshPtr cage;
    const int levels = scalpLevelsOn(joined, &cage);
    return subdivideScalp(cage, levels);
}

void GroomSession::bindScalp(std::vector<std::string>& errors) {
    if (!scalpMapsError_.empty()) errors.push_back(scalpMapsError_);
    // A scalp of another topology than the groom was made on (GroomData::scalpFingerprint): the
    // guides' faces and the vertex maps' vertices are not where they were. Worked out once per
    // topology (a deforming scalp's frames keep it). The groom's own scalp: other scalps joined
    // after it leave what is rooted on it where it was.
    if (const MeshPtr& own = data_.scalp; own && data_.scalpFingerprint) {
        const std::uint64_t key =
            hashCombine(hashCombine(hash64(0xF1A6), own->topologyHash()), static_cast<std::uint64_t>(own->pointCount()));
        if (key != fingerprintKey_) {
            fingerprintKey_ = key;
            fingerprintDiffers_ = own->topologyFingerprint() != data_.scalpFingerprint;
        }
        if (fingerprintDiffers_) errors.push_back(kScalpTopologyChanged);
    }
    const MeshPtr raw = joinedScalp();  // the scalp itself, or joined with the groom's other scalps
    // Several scalps: each refined at its own level (scalpSubdivisions), set on the cage's pieces.
    MeshPtr levelled;
    const int levels = scalpLevelsOn(raw, &levelled);
    const std::uint64_t levelsKey = levelled && levelled != raw ? levelled->piecesHash() : 0;
    const std::uint64_t key = hashCombine(hashCombine(hashCombine(raw ? raw->contentHash() : 0, scalpMapsHash_),
                                                      previewMapValues_ ? previewMapVersion_ : 0),
                                          levelsKey);
    if (key != mappedKey_ || !cageScalp_) {
        const std::uint64_t mapsKey = hashCombine(hashCombine(scalpMapsHash_, previewMapValues_ ? previewMapVersion_ : 0), levelsKey);
        if (raw && cageScalp_ && mappedFrom_ && cageScalp_ != mappedFrom_ && mapsKey == mappedMapsKey_ &&
            raw->samePointsLayout(*mappedFrom_)) {
            // The same maps on the scalp's next frame (only its points moved): the mapped scalp's
            // color sets stay; its points move (the closest-point tree refits). The same mesh a
            // fresh laying of the maps gives.
            cageScalp_ = std::make_shared<const ScalpMesh>(cageScalp_->withPoints(raw->points()));
        } else {
            mapErrors_.clear();
            cageScalp_ = withVertexMaps(levelled, scalpMaps_, previewMapName_, previewMapValues_.get(), &mapErrors_,
                                        previewMapChannels_);
        }
        mappedKey_ = key;
        mappedMapsKey_ = mapsKey;
        mappedFrom_ = raw;
    }
    errors.insert(errors.end(), mapErrors_.begin(), mapErrors_.end());
    // Catmull-Clark (subdivision.h): the mapped cage refined, its color sets (the vertex maps)
    // with it. The scalp's next frame moves the last refined mesh's points (the plan is kept).
    if (levels == 0 || !cageScalp_) {
        scalp_ = cageScalp_;
        refinedFrom_.reset();
    } else if (cageScalp_ != refinedFrom_ || !scalp_ || scalp_->subdivisionLevel() != levels) {
        scalp_ = subdivideScalp(cageScalp_, levels, refinedFrom_, scalp_ && scalp_ != cageScalp_ ? scalp_ : nullptr);
        refinedFrom_ = cageScalp_;
    }
}

void GroomSession::bindRefined() {
    const bool refined = scalp_ && scalp_->subdivisionLevel() > 0;
    // Flat guides (rooted by the host on the scalp it has): rooted again on the refined scalp.
    if (!refined || !data_.guides) {
        guides_ = data_.guides;
        guidesIn_.reset();
    } else if (data_.guides != guidesIn_ || scalp_->surfaceHash() != guidesOn_) {
        auto rooted = std::make_shared<GuideSet>(*data_.guides);
        rooted->rootOnMesh(*scalp_);
        guides_ = rooted;
        guidesIn_ = data_.guides;
        guidesOn_ = scalp_->surfaceHash();
    }
    // The scalp's earlier frames (Jiggle) on the refined scalp's vertices (the other scalps' after).
    const ScalpHistoryPtr scalpHistory = historyJoined();
    if (!refined || !scalpHistory) {
        history_ = scalpHistory;
        historyIn_.reset();
    } else if (scalpHistory != historyIn_ || cageScalp_ != historyOn_) {
        const auto plan = SubdivisionPlan::of(*cageScalp_, scalp_->subdivisionLevel());
        std::vector<std::vector<Vec3f>> frames;
        const ScalpHistory& in = *scalpHistory;
        for (const std::vector<Vec3f>& f : in.frames) {
            frames.push_back(f.size() == plan->cagePointCount() ? plan->refinePoints(f) : std::vector<Vec3f>{});
        }
        // Jiggle reads the frames that fit (an empty one: no more).
        history_ = ScalpHistory::make(in.step, std::move(frames), in.loopSeconds);
        historyIn_ = scalpHistory;
        historyOn_ = cageScalp_;
    }
}

// ---- Moving inputs --------------------------------------------------------------------------

void GroomSession::setGuides(GuidesPtr guides) {
    if (guides == data_.guides) return;
    data_.guides = std::move(guides);
}

void GroomSession::setGuidePoses(GuidesPtr poses) {
    if (poses == data_.guidePoses) return;
    // The same poses (the same guides read again): the posed hierarchy stays. Hashed once each.
    const std::uint64_t hash = poses ? poses->contentHash() : 0;
    if (poses && data_.guidePoses && hash == posesHash_) return;
    data_.guidePoses = std::move(poses);
    posesHash_ = hash;
}

void GroomSession::setControls(std::map<std::string, float> controls) {
    if (controls == data_.controls) return;
    data_.controls = std::move(controls);
}

void GroomSession::setMeshes(std::map<std::string, MeshPtr> meshes) {
    for (auto it = meshes.begin(); it != meshes.end();) it = it->second ? std::next(it) : meshes.erase(it);
    if (meshes == data_.meshes) return;
    data_.meshes = std::move(meshes);
}

void GroomSession::setMesh(const std::string& name, MeshPtr mesh) {
    if (mesh) {
        data_.meshes[name] = std::move(mesh);
    } else {
        data_.meshes.erase(name);
    }
}

void GroomSession::setScalpHistory(ScalpHistoryPtr history) { data_.scalpHistory = std::move(history); }

void GroomSession::setPathResolver(PathResolver resolver) {
    paths_ = resolver ? std::make_shared<const PathResolver>(std::move(resolver)) : nullptr;
}

void GroomSession::setPathResolver(std::shared_ptr<const PathResolver> resolver) { paths_ = std::move(resolver); }

const PathResolver* GroomSession::paths() const { return paths_ && *paths_ ? paths_.get() : nullptr; }

void GroomSession::setCardMaps(const std::string& cardMaps) {
    if (cardMaps == cardMapsText_) return;
    cardMapsText_ = cardMaps;
    cardMaps_ = parseCardMaps(cardMaps);
}

void GroomSession::setFollowScalp(bool on) { data_.followScalp = on; }

bool GroomSession::readsTime() const {
    if (doc_ && graphReadsTime(doc_->graph, doc_->output)) return true;
    if (data_.scalpHistory) return true;
    for (const CorrectiveLayer& l : correctives_.layers) {
        if (!l.enabled || l.offsets.empty()) continue;
        if (l.slot >= 0 && liveWeights_) continue;  // the host's live weight (keyed or not: the host knows)
        if (!l.weightCurve.empty()) {
            for (const auto& [seconds, w] : l.weightCurve) {
                if (w != l.weightCurve.front().second) return true;
            }
            continue;
        }
        if (!(l.holdBefore && l.holdAfter)) return true;  // it fades in or out
    }
    return false;
}

void GroomSession::setCacheIntermediates(bool enabled) {
    cacheIntermediates_ = enabled;
    for (auto* doc : {&doc_, &sculptDoc_, &cardDoc_, &fullDoc_}) {
        if (*doc) (*doc)->graph.setCacheIntermediates(enabled);
    }
    for (auto& [density, doc] : meshDocs_) doc->graph.setCacheIntermediates(enabled);
    if (restPose_) restPose_->setCacheIntermediates(enabled);
}

// ---- Previews -------------------------------------------------------------------------------

void GroomSession::setPreviewHierarchy(std::shared_ptr<GuideHierarchy> preview, bool final) {
    previewHierarchy_ = std::move(preview);
    previewFinal_ = previewHierarchy_ && final;
}

void GroomSession::setPreviewOverlays(std::shared_ptr<const PartSet> parts, std::shared_ptr<const FlowSet> flow) {
    previewParts_ = std::move(parts);
    previewFlow_ = std::move(flow);
}

void GroomSession::setPreviewVertexMap(const std::string& name, std::shared_ptr<const std::vector<float>> values, int channels) {
    previewMapName_ = name;
    previewMapValues_ = std::move(values);
    previewMapChannels_ = channels == 4 ? 4 : 1;
    ++previewMapVersion_;
}

void GroomSession::setPreviewTextureMap(const std::string& name, std::shared_ptr<const TexturePreview> image) {
    previewMapName_ = name;
    previewMapImage_ = std::move(image);
    ++previewMapVersion_;
}

void GroomSession::setSculptTarget(const std::string& node) {
    if (node == sculptTarget_) return;
    sculptTarget_ = node;
    ++sculptsVersion_;
    sculptShown_.reset();
    sculptShownFrom_.reset();
    sculptShownOn_.reset();
}

void GroomSession::setPreviewSculpt(const std::string& node, SculptPtr edits, std::shared_ptr<const GuideHierarchy> shown) {
    previewSculpt_ = std::move(edits);
    previewSculptNode_ = previewSculpt_ ? node : std::string();
    previewSculptHash_ = previewSculpt_ ? previewSculpt_->contentHash() : 0;
    previewSculptShown_ = previewSculpt_ ? std::move(shown) : nullptr;
}

SculptInput GroomSession::sculptInput(const std::string& node) {
    if (!doc_) throw std::runtime_error(graphError_.empty() ? "the groom has no graph" : graphError_);
    const auto found = doc_->nodesByName.find(node);
    if (found == doc_->nodesByName.end()) throw std::runtime_error("the graph has no node named '" + node + "'");
    if (doc_->graph.node(found->second).typeName() != op::kSculpt) {
        throw std::runtime_error("node '" + node + "' is a " + doc_->graph.node(found->second).typeName() + ", not a Sculpt node");
    }
    // A scratch copy of the view's document at the last view's settings: what the view made is
    // reused, and nothing it holds is evicted.
    GraphDocument scratch = parsed_->clone();
    scratch.graph.adoptCache(doc_->graph);
    const NodeId id = scratch.node(node);
    ExternalInputs externals;
    fillExternals(externals);
    EvalContext ctx = lastViewContext_;
    ctx.framesPerSecond = fps_;
    ctx.scale = groomScale_;  // the lattices sizes round to, at the groom's scale
    ctx.externals = &externals;
    ctx.paths = paths();
    SculptInput in;
    in.node = node;
    NodeId source = 0;
    std::size_t output = 0;
    if (!scratch.graph.inputSource(id, 0, &source, &output)) throw std::runtime_error("Sculpt node '" + node + "' is fed no guides");
    in.guides = valueAs<GuidesPtr>(scratch.graph.evaluateOutput(source, output, ctx));
    if (!in.guides) throw std::runtime_error("Sculpt node '" + node + "' is fed no guides");
    if (scratch.graph.inputSource(id, 1, &source, &output)) in.scalp = valueAs<MeshPtr>(scratch.graph.evaluateOutput(source, output, ctx));
    in.strength = static_cast<float>(doc_->graph.node(found->second).params().getDouble("strength"));
    if (const SculptEdits* stored = sculpts_.node(node)) in.edits = *stored;
    return in;
}

GuidesPtr GroomSession::sculptOutput(GraphDocument& doc, const EvalContext& ctx) {
    const auto found = doc.nodesByName.find(sculptTarget_);
    if (found == doc.nodesByName.end() || doc.graph.node(found->second).typeName() != op::kSculpt) return nullptr;
    try {
        return valueAs<GuidesPtr>(doc.graph.evaluate(found->second, ctx));
    } catch (const std::exception&) {
        return nullptr;  // the evaluation's own errors say why
    }
}

HierarchyPtr GroomSession::sculptHierarchy() {
    if (sculptTarget_.empty()) return nullptr;
    if (previewSculptShown_ && previewSculptNode_ == sculptTarget_) return previewSculptShown_;
    if (sculptShownVersion_ != sculptsVersion_) {  // not looked for since the target or edits changed
        ViewRequest request;
        request.density = lastViewContext_.previewDensity;
        request.pointBudget = lastViewContext_.maxPoints;
        request.time = lastViewContext_.time;
        request.viewport = lastViewContext_.viewport;
        evaluate(request);
    }
    return sculptShown_;
}

bool GroomSession::setLiveStroke(bool on) {
    if (on == liveStroke_) return false;
    liveStroke_ = on;
    if (on) {
        strokeDensityUsed_ = false;
        return false;
    }
    const bool again = strokeDensityUsed_;  // something evaluated at the stroke's density
    strokeDensityUsed_ = false;
    return again;
}

// ---- Guides ---------------------------------------------------------------------------------

HierarchyPtr GroomSession::rootedHierarchy() {
    // On the scalp as given (refined when it is subdivided): painting a vertex map changes the
    // mapped scalp's color sets, never where the guides sit, so a dab does not root them again.
    MeshPtr on = joinedScalp();
    if (on && scalpSubdivision() > 0) {
        std::vector<std::string> ignored;  // said by the evaluation
        bindScalp(ignored);
        on = scalp_;
    }
    const std::uint64_t scalpKey = on ? on->surfaceHash() : 0;
    if (rooted_ && rootedVersion_ == parsedVersion_ && scalpKey == rootedScalp_) return hierarchy_;
    rooted_ = true;
    rootedVersion_ = parsedVersion_;
    rootedScalp_ = scalpKey;
    hierarchy_.reset();
    if (parsedHierarchy_) {
        std::shared_ptr<const GuideHierarchy> source = parsedHierarchy_;
        // Saved bound to another topology (the scalp re-meshed or subdivided since): evaluating
        // that on each frame's scalp re-roots every guide at the point nearest its saved world
        // shape, so on a moving scalp the guides stay where they were groomed while the scalp
        // moves on. Bound again once on the scalp's rest pose (where the saved shapes belong)
        // and kept, the guides follow the scalp.
        const std::uint64_t bound = source->boundTopology();
        if (on && on->hasRest() && bound != 0 && bound != on->topologyHash()) {
            const std::uint64_t restKey = hashCombine(on->restPoseHash(), on->topologyHash());
            if (reboundFrom_ != source || reboundRest_ != restKey) {
                auto rebound = std::make_shared<GuideHierarchy>(*source);
                const ScalpMesh rest = on->restPose();
                rebound->evaluate(&rest);
                reboundHierarchy_ = std::move(rebound);
                reboundFrom_ = source;
                reboundRest_ = restKey;
            }
            source = reboundHierarchy_;
        }
        auto rooted = std::make_shared<GuideHierarchy>(*source);
        rooted->evaluate(on.get());
        hierarchy_ = rooted;
    }
    return hierarchy_;
}

void GroomSession::updateActiveHierarchy() {
    const HierarchyPtr base = previewHierarchy_ ? HierarchyPtr(previewHierarchy_) : rootedHierarchy();
    if (previewFinal_) {  // shot sculpting: the preview already has the poses and correctives
        activeHierarchy_ = base;
        activeValid_ = false;
        return;
    }
    const GuidesPtr& poses = data_.guidePoses;
    const bool posing = poses && base;
    const bool correcting = base && !correctives_.empty();
    // The same base, poses, correctives (weights, time) and scalp: the same active hierarchy. A
    // preview is edited in place between evaluations: always new.
    std::uint64_t key = hashCombine(hash64(0xAC7E), reinterpret_cast<std::uintptr_t>(base.get()));
    key = hashCombine(key, rootedVersion_);
    key = hashCombine(key, scalp_ ? scalp_->surfaceHash() : 0);
    key = hashCombine(key, posing ? posesHash_ : 0);
    if (correcting) {
        key = hashCombine(hashCombine(key, correctivesVersion_), static_cast<std::uint64_t>(liveWeights_));
        for (const auto& [slot, w] : weights_) key = hashFloat(hashCombine(key, static_cast<std::uint64_t>(slot)), w);
        key = hashCombine(key, doubleBits(time_));
    }
    if (!previewHierarchy_ && activeValid_ && key == activeKey_) return;
    activeKey_ = key;
    activeValid_ = true;
    posedHierarchy_ = base;  // nothing posed: as authored
    activeHierarchy_ = base;
    if (!posing && !correcting) return;
    // Poses, then correctives, on one copy (the stored one is never touched).
    auto working = std::make_shared<GuideHierarchy>(*base);
    if (posing) {
        applyGuidePoses(*working, *poses, scalp_.get());
        // Both: the posed-only hierarchy is made when asked for (posedHierarchy()).
        posedHierarchy_ = correcting ? nullptr : HierarchyPtr(working);
    }
    if (correcting) {
        if (liveWeights_) {
            // Layers with a live weight take its value on this frame.
            Correctives live = correctives_;
            useLiveWeights(live, [&](int slot) {
                const auto it = weights_.find(slot);
                return it == weights_.end() ? 1.0f : it->second;
            });
            live.apply(*working, time_, scalp_.get());
        } else {
            correctives_.apply(*working, time_, scalp_.get());
        }
    }
    activeHierarchy_ = working;
}

HierarchyPtr GroomSession::hierarchy() {
    if (!parsedHierarchy_ || (hierarchyFromText_ && blank(data_.hierarchyJson) && !previewHierarchy_)) return nullptr;
    std::vector<std::string> errors;
    bindScalp(errors);
    updateActiveHierarchy();
    return activeHierarchy_;
}

HierarchyPtr GroomSession::posedHierarchy() {
    if (!posedHierarchy_ && data_.guidePoses) {
        // Poses and correctives both applied: the evaluation kept one copy (corrected); the
        // posed-only hierarchy is made here, when a shot sculpt is recorded.
        const HierarchyPtr base = previewHierarchy_ ? HierarchyPtr(previewHierarchy_) : hierarchy_;
        if (base) {
            auto posed = std::make_shared<GuideHierarchy>(*base);
            applyGuidePoses(*posed, *data_.guidePoses, scalp_.get());
            posedHierarchy_ = posed;
        }
    }
    return posedHierarchy_;
}

// ---- Parts and flow -------------------------------------------------------------------------

void GroomSession::bindOverlays() {
    // The stored parts and flow are bound to the cage (whatever the subdivision level, so a new
    // level leaves them where they were drawn); a refined scalp gets them carried onto it, each
    // point to the same place of the surface (SubdivisionPlan::refinedRoot). A stroke's preview is
    // on the scalp as evaluated already.
    const std::shared_ptr<const SubdivisionPlan> plan =
        scalp_ && cageScalp_ && scalp_ != cageScalp_ ? refinementPlan(*cageScalp_, *scalp_) : nullptr;
    const std::uint64_t planKey =
        plan ? hashCombine(hashCombine(hashCombine(hashCombine(hash64(0xCA6E), cageScalp_->topologyHash()), cageScalp_->polygonHash()),
                                       static_cast<std::uint64_t>(plan->levels())),
                           cageScalp_->piecesHash())
             : 0;
    shownParts_ = &partSet_;
    if (previewParts_) {
        shownParts_ = previewParts_.get();
    } else if (plan && !partSet_.empty()) {
        const std::uint64_t key = hashCombine(partSet_.contentHash(), planKey);
        if (key != refinedPartsKey_) {
            refinedParts_ = partSet_.onRefined(*plan);
            refinedPartsKey_ = key;
        }
        shownParts_ = &refinedParts_;
    }
    shownFlow_ = &flowSet_;
    if (previewFlow_) {
        shownFlow_ = previewFlow_.get();
    } else if (plan && !flowSet_.empty()) {
        // Directions turned on the rest pose where there is one: the same on every frame.
        const std::uint64_t key = hashCombine(hashCombine(flowSet_.contentHash(), planKey), cageScalp_->restSurfaceHash());
        if (key != refinedFlowKey_) {
            if (cageScalp_->hasRest()) {
                refinedFlow_ = flowSet_.onRefined(*plan, cageScalp_->restPose(), scalp_->restPose());
            } else {
                refinedFlow_ = flowSet_.onRefined(*plan, *cageScalp_, *scalp_);
            }
            refinedFlowKey_ = key;
        }
        shownFlow_ = &refinedFlow_;
    }

    const std::uint64_t scalpHash = scalp_ ? scalp_->surfaceHash() : 0;  // color sets play no part
    const PartSet& activeParts = *shownParts_;
    const std::uint64_t partsKey = hashCombine(activeParts.contentHash(), scalpHash);
    if (partsKey != partsKey_ || (!parts_ && !activeParts.empty() && scalp_) || (parts_ && (activeParts.empty() || !scalp_))) {
        parts_ = !activeParts.empty() && scalp_ ? std::make_shared<const PartLines>(activeParts, *scalp_) : nullptr;
        partsKey_ = partsKey;
    }
    const FlowSet& activeFlow = *shownFlow_;
    const std::uint64_t flowKey = hashCombine(activeFlow.contentHash(), scalpHash);
    if (flowKey != flowKey_ || (!flow_ && !activeFlow.empty() && scalp_) || (flow_ && (activeFlow.empty() || !scalp_))) {
        flow_ = !activeFlow.empty() && scalp_ ? std::make_shared<const FlowField>(activeFlow, *scalp_) : nullptr;
        flowKey_ = flowKey;
    }
}

void GroomSession::prepareTime(double seconds, std::vector<std::string>& errors, std::vector<SessionMessage>& messages) {
    bindScalp(errors);
    bindRefined();
    rootedHierarchy();
    time_ = seconds;
    updateActiveHierarchy();
    bindOverlays();
    prepareFollow(messages);  // on the rest pose while following the scalp
}

void GroomSession::fillExternals(ExternalInputs& externals) const {
    // Each Simulate node's frame (in rest space while following), "sim:<node>".
    for (const auto& [name, node] : sims_) {
        if (node->shown) externals.set(simulationExternal(name), node->shown, node->shownHash);
    }
    // Each Sculpt node's edits, "sculpt:<node>": a live stroke's in place of its node's stored ones.
    for (const auto& [name, edits] : sculptExternals_) {
        if (!previewSculpt_ || name != previewSculptNode_) externals.set(sculptExternal(name), edits.first, edits.second);
    }
    if (previewSculpt_) externals.set(sculptExternal(previewSculptNode_), previewSculpt_, previewSculptHash_);
    if (following_) {
        // On the rest pose: the scalp, guides, parts and flow there (prepareFollow).
        setGroomExternals(externals, followRest_, followGuides_, followActive_, followParts_, followFlow_);
    } else {
        setGroomExternals(externals, scalp_, guides_, activeHierarchy_, parts_, flow_);
    }
    setMeshExternals(externals, data_.meshes);          // "mesh:<name>" for ColliderMesh nodes
    setControlExternals(externals, scaledControls());  // keyed lengths at the groom's scale
    setMapExternals(externals, data_.maps);             // where each painted map lives, for Map nodes
    // The scalp's earlier frames, for Jiggle nodes (in rest space while following).
    setScalpHistoryExternal(externals, following_ ? followHistory_ : history_);
}

void GroomSession::prepareFollow(std::vector<SessionMessage>& messages) {
    following_ = false;
    if (!data_.followScalp || !scalp_) return;
    if (!scalp_->hasRest()) {
        if (!followNoRestSaid_) {
            messages.push_back({"followScalp: the scalp has no rest pose (give the groom a rest mesh); the groom evaluates "
                                "on the scalp as it is", false});
            followNoRestSaid_ = true;
        }
        return;
    }
    followNoRestSaid_ = false;
    if (scalp_->points() == scalp_->restPoints()) return;  // at rest: as it is, to the bit
    following_ = true;

    // The scalp at rest (its maps as color sets), the same on every frame.
    const std::uint64_t restKey = scalp_->restPoseHash();
    if (!followRest_ || restKey != followRestKey_) {
        followRest_ = std::make_shared<const ScalpMesh>(scalp_->restPose());
        followRestKey_ = restKey;
    }
    const std::uint64_t restSurface = followRest_->surfaceHash();

    // The stored guides rooted on it.
    const std::uint64_t rootedKey = hashCombine(hashCombine(hash64(0xF0110), parsedVersion_), restSurface);
    if (followRootedKey_ != rootedKey || (parsedHierarchy_ && !followRooted_)) {
        followRooted_.reset();
        if (parsedHierarchy_) {
            auto rooted = std::make_shared<GuideHierarchy>(*parsedHierarchy_);
            rooted->evaluate(followRest_.get());
            followRooted_ = rooted;
        }
        followRootedKey_ = rootedKey;
    }

    // The active hierarchy at rest: the preview (on the scalp now) taken back, or the rooted
    // guides; then the poses (world space now, taken back by their guides' roots) and the
    // correctives, as updateActiveHierarchy does on the scalp now.
    const bool previewing = previewHierarchy_ != nullptr;
    {
        std::uint64_t key = hashCombine(hash64(0xF011AC), reinterpret_cast<std::uintptr_t>(followRooted_.get()));
        key = hashCombine(hashCombine(key, followRootedKey_), scalp_->surfaceHash());
        const GuidesPtr& poses = data_.guidePoses;
        key = hashCombine(key, poses ? posesHash_ : 0);
        const bool correcting = !correctives_.empty();
        if (correcting) {
            key = hashCombine(hashCombine(key, correctivesVersion_), static_cast<std::uint64_t>(liveWeights_));
            for (const auto& [slot, w] : weights_) key = hashFloat(hashCombine(key, static_cast<std::uint64_t>(slot)), w);
            key = hashCombine(key, doubleBits(time_));
        }
        if (previewing || key != followActiveKey_) {
            followActiveKey_ = key;
            HierarchyPtr base = followRooted_;
            if (previewing) base = carryHierarchyBack(*previewHierarchy_, *followRest_, *scalp_);
            if (previewFinal_ || !base) {
                followActive_ = base;  // shot sculpting: the preview has its poses and correctives
            } else if (!(poses && base) && !correcting) {
                followActive_ = base;
            } else {
                auto working = std::make_shared<GuideHierarchy>(*base);
                if (poses) {
                    const GuidesPtr atRest = carryPosesBack(poses, *base, *followRest_, *scalp_);
                    applyGuidePoses(*working, *atRest, followRest_.get());
                }
                if (correcting) {
                    if (liveWeights_) {
                        Correctives live = correctives_;
                        useLiveWeights(live, [&](int slot) {
                            const auto it = weights_.find(slot);
                            return it == weights_.end() ? 1.0f : it->second;
                        });
                        live.apply(*working, time_, followRest_.get());
                    } else {
                        correctives_.apply(*working, time_, followRest_.get());
                    }
                }
                followActive_ = working;
            }
        }
    }

    // Parts and flow bound to the rest pose.
    const PartSet& activeParts = *shownParts_;  // bound to scalp_, so to its rest pose too
    const std::uint64_t partsKey = hashCombine(activeParts.contentHash(), restSurface);
    if (partsKey != followPartsKey_ || (!followParts_ && !activeParts.empty()) || (followParts_ && activeParts.empty())) {
        followParts_ = activeParts.empty() ? nullptr : std::make_shared<const PartLines>(activeParts, *followRest_);
        followPartsKey_ = partsKey;
    }
    const FlowSet& activeFlow = *shownFlow_;
    const std::uint64_t flowKey = hashCombine(activeFlow.contentHash(), restSurface);
    if (flowKey != followFlowKey_ || (!followFlow_ && !activeFlow.empty()) || (followFlow_ && activeFlow.empty())) {
        followFlow_ = activeFlow.empty() ? nullptr : std::make_shared<const FlowField>(activeFlow, *followRest_);
        followFlowKey_ = flowKey;
    }

    // Flat guides (world space now) taken back to rest by their roots.
    if (guides_ != followGuidesIn_ || scalp_ != followGuidesOn_ || !guides_) {
        followGuides_ = guides_ ? carryGuidesBack(guides_, *followRest_, *scalp_) : nullptr;
        followGuidesIn_ = guides_;
        followGuidesOn_ = scalp_;
    }
    // The scalp's earlier frames, turned into rest space.
    if (history_ != followHistoryIn_ || scalp_ != followHistoryOn_) {
        followHistory_ = history_ ? historyAtRest(*history_, *followRest_, *scalp_) : nullptr;
        followHistoryIn_ = history_;
        followHistoryOn_ = scalp_;
    }
}

const StrandBoxes& GroomSession::followBoxes(const StrandsPtr& rest) {
    if (!followBoxes_ || followBoxesOf_.lock() != rest || followBoxesFrom_ != followRest_) {
        followBoxes_ = strandBoxes(*rest, *followRest_);
        followBoxesOf_ = rest;
        followBoxesFrom_ = followRest_;
    }
    return *followBoxes_;
}

StrandsPtr GroomSession::carry(const StrandsPtr& strands, int slot) {
    if (!following_ || !strands) return strands;
    Carried& c = carried_[slot];
    if (c.out && c.in == strands && c.from == followRest_ && c.to == scalp_) return c.out;
    c.in = strands;
    c.from = followRest_;
    c.to = scalp_;
    c.out = carryStrands(strands, *followRest_, *scalp_);
    return c.out;
}

std::shared_ptr<const std::vector<Vec3f>> GroomSession::clumpCenterDots(GraphDocument& doc, NodeId node, const StrandsPtr& clumped,
                                                                         const EvalContext& ctx) {
    auto dots = std::make_shared<std::vector<Vec3f>>();
    // Where a root is on the scalp now (the strands' roots are on its faces, following it or not).
    auto at = [&](const StrandRoot& root, const Vec3f& fallback) {
        return scalp_ && scalp_->contains(root) ? scalp_->position(root) : fallback;
    };
    const Operator& op = doc.graph.node(node);
    GuidesPtr centers;
    StrandsPtr centerStrands;
    NodeId source = 0;
    std::size_t output = 0;
    // The results as the nodes made them (a preview's hidden core strands in them: what the Clump
    // read and its kept state names, preview_core.h).
    (void)clumped;
    const std::string from = op.params().getString("centersFrom");
    if (from != "pick") {  // the centers input (guides or strands), when one is linked
        if (!doc.graph.inputSource(node, 1, &source, &output)) return dots;
        doc.graph.evaluateOutput(source, output, ctx);
        const Value value = doc.graph.cachedValue(source, output);
        centers = valueAs<GuidesPtr>(value);
        centerStrands = valueAs<StrandsPtr>(value);
    } else {
        // A Clump picking its own centers: the strands they are (their roots), else the centers'
        // curves as it kept them.
        ClumpState state;
        const StrandsPtr made = valueAs<StrandsPtr>(doc.graph.cachedValue(node));
        if (!made || !clumpState(op, made, &state)) return dots;
        if (doc.graph.inputSource(node, 0, &source, &output)) doc.graph.evaluateOutput(source, output, ctx);
        const StrandsPtr in = doc.graph.inputSource(node, 0, &source, &output)
                                  ? valueAs<StrandsPtr>(doc.graph.cachedValue(source, output)) : nullptr;
        if (in && !state.chosen.empty()) {
            for (const std::size_t s : state.chosen) {
                if (s < in->strandCount() && in->strandPointCount(s) > 0) dots->push_back(at(in->roots()[s], in->strandPoints(s)[0]));
            }
        } else {
            for (std::size_t c = 0; c * kClumpCenterSamples < state.curves.size(); ++c) dots->push_back(state.curves[c * kClumpCenterSamples]);
        }
        return dots;
    }
    // What Clump follows: the centers of 2 points or more, or with centersFrom inputPoints any (their roots).
    const std::size_t least = from == "inputPoints" ? 1 : 2;
    if (centers) {
        for (const Guide& g : centers->guides()) {
            if (g.points.size() >= least) dots->push_back(at(g.root, g.points.front()));
        }
    } else if (centerStrands) {
        const PreviewCore core(*centerStrands);  // the strands Clump takes as centers: their core
        for (std::size_t s = 0; s < centerStrands->strandCount(); ++s) {
            if (core.core(s) && centerStrands->strandPointCount(s) >= least) {
                dots->push_back(at(centerStrands->roots()[s], centerStrands->strandPoints(s)[0]));
            }
        }
    }
    return dots;
}

// ---- Evaluation -----------------------------------------------------------------------------

const ViewResult& GroomSession::evaluate(const ViewRequest& request) {
    std::vector<std::string> errors;
    std::vector<SessionMessage> messages;
    failure_ = nullptr;
    if (parsedVersion_ != 0 && reportedVersion_ != parsedVersion_) {
        reportedVersion_ = parsedVersion_;
        if (!hierarchyError_.empty()) messages.push_back({hierarchyError_, true});
    }

    const HierarchyPtr previousActive = activeHierarchy_;
    const PartsPtr previousParts = parts_;
    const FlowPtr previousFlow = flow_;
    prepareTime(request.time, errors, messages);
    messages.insert(messages.begin(), detachedMessages_.begin(), detachedMessages_.end());
    detachedMessages_.clear();
    // The Simulate nodes' frames at this time (stepping on where they are live): frames on the
    // way are fed by the host, and the inputs bound at this time again after.
    stepSimulations(request, errors);
    // A live brush stroke edits the preview in place: every stamp is a change.
    bool hierarchyChanged = activeHierarchy_.get() != previousActive.get() || previewHierarchy_ != nullptr;
    const HierarchyPtr previousShown = view_.hierarchy;
    if (!correctiveError_.empty()) errors.push_back(correctiveError_);
    if (!partError_.empty()) errors.push_back(partError_);
    if (!flowError_.empty()) errors.push_back(flowError_);

    ExternalInputs externals;
    fillExternals(externals);
    EvalContext ctx;
    ctx.previewDensity = request.density;
    // While a brush stroke is live, fewer strands keep the viewport responsive, evaluated on a
    // document of their own (the view's cache stays for the release).
    if (hasPreview()) ctx.previewDensity *= request.sculptDensity;
    if (liveStroke_) strokeDensityUsed_ = true;
    const bool sculpting = hasPreview() && ctx.previewDensity != request.density && doc_;
    if (sculpting && !sculptDoc_) {
        sculptDoc_ = std::make_unique<GraphDocument>(parsed_->clone());
        sculptDoc_->graph.setCacheIntermediates(cacheIntermediates_);
    }
    viewSculpt_ = sculpting;
    GraphDocument* const viewed = sculpting ? sculptDoc_.get() : doc_.get();
    ctx.maxPoints = request.pointBudget;
    ctx.time = request.time;
    ctx.framesPerSecond = fps_;
    ctx.scale = groomScale_;  // the lattices sizes round to, at the groom's scale
    ctx.viewport = request.viewport;
    ctx.liveDrag = liveDrag_;
    ctx.externals = &externals;
    ctx.paths = paths();
    ctx.buildCards = request.buildCards;
    lastViewContext_ = ctx;
    lastViewContext_.previewDensity = request.density;  // the view's (a stroke's density aside)
    lastViewContext_.externals = nullptr;
    lastViewContext_.paths = nullptr;

    ViewResult& out = view_;
    out.density = std::clamp(ctx.previewDensity, 0.0f, 1.0f);
    const StrandsPtr previousStrands = out.strands;
    const std::uint64_t previousSerial = out.strandsSerial;
    const std::shared_ptr<const CardMesh> previousCards = out.cards;
    const std::uint64_t previousStrandMeshes = out.versions.strandMeshes;
    const std::string previousView = out.viewedNode;
    const bool previousRoots = out.viewedRoots;
    const std::shared_ptr<const std::vector<Vec3f>> previousCenters = out.clumpCenters;
    const FollowCarryPtr previousFollow = out.follow;
    out.strands.reset();
    out.follow.reset();
    NodeId stopAt = 0;
    if (viewed && !request.stopAt.empty()) {
        const auto it = viewed->nodesByName.find(flat_.result(request.stopAt));
        if (it != viewed->nodesByName.end()) stopAt = it->second;
    }
    if (stopAt) {
        // The host runs this node and what follows: what feeds it, and the strands as they were.
        try {
            const std::size_t slots = viewed->graph.inputSpecs(stopAt).size();
            for (std::size_t slot = 0; slot < slots; ++slot) {
                NodeId source = 0;
                std::size_t output = 0;
                if (viewed->graph.inputSource(stopAt, slot, &source, &output)) viewed->graph.evaluateOutput(source, output, ctx);
            }
            out.strands = previousStrands;
            out.follow = previousFollow;
        } catch (const std::exception& e) {
            failure_ = std::current_exception();
            errors.push_back(e.what());
            messages.push_back({e.what(), true});
        }
    } else if (viewed) {
        try {
            // The node to show (a graph editor's "view this node"), else the output.
            NodeId shown = viewed->output;
            out.viewedNode.clear();
            out.viewedRoots = false;
            NodeId centersOf = 0;  // a Clump viewed: its centers draw as dots
            const auto it = viewed->nodesByName.find(flat_.result(request.viewNode));
            if (!request.viewNode.empty() && it != viewed->nodesByName.end() &&
                viewed->graph.outputType(it->second) == PortType::Strands) {  // as connected (shaping nodes)
                shown = it->second;
                out.viewedNode = request.viewNode;
                out.viewedRoots = viewed->graph.node(shown).typeName() == "Scatter";
                if (viewed->graph.node(shown).typeName() == op::kClump) centersOf = shown;
            }
            StrandsPtr strands = viewed->graph.evaluateStrands(shown, ctx);
            const StrandsPtr evaluated = strands;  // the node's result (before pinning or carrying)
            if (centersOf) {
                const std::shared_ptr<const std::vector<Vec3f>> dots = clumpCenterDots(*viewed, centersOf, evaluated, ctx);
                if (!previousCenters || *previousCenters != *dots) out.clumpCenters = dots;  // the same: kept
            } else {
                out.clumpCenters.reset();
            }
            // Instances pinned or fitted to their rest length: the rest pose (its own copy of the
            // graph, a cache hit from frame to frame) gives each its distance from the root.
            if (strands && strands->instances() && strands->instances()->needsRestPose()) {
                if (!restPose_) {
                    restPose_ = std::make_unique<RestPose>(parsed_->clone());
                    restPose_->setCacheIntermediates(cacheIntermediates_);
                }
                RestInputs rest;
                rest.scalp = following_ ? followRest_ : scalp_;
                if (previewHierarchy_ && !previewFinal_) {
                    // The stroke's working copy, on this scalp (taken to rest while following).
                    rest.hierarchy = following_ ? HierarchyPtr(carryHierarchyBack(*previewHierarchy_, *followRest_, *scalp_))
                                                : HierarchyPtr(previewHierarchy_);
                    rest.hierarchyOnScalp = true;
                } else if (!(hierarchyFromText_ && blank(data_.hierarchyJson))) {
                    rest.hierarchy = parsedHierarchy_;  // as authored, evaluated on the rest scalp
                }
                rest.guides = following_ ? followGuides_ : guides_;
                rest.parts = shownParts_;  // on scalp_'s faces, as the rest scalp's
                rest.flow = shownFlow_;
                rest.controls = scaledControls();
                rest.maps = data_.maps;
                strands = restPose_->pin(strands, out.viewedNode, rest, ctx);
            } else {
                restPose_.reset();
            }
            // Following the scalp: from the rest pose to the scalp now, root by root (or handed on at
            // rest to a host that carries them itself, with what carries them).
            if (following_ && request.hostCarries && strands && carriesPointsAndInstances(*strands)) {
                out.strands = strands;
                const bool same = previousFollow && previousFollow->rest() == strands && previousFollow->from() == followRest_ &&
                                  previousFollow->to() == scalp_;
                out.follow = same ? previousFollow : std::make_shared<const FollowCarry>(strands, followRest_, scalp_);
            } else {
                out.strands = carry(strands, 0);
            }
            // What these strands are, and how they differ from the ones this document gave last:
            // the graph's changes of the node shown, when the view hands its strands on as they
            // are (not carried to the scalp, nor pinned to rest).
            Shown& was = shown_[sculpting ? 1 : 0];
            const std::uint64_t graphSerial = viewed->graph.resultSerial(shown);
            const bool asEvaluated = out.strands == evaluated && !following_ && graphSerial != 0;
            if (asEvaluated && was.serial != 0 && was.node == shown && was.graphSerial == graphSerial &&
                was.strands.lock() == out.strands) {
                out.strandsSerial = was.serial;  // the same strands
                out.changesBase = was.serial;
                out.strandsChanges = viewed->graph.changesSince(shown, graphSerial);
            } else {
                out.strandsSerial = ++strandsSerial_;
                out.changesBase = was.serial;
                out.strandsChanges = asEvaluated && was.serial != 0 && was.node == shown
                                         ? viewed->graph.changesSince(shown, was.graphSerial) : nullptr;
                was.serial = asEvaluated ? out.strandsSerial : 0;
                was.node = shown;
                was.graphSerial = graphSerial;
                was.strands = out.strands;
            }
            if (request.wantCards) {
                // Mid-stroke too, while a rebuild is quick enough to follow the brush; slower
                // ones catch up when the stroke ends.
                if (!hasPreview() || lastCardBuildMs_ <= kLiveCardBuildBudgetMs) buildCards(ctx, request.cardDensity, messages);
            } else {
                cardDoc_.reset();
                out.cardStrands.reset();
                out.cards.reset();
                out.cardNodes.clear();
            }
            if (request.wantStrandMeshes) {
                // Mid-stroke as the cards: while quick enough to follow the brush.
                if (!hasPreview() || lastStrandMeshMs_ <= kLiveCardBuildBudgetMs) {
                    const auto started = std::chrono::steady_clock::now();
                    std::vector<StrandMeshSource> meshes = evaluateStrandMeshes(request.time);
                    lastStrandMeshMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                    if (!sameStrandMeshes(meshes, out.strandMeshes)) {
                        out.strandMeshes = std::move(meshes);
                        ++out.versions.strandMeshes;
                    }
                }
            } else if (!out.strandMeshes.empty()) {
                out.strandMeshes.clear();
                ++out.versions.strandMeshes;
            }
        } catch (const std::exception& e) {  // never thrown at the host: said in the errors
            shown_[0] = shown_[1] = Shown{};
            out.strandsSerial = out.changesBase = 0;
            out.strandsChanges.reset();
            failure_ = std::current_exception();
            errors.push_back(e.what());
            messages.push_back({e.what(), true});
        } catch (...) {
            shown_[0] = shown_[1] = Shown{};
            out.strandsSerial = out.changesBase = 0;
            out.strandsChanges.reset();
            failure_ = std::current_exception();
            errors.push_back("evaluation failed");
            messages.push_back({"evaluation failed", true});
        }
    }  // no graph (it did not load): no strands; the cards and the node shown stay as they were
    if (!graphError_.empty()) errors.insert(errors.begin(), graphError_);
    if (!hierarchyError_.empty()) errors.insert(errors.begin(), hierarchyError_);
    if (!sculptError_.empty()) errors.push_back(sculptError_);

    // A Sculpt node brushes shape: its output as the guides shown (a live stroke's working guides).
    HierarchyPtr shownHierarchy = activeHierarchy_;
    if (!sculptTarget_.empty()) {
        sculptShownVersion_ = sculptsVersion_;
        if (previewSculptShown_ && previewSculptNode_ == sculptTarget_) {
            shownHierarchy = previewSculptShown_;
            hierarchyChanged = true;  // the stroke edits it in place
        } else if (viewed) {
            const GuidesPtr output = sculptOutput(*viewed, ctx);
            if (!output) {
                sculptShown_.reset();
                sculptShownFrom_.reset();
            } else if (output != sculptShownFrom_ || scalp_ != sculptShownOn_ || !sculptShown_) {
                sculptShown_ = groomlab::sculptHierarchy(*output, sculptTarget_, following_ ? nullptr : scalp_.get());
                sculptShownFrom_ = output;
                sculptShownOn_ = scalp_;
            }
            if (sculptShown_) shownHierarchy = sculptShown_;
        }
    }
    if (shownHierarchy != previousShown) hierarchyChanged = true;

    // What changed, for drawing: the same cached results (a static groom at another frame)
    // leave the versions alone.
    // (Strands handed on at rest move with the scalp they are carried to.)
    auto carriedTo = [](const FollowCarryPtr& f) { return f ? f->to().get() : nullptr; };
    const bool strandsChanged = out.strands.get() != previousStrands.get() || carriedTo(out.follow) != carriedTo(previousFollow) ||
                                out.viewedNode != previousView || out.viewedRoots != previousRoots || out.clumpCenters != previousCenters;
    const bool cardsChanged = out.cards.get() != previousCards.get();
    const bool overlaysChanged = parts_.get() != previousParts.get() || flow_.get() != previousFlow.get();
    if (strandsChanged) ++out.versions.strands;
    if (hierarchyChanged) ++out.versions.hierarchy;
    if (cardsChanged) ++out.versions.cards;
    if ((strandsChanged || hierarchyChanged) && !request.wantBounds) boundsStale_ = true;
    if ((strandsChanged || hierarchyChanged || boundsStale_) && request.wantBounds) {
        boundsStale_ = false;
        constexpr float kBig = std::numeric_limits<float>::max();
        Vec3f lo(kBig, kBig, kBig), hi(-kBig, -kBig, -kBig);
        auto expand = [&](const Vec3f& p) {
            lo = Vec3f(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
            hi = Vec3f(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
        };
        if (out.strands) {  // one parallel pass, kept on the strands (drawing asks them too)
            // Only some strands changed since the strands the view had: their blocks measured again.
            const bool some = out.strandsChanges && !out.strandsChanges->all && previousStrands &&
                              out.changesBase == previousSerial;
            // Handed on at rest: each strand's box at rest where carrying it puts it (no copy made).
            const StrandSet::Extent e = out.follow ? carriedBounds(*out.strands, followBoxes(out.strands), *out.follow->to())
                                        : some ? out.strands->extentAfter(*previousStrands, out.strandsChanges->strands)
                                               : out.strands->extent();
            if (!e.finite) {  // said, never drawn into the bounds
                const std::string what = nonFiniteMessage(sculpting ? sculptDoc_.get() : doc_.get());
                errors.push_back(what);
                messages.push_back({what, true});
            }
            if (!e.empty() && e.finite) {
                expand(e.lo);
                expand(e.hi);
            }
        }
        for (const HierarchyPtr& h : {activeHierarchy_, shownHierarchy != activeHierarchy_ ? shownHierarchy : nullptr}) {
            if (!h) continue;
            for (const HierarchyGuide& g : h->guides()) {
                for (const Vec3f& p : g.points) expand(p);
            }
        }
        out.bounds.valid = lo.x <= hi.x;
        out.bounds.lo = out.bounds.valid ? lo : Vec3f();
        out.bounds.hi = out.bounds.valid ? hi : Vec3f();
    }
    const bool strandMeshesChanged = out.versions.strandMeshes != previousStrandMeshes;
    if (strandsChanged || hierarchyChanged || cardsChanged || overlaysChanged || strandMeshesChanged) ++out.versions.evaluation;

    out.scalp = scalp_;
    out.cageScalp = cageScalp_;
    out.hierarchy = shownHierarchy;
    out.authoredHierarchy = hierarchy_;
    out.parts = parts_;
    out.flow = flow_;
    out.errors = std::move(errors);
    out.messages = std::move(messages);
    return out;
}

StrandsPtr GroomSession::evaluate(float density, double seconds, bool buildCards) {
    ViewRequest request;
    request.density = density;
    request.time = seconds;
    request.buildCards = buildCards;
    request.wantBounds = false;  // exports and renders: the strands only
    const ViewResult& result = evaluate(request);
    if (failure_) std::rethrow_exception(failure_);
    return result.strands;
}

StrandsPtr GroomSession::viewedAsRendered(float density, double seconds) const {
    const ViewResult& view = view_;
    if (!doc_ || failure_ || !view.strands || !view.errors.empty() || hasPreview() || viewSculpt_ || !view.viewedNode.empty()) {
        return nullptr;
    }
    const EvalContext& was = lastViewContext_;
    if (was.previewDensity != density || was.time != seconds || was.buildCards) return nullptr;
    const Graph& graph = doc_->graph;
    for (const NodeId id : graph.nodeIds()) {
        const Operator& op = graph.node(id);
        const std::string type = op.typeName();
        if (type == op::kCompound || type == op::kReference) return nullptr;
        if (was.viewport && graph.evaluationMode(id) != EvaluationMode::Always) return nullptr;
        if (was.maxPoints > 0.0 && op.thinnedLastRun()) return nullptr;
    }
    return view.placed();
}

void GroomSession::buildCards(const EvalContext& ctx, float cardDensity, std::vector<SessionMessage>& messages) {
    ViewResult& out = view_;
    // The card graph's own node cache decides what is stale (its keys cover every input, painted
    // map, texture stamp and the time), so a hit returns the same strands and the cards stay.
    const auto started = std::chrono::steady_clock::now();
    // Its own copy of the graph, so this evaluation (another density, with cards) doesn't evict
    // the view's cached results.
    if (!cardDoc_) {
        cardDoc_ = std::make_unique<GraphDocument>(parsed_->clone());
        cardDoc_->graph.setCacheIntermediates(cacheIntermediates_);
    }
    EvalContext cardCtx = ctx;
    cardCtx.previewDensity = cardDensity;  // never the view's preview density
    cardCtx.maxPoints = 0.0;               // nor its point budget: every strand goes into its card
    cardCtx.buildCards = true;             // Cards nodes build their cards on this evaluation
    StrandsPtr cardStrands = carry(cardDoc_->graph.evaluateStrands(cardDoc_->output, cardCtx), 1);
    if (!out.cardStrands || cardStrands != out.cardStrands) {
        out.cardStrands = std::move(cardStrands);
        out.cards.reset();
        out.cardNodes.clear();
        if (out.cardStrands && !out.cardStrands->cardLayers().empty()) {
            auto cards = std::make_shared<CardMesh>(combineCardLayers(out.cardStrands->cardLayers()));
            layoutCards(*cards, cards->padding);
            // The baked atlas: rebuilt cards keep the cells they were baked with.
            refreshCardAtlas(messages);
            if (!cardAtlas_.empty()) applyCardAtlas(*cards, cardAtlas_, out.cardStrands.get());
            out.cardNodes = summarizeCardNodes(*cards, out.cardStrands.get());
            out.cards = std::move(cards);
        }
        lastCardBuildMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    } else if (out.cards && refreshCardAtlas(messages)) {
        // A new bake (or its atlas gone): the same cards, laid out for it, so the new maps fit at
        // once rather than after the next change upstream.
        auto cards = std::make_shared<CardMesh>(combineCardLayers(out.cardStrands->cardLayers()));
        layoutCards(*cards, cards->padding);
        if (!cardAtlas_.empty()) applyCardAtlas(*cards, cardAtlas_, out.cardStrands.get());
        out.cardNodes = summarizeCardNodes(*cards, out.cardStrands.get());
        out.cards = std::move(cards);
    }
}

bool GroomSession::refreshCardAtlas(std::vector<SessionMessage>& messages) {
    const auto atlas = cardMaps_.find("Atlas");
    std::string path;
    if (atlas != cardMaps_.end() && !atlas->second.empty()) {
        const ScopedPathResolver scope(paths());
        path = resolvePath(atlas->second);
    }
    const std::uint64_t stamp = path.empty() ? 0 : fileStamp(path);
    if (path == cardAtlasPath_ && stamp == cardAtlasStamp_) return false;
    cardAtlasPath_ = path;
    cardAtlasStamp_ = stamp;
    cardAtlas_ = CardAtlas{};
    if (!path.empty() && stamp != 0) {
        try {
            std::ifstream in(path, std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            cardAtlas_ = CardAtlas::fromJson(text.str());
        } catch (const std::exception& e) {
            messages.push_back({std::string("card atlas ") + path + ": " + e.what() + " (the cards use a fresh layout)", true});
        }
    }
    return true;
}

Value GroomSession::evaluateNode(const std::string& name, float density, double seconds, bool buildCards) {
    if (!doc_) throw std::runtime_error(graphError_.empty() ? "the groom has no graph" : graphError_);
    // A scratch copy of the graph: results that match the view's are reused, and what this
    // evaluation makes never evicts them.
    GraphDocument scratch = parsed_->clone();  // the graph as parsed (no second parse)
    scratch.graph.adoptCache(doc_->graph);
    const NodeId id = name.empty() ? scratch.output : scratch.node(flat_.result(name));  // throws GraphError when there is no such node
    ExternalInputs externals;
    fillExternals(externals);
    EvalContext ctx;
    ctx.previewDensity = std::clamp(density, 0.0f, 1.0f);
    ctx.time = seconds;
    ctx.framesPerSecond = fps_;
    ctx.scale = groomScale_;  // the lattices sizes round to, at the groom's scale
    ctx.externals = &externals;
    ctx.paths = paths();
    ctx.buildCards = buildCards;
    Value value = scratch.graph.evaluate(id, ctx);
    if (const StrandsPtr strands = valueAs<StrandsPtr>(value)) return Value(carry(strands, 3));
    return value;
}

StrandsPtr GroomSession::evaluateFullDensity(double seconds) {
    if (!doc_) return nullptr;
    // The groom at full density, on its own copy of the graph (the view's cache is left alone).
    if (!fullDoc_) {
        fullDoc_ = std::make_unique<GraphDocument>(parsed_->clone());
        fullDoc_->graph.setCacheIntermediates(cacheIntermediates_);
        fullDoc_->graph.adoptCache(doc_->graph);
    }
    ExternalInputs externals;
    fillExternals(externals);
    EvalContext ctx;
    ctx.previewDensity = 1.0f;
    ctx.time = seconds;
    ctx.framesPerSecond = fps_;
    ctx.scale = groomScale_;  // the lattices sizes round to, at the groom's scale
    ctx.externals = &externals;
    ctx.paths = paths();
    return carry(fullDoc_->graph.evaluateStrands(fullDoc_->output, ctx), 2);
}

void GroomSession::releaseFullDensity() { fullDoc_.reset(); }

std::vector<GroomSession::StrandMeshSource> GroomSession::evaluateStrandMeshes(double seconds) {
    std::vector<StrandMeshSource> out;
    if (!doc_ || !parsed_) return out;
    // The nodes making the output (a compound's inner ones are left out: their paths have a '/').
    std::vector<std::string> names;
    parsed_->graph.forEachActiveUpstream(parsed_->output, [&](const std::string& path, const Operator& op) {
        if (op.typeName() == op::kToMesh && path.find('/') == std::string::npos &&
            parsed_->nodesByName.count(path) != 0) {
            names.push_back(path);
        }
        return true;
    });
    if (names.empty()) {
        releaseStrandMeshes();
        return out;
    }
    ExternalInputs externals;
    fillExternals(externals);
    EvalContext ctx;
    ctx.time = seconds;
    ctx.framesPerSecond = fps_;
    ctx.scale = groomScale_;  // the lattices sizes round to, at the groom's scale
    ctx.externals = &externals;
    ctx.paths = paths();
    std::map<float, std::unique_ptr<GraphDocument>> docs;  // the densities still asked for
    std::map<std::string, MeshMemo> memos;
    for (const std::string& name : names) {
        const NodeId id = parsed_->nodesByName.at(name);
        const StrandMeshSettings settings = strandMeshSettings(parsed_->graph.node(id).params());
        const float density = settings.density > 0.0f ? settings.density : 1.0f;
        auto doc = docs.find(density);
        if (doc == docs.end()) {
            auto old = meshDocs_.find(density);
            std::unique_ptr<GraphDocument> made;
            if (old != meshDocs_.end()) {
                made = std::move(old->second);
            } else {
                made = std::make_unique<GraphDocument>(parsed_->clone());
                made->graph.setCacheIntermediates(cacheIntermediates_);
                if (density == 1.0f && fullDoc_) made->graph.adoptCache(fullDoc_->graph);
            }
            doc = docs.emplace(density, std::move(made)).first;
        }
        ctx.previewDensity = density;
        // Cards (the Cards nodes upstream build them): only when the node may make a mesh of them.
        ctx.buildCards = settings.options.make == StrandMeshOptions::Make::Auto ||
                         settings.options.make == StrandMeshOptions::Make::Cards;
        const StrandsPtr in = doc->second->graph.evaluateStrands(doc->second->nodesByName.at(name), ctx);
        // The same strands on the same scalps as last time: the same result (hosts keep their mesh).
        MeshMemo memo;
        memo.in = in;
        memo.from = following_ ? followRest_ : nullptr;
        memo.to = following_ ? scalp_ : nullptr;
        memo.maxStrands = settings.maxStrands;
        const auto was = meshMemos_.find(name);
        if (was != meshMemos_.end() && was->second.in == memo.in && was->second.from == memo.from &&
            was->second.to == memo.to && was->second.maxStrands == memo.maxStrands) {
            memo.out = was->second.out;
        } else if (in) {
            StrandsPtr kept = in;
            if (settings.maxStrands > 0 && in->strandCount() > settings.maxStrands) {
                kept = std::make_shared<StrandSet>(in->subset(thinStrandsById(*in, settings.maxStrands)));
            }
            memo.out = following_ && followRest_ && scalp_ ? carryStrands(kept, *followRest_, *scalp_) : kept;
        }
        out.push_back({name, memo.out, settings.options});
        memos[name] = std::move(memo);
    }
    meshDocs_ = std::move(docs);
    meshMemos_ = std::move(memos);
    return out;
}

void GroomSession::releaseStrandMeshes() {
    meshDocs_.clear();
    meshMemos_.clear();
}

bool GroomSession::warmStroke(const ViewRequest& request) {
    const float density = request.density * request.sculptDensity;
    // (Nothing evaluated yet: no inputs bound to warm it with.)
    if (!doc_ || hasPreview() || density == request.density || view_.versions.evaluation == 0) return false;
    if (!sculptDoc_) {
        sculptDoc_ = std::make_unique<GraphDocument>(parsed_->clone());
        sculptDoc_->graph.setCacheIntermediates(cacheIntermediates_);
    }
    // The inputs as the last evaluation bound them (the scalp with its maps, the active hierarchy,
    // parts, flow, the rest-space ones while following): nothing here binds or roots again, so the
    // view's next evaluation sees its changes as ever.
    ExternalInputs externals;
    fillExternals(externals);
    EvalContext ctx;
    ctx.previewDensity = density;
    ctx.maxPoints = request.pointBudget;
    ctx.time = request.time;
    ctx.framesPerSecond = fps_;
    ctx.scale = groomScale_;  // the lattices sizes round to, at the groom's scale
    ctx.viewport = request.viewport;
    ctx.liveDrag = liveDrag_;
    ctx.externals = &externals;
    ctx.paths = paths();
    // The node a stroke would show (the view's node, as evaluate picks it).
    NodeId shown = sculptDoc_->output;
    const auto it = sculptDoc_->nodesByName.find(flat_.result(request.viewNode));
    if (!request.viewNode.empty() && it != sculptDoc_->nodesByName.end() &&
        sculptDoc_->graph.outputType(it->second) == PortType::Strands) {
        shown = it->second;
    }
    Graph& graph = sculptDoc_->graph;
    const std::size_t evaluations = graph.evaluationCount(), incrementals = graph.incrementalCount();
    try {
        graph.evaluate(shown, ctx);
    } catch (...) {  // the view says it
    }
    warmEvaluations_ += graph.evaluationCount() - evaluations;
    warmIncrementals_ += graph.incrementalCount() - incrementals;
    return graph.evaluationCount() != evaluations;
}

std::shared_ptr<DetachedEvaluation> GroomSession::detachWarm(const ViewRequest& request) {
    const float density = request.density * request.sculptDensity;
    if (!doc_ || hasPreview() || density == request.density || view_.versions.evaluation == 0) return nullptr;
    if (!sculptDoc_) {
        sculptDoc_ = std::make_unique<GraphDocument>(parsed_->clone());
        sculptDoc_->graph.setCacheIntermediates(cacheIntermediates_);
    }
    // The inputs as warmStroke binds them (the last evaluation's), owned by the job.
    std::shared_ptr<DetachedEvaluation> job(new DetachedEvaluation);
    fillExternals(job->externals_);
    job->paths_ = paths() ? paths_ : nullptr;
    EvalContext& ctx = job->ctx_;
    ctx.previewDensity = density;
    ctx.maxPoints = request.pointBudget;
    ctx.time = request.time;
    ctx.framesPerSecond = fps_;
    ctx.scale = groomScale_;  // the lattices sizes round to, at the groom's scale
    ctx.viewport = request.viewport;
    ctx.liveDrag = liveDrag_;
    ctx.externals = &job->externals_;
    ctx.paths = job->paths_.get();
    const auto it = sculptDoc_->nodesByName.find(flat_.result(request.viewNode));
    if (!request.viewNode.empty() && it != sculptDoc_->nodesByName.end() && sculptDoc_->graph.outputType(it->second) == PortType::Strands) {
        job->shown_ = flat_.result(request.viewNode);
    }
    job->request_ = request;
    job->doc_ = std::make_unique<GraphDocument>(sculptDoc_->clone());
    job->basedOn_ = docGeneration_;
    job->warm_ = true;
    job->warmBase_ = sculptDoc_.get();
    job->warmBaseEvaluations_ = sculptDoc_->graph.evaluationCount();
    job->warmBaseGeneration_ = strokeDocGeneration_;
    return job;
}

bool GroomSession::adoptWarm(DetachedEvaluation& run) {
    if (!run.warm_ || !run.ran() || run.error_ || !run.doc_ || !sculptDoc_ || hasPreview()) return false;
    if (sculptDoc_.get() != run.warmBase_ || sculptDoc_->graph.evaluationCount() != run.warmBaseEvaluations_ ||
        strokeDocGeneration_ != run.warmBaseGeneration_) {
        return false;
    }
    // Not counted, as warmStroke's are not: the copy's evaluations join the warm ones.
    warmEvaluations_ += run.doc_->graph.evaluationCount();
    warmIncrementals_ += run.doc_->graph.incrementalCount();
    run.doc_->graph.addCounts(sculptDoc_->graph.evaluationCount(), sculptDoc_->graph.incrementalCount());
    std::swap(sculptDoc_, run.doc_);  // (the old document goes with the run)
    return true;
}

std::shared_ptr<DetachedEvaluation> GroomSession::detachStroke(const ViewRequest& request) {
    // (What only the session's own evaluation does: map, part and flow previews (the tool's objects
    // the scalp's binding reads), the Simulate nodes' stepping, cards, a host's stopAt.)
    if (!doc_ || !sculptDoc_ || strokeLent_ || !previewHierarchy_ || liveStroke_ || previewParts_ || previewFlow_ || previewMapValues_ ||
        previewMapImage_ || !sims_.empty() || request.wantCards || request.wantStrandMeshes || !request.stopAt.empty()) {
        return nullptr;
    }
    const float density = request.density * request.sculptDensity;
    if (density == request.density) return nullptr;  // the view's own document: nothing to lend
    // The inputs as evaluate(request) binds them now, the preview a copy of the tool's guides as
    // they are (they go on changing between dabs); the active hierarchy as the session had it
    // after, so nothing the session's caller reads meanwhile is the run's.
    const std::shared_ptr<GuideHierarchy> live = previewHierarchy_;
    const HierarchyPtr active = activeHierarchy_, posed = posedHierarchy_;
    const bool activeValid = activeValid_;
    const std::uint64_t activeKey = activeKey_;
    std::shared_ptr<DetachedEvaluation> job(new DetachedEvaluation);
    job->strokePreview_ = std::make_shared<GuideHierarchy>(*live);
    previewHierarchy_ = job->strokePreview_;
    std::vector<std::string> errors;  // said by the evaluation that takes the run in
    prepareTime(request.time, errors, detachedMessages_);
    fillExternals(job->externals_);
    previewHierarchy_ = live;
    activeHierarchy_ = active;
    posedHierarchy_ = posed;
    activeValid_ = activeValid;
    activeKey_ = activeKey;
    job->paths_ = paths() ? paths_ : nullptr;
    EvalContext& ctx = job->ctx_;
    ctx.previewDensity = density;
    ctx.maxPoints = request.pointBudget;
    ctx.time = request.time;
    ctx.framesPerSecond = fps_;
    ctx.scale = groomScale_;  // the lattices sizes round to, at the groom's scale
    ctx.viewport = request.viewport;
    ctx.liveDrag = liveDrag_;
    ctx.externals = &job->externals_;
    ctx.paths = job->paths_.get();
    ctx.buildCards = request.buildCards;
    const auto it = sculptDoc_->nodesByName.find(flat_.result(request.viewNode));
    if (!request.viewNode.empty() && it != sculptDoc_->nodesByName.end() && sculptDoc_->graph.outputType(it->second) == PortType::Strands) {
        job->shown_ = flat_.result(request.viewNode);
    }
    job->request_ = request;
    job->doc_ = std::move(sculptDoc_);  // lent, not copied: nothing else evaluates it until adopted
    job->basedOn_ = docGeneration_;
    job->stroke_ = true;
    job->strokeGraph_ = graphGeneration_;
    strokeLent_ = true;
    return job;
}

bool GroomSession::adoptStroke(DetachedEvaluation& run) {
    if (!run.stroke_ || !run.ran()) return false;
    strokeLent_ = false;
    // The guides the run evaluated stand for the tool's while the host shows its result.
    if (previewHierarchy_ && run.strokePreview_) previewHierarchy_ = run.strokePreview_;
    // The document back, unless the graph was set again (it is another graph's) or a stroke
    // document was made meanwhile (the session's own evaluation: it stands).
    if (!run.doc_ || run.strokeGraph_ != graphGeneration_ || sculptDoc_) return false;
    sculptDoc_ = std::move(run.doc_);
    return true;
}

std::shared_ptr<DetachedEvaluation> GroomSession::detach(const ViewRequest& request, const std::shared_ptr<DetachedEvaluation>& after) {
    // (What only the session's own evaluation does: a preview's stroke document, the Simulate
    // nodes' stepping through the host, the cards' document.)
    if (!doc_ || hasPreview() || !sims_.empty() || request.wantCards || request.wantStrandMeshes || !request.stopAt.empty()) {
        return nullptr;
    }
    if (after && (after->warm_ || after->basedOn_ != docGeneration_)) return nullptr;
    std::shared_ptr<DetachedEvaluation> job(new DetachedEvaluation);
    // The inputs as evaluate(request) binds them now; what binding them says is said by the
    // evaluation that takes the result in.
    std::vector<std::string> errors;
    prepareTime(request.time, errors, detachedMessages_);
    fillExternals(job->externals_);
    job->paths_ = paths() ? paths_ : nullptr;
    EvalContext& ctx = job->ctx_;
    ctx.previewDensity = request.density;
    ctx.maxPoints = request.pointBudget;
    ctx.time = request.time;
    ctx.framesPerSecond = fps_;
    ctx.scale = groomScale_;  // the lattices sizes round to, at the groom's scale
    ctx.viewport = request.viewport;
    ctx.liveDrag = liveDrag_;
    ctx.externals = &job->externals_;
    ctx.paths = job->paths_.get();
    ctx.buildCards = request.buildCards;
    // The node shown, as evaluate picks it.
    const auto it = doc_->nodesByName.find(flat_.result(request.viewNode));
    if (!request.viewNode.empty() && it != doc_->nodesByName.end() && doc_->graph.outputType(it->second) == PortType::Strands) {
        job->shown_ = flat_.result(request.viewNode);
    }
    job->request_ = request;
    if (after) {
        job->after_ = after;
        job->basedOn_ = after->basedOn_;
    } else {
        job->doc_ = std::make_unique<GraphDocument>(doc_->clone());
        job->basedOn_ = docGeneration_;
    }
    return job;
}

void DetachedEvaluation::run() {
    const auto started = std::chrono::steady_clock::now();
    try {
        if (after_) {
            // The evaluation before ran (the host runs them in order): this one goes on from what it made.
            if (!after_->ran()) throw std::logic_error("a detached evaluation ran before the one it follows");
            if (after_->error_) std::rethrow_exception(after_->error_);
            doc_ = std::make_unique<GraphDocument>(after_->doc_->clone());
            doc_->graph.addCounts(after_->doc_->graph.evaluationCount(), after_->doc_->graph.incrementalCount());
            after_.reset();  // (its document let go: its results are this copy's alone)
        }
        const auto it = doc_->nodesByName.find(shown_);
        doc_->graph.evaluateStrands(shown_.empty() || it == doc_->nodesByName.end() ? doc_->output : it->second, ctx_);
    } catch (...) {
        error_ = std::current_exception();
    }
    after_.reset();
    ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    ran_.store(true, std::memory_order_release);
}

bool GroomSession::adopt(DetachedEvaluation& run) {
    if (run.warm_ || !run.ran() || run.error_ || !run.doc_ || !doc_ || run.basedOn_ != docGeneration_) return false;
    // (Counted on: what this document evaluated, then the copy.)
    run.doc_->graph.addCounts(doc_->graph.evaluationCount(), doc_->graph.incrementalCount());
    std::swap(doc_, run.doc_);  // (the old document goes with the run)
    ++docGeneration_;
    return true;
}

void GroomSession::releaseStroke() {
    if (!sculptDoc_) return;
    releasedEvaluations_ += sculptDoc_->graph.evaluationCount() - warmEvaluations_;
    releasedIncrementals_ += sculptDoc_->graph.incrementalCount() - warmIncrementals_;
    warmEvaluations_ = warmIncrementals_ = 0;
    sculptDoc_.reset();
    ++strokeDocGeneration_;
    viewSculpt_ = false;
    shown_[1] = Shown{};
}

// ---- What the last evaluation made ----------------------------------------------------------

int GroomSession::scalpHistoryFrames(const std::string& viewNode) const {
    if (!doc_) return 0;
    int frames = groomlab::scalpHistoryFrames(doc_->graph, doc_->output);
    if (!viewNode.empty()) {
        const auto viewed = doc_->nodesByName.find(flat_.result(viewNode));
        if (viewed != doc_->nodesByName.end()) frames = std::max(frames, groomlab::scalpHistoryFrames(doc_->graph, viewed->second));
    }
    return frames;
}

std::vector<double> GroomSession::scalpHistoryLoopFrames(double now, const std::string& viewNode) const {
    if (!doc_) return {};
    // Past the most frames either reads (the history's plain frames).
    const int plain = scalpHistoryFrames(viewNode);
    std::vector<double> frames = groomlab::scalpHistoryLoopFrames(doc_->graph, doc_->output, now);
    if (!viewNode.empty()) {
        const auto viewed = doc_->nodesByName.find(flat_.result(viewNode));
        if (viewed != doc_->nodesByName.end()) {
            const std::vector<double> more = groomlab::scalpHistoryLoopFrames(doc_->graph, viewed->second, now);
            frames.insert(frames.end(), more.begin(), more.end());
        }
    }
    frames.erase(std::remove_if(frames.begin(), frames.end(),
                                [&](double f) {
                                    const double back = now - f;
                                    return std::abs(back - std::round(back)) < 1e-3 && back > -0.5 && back < plain + 0.5;
                                }),
                 frames.end());
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end(), [](double a, double b) { return std::abs(a - b) < 1e-6; }), frames.end());
    return frames;
}

std::string GroomSession::hairLookJson() const {
    const HairLook look = hairLook();
    const HairLobes lobes = hairLobes(look);
    nlohmann::json out = {{"node", look.node},
                          {"specular", look.specular},
                          {"roughness", look.roughness},
                          {"specularRandom", look.specularRandom},
                          {"roughnessRandom", look.roughnessRandom},
                          {"tilt", look.tilt},
                          {"dye",{look.dye[0], look.dye[1], look.dye[2]}},
                          {"colorsByRgb", look.colorsByRgb},
                          {"colorBlends", look.colorBlends},
                          {"iridescent", look.iridescent},
                          {"iridescenceColor", {look.iridescenceColor[0], look.iridescenceColor[1], look.iridescenceColor[2]}},
                          {"iridescenceShift", look.iridescenceShift},
                          {"wet", look.wet},
                          {"lobes",
                           {{"specularTint", lobes.specularTint},
                            {"roughness", lobes.roughness},
                            {"azimuthalRoughness", lobes.azimuthalRoughness},
                            {"ior", lobes.ior},
                            {"shift", lobes.shiftDegrees}}}};
    nlohmann::json hairColor, renderData, colors = nlohmann::json::array();
    if (doc_) {
        doc_->graph.forEachActiveUpstream(doc_->output, [&](const std::string& at, const Operator& op) {
            const std::string& type = op.typeName();
            if (type == "HairColor" && hairColor.is_null()) hairColor = nlohmann::json::parse(paramsJson(op.params()));
            if (type == "RenderData" && renderData.is_null()) renderData = nlohmann::json::parse(paramsJson(op.params()));
            if (colorNode(type)) colors.push_back({{"path", at}, {"type", type}, {"params", nlohmann::json::parse(paramsJson(op.params()))}});
            return true;
        });
    }
    out["hairColorParams"] = hairColor;
    out["renderDataParams"] = renderData;
    out["colorNodes"] = colors;
    return out.dump();
}

bool GroomSession::hasStrandMeshNode() const {
    if (!doc_) return false;
    bool found = false;
    doc_->graph.forEachActiveUpstream(doc_->output, [&](const std::string& path, const Operator& op) {
        found = op.typeName() == op::kToMesh && path.find('/') == std::string::npos;
        return !found;
    });
    return found;
}

bool GroomSession::hasCardsNode() const {
    if (!doc_) return false;
    bool cards = false;
    doc_->graph.forEachActiveUpstream(doc_->output, [&](const std::string&, const Operator& op) {
        cards = op.typeName() == "Cards";
        return !cards;
    });
    return cards;
}

HairLook GroomSession::hairLook() const {
    HairLook look;
    if (!doc_) return look;
    if (const Operator* node = activeHairColor(*doc_, &look.node)) {
        if (node->params().getString("mode", "melanin") == "melanin") {
            const Vec3f dye = node->params().getVec3("dye");
            look.dye = {dye.x, dye.y, dye.z};
        }
    }
    // A number param that is a map, or above 0.
    auto on = [](const ParamSet& p, const char* name) {
        return p.has(name) && (std::holds_alternative<MapRef>(p.get(name)) || p.getDouble(name, 0.0) > 0.0);
    };
    bool highlight = false;
    doc_->graph.forEachActiveUpstream(doc_->output, [&](const std::string&, const Operator& op) {
        const ParamSet& p = op.params();
        // The highlight: the first HairColor's (a map: its default).
        if (op.typeName() == "HairColor" && !highlight) {
            highlight = true;
            auto number = [&](const char* name, float fallback) {
                return p.has(name) && std::holds_alternative<MapRef>(p.get(name)) ? fallback
                                                                                  : static_cast<float>(p.getDouble(name, fallback));
            };
            look.specular = number("specular", kDefaultHairSpecular);
            look.roughness = number("roughness", kDefaultHairRoughness);
            look.specularRandom = number("specularRandom", 0.0f);
            look.roughnessRandom = number("roughnessRandom", 0.0f);
            look.tilt = number("tilt", kDefaultHairTilt);
        }
        if (op.typeName() == "FeatherColor") {
            if (!look.iridescent && on(p, "iridescence")) {
                look.iridescent = true;
                const Vec3f c = p.getVec3("iridescenceColor");
                look.iridescenceColor = {c.x, c.y, c.z};
                look.iridescenceShift = static_cast<float>(p.getDouble("iridescenceShift"));
            }
            look.colorsByRgb = true;  // a pattern, a down color, iridescence: RGB
        }
        if (op.typeName() == "Bands") look.colorsByRgb = true;
        if (op.typeName() == "Wet" || (op.typeName() == "Attribute" && p.getString("name") == kWetnessChannel)) look.wet = true;
        if (op.typeName() == "Simulate" && (p.getBool("water", false) || p.getDouble("rain", 0.0) > 0.0)) look.wet = true;
        // HairColor's RGB is the hair's color, whatever its mode (the pigment's color times the dye
        // in mode melanin): every host renders that one color.
        if (op.typeName() == "HairColor") look.colorsByRgb = true;
        return true;
    });
    look.colorBlends = !colorBlendNodes(*doc_).empty();
    return look;
}

ScalpMapView GroomSession::scalpMapView(const std::string& name, const std::string& channel) {
    ScalpMapView view;
    view.name = name;
    view.channel = channel;
    if (view.channel != "r" && view.channel != "g" && view.channel != "b" && view.channel != "a") view.channel = "rgb";
    if (view.name.empty() || !scalp_) return view;
    const auto it = scalpMaps_.maps.find(view.name);
    const bool previewing = previewMapName_ == view.name;
    if (it != scalpMaps_.maps.end()) {
        view.kind = it->second.kind;
        view.channels = it->second.channels == 4 ? 4 : 1;
        const std::uint64_t shownKey = hashText(hash64(0x5CA1AB), view.channels == 4 ? view.channel : std::string());
        if (view.kind == "vertices") {
            const std::uint64_t key = hashCombine(hashCombine(hashText(hash64(0x5CA1AA), view.name), scalpMapsHash_),
                                                  previewing && previewMapValues_ ? previewMapVersion_ : 0);
            const std::size_t per = static_cast<std::size_t>(view.channels);
            // Stored per vertex of the cage; a refined scalp shows them interpolated to its vertices.
            const ScalpMesh& cage = cageScalp_ ? *cageScalp_ : *scalp_;
            const int levels = scalp_->subdivisionLevel();
            auto shown = [&](const std::shared_ptr<const std::vector<float>>& values) -> std::shared_ptr<const std::vector<float>> {
                if (levels == 0) return values;
                return std::make_shared<const std::vector<float>>(
                    SubdivisionPlan::of(cage, levels)->refineVertexValues(*values, view.channels));
            };
            if (previewing && previewMapValues_ && previewMapValues_->size() == cage.pointCount() * per) {
                view.values = shown(previewMapValues_);
            } else if (it->second.values.size() == cage.pointCount() * per) {
                const std::uint64_t refinedKey = hashCombine(key, scalp_->topologyHash());
                if (refinedKey != mapViewKey_ || !mapViewValues_) {
                    mapViewValues_ = shown(std::make_shared<const std::vector<float>>(it->second.values));
                }
                view.values = mapViewValues_;
                mapViewKey_ = refinedKey;
            } else {
                view.kind.clear();  // does not fit this scalp
            }
            view.version = hashCombine(hashCombine(key, scalp_->contentHash()), shownKey);
        } else {
            {
                const ScopedPathResolver scope(paths());
                view.texture = resolvePath(it->second.texture);
            }
            if (previewing && previewMapImage_) view.preview = previewMapImage_;
            view.version = hashCombine(hashCombine(hashCombine(hashText(hash64(0x5CA1BB), view.texture), fileStamp(view.texture)),
                                                   hashCombine(previewing && previewMapImage_ ? previewMapVersion_ : 0, scalp_->contentHash())),
                                       shownKey);
        }
        return view;
    }
    // A guide layer: sampled at the scalp's vertices from the guides as they are now.
    const HierarchyPtr h = activeHierarchy_;
    if (!h) return view;
    const std::vector<std::string> names = h->attributeNames();
    if (std::find(names.begin(), names.end(), view.name) == names.end()) return view;
    view.kind = "guides";
    // The deepest level's guides, as h->flatten() gives them to sampleGuideLayer: each one's root
    // (on the scalp's rest pose) and its value of the layer. Sampled again only when those (or
    // the scalp) changed: combing a guide moves no root.
    const std::set<GroupId> deepest = h->deepestGroups();
    std::vector<Vec3f> roots;
    std::vector<float> values;
    std::uint64_t key = hashCombine(hashText(hash64(0x5CA1CC), view.name), scalp_->contentHash());
    for (const HierarchyGuide& g : h->guides()) {
        if (!deepest.count(g.group) || g.points.size() < 2) continue;
        roots.push_back(scalp_->contains(g.root) ? scalp_->restPositionOr(g.root, g.points.front()) : g.points.front());
        float value = 0.0f;
        values.push_back(h->effectiveAttribute(g, view.name, &value) ? value : 0.0f);
        key = hashFloat(hashFloat(hashFloat(hashFloat(key, roots.back().x), roots.back().y), roots.back().z), values.back());
    }
    key = hashCombine(key, roots.size());
    if (key != mapViewKey_ || !mapViewValues_) {
        mapViewValues_ = std::make_shared<const std::vector<float>>(sampleRootValues(roots, values, vertexSamplePoints(*scalp_)));
        mapViewKey_ = key;
    }
    view.values = mapViewValues_;
    view.version = key;
    return view;
}

GroomStats GroomSession::stats() const {
    GroomStats out;
    const ViewResult& v = view_;
    const StrandsPtr& strands = v.strands;
    out.view = v.viewedNode;
    out.distanceFieldsMade = MeshDistanceField::madeCount();
    out.strands = strands ? strands->strandCount() : 0;
    out.points = strands ? strands->pointCount() : 0;
    out.guides = activeHierarchy_ ? activeHierarchy_->guides().size() : 0;
    out.cards = v.cards ? v.cards->cards.size() : 0;
    out.cardStrands = v.cardStrands ? v.cardStrands->strandCount() : 0;  // the strands the cards were built from
    out.cardTriangles = v.cards ? v.cards->triangleCount() : 0;
    out.cardTextures = v.cards ? v.cards->textureCount() : 0;
    {
        // Baked maps, and whether each Cards node's cards still match what they were baked from
        // (a node the bake never saw is out of date too); stale: any of them.
        const auto coverage = cardMaps_.find("Coverage");
        const bool baked = coverage != cardMaps_.end() && !coverage->second.empty();
        const auto stampIt = cardMaps_.find("Strands");
        const std::string stamp = stampIt != cardMaps_.end() ? stampIt->second : std::string();
        const auto bakedHashes = parseCardsBakeStamp(stamp);  // no stamp at all: maps set by hand, never stale
        if (v.cards) {
            for (const CardNodeSummary& n : v.cardNodes) {
                auto it = bakedHashes.find(n.node);
                const bool stale = baked && !stamp.empty() && (it == bakedHashes.end() || it->second != std::to_string(n.bakeHash));
                out.cardsStale = out.cardsStale || stale;
                out.cardFeathers += n.feathers;
                out.cardNodes[n.node] = {n.cards, n.triangles, n.textures, n.feathers, stale};
            }
        }
        out.cardsBaked = baked;
    }
    if (doc_) {
        const GraphDocument& doc = *viewDoc();  // the times, notes and channels of the last view's evaluation
        out.cacheBytes = doc.graph.cachedBytes();
        out.cacheEvictions = doc.graph.cacheEvictions();
        activeHairColor(*doc_, &out.hairColor);
        activeNode(*doc_, "RenderData", &out.renderData);
        // Every color node making the output (HairColor, Bands, FeatherColor), in the same walk's
        // order (a Merge's other branches too); the Blend nodes making the output that mix two colors.
        doc_->graph.forEachActiveUpstream(doc_->output, [&](const std::string& at, const Operator& op) {
            if (colorNode(op.typeName())) out.colorNodes.push_back(at);
            return true;
        });
        out.colorBlends = colorBlendNodes(*doc_);
        for (const auto& [name, id] : doc.nodesByName) {
            auto time = doc.graph.lastEvaluationTimes().find(id);
            if (time != doc.graph.lastEvaluationTimes().end()) {
                out.times[name] = time->second;
                auto kind = doc.graph.lastRunKinds().find(id);
                if (kind != doc.graph.lastRunKinds().end()) out.runs[name] = time->second > 0.0 ? kind->second : "cached";
            }
            auto note = doc.graph.lastNotes().find(id);  // not errors: a Jiggle without scalp motion, say
            if (note != doc.graph.lastNotes().end()) out.notes[name] = note->second;
            // Each node's strand channels (from its cached result): what attribute maps downstream
            // can read. Not known: left out.
            std::vector<std::string> names;
            if (doc.graph.cachedChannels(id, &names)) out.channels[name] = std::move(names);
        }
        // Each compound as the one node hosts show (its nodes inline): their times summed, how
        // the last of them ran, their notes, its output node's channels.
        foldCompoundTimes(flat_, out.times);
        foldCompoundNotes(flat_, out.notes);
        for (const auto& [compound, output] : flat_.output) {
            const std::string prefix = compound + "/";
            for (const auto& [name, run] : std::map<std::string, std::string>(out.runs)) {
                if (name.rfind(prefix, 0) == 0 && (!out.runs.count(compound) || run != "cached")) out.runs[compound] = run;
            }
            if (const auto c = out.channels.find(flat_.result(compound)); c != out.channels.end()) out.channels[compound] = c->second;
        }
    }
    out.scalpHistoryFrames = scalpHistoryFrames(v.viewedNode);
    // The instances whose strand the view has (what resolve would place), counted without placing them.
    // Counted once per result (a host asks for stats after every evaluation, and between them).
    if (strands && strands->instances() && !strands->instances()->empty()) {
        if (instancesCountedOn_.lock() != strands || instancesCounted_.lock() != strands->instances()) {
            std::unordered_map<std::uint64_t, std::size_t> index;  // as InstanceSet::resolve finds a strand
            index.reserve(strands->strandCount());
            for (std::size_t s = 0; s < strands->strandCount(); ++s) index.emplace(strands->ids()[s], s);
            std::size_t count = 0;
            for (const auto& placed : strands->instances()->instances) {
                const auto it = index.find(placed.strand);
                if (it != index.end() && strands->strandPointCount(it->second) > 0) ++count;
            }
            instancesCountedOn_ = strands;
            instancesCounted_ = strands->instances();
            instanceCount_ = count;
        }
        out.instances = instanceCount_;
    }
    out.restEvaluations = restEvaluations();
    out.strokeDocument = sculptDoc_ != nullptr;
    out.simulations = simulationStatus();
    return out;
}

bool GroomSession::viewRefinesStrands() const {
    if (!doc_) return false;
    bool refines = false;
    doc_->graph.forEachActiveUpstream(doc_->output, [&](const std::string&, const Operator& op) {
        refines = refines || (op.typeName() == op::kSubdivide && op.params().getInt("viewportLevels") > 0);
        return !refines;
    });
    return refines;
}

std::vector<std::pair<std::string, double>> GroomSession::nodeTimes() const {
    std::vector<std::pair<std::string, double>> out;
    if (!doc_) return out;
    const auto& times = viewDoc()->graph.lastEvaluationTimes();
    std::map<std::string, double> byName;
    for (const auto& [name, id] : doc_->nodesByName) {
        auto it = times.find(id);
        if (it != times.end()) byName[name] = it->second;
    }
    foldCompoundTimes(flat_, byName);
    out.assign(byName.begin(), byName.end());
    return out;
}

std::vector<std::pair<std::string, std::string>> GroomSession::nodeNotes() const {
    std::vector<std::pair<std::string, std::string>> out;
    if (!doc_) return out;
    const auto& notes = viewDoc()->graph.lastNotes();
    std::map<std::string, std::string> byName;
    for (const auto& [name, id] : doc_->nodesByName) {
        auto it = notes.find(id);
        if (it != notes.end()) byName[name] = it->second;
    }
    foldCompoundNotes(flat_, byName);
    out.assign(byName.begin(), byName.end());
    return out;
}

std::size_t GroomSession::evaluationCount() const {
    // The view's and a live stroke's (both show the graph's result); not the stroke document's
    // evaluations ahead of a stroke (warmStroke).
    return (doc_ ? doc_->graph.evaluationCount() : 0) + (sculptDoc_ ? sculptDoc_->graph.evaluationCount() - warmEvaluations_ : 0) +
           releasedEvaluations_;
}

std::size_t GroomSession::incrementalCount() const {
    return (doc_ ? doc_->graph.incrementalCount() : 0) + (sculptDoc_ ? sculptDoc_->graph.incrementalCount() - warmIncrementals_ : 0) +
           releasedIncrementals_;
}

std::size_t GroomSession::restEvaluations() const { return restPose_ ? restPose_->evaluations() : 0; }

// ---- Simulation -----------------------------------------------------------------------------

namespace {

// Two frames of one cache (the same guides in order) blended: `a` toward `b` by w (the points
// and their wetness).
GuidesPtr blendFrames(const GuidesPtr& a, const GuidesPtr& b, float w) {
    if (!a || !b || a->size() != b->size()) return a;
    auto out = std::make_shared<GuideSet>(*a);
    for (std::size_t g = 0; g < out->size(); ++g) {
        Guide& guide = out->guides()[g];
        const Guide& next = b->guides()[g];
        std::vector<Vec3f>& points = guide.points;
        const std::vector<Vec3f>& to = next.points;
        if (to.size() != points.size()) continue;
        for (std::size_t j = 0; j < points.size(); ++j) points[j] = points[j] + (to[j] - points[j]) * w;
        const auto wet = guide.pointAttributes.find(kWetnessChannel);
        const auto wetTo = next.pointAttributes.find(kWetnessChannel);
        if (wet == guide.pointAttributes.end() || wetTo == next.pointAttributes.end() || wetTo->second.size() != wet->second.size()) {
            continue;
        }
        for (std::size_t j = 0; j < wet->second.size(); ++j) wet->second[j] += (wetTo->second[j] - wet->second[j]) * w;
    }
    return out;
}

// The frame at `seconds` and how far past it toward the next (0: on the frame).
int frameAt(double seconds, double fps, double* past) {
    const double at = secondsToFrame(seconds, fps);
    const int frame = static_cast<int>(std::floor(at + 1e-6));
    double w = at - frame;
    if (w < 1e-6) w = 0.0;
    if (past) *past = w;
    return frame;
}

}  // namespace

void GroomSession::setFramesPerSecond(double fps) {
    fps = canonicalFramesPerSecond(fps);  // 23.976 however a host spells it (frame_time.h)
    if (fps == fps_) return;
    fps_ = fps;
    for (auto& [name, node] : sims_) node->cache.clear();
}

std::vector<std::pair<std::string, NodeId>> GroomSession::simulateNodeIds(const std::string& viewNode) const {
    std::vector<std::pair<std::string, NodeId>> out;
    if (!doc_) return out;
    const Graph& graph = doc_->graph;
    // What feeds the output (or the node viewed), through every input.
    auto upstream = [&](NodeId from) {
        std::set<NodeId> reached;
        std::vector<NodeId> stack{from};
        while (!stack.empty()) {
            const NodeId id = stack.back();
            stack.pop_back();
            if (!graph.hasNode(id) || !reached.insert(id).second) continue;
            const std::size_t slots = graph.inputSpecs(id).size();
            for (std::size_t slot = 0; slot < slots; ++slot) {
                NodeId source = 0;
                if (graph.inputSource(id, slot, &source)) stack.push_back(source);
            }
        }
        return reached;
    };
    std::set<NodeId> reached = upstream(doc_->output);
    const auto viewed = doc_->nodesByName.find(flat_.result(viewNode));
    if (!viewNode.empty() && viewed != doc_->nodesByName.end()) {
        const std::set<NodeId> more = upstream(viewed->second);
        reached.insert(more.begin(), more.end());
    }
    // The Simulate nodes among them, those fed by more Simulate nodes later.
    std::vector<std::pair<std::size_t, std::pair<std::string, NodeId>>> found;
    for (const auto& [name, id] : doc_->nodesByName) {
        if (!reached.count(id) || graph.bypassed(id) || graph.node(id).typeName() != op::kSimulate) continue;
        std::size_t fedBy = 0;
        for (const NodeId other : upstream(id)) {
            if (other != id && graph.node(other).typeName() == op::kSimulate && !graph.bypassed(other)) ++fedBy;
        }
        found.push_back({fedBy, {name, id}});
    }
    std::sort(found.begin(), found.end());
    for (auto& f : found) out.push_back(std::move(f.second));
    return out;
}

std::vector<GroomSession::SimRef> GroomSession::simRefs(const std::string& viewNode) {
    std::vector<SimRef> refs;
    for (const auto& [name, id] : simulateNodeIds(viewNode)) {
        std::unique_ptr<SimNode>& node = sims_[name];
        if (!node) node = std::make_unique<SimNode>();
        node->cache.configure(SimSettings::fromParams(doc_->graph.node(id).params()), fps_);
        refs.push_back({name, id, node.get()});
    }
    // A node gone from the graph lets its frames go (a bake set for it stays).
    for (auto& [name, node] : sims_) {
        if (!doc_ || !doc_->nodesByName.count(name)) node->cache.clear();
    }
    return refs;
}

bool GroomSession::readsEarlierFrames(const std::string& viewNode, int firstFrame, int lastFrame) const {
    if (scalpHistoryFrames(viewNode) > 0) return true;
    for (const auto& [name, id] : simulateNodeIds(viewNode)) {
        const SimSettings settings = SimSettings::fromParams(doc_->graph.node(id).params());
        const int from = std::max(firstFrame, settings.startFrame);
        if (from > lastFrame) continue;  // it starts after them: the guides as groomed
        const auto it = sims_.find(name);
        const BakedSimulationPtr baked = it != sims_.end() ? it->second->baked : nullptr;
        const bool covered = baked && baked->read && (baked->stamp == 0 || baked->stamp == settings.hash()) &&
                             std::lround(secondsToFrame(baked->firstSeconds, fps_)) <= from &&
                             std::lround(secondsToFrame(baked->lastSeconds, fps_)) >= lastFrame;
        if (!covered) return true;
    }
    return false;
}

std::vector<std::string> GroomSession::simulateNodes() const {
    std::vector<std::string> out;
    for (const auto& [name, id] : simulateNodeIds({})) out.push_back(name);
    return out;
}

EvalContext GroomSession::simContext(double seconds, const ExternalInputs& externals) const {
    EvalContext ctx;
    ctx.previewDensity = simDensity_;
    ctx.maxPoints = simPointBudget_;
    ctx.time = seconds;
    ctx.framesPerSecond = fps_;
    ctx.scale = groomScale_;  // the lattices sizes round to, at the groom's scale
    ctx.externals = &externals;
    ctx.paths = paths();
    return ctx;
}

SimInputs GroomSession::simInputs(const SimRef& ref, const EvalContext& ctx, std::uint64_t* hash) {
    SimInputs in;
    Graph& graph = doc_->graph;
    NodeId source = 0;
    std::size_t output = 0;
    if (graph.inputSource(ref.id, 0, &source, &output)) in.guides = valueAs<GuidesPtr>(graph.evaluateOutput(source, output, ctx));
    if (graph.inputSource(ref.id, 1, &source, &output)) {
        if (MeshPtr mesh = valueAs<MeshPtr>(graph.evaluateOutput(source, output, ctx))) {
            in.colliders.push_back(std::move(mesh));
            // A ColliderMesh node's own friction (negative: the Simulate node's).
            const Operator& from = graph.node(source);
            in.colliderFriction.push_back(from.typeName() == op::kColliderMesh
                                              ? static_cast<float>(from.params().getDouble("friction", -1.0))
                                              : -1.0f);
        }
    }
    if (graph.inputSource(ref.id, 2, &source, &output)) in.water = valueAs<MeshPtr>(graph.evaluateOutput(source, output, ctx));
    in.scalp = scalp_;
    // Following the scalp the graph runs on the rest pose: its guides are carried to the scalp as it
    // is, where the head moves them (the frames go back to the rest pose for the graph, simFrameAt).
    if (following_ && in.guides && followRest_ && scalp_) in.guides = carryGuidesBack(in.guides, *scalp_, *followRest_);
    if (!in.guides) in.guides = std::make_shared<const GuideSet>();
    // The mappable params read on the guides (painted layers, textures, expressions, the fields
    // plugged into its exposed params, evaluated here as the graph would for the node).
    {
        ParamSet params = graph.node(ref.id).params();
        const std::vector<std::string>& exposed = graph.paramInputs(ref.id);
        const std::size_t own = graph.node(ref.id).inputs().size();
        for (std::size_t k = 0; k < exposed.size(); ++k) {
            if (!graph.inputSource(ref.id, own + k, &source, &output)) continue;
            const Value value = graph.evaluateOutput(source, output, ctx);
            if (FieldPtr field = valueAs<FieldPtr>(value)) params.setField(exposed[k], field);
            const UniformPtr uniform = valueAs<UniformPtr>(value);  // a number in a Field slot (one in a Value slot is set)
            if (uniform && !graph.paramDriven(ref.id, exposed[k])) {
                if (FieldPtr field = uniformField(*uniform)) params.setField(exposed[k], field);
            }
        }
        in.maps = simulationMaps(params, *in.guides, in.scalp);
    }
    if (hash) {
        std::uint64_t h = hashCombine(hash64(0x51A71), in.guides->contentHash());
        h = hashCombine(h, in.scalp ? in.scalp->surfaceHash() : 0);
        for (const MeshPtr& mesh : in.colliders) h = hashCombine(h, mesh->surfaceHash());
        if (in.water) h = hashCombine(hashCombine(h, 0x3A7E), in.water->surfaceHash());
        // Each collider's own friction and the maps, when there are any (else the hash made before them).
        for (const float f : in.colliderFriction) {
            if (f >= 0.0f) h = hashFloat(hashCombine(h, 0xF71C), f);
        }
        if (in.maps) h = hashCombine(h, in.maps->hash());
        *hash = h;
    }
    return in;
}

bool GroomSession::bakeUsable(const SimNode& node, double seconds) const {
    const BakedSimulationPtr& baked = node.baked;
    if (!baked || !baked->read) return false;
    if (baked->stamp != 0 && baked->stamp != node.cache.settings().hash()) return false;  // made with other settings
    const double slack = 0.5 / fps_;
    return seconds >= baked->firstSeconds - slack && seconds <= baked->lastSeconds + slack;
}

std::pair<GuidesPtr, std::uint64_t> GroomSession::simFrameAt(const SimNode& node, double seconds) const {
    double w = 0.0;
    const int f0 = frameAt(seconds, fps_, &w);
    if (f0 < node.cache.startFrame()) return {};  // before the start: the guides as groomed
    GuidesPtr frame;
    std::uint64_t hash = 0;
    if (bakeUsable(node, seconds)) {
        frame = node.baked->read(seconds);
        hash = frame ? hashCombine(hashCombine(hash64(0xBA4ED), frame->contentHash()), node.baked->stamp) : 0;
    } else if (node.cache.has(f0)) {
        frame = node.cache.frame(f0);
        hash = node.cache.frameHash(f0);
        if (w > 0.0 && node.cache.has(f0 + 1)) {
            frame = blendFrames(frame, node.cache.frame(f0 + 1), static_cast<float>(w));
            hash = hashFloat(hashCombine(hash, node.cache.frameHash(f0 + 1)), static_cast<float>(w));
        }
    }
    if (frame && following_ && followRest_ && scalp_) {
        if (bakeUsable(node, seconds)) {
            // A bake's file keeps no roots: each guide's is its hierarchy guide's, or for a guide
            // the graph made (ToGuides, ImportCurves: an ID the hierarchy has not) the point of the
            // scalp as it is nearest its first point, where it is rooted. (Without one it was not
            // carried back to the rest pose: the strands it shapes stood off the moving head by the
            // head's motion.)
            auto rooted = std::make_shared<GuideSet>(*frame);
            for (Guide& g : rooted->guides()) {
                if (const HierarchyGuide* h = hierarchy_ ? hierarchy_->guide(static_cast<GuideId>(g.id)) : nullptr) {
                    g.root = h->root;
                } else if (!g.points.empty()) {
                    g.root = scalp_->closestPoint(g.points.front()).root;
                }
            }
            frame = rooted;
        }
        frame = carryGuidesBack(frame, *followRest_, *scalp_);
        hash = hashCombine(hash, scalp_->surfaceHash());
    }
    return {frame, hash};
}

void GroomSession::checkSimInputs(SimRef& ref, double seconds, const ExternalInputs& externals) {
    SimNode& node = *ref.node;
    std::uint64_t hash = 0;
    simInputs(ref, simContext(seconds, externals), &hash);
    double w = 0.0;
    const int f = frameAt(seconds, fps_, &w);
    if (w == 0.0 && node.cache.has(f) && node.cache.inputHash(f) != 0) {
        if (hash != node.cache.inputHash(f)) node.cache.clear();
    } else if (node.checked && node.checkedSeconds == seconds && hash != node.checkedHash) {
        node.cache.clear();
    }
    node.checked = true;
    node.checkedSeconds = seconds;
    node.checkedHash = hash;
}

void GroomSession::stepSimulations(const ViewRequest& request, std::vector<std::string>& errors) {
    for (auto& [name, node] : sims_) {
        node->shown.reset();
        node->shownHash = 0;
    }
    // A live brush stroke shows the guides as groomed (the frames stay for after it).
    if (!doc_ || hasPreview()) return;
    std::vector<SimRef> refs = simRefs(request.viewNode);
    if (refs.empty()) return;
    simDensity_ = request.density;
    simPointBudget_ = request.pointBudget;
    const double t = request.time;
    double w = 0.0;
    const int f0 = frameAt(t, fps_, &w);
    const int f1 = w > 0.0 ? f0 + 1 : f0;
    try {
        // Node by node, upstream first: each one's frame is in the externals of those it feeds.
        for (SimRef& ref : refs) {
            SimNode& node = *ref.node;
            if (f0 >= node.cache.startFrame() && !bakeUsable(node, t)) {
                // What it was fed changed since (a groom edit, another animation): it starts again
                // from its start frame.
                {
                    ExternalInputs externals;
                    fillExternals(externals);
                    checkSimInputs(ref, t, externals);
                }
                if (simStepping_ && node.cache.settings().live && !(node.cache.has(f0) && node.cache.has(f1))) {
                    std::vector<SimRef> one{ref};
                    SimRun run;
                    advanceSimulations(one, f1, t, {}, run);
                    for (const std::string& e : run.errors) errors.push_back(e);
                }
            }
            std::tie(node.shown, node.shownHash) = simFrameAt(node, t);
        }
    } catch (const std::exception& e) {
        errors.push_back(std::string("simulation: ") + e.what());
    }
}

void GroomSession::advanceSimulations(std::vector<SimRef>& nodes, int target, double nowSeconds, const SimProgress& progress,
                                      SimRun& run) {
    const auto started = std::chrono::steady_clock::now();
    auto nextFrame = [](const SimNode& n) { return n.cache.started() ? n.cache.lastFrame() + 1 : n.cache.startFrame(); };
    int from = INT_MAX;
    for (const SimRef& ref : nodes) from = std::min(from, nextFrame(*ref.node));
    if (nodes.empty() || from > target) return;
    const int count = target - from + 1;
    auto stepError = [&](const SimRef& ref) {
        run.errors.push_back("Simulate '" + ref.name + "': its guides changed (another count, other IDs or points per "
                             "guide); it starts again from frame " + std::to_string(ref.node->cache.startFrame()));
    };

    // The inputs as they are (bound at nowSeconds), put back after the frames the feed set.
    struct Moving {
        MeshPtr scalp;
        NamedScalps scalps;
        GuidesPtr guides, poses;
        std::uint64_t posesHash = 0;
        std::map<std::string, MeshPtr> meshes;
        std::map<std::string, float> controls;
        ScalpHistoryPtr history;
    };
    const Moving saved{data_.scalp, data_.scalps, data_.guides, data_.guidePoses, posesHash_, data_.meshes, data_.controls, data_.scalpHistory};
    bool fed = false;
    std::vector<std::string> scratchErrors;
    std::vector<SessionMessage> scratchMessages;
    auto restore = [&] {
        if (!fed) return;
        data_.scalp = saved.scalp;
        data_.scalps = saved.scalps;
        data_.guides = saved.guides;
        data_.guidePoses = saved.poses;
        posesHash_ = saved.posesHash;
        data_.meshes = saved.meshes;
        data_.controls = saved.controls;
        data_.scalpHistory = saved.history;
        fed = false;
        scratchErrors.clear();
        scratchMessages.clear();
        prepareTime(nowSeconds, scratchErrors, scratchMessages);
    };
    try {
        for (int f = from; f <= target; ++f) {
            const double seconds = frameToSeconds(f, fps_);
            // The time the session is bound at, no feed, or a feed that cannot give that time
            // now: the inputs as they are. Else the feed's for that time.
            const bool now = std::abs(seconds - nowSeconds) <= 1e-9 * std::max(1.0, std::abs(nowSeconds));
            bool known = now;  // the inputs are the host's at f
            if (now || !simFeed_) {
                restore();
            } else {
                const bool wasFed = fed;
                fed = true;
                if (simFeed_(seconds)) {
                    known = true;
                    scratchErrors.clear();
                    scratchMessages.clear();
                    prepareTime(seconds, scratchErrors, scratchMessages);
                } else {
                    fed = wasFed;  // it set nothing: back to the inputs as they are, if a frame's are set
                    restore();
                }
            }
            ExternalInputs externals;
            fillExternals(externals);
            // Every Simulate node's frame at f (none: its guides pass through).
            for (const auto& [name, node] : sims_) {
                const auto [frame, frameHash] = simFrameAt(*node, seconds);
                externals.set(simulationExternal(name), frame ? Value(frame) : Value{}, frameHash);
            }
            const EvalContext ctx = simContext(seconds, externals);
            for (SimRef& ref : nodes) {
                SimNode& node = *ref.node;
                const int next = nextFrame(node);
                if (next > f) continue;
                std::uint64_t hash = 0;
                const SimInputs in = simInputs(ref, ctx, &hash);
                // A frame whose inputs are not known (no feed, or one that cannot give them: Maya
                // while it plays, the frames real-time playback skips) is stepped toward the
                // inputs as they are. Other inputs than its last frame was stepped toward (the
                // head moved since): it waits for the next frame whose inputs are known (or the
                // last), and the frames between are stepped in one go toward those, the roots
                // moving evenly over them (each stepped toward the inputs shown put the head's
                // whole motion into the first of them: the hair whipped). Frames stepped toward
                // inputs not known are stamped 0 (not checked): with the head's inputs as shown
                // stamped on them, a later view of one (not playing: the feed giving its own)
                // found other inputs and threw every frame away, and what was simulated again,
                // and baked, was not what had played.
                if (node.cache.started() && !known && f < target && hash != node.towardHash) continue;
                const std::uint64_t stamp = known ? hash : 0;
                if (!node.cache.started()) {
                    if (next != f) continue;
                    node.cache.start(in, stamp);
                } else if (!node.cache.step(in, stamp, f - next + 1)) {
                    stepError(ref);
                    continue;
                }
                node.towardHash = hash;
                const auto [frame, frameHash] = simFrameAt(node, seconds);
                externals.set(simulationExternal(ref.name), frame ? Value(frame) : Value{}, frameHash);
            }
            ++run.frames;
            if (progress && !progress(f - from + 1, count)) {
                run.cancelled = true;
                break;
            }
        }
    } catch (...) {
        restore();
        throw;
    }
    restore();
    run.milliseconds += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

SimRun GroomSession::simulateTo(int frame, const SimProgress& progress) {
    SimRun run;
    if (!doc_) {
        run.errors.push_back(graphError_.empty() ? "the groom has no graph" : graphError_);
        return run;
    }
    // Bound at the last view's time: what the frames on the way are put back to.
    std::vector<std::string> errors;
    std::vector<SessionMessage> messages;
    prepareTime(time_, errors, messages);
    std::vector<SimRef> refs = simRefs({});
    try {
        // A node whose inputs at the time shown changed since starts again.
        ExternalInputs externals;
        fillExternals(externals);
        for (SimRef& ref : refs) {
            checkSimInputs(ref, time_, externals);
        }
        advanceSimulations(refs, frame, time_, progress, run);
    } catch (const std::exception& e) {
        run.errors.push_back(std::string("simulation: ") + e.what());
    }
    return run;
}

void GroomSession::invalidateSimulation(const std::string& node) {
    for (auto& [name, sim] : sims_) {
        if (node.empty() || name == node) {
            sim->cache.clear();
            sim->shown.reset();
            sim->shownHash = 0;
        }
    }
}

std::vector<SimStatus> GroomSession::simulationStatus() const {
    std::vector<SimStatus> out;
    if (!doc_) return out;
    for (const auto& [name, id] : doc_->nodesByName) {
        const Operator& op = doc_->graph.node(id);
        if (op.typeName() != op::kSimulate) continue;
        const SimSettings settings = SimSettings::fromParams(op.params());
        SimStatus s;
        s.node = name;
        s.startFrame = settings.startFrame;
        s.live = settings.live;
        s.stamp = settings.hash();
        const auto it = sims_.find(name);
        if (it != sims_.end()) {
            const SimNode& node = *it->second;
            if (node.cache.settings().hash() == s.stamp) {  // frames of these settings (else none until stepped)
                s.ranges = node.cache.ranges();
                s.frames = node.cache.frameCount();
                s.stepMs = node.cache.lastStepMs();
                s.bytes = node.cache.bytes();
            }
            if (node.baked && node.baked->read) {
                const bool matches = node.baked->stamp == 0 || node.baked->stamp == s.stamp;
                s.baked = matches;
                s.bakeStale = !matches;
                s.bakedFirst = static_cast<int>(std::lround(secondsToFrame(node.baked->firstSeconds, fps_)));
                s.bakedLast = static_cast<int>(std::lround(secondsToFrame(node.baked->lastSeconds, fps_)));
                s.bakedFile = node.baked->file;
            }
        }
        out.push_back(std::move(s));
    }
    return out;
}

GuidesPtr GroomSession::simulatedFrame(const std::string& node, int frame) const {
    const auto it = sims_.find(node);
    return it == sims_.end() ? nullptr : it->second->cache.frame(frame);
}

void GroomSession::setBakedSimulation(const std::string& node, BakedSimulationPtr baked) {
    std::unique_ptr<SimNode>& sim = sims_[node];
    if (!sim) sim = std::make_unique<SimNode>();
    sim->baked = std::move(baked);
}

BakedSimulationPtr GroomSession::bakedSimulation(const std::string& node) const {
    const auto it = sims_.find(node);
    return it == sims_.end() ? nullptr : it->second->baked;
}

std::uint64_t GroomSession::simulationStamp(const std::string& node) const {
    if (!doc_) return 0;
    const auto it = doc_->nodesByName.find(node);
    if (it == doc_->nodesByName.end() || doc_->graph.node(it->second).typeName() != op::kSimulate) return 0;
    return SimSettings::fromParams(doc_->graph.node(it->second).params()).hash();
}

}  // namespace groomlab
