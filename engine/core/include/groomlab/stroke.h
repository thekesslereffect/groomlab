// SPDX-License-Identifier: Apache-2.0
#pragma once

// GroomLab's interactive tools without a host (docs/SESSION.md, Strokes): the brush, Place, Draw, Part
// and Flow tools and map painting, driven by mouse events in a view's pixels (BrushTool), or by
// world points (scriptedStroke). A host (Maya's brush context, Python, another DCC) hands the
// events over with a ToolView (view_projection.h) and implements StrokeHost: what a stroke reads
// (the scalp, the guides, the stored parts, flow, correctives and maps), where its live preview
// goes (a GroomSession's previews), and what to do with the result: one StrokeEdit per stroke,
// plain data (the stored texts after the stroke, a texture map's changed tiles), one undo step.
// SessionStrokeHost is the host for a GroomSession alone.

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "groomlab/brush.h"
#include "groomlab/correctives.h"
#include "groomlab/flow.h"
#include "groomlab/graph.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/parts.h"
#include "groomlab/scalp_maps.h"
#include "groomlab/scalp_paint.h"
#include "groomlab/sculpt.h"
#include "groomlab/view_projection.h"

namespace groomlab {

class GroomSession;

// ---- Settings ---------------------------------------------------------------------------------

// Everything a stroke needs besides the path: the core brush settings, the guide-creating tools,
// the interactive (screen-space) radius and what the brush acts on.
struct BrushToolSettings {
    enum class Tool { Brush, Place, Draw, Part, Flow };
    // Place: click one guide, drag to place many; Draw: draw a guide's shape; Part: draw a part
    // freehand on the scalp; Flow: lay flow arrows along the stroke and comb the ones it passes
    // (Ctrl: erase them).
    Tool tool = Tool::Brush;
    std::string partName = "part";  // Part: the new part's name (made unique)
    // Flow: "" lays arrows along strokes; whorl, whorlcw, converge or part places a sink of that
    // kind where clicked (brush radius; Ctrl: remove sinks).
    std::string flowSink;
    BrushSettings brush;
    PlaceSettings place;         // new guides (group comes from brush.group)
    bool pressure = true;        // pen pressure scales strength (interactive)
    float screenRadius = 60.0f;  // interactive tool radius, pixels
    bool selectedOnly = false;   // brush only the groom's selected guides, scaled by soft selection
    // Interactive: what the scalp hides from the camera gets no weight (guide points, part points,
    // flow arrows, painted vertices behind the head), so a stroke never reaches through the head.
    bool maskByScalp = true;
    // What the Erase brush removes where it passes (interactive); the Part tool with Ctrl erases
    // parts the same way.
    bool eraseGuides = true;
    bool eraseParts = true;
    bool eraseFlow = true;
    // What the shaping brushes act on (Comb, Smooth, Twist, Length, Attract, Straighten, Cut;
    // see brush.h for what each does to arrows and parts). Scripted strokes read these for every
    // mode, Erase included.
    bool brushGuides = true;
    bool brushParts = false;
    bool brushFlow = false;
    // The soft selection brush.mask points at while selectedOnly (prepareMask); shared, so copies
    // of the settings keep a valid mask.
    std::shared_ptr<const std::map<GuideId, float>> selectionMask;
    // Paint: where the map (brush.attribute) lives. "guides": a layer on the guides, painted where
    // the brush touches them. "vertices" (a value per scalp vertex) or "texture" (an image at the
    // scalp's UVs, made paintResolution square when new): painted where the brush hits the scalp;
    // guides are left alone. paintTexture is what a UI shows; the host's own map decides.
    std::string paintKind = "guides";
    std::string paintTexture;
    int paintResolution = 1024;
    // A color map is painted toward paintColor (r g b a; brush.value is for gray maps), in the
    // channels of paintChannels only. Ctrl paints toward 1 - paintColor in those channels (a gray
    // map: toward 0, or 1 while brush.value is 0: grayCtrlTarget); Shift+drag smooths them.
    PaintColor paintColor{1.0f, 1.0f, 1.0f, 1.0f};
    unsigned paintChannels = kAllChannels;
    PaintColor paintTarget(bool inverted) const {
        PaintColor c = paintColor;
        if (inverted) {
            for (float& x : c) x = 1.0f - x;
        }
        return c;
    }
    // Smoothing a map (Shift+drag while painting, or the Smooth brush on it): how much per stamp;
    // past 1 the stamp runs in passes (2: two full passes).
    float smoothStrength = 1.0f;
    // Paint on a scalp map through an alpha (brush_alpha.h): each dab weighs the falloff times the
    // image, dabs alphaSpacing x the brush's diameter apart along the cursor's path, each turned by
    // alphaAngle (degrees) as alphaRotation says: Fixed that angle, Stroke the way the stroke goes
    // plus it, Random a random angle per dab. Smoothing and guide maps take no alpha (stampsAlpha).
    enum class AlphaRotation { Fixed, Stroke, Random };
    std::string alphaPath;  // the alpha's file ("" none); setAlpha reads it into `alpha`
    BrushAlphaPtr alpha;
    AlphaRotation alphaRotation = AlphaRotation::Fixed;
    float alphaAngle = 0.0f;
    float alphaSpacing = 0.25f;
    bool alphaInvert = false;
    static constexpr float kMinAlphaSpacing = 0.02f, kMaxAlphaSpacing = 4.0f;
    // Whether dabs stamp the alpha: one is set and the brush paints a scalp map (not smoothing).
    bool stampsAlpha() const { return alpha && brush.mode == BrushMode::Attribute && paintsScalp(); }
    // A scalp map is painted (Paint) or smoothed (Smooth: Shift+drag while painting, or the brush).
    bool paintsScalp() const {
        return (brush.mode == BrushMode::Attribute || brush.mode == BrushMode::Smooth) && paintKind != "guides";
    }
    // Whether Ctrl selects guides rather than inverting the stroke: the guide brushes (comb,
    // length, cut...). Painting (maps, a guide attribute such as freeze) keeps Ctrl's opposite
    // action, as do Part and Flow (erase). Hosts: Ctrl+click or drag paints the selection within
    // the ring, Ctrl+Shift+drag lassos it (the rest frozen either way).
    bool ctrlSelects() const { return tool == Tool::Brush && brush.mode != BrushMode::Attribute && !paintsScalp(); }

