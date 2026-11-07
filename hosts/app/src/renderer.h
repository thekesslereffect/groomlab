// SPDX-License-Identifier: Apache-2.0
#pragma once

// The viewport's renderer on Qt's RHI: draws a DisplayFrame (the core's DisplayBuilder items:
// lit hair lines, flat and colored lines, overlays, shaded surfaces) and the scalp, on whatever
// QRhi the host gives (a QRhiWidget's, or an offscreen one in the tests). It keeps a GPU copy of
// each item and uploads it again only when the builder rebuilt it, so a redraw with nothing
// changed costs the draw calls alone.
//
// With lookdev on (the default where the device allows it), the strands are not the builder's
// lines but the LookdevRenderer's ribbons, shaded as the render would, over an HDR pass that
// also takes the scalp and the other depth-tested items; the overlays (guides, parts, flow) go
// over the tone-mapped image. Without it (or on a device without storage buffers) everything
// draws as before, Kajiya-Kay lines included.
//
// When only some strands changed (a brush dab, a stroke's release: DisplayItem::updated says
// which), only their vertex ranges are written into the strand lines' buffers and only their
// texels into the lookdev textures (the rest, the indices and the uvs kept); whole uploads
// when the layout changed. With a GPU look preview attached (gpu_preview.h), a drag's or a
// dab's run happens here, after this frame's uploads are recorded and before anything draws,
// straight into those buffers or the lookdev point texture.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <QColor>
#include <QImage>
#include <QRectF>
#include <QVector3D>
#include <rhi/qrhi.h>

#include "camera.h"
#include "groomlab/display.h"
#include "groomlab/strand_lod.h"
#include "look.h"
#include "lookdev_renderer.h"

namespace groomlab::app {

class GpuLookPreview;

class GroomRenderer {
public:
    GroomRenderer();
    ~GroomRenderer();
    GroomRenderer(const GroomRenderer&) = delete;
    GroomRenderer& operator=(const GroomRenderer&) = delete;

    // The frame the builder made: items it rebuilt (DisplayItem::rebuilt) are uploaded on the
    // next render, the others kept, and the guide dots (DisplayFrame::dots) with the guides.
    // Call it after every DisplayBuilder::update.
    void setFrame(const DisplayFrame& frame);
    // The tools' overlay (the brush ring, the hovered guides, a part being drawn: colored lines,
    // no depth test), drawn last; the same version keeps the upload. Empty: nothing.
    void setToolOverlay(const DisplayGeometry& lines, std::uint64_t version);
    // The scalp to shade (null: none); the same pointer keeps its buffers.
    void setScalp(MeshPtr scalp);
    // What the scalp is shaded as in the lookdev path (the "scalp" preset by default).
    void setScalpMaterial(const SurfaceMaterial& material);
    const SurfaceMaterial& scalpMaterial() const { return scalpSurface_->material; }
    // Meshes drawn beside the groom with materials of their own (a scanned head under it,
    // eyes): shaded, shadowed and casting in the lookdev path; Lambert in the line path.
    void setProps(std::vector<SurfacePtr> props);
    const std::vector<SurfacePtr>& props() const { return props_; }
    // A prop's material was edited in place: the maps are drawn again.
    void propsChanged();
    void setHairLook(const HairLook& look) { look_ = look; }
    void setBackground(const QColor& color) { background_ = color; }
    void setShowScalp(bool show);
    bool showScalp() const { return showScalp_; }
    // The hair shown (the Groom Tools' strands eye): off, the frame's hair items (isHairItem: the
    // strands in every form, the instances, the scales, the cards, the strand meshes) are not
    // drawn (their buffers kept) and the lookdev path draws no strands (the ones handed meanwhile
    // held for when the hair shows again); the guides, their dots and the overlays draw as ever.
    void setHairShown(bool shown);
    bool hairShown() const { return hairShown_; }
    // Width as coverage (DisplaySettings::widthCoverage, on by default): the strand lines cover
    // each point's width's share of a pixel (display.h lineCoverage), the widths times `widthScale`
    // (DisplaySettings::ribbonWidthScale), at the render's density from a preview at `density`
    // (lineCoverageAtDensity; 1: as it is); hashed (lineHashThreshold: a coverage mask on the
    // multisampled target, else kept or not, the top of the range blended: lineKeepShare), depth
    // written. Off: opaque lines.
    void setWidthCoverage(bool on, float widthScale = 1.0f, float density = 1.0f) {
        widthCoverage_ = on;
        widthScale_ = widthScale;
        coverageDensity_ = density;
    }
    bool widthCoverage() const { return widthCoverage_; }
    // Screen-space level of detail (strand_lod.h; DisplaySettings::clumpLod, on by default): with
    // width as coverage the line path draws each clump's strands by class (a share of them, each
    // widened to stand for the rest: the lines' uniform's class exponent), the clumps under a pixel
    // as one line each (impostors), chosen per view (the camera's distance: an orbit keeps them);
    // class 0's segments that may blend drawn again after everything, their blended fragments alone,
    // depth not written. `error`: the coverage error allowed (StrandLodSettings::error). The
    // lookdev path's visibility buffer thins the same way (LookdevRenderer::setClumpLod).
    void setClumpLod(bool on, float error = 0.175f) {
        lodOn_ = on;
        lodError_ = error;
        lookdev_.setClumpLod(on, error);
    }
    bool clumpLod() const { return lodOn_; }
    // What the last render drew with it (tests, stats).
    struct LodStats {
        bool on = false;  // the strand lines drawn by class
        int level = kNoLodLevel;
        std::size_t drawnStrands = 0, totalStrands = 0, impostors = 0, blendSegments = 0, rebuilds = 0, draws = 0;
    };
    const LodStats& lodStats() const { return lodStats_; }
    // The key light's direction (toward the light, world space) and the ambient floor (the
    // line path's).
    void setLight(const QVector3D& toward, float ambient) {
        lightDirection_ = toward.normalized();
        ambient_ = ambient;
    }

