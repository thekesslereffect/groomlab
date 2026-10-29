// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>

#include <maya/MBoundingBox.h>
#include <maya/MCacheSchema.h>
#include <maya/MEvaluationNode.h>
#include <maya/MMessage.h>
#include <maya/MNodeCacheDisablingInfo.h>
#include <maya/MNodeCacheSetupInfo.h>
#include <maya/MObjectArray.h>
#include <maya/MPxData.h>
#include <maya/MPxSurfaceShape.h>
#include <maya/MPxSurfaceShapeUI.h>

#include "groomlab/async_evaluation.h"
#include "groomlab/correctives.h"
#include "groomlab/card_files.h"
#include "groomlab/cards.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom_session.h"
#include "groomlab/scalp_maps.h"
#include "maya_inputs.h"

namespace groomlab::maya {

// One evaluation of the groom as the viewport draws it: what the shape's outFrame holds, so Maya's
// cached playback keeps it frame by frame and the viewport draws a cached frame without evaluating
// (docs/DEVELOPING.md, Evaluation Manager and Cached Playback). Never changed once made: frames
// share what did not change between them (a static groom's strands, the parts...).
struct GroomFrame {
    ViewResult view;  // strands, scalp, hierarchies, parts, flow, cards (no messages)
    MBoundingBox bounds;
    // Moved when what they name changed since the evaluation before it; taken from one counter for
    // the whole process, so frames from the live evaluation, the background one and Maya's cache
    // never share a version for different content (the draw override compares them).
    std::uint64_t version = 0, strandsVersion = 0, hierarchyVersion = 0, cardsVersion = 0, strandMeshVersion = 0;
    std::uint64_t liveEvaluation = 0;  // the live evaluation that made it (GroomShape::evaluationCount); 0: a background one
    double seconds = 0.0;
    std::string errors;  // lastError
};
using FramePtr = std::shared_ptr<const GroomFrame>;

// outFrame's data type ("groomLabFrameData"): a GroomFrame, shared (a copy is a pointer copy).
class GroomFrameData : public MPxData {
public:
    static const MTypeId kTypeId;
    static const MString kTypeName;
    static void* creator() { return new GroomFrameData(); }
    void copy(const MPxData& src) override { frame = static_cast<const GroomFrameData&>(src).frame; }
    MTypeId typeId() const override { return kTypeId; }
    MString name() const override { return kTypeName; }
    FramePtr frame;
};

// The groomLabShape node: one node per groom holding the whole graph
// internally (Yeti-style) rather than one DG node per operator.
//
// Inputs feed the graph as external inputs: the scalp mesh as "scalp" and the
// guide curves as "guides". The graph is stored as JSON in the `graph`
// attribute; empty or "{}" means the default graph.
//
// Everything is world space: the scalp comes in as a world mesh, the guides
// and strands are grown on it, and the shape draws them as they are (no
// transform is applied; create_groom locks the groom's transform). The tools
// (brush, select) work in the same space.
//
// The shape is the Maya host of a GroomSession (groom_session.h, docs/SESSION.md): it reads
// its plugs (only those dirtied), converts Maya data, hands both to the session's setters and
// asks it for the view; the session evaluates, caches, previews and counts. What stays here is
// Maya's: plugs, conversions, reporting, the out meshes, bakes to files and drawing's inputs.
class GroomShape : public MPxSurfaceShape {
public:
    static const char* kTypeName;
    static const MTypeId kTypeId;
    static const MString kDrawDbClassification;

