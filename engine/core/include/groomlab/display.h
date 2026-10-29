// SPDX-License-Identifier: Apache-2.0
#pragma once

// Viewport geometry for any host (docs/SESSION.md, "Display"): what Maya's Viewport 2.0 override
// draws, as flat buffers, so a standalone OpenGL/Vulkan viewer, a Houdini or Blender drawer or
// Python draws a groom exactly as Maya does. A host hands a DisplayBuilder the view it evaluated
// (DisplayInputs, from a GroomSession's ViewResult) and its display settings (the shape's
// display attributes: colors, modes, ribbons, guides, overlays...), and gets one DisplayItem per
// render item: buffers rebuilt only when what they draw changed, versions that say so, and the
// item's style (color, line width, layer); the geometry's flags say how it shades (lit hair,
// surface, cards, overlay lines, the map's image, per-vertex colors). The host only uploads
// buffers and keeps its shaders.
//
// Everything is world space. Buffers are float xyz / rgba / uv streams and uint32 indices.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "groomlab/cards.h"
#include "groomlab/flow.h"
#include "groomlab/graph.h"
#include "groomlab/groom_session.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/instances.h"
#include "groomlab/math.h"
#include "groomlab/parts.h"
#include "groomlab/scales.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_geometry.h"
#include "groomlab/strand_set.h"
#include "groomlab/texture.h"

