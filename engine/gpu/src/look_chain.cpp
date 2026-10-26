// SPDX-License-Identifier: Apache-2.0
#include "groomlab/gpu/look_chain.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <variant>

#include "groomlab/curve.h"
#include "groomlab/export_view.h"
#include "groomlab/feathers.h"
#include "groomlab/field.h"
#include "groomlab/flow.h"
#include "groomlab/look/look_cpu.h"
#include "groomlab/map_ref.h"
#include "groomlab/math.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/point_index.h"
#include "groomlab/preview_core.h"
#include "groomlab/random.h"
#include "groomlab/sculpt.h"

namespace groomlab::gpu {

namespace {

constexpr std::size_t kMaxInterpolateSamples = 64;  // look.slang MAX_SAMPLES

// The node feeding `id`'s input `slot` (from its primary output), or 0.
NodeId upstream(const Graph& graph, NodeId id, std::size_t slot = 0) {
    NodeId src = 0;
    std::size_t output = 0;
    // (Or from any output of a Split: a run's first stage, its result the output it hands, inputStrands.)
    if (!graph.inputSource(id, slot, &src, &output) || (output != 0 && graph.node(src).typeName() != op::kSplit)) return 0;
    return src;
}
// Which output of the node feeding `id`'s input `slot` it takes (0: none, or its first).
std::size_t upstreamOutput(const Graph& graph, NodeId id, std::size_t slot = 0) {
    NodeId src = 0;
    std::size_t output = 0;
    return graph.inputSource(id, slot, &src, &output) ? output : 0;
}

// The node whose result `id` hands on, where it hands one on: a Switch's picked input (SwitchOp:
// the very strands of input a or b by its index), any other node's (and a bypassed Switch's) first.
std::size_t handedSlot(const Graph& graph, NodeId id) {
    return graph.node(id).typeName() == op::kSwitch && !graph.bypassedFor(id, true) && graph.node(id).params().getInt("index") != 0 ? 1 : 0;
}
NodeId handedFrom(const Graph& graph, NodeId id) { return upstream(graph, id, handedSlot(graph, id)); }

// Whether a Control node is plugged into `param` of node `id` (ControlOp: a constant field).
bool pluggedControl(const Graph& graph, NodeId id, const std::string& param) {
    const std::vector<std::string>& exposed = graph.paramInputs(id);
    for (std::size_t k = 0; k < exposed.size(); ++k) {
        NodeId src = 0;
        if (exposed[k] == param && graph.inputSource(id, graph.node(id).inputs().size() + k, &src)) {
            return graph.node(src).typeName() == op::kControl;
        }
    }
    return false;
}

// ---- Affect masks ----
// Whether a field is plugged into node `id`'s affect.
bool affectPluggedIn(const Graph& graph, NodeId id) {
    const std::vector<std::string>& exposed = graph.paramInputs(id);
    for (std::size_t k = 0; k < exposed.size(); ++k) {
        if (exposed[k] == kAffectParam && graph.inputSource(id, graph.node(id).inputs().size() + k, nullptr)) return true;
    }
    return false;
}
// Whether node `id` is restricted by its affect (a map, a plugged field, a number between 0 and 1):
// the graph mixes its result back toward its input (applyAffect).
bool maskedNode(const Graph& graph, NodeId id) {
    return (graph.node(id).params().has(kAffectParam) && affectOf(graph.node(id)) == AffectKind::Mask) || affectPluggedIn(graph, id);
}

// A node's params as its last evaluation read them: the fields and curves plugged into its exposed
// params set (the graph sets them for an evaluation only), from their sources' cached results.
ParamSet evaluatedParams(const Graph& graph, NodeId id) {
    ParamSet params = graph.node(id).params();
    const std::vector<std::string>& exposed = graph.paramInputs(id);
    const std::size_t own = graph.node(id).inputs().size();
    for (std::size_t k = 0; k < exposed.size(); ++k) {
        NodeId src = 0;
        std::size_t output = 0;
        if (!graph.inputSource(id, own + k, &src, &output)) continue;  // nothing plugged: its own value
        const Value value = graph.cachedValue(src, output);
        if (FieldPtr field = valueAs<FieldPtr>(value)) params.setField(exposed[k], field);
        if (CurvePtr curve = valueAs<CurvePtr>(value)) params.setCurve(exposed[k], curve);
        // A Value in a Field slot: its number on every strand (one in a Value slot set the param
        // itself before the CPU's evaluation: the node's params hold it).
        const UniformPtr uniform = valueAs<UniformPtr>(value);
        if (uniform && !graph.paramDriven(id, exposed[k])) {
            if (const FieldPtr constant = uniformField(*uniform)) params.setField(exposed[k], constant);
        }
    }
    return params;
}

StrandsPtr cachedStrands(const Graph& graph, NodeId id) { return id ? valueAs<StrandsPtr>(graph.cachedValue(id)) : nullptr; }
// The strands node `id` takes at input `slot`, as cached (from whichever output feeds it).
StrandsPtr inputStrands(const Graph& graph, NodeId id, std::size_t slot = 0) {
    NodeId src = 0;
    std::size_t output = 0;
    return graph.inputSource(id, slot, &src, &output) ? valueAs<StrandsPtr>(graph.cachedValue(src, output)) : nullptr;
}
// A chain's base strands as cached: the output of its base node it takes (a Split's b, c or d).
StrandsPtr baseStrands(const Graph& graph, const LookChain& chain) {
    return chain.base ? valueAs<StrandsPtr>(graph.cachedValue(chain.base, chain.baseOutput)) : nullptr;
}
// Stage k's result as cached (a Cull's, Split's or Mirror's: the base's).
StrandsPtr stageResult(const Graph& graph, const LookChain& chain, std::size_t k) {
    return k == 0 && chain.stages.front().node == chain.base ? baseStrands(graph, chain) : cachedStrands(graph, chain.stages[k].node);
}

// What the GPU knows of a node type (look_stages.inc): the stage it runs as, the params a drag
// may set while it previews, whether its points go through as they came.
struct GpuNode {
    bool staged = false;
    StageKind kind = StageKind::Noise;
    std::set<std::string> ramps, vectors, numbers;
    bool passThrough = false;
};

std::set<std::string> words(const char* text) {
    std::set<std::string> out;
    std::istringstream in(text);
    for (std::string w; in >> w;) out.insert(w);
    return out;
}

const GpuNode* gpuNode(const std::string& type) {
    static const std::unordered_map<std::string, GpuNode> nodes = [] {
        std::unordered_map<std::string, GpuNode> out;
        auto row = [&](const char* type, bool staged, StageKind kind, const char* ramps, const char* vectors, const char* numbers) {
            GpuNode& n = out[type];
            n.staged = staged;
            n.kind = kind;
            n.ramps = words(ramps);
            n.vectors = words(vectors);
            n.numbers = words(numbers);
        };
#define GROOMLAB_STAGE(Kind, name, mapped0, mapped1, ramps, vectors, numbers) row(#Kind, true, StageKind::Kind, ramps, vectors, numbers);
#define GROOMLAB_STAGE_NODE(Type, Kind, ramps, vectors, numbers) row(#Type, true, StageKind::Kind, ramps, vectors, numbers);
#define GROOMLAB_GPU_PARAMS(Type, ramps, vectors, numbers) row(#Type, false, StageKind::Noise, ramps, vectors, numbers);
#define GROOMLAB_PASS_THROUGH(Type) out[#Type].passThrough = true;
#include "groomlab/look/look_stages.inc"
#undef GROOMLAB_STAGE
#undef GROOMLAB_STAGE_NODE
#undef GROOMLAB_GPU_PARAMS
#undef GROOMLAB_PASS_THROUGH
        return out;
    }();
    const auto it = nodes.find(type);
    return it == nodes.end() ? nullptr : &it->second;
}

bool stageOf(const std::string& type, StageKind* kind) {
    const GpuNode* node = gpuNode(type);
    if (!node || !node->staged) return false;
    *kind = node->kind;
    return true;
}

// A Collide's surfaces (its scalp and collider inputs, as last evaluated).
// `colliders`: which of them are the collider input's, a bit each (softness and smoothing act on it).
std::vector<MeshPtr> collideMeshes(const Graph& graph, NodeId id, std::uint32_t* colliders = nullptr) {
    std::vector<MeshPtr> meshes;
    if (colliders) *colliders = 0;
    for (std::size_t slot = 1; slot < 3; ++slot) {
        NodeId src = 0;
        std::size_t output = 0;
        if (!graph.inputSource(id, slot, &src, &output)) continue;
        const MeshPtr mesh = valueAs<MeshPtr>(graph.cachedValue(src, output));
        if (!mesh || mesh->triangleCount() == 0) continue;
        if (colliders && slot == 2) *colliders |= 1u << meshes.size();
        meshes.push_back(mesh);
    }
    return meshes;
}

// A Collide pushing out of a surface deformed away from its rest pose (an animated scalp, a
// collider given its rest pose): it asks the rest pose's field through each vertex's frame
// (RestPoseCollision), which the GPU does not. The CPU's: the GPU runs from the node after it.
bool collidesWithADeformedMesh(const Graph& graph, NodeId id) {
    for (const MeshPtr& mesh : collideMeshes(graph, id)) {
        if (mesh->hasRest() && mesh->restPoints() != mesh->points()) return true;
    }
    return false;
}

// A Jiggle handing its input back as it came (no history of the scalp's motion, or a scalp that
// did not move: JiggleOp gives the very strands it took); a MeshCut too (no mesh to cut by).
bool jiggleHandsOn(const Graph& graph, NodeId id) {
    const NodeId in = upstream(graph, id);
    const StrandsPtr out = in ? valueAs<StrandsPtr>(graph.cachedValue(id)) : nullptr;
    return out && out == valueAs<StrandsPtr>(graph.cachedValue(in));
}

// Nodes that add channels and leave the points as they came (their strands share the points of
// those they take): the GPU's points go through them (look_stages.inc GROOMLAB_PASS_THROUGH, which
// says why each). (What they write shows, in a color mode or as the lit strands' own colors, as
// the CPU's last evaluation wrote it: the item's colors stream stays bound through the drag, as the
// colors of any drag do.)
bool channelsOnly(const std::string& type) {
    const GpuNode* node = gpuNode(type);
    return node && node->passThrough;
}

// A Bands node whose colors depend on the strands' shapes: guard hairs picked as the longest (its
// band colors go by point index, FeatherColor's patterns by vane space, which no stage changes).
bool readsLengths(const Graph& graph, NodeId id) {
    const ParamSet& p = graph.node(id).params();
    return p.getString("guardsBy") == "longest" && p.getDouble("guards") < 1.0;
}

// A Curl that added points to strands too few a turn (curl_op.cpp: its tolerance), as last
// evaluated: its result is laid out unlike its input, and a run keeps the base's layout. The CPU's:
// the GPU runs from the node after it.
bool curlAddsPoints(const Graph& graph, NodeId id) {
    const NodeId in = upstream(graph, id);
    const StrandsPtr out = valueAs<StrandsPtr>(graph.cachedValue(id));
    const StrandsPtr before = in ? valueAs<StrandsPtr>(graph.cachedValue(in)) : nullptr;
    return out && before && out->pointCount() != before->pointCount();
}

// A Resample that hands its input on as it came whatever the points are: even, the strands at its count.
bool evenResample(const Graph& graph, NodeId id) {
    const ParamSet& p = graph.node(id).params();
    return p.getString("mode") == "even";
}

// A Wet at a wetness of 0: dry, it hands its input on as it came (WetOp) and runs no level.
bool dryWet(const Graph& graph, NodeId id) {
    return graph.node(id).typeName() == op::kWet && !(graph.node(id).params().getDouble("wetness") > 0.0);
}

// A Cull's keep or a Split's select (mode map) the run cannot change: a number, a texture or a color
// set read at the roots (the strands it keeps stay the ones its last evaluation kept); a Mirror's
// sides go by the roots on the rest scalp. Anything else (a channel, an expression, a guide map, a
// field plugged in) may read what the run moves: the CPU's.
bool picksHold(const Graph& graph, NodeId id) {
    const Operator& node = graph.node(id);
    const ParamSet& p = node.params();
    std::string param;
    if (node.typeName() == op::kCull) param = "keep";
    if (node.typeName() == op::kSplit && p.getString("mode") == "map") param = "select";
    if (param.empty()) return true;
    if (p.field(param)) return false;
    const auto* map = std::get_if<MapRef>(&p.get(param));
    return !map || map->source == MapRef::Source::Texture || map->source == MapRef::Source::ColorSet;
}

// A stage node's stages, its last first (findLookChainTo gathers them from the output up): a
// Wet's levels, each a Clump stage, the smallest last.
void pushStages(const Graph& graph, NodeId id, StageKind kind, std::vector<LookStage>* stages) {
    if (graph.node(id).typeName() != op::kWet) {
        stages->push_back({kind, id});
        return;
    }
    for (std::int64_t j = wetLevelCount(graph.node(id).params()); j-- > 0;) {
        LookStage s;
        s.kind = StageKind::Clump;
        s.node = id;
        s.level = static_cast<int>(j);
        stages->push_back(s);
    }
}

}  // namespace

namespace {

// The ramp params a stage reads (look_stages.inc: an override of it is a ramp, not a number).
bool rampParam(const std::string& type, const std::string& param) {
    const GpuNode* node = gpuNode(type);
    return node && node->ramps.count(param);
}
// The vector params a stage reads (an override of it is a Vec3f).
bool vectorParam(const std::string& type, const std::string& param) {
    const GpuNode* node = gpuNode(type);
    return node && node->vectors.count(param);
}

}  // namespace

bool previewableParam(const std::string& type, const std::string& param) {
    // look_stages.inc: its ramps, vectors and numbers (its seed, its affect where it is masked).
    const GpuNode* node = gpuNode(type);
    return node && (node->ramps.count(param) || node->vectors.count(param) || node->numbers.count(param));
}

bool previewableOverrides(const GraphDocument& doc, const Overrides& overrides, std::string* why) {
    for (const auto& [node, values] : overrides) {
        const auto it = doc.nodesByName.find(node);
        if (it == doc.nodesByName.end()) {
            *why = "no node '" + node + "'";
            return false;
        }
        const std::string type = doc.graph.node(it->second).typeName();
        for (const auto& [param, value] : values) {
            if (!previewableParam(type, param)) {
                *why = node + "." + param + " is the CPU's";
                return false;
            }
            // A Value input sets it: a number dragged over it does nothing (as on the CPU).
            if (doc.graph.paramDriven(it->second, param)) {
                *why = node + "." + param + " is set by a Value input (the CPU's)";
                return false;
            }
            // An affect of none of its own: the node hands its input on, no stage of the run.
            if (param == kAffectParam && doc.graph.node(it->second).params().has(kAffectParam) &&
                affectOf(doc.graph.node(it->second)) == AffectKind::None && !affectPluggedIn(doc.graph, it->second)) {
                *why = node + " works nowhere at its own affect (the CPU's)";
                return false;
            }
            if (rampParam(type, param) != std::holds_alternative<Ramp>(value) ||
                vectorParam(type, param) != std::holds_alternative<Vec3f>(value)) {
                *why = node + "." + param +
                       (rampParam(type, param) ? " takes a ramp" : vectorParam(type, param) ? " takes a vector" : " takes a number");
                return false;
            }
        }
    }
    return true;
}

namespace {

// ---- Affect masks ----
// Every stage restricted by its affect is mixed back by it one way, after it (StageParams::masked,
// look.slang AffectCS), but Tie: its affect also picks which strands it ties and mixes its tail
// channel, so its kernels read it themselves (StageParams flag[1-2]).
bool mixesByItsAffect(StageKind kind) { return kind == StageKind::Tie; }
// The shapers reading their neighbours or a reference.
bool readsNeighbours(StageKind kind) {
    return kind == StageKind::Smooth || kind == StageKind::RestoreLength || kind == StageKind::BlendNeighbors || kind == StageKind::Tangle ||
           kind == StageKind::Flow;
}

// What a node is to the chain: a node handing its input (or its points) on as it came, a stage, the
// end of the run (the CPU's), or one that refuses the GPU its preview (*why).
enum class Role { PassOn, Stage, End, Refuse };
// `output`: the output of the node the chain takes (a Split's b, c or d).
Role roleOf(const GraphDocument& doc, NodeId id, StageKind* kind, std::string* why, std::size_t output = 0) {
    const Graph& graph = doc.graph;
    const std::string type = graph.node(id).typeName();
    // Off in the viewport (bypassed, or set to run in renders only): it hands its input on.
    const bool off = graph.bypassedFor(id, true);
    // Restricted by its affect (a mask, or a field plugged into it): a stage all the same, its result
    // mixed back by it (StageParams::masked; mixesByItsAffect: its own kernels). Of none: its input
    // handed on.
    const AffectKind affect = graph.node(id).params().has(kAffectParam) ? affectOf(graph.node(id)) : AffectKind::All;
    const bool affectPlugged = affectPluggedIn(graph, id);
    // (A Tie reads its mask itself: which strands it ties, the result mixed by it per strand; the
    // shapers reading their neighbours or a reference mix their results by it as the graph does; any
    // other stage is mixed after it: AffectCS.)
    // A RestoreLength with nothing to restore from hands its input on (RestoreLengthOp).
    if (type == op::kRestoreLength && graph.node(id).params().getString("mode") != "even" && !graph.inputSource(id, 1, nullptr)) {
        return Role::PassOn;
    }
    if (type == op::kBands && !off && readsLengths(graph, id)) {
        // Its colors follow the strands' lengths, which the run before it changes: they would
        // show as the CPU's last evaluation left them.
        *why = doc.nameOf(id) + " picks its guard hairs by length (the CPU's)";
        return Role::Refuse;
    }
    if (type == op::kOutput || off || (affect == AffectKind::None && !affectPlugged) || type == op::kSwitch ||
        (type == op::kResample && evenResample(graph, id)) || channelsOnly(type) ||
        // A Subdivide of no viewport levels hands its input on in the viewport (SubdivideOp), as one
        // does while a host drags a param (EvalContext::liveDrag: its strands handed on as they came).
        (type == op::kSubdivide && (graph.node(id).params().getInt("viewportLevels") == 0 || jiggleHandsOn(graph, id))) ||
        ((type == op::kJiggle || type == op::kMeshCut) && jiggleHandsOn(graph, id)) ||
        dryWet(graph, id)) {
        return Role::PassOn;
    }
    if (type == op::kMerge) {
        // Nothing on b: a handed on as it came. A seam blended (b's strands lean toward a's): the CPU's.
        const StrandsPtr out = cachedStrands(graph, id), a = inputStrands(graph, id, 0);
        if (out && a && out == a) return Role::PassOn;
        const ParamSet& p = graph.node(id).params();
        if (p.getDouble("blend") > 0.0 && p.getDouble("match") > 0.0) return Role::End;
    }
    if (type == op::kCull || type == op::kSplit || type == op::kMirror) {
        // Every strand kept, none added: its input handed on as it came. Strands kept by what the run
        // may change: the CPU's.
        const StrandsPtr out = valueAs<StrandsPtr>(graph.cachedValue(id, output)), in = inputStrands(graph, id);
        if (out && in && out == in) return Role::PassOn;
        if (!picksHold(graph, id)) return Role::End;
    }
    if (stageOf(type, kind) && (*kind != StageKind::Interpolate || upstream(graph, id, 1)) &&
        (*kind != StageKind::Collide || !collidesWithADeformedMesh(graph, id)) && (*kind != StageKind::Curl || !curlAddsPoints(graph, id))) {
        return Role::Stage;
    }
    return Role::End;
}

// A Blend's looks, each a run of the GPU's nodes up from its input to where they part: the first
// node both come from. The nodes of look a then b (each from the Blend up) into `a` and `b`, that
// node into *parted; false when they part nowhere the GPU runs both to, or a look has a Width (its
// widths would differ from the other's: the CPU's).
bool blendLooks(const GraphDocument& doc, NodeId blend, std::vector<NodeId>* a, std::vector<NodeId>* b, NodeId* parted) {
    const Graph& graph = doc.graph;
    // The nodes from `id` up the chain could take, the first it could not (or a Blend) last.
    auto path = [&](NodeId id) {
        std::vector<NodeId> nodes;
        for (int guard = 0; id && guard < 256; ++guard) {
            nodes.push_back(id);
            StageKind kind;
            std::string ignored;
            const Role role = roleOf(doc, id, &kind, &ignored);
            if (role == Role::End || role == Role::Refuse || (role == Role::Stage && kind == StageKind::Blend)) break;
            id = handedFrom(graph, id);
        }
        return nodes;
    };
    const std::vector<NodeId> up[2] = {path(upstream(graph, blend, 0)), path(upstream(graph, blend, 1))};
    for (std::size_t j = 0; j < up[1].size(); ++j) {
        const auto at = std::find(up[0].begin(), up[0].end(), up[1][j]);
        if (at == up[0].end()) continue;
        a->assign(up[0].begin(), at);
        b->assign(up[1].begin(), up[1].begin() + static_cast<std::ptrdiff_t>(j));
        *parted = up[1][j];
        for (const std::vector<NodeId>* look : {a, b}) {
            for (const NodeId id : *look) {
                if (graph.node(id).typeName() == op::kWidth && !graph.bypassedFor(id, true)) return false;
                // (A Cull, Split or Mirror lays a look out unlike the other: a run's first stage alone.)
                StageKind kind;
                std::string ignored;
                if (roleOf(doc, id, &kind, &ignored) == Role::Stage && gathers(kind)) return false;
            }
        }
        return true;
    }
    return false;
}

// Each stage's `from` (and a Blend's `from2`): the stage whose result its input is, through nodes
// handing it on (kNoStage: the base's, or anything not a stage of the chain).
void linkStages(const Graph& graph, LookChain* c) {
    const std::set<NodeId> passOn(c->passOn.begin(), c->passOn.end());
    auto stageAbove = [&](NodeId id) {
        for (int guard = 0; id && guard < 256; ++guard) {
            std::size_t k = c->indexOf(id);
            if (k != kNoStage) {
                while (k + 1 < c->stages.size() && c->stages[k + 1].node == id) ++k;  // its last (a Wet's smallest level)
                return k;
            }
            // (The base: a Cull, Split or Mirror is one and the first stage too, its result the base's.)
            if (id == c->base || !passOn.count(id)) break;
            id = handedFrom(graph, id);
        }
        return kNoStage;
    };
    for (std::size_t k = 0; k < c->stages.size(); ++k) {
        LookStage& s = c->stages[k];
        // A Wet's smaller levels: each takes the one before's result.
        s.from = s.level > 0 && k > 0 ? k - 1 : stageAbove(upstream(graph, s.node, 0));
        s.from2 = s.kind == StageKind::Blend || s.kind == StageKind::RestoreLength ? stageAbove(upstream(graph, s.node, 1)) : kNoStage;
    }
}

}  // namespace

bool findLookChain(const GraphDocument& doc, LookChain* chain, std::string* why) { return findLookChainTo(doc, doc.output, chain, why); }

bool findLookChainTo(const GraphDocument& doc, NodeId end, LookChain* chain, std::string* why) {
    const Graph& graph = doc.graph;
    LookChain c;
    c.output = end;
    NodeId id = end;
    if (!id || !graph.hasNode(id)) {
        *why = "no output";
        return false;
    }
    // From the output up: nodes handing their input on as it came, and stages, until the first
    // other node: the base. A Blend's looks up to where they part (look b's nodes first: the list
    // is reversed), the walk going on from there.
    std::vector<LookStage> stages;  // output first
    std::size_t merged = kNoStage;  // the Merge's place among stages (those pushed after it are up its input a)
    std::size_t port = 0;           // the output of `id` the node below it takes
    for (int guard = 0; guard < 256; ++guard) {
        StageKind kind;
        Role role = roleOf(doc, id, &kind, why, port);
        if (role == Role::Refuse) return false;
        if (role == Role::Stage && kind == StageKind::Merge) {
            if (merged != kNoStage) role = Role::End;  // one Merge a run
            else merged = stages.size();
        }
        // Up a Merge's input a, a Cull, Split or Mirror (a run of its own up its input) ends the run.
        if (role == Role::Stage && gathers(kind) && merged != kNoStage) role = Role::End;
        if (role == Role::End) {
            c.base = id;
            c.baseOutput = port;
            break;
        }
        if (role == Role::PassOn) {
            c.passOn.push_back(id);
        } else if (kind == StageKind::Blend) {
            std::vector<NodeId> looks[2];
            NodeId parted = 0;
            // (Parting at a Split: which of its outputs each look takes is the CPU's.)
            if (!blendLooks(doc, id, &looks[0], &looks[1], &parted) || graph.node(parted).typeName() == op::kSplit) {
                c.base = id;
                break;
            }
            stages.push_back({kind, id});
            for (const int look : {1, 0}) {
                for (const NodeId n : looks[look]) {
                    StageKind k;
                    std::string ignored;
                    if (roleOf(doc, n, &k, &ignored) == Role::PassOn) {
                        c.passOn.push_back(n);
                    } else {
                        pushStages(graph, n, k, &stages);
                    }
                }
            }
            id = parted;
            continue;
        } else {
            pushStages(graph, id, kind, &stages);
            if (gathers(kind)) {  // the first stage, its result the base; the nodes up its input a branch of their own
                c.base = id;
                c.baseOutput = port;
                break;
            }
        }
        if (port != 0) {  // (a Split's b, c or d handed on whole: the node below takes all it took)
            *why = "node '" + doc.nameOf(id) + "' hands on another output than its first";
            return false;
        }
        port = upstreamOutput(graph, id, handedSlot(graph, id));
        const NodeId next = handedFrom(graph, id);
        if (!next) {
            *why = std::string("node '") + doc.nameOf(id) + "' has no input";
            return false;
        }
        id = next;
    }
    if (!c.base) {
        *why = "no node feeds the GPU's";
        return false;
    }
    if (stages.empty()) {
        *why = std::string("the graph ends with ") + graph.node(c.base).typeName() + ", which the GPU does not run";
        if (graph.node(c.base).typeName() == op::kScript) *why += " (a script node runs on the CPU)";
        return false;
    }
    if (merged != kNoStage) {
        for (std::size_t k = merged + 1; k < stages.size(); ++k) stages[k].prefix = true;
    }
    c.stages.assign(stages.rbegin(), stages.rend());
    linkStages(graph, &c);
    *chain = c;
    return true;
}

LookChain chainFrom(const GraphDocument& doc, const LookChain& chain, std::size_t first) {
    // Every stage from `first` on takes the result of one from it on or what stage `first` takes
    // (the new base's): within a Blend's looks, back to where they part.
    auto runsFrom = [&](std::size_t f) {
        const std::size_t taken = chain.stages[f].from;
        for (std::size_t k = f; k < chain.stages.size(); ++k) {
            const LookStage& s = chain.stages[k];
            for (const std::size_t from : {s.from, s.kind == StageKind::Blend ? s.from2 : s.from}) {
                if (from != taken && (from == kNoStage || from < f)) return false;
            }
        }
        return true;
    };
    // (A node's stages run from its first: a Wet from its largest level.)
    while (first > 0 && first < chain.stages.size() && (chain.stages[first].level > 0 || !runsFrom(first))) --first;
    if (first == 0 || first >= chain.stages.size()) return chain;
    LookChain c = chain;
    c.stages.erase(c.stages.begin(), c.stages.begin() + static_cast<std::ptrdiff_t>(first));
    c.base = upstream(doc.graph, c.stages.front().node);
    c.baseOutput = upstreamOutput(doc.graph, c.stages.front().node);
    // Only what hands on what the remaining stages make (the rest is before the new base): the
    // nodes up from the output to the base, through each Blend's looks.
    std::set<NodeId> after;
    std::vector<NodeId> todo{c.output};
    while (!todo.empty()) {
        const NodeId walk = todo.back();
        todo.pop_back();
        if (!walk || walk == c.base || !after.insert(walk).second) continue;
        const std::size_t k = c.indexOf(walk);
        if (k != kNoStage && c.stages[k].kind == StageKind::Blend) todo.push_back(upstream(doc.graph, walk, 1));
        todo.push_back(k != kNoStage ? upstream(doc.graph, walk, 0) : handedFrom(doc.graph, walk));
    }
    std::vector<NodeId> kept;
    for (const NodeId id : chain.passOn) {
        if (after.count(id)) kept.push_back(id);
    }
    c.passOn = std::move(kept);
    linkStages(doc.graph, &c);
    return c;
}

namespace {

// A drag's value for node `id`'s param, if it has one.
const OverrideValue* overridden(const GraphDocument& doc, const Overrides& overrides, NodeId id, const std::string& name) {
    const auto node = overrides.find(doc.nameOf(id));
    if (node == overrides.end()) return nullptr;
    const auto v = node->second.find(name);
    return v == node->second.end() ? nullptr : &v->second;
}
const double* overriddenNumber(const GraphDocument& doc, const Overrides& overrides, NodeId id, const std::string& name) {
    const OverrideValue* v = overridden(doc, overrides, id, name);
    return v ? std::get_if<double>(v) : nullptr;
}


bool mappedParam(const ParamSet& params, const std::string& name) { return ownMap(params, name) || params.field(name); }


// A vector param that is one world vector for every strand (a VectorField without an expression),
// a drag's in place of the node's own; false for one per strand.
bool constantVector(const GraphDocument& doc, const Overrides& overrides, NodeId id, const ParamSet& params, const std::string& name,
                    Vec3f* out) {
    const auto node = overrides.find(doc.nameOf(id));
    if (node != overrides.end()) {
        const auto v = node->second.find(name);
        if (v != node->second.end() && std::holds_alternative<Vec3f>(v->second)) {
            *out = std::get<Vec3f>(v->second);
            return true;
        }
    }
    try {
        const VectorField field(params, name, nullptr);
        *out = field.constant();
        return field.isConstant();
    } catch (const std::exception&) {
        return false;
    }
}

// A node's params as the preview reads them (look_stage_params.h): a drag's values in place of its
// own, within the spec's hard limits (ParamSet clamps a number); a ramp as the operator reads it
// (getRamp: a plugged curve still wins, as on the CPU).
class PreviewParamReader : public StageParamReader {
public:
    PreviewParamReader(const GraphDocument& doc, const Overrides& overrides, NodeId id, const ParamSet& params)
        : StageParamReader(params, doc.nameOf(id)), doc_(doc), overrides_(overrides), id_(id) {}
    double number(const std::string& param) const override {
        const ParamSet& own = doc_.graph.node(id_).params();
        if (const double* v = overriddenNumber(doc_, overrides_, id_, param)) return own.clamped(param, *v);
        return own.getDouble(param);
    }
    Ramp ramp(const std::string& param) const override {
        const OverrideValue* v = overridden(doc_, overrides_, id_, param);
        const Ramp* dragged = v ? std::get_if<Ramp>(v) : nullptr;
        if (!dragged) return params().getRamp(param);
        ParamSet with = params();
        with.set(param, *dragged);
        return with.getRamp(param);
    }
    Vec3f vector(const std::string& param) const override {
        const OverrideValue* v = overridden(doc_, overrides_, id_, param);
        const Vec3f* dragged = v ? std::get_if<Vec3f>(v) : nullptr;
        return dragged ? *dragged : params().getVec3(param);
    }
    bool constantVector(const std::string& param, Vec3f* out) const override {
        return groomlab::gpu::constantVector(doc_, overrides_, id_, params(), param, out);
    }
    // A plugged field's own number, 1 for a map that is the param's own value (dragging that as a
    // number replaces the map: the CPU's).
    bool mappable(const std::string& param, float* value, std::string* why) const override {
        if (ownMap(doc_.graph.node(id_).params(), param)) {
            if (overridden(doc_, overrides_, id_, param)) {
                *why = name() + "." + param + " holds a map: a number dragged over it is the CPU's";
                return false;
            }
            *value = 1.0f;
            return true;
        }
        *value = static_cast<float>(number(param));
        return true;
    }

private:
    const GraphDocument& doc_;
    const Overrides& overrides_;
    NodeId id_;
};

// The nodes a drag sets values of.
std::set<NodeId> draggedNodes(const GraphDocument& doc, const Overrides& overrides) {
    std::set<NodeId> nodes;
    for (const auto& [name, values] : overrides) {
        const auto it = doc.nodesByName.find(name);
        if (it != doc.nodesByName.end()) nodes.insert(it->second);
    }
    return nodes;
}

}  // namespace

std::size_t firstOverridden(const GraphDocument& doc, const LookChain& chain, const Overrides& overrides) {
    const std::set<NodeId> dragged = draggedNodes(doc, overrides);
    for (std::size_t k = 0; k < chain.stages.size(); ++k) {
        const LookStage& s = chain.stages[k];
        if (dragged.count(s.node)) return k;
        // A Merge whose input b the drag moves: b's own run (mergedBranch) before it. A Cull, Split or
        // Mirror whose input the drag moves: the run up it (gatheredBranch).
        const NodeId b = s.kind == StageKind::Merge ? upstream(doc.graph, s.node, 1) : gathers(s.kind) ? upstream(doc.graph, s.node) : 0;
        if (b && madeFrom(doc.graph, b, dragged)) return k;
    }
    return kNoStage;
}

namespace {

// A Merge's input b where the drag moves it (a node of the drag makes b's strands): b's own run, from
// the first of its stages the drag sets, its params read with the drag's values (null: none, b's
// strands as cached). False with *why where the GPU does not run the dragged node up b.
bool mergedBranch(const GraphDocument& doc, NodeId merge, const Overrides& overrides, std::shared_ptr<const LookBranch>* out,
                  std::string* why) {
    const Graph& graph = doc.graph;
    out->reset();
    const NodeId b = upstream(graph, merge, 1);
    const std::set<NodeId> dragged = draggedNodes(doc, overrides);
    if (!b || !madeFrom(graph, b, dragged)) return true;
    const std::string name = doc.nameOf(merge) + "'s input b";
    LookChain full;
    std::string whyNot;
    if (!findLookChainTo(doc, b, &full, &whyNot)) {
        *why = name + ": " + whyNot;
        return false;
    }
    const std::size_t first = firstOverridden(doc, full, overrides);
    if (first == kNoStage) {
        *why = name + " is made from the dragged node through nodes the GPU does not run (" + doc.nameOf(full.base) + ")";
        return false;
    }
    auto branch = std::make_shared<LookBranch>();
    branch->chain = chainFrom(doc, full, first);
    if (madeFrom(graph, branch->chain.base, dragged)) {
        *why = name + ": the strands its run starts from (" + doc.nameOf(branch->chain.base) + ") are made from a dragged node (the CPU's)";
        return false;
    }
    if (!readLookParams(doc, branch->chain, overrides, &branch->params, why)) return false;
    *out = std::move(branch);
    return true;
}

// A Cull's, Split's or Mirror's input where the drag moves it: the run up it, from the first of its
// stages the drag sets (null: none, its result as cached). False with *why where the GPU does not
// run the dragged node up there.
bool gatheredBranch(const GraphDocument& doc, NodeId gather, const Overrides& overrides, std::shared_ptr<const LookBranch>* out,
                    std::string* why) {
    out->reset();
    const NodeId in = upstream(doc.graph, gather);
    if (!in || !madeFrom(doc.graph, in, draggedNodes(doc, overrides))) return true;
    LookChain full;
    if (!gatherInputChain(doc, gather, &full, why)) return false;
    const std::size_t first = firstOverridden(doc, full, overrides);
    if (first == kNoStage) {
        *why = doc.nameOf(gather) + "'s input is made from the dragged node through nodes the GPU does not run (" + doc.nameOf(full.base) + ")";
        return false;
    }
    return gatherBranch(doc, full, first, overrides, out, why);
}

}  // namespace

bool gatherInputChain(const GraphDocument& doc, NodeId gather, LookChain* chain, std::string* why) {
    const NodeId in = upstream(doc.graph, gather);
    std::string whyNot;
    if (!in || !findLookChainTo(doc, in, chain, &whyNot)) {
        *why = doc.nameOf(gather) + "'s input: " + (in ? whyNot : "none");
        return false;
    }
    return true;
}

bool gatherBranch(const GraphDocument& doc, const LookChain& inputChain, std::size_t first, const Overrides& overrides,
                  std::shared_ptr<const LookBranch>* out, std::string* why) {
    auto branch = std::make_shared<LookBranch>();
    branch->chain = chainFrom(doc, inputChain, first);
    if (madeFrom(doc.graph, branch->chain.base, draggedNodes(doc, overrides))) {
        *why = "the strands the run up " + doc.nameOf(inputChain.output) + " starts from (" + doc.nameOf(branch->chain.base) +
               ") are made from a dragged node (the CPU's)";
        return false;
    }
    if (!readLookParams(doc, branch->chain, overrides, &branch->params, why)) return false;
    *out = std::move(branch);
    return true;
}

bool readLookParams(const GraphDocument& doc, const LookChain& chain, const Overrides& overrides, LookParams* out,
                    std::string* why) {
    const Graph& graph = doc.graph;
    LookParams p;
    // A Merge's input a: the strands the stages up it run on (the base's first).
    std::size_t mergedA = 0;
    for (const LookStage& stage : chain.stages) {
        if (stage.kind != StageKind::Merge) continue;
        const StrandsPtr a = inputStrands(graph, stage.node, 0);
        if (!a) {
            *why = "not evaluated";
            return false;
        }
        mergedA = a->strandCount();
    }
    for (const LookStage& stage : chain.stages) {
        const NodeId id = stage.node;
        const ParamSet params = evaluatedParams(graph, id);
        StageParams s;
        s.stage = stage;
        if (stage.prefix) s.strands = mergedA;
        const PreviewParamReader reader(doc, overrides, id, params);
        auto number = [&](const std::string& name) { return reader.number(name); };
        auto ramp = [&](const std::string& name) { return reader.ramp(name); };
        auto mappable = [&](const std::string& name, float* value) { return reader.mappable(name, value, why); };
        s.seed = static_cast<std::uint64_t>(params.getInt("seed"));
        if (const double* v = overriddenNumber(doc, overrides, id, "seed")) {
            // An integer param: a dragged number truncated as a graph loads one (graph_io.cpp).
            const double d = std::trunc(graph.node(id).params().clamped("seed", *v));
            if (!(d >= -9.2233720368547758e18 && d < 9.2233720368547758e18)) {
                *why = doc.nameOf(id) + ".seed: out of range for an integer";
                return false;
            }
            const auto seed = static_cast<std::uint64_t>(static_cast<std::int64_t>(d));
            if (stage.kind == StageKind::Clump && seed != s.seed && graph.node(id).params().getString("centersFrom") == "pick") {
                *why = doc.nameOf(id) + " picks its own centers by its seed (the CPU's)";
                return false;
            }
            s.seed = seed;
        }
        switch (stage.kind) {
            // (look_stage_params.h: the operators' own reading.)
            case StageKind::Length:
            case StageKind::Gravity:
            case StageKind::Flyaway:
            case StageKind::Bend:
            case StageKind::Direction:
            case StageKind::Displace:
            case StageKind::Transform:
            case StageKind::Twist:
            case StageKind::Smooth:
            case StageKind::BlendNeighbors:
            case StageKind::Tangle:
            case StageKind::Flow:
                if (!readStageParams(stage.kind, reader, &s, why)) return false;
                break;
            case StageKind::Interpolate: {
                if (params.getString("take") != "shape") {
                    *why = doc.nameOf(id) + " takes the guides' length alone: the CPU's";
                    return false;
                }
                if (params.getBool("fillTubes")) {
                    *why = doc.nameOf(id) + " fills the guides' tubes (fillTubes): the CPU's";
                    return false;
                }
                const StrandsPtr in = inputStrands(graph, id);
                const std::size_t samples = in ? interpolateSamples(*in) : 0;
                if (samples > kMaxInterpolateSamples) {
                    *why = doc.nameOf(id) + ": strands of more than " + std::to_string(kMaxInterpolateSamples) + " points are the CPU's";
                    return false;
                }
                s.value[0] = static_cast<float>(number("limitDistance"));
                s.flag[0] = static_cast<int>(samples);
                s.flag[1] = params.getBool("preserveLength") ? 1 : 0;
                s.ramp = ramp("falloff");
                break;
            }
            case StageKind::Clump: {
                // Strands filling tube guides (an Interpolate's fillTubes) clumped inside their tubes
                // (a Clump's followTubes picking its own centers, a Wet's levels): the CPU's.
                if (stage.level >= 0 || (params.getBool("followTubes") && params.getString("centersFrom") == "pick")) {
                    const StrandsPtr in = inputStrands(graph, id);
                    const std::vector<float>* tubes = in ? in->strandChannel("tubeId") : nullptr;
                    if (tubes && in->strandChannel("tubeU") && in->strandChannel("tubeV") &&
                        std::any_of(tubes->begin(), tubes->end(), [](float t) { return t >= 0.0f; })) {
                        *why = doc.nameOf(id) + " clumps strands inside their tubes (followTubes): the CPU's";
                        return false;
                    }
                }
                // Each strand's own share of the pull (strandStrengthRandom): the CPU's for now.
                if (stage.level < 0 && params.getDouble("strandStrengthRandom") > 0.0) {
                    *why = doc.nameOf(id) + " varies each strand's pull (strandStrengthRandom): the CPU's";
                    return false;
                }
                // A Clump's params but its strength, volumize and profiles, `get` reading a number.
                auto shape = [&](const ParamSet& clump, const auto& get) {
                    s.value[2] = static_cast<float>(get("flatness"));
                    s.value[3] = static_cast<float>(get("lockRadius"));
                    const float repelRadius = static_cast<float>(get("repelRadius"));
                    std::memcpy(&s.flag[0], &repelRadius, sizeof(float));  // the kernel reads its bits as a float
                    s.flag[1] = clump.getString("pastTip") == "extend" ? 1 : 0;
                    const std::string flatten = clump.getString("flatten");
                    s.flag[2] = flatten == "random" ? 1 : flatten == "direction" ? 2 : 0;
                    // The per-clump variation (by each center's ID: ClumpInputs::variation) and copy.
                    s.value2[0] = static_cast<float>(get("strengthRandom"));
                    s.value2[1] = static_cast<float>(get("lockRadiusRandom"));
                    s.value2[2] = static_cast<float>(get("copyAmount"));
                    const Vec3f direction = clump.getVec3("flattenDirection");
                    std::memcpy(&s.flag2[0], &direction.x, sizeof(float));
                    std::memcpy(&s.flag2[1], &direction.y, sizeof(float));
                    std::memcpy(&s.flag2[2], &direction.z, sizeof(float));
                    s.tabled = true;
                    s.hasRamp2 = true;
                };
                if (stage.level >= 0) {
                    // A Wet's level: its Clump's params as Wet set them for its last result
                    // (WetState), its profile at the wetness (a drag's: the levels' centers stay).
                    std::shared_ptr<const WetState> wet;
                    const std::int64_t levels = wetLevelCount(graph.node(id).params());
                    if (!wetState(graph.node(id), cachedStrands(graph, id), &wet) || wet->params.size() != static_cast<std::size_t>(levels) ||
                        static_cast<std::size_t>(stage.level) >= wet->params.size()) {
                        *why = doc.nameOf(id) + " keeps no levels for its result";
                        return false;
                    }
                    const ParamSet& level = wet->params[static_cast<std::size_t>(stage.level)];
                    auto get = [&](const char* name) { return level.getDouble(name); };
                    if (ownMap(level, "strength") || ownMap(level, "volumize")) {
                        *why = doc.nameOf(id) + "'s levels map their pull (the CPU's)";
                        return false;
                    }
                    s.value[0] = static_cast<float>(get("strength"));
                    s.value[1] = static_cast<float>(get("volumize"));
                    shape(level, get);
                    s.seed = static_cast<std::uint64_t>(level.getInt("seed"));
                    s.ramp = wetLevelProfile(stage.level, levels, static_cast<float>(number("wetness")));
                    s.ramp2 = level.getRamp("volumizeProfile");
                    break;
                }
                if (!mappable("strength", &s.value[0]) || !mappable("volumize", &s.value[1])) return false;
                shape(params, number);
                s.ramp = ramp("profile");
                s.ramp2 = ramp("volumizeProfile");
                break;
            }
            case StageKind::Noise: {
                if (!mappable("amplitude", &s.value[0]) || !mappable("fraction", &s.value[2])) return false;
                s.value[1] = static_cast<float>(number("frequency"));
                s.flag[0] = static_cast<int>(std::floor(number("octaves")));
                s.flag[1] = params.getString("space") == "world" ? 1 : 0;
                s.flag[2] = params.getString("mode") == "kinky" ? 1 : 0;
                s.ramp = ramp("profile");  // a plugged curve too
                s.tabled = true;
                break;
            }
            case StageKind::Curl: {
                const std::string shape = params.getString("shape");
                s.flag[2] = params.getString("coherence") == "clump" ? 1 : 0;  // each clump as one (CurlInputs)
                if (shape == "coil") {
                    // A coil's radius: coilWidths strand widths at each point (its values: the widths).
                    s.value[0] = static_cast<float>(number("coilWidths"));
                } else if (!mappable("radius", &s.value[0])) {
                    return false;
                }
                s.value[1] = static_cast<float>(number("frequency"));
                s.value[2] = static_cast<float>(number("randomness"));
                s.value[3] = static_cast<float>(number("phase"));
                s.flag[0] = shape == "wave" ? 1 : shape == "zigzag" ? 2 : shape == "coil" ? 3 : 0;
                const float phaseRandom = static_cast<float>(number("phaseRandom"));
                std::memcpy(&s.flag[1], &phaseRandom, sizeof(float));  // the kernel reads its bits as a float
                s.ramp = ramp("profile");
                s.tabled = true;
                // The spacing: spacingRandom, and a frequencyProfile other than 1 everywhere (the
                // angle by the profiled arc: CurlOp's `profiled`).
                s.value2[0] = std::clamp(static_cast<float>(number("spacingRandom")), 0.0f, 1.0f);
                s.ramp2 = ramp("frequencyProfile");
                s.flag2[0] = std::any_of(s.ramp2.keys.begin(), s.ramp2.keys.end(), [](const auto& k) { return k.second != 1.0f; }) ? 1 : 0;
                s.hasRamp2 = s.flag2[0] != 0;
                // Weighted curls (CurlOp's weight: the turns spread where hair hangs below them):
                // the weight over kWeightTurns and the unit weightDirection, as CurlOp::run takes
                // them (none without a direction).
                const OverrideValue* draggedDirection = overridden(doc, overrides, id, "weightDirection");
                const Vec3f* dragged = draggedDirection ? std::get_if<Vec3f>(draggedDirection) : nullptr;
                const Vec3f weightDirection = dragged ? *dragged : params.getVec3("weightDirection");
                if (weightDirection.length() > 0.0f) {
                    const Vec3f down = weightDirection.normalized();
                    s.value2[1] = std::max(0.0f, static_cast<float>(number("weight"))) / 10.0f;  // (curl_op.cpp kWeightTurns)
                    std::memcpy(&s.flag2[1], &down.x, sizeof(float));  // the kernel reads their bits as floats
                    std::memcpy(&s.flag2[2], &down.y, sizeof(float));
                    std::memcpy(&s.flag2[3], &down.z, sizeof(float));
                }
                break;
            }
            case StageKind::Width: {
                // Feathers' shafts with a rachis keep their widths (WidthOp): the CPU's.
                if (const StrandsPtr fed = inputStrands(graph, id); fed && fed->strandChannel(kRachisChannel)) {
                    *why = doc.nameOf(id) + " leaves the feathers' shafts as wide as their rachis (the CPU's)";
                    return false;
                }
                if (!readStageParams(stage.kind, reader, &s, why)) return false;
                break;
            }
            case StageKind::Part: {
                // Auto parts are found in the strands it takes, which a live drag or stroke changes:
                // the run opens the lines its last evaluation found (LookInputs::read), so it must
                // have kept them for its result.
                PartsPtr found;
                if (params.getString("partsFrom") == "auto" && !autoPartState(graph.node(id), cachedStrands(graph, id), &found)) {
                    *why = doc.nameOf(id) + " keeps no parts found in the hair's flow for its result";
                    return false;
                }
                if (!readStageParams(stage.kind, reader, &s, why)) return false;
                break;
            }
            case StageKind::Collide: {
                if (params.getString("method") != "field") {
                    *why = doc.nameOf(id) + " asks the mesh at every point (method exact: the CPU's)";
                    return false;
                }
                const auto offset = static_cast<float>(number("offset"));
                const auto maxDepth = static_cast<float>(params.getDouble("maxDepth"));
                const auto softness = static_cast<float>(params.getDouble("softness"));
                s.value[0] = offset;
                s.value[1] = maxDepth;
                s.value[2] = std::max(offset + softness, maxDepth) * 1.001f + 1e-6f;  // CollideOp's search radius
                s.flag[0] = static_cast<int>(params.getInt("iterations"));
                s.value2[0] = softness;
                s.value2[1] = static_cast<float>(params.getDouble("smoothing"));
                break;
            }
            case StageKind::Wind: {
                // At the time its cached result was made at, as WindOp works it out: how far the
                // gusts travelled along the direction and the flutter's phase (in double, as the
                // CPU's, then rounded to floats as it rounds them).
                double time = 0.0, fps = 24.0;
                if (!windTime(graph.node(id), cachedStrands(graph, id), &time, &fps)) {
                    *why = doc.nameOf(id) + " keeps no time for its result";
                    return false;
                }
                if (!mappable("strength", &s.value[0])) return false;
                // A plain 0 of its own blows nowhere and stays cached whatever the time (WindOp's
                // contextSalt): the time kept may be an earlier one than a drag of it would blow at.
                const ParamSet& own = graph.node(id).params();
                const double* still = own.has("strength") && !params.field("strength") ? std::get_if<double>(&own.get("strength")) : nullptr;
                if (still && *still == 0.0 && overriddenNumber(doc, overrides, id, "strength")) {
                    *why = doc.nameOf(id) + " blows nowhere at its own strength, so its time is not kept (the CPU's)";
                    return false;
                }
                const auto frequency = static_cast<float>(number("frequency"));
                const double speed = number("speed");
                s.value[1] = static_cast<float>(number("gust"));
                s.value[2] = frequency;
                s.value[3] = static_cast<float>(number("turbulence"));
                const Vec3f direction = params.getVec3("direction");
                const Vec3f dir = direction.length() < 1e-6f ? Vec3f{} : direction.normalized();
                const WindPhase phase = windPhase(dir, frequency, speed, time, fps, params.getBool("loop"),
                                                  static_cast<double>(params.getInt("loopStart")),
                                                  static_cast<double>(params.getInt("loopEnd")));
                auto bits = [](int* to, float v) { std::memcpy(to, &v, sizeof(float)); };  // the kernel reads them as floats
                if (!(direction.length() < 1e-6f)) {
                    s.value2[0] = dir.x;
                    s.value2[1] = dir.y;
                    s.value2[2] = dir.z;
                    if (phase.looped) {
                        bits(&s.flag[0], phase.angle);
                        bits(&s.flag[1], phase.turnsPer);
                        s.flag[3] = 1;
                        bits(&s.flag2[0], phase.across1.x);
                        bits(&s.flag2[1], phase.across1.y);
                        bits(&s.flag2[2], phase.across1.z);
                        bits(&s.flag2[3], phase.radius);
                    } else {
                        bits(&s.flag[0], phase.shift.x);
                        bits(&s.flag[1], phase.shift.y);
                        bits(&s.flag[2], phase.shift.z);
                    }
                }
                s.value2[3] = phase.flutterPhase;
                s.ramp = ramp("profile");
                s.tabled = true;
                break;
            }
            case StageKind::Blend: {
                if (!mappable("mix", &s.value[0])) return false;
                s.ramp = ramp("profile");
                s.tabled = true;
                break;
            }
            case StageKind::SelfCollide: {
                if (!mappable("strength", &s.value[0])) return false;
                const double packing = std::max(number("packing"), 1e-3);  // (as SelfCollideOp reads it)
                s.value[1] = static_cast<float>(packing);
                // Overpacked past it: the hair's fixed point (2^32 a unit) above ceil(packing 2^32).
                const auto limit = static_cast<std::uint64_t>(std::ceil(packing * 4294967296.0));
                s.flag2[0] = static_cast<int>(static_cast<std::uint32_t>(limit));
                s.flag2[1] = static_cast<int>(static_cast<std::uint32_t>(limit >> 32));
                s.ramp = ramp("profile");
                s.tabled = true;
                break;
            }
            case StageKind::Static: {
                if (!mappable("strength", &s.value[0])) return false;
                s.value[1] = static_cast<float>(std::clamp(number("randomness"), 0.0, 1.0));
                s.ramp = ramp("profile");
                s.tabled = true;
                break;
            }
            case StageKind::Tie: {
                // TieOp::evaluate's numbers, as it clamps them; its vectors dragged or its own.
                auto vector = [&](const char* name) {
                    const OverrideValue* v = overridden(doc, overrides, id, name);
                    const Vec3f* dragged = v ? std::get_if<Vec3f>(v) : nullptr;
                    return dragged ? *dragged : params.getVec3(name);
                };
                auto bits = [](int* to, float v) { std::memcpy(to, &v, sizeof(float)); };  // the kernels read them as floats
                s.value[0] = std::max(0.0f, static_cast<float>(number("radius")));
                s.value[1] = std::clamp(static_cast<float>(number("tightness")), 0.0f, 1.0f);
                s.value[2] = std::clamp(static_cast<float>(number("spread")), 0.0f, 80.0f) * 3.14159265358979f / 180.0f;
                s.value[3] = static_cast<float>(number("twist"));
                s.value2[0] = std::max(0.0f, static_cast<float>(number("ease")));
                const Vec3f direction = vector("direction"), center = vector("center");
                s.value2[1] = direction.x;
                s.value2[2] = direction.y;
                s.value2[3] = direction.z;
                bits(&s.flag2[0], center.x);
                bits(&s.flag2[1], center.y);
                bits(&s.flag2[2], center.z);
                s.flag[0] = params.getString("tailChannel").empty() ? 0 : 1;
                // Its affect a mask (a map, a field plugged in, a number under 1): which strands it
                // ties (field 0, per strand) and how much of its result each keeps.
                const bool masked = maskedNode(graph, id) || params.field(kAffectParam) != nullptr ||
                                    overridden(doc, overrides, id, kAffectParam) != nullptr;
                float affect = 1.0f;
                if (masked && !mappable(kAffectParam, &affect)) return false;
                bits(&s.flag[1], affect);
                s.flag[2] = masked ? 1 : 0;
                break;
            }
            // ---- The shapers reading their neighbours or a reference ----
            case StageKind::RestoreLength: {
                const std::string mode = params.getString("mode");
                s.flag[0] = mode == "scale" ? 1 : mode == "even" ? 2 : 0;
                s.flag[1] = stage.from2 != kNoStage ? 1 : 0;  // the reference a stage of the run makes
                break;
            }
            // ---- Jiggle and MeshCut ----
            case StageKind::Jiggle: {
                // Its springs over the scalp and history its last evaluation read (operator_state.h
                // jiggleState); a history dragged past the frames held reads frames the host would
                // sample anew on the release: the CPU's.
                JiggleState state;
                if (!jiggleState(graph.node(id), cachedStrands(graph, id), &state)) {
                    *why = doc.nameOf(id) + " keeps no scalp motion for its result";
                    return false;
                }
                if (!mappable("strength", &s.value[0])) return false;
                // (In double too, as JiggleOp weighs the frames by them: more[0-1], [2-3].)
                const double frequency = number("frequency"), damping = number("damping");
                s.value[1] = static_cast<float>(frequency);
                s.value[2] = static_cast<float>(damping);
                std::memcpy(&s.more[0], &frequency, sizeof frequency);
                std::memcpy(&s.more[2], &damping, sizeof damping);
                const std::int64_t own = graph.node(id).params().getInt("history");
                std::int64_t history = own;
                if (const double* v = overriddenNumber(doc, overrides, id, "history")) {
                    history = static_cast<std::int64_t>(std::trunc(graph.node(id).params().clamped("history", *v)));
                    ParamSet with = params, held = params;
                    with.set("history", history);
                    const std::size_t read = jiggleFramesRead(with, state);
                    if (read != static_cast<std::size_t>(history) && jiggleFramesRead(held, state) == static_cast<std::size_t>(own)) {
                        *why = doc.nameOf(id) + ".history reads frames of the scalp not sampled for it (the CPU's)";
                        return false;
                    }
                }
                s.flag[0] = static_cast<int>(history);
                s.ramp = ramp("profile");
                s.tabled = true;
                break;
            }
            case StageKind::MeshCut: {
                s.value[0] = static_cast<float>(number("offset"));
                s.flag[0] = params.getString("keep") == "inside" ? 1 : 0;
                break;
            }
            case StageKind::Merge: {
                s.flag[0] = static_cast<int>(mergedA);
                if (!mergedBranch(doc, id, overrides, &s.branch, why)) return false;
                break;
            }
            case StageKind::Cull:
            case StageKind::Split:
            case StageKind::Mirror: {
                if (&stage != &chain.stages.front()) {
                    *why = doc.nameOf(id) + " is not the run's first node";
                    return false;
                }
                if (stage.kind == StageKind::Mirror) {
                    const std::string axis = params.getString("axis");
                    s.flag[0] = axis == "y" ? 1 : axis == "z" ? 2 : 0;
                    s.value[0] = static_cast<float>(params.getDouble("offset"));
                }
                if (!gatheredBranch(doc, id, overrides, &s.branch, why)) return false;
                break;
            }
        }
        // ---- Affect masks ----
        // Restricted by its affect (or one dragged): its result mixed back by it after its last stage
        // (but Tie's: mixesByItsAffect).
        if (!mixesByItsAffect(stage.kind) &&
            (maskedNode(graph, id) || params.field(kAffectParam) != nullptr || overridden(doc, overrides, id, kAffectParam))) {
            if (!mappable(kAffectParam, &s.affect)) return false;
            s.masked = true;
            const StrandsPtr in = inputStrands(graph, id);
            s.mixWidths = stage.kind == StageKind::Width && in && in->widths().size() == in->pointCount();
        }
        p.stages.push_back(std::move(s));
    }
    *out = std::move(p);
    return true;
}

namespace {

// The base of a run through a Merge (its stage `merge`): the Merge's cached result, its input a's
// strands (the first) with the points `a` holds (what the run's first stage up there takes), b's as
// the Merge made them; null with *why when `a` is not laid out as the Merge's first strands. Kept
// for the next run of the same strands (a drag's every step plans again).
StrandsPtr mergedBase(const Graph& graph, NodeId merge, const StrandsPtr& a, std::string* why) {
    const StrandsPtr merged = cachedStrands(graph, merge);
    const StrandsPtr input = inputStrands(graph, merge, 0);
    if (!merged || !input) {
        *why = "not evaluated";
        return nullptr;
    }
    static std::mutex keptMutex;
    static std::weak_ptr<const StrandSet> keptA, keptMerged;
    static StrandsPtr kept;
    {
        const std::lock_guard<std::mutex> lock(keptMutex);
        if (kept && keptA.lock() == a && keptMerged.lock() == merged) return kept;
    }
    const std::size_t n = a->strandCount();
    bool laidOut = n == input->strandCount() && n <= merged->strandCount() && a->pointCount() <= merged->pointCount() &&
                   std::equal(a->offsets().begin(), a->offsets().end(), merged->offsets().begin()) &&
                   std::equal(a->ids().begin(), a->ids().end(), merged->ids().begin());
    if (!laidOut) {
        *why = "the strands the GPU starts from up a Merge's input a are not laid out as its first";
        return nullptr;
    }
    auto out = std::make_shared<StrandSet>(*merged);
    Vec3f* to = out->overwritePositions().data();
    const Vec3f* fromA = a->positions().data();
    const Vec3f* fromMerged = merged->positions().data();
    const std::size_t pointsA = a->pointCount(), points = merged->pointCount();
    parallelFor(0, (points + 65535) / 65536, [&](std::size_t c) {
        const std::size_t begin = c * 65536, end = std::min(points, begin + 65536);
        for (std::size_t i = begin; i < end; ++i) to[i] = i < pointsA ? fromA[i] : fromMerged[i];
    }, 1);
    const std::lock_guard<std::mutex> lock(keptMutex);
    keptA = a;
    keptMerged = merged;
    kept = out;
    return out;
}

}  // namespace

bool lookStrands(const GraphDocument& doc, const LookChain& chain, StrandsPtr* base, StrandsPtr* output, std::string* why) {
    const Graph& graph = doc.graph;
    *base = baseStrands(graph, chain);
    const StrandsPtr made = cachedStrands(graph, chain.output);
    if (!*base || !made) {
        *why = "not evaluated";
        return false;
    }
    for (const LookStage& s : chain.stages) {
        if (s.kind != StageKind::Merge) continue;
        // (As made: hidden core strands in it, preview_core.h, as the Merge's roles channel says.)
        *base = mergedBase(graph, s.node, *base, why);
        if (!*base) return false;
    }
    // The output as the viewport was handed it: without the roles' channel and, in a preview under
    // its core share, without the core strands it holds hidden (preview_core.h), which the engine
    // runs and leaves out of its results.
    *output = valueAs<StrandsPtr>(graph.shownCachedValue(chain.output));
    if (!*output) *output = made;
    for (const NodeId id : chain.passOn) {
        const NodeId in = handedFrom(graph, id);
        const StrandsPtr out = cachedStrands(graph, id), taken = inputStrands(graph, id, handedSlot(graph, id));
        const bool same = out && taken &&
                          (out == taken || (channelsOnly(graph.node(id).typeName()) &&
                                            out->positions().data() == taken->positions().data() && out->offsets() == taken->offsets()));
        if (!in || !same) {
            *why = "node '" + doc.nameOf(id) + "' changes the strands";
            return false;
        }
    }
    if ((*base)->strandCount() != made->strandCount() || (*base)->pointCount() != made->pointCount() ||
        (*base)->offsets() != made->offsets()) {
        *why = "the strands' layout changes after the base";
        return false;
    }
    return true;
}

namespace {

// The strands each stage takes (its first input's cached result) and the stages' own results.
std::vector<StrandsPtr> fieldSources(const GraphDocument& doc, const LookChain& chain) {
    const Graph& graph = doc.graph;
    std::vector<StrandsPtr> sources{baseStrands(graph, chain)};
    for (std::size_t k = 0; k < chain.stages.size(); ++k) {
        const LookStage& s = chain.stages[k];
        sources.push_back(inputStrands(graph, s.node));
        sources.push_back(stageResult(graph, chain, k));
    }
    return sources;
}

// One mapped param's values on `in`: the core's (look_cpu.h), as the operators read them.
void readValues(const ScalarField& field, const StrandSet& in, bool pointsAllowed, FieldValues* out) {
    readFieldValues(field, in, pointsAllowed, out);
}


// What a mapped param reads, against what a run of `chain` from its first stage changes before
// stage k: channels the stages before it write (their cached result's channel is not the very
// array their cached input holds) are Interpolate's (from its binding: its guides' influence, the
// guide attributes it blends, its layers along the strands) or another's (Clump's clump numbers:
// its choice, fixed through a run).
struct FieldRead {
    const ScalarField* field = nullptr;  // the one holding the map (a plugged Map or Expression node's)
    const MapRef* map = nullptr;         // null: a number
    bool unknown = false;                // a plugged field that says nothing of what it reads
    bool positions = false;              // the points (ScalarField::readsPositions)
    bool influence = false;              // the guides' influence the run's Interpolate makes
    std::string influenceName;           // its channel
    bool binding = false;                // a strand channel Interpolate blends from its binding
    bool layer = false;                  // a point channel Interpolate writes along the strands
    bool wet = false;                    // the wetness a Wet writes (a drag of it changes it)
    bool tie = false;                    // the tail a Tie writes (on the GPU for a Tie after it, an affect mapped to it)
    std::size_t tieStage = kNoStage;     // the last Tie writing it
    bool otherUnknown = false;           // unknown for another reason than a Tie's tail
};
FieldRead whatItReads(const Graph& graph, const LookChain& chain, std::size_t k, const ScalarField& top) {
    FieldRead r;
    r.field = &top;
    if (top.plugged()) {
        r.field = top.plugged()->scalarField();
        if (!r.field) {
            r.unknown = r.otherUnknown = true;
            r.positions = top.readsPositions();
            return r;
        }
    }
    r.map = r.field->map();
    if (!r.map) return r;
    r.positions = r.field->readsPositions();
    auto channel = [&](const std::string& name) {
        for (std::size_t j = 0; j < k; ++j) {
            const NodeId id = chain.stages[j].node;
            const StrandsPtr in = inputStrands(graph, id), out = cachedStrands(graph, id);
            if (!in || !out) {
                r.unknown = r.otherUnknown = true;
                return;
            }
            const bool point = out->pointChannel(name) != in->pointChannel(name);
            const bool strand = out->strandChannel(name) != in->strandChannel(name);
            if ((point || strand) && (chain.stages[j].kind == StageKind::Flyaway || chain.stages[j].kind == StageKind::Blend)) {
                // Flyaway's strays' flags (the GPU draws the strays, not the flags), Blend's mix
                // and its look's mixed channels: not made on the GPU.
                r.unknown = r.otherUnknown = true;
                continue;
            }
            if (point && chain.stages[j].kind == StageKind::Tie) {
                // A Tie's tail (the GPU's run writes it for the Ties after it, and an affect mapped to it).
                r.unknown = true;
                r.tie = true;
                r.tieStage = j;
                continue;
            }
            if (point && chain.stages[j].level >= 0) {
                // A Wet's point channel, its wetness (its clump numbers: its levels' choice, fixed).
                r.unknown = r.otherUnknown = true;
                r.wet = true;
                continue;
            }
            if ((!point && !strand) || chain.stages[j].kind != StageKind::Interpolate) continue;
            if (point) {
                r.layer = true;
            } else if (graph.node(id).params().getString("influenceChannel") == name) {
                r.influence = true;
                r.influenceName = name;
            } else {
                r.binding = true;
            }
        }
    };
    switch (r.map->source) {
        case MapRef::Source::Texture:
        case MapRef::Source::ColorSet: break;
        case MapRef::Source::Attribute:
        case MapRef::Source::Guide: channel(r.map->name); break;
        case MapRef::Source::Expression:
            if (const Expression* e = r.field->expression()) {
                for (const std::string& name : e->channels()) channel(name);
            } else {
                r.unknown = r.otherUnknown = true;
            }
            break;
    }
    return r;
}

// The GPU's ops (look.slang FieldCS): kind, code, a value's bits, a slot.
enum FieldOp : std::uint32_t { kOpNumber, kOpShape, kOpStatic, kOpInfluence, kOpNegate, kOpBinary, kOpSelect, kOpCall };
// The functions FieldCS runs (look.slang fieldCall) are those whose row in expression.cpp's kFns
// names a GPU code (Expression::gpuFunction); any other runs on the CPU. smoothstep and linearstep
// reach the GPU edges first, whichever order they were written in (expression.cpp orders them).
const char* const kGpuOperators[] = {"+", "-", "*", "/", "^", "<", "<=", ">", ">=", "==", "!="};
// The strand's shape variables FieldCS makes from the run's points, by their name.
const char* const kGpuShape[] = {"length", "longest", "reach", "straightness", "lift", "tipx", "tipy", "tipz"};
constexpr std::size_t kMaxFieldStack = 16, kMaxFieldOps = 256;

// A field's program for FieldCS (FieldProgram), per strand on `in` (validated: the expression bound
// there), or null with *why for what the kernels do not run.
std::shared_ptr<const FieldProgram> fieldProgram(const FieldRead& r, const StrandSet& in, const ScalpMesh* scalp,
                                                 std::string* why) {
    auto program = std::make_shared<FieldProgram>();
    program->low = r.map->min;
    program->high = r.map->max;
    auto push = [&](std::uint32_t kind, std::uint32_t code, float value, std::uint32_t slot) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof bits);
        program->ops.insert(program->ops.end(), {kind, code, bits, slot});
    };
    const MapRef::Source source = r.map->source;
    if (source == MapRef::Source::Attribute || source == MapRef::Source::Guide) {
        // The guides' influence (a strand channel Interpolate writes), as the kernels make it.
        push(kOpInfluence, 0, 0.0f, 0);
        program->depth = 1;
        program->influence = true;
        return program;
    }
    const Expression* e = r.field->expression();
    if (source != MapRef::Source::Expression || !e || e->isVector()) {
        *why = "a map the GPU does not evaluate";
        return nullptr;
    }
    if (e->stackDepth() > kMaxFieldStack || e->program().size() > kMaxFieldOps) {
        *why = "an expression too long for the GPU";
        return nullptr;
    }
    // Function, operator and shape codes by name (the expression's own codes may move).
    std::map<int, std::uint32_t> operators, shapes;
    for (std::uint32_t b = 0; b < sizeof kGpuOperators / sizeof kGpuOperators[0]; ++b) operators[Expression::binaryCode(kGpuOperators[b])] = b;
    for (std::uint32_t v = 0; v < sizeof kGpuShape / sizeof kGpuShape[0]; ++v) shapes[Expression::variableCode(kGpuShape[v])] = v;
    const int lift = Expression::variableCode("lift"), longest = Expression::variableCode("longest");
    const int nx = Expression::variableCode("nx"), ny = Expression::variableCode("ny"), nz = Expression::variableCode("nz");
    // The static slots: the root's normal first where the shape is read (lift), then each variable
    // and channel as first read.
    std::vector<int> slotVariables;               // per slot: a variable's code, or -1 - channel
    std::map<int, std::uint32_t> slotOf;
    auto slot = [&](int key) {
        const auto it = slotOf.find(key);
        if (it != slotOf.end()) return it->second;
        const auto s = static_cast<std::uint32_t>(slotVariables.size());
        slotOf[key] = s;
        slotVariables.push_back(key);
        return s;
    };
    for (const Expression::Op& op : e->program()) {
        if (op.kind == Expression::Op::Variable && op.code == lift) {
            slot(nx);
            slot(ny);
            slot(nz);
            break;
        }
    }
    const std::vector<std::string>& channels = e->channels();
    for (const Expression::Op& op : e->program()) {
        switch (op.kind) {
            case Expression::Op::Number: push(kOpNumber, 0, op.value, 0); break;
            case Expression::Op::Variable: {
                const auto shape = shapes.find(op.code);
                if (shape != shapes.end()) {
                    push(kOpShape, shape->second, 0.0f, 0);
                    program->shape = true;
                    program->longest = program->longest || op.code == longest;
                } else {
                    push(kOpStatic, 0, 0.0f, slot(op.code));  // the root's, rand, index, the time: fixed
                }
                break;
            }
            case Expression::Op::Channel: {
                const std::string& name = op.channel < channels.size() ? channels[op.channel] : std::string();
                if (r.influence && name == r.influenceName) {
                    push(kOpInfluence, 0, 0.0f, 0);
                    program->influence = true;
                } else {
                    push(kOpStatic, 0, 0.0f, slot(-1 - static_cast<int>(op.channel)));
                }
                break;
            }
            case Expression::Op::Unary: push(kOpNegate, 0, 0.0f, 0); break;
            case Expression::Op::Binary: {
                const auto it = operators.find(op.code);
                if (it == operators.end()) {
                    *why = "an operator the GPU does not run";
                    return nullptr;
                }
                push(kOpBinary, it->second, 0.0f, 0);
                break;
            }
            case Expression::Op::Select: push(kOpSelect, 0, 0.0f, 0); break;
            case Expression::Op::Call: {
                const int gpu = Expression::gpuFunction(op.code);
                if (gpu < 0 || op.results != 1) {
                    *why = "a function the GPU does not run";
                    return nullptr;
                }
                push(kOpCall, static_cast<std::uint32_t>(gpu), 0.0f, op.args);
                break;
            }
            default:
                *why = "vectors (the GPU runs numbers alone)";
                return nullptr;
        }
    }
    program->depth = static_cast<std::uint32_t>(e->stackDepth());
    program->slotCount = static_cast<std::uint32_t>(slotVariables.size());
    if (program->slotCount) {
        program->statics.assign(in.strandCount() * program->slotCount, 0.0f);
        const Expression::Bound& bound = r.field->boundExpression();
        parallelFor(0, in.strandCount(), [&](std::size_t s) {
            float vars[Expression::kVariables];
            e->strandVariablesInto(in, s, scalp, bound, vars);
            float* to = program->statics.data() + s * program->slotCount;
            for (std::uint32_t k = 0; k < program->slotCount; ++k) {
                const int key = slotVariables[k];
                if (key >= 0) {
                    to[k] = vars[key];
                    continue;
                }
                const auto c = static_cast<std::size_t>(-1 - key);
                const std::vector<float>* strand = c < bound.channels.size() ? bound.channels[c] : nullptr;
                const std::vector<float>* point = c < bound.pointChannels.size() ? bound.pointChannels[c] : nullptr;
                if (strand) {
                    to[k] = s < strand->size() ? (*strand)[s] : 0.0f;
                } else {
                    const std::size_t at = in.strandBegin(s);
                    to[k] = point && at < point->size() ? (*point)[at] : 0.0f;
                }
            }
        }, 1024);
    }
    if (program->longest) {
        const PreviewCore core(in);
        for (std::size_t s = 0; s < in.strandCount(); ++s) {
            if (core.core(s)) program->core.push_back(static_cast<std::uint32_t>(s));
        }
    }
    return program;
}

}  // namespace