    // comb, smooth... (brush.h), clump, place, draw, part, flow.
    std::string modeName() const;
    // Throws std::invalid_argument for an unknown name; for "width" and "density" (brushes no
    // more) it names the way to paint those params now (kRemovedParamBrushes).
    void setMode(const std::string& name);
    // "guides,parts,flow" (any of them, or "none") <-> brushGuides / brushParts / brushFlow.
    std::string targetsName() const;
    void setTargets(const std::string& names);  // throws std::invalid_argument
    // The settings hosts set from their UIs and scripts, kept in range the same way everywhere:
    // the radius at least a pixel, at least one segment, a new texture kMinPaintResolution to
    // kMaxPaintResolution square, the hardness and the paint color 0 to 1, smoothing at least 0.
    static constexpr int kMinPaintResolution = 4, kMaxPaintResolution = 16384;
    void setScreenRadius(float pixels);
    void setSegments(int segments);
    void setPaintResolution(int pixels);
    void setHardness(float hardness);
    // The brush's curves as every host hands them over: a ramp's JSON ([[position, value], ...],
    // or {"keys": [...], "linear": true}). "" clears it (and so does a strength profile of 1
    // all along: the brush as without one); throws std::invalid_argument for anything else.
    // The getters give "" for none.
    void setStrengthProfile(const std::string& json);
    void setFalloffProfile(const std::string& json);
    std::string strengthProfileJson() const;
    std::string falloffProfileJson() const;
    void setSmoothStrength(float strength);
    void setPaintColor(const PaintColor& color);
    // The alpha read from its file ("" none). Throws TextureError or std::invalid_argument (a
    // file it cannot read), the alpha left as it was.
    void setAlpha(const std::string& path);
    // "fixed", "stroke" or "random"; throws std::invalid_argument for anything else.
    void setAlphaRotation(const std::string& name);
    std::string alphaRotationName() const;
    void setAlphaSpacing(float fraction);  // kMinAlphaSpacing .. kMaxAlphaSpacing
    // Symmetry by name, as every host offers it: "x", "y", "z" (a world plane at
    // brush.mirrorCenter), "topology" (across the scalp's own surface: brush.mirrorTopology),
    // "none". Throws std::invalid_argument for anything else.
    void setSymmetry(const std::string& name);
    std::string symmetryName() const;
    // brush.hiddenGroups set, and brush.mask pointed at the soft selection of `selection` (soft
    // radius `softRadius`) when selectedOnly (an empty selection: nothing to brush), else null.
    void prepareMask(const GuideHierarchy& h, std::set<std::string> hiddenGroups, const std::vector<GuideId>& selection,
                     float softRadius);
};

// The colors every host draws the tool in (RGB 0..1): the ring under the cursor (red for Part and
// Erase, orange for Flow, green for the guide-making tools, else yellow), a Part or Flow stroke's
// path so far on the scalp (red), and the guides the brush would touch under the hover (red while
// erasing, else white).
std::array<float, 3> brushRingColor(const BrushToolSettings& settings);
std::array<float, 3> brushPathColor(const BrushToolSettings& settings);
std::array<float, 3> brushHoverColor(const BrushToolSettings& settings);

// "x", "y", "z" -> 0, 1, 2; "", "none", "off" -> -1. Throws std::invalid_argument ("-symmetry
// takes x, y, z, topology or none").
int symmetryAxisFromName(const std::string& name);
const char* symmetryAxisName(int axis);  // x, y, z or none

// ---- The tools as functions of data -------------------------------------------------------------

// The group Place and Draw add to: the brush's group, else the first group at the brush's level. A
// level with no group yet gets one under the first group a level up ("<parent>_children"), so
// children can be placed by hand from the start. Throws std::invalid_argument when there is no
// level above.
std::string placeGroup(GuideHierarchy& h, const BrushSettings& brush);
// New guides for the Place tool at a scalp point, and at its mirror image when symmetry is on.
// With `spaced` (Place Many while dragging), points closer than the spacing to an existing root
// are skipped. Returns the guides added.
std::size_t placeWithSymmetry(GuideHierarchy& h, const ScalpMesh& scalp, const BrushToolSettings& s,
                              const SurfacePoint& at, bool spaced);
// A drawn guide, and its mirror image when symmetry is on. Throws std::invalid_argument for a
// path too short.
void drawWithSymmetry(GuideHierarchy& h, const ScalpMesh& scalp, const BrushToolSettings& s,
                      const std::vector<Vec3f>& path);
// A part through `path` (s.partName, made unique) and its mirror image when symmetry is on (a
// part on the seam is its own mirror). Throws std::invalid_argument for a path too short.
void addPartWithSymmetry(PartSet& parts, const ScalpMesh& scalp, const BrushToolSettings& s,
                         const std::vector<Vec3f>& path);
// Flow arrows after a stroke along `path` (points on the scalp): arrows the brush passes over turn
// toward the stroke by `weight` x strength (erase: they go), and new arrows are laid along the
// stroke every `spacing` where none is closer than 3/4 of that, as one line of pins. With
// symmetry the mirror image does the same.
using FlowWeightFn = std::function<float(const Vec3f& pin, std::size_t pathIndex)>;
void flowAlongStroke(FlowSet& flow, const ScalpMesh& scalp, const BrushToolSettings& s, const std::vector<Vec3f>& path,
                     const FlowWeightFn& weight, float spacing, bool erase);
// Flow sinks after a click at each of `points` (on the scalp): a sink of s.flowSink's kind and
// `radius` at each (erase: sinks within `radius` go), mirrored with symmetry (a whorl's turn flips
// in the mirror).
void flowSinksAt(FlowSet& flow, const ScalpMesh& scalp, const BrushToolSettings& s, const std::vector<Vec3f>& points,
                 float radius, bool erase);

// ---- Map strokes ------------------------------------------------------------------------------

// The map a stroke paints: a vertex map's values (channels per scalp point) or a texture map.
struct MapCanvas {
    std::vector<float>* vertices = nullptr;
    const std::vector<float>* strokeStartVertices = nullptr;  // non-Replace modes: values at press
    const VertexNeighbors* neighbors = nullptr;  // smoothing a vertex map
    TexturePaint* texture = nullptr;
    int channels = kGrayMap;
};

// How a dab paints: toward `target` by `strength` in the channels of `mask`, or (smoothing) in
// `passes` passes of `perPass`; mirrored with the brush's symmetry (the mirrored dab a world sphere
// of the dab's radius and `falloff`).
struct MapDab {
    bool smoothing = false;
    bool fromStrokeStart = false;  // non-Replace: overlapping dabs share one compositing baseline
    int passes = 1;
    float perPass = 1.0f;
    float strength = 1.0f;
    PaintColor target{1.0f, 1.0f, 1.0f, 1.0f};
    unsigned mask = kAllChannels;
    PaintMode mode = PaintMode::Replace;
    BrushFalloff falloff;
    int mirrorAxis = -1;
    float mirrorCenter = 0.0f;
    bool mirrorTopology = false;  // the mirrored dab at its partner spot on the painted mesh (ScalpMirror)
    // The mirrored dab weighs each point by the dab's own weight at its mirror image (an alpha's
    // pattern mirrored), not by a world sphere.
    bool mirrorWeight = false;
    // A dab of `brush` (the pressed settings: Ctrl's value already flipped) on a map of `channels`:
    // a color map toward s.paintTarget(inverted), a gray map toward brush.value. Smoothing runs
    // s.smoothStrength x `smoothScale` (the pen's pressure; a scripted stroke's |strength|), in
    // passes past 1.
    static MapDab of(const BrushToolSettings& s, const BrushSettings& brush, int channels, bool inverted, float smoothScale);
};

// One dab at `center` (radius world units; `weight` replaces the sphere's falloff, `threadSafe`:
// it may weigh texels in parallel) and its mirror image. Returns the vertices or texels changed.
std::size_t paintMapDab(MapCanvas& map, const ScalpMesh& scalp, const MapDab& dab, const Vec3f& center, float radius,
                        const BrushWeightFn& weight, bool threadSafe);
// A scripted stroke on a map: dabs spaced a quarter radius apart along `path` (world space), each
// a sphere of `radius`. Returns the vertices or texels changed.
std::size_t paintMapStroke(MapCanvas& map, const ScalpMesh& scalp, const MapDab& dab, const std::vector<Vec3f>& path,
                           float radius);
// The channels of map `name`: 4 for a color map, else 1 (gray, or no such map).
int scalpMapChannels(const ScalpMaps& maps, const std::string& name);
// The values a stroke on vertex map `name` starts from: the stored ones when they hold `channels`
// per scalp point, else black (alpha 1 on a color map). *restarted: the stored map was there but
// fits another vertex count.
std::vector<float> vertexMapToPaint(const ScalpMaps& maps, const std::string& name, int channels,
                                    std::size_t pointCount, bool* restarted = nullptr);
// The maps with vertex map `name` set to `values`.
ScalpMaps withVertexMap(ScalpMaps maps, const std::string& name, std::vector<float> values, int channels);

// What Ctrl paints a gray map toward while the brush paints toward `value`: 0 (it lowers the param
// the map feeds: no hair where a density map is 0, thinner on a width map), or 1 when `value` is
// 0 (Ctrl then paints back up). The same for every gray map: guide layers (Ctrl on the freeze
// mask unfreezes), scalp maps, maps feeding a param.
float grayCtrlTarget(float value);

// What setMode("width") or setMode("density") says: those brushes became Paint on a param's map
// (Paint tab > + New Map > for a setting).
extern const char* const kRemovedParamBrushes;

// ---- Painting a param's map (+ New Map > for a setting; a param's Paint) ------------------------

// The settings + New Map offers by name, in this order (every host's short list). Every host's
// "Other" lists the rest: every mappable param of the groom's graph.
struct PaintableSetting {
    const char* id;     // "density", "width", ...
    const char* label;  // "Density"
    const char* type;   // the node type
    const char* param;
    const char* hint;   // what painting it does
};
const std::vector<PaintableSetting>& paintableSettings();
const PaintableSetting* paintableSetting(const std::string& id);  // null for no such id

// The name of the graph's first node of type `type`, "" when it has none ("" or "{}": the
// default graph).
std::string firstNodeOfType(const std::string& graphJson, const std::string& type);

// The map node.param reads: a map param naming a scalp map or a guide layer (colorSet, guide; a
// texture param a texture map by its file), or a Map node wired to the param's input (Maya's
// Graph Editor's Paint). map "" when it reads none (a number). Throws std::runtime_error when
// the graph has no node `node`, or the param reads what a brush cannot paint (an image file that
// is no map of the groom, an expression, a channel). `guides` names the guide layers (may be null).
struct ParamMapRef {
    std::string map, kind;  // kind: guides, vertices or texture
};
ParamMapRef paramMapOf(const std::string& graphJson, const ScalpMaps& maps, const GuideHierarchy* guides, const std::string& node,
                       const std::string& param);

// The graph with node.param reading map `map` of kind `kind` (a vertex map by its color set, a
// texture map of `maps` by its file, "guides" a guide layer), white the param's value now (min
// 0, max the value; 1 when it is no number). Throws std::runtime_error for no such node, or a
// texture map not in `maps`.
std::string wireParamMap(const std::string& graphJson, const ScalpMaps& maps, const std::string& node, const std::string& param,
                         const std::string& map, const std::string& kind);

// The map node.param is painted on. When none drives the param yet: a vertex map named
// <node>_<param> (made unique against the maps and the guides' layers), `vertexCount` values of
// 1 (white: the look stays), wired to the param (wireParamMap).
struct ParamMapBinding {
    std::string node, map;
    std::string kind;       // where the map lives: guides, vertices or texture
    std::string graphJson;  // the graph rewired; "" when a map drove the param already
    ScalpMaps maps;         // `maps` with the new one (when graphJson is set)
};
// Throws std::runtime_error as paramMapOf does, and when a map is to be made with no scalp
// (vertexCount 0).
ParamMapBinding bindParamMap(const std::string& graphJson, const ScalpMaps& maps, const GuideHierarchy* guides,
                             std::size_t vertexCount, const std::string& node, const std::string& param);

class StrokeHost;
// + New Map > for a setting for a host with a StrokeHost (the app, the Blender helper): the map
// node.param reads, or one made and wired (bindParamMap) as one undo step (host.commit: the graph
// and the maps), and `settings` set to Paint on it (paint mode, the map and its kind, value 1:
// strokes paint the param back to its value, Ctrl toward 0). Throws std::runtime_error.
ParamMapBinding paintParamMap(StrokeHost& host, BrushToolSettings& settings, const std::string& node, const std::string& param);

// ---- What a stroke changed ---------------------------------------------------------------------

// The stored inputs a stroke edits (Maya: the groom's attributes of these names).
// Graph: a param's map made and wired (paintParamMap: the graph's JSON).
// Sculpts: a stroke on a Sculpt node's guides (sculpt.h: SculptSet's JSON).
enum class StrokeField { Guides, Parts, Flow, Correctives, ScalpMaps, ShotLayer, Graph, Sculpts };
constexpr std::size_t kStrokeFields = 8;
// guideData, partData, flowData, correctiveData, scalpMaps, shotLayer, graph, sculptData
const char* strokeFieldName(StrokeField field);

// One stroke's change, one undo step: the stored texts it set (after, and before when the host
// recorded them), and a texture map's changed tiles.
struct StrokeEdit {
    struct Text {
        bool set = false;
        std::string after;
        bool hasBefore = false;
        std::string before;
    };
    std::string mode;  // the tool's mode name when the stroke ended (journals)
    std::array<Text, kStrokeFields> texts;
    std::string textureMap;  // a texture map stroke: its name...
    TextureEdit texture;     // ... and its changed tiles (none for a host that keeps the file's bytes)