    // Inputs
    static MObject aInMesh;          // scalp mesh (world space)
    static MObject aSimGuides;       // array of {simCurve, simGuideId}: curves posing hierarchy guides (simulation)
    static MObject aSimCurve;
    static MObject aSimGuideId;
    static MObject aGuideCache;      // Alembic curves posing hierarchy guides by strandId (external simulation)
    static MObject aGuideCacheObject;
    static MObject aSimCaches;       // the Simulate nodes' bakes (JSON: node -> file, object, stamp; groom_file.h simCachesToJson)
    static MObject aTransferMarkers;  // the last transfer's marker pairs (JSON, transfer.h); read by no evaluation
    static MObject aRestMesh;        // optional undeformed scalp (same vertices): stable strands on a deforming scalp
    // Follow the scalp (follow_scalp.h): evaluate on the rest mesh, carry each strand to the scalp
    // by its root's rigid frame. Off by default.
    static MObject aFollowScalp;
    static MObject aScalpTopology;   // the topology the groom was made on (an imported file's scalp fingerprint; "": not known)
    // Scale with the scalp (GroomSession::setScaleSource): the groom grows with its scalp. Scale
    // Source Surface Area (0, the default): k = sqrt(area / scaleReference), the area the scalp's
    // world surface at the scene's start frame (the rest mesh sized to it: withRestArea); Transform
    // Only (1): k = the scalp's transform scale at the start frame (its world matrix's, its rig's
    // root joint's: scalpTransformScaleAt) / scaleTransformReference; Off (2): k = 1. The
    // references 0: the first one measured (Set Scale Reference and a save store them). groomScale
    // (k), scaleArea and scaleTransform (what was measured) are outputs for the Attribute Editor
    // and groomlab.set_scale_reference.
    static MObject aScaleSource;
    static MObject aScaleReference;
    static MObject aScaleTransformReference;
    static MObject aGroomScale;
    static MObject aScaleArea;
    static MObject aScaleTransform;
    // Colliders: array of {colliderMesh (world space), colliderName, colliderRestMesh}. Each is the
    // graph's external mesh "mesh:<name>" (a ColliderMesh node reads it, a Collide node pushes
    // strands out of it); an empty name is the connected mesh's transform name. colliderRestMesh,
    // optional: the same mesh undeformed (any space: groomlab.add_collider connects a deformed
    // mesh's original shape, in object space), its rest pose: collisions with a deforming collider
    // ask its rest pose's distance field, made once, instead of one made for every frame.
    static MObject aColliderMeshes;
    static MObject aColliderMesh;
    static MObject aColliderName;
    static MObject aColliderRestMesh;
    // Several scalps on one groom (scalp_pieces.h): array of {extraScalpMesh (world space),
    // extraScalpName, extraScalpRestMesh}, joined after inMesh in index order. A Scalp node whose
    // input is the name grows on that mesh alone; an empty name is the mesh's transform name.
    // extraScalpRestMesh, optional: the same mesh undeformed (its rest pose, as restMesh is inMesh's).
    static MObject aExtraScalps;
    static MObject aExtraScalpMesh;
    static MObject aExtraScalpName;
    static MObject aExtraScalpRestMesh;
    static MObject aGuideCurves;     // array of nurbsCurve (world space)
    static MObject aTime;            // scene time
    static MObject aGraph;           // graph JSON (stored string)
    static MObject aGuideData;       // guide hierarchy JSON (stored string, edited by groomLabGuides)
    static MObject aPartData;        // parts JSON (stored string, edited by groomLabParts)
    static MObject aGuideSelection;   // selected guide IDs (decimal strings; GT-5), stored
    static MObject aSoftSelectRadius; // soft selection falloff radius, scene units (0: hard)
    static MObject aStrandColorMode;   // viewport strand colors (VP-1): solid, colorSet, attribute, random, clump, density
    static MObject aStrandColorSource; // color set (colorSet) or strand channel (attribute, clump)
    static MObject aStrandColorMin;    // attribute range mapped onto the color ramp
    static MObject aStrandColorMax;
    static MObject aDisplayRibbons;    // strands as shaded ribbons with their widths (VP-3)
    static MObject aRibbonWidthScale;  // the ribbons' widths times this, and the lines' (width as coverage)
    // Strand lines with each point covering the share of a pixel its width covers (display.h
    // lineCoverage, hashed): tapered tips fade, thin strands read light, as ribbons of the true
    // width show, and dense hair builds up to what its strands cover together.
    static MObject aDisplayWidthCoverage;
    // ... at the render's density: a thinned preview's strands each cover what the strands they
    // stand for would (display.h lineCoverageAtDensity, at the density the view was evaluated at),
    // so a preview reads as dense as the render.
    static MObject aDisplayCoverageDensity;
    // Screen-space level of detail (strand_lod.h; on by default): with width as coverage, each
    // clump draws as few of its strands as cover its pixels to within clumpLodError, each standing
    // for the rest, and a clump thinner than a pixel as one line; the strands chosen per view (the
    // camera's distance), the same while it orbits. The viewport only: renders never thin.
    static MObject aDisplayClumpLod;
    static MObject aClumpLodError;  // the coverage error allowed, relative (0.175: the widened strands cover at most 0.35)
    // Lit strands (solid color mode): Maya's hair tube shading with the scene's
    // lights, shadows and ambient occlusion. The color is the assigned
    // material's (aiStandardHair melanin approximated) when useMaterialColor
    // is on and a material is assigned, else displayColor.
    // Render LOD (lod.h): thin strands with distance from the render camera
    // (lodDistance, lodNear, lodFar, lodMinDensity, lodWidthCompensation) and
    // drop strands outside its view (cameraCull, cullMargin) or rooted facing away from it
    // (backfaceCull, backfaceAngle: degrees of padding past side-on).
    static MObject aLodDistance;
    static MObject aLodNear;
    static MObject aLodFar;
    static MObject aLodMinDensity;
    static MObject aLodWidthCompensation;
    static MObject aCameraCull;
    static MObject aCullMargin;
    static MObject aBackfaceCull;
    static MObject aBackfaceAngle;
    // Shot sculpting (correctives.h): corrective layers over the animated
    // groom (correctiveData, JSON); with shotSculpt on, brushes record into
    // the layer shotLayer instead of editing the guides.
    // Flow arrows on the scalp (flow.h, JSON), read by the Flow node; drawn
    // with a preview of the field when displayFlow is on.
    static MObject aFlowData;
    static MObject aDisplayFlow;
    static MObject aCorrectiveData;
    // Sculpt nodes (sculpt.h): their brushed edits by node name (sculptData, JSON), and the Sculpt
    // node brushes shape ("" none: the guides or a corrective), whose output the viewport shows as
    // the guides meanwhile.
    static MObject aSculptData;
    static MObject aSculptNode;
    static MObject aCorrectiveWeight;  // keyable per-layer weights (multi; a layer's `slot` picks its element)
    static MObject aShotSculpt;
    static MObject aShotLayer;
    // Guides: each level in the UI theme's color for it (a guide group's own color wins),
    // guideWidth pixels wide.
    static MObject aGuideWidth;
    static MObject aGuidePoints;       // dots on guide points: off, roots and tips, all
    static MObject aGuideSmoothing;    // guides draw smooth: pieces per segment (1: straight)
    static MObject aHiddenGuideGroups; // guide groups hidden in the viewport (names): not drawn, brushed or selected
    static MObject aGuideGroupColors;  // guide groups' own viewport colors (name:#rrggbb)
    static MObject aDisplayLighting;
    static MObject aDisplayShadows;  // 0 off, 1 cast (on the scalp, body, floor), 2 cast and receive (self-shadowing)
    // How the lit strands and ribbons shade (hair_shading.h): 1 GroomLab's hair (the default:
    // Marschner's lobes from the strands' pigment, as the standalone app and Arnold show them, lit
    // by the viewport's lights), 0 Maya's hair tube shading (as paint effects tubes draw).
    static MObject aDisplayHairShading;
    // GroomLab's hair: how opaque the hair is to its own light (its shadows, ambient occlusion and
    // the light scattered through it, from a GPU grid of its density; 1 as rendered, 0 none).
    static MObject aDisplaySelfShadowing;
    // GroomLab's hair: each strand's own highlight tilt, width and pigment, a little apart from its
    // neighbors' (hashed from its ID), on top of HairColor's randomness; 0 none, 1 hair's usual.
    static MObject aDisplayStrandVariation;
    static MObject aDisplayColor;
    static MObject aUseMaterialColor;
    // Sculpting display (UX): while a brush stroke is live, strands draw at
    // sculptDensity x previewDensity and dimmed (sculptDisplay 1) or not at
    // all (2); 0 leaves them alone.
    static MObject aSculptDisplay;
    static MObject aSculptDensity;
    // Guides: isolateGroup names a group drawn at full color with its child groups (others
    // dim); guideColorMode 0 colors them by group, 1 by the layer guideColorLayer (black to
    // white ramp).
    static MObject aIsolateGroup;
    static MObject aGuideColorMode;
    static MObject aGuideColorLayer;
    static MObject aRenderMode;      // Arnold: 0 curves (evaluated in Maya), 1 groom file (groomlab_procedural)
    static MObject aGroomFile;       // Arnold: the .groom file the procedural renders
    // Render overrides: applied to renders alone (the Arnold translator and procedural; exports
    // with groomLabExport -renderOverrides), never the viewport. renderDensity: the share of the
    // strands rendered (the preview density's thinning: the same look, fewer strands); width and
    // length scales: each strand's widths, and its points about its root (render_overrides.h).
    static MObject aRenderDensity;
    static MObject aRenderWidthScale;
    static MObject aRenderLengthScale;
    // The viewport drawing the groom as a render shows it (on while Maya's Hardware 2.0 renders it:
    // groomlab.render_hardware2): at renderDensity with no point budget, renderOnly nodes on, and
    // the render width and length scales applied. Hidden, never stored.
    static MObject aRenderView;
    static MObject aGraphUi;        // graph editor layout JSON (stored; does not affect evaluation)
    static MObject aViewNode;  // the graph node the viewport shows ("": the output); renders use the output
    static MObject aLocked;    // brushes and parts leave a locked groom alone (a finished groom)
    static MObject aDisplayInstances;  // instanced geometry (Instance node) drawn as small markers
    // Game cards: the Cards nodes' cards, built from an evaluation at
    // cardDensity while outCards is connected or displayCards shows them.
    static MObject aCardDensity;
    static MObject aOutCards;
    // Feathers for engines (feathers.h): the Propagate nodes' rachis tubes as a mesh (UV set
    // "map1": u around, v root to tip; "featherId": u the feather's index, v its groom group), from
    // an evaluation at full density while connected, to skin into the character.
    static MObject aOutRachis;
    // The StrandMesh nodes' strands as one polygon mesh (tubes or ribbons; strand_geometry.h), made
    // while connected from an evaluation at each node's density at the current time: it follows
    // the scalp, Simulate and keys. A frame whose strands are the same ones with the same point
    // counts changes the points alone, so caches (Alembic) see a deforming mesh.
    static MObject aOutStrandMesh;
    static MObject aDisplayCards;  // cards view: the Cards nodes' cards instead of the strands
    // The StrandMesh nodes' meshes instead of the strands (on by default; drawn by the groom unless
    // a mesh on outStrandMesh shows them).
    static MObject aDisplayStrandMesh;
    static MObject aCardMaps;      // the last bake's maps ("<map>=<path>;..."): the cards view shows them
    static MObject aScalpMaps;     // maps painted on the scalp (scalp_maps.h): vertex maps and texture maps (stored JSON)
    static MObject aScalpMapDisplay;  // the map the viewport shows on the scalp ("": none)
    static MObject aScalpMapChannel;  // what of a color map it shows: "rgb" (its color), or one channel "r" "g" "b" "a" (gray)
    // Cards view: the cards with their baked maps (cut out by the coverage); off, flat in
    // the theme's card color, to look at the geometry.
    static MObject aDisplayCardTextures;
    // Guides, parts and flow drawn over the strands, tubes and cards (pulled toward the
    // camera by the hair's depth in the viewport; the head still hides what is behind it).
    static MObject aDisplayOverlaysOnTop;
    static MObject aPreviewDensity;  // 0..1, viewport only (PG-4)
    static MObject aPreviewPointBudget;  // millions of points the viewport may hold; the preview thins past it

