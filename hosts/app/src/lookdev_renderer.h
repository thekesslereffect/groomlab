// SPDX-License-Identifier: Apache-2.0
#pragma once

// The lookdev path of the viewport's renderer. Its hair is drawn one of two ways (HairPath): a
// visibility buffer rasterized, shaded and voxelized in compute shaders (the default; see
// setHairPath), or, as described below, the legacy way. The legacy path: the strands as
// camera-facing ribbons pulled from
// storage buffers (hair.vert), shaded as the render would (hair_lookdev.frag: Marschner's
// lobes, melanin absorption, dual scattering), under key, fill and rim lights and a generated
// environment, with hair-on-hair shadows and ambient occlusion from deep opacity maps, drawn
// into a linear HDR multisampled target, resolved, accumulated over jittered frames while
// nothing moves (refine mode), and tone mapped into the host's target. The surfaces (the
// scalp, props such as a scanned head, eyes: surface.h) draw in the same pass through
// surface_lookdev.frag (metal/roughness PBR, skin, eyes) with their textures; the hair's deep
// opacity maps shadow them, and they shadow the hair and each other through a filtered
// shadow map per light fit to the surfaces' own bounds. Skin scatters light in screen space
// (Jimenez 2015): the skin surfaces draw once more before the HDR pass into an irradiance and
// a depth target, a separable blur with the diffusion profile's kernel runs over them, and
// the HDR pass reads the result back under the specular. The other display items (guides,
// overlays) are the GroomRenderer's: it draws the depth-tested ones into the HDR pass through
// a callback and the overlays after the tone map.
//
// It needs texel fetch in the vertex stage and float render targets; GroomRenderer keeps the
// line path for devices without them.

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <QByteArray>
#include <QElapsedTimer>
#include <QMatrix4x4>
#include <QSize>
#include <rhi/qrhi.h>

#include "camera.h"
#include "groomlab/groom_session.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_lod.h"
#include "groomlab/strand_set.h"
#include "look.h"
#include "surface.h"

namespace groomlab::app {

// The hair as the GPU draws it: two texels a point (position and its strand; tangent and
// width), three a strand (melanin, redness, random, its first point; the RGB channels and its
// point count; the scalp's normal at its root, what the head hides from the lights), and a
// triangle strip's indices (a restart between strands) or a triangle list's.
struct HairBuffers {
    std::vector<float> points;      // 4 per point: x y z width (hair_points.glsl)
    // Each point's strand (a float a point, whole rows): built with the indices, for a new layout.
    std::vector<float> pointStrands;
    std::vector<float> strands;     // 12 per strand: melanin redness random first, r g b count, nx ny nz 0
    // 4 per point, r g b iridescence, when the strands carry per-point colors or an iridescence
    // (hairPointColored: a tip color, Bands, FeatherColor's patterns); else empty.
    std::vector<float> pointColors;
    bool pointColored = false;
    std::vector<std::uint32_t> strip, list;
    std::size_t pointCount = 0, strandCount = 0;
    Bounds bounds;
};
// Whether a strand set has colors of its own per point (HairColor's colorR/G/B point channels)
// or an iridescence channel: what HairBuffers::pointColors holds.
bool hairPointColored(const StrandSet& strands);
// Builds them from a strand set (parallel over strands): the tangents central differences,
// the channels melanin / melaninRedness / colorR,G,B (their defaults where the set lacks
// them: HairColor's brown), the root normals from the scalp (null, or a root off it: the
// strand's first segment).
// `indices`: which index arrays to build (a renderer draws one kind, and none while the GPU holds
// the strands' layout: they depend on the point counts alone).
enum class HairIndices { Both, Strip, List, None };
HairBuffers buildHairBuffers(const StrandSet& strands, const ScalpMesh* scalp, HairIndices indices = HairIndices::Both);
// The index arrays of `b` (for `strands`, as buildHairBuffers lays them out) built where missing.
void buildHairIndices(const StrandSet& strands, HairIndices indices, HairBuffers& b);
// The same for strands [first, end) alone: their point entries (4 floats a point, from strand
// `first`'s first point on) and strand entries (12 a strand), as a partial upload writes them
// (DisplayItem::updated's runs), and with `pointColors` their per-point colors (4 floats a point).
void buildHairRange(const StrandSet& strands, const ScalpMesh* scalp, std::size_t first, std::size_t end, std::vector<float>& points,
                    std::vector<float>& strandData, Bounds* bounds, std::vector<float>* pointColors = nullptr);

// How a strand set differs from the one shown before it (ViewResult::strandsSerial,
// changesBase, strandsChanges): the lookdev textures holding the strands of serial
// `changesBase` are written again only where `changes` says (0 or null: not known, written
// whole when the strands changed).
struct HairChanges {
    std::uint64_t serial = 0, changesBase = 0;
    ChangesPtr changes;
    // The strands are what a GPU preview's drag drew last (its release committed the value it
    // showed): where the point texture still holds that run, only the strands' entries and point
    // colors are written (the GPU's points stay: no whole write of the points, no stall).
    bool pointsHeld = false;
};

class LookdevRenderer {
public:
    LookdevRenderer();
    ~LookdevRenderer();
    LookdevRenderer(const LookdevRenderer&) = delete;
    LookdevRenderer& operator=(const LookdevRenderer&) = delete;