    Text& text(StrokeField f) { return texts[static_cast<std::size_t>(f)]; }
    const Text& text(StrokeField f) const { return texts[static_cast<std::size_t>(f)]; }
    void set(StrokeField f, std::string after) {
        Text& t = text(f);
        t.set = true;
        t.after = std::move(after);
    }
    bool empty() const;
    std::size_t bytes() const;
    // The texts after (redo) or before (undo) handed to the session's setters (guides, parts,
    // flow, correctives, scalp maps, graph, sculpts; the shot layer is the host's). Undo needs the befores.
    void apply(GroomSession& session, bool after) const;
};

// ---- The host -----------------------------------------------------------------------------------

// What a stroke reads and where it goes. Inputs are read when a stroke needs them.
class StrokeHost {
public:
    virtual ~StrokeHost() = default;

    // A stroke may start (the host's own checks: a groom to brush, not locked...); false refuses
    // it, the host having said why. attach() is the same without the checks (hover, pick).
    virtual bool prepareStroke() { return attach(); }
    virtual bool attach() { return true; }

    // The scalp as evaluated (world space, at the current time); null or empty: no stroke.
    virtual MeshPtr scalp() = 0;
    // The mesh vertex maps are painted on: the scalp before it is refined (GroomSession::cageScalp),
    // whose vertices the maps are stored by, and parts and flow are stored bound to (OverlayBinding).
    // The scalp itself without subdivision. In the same pose as scalp().
    virtual MeshPtr paintScalp() { return scalp(); }
    // The guides as stored, rooted on the scalp: what strokes edit and write back.
    virtual std::shared_ptr<const GuideHierarchy> authoredHierarchy() = 0;
    // A copy to edit (a scripted stroke); a host that parses the stored text anyway returns its own.
    virtual std::shared_ptr<GuideHierarchy> editableHierarchy();
    // The guides as shown: posed and corrected (shot sculpting starts from these; hover and pick
    // read them), and a number that changes when they do (hover keeps what it found until then).
    virtual std::shared_ptr<const GuideHierarchy> shownHierarchy() = 0;
    virtual std::uint64_t shownHierarchyVersion() { return 0; }
    // Posed, not corrected (what shot sculpts are recorded against), evaluated without a preview.
    virtual std::shared_ptr<const GuideHierarchy> posedHierarchy() { return shownHierarchy(); }
    // A stored text: partData, flowData, correctiveData, shotLayer, graph, sculptData (guides and
    // maps above).
    virtual std::string storedText(StrokeField field) = 0;
    // Sculpt nodes (sculpt.h): the Sculpt node strokes shape ("" none, the default). While it names
    // one, shaping brushes reshape that node's output guides (sculptInput) instead of the stored
    // guides, preview through previewSculpt, and commit the node's edits (StrokeField::Sculpts);
    // the guides shown (shownHierarchy, for hover and pick) are its output.
    virtual std::string sculptNode() { return {}; }
    // What the node is fed, its strength and its stored edits (GroomSession::sculptInput). Throws
    // std::exception to refuse (no such node...).
    virtual SculptInput sculptInput(const std::string& node);
    // A live sculpt stroke: the node's edits as the stroke has them and its working guides (null
    // edits: the stroke is over, the stored edits stand).
    virtual void previewSculpt(const std::string& /*node*/, SculptPtr /*edits*/, std::shared_ptr<const GuideHierarchy> /*shown*/) {}
    virtual const ScalpMaps& scalpMaps() = 0;
    virtual std::vector<GuideId> selectedGuides() { return {}; }
    virtual float softSelectRadius() { return 0.0f; }
    virtual std::set<std::string> hiddenGroups() { return {}; }
    virtual int guideSmoothing() { return 0; }  // how hover lines are drawn (smoothPolyline)
    // Shot sculpting: strokes reshape the groom as shown and record a corrective at this time.
    virtual bool shotSculpt() { return false; }
    virtual double seconds() { return 0.0; }
    virtual long frame() { return 0; }  // names a new shot layer ("shot<frame>")
    // Keyable corrective weights by slot (useLiveWeights): a shot sculpt is recorded against the
    // weights as shown when the host has them.
    virtual bool usesLiveCorrectiveWeights() { return false; }
    virtual float liveCorrectiveWeight(int /*slot*/) { return 1.0f; }
    // A texture map's paint (the groom's map `name`, opened from its file or blank at `resolution`
    // with `channels`), kept by the host between strokes while it holds what they painted. Throws
    // std::exception to refuse ("this groom has no texture map named x"). dropTexturePaint: the
    // stroke's changes were dropped, open the file again next time.
    virtual std::shared_ptr<TexturePaint> texturePaint(const std::string& name, int channels, int resolution);
    virtual void dropTexturePaint() {}
    virtual std::string scalpMapChannel() { return "rgb"; }  // a color map shown as rgb, or r g b a as gray

