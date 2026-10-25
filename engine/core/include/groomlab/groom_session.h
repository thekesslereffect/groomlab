// SPDX-License-Identifier: Apache-2.0
#pragma once

// GroomSession: everything a host needs to keep one groom evaluated (docs/SESSION.md).
//
// A host (Maya's groomLabShape, an export's NodeEvaluator, the Arnold procedural through
// GroomScene, the CLI, Python, another DCC) converts its own data, hands it to the session
// through setters and asks for views. The session parses each stored input once per change,
// roots the guides, poses and corrects them, binds parts and flow, lays painted vertex maps on
// the scalp, keeps the graph and its caches (the view's, the cards', the rachis', the rest
// pose's), previews a brush stroke's working copies, builds and lays out the cards (keeping a
// bake's atlas), and says what changed (versions) and what the last evaluation did (stats).
//
// Setters no-op on the same pointer, text or content hash, so hosts may call them on every
// evaluation. A session is used from one thread at a time; it evaluates in parallel inside.
//
// `Groom` (groom.h) is this class: `Groom(GroomData)` and `evaluate(density, seconds, cards)`.

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "groomlab/cards.h"
#include "groomlab/correctives.h"
#include "groomlab/follow_scalp.h"
#include "groomlab/graph.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/scalp_maps.h"
#include "groomlab/sculpt.h"
#include "groomlab/simulation.h"
#include "groomlab/strand_geometry.h"

namespace groomlab {

class TexturePreview;  // scalp_paint.h: a texture map's live image

// Cards rebuild while a brush stroke is live only when the last rebuild (the groom at
// cardDensity plus the cards) took no longer than this; slower grooms catch up at the end.
constexpr double kLiveCardBuildBudgetMs = 250.0;

// What to evaluate.
struct ViewRequest {
    float density = 1.0f;        // the graph's preview density (EvalContext::previewDensity)
    float sculptDensity = 1.0f;  // multiplies `density` while a stroke is live (hasPreview)
    double pointBudget = 0.0;    // points the view may hold (EvalContext::maxPoints); 0: no limit
    std::string viewNode;        // a node whose strands to show instead of the output ("" or not a Strands node: the output)
    double time = 0.0;           // seconds
    // A host's interactive view (EvalContext::viewport): nodes set to renderOnly are bypassed and
    // viewportOnly ones run. False (exports, renders, the procedural, the CLI): the other way round.
    bool viewport = false;
    bool buildCards = false;     // the evaluation itself builds the Cards nodes' cards (exports)
    // Also the Cards nodes' cards, from the output at cardDensity with no point budget, on a copy
    // of the graph of their own, laid out in one atlas (the baked one's cells kept): ViewResult::cards.
    bool wantCards = false;
    float cardDensity = 1.0f;
    // Also the StrandMesh nodes' strands (evaluateStrandMeshes at the view's time): ViewResult::strandMeshes.
    bool wantStrandMeshes = false;
    // ViewResult::bounds (a pass over the strands' points when they changed); off, the bounds
    // are left as they were and worked out by the next view that wants them.
    bool wantBounds = true;
    // A node the host runs itself, with what follows it (Maya's GPU preview of a live brush
    // stroke runs Interpolate on): only what feeds its inputs is evaluated, and the view keeps the
    // strands it had (the host draws its own over them). "" or no such node: the view as ever.
    std::string stopAt;
    // Following the scalp (setFollowScalp), the host carries the strands to the scalp now itself (Maya's
    // viewport, on the GPU): the view hands them on at rest with how to carry them (ViewResult::follow)
    // instead of carrying them, where carryStrands would move nothing but their points and the
    // instances placed on them (carriesPointsAndInstances: the host's DisplayBuilder places those
    // on the instanced strands carried, DisplayInputs::follow). The bounds are the carried strands'
    // all the same.
    bool hostCarries = false;
};

// Counters that move only when what they name changed (a cache hit leaves them).
struct ViewVersions {
    std::uint64_t evaluation = 0;  // any of the below, the parts or the flow
    std::uint64_t strands = 0;     // the strands (or the node shown)
    std::uint64_t hierarchy = 0;   // the active hierarchy (every evaluation while a guide preview is set)
    std::uint64_t cards = 0;       // the cards
    std::uint64_t strandMeshes = 0;  // the StrandMesh nodes' strands
};

struct Bounds {
    Vec3f lo, hi;
    bool valid = false;  // false: nothing to bound
};

// What an evaluation's errors say while the scalp set is of another topology than the groom was
// made on (GroomSession::setScalpFingerprint).
inline constexpr const char* kScalpTopologyChanged =
    "the scalp's vertex order or triangles differ from the groom's: strands are rooted again by position";

// Something the host should say once (a Script Editor message): it is in errors too, or not
// worth an error (an unreadable card atlas).
struct SessionMessage {
    std::string text;
    bool error = true;
};

struct ViewResult {
    StrandsPtr strands;              // the shown node's strands (instances pinned to rest); null on failure
    // Set (ViewRequest::hostCarries, following the scalp): strands are at rest and these carry them to
    // the scalp now (FollowCarry::carried() makes them where they are, on demand); placed() is that.
    FollowCarryPtr follow;
    StrandsPtr placed() const { return follow ? follow->carried() : strands; }
    // Which strands those are, and how they differ from earlier ones, for a host that keeps what it
    // drew (DisplayBuilder): `strandsSerial` names this set of strands (the same serial: the same
    // strands); `strandsChanges` says which strands differ from those of serial `changesBase` (the
    // strands this view's document gave before: a live stroke's and the view's are kept apart),
    // null when not known (StrandChanges, graph.h).
    std::uint64_t strandsSerial = 0;
    std::uint64_t changesBase = 0;
    ChangesPtr strandsChanges;
    // The preview density the strands were evaluated at (EvalContext::previewDensity: the
    // request's, times sculptDensity for a live stroke's): the share of a thinned source's strands
    // shown (fewer under a point budget; all of a source never thinned: previewAll, imported curves).
    float density = 1.0f;
    std::string viewedNode;          // the node shown ("" the output)
    bool viewedRoots = false;        // ... and it is a Scatter (its roots draw as dots)
    // ... or a Clump: its clump centers, each its root on the scalp, drawn as dots
    // (DisplayInputs::clumpCenters). Null otherwise.
    std::shared_ptr<const std::vector<Vec3f>> clumpCenters;
    MeshPtr scalp;                   // with the vertex maps as color sets, refined (GroomSession::scalpSubdivision)
    MeshPtr cageScalp;               // the same before it is refined: what vertex maps are painted on
    // Active: the preview, else the authored one; posed and corrected. While a Sculpt node is the
    // brushes' target (GroomSession::setSculptTarget), that node's output as editable guides.
    HierarchyPtr hierarchy;
    HierarchyPtr authoredHierarchy;  // as stored, rooted on the scalp (null without guide data)
    PartsPtr parts;                  // bound to the scalp (the preview's while set); null without
    FlowPtr flow;
    std::shared_ptr<const CardMesh> cards;  // wantCards: the Cards nodes' cards, laid out (null without)
    StrandsPtr cardStrands;                 // what they were built from
    std::vector<CardNodeSummary> cardNodes;
    std::vector<StrandMeshSource> strandMeshes;  // wantStrandMeshes: the StrandMesh nodes' strands (empty without)
    Bounds bounds;                   // the strands and the active hierarchy's points
    ViewVersions versions;
    std::vector<std::string> errors;         // why the evaluation failed or was partial, line by line
    std::vector<SessionMessage> messages;    // to say once per change
};

// The hair's look for the viewport and hosts, from the color nodes making the output: the dye and
// the highlight from the first HairColor met walking up from the output
// (Graph::forEachActiveUpstream; its defaults, hair_color.h kDefaultHairSpecular,
// kDefaultHairRoughness and kDefaultHairTilt, without one or for a param set to a map).
struct HairLook {
    float specular = 0.3f;
    float roughness = 0.5f;
    // Each strand's specular and roughness up to this share more or less, at random (HairColor's).
    float specularRandom = 0.0f;
    float roughnessRandom = 0.0f;
    float tilt = 3.0f;  // the cuticle tilt, degrees: Arnold's shift (hair_color.h hairLobes)
    std::string node;  // the HairColor's path ("compound/inner" inside a compound), "" without one
    // The structural color (iridescentTint, color_patterns.h) of the first FeatherColor making the
    // output with any iridescence (the strands' iridescence channel says how much, point by
    // point); `iridescent` false without one.
    bool iridescent = false;
    std::array<float, 3> iridescenceColor{0.05f, 0.55f, 0.2f};
    float iridescenceShift = 0.3f;
    // Whether a color node makes the output (HairColor, Bands, FeatherColor): the strands' RGB
    // (a hair color, hair_color.h: HairColor's pigment and dye already in it) is then the hair's
    // color, as create_hair_shader gives it to aiStandardHair as its base color over no melanin;
    // without one the hosts color by the melanin channels (an Attribute's) or their defaults.
    bool colorsByRgb = false;
    // The HairColor node's dye in mode melanin (in its RGB already); white without one.
    std::array<float, 3> dye{1.0f, 1.0f, 1.0f};
    // Whether a Blend node making the output mixes two different colors (GroomStats' colorBlends):
    // the RGB is then the hair's color too (create_hair_shader's base color).
    bool colorBlends = false;
    // Whether the strands can carry a wetness channel (export_view.h kWetnessChannel): a Wet node
    // making the output (at any wetness: it may be animated), or a Simulate with water or rain.
    // create_hair_shader then adds its wet network (hair_color.h wetShading).
    bool wet = false;
};

// What a scalp shows of a painted map: a vertex map's values, a guide layer sampled at the
// scalp's vertices, or a texture map's file (and the brush's image while it paints).
struct ScalpMapView {
    std::string name, kind;                            // kind: vertices, guides or texture ("": nothing to show)
    int channels = 1;                                  // the map's: 1 gray, 4 color (r g b a)
    std::string channel = "rgb";                       // rgb, or r g b a shown as gray
    std::shared_ptr<const std::vector<float>> values;  // vertices, guides: `channels` per scalp vertex
    std::string texture;                               // texture: the file, resolved
    std::shared_ptr<const TexturePreview> preview;     // texture: the brush's image while painting
    std::uint64_t version = 0;                         // changes when any of the above does
    // The color shown for a map's color (`c`, r g b a; a gray map's r = g = b): its r g b, or
    // with one channel picked on a color map that channel as gray.
    std::array<float, 3> shown(const std::array<float, 4>& c) const {
        if (channels != 4 || channel.size() != 1) return {c[0], c[1], c[2]};
        const float v = c[channel == "g" ? 1 : channel == "b" ? 2 : channel == "a" ? 3 : 0];
        return {v, v, v};
    }
};

// What the last evaluation did, for editors and tools (groomLabGraphStats).
struct GroomStats {
    std::string view;  // the node shown
    std::size_t strands = 0, points = 0, guides = 0;
    std::size_t cards = 0, cardStrands = 0, cardTriangles = 0, cardTextures = 0, cardFeathers = 0;
    bool cardsBaked = false, cardsStale = false;
    struct CardNode {
        std::size_t cards = 0, triangles = 0, textures = 0, feathers = 0;
        bool stale = false;
    };
    std::map<std::string, CardNode> cardNodes;  // by Cards node path
    std::string hairColor;                      // the HairColor node the look comes from
    std::string renderData;                     // the RenderData node the opacity comes from
    std::vector<std::string> colorNodes;        // every color node (HairColor, Bands, FeatherColor) making the output, in walk order
    std::vector<std::string> colorBlends;       // the Blend nodes mixing two colors
    std::map<std::string, double> times;        // ms per node in its last run
    std::map<std::string, std::string> runs;    // how each node ran in the last evaluation (Graph::lastRunKinds; "cached")
    std::map<std::string, std::vector<std::string>> channels;  // each node's strand channels (cached)
    std::map<std::string, std::string> notes;
    int scalpHistoryFrames = 0;
    std::size_t scalpHistoryReads = 0;  // the host's reads of earlier frames (Maya: inMesh at other times)
    std::size_t instances = 0;          // instances with a strand in the view
    std::size_t restEvaluations = 0;
    // Distance fields made in this process so far (MeshDistanceField::madeCount; tests: a deforming
    // collider with a rest pose makes none after its first frame).
    std::size_t distanceFieldsMade = 0;
    // The view document's cached results: bytes held (Graph::cachedBytes), results its limit let go.
    std::size_t cacheBytes = 0, cacheEvictions = 0;
    bool strokeDocument = false;  // a stroke document is kept (a stroke made one, or warmStroke)
    std::vector<SimStatus> simulations;  // the Simulate nodes' caches (GroomSession::simulationStatus)