bool readLookFields(const GraphDocument& doc, const LookChain& chain, const ScalpMesh* scalp, LookFields* out, std::string* why) {
    static std::atomic<std::uint64_t> serials{0};
    const Graph& graph = doc.graph;
    LookFields f;
    f.sources = fieldSources(doc, chain);
    f.stages.resize(chain.stages.size());
    for (std::size_t k = 0; k < chain.stages.size(); ++k) {
        const LookStage& stage = chain.stages[k];
        const NodeId id = stage.node;
        const StrandsPtr in = inputStrands(graph, id);
        // (`tieChannel`: a Tie's tail it maps read on the GPU where the Tie writes it: an affect's.)
        auto read = [&](const char* name, bool pointsAllowed, FieldValues* values, bool tieChannel = false) {
            const ParamSet params = evaluatedParams(graph, id);
            const bool mapped = ownMap(params, name) || params.field(name);
            if (!mapped) return true;
            if (!in) {
                *why = "not evaluated";
                return false;
            }
            // The field alone: a plugged one read with the param's number 1 (the CPU multiplies by the
            // number, the kernels by the dragged one); a map of the param's own as it is.
            ParamSet unit = params;
            if (!ownMap(params, name)) unit.set(name, 1.0);
            if (ownMap(params, name)) {
                const MapRef& map = std::get<MapRef>(params.get(name));
                // The time and the groom's maps are the graph's evaluation's (expressionEnvironment).
                if (map.source == MapRef::Source::Expression) {
                    const ExpressionDependencies deps = scanExpression(map.name);
                    if (deps.time || !deps.maps.empty()) {
                        *why = doc.nameOf(id) + "." + name + " reads the time or a map (the CPU's)";
                        return false;
                    }
                }
            }
            try {
                const ScalarField field(unit, name, scalp);
                readValues(field, *in, pointsAllowed, values);
                // What it reads against what the run moves before this node: fixed values hold
                // whatever moves; values of the points or of the guides' influence (which the run's
                // Interpolate makes) the GPU makes again from the run's points, per strand, where
                // its kernels run the map (FieldProgram).
                const FieldRead r = whatItReads(graph, chain, k, field);
                if (r.wet) {
                    *why = doc.nameOf(id) + "." + name + " reads the wetness a Wet before it writes (the CPU's)";
                    return false;
                }
                if (r.tie) {
                    // An affect mapped to it alone (an attribute map, per point): read where the
                    // Tie writes it on the GPU, remapped as the map remaps it.
                    if (tieChannel && r.map && r.map->source == MapRef::Source::Attribute && !r.otherUnknown && !r.layer &&
                        !r.binding && !r.influence && r.tieStage != kNoStage && values->mode == FieldValues::kPerPoint) {
                        values->channelFrom = r.tieStage;
                        values->low = r.map->min;
                        values->high = r.map->max;
                        return true;
                    }
                    *why = doc.nameOf(id) + "." + name + " reads the tail a Tie before it writes (the CPU's)";
                    return false;
                }
                const bool known = !r.unknown && !r.layer;
                // A Control plugged in: a number the host sets, the same everywhere.
                values->fixed = (known && !r.positions && !r.influence && !r.binding) || pluggedControl(graph, id, name);
                values->binding = known && r.binding;
                if (k > 0 && known && (r.positions || r.influence) && values->mode == FieldValues::kPerStrand) {
                    std::string whyNot;
                    values->program = fieldProgram(r, *in, scalp, &whyNot);
                }
            } catch (const std::exception& e) {
                *why = doc.nameOf(id) + "." + name + ": " + e.what();
                return false;
            }
            return true;
        };
        FieldValues* v = f.stages[k].field;
        bool ok = true;
        // ---- Affect masks ----
        // A masked node's affect, at its first stage (a Wet's largest level), on what that takes, per
        // strand or per point (a Tie mixes by its own: field 0).
        if (!mixesByItsAffect(stage.kind) && (k == 0 || chain.stages[k - 1].node != id) && !read(kAffectParam, true, &f.stages[k].affect, true)) {
            return false;
        }
        if (stage.level >= 0) continue;  // a Wet's level: its pull a number of its own
        switch (stage.kind) {
            case StageKind::Interpolate: break;
            case StageKind::Length: ok = read("keep", false, &v[0]) && read("fraction", false, &v[1]); break;
            case StageKind::Clump: ok = read("strength", true, &v[0]) && read("volumize", false, &v[1]); break;
            case StageKind::Noise: ok = read("amplitude", true, &v[0]) && read("fraction", false, &v[1]); break;
            case StageKind::Curl:
                if (graph.node(id).params().getString("shape") == "coil") {
                    // A coil's radius by the strands' widths at each point (times coilWidths), as
                    // the strands it takes carry them.
                    if (!in || in->widths().size() != in->pointCount()) {
                        *why = in ? doc.nameOf(id) + ": the strands carry no widths" : std::string("not evaluated");
                        return false;
                    }
                    v[0].mode = FieldValues::kPerPoint;
                    v[0].values = in->widths();
                    v[0].fixed = true;
                    v[0].widths = true;
                } else {
                    ok = read("radius", true, &v[0]);
                }
                break;
            case StageKind::Width: ok = read("width", true, &v[0]); break;
            case StageKind::Part: ok = read("strength", false, &v[0]); break;
            case StageKind::Gravity: ok = read("strength", false, &v[0]); break;
            case StageKind::Wind: ok = read("strength", false, &v[0]); break;
            case StageKind::Blend: ok = read("mix", true, &v[0]); break;
            case StageKind::Flyaway: ok = read("stray", false, &v[0]) && read("fraction", false, &v[1]); break;
            case StageKind::Collide: break;
            case StageKind::SelfCollide:
            case StageKind::Static: ok = read("strength", false, &v[0]); break;
            case StageKind::Tie:
                // Its affect: which strands it ties and how much each keeps of its result (the
                // graph mixes it by the strand's value; one varying along the strands: the CPU's).
                ok = read(kAffectParam, true, &v[0]);
                if (ok && v[0].mode == FieldValues::kPerPoint) {
                    *why = doc.nameOf(id) + "'s affect varies along its strands (the CPU's)";
                    return false;
                }
                break;
            case StageKind::Bend: ok = read("angle", false, &v[0]); break;
            case StageKind::Direction: {
                // The first two of its mapped params (readLookParams refuses a third).
                const std::vector<std::string> mapped = directionMapped(evaluatedParams(graph, id));
                for (std::size_t i = 0; i < mapped.size() && i < 2 && ok; ++i) ok = read(mapped[i].c_str(), false, &v[i]);
                break;
            }
            case StageKind::Transform: ok = read("scale", false, &v[0]); break;
            case StageKind::Displace: ok = read("distance", true, &v[0]); break;
            case StageKind::Twist: ok = read("turns", false, &v[0]); break;
            // The shapers reading their neighbours: their strength per strand (their affect, where a
            // mask, the stage's own: StageFields::affect).
            case StageKind::Smooth:
            case StageKind::BlendNeighbors:
            case StageKind::Tangle: ok = read("strength", false, &v[0]); break;
            case StageKind::RestoreLength: break;
            case StageKind::Flow: ok = read("lift", false, &v[0]); break;
            case StageKind::Jiggle: ok = read("strength", false, &v[0]); break;
            case StageKind::MeshCut:
            case StageKind::Merge:
            case StageKind::Cull:
            case StageKind::Split:
            case StageKind::Mirror: break;
        }
        if (!ok) return false;
    }
    f.serial = ++serials;
    *out = std::move(f);
    return true;
}

