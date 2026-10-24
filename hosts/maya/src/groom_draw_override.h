// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <maya/MCallbackIdArray.h>
#include <maya/MBoundingBox.h>
#include <maya/MMatrix.h>
#include <maya/MObject.h>
#include <maya/MObjectHandle.h>
#include <maya/MPxSubSceneOverride.h>
#include <maya/MString.h>

#include "groomlab/display.h"
#include "groomlab/math.h"
#include "groomlab/strand_lod.h"
#include "groom_shape.h"
#include "hair_shading.h"

#include <maya/MApiNamespace.h>  // Maya's classes declared (MHWRender's are using-declarations of a
                                  // versioned namespace's: a plain forward declaration conflicts, Clang/GCC)

namespace groomlab::gpu {
class LineFrames;
}

namespace groomlab::maya {

class GpuLines;
class GpuLookPreview;
struct GpuLineStreams;
struct GpuRibbonStreams;

// How the map shown on the scalp sends a brush stroke's texels to its texture on this device:
// "region" or "regionInImage" (MTexture::update with a region, the region's rows packed or the
// whole image's), "whole" (the whole image each time), "unknown" before the first upload.
std::string scalpMapUploadMode();
// Where a texture map's file image comes from when the scalp shows it: "cache" (the core's decode
// of it, which the brush primes when it writes the file; checked once against MImage's read),
// "file" (MImage reads it: the check failed), "unknown" before the first.
std::string scalpMapFileImageSource();
// The last viewport update that uploaded geometry, timed (report-only: groomLabGraphStats
// -drawTimes): JSON {"updateMs", "buildMs", "uploadMs", "items": {item: {"acquireMs", "writeMs",
// "commitMs", "bindMs", "megabytes"}}}.
std::string lastDrawTimesJson();
// The lit strands' buffers as drawn against the CPU's (test hook: groomLabGpuPreview -checkFrames):
// JSON {"gpuFrames": whether the GPU wrote their frames (gpu_lines.h), "api", "why" (why not),
// "points", and the largest difference of "positions", "normals", "tangents", "u" (the uvs'
// first float) and "widths" (their second) from what StrandLines writes on the CPU}.
std::string gpuFramesCheckJson(const GroomShape* shape);
// The hair's density grid of the groom drawn (GroomLab's hair shading): JSON {hairShading, valid, why,
// size, cell, origin, serial, ignoredLights, gpuMs (the engine's last timed run, waited for)}.
std::string hairGridJson(const GroomShape* shape);
// Playback stopped (Maya's playingBack condition): the grooms whose hair grid was left as it was
// for some frames of the playback draw once more, making it again for the frame they stopped on.
void hairGridsPlaybackStopped();
// GPU frames were turned on or off (gpu_lines.h setGpuFramesEnabled): every groom's strands are
// written again.
void gpuFramesSwitched();
// Whether a viewport draws the GPU preview's run of a drag's values at ersion (GpuPreviewRequest):
// what its release commits is drawn already (groom_shape.h expectCommit).
bool gpuPreviewDrawn(const GroomShape* shape, std::uint64_t version);

// Viewport 2.0 drawing for groomLabShape:
// - strands as line segments (VP-1),
// - hierarchy guides as thicker-priority lines, one color per level (VP-2),
// - optional tube rings around each guide (VP-2),
// - strand colors by root color, attribute or group (VP-1), and ribbons with width (VP-3).
// The geometry and colors come from the core's DisplayBuilder (display.h: any host draws a
// groom as this does); this uploads its buffers into render items and keeps the shaders.
// Each part (strands, guides, selection, flow...) keeps its GPU buffers until
// something it draws changes: a selection click doesn't re-upload the strands,
// a color change only sets a shader parameter.
// Everything draws in world space (see GroomShape): the items carry the
// identity matrix, whatever the groom's (locked) transform holds.
class GroomSubSceneOverride : public MHWRender::MPxSubSceneOverride {
public:
    static const char* kRegistrantId;
    static MHWRender::MPxSubSceneOverride* creator(const MObject& obj);

    ~GroomSubSceneOverride() override;

    MHWRender::DrawAPI supportedDrawAPIs() const override;
    bool requiresUpdate(const MHWRender::MSubSceneContainer& container,
                        const MHWRender::MFrameContext& frameContext) const override;
    void update(MHWRender::MSubSceneContainer& container, const MHWRender::MFrameContext& frameContext) override;
    void updateGuarded(MHWRender::MSubSceneContainer& container, const MHWRender::MFrameContext& frameContext);
    bool hasUIDrawables() const override;
    bool areUIDrawablesDirty() const override;
    void addUIDrawables(MHWRender::MUIDrawManager& drawManager, const MHWRender::MFrameContext& frameContext) override;
    void addDots(MHWRender::MUIDrawManager& drawManager, const MHWRender::MFrameContext& frameContext);