namespace groomlab {

// ---- Buffers ----------------------------------------------------------------------------------

struct StrandColors;
struct StrandLines;

// One render item's geometry: positions (xyz) and index pairs (lines) or triples (triangles),
// optional per-vertex RGBA colors (empty: the item's color). Lit hair also has per-vertex
// normals, tangents (along the strand) and uvs (u alone, root to tip: `uvFloats` 1): Maya's hair
// tube shading needs them (the bitangent across the strand, tangent x normal, it does not read:
// a host's shader takes the cross product where it wants one); a shaded surface has normals
// only. Cards and scales carry their shading normal as `bitangents` and atlas uvs (`uvFloats` 2).
// Strand lines built for a host that writes them straight into its own buffers
// (DisplayBuilder::setWriteLines) leave the vectors empty and carry `lines` instead; so do the
// ribbons for a host that expands them from the lines itself (DisplayBuilder::setRibbonLines:
// `ribbons`, materialized() builds them as strandRibbons does).
struct DisplayGeometry {
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    std::vector<float> colors;
    std::vector<float> normals, tangents, bitangents, uvs;
    std::size_t uvFloats = 2;                  // floats a vertex in `uvs`: 1 (u) for lit strands and ribbons, else 2 (u, v)
    std::shared_ptr<const StrandLines> lines;  // the streams, to be written by the host (vectors empty)
    bool triangles = false;
    bool surface = false;       // a shaded surface (normals only, Lambert-like), not hair
    bool cardTextures = false;  // game cards with their baked maps (normals and atlas uvs; the card material)
    // Scales (scales.h): a surface lit from its normals (the bitangents again, as the cards'), in
    // its per-vertex colors, highlights along the tangents; uvs x base to tip (the root-to-tip ramp).
    bool scaleSurface = false;
    // Guides, parts, flow: lines drawn over the hair (the overlay material pulls them toward the
    // camera by the hair's depth, DisplayFrame::overlayPull; overlayPullLimits stops them short of
    // what is in the way).
    bool overlay = false;
    bool mapTexture = false;  // drawn unlit with the scalp map's image at its uvs
    // Ribbons carried as `lines` (triangles): two vertices a point (vertexCount), two triangles a
    // segment, as wide as each point's width times `ribbonWidthScale` (strandRibbons); lit, in the
    // lines' colors (the strands' own) when they carry any; unlit, in their colors one a strand
    // (StrandColors::colors) with the key light baked in.
    bool ribbons = false;
    float ribbonWidthScale = 1.0f;
    bool lit() const;
    bool empty() const;
    std::size_t vertexCount() const;
    std::size_t indexCount() const;
    // A hash of every stream's bytes and the flags: equal digests, equal buffers (`lines` hash as
    // the vectors they write).
    std::uint64_t digest() const;
    // The same geometry with `lines` written into the vectors (a copy when there are none).
    DisplayGeometry materialized() const;
};

// A hash of a buffer's bytes (DisplayGeometry::digest, tests and hosts comparing uploads).
std::uint64_t bufferDigest(const void* data, std::size_t bytes);

// ---- Theme ------------------------------------------------------------------------------------

// The overlays' colors (linear RGB), set by a host's UI theme (Maya: groomLabDisplayTheme).
struct DisplayTheme {
    using RGB = std::array<float, 3>;
    using RGBA = std::array<float, 4>;
    float pointSize = 6.0f;  // pixels
    RGBA selected{1.0f, 1.0f, 1.0f, 1.0f};
    RGBA softSelected{1.0f, 0.55f, 0.15f, 1.0f};
    RGB frozen{0.45f, 0.6f, 0.85f};
    RGBA parts{1.0f, 0.2f, 0.15f, 1.0f};
    RGBA flow{1.0f, 0.55f, 0.1f, 1.0f};
    RGBA flowField{0.62f, 0.7f, 0.8f, 1.0f};
    RGBA tubes{0.55f, 0.55f, 0.6f, 1.0f};
    RGBA instances{0.75f, 0.9f, 1.0f, 1.0f};
    RGBA cards{0.58f, 0.5f, 0.42f, 1.0f};  // game cards without their maps (before a bake, or textures off)
    RGB clumpCenters{1.0f, 0.3f, 0.85f};    // the dots of a viewed Clump's centers
    std::uint64_t version = 1;  // bumped on every change: guides rebuild
};

// ---- Colors -----------------------------------------------------------------------------------

// The strands' color when nothing else colors them.
constexpr std::array<float, 3> kDefaultStrandColor{0.62f, 0.45f, 0.28f};
// The guide levels a viewport draws (deeper ones are not drawn).
constexpr int kMaxDrawnGuideLevels = 8;

std::array<float, 3> hueColor(float hue, float saturation, float value);

// The automatic guide colors' hue turn for a groom, from its name (Maya's groom transform,
// Blender's groom object, the app's groom file name): deterministic (FNV-1a of the name, mixed),
// so two grooms' guide groups start from different hues, names a digit apart too. In [0, 1); 0 for "" (no turn).
float guideHueSeed(std::string_view groomName);
// Every guide group's automatic color (linear RGB; every host's, through
// DisplaySettings::guideColorSeed): a hue of its own, child groups included, in golden-ratio steps
// from the seed in the order the groups were made (a new group leaves the others' colors as they
// were). A group whose step lands near its parent's hue or an earlier sibling's turns away from
// them, so a parent and its children, and siblings, never look alike. A group's own color
// (GuideGroup::color, DisplaySettings::guideGroupColors) is drawn over it.
std::map<GroupId, std::array<float, 3>> autoGuideColors(const GuideHierarchy& hierarchy, float seed);
// Blue, cyan, green, yellow, red over t in [0, 1].
std::array<float, 3> rampColor(float t);
// Density heatmap per strand (strands per area around each root on a log scale around the
// median: blue at a quarter or less, green at it, red at 4x or more). Fewer than 9 strands: `out`
// is left as it is.
void densityColors(const StrandSet& strands, std::vector<std::array<float, 3>>& out);

// Strand color modes (Maya's strandColorMode).
enum StrandColorMode { kSolidColors = 0, kColorSetColors, kAttributeColors, kRandomColors, kClumpColors, kDensityColors };
// One color per strand for a color mode: the color set `source` at the root, the strand channel
// `source` from lo to hi on the ramp, a random hue per strand, a hue per clump (channel
// `source`), the density heatmap; kDefaultStrandColor where there is nothing to read.
std::vector<std::array<float, 3>> strandColors(const StrandSet& strands, const ScalpMesh* scalp, int mode,
                                               const std::string& source, float lo, float hi);
// Per point RGBA (alpha 1) from one color per strand.
std::vector<float> strandPointColors(const StrandSet& strands, const std::vector<std::array<float, 3>>& colors);
void writeStrandPointColors(const StrandSet& strands, const std::vector<std::array<float, 3>>& colors, float* rgba);

// A strand channel's mean per strand set, kept until the set's version changes (the lit color's
// HairColor channels, a material's user data).
class StrandChannelMeans {
public:
    // The mean of `channel` over `strands` (found: the set has it, non empty).
    float mean(const StrandSet* strands, std::uint64_t version, const std::string& channel, bool* found);

private:
    std::uint64_t version_ = ~std::uint64_t{0};
    std::map<std::string, std::pair<bool, float>> means_;
};

// The lit strand color without a material: a HairColor node's RGB (colorR/G/B), else its
// melanin channels as the render shows them, else `displayColor`. The mean over the strands: the
// lit item's one color, which the strands' own colors (strandLookColors) replace when they vary.
std::array<float, 3> strandsLookColor(const StrandSet* strands, std::uint64_t version, const std::array<float, 3>& displayColor,
                                      StrandChannelMeans& means);

// Each strand's own lit color, as the render shows it: HairColor's per-point colors (colorR/G/B
// per point: a tip blend), else its per-strand colorR/G/B, else the color of its melanin and
// redness (melaninColor). Null when the strands carry none of them, or when every point reads
// the same color (strandsLookColor gives it: the lit item's own color, no stream to keep).
// `appearance`: the colors as the hair looks (hair_color.h hairAppearanceColor), for a host drawing
// them without a hair BSDF (Maya's hair tube shading, the app's plain modes); else the base color
// a hair BSDF takes (GroomLab's hair shading).
std::shared_ptr<const StrandColors> strandLookColors(const StrandsPtr& strands, bool appearance = false);
// With Wet's wetness channel (hair_color.h wetShading) the colors are kept even when they do not
// vary, the wetness in their alpha; and where the strands carry no colors of their own (or the
// host shows a material's color), wetLookColors gives every strand `color` with the wetness. Null
// without a wetness channel.
std::shared_ptr<const StrandColors> wetLookColors(const StrandsPtr& strands, const std::array<float, 3>& color);

// The hair shading's highlight for a HairLook: a warm white scaled by its specular, and the
// lobe's power from its roughness (narrow at 0, broad at 1).
struct HairHighlight {
    std::array<float, 3> color;
    float power;
};
HairHighlight hairHighlight(const HairLook& look);

// The root-to-tip brightness ramp the lit hair's color map takes (`width` RGBA8 texels: roots a
// little darker, as rendered hair reads).
std::vector<std::uint8_t> rootToTipRamp(unsigned width = 32);

// Groups' own guide colors: "name:#rrggbb ..." (sRGB, returned linear).
std::map<std::string, std::array<float, 3>> parseGuideGroupColors(const std::string& text);
// A linear color as "#rrggbb" (sRGB, lower case): parseGuideGroupColors' reverse (hosts' swatches).
std::string guideColorHex(const std::array<float, 3>& linear);
// Hidden guide groups: names separated by spaces or commas.
std::set<std::string> parseHiddenGuideGroups(const std::string& text);

// ---- Strands ----------------------------------------------------------------------------------

void appendPolyline(DisplayGeometry& geometry, const Vec3f* points, std::size_t count);
// Every strand as line segments.
void appendStrandLines(DisplayGeometry& geometry, const StrandSet& strands);
// Shading frames per strand point (lit lines): normals leaning out of the scalp, tangents along
// the strand, u root to tip (uvFloats 1).
void addLineFrames(DisplayGeometry& geometry, const StrandSet& strands, const ScalpMesh* scalp);
// Ribbons facing away from the scalp at each root, as wide as the strand x widthScale. Lit:
// shading frames (the uvs u plus twice the strand's number, lineIdOf, as the lines'), and
// `litColors` per vertex when given (the strands' own colors); else `colors` (per strand) with a
// fixed key light baked in.
DisplayGeometry strandRibbons(const StrandSet& strands, const ScalpMesh* scalp, const std::vector<std::array<float, 3>>& colors,
                              float widthScale, bool lit, const StrandColors* litColors = nullptr);
// The fixed key light unlit ribbons bake in (each vertex 0.35 + 0.65 x |its face normal . light|
// of its strand color): a host expanding ribbons itself (groomlab/gpu/line_frames.h) uses it too.
Vec3f ribbonKeyLight();
// The width ribbons take where the strands carry none (a host reading the lines' uvs: where they
// say kNoLineWidth).
constexpr float kRibbonDefaultWidth = 0.01f;
// Roots as small hexagonal dots on the scalp (a Scatter shown alone).
void appendRootDots(DisplayGeometry& geometry, const StrandSet& strands, const ScalpMesh* scalp);
// How far overlays are pulled toward the camera: the longest strand (x 1.05).
float overlayPullDistance(const StrandSet* strands);

// ---- Guides -----------------------------------------------------------------------------------

// A guide as drawn: a smooth curve through its points (`subdivisions` pieces per segment);
// returns the vertices added.
std::size_t appendGuideCurve(DisplayGeometry& geometry, const std::vector<Vec3f>& points, int subdivisions);
// The guides whose tubes the Shape brush's wire shows: every guide of two or more points not in a
// hidden group (parseHiddenGuideGroups of `hiddenGuideGroups`) and not frozen (its `freeze` layer,
// inherited, 0.5 or more): the ones the brush can shape. In the hierarchy's order.
std::vector<const HierarchyGuide*> activeTubeGuides(const GuideHierarchy& hierarchy, const std::string& hiddenGuideGroups,
                                                    const std::string& freeze = "freeze");
// A guide's tube as a shaded surface: its shape's outline at rings along it, each section turned,
// flattened and sized as the tube says (tube.h; widths times scale, the size shown), capped at the tip.
void appendTubeSurface(DisplayGeometry& geometry, const HierarchyGuide& guide, float scale = 1.0f);
// The same tube as a wire (what the hosts draw while the Shape brush is on): its shape's outline at
// every guide point, where the sections the brush paints sit (every `ringStep`-th point's, the
// root's and the tip's always), and `lengthwise` lines along the tube through the same outline
// points. Line segments as pairs of points, appended to `out`; nothing for a guide of fewer than
// two points.
void appendTubeWire(std::vector<Vec3f>& out, const HierarchyGuide& guide, float scale = 1.0f, std::size_t lengthwise = 8,
                    std::size_t ringStep = 1);

// ---- Instances --------------------------------------------------------------------------------

// Instances draw their prototype meshes (InstancePrototype, instances.h: the host's scene meshes,
// the core's built-in ones).
constexpr std::size_t kMaxDrawnInstances = 200000;
// Triangles the instances' meshes may take together; past it (and where a prototype is missing)
// instances draw as small wire octahedra.
constexpr std::size_t kInstanceTriangleBudget = 2000000;
void appendInstances(DisplayGeometry& markers, DisplayGeometry& meshes, const std::vector<InstanceSet::Placed>& placed,
                     const std::vector<const InstancePrototype*>& prototypes);
// The scales' mesh as the viewport draws it (DisplayGeometry::scaleSurface).
DisplayGeometry scaleGeometry(const ScaleMesh& scales);

// ---- Overlays ---------------------------------------------------------------------------------

void appendPartLines(DisplayGeometry& geometry, const PartLines& parts);
// Flow arrows (strokes as smooth curves with heads, lone pins, sinks' rings and streamlines) and
// the field sampled over the scalp.
void buildFlowOverlay(const FlowField& flow, const ScalpMesh& scalp, DisplayGeometry& arrows, DisplayGeometry& field);
// Per point: how much of `pull` it may move toward `camera` (0..1): all of it, or with one of
// `occluders` in the way, nine tenths of the way to it (a ray per point, every stride-th on big
// batches; parallel). A point hidden behind the scalp stays behind it: pulled right up to it, a
// guide's dot on the far side showed half of itself through the front.
std::vector<float> overlayPullLimits(const std::vector<Vec3f>& points, const Vec3f& camera, float pull,
                                     const std::vector<std::shared_ptr<const ScalpMesh>>& occluders);
std::vector<float> overlayPullLimits(const DisplayGeometry& geometry, const Vec3f& camera, float pull,
                                     const std::vector<std::shared_ptr<const ScalpMesh>>& occluders);
// The overlay material's colors stream (RGBA per vertex): the geometry's colors (or white, for
// the item's color) with each vertex's pull limit in the alpha.
void writeOverlayColors(const DisplayGeometry& geometry, const std::vector<float>& limits, float* rgba);

// ---- Cards ------------------------------------------------------------------------------------

// The Cards nodes' cards as a lit surface (normals, tangents up the card, the normals again as
// bitangents: the card effect's, which two-sided lighting never turns round) with the atlas uvs
// when `textured` (else zeros) and each card's tint as colors (RGBA: half the tint, alpha 1).
DisplayGeometry cardGeometry(const CardMesh& cards, bool textured);
// The edges of cardGeometry's quads (not the diagonals its triangles split them by), each once, as
// lines over its positions: the cards' wireframe (Maya's wireframe on shaded).
DisplayGeometry cardWireGeometry(const DisplayGeometry& cards);

// ---- StrandMesh nodes -------------------------------------------------------------------------

// The StrandMesh nodes' meshes (buildStrandMesh's tubes and ribbons: the same points) as a lit
// surface, drawn as the scales are (scaleSurface): normals out of each tube (a ribbon's across its
// plane), tangents along the strand, each point in its strand's own color (HairColor's channels;
// `color` without them), uvs x root (0) to tip (1).
DisplayGeometry strandMeshGeometry(const std::vector<StrandMeshSource>& meshes, const ScalpMesh* scalp,
                                   const std::array<float, 3>& color);
// The feathers' rachis tubes (buildRachisMesh of the strands' feather layers: a Propagate node's
// shaftWidth) as the same lit surface, each feather in its shaft strand's color (`color` without).
DisplayGeometry rachisGeometry(const StrandSet& strands, const std::array<float, 3>& color);
// The card map: `color` (sRGB) darkened by half of `ao` (when given), the coverage in the
// alpha; RGBA8 images of one size (as read: the rows' order is kept).
std::vector<std::uint8_t> combineCardMaps(const std::uint8_t* color, const std::uint8_t* coverage, const std::uint8_t* ao,
                                          std::size_t width, std::size_t height);
// A card bake's files (<prefix>Color.tga, <prefix>Coverage.tga, <prefix>AO.tga: the hosts'
// layout) combined into the viewport's map (combineCardMaps), written as <prefix>Preview.png:
// what the app shows a bake with, for a bake another host wrote. Throws std::runtime_error when
// the maps cannot be read or differ in size.
void writeCardBakePreview(const std::string& prefix);

// ---- Scalp map --------------------------------------------------------------------------------

// The map shown on the scalp: the scalp lifted a hair off its surface, with per-vertex colors (a
// vertex map or guide layer) or, `textured`, per-corner uvs for the map's image. Empty when there
// is nothing to show.
DisplayGeometry scalpMapGeometry(const ScalpMesh* scalp, const ScalpMapView& view, bool textured);
// A texture map's file as the scalp shows it, in place on its RGBA8 pixels: a gray map opaque, a
// color map its color or one channel picked as gray (`pick` 0-3, -1 none), opaque.
void scalpMapFilePixels(std::uint8_t* rgba, std::size_t pixels, int channels, int pick);
// The same from the core's 8-bit decode of the file (Texture::Image).
std::vector<std::uint8_t> scalpMapCachedPixels(const Texture::Image& image, int channels, int pick);
// The channel a ScalpMapView picks (0-3 r g b a, -1 none).
int scalpMapPick(const std::string& channel);

// ---- Builder ----------------------------------------------------------------------------------

// The display attributes (Maya: the shape's), read by the host each update.
struct DisplaySettings {
    // Strands.
    std::array<float, 3> hairColor = kDefaultStrandColor;  // the solid/lit color (material, strandsLookColor)
    // The lit strands (lines and ribbons) in each strand's own color (strandLookColors: HairColor's
    // channels) where they vary, hairColor where they do not; off, hairColor for all (a material's
    // or the display color, which the host gives for every strand).
    bool strandLookColors = true;
    // The host draws the lit strands without a hair BSDF (Maya's hair tube shading): their own
    // colors as the hair looks (strandLookColors' appearance), not its base color.
    bool appearanceColors = false;
    int colorMode = kSolidColors;
    std::string colorSource = "clumpId";
    float colorMin = 0.0f, colorMax = 1.0f;
    bool ribbons = false;
    float ribbonWidthScale = 1.0f;  // the ribbons' widths, and the lines' with width as coverage
    // Strand lines with each point covering the share of a pixel its width covers (lineCoverage):
    // the host's shaders read it, the builder writes the widths into the lines' uvs either way.
    bool widthCoverage = true;
    // ... at the render's density: a thinned preview's strands each cover what the strands they
    // stand for would (lineCoverageAtDensity, the density the view was evaluated at:
    // ViewResult::density), so the preview reads as dense as the render. The host's shaders.
    bool coverageDensity = true;
    // Screen-space level of detail (strand_lod.h): the strand lines with width as coverage drawn by
    // class, a share of each clump's strands standing for the rest, clumps under a pixel as one
    // line; within `clumpLodError` of the coverage (StrandLodSettings::error). The hosts' drawing
    // reads them (per view), not the builder.
    bool clumpLod = true;
    float clumpLodError = 0.175f;
    bool lighting = true;
    int sculptDisplay = 0;   // while a stroke is live: 0 as ever, 1 dimmed and flat, 2 hidden
    bool cardsView = false;  // the Cards nodes' cards show instead of the strands
    bool instances = true;
    // The hair shown (the hosts' strands eye): off, nothing the groom grows is built (strands,
    // ribbons, root dots, instances, scales, cards, strand meshes: isHairItem), as sculptDisplay 2
    // hides them for a stroke; the guides, the selection and the overlays stay. A viewport toggle:
    // renders and exports never read it.
    bool hair = true;
    // Guides.
    bool guides = true, tubes = false;
    float guideWidth = 4.0f;
    // 1: the painted layer guideColorLayer, black 0 to white 1; anything else (0): each group's
    // color (its own, else autoGuideColors').
    int guideColorMode = 0;
    // The groom's name: the automatic guide colors start from its hue (guideHueSeed).
    std::string guideColorSeed;
    std::string guideColorLayer;
    std::string isolateGroup;  // a guide group's name: it and its child groups at full color, the others dimmed
    int guidePoints = 2;     // dots: 0 off, 1 roots and tips, 2 all
    int guideSmoothing = 4;  // pieces per segment (1 straight)
    std::string hiddenGuideGroups, guideGroupColors;
    // Overlays and cards.
    bool flow = true;
    bool drawCards = false;     // the cards draw here (cardsView, and no card mesh draws them)
    bool cardTextures = false;  // ... with their baked maps (loaded by the host)
    // The StrandMesh nodes' meshes show instead of the strands (when there are any).
    bool strandMeshView = false;
    bool drawStrandMesh = false;  // ... and draw here (no mesh of the host's shows them)
    DisplayTheme theme;
};

// What the host evaluated (a GroomSession's ViewResult) and what only it knows.
struct DisplayInputs {
    StrandsPtr strands;
    bool viewedRoots = false;  // the strands are a Scatter's roots: dots
    // A viewed Clump's clump centers, each its root on the scalp: dots in the
    // theme's clumpCenters color, with the guides' (ViewResult::clumpCenters). Null: none.
    std::shared_ptr<const std::vector<Vec3f>> clumpCenters;
    MeshPtr scalp;
    HierarchyPtr hierarchy;
    PartsPtr parts;
    FlowPtr flow;
    std::shared_ptr<const CardMesh> cards;
    std::vector<StrandMeshSource> strandMeshes;  // ViewResult::strandMeshes
    // Counters that move when the strands, guides, cards or strand meshes changed
    // (ViewResult::versions or the host's own).
    std::uint64_t strandsVersion = 0, hierarchyVersion = 0, cardsVersion = 0, strandMeshVersion = 0;
    // Which strands these are and how they differ from earlier ones (ViewResult::strandsSerial,
    // changesBase, strandsChanges): strand lines whose host holds those of serial changesBase are
    // written again only where the strands changed (DisplayItem::updated). 0: not known.
    std::uint64_t strandsSerial = 0, changesBase = 0;
    ChangesPtr strandsChanges;
    // Following the scalp: `strands` handed on at rest, which the host carries to the scalp now
    // itself (ViewResult::follow: Maya's viewport, on the GPU). What rides on them that the builder
    // draws, the instances, it places on their strands carried. Null: the strands are where they are.
    FollowCarryPtr follow;
    bool stroke = false;  // a brush stroke is live (sculptDisplay)
    // Guide selection weights (1 selected, below soft) and a hash of what they come from: asked
    // only when the selection rebuilds.
    std::function<std::map<GuideId, float>()> selection;
    std::uint64_t selectionHash = 0;
    // The instances' prototypes by name (null: none found).
    const std::map<std::string, InstancePrototype>* prototypes = nullptr;
    // The map shown on the scalp, and whether the host holds a texture of its image (with a
    // counter that moves when the host made another texture).
    ScalpMapView scalpMap;
    bool scalpMapTexture = false;
    std::uint64_t scalpMapTextureGeneration = 0;
    // The colored strand lines may take new colors alone (the host still has them as built).
    bool recolorable = true;