bool sameLookFields(const GraphDocument& doc, const LookChain& chain, const LookFields& fields) {
    return fields.serial != 0 && fields.stages.size() == chain.stages.size() && fields.sources == fieldSources(doc, chain);
}

bool lookFieldsHold(const GraphDocument& doc, const LookChain& chain, const LookFields& fields, std::string* why) {
    if (fields.stages.size() != chain.stages.size()) {
        *why = "the mapped params were read for another chain";
        return false;
    }
    // Each stage's mapped params (look_stages.inc).
    static const char* const kNames[kStageKinds][2] = {
#define GROOMLAB_STAGE(Kind, name, mapped0, mapped1, ramps, vectors, numbers) {mapped0, mapped1},
#include "groomlab/look/look_stages.inc"
#undef GROOMLAB_STAGE
    };
    bool interpolateBefore = false, widthBefore = false, channelsBefore = false;
    for (std::size_t k = 0; k < chain.stages.size(); ++k) {
        const LookStage& stage = chain.stages[k];
        // Its mapped params, then its affect (i 2: read on what it takes, mixing what it made after:
        // no point read after the ones before it moved; a Tie's tail read where the run writes it).
        for (int i = 0; i < 3; ++i) {
            const FieldValues& v = i < 2 ? fields.stages[k].field[i] : fields.stages[k].affect;
            if (!v.mode || v.channelFrom != kNoStage) continue;
            std::string param = i < 2 ? kNames[static_cast<int>(stage.kind)][i] : kAffectParam;
            if (i < 2 && stage.kind == StageKind::Direction) {
                const std::vector<std::string> mapped = directionMapped(evaluatedParams(doc.graph, stage.node));
                param = static_cast<std::size_t>(i) < mapped.size() ? mapped[static_cast<std::size_t>(i)] : "?";
            }
            const std::string name = doc.nameOf(stage.node) + "." + param;
            if (v.widths) {
                // A coil's widths: a Width before it in the run makes them anew, which the engine
                // reads in place of these (a coil by clump has no room for them: the CPU's).
                if (widthBefore && doc.graph.node(stage.node).params().getString("coherence") == "clump") {
                    *why = doc.nameOf(stage.node) + " coils each clump by widths a Width before it makes (the CPU's)";
                    return false;
                }
                continue;
            }
            if (channelsBefore && !v.fixed && !v.binding && !v.program) {
                *why = name + " reads what a Flyaway or Blend before it writes (the CPU's)";
                return false;
            }
            // Made again on the GPU from the run's points (its channels read as LookInputs::read
            // checks: the binding held).
            if (v.program) continue;
            if (k == 0) {
                if (i < 2 && v.readsPositions && v.mode == FieldValues::kPerPoint && movesPointByPoint(stage.kind)) {
                    *why = name + " reads each point after the ones before it moved (the CPU's)";
                    return false;
                }
                continue;
            }
            if (v.readsPositions) {
                *why = name + " reads the points, which the run moves before it";
                return false;
            }
            if (interpolateBefore && !v.fixed && !v.binding) {
                *why = name + " reads what Interpolate makes (the CPU's while Interpolate runs)";
                return false;
            }
        }
        if (stage.kind == StageKind::Interpolate) interpolateBefore = true;
        if (stage.kind == StageKind::Width) widthBefore = true;
        if (stage.kind == StageKind::Flyaway || stage.kind == StageKind::Blend) channelsBefore = true;
        // A Cull, Split or Mirror: any of those may run up its input.
        if (gathers(stage.kind)) interpolateBefore = widthBefore = channelsBefore = true;
    }
    return true;
}