    // Previews of the live stroke (the objects are the tool's; it edits them between calls).
    virtual void previewHierarchy(std::shared_ptr<GuideHierarchy> preview, bool final) = 0;
    // Parts and flow previewed bound to scalp() (as the tools edit them), not to the cage.
    virtual void previewOverlays(std::shared_ptr<const PartSet> parts, std::shared_ptr<const FlowSet> flow) = 0;
    virtual void previewVertexMap(const std::string& name, std::shared_ptr<const std::vector<float>> values, int channels) = 0;
    virtual void previewTextureMap(const std::string& name, std::shared_ptr<const TexturePreview> image) = 0;
    virtual void liveStroke(bool on) = 0;

    // The stroke's result: one undo step.
    virtual void commit(StrokeEdit edit) = 0;
    virtual void warn(const std::string& message) = 0;
};

// The host for a GroomSession alone (Python, a custom app): inputs from the session, previews to
// its preview setters, edits applied to it and kept for undo and redo. The selection, hidden
// groups, shot sculpting and texture maps' paints are set on the host.
class SessionStrokeHost : public StrokeHost {
public:
    explicit SessionStrokeHost(GroomSession& session);

    MeshPtr scalp() override;
    MeshPtr paintScalp() override;
    std::shared_ptr<const GuideHierarchy> authoredHierarchy() override;
    // The session's sculpt target's guides while it has one (GroomSession::sculptHierarchy).
    std::shared_ptr<const GuideHierarchy> shownHierarchy() override;
    std::uint64_t shownHierarchyVersion() override;
    // The session's sculpt target (GroomSession::setSculptTarget), what it is fed, its previews.
    std::string sculptNode() override;
    SculptInput sculptInput(const std::string& node) override;
    void previewSculpt(const std::string& node, SculptPtr edits, std::shared_ptr<const GuideHierarchy> shown) override;
    std::shared_ptr<const GuideHierarchy> posedHierarchy() override;
    std::string storedText(StrokeField field) override;
    const ScalpMaps& scalpMaps() override;
    std::vector<GuideId> selectedGuides() override { return selection_; }
    float softSelectRadius() override { return softRadius_; }
    std::set<std::string> hiddenGroups() override { return hidden_; }
    int guideSmoothing() override { return smoothing_; }
    bool shotSculpt() override { return shot_; }
    double seconds() override { return seconds_; }
    long frame() override { return frame_; }
    bool usesLiveCorrectiveWeights() override { return !weights_.empty(); }
    float liveCorrectiveWeight(int slot) override {
        const auto it = weights_.find(slot);
        return it == weights_.end() ? 1.0f : it->second;
    }
    std::shared_ptr<TexturePaint> texturePaint(const std::string& name, int channels, int resolution) override;
    void previewHierarchy(std::shared_ptr<GuideHierarchy> preview, bool final) override;
    void previewOverlays(std::shared_ptr<const PartSet> parts, std::shared_ptr<const FlowSet> flow) override;
    void previewVertexMap(const std::string& name, std::shared_ptr<const std::vector<float>> values, int channels) override;
    void previewTextureMap(const std::string& name, std::shared_ptr<const TexturePreview> image) override;
    void liveStroke(bool on) override;
    // Records the stored texts before the edit, applies it (texts to the session, tiles to the
    // map's paint) and keeps it for undo; clears what could be redone.
    void commit(StrokeEdit edit) override;
    void warn(const std::string& message) override { warnings_.push_back(message); }