    std::string toJson() const;
};

// The cardMaps attribute, "<map>=<path>;<map>=<path>;...", as map -> path (Color, Coverage, AO,
// Atlas; Strands and Bake hold the stamp and number).
std::map<std::string, std::string> parseCardMaps(const std::string& maps);
// The "Strands=" stamp of a bake: what each Cards node's cards were made from.
std::string cardsBakeStamp(const std::vector<CardNodeSummary>& nodes);
std::map<std::string, std::string> parseCardsBakeStamp(const std::string& stamp);

// Corrective layers with a weight slot take `weightOf(slot)` on every frame (a host's live,
// keyable weight): their own envelope and curve are set aside.
void useLiveWeights(Correctives& correctives, const std::function<float(int slot)>& weightOf);

// The graph part of a session's evaluation, made by GroomSession::detach and run on any thread
// while the session goes on (see detach). It owns everything it reads: a copy of the session's
// document and the inputs as they were.
class DetachedEvaluation {
public:
    DetachedEvaluation(const DetachedEvaluation&) = delete;
    DetachedEvaluation& operator=(const DetachedEvaluation&) = delete;
    // Evaluates: the copy taken by detach, or (chained) a copy of the document the evaluation
    // before evaluated, which must have run. Never throws: a failure is kept (error()) and the
    // session evaluates for itself when it adopts nothing.
    void run();
    bool ran() const { return ran_.load(std::memory_order_acquire); }
    std::exception_ptr error() const { return error_; }  // (once ran)
    double milliseconds() const { return ms_; }          // run's (once ran)
    const ViewRequest& request() const { return request_; }
    // A warm of the stroke document (GroomSession::detachWarm), adopted by adoptWarm, not adopt.
    bool warm() const { return warm_; }
    // A live stroke's dab (GroomSession::detachStroke), adopted by adoptStroke.
    bool stroke() const { return stroke_; }

private:
    friend class GroomSession;
    DetachedEvaluation() = default;
    std::unique_ptr<GraphDocument> doc_;          // the copy evaluated
    std::shared_ptr<DetachedEvaluation> after_;   // chained: its document copied once it ran
    std::uint64_t basedOn_ = 0;                   // the session's document the first copy was of (docGeneration_)
    ExternalInputs externals_;
    std::shared_ptr<const PathResolver> paths_;
    EvalContext ctx_;
    std::string shown_;                           // the node shown ("" the output)
    ViewRequest request_;
    std::exception_ptr error_;
    double ms_ = 0.0;
    std::atomic<bool> ran_{false};
    // A warm: the stroke document copied (warmBase_) and its evaluations then (what adoptWarm checks).
    bool warm_ = false;
    const GraphDocument* warmBase_ = nullptr;
    std::size_t warmBaseEvaluations_ = 0;
    std::uint64_t warmBaseGeneration_ = 0;  // (a stroke document let go and made again since: not adopted)
    // A dab: the stroke document lent (doc_), the preview guides copied as they were, and the
    // graph they were made for (graphGeneration_).
    bool stroke_ = false;
    std::shared_ptr<GuideHierarchy> strokePreview_;
    std::uint64_t strokeGraph_ = 0;
};

class GroomSession {
public:
    // The default graph, no inputs.
    GroomSession();
    // Every input of `data`. Throws GraphError for a bad graph, std::runtime_error for bad
    // guide, part, flow or corrective data (the parser's message).
    explicit GroomSession(GroomData data);
    ~GroomSession();
    GroomSession(const GroomSession&) = delete;
    GroomSession& operator=(const GroomSession&) = delete;

