// SPDX-License-Identifier: Apache-2.0
#pragma once

// A groom's complete input, independent of the host: what the Maya node
// holds in its attributes, and what a groom file stores. Maya, the CLI and
// the Arnold procedural all evaluate grooms through GroomSession (Groom), so
// they agree.

#include <map>
#include <memory>
#include <string>

#include "groomlab/correctives.h"
#include "groomlab/graph.h"
#include "groomlab/graph_io.h"
#include "groomlab/scalp_pieces.h"

namespace groomlab {

// What a groom's scale (GroomSession::groomScale) follows: the scalp's surface area (survives
// Freeze Transformations and re-meshing at the same size; a remodel that changes the area changes
// it), the scalp's transform scale alone (survives remodelling; a freeze resets a transform's
// scale, so it does not survive Freeze Transformations), or nothing (the groom keeps its sizes).
enum class ScaleSource { Area, Transform, Off };
// "area", "transform", "off"; and back (anything else: Area).
const char* scaleSourceName(ScaleSource source);
ScaleSource scaleSourceFromName(const std::string& name);

struct GroomData {
    std::string graphJson;      // empty or "{}": the default graph
    std::string hierarchyJson;  // GuideHierarchy::toJson; empty: no hierarchy
    MeshPtr scalp;              // world space, at the evaluation time; may carry a rest pose
    // The groom's other scalps by name, in order (several scalps on one groom, scalp_pieces.h:
    // Maya's extraScalps), world space, each may carry a rest pose: joined after `scalp` into the
    // mesh the session binds everything to; a Scalp node whose input names one grows on it alone.
    // Stored in groom files ("scalps").
    NamedScalps scalps;
    GuidesPtr guides;           // flat guides (e.g. Maya curves), world space, rooted
    GuidesPtr guidePoses;       // hierarchy guides replaced by ID, e.g. simulated guides
    std::string partsJson;      // PartSet::toJson; empty: no parts
    std::string correctivesJson;  // Correctives::toJson (shot sculpts); empty: none
    std::string flowJson;       // FlowSet::toJson (flow arrows on the scalp); empty: none
    // SculptSet::toJson (sculpt.h): the Sculpt nodes' brushed edits by node name; empty: none.
    // Stored in groom files ("sculpts").
    std::string sculptsJson;
    std::map<std::string, float> controls;  // Control node values by name (Maya: the groom's control attributes)
    MapCatalog maps;            // the maps painted on the scalp, by name (Map nodes read them; graph.h)
    // Other meshes by name, read by ColliderMesh nodes (e.g. "collider" for Collide), world space,
    // each may carry a rest pose. Stored in groom files ("colliders": as they were when written).
    std::map<std::string, MeshPtr> meshes;
    // The topology the groom was made on (ScalpMesh::topologyFingerprint: a groom file's
    // scalp.topology); 0: not known. A scalp set with another (another vertex order or
    // triangulation) is said in the evaluation's errors: the guides are rooted again by position.
    std::uint64_t scalpFingerprint = 0;
    double time = 0.0;          // seconds: hierarchy() applies correctives here until evaluate() moves it
    // The scalp's points at earlier frames, for Jiggle nodes (graph.h ScalpHistory): the host
    // fills it when scalpHistoryFrames() asks for some. Not stored in groom files (GroomScene
    // samples it from the scalp cache).
    ScalpHistoryPtr scalpHistory;
    // Follow the scalp (follow_scalp.h, GroomSession::setFollowScalp): evaluate on the scalp's
    // rest pose and carry each strand to the scalp by its root's frame. Off: on the scalp as it is.
    // Stored in groom files ("followScalp": true).
    bool followScalp = false;
    // Scale with the scalp (GroomSession::groomScale): the groom is evaluated `k` times its size.
    // What k follows (ScaleSource): the scalp's surface area (k = sqrt(scalp area / scaleReference),
    // the area measured on the scalp's rest pose, else the scalp as it is), its transform's scale
    // alone (k = the host's transform scale / scaleTransformReference), or nothing (k = 1). Stored
    // in groom files ("scaleSource": "transform" or "off", written when not "area").
    ScaleSource scaleSource = ScaleSource::Area;
    // The scalp area the groom is at 1x in (scene units squared); 0: not set, the first area the
    // session measures is (k = 1 then). Stored in groom files ("scaleReference", written when set).
    double scaleReference = 0.0;
    // The transform scale the groom is at 1x in (ScaleSource::Transform); 0: not set, the first one
    // the host hands in is. Stored in groom files ("scaleTransformReference", written when set).
    double scaleTransformReference = 0.0;
};

// Registers the external inputs the graph reads: "scalp", "guides",
// "hierarchy" and "parts". Null or empty inputs are skipped. A scalp joined from several
// (scalp_pieces.h) registers each one's own mesh too, as scalpPieceExternal(name).
void setGroomExternals(ExternalInputs& externals, const MeshPtr& scalp, const GuidesPtr& guides,
                       const HierarchyPtr& hierarchy, const PartsPtr& parts = nullptr, const FlowPtr& flow = nullptr);

// Registers each mesh as the external "mesh:<name>" (what a ColliderMesh node
// of that name reads). Null meshes are skipped.
void setMeshExternals(ExternalInputs& externals, const std::map<std::string, MeshPtr>& meshes);

// Replaces the points of hierarchy guides found in `poses` (matched by ID)
// and re-evaluates their children, which follow their posed parents. Poses
// for unknown IDs are ignored. Call after GuideHierarchy::evaluate.
void applyGuidePoses(GuideHierarchy& hierarchy, const GuideSet& poses, const ScalpMesh* scalp);

// Export Guides for Simulation (every host's): the guides a GuideCache node after the Guides node
// receives, from an evaluation's hierarchy (ViewResult::hierarchy: bound, posed, corrected) and the
// flat guides (GroomData::guides): `group` "*" every guide a Guides node of the default settings
// feeds (the hierarchy's every level, then the flat guides), else one guide group's (throws
// std::runtime_error for no such group). Each keeps its guide's ID.
GuideSet guidesToSimulate(const HierarchyPtr& hierarchy, const GuidesPtr& flat, const std::string& group = "*");

// Registers control values for the graph's Control nodes (by name).
void setControlExternals(ExternalInputs& externals, const std::map<std::string, float>& controls);

// Registers the map catalog as the external "maps" (skipped when empty).
void setMapExternals(ExternalInputs& externals, const MapCatalog& maps);

// Registers the scalp's recent motion as the external "scalpHistory" (skipped when null).
void setScalpHistoryExternal(ExternalInputs& externals, const ScalpHistoryPtr& history);

// How many earlier frames of the scalp the graph's result reads: the most `history` any Jiggle
// node making `output` asks for (Graph::forEachActiveUpstream: not bypassed, on a Switch's picked
// input, inside compounds); 0 without one. Hosts sample the scalp's past only when this is > 0.
int scalpHistoryFrames(const Graph& graph, NodeId output);
// The frames a looping Jiggle making `output` reads at frame `now` past those (jiggleLoopFrame:
// the frames before its loop's start, read from the end of its range), sorted, each once: the host
// samples the scalp at these too (ScalpHistory's loop frames). Empty without a looping one.
std::vector<double> scalpHistoryLoopFrames(const Graph& graph, NodeId output, double now);

// Whether `output` depends on the evaluation time through the graph itself: a node upstream of
// it (any input, inside compounds too) salts its key with the time (Wind with some strength,
// ImportCurves: Operator::contextSalt differs between two times), a Jiggle reads the scalp's
// motion, or a Simulate node steps over the frames. Conservative: a bypassed node or a Switch's
// other input counts too. What the host feeds that moves with time (a deforming scalp, guide
// caches and nHair simulations, keyed controls and weights) is the host's to add
// (GroomSession::readsTime adds what the session knows).
bool graphReadsTime(const Graph& graph, NodeId output);

// ---------------------------------------------------------------------------
// The rest pose, for Instance nodes set to pinToRest or sized to fit their
// strands (sizeMode fitStrand): a pinned instance keeps the distance from its
// strand's root that its fraction `t` gives on the strand at rest, a fitted
// one the size its strand's rest length gives, however the strand stretches
// now (simulated or cached guides, correctives, animated curves).
//
// The rest pose is the groom evaluated without anything that moves: the
// scalp's rest pose (its points when it has none), the guide hierarchy as
// authored (no poses, no correctives) evaluated on it, no colliders, no scalp
// history (Jiggle stays still) and time 0 (for Wind and ImportCurves); flat
// guides, controls, parts, flow and maps as they are. None of that changes
// with time, so it evaluates once per edit, not per frame: RestPose keeps its
// own copy of the graph, so the host's cache for the current time is
// untouched, and from frame to frame its evaluation is a cache hit (or, with
// caching off, skipped by the rest inputs' hash). Rest lengths are taken on
// the evaluated node's strands, matched to the current ones by strand ID;
// a strand missing at rest leaves its instances at their fraction.

// What the groom is made of, as the rest pose reads it.
struct RestInputs {
    MeshPtr scalp;  // the scalp now: its rest pose (else these points) is used
    // The guide hierarchy as authored (no poses or correctives); null: none.
    std::shared_ptr<const GuideHierarchy> hierarchy;
    // `hierarchy` is already evaluated on `scalp`: without a rest pose it is used as it is.
    bool hierarchyOnScalp = false;
    GuidesPtr guides;               // flat guides, as they are
    const PartSet* parts = nullptr;  // resolved on the rest scalp
    const FlowSet* flow = nullptr;
    std::map<std::string, float> controls;
    MapCatalog maps;
};

// Each pinned instance's restDistance: its t times the arc length of the rest
// strand with its strand's ID, and each fitted one's restLength: that arc
// length (-1 when there is none). `strands` itself when no instance needs the
// rest pose, else a copy (sharing every array) with the new instances.
StrandsPtr pinRestDistances(const StrandsPtr& strands, const StrandSet& rest);

class RestPose {
public:
    // The graph (JSON, as the host evaluates it). `previous`: a RestPose of an
    // earlier version of the graph whose cached results carry over.
    explicit RestPose(const std::string& graphJson, const RestPose* previous = nullptr);
    // The same from a graph already loaded (a host's GraphDocument::clone of its own: no
    // second parse of the JSON).
    explicit RestPose(GraphDocument doc, const RestPose* previous = nullptr);
    ~RestPose();
    // As Graph::setCacheIntermediates. Off, the rest inputs' hash decides
    // whether the rest pose is evaluated again (renders: once for every motion key).
    void setCacheIntermediates(bool enabled);

