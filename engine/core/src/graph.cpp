// SPDX-License-Identifier: Apache-2.0
#include "groomlab/graph.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>
#include <unordered_map>

#include <type_traits>

#include "groomlab/field.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/preview_core.h"
#include "groomlab/random.h"
#include "groomlab/texture.h"
#include "operators/internal.h"

namespace groomlab {

namespace {

std::uint64_t hashBytes(std::uint64_t seed, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) seed = hashCombine(seed, bytes[i]);
    return seed;
}

std::uint64_t hashParam(std::uint64_t seed, const ParamValue& value) {
    seed = hashCombine(seed, value.index());
    return std::visit(
        [seed](const auto& v) -> std::uint64_t {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                return hashString(seed, v);
            } else if constexpr (std::is_same_v<T, Vec3f>) {
                const float xyz[3] = {v.x, v.y, v.z};
                return hashBytes(seed, xyz, sizeof xyz);
            } else if constexpr (std::is_same_v<T, MapRef>) {
                std::uint64_t h = hashCombine(seed, static_cast<std::uint64_t>(v.source));
                h = hashString(h, v.name);
                h = hashCombine(h, static_cast<std::uint64_t>(v.component));
                const float range[2] = {v.min, v.max};
                h = hashBytes(h, range, sizeof range);
                h = hashCombine(h, v.constants.size());
                for (const auto& [name, value] : v.constants) h = hashFloat(hashString(h, name), value);
                return h;
            } else if constexpr (std::is_same_v<T, Ramp>) {
                std::uint64_t h = hashCombine(seed, v.keys.size() * 2 + (v.straight ? 1 : 0));
                for (const auto& [t, value] : v.keys) {
                    const float kv[2] = {t, value};
                    h = hashBytes(h, kv, sizeof kv);
                }
                return h;
            } else {
                return hashBytes(seed, &v, sizeof v);
            }
        },
        value);
}

template <class T>
const T* paramAs(const ParamSet& params, const std::string& name) {
    if (!params.has(name)) return nullptr;
    return std::get_if<T>(&params.get(name));
}

}  // namespace

// ---------------------------------------------------------------------------

const char* portTypeName(PortType type) {
    switch (type) {
        case PortType::Strands: return "Strands";
        case PortType::Guides: return "Guides";
        case PortType::Mesh: return "Mesh";
        case PortType::Field: return "Field";
        case PortType::Curve: return "Curve";
        case PortType::Curves: return "Curves";
        case PortType::Value: return "Value";
    }
    return "Unknown";
}

PortType portTypeFromName(const std::string& name) {
    for (const PortType type : {PortType::Strands, PortType::Guides, PortType::Mesh, PortType::Field, PortType::Curve,
                                PortType::Curves, PortType::Value}) {
        if (name == portTypeName(type)) return type;
    }
    throw GraphError("unknown port type '" + name + "' (Strands, Guides, Mesh, Field, Curve, Curves or Value)");
}

std::uint64_t mapParamsKey(const ParamSet& params, const EvalContext& ctx) {
    std::uint64_t key = 0;
    for (const auto& [name, value] : params.values()) {
        // An expression reading the time or a map (expressionSalt; 0, and the key as ever, otherwise).
        if (const std::string* text = std::get_if<std::string>(&value); text && name == "expression") {
            if (const std::uint64_t salt = expressionSalt(*text, ctx)) key = hashCombine(key, salt);
            continue;
        }
        const MapRef* map = std::get_if<MapRef>(&value);
        if (map && map->source == MapRef::Source::Expression) {
            if (const std::uint64_t salt = expressionSalt(map->name, ctx)) key = hashCombine(key, salt);
        }
        if (!map || map->source == MapRef::Source::Attribute) continue;
        if (map->source == MapRef::Source::Texture) key = hashCombine(key, fileStamp(resolvePath(map->name)));
        // The scalp's key is its surface; a color set read from it adds its own hash.
        if (map->source == MapRef::Source::ColorSet) key = hashCombine(key, colorSetSalt(ctx, map->name));
        const ExternalInputs::Entry* scalp = ctx.externals ? ctx.externals->find("scalp") : nullptr;
        key = hashCombine(key, scalp ? scalp->hash : 0);
    }
    return key;
}

// ---------------------------------------------------------------------------

struct EvalContext::NoteSink {
    std::mutex mutex;
    std::vector<std::string> notes;
};

void EvalContext::note(const std::string& text) const {
    NoteSink* sink = noteSink;
    if (!sink || text.empty()) return;
    std::lock_guard<std::mutex> lock(sink->mutex);
    if (std::find(sink->notes.begin(), sink->notes.end(), text) == sink->notes.end()) sink->notes.push_back(text);
}

namespace {
// Collects the notes of the operator call it is alive around (the context's sink, restored after:
// an operator evaluating a graph of its own, a Compound, sets it for its inner nodes).
class NoteScope {
public:
    explicit NoteScope(const EvalContext& ctx) : ctx_(ctx), saved_(ctx.noteSink) { ctx.noteSink = &sink_; }
    ~NoteScope() { ctx_.noteSink = saved_; }
    NoteScope(const NoteScope&) = delete;
    NoteScope& operator=(const NoteScope&) = delete;
    std::string text() const {
        std::string out;
        for (const std::string& n : sink_.notes) out += (out.empty() ? "" : "; ") + n;
        return out;
    }

private:
    const EvalContext& ctx_;
    EvalContext::NoteSink* saved_;
    EvalContext::NoteSink sink_;
};
}  // namespace

const ParamValue& ParamSet::get(const std::string& name) const {
    noteRead(name);
    auto it = values_.find(name);
    if (it == values_.end()) throw GraphError("unknown parameter '" + name + "'");
    return it->second;
}

const ParamValue* ParamSet::defaultValue(const std::string& name) const {
    noteRead(name);
    if (!defaults_) return nullptr;
    auto it = defaults_->find(name);
    return it == defaults_->end() ? nullptr : &it->second;
}

namespace {
// The spec default of `name` when it is a T.
template <class T>
const T* defaultAs(const ParamSet& params, const std::string& name) {
    const ParamValue* v = params.defaultValue(name);
    return v ? std::get_if<T>(v) : nullptr;
}
// The stored value when it is a T, else the spec default when it is one, else the fallback.
template <class T>
T paramOr(const ParamSet& params, const std::string& name, const T& fallback) {
    if (const T* v = paramAs<T>(params, name)) return *v;
    if (const T* d = defaultAs<T>(params, name)) return *d;
    return fallback;
}
}  // namespace

double ParamSet::clamped(const std::string& name, double value) const {
    if (!limits_) return value;
    auto it = limits_->find(name);
    if (it == limits_->end()) return value;
    const Limits& l = it->second;
    if (l.min && !(value >= *l.min)) value = *l.min;  // NaN too
    if (l.max && !(value <= *l.max)) value = *l.max;
    return value;
}

bool ParamSet::getBool(const std::string& name, bool fallback) const { return paramOr(*this, name, fallback); }

std::int64_t ParamSet::getInt(const std::string& name, std::int64_t fallback) const {
    const std::int64_t v = paramOr(*this, name, fallback);
    if (!limits_ || !limits_->count(name)) return v;
    const Limits& l = limits_->at(name);
    if (l.min && static_cast<double>(v) < *l.min) return static_cast<std::int64_t>(std::ceil(*l.min));
    if (l.max && static_cast<double>(v) > *l.max) return static_cast<std::int64_t>(std::floor(*l.max));
    return v;
}

double ParamSet::getDouble(const std::string& name, double fallback) const {
    if (const double* v = paramAs<double>(*this, name)) return clamped(name, *v);
    if (const std::int64_t* i = paramAs<std::int64_t>(*this, name)) return clamped(name, static_cast<double>(*i));
    if (const double* d = defaultAs<double>(*this, name)) return *d;
    if (const std::int64_t* i = defaultAs<std::int64_t>(*this, name)) return static_cast<double>(*i);
    return fallback;
}

std::string ParamSet::getString(const std::string& name, const std::string& fallback) const {
    return paramOr(*this, name, fallback);
}

Vec3f ParamSet::getVec3(const std::string& name, Vec3f fallback) const { return paramOr(*this, name, fallback); }

namespace {
// Curves live in 0-1 on both axes: points outside (typed in, scripted, older
// scenes) are brought back in, and a NaN key becomes 0.
Ramp unitRamp(Ramp r) {
    auto unit = [](float x) { return x > 0.0f ? (x < 1.0f ? x : 1.0f) : 0.0f; };
    for (auto& key : r.keys) key = {unit(key.first), unit(key.second)};
    std::stable_sort(r.keys.begin(), r.keys.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return r;
}
}  // namespace

Ramp ParamSet::getRamp(const std::string& name, const Ramp& fallback) const {
    noteRead(name);
    if (auto it = curves_.find(name); it != curves_.end() && it->second) return unitRamp(*it->second);
    if (const Ramp* r = paramAs<Ramp>(*this, name)) return unitRamp(*r);
    if (const double* d = paramAs<double>(*this, name)) return unitRamp(Ramp::constant(static_cast<float>(*d)));
    if (const std::int64_t* i = paramAs<std::int64_t>(*this, name)) {
        return unitRamp(Ramp::constant(static_cast<float>(*i)));
    }
    if (const Ramp* r = defaultAs<Ramp>(*this, name)) return unitRamp(*r);
    if (const double* d = defaultAs<double>(*this, name)) return unitRamp(Ramp::constant(static_cast<float>(*d)));
    return fallback;
}

std::uint64_t ParamSet::hash() const {
    std::uint64_t h = hash64(values_.size());
    for (const auto& [name, value] : values_) {
        h = hashString(h, name);
        h = hashParam(h, value);
    }
    return h;
}

std::shared_ptr<const ScalpHistory> ScalpHistory::make(double step, std::vector<std::vector<Vec3f>> frames,
                                                       std::vector<double> loopSeconds) {
    auto history = std::make_shared<ScalpHistory>();
    history->step = step;
    if (loopSeconds.size() > frames.size()) loopSeconds.resize(frames.size());
    std::uint64_t h = hashBytes(hash64(0x5CA1B), &step, sizeof step);
    h = hashCombine(h, frames.size());
    for (const std::vector<Vec3f>& points : frames) {
        h = hashCombine(h, points.size());
        for (const Vec3f& p : points) h = hashFloat(hashFloat(hashFloat(h, p.x), p.y), p.z);
    }
    if (!loopSeconds.empty()) {
        h = hashCombine(h, loopSeconds.size());
        for (const double t : loopSeconds) h = hashBytes(h, &t, sizeof t);
    }
    history->frames = std::move(frames);
    history->loopSeconds = std::move(loopSeconds);
    history->hash = h;
    return history;
}

std::uint64_t ExternalInputs::hash() const {
    std::uint64_t h = hash64(0xE7E5);
    for (const auto& [name, entry] : entries_) h = hashCombine(hashString(h, name), entry.hash);
    return h;
}

std::uint64_t EvalContext::hash() const {
    // Time is not part of the key: anything that changes with time (a deforming
    // scalp, guide poses, correctives, controls) comes in as an external input
    // with its own hash, so a static groom stays cached across frames. The few
    // operators that read time or buildCards (ImportCurves, Cards) salt their
    // own keys with it (Operator::contextSalt).
    std::uint64_t h = hashBytes(0, &previewDensity, sizeof previewDensity);
    h = hashBytes(h, &maxPoints, sizeof maxPoints);
    if (scale != 1.0) h = hashBytes(h, &scale, sizeof scale);
    return h;
}

// ---------------------------------------------------------------------------

OperatorRegistry& OperatorRegistry::instance() {
    static OperatorRegistry registry = [] {
        OperatorRegistry r;
        registerBuiltinOperators(r);
        return r;
    }();
    return registry;
}

void OperatorRegistry::registerType(const std::string& typeName, NodeInfo info, Factory factory) {
    types_[typeName] = {std::move(factory), std::move(info)};
}

bool OperatorRegistry::contains(const std::string& typeName) const {
    return types_.count(typeName) != 0;
}

std::unique_ptr<Operator> OperatorRegistry::create(const std::string& typeName) const {
    auto it = types_.find(typeName);
    if (it == types_.end()) throw GraphError("unknown operator type '" + typeName + "'");
    std::unique_ptr<Operator> op = it->second.factory();
    op->applyDefaults();
    return op;
}

std::vector<std::string> OperatorRegistry::typeNames() const {
    std::vector<std::string> names;
    for (const auto& [name, type] : types_) names.push_back(name);
    return names;
}

const NodeInfo* OperatorRegistry::info(const std::string& typeName) const {
    const auto it = types_.find(typeName);
    return it == types_.end() ? nullptr : &it->second.info;
}

// ---------------------------------------------------------------------------

NodeId Graph::addNode(std::unique_ptr<Operator> op) {
    if (!op) throw GraphError("addNode: null operator");
    op->applyDefaults();
    const NodeId id = nextId_++;
    Node node;
    node.resizeInputs(op->inputs().size());
    node.op = std::move(op);
    nodes_.emplace(id, std::move(node));
    return id;
}

NodeId Graph::addNode(const std::string& typeName) {
    return addNode(OperatorRegistry::instance().create(typeName));
}

void Graph::removeNode(NodeId id) {
    lastTimes_.erase(id);
    lastRunKinds_.erase(id);
    lastNotes_.erase(id);
    nodeRecord(id);  // throws if missing
    nodes_.erase(id);
    for (auto& [otherId, node] : nodes_) {
        bool fed = false;
        for (std::size_t slot = 0; slot < node.inputs.size(); ++slot) {
            if (node.inputs[slot] != id) continue;
            node.inputs[slot] = kNoNode;
            node.inputOutputs[slot] = 0;
            fed = true;
        }
        if (fed) releaseDriven(node);
    }
}

std::vector<NodeId> Graph::nodeIds() const {
    std::vector<NodeId> ids;
    for (const auto& [id, node] : nodes_) ids.push_back(id);
    return ids;
}

Operator& Graph::node(NodeId id) { return *nodeRecord(id).op; }
const Operator& Graph::node(NodeId id) const { return *nodeRecord(id).op; }

Graph::Node& Graph::nodeRecord(NodeId id) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) throw GraphError("unknown node " + std::to_string(id));
    return it->second;
}