    // ---- Stored inputs -----------------------------------------------------------------------
    // Swaps in another graph: parsed first (GraphError for a bad one, leaving the session as it
    // was), every document of it (the view's, cards', rachis', rest pose's) adopts its old one's
    // cache, so the nodes whose type, params, inputs and data are the same are not evaluated
    // again (LOD graphs that differ only in their Cards node re-run Cards and what follows).
    // Results are what a fresh session of the new graph gives. Same text: nothing.
    void setGraphJson(const std::string& graphJson);
    // Drops the graph (a host showing why it failed): evaluations give no strands and `error`.
    // `graphJson` is the failing text: setting it again does nothing.
    void dropGraph(const std::string& graphJson, const std::string& error);
    bool hasGraph() const { return doc_ != nullptr; }
    const std::string& graphError() const { return graphError_; }
    const std::string& hierarchyError() const { return hierarchyError_; }  // "guideData: <why>", "" when it parsed
    // Guides as stored (GuideHierarchy::toJson; empty: none). A bad text keeps the error and no guides.
    void setHierarchyJson(const std::string& json);
    void setHierarchy(std::shared_ptr<const GuideHierarchy> hierarchy);  // as stored, not rooted
    // The guides as stored (not rooted); null when the text does not parse.
    std::shared_ptr<const GuideHierarchy> storedHierarchy() const { return parsedHierarchy_; }
    void setPartsJson(const std::string& json);
    void setFlowJson(const std::string& json);
    void setCorrectivesJson(const std::string& json);
    // Live weights by slot (Correctives' `slot`): set, layers with a slot use them (useLiveWeights).
    void setCorrectiveWeights(std::map<int, float> weights);
    void clearCorrectiveWeights();
    // The Sculpt nodes' edits (sculpt.h: SculptSet's JSON, by node name; empty: none). A bad text
    // keeps the error ("sculptData: <why>") and no edits.
    void setSculptsJson(const std::string& json);
    const SculptSet& sculpts() const { return sculpts_; }
    // The stored inputs that did not parse, "<label>: <why>" (guideData, graph, partData,
    // flowData, correctiveData, scalpMaps, sculptData), in that order.
    std::vector<std::string> inputErrors() const;

    // ---- Scalp and maps ----------------------------------------------------------------------
    // World space, at the evaluation time; may carry a rest pose and color sets. The same content
    // keeps the old mesh (with what it built).
    void setScalp(MeshPtr scalp);
    // The scalp's points at another time, on the current scalp's topology, UVs and colors
    // (ScalpMesh::withPoints: only what the points change is worked out again, the closest-point
    // tree is refitted). `restPoints`, when given, replace the rest pose (a rest mesh that moves
    // with a transform); otherwise the rest pose stays. Throws std::invalid_argument for another
    // point count, std::runtime_error without a scalp. The same points (and rest): nothing.
    void updateScalpPoints(std::vector<Vec3f> points, const std::vector<Vec3f>* restPoints = nullptr);
    const MeshPtr& rawScalp() const { return data_.scalp; }
    // The groom's other scalps by name, in order (several scalps on one groom, scalp_pieces.h; each
    // world space at the evaluation time, may carry a rest pose): joined after the scalp into the
    // mesh everything binds to (scalp(), cageScalp(): the groom's own scalp's triangles and points
    // first), each one's own mesh registered for the Scalp node naming it. Empty: the scalp alone.
    // Null meshes are left out; the same names and contents: nothing.
    void setScalps(NamedScalps scalps);
    // The scalp as given joined with the other scalps (combineScalps): the scalp itself without any.
    MeshPtr joinedScalp();
    // The topology the groom was made on (GroomData::scalpFingerprint, a groom file's
    // scalp.topology; 0: not known). While the scalp set has another, every evaluation says so in
    // its errors (kScalpTopologyChanged): the guides are rooted again by position, vertex maps
    // read other vertices. A host moving the groom to another scalp on purpose sets the new one's.
    void setScalpFingerprint(std::uint64_t fingerprint);
    // The scalp as evaluated: with the vertex maps as color sets (and the previewed map), refined
    // by scalpSubdivision() levels of Catmull-Clark (subdivision.h). Everything the session binds
    // (guides, flat guides, parts, flow, following, simulation, the graph's "scalp") sits on it.
    MeshPtr scalp();
    // The same before it is refined: the scalp as given with its vertex maps. Vertex maps are
    // stored and painted per vertex of this mesh (the refined one reads them interpolated).
    MeshPtr cageScalp();
    // The Catmull-Clark levels the scalp is refined by: the graph's Scalp node's `subdivision`
    // (the most of several), "matchRender" taking the scalp's ScalpMesh::renderSubdivision (what
    // the host says the renderer does; a Scalp node reading another scalp, that scalp's). 0: the
    // scalp as given. With several scalps, the most of scalpSubdivisions().
    int scalpSubdivision() const;
    // Each scalp's own levels by name ("scalp" the groom's own, scalp_pieces.h): the most the Scalp
    // nodes reading it ask for (match render: its own render level; a node naming no other scalp
    // reads the groom's own), and a scalp no Scalp node reads the groom's own scalp's. Each scalp
    // of the joined mesh is refined at its own (ScalpPiece::subdivision), so adding a scalp, or
    // refining it, leaves the others' strands where they were.
    std::map<std::string, int> scalpSubdivisions() const;
    // `joined` (the scalp joined with the other scalps as joinedScalp joins them: a host's own
    // reading of the same meshes) refined as the session refines its scalp, each scalp at its own
    // level; its cage() carries those levels. `joined` itself at 0 levels.
    MeshPtr refineAsScalp(const MeshPtr& joined) const;
    // Painted maps (scalp_maps.h): vertex maps laid on the scalp as color sets of their names,
    // and the map catalog the Map nodes read. A bad text keeps the error and no maps.
    void setScalpMapsJson(const std::string& json);
    void setScalpMaps(ScalpMaps maps);
    const ScalpMaps& scalpMaps() const { return scalpMaps_; }
    // The maps as stored: the text set (setScalpMapsJson), else the maps' JSON.
    std::string scalpMapsJson() const { return scalpMapsFromText_ ? scalpMapsText_ : scalpMaps_.toJson(); }
    const std::string& scalpMapsError() const { return scalpMapsError_; }
    // The catalog when the scalp already carries its vertex maps (groom files): used while no
    // scalpMaps are set.
    void setMapCatalog(MapCatalog maps);

    // ---- Moving inputs -----------------------------------------------------------------------
    void setGuides(GuidesPtr guides);      // flat guides (e.g. Maya curves), world space, rooted
    void setGuidePoses(GuidesPtr poses);   // hierarchy guides replaced by ID (simulated, cached)
    GuidesPtr guidePoses() const { return data_.guidePoses; }
    void setControls(std::map<std::string, float> controls);  // Control node values by name
    // Other meshes for ColliderMesh nodes, by name; a null mesh removes the name.
    void setMeshes(std::map<std::string, MeshPtr> meshes);
    void setMesh(const std::string& name, MeshPtr mesh);
    // The scalp's earlier frames (Jiggle); null: none. Each frame holds the points of the scalp
    // joined with the other scalps (joinedScalp: every scalp's own earlier points, so hair on a
    // moving other scalp jiggles too), or the groom's own scalp's alone (the other scalps then
    // keep still: their points now). A frame of another count is still.
    void setScalpHistory(ScalpHistoryPtr history);
    void setPathResolver(PathResolver resolver);
    void setPathResolver(std::shared_ptr<const PathResolver> resolver);
    // The last card bake (cardMaps: "Atlas=<path>;Strands=<stamp>;..."): rebuilt cards keep the
    // baked atlas' cells (the file read again when its stamp changes), and stats say which
    // Cards nodes changed since.
    void setCardMaps(const std::string& cardMaps);
    // Off for one-shot evaluation (render): lower peak memory, nothing kept. See Graph.
    void setCacheIntermediates(bool enabled);