    // A prototype mesh the instances draw, fetched from the scene by its name
    // (a transform or mesh shape, as the Instance node holds it): object-space
    // positions and normals per face-vertex (hard edges stay hard, as Maya
    // shades the mesh itself) and its triangles; `signature` is the scene mesh's
    // fingerprint when fetched (0: no such mesh).
    using PrototypeMesh = InstancePrototype;

private:
    struct ItemBuffers {
        // DirectX 11, lit strand lines whose frames the GPU writes (gpu_lines.h): the buffers
        // positions, normals, tangents and uvs name; destroyed after them (declared first).
        std::shared_ptr<GpuLineStreams> gpuStreams;
        // Ribbons the GPU makes from the lines (writeGpuRibbons: every buffer below then
        // names the streams'). The lines' points and uvs (u, width) they are made from, not drawn.
        std::shared_ptr<GpuRibbonStreams> ribbonStreams;
        std::unique_ptr<MHWRender::MVertexBuffer> ribbonPoints, ribbonUvs;
        std::unique_ptr<MHWRender::MVertexBuffer> positions;
        std::unique_ptr<MHWRender::MVertexBuffer> colors;
        std::unique_ptr<MHWRender::MVertexBuffer> normals, tangents, bitangents, uvs;
        std::unique_ptr<MHWRender::MIndexBuffer> indices;
        std::unique_ptr<MHWRender::MIndexBuffer> lineIndices;  // facing ribbons: their thin segments as lines
        MHWRender::MShaderInstance* shader = nullptr;
        // Which shader that is (a new number each time the item gets another): a copy made like it
        // knows it by this, not by its address, which a later shader may take once it is released.
        std::uint64_t shaderSerial = 0;
        int shaderKind = -1;  // 0 solid, 1 per-vertex colors, 2 lit hair, 3 thick solid, 4 thick per-vertex,
                              // 5 shaded surface, 6 textured cards (the card effect), 7 overlay lines,
                              // 8 an image of the item's own (a map shown on the scalp), 9 lit hair in
                              // per-vertex colors (GroomLab's hair graph: the strands' own colors)
        bool vertexColors = false;  // a colors stream bound (kind 7: the lines' own colors, else the item color)
        // Kind 9: the key of the strands' colors the colors stream holds (StrandColors::key; 0: none
        // known), so colors that did not change are not written again.
        std::uint64_t colorsKey = 0;
        // The strand lines' point counts (StrandLines::layoutKey) the indices hold, and those with
        // their widths (StrandLines::widthsKey) the uvs (u, width) hold (0: none).
        std::uint64_t layout = 0, uvsKey = 0;
        // The point counts and strand IDs (StrandLines::idsKey) the uvs' first float holds (u with
        // the strand's number), whatever their widths (0: none): new widths alone may go up alone.
        std::uint64_t uvsIdsKey = 0;
        // GPU ribbons: the point counts their triangles (and lit uvs) were written for, the widths
        // the lines' uvs hold (their layout with StrandLines::widthsKey), the colors they hold
        // (StrandColors::key; 0: none known); and the width scale, lit and unlit, they were made with.
        std::uint64_t ribbonLayout = 0, ribbonUvsKey = 0, ribbonColorsKey = 0, ribbonIdsKey = 0;
        float ribbonWidthScale = 1.0f;
        // Following the scalp on the GPU (carryOnGpu): the strands at rest (followRestVersion_) and
        // the scalp (its content hash) its points were carried for (0: none, the CPU's points).
        std::uint64_t followRest = 0, followTo = 0;
        // GPU ribbons facing the camera (true width: facing_), and the view-projection they were
        // last made for (refaceRibbons makes them again for another).
        bool facing = false;
        MMatrix facedFor;
        bool lit = false;     // strand lines with frames (normals, tangents; bitangents carry the cards' and scales' shading normal only)
        // Strand lines drawn with width as coverage (displayWidthCoverage): their shader reads the
        // uvs' widths and draws with the lines' blending (the flat lines' effect, the lit graph's
        // coverage on), so the uvs are bound with them.
        bool coverage = false;
        // Kinds 2 and 9 drawn with GroomLab's hair effect (hair_shading.h; displayHairShading 1)
        // rather than Maya's hair tube graph.
        bool hair = false;
        // Strand lines (or the ribbons' lines): their points (the hair's density grid, updateHairGrid).
        std::size_t linePoints = 0;
        bool shown = false;   // has geometry and is enabled
    };