    // ---- Partial uploads and the GPU preview ------------------------------------------------------
    // Partial writes of the strand buffers and textures (default on; GROOMLAB_APP_PARTIAL_UPLOADS=0
    // turns them off): off, a changed item is written whole (the tests compare both).
    void setPartialUploads(bool on);
    bool partialUploads() const { return partialUploads_; }
    // The lookdev hair's whole writes built ahead, off this thread (LookdevRenderer::setBuildAhead).
    void setBuildAhead(bool on) { lookdev_.setBuildAhead(on); }
    // The last write of the strand lines' buffers: whether it wrote only the changed strands,
    // how many, and the CPU milliseconds it took (building and handing over).
    bool lastUploadPartial() const { return lineUploadPartial_; }
    std::size_t lastUploadStrands() const { return lineUploadStrands_; }
    double lastUploadMs() const { return lineUploadMs_; }
    // How many such writes there have been (a frame that wrote none leaves it: its upload cost 0).
    std::size_t uploads() const { return lineUploads_; }
    // The GPU look preview whose pending run the next render makes (null: none). Its results
    // stand in the buffers until the CPU's strands are uploaded again.
    void setGpuPreview(GpuLookPreview* preview);
    GpuLookPreview* gpuPreview() const { return gpuPreview_; }
    // Whether the last render ran the GPU preview (and what it wrote stands), and why not.
    bool gpuRanLastFrame() const { return gpuRan_; }
    const std::string& gpuWhy() const { return gpuWhy_; }

    // ---- Lookdev ------------------------------------------------------------------------------
    // The strands the lookdev path draws (the view's; `version` moving means new buffers,
    // written whole or, when `changes` says which strands moved from the ones held, only those).
    void setHairStrands(StrandsPtr strands, std::uint64_t version, const HairChanges& changes = {});
    void setMaterial(const HairMaterial& material);
    void setLighting(const LightingSetup& lighting);
    const HairMaterial& material() const { return lookdev_.material(); }
    const LightingSetup& lighting() const { return lookdev_.lighting(); }
    void setLookdev(bool on);
    bool lookdev() const { return lookdevWanted_; }
    // The lookdev path drew the last frame (wanted, and the device allows it).
    bool lookdevActive() const { return lookdevActive_; }
    // Refine mode and the image still converging, or a GPU preview run put off until this
    // frame's uploads landed: the host should render again when idle.
    bool wantsAnotherFrame() const { return (lookdevActive_ && lookdev_.wantsAnotherFrame()) || gpuDeferred_; }
    // The camera (or anything) moved: the accumulation starts over.
    void invalidateAccumulation() { lookdev_.invalidate(); }
    int accumulatedFrames() const { return lookdev_.accumulatedFrames(); }
    LookdevRenderer& lookdevRenderer() { return lookdev_; }
    const LookdevRenderer& lookdevRenderer() const { return lookdev_; }

    // Draws one frame into `target` with `cb` (inside the host's frame: QRhiWidget::render, or
    // beginOffscreenFrame): uploads what changed, the passes, every shown item. Pipelines
    // are made for the target's render pass on first use (and again for an incompatible one).
    void render(QRhi* rhi, QRhiCommandBuffer* cb, QRhiRenderTarget* target, const OrbitCamera& camera);
    // Drops every item (another groom opens); the scalp stays until setScalp.
    void clear();
    // Drops every GPU object (the host's QRhi is going away: QRhiWidget::releaseResources).
    void releaseResources();

    // Draw calls the last render made (tests, stats).
    int lastDrawCalls() const { return drawCalls_; }