    // ---- Following the scalp (follow_scalp.h) -------------------------------------------------
    // On: while the scalp is away from its rest pose, the graph evaluates on the rest pose (the
    // scalp's rest points: a host's rest mesh) and every strand is carried to the scalp as it is
    // by its root's rigid frame; the result is the groom at rest, moved root by root. What the
    // graph is handed in world space at this time is taken to rest space by the inverse motion of
    // its roots (flat guides, guide poses: simulations and caches) and the scalp's earlier frames
    // are turned into rest space (Jiggle); the stored guides, parts and flow are bound to the rest
    // pose. Colliders, ImportCurves' curves, Wind's direction and world-space noise are read in
    // rest space as they are (they do not follow the head). Off (the default), or a scalp without
    // a rest pose or at it: the groom evaluates on the scalp as it is, as it always has.
    void setFollowScalp(bool on);
    bool followScalp() const { return data_.followScalp; }
    // The last evaluation followed the scalp (on, and the scalp away from its rest pose).
    bool following() const { return following_; }

    // ---- Scaling with the scalp ---------------------------------------------------------------
    // On (the default), the groom grows with its scalp like a child object: it is evaluated k times
    // its size, k = sqrt(area / reference), `area` the scalp's surface area on its rest pose (the
    // scalp as it is without one: ScalpMesh::restTotalArea; the groom's own scalp, not the other
    // scalps), so scaling the scalp's transform, freezing that scale into its points or re-meshing it
    // give the same k, and a deforming scalp with a rest pose keeps one k on every frame. Applied at
    // evaluation; what is stored stays as it is: the graph's params scale by their units
    // (scaleGraphJson: lengths and widths by k, densities by 1 / k^2 so there are as many strands,
    // per-length rates by 1 / k), keyed controls by their params' units (controlScaleFactors), the
    // guides' shapes about their roots (GuideHierarchy::setScale, shot sculpts' offsets with them),
    // so the result is the groom at 1x scaled up (Simulate's gravity and offsets are lengths: its
    // frames are dropped when k changes, and a bake records other settings). k changes below 1e-4
    // (a moving scalp's float noise) are left out. Off: k is 1 (the groom keeps its sizes in scene
    // units, as before). A host fitting new nodes or presets to the scalp's size divides it by k.
    // What k follows (ScaleSource): Area as above; Transform: the scale the host hands in
    // (setScaleTransform: the scalp's transform and rig at the start frame) over
    // scaleTransformReference, mesh edits left out (a host without transforms hands none in: k is
    // then the area's on the first measure and stays); Off: k is 1.
    void setScaleSource(ScaleSource source);
    ScaleSource scaleSource() const { return data_.scaleSource; }
    // On: Area (unless it follows the transform already); off: Off.
    void setScaleWithScalp(bool on);
    bool scaleWithScalp() const { return data_.scaleSource != ScaleSource::Off; }
    // The scalp's transform scale (uniform: the cube root of its matrix's determinant, a rig's root
    // joints' with it), as the host measures it; 0 or less: none (a host without transforms).
    // Handed in before the scalp it was measured with (setScalp).
    void setScaleTransform(double scale);
    double scaleTransform() const { return scaleTransform_; }
    // The transform scale the groom is 1x at (0: not set, the first one handed in is).
    void setScaleTransformReference(double scale);
    double scaleTransformReference() const {
        return data_.scaleTransformReference > 0.0 ? data_.scaleTransformReference : firstTransform_;
    }
    // The last time k changed while the transform scale did not (the scalp's area changed: an edit,
    // a remodel, another scalp) under Area: a line for the user ("GroomLab: the scalp's area changed
    // 12%: the groom scaled x1.06. Set Scale Reference to keep its sizes."), and its number (0: none
    // yet), so a host says it once.
    const std::string& scaleNotice() const { return scaleNotice_; }
    std::uint64_t scaleNoticeSerial() const { return scaleNoticeSerial_; }
    // The area the groom is 1x at (scene units squared). 0 or less: not set, the first area measured
    // is (scaleReference() says which). Set Scale Reference sets scaleArea().
    void setScaleReference(double area);
    // The reference in use: the one set, else the first area measured (0 before any scalp).
    double scaleReference() const { return data_.scaleReference > 0.0 ? data_.scaleReference : firstArea_; }
    // The scalp's area as last measured (0 without a scalp).
    double scaleArea() const { return scaleArea_; }
    // k: what the groom is scaled by now (1 while off, without a scalp or a reference).
    double groomScale() const { return groomScale_; }

    // Whether the groom's result depends on the time through what the session knows: the graph
    // (graphReadsTime: Wind, ImportCurves, Jiggle, Simulate), corrective layers whose weight changes with
    // time (a weight curve, a fade in or out) and a scalp history handed in. A host adds what only
    // it knows moves (a deforming scalp, guide caches and simulations, keyed controls and
    // weights); when none of it does, frames other than the last evaluated one give the same
    // strands and the host may skip evaluating them.
    bool readsTime() const;

    // ---- Simulation (simulation.h; docs/SESSION.md) -------------------------------------------
    // The Simulate nodes' frames: each node keeps every frame from its start frame to the last one
    // stepped (SimCache). A view at a time steps a node in cacheMode "live" on to that frame (from
    // its last frame) when the frame is not cached; scrubbing back reads the cache. A change of a
    // node's settings drops its frames, as does other input at a frame it shows again (a groom
    // edit, another animation). While a brush stroke is live the nodes pass their guides through.
    // Frames are at framesPerSecond: frame f at frameToSeconds(f, fps) (frame_time.h: f / fps, an
    // NTSC rate exact); a time between two frames (motion blur) blends them. Before its start
    // frame a node passes its guides through.
    // 24 by default, an NTSC rate made exact (canonicalFramesPerSecond); another drops every frame.
    void setFramesPerSecond(double fps);
    double framesPerSecond() const { return fps_; }
    // How the host's inputs at another time reach the session (SimFeed): what the frames stepped on
    // the way to the one asked for read. Without a feed, or where it cannot give a time (Maya while
    // it plays reads no other time: the frames real-time playback skips), a node steps over those
    // frames in one go toward the inputs as they are (the roots moving evenly from its last frame's
    // to these), and they are stamped as not checked: a later view of one, with the feed giving its
    // own inputs, keeps them (what played is what a bake writes).
    void setSimulationFeed(SimFeed feed) { simFeed_ = std::move(feed); }
    // Off: a view never steps a node; a frame it has neither cached nor baked shows its guides as
    // groomed. For a host evaluating frames in any order, apart from its live view (Maya's cached
    // playback filling frames in the background), where stepping from the last frame would make
    // other frames than playing through. On by default.
    void setSimulationStepping(bool on) { simStepping_ = on; }
    // The groom at a frame depends on the frames before it: a Jiggle node (the scalp's history), or
    // a Simulate node whose frames from max(firstFrame, its start frame) to lastFrame are not all in
    // a bake that matches its settings (stepped live, or in cacheMode "cached" the frames a
    // Simulate Range made in this session). Such a groom must be evaluated by the session that
    // steps it (the host's live view), not by another one frame by frame in any order.
    bool readsEarlierFrames(const std::string& viewNode, int firstFrame, int lastFrame) const;
    // The Simulate nodes making the output (not bypassed, top level), upstream first.
    std::vector<std::string> simulateNodes() const;
    // Simulate Range: every Simulate node making the output (in either cacheMode) stepped on to
    // `frame` from its last frame, the frames on the way fed by the host's feed; the session's
    // inputs are put back as they were after (as the last view had them).
    SimRun simulateTo(int frame, const SimProgress& progress = {});
    // Drops a node's frames ("": every node's); a bake stays.
    void invalidateSimulation(const std::string& node = {});
    std::vector<SimStatus> simulationStatus() const;
    // A node's frame as simulated (null when not cached): what a bake writes.
    GuidesPtr simulatedFrame(const std::string& node, int frame) const;
    // A bake of a node (engine/io sim_cache.h reads one from its file); null removes it. While its
    // stamp is the node's settings' (simulationStamp) the frames within its span come from it.
    void setBakedSimulation(const std::string& node, BakedSimulationPtr baked);
    BakedSimulationPtr bakedSimulation(const std::string& node) const;
    // What a bake of the node made now records (SimSettings::hash of its params); 0 without the node.
    std::uint64_t simulationStamp(const std::string& node) const;

