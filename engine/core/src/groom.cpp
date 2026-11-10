// SPDX-License-Identifier: Apache-2.0
#include "groomlab/groom.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "groomlab/curve.h"
#include "groomlab/field.h"
#include "groomlab/flow.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/instances.h"
#include "groomlab/parts.h"
#include "groomlab/strand_set.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "groomlab/random.h"
#include "groomlab/scalp_pieces.h"

namespace groomlab {

const char* scaleSourceName(ScaleSource source) {
    switch (source) {
        case ScaleSource::Transform: return "transform";
        case ScaleSource::Off: return "off";
        default: return "area";
    }
}

ScaleSource scaleSourceFromName(const std::string& name) {
    if (name == "transform") return ScaleSource::Transform;
    if (name == "off") return ScaleSource::Off;
    return ScaleSource::Area;
}

void setGroomExternals(ExternalInputs& externals, const MeshPtr& scalp, const GuidesPtr& guides,
                       const HierarchyPtr& hierarchy, const PartsPtr& parts, const FlowPtr& flow) {
    if (parts && !parts->empty()) externals.set("parts", parts, parts->contentHash());
    if (flow && !flow->empty()) externals.set("flow", flow, flow->contentHash());
    // Keyed by its surface: the nodes reading a color set (a vertex map) salt their keys with it,
    // so painting one re-evaluates only them and what follows (Scatter, Interpolate stay cached).
    if (scalp) externals.set("scalp", scalp, scalp->surfaceHash());
    // Several scalps (scalp_pieces.h): each one's own mesh, for the Scalp node picking it.
    for (const auto& [name, piece] : scalpPieces(scalp)) {
        externals.set(scalpPieceExternal(name), piece,
                      hashCombine(piece->surfaceHash(), static_cast<std::uint64_t>(piece->wholeFaceOffset())));
    }
    if (guides && !guides->empty()) externals.set("guides", guides, guides->contentHash());
    // A hierarchy of groups alone too (no guides drawn yet): a Guides node on a group made empty
    // gives no guides instead of nothing (sources.cpp).
    if (hierarchy && (!hierarchy->guides().empty() || !hierarchy->groups().empty())) {
        externals.set("hierarchy", hierarchy, hierarchy->contentHash());
    }
}

void setMeshExternals(ExternalInputs& externals, const std::map<std::string, MeshPtr>& meshes) {
    for (const auto& [name, mesh] : meshes) {
        if (mesh) externals.set("mesh:" + name, mesh, mesh->contentHash());
    }
}

void setControlExternals(ExternalInputs& externals, const std::map<std::string, float>& controls) {
    for (const auto& [name, value] : controls) {
        externals.set(controlInputName(name), constantField(value), hashFloat(hash64(0xC7), value));
    }
}

void setMapExternals(ExternalInputs& externals, const MapCatalog& maps) {
    if (maps.empty()) return;
    std::uint64_t h = hash64(0x3A95);
    for (const auto& [name, info] : maps) h = hashString(hashString(hashString(h, name), info.kind), info.texture);
    externals.set("maps", std::make_shared<const MapCatalogValue>(MapCatalogValue{maps}), h);
}

void setScalpHistoryExternal(ExternalInputs& externals, const ScalpHistoryPtr& history) {
    if (history) externals.set("scalpHistory", history, history->hash);
}

int scalpHistoryFrames(const Graph& graph, NodeId output, int viewport) {
    if (!graph.hasNode(output)) return 0;
    std::int64_t frames = 0;
    graph.forEachActiveUpstream(output, [&](const std::string&, const Operator& op) {
        if (op.typeName() == op::kJiggle) {
            frames = std::max(frames, op.params().getInt("history"));
        }
        return true;
    }, {}, viewport);
    return static_cast<int>(frames);
}

std::vector<double> scalpHistoryLoopFrames(const Graph& graph, NodeId output, double now, int viewport) {
    if (!graph.hasNode(output)) return {};
    const auto plain = static_cast<double>(scalpHistoryFrames(graph, output, viewport));
    std::vector<double> frames;
    graph.forEachActiveUpstream(output, [&](const std::string&, const Operator& op) {
        if (op.typeName() != op::kJiggle || !op.params().getBool("loop")) return true;
        const auto start = static_cast<double>(op.params().getInt("loopStart")),
                   end = static_cast<double>(op.params().getInt("loopEnd"));
        for (std::int64_t k = 1; k <= op.params().getInt("history"); ++k) {
            const double frame = jiggleLoopFrame(now - static_cast<double>(k), now, start, end);
            const double back = now - frame;
            // (Ones among the plain history's, or now itself, the history has.)
            if (std::abs(back - std::round(back)) < 1e-3 && back > -0.5 && back < plain + 0.5) continue;
            frames.push_back(frame);
        }
        return true;
    }, {}, viewport);
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end(), [](double a, double b) { return std::abs(a - b) < 1e-6; }), frames.end());
    return frames;
}