    explicit GroomSubSceneOverride(const MObject& obj);
    bool displayFlag(const MObject& attr) const;
    bool needsUpdate() const;  // requiresUpdate's work (which must not throw)
    static void releaseGeometry(ItemBuffers& buffers);  // an item's GPU buffers, not its shader
    float floatPlug(const MObject& attr) const;
    // The display attributes as the core's builder takes them (display.h).
    DisplaySettings readSettings();
    // Uploads a geometry as an item's (hides the item when empty) and styles it. lineWidth > 1:
    // thick lines (pixels); mapTexture: the item's own image (kind 8).
    void updateItem(MHWRender::MSubSceneContainer& container, const MString& name, const float color[4],
                    unsigned depthPriority, const std::shared_ptr<const DisplayGeometry>& geometry, float lineWidth = 1.0f,
                    MHWRender::MTexture* mapTexture = nullptr);
    // Strand lines holding the strands and point counts the item's buffers hold: only the changed
    // strands' vertex ranges are written (MVertexBuffer::update), the indices and uvs kept. False
    // when the buffers do not hold such lines (the caller uploads the whole item).
    bool updateItemPartly(MHWRender::MSubSceneContainer& container, const MString& name, const float color[4],
                          const DisplayItem& item);
    // An item kept hidden (DisplayItem::keep): disabled, its buffers kept for when it comes back.
    void hideItem(MHWRender::MSubSceneContainer& container, const MString& name);
    // Styles an item and keeps its geometry.
    void restyleItem(MHWRender::MSubSceneContainer& container, const MString& name, const float color[4],
                     float lineWidth = 1.0f);
    // The cards' wireframe (cardWireGeometry), drawn as Maya draws a mesh's: in wireframe and
    // wireframe on shaded only (the cards themselves in shaded and textured), in the groom's
    // wireframe color (Maya's: dormant, or lead and active when selected). `rebuilt`: the cards
    // were built again (`cards` what they are now); else only shown, hidden or recolored with them.
    void updateCardWires(MHWRender::MSubSceneContainer& container, bool rebuilt, const std::shared_ptr<const DisplayGeometry>& cards);
    std::array<float, 4> wireColor() const;  // Maya's wireframe color for the groom now
    std::shared_ptr<const DisplayGeometry> cardWires_;
    std::array<float, 4> cardWireColor_{};
    bool cardWiresShown_ = false;
    // The colored strand lines take new colors (their range or source changed), their positions
    // and indices kept.
    void recolorStrands(MHWRender::MSubSceneContainer& container, const StrandColors& colors, const float color[4]);
    // Whether the colored strand lines are on the GPU as built for `vertexCount` points (they may
    // then take new colors alone).
    bool recolorable(MHWRender::MSubSceneContainer& container, std::size_t vertexCount);
    void style(MHWRender::MRenderItem& item, ItemBuffers& buffers, const float color[4], float lineWidth);
    // The hair shading's own settings (ambient, scatter, the root-to-tip map...) of a shader of
    // kind 2, 6, 9 or 10, set once when made (and on the level of detail's copies of it).
    void hairShaderDefaults(MHWRender::MShaderInstance& shader, int kind);
    // setGeometryForRenderItem, the streams kept by item name (boundStreams_).
    void bindGeometry(MHWRender::MRenderItem& item, const MHWRender::MVertexBufferArray& streams,
                      const MHWRender::MIndexBuffer& indices, const MBoundingBox* bounds);
    std::map<std::string, std::vector<std::pair<MString, MHWRender::MVertexBuffer*>>> boundStreams_;
    // Each item's last binding (bindGeometry), numbered: the level of detail's items bind the
    // strands' streams again after every binding of theirs (a buffer made anew can take a freed
    // one's address, and Maya's copy of a staged buffer can move when it is written again).
    std::uint64_t bindSerial_ = 0;
    std::map<std::string, std::uint64_t> boundSerials_;
    // The streams an item's shader reads, as its buffers hold them (`positions`, `frames` and
    // `uvs`: other buffers in their place, a GPU preview's), into `streams`.
    static void addStreams(MHWRender::MVertexBufferArray& streams, const ItemBuffers& buffers,
                           MHWRender::MVertexBuffer* positions = nullptr, MHWRender::MVertexBuffer* normals = nullptr,
                           MHWRender::MVertexBuffer* tangents = nullptr, MHWRender::MVertexBuffer* uvs = nullptr);
    // Lit strand lines' points into the item's buffers and their frames written by the GPU from
    // them (gpu_lines.h): whole, or only the strands of `runs` (their points, and their uvs with
    // `uvs` when the widths changed; the frames all written again, it costs the GPU little). The
    // uvs (whole writes) go up when their point counts or IDs changed, else the widths alone when
    // they did. False where the GPU cannot (no engine; under OpenGL, Maya's buffers not made for
    // these points yet; a partial write under DirectX 11 without the streams): nothing written,
    // the caller writes them on the CPU. *megabytes: what went up; ms[0..2]: the CPU's milliseconds
    // sending the points, the layout, making the root normals, sending them, the frames' kernel, the uvs.
    bool writeGpuLines(ItemBuffers& buffers, const StrandLines& lines, const std::vector<std::pair<std::size_t, std::size_t>>* runs,
                       bool uvs, double* megabytes, double* ms);
    // ... its work on the engine (throws where the device would not).
    bool writeGpuLines(gpu::LineFrames& engine, ItemBuffers& buffers, const StrandLines& lines,
                       const std::vector<std::pair<std::size_t, std::size_t>>* runs, bool uvs, double* megabytes, double* ms);
    // The item's GPU streams (DirectX 11) go: the CPU writes Maya's own buffers again.
    static void dropGpuStreams(ItemBuffers& buffers);
    // Ribbons carried as lines (DisplayGeometry::ribbons): the lines' points (and their uvs' widths,
    // the layout's triangles and uvs, the colors, when they changed) sent, the ribbons' vertices
    // made from them by the GPU (line_frames.h runRibbons) into the item's buffers, which `streams`
    // then names (the caller binds them). False where the GPU cannot (no engine, the GPU frames
    // off, a failure: warned, the GPU's lines off from then): nothing bound, the caller builds the
    // ribbons on the CPU (materialized()). *megabytes: what went up; ms as writeGpuLines'.
    bool writeGpuRibbons(ItemBuffers& buffers, const DisplayGeometry& ribbons, MHWRender::MVertexBufferArray& streams,
                         double* megabytes, double* ms);
    bool writeGpuRibbons(gpu::LineFrames& engine, ItemBuffers& buffers, const DisplayGeometry& ribbons, double* megabytes, double* ms);
    // The ribbons' vertices from the lines' points and uvs (`points`, `lineUvs`: the item's own, or
    // a GPU preview's), into the item's buffers as they hold them.
    void expandGpuRibbons(gpu::LineFrames& engine, ItemBuffers& buffers, void* points, void* lineUvs);
    // The engine's targets for the ribbons' buffers (GpuRibbonStreams' order; null where none).
    static void gpuRibbonTargets(gpu::LineFrames& engine, const ItemBuffers& buffers, void* targets[8]);
    // The ribbons the GPU made go: their streams, and the buffers naming them.
    static void dropGpuRibbons(ItemBuffers& buffers);
    std::unique_ptr<GpuLines> gpuLines_;
    // ---- Following the scalp on the GPU (line_frames.h runFollow) ----
    // A frame handing its strands on at rest (ViewResult::follow: the shape asks for it while
    // setViewportCarries says the viewport can) draws them as the builder's strands at rest, built
    // again only when those change; every update the GPU carries them to the frame's scalp into the
    // strands' points (lit lines, then their frames) or the ribbons' lines (then the ribbons), with
    // the scalp's points and normals alone sent. Anything else (unlit or colored lines, a stroke,
    // root dots, cards, feathers' vanes, no GPU) draws the strands the CPU carries (placed()).
    // Whether strands drawn with these settings are ones the GPU may carry.
    bool followDrawable(const DisplaySettings& settings, const StrandSet* strands, bool stroke) const;
    // `points` (float3 a point: the lines' points, or the ribbons' lines) carried to follow_'s scalp
    // from the strands at rest, the root normals for the frames or ribbons after it (the layout
    // `layout` set first). *megabytes: what went up. Throws where the device would not.
    void carryOnGpu(gpu::LineFrames& engine, ItemBuffers& buffers, void* points, std::uint64_t layout, double* megabytes);
    // The strands' or ribbons' item carried again for follow_ where the builder did not write it
    // this update (another scalp, the same strands at rest), and bound with the bounds now.
    void carryFollowed(MHWRender::MSubSceneContainer& container);
    FollowCarryPtr follow_;          // this update's (null: the strands drawn are the CPU's)
    StrandsPtr followRest_;          // the strands at rest the builder was handed (held: their pointer names them)
    std::uint64_t followRestVersion_ = 0;  // ... their version for the builder and key for the engine
    MeshPtr followRootsFrom_;        // the rest scalp followRoots_ were found on
    StrandsPtr followRootsOf_;       // ... and the strands whose roots they are
    std::vector<FollowRoot> followRoots_;
    std::uint64_t followRootsKey_ = 0;
    bool followBroken_ = false;      // the GPU failed carrying: the CPU's strands from now on
    friend std::string gpuFramesCheckJson(const GroomShape* shape);
    friend void gpuFramesSwitched();
    friend std::string hairGridJson(const GroomShape* shape);
    friend void hairGridsPlaybackStopped();
    friend bool gpuPreviewDrawn(const GroomShape* shape, std::uint64_t version);
    // displayWidthCoverage as this update read it (the strands are built again when it changes).
    bool widthCoverage_ = true;
    // Ribbons at the strands' true width (displayRibbons, where the GPU makes them): facing the camera
    // (kRibbonsItem), at least a pixel wide, their segments thinner than a pixel drawn as lines with
    // width as coverage (kThinRibbonsItem); a camera move makes them again for the new view
    // (refaceRibbons). The view they face (readFacingView: the eye, or the direction toward an
    // orthographic viewer; a pixel's world size at distance 1) and the view-projection and height it
    // was read from. (Where the GPU cannot, the CPU builds ribbons along the scalp as before.)
    bool facing_ = false;
    float facingEye_[3] = {0.0f, 0.0f, 0.0f};
    float facingPixelWorld_ = 0.0f;
    bool facingOrthographic_ = false;
    mutable MMatrix facingCamera_;
    mutable int facingHeight_ = 0;
    void readFacingView(const MHWRender::MFrameContext& context);
    void refaceRibbons();
    // The facing ribbons' thin segments as lines (kThinRibbonsItem): drawn from the ribbons' item's
    // buffers (`base`) with its line indices and its shader's lines kind with width as coverage (lit:
    // the hair graph's; unlit: the flat lines' effect), styled as it is; dropped when the ribbons no longer face the camera (before the
    // ribbons' buffers go: it draws them).
    void updateThinRibbons(MHWRender::MSubSceneContainer& container, ItemBuffers& base, const MHWRender::MRenderItem& baseItem,
                           const MBoundingBox& bounds, const float color[4], float lineWidth);
    void dropThinRibbons(MHWRender::MSubSceneContainer& container);
    // GroomLab's hair shading (hair_shading.h): displayHairShading as this update read it (and
    // whether the effect compiles), what the groom's hair shaders share (the lights it ignores,
    // refreshed now and then: lightsChecked_).
    bool hairShading_ = true;
    // The cards drawn with their baked maps this update (style(): out of the post effects).
    bool cardsTextured_ = false;
    std::shared_ptr<HairGroomState> hairState_;
    std::chrono::steady_clock::time_point lightsChecked_{};
    // The shader the lit strands (kinds 2 and 9) draw with now: GroomLab's hair effect or Maya's
    // hair tube graph (with the coverage callbacks when `coverage`); null where none could be made.
    MHWRender::MShaderInstance* acquireLitShader(const MHWRender::MShaderManager& shaders, int kind, bool coverage);
    // The hair's density grid (self-shadowing, displaySelfShadowing) written again by the GPU from
    // the lit strands' points as drawn (theirs, a GPU preview's, the carried ones) when they moved,
    // and set on every hair shader of the groom.
    void updateHairGrid(const MHWRender::MFrameContext& context);
    std::uint64_t hairGridKey_ = 0;
    // During playback the grid is made again every few frames (hairGridSkipped_ counts the ones it
    // was left as it was), and once more when playback stops (hairGridStale_).
    int hairGridSkipped_ = 0;
    bool hairGridStale_ = false;
    // The grid's texture read back against the grid the kernels made (HairGroomState textureSum),
    // written from the CPU where they differ; the texture last checked (its volumeSerial).
    void checkHairGrid(gpu::LineFrames& engine, HairGroomState& grid, void* target);
    std::uint64_t hairGridChecked_ = 0;
    // The lit strand color (a hair color, hair_color.h): with useMaterialColor the assigned
    // material's, else a HairColor node's; otherwise displayColor. Walks the shading
    // group, so it runs only when colorDirty_ says something it read changed:
    // the shape's assignment (displayVersion), or the shading group or material
    // node it found last time (node dirty callbacks, kept in callbacks_).
    // `groomColors`: the color is the groom's own (no material), so the lit strands
    // take each strand's own (DisplaySettings::strandLookColors).
    std::array<float, 3> litColor(MObject* engine = nullptr, MObject* material = nullptr, bool* groomColors = nullptr);
    void watchMaterial(const MObject& engine, const MObject& material);
    static void materialDirty(MObject& node, void* clientData);
    mutable bool colorDirty_ = true;
    MCallbackIdArray callbacks_;
    MObject watchedEngine_, watchedMaterial_;
    // Strand channel means (the lit color's HairColor channels, a material's user data), kept per evaluation.
    StrandChannelMeans means_;
    MHWRender::MTexture* rootToTipTexture();
    MHWRender::MTexture* rootToTip_ = nullptr;
    // The baked card maps the cards view shows (cardMaps: "Color=<path>;Coverage=<path>;AO=<path>;..."),
    // read from disk into textures once per change: a new cardMaps string, or a file rewritten in
    // place (a rebake; their modification times and sizes are kept and looked at now and then).
    struct CardMap {
        std::string path;
        std::uint64_t stamp = 0;  // the file's modification time and size when loaded (0: none)
        MHWRender::MTexture* texture = nullptr;
    };
    enum CardMapKind { kCardColorMap, kCardCoverageMap, kCardAoMap, kCardMapCount };
    // True when the map texture is loaded: the color and coverage files (the AO file is optional).
    bool loadCardTextures(const std::string& maps);
    void releaseCardTextures();
    void bindCardTextures(ItemBuffers& buffers);
    template <class Bind>
    void bindCardIridescence(ItemBuffers& buffers, const Bind& bind);
    // An item's own image on Maya's textured shader (kind 8): the map shown on the scalp.
    void bindItemTexture(ItemBuffers& buffers, MHWRender::MTexture* texture);
    bool cardFilesChanged() const;  // a loaded map's file changed on disk (checked at most twice a second)
    bool cardTexturesLoaded() const;
    std::array<CardMap, kCardMapCount> cardMaps_;
    MHWRender::MTexture* cardIridescence_ = nullptr;  // the Color map's alpha (one texel of none without)
    std::string cardMapsLoaded_;
    // The map shown on the scalp (scalpMapDisplay; Groom Tools > Paint): a texture map's image
    // as a texture (its file, or the brush's image while it paints), kept per key.
    MHWRender::MTexture* scalpMapTexture(const GroomShape::ScalpMapView& view);
    void releaseScalpMapTexture();
    MHWRender::MTexture* scalpMapTexture_ = nullptr;
    std::string scalpMapTextureKey_;
    std::uint64_t scalpMapTextureGeneration_ = 0;  // bumped by every texture made
    // The brush's image the texture holds, and its revision there (uploads go by the change since).
    const TexturePreview* scalpMapPreview_ = nullptr;
    std::uint64_t scalpMapPreviewRevision_ = 0, scalpMapPreviewLayout_ = 0;
    std::uint64_t scalpMapVersion_ = ~std::uint64_t{0};  // the view drawn (needsUpdate compares)
    // How far the overlay effect pulls guides, parts and flow toward the camera (scene units):
    // the longest strand, so anything within the hair shows through it, while the head still
    // hides what is behind it. 0 with displayOverlaysOnTop off.
    float overlayPull_ = 0.0f;
    // What still hides the overlays: every visible mesh in the scene but the groom's own card
    // mesh, as world-space meshes (an AABB tree each), refreshed now and then by a fingerprint.
    // Each overlay vertex's pull stops short of the first of them on its way to the camera (a
    // ray per vertex, redone when the camera moves: overlayPullLimits), so the hair never hides
    // guides, parts or flow, and the head, or anything else in the scene, still does.
    struct Occluder {
        std::string path;
        std::uint64_t signature = 0;
        std::shared_ptr<const ScalpMesh> mesh;
        bool scalp = false;  // the groom's own scalp: the drawn frame's, whenever it is drawn
    };
    std::vector<Occluder> occluders_;
    std::vector<std::shared_ptr<const ScalpMesh>> occluderMeshes_;  // occluders_' meshes
    mutable std::chrono::steady_clock::time_point occludersChecked_{};
    void refreshOccluders();
    void uploadOverlayColors(ItemBuffers& buffers, const DisplayGeometry& geometry, const std::vector<float>& limits);
    // The overlay items' geometry, for new limits when the camera moves.
    std::map<std::string, std::shared_ptr<const DisplayGeometry>> overlayBatches_;
    std::set<std::string> overlayFresh_;               // items whose limits this update computed already
    Vec3f overlayCamera_{0.0f, 0.0f, 0.0f};            // the camera the limits were computed for
    bool overlayCameraKnown_ = false;
    mutable std::chrono::steady_clock::time_point cardFilesChecked_{};
    // The prototype meshes of the instances drawn, by name. Each update compares
    // them with a cheap fingerprint of the scene mesh (counts, bounds, points) and
    // fetches again what changed, so an edited, replaced or deleted prototype
    // redraws; needsUpdate asks the same question (prototypesChanged) so the
    // edit alone refreshes the viewport.
    void refreshPrototypes(const std::vector<std::string>& names);
    // Asked on every redraw, so it looks at the scene at most twice a second; an edit to a
    // prototype mesh (a dirty callback on it) makes it look at once.
    bool prototypesChanged() const;
    std::map<std::string, PrototypeMesh> prototypes_;
    mutable std::chrono::steady_clock::time_point prototypesChecked_{};
    void watchPrototypes();
    static void prototypeDirty(MObject& node, void* clientData);
    MCallbackIdArray prototypeCallbacks_;
    std::vector<MObjectHandle> watchedPrototypes_;