    static DisplayInputs fromView(const ViewResult& view, bool stroke = false);
};

enum class DisplayPart { Strands, Instances, Guides, Selection, Parts, Flow, Cards, ScalpMap, StrandMesh, Count };
// In drawing (upload) order.
enum class DisplayItemId {
    DimStrands, Strands, ColoredStrands, Ribbons, InstanceMarkers, InstanceMeshes, Scales, Guides, Tubes, Selected,
    SoftSelected, Parts, Flow, FlowField, Cards, ScalpMap, StrandMesh, Count
};
constexpr std::size_t kDisplayItemCount = static_cast<std::size_t>(DisplayItemId::Count);
// The hair's items: what the groom grows (the strands in every form, the instances, the scales,
// the cards, the strand meshes), which the hosts' strands eye hides (DisplaySettings::hair); the
// guides, tubes, selection, parts, flow and the scalp's map are not.
constexpr bool isHairItem(DisplayItemId id) {
    switch (id) {
    case DisplayItemId::DimStrands: case DisplayItemId::Strands: case DisplayItemId::ColoredStrands:
    case DisplayItemId::Ribbons: case DisplayItemId::InstanceMarkers: case DisplayItemId::InstanceMeshes:
    case DisplayItemId::Scales: case DisplayItemId::Cards: case DisplayItemId::StrandMesh:
        return true;
    default:
        return false;
    }
}
constexpr std::size_t kDisplayPartCount = static_cast<std::size_t>(DisplayPart::Count);

// How an item is drawn (its depth priority in Maya): dormant wire, dormant filled (surfaces),
// active wire (guides, flow field), selection (selected guides, parts, flow arrows).
enum class DisplayLayer { DormantWire, DormantFilled, ActiveWire, Selection };

// The colors of a strand set's points: one color per strand (`colors`; each strand's points its
// color), or the strands' own color channels (`channels`, r g b, held by `strands`: per point when
// `perPoint`, else per strand). Written as per-point RGBA (alpha 1): new colors for strand lines
// whose points stay, or the lit lines' own colors (strandLookColors), whose alpha is 1 plus the
// point's iridescence (FeatherColor's channel, 0-1: the hair shading's structural color; alpha 1
// without it, so a reader of the alpha as opacity sees what it always did); with Wet's wetness
// channel, the iridescence and the wetness packed (hair_color.h packWetLook).
struct StrandColors {
    StrandsPtr strands;
    std::vector<std::array<float, 3>> colors;  // per strand (without channels)
    std::array<const std::vector<float>*, 3> channels{};
    bool perPoint = false;
    const std::vector<float>* iridescence = nullptr;  // per point (strandLookColors), held by `strands`
    const std::vector<float>* wetness = nullptr;      // per point, or per strand (wetnessChannel), held by `strands`
    bool wetnessPerPoint = true;
    bool appearance = false;  // channels written as the hair's appearance (hairAppearanceColor)
    // A hash of the colors (strandLookColors, and the color modes' one a strand; 0: not known):
    // equal keys, equal colors for the same point counts, so a host holding colors of this key
    // keeps them (a tweak that moves the strands alone).
    std::uint64_t key = 0;