// ---- What Interpolate and Clump take ---------------------------------------------------------

namespace {

std::vector<const Guide*> keptGuides(const GuideSet& guides) {
    std::vector<const Guide*> kept;
    for (const Guide& g : guides.guides()) {
        if (g.points.size() >= 2) kept.push_back(&g);
    }
    return kept;
}

// Whether any of `targets` makes `id`'s result (is it or is upstream of it).
bool dependsOn(const Graph& graph, NodeId id, const std::set<NodeId>& targets) {
    std::vector<NodeId> todo{id};
    std::set<NodeId> seen;
    while (!todo.empty()) {
        const NodeId n = todo.back();
        todo.pop_back();
        if (!n || !seen.insert(n).second) continue;
        if (targets.count(n)) return true;
        const std::size_t slots = graph.inputSpecs(n).size();
        for (std::size_t slot = 0; slot < slots; ++slot) {
            NodeId src = 0;
            if (graph.inputSource(n, slot, &src)) todo.push_back(src);
        }
    }
    return false;
}

// Whether a Clump's centers made by a branch the run moves run on the GPU (GROOMLAB_GPU_CENTERS_BRANCH=0:
// the CPU's, as before they did, for comparisons).
bool centersBranchesOn() {
    static const bool on = [] {
#ifdef _WIN32
        char* v = nullptr;
        std::size_t n = 0;
        const bool off = _dupenv_s(&v, &n, "GROOMLAB_GPU_CENTERS_BRANCH") == 0 && v && v[0] == '0';
        std::free(v);
#else
        const char* v = std::getenv("GROOMLAB_GPU_CENTERS_BRANCH");
        const bool off = v && v[0] == '0';
#endif
        return !off;
    }();
    return on;
}

bool sameStages(const LookChain& a, const LookChain& b) {
    if (a.stages.size() != b.stages.size() || a.base != b.base || a.output != b.output) return false;
    for (std::size_t k = 0; k < a.stages.size(); ++k) {
        if (a.stages[k].node != b.stages[k].node || a.stages[k].level != b.stages[k].level) return false;
    }
    return true;
}

}  // namespace

bool madeFrom(const Graph& graph, NodeId id, const std::set<NodeId>& nodes) { return dependsOn(graph, id, nodes); }

std::size_t firstMadeFrom(const GraphDocument& doc, const LookChain& chain, const std::set<NodeId>& nodes) {
    for (std::size_t k = 0; k < chain.stages.size(); ++k) {
        if (dependsOn(doc.graph, chain.stages[k].node, nodes)) return k;
    }
    return kNoStage;
}

bool sculptedGuides(const GraphDocument& doc, NodeId sculpt, const SculptEdits* edits, double strength, GuidesPtr* out,
                    std::string* why) {
    const Graph& graph = doc.graph;
    const std::string name = doc.nameOf(sculpt);
    if (graph.node(sculpt).typeName() != op::kSculpt) {
        *why = name + " is no Sculpt node";
        return false;
    }
    if (pluggedControl(graph, sculpt, "strength")) {
        *why = name + ".strength is plugged (the CPU's)";
        return false;
    }
    if (graph.bypassedFor(sculpt, true)) {  // handing its input on as it came, whatever the strength
        *out = valueAs<GuidesPtr>(graph.cachedValue(sculpt));
        if (!*out) *why = name + " is not evaluated";
        return *out != nullptr;
    }
    NodeId in = 0, scalpNode = 0;
    std::size_t inOutput = 0, scalpOutput = 0;
    const GuidesPtr fed = graph.inputSource(sculpt, 0, &in, &inOutput) ? valueAs<GuidesPtr>(graph.cachedValue(in, inOutput)) : nullptr;
    if (!fed || !valueAs<GuidesPtr>(graph.cachedValue(sculpt))) {
        *why = name + " is not evaluated";
        return false;
    }
    const MeshPtr scalp = graph.inputSource(sculpt, 1, &scalpNode, &scalpOutput) ? valueAs<MeshPtr>(graph.cachedValue(scalpNode, scalpOutput))
                                                                                  : nullptr;
    // As SculptOp evaluates it, the number clamped as a stored one.
    const float s = static_cast<float>(graph.node(sculpt).params().clamped("strength", strength));
    *out = !edits || edits->empty() || s == 0.0f ? fed : applySculpts(fed, *edits, s, scalp.get());
    return true;
}

bool LookInputs::centersRun(const GraphDocument& doc, NodeId clump, NodeId end, const std::set<NodeId>& moved, const PartLines* parts,
                            const ScalpMesh* scalp, std::shared_ptr<const LookCentersRun>* out, std::string* why,
                            const std::map<NodeId, GuidesPtr>& standIn) {
    const Graph& graph = doc.graph;
    const std::string name = doc.nameOf(clump) + "'s centers";
    LookChain full;
    std::string whyNot;
    if (!findLookChainTo(doc, end, &full, &whyNot)) {
        *why = name + ": " + whyNot;
        return false;
    }
    // From the first of its nodes the run changes (an Interpolate of a stroke's guides): those
    // before it keep their cached results, and the strands it starts from must be among those.
    std::size_t first = kNoStage;
    for (std::size_t k = 0; k < full.stages.size() && first == kNoStage; ++k) {
        if (dependsOn(graph, full.stages[k].node, moved)) first = k;
    }
    if (first == kNoStage) {
        *why = name + " are made by nodes the GPU does not run (" + doc.nameOf(full.base) + ")";
        return false;
    }
    auto run = std::make_shared<LookCentersRun>();
    run->chain = chainFrom(doc, full, first);
    if (dependsOn(graph, run->chain.base, moved)) {
        *why = name + " are made from strands the run moves before the GPU's part of their branch (the CPU's)";
        return false;
    }
    StrandsPtr made;
    if (!readLookParams(doc, run->chain, {}, &run->params, &whyNot) || !lookStrands(doc, run->chain, &run->base, &made, &whyNot)) {
        *why = name + ": " + whyNot;
        return false;
    }
    CentersMemo& memo = centers_[clump];
    if (!memo.inputs) memo.inputs = std::make_shared<LookInputs>();
    if (!sameStages(memo.fieldsChain, run->chain) || !sameLookFields(doc, run->chain, memo.fields)) {
        LookFields read;
        if (!readLookFields(doc, run->chain, scalp, &read, &whyNot)) {
            memo.fields = LookFields{};
            memo.fieldsChain = LookChain{};
            *why = name + ": " + whyNot;
            return false;
        }
        memo.fields = std::move(read);
        memo.fieldsChain = run->chain;
    }
    if (!lookFieldsHold(doc, run->chain, memo.fields, &whyNot)) {
        *why = name + ": " + whyNot;
        return false;
    }
    run->fields = memo.fields;
    if (!memo.inputs->read(doc, run->chain, run->params, parts, &run->inputs, &whyNot, moved, scalp, &run->fields, scalpMesh_, standIn)) {
        *why = name + ": " + whyNot;
        return false;
    }
    rebinds_ += memo.inputs->rebinds() - memo.rebindsSeen;
    memo.rebindsSeen = memo.inputs->rebinds();
    guidesMade_ += memo.inputs->lastGuidesMade();
    *out = std::move(run);
    return true;
}