    // ---- Previews (a live brush stroke; the tool mutates the objects between evaluations) -----
    // While set, this hierarchy replaces the stored one. `final`: shown as it is (it already has
    // the poses and correctives: shot sculpting); otherwise they apply on top. Null clears.
    void setPreviewHierarchy(std::shared_ptr<GuideHierarchy> preview, bool final = false);
    std::shared_ptr<GuideHierarchy> previewHierarchy() const { return previewHierarchy_; }
    bool previewFinal() const { return previewFinal_; }
    // Whichever is set replaces the stored parts or flow; null goes back to them. The stored ones
    // are bound to the cage (cageScalp: parts.h, flow.h), a preview to the scalp as evaluated
    // (scalp(): refined when it is subdivided), as the tools work on it.
    void setPreviewOverlays(std::shared_ptr<const PartSet> parts, std::shared_ptr<const FlowSet> flow);
    bool hasPreviewOverlays() const { return previewParts_ || previewFlow_; }
    // `values` (`channels` per vertex: 1 gray, 4 color) stand in for the stored map `name`; null clears.
    void setPreviewVertexMap(const std::string& name, std::shared_ptr<const std::vector<float>> values, int channels = 1);
    // A texture map's live image, for scalpMapView only; null clears.
    void setPreviewTextureMap(const std::string& name, std::shared_ptr<const TexturePreview> image);
    // A stroke that previews no guides is live (a scalp map stroke). Off: true when an evaluation
    // took the stroke's density, so the host evaluates again at the full one.
    bool setLiveStroke(bool on);
    // A brush stroke is live: a guide preview, or setLiveStroke.
    bool hasPreview() const { return previewHierarchy_ != nullptr || liveStroke_; }
    bool liveStroke() const { return liveStroke_; }
    std::uint64_t previewMapVersion() const { return previewMapVersion_; }

    // ---- Sculpt nodes (sculpt.h) ---------------------------------------------------------------
    // The Sculpt node brushes shape ("" none): while it names a Sculpt node of the graph, a view's
    // `hierarchy` is that node's output as editable guides (sculptHierarchy: one root group named
    // after the node), what hosts draw, hover and pick as the guides; the stored guides are still
    // what the graph reads. Its output is evaluated in the view's document when the view did not.
    void setSculptTarget(const std::string& node);
    const std::string& sculptTarget() const { return sculptTarget_; }
    // A live stroke on node `node`: `edits` stand in for its stored ones (null clears the preview),
    // and `shown` (the stroke's working guides) is the view's hierarchy meanwhile.
    void setPreviewSculpt(const std::string& node, SculptPtr edits, std::shared_ptr<const GuideHierarchy> shown);
    bool hasPreviewSculpt() const { return previewSculpt_ != nullptr; }
    const std::string& previewSculptNode() const { return previewSculptNode_; }  // "" without a preview
    // What a stroke on Sculpt node `node` reads (sculpt.h SculptInput): the guides it is fed and
    // the scalp it takes (its inputs, with the last view's inputs and settings), its strength and
    // its stored edits. Throws std::runtime_error: no graph, no node of that name, not a Sculpt node, nothing fed.
    SculptInput sculptInput(const std::string& node);
    // The target's output as editable guides (the stroke's working guides while one is live); null
    // without a target (or one that is no Sculpt node, or fails). Evaluated if needed.
    HierarchyPtr sculptHierarchy();

    // ---- Evaluation --------------------------------------------------------------------------
    // Never throws: a failure leaves no strands and says why in errors. The result stays valid
    // until the next evaluate.
    const ViewResult& evaluate(const ViewRequest& request);
    const ViewResult& last() const { return view_; }
    // The output at `density` and `seconds` (buildCards: Cards nodes build their cards), instances
    // pinned or fitted to rest. Throws what the evaluation threw (GraphError...).
    StrandsPtr evaluate(float density, double seconds, bool buildCards = false);
    // What evaluate(density, seconds) gives, when the last view evaluated it alike (an export of
    // the frame the viewport shows, at the view's density): its strands placed (FollowCarry
    // carried), no evaluation. Alike: the output viewed (no node), no stroke, the same density and
    // time, cards not built, no Scatter thinned to the point budget, and no node off in one and on
    // in the other (evaluation modes; nor a Compound or Reference, whose insides would say). Null
    // otherwise. The caller has the view evaluated with the inputs as they are (Maya's export
    // pulls the shape's frame first).
    StrandsPtr viewedAsRendered(float density, double seconds) const;
    // One node by name ("" the output) with the last evaluation's inputs at `density`, in a
    // scratch copy of the graph (results matching the view's are reused, none evicted). Throws
    // GraphError (no such node, a failing node) or std::runtime_error (no graph).
    Value evaluateNode(const std::string& name, float density, double seconds);
    // The output at full density with the last evaluation's inputs, on a copy of the graph of
    // its own (Maya's outRachis: every feather, as exports see them); releaseFullDensity drops it.
    // Throws as the graph does; null without a graph.
    StrandsPtr evaluateFullDensity(double seconds);
    void releaseFullDensity();
    // The StrandMesh nodes making the output (top-level ones, in Graph::forEachActiveUpstream's
    // order), each with the strands it is handed at `seconds` with the last evaluation's inputs: on a
    // copy of the graph at the node's density (one per density), thinned to its maxStrands
    // (thinStrandsById), carried to the scalp now while following. A node whose strands did not
    // change hands the same pointer again. Hosts make the mesh of them (buildStrandMesh, and
    // strandMeshPoints while strandMeshTopologyKey stays). Throws as the graph does; empty without a
    // graph. releaseStrandMeshes drops the copies.
    using StrandMeshSource = groomlab::StrandMeshSource;
    std::vector<StrandMeshSource> evaluateStrandMeshes(double seconds);
    void releaseStrandMeshes();
    // A brush stroke's first dab evaluates the stroke document (the graph at the request's density
    // times its sculptDensity, a cache of its own). Evaluated ahead, with the last evaluation's
    // inputs and no preview (a host's idle time while its brush tool is up), that dab makes again
    // only what the stroke changed. Nothing while a stroke is live, at a sculptDensity that leaves
    // the density as it is, or without a graph; true when a node was evaluated. Failures are left
    // for the view to say. Its evaluations are not counted in evaluationCount (nothing shows them).
    bool warmStroke(const ViewRequest& request);
    // warmStroke's evaluation away from the session, as detach is evaluate's: detachWarm copies the
    // stroke document (made first if there is none) with the inputs warmStroke would take now, its
    // DetachedEvaluation::run evaluates the copy on any thread (a DetachedRunner) while the session
    // stays the caller's, and adoptWarm makes the evaluated copy the stroke document, unless that
    // document was evaluated, released or made again since (a stroke pressed meanwhile: its own
    // evaluation stands). Null where warmStroke would evaluate nothing. A warm is not a commit:
    // detach never chains on one (it says null: take it in first).
    std::shared_ptr<DetachedEvaluation> detachWarm(const ViewRequest& request);
    bool adoptWarm(DetachedEvaluation& run);
    // A live stroke's dab evaluated away from the session (a host's CPU brush preview off its UI
    // thread). detachStroke binds the inputs evaluate(request) would bind now, with a copy of the
    // preview guides as they are (the tool goes on editing its own between dabs), and lends the
    // stroke document to the run: DetachedEvaluation::run evaluates it on any thread while the
    // session stays the caller's (the tool's dabs go on setting its preview), but until the run is
    // adopted nothing may evaluate the stroke document (evaluate with the preview, warmStroke,
    // releaseStroke: the host takes the run in first). adoptStroke (once run) gives the document
    // back and makes the copy the preview, so the evaluate(request) that follows finds every result
    // made, bit for bit what a dab evaluated here would have made of those guides; the host puts
    // its tool's preview back after (setPreviewHierarchy, previewFinal). False when the document
    // could not come back (the graph was set again meanwhile: the session makes another). Null
    // where only the session's own evaluation can: no guide preview (a map, part or flow stroke),
    // the stroke at the view's density, Simulate nodes, cards, stopAt, no stroke document yet.
    std::shared_ptr<DetachedEvaluation> detachStroke(const ViewRequest& request);
    bool adoptStroke(DetachedEvaluation& run);
    // A stroke document lent to a run not yet adopted (detachStroke).
    bool strokeLent() const { return strokeLent_; }
    // Drops the stroke document and its cache (the brush tool put away): the next stroke's first
    // dab, or warmStroke, makes it again.
    void releaseStroke();
    bool strokeDocument() const { return sculptDoc_ != nullptr; }
    // The graph part of evaluate(request) run away from the session (a host's commit off its UI
    // thread while the session goes on with the next brush stroke: docs/SESSION.md, "Evaluating
    // off the UI thread"). detach takes what evaluate(request) would take now (its inputs, the
    // node shown) and a copy of the viewed document, its cached results shared (Graph::clone),
    // and DetachedEvaluation::run evaluates that copy on any thread: the session stays the
    // caller's, its own document as it was (it may evaluate meanwhile: a stroke's dabs). `after`, a
    // detached evaluation not yet adopted: this one copies the document that one evaluates, once it
    // has run (the host runs them in order), so each commit goes on from the one before. Null when
    // nothing can be detached: no graph, a live preview, Simulate nodes (they step through the
    // host), cards wanted. adopt (once run) makes the evaluated copy the session's document, so the
    // evaluate(request) that follows finds every result it made, bit for bit what that evaluation
    // would have made; false (the session's document changed since: another graph) leaves it.
    std::shared_ptr<DetachedEvaluation> detach(const ViewRequest& request, const std::shared_ptr<DetachedEvaluation>& after = nullptr);
    bool adopt(DetachedEvaluation& run);