    // Whether this device can run the path.
    static bool supported(QRhi* rhi);

    // The strands to draw (null: none) and the scalp their roots sit on (null: none; its
    // normals say what the head hides from the lights); `version` moving means new buffers:
    // written whole, or, when `changes` names the strands held and lists what moved (a brush
    // dab's incremental evaluation) and the layout is the same, only those strands' texels.
    void setHair(StrandsPtr strands, MeshPtr scalp, std::uint64_t version, const HairChanges& changes = {});
    // Partial texture writes (default on; GROOMLAB_APP_PARTIAL_UPLOADS=0 turns them off): off,
    // every change writes the textures whole (the tests compare both).
    void setPartialUploads(bool on) { partialUploads_ = on; }
    bool partialUploads() const { return partialUploads_; }
    // Whole writes built ahead (default off: tests, scripted runs): setHair builds the buffers a
    // whole write needs on a thread of their own, and until they are built the frames keep
    // drawing the hair the textures hold (wantsAnotherFrame), so the frame a commit lands in
    // costs the writes alone, not the build. Partial writes are built in the frame, as ever.
    void setBuildAhead(bool on) { buildAhead_ = on; }
    bool buildAhead() const { return buildAhead_; }
    // Whether a whole write's buffers are building (the hair drawn is the one before).
    bool hairBuilding() const;
    // How the hair is drawn.
    //  - Visibility (the default where the device computes): a visibility buffer rasterized in
    //    compute shaders (hair_raster.comp: each segment walked a pixel column at a time with its
    //    analytic box-filtered coverage, no MSAA; per pixel the nearest fragment its hashed
    //    coverage keeps and the order-independent optical depth of all of them), the visible
    //    fragment shaded once (hair_shade.comp), its alpha 1 - exp(-tau), laid over the surfaces;
    //    the hair's shadows from voxel volumes (hair_voxelize.comp, hair_volume.comp: one count of
    //    hairs toward each light per voxel, made again only when the hair or the lights change);
    //    the clumps' level of detail (strand_lod.h) thins what the view cannot resolve.
    //  - Legacy: the strands as ribbons through the raster pipeline into the 4x MSAA target,
    //    shaded per fragment or per point, the deep opacity maps for shadows
    //    (GROOMLAB_APP_HAIR_PATH=legacy, or a device without compute).
    enum class HairPath { Legacy, Visibility };
    void setHairPath(HairPath path) {
        if (path != hairPath_) sceneChanged();
        hairPath_ = path;
    }
    HairPath hairPath() const { return hairPath_; }
    // What the last frame drew with (Legacy on a device that cannot compute).
    HairPath lastHairPath() const { return lastPath_; }
    // The visibility path's level of detail (DisplaySettings::clumpLod, its error), on by default.
    void setClumpLod(bool on, float error) {
        lodOn_ = on;
        lodError_ = error;
    }
    // The strands the visibility path's last frame drew (fewer than the set's with the level of
    // detail thinning clumps), and whether it thinned any.
    std::size_t lastDrawnStrands() const { return drawnStrands_; }
    bool lastLodActive() const { return lodActive_; }
    // The interactive frame shaded once per point (hair_lookdev.frag under HAIR_POINT_SHADE,
    // the ribbons lit from the shaded points: the default) or per fragment, every overlapping
    // layer (GROOMLAB_APP_HAIR_SHADING=fragment: the reference the tests compare against).
    // The legacy path's alone.
    void setPointShading(bool on) { pointShading_ = on; }
    bool pointShading() const { return pointShading_; }
    // The last upload: whether it wrote only the changed strands, and how many.
    bool lastUploadPartial() const { return uploadPartial_; }
    std::size_t lastUploadStrands() const { return uploadStrands_; }
    // It kept the GPU preview's points (HairChanges::pointsHeld): the strands' entries alone written.
    bool lastUploadHeld() const { return uploadHeld_; }