    // Display only
    static MObject aDisplayGuides;  // draw hierarchy guides, colored by group
    static MObject aDisplayTubes;   // draw guide tubes as rings
    static MObject aDisplayHair;    // draw the hair (strands, instances, cards): the Groom Tools' eye

    // Internal: bumped by the brush tool to re-evaluate with its preview.
    static MObject aBrushTick;

    // Outputs
    static MObject aOutStrandCount;
    static MObject aLastError;       // why the last evaluation failed or was partial ("" when fine)
    // The evaluation as the viewport draws it (GroomFrame), made with outStrandCount and lastError:
    // what cached playback keeps of the groom (configCache) and the viewport reads.
    static MObject aOutFrame;

    static void* creator();
    static MStatus initialize();
    GroomShape();
    ~GroomShape() override;
    void postConstructor() override;

    // In the normal context (the current time) the live evaluation: the session the viewport,
    // the tools and the commands share. In any other (cached playback filling frames in the
    // background, on its own threads, at other times) the background evaluation: a session of its
    // own, fed from the data block in that context, which leaves the live one alone.
    MStatus compute(const MPlug& plug, MDataBlock& data) override;
    // The Evaluation Manager does no dirty propagation while it plays: the plugs it evaluates
    // dirty mark the conversions and stored texts to read again, as setDependentsDirty does.
    MStatus preEvaluation(const MDGContext& context, const MEvaluationNode& evaluationNode) override;
    // Never alongside other nodes: the live evaluation pulls the scalp at other times (the
    // Simulate nodes' frames on the way, Jiggle's history: MDGContextGuard inside compute).
    SchedulingType schedulingType() const override { return SchedulingType::kUntrusted; }
    // Cached playback keeps the groom's frames (kWantToCacheByDefault; configCache: outFrame,
    // outStrandCount, lastError and the meshes), unless the groom depends on the frames before
    // the one shown (GroomSession::readsEarlierFrames over the playback range: a Simulate node
    // without a bake covering it, a Jiggle node): then it is left out, evaluated live as it plays
    // while the rest of the scene is cached. Never disables cached playback (that is its safe mode).
    void getCacheSetup(const MEvaluationNode& evalNode, MNodeCacheDisablingInfo& disablingInfo,
                       MNodeCacheSetupInfo& cacheSetupInfo, MObjectArray& monitoredAttributes) const override;
    void configCache(const MEvaluationNode& evalNode, MCacheSchema& schema) const override;
    MStatus setDependentsDirty(const MPlug& plug, MPlugArray& affected) override;
    // A card mesh hooked up to outCards (or removed): the viewport switches who draws the cards.
    MStatus connectionMade(const MPlug& plug, const MPlug& otherPlug, bool asSrc) override;
    MStatus connectionBroken(const MPlug& plug, const MPlug& otherPlug, bool asSrc) override;
    bool isBounded() const override { return true; }
    MBoundingBox boundingBox() const override;

    // The frame the viewport draws now: outFrame, evaluated live when dirty, else as it is (a
    // frame restored by cached playback, made in the background: no evaluation). Null before
    // the first evaluation.
    FramePtr frame();
    // The live session holds the frame shown now: evaluated (live) when dirty, and again when the
    // frame shown came from cached playback, so tools and commands work on what the viewport
    // shows (the hierarchy, the scalp, the graph's cache at this time).
    void ensureEvaluated();
    // The drawn frame's strands (frame()), where they are (carried to the scalp, on demand, when the
    // frame hands them on at rest: ViewResult::placed); may be null.
    StrandsPtr evaluatedStrands();
    // The drawn frame's strands as the session handed them on: at rest while evaluatedFollow() is set
    // (their counts, IDs, widths and channels are the placed ones'), else evaluatedStrands().
    StrandsPtr handedStrands();
    // What carries the drawn frame's strands to the scalp (ViewResult::follow), or null.
    FollowCarryPtr evaluatedFollow();
    // The viewport carries followed strands itself (on the GPU: GroomSubSceneOverride): evaluations
    // from now on hand them on at rest (ViewRequest::hostCarries). Any thread.
    void setViewportCarries(bool on) { viewportCarries_.store(on, std::memory_order_relaxed); }
    bool viewportCarries() const { return viewportCarries_.load(std::memory_order_relaxed); }