bool LookInputs::branchRun(const GraphDocument& doc, NodeId node, const LookBranch& branch, const PartLines* parts, const ScalpMesh* scalp,
                           const std::set<NodeId>& changed, const std::map<NodeId, GuidesPtr>& standIn,
                           std::shared_ptr<const LookCentersRun>* out, std::string* why) {
    const Graph& graph = doc.graph;
    const bool merging = graph.node(node).typeName() == op::kMerge;
    const std::string name = doc.nameOf(node) + (merging ? "'s input b" : "'s input");
    auto run = std::make_shared<LookCentersRun>();
    run->chain = branch.chain;
    run->params = branch.params;
    run->scalp = scalp;  // (the groom's, as the run's own strands take it)
    StrandsPtr made;
    std::string whyNot;
    if (!lookStrands(doc, run->chain, &run->base, &made, &whyNot)) {
        *why = name + ": " + whyNot;
        return false;
    }
    if (merging) {
        // b's strands after a's in the Merge's result, as its run's strands are laid out (MergeOp appends them).
        const StrandsPtr merged = cachedStrands(graph, node), a = inputStrands(graph, node, 0);
        if (!merged || !a || merged->strandCount() != a->strandCount() + run->base->strandCount() ||
            merged->pointCount() != a->pointCount() + run->base->pointCount()) {
            *why = name + ": its strands are not laid out after a's in " + doc.nameOf(node) + "'s result";
            return false;
        }
    } else {
        // Laid out as the strands the node took (as made: readGather's layout is from them).
        const StrandsPtr in = inputStrands(graph, node);
        if (!in || in->strandCount() != run->base->strandCount() || in->pointCount() != run->base->pointCount()) {
            *why = name + ": its run's strands are laid out unlike the strands " + doc.nameOf(node) + " took";
            return false;
        }
    }
    CentersMemo& memo = branches_[node];
    if (!memo.inputs) memo.inputs = std::make_shared<LookInputs>();
    if (!sameStages(memo.fieldsChain, run->chain) || !sameLookFields(doc, run->chain, memo.fields)) {
        LookFields read;
        if (!readLookFields(doc, run->chain, scalp, &read, &whyNot)) {
            memo.fields = LookFields{};
            memo.fieldsChain = LookChain{};
            *why = name + ": " + whyNot;
            return false;
        }
        memo.fields = std::move(read);
        memo.fieldsChain = run->chain;
    }
    if (!lookFieldsHold(doc, run->chain, memo.fields, &whyNot)) {
        *why = name + ": " + whyNot;
        return false;
    }
    run->fields = memo.fields;
    if (!memo.inputs->read(doc, run->chain, run->params, parts, &run->inputs, &whyNot, changed, scalp, &run->fields, scalpMesh_, standIn)) {
        *why = name + ": " + whyNot;
        return false;
    }
    rebinds_ += memo.inputs->rebinds() - memo.rebindsSeen;
    memo.rebindsSeen = memo.inputs->rebinds();
    guidesMade_ += memo.inputs->lastGuidesMade();
    *out = std::move(run);
    return true;
}