    // The GPU look preview's targets (gpu_preview.h): the point texture the kernels write
    // (null until the hair is uploaded), its width, the points it holds, and whether this
    // frame's prepare uploaded it (a run then would land before the upload: run next frame).
    struct GpuTargets {
        QRhiTexture* points = nullptr;
        int width = 0;
        std::size_t pointCount = 0;
        bool fresh = false;
    };
    GpuTargets gpuTargets() const;
    // The GPU preview wrote the point texture: the maps are drawn again over `bounds` (what the
    // run can have reached), and the next change of the strands writes the textures whole.
    void hairWritten(const Bounds& bounds);
    void setMaterial(const HairMaterial& material);
    void setLighting(const LightingSetup& lighting);
    // The surfaces to draw (the scalp among them when shown), in order. A surface's geometry
    // is uploaded again when its `version` moves; its material is read every frame.
    void setSurfaces(std::vector<SurfacePtr> surfaces);
    const std::vector<SurfacePtr>& surfaces() const { return surfaces_; }
    const HairMaterial& material() const { return material_; }
    const LightingSetup& lighting() const { return lighting_; }
    // The camera moved: the accumulation starts over (the shadow maps hold).
    void invalidate();
    // The scene changed (the scalp, what casts shadows): the maps are drawn again too.
    void sceneChanged();
    int accumulatedFrames() const { return accumulated_; }
    // Refine mode and the image still converging: another frame would improve it (not after
    // a reduced frame: the host draws the full one once nothing moves, settle).
    bool wantsAnotherFrame() const;
    static constexpr int kMaxAccumulatedFrames = 256;

    // Adaptive interactive quality (LightingSetup::interactiveQuality). A stream of changes
    // (frames back to back, each after a change: a camera drag, a slider's drag, a stroke;
    // or the host holding a drag, setInteracting) draws its frames at a resolution scaled
    // down to fit a display's refresh: the full frame's cost is measured from the frames
    // drawn at full scale that came back to back (the refine frames, a stream's first), a
    // reduced frame over budget lowers the stream's scale further, and the point count stands
    // in until a measurement comes. The picture draws into the same targets' corner (no
    // reallocation) and the display transform scales it up. settle: the host saw nothing move
    // for a while, the next frame is the full picture.
    void setInteracting(bool on) { interacting_ = on; }
    void settle();
    bool lastFrameReduced() const { return reduced_; }
    float lastFrameScale() const { return frameScale_; }
    double fullFrameMs() const { return fullFrameMs_; }
    static constexpr double kTargetFrameMs = 12.0;  // under a 60 Hz refresh with room for the rest of the frame
    static constexpr float kMinScale = 0.25f;

    // A frame, inside the host's frame, in three steps. prepare: the targets at the host's
    // size, the hair's buffers and the environment uploaded (committed on `cb`), the
    // pipelines; false when the path cannot run (then the host draws its line path). The host
    // then makes its own pipelines for the HDR pass (hdrPassDescriptor, hdrSampleCount) and
    // the jittered camera (jitter x its view-projection). render: the passes before the
    // host's own, the deep opacity maps, the HDR scene (`depthItems` draws the host's
    // depth-tested items into it; the surfaces are drawn here), the accumulation.
    // composite: inside the host's pass into `target`, the tone-mapped image (the host's
    // overlays go over it).
    bool prepare(QRhi* rhi, QRhiCommandBuffer* cb, QRhiRenderTarget* target);
    QRhiRenderPassDescriptor* hdrPassDescriptor() const;
    int hdrSampleCount() const;
    // The sub-pixel offset of the next frame (identity while interacting), in clip space.
    QMatrix4x4 jitter(const QSize& size) const;
    bool render(QRhi* rhi, QRhiCommandBuffer* cb, QRhiRenderTarget* target, const OrbitCamera& camera, const QMatrix4x4& viewProj,
                const std::function<void(QRhiCommandBuffer*)>& depthItems);
    void composite(QRhiCommandBuffer* cb, const QRhiViewport& viewport);
    void releaseResources();
    // The viewport of the pass render() is drawing (the HDR pass's when it calls `depthItems`):
    // a host binding its own pipelines there sets it again after each (on Direct3D 12 QRhi sets
    // the scissor from the viewport only while a pipeline is bound).
    const QRhiViewport& passViewport() const { return passViewport_; }