    // The live evaluation (normal context): outStrandCount, lastError and outFrame.
    MStatus computeStrands(MDataBlock& data);
    // A message for the Script Editor, once per change (compute runs on every stamp).
    void report(const std::string& message, bool error);
    // {"view": node shown, "strands", "points", "guides", "cards"..., "times": {node: ms of
    // the last evaluation}, "channels": {node: [strand channels]}} for the graph editor.
    std::string graphStatsJson();
    // Evaluates one graph node by name with the groom's current inputs (scalp, guides,
    // hierarchy as posed and corrected, parts, flow, controls) at `density`, in a scratch
    // copy of the graph (the viewport's cache is left alone; matching results are reused).
    // An empty name evaluates the graph's output.
    // Throws GraphError (no such node, a failing node) or std::runtime_error (no graph).
    Value evaluateNode(const std::string& name, float density);
    // Simulate nodes (simulation.h, groomLabSimulate): Simulate Range to `frame` (the scalp at the
    // frames on the way read from inMesh at those times), the frames dropped, their status (a bake
    // writes session()'s frames: sim_cache.h).
    SimRun simulateTo(int frame, const SimProgress& progress = {});
    void invalidateSimulation(const std::string& node = {});
    // A param drag in the viewport (GroomSession::setLiveDrag): on, the viewport's Subdivide hands
    // its strands on so the GPU's preview runs the drag; off, the next evaluation refines again.
    // The groom evaluates again either way.
    void setLiveDrag(bool on);
    std::vector<SimStatus> simulationStatus();
    // Bakes the current cards' textures as TGA files in `directory`, Unreal's
    // hair card layout: <base>_Depth, _Coverage, _Tangent, _Attribute (root UV,
    // seed, coordU), _Color, _AO and _Normal; returns the paths. resolution 0: the
    // Cards nodes' atlasSize. No card mesh needed. `store`: the groom's cardMaps
    // name the bake (its cards view shows it); false for an export. `base` is made
    // file safe, and takes _2, _3... when another groom's bake has it in `directory`.
    // Throws std::runtime_error.
    // `layout` "unity" writes PNG maps for Unity instead (never stored): <base>_BaseColor
    // (RGB color, A coverage), _Mask (HDRP's mask map: R metallic 0, G AO, B root to tip,
    // A smoothness from the hair look), _AO, _Tangent, _Depth, _Attribute and _Normal
    // (OpenGL's green); no atlas record.
    std::vector<std::string> bakeCards(const std::string& directory, const std::string& base, int resolution,
                                       float occlusion = 1.0f, bool store = true, const std::string& layout = "unreal");
    // The cards a bake makes and their maps, in memory: the groom at cardDensity (with
    // `graphJson` instead of its own graph when given: another level of detail) with its
    // Cards nodes, laid out in a fresh atlas (card_files.h). Throws std::runtime_error (no
    // strands, no cards).
    // `evaluator`, when given, evaluates (an export's one evaluator for every level of detail
    // and the scalp bake, with caching on: what their graphs share is evaluated once); else a
    // one-off evaluation with nothing kept.
    CardBake bakeCardsInMemory(int resolution, float occlusion, const std::string& graphJson = {},
                               NodeEvaluator* evaluator = nullptr);
    // A card map export (card_maps.h): the cards baked as bakeCardsInMemory does, at the
    // settings' resolution, padding and supersampling, written as the preset says into
    // `directory` named after `base` (file safe; an export: never stored). Returns the paths.
    // Throws std::runtime_error.
    std::vector<std::string> exportCardMaps(const std::string& directory, const std::string& base, const CardMapPreset& preset,
                                            const CardMapExport& settings, float occlusion = 1.0f);
    // The scalp bake in memory (see bakeScalp); *scalp receives the scalp. Throws std::runtime_error.
    // `evaluator` as for bakeCardsInMemory (the groom's own graph).
    ScalpTextures bakeScalpInMemory(int resolution, float reach, float density, MeshPtr* scalp,
                                    NodeEvaluator* evaluator = nullptr);
    // Bakes the groom onto its scalp (a hair cap for low card LODs): <base>_ScalpColor and
    // _ScalpCoverage .tga in the scalp's UVs, from the strands at `density`, the hair within
    // `reach` of the skin (0: 3% of the scalp's size); `base` as for bakeCards. Throws
    // std::runtime_error.
    // `layout` "unity": <base>_ScalpBaseColor.png instead (RGB color, A coverage).
    std::vector<std::string> bakeScalp(const std::string& directory, const std::string& base, int resolution,
                                       float reach, float density, const std::string& layout = "unreal");
    // The hierarchy as shown: rooted on the current scalp, posed (simulation, guide
    // cache) and corrected (shot sculpts); the brush preview while a stroke is live.
    HierarchyPtr evaluatedHierarchy();
    // The hierarchy as stored, rooted on the current scalp: no poses, no correctives,
    // no preview. What guide edits (brush strokes) are made against and written back.
    HierarchyPtr authoredHierarchy();
    // What the live evaluation scales the groom by with its scalp (GroomSession::groomScale).
    double groomScale() const { return session_.groomScale(); }
    PartsPtr evaluatedParts();  // null without parts
    FlowPtr evaluatedFlow();    // null without flow arrows
    // The Cards nodes' cards (laid out in one atlas) while displayCards is on
    // or outCards is connected; null otherwise.
    std::shared_ptr<const CardMesh> evaluatedCards();
    // The StrandMesh nodes' strands the viewport shows (displayStrandMesh; empty without), and
    // whether outStrandMesh is connected (a Maya mesh shows them).
    std::vector<StrandMeshSource> evaluatedStrandMeshes();
    std::uint64_t strandMeshVersion();
    bool strandMeshConnected() const;

    // Brush preview: while set, this hierarchy replaces the stored one for
    // evaluation and drawing. The brush tool mutates it between evaluations
    // and bumps brushTick; clearing it (null) returns to the stored data.
    // `final`: the preview is shown as it is (shot sculpting: it already has the
    // poses and correctives); otherwise poses and correctives apply on top.
    void setPreviewHierarchy(std::shared_ptr<GuideHierarchy> preview, bool final = false);
    // A brush stroke is live (a guide preview, or setLiveStroke): the strands step back
    // (sculptDisplay) and evaluate at sculptDensity. (Not while a commit's frame is held: a commit
    // starts only once the stroke ended, and setting a preview waits for it first, unless the new
    // stroke chains on the GPU: chainStroke.)
    bool hasPreview() const { return (!commitPending_ || chain_ == Chain::Armed) && session_.hasPreview(); }
    // The groom's session as the last evaluation left it (the GPU preview reads its graph's cache);
    // a pending commit is waited for first (not by a stroke chained on it).
    const GroomSession& session() const;