    void setSelection(std::vector<GuideId> ids, float softRadius = 0.0f) {
        selection_ = std::move(ids);
        softRadius_ = softRadius;
    }
    void setHiddenGroups(std::set<std::string> groups) { hidden_ = std::move(groups); }
    void setGuideSmoothing(int smoothing) { smoothing_ = smoothing; }
    void setShotSculpt(bool on, double seconds = 0.0, long frame = 0, std::string layer = {});
    // The session's live corrective weights (setCorrectiveWeights), for recording shot sculpts.
    void setCorrectiveWeights(std::map<int, float> weights) { weights_ = std::move(weights); }
    // A texture map's paint by name (painted by strokes on that map; the host writes no file).
    void setTexturePaint(const std::string& name, std::shared_ptr<TexturePaint> paint);
    std::shared_ptr<TexturePaint> paintOf(const std::string& name) const;

    bool undo();  // false: nothing to undo
    bool redo();
    const std::vector<StrokeEdit>& edits() const { return done_; }
    const std::vector<std::string>& warnings() const { return warnings_; }
    void clearWarnings() { warnings_.clear(); }
    GroomSession& session() { return session_; }

private:
    void applyEdit(const StrokeEdit& edit, bool after);

    GroomSession& session_;
    std::vector<GuideId> selection_;
    float softRadius_ = 0.0f;
    std::set<std::string> hidden_;
    int smoothing_ = 0;
    bool shot_ = false;
    double seconds_ = 0.0;
    long frame_ = 0;
    std::string shotLayer_;
    std::map<int, float> weights_;
    std::map<std::string, std::shared_ptr<TexturePaint>> paints_;
    std::vector<StrokeEdit> done_, undone_;
    std::vector<std::string> warnings_;
};

// ---- The interactive tool -------------------------------------------------------------------------

// Modifier keys held when a stroke starts.
enum StrokeModifier : unsigned { kStrokeCtrl = 1u, kStrokeShift = 2u };

// Parts and flow are stored bound to the cage (parts.h, flow.h: whatever the Scalp node's subdivision
// level); the tools draw and brush them on the scalp as the host shows it, refined when it is
// subdivided. The stored data is carried there to be edited (shown) and back to be stored
// (stored): each point to the same place of the surface, both ways (SubdivisionPlan::refinedRoot).
struct OverlayBinding {
    MeshPtr scalp, cage;
    std::shared_ptr<const SubdivisionPlan> plan;  // null: the scalp is the cage