    std::size_t vertexCount() const { return strands ? strands->pointCount() : 0; }
    // Point `point` (an index into the set's points) of strand `strand`.
    std::array<float, 3> at(std::size_t strand, std::size_t point) const;
    // Point `point` (of strand `strand`)'s alpha: 1 + its iridescence, or that and its wetness packed.
    float alpha(std::size_t strand, std::size_t point) const;
    void write(float* rgba) const;  // vertexCount() x 4 floats
    // Strands [first, end) alone, into an array that starts at strand `first`'s first vertex (a
    // host writing part of its buffer again, DisplayItem::updated).
    void write(float* rgba, std::size_t first, std::size_t end) const;
    std::vector<float> rgba() const;
};

// Strand lines as a host writes them straight into its own buffers (a GPU buffer's mapped
// memory: no copy of them kept here): the points, the segments' index pairs, the shading frames
// when lit (normals leaning out of the scalp, tangents along the strand; u root to tip) and
// per-point colors when colored (a color mode's, flat; or lit, the strands' own:
// strandLookColors). Each writer runs in parallel and writes the very bytes the geometry's vectors
// hold when built into them (DisplayGeometry::materialized).
struct StrandLines {
    StrandLines(StrandsPtr strands, MeshPtr scalp, bool lit, std::shared_ptr<const StrandColors> colors);
    StrandsPtr strands;
    MeshPtr scalp;                               // the root normals (lit)
    bool lit = false;                            // normals, tangents and uvs
    std::shared_ptr<const StrandColors> colors;  // per-point colors (null: the item's color)