    // Committing off the UI thread (B7; docs/DEVELOPING.md, "Commits off the UI thread"). The
    // release of a GPU drag or of a brush stroke whose dabs the GPU drew commits what the viewport
    // already shows: the tool says so before committing (expectCommit, consumed by the next live
    // evaluation). That evaluation, when only the committed text (or a keyed param's control)
    // changed and nothing in the groom needs Maya while evaluating (no Simulate node, no scalp
    // history, no cards wanted), evaluates on a worker thread a copy of the session's document with
    // the inputs as they are (GroomSession::detach, DetachedRunner; the session stays the main
    // thread's) and returns at once: outFrame, outStrandCount and lastError keep the frame from before the commit
    // (the same strand counts: a change the GPU previews keeps every strand), so the viewport keeps
    // drawing the GPU preview's results (GroomSubSceneOverride holds while pending). The commit
    // lands on Maya's idle once evaluated (its frame written to the outputs, the views redrawn), or as
    // soon as anything needs the live session's results: every member reading them or changing
    // its inputs (and session(), the tools, the commands, a compute for a dirtied output) takes it in
    // first (settleCommit: the session adopts the evaluated copy, GroomSession::adopt).
    // GPU preview off, a change it did not draw, or GROOMLAB_ASYNC_COMMIT=0: the commit evaluates
    // in compute, as ever.
    //
    // A brush stroke pressed while a stroke's commit evaluates chains on it, waiting for nothing:
    // its press (chainStroke, from the brush host's prepareStroke) takes nothing in; when it sets
    // its guide preview the GPU preview arms at once on the session's document as it was (the
    // strands drawn: the frame held), so even the press's evaluation stops at the GPU's node, and
    // its dabs are drawn as any GPU stroke's. Its release commits detached after the pending one,
    // going on from what that one made (DetachedRunner chains them); only the last lands. A stroke
    // the GPU does not take (maps, parts, flow, shot sculpting, refused) takes the commit in first.
    enum class CommitKind { None, Graph, Guides };
    void expectCommit(CommitKind kind) { expectedCommit_ = kind; }
    // A commit's frame is held (the viewport keeps drawing the GPU preview's results), not while a
    // stroke chained on it is live.
    bool commitPending() const { return commitPending_ && chain_ != Chain::Armed; }
    // The brush host's press of a stroke of the guides alone: true when it chains on a pending
    // stroke commit (nothing is waited for; the host skips ensureEvaluated), false otherwise.
    bool chainStroke();
    // The stroke is over (or never began): a chained stroke's state goes.
    void endChain() { chain_ = Chain::None; }
    std::uint64_t chainedStrokes() const { return chainedStrokes_; }  // strokes chained so far
    // Waits for a pending commit and lands it (no-op without one, or while a stroke chained on it is
    // live). Main thread. who: what asked (the last one that waited: settledBy(), groomLabGraphStats
    // -frameInfo).
    void settleCommit(const char* who = "");
    const std::string& settledBy() const { return settledBy_; }
    double lastCommitMs() const { return lastCommitMs_; }   // the last commit's evaluation on the worker
    std::uint64_t commitCount() const { return commits_; }  // commits evaluated off the UI thread so far
    static bool asyncCommitsEnabled();
    // A live stroke's dabs drawn by the GPU preview from this node on (gpu_preview.h, set once the
    // press is drawn, "" none): while the stroke lasts each dab's evaluation stops at it, unless
    // the preview says the dab's guides are the CPU's (then it evaluates in full, and stops no more).
    void setGpuStroke(const std::string& node) { gpuStroke_ = node; }
    const std::string& gpuStroke() const { return gpuStroke_; }
    // A stroke that previews no guides is live (a scalp map stroke): drawn as any stroke, and
    // evaluated at sculptDensity when something evaluates during it (a vertex map's dabs). Turning
    // it off evaluates again (at full density) only when an evaluation took the stroke's density.
    void setLiveStroke(bool on);
    // The same for the parts and the flow arrows while a brush stroke shapes them:
    // whichever is set replaces the stored one for evaluation and drawing (the
    // tool mutates it between evaluations); null goes back to the stored data.
    void setPreviewOverlays(std::shared_ptr<const PartSet> parts, std::shared_ptr<const FlowSet> flow);
    // A live stroke on a Sculpt node (sculpt.h): its edits as the stroke has them and its working
    // guides, drawn as the guides; null edits go back to the stored ones.
    void setPreviewSculpt(const std::string& node, SculptPtr edits, std::shared_ptr<const GuideHierarchy> shown);
    // What Sculpt node `node` is fed (GroomSession::sculptInput; a pending commit landed first).
    SculptInput sculptInput(const std::string& node);
    // Cards view (displayCards, with a Cards node): the cards show instead of the strands.
    // They draw here only without a card mesh; with one (outCards), the mesh shows them
    // (with its baked textures), so they never show twice.
    bool cardsView();
    // The viewport's hair highlight, from the HairColor node making the output (the node's
    // defaults without one): strength 0-1, spread 0-1 and tilt. The node: the first one met walking
    // up from the output, first inputs first (its whole first-input chain before any other
    // branch), past bypassed nodes and a Switch's unpicked input, into compounds
    // (Graph::forEachActiveUpstream); a look merged from several takes the main one's.
    using HairLook = groomlab::HairLook;
    HairLook hairLook();
    // The same as JSON with its lobes and its nodes' params (GroomSession::hairLookJson):
    // groomLabGraphStats -hairLook, what create_hair_shader builds the Arnold shader from.
    std::string hairLookJson();
    bool cardMeshConnected() const;
    // The viewport shows a Scatter node: its roots draw as dots on the scalp.
    bool showsRoots();
    // The viewport shows a Clump: its clump centers, as dots on the scalp (null: none).
    std::shared_ptr<const std::vector<groomlab::Vec3f>> clumpCenters();
    // The graph node the viewport shows instead of the output ("": the output).
    std::string viewedNode();
    // The preview density the shown strands were evaluated at (ViewResult::density; 1 without).
    float evaluatedDensity();
    // The hierarchy with poses (simulation, guide cache) but no correctives:
    // what shot sculpts are recorded against.
    HierarchyPtr posedHierarchy();