    // The binding for the host's scalp (StrokeHost::scalp) and its cage (paintScalp).
    static OverlayBinding of(StrokeHost& host, const MeshPtr& scalp);
    PartSet shown(const PartSet& stored) const;
    PartSet stored(const PartSet& shown) const;
    FlowSet shown(const FlowSet& stored) const;
    FlowSet stored(const FlowSet& shown) const;
};

// What a release did.
enum class StrokeRelease {
    None,        // nothing was pressed
    Stroke,      // a stroke ended (committed, or nothing changed)
};

// The interactive brush, Place, Draw, Part and Flow tools. Mouse events come in port pixels
// (origin bottom left) of a ToolView. A stroke starts on hair or on the scalp under the cursor
// (brushes: the guide point nearest the cursor within the brush circle, hair sticking out past
// the scalp counting, else the scalp; brushes keep looking while the cursor moves), drags in the
// plane through that point facing the camera, weighs everything with the screen circle's falloff
// (nothing the scalp hides with maskByScalp), previews through the host and commits one
// StrokeEdit when it ends. Ctrl held at the press: the opposite action (signed brushes flip, paint
// goes to 1 - value, Cut grows, Part and Flow erase). Shift: a drag (Brush tool) smooths, the
// brush's mode coming back when it ends; a click does nothing (selecting guides is Ctrl+Shift,
// the hosts' own: a click adds the guide under it, a drag paints the selection). The
// pen's pressure scales strength (settings.pressure) and map smoothing. Not thread-safe; kept
// between strokes (what the scalp hides from the camera, hover's projected guides, a vertex map's
// neighbors, the texture preview).
class BrushTool {
public:
    BrushTool();
    ~BrushTool();
    BrushTool(const BrushTool&) = delete;
    BrushTool& operator=(const BrushTool&) = delete;