    // Stats of the last frame.
    int lastHairDraws() const { return hairDraws_; }
    int lastSurfaceDraws() const { return surfaceDraws_; }
    std::size_t pointCount() const { return pointCount_; }
    std::size_t strandCount() const { return strandCount_; }
    double lastUploadMs() const { return uploadMs_; }
    // How many hair uploads there have been (a frame that made none leaves it: its upload cost 0).
    std::size_t uploads() const { return uploads_; }
    // Textures loaded from files (by path) and made here (the tables, the detail normal).
    int textureCount() const;

private:
    struct Gpu;
    struct GpuSurface;
    struct FrameUniforms;
    struct PassUniforms;
    struct MaterialUniforms;
    struct SssUniforms;
    bool ensureResources(QRhi* rhi, QRhiRenderTarget* target);
    bool ensurePipelines(QRhi* rhi, QRhiRenderTarget* target);
    void uploadHair(QRhi* rhi, QRhiResourceUpdateBatch* batch);
    // Whether strands_ go in by partial writes (the textures hold the strands they change from).
    bool partialUpload() const;
    void joinBuild();
    void uploadEnvironment(QRhi* rhi, QRhiResourceUpdateBatch* batch);
    void uploadSurfaces(QRhi* rhi, QRhiResourceUpdateBatch* batch);
    void uploadTables(QRhi* rhi, QRhiResourceUpdateBatch* batch);
    QRhiTexture* textureFor(QRhi* rhi, QRhiResourceUpdateBatch* batch, const std::string& path, bool srgb);
    QRhiTexture* maskTextureFor(QRhi* rhi, QRhiResourceUpdateBatch* batch, const SurfaceMaterial& material);
    void fillFrame(FrameUniforms& u, const OrbitCamera& camera, int frame, bool refine) const;
    void fillMaterial(MaterialUniforms& u, const Surface& surface) const;
    void updateBounds();
    // The enabled lights in the shader's slots (each with a deep opacity map when it casts).
    std::vector<DirectionalLight> activeLights() const;
    // An orthographic view from `toward` over `bounds` for a map of `size` texels, as the
    // pass `which` (the view block's index, which the hair shader reads).
    void lightPass(PassUniforms& u, const std::array<float, 3>& toward, int which, int size, const Bounds& bounds, QRhi* rhi) const;
    // The first skin surface the screen-space blur serves (null: none this frame).
    const Surface* screenSpaceSkin() const;
    // The visibility path: whether this device runs it, its buffers and pipelines (false: the
    // legacy path draws), the voxel grid fit to the hair (the Frame block's voxel fields), the
    // level of detail's exponents for this view (uploaded when the selection changed).
    bool visSupported(QRhi* rhi) const;
    bool ensureVis(QRhi* rhi);
    void fitVoxelGrid();
    void selectLod(QRhi* rhi, QRhiResourceUpdateBatch* batch, const OrbitCamera& camera, const QSize& size);