    std::size_t vertexCount() const { return strands ? strands->pointCount() : 0; }
    std::size_t indexCount() const { return indexCount_; }
    // A hash of the strands' point counts: the indices and the uvs of one float a point depend on
    // nothing else, so a host keeps them while it stays the same (a tweak, a frame of playback).
    std::uint64_t layoutKey() const { return layoutKey_; }
    // A hash of the strands' widths and IDs (0: they carry no widths, StrandSet::widths not one a
    // point): the uvs of two floats a point (u with the strand's number, width) hold while it and
    // the layout stay.
    std::uint64_t widthsKey() const { return widthsKey_; }
    // A hash of the strands' point counts and IDs (never 0): the uvs' first float a point (u with
    // the strand's number) holds while it stays, whatever the widths (a host writing the widths
    // alone into the second: groomlab/gpu/line_frames.h).
    std::uint64_t idsKey() const { return idsKey_; }
    void writePositions(float* xyz) const;
    void writeIndices(std::uint32_t* pairs) const;
    // Lit: either may be null (not written).
    void writeFrames(float* normals, float* tangents) const;
    // The same for strands [first, end) alone, into arrays that start at strand `first`'s first
    // vertex: a host writing part of its buffers again (DisplayItem::updated, strandRuns).
    void writePositions(float* xyz, std::size_t first, std::size_t end) const;
    void writeFrames(float* normals, float* tangents, std::size_t first, std::size_t end) const;
    // What writeFrames leans each strand's normals by: its root normal (the scalp's under its root,
    // a feather's vane normal, up when unrooted), xyz and a 0, 4 floats a strand: a host computing
    // the frames on the GPU from the points (groomlab/gpu/line_frames.h) sends these beside them.
    // Returns a hash of what it wrote (never 0), so a host keeps the ones it holds while it stays.
    std::uint64_t writeRootNormals(float* xyzw) const;
    // u root to tip, `floatsPerVertex` floats a vertex: 1 (the geometry's stream), or 2 for a
    // host whose shader reads a float2 (Maya's uvCoord, the app's lines): (u plus twice the
    // strand's number, lineUvId: what its hashed coverage is drawn with, lineUvU takes u back; the
    // point's width, scene units: kNoLineWidth where the strands carry none), what the lines'
    // coverage is drawn from (lineCoverage). For strands [first, end) as writePositions.
    void writeUvs(float* uvs, std::size_t floatsPerVertex = 1) const;
    void writeUvs(float* uvs, std::size_t floatsPerVertex, std::size_t first, std::size_t end) const;
    // RGBA per vertex; for strands [first, end) as writePositions.
    void writeColors(float* rgba) const { colors->write(rgba); }
    void writeColors(float* rgba, std::size_t first, std::size_t end) const { colors->write(rgba, first, end); }

private:
    std::size_t indexCount_ = 0;
    std::uint64_t layoutKey_ = 0, widthsKey_ = 0, idsKey_ = 0;
};

// ---- Width as coverage (strand lines) ---------------------------------------------------------
//
// Strand lines draw a pixel wide whatever their widths. With width as coverage (the hosts'
// DisplaySettings::widthCoverage) each point covers the share of that pixel its width covers (the
// width the uvs carry, StrandLines::writeUvs, projected to pixels in the vertex stage or the
// pixel's), so tapered tips fade, thin strands read light and strands a pixel wide or more stay
// solid: the density a ribbon of the true width shows.
//
// The coverage is drawn stochastically, never blended (hashed alpha testing, Wyman and McGuire
// 2017): a strand's fragment is kept, opaque and depth written, where its coverage reaches a
// threshold hashed from the strand (lineUvId) and the stretch of it the pixel lies on
// (lineHashThreshold), so n strands crossing a pixel cover 1 - (1 - c)^n of it on average, as
// their ribbons do, in any order (blended with depth writes, the strand in front hid the ones
// behind it and a dense groom seen from afar faded to its single front strand's coverage). The
// threshold belongs to the strand, not to the screen: a still view draws the same pixels every
// frame, and an orbit moves the pattern with the strands. On a multisampled target each strand's
// fragment covers a subset of the pixel's samples of its own (lineSampleMask), so the pixel
// takes the coverage in steps of a sample. With one sample a pixel the top of the range blends
// instead (lineKeepShare): a strand covering much of a pixel is one seen close, where strands
// seldom share a pixel and a stipple would read as noise, so it draws smooth as before; and the
// thresholds' stretches are about four pixels long (lineHashThreshold). The hosts' shaders
// do this arithmetic, one source (engine/gpu/shaders/line_coverage.glsl); these are its reference
// (and the tests').

// The width uvs carry for strands without widths: drawn opaque.
constexpr float kNoLineWidth = -1.0f;
// The least coverage drawn: none. (It was 0.05, so the thinnest strands never vanished; drawing a
// tip of 0.008 pixels as 0.05 added 3 to 7% of ink far away and doubled the color error, the tips
// weighted like the strands' middles: docs/research/clump_lod.md. Many thin strands still add up,
// each its own share.)
constexpr float kLineCoverageFloor = 0.0f;
// Pixels a scene unit spans at clip w 1: half the viewport's height in pixels times the projection's
// [1][1] (1 / tan(fov / 2) in perspective, 2 / (top - bottom) orthographic).
float linePixelScale(float projection11, float viewportHeight);
// The share of a pixel a strand `width` wide (scene units) times `widthScale` covers at clip w
// `clipW` (the view depth in perspective, 1 orthographic): width x widthScale x pixelScale / clipW,
// clamped to [floor, 1]; 1 for a negative width (kNoLineWidth) or a point at or behind the eye.
float lineCoverage(float width, float widthScale, float pixelScale, float clipW, float floor = kLineCoverageFloor);
// A thinned preview's coverage (DisplaySettings::coverageDensity): each strand a preview at
// `density` (0 to 1: the share of the strands it shows) holds stands for 1 / density strands, which
// together cover 1 - (1 - coverage)^(1 / density) of a pixel: never more than all of it. The
// coverage itself at density 1 (or 0: unknown).
float lineCoverageAtDensity(float coverage, float density);

// The strand numbers the lines' uvs carry (StrandLines::writeUvs: u + 2 x the number, so u's
// float keeps 13 bits below the point): a strand's from its ID (the same strand keeps its pattern
// from evaluation to evaluation and at any preview density), strands that share one drawn alike.
constexpr std::uint32_t kLineIdCount = 1024;
std::uint32_t lineIdOf(StrandId id);
// u and the strand's number back from the uvs' first float (strands without a number: 0).
inline float lineUvU(float encoded) { return encoded - 2.0f * std::floor(0.5f * encoded); }
inline std::uint32_t lineUvId(float encoded) { return static_cast<std::uint32_t>(std::max(0.0f, std::floor(0.5f * encoded))); }
// Jarzynski and Olano 2020's pcg3d: the integer hash the shaders run.
std::array<std::uint32_t, 3> linePcg3d(std::uint32_t x, std::uint32_t y, std::uint32_t z);
// The threshold a fragment of strand `strand` at `u` (root to tip) is kept above (its coverage at
// least that: lineSampleMask) on a target of `samples` samples a pixel, with `uPerPixel` the change
// of u from one pixel to the next along the line (the shaders' derivatives): uniform in (0, 1].
// One per strand and stretch of it a pixel long, the stretches of the two powers of two around a
// pixel blended (Wyman and McGuire's interpolation, its distribution made uniform again), so the
// pattern neither pops nor swims as the view zooms. With one sample a pixel the stretches are about
// four pixels long, so a thin strand shows as short dashes rather than scattered dots (not one
// threshold a whole strand: strands share their numbers, kLineIdCount of them, and those drawn over
// each other would come and go together). The shaders run engine/gpu/shaders/line_coverage.glsl
// (lineThreshold).
float lineHashThreshold(std::uint32_t strand, float u, float uPerPixel, int samples);
// The samples of `samples` (1: not multisampled) a fragment of coverage `coverage` covers, its
// threshold `threshold` (lineHashThreshold): floor(coverage x samples + 1 - threshold) of them
// (so coverage x samples on average, at least one where coverage reaches the threshold, and every
// one at coverage 1), a run of them rotated by the strand's own hash; 0: the fragment is not drawn.
std::uint32_t lineSampleMask(float coverage, float threshold, std::uint32_t strand, int samples);
// One sample a pixel (and the lit lines in Maya, whose fragments cannot write a mask): a fragment
// of coverage c is kept as one of coverage lineKeepShare(c, own) is (lineSampleMask with it),
// drawn with alpha c / share and blended, so its pixel takes c on average either way. The share
// is c itself while the strand's own coverage (`own`: before lineCoverageAtDensity; the same at
// density 1) is up to kLineHashedBelow (hashed, opaque: dense hair builds up), rising smoothly to
// 1 as it reaches kLineBlendedAbove (blended: the strand smooth, as close views show it). The own
// coverage says how close the view is: a thinned preview from afar, its coverage taken up by the
// density, still has many strands to a pixel, and stays hashed.
constexpr float kLineHashedBelow = 0.1f, kLineBlendedAbove = 0.4f;
float lineKeepShare(float coverage, float own);
// The screen-space level of detail (strand_lod.h) draws through the same arithmetic: a thinned
// class's strand standing for e strands takes its coverage to 1 - (1 - own)^(e / density) and is
// kept with own 0 (hashed alone: it stands for several), the shaders' lineLod (the app's lines.z);
// and a draw may keep its opaque fragments alone or its blended ones alone (lineBlend 1, 2; the
// app's lines.w), class 0's segments that may blend drawn twice so a blended fragment writes no
// depth.

struct DisplayItem {
    DisplayItemId id = DisplayItemId::Count;
    DisplayPart part = DisplayPart::Count;
    const char* name = "";  // "strands", "dimStrands", ... (stable)
    // The geometry as last built (null: never built, or not kept: DisplayBuilder::setRetain).
    std::shared_ptr<const DisplayGeometry> geometry;
    // Colors that replaced the geometry's since it was built (colored strand lines), or null.
    std::shared_ptr<const StrandColors> recolored;
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};  // the item's color (per-vertex colors: white)
    float lineWidth = 1.0f;                              // pixels (lines)
    DisplayLayer layer = DisplayLayer::DormantWire;
    std::uint64_t geometryVersion = 0;  // moves when the geometry was built again
    std::uint64_t colorVersion = 0;     // moves with it, and when only the colors were replaced
    bool rebuilt = false;   // this update built the geometry (upload it; empty: remove the item)
    bool recolor = false;   // this update replaced only its colors (upload recolored)
    // Rebuilt strand lines holding the strands and point counts of the geometry this item was last
    // built with (what the host uploaded): only these strands' points (and frames, when lit, and
    // colors, when it has them: StrandColors::key says whether any changed) differ from it, so a
    // host may write just their vertex ranges (strandRuns) and keep the rest of its buffers,
    // indices and uvs; an empty list: nothing differs. Null: write it all.
    ChangesPtr updated;
    // Rebuilt empty, but kept: the host hides the item and keeps its buffers (the strands stepping
    // back for a stroke, the dimmed ones after it), so when it comes back with the strands it held
    // (`updated`), only what the stroke changed is written. A host dropping it instead writes the
    // whole item when it comes back (it has nothing to update).
    bool keep = false;
    std::size_t vertexCount = 0;  // of the geometry as built (kept without retain)
    bool shown = false;     // the geometry as built draws something
};