    BrushToolSettings settings;

    // Mouse events. press starts a stroke (or with Shift waits for a drag: smoothing); drag
    // continues it; release ends and commits it.
    void press(StrokeHost& host, const ToolView& view, float x, float y, float pressure, unsigned modifiers);
    void drag(StrokeHost& host, const ToolView& view, float x, float y, float pressure);
    StrokeRelease release(StrokeHost& host, const ToolView& view);

    // A stroke from its first point, as press without Shift (Ctrl in `modifiers` inverts); false
    // when none started (the host said why). moveTo continues it, end commits it.
    bool begin(StrokeHost& host, const ToolView& view, float x, float y, float pressure, unsigned modifiers = 0);
    void moveTo(StrokeHost& host, const ToolView& view, float x, float y, float pressure);
    void end(StrokeHost& host, const ToolView& view);
    // Drops a stroke without committing it (an error): previews off, a map stroke's changes
    // undone. `host` may be null (none to tell).
    void cancel(StrokeHost* host);
    // A whole stroke through press, drag and release from points in the view (Shift: smoothing
    // for this stroke). `afterDab` runs after each point past the first (a host redraws). True
    // when the stroke changed something.
    bool stroke(StrokeHost& host, const ToolView& view, const std::vector<std::array<float, 2>>& points,
                float pressure = 1.0f, unsigned modifiers = 0, const std::function<void()>& afterDab = {});

    bool stroking() const { return stroking_; }
    // A Shift press waiting for a drag (smoothing) or a release (a Shift+click): drags go on.
    bool shiftPending() const { return shiftPending_; }
    bool changed() const { return changed_ || mapChanged_; }
    // Part and Flow: the path drawn so far on the scalp (the host draws it until it is committed).
    const std::vector<Vec3f>& drawnPath() const { return drawPath_; }
    // The mode the tool is set to: during a Shift+drag's smoothing still the tool's own mode.
    std::string toolModeName() const;

    // The level (and group) of the guide nearest (x, y) on screen within the brush circle (8 to 60
    // pixels) becomes the brush's. Returns the level, -2 when no guide is there.
    int pickLevel(StrokeHost& host, const ToolView& view, float x, float y);
    // Hovering at (x, y): the guides the brush would touch (its level or group, not hidden), as
    // drawn (smoothed), at most 4001.
    std::vector<std::vector<Vec3f>> affectedLines(StrokeHost& host, const ToolView& view, float x, float y);

private:
    struct HoverCache;

    bool startStroke(StrokeHost& host, const ToolView& view, float x, float y);
    BrushSettings pressed() const;  // the brush with the pen's pressure and Ctrl applied
    BrushWeightFn screenWeight(const ToolView& view, float x, float y) const;
    std::shared_ptr<const ScalpVisibility> visibility(const MeshPtr& scalp, const ToolView& view);
    bool anchorAt(const ToolView& view, float x, float y, const Vec3f& origin, const Vec3f& direction, Vec3f& anchor) const;
    float worldRadiusAt(const ToolView& view, const Vec3f& center, float x, float y) const;
    void stampAt(StrokeHost& host, const ToolView& view, float x, float y, bool first);
    void stampOverlays(const ToolView& view, const Vec3f& center, const Vec3f& drag, float x, float y);
    void rebuildDrawn();
    bool beginMapStroke(StrokeHost& host);
    void stampMap(StrokeHost& host, const ToolView& view, const Vec3f& center, float x, float y, AlphaStamp alpha = {});
    // An alpha stroke's dabs up to the cursor (stampsAlpha), and one of them at (x, y).
    void stampAlpha(StrokeHost& host, const ToolView& view, float x, float y);
    void alphaDab(StrokeHost& host, const ToolView& view, float x, float y, float direction);
    void finishAlpha(StrokeHost& host, const ToolView& view);  // the click's dab still waiting, if any
    void updateTexturePreview(StrokeHost& host);
    void commitMap(StrokeHost& host);
    void dropMapStroke(StrokeHost* host);
    void commitPart(StrokeHost& host);
    void commitFlow(StrokeHost& host, const ToolView& view);
    void commitShot(StrokeHost& host);
    // A Sculpt node's stroke: its edits recorded from the working guides and previewed.
    void previewGuides(StrokeHost& host);
    void commitSculpt(StrokeHost& host);