bool LookInputs::read(const GraphDocument& doc, const LookChain& chain, const LookParams& params, const PartLines* parts,
                      std::vector<StageInputs>* inputs, std::string* why, const std::set<NodeId>& changed,
                      const ScalpMesh* groomScalp, const LookFields* fields, const MeshPtr& scalpMesh,
                      const std::map<NodeId, GuidesPtr>& standIn) {
    const auto start = std::chrono::steady_clock::now();
    const Graph& graph = doc.graph;
    if (doc_ != &doc) {  // another document: its operators keep their own
        interpolate_.clear();
        clump_.clear();
        curl_.clear();
        part_.clear();
        collide_.clear();
        reach_.clear();
        centers_.clear();
        grid_.clear();
        tie_.clear();
        roots_.clear();
        twist_.clear();
        shaper_.clear();
        jiggle_.clear();
        meshCut_.clear();
        branches_.clear();
        gather_.clear();
        doc_ = &doc;
    }
    scalpMesh_ = scalpMesh;
    bool rebound = false;  // a running Interpolate's binding bound again for moved guide roots
    // Whether the binding a run's Interpolate blends with gives the guides' influence its cached
    // result holds (not bound again, its radius and falloff its own).
    bool influenceHolds = true;
    guidesMade_ = 0;
    std::vector<StageInputs> out(chain.stages.size());
    std::set<NodeId> before = changed;  // the stages before the current one (and changed): what the run moves
    for (std::size_t k = 0; k < chain.stages.size(); ++k) {
        const LookStage& stage = chain.stages[k];
        const NodeId id = stage.node;
        const std::string name = doc.nameOf(id);
        if (stage.kind == StageKind::Interpolate) {
            InterpolateState state;
            const StrandsPtr result = cachedStrands(graph, id);
            if (!interpolateState(graph.node(id), result, &state)) {
                *why = name + " keeps no binding for its result";
                return false;
            }
            const StrandsPtr in = inputStrands(graph, id);
            if (!in || in != state.strands || state.strandCount != in->strandCount()) {
                *why = name + "'s binding is for other strands";
                return false;
            }
            NodeId guidesNode = 0, scalpNode = 0;
            std::size_t guidesOutput = 0, scalpOutput = 0;
            graph.inputSource(id, 1, &guidesNode, &guidesOutput);
            const bool hasScalp = graph.inputSource(id, 2, &scalpNode, &scalpOutput);
            // The guides it takes: made for the run (a dragged Sculpt node's), else as cached.
            const auto stands = guidesOutput == 0 ? standIn.find(guidesNode) : standIn.end();
            if (stands == standIn.end() && !standIn.empty()) {
                std::set<NodeId> makers;
                for (const auto& m : standIn) makers.insert(m.first);
                if (dependsOn(graph, guidesNode, makers)) {
                    *why = name + "'s guides are made from " + doc.nameOf(*makers.begin()) + " through nodes the GPU does not run (the CPU's)";
                    return false;
                }
            }
            const GuidesPtr guides = stands != standIn.end() ? stands->second : valueAs<GuidesPtr>(graph.cachedValue(guidesNode, guidesOutput));
            const MeshPtr scalp = hasScalp ? valueAs<MeshPtr>(graph.cachedValue(scalpNode, scalpOutput)) : nullptr;
            if (!guides || !state.guides) {
                *why = name + " has no guides";
                return false;
            }
            const StageParams* p = k < params.stages.size() ? &params.stages[k] : nullptr;
            const std::size_t samples = interpolateSamples(*in);
            if (!p || static_cast<std::size_t>(p->flag[0]) != samples) {
                *why = name + ": its params were read for other strands";
                return false;
            }
            const bool smooth = graph.node(id).params().getBool("smoothGuides");
            InterpolateMemo& memo = interpolate_[id];
            if (memo.made != state.binding) {
                // The operator bound again (an evaluation: a stroke's press). Where it bound the same
                // strands, with the same params, to guide roots a rounding from those the binding held
                // here (a reshaped guide's smooth curve starts a rounding away), the one held stands:
                // its weights are a rounding apart, and it is on the GPU already.
                bool same = memo.current && memo.current->baseKey == state.baseKey && memo.current->strandCount == state.strandCount &&
                            memo.current->strands == state.strands && memo.current->roots.size() == state.roots.size();
                for (std::size_t g = 0; same && g < state.roots.size(); ++g) {
                    same = (state.roots[g] - memo.current->roots[g]).length() <= 1e-5f * std::max(1.0f, memo.current->roots[g].length());
                }
                memo.made = state.binding;
                if (!same) {
                    memo.current = std::make_shared<InterpolateState>(state);
                    memo.fitting.clear();
                }
                memo.rows.reset();
                memo.rebound = false;  // (held or not, a rounding from the binding its result was made with)
            }
            if (memo.guides != guides || memo.samples != samples || memo.smooth != smooth || memo.scalp != scalp.get() || !memo.rows) {
                // The guides the binding was made for: as many kept, the same IDs in order, and
                // where guide attributes or layers become channels, the same of those.
                const std::vector<const Guide*> kept = keptGuides(*guides), was = keptGuides(*state.guides);
                if (kept.size() != was.size() || kept.size() != state.roots.size()) {
                    *why = name + ": guides were added or removed (the CPU binds again)";
                    return false;
                }
                for (std::size_t g = 0; g < kept.size(); ++g) {
                    if (kept[g]->id != was[g]->id) {
                        *why = name + ": other guides (the CPU binds again)";
                        return false;
                    }
                    if (state.channels && (kept[g]->attributes != was[g]->attributes || kept[g]->pointAttributes != was[g]->pointAttributes)) {
                        *why = name + ": guide attributes changed (the CPU blends them)";
                        return false;
                    }
                }
                auto rows = std::make_shared<InterpolateGuideRows>();
                interpolateGuideRows(graph.node(id).params(), kept, scalp, samples, rows.get());
                // Guide roots moved (a dab bends a guide at its root, and the children following it
                // root elsewhere): the strands bound again as Interpolate binds them, sharing what no
                // moved root reaches. A reshaped guide's root moves by rounding alone (the smooth
                // curve's first point is a blend of its first point with itself), which the CPU binds
                // again too, to the same guides with weights a rounding apart: those keep the
                // binding's roots, so only the strands near roots that did move are bound and sent again.
                std::vector<Vec3f> roots = memo.current->roots;
                bool moved = false;
                for (std::size_t g = 0; g < kept.size(); ++g) {
                    const float rounding = 1e-5f * std::max(1.0f, roots[g].length());
                    if (!((rows->roots[g] - roots[g]).length() <= rounding)) {
                        roots[g] = rows->roots[g];
                        moved = true;
                    }
                }
                if (moved) {
                    auto bound = std::make_shared<InterpolateState>(*memo.current);
                    std::vector<StrandRoot> guideRoots(kept.size());
                    for (std::size_t g = 0; g < kept.size(); ++g) guideRoots[g] = kept[g]->root;
                    if (!interpolateRebind(graph.node(id), parts, roots, guideRoots, bound.get())) {
                        *why = name + ": its guides could not be bound again";
                        return false;
                    }
                    memo.current = std::move(bound);
                    memo.rebound = true;
                    ++rebinds_;
                }
                memo.guides = guides;
                memo.samples = samples;
                memo.smooth = smooth;
                memo.scalp = scalp.get();
                memo.rows = std::move(rows);
                guidesMade_ += kept.size();
            }
            // keepClearance: the scalp's field as tables, once per scalp.
            std::shared_ptr<const InterpolateClearance> clearance;
            if (interpolateKeepsClearance(graph.node(id).params(), scalp.get())) {
                if (memo.clearanceScalp != scalp || !memo.clearance) {
                    std::string whyNot;
                    memo.clearance = interpolateClearance(graph.node(id).params(), scalp, &whyNot);
                    memo.clearanceScalp = scalp;
                    if (!memo.clearance) {
                        *why = name + ": " + whyNot + " (the CPU's)";
                        return false;
                    }
                }
                clearance = memo.clearance;
                // Over a deformed scalp: each strand's walk starts at the vertex of its root's face
                // nearest its first point (RestPoseCollision::cursor).
                if (clearance->rest && (memo.cursorsFor != clearance || memo.cursorStrands != in || !memo.cursors)) {
                    auto cursors = std::make_shared<std::vector<std::uint32_t>>(in->strandCount(), 0xFFFFFFFFu);
                    const RestPoseCollision& rest = *clearance->rest;
                    parallelFor(0, in->strandCount(), [&](std::size_t s) {
                        if (in->strandPointCount(s) == 0) return;
                        const RestPoseCollision::Cursor c = rest.cursor(in->roots()[s], in->strandPoints(s)[0]);
                        if (c.vertex != RestPoseCollision::Cursor::kNone) (*cursors)[s] = static_cast<std::uint32_t>(c.vertex);
                    }, 4096);
                    memo.cursors = std::move(cursors);
                    memo.cursorsFor = clearance;
                    memo.cursorStrands = in;
                    // How near the strands' roots the scalp's degenerate faces' vertices come (which
                    // push asks the mesh at: the CPU's alone).
                    const std::vector<std::uint32_t>& w = *clearance->walk;
                    std::vector<Vec3f> degenerate;
                    for (std::uint32_t v = 0; v < w[RestPoseCollision::kWalkVertices]; ++v) {
                        if (w[w[RestPoseCollision::kWalkMotions] + v * RestPoseCollision::kWalkMotionWords] != RestPoseCollision::kWalkNoMotion) continue;
                        Vec3f q;
                        std::memcpy(&q.x, &w[w[RestPoseCollision::kWalkPoints] + v * 3], sizeof(Vec3f));
                        degenerate.push_back(q);
                    }
                    memo.degenerateApart = std::numeric_limits<float>::infinity();
                    if (!degenerate.empty()) {
                        const PointIndex index(degenerate);
                        for (std::size_t s = 0; s < in->strandCount(); ++s) {
                            if (in->strandPointCount(s) == 0) continue;
                            float d2 = 0.0f;
                            index.nearest(in->strandPoints(s)[0], &d2);
                            memo.degenerateApart = std::min(memo.degenerateApart, std::sqrt(d2));
                        }
                    }
                }
                if (clearance->rest) {
                    // A walk from a root reaches no vertex farther from it than twice the strand's
                    // reach (twice the longest guide: lookBounds) and two of the longest edge, and
                    // asks a vertex's motion only within the longest edge and the search of it.
                    float longest = 0.0f;
                    for (const float length : memo.rows->lengths) longest = std::max(longest, length);
                    float edge = 0.0f;
                    std::memcpy(&edge, &(*clearance->walk)[RestPoseCollision::kWalkLongestEdge], sizeof(float));
                    const float reach = 4.0f * longest + 3.0f * edge + clearance->searchRadius;
                    if (!(memo.degenerateApart > reach * 1.01f)) {
                        *why = name + ": its strands come near a degenerate face of the deformed scalp (the CPU's)";
                        return false;
                    }
                }
            }
            // Each strand's guides fit the GPU's slots (kInterpolateSlots): the chunks not seen yet.
            // (Only the binding's live chunks are remembered: a freed chunk's address may come back.)
            const std::vector<const void*> chunks = interpolateChunks(*memo.current);
            const std::size_t per = interpolateChunkStrands();
            std::set<const void*> fitting;
            for (std::size_t c = 0; c < chunks.size(); ++c) {
                if (!memo.fitting.count(chunks[c])) {
                    InterpolateRecords records;
                    interpolateRecords(*memo.current, c * per, per, &records);
                    for (std::size_t s = 0; s + 1 < records.offsets.size(); ++s) {
                        if (records.offsets[s + 1] - records.offsets[s] > kInterpolateSlots) {
                            *why = name + ": strands blend more than " + std::to_string(kInterpolateSlots) + " guides (the CPU's)";
                            return false;
                        }
                    }
                }
                fitting.insert(chunks[c]);
            }
            memo.fitting.swap(fitting);
            if (memo.strands != in || !memo.normals) {
                // Each strand's normal where the blend turns its guides to (zero: not turned).
                auto normals = std::make_shared<std::vector<Vec3f>>(in->strandCount());
                const bool align = scalp && scalp->triangleCount() > 0;
                parallelFor(0, in->strandCount(), [&](std::size_t s) {
                    const StrandRoot& root = in->roots()[s];
                    (*normals)[s] = align && scalp->contains(root) ? scalp->normal(root) : Vec3f{};
                }, 4096);
                memo.strands = in;
                memo.normals = std::move(normals);
            }
            auto data = std::make_shared<InterpolateInputs>();
            data->binding = memo.current;
            data->rows = memo.rows;
            data->strandNormals = memo.normals;
            if (clearance && clearance->rest) data->cursors = memo.cursors;
            data->clearance = std::move(clearance);
            out[k].interpolate = std::move(data);
            // The guides' influence as its cached result holds it: the same binding, radius and
            // falloff (what a centerWeight reading it picked the Clump's centers by).
            rebound = rebound || memo.rebound;
            const ParamSet own = evaluatedParams(graph, id);
            influenceHolds = influenceHolds && !memo.rebound && p->value[0] == static_cast<float>(own.getDouble("limitDistance")) &&
                             p->ramp == own.getRamp("falloff");
        } else if (stage.kind == StageKind::Clump) {
            const StrandsPtr result = cachedStrands(graph, id);
            ClumpMemo& memo = clump_[{id, stage.level}];
            if (memo.output != result) memo = ClumpMemo{};
            const StageParams* p = k < params.stages.size() ? &params.stages[k] : nullptr;
            const std::uint64_t seed = p ? p->seed : 0;
            // A Wet's level: its Clump's params and center choice as Wet kept them for its result
            // (WetState). A smaller level takes the strands the one before it moves in the run.
            std::shared_ptr<const WetState> wet;
            const auto level = static_cast<std::size_t>(std::max(stage.level, 0));
            if (stage.level >= 0 && (!wetState(graph.node(id), result, &wet) || level >= wet->levels.size())) {
                *why = name + " keeps no center choice for its result";
                return false;
            }
            const ParamSet& clumpParams = wet ? wet->params[level] : graph.node(id).params();
            auto keptState = [&](ClumpState* s) { return wet ? (*s = wet->levels[level], true) : clumpState(graph.node(id), result, s); };
            ClumpState state;
            if (!memo.fixed) {
                if (!keptState(&state)) {
                    *why = name + " keeps no center choice for its result";
                    return false;
                }
                if (state.centerIds.size() != state.centerGuides.size() || state.centerRoots.size() != state.centerGuides.size()) {
                    *why = name + " keeps no centers' IDs";
                    return false;
                }
                auto fixed = std::make_shared<ClumpInputs>();
                fixed->assigned = state.assigned;
                fixed->centers = state.centerGuides.size();
                fixed->curves = std::make_shared<const std::vector<Vec3f>>(state.curves);
                fixed->normals = std::make_shared<const std::vector<Vec3f>>(state.centerNormals);
                // copyAmount, for centers made on the GPU: each center's nearest centers by the
                // roots it was assigned by (ClumpOp::shareCenters; a center of centersFrom inputPoints
                // gathering no strand neither gives nor takes), in the order it sums them.
                const std::size_t count = fixed->centers;
                if (clumpParams.getDouble("copyAmount") > 0.0 && count > 1) {
                    const std::size_t want = std::min<std::size_t>(static_cast<std::size_t>(clumpParams.getInt("copyNeighbors")), count - 1);
                    const PointIndex index(state.centerRoots);
                    const bool axes = state.axes.size() == count;
                    auto shapeless = [&](std::size_t c) { return axes && state.axes[c] == kNoClumpCenter; };
                    auto neighbors = std::make_shared<std::vector<std::uint32_t>>(count * 9, 0u);
                    parallelFor(0, count, [&](std::size_t c) {
                        if (shapeless(c)) return;
                        std::array<std::size_t, 9> near{};
                        std::array<float, 9> d2{};
                        const std::size_t found = index.kNearest(state.centerRoots[c], want + 1, near.data(), d2.data());
                        std::uint32_t* to = neighbors->data() + c * 9;
                        std::uint32_t taken = 0;
                        for (std::size_t j = 0; j < found && taken < want; ++j) {
                            if (near[j] == c || shapeless(near[j])) continue;
                            to[1 + taken++] = static_cast<std::uint32_t>(near[j]);
                        }
                        to[0] = taken;
                    }, 16);
                    fixed->neighbors = std::move(neighbors);
                }
                memo.ids = std::make_shared<const std::vector<StrandId>>(state.centerIds);
                if (clumpParams.getString("pastTip") == "extend") {
                    // How far past an axis's tip a point can go: the longest strand it takes (a
                    // Wet's level: the longest the Wet takes).
                    if (const StrandsPtr in = inputStrands(graph, id)) {
                        for (std::size_t i = 0; i < in->strandCount(); ++i) {
                            fixed->reach = std::max(fixed->reach, arcLength(in->strandPoints(i), in->strandPointCount(i)));
                        }
                    }
                }
                memo.output = result;
                memo.fixed = std::move(fixed);
            }
            NodeId centersNode = 0;
            std::size_t centersOutput = 0;
            // centersFrom pick: the Clump picks its centers among the strands it takes (a centers
            // input ignored); else they are its centers input's, and with nothing linked it clumps
            // nothing (the CPU's).
            const std::string centersFrom = clumpParams.getString("centersFrom");
            const bool own = centersFrom == "pick";
            if (!own && !graph.inputSource(id, 1, &centersNode, &centersOutput)) {
                *why = name + " has no centers connected";
                return false;
            }
            // inputPoints: each center's curve is a strand it takes (its axis).
            const bool fromStrands = centersFrom == "inputPoints";
            // inputCurves: each center's curve is a strand of its centers input (their preview core,
            // strandsAsGuides), resampled.
            const bool fromCurves = centersFrom == "inputCurves";
            // Centers the run moves (made from a stage before this one): the Clump's own choice of
            // the strands it takes, or its axis strands, resampled on the GPU; or the curves of a
            // branch of their own the run moves (a stroke's guides through another Interpolate),
            // made by that branch's run on the GPU, then resampled.
            if (stage.level <= 0 && !dependsOn(graph, own || fromStrands ? upstream(graph, id) : centersNode, before)) {
                out[k].clump = memo.fixed;
            } else {
                if (!memo.tried) {
                    memo.tried = true;
                    const bool kept = (own || fromStrands || fromCurves) && keptState(&state);
                    auto strands = std::make_shared<std::vector<std::uint32_t>>();
                    bool ok = false;
                    // Its own choice by the roots alone (`chosen`), else by centerWeight's values
                    // too (`picked`): those hold while what the weight reads does (below).
                    const bool byRoots = kept && own && !state.chosen.empty();
                    const std::vector<std::size_t>& chosen = byRoots ? state.chosen : state.picked;
                    if (kept && own && !chosen.empty()) {
                        ok = true;
                        for (const std::size_t g : state.centerGuides) {
                            ok = ok && g < chosen.size();
                            if (ok) strands->push_back(static_cast<std::uint32_t>(chosen[g]));
                        }
                        memo.weighed = !byRoots;
                    } else if (kept && fromStrands && state.axes.size() == state.centerGuides.size()) {
                        // A center gathering no strand: any strand (no strand clumps toward it).
                        ok = true;
                        for (const std::uint32_t a : state.axes) strands->push_back(a == kNoClumpCenter ? 0u : a);
                    } else if (kept && fromCurves && centersOutput == 0 && centersBranchesOn()) {
                        // Center c is guide centerGuides[c] of the centers input as guides: its
                        // strands' preview core in order.
                        if (const StrandsPtr curves = valueAs<StrandsPtr>(graph.cachedValue(centersNode, centersOutput))) {
                            std::vector<std::uint32_t> core;
                            const PreviewCore preview(*curves);
                            for (std::size_t s = 0; s < curves->strandCount(); ++s) {
                                if (preview.core(s)) core.push_back(static_cast<std::uint32_t>(s));
                            }
                            ok = true;
                            for (const std::size_t g : state.centerGuides) {
                                ok = ok && g < core.size() && curves->strandPointCount(core[g]) >= 2;
                                if (ok) strands->push_back(core[g]);
                            }
                            memo.branch = true;
                        }
                    }
                    if (ok) {
                        auto made = std::make_shared<ClumpInputs>(*memo.fixed);
                        made->curves = nullptr;
                        made->chosen = std::move(strands);
                        memo.made = std::move(made);
                    }
                }
                if (!memo.made) {
                    *why = name + "'s centers move with the strands the run moves, and are not strands it takes";
                    return false;
                }
                if (memo.weighed && !wet) {  // (a Wet's levels weigh every strand alike: their choice holds)
                    // Picked by centerWeight's values: they hold for the run while what they read
                    // does (the roots, channels nothing before writes, the binding's channels while
                    // it is not bound again, the guides' influence while its radius and falloff too).
                    const StrandsPtr in = inputStrands(graph, id);
                    bool holds = false;
                    try {
                        const ScalarField weight(evaluatedParams(graph, id), "centerWeight", groomScalp);
                        if (in) weight.validate(*in);
                        const FieldRead r = whatItReads(graph, chain, k, weight);
                        holds = in && !r.unknown && !r.layer && !r.positions && (!r.binding || !rebound) &&
                                (!r.influence || influenceHolds);
                    } catch (const std::exception&) {
                        holds = false;
                    }
                    if (!holds) {
                        *why = name + " picks its centers by a centerWeight the run changes (the CPU's)";
                        return false;
                    }
                }
                out[k].clump = memo.made;
                if (memo.branch) {
                    // The centers' branch run on the GPU before the Clump, for the strands it names.
                    std::shared_ptr<const LookCentersRun> run;
                    if (!centersRun(doc, id, centersNode, before, parts, groomScalp, &run, why, standIn)) return false;
                    for (const std::uint32_t s : *memo.made->chosen) {
                        if (s >= run->base->strandCount()) {
                            *why = name + "'s centers are other strands than its centers' branch makes";
                            return false;
                        }
                    }
                    auto made = std::make_shared<ClumpInputs>(*memo.made);
                    made->centersRun = std::move(run);
                    out[k].clump = std::move(made);
                }
            }
            // The per-clump variation: drawn from the centers' IDs and the seed (a seed dragged
            // draws again).
            if (memo.seed != seed || !memo.variation) {
                auto variation = std::make_shared<std::vector<float>>(memo.ids->size() * 4, 0.0f);
                for (std::size_t c = 0; c < memo.ids->size(); ++c) {
                    const StrandId cid = (*memo.ids)[c];
                    float* v = variation->data() + c * 4;
                    v[0] = unitFloat(hashCombine(cid, seed ^ 0x57E4Aull));
                    v[1] = unitFloat(hashCombine(cid, seed ^ 0x2AD1Bull));
                    v[2] = unitFloat(hashCombine(cid, seed ^ 0xF1A7));
                }
                memo.variation = std::move(variation);
                memo.seed = seed;
            }
            auto with = std::make_shared<ClumpInputs>(*out[k].clump);
            with->variation = memo.variation;
            out[k].clump = std::move(with);
        } else if (stage.kind == StageKind::Curl && k < params.stages.size() && params.stages[k].flag[2] != 0) {
            // Coherent by clump: the clumps found on the strands it takes, as the CPU finds them.
            const StrandsPtr in = inputStrands(graph, id);
            if (!in) {
                *why = "not evaluated";
                return false;
            }
            const std::uint64_t seed = params.stages[k].seed;
            CurlMemo& memo = curl_[id];
            if (memo.in != in || memo.seed != seed || !memo.inputs) {
                const std::string channel = graph.node(id).params().getString("clumpChannel");
                const std::vector<float>* groups = in->strandChannel(channel);
                if (!groups) {
                    *why = name + ": coherence clump needs a Clump before it (no '" + channel + "' channel)";
                    return false;
                }
                const std::size_t strands = in->strandCount();
                auto clumpOf = std::make_shared<std::vector<std::uint32_t>>(strands, 0xffffffffu);
                std::map<long long, std::size_t> slot;
                std::vector<long long> keys;
                std::vector<std::vector<std::uint32_t>> members;
                const PreviewCore core(*in);
                for (std::size_t s = 0; s < strands; ++s) {
                    const long long key = std::llround((*groups)[s]);
                    if (key < 0 || in->strandPointCount(s) < 2) continue;
                    auto it = slot.find(key);
                    if (it == slot.end()) {
                        it = slot.emplace(key, members.size()).first;
                        members.emplace_back();
                        keys.push_back(key);
                    }
                    (*clumpOf)[s] = static_cast<std::uint32_t>(it->second);
                    if (core.core(s)) members[it->second].push_back(static_cast<std::uint32_t>(s));
                }
                for (std::uint32_t& c : *clumpOf) {
                    if (c != 0xffffffffu && members[c].empty()) c = 0xffffffffu;  // no core strand: alone
                }
                auto list = std::make_shared<std::vector<std::uint32_t>>(members.size() + 1, 0u);
                for (std::size_t c = 0; c < members.size(); ++c) (*list)[c + 1] = (*list)[c] + static_cast<std::uint32_t>(members[c].size());
                for (const auto& m : members) list->insert(list->end(), m.begin(), m.end());
                auto clumps = std::make_shared<std::vector<float>>(members.size() * 8, 0.0f);
                parallelFor(0, members.size(), [&](std::size_t c) {
                    float* to = clumps->data() + c * 8;
                    Vec3f normal;
                    for (const std::uint32_t s : members[c]) {
                        const StrandRoot& root = in->roots()[s];
                        normal += groomScalp && groomScalp->contains(root) ? groomScalp->normal(root) : Vec3f{};
                    }
                    normal = normal.normalized();
                    to[0] = normal.x;
                    to[1] = normal.y;
                    to[2] = normal.z;
                    // CurlOp::drawFor's four draws, from the clump's key.
                    Rng rng(hashCombine(hashCombine(hash64(static_cast<std::uint64_t>(keys[c])), 0xC1D0C0E5ull), seed ^ 0xC041));
                    for (int d = 0; d < 4; ++d) to[4 + d] = rng.nextFloat();
                }, 16);
                auto made = std::make_shared<CurlInputs>();
                made->clumpOf = std::move(clumpOf);
                made->members = std::move(list);
                made->clumps = std::move(clumps);
                made->count = members.size();
                memo.in = in;
                memo.seed = seed;
                memo.inputs = std::move(made);
            }
            out[k].curl = memo.inputs;
        } else if (stage.kind == StageKind::Part) {
            // Each strand's side of the parts, from its root on the rest scalp (fixed).
            const StrandsPtr in = inputStrands(graph, id);
            if (!in) {
                *why = "not evaluated";
                return false;
            }
            // The groom's painted parts, or with partsFrom auto the lines its last evaluation found
            // in the strands it took (as it found them: a run upstream moves those strands, the
            // release finds them again), every one of them.
            const ParamSet& own = graph.node(id).params();
            const bool flow = own.getString("partsFrom") == "auto";
            PartsPtr found;
            if (flow && !autoPartState(graph.node(id), cachedStrands(graph, id), &found)) {
                *why = name + " keeps no parts found in the hair's flow for its result";
                return false;
            }
            const PartLines* lines = flow ? found.get() : parts;
            PartMemo& memo = part_[id];
            if (memo.in != in || memo.parts != lines || memo.found != found || memo.scalp != groomScalp || !memo.inputs) {
                const std::shared_ptr<PartInputs> made = partInputs(*in, lines, groomScalp, flow ? std::string() : own.getString("part"));
                for (std::size_t s = 0; s < in->strandCount(); ++s) {
                    made->reach = std::max(made->reach, arcLength(in->strandPoints(s), in->strandPointCount(s)));
                }
                memo.in = in;
                memo.parts = lines;
                memo.found = found;
                memo.scalp = groomScalp;
                memo.inputs = std::move(made);
            }
            out[k].part = memo.inputs;
        }
        if (stage.kind == StageKind::Gravity || stage.kind == StageKind::Flyaway || stage.kind == StageKind::Collide ||
            stage.kind == StageKind::Wind || stage.kind == StageKind::SelfCollide || stage.kind == StageKind::Static ||
            stage.kind == StageKind::Tie || stage.kind == StageKind::Bend || stage.kind == StageKind::Direction ||
            stage.kind == StageKind::Transform || stage.kind == StageKind::Jiggle || readsNeighbours(stage.kind)) {
            // How far from its root a point the node moves can go: the longest strand it took.
            const StrandsPtr in = inputStrands(graph, id);
            if (!in) {
                *why = "not evaluated";
                return false;
            }
            ReachMemo& memo = reach_[id];
            if (memo.in != in) {
                std::vector<float> lengths(in->strandCount(), 0.0f);
                parallelFor(0, in->strandCount(), [&](std::size_t s) { lengths[s] = arcLength(in->strandPoints(s), in->strandPointCount(s)); },
                            4096);
                memo.reach = lengths.empty() ? 0.0f : *std::max_element(lengths.begin(), lengths.end());
                memo.in = in;
            }
            out[k].reach = memo.reach;
        }
        if (stage.kind == StageKind::SelfCollide || stage.kind == StageKind::Static) {
            // The hair grid as the node lays it on the strands it took (the strands splatted are the
            // core by ID, the cell by the scalp's area: the same for the strands a run hands it), and
            // the scalp near the grid's nodes for that cell (kept while the scalp and the cell are).
            const StrandsPtr in = inputStrands(graph, id);
            if (!in) {
                *why = "not evaluated";
                return false;
            }
            if (groomScalp && (!scalpMesh || scalpMesh.get() != groomScalp)) {
                *why = name + ": the groom's scalp is not at hand";
                return false;
            }
            GridMemo& memo = grid_[id];
            if (memo.in != in || memo.scalp != scalpMesh || !memo.inputs) {
                auto layout = std::make_shared<HairGridLayout>();
                std::string whyNot;
                if (!hairGridLayout(graph.node(id).typeName(), *in, scalpMesh, layout.get(), &whyNot)) {
                    *why = name + ": " + whyNot + " (the CPU's)";
                    return false;
                }
                if (layout->coarsened) {
                    *why = name + "'s hair spreads far wider than its scalp: its grid's cells coarsen (the CPU's)";
                    return false;
                }
                auto made = std::make_shared<HairGridInputs>();
                made->layout = layout;
                if (scalpMesh) {
                    const HairGridInputs* held = memo.inputs.get();
                    if (held && held->walls && memo.scalp == scalpMesh && held->layout->h == layout->h &&
                        held->layout->wallReach == layout->wallReach) {
                        made->walls = held->walls;
                    } else {
                        made->walls = hairGridWalls(*scalpMesh, layout->h, layout->wallReach);
                    }
                    if (!made->walls) {
                        *why = name + ": its scalp is too large for its grid's cell (the CPU's)";
                        return false;
                    }
                }
                memo.in = in;
                memo.scalp = scalpMesh;
                memo.inputs = std::move(made);
            }
            // Without a scalp the cell is a share of the strands' size: of the strands it took, which
            // a run moving them before it changes.
            if (!scalpMesh && stage.from != kNoStage) {
                *why = name + " has no scalp: its grid's cell goes by the strands the run moves before it (the CPU's)";
                return false;
            }
            out[k].grid = memo.inputs;
        }
        if (stage.kind == StageKind::Collide) {
            // Its surfaces' fields as tables, once per surface (the scalp's, then the collider's).
            const StageParams* p = k < params.stages.size() ? &params.stages[k] : nullptr;
            if (!p) {
                *why = name + ": its params were read for another chain";
                return false;
            }
            std::uint32_t colliders = 0;
            const std::vector<MeshPtr> meshes = collideMeshes(graph, id, &colliders);
            // The fields the CPU's Collide asks at this offset (collisionField: one for every
            // offset of the same cell size and reach), their tables made once for every offset
            // sharing them (a dragged offset is the kernel's number): for offsets up to the most of
            // those plus the softness, at the field's reach, more than any of their pushes asks of
            // the tables (a push the tables' bounds settle at that offset and radius is settled at
            // a smaller one: the walk's answers are the same).
            const float offset = p->value[0], softness = p->value2[0];
            DistanceFieldPtr surfaceFields[2];
            for (std::size_t m = 0; m < meshes.size() && m < 2; ++m) {
                surfaceFields[m] = collisionField(meshes[m], offset, p->value[2]);
                if (!surfaceFields[m]) {
                    *why = name + ": a surface has no field the GPU can read (deformed: the CPU's)";
                    return false;
                }
            }
            CollideMemo& memo = collide_[id];
            bool same = memo.inputs && memo.softness == softness;
            for (std::size_t m = 0; m < 2; ++m) {
                same = same && memo.meshes[m] == (m < meshes.size() ? meshes[m] : nullptr) && memo.fields[m] == surfaceFields[m] &&
                       (!surfaceFields[m] || offset + softness <= memo.most[m]);
            }
            if (!same) {
                auto made = std::make_shared<CollideInputs>();
                float most[2] = {0.0f, 0.0f};
                for (std::size_t m = 0; m < meshes.size() && m < 2; ++m) {
                    // cellSizeFor rounds the offset's log2 to the nearest whole number: the offsets of
                    // a cell are under sqrt 2 times it (any past the largest cell it takes are the
                    // largest's), and those of a reach are within it.
                    const MeshDistanceField& field = *surfaceFields[m];
                    float upTo = field.reach();
                    if (MeshDistanceField::cellSizeFor(*meshes[m], std::numeric_limits<float>::max()) > field.cellSize()) {
                        upTo = std::min(upTo, field.cellSize() * 1.4143f + softness);
                    }
                    most[m] = std::max(offset + softness, upTo);
                }
                for (std::size_t m = 0; m < meshes.size() && m < 2; ++m) {
                    const MeshPtr& mesh = meshes[m];
                    MeshDistanceField::Tables tables = surfaceFields[m]->tables(most[m], surfaceFields[m]->reach());
                    if (!tables) {
                        *why = name + ": a surface has no field the GPU can read (too big for its grid: the CPU's)";
                        return false;
                    }
                    made->tables.push_back(std::move(tables));
                    made->colliders = colliders;
                    Vec3f lo = mesh->points().empty() ? Vec3f{} : mesh->points().front(), hi = lo;
                    for (const Vec3f& q : mesh->points()) {
                        lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
                        hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
                    }
                    made->boxes.push_back(lo);
                    made->boxes.push_back(hi);
                }
                memo = CollideMemo{};
                for (std::size_t m = 0; m < meshes.size() && m < 2; ++m) {
                    memo.meshes[m] = meshes[m];
                    memo.fields[m] = surfaceFields[m];
                    memo.most[m] = most[m];
                }
                memo.softness = softness;
                memo.inputs = std::move(made);
            }
            out[k].collide = memo.inputs;
        }
        if (stage.kind == StageKind::Tie && !readTie(doc, chain, k, groomScalp, scalpMesh, &out[k], why)) return false;
        // ---- The shapers working on each strand alone ----
        if (stage.kind == StageKind::Bend || stage.kind == StageKind::Direction || stage.kind == StageKind::Transform ||
            stage.kind == StageKind::Displace) {
            if (!shaperRoots(doc, id, groomScalp, &out[k], why)) return false;
        }
        if (stage.kind == StageKind::Twist) {
            std::set<NodeId> stands;
            for (const auto& m : standIn) stands.insert(m.first);
            if (!twistAxes(doc, id, stands, &out[k], why)) return false;
        }
        if ((stage.kind == StageKind::RestoreLength || stage.kind == StageKind::BlendNeighbors || stage.kind == StageKind::Tangle ||
             stage.kind == StageKind::Flow) &&
            !readShaper(doc, chain, k, before, rebound, groomScalp, scalpMesh, &out[k], why)) {
            return false;
        }
        // ---- Jiggle and MeshCut ----
        if (stage.kind == StageKind::Jiggle) {
            if (k >= params.stages.size()) {
                *why = name + ": its params were read for another chain";
                return false;
            }
            if (!readJiggle(doc, id, params.stages[k], &out[k], why)) return false;
        }
        if (stage.kind == StageKind::MeshCut && !readMeshCut(doc, id, &out[k], why)) return false;
        // ---- A Merge's input b moved by the drag: b's own run ----
        if (stage.kind == StageKind::Merge && k < params.stages.size() && params.stages[k].branch &&
            !branchRun(doc, id, *params.stages[k].branch, parts, groomScalp, changed, standIn, &out[k].merged, why)) {
            return false;
        }
        // ---- A Cull, Split or Mirror: its layout, and the run up its input ----
        if (gathers(stage.kind)) {
            if (!readGather(doc, id, id == chain.base ? chain.baseOutput : 0, groomScalp, &out[k], why)) return false;
            const LookBranch* branch = k < params.stages.size() ? params.stages[k].branch.get() : nullptr;
            if (branch) {
                if (!branchRun(doc, id, *branch, parts, groomScalp, changed, standIn, &out[k].merged, why)) return false;
                // The channels its run's Interpolate blends anew are copied as cached: a value read
                // from them after it would not hold.
                if (branch->chain.node(StageKind::Interpolate) && fields) {
                    for (std::size_t j = 0; j < fields->stages.size(); ++j) {
                        const StageFields& f = fields->stages[j];
                        for (const FieldValues* v : {&f.field[0], &f.field[1], &f.affect}) {
                            if (v->binding || (v->program && v->program->influence)) {
                                *why = doc.nameOf(chain.stages[j].node) + " reads what Interpolate blends before " + name + " (the CPU's)";
                                return false;
                            }
                        }
                    }
                }
            }
        }
        before.insert(id);
    }
    // Mapped params reading channels Interpolate blends from its binding: they hold while it is
    // the one its cached result was made with.
    if (rebound && fields) {
        for (std::size_t k = 0; k < fields->stages.size() && k < chain.stages.size(); ++k) {
            const StageFields& f = fields->stages[k];
            for (const FieldValues* v : {&f.field[0], &f.field[1], &f.affect}) {
                if (v->binding) {
                    *why = doc.nameOf(chain.stages[k].node) + " reads what Interpolate blends from guides bound again (the CPU's)";
                    return false;
                }
            }
        }
    }
    *inputs = std::move(out);
    readMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return true;
}

// ---- What Tie takes ------------------------------------------------------------------------------

namespace {

// The field's tables with each vertex's triangles joined with those of the vertices at its position
// (within a millionth of the mesh's size: a seam's split vertices, a UV seam, a sphere's first and
// last meridian, its poles), in triangle order, after the rest (the header pointing at them): the
// walks down the mesh to a closest point cross the seam, as the CPU's closest point (the mesh's
// tree) knows no seams. The same tables where no two vertices are that near.
MeshDistanceField::Tables weldedRings(const MeshDistanceField::Tables& tables, const ScalpMesh& mesh) {
    const std::vector<Vec3f>& points = mesh.points();
    const std::size_t count = points.size();
    if (count == 0) return tables;
    Vec3f lo = points.front(), hi = lo;
    for (const Vec3f& q : points) {
        lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
        hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
    }
    const float within = std::max((hi - lo).length() * 1e-6f, 1e-30f);
    // Each vertex's group (its lowest member), the near ones joined (by cells `within` wide).
    std::vector<std::uint32_t> group(count);
    for (std::size_t v = 0; v < count; ++v) group[v] = static_cast<std::uint32_t>(v);
    std::function<std::uint32_t(std::uint32_t)> root = [&](std::uint32_t v) { return group[v] == v ? v : group[v] = root(group[v]); };
    std::map<std::array<std::int64_t, 3>, std::vector<std::uint32_t>> cells;
    auto cellOf = [&](const Vec3f& q) {
        return std::array<std::int64_t, 3>{static_cast<std::int64_t>(std::floor((q.x - lo.x) / within)),
                                           static_cast<std::int64_t>(std::floor((q.y - lo.y) / within)),
                                           static_cast<std::int64_t>(std::floor((q.z - lo.z) / within))};
    };
    bool shared = false;
    for (std::size_t v = 0; v < count; ++v) {
        const std::array<std::int64_t, 3> c = cellOf(points[v]);
        for (std::int64_t dx = -1; dx <= 1; ++dx) {
            for (std::int64_t dy = -1; dy <= 1; ++dy) {
                for (std::int64_t dz = -1; dz <= 1; ++dz) {
                    const auto it = cells.find({c[0] + dx, c[1] + dy, c[2] + dz});
                    if (it == cells.end()) continue;
                    for (const std::uint32_t u : it->second) {
                        if (!((points[u] - points[v]).length() <= within)) continue;
                        const std::uint32_t a = root(u), b = root(static_cast<std::uint32_t>(v));
                        if (a != b) group[std::max(a, b)] = std::min(a, b);
                        shared = true;
                    }
                }
            }
        }
        cells[c].push_back(static_cast<std::uint32_t>(v));
    }
    if (!shared) return tables;
    std::vector<std::vector<std::uint32_t>> members(count);
    for (std::size_t v = 0; v < count; ++v) members[root(static_cast<std::uint32_t>(v))].push_back(static_cast<std::uint32_t>(v));
    const std::vector<std::uint32_t>& w = *tables;
    const std::uint32_t starts = w[MeshDistanceField::kTableRingStart], rings = w[MeshDistanceField::kTableRings];
    auto words = std::make_shared<std::vector<std::uint32_t>>(w);
    std::vector<std::uint32_t> start(count + 1, 0), faces;
    for (std::size_t v = 0; v < count; ++v) {
        start[v] = static_cast<std::uint32_t>(faces.size());
        std::vector<std::uint32_t> joined;
        for (const std::uint32_t u : members[root(static_cast<std::uint32_t>(v))]) {
            joined.insert(joined.end(), w.begin() + rings + w[starts + u], w.begin() + rings + w[starts + u + 1]);
        }
        std::sort(joined.begin(), joined.end());
        joined.erase(std::unique(joined.begin(), joined.end()), joined.end());
        faces.insert(faces.end(), joined.begin(), joined.end());
    }
    start[count] = static_cast<std::uint32_t>(faces.size());
    (*words)[MeshDistanceField::kTableRingStart] = static_cast<std::uint32_t>(words->size());
    words->insert(words->end(), start.begin(), start.end());
    (*words)[MeshDistanceField::kTableRings] = static_cast<std::uint32_t>(words->size());
    words->insert(words->end(), faces.begin(), faces.end());
    return words;
}

}  // namespace