    // ---- What the last evaluation used and made ------------------------------------------------
    // The hierarchy on the current scalp with poses then correctives (at the last evaluated
    // time, or data().time); null without guides. Evaluated if needed (no graph run).
    HierarchyPtr hierarchy();
    // The active hierarchy with poses but no correctives: what shot sculpts are recorded against.
    HierarchyPtr posedHierarchy();
    // The guides as stored, rooted on the scalp as given: no poses, correctives or preview (what
    // brush strokes edit and write back). Null without guides. No graph run.
    HierarchyPtr authoredHierarchy() { return rootedHierarchy(); }
    // Earlier frames of the scalp the graph reads: the most `history` of the Jiggle nodes making
    // the output (and `viewNode` when given).
    int scalpHistoryFrames(const std::string& viewNode = {}) const;
    // The frames a looping Jiggle among those reads at frame `now` past them (groom.h
    // scalpHistoryLoopFrames): the host samples these too.
    std::vector<double> scalpHistoryLoopFrames(double now, const std::string& viewNode = {}) const;
    bool hasCardsNode() const;  // a Cards node makes the output
    bool hasStrandMeshNode() const;  // a StrandMesh node makes the output
    bool cardsEvaluated() const { return cardDoc_ != nullptr; }  // the last evaluation wanted cards
    HairLook hairLook() const;
    // The look as JSON for hosts (Maya's groomLabGraphStats -hairLook, Blender's evaluate): the
    // HairLook's fields (node, specular, roughness, specularRandom, roughnessRandom, tilt, dye,
    // colorsByRgb, colorBlends, iridescent, iridescenceColor, iridescenceShift, wet),
    // "lobes" (hair_color.h hairLobes: specularTint, roughness, azimuthalRoughness, ior, shift), and
    // the params of the nodes it comes from, inner ones with their compound's exposed params set
    // (graph_io.h paramsJson): "hairColorParams" and "renderDataParams" (the first HairColor and
    // RenderData making the output; null without), "colorNodes" ([{path, type, params}] of every
    // HairColor, Bands and FeatherColor making the output, as GroomStats::colorNodes).
    std::string hairLookJson() const;
    // A painted map (or guide layer) by name as the scalp shows it; `channel` rgb or r g b a.
    ScalpMapView scalpMapView(const std::string& name, const std::string& channel);
    GroomStats stats() const;
    std::vector<std::pair<std::string, double>> nodeTimes() const;   // ms per node, by name
    std::vector<std::pair<std::string, std::string>> nodeNotes() const;
    // Operator evaluations of the current graph since it was set (Graph::evaluationCount), and of
    // those the ones that made only what changed again (Graph::incrementalCount).
    std::size_t evaluationCount() const;
    std::size_t incrementalCount() const;
    std::size_t restEvaluations() const;  // the rest pose's evaluations (0 while no instance needs it)
    const GroomData& data() const { return data_; }  // the inputs as set (texts, scalp as given)
    // The graph document the last view evaluated, its cached results as they are (null without a
    // graph): what a host reads to run part of the graph elsewhere (Maya's GPU preview reads the
    // strands entering its look nodes). Valid until the next setGraphJson or evaluate.
    const GraphDocument* viewDocument() const { return viewDoc(); }
    // The compounds the documents hold as their nodes inline (GraphEdit::flattenCompounds): a
    // host's node name or param (a compound's, an exposed param) as the documents name them.
    const GraphEdit::FlatCompounds& flatCompounds() const { return flat_; }
    // A host's live param drag (EvalContext::liveDrag): while on, the viewport's Subdivide nodes
    // hand their input on, so the GPU's preview runs the nodes before them (the next view's
    // evaluation makes their result again); off, the next view refines them again.
    void setLiveDrag(bool on) { liveDrag_ = on; }
    bool liveDrag() const { return liveDrag_; }
    // Whether the output is refined for the viewport by a Subdivide (one not bypassed, of viewport
    // levels): what a live drag (setLiveDrag) hands on.
    bool viewRefinesStrands() const;

private:
    void parseAll();  // GroomData's texts
    // The document the last view evaluated (its times, notes and channels are the stats').
    const GraphDocument* viewDoc() const { return viewSculpt_ && sculptDoc_ ? sculptDoc_.get() : doc_.get(); }
    void bindScalp(std::vector<std::string>& errors);
    // The flat guides and the scalp's earlier frames on the refined scalp (rooted again, the
    // frames refined); the host's own without subdivision.
    void bindRefined();
    // The inputs bound at `seconds`: the mapped scalp, the rooted, posed and corrected guides, parts
    // and flow, the rest-space ones while following.
    void prepareTime(double seconds, std::vector<std::string>& errors, std::vector<SessionMessage>& messages);
    HierarchyPtr rootedHierarchy();
    void updateActiveHierarchy();
    void bindOverlays();
    void prepareFollow(std::vector<SessionMessage>& messages);  // the rest-space inputs while following_
    StrandsPtr carry(const StrandsPtr& strands, int slot);      // to the scalp now (memo per slot)
    // The boxes of strands handed on at rest (ViewResult::follow), made once for them (the bounds).
    const StrandBoxes& followBoxes(const StrandsPtr& rest);
    std::shared_ptr<const StrandBoxes> followBoxes_;
    std::weak_ptr<const StrandSet> followBoxesOf_;
    MeshPtr followBoxesFrom_;
    // The clump centers of Clump `node` (whose output `clumped` is), each its
    // root on the scalp now (ViewResult::clumpCenters).
    std::shared_ptr<const std::vector<Vec3f>> clumpCenterDots(GraphDocument& doc, NodeId node, const StrandsPtr& clumped,
                                                              const EvalContext& ctx);
    void fillExternals(ExternalInputs& externals) const;
    // Simulation: the Simulate nodes the view needs (upstream first), each with its cache.
    struct SimNode {
        SimCache cache;
        BakedSimulationPtr baked;
        GuidesPtr shown;  // its frame for the last view (the external "sim:<node>"); null: none
        std::uint64_t shownHash = 0;
        // Its inputs' hash at the time last checked: the same time again with other inputs (an
        // edit shown at a subframe or a frame not simulated yet) starts it again, as at a frame it kept.
        bool checked = false;
        double checkedSeconds = 0.0;
        std::uint64_t checkedHash = 0;
        // The hash of the inputs its last frame was stepped toward (advanceSimulations).
        std::uint64_t towardHash = 0;
    };
    struct SimRef {
        std::string name;
        NodeId id = 0;
        SimNode* node = nullptr;
    };
    // The Simulate nodes making the output (and `viewNode`), not bypassed, upstream first.
    std::vector<std::pair<std::string, NodeId>> simulateNodeIds(const std::string& viewNode) const;
    std::vector<SimRef> simRefs(const std::string& viewNode);  // the same, each with its cache set up
    // Clears a node's frames when what it is fed at `seconds` changed since it was simulated: at a
    // frame it kept, against that frame's inputs; elsewhere against the last check at that time.
    void checkSimInputs(SimRef& ref, double seconds, const ExternalInputs& externals);
    // The frame a node shows at `seconds` (its bake's, else its cache's; between two frames the
    // two blended), carried to the rest pose while following, and its hash; null: none.
    std::pair<GuidesPtr, std::uint64_t> simFrameAt(const SimNode& node, double seconds) const;
    EvalContext simContext(double seconds, const ExternalInputs& externals) const;
    // What a Simulate node is fed at ctx's time (its guides and collider, the scalp now; guides
    // given on the rest pose while following are carried to the scalp), and its hash.
    SimInputs simInputs(const SimRef& ref, const EvalContext& ctx, std::uint64_t* hash);
    // The nodes' frames at the request's time (stepping live nodes), into each node's `shown`.
    void stepSimulations(const ViewRequest& request, std::vector<std::string>& errors);
    // Steps `nodes` on to `target` (frames other than `nowSeconds` fed by the host's feed).
    void advanceSimulations(std::vector<SimRef>& nodes, int target, double nowSeconds, const SimProgress& progress, SimRun& run);
    bool bakeUsable(const SimNode& node, double seconds) const;
    const PathResolver* paths() const;
    void buildCards(const EvalContext& ctx, float cardDensity, std::vector<SessionMessage>& messages);
    bool refreshCardAtlas(std::vector<SessionMessage>& messages);