    MObject object_;
    GroomShape* shape_ = nullptr;

    bool visible() const;  // any instance visible (hidden objects, parents and layers draw nothing)
    void forgetDrawn();    // the next update rebuilds everything
    std::uint64_t drawnVersion_ = ~std::uint64_t{0};
    std::uint64_t drawnDisplay_ = ~std::uint64_t{0};
    // The guide selection drawn (GroomShape::selectionHash): compared by value, so a new selection
    // draws even when no dirty message for guideSelection reached setDependentsDirty.
    std::uint64_t drawnSelection_ = ~std::uint64_t{0};
    bool drawnVisible_ = true;
    std::uint64_t drawnTheme_ = 0;  // DisplayTheme version
    // The groom's scale with its scalp the strands were last drawn at (GroomShape::groomScale): another
    // one (a scalp scaled and frozen) draws the strands afresh, as width as coverage turned off and on
    // does: their items' shaders acquired again, every stream written, the level of detail and the
    // self-shadowing grid made again (0: none drawn yet).
    double drawnScale_ = 0.0;
    void drawAfreshForScale(MHWRender::MSubSceneContainer& container);
    // Guide point dots, drawn as UI drawables (see addUIDrawables). They face the
    // camera at a constant screen size, so they are rebuilt when the guides, the
    // camera or the viewport size changed (uiDirty_, set by requiresUpdate).
    std::vector<Vec3f> points_;
    std::vector<std::array<float, 3>> pointColors_;
    float pointSize_ = 0.0f;
    mutable bool uiDirty_ = true;
    std::uint64_t seamDrawn_ = ~std::uint64_t{0};  // the brush's symmetry line drawn (GroomShape::symmetrySeamVersion)
    // The Shape brush's tube wires (GroomShape::tubeWires): every guide it can shape (shown, not
    // frozen), the selected ones' segments apart (drawn brighter), and what they were made from
    // (the guides, the selection, the hidden groups), made again only when that changes.
    void addTubeWires(MHWRender::MUIDrawManager& draw);
    std::vector<Vec3f> tubeWires_, tubeWiresSelected_;
    std::uint64_t tubeWiresKey_ = ~std::uint64_t{0};
    bool tubeWiresDrawn_ = false;
    std::uint64_t uiSelection_ = 0;  // the guide selection the tube wires were drawn with
    mutable MMatrix uiCamera_;  // the view-projection the dots were last built for
    mutable int uiHeight_ = 0;
    std::array<float, 3> drawnColor_{};
    bool drawnGroomColors_ = false;  // drawnColor_ is the groom's own (litColor): the strands take theirs
    // The scalp's bounds (for which meshes can hide the overlays), kept per scalp.
    MBoundingBox scalpBounds_;
    std::uint64_t scalpBoundsOf_ = 0;