bool graphReadsTime(const Graph& graph, NodeId output) {
    if (!graph.hasNode(output)) return false;
    EvalContext then, now;
    then.time = 0.0;
    now.time = 4.0 / 3.0;
    auto moves = [&](const Operator& op) {
        return op.typeName() == op::kJiggle || op.typeName() == op::kSimulate || op.contextSalt(then) != op.contextSalt(now);
    };
    std::vector<NodeId> stack{output}, seen;
    while (!stack.empty()) {
        const NodeId id = stack.back();
        stack.pop_back();
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
        seen.push_back(id);
        const Operator& op = graph.node(id);
        if (moves(op)) return true;
        bool inner = false;
        op.forEachActiveInner("", [&](const std::string&, const Operator& o) {
            inner = moves(o);
            return !inner;
        });
        if (inner) return true;
        for (std::size_t slot = 0; slot < op.inputs().size(); ++slot) {
            NodeId source = 0;
            if (graph.inputSource(id, slot, &source)) stack.push_back(source);
        }
    }
    return false;
}

GuideSet guidesToSimulate(const HierarchyPtr& hierarchy, const GuidesPtr& flat, const std::string& group) {
    if (group != "*") {
        const GroupId gid = hierarchy ? hierarchy->findGroup(group) : kNoGroup;
        if (gid == kNoGroup) throw std::runtime_error("no guide group named " + group);
        return hierarchy->flattenGroup(gid);
    }
    GuideSet out;
    if (hierarchy && !hierarchy->guides().empty()) out = hierarchy->flatten(-1);
    if (flat) {
        for (const Guide& g : flat->guides()) {
            Guide& added = out.add(g.id, g.points);
            added.root = g.root;
        }
    }
    return out;
}

void applyGuidePoses(GuideHierarchy& hierarchy, const GuideSet& poses, const ScalpMesh* scalp) {
    std::vector<GuideId> posed;
    for (const Guide& pose : poses.guides()) {
        if (pose.points.size() < 2 || !hierarchy.guide(pose.id)) continue;
        hierarchy.pose(pose.id, pose.points);
        posed.push_back(pose.id);
    }
    if (!posed.empty()) hierarchy.evaluateDescendants(scalp, posed);
}

StrandsPtr pinRestDistances(const StrandsPtr& strands, const StrandSet& rest) {
    if (!strands || !strands->instances() || !strands->instances()->needsRestPose()) return strands;
    std::unordered_map<StrandId, std::size_t> index;
    index.reserve(rest.strandCount());
    for (std::size_t s = 0; s < rest.strandCount(); ++s) index.emplace(rest.ids()[s], s);
    std::unordered_map<StrandId, float> lengths;  // worked out once per strand
    auto instances = std::make_shared<InstanceSet>(*strands->instances());
    for (InstanceSet::Instance& i : instances->instances) {
        if (!i.pinToRest && !i.fitStrand && !i.onSkin) continue;
        if (i.pinToRest) i.restDistance = -1.0f;
        if (i.fitStrand || i.onSkin) i.restLength = -1.0f;
        auto known = lengths.find(i.strand);
        if (known == lengths.end()) {
            auto it = index.find(i.strand);
            float length = -1.0f;
            if (it != index.end()) {
                const std::size_t n = rest.strandPointCount(it->second);
                length = n > 1 ? arcLength(rest.strandPoints(it->second), n) : 0.0f;
            }
            known = lengths.emplace(i.strand, length).first;
        }
        if (known->second < 0.0f) continue;  // no such strand at rest: at t, sized by its length now
        if (i.pinToRest) i.restDistance = std::clamp(i.t, 0.0f, 1.0f) * known->second;
        if (i.fitStrand || i.onSkin) i.restLength = known->second;
    }
    auto out = std::make_shared<StrandSet>(*strands);
    out->setInstances(std::move(instances));
    return out;
}