    GroomData data_;  // texts as set, raw scalp, guides, poses, controls, meshes, catalog, history, time
    bool cacheIntermediates_ = true;
    std::shared_ptr<const PathResolver> paths_;

    // The graph: parsed once per change; the documents below are its clones.
    std::unique_ptr<GraphDocument> parsed_;
    GraphEdit::FlatCompounds flat_;  // parsed_'s compounds inline (flattenCompounds)
    bool liveDrag_ = false;          // setLiveDrag
    std::unique_ptr<GraphDocument> doc_;        // the view's
    std::uint64_t docGeneration_ = 0;           // each new doc_ (a detached evaluation's base)
    std::uint64_t strokeDocGeneration_ = 0;     // each time sculptDoc_ goes or takes another graph (adoptWarm)
    std::vector<SessionMessage> detachedMessages_;  // said by detach's binding, for the next evaluate to say
    // A live stroke's view at its own density (previewDensity x sculptDensity): a document of its
    // own, so the stroke's first dab does not evict the view's cache (its release would scatter
    // and evaluate everything again), and the next stroke finds this one's results.
    std::unique_ptr<GraphDocument> sculptDoc_;
    bool viewSculpt_ = false;                   // the last view evaluated sculptDoc_
    bool strokeLent_ = false;                   // sculptDoc_ is a dab's run's (detachStroke)
    std::uint64_t graphGeneration_ = 0;         // each graph set (what a lent stroke document was made of)
    // sculptDoc_'s evaluations by warmStroke (left out of the counts), and the shown ones of
    // stroke documents released since the graph was set (kept in them).
    std::size_t warmEvaluations_ = 0, warmIncrementals_ = 0;
    std::size_t releasedEvaluations_ = 0, releasedIncrementals_ = 0;
    std::unique_ptr<GraphDocument> cardDoc_;    // cards at cardDensity
    std::unique_ptr<GraphDocument> fullDoc_;    // full density (rachis)
    std::map<float, std::unique_ptr<GraphDocument>> meshDocs_;  // the StrandMesh nodes', by density
    struct MeshMemo {
        StrandsPtr in;     // what the node was handed
        MeshPtr from, to;  // the carry's scalps (null: not following)
        std::size_t maxStrands = 0;
        StrandsPtr out;
    };
    std::map<std::string, MeshMemo> meshMemos_;  // by node
    double lastStrandMeshMs_ = 0.0;              // the view's last evaluateStrandMeshes
    std::unique_ptr<RestPose> restPose_;        // while an instance is pinned or fitted to rest
    bool graphSet_ = false;
    std::string graphError_;

    // Scaling with the scalp (setScaleWithScalp).
    double groomScale_ = 1.0;   // k
    double scaleArea_ = 0.0;    // the area last measured
    double firstArea_ = 0.0;    // the first area measured: the reference while none is set
    double scaleTransform_ = 0.0;   // setScaleTransform (0: none handed in)
    double firstTransform_ = 0.0;   // the first transform scale handed in: the reference while none is set
    double measuredArea_ = 0.0, measuredTransform_ = 0.0;  // what k was last worked out from (the notice)
    bool scaleMeasured_ = false;    // k worked out once (Transform without a host's transform: kept)
    std::string scaleNotice_;
    std::uint64_t scaleNoticeSerial_ = 0;
    std::map<std::string, double> controlFactors_;  // controlScaleFactors at k
    // The graph parsed from data_.graphJson scaled by k (scaleGraphJson), its compounds as their
    // nodes inline (`flat`: where they went); throws as loadGraphJson.
    std::unique_ptr<GraphDocument> parseScaled(const std::string& graphJson, GraphEdit::FlatCompounds* flat) const;
    // Swaps in a parsed graph: every document adopts its old one's cache.
    void installGraph(std::unique_ptr<GraphDocument> parsed);
    // Measures the scalp and, when k changes, parses the graph again and scales the guides.
    void refreshScale();
    // The transform scale handed in taken as measured with the scalp's area as it stands (the same
    // scalp handed in again: a frozen transform).
    void absorbScaleTransform();
    // parsedHierarchy_ from rawHierarchy_ at k (a copy with GuideHierarchy::setScale).
    void scaleHierarchy();
    // data_.controls with controlFactors_ applied.
    std::map<std::string, float> scaledControls() const;

    // Guides.
    bool hierarchySet_ = false;
    bool hierarchyFromText_ = false;
    std::shared_ptr<const GuideHierarchy> rawHierarchy_;     // as set (stored)
    std::shared_ptr<const GuideHierarchy> parsedHierarchy_;  // the same at k (scaleHierarchy)
    // The stored hierarchy bound again on the scalp's rest pose, when it was saved bound to
    // another topology (rootedHierarchy), with what it was made from.
    std::shared_ptr<const GuideHierarchy> reboundHierarchy_;
    std::shared_ptr<const GuideHierarchy> reboundFrom_;
    std::uint64_t reboundRest_ = 0;
    std::string hierarchyError_;
    std::uint64_t parsedVersion_ = 0;        // each new stored hierarchy
    std::uint64_t reportedVersion_ = 0;      // the parse whose error was last said
    std::uint64_t rootedVersion_ = 0;
    std::uint64_t rootedScalp_ = 0;
    bool rooted_ = false;
    HierarchyPtr hierarchy_;                 // rooted on the scalp
    std::shared_ptr<GuideHierarchy> previewHierarchy_;
    bool previewFinal_ = false;
    HierarchyPtr activeHierarchy_;           // preview else hierarchy_; posed and corrected
    HierarchyPtr posedHierarchy_;            // posed only (null when made on demand)
    std::uint64_t activeKey_ = 0;
    std::uint64_t posesHash_ = 0;            // data_.guidePoses' content hash
    bool activeValid_ = false;
    Correctives correctives_;
    std::string correctiveError_;
    std::uint64_t correctivesVersion_ = 0;
    bool liveWeights_ = false;
    std::map<int, float> weights_;
    double time_ = 0.0;