bool LookInputs::readTie(const GraphDocument& doc, const LookChain& chain, std::size_t k, const ScalpMesh* groomScalp,
                         const MeshPtr& scalpMesh, StageInputs* out, std::string* why) {
    const Graph& graph = doc.graph;
    const NodeId id = chain.stages[k].node;
    const std::string name = doc.nameOf(id);
    const StrandsPtr in = inputStrands(graph, id);
    if (!in) {
        *why = "not evaluated";
        return false;
    }
    if (groomScalp && (!scalpMesh || scalpMesh.get() != groomScalp)) {
        *why = name + ": the groom's scalp is not at hand";
        return false;
    }
    // The scalp TieOp takes (the "scalp" external). Deformed away from its rest pose, its push
    // asks the rest pose's field through each vertex's frame (RestPoseCollision): the CPU's.
    const MeshPtr scalp = scalpMesh && scalpMesh->triangleCount() > 0 ? scalpMesh : nullptr;
    if (scalp && scalp->hasRest() && scalp->restPoints() != scalp->points()) {
        *why = name + ": its scalp is deformed away from its rest pose (the CPU's)";
        return false;
    }
    const std::string channel = graph.node(id).params().getString("tailChannel");
    TieMemo& memo = tie_[id];
    if (memo.in != in || memo.scalp != scalp || memo.channel != channel || !memo.inputs) {
        auto made = std::make_shared<TieInputs>();
        if (scalp) {
            // TieOp's scalp: its box's center and size, the clearance, depth and search radius of
            // its push, and the field it pushes through (CollisionSurface::of: collisionField).
            Vec3f lo = scalp->points().front(), hi = lo;
            for (const Vec3f& q : scalp->points()) {
                lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
                hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
            }
            made->hasScalp = true;
            made->scalpSize = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
            made->scalpCenter = (lo + hi) * 0.5f;
            made->clearance = made->scalpSize * (1.0f / 400.0f);  // (tie_op.cpp kClearanceShare, kDepthShare)
            made->depth = made->scalpSize * (1.0f / 8.0f);
            made->searchRadius = std::max(made->clearance, made->depth) * 1.001f + 1e-6f;
            if (made->clearance > 0.0f && (memo.tablesScalp != scalp || !memo.tables)) {
                const DistanceFieldPtr field = collisionField(scalp, made->clearance, made->searchRadius);
                memo.tables = field ? field->tables(made->clearance, made->searchRadius) : nullptr;
                if (memo.tables) memo.tables = weldedRings(memo.tables, *scalp);
                memo.tablesScalp = scalp;
            }
            if (made->clearance > 0.0f) made->tables = memo.tables;
            // The routes' closest points are found down the mesh the tables hold.
            if (!made->tables) {
                *why = name + ": its scalp has no field the GPU can read (too big for its grid: the CPU's)";
                return false;
            }
        }
        // Each strand's start as TieOp finds it (after an earlier Tie's tail start), and the face
        // nearest it: its root's where it is rooted on the scalp, else the scalp's closest point's.
        const std::vector<float>* earlier = channel.empty() ? nullptr : in->pointChannel(channel);
        if (earlier && earlier->size() != in->pointCount()) earlier = nullptr;
        auto seeds = std::make_shared<std::vector<std::uint32_t>>(in->strandCount(), 0xFFFFFFFFu);
        if (scalp) {
            parallelFor(0, in->strandCount(), [&](std::size_t s) {
                const std::size_t n = in->strandPointCount(s);
                if (n < 2) return;
                std::size_t start = 0;
                if (earlier) {
                    const float* t = earlier->data() + in->strandBegin(s);
                    for (std::size_t i = 1; i < n; ++i) {
                        if (t[i] >= 0.5f) {
                            start = i;
                            break;
                        }
                    }
                }
                const StrandRoot& root = in->roots()[s];
                if (start == 0 && scalp->contains(root)) {
                    (*seeds)[s] = static_cast<std::uint32_t>(root.face);
                    return;
                }
                const SurfacePoint sp = scalp->closestPoint(in->strandPoints(s)[start]);
                if (sp.root.face >= 0) (*seeds)[s] = static_cast<std::uint32_t>(sp.root.face);
            }, 1024);
        }
        made->seeds = std::move(seeds);
        auto core = std::make_shared<std::vector<std::uint32_t>>();
        const PreviewCore preview(*in);
        for (std::size_t s = 0; s < in->strandCount(); ++s) {
            if (preview.core(s)) core->push_back(static_cast<std::uint32_t>(s));
        }
        made->core = std::move(core);
        if (earlier) made->channel = std::shared_ptr<const std::vector<float>>(in, earlier);  // (holding the strands)
        memo.in = in;
        memo.scalp = scalp;
        memo.channel = channel;
        memo.inputs = std::move(made);
    }
    // The tail channel it takes: an earlier Tie of the run writing it (its GPU's), else as the
    // strands it took carry it; another node of the run writing it: the CPU's.
    std::size_t from = kNoStage;
    if (!channel.empty()) {
        for (std::size_t j = 0; j < k; ++j) {
            const NodeId jd = chain.stages[j].node;
            const StrandsPtr jin = inputStrands(graph, jd), jout = cachedStrands(graph, jd);
            if (!jin || !jout) {
                *why = "not evaluated";
                return false;
            }
            if (jout->pointChannel(channel) == jin->pointChannel(channel)) continue;
            if (chain.stages[j].kind != StageKind::Tie || graph.node(jd).params().getString("tailChannel") != channel) {
                *why = name + " reads the '" + channel + "' channel " + doc.nameOf(jd) + " before it writes (the CPU's)";
                return false;
            }
            from = j;
        }
        if (from != kNoStage && in->pointChannel(channel) != stageResult(graph, chain, from)->pointChannel(channel)) {
            *why = name + " reads a '" + channel + "' channel a node after " + doc.nameOf(chain.stages[from].node) + " writes (the CPU's)";
            return false;
        }
    }
    if (from == memo.inputs->channelFrom) {
        out->tie = memo.inputs;
    } else {
        auto with = std::make_shared<TieInputs>(*memo.inputs);
        with->channelFrom = from;
        out->tie = std::move(with);
    }
    return true;
}

// ---- What the shapers working on each strand alone take ----------------------------------------

bool LookInputs::shaperRoots(const GraphDocument& doc, NodeId id, const ScalpMesh* scalp, StageInputs* out, std::string* why) {
    const StrandsPtr in = inputStrands(doc.graph, id);
    if (!in) {
        *why = "not evaluated";
        return false;
    }
    RootMemo& memo = roots_[id];
    if (memo.in != in || memo.scalp != scalp || !memo.inputs) {
        // Another such node's, for the very roots (a run's strands share them) on this scalp.
        std::shared_ptr<const RootInputs> same;
        for (const auto& [node, m] : roots_) {
            if (m.inputs && m.in && m.scalp == scalp && &m.in->roots() == &in->roots()) same = m.inputs;
        }
        if (!same) same = rootInputs(*in, scalp);
        memo.in = in;
        memo.scalp = scalp;
        memo.inputs = std::move(same);
    }
    out->roots = memo.inputs;
    return true;
}

bool LookInputs::twistAxes(const GraphDocument& doc, NodeId id, const std::set<NodeId>& standIn, StageInputs* out, std::string* why) {
    const Graph& graph = doc.graph;
    const std::string name = doc.nameOf(id);
    const StrandsPtr in = inputStrands(graph, id);
    if (!in) {
        *why = "not evaluated";
        return false;
    }
    NodeId axesNode = 0;
    std::size_t axesOutput = 0;
    const bool plugged = graph.inputSource(id, 1, &axesNode, &axesOutput);
    // Its axes as cached (a stroke's guides as its dab made them); guides standing in for a node's
    // cached result (a dragged Sculpt node's) are not.
    if (plugged && dependsOn(graph, axesNode, standIn)) {
        *why = name + "'s axes are guides made for the run (the CPU's)";
        return false;
    }
    const GuidesPtr axesIn = plugged ? valueAs<GuidesPtr>(graph.cachedValue(axesNode, axesOutput)) : nullptr;
    const bool aroundAxes = axesIn && !axesIn->empty();
    const std::string channel = graph.node(id).params().getString("clumpChannel");
    TwistMemo& memo = twist_[id];
    // New axes alone (a stroke's dab on the guides they are): each strand's nearest found on the GPU
    // (its root words other than any root), the CPU's search for every strand spared.
    const bool axesAlone = memo.inputs && memo.in == in && memo.channel == channel && memo.axes != axesIn && aroundAxes &&
                           memo.inputs->axes;
    if (memo.in != in || memo.axes != axesIn || memo.channel != channel || !memo.inputs) {
        std::string whyNot;
        std::shared_ptr<TwistInputs> made = twistInputs(*in, axesIn.get(), channel, axesAlone, &whyNot);
        if (!made) {
            *why = name + ": " + whyNot;
            return false;
        }
        memo.in = in;
        memo.axes = axesIn;
        memo.channel = channel;
        memo.inputs = std::move(made);
    }
    out->twist = memo.inputs;
    return true;
}


// ---- What Jiggle and MeshCut take ----------------------------------------------------------------

bool LookInputs::readJiggle(const GraphDocument& doc, NodeId id, const StageParams& p, StageInputs* out, std::string* why) {
    const Graph& graph = doc.graph;
    const std::string name = doc.nameOf(id);
    const StrandsPtr in = inputStrands(graph, id);
    JiggleState state;
    if (!in || !jiggleState(graph.node(id), cachedStrands(graph, id), &state) || !state.scalp || !state.history) {
        *why = name + " keeps no scalp motion for its result";
        return false;
    }
    double frequency = 0.0, damping = 0.0;
    std::memcpy(&frequency, &p.more[0], sizeof frequency);
    std::memcpy(&damping, &p.more[2], sizeof damping);
    JiggleMemo& memo = jiggle_[id];
    if (memo.in != in || memo.scalp != state.scalp || memo.history != state.history || memo.nowFrame != state.nowFrame ||
        memo.frequency != frequency || memo.damping != damping || memo.frames != p.flag[0] || !memo.inputs) {
        // The lags at these params (JiggleOp's: the frames read, the springs' weights, each vertex's
        // and face's drag), for the faces the strands grow from.
        ParamSet params = evaluatedParams(graph, id);
        params.set("frequency", frequency);
        params.set("damping", damping);
        params.set("history", static_cast<std::int64_t>(p.flag[0]));
        const ScalpMesh& scalp = *state.scalp;
        const auto& tris = scalp.triangles();
        std::vector<char> used(tris.size(), 0);
        for (const StrandRoot& root : in->roots()) {
            if (scalp.contains(root)) used[static_cast<std::size_t>(root.face)] = 1;
        }
        JiggleLags lags;
        jiggleLags(params, state, used, &lags);
        auto strands = std::make_shared<std::vector<float>>(in->strandCount() * 4, 0.0f);
        parallelFor(0, in->strandCount(), [&](std::size_t s) {
            float* to = strands->data() + s * 4;
            std::uint32_t face = 0xFFFFFFFFu;
            const StrandRoot& root = in->roots()[s];
            if (in->strandPointCount(s) >= 2 && scalp.contains(root) && !lags.vertexLag.empty()) {
                const auto& t = tris[static_cast<std::size_t>(root.face)];
                const float wa = 1.0f - root.u - root.v;
                const auto a = static_cast<std::size_t>(t[0]), b = static_cast<std::size_t>(t[1]), c = static_cast<std::size_t>(t[2]);
                const Vec3f lag = lags.vertexLag[a] * wa + lags.vertexLag[b] * root.u + lags.vertexLag[c] * root.v;  // (as JiggleOp blends it)
                to[0] = lag.x;
                to[1] = lag.y;
                to[2] = lag.z;
                face = static_cast<std::uint32_t>(root.face);
            }
            std::memcpy(&to[3], &face, sizeof face);
        }, 4096);
        auto faces = std::make_shared<std::vector<float>>(std::max<std::size_t>(tris.size(), 1) * 12, 0.0f);
        for (std::size_t f = 0; f < tris.size() && f < lags.faceTurned.size(); ++f) {
            if (!lags.faceTurned[f]) continue;
            float* to = faces->data() + f * 12;
            for (int r = 0; r < 3; ++r) {
                for (int col = 0; col < 3; ++col) to[r * 4 + col] = static_cast<float>(lags.faceTurn[f][static_cast<std::size_t>(r * 3 + col)]);
            }
            to[3] = 1.0f;
        }
        auto made = std::make_shared<JiggleInputs>();
        made->strands = std::move(strands);
        made->faces = std::move(faces);
        memo.in = in;
        memo.scalp = state.scalp;
        memo.history = state.history;
        memo.nowFrame = state.nowFrame;
        memo.frequency = frequency;
        memo.damping = damping;
        memo.frames = p.flag[0];
        memo.inputs = std::move(made);
    }
    out->jiggle = memo.inputs;
    return true;
}

bool LookInputs::readMeshCut(const GraphDocument& doc, NodeId id, StageInputs* out, std::string* why) {
    (void)why;
    const Graph& graph = doc.graph;
    NodeId src = 0;
    std::size_t output = 0;
    const MeshPtr mesh = graph.inputSource(id, 1, &src, &output) ? valueAs<MeshPtr>(graph.cachedValue(src, output)) : nullptr;
    MeshCutMemo& memo = meshCut_[id];
    if (memo.mesh != mesh || !memo.inputs) {
        auto made = std::make_shared<MeshCutInputs>();
        if (mesh && mesh->triangleCount() > 0) made->tree = mesh->rayTree();
        memo.mesh = mesh;
        memo.inputs = std::move(made);
    }
    out->meshCut = memo.inputs;
    return true;
}

// ---- Cull, Split, Mirror: their layout ----------------------------------------------------------

bool LookInputs::readGather(const GraphDocument& doc, NodeId id, std::size_t output, const ScalpMesh* groomScalp, StageInputs* out,
                            std::string* why) {
    const Graph& graph = doc.graph;
    const std::string name = doc.nameOf(id);
    // (As made: hidden core strands in both, preview_core.h, as in the strands the runs hold.)
    const StrandsPtr in = inputStrands(graph, id), made = valueAs<StrandsPtr>(graph.cachedValue(id, output));
    if (!in || !made) {
        *why = "not evaluated";
        return false;
    }
    const bool mirror = graph.node(id).typeName() == op::kMirror;
    GatherMemo& memo = gather_[id];
    if (memo.inputs && memo.in == in && memo.out == made && (!mirror || memo.scalp == groomScalp)) {
        out->gather = memo.inputs;
        return true;
    }
    auto inputs = std::make_shared<GatherInputs>();
    auto words = std::make_shared<std::vector<std::uint32_t>>(made->strandCount() * 8, 0u);
    // Result strand `to` copied from strand `from` of the input, as it came or mirrored (its first
    // point checked against the result's).
    auto put = [&](std::size_t to, std::size_t from, bool mirrored, bool moved, const Vec3f& onto) {
        const std::size_t n = in->strandPointCount(from);
        if (n != made->strandPointCount(to)) return false;
        Vec3f first = n ? in->strandPoints(from)[0] : Vec3f{};
        if (mirrored) first = mirrorPoint(first, inputs->axis, inputs->offset);
        if (moved) first += onto;
        if (n && std::memcmp(&first, &made->strandPoints(to)[0], sizeof(Vec3f)) != 0) return false;
        std::uint32_t* w = &(*words)[to * 8];
        w[0] = static_cast<std::uint32_t>(in->strandBegin(from));
        w[1] = (mirrored ? 1u : 0u) | (moved ? 2u : 0u);
        std::memcpy(&w[4], &onto, sizeof(Vec3f));
        return true;
    };
    // The strands it kept, the first of its result in the input's order (by ID: Cull and Split keep
    // their order, Mirror its kept side's before the mirrored ones).
    std::unordered_map<StrandId, std::uint32_t> at;
    at.reserve(in->strandCount());
    for (std::size_t s = 0; s < in->strandCount(); ++s) at.emplace(in->ids()[s], static_cast<std::uint32_t>(s));
    std::vector<std::uint32_t> kept;
    std::size_t j = 0;
    bool laidOut = true;
    for (; j < made->strandCount() && laidOut; ++j) {
        const auto found = at.find(made->ids()[j]);
        if (found == at.end() || (!kept.empty() && found->second <= kept.back())) break;
        kept.push_back(found->second);
        laidOut = put(j, found->second, false, false, {});
    }
    if (mirror && laidOut) {
        // Then a mirrored copy of each kept strand off the plane, in their order, under an ID
        // derived from its own (MirrorOp: hashCombine with its salt; one made again for a clash with
        // another is not found here: the CPU's), moved onto the scalp where its root lands.
        const ParamSet& p = graph.node(id).params();
        const std::string axis = p.getString("axis");
        inputs->axis = axis == "y" ? 1 : axis == "z" ? 2 : 0;
        inputs->offset = static_cast<float>(p.getDouble("offset"));
        const bool reattach = p.getBool("reattach") && groomScalp && groomScalp->triangleCount() > 0;
        const std::uint64_t salt = 0x4D1AAC0Full + static_cast<std::uint64_t>(inputs->axis);
        for (const std::uint32_t s : kept) {
            if (j >= made->strandCount() || !laidOut) break;
            if (made->ids()[j] != hashCombine(in->ids()[s], salt)) continue;  // on the plane, or no points: kept once
            Vec3f onto;
            bool moved = false;
            if (reattach) {
                const Vec3f root = mirrorPoint(in->strandPoints(s)[0], inputs->axis, inputs->offset);
                const SurfacePoint lands = groomScalp->closestPoint(root);
                if (lands.root.face >= 0) {
                    onto = lands.position - root;
                    moved = true;
                    inputs->reach = std::max(inputs->reach, onto.length());
                }
            }
            laidOut = put(j, s, true, moved, onto);
            ++j;
        }
    }
    if (!laidOut || j != made->strandCount()) {
        *why = name + "'s result holds strands it did not take as they came (the CPU's)";
        return false;
    }
    inputs->strands = std::move(words);
    memo.in = in;
    memo.out = made;
    memo.scalp = groomScalp;
    memo.inputs = inputs;
    out->gather = std::move(inputs);
    return true;
}

// ---- What the shapers reading their neighbours or a reference take -----------------------------

