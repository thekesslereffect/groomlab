// SPDX-License-Identifier: Apache-2.0
#pragma once

// Conversion from Maya data to GroomLab core inputs, shared by the
// groomLabShape node (viewport) and the Arnold translator (render).

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <maya/MDagPath.h>
#include <maya/MObject.h>
#include <maya/MString.h>
#include <maya/MTime.h>

#include "groomlab/frame_time.h"
#include "groomlab/graph.h"
#include "groomlab/groom.h"
#include "groomlab/groom_session.h"
#include "groomlab/instances.h"
#include "groomlab/scalp_history.h"
#include "groomlab/scalp_maps.h"

namespace groomlab::maya {

// Registers operators beyond the core built-ins (e.g. ImportCurves when built
// with Alembic). Each binary (plugin, translator) holds its own registry, so
// each calls this once when it loads.
void registerHostOperators();

// Relative paths in grooms (texture maps, caches, groom files, card maps)
// resolve against the open scene's folder (where GroomLab keeps its own files,
// in a groomlab/ folder beside the scene), then the Maya project root. Those
// are read on the main thread by refreshMayaPaths (plugin load, scene
// open/new/save, workspace change) and cached, because compute may run on
// other threads.
void refreshMayaPaths();
std::vector<std::string> mayaSearchPaths();
std::shared_ptr<const PathResolver> mayaPathResolver();
std::string resolveMayaPath(const std::string& path);  // variables expanded, relative paths resolved
// A path to store (paths.h portablePath, every host's rule): relative to the first search path
// holding it (the scene's folder, else the project), else absolute (the machine's); forward
// slashes.
std::string portableMayaPath(const std::string& path);
// A node's own resolution: a node from a referenced file resolves relative paths against that
// file's folder first (where its scene kept its files), then the search paths above. The
// reference folders are read by refreshMayaPaths too (also run when references load), so
// these are safe from compute threads.
std::string referenceFolderOf(const MObject& node);  // "" for a node of the open scene
std::vector<std::string> nodeSearchPaths(const MObject& node);
std::shared_ptr<const PathResolver> nodePathResolver(const MObject& node);
std::string resolveNodePath(const MObject& node, const std::string& path);

// Points sampled along each guide curve, evenly by arc length.
constexpr unsigned kGuideCurveSamples = 24;

struct GuideCurveData {
    std::uint64_t id = 0;  // logical index of the guideCurves element
    MObject curve;         // nurbsCurve data (world space)
};

struct NodeInputCache;  // below: conversions kept across the frames of an export

// Triangulated scalp from mesh data. Null if meshData is not a mesh.
// restData, when it is a mesh with the same vertex count, becomes the rest pose;
// with another vertex count it is dropped and *restError (when given) says so.
MeshPtr toScalpMesh(const MObject& meshData, const MObject& restData = MObject::kNullObj,
                    std::string* restError = nullptr);

// Colliders (the node's colliderMeshes elements): the name a connected collider
// registers its mesh under, so the graph's ColliderMesh node of that name reads it
// as "mesh:<name>". `given` is the element's colliderName; empty, it is the
// connected mesh's transform name (the shape's parent, no namespace path).
// Empty when the mesh child is not connected.
std::string colliderNameFor(const MPlug& meshChild, const std::string& given);
// Match render (subdivision.h): the Catmull-Clark levels the renderer refines the mesh connected to
// meshPlug by: its Arnold subdivision (aiSubdivType catclark, aiSubdivIterations), else, with
// Arnold's off, its smooth mesh preview level (displaySmoothMesh, smoothLevel); 0 for neither (or
// Arnold's linear subdivision). At most kMaxSubdivisionLevels; rom says which.
int renderSubdivisionOf(const MPlug& meshPlug, std::string* from);
// mesh with that render subdivision (ScalpMesh::setRenderSubdivision, on a copy); itself when it has it.
MeshPtr withRenderSubdivision(const MeshPtr& mesh, int levels, const std::string& from);
// The node's connected colliders as world-space meshes by name (from the plugs,
// at the current time or the time of an active MDGContextGuard), each with its connected rest
// mesh (colliderRestMesh) as its rest pose. With `cache`, the meshes of the last call when their
// data reads the same.
std::map<std::string, MeshPtr> colliderMeshesFromNode(const MObject& node, NodeInputCache* cache = nullptr);
// Several scalps on one groom (scalp_pieces.h): the node's connected extraScalps elements as
// world-space meshes by name, in their order, each with its connected rest mesh
// (extraScalpRestMesh) and its render subdivision; an empty extraScalpName is the connected
// mesh's transform name (colliderNameFor's rule). Names that are not scalp names (validScalpName)
// or are taken already are left out. With `cache`, the meshes of the last call when their data
// reads the same.
NamedScalps extraScalpsFromNode(const MObject& node, NodeInputCache* cache = nullptr);
// The node's scalp (inMesh, no painted maps) joined with its other scalps (combineScalps): what
// the groom's session binds to before refining, for commands rooting things on the scalp.
MeshPtr joinedScalpFromNode(const MObject& node);

// World-space points evenly spaced by arc length along nurbsCurve data
// (the data's matrix is applied). Empty if curveData is not a curve.
std::vector<Vec3f> sampleCurveData(const MObject& curveData, unsigned samples = kGuideCurveSamples);

// Guides from curve data. The end nearer the scalp becomes the root, and
// guides are rooted on the scalp when one is given.
GuidesPtr toGuides(const std::vector<GuideCurveData>& curves, const ScalpMesh* scalp);
// Driver curves (driverCurves elements) as DriveGuides nodes read them: each sampled evenly by arc
// length, its ID its logical index, nothing rooted or turned (null when there are none).
GuidesPtr toDrivers(const std::vector<GuideCurveData>& curves);
// The node's driver curves as the current DG context shows them (an MDGContextGuard picks a time).
GuidesPtr driverCurvesFromNode(const MObject& node);

// A simulated (or otherwise animated) curve posing one hierarchy guide.
struct SimCurveData {
    std::uint64_t guideId = 0;
    MObject curve;  // nurbsCurve data (world space)
};

// An open guide cache (an Alembic curves reader and what it reads), for toGuidePoses.
struct GuideCacheReader;

// Poses for hierarchy guides at `seconds`: an Alembic guide cache (curves
// whose strandId is the guide ID), overridden by connected sim curves. Null
// when there are none; on a cache error sets *error and skips the cache.
// `reader`, when given, keeps the cache open from call to call (an export's frames).
GuidesPtr toGuidePoses(const std::vector<SimCurveData>& curves, const std::string& cacheFile,
                       const std::string& cacheObject, double seconds, std::string* error,
                       std::shared_ptr<GuideCacheReader>* reader = nullptr);

// A groom node's guide selection (guideSelection: decimal guide IDs) and its
// soft selection radius (softSelectRadius; 0: hard selection).
std::vector<GuideId> readGuideSelection(const MObject& node);
// The guide groups hidden in the viewport (hiddenGuideGroups: names, space separated).
std::set<std::string> readHiddenGuideGroups(const MObject& node);
float readSoftSelectRadius(const MObject& node);

// Parses the node's guideData JSON and evaluates it on the scalp. Returns an
// empty hierarchy for empty text; on malformed text returns null and sets *error.
// `scale`: the groom's (groomScaleOf): the guides shown at it (GuideHierarchy::setScale), and edits
// made in world space stored at the groom's own size.
std::shared_ptr<GuideHierarchy> toHierarchy(const std::string& json, const ScalpMesh* scalp, std::string* error, float scale = 1.0f);
// What a groom node is scaled by with its scalp now (its groomScale output, read in the normal
// context: the groom is evaluated when it is dirty); 1 for a node without one.
float groomScaleOf(const MObject& node);

// A nurbsCurve's (transform or shape name) world space curve data; false
// when it isn't one.
bool curveWorldData(const MString& name, MObject& data);

// Control attributes: dynamic numeric attributes named ctl_<name> on a
// groomLabShape drive the graph's Control nodes named <name> (keyable sliders).
constexpr const char* kControlPrefix = "ctl_";
bool isControlAttribute(const MObject& attribute);
// {name: value} of the node's control attributes (at the current time or context).
std::map<std::string, float> readControls(const MObject& node);
// The node's control attributes (numeric, not arrays or compounds: what readControls reads),
// and their values: readControls without walking every attribute (hosts keep the list).
std::vector<MObject> controlAttributes(const MObject& node);
std::map<std::string, float> readControls(const MObject& node, const std::vector<MObject>& attributes);

// The scalp's motion over the last frames, for the graph's Jiggle nodes (graph.h ScalpHistory).
// Maya keeps no mesh data for other times, so the scalp's earlier frames are read by evaluating
// the node's inMesh plug in those times' contexts (MDGContextGuard), and kept by the core's
// sampler (scalp_history.h: a frame read once, or seen as the current frame, is not read again).
using ScalpPointCache = ScalpHistorySampler;

// Fingerprints of mesh data, into `h`. Points: the points as stored, the face count and the
// matrix (what moves when a scalp deforms or its transform animates). Topology: vertex and face
// counts, each face's vertices, the current UV set's name and UV count, the color sets' names
// (what a scalp keeps from frame to frame: a match lets only the points be read again).
// Attributes: everything toScalpMesh reads but the points and matrix (topology, UVs and their
// assignment, color values).
std::uint64_t meshPointsKey(std::uint64_t h, const MObject& meshData);
std::uint64_t meshTopologyKey(std::uint64_t h, const MObject& meshData);
std::uint64_t meshAttributesKey(std::uint64_t h, const MObject& meshData);
// World-space vertex positions of mesh data (its matrix applied); empty if it is not a mesh.
std::vector<Vec3f> meshWorldPoints(const MObject& meshData);

// The scalp's (inMesh plug's) world-space points at `time`, evaluated in that context; empty
// without a mesh.
std::vector<Vec3f> scalpPointsAt(const MObject& node, const MTime& time);
// The scalp's transform scale at `time` (GroomSession::setScaleTransform): the uniform scale of the
// inMesh mesh's world matrix, times its rig's root joint's (the skinCluster influence nearest the
// top of the DAG) where it is skinned; 0 without a mesh.
double scalpTransformScaleAt(const MObject& node, const MTime& time);
// The scalp joined with the node's other scalps at `time` (each read in that time's context, as
// scalpPointsAt reads inMesh): inMesh's points, then each of `extras`' (the other scalps as joined,
// by name: their extraScalps element's mesh). An other scalp that does not read with its points'
// count keeps its points now. Empty without inMesh.
std::vector<Vec3f> joinedScalpPointsAt(const MObject& node, const MTime& time, const NamedScalps& extras);
// The joined scalp's points now: `scalp`'s then each of `extras`' (what joinedScalpPointsAt reads
// at another time).
std::vector<Vec3f> joinedScalpPoints(const MeshPtr& scalp, const NamedScalps& extras);

// A groom's Simulate nodes (simulation.h) in a session of Maya's: the scene's frame rate, how the
// session reads the scalp at another time (the inMesh plug in that time's context: the frames a
// simulation steps over on the way to the one shown) and the bakes the node names (its simCaches
// attribute: {"<Simulate node>": {"file", "object", "stamp"}}, paths as the node resolves them),
// read again only when the text changed. A bake that does not read is said in `error` and left
// out (the node simulates).
struct SimCacheInputs {
    std::string text;                  // simCaches as last read
    std::vector<std::string> attached;  // the nodes given a bake
    std::string error;
};
void prepareSimulation(const MObject& node, GroomSession& session, SimCacheInputs& inputs);
// The bakes alone, from a simCaches text, paths resolved by `paths` (the groom's background
// evaluation: no plug read, no feed).
void attachSimulationCaches(const std::shared_ptr<const PathResolver>& paths, const std::string& text, GroomSession& session,
                            SimCacheInputs& inputs);

// Maya's time by every host's rule (frame_time.h): the scene's frame rate (MTime's ui unit, an
// NTSC rate exact), a time's seconds (its frame at that rate, frameToSeconds: the CLI's, the
// procedural's and the C API's seconds for the same frame, to the bit), and seconds (the core's: a
// Simulate node's frame) back as a time in the ui unit.
double mayaFramesPerSecond();
double mayaSeconds(const MTime& time);
MTime mayaTime(double seconds);

// The node's scalp at the `frames` frames before `now` (one frame of the scene's time unit apart)
// as a ScalpHistory, `current` being the scalp's points now. Frames `cache` holds are reused,
// unless `reread` (the animation may have changed: every frame is read again, and when one reads
// differently the other remembered frames are dropped); the current frame is remembered, and when
// it reads differently from what was remembered for that time the animation changed and the cache
// is dropped. A frame with another point count than `current`
// (a topology change) counts as still. The same request with nothing changed returns the same
// history (its hash is not worked out again). Then the scalp at `loopFrames` (frames, the
// session's scalpHistoryLoopFrames: a looping Jiggle's), as the history's loop frames. With
// `extras` (the groom's other scalps, as joined) every frame is the joined scalp's
// (joinedScalpPointsAt; `current` the joined points now, joinedScalpPoints): each other scalp's own
// earlier frames, so hair on a moving one jiggles too.
ScalpHistoryPtr sampleScalpHistory(const MObject& node, const MTime& now, int frames, const std::vector<Vec3f>& current,
                                   ScalpPointCache& cache, bool reread, const std::vector<double>& loopFrames = {},
                                   const NamedScalps& extras = {});

// What an export (or the Arnold translator's motion steps) converted from Maya data on its last
// frame: the scalp, its vertex maps, the guide curves and the colliders are converted again only
// when their data reads differently (points, topology, UVs, color sets, matrices; CVs and knots),
// and a guide cache stays open. The same data gives the same conversions, so the results are
// those of converting every frame.
struct NodeInputCache {
    bool scalpRead = false;
    // The scalp's fingerprints when it was last converted in full (toScalpMesh): its topology
    // and its rest mesh's (meshTopologyKey), its UVs and colors (meshAttributesKey); its points'
    // and its rest mesh's points' (meshPointsKey) as last read.
    std::uint64_t topologyKey = 0, restTopologyKey = 0, attributesKey = 0, pointsKey = 0, restKey = 0;
    // The next read checks the UVs and colors before taking new points on the old topology
    // (refreshStoredInputs: anything may have been edited since).
    bool verifyScalp = false;
    MeshPtr rawScalp;  // toScalpMesh's, or its points moved (ScalpMesh::withPoints)
    bool mapsRead = false;
    std::string mapsText;  // scalpMaps
    std::shared_ptr<const ScalpMaps> parsedMaps;  // mapsText parsed (null when it does not parse)
    MeshPtr mapsOn;        // the raw scalp the maps were laid on
    MeshPtr scalp;         // with the vertex maps
    MapCatalog maps;
    // The control attributes (controlAttributes), listed once: readControls without the walk.
    bool controlsListed = false;
    std::vector<MObject> controlAttributes;
    bool guidesRead = false;
    std::uint64_t curvesKey = 0;
    MeshPtr guidesOn;  // the scalp the guides were rooted on
    GuidesPtr guides;
    bool collidersRead = false;
    std::uint64_t collidersKey = 0;
    std::map<std::string, MeshPtr> colliders;
    bool scalpsRead = false;  // extraScalps
    std::uint64_t scalpsKey = 0;
    NamedScalps scalps;
    std::shared_ptr<GuideCacheReader> guideCache;
    // Scaling with the scalp as the node evaluates it (readScale), read once per export.
    bool scaleRead = false;
    bool scaleOn = true;
    int scaleSource = 0;
    double scaleMeasure = 0.0, scaleReference = 0.0, scaleTransform = 0.0, scaleTransformReference = 0.0, scaleK = 1.0;
};

// Scaling with the scalp as the groom node evaluates it (groom_shape.h scaleSource): what it follows
// (on: not Off), the scalp's world area and transform scale at the start frame the node measured
// (its outputs, read in the normal context: the groom is evaluated when they are dirty), the
// references it is 1x at (scaleReference and scaleTransformReference, else the first ones the node
// measured) and its scale k. With a cache, read once.
struct ScaleRead {
    bool on = true;
    ScaleSource source = ScaleSource::Area;
    double measure = 0.0, reference = 0.0;
    double transform = 0.0, transformReference = 0.0;
    double k = 1.0;
};
ScaleRead readScale(const MObject& node, NodeInputCache* cache = nullptr);

// A groomLabShape node's inputs read from its plugs at the current time
// (or the time of an active MDGContextGuard). Corrective layers with a weight
// slot carry that weight: its keys sampled into a weight curve (so groom files
// and renders fade them as keyed), or its value when it isn't keyed. `cache`: see NodeInputCache.
// With `scalpMapsText`, the scalp is the raw one and the scalpMaps text comes back there (for a
// GroomSession to lay: NodeEvaluator); otherwise the maps are laid on it as color sets and their
// catalog is in the data (groom files carry them so).
GroomData groomDataFromNode(const MObject& node, NodeInputCache* cache = nullptr, std::string* scalpMapsText = nullptr);
// What changes with time, for the frames (or motion keys) after `cache` was filled by
// groomDataFromNode: the scalp (raw: no maps laid; NodeEvaluator's session lays them), the guide curves, the guide poses, the controls (their attributes as
// listed then), the colliders and the time. The stored texts (graph, guideData, partData,
// flowData, correctiveData) are left empty: they do not change with time, and the caller holds
// them from the first read. No JSON is parsed.
GroomData groomMotionFromNode(const MObject& node, NodeInputCache& cache);
// The node's scalp as converted from its mesh, with no painted maps (rawScalpFromNode); with them
// (scalpFromNode: as groomDataFromNode gives it) and, when given,
// the map catalog. `readMaps` false (with a cache that read them): the maps as last read.
MeshPtr rawScalpFromNode(const MObject& node, NodeInputCache* cache = nullptr);
MeshPtr scalpFromNode(const MObject& node, NodeInputCache* cache = nullptr, MapCatalog* maps = nullptr,
                      bool readMaps = true);
// Only the node's guide poses (as groomDataFromNode gives them: the guide cache, sim curves).
GuidesPtr guidePosesFromNode(const MObject& node, NodeInputCache* cache = nullptr, std::string* error = nullptr);

// (useLiveWeights, groom_session.h: corrective layers with a weight slot take the keyable weight's value now.)
// The value of the groom's correctiveWeight[slot] (1 when it has no such element).
float correctiveWeightPlug(const MObject& node, int slot);

// The mesh an Instance prototype's name (a transform or shape) refers to, as the viewport and the
// Arnold translator find it; false when the scene has no such mesh.
bool prototypePath(const std::string& name, MDagPath* path);
// Reads the prototype mesh `name` from the scene into `out` (object space, one vertex per
// face-vertex so its normals shade as Maya shades the mesh, the current UV set's uvs). Not found:
// `out` is empty and says so (found false). The signature is left as it was.
void readInstancePrototype(const std::string& name, InstancePrototype& out);

// Evaluates a groomLabShape node from its plugs, outside of its compute
// (render-time and export path), at successive times (motion steps, export
// frames) with one GroomSession: the graph and guides are parsed once and the
// graph's cache carries over, so only what the time changes re-evaluates.
// After the first evaluation only what changes with time is read from the node
// (groomMotionFromNode) and handed to the session's setters, which do nothing
// for what reads the same; refreshStoredInputs hands it the stored texts again.
class NodeEvaluator {
public:
    // cacheIntermediates false: lowest peak memory for a single evaluation.
    explicit NodeEvaluator(const MObject& node, bool cacheIntermediates = true);
    ~NodeEvaluator();