    float pressure_ = 1.0f;
    std::shared_ptr<GuideHierarchy> working_;
    // The parts and flow arrows a brush stroke shapes (or erases): working copies of the stored
    // ones, previewed live and committed with the guides.
    std::shared_ptr<PartSet> workingParts_;
    std::shared_ptr<FlowSet> workingFlow_;
    bool partsChanged_ = false, flowChanged_ = false;
    bool overlayStamped_ = false;  // the last stamp changed parts or arrows
    MeshPtr scalp_;
    OverlayBinding overlays_;  // parts and flow: the stored ones on scalp_ (set as a stroke starts)
    MeshPtr paintScalp_;  // a vertex map's stroke: the mesh it paints (StrokeHost::paintScalp)
    std::shared_ptr<const ScalpVisibility> visibility_;
    BrushStrokeCache strokeCache_;
    std::shared_ptr<HoverCache> hover_;
    bool stroking_ = false;
    bool changed_ = false;
    bool hitScalp_ = false;  // the stroke has started (on hair or on the scalp)
    bool inverted_ = false;  // Ctrl held at press
    Vec3f planePoint_, planeNormal_, last_;
    std::shared_ptr<GuideHierarchy> base_;  // Draw: the guides before the stroke
    std::set<GuideId> placedBefore_;        // Place, Draw: the guides before the stroke (solo new guides)
    std::vector<Vec3f> drawPath_;
    // A scalp map stroke: the vertex map's working values or the texture map's paint (the host's,
    // recording the stroke's tiles), previewed live, committed when it ends.
    bool mapStroke_ = false;
    std::string paintMapName_;
    std::shared_ptr<std::vector<float>> workingVertexMap_;
    std::shared_ptr<const std::vector<float>> startingVertexMap_;
    std::shared_ptr<TexturePaint> paint_;
    std::shared_ptr<TexturePreview> preview_;
    int workingChannels_ = 1;
    VertexNeighbors neighbors_;
    std::uint64_t neighborsScalp_ = 0;
    bool mapChanged_ = false;
    std::chrono::steady_clock::time_point lastTexturePreview_{};
    // An alpha stroke: where its last dab is (pixels), whether the first waits for the stroke's
    // direction (AlphaRotation::Stroke), and the dabs so far (Random's angles).
    bool alphaStarted_ = false, alphaPending_ = false;
    float alphaX_ = 0.0f, alphaY_ = 0.0f;
    std::uint64_t alphaStroke_ = 0, alphaDabs_ = 0;
    // Shot sculpting: this stroke records a corrective.
    bool shot_ = false;
    // A stroke on a Sculpt node's guides (StrokeHost::sculptNode): what the node is fed, its
    // edits as the stroke has made them so far.
    bool sculpt_ = false;
    SculptInput sculptIn_;
    SculptEdits sculptEdits_;
    // Shift: pending until a drag (smoothing) or a release (a click: pick the level).
    bool shiftPending_ = false;
    bool shiftSmoothing_ = false;
    float pressX_ = 0.0f, pressY_ = 0.0f;
    BrushMode savedMode_ = BrushMode::Comb;
};

// ---- Scripted strokes ------------------------------------------------------------------------------

// A stroke along world points (groomLabBrushStroke -point, groomlab.brush_stroke): brush spheres
// of s.brush.radius along `path` (applyBrushStroke's spacing for guides, a quarter radius for
// maps). Place adds a guide at the scalp point nearest each point; Draw makes one guide shaped
// like the points; Part adds a part through them; Flow lays arrows along them (every half radius;
// negative strength erases) or, with s.flowSink, sinks at them. A brush also shapes the parts and arrows its targets name. The edit
// is handed to host.commit. Returns the vertices or texels a map stroke changed (0 otherwise).
// Throws std::runtime_error or std::invalid_argument (no scalp, bad stored data, an unnamed map).
// With a Sculpt node targeted (StrokeHost::sculptNode) a shaping brush reshapes that node's guides
// and commits its edits (StrokeField::Sculpts), as the interactive tool does.
std::size_t scriptedStroke(StrokeHost& host, BrushToolSettings s, const std::vector<Vec3f>& path);

// Whether a Sculpt node takes this brush: the shaping ones (comb, smooth, lift, length, twist, curl,
// straighten, attract, cut, direction, noise), not place, draw, part, flow, erase, paint, shape or
// slide. "" when it does, else why not.
std::string sculptRefusal(const BrushToolSettings& s);

}  // namespace groomlab