    // `strands` (node `node` of the graph, "" its output, evaluated with `ctx`)
    // with their pinned instances' rest distances and fitted ones' rest lengths
    // filled in from the rest pose evaluated at the same density, point budget
    // and segments. `strands` as they are when no instance needs the rest pose
    // (InstanceSet::needsRestPose). The same inputs give the same
    // pointer back (hosts key their redraws on it). Throws GraphError.
    StrandsPtr pin(const StrandsPtr& strands, const std::string& node, const RestInputs& in, const EvalContext& ctx);

    // How many times the rest pose was evaluated through the graph (tests).
    std::size_t evaluations() const { return evaluations_; }

private:
    std::unique_ptr<GraphDocument> doc_;
    bool cacheIntermediates_ = true;
    MeshPtr restScalp_;
    std::uint64_t restScalpKey_ = 0;
    HierarchyPtr hierarchy_;
    std::uint64_t hierarchyKey_ = 0;
    PartsPtr parts_;
    std::uint64_t partsKey_ = 0;
    FlowPtr flow_;
    std::uint64_t flowKey_ = 0;
    StrandsPtr rest_;             // the last rest strands
    std::uint64_t restKey_ = 0;   // their inputs' hash (with caching off)
    StrandsPtr lastIn_, lastRest_, lastOut_;  // the last pin(), returned again for the same strands
    std::size_t evaluations_ = 0;
};

}  // namespace groomlab

// The groom evaluated: Groom is GroomSession (groom_session.h), which needs the types above.
#include "groomlab/groom_session.h"