    // At the node's current time (or an active MDGContextGuard's). Returns
    // null and sets *error on failure; *scalp, when given, receives the
    // scalp the strands grew on.
    StrandsPtr evaluate(float density, std::string* error, MeshPtr* scalp = nullptr, bool buildCards = false,
                        bool gameExport = false);
    // Evaluates this graph (JSON) instead of the node's own from the next evaluate on (an
    // export of another level of detail); empty: the node's. After a first evaluation the graph
    // is swapped into the same session (GroomSession::setGraphJson): with caching on, the nodes
    // the graphs share are not evaluated again (an LOD's Cards node and what follows it re-run).
    void setGraphJson(std::string graphJson);
    // Reads the node's stored inputs again (graph, guideData, partData, flowData,
    // correctiveData, scalpMaps, the control attributes) for the next evaluate: a render's next
    // frame or an interactive render's update after an edit. Each goes to the session's setter:
    // an edited graph is swapped in, edited guides, parts, flow or correctives parsed again,
    // edited maps laid on the scalp again; what they do not reach stays cached. Nothing
    // changed: the cache stays as it is.
    void refreshStoredInputs();
    // The guide hierarchy of the last evaluate (posed and corrected); null without one.
    HierarchyPtr hierarchy();
    // The groom's look (GroomSession::hairLook) as of the last evaluate; false before one.
    bool hairLook(HairLook* look) const;

private:
    MObject node_;
    bool cacheIntermediates_;
    std::unique_ptr<GroomSession> session_;
    std::string graphJson_;
    bool graphPending_ = false;  // graphJson_ (or the node's) to swap into session_ on the next evaluate
    std::string nodeGraphJson_;  // the node's own graph as last read
    ScalpPointCache scalpPast_;  // the scalp's earlier frames (Jiggle), kept across the evaluated times
    NodeInputCache inputs_;      // Maya data converted on the last evaluated time
    SimCacheInputs sims_;        // the bakes the Simulate nodes read
};

// One evaluation (NodeEvaluator for a single time); `graphJson`, when given, instead of the node's graph;
// `gameExport`: a game asset (cards, scalp bakes), the Output's rootDepth left off.
StrandsPtr evaluateGroomNode(const MObject& node, float density, std::string* error, MeshPtr* scalp = nullptr,
                             bool buildCards = false, const std::string* graphJson = nullptr, bool gameExport = false);

}  // namespace groomlab::maya