    // The drawn frame's scalp (world space) or null, with the vertex maps (scalpMaps) as color
    // sets of their names.
    MeshPtr evaluatedScalp();
    MeshPtr evaluatedCageScalp();  // before it is refined: what vertex maps are painted on

    // The maps painted on the scalp (scalpMaps), parsed once per change of the attribute;
    // *error (when given) says why the text did not parse ("" when it did).
    const ScalpMaps& scalpMaps(std::string* error = nullptr);
    // The guide hierarchy as stored (guideData), parsed once per change of the attribute and not
    // rooted on the scalp (groups, layers and IDs are all there): null with *error when the text
    // does not parse. No evaluation.
    std::shared_ptr<const GuideHierarchy> storedHierarchy(std::string* error = nullptr);
    // Brush preview of a vertex map: while set, `values` (`channels` per vertex: 1 gray, 4 color)
    // stand in for the stored map `name` in evaluation and drawing (the tool mutates a copy and
    // calls this per stamp); null clears.
    void setPreviewVertexMap(const std::string& name, std::shared_ptr<const std::vector<float>> values, int channels = 1);
    // Brush preview of a texture map's image (the brush keeps it up to date, texels it changed
    // only: TexturePreview, scalp_paint.h), for drawing only; the brush calls this again after
    // each update (the drawing uploads what changed since it last did); null clears.
    void setPreviewTextureMap(const std::string& name, std::shared_ptr<const TexturePreview> image);
    // What the viewport shows on the scalp (scalpMapDisplay, scalpMapChannel): a vertex map's
    // values, a guide layer sampled at the scalp's vertices, or a texture map's file (and the
    // brush's image while it paints). kind "" when there is nothing to show.
    using ScalpMapView = groomlab::ScalpMapView;
    ScalpMapView scalpMapView();

    // The drawn frame's versions (GroomFrame): they move when an evaluation changed what there is
    // to draw (the strands, the hierarchy, the parts, the flow or the cards), so draw code can
    // tell when to rebuild. An evaluation that returned the same cached results (the same static
    // groom at another frame) leaves them alone.
    std::uint64_t evaluationVersion();
    // The parts of that: the strands (or instances) changed; the hierarchy changed
    // (or is being brushed); the Cards nodes' cards changed.
    std::uint64_t strandsVersion();
    // Which strands those are and how they differ from earlier ones (ViewResult::strandsSerial,
    // changesBase, strandsChanges): the viewport writes again only the strands that changed.
    std::uint64_t strandsSerial();
    std::uint64_t strandsChangesBase();
    ChangesPtr strandsChanges();
    std::uint64_t hierarchyVersion();
    std::uint64_t cardsVersion();
    // Increments when a display-only attribute (colors, guide display,
    // selection, cards shown...) changes, so draw code can skip reading them
    // on every refresh.
    std::uint64_t displayVersion() const { return displayVersion_; }
    // How many times the groom evaluated live (its strands, errors, timings): tools showing the
    // last evaluation's stats read them again only when it moved.
    std::uint64_t evaluationCount() const { return live_.evaluations; }
    // How many frames the background evaluation made (cached playback's background fill).
    std::uint64_t backgroundEvaluationCount();
    // How long the last live evaluation of the strands took in compute (ms; groomLabGraphStats -drawTimes).
    double lastComputeMs() const { return live_.lastComputeMs; }

    // The groom the brush tool is on (null: none). Its stroke document (GroomSession::warmStroke:
    // the graph at sculptDensity, a cache of its own) is evaluated on the first idle after each
    // of its evaluations, so a stroke's first dab makes again only what the stroke changed; the
    // groom it was on before drops its stroke document, and its viewport the dimmed strands a
    // stroke drew (strokeKept). Never during a stroke. stopStrokeWarmup: the plugin unloads.
    static void setBrushTarget(GroomShape* shape);
    static void stopStrokeWarmup();
    static void stopCommitLanding();  // the plugin unloads: the idle callback landing commits goes
    // The brush tool is on this groom: what its strokes use is kept (the stroke document, the
    // viewport's dimmed strands).
    bool strokeKept() const;
    // The brush's line of symmetry on the scalp (symmetrySeam's segments, pairs of points; empty:
    // none), drawn with the groom (its UI drawables) on every redraw while a brush with symmetry is
    // on it, not only while the cursor moves. The version moves when it changes.
    void setSymmetrySeam(std::vector<Vec3f> segments);
    const std::vector<Vec3f>& symmetrySeam() const { return symmetrySeam_; }
    std::uint64_t symmetrySeamVersion() const { return symmetrySeamVersion_; }
    // The Shape brush is on this groom: its guides' tubes drawn as wires (appendTubeWire) with the
    // groom on every redraw: every guide the brush can shape (activeTubeGuides: shown, not
    // frozen), the selected ones brighter. Off: nothing drawn and nothing worked out.
    void setTubeWires(bool on);
    bool tubeWires() const { return tubeWires_; }