    // ---- A reference photo (the Photos tab's Look Through) -------------------------------------
    // The photo the viewport looks through, fitted inside the frame at its aspect (as the camera's
    // field of view fits it: OrbitCamera::setLookThrough), at `opacity`: in front, over the
    // strands and the meshes (the overlays stay on top); behind, the backdrop (the strands then
    // draw as lines: the lookdev's own backdrop is opaque). A null image: none. `key` moving
    // uploads the image again.
    struct Photo {
        QImage image;
        std::uint64_t key = 0;
        float opacity = 0.6f;
        bool front = true;
    };
    void setPhoto(const Photo& photo);
    void clearPhoto() { setPhoto({}); }

    // ---- The texture map on the scalp (the Paint view) --------------------------------------------
    // The image the frame's scalp map item (mapTexture) reads: a texture map's file, or the brush's
    // live image while painting (uploaded again as its revision moves), as GroomDocument::
    // shownScalpMap has it. Kind "": none.
    void setScalpMap(const ScalpMapView& view);
    bool photoShown() const { return !photo_.image.isNull(); }
    const Photo& photo() const { return photo_; }
    // The photo's rectangle in a frame `size` big (pixels, y down): letterboxed at its aspect.
    static QRectF photoRect(const QSize& size, double photoAspect);

private:
    // StrandLines and StrandLinesColored: the strand lines flat (one color, or the color modes'),
    // with their uvs (u, width) for width as coverage, as LinesLit has them; Lines and
    // LinesColored: other lines.
    enum class Kind {
        LinesLit, Lines, LinesColored, StrandLines, StrandLinesColored, Overlay, OverlayColored, Triangles, TrianglesColored,
        Surface, Dots, MapTextured, Count
    };
    struct GpuItem;
    struct Pipeline;
    // The pipelines for one render pass (the host's target, or the lookdev HDR pass).
    struct PipelineSet {
        std::array<std::unique_ptr<Pipeline>, static_cast<std::size_t>(Kind::Count)> pipelines;
        // The strand lines' (LinesLit, StrandLines, StrandLinesColored) without depth writes: the
        // level of detail's blended fragments, drawn after everything (null for the other kinds).
        std::array<std::unique_ptr<Pipeline>, static_cast<std::size_t>(Kind::Count)> noDepthWrite;
        QVector<quint32> format;
        int sampleCount = 0;
        bool ready() const { return pipelines[0] != nullptr; }
    };
    void ensurePipelines(QRhi* rhi, QRhiRenderPassDescriptor* pass, int sampleCount, PipelineSet& set);
    void upload(QRhi* rhi, QRhiResourceUpdateBatch* batch, GpuItem& item);
    // The GPU preview's pending run into the strands' buffers (line path) or the lookdev point
    // texture, unless this frame uploaded them (then next frame: gpuDeferred_).
    void runGpuPreview(QRhi* rhi, bool lookdev);
    void writeUniforms(QRhiResourceUpdateBatch* batch, GpuItem& item, const QMatrix4x4& mvp, const QVector3D& eye, const QSize& size,
                       bool hdr, float pixelScale);
    // The uniforms of `item` into `uniforms` (its own: writeUniforms), the lines' level of detail
    // (lines.z: the class's exponent, 0 none), pass (lines.w: 0 every fragment, 1 the opaque, 2
    // the blended) and density (lines.x; < 0: the item's).
    void fillUniforms(QRhiResourceUpdateBatch* batch, QRhiBuffer* uniforms, const GpuItem& item, const QMatrix4x4& mvp,
                      const QVector3D& eye, const QSize& size, bool hdr, float pixelScale, float lod = 0.0f, float pass = 0.0f,
                      float density = -1.0f);
    void draw(QRhiCommandBuffer* cb, const GpuItem& item, const PipelineSet& set, bool hdr);
    // An item's streams and `indices` (its own when null) set for a draw; false when it lacks one.
    bool setInputs(QRhiCommandBuffer* cb, const GpuItem& item, QRhiBuffer* indices = nullptr);
    // The level of detail for this frame (the line path): the selection, its indices, the classes'
    // uniforms, the impostors (uploaded with `batch`); lod_->item the item it draws (null: none).
    struct LodDraw;
    void prepareLod(QRhi* rhi, QRhiResourceUpdateBatch* batch, const OrbitCamera& camera, const QMatrix4x4& mvp, const QSize& size,
                    float pixelScale);
    // Its draws: the classes and the blendable segments' opaque fragments (with the opaque items),
    // the blended fragments (after everything else).
    void drawLod(QRhiCommandBuffer* cb, bool blended);
    Kind kindOf(const DisplayGeometry& geometry, bool overlay) const;
    static bool isOverlay(Kind kind) { return kind == Kind::Overlay || kind == Kind::OverlayColored || kind == Kind::Dots; }
    void setDots(const GuideDots& dots, float pointSize);