const Graph::Node& Graph::nodeRecord(NodeId id) const {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) throw GraphError("unknown node " + std::to_string(id));
    return it->second;
}

bool Graph::forEachActiveUpstream(NodeId id, const ActiveNodeVisitor& visit, const std::string& prefix) const {
    std::vector<NodeId> seen;
    std::function<bool(NodeId)> walk = [&](NodeId n) -> bool {
        if (n == kNoNode || !nodes_.count(n) || std::find(seen.begin(), seen.end(), n) != seen.end()) return true;
        seen.push_back(n);
        const Node& node = nodeRecord(n);
        const auto specs = node.op->inputs();
        // Bypassed, it hands its first input on (as evaluateWithKeys does): only that runs.
        const bool passThrough = node.bypass && !specs.empty() && specs[0].type == node.op->output().type &&
                                 !node.inputs.empty() && node.inputs[0] != kNoNode;
        if (passThrough) return walk(node.inputs[0]);
        const std::string path = prefix + node.label;
        if (!visit(path, *node.op)) return false;
        if (!node.op->forEachActiveInner(path.empty() ? prefix : path + "/", visit)) return false;
        for (std::size_t i = 0; i < specs.size() && i < node.inputs.size(); ++i) {
            if (specs[i].type != PortType::Strands && specs[i].type != PortType::Curves) continue;
            if (!node.op->inputActive(i)) continue;
            if (!walk(node.inputs[i])) return false;
        }
        return true;
    };
    return walk(id);
}

bool Graph::reaches(NodeId from, NodeId target) const {
    // Depth-first over inputs, each node once (diamonds would otherwise be walked again and again).
    std::vector<NodeId> stack{from};
    std::vector<NodeId> seen;
    while (!stack.empty()) {
        const NodeId id = stack.back();
        stack.pop_back();
        if (id == target) return true;
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
        seen.push_back(id);
        for (NodeId src : nodeRecord(id).inputs) {
            if (src != kNoNode) stack.push_back(src);
        }
    }
    return false;
}

bool portTypesMatch(PortType out, PortType in) {
    if (out == in) return true;
    if (out == PortType::Value && in == PortType::Field) return true;  // a number on every strand
    auto curve = [](PortType t) { return t == PortType::Strands || t == PortType::Guides || t == PortType::Curves; };
    return (in == PortType::Curves && curve(out)) || (out == PortType::Curves && curve(in));
}

std::size_t Graph::outputIndex(NodeId id, const std::string& name) const {
    const auto outs = outputSpecs(id);
    std::string names;
    for (std::size_t i = 0; i < outs.size(); ++i) {
        if (outs[i].name == name) return i;
        names += (i ? ", " : "") + outs[i].name;
    }
    throw GraphError(node(id).typeName() + " has no output '" + name + "' (" + names + ")");
}

PortType Graph::outputType(NodeId id, std::size_t output) const {
    for (int depth = 0; depth < 1000; ++depth) {  // a chain of Curves nodes: follow it up
        const Node& node = nodeRecord(id);
        const auto outs = node.op->outputs();
        if (output >= outs.size()) {
            throw GraphError(node.op->typeName() + " has no output " + std::to_string(output));
        }
        const PortType out = outs[output].type;
        if (out != PortType::Curves) return out;
        const auto specs = node.op->inputs();
        NodeId source = kNoNode;
        std::size_t sourceOutput = 0;
        for (std::size_t i = 0; i < specs.size() && i < node.inputs.size(); ++i) {
            if (specs[i].type == PortType::Curves) {
                source = node.inputs[i];
                sourceOutput = node.inputOutputs[i];
                break;
            }
        }
        if (source == kNoNode) return PortType::Curves;
        id = source;
        output = sourceOutput;
    }
    return PortType::Curves;
}

// Guides as strands for a Curves node, and back: IDs, roots and points, painted
// maps as strand channels and per-point layers as point channels (and the
// channels back as maps and layers). A guide without a layer, or with one of
// the wrong length, reads 0 along it.
StrandsPtr guidesAsStrands(const GuideSet& guides) {
    auto out = std::make_shared<StrandSet>();
    std::vector<std::string> names, layers;
    for (const Guide& g : guides.guides()) {
        for (const auto& [name, value] : g.attributes) {
            if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
        }
        for (const auto& [name, values] : g.pointAttributes) {
            if (std::find(layers.begin(), layers.end(), name) == layers.end()) layers.push_back(name);
        }
    }
    std::vector<std::vector<float>*> columns, pointColumns;
    for (const std::string& name : names) columns.push_back(&out->addStrandChannel(name, 0.0f));
    for (const std::string& name : layers) pointColumns.push_back(&out->addPointChannel(name, 0.0f));
    for (const Guide& g : guides.guides()) {
        const std::size_t s = out->addStrand(g.id, g.points, {}, g.root);
        for (std::size_t c = 0; c < names.size(); ++c) {
            auto it = g.attributes.find(names[c]);
            (*columns[c])[s] = it == g.attributes.end() ? 0.0f : it->second;
        }
        const std::size_t begin = out->strandBegin(s);
        for (std::size_t c = 0; c < layers.size(); ++c) {
            auto it = g.pointAttributes.find(layers[c]);
            if (it == g.pointAttributes.end() || it->second.size() != g.points.size()) continue;
            std::copy(it->second.begin(), it->second.end(), pointColumns[c]->begin() + static_cast<std::ptrdiff_t>(begin));
        }
    }
    return out;
}

GuidesPtr strandsAsGuides(const StrandSet& strands) {
    auto out = std::make_shared<GuideSet>();
    std::vector<std::string> names = strands.strandChannelNames();
    names.erase(std::remove(names.begin(), names.end(), std::string(kPreviewChannel)), names.end());
    const std::vector<std::string> layers = strands.pointChannelNames();
    // Each guide made on its own (in strand order, as GuideSet::add appends them).
    std::vector<Guide>& guides = out->guides();
    guides.resize(strands.strandCount());
    parallelFor(0, guides.size(), [&](std::size_t s) {
        const Vec3f* pts = strands.strandPoints(s);
        const std::size_t n = strands.strandPointCount(s), begin = strands.strandBegin(s);
        Guide& g = guides[s];
        g.id = strands.ids()[s];
        g.points.assign(pts, pts + n);
        g.root = strands.roots()[s];
        for (const std::string& name : names) g.attributes[name] = (*strands.strandChannel(name))[s];
        for (const std::string& name : layers) {
            const float* values = strands.pointChannel(name)->data() + begin;
            g.pointAttributes[name].assign(values, values + n);
        }
    }, 64);
    return out;
}

namespace {

const char* valueKind(const Value& v) {
    if (valueAs<StrandsPtr>(v)) return "strands";
    if (valueAs<GuidesPtr>(v)) return "guides";
    return "nothing";
}

}  // namespace

std::vector<PortSpec> Graph::inputSpecs(NodeId id) const {
    const Node& node = nodeRecord(id);
    std::vector<PortSpec> specs = node.op->inputs();
    if (node.paramInputs.empty()) return specs;
    const auto params = nodeParamSpecs(*node.op);
    for (const std::string& p : node.paramInputs) {
        const auto it = std::find_if(params.begin(), params.end(), [&](const ParamSpec& s) { return s.name == p; });
        PortSpec port{p, PortType::Field, true};  // a param its node no longer has (a compound's definition changed)
        if (it != params.end()) {
            paramPortSpec(*it, &port);
            if (it->colorField) port.valueKind = ValueKind::Vec3;
        }
        specs.push_back(port);
    }
    return specs;
}

bool Graph::valueSlot(const Node& node, std::size_t k) const {
    if (k >= node.paramInputs.size()) return false;
    for (const ParamSpec& s : nodeParamSpecs(*node.op)) {
        PortSpec port;
        if (s.name == node.paramInputs[k]) return paramPortSpec(s, &port) && port.type == PortType::Value;
    }
    return false;
}

bool Graph::paramDriven(NodeId id, const std::string& param) const {
    const Node& node = nodeRecord(id);
    const std::size_t own = node.op->inputs().size();
    for (std::size_t k = 0; k < node.paramInputs.size(); ++k) {
        if (node.paramInputs[k] != param) continue;
        const std::size_t slot = own + k;
        return slot < node.inputs.size() && node.inputs[slot] != kNoNode && valueSlot(node, k);
    }
    return false;
}

void Graph::setBypass(NodeId id, bool bypass) { nodeRecord(id).bypass = bypass; }