    // Selection weights on the evaluated hierarchy (1: selected, below 1: soft),
    // and a hash of what they depend on, for the draw override.
    std::map<GuideId, float> guideSelectionWeights();
    std::uint64_t selectionHash() const;

private:
    // An evaluation of the groom from the data block: the live one (session_) or the background
    // one (BackgroundLane). What each keeps between its evaluations: its inputs as converted
    // (skipped while their data reads the same), the meshes it made, its last frame.
    struct Lane {
        Lane(GroomSession& s, bool isLive) : session(s), live(isLive) {}
        GroomSession& session;
        const bool live;
        // Input conversions are skipped while their plugs were not dirtied (a brush stamp, a
        // frame of a static groom) and their data reads the same; see setDependentsDirty and
        // evaluate. The background lane takes every plug as dirtied (its data block's values are
        // another time's): it fingerprints them all.
        bool meshDirty = true;       // inMesh or restMesh (or their connections) changed
        bool curvesDirty = true;     // guideCurves changed (their CVs are fingerprinted only then)
        bool collidersDirty = true;  // colliderMeshes (or their connections) changed
        bool scalpsDirty = true;     // extraScalps (or their connections) changed
        bool meshReconnected = false;
        std::uint64_t attributesKey = 0;  // the background lane's: the meshes' UVs and colors (meshAttributesKey)
        std::uint64_t meshKey = 0;  // fingerprint of the mesh data rawScalp was made from
        // The topology (meshTopologyKey) of the scalp and rest mesh of the last full conversion: a
        // match takes only the points of the next frame (and the rest mesh's when they moved:
        // restKey, meshPointsKey). A new connection to inMesh or restMesh converts in full.
        std::uint64_t topologyKey = 0, restTopologyKey = 0, restKey = 0;
        std::uint64_t curvesDataKey = 0;  // fingerprint of the curve data, taken when the curves were dirtied
        std::uint64_t curvesKey = 0;      // ... with the scalp's: what guides was made from
        std::uint64_t collidersKey = 0;   // fingerprint of the collider mesh data colliders was made from
        std::map<std::string, MeshPtr> colliders;  // by name: the externals "mesh:<name>"
        std::uint64_t scalpsKey = 0;               // fingerprint of the extra scalps' mesh data scalps was made from
        NamedScalps scalps;                        // the groom's other scalps (extraScalps), in order
        std::string restError;                     // the rest mesh was dropped (vertex counts differ)
        GuidesPtr guides;                          // flat guides from guideCurves, rooted on the scalp
        MeshPtr rawScalp;     // the scalp as converted from the mesh (the session lays the maps on it)
        std::string poseKey;  // guide cache file, object and time of posesFromCache
        GuidesPtr posesFromCache;
        SimCacheInputs simInputs;  // the bakes the Simulate nodes read (simCaches)
        // The control attributes (ctl_*), found by walking the node's attributes once per change
        // of its dynamic attributes (an added, removed or renamed one: the live lane's
        // attribute-changed callback; the attribute count); their values are read on every
        // evaluation (keys, sliders).
        std::vector<MObject> controls;
        bool controlsKnown = false;
        unsigned controlCount = 0;
        // outCards and outRachis, kept while what they are made from stands (the cards; the rachis
        // evaluation's strands): another pull of the plug (any input dirtied) hands the same mesh.
        MObject cardsMeshData;
        std::shared_ptr<const CardMesh> cardsMeshFrom;
        bool cardsMeshMade = false;
        MObject rachisMeshData;
        StrandsPtr rachisMeshFrom;
        bool rachisMeshMade = false;
        MObject unusedRachisData;  // an empty mesh: outRachis while nothing is connected to it
        // outStrandMesh: the mesh, what it was made from (each StrandMesh node's strands and
        // widthScale), its topology key and points (strandMeshTopologyKey of each, joined).
        MObject strandMeshData;
        std::vector<StrandsPtr> strandMeshFrom;
        std::vector<float> strandMeshWidths;
        std::uint64_t strandMeshKey = 0;
        unsigned strandMeshPoints = 0;
        bool strandMeshMade = false;
        MObject unusedStrandMeshData;  // an empty mesh: outStrandMesh while nothing is connected to it
        // Its last frame, and the session's versions it was made at (the next frame's versions
        // move for what changed since).
        FramePtr frame;
        ViewVersions seen;
        // renderView's width and length scales as the last evaluation read them (makeFrame applies
        // them to the strands it hands on).
        bool renderView = false;
        float renderWidthScale = 1.0f;
        float renderLengthScale = 1.0f;
        std::uint64_t evaluations = 0;
        double lastComputeMs = 0.0;
    };
    // What the background lane takes of the node from the live one, which reads it through plugs
    // on Maya's main thread: off it, only the data block is read (plugs, function sets and the
    // reference a path resolves against are the main thread's).
    struct NodeLinks {
        bool restConnected = false, cardsConnected = false, rachisConnected = false, strandMeshConnected = false;
        std::set<unsigned> simCurves;                  // simGuides elements whose curve is connected
        std::map<unsigned, std::string> colliderNames;  // colliderMeshes elements connected: the mesh's transform
        std::set<unsigned> colliderRests;               // colliderMeshes elements whose rest mesh is connected
        std::map<unsigned, std::string> scalpNames;     // extraScalps elements connected: the mesh's transform
        std::set<unsigned> scalpRests;                  // extraScalps elements whose rest mesh is connected
        std::vector<MObject> controls;                 // the control attributes
        std::shared_ptr<const PathResolver> paths;     // how the node resolves paths
    };
    std::shared_ptr<const NodeLinks> links();  // the live lane's last (null before its first evaluation)
    // The background lane: a session of its own, used by one thread at a time.
    struct BackgroundLane {
        std::recursive_mutex mutex;
        GroomSession session;
        Lane lane{session, false};
    };

    // Reads the lane's inputs from the data block (in its context) into its session, evaluates
    // them at the context's time and sets outStrandCount, lastError and outFrame. The live lane
    // reports and schedules; the background one touches nothing outside its lane (another
    // thread, while the live one may run).
    MStatus evaluate(MDataBlock& data, Lane& lane);
    MStatus computeCards(MDataBlock& data, Lane& lane);
    MStatus computeRachis(MDataBlock& data, Lane& lane);
    MStatus computeStrandMesh(MDataBlock& data, Lane& lane);  // groom_shape_strand_mesh.cpp
    // The lane's next frame from its session's last view (or, failed, no strands and the error).
    void makeFrame(MDataBlock& data, Lane& lane, const ViewResult* view, double seconds, const std::string& errors);
    // outFrame, outStrandCount and lastError set (and clean) from `frame`.
    void writeFrame(MDataBlock& data, const FramePtr& frame);
    // What the live evaluation does with its view (reports, the warm-up's request); the joined errors.
    std::string finishView(const ViewResult& result, const ViewRequest& request, std::vector<std::string> hostErrors);
    // The live evaluation's commit (of `kind`), started off the UI thread (after a pending one): true
    // when it was (the outputs keep the frame before it).
    bool startCommit(MDataBlock& data, const ViewRequest& request, double seconds, std::vector<std::string> hostErrors,
                     CommitKind kind);
    // A pending commit waited for and taken: the reports and the warm-up's request; then, unless the
    // caller evaluates after (compute's evaluate), its frame written to the outputs (into, else the
    // node's data block; not when an input dirtied them since: the next pull evaluates) and the views
    // redrawn.
    void finishCommit(bool evaluatesAfter, MDataBlock* into, const char* who);
    static void landCommits(void*);  // Maya's idle while a commit is pending
    void unchain(const char* who);   // a stroke the GPU does not take: no chain, the commit taken in
    std::map<std::string, float> readControlValues(MDataBlock& data, Lane& lane, const NodeLinks* links);
    // The groom depends on the frames before the one shown over the playback range (getCacheSetup).
    bool readsEarlierFrames();
    void updateGraph(GroomSession& session, const std::string& text, bool report);  // a graph that does not load is reported