bool LookInputs::readShaper(const GraphDocument& doc, const LookChain& chain, std::size_t k, const std::set<NodeId>& before, bool rebound,
                            const ScalpMesh* groomScalp, const MeshPtr& scalpMesh, StageInputs* out, std::string* why) {
    const Graph& graph = doc.graph;
    const LookStage& stage = chain.stages[k];
    const NodeId id = stage.node;
    const std::string name = doc.nameOf(id);
    const ParamSet& own = graph.node(id).params();
    const StrandsPtr in = inputStrands(graph, id);
    if (!in) {
        *why = "not evaluated";
        return false;
    }
    // A strand channel whom a strand reads goes by (clump numbers, regions) holds through the run
    // where no stage before it writes it but a Clump (its choice the CPU's, fixed) or an Interpolate
    // that is not bound again.
    auto channelHolds = [&](const std::string& channel) {
        if (channel.empty()) return true;
        for (std::size_t j = 0; j < k; ++j) {
            const NodeId jd = chain.stages[j].node;
            const StrandsPtr jin = inputStrands(graph, jd), jout = cachedStrands(graph, jd);
            if (!jin || !jout) return false;
            if (jout->strandChannel(channel) == jin->strandChannel(channel)) continue;
            const StageKind kind = chain.stages[j].kind;
            if (kind == StageKind::Clump || (kind == StageKind::Interpolate && !rebound)) continue;
            return false;
        }
        return true;
    };
    ShaperMemo& memo = shaper_[id];
    switch (stage.kind) {
        case StageKind::RestoreLength: {
            const std::string mode = own.getString("mode");
            if (mode == "even") return true;  // (its own points alone)
            if (stage.from2 != kNoStage) {
                // Made by a stage of the run: the same strands (IDs, layout) as it takes, read on the GPU.
                const StrandsPtr ref = stageResult(graph, chain, stage.from2);
                if (!ref || ref->ids() != in->ids() || ref->offsets() != in->offsets()) {
                    *why = name + "'s reference is other strands than it takes (the CPU's)";
                    return false;
                }
                for (std::size_t s = 0; s < ref->strandCount(); ++s) {
                    out->reach = std::max(out->reach, arcLength(ref->strandPoints(s), ref->strandPointCount(s)));
                }
                return true;
            }
            NodeId src = 0;
            std::size_t output = 0;
            if (!graph.inputSource(id, 1, &src, &output)) return true;  // (handed on: not a stage)
            if (dependsOn(graph, src, before)) {
                *why = name + "'s reference is made from what the run moves, through nodes the GPU does not run (the CPU's)";
                return false;
            }
            const Value value = graph.cachedValue(src, output);
            const StrandsPtr refStrands = valueAs<StrandsPtr>(value);
            const GuidesPtr refGuides = refStrands ? nullptr : valueAs<GuidesPtr>(value);
            const void* made = refStrands ? static_cast<const void*>(refStrands.get()) : static_cast<const void*>(refGuides.get());
            if (!made) {
                *why = name + "'s reference is not evaluated";
                return false;
            }
            if (memo.in != in || memo.made != made || !memo.restore) {
                // The reference curves by ID (RestoreLengthOp: strands, or guides), each at the
                // strand's point count as the node resamples it.
                struct Ref {
                    const Vec3f* points;
                    std::size_t count;
                };
                std::unordered_map<StrandId, Ref> reference;
                float longest = 0.0f;
                if (refStrands) {
                    for (std::size_t s = 0; s < refStrands->strandCount(); ++s) {
                        if (refStrands->strandPointCount(s) >= 2) reference[refStrands->ids()[s]] = {refStrands->strandPoints(s), refStrands->strandPointCount(s)};
                    }
                } else {
                    for (const Guide& g : refGuides->guides()) {
                        if (g.points.size() >= 2) reference[g.id] = {g.points.data(), g.points.size()};
                    }
                }
                auto lengths = std::make_shared<std::vector<float>>(in->pointCount(), 0.0f);
                auto firsts = std::make_shared<std::vector<float>>(in->strandCount() * 4, 0.0f);
                std::vector<float> reaches(in->strandCount(), 0.0f);
                parallelFor(0, in->strandCount(), [&](std::size_t s) {
                    const std::size_t n = in->strandPointCount(s), begin = in->strandBegin(s);
                    if (n == 0) return;
                    (*lengths)[begin] = -1.0f;
                    if (n < 2) return;
                    const auto it = reference.find(in->ids()[s]);
                    if (it == reference.end()) return;
                    const Ref& ref = it->second;
                    thread_local std::vector<Vec3f> sampled;
                    const Vec3f* pts = ref.points;
                    if (ref.count != n) {
                        sampled.resize(n);
                        resampleByArcLengthInto(ref.points, ref.count, sampled.data(), n);
                        pts = sampled.data();
                    }
                    const float whole = arcLength(ref.points, ref.count);
                    (*lengths)[begin] = whole;
                    reaches[s] = whole;
                    for (std::size_t i = 1; i < n; ++i) (*lengths)[begin + i] = (pts[i] - pts[i - 1]).length();
                    const Vec3f first = pts[1] - pts[0];
                    float* f = firsts->data() + s * 4;
                    f[0] = first.x;
                    f[1] = first.y;
                    f[2] = first.z;
                }, 1024);
                for (const float r : reaches) longest = std::max(longest, r);
                auto made2 = std::make_shared<RestoreInputs>();
                made2->lengths = std::move(lengths);
                made2->firsts = std::move(firsts);
                memo.in = in;
                memo.made = made;
                memo.keep = refStrands ? std::shared_ptr<const void>(refStrands) : std::shared_ptr<const void>(refGuides);
                memo.restore = std::move(made2);
                memo.reach = longest;
            }
            out->restore = memo.restore;
            out->reach = std::max(out->reach, memo.reach);
            return true;
        }
        case StageKind::BlendNeighbors: {
            std::shared_ptr<const BlendNeighborsState> state;
            if (!blendNeighborsState(graph.node(id), cachedStrands(graph, id), &state) || state->offsets.size() != in->strandCount() + 1 ||
                state->normals.size() != in->strandCount()) {
                *why = name + " keeps no neighbours for its result";
                return false;
            }
            if (!channelHolds(own.getString("regionChannel"))) {
                *why = name + " blends within regions the run writes anew (the CPU's)";
                return false;
            }
            out->neighbors = std::move(state);
            return true;
        }
        case StageKind::Tangle: {
            std::shared_ptr<const TangleState> state;
            if (!tangleState(graph.node(id), cachedStrands(graph, id), &state) || state->partners.size() != in->strandCount() ||
                state->draws.size() != in->strandCount()) {
                *why = name + " keeps no partners for its result";
                return false;
            }
            if (!channelHolds(own.getBool("sameClump") ? std::string(kClumpIdChannel) : std::string()) ||
                !channelHolds(own.getString("regionChannel"))) {
                *why = name + " picks its partners by clumps or regions the run writes anew (the CPU's)";
                return false;
            }
            // Its fraction picks which strands stick: on the strands the run hands it, where the
            // values it reads hold (the roots, IDs, channels the run does not write anew).
            const ParamSet params = evaluatedParams(graph, id);
            if (k > 0 && (ownMap(params, "fraction") || params.field("fraction"))) {
                bool holds = false;
                try {
                    const ScalarField fraction(params, "fraction", groomScalp);
                    fraction.validate(*in);
                    const FieldRead r = whatItReads(graph, chain, k, fraction);
                    holds = !r.unknown && !r.layer && !r.positions && !r.influence && (!r.binding || !rebound);
                } catch (const std::exception&) {
                    holds = false;
                }
                if (!holds) {
                    *why = name + " picks the strands that stick by a fraction the run changes (the CPU's)";
                    return false;
                }
            }
            if (memo.in != in || memo.made != state.get() || !memo.tangle) {
                std::shared_ptr<const TangleInputs> made = tangleInputs(*in, *state);
                memo.in = in;
                memo.made = state.get();
                memo.keep = state;
                memo.tangle = std::move(made);
            }
            out->tangle = memo.tangle;
            return true;
        }
        case StageKind::Flow: {
            FlowState state;
            if (!flowState(graph.node(id), cachedStrands(graph, id), &state)) {
                *why = name + " keeps no flow for its result";
                return false;
            }
            // The scalp it traced the strands over (its "scalp" external, the groom's), as Tie
            // finds closest points on it: still in its rest pose.
            const ScalpMesh* traced = state.scalp && state.scalp->triangleCount() > 0 ? state.scalp : nullptr;
            if (traced && (!scalpMesh || scalpMesh.get() != traced)) {
                *why = name + ": the scalp it lays its strands over is not at hand";
                return false;
            }
            if (traced && traced->hasRest() && traced->restPoints() != traced->points()) {
                *why = name + ": its scalp is deformed away from its rest pose (the CPU's)";
                return false;
            }
            const FlowField* field = state.flow.get();
            constexpr std::size_t kMaxPins = 4096, kMaxSinks = 256;  // (look.slang FlowCS asks every one where it asks the field)
            if (field && (field->positions().size() > kMaxPins || field->sinkPositions().size() > kMaxSinks)) {
                *why = name + ": its flow has too many arrows for the GPU (the CPU's)";
                return false;
            }
            if (memo.in != in || memo.made != field || memo.tablesScalp != (traced ? scalpMesh : nullptr) || !memo.flow) {
                auto made = std::make_shared<FlowInputs>();
                made->arrows = field != nullptr;
                if (traced) {
                    // Tie's tables of this scalp (readTie: its box's size, the clearance and search
                    // radius of its push): their mesh is what the closest points are found down.
                    if (memo.tablesScalp != scalpMesh || !memo.tables) {
                        Vec3f lo = traced->points().front(), hi = lo;
                        for (const Vec3f& q : traced->points()) {
                            lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
                            hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
                        }
                        const float size = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
                        const float clearance = size * (1.0f / 400.0f), depth = size * (1.0f / 8.0f);
                        const float searchRadius = std::max(clearance, depth) * 1.001f + 1e-6f;
                        const DistanceFieldPtr df = clearance > 0.0f ? collisionField(scalpMesh, clearance, searchRadius) : nullptr;
                        memo.tables = df ? df->tables(clearance, searchRadius) : nullptr;
                        if (memo.tables) memo.tables = weldedRings(memo.tables, *traced);
                        memo.tablesScalp = scalpMesh;
                    }
                    if (!memo.tables) {
                        *why = name + ": its scalp has no field the GPU can read (too big for its grid: the CPU's)";
                        return false;
                    }
                    made->tables = memo.tables;
                }
                // Each strand's root on the scalp, its normal and the field there (FlowOp's first
                // heading), as the CPU reads them.
                const std::size_t count = in->strandCount();
                auto strands = std::make_shared<std::vector<float>>(count * 12, 0.0f);
                parallelFor(0, count, [&](std::size_t s) {
                    float* to = strands->data() + s * 12;
                    const StrandRoot& root = in->roots()[s];
                    const bool onScalp = traced && traced->contains(root);
                    const std::uint32_t face = onScalp ? static_cast<std::uint32_t>(root.face) : 0xFFFFFFFFu;
                    std::memcpy(&to[0], &face, 4);
                    if (!onScalp) return;
                    to[1] = root.u;
                    to[2] = root.v;
                    const Vec3f normal = traced->normal(root);
                    to[4] = normal.x;
                    to[5] = normal.y;
                    to[6] = normal.z;
                    if (field) {
                        const Vec3f d = field->atRoot(*traced, root, normal);
                        to[8] = d.x;
                        to[9] = d.y;
                        to[10] = d.z;
                    }
                }, 1024);
                made->strands = std::move(strands);
                auto words = std::make_shared<std::vector<float>>();
                if (field) {
                    const std::vector<Vec3f>& baked = field->vertexDirections();
                    if (traced && baked.size() == traced->pointCount()) {
                        for (const Vec3f& d : baked) words->insert(words->end(), {d.x, d.y, d.z, 0.0f});
                        made->vertices = static_cast<std::uint32_t>(baked.size());
                    }
                    for (std::size_t p = 0; p < field->positions().size(); ++p) {
                        const Vec3f& at = field->positions()[p];
                        const Vec3f& d = field->directions()[p];
                        words->insert(words->end(), {at.x, at.y, at.z, 0.0f, d.x, d.y, d.z, 0.0f});
                    }
                    made->pins = static_cast<std::uint32_t>(field->positions().size());
                    for (std::size_t k = 0; k < field->sinkPositions().size(); ++k) {
                        const Vec3f& at = field->sinkPositions()[k];
                        const Vec3f& n = field->sinkNormals()[k];
                        words->insert(words->end(), {at.x, at.y, at.z, field->sinkRadii()[k], n.x, n.y, n.z, field->sinkAngles()[k]});
                    }
                    made->sinks = static_cast<std::uint32_t>(field->sinkPositions().size());
                }
                if (words->empty()) words->push_back(0.0f);
                made->field = std::move(words);
                memo.in = in;
                memo.made = field;
                memo.keep = state.flow;
                memo.flow = std::move(made);
            }
            out->flow = memo.flow;
            return true;
        }
        default: return true;
    }

}

LookBounds lookBounds(const LookParams& p, const LookFields& fields, const std::vector<StageInputs>& inputs, const StrandSet& base,
                      float slack) {
    LookBounds b;
    const auto extent = base.extent();
    if (extent.empty()) return b;
    Vec3f lo = extent.lo, hi = extent.hi;
    auto grow = [&](float by) {
        const Vec3f g(by, by, by);
        lo = lo - g;
        hi = hi + g;
    };
    // The largest |value| of a param: its number, or its values times the multiplier.
    auto largest = [](float number, const FieldValues& f) {
        if (!f.mode) return std::abs(number);
        float m = 0.0f;
        for (const float v : f.values) m = std::max(m, std::abs(v));
        return m * std::abs(number);
    };
    // A profile never leaves its keys' range (monotone cubic Hermite): its largest |key|.
    auto peak = [](const Ramp& r) {
        if (r.keys.empty()) return 1.0f;
        float m = 0.0f;
        for (const auto& k : r.keys) m = std::max(m, std::abs(k.second));
        return m;
    };
    for (std::size_t k = 0; k < p.stages.size(); ++k) {
        const StageParams& s = p.stages[k];
        const FieldValues* f = k < fields.stages.size() ? fields.stages[k].field : nullptr;
        static const FieldValues kNone;
        const FieldValues& f0 = f ? f[0] : kNone;
        switch (s.stage.kind) {
            case StageKind::Interpolate: {
                // Within twice the guides' longest offset of the roots (inside the box so far); keeping
                // clearance (points lifted, the segments' lengths kept), twice the longest guide.
                float longest = 0.0f;
                const InterpolateInputs* in = k < inputs.size() ? inputs[k].interpolate.get() : nullptr;
                if (in && in->rows) {
                    for (const Vec3f& o : in->rows->offsets) longest = std::max(longest, o.length());
                    if (in->clearance) {
                        for (const float length : in->rows->lengths) longest = std::max(longest, length);
                    }
                }
                grow(2.0f * longest * slack);
                break;
            }
            case StageKind::Length:
            case StageKind::MeshCut: break;  // trimmed: within the strand
            case StageKind::Cull:
            case StageKind::Split:
            case StageKind::Mirror: {
                // The strands of the run up its input: within its box, and Mirror's mirrored across
                // the plane and moved onto the scalp (by its longest move).
                const LookCentersRun* run = k < inputs.size() ? inputs[k].merged.get() : nullptr;
                const GatherInputs* in = k < inputs.size() ? inputs[k].gather.get() : nullptr;
                const LookBounds made = run && run->base ? lookBounds(run->params, run->fields, run->inputs, *run->base, slack) : LookBounds{};
                if (!made.valid) break;
                Vec3f mlo = made.lo, mhi = made.hi;
                if (s.stage.kind == StageKind::Mirror && in) {
                    const Vec3f a = mirrorPoint(made.lo, in->axis, in->offset), c = mirrorPoint(made.hi, in->axis, in->offset);
                    const float r = in->reach * slack;
                    mlo = {std::min({mlo.x, a.x - r, c.x - r}), std::min({mlo.y, a.y - r, c.y - r}), std::min({mlo.z, a.z - r, c.z - r})};
                    mhi = {std::max({mhi.x, a.x + r, c.x + r}), std::max({mhi.y, a.y + r, c.y + r}), std::max({mhi.z, a.z + r, c.z + r})};
                }
                lo = {std::min(lo.x, mlo.x), std::min(lo.y, mlo.y), std::min(lo.z, mlo.z)};
                hi = {std::max(hi.x, mhi.x), std::max(hi.y, mhi.y), std::max(hi.z, mhi.z)};
                break;
            }
            case StageKind::Merge: {
                // b's strands as cached: within the base's box; made by b's own run, within its box.
                const LookCentersRun* run = k < inputs.size() ? inputs[k].merged.get() : nullptr;
                const LookBounds made = run && run->base ? lookBounds(run->params, run->fields, run->inputs, *run->base, slack) : LookBounds{};
                if (made.valid) {
                    lo = {std::min(lo.x, made.lo.x), std::min(lo.y, made.lo.y), std::min(lo.z, made.lo.z)};
                    hi = {std::max(hi.x, made.hi.x), std::max(hi.y, made.hi.y), std::max(hi.z, made.hi.z)};
                }
                break;
            }
            case StageKind::Clump: {
                // Between each point and its center's (the centers' box grown by the lock).
                const ClumpInputs* in = k < inputs.size() ? inputs[k].clump.get() : nullptr;
                Vec3f clo = lo, chi = hi;  // made from the strands: inside the box so far
                if (in && in->curves && !in->curves->empty()) {
                    clo = chi = in->curves->front();
                    for (const Vec3f& c : *in->curves) {
                        clo = {std::min(clo.x, c.x), std::min(clo.y, c.y), std::min(clo.z, c.z)};
                        chi = {std::max(chi.x, c.x), std::max(chi.y, c.y), std::max(chi.z, c.z)};
                    }
                } else if (in && in->centersRun && in->centersRun->base) {
                    // Made by their branch's run: within its box (resampled, shared: within it still).
                    const LookCentersRun& run = *in->centersRun;
                    const LookBounds made = lookBounds(run.params, run.fields, run.inputs, *run.base, slack);
                    if (made.valid) {
                        clo = made.lo;
                        chi = made.hi;
                    }
                }
                // pastTip extend: on past an axis's tip by up to the longest strand. (A lock radius
                // varied per clump up to 1 + lockRadiusRandom of it.)
                const float lock = s.value[3] * (1.0f + std::abs(s.value2[1]));
                const float r = (lock + (s.flag[1] != 0 && in ? in->reach : 0.0f)) * slack;
                lo = {std::min(lo.x, clo.x - r), std::min(lo.y, clo.y - r), std::min(lo.z, clo.z - r)};
                hi = {std::max(hi.x, chi.x + r), std::max(hi.y, chi.y + r), std::max(hi.z, chi.z + r)};
                // Volumize pushes a point out from its axis by a share of how far it is across it
                // (both within the box so far).
                const FieldValues& f1 = f ? f[1] : kNone;
                const float volume = largest(s.value[1], f1) * (s.hasRamp2 ? peak(s.ramp2) : 1.0f);
                if (volume > 0.0f) {
                    const Vec3f d = hi - lo;
                    grow(volume * d.length() * slack);
                }
                // A negative strength pushes points out by the repel radius at most.
                float lowest = s.value[0];
                if (f0.mode) {
                    lowest = 0.0f;
                    for (const float v : f0.values) lowest = std::min(lowest, v * s.value[0]);
                }
                float repelRadius = 0.0f;
                std::memcpy(&repelRadius, &s.flag[0], sizeof(float));
                if (lowest < 0.0f) grow(repelRadius * slack);
                break;
            }
            case StageKind::Noise:
                grow(kNoiseReach * largest(s.value[0], f0) * peak(s.ramp) * slack);
                break;
            case StageKind::Curl: {
                float radius = largest(s.value[0], f0);
                if (f0.widths) {
                    // A coil by widths a Width before it makes in the run: its largest width.
                    for (std::size_t j = 0; j < k; ++j) {
                        const StageParams& w = p.stages[j];
                        if (w.stage.kind != StageKind::Width) continue;
                        const FieldValues& wf = j < fields.stages.size() ? fields.stages[j].field[0] : kNone;
                        radius = std::max(radius, std::abs(s.value[0]) * largest(w.value[0], wf) * (1.0f + std::abs(w.value[1])) * peak(w.ramp));
                    }
                }
                grow(radius * (1.0f + std::abs(s.value[2])) * peak(s.ramp) * slack);
                break;
            }
            case StageKind::Width: break;
            case StageKind::Blend: break;  // between its looks' points: inside the box so far
            case StageKind::Displace: {
                // Each point moved along a unit way by its distance times the profile.
                grow(largest(s.value[0], f0) * peak(s.ramp) * slack);
                break;
            }
            case StageKind::Twist: {
                // A point turned about a centre on its axis keeps its distance from it: within the
                // box so far and its axes' (a clump's middle is inside the box so far) grown by that
                // box's diagonal.
                const TwistInputs* in = k < inputs.size() ? inputs[k].twist.get() : nullptr;
                if (in && in->axes) {
                    for (std::size_t a = 0; a < in->count * kTwistAxisSamples; ++a) {
                        const Vec3f q((*in->axes)[a * 3], (*in->axes)[a * 3 + 1], (*in->axes)[a * 3 + 2]);
                        lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
                        hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
                    }
                }
                grow((hi - lo).length() * slack);
                break;
            }
            case StageKind::Transform: {
                // Each strand scaled by k at most and turned (lengths times k) about its root, then
                // moved; or the set about the pivot.
                const float most = largest(s.value[0], f0) * (1.0f + std::max(s.value[1], 0.0f));
                const Vec3f translate(s.value2[0], s.value2[1], s.value2[2]), random(s.more[8], s.more[9], s.more[10]);
                const float move = translate.length() + random.length();
                if (s.flag[0] != 0) {
                    const Vec3f pivot(s.more[4], s.more[5], s.more[6]);
                    float far = 0.0f;
                    for (int c = 0; c < 8; ++c) {
                        const Vec3f corner(c & 1 ? hi.x : lo.x, c & 2 ? hi.y : lo.y, c & 4 ? hi.z : lo.z);
                        far = std::max(far, (corner - pivot).length());
                    }
                    const float r = (most * far + move) * slack;
                    lo = {std::min(lo.x, pivot.x - r), std::min(lo.y, pivot.y - r), std::min(lo.z, pivot.z - r)};
                    hi = {std::max(hi.x, pivot.x + r), std::max(hi.y, pivot.y + r), std::max(hi.z, pivot.z + r)};
                    break;
                }
                float reach = k < inputs.size() ? inputs[k].reach : 0.0f;
                for (std::size_t j = 0; j < k && j < inputs.size(); ++j) {
                    const InterpolateInputs* interpolate = inputs[j].interpolate.get();
                    if (!interpolate || !interpolate->rows) continue;
                    for (const float length : interpolate->rows->lengths) reach = std::max(reach, 2.0f * length);
                }
                grow((most * reach + move) * slack);
                break;
            }
            case StageKind::Bend:
            case StageKind::Direction:
            case StageKind::Part:
            case StageKind::Gravity:
            case StageKind::Flyaway:
            case StageKind::Collide:
            case StageKind::Wind:
            case StageKind::SelfCollide:
            case StageKind::Static:
            case StageKind::Tie:
            case StageKind::Smooth:
            case StageKind::RestoreLength:
            case StageKind::BlendNeighbors:
            case StageKind::Tangle:
            case StageKind::Flow:
            case StageKind::Jiggle: {
                // Each segment turned (Part, Gravity, Wind, Static, Jiggle) or pushed and walked from the root
                // (SelfCollide), or laid along a path from its start (Tie), its length kept: a point stays within its
                // strand's length of its root (the longest strand it took; after a running
                // Interpolate, its guides' too). Collide keeps lengths from points it pushed out to
                // its offset above its surfaces: within that of their boxes. Flyaway's strays carry
                // on past their tips by lengthen, wandering by their stray under the noise's reach
                // (its length at most sqrt 3 of it, stood out by 1 at most). Smooth and Tangle keep
                // their segments' lengths from the root, BlendNeighbors' shapes are mixed of strands no
                // longer than the longest it took (its own length kept or not), RestoreLength's lengths
                // its reference's (the reach the longer of the two).
                const PartInputs* in = k < inputs.size() ? inputs[k].part.get() : nullptr;
                float reach = in ? in->reach : k < inputs.size() ? inputs[k].reach : 0.0f;
                for (std::size_t j = 0; j < k && j < inputs.size(); ++j) {
                    const InterpolateInputs* interpolate = inputs[j].interpolate.get();
                    if (!interpolate || !interpolate->rows) continue;
                    for (const float length : interpolate->rows->lengths) reach = std::max(reach, 2.0f * length);
                }
                if (s.stage.kind == StageKind::Collide) {
                    const CollideInputs* collide = k < inputs.size() ? inputs[k].collide.get() : nullptr;
                    for (std::size_t m = 0; collide && m + 1 < collide->boxes.size(); m += 2) {
                        const float r = 2.0f * std::abs(s.value[0] + s.value2[0]);  // (the offset, and the softness above it)
                        const Vec3f clo = collide->boxes[m], chi = collide->boxes[m + 1];
                        lo = {std::min(lo.x, clo.x - r), std::min(lo.y, clo.y - r), std::min(lo.z, clo.z - r)};
                        hi = {std::max(hi.x, chi.x + r), std::max(hi.y, chi.y + r), std::max(hi.z, chi.z + r)};
                    }
                }
                if (s.stage.kind == StageKind::Flyaway) {
                    const FieldValues& f1 = f ? f[0] : kNone;
                    reach = reach * (1.0f + std::max(s.value2[1], 0.0f)) + largest(s.value[0], f1) * (1.7320508f * kNoiseReach + 1.0f);
                }
                // Flow lays a strand along the skin and lifts it off it: within its length of its
                // root along the skin and as far off it, sqrt 2 of it at most (twice, rounded up).
                if (s.stage.kind == StageKind::Flow) reach *= 2.0f;
                grow(reach * slack);
                break;
            }
        }
    }
    b.lo = lo;
    b.hi = hi;
    b.valid = true;
    return b;
}

PointError compareVectors(const std::vector<Vec3f>& a, const std::vector<Vec3f>& b) {
    PointError e;
    if (a.size() != b.size()) {
        e.max = std::numeric_limits<double>::infinity();
        return e;
    }
    double sum = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double d = (a[i] - b[i]).length();
        if (!(d <= e.max)) e.max = d;  // NaN shows as NaN
        sum += d;
    }
    e.count = a.size();
    e.mean = a.empty() ? 0.0 : sum / static_cast<double>(a.size());
    return e;
}

}  // namespace groomlab::gpu