    void updateSurfaces();  // the lookdev path's surface list: the scalp (when shown), the props
    std::vector<std::unique_ptr<GpuItem>> items_;  // one per DisplayItemId, then the scalp
    std::unique_ptr<GpuItem> scalp_;
    std::unique_ptr<GpuItem> dots_;  // the guide points, camera-facing diamonds (dots.vert)
    std::unique_ptr<GpuItem> tool_;  // the tools' overlay
    std::uint64_t toolVersion_ = 0;
    MeshPtr scalpMesh_;
    SurfacePtr scalpSurface_;
    std::vector<SurfacePtr> props_;
    std::vector<std::unique_ptr<GpuItem>> propItems_;  // the line path's copies
    PipelineSet main_, hdr_;
    QRhi* rhi_ = nullptr;
    // The viewport of the pass being drawn, set again after each pipeline is bound (on Direct3D
    // 12 QRhi sets the scissor from the viewport only while a pipeline is bound: a viewport set
    // before the pass's first pipeline left the scissor empty and every draw clipped away).
    QRhiViewport passViewport_;
    HairLook look_;
    QColor background_{23, 24, 27};  // deep charcoal, as the look-dev default
    QVector3D lightDirection_{0.4f, 0.8f, 0.6f};
    float ambient_ = 0.25f;
    bool widthCoverage_ = true;
    float coverageDensity_ = 1.0f;
    float widthScale_ = 1.0f;
    bool lodOn_ = true;
    float lodError_ = 0.175f;
    std::unique_ptr<LodDraw> lod_;
    LodStats lodStats_;
    bool showScalp_ = true;
    bool hairShown_ = true;
    // The strands last handed (setHairStrands), held while the hair is hidden.
    StrandsPtr hairStrands_;
    std::uint64_t hairVersion_ = 0;
    int drawCalls_ = 0;
    LookdevRenderer lookdev_;
    bool lookdevWanted_ = true;
    bool lookdevActive_ = false;
    bool partialUploads_ = true;
    bool lineUploadPartial_ = false;
    std::size_t lineUploadStrands_ = 0, lineUploads_ = 0;
    double lineUploadMs_ = 0.0;
    GpuLookPreview* gpuPreview_ = nullptr;
    GpuItem* gpuItem_ = nullptr;  // the line path's item the last run wrote
    bool gpuDeferred_ = false, gpuRan_ = false;
    std::string gpuWhy_;
    // The strand lines written whole go through these (kept: no fresh pages each upload).
    struct Scratch {
        std::vector<float> positions, normals, tangents, uvs, colors;
        std::vector<std::uint32_t> indices;
    };
    Scratch scratch_;
    // The reference photo: its texture (uploaded when the key moves), quad uniforms and pipeline
    // (for the host target's pass).
    Photo photo_;
    struct PhotoGpu {
        std::unique_ptr<QRhiTexture> texture;
        std::unique_ptr<QRhiSampler> sampler;
        std::unique_ptr<QRhiBuffer> uniforms;
        std::unique_ptr<QRhiShaderResourceBindings> bindings;
        std::unique_ptr<QRhiGraphicsPipeline> pipeline;
        QVector<quint32> format;
        int sampleCount = 0;
        std::uint64_t key = ~0ull;
    };
    PhotoGpu photoGpu_;
    // The scalp map's image: the view (its preview kept alive), its texture and the key it holds.
    ScalpMapView map_;
    std::string mapFileKey_;  // the file's (its stamp), worked out when the view is set
    struct MapGpu {
        std::unique_ptr<QRhiTexture> texture;
        std::unique_ptr<QRhiSampler> sampler;
        std::string key;
    };
    MapGpu mapGpu_;
    // The image into the texture when it changed (in `batch`); the scalp map item's bindings
    // (they hold the texture) made again when the texture is new. False: no image.
    bool prepareMapTexture(QRhi* rhi, QRhiResourceUpdateBatch* batch);
    // The photo's texture and uniforms for this frame (its own batch, before any pass); false
    // when none draws.
    bool preparePhoto(QRhi* rhi, QRhiCommandBuffer* cb, QRhiRenderTarget* target);
    void drawPhoto(QRhiCommandBuffer* cb);
};

// The pixels of a texture the renderer drew into, rows top to bottom whatever the backend's
// framebuffer orientation (QRhi::isYUpInFramebuffer), as read back after the frame: the tests'
// and the screenshot's path. Empty when the readback gave nothing (the Null backend).
QImage imageFromReadback(const QRhiReadbackResult& result, bool yUpInFramebuffer);

}  // namespace groomlab::app