RestPose::RestPose(const std::string& graphJson, const RestPose* previous) : RestPose(loadGraphJson(graphJson), previous) {}

RestPose::RestPose(GraphDocument doc, const RestPose* previous) : doc_(std::make_unique<GraphDocument>(std::move(doc))) {
    if (previous) {
        doc_->graph.adoptCache(previous->doc_->graph);
        setCacheIntermediates(previous->cacheIntermediates_);
    }
}

RestPose::~RestPose() = default;

void RestPose::setCacheIntermediates(bool enabled) {
    cacheIntermediates_ = enabled;
    doc_->graph.setCacheIntermediates(enabled);
}

StrandsPtr RestPose::pin(const StrandsPtr& strands, const std::string& node, const RestInputs& in, const EvalContext& ctx) {
    if (!strands || !strands->instances() || !strands->instances()->needsRestPose()) return strands;
    NodeId id = doc_->output;
    if (!node.empty()) {
        auto it = doc_->nodesByName.find(node);
        if (it == doc_->nodesByName.end()) return strands;
        id = it->second;
    }
    // The scalp at rest, made once per rest pose (the same on every frame of a deforming scalp).
    MeshPtr scalp;
    if (in.scalp) {
        const std::uint64_t key = in.scalp->restPoseHash();
        if (!restScalp_ || key != restScalpKey_) {
            restScalp_ = in.scalp->hasRest() ? std::make_shared<const ScalpMesh>(in.scalp->restPose()) : in.scalp;
            restScalpKey_ = key;
        }
        scalp = restScalp_;
    } else {
        restScalp_.reset();
    }
    const std::uint64_t scalpHash = scalp ? scalp->surfaceHash() : 0;  // color sets play no part below
    // The hierarchy as authored, on the rest scalp.
    HierarchyPtr hierarchy;
    if (in.hierarchy) {
        if (in.hierarchyOnScalp && !(in.scalp && in.scalp->hasRest())) {
            hierarchy = in.hierarchy;
        } else {
            const std::uint64_t key = hashCombine(in.hierarchy->contentHash(), scalpHash);
            if (!hierarchy_ || key != hierarchyKey_) {
                auto h = std::make_shared<GuideHierarchy>(*in.hierarchy);
                h->evaluate(scalp.get());
                hierarchy_ = std::move(h);
                hierarchyKey_ = key;
            }
            hierarchy = hierarchy_;
        }
    }
    PartsPtr parts;
    if (in.parts && !in.parts->empty() && scalp) {
        const std::uint64_t key = hashCombine(in.parts->contentHash(), scalpHash);
        if (!parts_ || key != partsKey_) {
            parts_ = std::make_shared<const PartLines>(*in.parts, *scalp);
            partsKey_ = key;
        }
        parts = parts_;
    }
    FlowPtr flow;
    if (in.flow && !in.flow->empty() && scalp) {
        const std::uint64_t key = hashCombine(in.flow->contentHash(), scalpHash);
        if (!flow_ || key != flowKey_) {
            flow_ = std::make_shared<const FlowField>(*in.flow, *scalp);
            flowKey_ = key;
        }
        flow = flow_;
    }
    ExternalInputs externals;
    setGroomExternals(externals, scalp, in.guides, hierarchy, parts, flow);
    setControlExternals(externals, in.controls);
    setMapExternals(externals, in.maps);  // no colliders, no scalp history: nothing moves at rest

    EvalContext restCtx = ctx;
    restCtx.time = 0.0;
    restCtx.buildCards = false;
    restCtx.externals = &externals;
    StrandsPtr rest;
    const std::uint64_t key = hashCombine(hashCombine(externals.hash(), restCtx.hash()), id);
    if (!cacheIntermediates_ && rest_ && key == restKey_) {
        rest = rest_;  // nothing cached in the graph: the same inputs, the same rest pose
    } else {
        const std::size_t before = doc_->graph.evaluationCount();
        rest = doc_->graph.evaluateStrands(id, restCtx);
        if (doc_->graph.evaluationCount() != before) ++evaluations_;
        rest_ = rest;
        restKey_ = key;
    }
    if (!rest) return strands;
    if (strands == lastIn_ && rest == lastRest_ && lastOut_) return lastOut_;
    lastOut_ = pinRestDistances(strands, *rest);
    lastIn_ = strands;
    lastRest_ = rest;
    return lastOut_;
}

}  // namespace groomlab