// Guide point dots (drawn by the host facing the camera at a constant size).
struct GuideDots {
    std::vector<Vec3f> points;
    std::vector<std::array<float, 3>> colors;
};

struct DisplayFrame {
    std::array<DisplayItem, kDisplayItemCount> items;
    std::array<bool, kDisplayPartCount> rebuilt{};  // parts built this update
    GuideDots dots;                                 // rebuilt with the guides
    float pointSize = 0.0f;                         // the dots' size (pixels)
    float overlayPull = 0.0f;                       // overlays' pull toward the camera (scene units)
    const DisplayItem& item(DisplayItemId id) const { return items[static_cast<std::size_t>(id)]; }
};

// The strands `changed` lists (sorted) as runs [first, end) to write again; runs fewer than
// `gap` strands apart are joined (writing a few strands that did not change costs less than
// another write).
std::vector<std::pair<std::size_t, std::size_t>> strandRuns(const std::vector<std::uint32_t>& changed, std::size_t gap = 32);

// Keeps what each part was built for and builds again only what changed. One host thread at a
// time; builders run in parallel inside (output equal to a serial run).
class DisplayBuilder {
public:
    DisplayBuilder();
    const DisplayFrame& update(const DisplayInputs& inputs, const DisplaySettings& settings);
    const DisplayFrame& frame() const { return frame_; }
    // The part builds again on the next update (its host lost the items).
    void invalidate(DisplayPart part);
    void invalidateAll();
    // The host let go of a kept item's buffers (DisplayItem::keep: the dimmed strands between
    // strokes): it holds nothing for it, and the next time the item shows it is written whole.
    // True when the item was kept (hidden, holding strands); nothing else changes.
    bool forgetKept(DisplayItemId id);
    // The host's buffers of strand-lines item `id` hold other points now (a GPU preview wrote them
    // in place), the same point counts: its next build writes all its points (its indices and uvs
    // kept), nothing rebuilt before then; kept hidden meanwhile as it was.
    void forgetHeld(DisplayItemId id) {
        Held& held = held_[static_cast<std::size_t>(id)];
        if (held.serial != 0) held.serial = ~std::uint64_t{0};  // (no strands' serial: none matches it)
    }
    // Keep the built geometry in the items (default on); off, an item holds its geometry only in
    // the update that built it (a host that uploads at once and keeps no second copy).
    void setRetain(bool retain) { retain_ = retain; }
    // On, the strand lines (strands, dimmed strands, colored strands) come as StrandLines
    // (DisplayGeometry::lines) the host writes into its buffers, their vectors left empty
    // (default off: the vectors filled).
    void setWriteLines(bool writeLines) { writeLines_ = writeLines; }
    // On (with setWriteLines), the ribbons come as StrandLines too (DisplayGeometry::ribbons), for a
    // host that expands them on the GPU from the lines (Maya: two vertices a point made by a
    // kernel from the points alone); materialized() gives the triangles strandRibbons builds, for
    // where it cannot (default off: the triangles built here).
    void setRibbonLines(bool ribbonLines) { ribbonLines_ = ribbonLines; }

private:
    void buildStrands(const DisplayInputs& in, const DisplaySettings& s);
    // A strand-lines item: `updated` when its host holds the strands these changed from (or
    // these), kept when `hidden` (stepping back for a stroke, or back after it).
    void putLines(DisplayItemId id, DisplayGeometry&& geometry, const DisplayInputs& in, bool hidden);
    void buildInstances(const DisplayInputs& in, const DisplaySettings& s);
    void buildGuides(const DisplayInputs& in, const DisplaySettings& s);
    void buildSelection(const DisplayInputs& in, const DisplaySettings& s);
    void buildOverlays(const DisplayInputs& in, const DisplaySettings& s);
    void buildCards(const DisplayInputs& in, const DisplaySettings& s);
    void buildStrandMesh(const DisplayInputs& in, const DisplaySettings& s);
    void buildScalpMap(const DisplayInputs& in, const DisplaySettings& s);
    bool rebuild(DisplayPart part, std::uint64_t key);
    void put(DisplayItemId id, DisplayGeometry&& geometry);
    DisplayItem& item(DisplayItemId id) { return frame_.items[static_cast<std::size_t>(id)]; }