    GroomSession session_;  // the groom evaluated live (fed by compute; previews by the tools)
    Lane live_{session_, true};
    // A commit off the UI thread (expectCommit): the detached evaluations of copies of session_'s
    // document (in order, the last landing), what the evaluation that started the last one read
    // (its time, the host's errors, the request).
    DetachedRunner commit_;
    CommitKind pendingKind_ = CommitKind::None;  // what the pending commit commits
    // A stroke pressed while a commit is pending: Provisional from its press until its guide
    // preview is set, Armed once the GPU took it.
    enum class Chain { None, Provisional, Armed };
    Chain chain_ = Chain::None;
    std::uint64_t chainedStrokes_ = 0;
    CommitKind expectedCommit_ = CommitKind::None;
    bool commitPending_ = false;
    bool computing_ = false;  // the live lane's compute is running (a commit lands into it)
    double pendingSeconds_ = 0.0;
    std::vector<std::string> pendingErrors_;
    ViewRequest pendingRequest_;
    double lastCommitMs_ = 0.0;
    std::uint64_t commits_ = 0;
    std::string settledBy_;  // what the last commit landed for ("idle": evaluated, nothing asked)
    std::unique_ptr<BackgroundLane> background_;  // made with the node: compute may come on any thread
    std::shared_ptr<const NodeLinks> links_;  // links(): set by the live lane, read by the background one
    std::mutex linksMutex_;
    std::atomic<bool> viewportCarries_{false};  // setViewportCarries
    std::string gpuStroke_;  // setGpuStroke
    std::uint64_t displayVersion_ = 0;
    std::vector<Vec3f> symmetrySeam_;
    std::uint64_t symmetrySeamVersion_ = 0;
    bool tubeWires_ = false;
    std::string reported_;  // the last message sent to the Script Editor
    std::string reportedHeavy_;  // the last "heavy" Scatter note warned about (finishView)

    // Scaling with the scalp (aScaleSource): the scalp's world area at the scene's start frame
    // (measured by the live lane: at that frame as it is, else read at it when the scalp changed
    // while Maya does not play or scrub, a first evaluation or another start frame), the start
    // frame it was measured at, and the first area the groom was evaluated at (the reference while
    // scaleReference is 0). Shared by both lanes and exports (the scaleArea output).
    MeshPtr scaleScalp(Lane& lane, MDataBlock& data, const MeshPtr& scalp, bool meshTouched);
    std::atomic<double> scaleMeasure_{0.0};
    std::atomic<double> scaleTransformMeasure_{0.0};  // the transform scale at the start frame (measured with the area)
    std::atomic<double> firstScaleTransform_{0.0};    // the first one (the reference while scaleTransformReference is 0)
    std::uint64_t scaleNoticeSaid_ = 0;               // the live session's scaleNoticeSerial last said
    MTime scaleMeasuredAt_;
    std::atomic<double> firstScaleArea_{0.0};

    // The stored texts (graph, guideData, scalpMaps, correctiveData, partData, flowData) are
    // read live (copied out of the node and handed to the session, which compares them with the
    // last) only after their plug was dirtied: setAttr, undo and redo, a file read, a reference
    // edit and a connection all dirty it (setDependentsDirty, connectionMade/Broken; the
    // Evaluation Manager's dirty plugs in preEvaluation). A connected one is read on every
    // compute, as a connection may bring a new value without the plug being dirtied here. The
    // background lane reads them all on every frame.
    enum TextInput { kGraphText, kGuideText, kScalpMapsText, kCorrectiveText, kPartText, kFlowText, kSculptText, kTextInputs };
    bool textDirty_[kTextInputs] = {true, true, true, true, true, true, true};
    static const MObject* textAttribute(int input);
    // True (and clears the flag) when the live lane must read that text again.
    bool takeTextDirty(int input);
    void markDirty(const MPlug& plug);  // the flags above for a dirtied input plug
    MCallbackId attrCallback_ = 0;
    // Match render (subdivision.h): the scalp mesh's Arnold subdivision and smooth preview are
    // attributes of the mesh, not its data; a change of one re-reads the scalp (watchScalpMesh).
    MCallbackId scalpAttrCallback_ = 0;
    void watchScalpMesh(const MObject& mesh);
    struct RenderLevel {
        int levels = 0;
        std::string from;
    };
    RenderLevel scalpRender_;                          // the scalp mesh's, as last read live
    std::map<unsigned, RenderLevel> colliderRender_;  // each collider's, by colliderMeshes index
    std::map<unsigned, RenderLevel> scalpsRender_;    // each extra scalp's, by extraScalps index
    // The scalp's earlier frames for Jiggle nodes (maya_inputs.h), kept across frames (live only).
    ScalpPointCache scalpPast_;
    int scalpHistoryFrames_ = 0;
    // scalpMapView() itself, kept while nothing it reads changed (it is asked on every redraw):
    // the evaluation, the shown map's attributes, the maps, the previewed map, the brush tick (a
    // file rewritten and read again) and the plugin's file writes; a texture's file stamp is
    // looked at again at most twice a second (an edit outside Maya).
    struct MapViewMemo {
        std::uint64_t key = 0;
        bool valid = false;
        std::chrono::steady_clock::time_point stampChecked{};
        std::uint64_t stamp = 0;
        ScalpMapView view;
    };
    MapViewMemo mapViewMemo_;
    ScalpMapView makeScalpMapView();
    std::uint64_t mapInputsVersion_ = 0;  // bumped when brushTick or scalpMaps is dirtied
    ViewRequest lastRequest_;    // what the last evaluation asked for (the stroke document's warm-up)
    bool requested_ = false;
    void scheduleStrokeWarmup();  // on the next idle, when the brush tool is on this groom
    static void warmStrokeOnIdle(void*);
    // The warm-up off the UI thread (GroomSession::detachWarm; GROOMLAB_ASYNC_WARM=0: on it, as
    // warmStroke): started on that idle (after an evaluation, or a commit's landing), taken in
    // (adoptWarm) on a later idle once it ran, or by the next press, which waits for it. One at a
    // time: one due meanwhile starts once that one is taken in.
    DetachedRunner warm_;
    bool warmAgain_ = false;
    void startWarm();
    void takeWarm(bool wait);
    static void adoptWarmsOnIdle(void*);
};

// (parseCardMaps, the cardMaps attribute as map -> path: groom_session.h.)

// The name the groom's automatic guide colors start from (DisplaySettings::guideColorSeed): its
// transform's (as Blender's object and the app's file name), so two grooms' groups differ.
std::string guideColorSeedOf(const MObject& groomShape);

// Legacy-viewport UI class; registerShape requires one even when drawing
// happens through Viewport 2.0.
class GroomShapeUI : public MPxSurfaceShapeUI {
public:
    static void* creator();
};

}  // namespace groomlab::maya