    // The buffers each item draws (core display.h): built again only for what changed, kept
    // nowhere but on the GPU (retain off).
    DisplayBuilder builder_;
    std::map<std::string, ItemBuffers> items_;

    // GPU look preview (gpu_preview.h; its engine made at the first drag or stroke). While a
    // drag's values are set, or a stroke's dabs are the GPU's, the strands item (a stroke's dimmed
    // one) draws the GPU's results (gpuWrote_): from the preview's own buffers (gpuBound_: DirectX
    // 11, the item gpuItem_) or written into its own (gpuWroteItem_: OpenGL); once they end the
    // item draws its own buffers again (bound back, or written whole from the CPU's) and the
    // preview's buffers go.
    void applyGpuPreview(MHWRender::MSubSceneContainer& container, const StrandsPtr& strands);
    void ownBuffersBound(const MString& name);  // an item bound (or wrote) its own buffers
    // An item bound to its own buffers again (the preview's drawn no more).
    void rebindOwnBuffers(MHWRender::MSubSceneContainer& container, const MString& name);
    bool gpuRunning() const;  // a drag's values are set, or a stroke's dabs are the GPU's
    std::unique_ptr<GpuLookPreview> gpuPreview_;
    std::uint64_t gpuDrawn_ = 0, gpuStrandsDrawn_ = 0;
    std::uint64_t gpuShown_ = 0;  // the drag's request version the last run drew (0: none, or a stroke's)
    bool gpuWrote_ = false, gpuBound_ = false, gpuStale_ = false;
    bool gpuWroteItem_ = false;  // ... into the item's own buffers (OpenGL in place, a copy)
    MBoundingBox gpuBounds_;      // the bounds the item was bound with for the drag
    bool gpuBoundsSet_ = false;   // the item is bound with gpuBounds_
    MString gpuItem_;