    DisplayFrame frame_;
    ScaleMeshBuilder scales_;  // the scales' bent meshes, kept per scale (buildInstances)
    std::array<std::uint64_t, kDisplayPartCount> keys_;
    // What each strand-lines item was last built with (what its host holds): the strands' serial,
    // how its lines are written (lit, the scalp they lean out of, colored), their point counts.
    struct Held {
        std::uint64_t serial = 0, shape = 0, layout = 0;
        bool hidden = false;  // kept hidden (DisplayItem::keep said)
    };
    std::array<Held, kDisplayItemCount> held_{};
    // The clump centers last hashed into the guides' key (held, so the pointer names them) and
    // their hash: hashed once per set of centers, not on every update.
    std::shared_ptr<const std::vector<Vec3f>> clumpCentersHashed_;
    std::uint64_t clumpCentersHash_ = 0;
    // The instances as last built (buildInstances): their set (held, so the pointer names it), the
    // strands' serial they sit on, a key of those strands' points (kept while later strands hold the
    // same points: StrandChanges::pointsKept) and a digest of their widths (scaled by width).
    std::shared_ptr<const InstanceSet> instancesOf_;
    std::uint64_t instancesSerial_ = 0, instancesVersion_ = 0, instancesPoints_ = 0, instancesWidths_ = 0;
    // Following the scalp (DisplayInputs::follow): the strands at rest holding instances (a subset
    // of instancedFrom_'s), carried to the scalp on every update the instances are placed again.
    StrandsPtr instancedFrom_, instancedRest_;
    std::uint64_t strandShape_ = ~std::uint64_t{0};
    std::uint64_t pullVersion_ = ~std::uint64_t{0};
    std::uint64_t counter_ = 0;
    bool retain_ = true;
    bool writeLines_ = false;
    bool ribbonLines_ = false;
};

}  // namespace groomlab