void Graph::setParamInputs(NodeId id, std::vector<std::string> params) {
    Node& node = nodeRecord(id);
    const auto ports = node.op->inputs();
    const std::vector<ParamSpec> specs = nodeParamSpecs(*node.op);
    for (const std::string& p : params) {
        const auto spec = std::find_if(specs.begin(), specs.end(), [&](const ParamSpec& s) { return s.name == p; });
        if (spec == specs.end()) throw GraphError(node.op->typeName() + " has no param '" + p + "' to expose");
        if (!paramPortSpec(*spec, nullptr)) {
            throw GraphError(node.op->typeName() + " param '" + p + "' cannot be an input: " +
                             (spec->port == ParamSpec::Port::None ? "it sets the node's own ports"
                                                                  : "no node gives what it holds"));
        }
        for (const PortSpec& port : ports) {
            if (port.name == p) throw GraphError("param '" + p + "' has the name of an input of " + node.op->typeName());
        }
        if (std::count(params.begin(), params.end(), p) > 1) throw GraphError("param '" + p + "' exposed twice");
    }
    node.resizeInputs(ports.size());  // drop old param slots
    node.paramInputs = std::move(params);
    node.resizeInputs(ports.size() + node.paramInputs.size());
    releaseDriven(node);
}

namespace {

// The kind a linked param takes: its spec's default's, else the value it holds (a compound's
// exposed param has no spec of its own).
const ParamValue* keyedKind(const Operator& op, const std::string& param, const ParamSpec** spec) {
    static thread_local std::vector<ParamSpec> specs;
    specs = allParamSpecs(op);
    *spec = nullptr;
    for (const ParamSpec& s : specs) {
        if (s.name == param) *spec = &s;
    }
    if (*spec) return &(*spec)->defaultValue;
    return op.params().has(param) ? &op.params().get(param) : nullptr;
}

bool sameParam(const ParamValue& a, const ParamValue& b) { return hashParam(0, a) == hashParam(0, b); }

}  // namespace

void Graph::setParamControls(NodeId id, std::map<std::string, std::vector<std::string>> links) {
    Node& node = nodeRecord(id);
    for (const auto& [param, controls] : links) {
        const ParamSpec* spec = nullptr;
        const ParamValue* kind = keyedKind(*node.op, param, &spec);
        const std::string what = node.op->typeName() + " param '" + param + "'";
        if (!kind) throw GraphError(node.op->typeName() + " has no param '" + param + "' to key");
        if (spec && !spec->animatable) {
            throw GraphError(what + " cannot be keyed: it sets how many strands, points or cards there are, or "
                                    "where they go, which must stay the same on every frame");
        }
        const bool vector = std::holds_alternative<Vec3f>(*kind);
        const bool number = std::holds_alternative<double>(*kind) || std::holds_alternative<std::int64_t>(*kind) ||
                            std::holds_alternative<bool>(*kind);
        if (!vector && !number) throw GraphError(what + " cannot be keyed: only numbers, switches and vectors can");
        if (controls.size() != (vector ? 3u : 1u)) {
            throw GraphError(what + " is keyed by " + (vector ? "three controls (x, y, z)" : "one control"));
        }
        for (const std::string& c : controls) {
            if (c.empty()) throw GraphError(what + ": a control needs a name");
        }
    }
    node.paramControls = std::move(links);
    for (auto it = node.keyed.begin(); it != node.keyed.end();) {
        auto& [param, keyed] = *it;
        if (node.paramControls.count(param)) {
            ++it;
            continue;
        }
        // No longer linked: back to its own value if the last evaluation set it.
        if (keyed.hasApplied && node.op->params().has(param) && sameParam(node.op->params().get(param), keyed.applied)) {
            node.op->params().set(param, keyed.own);
        }
        it = node.keyed.erase(it);
    }
}

ParamSet Graph::storedParams(NodeId id) const {
    const Node& node = nodeRecord(id);
    ParamSet out = node.op->params();
    out.clearFields();
    for (const auto* set : {&node.keyed, &node.driven}) {
        for (const auto& [param, keyed] : *set) {
            if (keyed.hasApplied && out.has(param) && sameParam(out.get(param), keyed.applied)) out.set(param, keyed.own);
        }
    }
    return out;
}

void Graph::releaseDriven(Node& node) {
    if (node.driven.empty()) return;
    const std::size_t own = node.op->inputs().size();
    for (auto it = node.driven.begin(); it != node.driven.end();) {
        const auto at = std::find(node.paramInputs.begin(), node.paramInputs.end(), it->first);
        const auto k = static_cast<std::size_t>(at - node.paramInputs.begin());
        const bool linked = at != node.paramInputs.end() && own + k < node.inputs.size() && node.inputs[own + k] != kNoNode &&
                            valueSlot(node, k);
        if (linked) {
            ++it;
            continue;
        }
        // No Value sets it now: back to its own value, if what it holds is what the input set.
        const Node::Keyed& d = it->second;
        if (d.hasApplied && node.op->params().has(it->first) && sameParam(node.op->params().get(it->first), d.applied)) {
            node.op->params().set(it->first, d.own);
        }
        it = node.driven.erase(it);
    }
}

void Graph::applyValueInputs(NodeId id, const EvalContext& ctx, Keys& keys, Transient* transient, std::set<NodeId>& done) {
    if (id == kNoNode || !nodes_.count(id) || !done.insert(id).second) return;
    // Upstream first: a node's key is made from its inputs' keys, so theirs are set before.
    for (std::size_t slot = 0; slot < nodeRecord(id).inputs.size(); ++slot) {
        applyValueInputs(nodeRecord(id).inputs[slot], ctx, keys, transient, done);
    }
    Node& node = nodeRecord(id);
    releaseDriven(node);
    node.drivenVaries = false;
    if (node.paramInputs.empty()) return;
    const std::size_t own = node.op->inputs().size();
    std::vector<ParamSpec> specs;
    for (std::size_t k = 0; k < node.paramInputs.size(); ++k) {
        const std::size_t slot = own + k;
        if (slot >= node.inputs.size() || node.inputs[slot] == kNoNode || !valueSlot(node, k)) continue;
        const std::string& param = node.paramInputs[k];
        if (specs.empty()) specs = nodeParamSpecs(*node.op);
        const auto spec = std::find_if(specs.begin(), specs.end(), [&](const ParamSpec& s) { return s.name == param; });
        if (spec == specs.end()) continue;
        auto fail = [&](const std::string& message) {
            const std::string what = node.op->typeName() + " param '" + param + "' " + message;
            throw GraphError(node.label.empty() ? what : "node '" + node.label + "': " + what);
        };
        const NodeId src = node.inputs[slot];
        const std::vector<Value> outs = evaluateWithKeys(src, ctx, keys, transient);
        const std::size_t output = node.inputOutputs[slot];
        const Value value = output < outs.size() ? outs[output] : Value{};
        if (std::holds_alternative<std::monostate>(value)) continue;  // nothing (a compound's input left open): its own value
        const UniformPtr uniform = valueAs<UniformPtr>(value);
        if (!uniform) fail("takes one value for the whole node, not a value per strand or a curve");
        const Node& source = nodeRecord(src);
        const bool varies = uniform->varies || !source.paramControls.empty() || source.drivenVaries;
        if (varies && !spec->animatable) {
            fail("cannot take a value that changes over time (an expression of the frame, a keyed value): it sets how "
                 "many strands, points or cards there are, or where they go, which must stay the same on every frame");
        }
        ParamValue set;
        try {
            set = uniformAsParam(uniform->value, *spec);
        } catch (const GraphError& e) {
            fail(std::string("is fed a value it cannot take: ") + e.what());
        }
        Node::Keyed& d = node.driven[param];
        const ParamValue current = node.op->params().has(param) ? node.op->params().get(param) : spec->defaultValue;
        // Set since the last evaluation (loaded, edited): its own value.
        if (!d.hasApplied || !sameParam(current, d.applied)) d.own = current;
        node.op->params().set(param, set);
        d.applied = set;
        d.hasApplied = true;
        node.drivenVaries = node.drivenVaries || varies;
    }
}