    // ---- Screen-space level of detail (strand_lod.h; displayClumpLod) ----
    // The strand lines (the strands' item, or the colored strands') drawn by class: an item per
    // class beside the strands' own, binding its streams (whichever it draws: its own, a GPU
    // preview's, the GPU's carried points) with the class's indices, its shader a copy of the
    // strands' styled the same with the class's exponent (lineLod); class 0's segments that may
    // blend in an item drawn after everything without writing depth (the blend item, Maya's
    // transparent pass); the clumps thinner than a pixel as one line each (the impostors' item, its
    // own small buffers). The strands' own item is off meanwhile. The view is each camera's
    // (lodViewOf, asked by requiresUpdate: another level draws again), orbiting the point Maya's
    // tumble turns it about, the finest of those seen in the last two seconds (several panels); a
    // class's indices are written only when its strands changed (StrandLod::Selection::slotKeys),
    // and its item bound again only then (or for other streams).
    void updateLod(MHWRender::MSubSceneContainer& container, const MHWRender::MFrameContext& context);
    // The classes' items gone, the strands' own drawn again.
    void hideLod(MHWRender::MSubSceneContainer& container);
    void dropLodImpostors(MHWRender::MSubSceneContainer& container);  // the impostors' item, its buffers and shader gone
    // The finest view among the cameras seen lately, this frame's camera's taken in (and kept with
    // its level's hysteresis).
    StrandLodView lodViewOf(const MHWRender::MFrameContext& context) const;
    // Whether the level of detail draws (or should) the strand lines.
    bool lodWanted() const;
    StrandLod lod_;
    struct LodCamera {
        StrandLodView view;
        std::chrono::steady_clock::time_point seen;
    };
    mutable std::map<std::string, LodCamera> lodCameras_;
    StrandLodView lodDrawn_;     // the view last drawn for
    bool lodAntiAliased_ = true; // and whether the target was multisampled then (no blended split)
    bool lodOn_ = false;         // the classes draw (the strands' own item off)
    std::uint64_t lodKey_ = 0;   // the selection the indices hold
    std::array<std::uint64_t, kLodSlotCount> lodSlotKeys_{};  // each slot's indices as written (Selection::slotKeys)
    std::uint64_t lodImpostorKey_ = 0;
    std::uint64_t lodImpostorColors_ = 0;  // the impostors' colors' key as uploaded
    MString lodBase_;            // the item they stand in for
    // Per class the indices of its segments that do not blend, then class 0's blendable segments'
    // (drawn by two items: their opaque fragments, then the blend item's blended ones); and the
    // items' shaders (the classes', the blend item's, the blendable segments' opaque item's).
    std::array<std::unique_ptr<MHWRender::MIndexBuffer>, kLodClassCount + 1> lodIndices_;
    std::array<MHWRender::MShaderInstance*, kLodClassCount + 2> lodShaders_{};
    std::array<std::uint64_t, kLodClassCount + 2> lodShaderOf_{};  // the strands' shader each is made like (its shaderSerial)
    std::uint64_t shaderSerials_ = 0;  // the last shaderSerial given
    // What each item was last bound with (bound again only when it changes).
    struct LodBinding {
        bool valid = false;
        std::vector<const MHWRender::MVertexBuffer*> streams;
        std::uint64_t indicesKey = 0;
        MBoundingBox bounds;
        const MHWRender::MRenderItem* item = nullptr;
        std::uint64_t baseSerial = 0;  // the strands' item's binding it took (boundSerials_)
    };
    std::array<LodBinding, kLodClassCount + 2> lodBound_{};
    // The strands' and the colored strands' lines as last built (the builder keeps none).
    std::array<std::shared_ptr<const StrandLines>, 2> lodLines_;
};

}  // namespace groomlab::maya