    std::unique_ptr<Gpu> gpu_;
    QRhiViewport passViewport_;
    StrandsPtr strands_;
    MeshPtr scalp_;
    std::uint64_t hairVersion_ = 0, uploadedVersion_ = ~std::uint64_t{0};
    std::shared_ptr<HairBuffers> pending_;  // built, waiting for the next render
    bool pointsHeld_ = false;      // setHair's changes.pointsHeld
    QByteArray pointStrandsScratch_;  // the strands of the points, for a batch's upload (kept until it runs)
    bool pointsFromGpu_ = false;
    bool uploadHeld_ = false;      // the last upload was a held one   // the point texture holds a GPU run's points (hairWritten), no upload since
    // A held upload (pointsHeld_): the strands' entries and point colors alone. False when the
    // texture does not hold the GPU's points of this layout (a whole write follows).
    bool heldUpload(QRhiResourceUpdateBatch* batch);
    HairIndices indicesNeeded() const;      // what a whole upload of strands_ needs built
    // A whole write's buffers building ahead (setBuildAhead): for hair version `version`, on
    // `thread`, `buffers` once `done`.
    struct HairBuild {
        std::uint64_t version = 0;
        std::atomic<bool> done{false};
        std::shared_ptr<HairBuffers> buffers;
    };
    std::shared_ptr<HairBuild> build_;
    std::thread buildThread_;
    bool buildAhead_ = false;
    bool waitingForBuild_ = false;  // the last frame kept the old hair while the build went on
    HairChanges changes_;                   // how strands_ differ from the strands shown before
    std::uint64_t uploadedSerial_ = 0;      // the serial of the strands the textures hold (0: unknown)
    std::uint64_t uploadedLayout_ = 0;      // their point counts' hash
    bool partialUploads_ = true;
    bool pointShading_ = true;
    HairPath hairPath_ = HairPath::Visibility;
    HairPath lastPath_ = HairPath::Legacy;
    bool volumesStale_ = true;  // the voxel volumes are to be made again (the hair or the lights changed)
    // The voxel grid: its lowest corner, a voxel's size, voxels along each axis.
    Vec3f gridLo_{0.0f, 0.0f, 0.0f};
    float voxelSize_ = 1.0f;
    std::array<int, 3> gridDims_{0, 0, 0};
    bool lodOn_ = true, lodActive_ = false;
    float lodError_ = 0.175f;
    std::unique_ptr<StrandLod> lod_;
    std::uint64_t lodKey_ = 0;  // the selection the exponents' buffer holds (0: none)
    StrandLodView lodPrevious_;  // the view last chosen (its levels' hysteresis)
    std::size_t drawnStrands_ = 0;
    bool uploadPartial_ = false, uploadedThisFrame_ = false;
    std::size_t uploadStrands_ = 0, uploads_ = 0;
    QByteArray pointsScratch_, strandsScratch_, pointColorsScratch_;  // the textures' padded rows, kept between whole uploads
    bool pointColored_ = false;  // the point colors texture holds a texel a point (HairBuffers::pointColored)
    HairMaterial material_;
    LightingSetup lighting_;
    std::vector<SurfacePtr> surfaces_;
    bool surfacesDirty_ = true;
    EnvironmentMap environment_;
    std::array<RGB, 9> sh_{};
    std::vector<DirectionalLight> envLights_;
    bool environmentDirty_ = true;
    bool domsStale_ = true;  // the deep opacity maps are to be drawn again
    Environment environmentKind_ = Environment::None;
    Bounds hairBounds_, surfaceBounds_, bounds_;  // the hair's (its maps), the surfaces' (their shadow maps), both
    std::vector<SssKernelSample> kernel_;         // the separable blur's, for kernelMfp_
    RGB kernelMfp_{0.0f, 0.0f, 0.0f};
    int accumulated_ = 0;
    int hairDraws_ = 0, surfaceDraws_ = 0;
    std::size_t pointCount_ = 0, strandCount_ = 0;
    double uploadMs_ = 0.0;
    // Adaptive interactive quality: the frames' clock, when the last frame started (the next
    // one's start measures it when they come back to back), whether a change came since it
    // and before it (a stream), whether the host holds a drag, the full frame's cost (an
    // average of the measured ones; 0: none yet), the stream's scale (chosen as it starts,
    // lowered when a frame goes over budget), what the last frame did.
    float chooseScale(const QSize& size) const;
    void noteFrameCost(double ms);
    QElapsedTimer clock_;
    qint64 lastRenderNs_ = -1;
    bool changedSinceFrame_ = false, changedBeforeLast_ = false, interacting_ = false;
    double fullFrameMs_ = 0.0;
    float streamScale_ = 0.0f;  // 0: no stream running
    float frameScale_ = 1.0f;
    bool reduced_ = false, lastFrameHeavy_ = false;
};

}  // namespace groomlab::app