    // Sculpt nodes: the stored edits (each node's shared, with its hash, for the externals), a
    // live stroke's, the target and its shown guides (made from the node's output of that serial).
    SculptSet sculpts_;
    std::string sculptError_;
    std::map<std::string, std::pair<SculptPtr, std::uint64_t>> sculptExternals_;
    std::string previewSculptNode_;
    SculptPtr previewSculpt_;
    std::uint64_t previewSculptHash_ = 0;
    std::shared_ptr<const GuideHierarchy> previewSculptShown_;
    std::string sculptTarget_;
    std::uint64_t sculptsVersion_ = 0;     // each new sculpts text or target
    std::uint64_t sculptShownVersion_ = 0; // ... the one sculptShown_ was looked for at
    HierarchyPtr sculptShown_;
    GuidesPtr sculptShownFrom_;
    MeshPtr sculptShownOn_;
    // The target's output, from the view's document (evaluated there when the view did not reach it).
    GuidesPtr sculptOutput(GraphDocument& doc, const EvalContext& ctx);

    // Parts and flow.
    PartSet partSet_;
    std::string partError_;
    std::shared_ptr<const PartSet> previewParts_;
    PartsPtr parts_;
    std::uint64_t partsKey_ = 0;
    FlowSet flowSet_;
    std::string flowError_;
    std::shared_ptr<const FlowSet> previewFlow_;
    FlowPtr flow_;
    std::uint64_t flowKey_ = 0;
    // The parts and flow shown, bound to scalp_: the preview, the stored ones (on the cage) carried
    // onto the refined scalp (kept while they and the cage's topology, and the flow's rest surface,
    // are), or the stored ones themselves. Set by bindOverlays.
    const PartSet* shownParts_ = &partSet_;
    const FlowSet* shownFlow_ = &flowSet_;
    PartSet refinedParts_;
    std::uint64_t refinedPartsKey_ = 0;
    FlowSet refinedFlow_;
    std::uint64_t refinedFlowKey_ = 0;

    // The scalp joined with the other scalps (joinedScalp), from the scalp and the others it was
    // joined from; the scalp's earlier frames with the other scalps' points after (they keep still).
    MeshPtr joined_, joinedMain_;
    NamedScalps joinedExtras_;
    ScalpHistoryPtr joinedHistory_, joinedHistoryIn_;
    MeshPtr joinedHistoryOn_;
    ScalpHistoryPtr historyJoined();  // data_.scalpHistory on the joined scalp's points
    // The levels `joined` is refined by (the most of the scalps'), and in *levelled `joined` with
    // each scalp's own (withScalpLevels; `joined` itself for one scalp).
    int scalpLevelsOn(const MeshPtr& joined, MeshPtr* levelled) const;
    // The scalp with its maps (cageScalp_), refined (scalp_: cageScalp_ itself without subdivision).
    MeshPtr scalp_;
    MeshPtr cageScalp_;
    MeshPtr refinedFrom_;      // the cage scalp_ was refined from
    GuidesPtr guides_;         // data_.guides rooted on the refined scalp (bindRefined)
    GuidesPtr guidesIn_;
    std::uint64_t guidesOn_ = 0;
    ScalpHistoryPtr history_;  // data_.scalpHistory refined
    ScalpHistoryPtr historyIn_;
    MeshPtr historyOn_;
    std::uint64_t mappedKey_ = 0;
    MeshPtr mappedFrom_;                 // the raw scalp scalp_ was made from
    std::uint64_t mappedMapsKey_ = 0;    // ... and the maps (and preview) laid on it
    std::vector<std::string> mapErrors_;
    bool scalpMapsSet_ = false;
    std::string scalpMapsText_;
    bool scalpMapsFromText_ = false;
    ScalpMaps scalpMaps_;
    std::uint64_t scalpMapsHash_ = 0;
    std::string scalpMapsError_;
    std::string previewMapName_;
    std::shared_ptr<const std::vector<float>> previewMapValues_;
    int previewMapChannels_ = 1;
    std::shared_ptr<const TexturePreview> previewMapImage_;
    std::uint64_t previewMapVersion_ = 0;
    std::uint64_t mapViewKey_ = 0;
    std::shared_ptr<const std::vector<float>> mapViewValues_;

    bool liveStroke_ = false;
    bool strokeDensityUsed_ = false;

    // Cards.
    std::string cardMapsText_;
    std::map<std::string, std::string> cardMaps_;
    CardAtlas cardAtlas_;
    std::string cardAtlasPath_;
    std::uint64_t cardAtlasStamp_ = 0;
    double lastCardBuildMs_ = 0.0;

    std::uint64_t fingerprintKey_ = 0;  // the raw scalp's topology the check below was made for
    bool fingerprintDiffers_ = false;   // ... and whether it is another than data_.scalpFingerprint

    // Following the scalp: the inputs on the rest pose (each kept while what it comes from is).
    bool following_ = false;
    bool followNoRestSaid_ = false;           // "no rest pose" said for this scalp
    MeshPtr followRest_;                      // the mapped scalp's rest pose
    std::uint64_t followRestKey_ = 0;
    HierarchyPtr followRooted_;               // the stored guides rooted on it
    std::uint64_t followRootedKey_ = 0;
    HierarchyPtr followActive_;               // + preview, poses, correctives, at rest
    std::uint64_t followActiveKey_ = 0;
    PartsPtr followParts_;
    std::uint64_t followPartsKey_ = 0;
    FlowPtr followFlow_;
    std::uint64_t followFlowKey_ = 0;
    GuidesPtr followGuidesIn_, followGuides_;  // the flat guides, taken to rest
    MeshPtr followGuidesOn_;
    ScalpHistoryPtr followHistoryIn_, followHistory_;  // the scalp's earlier frames, in rest space
    MeshPtr followHistoryOn_;
    struct Carried {
        StrandsPtr in;
        MeshPtr from, to;
        StrandsPtr out;
    };
    Carried carried_[4];  // the view, the cards, full density, a node
    // stats()' instance count, for the strands (and their instances) it was counted on.
    mutable std::weak_ptr<const StrandSet> instancesCountedOn_;
    mutable std::weak_ptr<const InstanceSet> instancesCounted_;
    mutable std::size_t instanceCount_ = 0;

    // Simulation.
    std::map<std::string, std::unique_ptr<SimNode>> sims_;  // by node name
    double fps_ = 24.0;
    SimFeed simFeed_;
    bool simStepping_ = true;  // setSimulationStepping
    float simDensity_ = 1.0f;  // the last view's density and budget (what Simulate Range evaluates inputs at)
    double simPointBudget_ = 0.0;

    ViewResult view_;
    EvalContext lastViewContext_;  // the last view's density, budget, time and viewport (no externals)
    bool boundsStale_ = false;  // the strands or guides changed while no view wanted bounds
    // The strands each document (the view's, a live stroke's) last gave a view: its node, the
    // graph's serial of them, the view's serial of them, and the set (not kept alive).
    struct Shown {
        NodeId node = 0;
        std::uint64_t graphSerial = 0, serial = 0;
        std::weak_ptr<const StrandSet> strands;
    };
    Shown shown_[2];
    std::uint64_t strandsSerial_ = 0;
    std::exception_ptr failure_;  // what the last evaluate threw
};

// One concept: the groom evaluated (exports, renders, tests: Groom(data).evaluate(...)).
using Groom = GroomSession;

}  // namespace groomlab