std::vector<std::string> Graph::controlInputs() const {
    std::vector<std::string> out;
    for (const auto& [id, node] : nodes_) {
        for (const auto& [param, controls] : node.paramControls) {
            for (const std::string& c : controls) out.push_back(controlInputName(c));
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void Graph::applyControls(const EvalContext& ctx) {
    for (auto& [id, node] : nodes_) {
        for (const auto& [param, controls] : node.paramControls) {
            if (paramDriven(id, param)) continue;  // a Value input sets it (applyValueInputs)
            Operator& op = *node.op;
            const ParamSpec* spec = nullptr;
            const ParamValue* kind = keyedKind(op, param, &spec);
            if (!kind) continue;
            Node::Keyed& keyed = node.keyed[param];
            const ParamValue current = op.params().has(param) ? op.params().get(param) : *kind;
            // Set since the last evaluation (loaded, edited, a compound's exposed value): its own value.
            if (!keyed.hasApplied || !sameParam(current, keyed.applied)) keyed.own = current;
            float values[3] = {0.0f, 0.0f, 0.0f};
            bool all = true;
            for (std::size_t k = 0; k < controls.size() && k < 3; ++k) {
                const FieldPtr set = valueAs<FieldPtr>(ctx.external(controlInputName(controls[k])));
                if (set) {
                    values[k] = set->maximum();
                } else {
                    all = false;
                }
            }
            ParamValue value = keyed.own;
            if (all) {
                if (std::holds_alternative<Vec3f>(*kind)) {
                    value = Vec3f(values[0], values[1], values[2]);
                } else if (std::holds_alternative<bool>(*kind)) {
                    value = values[0] >= 0.5f;
                } else if (std::holds_alternative<std::int64_t>(*kind)) {
                    value = static_cast<std::int64_t>(std::llround(values[0]));
                } else {
                    value = static_cast<double>(values[0]);
                }
            }
            op.params().set(param, value);
            keyed.applied = value;
            keyed.hasApplied = true;
        }
    }
}

void Graph::connect(NodeId src, NodeId dst, std::size_t inputSlot, std::size_t output) {
    const Node& srcNode = nodeRecord(src);  // throws for an unknown node
    Node& dstNode = nodeRecord(dst);

    const auto specs = inputSpecs(dst);
    if (inputSlot >= specs.size()) {
        throw GraphError(dstNode.op->typeName() + " has no input slot " + std::to_string(inputSlot));
    }
    if (output >= srcNode.op->outputs().size()) {
        throw GraphError(srcNode.op->typeName() + " has no output " + std::to_string(output));
    }
    const PortType outType = outputType(src, output);
    const PortSpec& in = specs[inputSlot];
    if (outType == PortType::Field && in.type == PortType::Value) {
        throw GraphError("type mismatch: Field -> Value: '" + in.name + "' takes one value for the whole node, and a " +
                         "field gives one per strand (drive it with a Value node, or an Expression set to give a value)");
    }
    if (!portTypesMatch(outType, in.type)) {
        throw GraphError(std::string("type mismatch: ") + portTypeName(outType) + " -> " + portTypeName(in.type));
    }
    if (outType == PortType::Value) {
        const ValueKind outKind = srcNode.op->outputs()[output].valueKind;
        const ValueKind inKind = in.type == PortType::Field && in.valueKind == ValueKind::Any ? ValueKind::Number : in.valueKind;
        if (!valueKindsMatch(outKind, inKind)) {
            throw GraphError(std::string("type mismatch: a ") + valueKindName(outKind) + " Value cannot feed '" + in.name +
                             "', which takes " + (in.type == PortType::Field ? "a field or a number" : valueKindName(inKind)));
        }
    }
    if (reaches(src, dst)) throw GraphError("connection would create a cycle");

    // Ports can depend on params (a compound's definition): grow the slots to match.
    if (dstNode.inputs.size() < specs.size()) dstNode.resizeInputs(specs.size());
    dstNode.inputs[inputSlot] = src;
    dstNode.inputOutputs[inputSlot] = output;
}

void Graph::disconnect(NodeId dst, std::size_t inputSlot) {
    Node& dstNode = nodeRecord(dst);
    if (inputSlot < dstNode.inputs.size()) {
        dstNode.inputs[inputSlot] = kNoNode;
        dstNode.inputOutputs[inputSlot] = 0;
    }
    releaseDriven(dstNode);
}

bool Graph::inputSource(NodeId dst, std::size_t inputSlot, NodeId* src, std::size_t* output) const {
    const Node& dstNode = nodeRecord(dst);
    if (inputSlot >= dstNode.inputs.size() || dstNode.inputs[inputSlot] == kNoNode) return false;
    if (src) *src = dstNode.inputs[inputSlot];
    if (output) *output = dstNode.inputOutputs[inputSlot];
    return true;
}

std::set<NodeId> Graph::nodesReachingOutput() const {
    std::set<NodeId> live;
    std::vector<NodeId> stack;
    for (const auto& [id, node] : nodes_) {
        if (node.op->typeName() == op::kOutput) stack.push_back(id);
    }
    while (!stack.empty()) {
        const NodeId n = stack.back();
        stack.pop_back();
        if (n == kNoNode || !live.insert(n).second) continue;
        const Node& up = nodeRecord(n);
        stack.insert(stack.end(), up.inputs.begin(), up.inputs.end());
    }
    return live;
}

std::map<NodeId, Graph::Resolution> Graph::strandResolutions(bool viewport) const {
    // With an Output node (a groom's graph), only the Resamples on the way to it count: one on a
    // branch reaching no Output, or fed nothing, never changes the groom. Without one (a Compound's
    // inner graph, a graph built in code), every Resample counts, and one fed nothing sets the
    // default of the Scatters with none after them.
    const std::set<NodeId> live = nodesReachingOutput();
    // What each Resample asks for (a bypassed one, one fed guides or one not counted, nothing), and
    // who reads each node.
    std::map<NodeId, int> asks;
    std::map<NodeId, std::vector<NodeId>> readers;
    Resolution fallback;
    for (const auto& [id, node] : nodes_) {
        for (const NodeId src : node.inputs) {
            if (src != kNoNode) readers[src].push_back(id);
        }
        if (node.op->typeName() != op::kResample || off(node, viewport)) continue;
        if (!live.empty() && !live.count(id)) continue;
        PortType type = PortType::Curves;
        try {
            type = outputType(id);
        } catch (const GraphError&) {
        }
        if (type == PortType::Guides) continue;
        const int segments = static_cast<int>(std::clamp<std::int64_t>(node.op->params().getInt("segments"), 0, 256));
        if (node.inputs.empty() || node.inputs[0] == kNoNode) {
            if (live.empty() && segments > fallback.segments) fallback = {segments, id};
            continue;
        }
        asks[id] = segments;
    }
    // Downstream first: each node takes the most any reader asks for or takes (the first such
    // reader's Resample, by ID, on a tie).
    std::map<NodeId, Resolution> out;
    std::set<NodeId> visiting;
    std::function<Resolution(NodeId)> resolve = [&](NodeId id) -> Resolution {
        if (const auto it = out.find(id); it != out.end()) return it->second;
        Resolution best;
        if (!visiting.insert(id).second) return best;  // (a cycle: never connected)
        if (const auto r = readers.find(id); r != readers.end()) {
            std::vector<NodeId> after = r->second;
            std::sort(after.begin(), after.end());
            after.erase(std::unique(after.begin(), after.end()), after.end());
            for (const NodeId reader : after) {
                const auto a = asks.find(reader);
                if (a != asks.end() && a->second > best.segments) best = {a->second, reader};
                const Resolution further = resolve(reader);
                if (further.segments > best.segments) best = further;
            }
        }
        visiting.erase(id);
        out[id] = best;
        return best;
    };
    for (const auto& [id, node] : nodes_) resolve(id);
    for (auto& [id, r] : out) {
        if (r.segments <= 0) r = fallback;
    }
    return out;
}

Graph::Resolution Graph::strandResolution(NodeId id, bool viewport) const {
    nodeRecord(id);  // (an unknown node throws)
    return strandResolutions(viewport)[id];
}

std::string Graph::strandResolutionNote(NodeId id, bool viewport) const {
    const Node& node = nodeRecord(id);
    const std::string type = node.op->typeName();
    if ((type != op::kScatter && type != op::kResample) || off(node, viewport)) return {};
    const std::map<NodeId, Resolution> all = strandResolutions(viewport);
    auto name = [&](NodeId n) {
        const std::string& label = nodeRecord(n).label;
        return "'" + (label.empty() ? "#" + std::to_string(n) : label) + "'";
    };
    auto points = [](int segments) {
        return std::to_string(segments) + " segments (" + std::to_string(segments + 1) + " points)";
    };
    auto fedNothing = [&](NodeId n) {
        const Node& r = nodeRecord(n);
        return r.inputs.empty() || r.inputs[0] == kNoNode;
    };
    if (type == op::kScatter) {
        const Resolution& r = all.at(id);
        if (r.from == kNoNode || r.segments <= 0) {
            return "Its strands grow with " + points(8) + ": no Resample after this Scatter sets their resolution (add one to its "
                   "branch)";
        }
        if (fedNothing(r.from)) {
            return "Its strands grow with " + points(r.segments) + ", set by Resample " + name(r.from) +
                   " (fed nothing, it sets the default: no Resample after this Scatter does)";
        }
        return "Its strands grow with " + points(r.segments) + ", set by Resample " + name(r.from) +
               " (the most segments any Resample after it asks for)";
    }
    try {
        if (outputType(id) == PortType::Guides) return {};
    } catch (const GraphError&) {
        return {};
    }
    const std::set<NodeId> live = nodesReachingOutput();
    if (fedNothing(id) && !live.empty()) {
        return "Fed nothing: it sets no Scatter's resolution (connect it after the strands it should resample)";
    }
    if (fedNothing(id)) {
        std::string list;
        for (const auto& [n, r] : all) {
            if (r.from == id && nodeRecord(n).op->typeName() == op::kScatter && !off(nodeRecord(n), viewport)) {
                list += (list.empty() ? "" : ", ") + name(n);
            }
        }
        return list.empty() ? std::string("Fed nothing: it sets the default resolution, which no Scatter takes (each has a Resample "
                                          "after it, or another Resample fed nothing asks for more)")
                            : "Fed nothing: it sets the default resolution, " +
                                  points(static_cast<int>(std::clamp<std::int64_t>(node.op->params().getInt("segments"), 0, 256))) +
                                  ", of every Scatter with no Resample after it (Scatter " + list + ")";
    }
    if (!live.empty() && !live.count(id)) {
        return "Its strands reach no Output: it sets no Scatter's resolution (connect it on the way to the Output)";
    }
    // The Scatters before it, and the ones it sets.
    std::vector<NodeId> scatters, sets;
    std::vector<NodeId> stack(node.inputs.begin(), node.inputs.end());
    std::set<NodeId> seen;
    while (!stack.empty()) {
        const NodeId n = stack.back();
        stack.pop_back();
        if (n == kNoNode || !seen.insert(n).second) continue;
        const Node& up = nodeRecord(n);
        if (up.op->typeName() == op::kScatter && !off(up, viewport)) {
            scatters.push_back(n);
            if (all.at(n).from == id) sets.push_back(n);
        }
        stack.insert(stack.end(), up.inputs.begin(), up.inputs.end());
    }
    std::sort(scatters.begin(), scatters.end());
    std::sort(sets.begin(), sets.end());
    if (scatters.empty()) return "No Scatter comes before it: it sets no Scatter's resolution (a Resample sets only its own branch's)";
    if (sets.empty()) {
        const NodeId wins = all.at(scatters.front()).from;
        return wins == kNoNode ? std::string("Sets no Scatter's resolution (0 segments keeps each strand's)")
                               : "Sets no Scatter's resolution: Resample " + name(wins) + " after it asks for more";
    }
    std::string list;
    for (const NodeId n : sets) list += (list.empty() ? "" : ", ") + name(n);
    return "Sets the resolution of Scatter " + list + ": " + points(all.at(sets.front()).segments) +
           " (a Resample sets only its own branch's)";
}

EvalContext Graph::nodeContext(NodeId id, const EvalContext& ctx, const Keys& keys) const {
    if (keys.segments.empty()) return ctx;
    EvalContext out = ctx;
    const auto it = keys.segments.find(id);
    out.strandSegments = it == keys.segments.end() ? 0 : it->second.segments;
    return out;
}

std::uint64_t Graph::computeKey(NodeId id, const EvalContext& ctx, Keys& memo) const {
    if (auto it = memo.full.find(id); it != memo.full.end()) return it->second;

    const Node& node = nodeRecord(id);
    std::uint64_t key = hashString(ctx.hash(), node.op->typeName());
    // Its bypass, or its evaluation mode's in this evaluation (a node always evaluated keys as ever).
    const bool bypassed = off(node, ctx.viewport);
    key = hashCombine(key, bypassed ? 0xB1FA55u : 0u);
    // Bypassed and passing its first input through: its result is that input, whatever its params,
    // salts, externals or the time (a bypassed Wind), so only the input's key counts and what
    // follows stays cached from frame to frame.
    const auto ownSpecs = bypassed ? node.op->inputs() : std::vector<PortSpec>{};
    if (bypassed && !ownSpecs.empty() && ownSpecs[0].type == node.op->output().type && !node.inputs.empty() &&
        node.inputs[0] != kNoNode) {
        memo.own[id] = hashCombine(key, 0x0C7u + node.inputOutputs[0]);
        key = hashCombine(key, computeKey(node.inputs[0], ctx, memo));
        if (node.inputOutputs[0] != 0) key = hashCombine(key, 0x0C7u + node.inputOutputs[0]);
        memo.full[id] = key;
        return key;
    }
    key = hashCombine(key, node.op->params().hash());
    // A node kept at an older behaviour (Operator::nodeVersion) is another result.
    if (node.op->nodeVersion() != node.op->version()) key = hashCombine(key, 0x7E5000u + static_cast<unsigned>(node.op->nodeVersion()));
    key = hashCombine(key, node.op->cacheSalt());
    key = hashCombine(key, node.op->contextSalt(nodeContext(id, ctx, memo)));
    key = hashCombine(key, mapParamsKey(node.op->params(), ctx));
    for (const std::string& name : node.op->externalInputs()) {
        const ExternalInputs::Entry* e = ctx.externals ? ctx.externals->find(name) : nullptr;
        key = hashString(key, name);
        key = hashCombine(key, e ? node.op->externalKey(name, *e) : 0);
    }
    // Its own key: everything but what its inputs bring (and which of their outputs it reads).
    std::uint64_t own = hashCombine(key, node.inputs.size());
    for (std::size_t slot = 0; slot < node.inputs.size(); ++slot) {
        own = hashCombine(own, node.inputs[slot] == kNoNode ? 0 : 0x5107u + node.inputOutputs[slot]);
    }
    memo.own[id] = own;
    for (std::size_t slot = 0; slot < node.inputs.size(); ++slot) {
        const NodeId src = node.inputs[slot];
        key = hashCombine(key, src == kNoNode ? 0 : computeKey(src, ctx, memo));
        // Which of the source's outputs (the primary adds nothing: keys stay as they were).
        if (src != kNoNode && node.inputOutputs[slot] != 0) key = hashCombine(key, 0x0C7u + node.inputOutputs[slot]);
    }
    memo.full[id] = key;
    return key;
}

namespace {

std::atomic<bool> incrementalOn{true};
std::atomic<bool> verifyIncrementalOn{false};
std::atomic<bool> diffResultsOn{true};

// The same object (shared results are immutable: the same pointer is the same value).
bool sameValue(const Value& a, const Value& b) {
    if (a.index() != b.index()) return false;
    return std::visit(
        [&](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return true;
            } else {
                return x.get() == std::get<T>(b).get();
            }
        },
        a);
}

const ChangesPtr& noChanges() {
    static const ChangesPtr none = std::make_shared<const StrandChanges>(StrandChanges::listed({}));
    return none;
}

// Testing (Graph::setVerifyIncremental): what an incremental evaluation says against the truth.
// Its result against a full evaluation's, and the strands it says it left alone against the
// node's last result: the same bytes in every array.
std::string incrementalMismatch(const Value& incremental, const Value& full, const Value& previous,
                                const StrandChanges& changed) {
    const StrandsPtr a = valueAs<StrandsPtr>(incremental), b = valueAs<StrandsPtr>(full);
    if (!a || !b) {
        const GuidesPtr ga = valueAs<GuidesPtr>(incremental), gb = valueAs<GuidesPtr>(full);
        if (ga && gb) return ga->contentHash() == gb->contentHash() ? std::string() : "its guides differ from a full evaluation's";
        return a || b || ga || gb ? "it made another kind of value than a full evaluation" : std::string();
    }
    if (a->contentHash() != b->contentHash()) return "its strands differ from a full evaluation's";
    const StrandsPtr p = valueAs<StrandsPtr>(previous);
    if (changed.all || !p) return {};
    if (p->strandCount() != a->strandCount() || p->pointCount() != a->pointCount() || p->ids() != a->ids() ||
        p->offsets() != a->offsets() || p->strandChannelNames() != a->strandChannelNames() ||
        p->pointChannelNames() != a->pointChannelNames()) {
        return "it says only some strands changed, but the set's layout did";
    }
    std::vector<char> listed(a->strandCount(), 0);
    for (const std::uint32_t s : changed.strands) {
        if (s >= listed.size()) return "it lists a strand past the set's end";
        listed[s] = 1;
    }
    auto samePoints = [&](const void* x, const void* y, std::size_t bytesPerPoint, std::size_t s) {
        const std::size_t begin = a->strandBegin(s), n = a->strandPointCount(s);
        return std::memcmp(static_cast<const char*>(x) + begin * bytesPerPoint,
                           static_cast<const char*>(y) + begin * bytesPerPoint, n * bytesPerPoint) == 0;
    };
    for (std::size_t s = 0; s < listed.size(); ++s) {
        if (listed[s]) continue;
        bool same = std::memcmp(&a->roots()[s], &p->roots()[s], sizeof(StrandRoot)) == 0 &&
                    samePoints(a->positions().data(), p->positions().data(), sizeof(Vec3f), s) &&
                    samePoints(a->widths().data(), p->widths().data(), sizeof(float), s);
        for (const std::string& name : a->strandChannelNames()) {
            same = same && std::memcmp(&(*a->strandChannel(name))[s], &(*p->strandChannel(name))[s], sizeof(float)) == 0;
        }
        for (const std::string& name : a->pointChannelNames()) {
            same = same && samePoints(a->pointChannel(name)->data(), p->pointChannel(name)->data(), sizeof(float), s);
        }
        if (!same) return "strand " + std::to_string(s) + " changed but is not listed as changed";
    }
    return {};
}

}  // namespace

ChangesPtr strandChangesBetween(const StrandsPtr& p, const StrandsPtr& n) {
    if (!p || !n) return nullptr;
    if (p == n) return noChanges();
    const std::size_t count = n->strandCount();
    if (p->strandCount() != count || p->pointCount() != n->pointCount() || p->cardLayers() != n->cardLayers() ||
        p->featherLayers() != n->featherLayers()) {
        return nullptr;
    }
    const bool instancesChanged = p->instances() != n->instances();
    const std::vector<std::string> strandNames = n->strandChannelNames(), pointNames = n->pointChannelNames();
    if (p->strandChannelNames() != strandNames || p->pointChannelNames() != pointNames) return nullptr;
    for (const std::string& name : strandNames) {
        if (p->strandChannelDefault(name) != n->strandChannelDefault(name)) return nullptr;
    }
    for (const std::string& name : pointNames) {
        if (p->pointChannelDefault(name) != n->pointChannelDefault(name)) return nullptr;
    }
    // Arrays compared byte for byte (a NaN equal to itself), skipped where the two share one.
    auto same = [](const auto& x, const auto& y) {
        return &x == &y || (x.size() == y.size() && (x.empty() || std::memcmp(x.data(), y.data(), x.size() * sizeof(x[0])) == 0));
    };
    if (!same(p->offsets(), n->offsets()) || !same(p->ids(), n->ids()) || !same(p->roots(), n->roots())) return nullptr;
    struct Arrays {
        const void* a;
        const void* b;
        std::size_t bytes;  // per strand or point
        bool perPoint;
    };
    std::vector<Arrays> arrays;
    auto add = [&](const auto& x, const auto& y, bool perPoint) {
        if (&x != &y && !x.empty()) arrays.push_back({x.data(), y.data(), sizeof(x[0]), perPoint});
    };
    add(p->positions(), n->positions(), true);
    if (p->widths().size() != n->widths().size()) return nullptr;
    if (p->widths().size() == n->pointCount()) add(p->widths(), n->widths(), true);
    for (const std::string& name : strandNames) add(*p->strandChannel(name), *n->strandChannel(name), false);
    for (const std::string& name : pointNames) add(*p->pointChannel(name), *n->pointChannel(name), true);
    constexpr std::size_t kBlock = 4096;
    const std::size_t blocks = (count + kBlock - 1) / kBlock;
    std::vector<std::vector<std::uint32_t>> found(blocks);
    const std::vector<std::size_t>& offsets = n->offsets();
    parallelFor(0, blocks, [&](std::size_t b) {
        const std::size_t first = b * kBlock, end = std::min(count, first + kBlock);
        // A block whose arrays are the same as a whole holds no changed strand.
        bool any = false;
        for (const Arrays& x : arrays) {
            const std::size_t from = x.perPoint ? offsets[first] : first, to = x.perPoint ? offsets[end] : end;
            if (std::memcmp(static_cast<const char*>(x.a) + from * x.bytes, static_cast<const char*>(x.b) + from * x.bytes,
                            (to - from) * x.bytes) != 0) {
                any = true;
                break;
            }
        }
        if (!any) return;
        for (std::size_t s = first; s < end; ++s) {
            for (const Arrays& x : arrays) {
                const std::size_t from = x.perPoint ? offsets[s] : s, to = x.perPoint ? offsets[s + 1] : s + 1;
                if (std::memcmp(static_cast<const char*>(x.a) + from * x.bytes, static_cast<const char*>(x.b) + from * x.bytes,
                                (to - from) * x.bytes) != 0) {
                    found[b].push_back(static_cast<std::uint32_t>(s));
                    break;
                }
            }
        }
    }, 1);
    std::vector<std::uint32_t> changed;
    for (const auto& list : found) changed.insert(changed.end(), list.begin(), list.end());
    StrandChanges made = StrandChanges::listed(std::move(changed));
    made.instancesChanged = instancesChanged;
    return std::make_shared<const StrandChanges>(std::move(made));
}

namespace {

// The last result as the check of an incremental evaluation reads it after the operator ran: its
// points and widths copied (what an operator may take from it, IncrementalInputs::sole).
Value detachedPrevious(const Value& previous) {
    const StrandsPtr strands = valueAs<StrandsPtr>(previous);
    if (!strands) return previous;
    auto copy = std::make_shared<StrandSet>(*strands);
    copy->copyPositionsFrom(*strands);
    copy->copyWidthsFrom(*strands);
    return StrandsPtr(copy);
}

// Whether `next` holds the very points of `previous` (StrandChanges::pointsKept): the same
// positions array (two live sets share one only as copies of each other), the same strands (IDs,
// roots, offsets: shared, or equal byte for byte) and the same instances, card and feather layers.
bool samePointsAs(const StrandsPtr& previous, const StrandsPtr& next) {
    if (!previous || !next) return false;
    if (previous == next) return true;
    if (previous->pointCount() != next->pointCount() || previous->strandCount() != next->strandCount() ||
        previous->positions().data() != next->positions().data() || previous->instances() != next->instances() ||
        previous->cardLayers() != next->cardLayers() || previous->featherLayers() != next->featherLayers()) {
        return false;
    }
    const std::vector<std::string> strandNames = next->strandChannelNames(), pointNames = next->pointChannelNames();
    if (previous->strandChannelNames() != strandNames || previous->pointChannelNames() != pointNames ||
        (previous->widths().size() == previous->pointCount()) != (next->widths().size() == next->pointCount())) {
        return false;
    }
    for (const std::string& name : strandNames) {
        if (previous->strandChannelDefault(name) != next->strandChannelDefault(name)) return false;
    }
    for (const std::string& name : pointNames) {
        if (previous->pointChannelDefault(name) != next->pointChannelDefault(name)) return false;
    }
    auto same = [](const auto& x, const auto& y) {
        return x.data() == y.data() ||
               (x.size() == y.size() && (x.empty() || std::memcmp(x.data(), y.data(), x.size() * sizeof(x[0])) == 0));
    };
    return same(previous->offsets(), next->offsets()) && same(previous->ids(), next->ids()) && same(previous->roots(), next->roots());
}

}  // namespace

void Graph::setIncrementalEvaluation(bool on) { incrementalOn = on; }
bool Graph::incrementalEvaluation() { return incrementalOn.load(); }
void Graph::setVerifyIncremental(bool on) { verifyIncrementalOn = on; }
void Graph::setDiffFullResults(bool on) { diffResultsOn = on; }

namespace {
// Result serials come from one counter for the process, not one per graph: a serial a host holds
// from another document (a graph parsed again, a clone's lineage) then never names a result of
// this one, so changesSince never answers for strands it did not make.
std::uint64_t newResultSerial() {
    static std::atomic<std::uint64_t> serials{0};
    return ++serials;
}
}  // namespace

std::uint64_t Graph::resultSerial(NodeId id) const {
    const Node& node = nodeRecord(id);
    return node.hasCache ? node.serial : 0;
}

ChangesPtr Graph::changesSince(NodeId id, std::uint64_t since, bool asMade) const {
    const Node& node = nodeRecord(id);
    if (since == 0 || !node.hasCache) return nullptr;
    if (node.serial == since) return noChanges();
    ChangesPtr changes = node.changesBase == since && !node.changes.empty() ? node.changes[0] : nullptr;
    const StrandsPtr now = node.cachedValues.empty() ? nullptr : valueAs<StrandsPtr>(node.cachedValues[0]);
    if (asMade || !changes || !now || !now->strandChannel(kPreviewChannel)) return changes;
    // A preview holding hidden strands: its callers hold the shown ones (shownValues). Between two
    // results hiding the same strands, the changes of the shown ones, at their places there.
    if (node.hiddenSerial[0] != node.serial || node.hiddenSerial[1] != since || node.hiddenHash[0] != node.hiddenHash[1]) {
        return nullptr;
    }
    if (node.hiddenHash[0] == 0 || changes->all) return changes;
    if (!node.shownAt) return nullptr;
    std::vector<std::uint32_t> shown;
    shown.reserve(changes->strands.size());
    for (const std::uint32_t s : changes->strands) {
        if (s < node.shownAt->size() && (*node.shownAt)[s] != UINT32_MAX) shown.push_back((*node.shownAt)[s]);
    }
    StrandChanges made = StrandChanges::listed(std::move(shown));
    made.instancesChanged = changes->instancesChanged;
    return std::make_shared<const StrandChanges>(std::move(made));
}

ChangesPtr Graph::inputChanges(const Node& node, std::size_t slot, const Node& source) const {
    const std::uint64_t seen = slot < node.consumed.size() ? node.consumed[slot] : 0;
    if (seen == 0 || !source.hasCache) return nullptr;
    if (source.serial == seen) return noChanges();
    // Each output's changes, as its node said them (a Split's), else unknown (a Map node's r).
    const std::size_t output = node.inputOutputs[slot];
    return source.changesBase == seen && output < source.changes.size() ? source.changes[output] : nullptr;
}

struct Graph::Transient {
    std::map<NodeId, int> consumers;  // uses left in this evaluation
    std::map<NodeId, std::vector<Value>> results;  // held until the last use
};

std::vector<Value> Graph::evaluateWithKeys(NodeId id, const EvalContext& graphCtx, Keys& keys, Transient* transient) {
    Node& node = nodeRecord(id);
    // The context its operator runs with: its own strand segments (its branch's).
    const EvalContext nodeCtx = nodeContext(id, graphCtx, keys);
    const EvalContext& ctx = nodeCtx;
    std::uint64_t key = 0;
    if (transient) {
        auto it = transient->results.find(id);
        if (it != transient->results.end()) return it->second;
    } else {
        key = computeKey(id, graphCtx, keys);
        // A source handing on an external keyed by less than all of it (the scalp, by its surface):
        // the same key, the external as it is now.
        auto fresh = [&]() -> const std::vector<Value>& {
            if (node.op->refreshesOnCacheHit()) node.cachedValues = node.op->evaluateOutputs(ctx, {});
            if (auto t = lastTimes_.find(id); t != lastTimes_.end()) t->second = 0.0;  // served from the cache
            return node.cachedValues;
        };
        if (node.hasCache && node.cachedKey == key) return fresh();
        auto adopted = adopted_.find(key);
        if (adopted != adopted_.end()) {  // the same node before the graph was reloaded
            adopt(node, adopted->second, key, keys.own[id]);
            return fresh();
        }
    }

    std::vector<Value> result;
    std::vector<ChangesPtr> changes;  // per output, against the node's last result (null or missing: unknown)
    bool passedOn = false;
    std::vector<std::uint64_t> consumed(node.inputs.size(), 0);
    {
        std::vector<Value> inputValues;
        std::vector<ChangesPtr> changed(node.inputs.size());  // per slot, since what the last result was made from
        inputValues.reserve(node.inputs.size());
        for (std::size_t slot = 0; slot < node.inputs.size(); ++slot) {
            const NodeId src = node.inputs[slot];
            if (src == kNoNode) {
                inputValues.emplace_back();
                changed[slot] = noChanges();
                continue;
            }
            // The source runs once for all its outputs; this slot takes the one it is linked to.
            std::vector<Value> outs = evaluateWithKeys(src, graphCtx, keys, transient);
            const std::size_t output = node.inputOutputs[slot];
            inputValues.push_back(output < outs.size() ? std::move(outs[output]) : Value{});
            if (!transient) {
                const Node& source = nodeRecord(src);
                consumed[slot] = source.serial;
                changed[slot] = inputChanges(node, slot, source);
            }
        }
        const std::size_t own = node.op->inputs().size();
        inputValues.resize(std::max(inputValues.size(), own + node.paramInputs.size()));  // unconnected: empty
        changed.resize(inputValues.size(), noChanges());
        const auto ownSpecs = node.op->inputs();
        // Exposed params: plug the connected fields in for this evaluation only.
        ParamSet& params = node.op->params();
        for (std::size_t k = 0; k < node.paramInputs.size(); ++k) {
            if (FieldPtr field = valueAs<FieldPtr>(inputValues[own + k])) params.setField(node.paramInputs[k], field);
            if (CurvePtr curve = valueAs<CurvePtr>(inputValues[own + k])) params.setCurve(node.paramInputs[k], curve);
            // A Value in a Field slot: its number on every strand (one in a Value slot set the
            // param already: applyValueInputs).
            const UniformPtr uniform = valueAs<UniformPtr>(inputValues[own + k]);
            if (uniform && !node.driven.count(node.paramInputs[k])) {
                if (const FieldPtr constant = uniformField(*uniform)) params.setField(node.paramInputs[k], constant);
            }
        }
        // Its affect (commonParamSpecs; a field plugged into it too): all (as ever), none (its input
        // on, as bypassed) or a mask.
        const AffectKind affect = affectOf(*node.op);
        const bool passThrough = (off(node, ctx.viewport) || affect == AffectKind::None) && !ownSpecs.empty() &&
                                 ownSpecs[0].type == node.op->output().type &&
                                 !std::holds_alternative<std::monostate>(inputValues[0]);
        const bool masked = affect == AffectKind::Mask;
        // A field or curve plugged in that is not the one the last result read: in full.
        bool sameParamInputs = true;
        for (std::size_t slot = own; slot < changed.size(); ++slot) {
            sameParamInputs = sameParamInputs && changed[slot] && changed[slot]->none();
        }
        inputValues.resize(own);
        // Each input gets the kind it takes (a Curves output resolves only now): a clear error otherwise.
        for (std::size_t i = 0; i < ownSpecs.size() && i < inputValues.size(); ++i) {
            const PortType want = ownSpecs[i].type;
            // A Value in a Field input: its number on every strand.
            if (want == PortType::Field) {
                if (const UniformPtr uniform = valueAs<UniformPtr>(inputValues[i])) inputValues[i] = uniformField(*uniform);
            }
            const bool strands = valueAs<StrandsPtr>(inputValues[i]) != nullptr;
            const bool guides = valueAs<GuidesPtr>(inputValues[i]) != nullptr;
            if ((want == PortType::Strands && guides) || (want == PortType::Guides && strands)) {
                const std::string message = node.op->typeName() + " input '" + ownSpecs[i].name + "' takes " +
                                            (want == PortType::Strands ? "strands" : "guides") + " but gets " +
                                            valueKind(inputValues[i]) + " from the node before it";
                throw GraphError(node.label.empty() ? message : "node '" + node.label + "': " + message);
            }
        }
        // A Curves node fed guides works on them as strands and hands guides back.
        const auto outSpecs = node.op->outputs();
        const bool onGuides = node.op->output().type == PortType::Curves && !ownSpecs.empty() &&
                              ownSpecs[0].type == PortType::Curves && valueAs<GuidesPtr>(inputValues[0]) != nullptr;
        const Value original = inputValues.empty() ? Value{} : inputValues[0];
        if (onGuides && !passThrough) inputValues[0] = guidesAsStrands(*valueAs<GuidesPtr>(inputValues[0]));
        // Its own key is its last result's (the same params, salts, externals and context), so only
        // its inputs changed: the operator may make again only what their changes reach.
        const bool incremental = !transient && incrementalOn.load(std::memory_order_relaxed) && node.hasCache &&
                                 node.ownKey == keys.own[id] && !passThrough && !onGuides && sameParamInputs &&
                                 node.cachedValues.size() == outSpecs.size();
        const bool multi = outSpecs.size() > 1;  // evaluateOutputsIncremental
        NoteScope notes(ctx);  // what the operator notes (EvalContext::note)
        bool madeIncrementally = false;
        const auto start = std::chrono::steady_clock::now();
        bool letGo = false;  // the node let go of its last result for an incremental evaluation
        try {
            if (passThrough) {
                // Bypassed: the first input on every output of its type (the primary always).
                result.assign(outSpecs.size(), Value{});
                for (std::size_t o = 0; o < outSpecs.size(); ++o) {
                    if (o == 0 || outSpecs[o].type == ownSpecs[0].type) result[o] = original;
                }
                passedOn = true;
            } else {
                bool done = false;
                // An input's instances another set: in full (its result compared with its last, below).
                bool instancesChanged = false;
                for (std::size_t slot = 0; slot < own; ++slot) {
                    instancesChanged = instancesChanged || (changed[slot] && changed[slot]->instancesChanged);
                }
                if (incremental && !masked && !instancesChanged) {  // masked: in full, then mixed (its changes found below)
                    IncrementalInputs in;
                    // The node lets go of its last result while the operator runs (it is put back
                    // after), holding it only through `in`: an operator holding it too and nobody
                    // else may take its arrays instead of copying them (a 96 MB copy per node at
                    // 500k strands). What it held is not read again but by the check, which reads
                    // a copy of it.
                    if (multi) {
                        in.previousOutputs = std::move(node.cachedValues);
                    } else {
                        in.previous = std::move(node.cachedValues[0]);
                    }
                    in.sole = true;
                    letGo = true;
                    auto putBack = [&] {
                        if (multi) {
                            node.cachedValues = std::move(in.previousOutputs);
                        } else {
                            node.cachedValues[0] = std::move(in.previous);
                        }
                    };
                    const bool verify = verifyIncrementalOn.load(std::memory_order_relaxed);
                    std::vector<Value> before;
                    if (verify) {
                        for (const Value& v : multi ? in.previousOutputs : std::vector<Value>{in.previous}) {
                            before.push_back(detachedPrevious(v));
                        }
                    }
                    in.inputs.assign(changed.begin(), changed.begin() + static_cast<std::ptrdiff_t>(own));
                    std::vector<Value> values(1);
                    std::vector<StrandChanges> made(1);
                    bool madeAgain = false;
                    try {
                        madeAgain = multi ? node.op->evaluateOutputsIncremental(ctx, inputValues, in, &values, &made)
                                          : node.op->evaluateIncremental(ctx, inputValues, in, &values[0], &made[0]);
                    } catch (...) {
                        putBack();
                        throw;
                    }
                    putBack();
                    if (madeAgain && (values.size() != outSpecs.size() || made.size() != outSpecs.size())) {
                        throw GraphError(node.op->typeName() + " made " + std::to_string(values.size()) +
                                         " values incrementally for " + std::to_string(outSpecs.size()) + " outputs");
                    }
                    if (madeAgain) {
                        done = true;
                        madeIncrementally = true;
                        ++incrementalCount_;
                        if (verify) {
                            // On a twin of the operator (its params and the fields plugged in), so the
                            // check leaves what the operator keeps between evaluations as it is.
                            std::unique_ptr<Operator> twin;
                            if (OperatorRegistry::instance().contains(node.op->typeName())) {
                                twin = OperatorRegistry::instance().create(node.op->typeName());
                                twin->params() = node.op->params();
                                twin->setNodeVersion(node.op->nodeVersion());
                                twin->setNodePath(node.op->nodePath());
                            }
                            const std::vector<Value> full = (twin ? *twin : *node.op).evaluateOutputs(ctx, inputValues);
                            for (std::size_t o = 0; o < values.size(); ++o) {
                                const std::string wrong =
                                    incrementalMismatch(values[o], o < full.size() ? full[o] : Value{}, before[o], made[o]);
                                if (wrong.empty()) continue;
                                throw GraphError(node.op->typeName() + " evaluated incrementally: " +
                                                 (multi ? "output '" + outSpecs[o].name + "': " : std::string()) + wrong);
                            }
                        }
                        result = std::move(values);
                        for (StrandChanges& c : made) changes.push_back(std::make_shared<const StrandChanges>(std::move(c)));
                    }
                }
                if (!done) {
                    result = node.op->evaluateOutputs(ctx, inputValues);
                    if (result.size() != outSpecs.size()) {
                        throw GraphError(node.op->typeName() + " made " + std::to_string(result.size()) + " values for " +
                                         std::to_string(outSpecs.size()) + " outputs");
                    }
                    // Masked: its result mixed back toward its input, strand by strand (point by
                    // point), by the affect field; a result of other strands keeps all of it.
                    if (masked) {
                        const StrandsPtr before = valueAs<StrandsPtr>(inputValues[0]);
                        const StrandsPtr made = valueAs<StrandsPtr>(result[0]);
                        if (before && made && made != before) {
                            const ScalarField field(params, kAffectParam, scalpOf(ctx));
                            if (const StrandsPtr mixed = applyAffect(*before, made, field)) {
                                node.op->resultMixed(made, mixed);
                                result[0] = mixed;
                            } else {
                                ctx.note("affect is not applied: this node changes how many strands or points there are");
                            }
                        }
                    }
                    // An operator handing its first input on as it came (Output, a Resample with
                    // nothing to do, a Clump without centers).
                    passedOn = !onGuides && !result.empty() && !std::holds_alternative<std::monostate>(original) &&
                               sameValue(result[0], original);
                    // In full, though only some strands of its inputs changed (an operator with no
                    // incremental path, or one that could not take it): its result against its last
                    // one, strand by strand, so the nodes after it go on incrementally.
                    if (incremental && !passedOn && diffResultsOn.load(std::memory_order_relaxed)) {
                        bool some = true;
                        for (std::size_t slot = 0; slot < own && some; ++slot) some = changed[slot] && !changed[slot]->all;
                        for (std::size_t o = 0; some && o < result.size(); ++o) {
                            changes.push_back(strandChangesBetween(valueAs<StrandsPtr>(node.cachedValues[o]),
                                                                   valueAs<StrandsPtr>(result[o])));
                        }
                    }
                }
            }
            // Its first input as it came, as the last result was: it changed as that input did.
            if (passedOn && node.passedOn && (changes.empty() || !changes[0])) {
                changes.resize(std::max<std::size_t>(changes.size(), 1));
                changes[0] = changed.empty() ? nullptr : changed[0];
            }
            if (onGuides && !passThrough) {
                for (std::size_t o = 0; o < result.size(); ++o) {
                    if (outSpecs[o].type != PortType::Curves) continue;
                    if (const StrandsPtr shaped = valueAs<StrandsPtr>(result[o])) result[o] = strandsAsGuides(*shaped);
                }
            }
        } catch (const GraphError& e) {
            params.clearFields();
            if (letGo) node.hasCache = false;  // its last result may have given its arrays away
            const std::string what = e.what();
            // Name the failing node (once: inner compound nodes name themselves).
            if (node.label.empty() || what.rfind("node '", 0) == 0) throw;
            throw GraphError("node '" + node.label + "': " + what);
        } catch (...) {
            params.clearFields();
            if (letGo) node.hasCache = false;
            throw;
        }
        params.clearFields();
        lastTimes_[id] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        lastRunTimes_[id] = lastTimes_[id];
        {
            const ChangesPtr first = changes.empty() ? nullptr : changes[0];
            const std::string count = first && !first->all ? std::to_string(first->strands.size()) : std::string("all");
            lastRunKinds_[id] = passThrough      ? std::string("bypass")
                                : madeIncrementally ? "incremental " + count
                                : first             ? "full " + count
                                                    : std::string("full");
        }
        // Its points the very ones of its last result (a Width, a HairColor, a Collide or Merge
        // after one): said, so the nodes after take their last points too (StrandChanges::pointsKept).
        if (!transient && node.hasCache) {
            for (std::size_t o = 0; o < result.size() && o < node.cachedValues.size(); ++o) {
                const StrandsPtr now = valueAs<StrandsPtr>(result[o]);
                const StrandsPtr before = valueAs<StrandsPtr>(node.cachedValues[o]);
                if (!now || now == before) continue;
                const bool kept = samePointsAs(before, now);
                ChangesPtr said = o < changes.size() ? changes[o] : nullptr;
                if (said && said->pointsKept == kept) continue;
                if (!said && !kept) continue;
                StrandChanges marked = said ? *said : StrandChanges{};
                marked.pointsKept = kept;
                changes.resize(std::max(changes.size(), o + 1));
                changes[o] = std::make_shared<const StrandChanges>(std::move(marked));
            }
        }
        std::string note;
        if (!passThrough) {
            note = notes.text();
            const std::string own = node.op->note(ctx);
            if (!own.empty()) note += (note.empty() ? "" : "; ") + own;
        }
        // Made again incrementally: its params and its inputs' strands are what its last run had,
        // so what that run noted still holds unless the operator noted something now.
        if (!note.empty()) {
            lastNotes_[id] = std::move(note);
        } else if (!madeIncrementally) {
            lastNotes_.erase(id);
        }
        // A value from a node keyed or fed a value that changes over time changes over time too.
        if (!node.paramControls.empty() || node.drivenVaries) {
            for (Value& v : result) {
                const UniformPtr uniform = valueAs<UniformPtr>(v);
                if (uniform && !uniform->varies) v = UniformPtr(std::make_shared<const UniformValue>(UniformValue{uniform->value, true}));
            }
        }
        ++evaluationCount_;
    }  // inputs released here

    if (transient) {
        for (NodeId src : node.inputs) {
            if (src != kNoNode && --transient->consumers[src] <= 0) transient->results.erase(src);
        }
        if (transient->consumers[id] > 0) transient->results[id] = result;
        return result;
    }
    // The same values as before (an input handed on that did not change, an incremental result
    // with nothing to make again): the same result, its serial kept, so what reads it sees no
    // change either.
    bool same = node.hasCache && result.size() == node.cachedValues.size();
    for (std::size_t o = 0; same && o < result.size(); ++o) same = sameValue(result[o], node.cachedValues[o]);
    if (!same) {
        node.changesBase = node.hasCache ? node.serial : 0;
        node.changes = node.hasCache ? std::move(changes) : std::vector<ChangesPtr>{};
        node.serial = newResultSerial();
    }
    node.cachedKey = key;
    node.cachedValues = result;
    node.hasCache = true;
    node.ownKey = keys.own[id];
    node.consumed = std::move(consumed);
    node.passedOn = passedOn;
    return result;
}

bool Graph::cachedChannels(NodeId id, std::vector<std::string>* names) const {
    auto it = nodes_.find(id);
    if (it == nodes_.end() || !it->second.hasCache || it->second.cachedValues.empty()) return false;
    const StrandsPtr strands = valueAs<StrandsPtr>(it->second.cachedValues[0]);
    if (!strands) return false;
    *names = strands->strandChannelNames();
    names->erase(std::remove(names->begin(), names->end(), std::string(kPreviewChannel)), names->end());
    return true;
}

Value Graph::cachedValue(NodeId id) const { return cachedValue(id, 0); }

Value Graph::cachedValue(NodeId id, std::size_t output) const {
    auto it = nodes_.find(id);
    if (it == nodes_.end() || !it->second.hasCache || output >= it->second.cachedValues.size()) return {};
    return it->second.cachedValues[output];
}

Value Graph::shownCachedValue(NodeId id, std::size_t output) const {
    auto it = nodes_.find(id);
    if (it == nodes_.end() || !it->second.hasCache || output >= it->second.cachedValues.size()) return {};
    const Node& node = it->second;
    if (output < node.shown.size() && output < node.shownFrom.size()) {
        const StrandsPtr from = node.shownFrom[output].lock();
        if (from && from == valueAs<StrandsPtr>(node.cachedValues[output])) return node.shown[output];
    }
    return node.cachedValues[output];
}

void Graph::setCacheIntermediates(bool enabled) {
    cacheIntermediates_ = enabled;
    if (!enabled) clearCache();
}

void Graph::adoptCache(const Graph& previous) {
    std::map<std::string, const Node*> byLabel;
    for (const auto& [id, node] : previous.nodes_) {
        if (node.hasCache) adopted_[node.cachedKey] = Adopted{node.cachedValues, node.serial, node.consumed, node.passedOn};
        if (!node.label.empty()) byLabel[node.label] = &node;
    }
    for (auto& [id, node] : nodes_) {
        const auto it = node.label.empty() ? byLabel.end() : byLabel.find(node.label);
        if (it == byLabel.end() || it->second->op->typeName() != node.op->typeName()) continue;
        node.op->adoptKeptWork(*it->second->op);
        // The same node's last result, under its old key: an evaluation keying it alike serves it
        // (as the adopted results above), one keying it otherwise makes it again, from it
        // incrementally where only its inputs changed (its own key the same: a param edited
        // upstream, the graph parsed again). Its serial and what it was made from are its own, so
        // the nodes after go on incrementally too (StrandChanges::pointsKept after a Width edit).
        const Node& was = *it->second;
        if (!was.hasCache || node.hasCache) continue;
        node.cachedKey = was.cachedKey;
        node.cachedValues = was.cachedValues;
        node.hasCache = true;
        node.serial = was.serial;
        node.ownKey = was.ownKey;
        node.consumed = was.consumed;
        node.changes.clear();
        node.changesBase = 0;
        node.passedOn = was.passedOn;
    }
}

void Graph::adopt(Node& node, const Adopted& adopted, std::uint64_t key, std::uint64_t ownKey) {
    node.cachedKey = key;
    node.cachedValues = adopted.values;
    node.hasCache = true;
    // The result's own serial and what it was made from: a node reading it (adopted too) sees
    // no change, so the next evaluation after an edit goes on incrementally from it. How it
    // came about (its changes) is not known here.
    node.serial = adopted.serial;
    node.ownKey = ownKey;
    node.consumed = adopted.consumed;
    node.changes.clear();
    node.changesBase = 0;
    node.passedOn = adopted.passedOn;
}

Graph Graph::clone() const {
    Graph out;
    for (const auto& [id, node] : nodes_) {
        Node copy;
        copy.op = OperatorRegistry::instance().create(node.op->typeName());
        copy.op->params() = node.op->params();
        copy.op->params().clearFields();  // (only ever set during an evaluation)
        copy.op->setNodeVersion(node.op->nodeVersion());
        copy.op->setNodePath(node.op->nodePath());
        copy.op->adoptKeptWork(*node.op);
        copy.inputs = node.inputs;
        copy.inputOutputs = node.inputOutputs;
        copy.paramInputs = node.paramInputs;
        copy.paramControls = node.paramControls;
        copy.keyed = node.keyed;
        copy.driven = node.driven;
        copy.drivenVaries = node.drivenVaries;
        copy.bypass = node.bypass;
        copy.evaluation = node.evaluation;
        copy.label = node.label;
        copy.cachedKey = node.cachedKey;
        copy.hasCache = node.hasCache;
        copy.cachedValues = node.cachedValues;
        copy.serial = node.serial;
        copy.ownKey = node.ownKey;
        copy.consumed = node.consumed;
        copy.changes = node.changes;
        copy.changesBase = node.changesBase;
        copy.passedOn = node.passedOn;
        // What the preview showed of its result (shownValues), so the copy's changesSince of a
        // preview holding hidden strands answers as the original's (a stroke document warmed on a
        // copy and adopted: GroomSession::adoptWarm).
        copy.shown = node.shown;
        copy.shownFrom = node.shownFrom;
        copy.shownAt = node.shownAt;
        for (int k = 0; k < 2; ++k) {
            copy.hiddenSerial[k] = node.hiddenSerial[k];
            copy.hiddenHash[k] = node.hiddenHash[k];
        }
        out.nodes_.emplace(id, std::move(copy));
    }
    out.nextId_ = nextId_;
    out.lastTimes_ = lastTimes_;
    out.lastRunTimes_ = lastRunTimes_;
    out.lastRunKinds_ = lastRunKinds_;
    out.lastNotes_ = lastNotes_;
    out.adopted_ = adopted_;
    out.cacheIntermediates_ = cacheIntermediates_;
    out.cacheLimit_ = cacheLimit_;
    return out;
}

Value Graph::evaluate(NodeId id, const EvalContext& ctx) {
    std::vector<Value> outs = evaluateOutputs(id, ctx);
    return outs.empty() ? Value{} : std::move(outs[0]);
}

Value Graph::evaluateOutput(NodeId id, std::size_t output, const EvalContext& ctx) {
    if (output >= outputSpecs(id).size()) {
        throw GraphError(node(id).typeName() + " has no output " + std::to_string(output));
    }
    std::vector<Value> outs = evaluateOutputs(id, ctx);
    return output < outs.size() ? std::move(outs[output]) : Value{};
}

std::vector<Value> Graph::evaluateOutputs(NodeId id, const EvalContext& ctx) {
    // Strand resolution is the Resample node's: Scatter only places roots and grows them with the
    // most segments any Resample downstream of it asks for (8 without), whichever node is being
    // evaluated (viewNode too, so a node viewed before the Resample shows what the output is made
    // from). Each branch its own: a Resample on the whiskers leaves the fur as it is. With an
    // Output node, a Resample fed nothing or on a branch reaching no Output sets nothing.
    Keys keys;
    if (ctx.strandSegments <= 0) keys.segments = strandResolutions(ctx.viewport);
    const ScopedPathResolver paths(ctx.paths);  // operators and cache keys resolve file paths through it
    const ScopedFileStamps stamps;  // and read each file's stamp once (keys and texture loads alike)
    // Expressions compiled by the operators read this evaluation's time and the groom's maps.
    const ExpressionEnvironment environment = expressionEnvironment(ctx);
    const ExpressionEnvironmentScope expressions(&environment);
    applyControls(ctx);  // keyed params take this evaluation's values (so they key the cache too)
    std::set<NodeId> valuesSet;
    if (cacheIntermediates_) {
        // What matched is cached on the nodes after this; the rest of the old graph's
        // results go now, whether or not the evaluation succeeds (they doubled memory).
        struct Release {
            std::unordered_map<std::uint64_t, Adopted>& adopted;
            ~Release() { adopted.clear(); }
        } release{adopted_};
        // Params a Value input drives take its value (so they key the cache too).
        applyValueInputs(id, ctx, keys, nullptr, valuesSet);
        std::vector<Value> result = evaluateWithKeys(id, ctx, keys, nullptr);
        if (!ctx.keepPreviewCore) result = shownValues(id, std::move(result), true);
        // The nodes it reached (each keyed), for the cache limit's age; then the limit.
        ++tick_;
        for (const auto& [reached, key] : keys.full) {
            if (auto it = nodes_.find(reached); it != nodes_.end()) it->second.usedTick = tick_;
        }
        if (cacheLimit_) trimCache(id);
        if (!adopted_.empty()) {
            // Nodes this evaluation did not run or reach (upstream of an adopted result, or not
            // feeding this output) keep their adopted results too, for the next edit or swap.
            for (auto& [nodeId, node] : nodes_) {
                const std::uint64_t key = computeKey(nodeId, ctx, keys);
                if (node.hasCache && node.cachedKey == key) continue;
                const auto adopted = adopted_.find(key);
                if (adopted == adopted_.end()) continue;
                adopt(node, adopted->second, key, keys.own[nodeId]);
            }
        }
        return result;
    }
    adopted_.clear();
    {
        Transient values;  // the values' own evaluations, kept for nothing after
        applyValueInputs(id, ctx, keys, &values, valuesSet);
    }

    // Count each node's consumers among the nodes this evaluation reaches.
    Transient transient;
    std::vector<NodeId> stack{id};
    std::map<NodeId, bool> seen;
    while (!stack.empty()) {
        const NodeId n = stack.back();
        stack.pop_back();
        if (seen[n]) continue;
        seen[n] = true;
        for (NodeId src : nodeRecord(n).inputs) {
            if (src == kNoNode) continue;
            ++transient.consumers[src];
            stack.push_back(src);
        }
    }
    std::vector<Value> result = evaluateWithKeys(id, ctx, keys, &transient);
    return ctx.keepPreviewCore ? result : shownValues(id, std::move(result), false);
}

std::vector<Value> Graph::shownValues(NodeId id, std::vector<Value> values, bool cached) {
    bool roles = false;
    for (const Value& v : values) {
        const StrandsPtr strands = valueAs<StrandsPtr>(v);
        roles = roles || (strands && strands->strandChannel(kPreviewChannel));
    }
    if (!roles) return values;
    Node& node = nodeRecord(id);
    // The same strands (alive, so never another set made where a freed one was), or both none.
    auto same = [](const std::weak_ptr<const StrandSet>& was, const Value& v) {
        const StrandsPtr now = valueAs<StrandsPtr>(v);
        const StrandsPtr then = was.lock();
        return then == now && (now || was.expired());
    };
    const bool held = cached && node.hasCache && node.cachedValues.size() == values.size();
    if (held && node.shownFrom.size() == values.size()) {
        bool all = true;
        for (std::size_t k = 0; all && k < values.size(); ++k) all = same(node.shownFrom[k], values[k]);
        if (all) return node.shown;
    }
    std::vector<Value> shown = values;
    for (Value& v : shown) {
        if (const StrandsPtr strands = valueAs<StrandsPtr>(v)) v = shownStrands(strands);
    }
    if (!held) return shown;
    node.shown = shown;
    node.shownFrom.resize(values.size());
    for (std::size_t k = 0; k < values.size(); ++k) node.shownFrom[k] = valueAs<StrandsPtr>(values[k]);
    // Which strands the primary output hid, for changesSince.
    std::uint64_t hidden = 0;
    node.shownAt.reset();
    if (const StrandsPtr first = valueAs<StrandsPtr>(values[0])) {
        std::vector<std::uint32_t> at = shownIndices(*first);
        if (!at.empty()) {
            hidden = hash64(at.size());
            for (std::size_t s = 0; s < at.size(); ++s) {
                if (at[s] == UINT32_MAX) hidden = hashCombine(hidden, s);
            }
            node.shownAt = std::make_shared<const std::vector<std::uint32_t>>(std::move(at));
        }
    }
    if (node.hiddenSerial[0] != node.serial) {
        node.hiddenSerial[1] = node.hiddenSerial[0];
        node.hiddenHash[1] = node.hiddenHash[0];
    }
    node.hiddenSerial[0] = node.serial;
    node.hiddenHash[0] = hidden;
    return shown;
}

StrandsPtr Graph::evaluateStrands(NodeId id, const EvalContext& ctx) {
    Value v = evaluate(id, ctx);
    if (auto* strands = std::get_if<StrandsPtr>(&v)) return *strands;
    return nullptr;
}

void Graph::dropCache(Node& node) {
    node.hasCache = false;
    node.cachedValues.clear();
    node.consumed.clear();
    node.changes.clear();
    node.changesBase = 0;
    node.passedOn = false;
    node.shown.clear();
    node.shownFrom.clear();
    node.shownAt.reset();
}

void Graph::clearCache() {
    for (auto& [id, node] : nodes_) dropCache(node);
}

std::size_t Graph::defaultCacheLimit() {
    static const std::size_t limit = [] {
        const char* text = std::getenv("GROOMLAB_CACHE_LIMIT_MB");  // NOLINT(concurrency-mt-unsafe): read once
        if (!text || !*text) return kDefaultCacheLimit;
        char* end = nullptr;
        const double mb = std::strtod(text, &end);
        if (end == text || !(mb >= 0.0)) return kDefaultCacheLimit;
        return static_cast<std::size_t>(mb * 1048576.0);
    }();
    return limit;
}

namespace {

// Each array a node's cached strands (and what its callers were handed of them) hold, once.
void nodeArrays(const std::vector<Value>& values, const std::vector<Value>& shown, std::unordered_map<const void*, std::size_t>* arrays) {
    for (const std::vector<Value>* list : {&values, &shown}) {
        for (const Value& v : *list) {
            if (const StrandsPtr strands = valueAs<StrandsPtr>(v)) {
                strands->forEachArray([&](const void* data, std::size_t bytes) { (*arrays)[data] = bytes; });
            }
        }
    }
}

}  // namespace

std::size_t Graph::cachedBytes() const {
    std::unordered_map<const void*, std::size_t> arrays;
    for (const auto& [id, node] : nodes_) {
        if (node.hasCache) nodeArrays(node.cachedValues, node.shown, &arrays);
    }
    std::size_t total = 0;
    for (const auto& [data, bytes] : arrays) total += bytes;
    return total;
}

void Graph::trimCache(NodeId keep) {
    // Each node's arrays, and how many nodes hold each.
    struct Held {
        NodeId id;
        std::vector<std::pair<const void*, std::size_t>> arrays;
        std::size_t bytes = 0;  // its arrays' bytes
    };
    std::vector<Held> held;
    std::unordered_map<const void*, std::size_t> holders, sizes;
    for (const auto& [id, node] : nodes_) {
        if (!node.hasCache) continue;
        std::unordered_map<const void*, std::size_t> own;
        nodeArrays(node.cachedValues, node.shown, &own);
        Held h{id, {own.begin(), own.end()}, 0};
        for (const auto& [data, bytes] : own) {
            ++holders[data];
            sizes[data] = bytes;
            h.bytes += bytes;
        }
        held.push_back(std::move(h));
    }
    std::size_t total = 0;
    for (const auto& [data, bytes] : sizes) total += bytes;
    if (total <= cacheLimit_) return;
    // The order they go in: not reached by the last evaluation first (the longest unused first),
    // then the cheapest to make again for their bytes (their last run's milliseconds a byte).
    auto cost = [&](const Held& h) {
        const auto t = lastRunTimes_.find(h.id);
        return (t == lastRunTimes_.end() ? 0.0 : t->second) / static_cast<double>(std::max<std::size_t>(h.bytes, 1));
    };
    std::sort(held.begin(), held.end(), [&](const Held& a, const Held& b) {
        const std::uint64_t ua = nodes_.at(a.id).usedTick, ub = nodes_.at(b.id).usedTick;
        if (ua != ub) return ua < ub;
        const double ca = cost(a), cb = cost(b);
        if (ca != cb) return ca < cb;
        return a.id < b.id;
    });
    for (const Held& h : held) {
        if (total <= cacheLimit_) break;
        if (h.id == keep) continue;
        // (A result whose arrays others hold too frees nothing: it stays.)
        std::size_t freed = 0;
        for (const auto& [data, bytes] : h.arrays) freed += holders[data] == 1 ? bytes : 0;
        if (freed == 0) continue;
        for (const auto& [data, bytes] : h.arrays) --holders[data];
        dropCache(nodes_.at(h.id));
        ++evictions_;
        total -= std::min(total, freed);
    }
}

}  // namespace groomlab
