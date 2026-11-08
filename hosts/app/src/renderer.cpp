// SPDX-License-Identifier: Apache-2.0
#include "renderer.h"

#include <cmath>
#include <cstring>

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <rhi/qrhi_platform.h>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#endif

#include "gpu_preview.h"
#include "groomlab/math.h"
#include "groomlab/scalp_paint.h"

// Q_INIT_RESOURCE cannot sit in a namespace: the shaders' resources (a static library's) are
// pulled in here.
static void initShaderResources() {
    Q_INIT_RESOURCE(groomlab_app_shaders);
    Q_INIT_RESOURCE(groomlab_lookdev_shaders);
    Q_INIT_RESOURCE(groomlab_sss_prepass_shader);
    Q_INIT_RESOURCE(groomlab_hair_point_shader);
    Q_INIT_RESOURCE(groomlab_hair_gouraud_shader);
    Q_INIT_RESOURCE(groomlab_hair_compute_shaders);
    Q_INIT_RESOURCE(groomlab_strand_shaders);
    Q_INIT_RESOURCE(groomlab_strand_mask_shaders);
}

namespace groomlab::app {

namespace {

// The uniform block every line shader takes (std140: a mat4 then four vec4s, 128 bytes), and
// the viewport after them, which the dots and the strand lines read: its size (xy), the pixels a
// scene unit spans at clip w 1 for width as coverage (z: half the height times the projection's
// [1][1], display.h linePixelScale; 0: off) and the widths' scale (w); then the strand lines'
// `lines`: the preview's density the coverage is taken from (x: lineCoverageAtDensity, 1 as it
// is) and the target's samples (y: the coverage masks'). The others declare the first 128 or 144
// bytes of the same buffer.
struct Uniforms {
    float mvp[16];
    float color[4];
    float highlight[4];
    float camera[4];
    float light[4];
    float viewport[4];
    float lines[4];
};
static_assert(sizeof(Uniforms) == 160, "the shaders' uniform block is 128 bytes, the dots' 144, the strand lines' 160");

QShader loadShader(const QString& name) {
    QFile file(QStringLiteral(":/groomlab/shaders/") + name + QStringLiteral(".qsb"));
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning("GroomLab: shader %s is missing from the resources", qPrintable(name));
        return {};
    }
    return QShader::fromSerialized(file.readAll());
}

constexpr int kStrideXYZ = 3 * sizeof(float);
constexpr int kStrideUV = 2 * sizeof(float);
// The strand lines' uvs: u root to tip and the point's width (StrandLines::writeUvs, 2 floats).
constexpr int kStrideRGBA = 4 * sizeof(float);
// Runs of changed strands fewer than this many strands apart are written as one (strandRuns):
// a few unchanged strands rewritten cost less than another write.
constexpr std::size_t kRunGap = 64;

// A whole buffer written straight through the device where the API allows it: on Direct3D
// 11, UpdateSubresource on the buffer's own object from `data` (the driver's copy to the GPU is
// the only one), where QRhi's upload batch would first copy the bytes into itself (tens of MB
// a tweak at 500k strands, as much again as the write itself). Ordered as the batch would be:
// the device runs it now, before the frame's recorded draws, after the last frame's. False
// when this device cannot (the batch is used).
bool writeBufferNative(QRhi* rhi, QRhiBuffer* buffer, const void* data, std::size_t bytes) {
#ifdef Q_OS_WIN
    if (rhi->backend() != QRhi::D3D11 || !buffer || bytes == 0) return false;
    const auto* handles = static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
    auto* context = handles ? static_cast<ID3D11DeviceContext*>(handles->context) : nullptr;
    const QRhiBuffer::NativeBuffer nb = buffer->nativeBuffer();
    if (!context || nb.slotCount < 1 || !nb.objects[0]) return false;
    auto* native = *static_cast<ID3D11Buffer* const*>(nb.objects[0]);
    if (!native) return false;
    context->UpdateSubresource(native, 0, nullptr, data, 0, 0);
    return true;
#else
    (void)rhi;
    (void)buffer;
    (void)data;
    (void)bytes;
    return false;
#endif
}

// Smooth normals of a scalp for shading: each vertex the area-weighted mean of its triangles'.
void scalpSurface(const ScalpMesh& scalp, std::vector<float>& positions, std::vector<float>& normals,
                  std::vector<std::uint32_t>& indices) {
    const auto& points = scalp.points();
    std::vector<Vec3f> sums(points.size(), Vec3f{0.0f, 0.0f, 0.0f});
    indices.reserve(scalp.triangleCount() * 3);
    for (const ScalpMesh::Triangle& t : scalp.triangles()) {
        const Vec3f n = (points[t[1]] - points[t[0]]).cross(points[t[2]] - points[t[0]]);  // twice the area, outward
        for (int k = 0; k < 3; ++k) {
            sums[t[k]] += n;
            indices.push_back(static_cast<std::uint32_t>(t[k]));
        }
    }
    positions.resize(points.size() * 3);
    normals.resize(points.size() * 3);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const Vec3f n = sums[i].normalized();
        positions[i * 3] = points[i].x;
        positions[i * 3 + 1] = points[i].y;
        positions[i * 3 + 2] = points[i].z;
        normals[i * 3] = n.x;
        normals[i * 3 + 1] = n.y;
        normals[i * 3 + 2] = n.z;
    }
}

}  // namespace

// One item's GPU copy: the streams its kind needs, its uniform buffers (one per pass it may
// draw in: the host's target, the lookdev HDR pass) and bindings.
struct GroomRenderer::GpuItem {
    Kind kind = Kind::Lines;
    std::unique_ptr<QRhiBuffer> positions, normals, tangents, uvs, colors, indices;
    std::unique_ptr<QRhiBuffer> offsets;  // dots: each vertex's pixel offset
    std::unique_ptr<QRhiBuffer> uniforms, uniformsHdr;
    std::unique_ptr<QRhiShaderResourceBindings> bindings, bindingsHdr;
    std::uint32_t indexCount = 0;
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
    bool shown = false;
    bool overlay = false;
    // What waits for the next render: a geometry to upload (whole, or, strand lines whose
    // `pendingUpdated` lists the strands that moved from the ones the buffers hold, those
    // strands' ranges alone), or colors alone.
    std::shared_ptr<const DisplayGeometry> pending;
    ChangesPtr pendingUpdated;
    std::shared_ptr<const StrandColors> pendingColors;
    // The strand lines the buffers hold, as last written (the level of detail's strands).
    std::shared_ptr<const StrandLines> lines;
    // The buffers hold strand lines as last built (`heldVertices` points): a partial write may
    // follow. Cleared when they are released or a GPU preview wrote them in place.
    bool holds = false;
    std::size_t heldVertices = 0;
    std::uint64_t heldLayout = 0;  // their point counts' hash (StrandLines::layoutKey): the indices hold with it
    std::uint64_t heldWidths = 0;  // ... and their widths' (StrandLines::widthsKey): the uvs (u, width) with both
    bool fresh = false;  // uploaded (whole or in part) this frame
    // A surface of the renderer's own (the scalp), or the dots: their streams.
    std::vector<float> ownPositions, ownNormals, ownOffsets, ownColors;
    std::vector<std::uint32_t> ownIndices;
    bool ownPending = false;

    void release() {
        positions.reset();
        normals.reset();
        tangents.reset();
        uvs.reset();
        colors.reset();
        offsets.reset();
        indices.reset();
        bindings.reset();
        bindingsHdr.reset();
        uniforms.reset();
        uniformsHdr.reset();
        indexCount = 0;
        holds = false;
        heldVertices = 0;
        lines.reset();
    }
};

// The level of detail's state (strand_lod.h): the selection, its indices (every class's segments
// that do not blend, then class 0's blendable ones: StrandLod::classSegments), a uniform buffer and
// bindings per slot (the classes, the blendable segments' opaque pass, their blended pass), the
// impostors' item.
struct GroomRenderer::LodDraw {
    static constexpr std::size_t kBlendOpaque = kLodClassCount, kBlended = kLodClassCount + 1, kSlots = kLodClassCount + 2;
    StrandLod lod;
    StrandLodView view;  // the view last chosen (its levels' hysteresis)
    std::uint64_t key = 0;  // the selection the indices hold
    std::unique_ptr<QRhiBuffer> indices;
    std::array<std::size_t, kLodClassCount + 2> firsts{};
    std::array<std::unique_ptr<QRhiBuffer>, kSlots> uniforms;
    std::array<std::unique_ptr<QRhiShaderResourceBindings>, kSlots> bindings;
    GpuItem impostors;
    std::uint64_t impostorKey = 0, impostorColors = 0;
    GpuItem* item = nullptr;  // drawn by class this frame (null: as it is)
    void release() {
        indices.reset();
        for (auto& u : uniforms) u.reset();
        for (auto& b : bindings) b.reset();
        impostors.release();
        impostors.pending.reset();
        impostors.shown = false;
        key = impostorKey = impostorColors = 0;
        item = nullptr;
    }
};

struct GroomRenderer::Pipeline {
    std::unique_ptr<QRhiShaderResourceBindings> layout;
    std::unique_ptr<QRhiGraphicsPipeline> pipeline;
};

GroomRenderer::GroomRenderer() : lod_(std::make_unique<LodDraw>()) {
    initShaderResources();
    for (std::size_t i = 0; i < kDisplayItemCount; ++i) items_.push_back(std::make_unique<GpuItem>());
    scalp_ = std::make_unique<GpuItem>();
    scalp_->kind = Kind::Surface;
    dots_ = std::make_unique<GpuItem>();
    dots_->kind = Kind::Dots;
    dots_->overlay = true;
    tool_ = std::make_unique<GpuItem>();
    tool_->kind = Kind::OverlayColored;
    tool_->overlay = true;
    scalpSurface_ = std::make_shared<Surface>();
    scalpSurface_->name = "scalp";
    scalpSurface_->material = surfacePreset("scalp");
    const RGB& c = scalpSurface_->material.baseColor;
    scalp_->color = {c[0], c[1], c[2], 1.0f};
}

GroomRenderer::~GroomRenderer() = default;

GroomRenderer::Kind GroomRenderer::kindOf(const DisplayGeometry& g, bool overlay) const {
    if (g.lines) {
        // Strand lines not written yet (StrandLines): what they will hold.
        if (overlay || g.overlay) return g.lines->colors ? Kind::OverlayColored : Kind::Overlay;
        if (g.lines->lit) return Kind::LinesLit;
        return g.lines->colors ? Kind::StrandLinesColored : Kind::StrandLines;
    }
    const bool colored = !g.colors.empty();
    if (g.triangles && g.mapTexture && !g.uvs.empty()) return Kind::MapTextured;  // the scalp map's image
    if (g.triangles) {
        if (g.surface || g.cardTextures || (!g.normals.empty() && !colored)) return Kind::Surface;
        return colored ? Kind::TrianglesColored : Kind::Triangles;
    }
    if (overlay || g.overlay) return colored ? Kind::OverlayColored : Kind::Overlay;
    if (g.lit() && !g.tangents.empty() && !g.uvs.empty()) return Kind::LinesLit;
    return colored ? Kind::LinesColored : Kind::Lines;
}

void GroomRenderer::setFrame(const DisplayFrame& frame) {
    for (std::size_t i = 0; i < kDisplayItemCount; ++i) {
        const DisplayItem& item = frame.items[i];
        GpuItem& gpu = *items_[i];
        gpu.color = item.color;
        gpu.shown = item.shown && item.geometry && !item.geometry->empty();
        if (item.rebuilt) {
            if (!item.geometry || item.geometry->empty()) {
                // Rebuilt empty: gone, or stepping back (keep); either way nothing to draw now.
                gpu.pending.reset();
                gpu.pendingUpdated.reset();
                gpu.pendingColors.reset();
                if (!item.keep) gpu.release();
            } else {
                // Strand lines stay as StrandLines until the upload writes them (whole into the
                // vectors, or the changed strands' ranges straight into the buffers).
                gpu.pending = item.geometry;
                gpu.pendingUpdated = item.updated;
                gpu.pendingColors.reset();
                gpu.overlay = item.geometry->overlay;
                const Kind kind = kindOf(*gpu.pending, gpu.overlay);
                if (kind != gpu.kind && (kind == Kind::MapTextured || gpu.kind == Kind::MapTextured)) {
                    // The scalp map between its image and per-vertex colors: its bindings hold the
                    // texture or not, made again for the new kind.
                    gpu.bindings.reset();
                    gpu.bindingsHdr.reset();
                }
                gpu.kind = kind;
            }
        } else if (item.recolor && item.recolored) {
            gpu.pendingColors = item.recolored;
        }
    }
    if (frame.rebuilt[static_cast<std::size_t>(DisplayPart::Guides)]) setDots(frame.dots, frame.pointSize);
    lookdev_.invalidate();
}

void GroomRenderer::setDots(const GuideDots& dots, float pointSize) {
    // Four vertices a dot, a diamond of the point size across (half of it each way), two
    // triangles; the shader moves each vertex by its offset in pixels.
    GpuItem& item = *dots_;
    const std::size_t n = dots.points.size();
    item.ownPositions.resize(n * 12);
    item.ownOffsets.resize(n * 8);
    item.ownColors.resize(n * 16);
    item.ownIndices.resize(n * 6);
    const float s = std::max(pointSize, 1.0f) * 0.5f;
    const float offsets[8] = {-s, 0.0f, 0.0f, s, s, 0.0f, 0.0f, -s};
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3f& p = dots.points[i];
        const std::array<float, 3>& c = dots.colors[i];
        for (std::size_t k = 0; k < 4; ++k) {
            float* pos = item.ownPositions.data() + (i * 4 + k) * 3;
            pos[0] = p.x;
            pos[1] = p.y;
            pos[2] = p.z;
            float* col = item.ownColors.data() + (i * 4 + k) * 4;
            col[0] = c[0];
            col[1] = c[1];
            col[2] = c[2];
            col[3] = 1.0f;
        }
        std::memcpy(item.ownOffsets.data() + i * 8, offsets, sizeof offsets);
        const auto first = static_cast<std::uint32_t>(i * 4);
        std::uint32_t* idx = item.ownIndices.data() + i * 6;
        idx[0] = first;
        idx[1] = first + 1;
        idx[2] = first + 2;
        idx[3] = first;
        idx[4] = first + 2;
        idx[5] = first + 3;
    }
    item.ownPending = true;
    item.shown = n > 0;
}

void GroomRenderer::setToolOverlay(const DisplayGeometry& lines, std::uint64_t version) {
    if (version == toolVersion_ && tool_->shown == !lines.empty()) return;
    toolVersion_ = version;
    tool_->shown = !lines.empty() && !lines.colors.empty();
    if (tool_->shown) {
        tool_->pending = std::make_shared<DisplayGeometry>(lines);
        tool_->kind = Kind::OverlayColored;
    } else {
        tool_->pending.reset();
    }
}

void GroomRenderer::setScalp(MeshPtr scalp) {
    if (scalp == scalpMesh_) return;
    scalpMesh_ = std::move(scalp);
    scalp_->ownPositions.clear();
    scalp_->ownNormals.clear();
    scalp_->ownIndices.clear();
    if (scalpMesh_ && scalpMesh_->triangleCount() > 0) {
        scalpSurface(*scalpMesh_, scalp_->ownPositions, scalp_->ownNormals, scalp_->ownIndices);
    }
    scalp_->ownPending = true;
    scalp_->shown = !scalp_->ownIndices.empty();
    scalpSurface_->mesh = scalpMesh_;
    ++scalpSurface_->version;
    updateSurfaces();
}

void GroomRenderer::setScalpMaterial(const SurfaceMaterial& material) {
    scalpSurface_->material = material;
    const RGB& c = material.baseColor;
    scalp_->color = {c[0], c[1], c[2], 1.0f};
    lookdev_.sceneChanged();
}

void GroomRenderer::setProps(std::vector<SurfacePtr> props) {
    props_ = std::move(props);
    propItems_.clear();
    for (const SurfacePtr& p : props_) {
        auto item = std::make_unique<GpuItem>();
        item->kind = Kind::Surface;
        const RGB& c = p->material.baseColor;
        item->color = {c[0], c[1], c[2], 1.0f};
        if (p->mesh && p->mesh->triangleCount() > 0) scalpSurface(*p->mesh, item->ownPositions, item->ownNormals, item->ownIndices);
        item->ownPending = true;
        item->shown = !item->ownIndices.empty();
        propItems_.push_back(std::move(item));
    }
    updateSurfaces();
}

void GroomRenderer::propsChanged() { lookdev_.sceneChanged(); }

void GroomRenderer::setOtherGrooms(const std::vector<OtherGroom>& grooms) {
    std::vector<std::unique_ptr<GpuItem>> items;
    std::vector<std::shared_ptr<const DisplayGeometry>> geometry;
    for (const OtherGroom& g : grooms) {
        if (!g.lines || g.lines->empty()) continue;
        // The same geometry: its item and buffers kept.
        std::unique_ptr<GpuItem> item;
        for (std::size_t i = 0; i < otherGeometry_.size(); ++i) {
            if (otherGeometry_[i] == g.lines && otherItems_[i]) item = std::move(otherItems_[i]);
        }
        if (!item) {
            item = std::make_unique<GpuItem>();
            item->pending = g.lines;
            item->kind = kindOf(*g.lines, false);
        }
        item->color = g.color;
        item->shown = true;
        items.push_back(std::move(item));
        geometry.push_back(g.lines);
    }
    otherItems_ = std::move(items);
    otherGeometry_ = std::move(geometry);
    lookdev_.invalidate();
}

void GroomRenderer::updateSurfaces() {
    std::vector<SurfacePtr> surfaces;
    if (showScalp_ && scalpSurface_->mesh && scalpSurface_->mesh->triangleCount() > 0) surfaces.push_back(scalpSurface_);
    for (const SurfacePtr& p : props_) surfaces.push_back(p);
    lookdev_.setSurfaces(std::move(surfaces));
}

void GroomRenderer::setShowScalp(bool show) {
    if (showScalp_ == show) return;
    showScalp_ = show;
    updateSurfaces();  // the scalp casts in the shadow maps
}

void GroomRenderer::setHairStrands(StrandsPtr strands, std::uint64_t version, const HairChanges& changes) {
    hairStrands_ = strands;
    hairVersion_ = version;
    if (hairShown_) lookdev_.setHair(std::move(strands), scalpMesh_, version, changes);
}

void GroomRenderer::setHairShown(bool shown) {
    if (shown == hairShown_) return;
    hairShown_ = shown;
    // The lookdev's strands go at once, or come back (written whole) from the ones held.
    lookdev_.setHair(shown ? hairStrands_ : nullptr, scalpMesh_, shown ? hairVersion_ : 0);
}

void GroomRenderer::setPartialUploads(bool on) {
    partialUploads_ = on;
    lookdev_.setPartialUploads(on);
}

void GroomRenderer::setGpuPreview(GpuLookPreview* preview) {
    if (gpuPreview_ && gpuPreview_ != preview) gpuPreview_->releaseResources();
    gpuPreview_ = preview;
}

void GroomRenderer::runGpuPreview(QRhi* rhi, bool lookdev) {
    gpuDeferred_ = false;
    gpuRan_ = false;
    gpuItem_ = nullptr;
    if (gpuPreview_) gpuPreview_->prepare(rhi);  // (OpenGL: the kernels compiled before the first drag or stroke)
    if (!gpuPreview_ || !gpuPreview_->pending()) return;
    // The targets: the lookdev path's point texture, or the strands item's buffers (and the
    // frames the lit lines read). Uploaded this frame, they would land after the run (QRhi
    // records uploads and draws, the kernels run now): the run waits for the next frame.
    GpuLookPreview::Targets t;
    bool fresh = false;
    if (lookdev) {
        const LookdevRenderer::GpuTargets h = lookdev_.gpuTargets();
        t.pointTexture = h.points;
        t.textureWidth = h.width;
        t.pointCount = h.pointCount;
        fresh = h.fresh;
    } else {
        // The item drawing the strands the run stands for: the strands, or, during a stroke,
        // the dimmed ones (the builder draws the stroke's strands flat there), or the colored
        // ones: whichever is shown with the output's point count.
        GpuItem* s = nullptr;
        for (const DisplayItemId id : {DisplayItemId::Strands, DisplayItemId::DimStrands, DisplayItemId::ColoredStrands}) {
            GpuItem& item = *items_[static_cast<std::size_t>(id)];
            const std::size_t points = item.positions ? item.positions->size() / kStrideXYZ : 0;
            if (item.shown && points > 0 && points == gpuPreview_->outputPoints()) {
                s = &item;
                break;
            }
        }
        if (!s) {
            gpuWhy_ = "no strand lines item draws the graph's output";
            gpuDeferred_ = true;
            return;
        }
        t.positions = s->positions.get();
        if (s->kind == Kind::LinesLit) {
            t.normals = s->normals.get();
            t.tangents = s->tangents.get();
        }
        t.uvs = s->uvs.get();  // (u, width): a Width the run has writes its widths there
        t.pointCount = s->positions->size() / kStrideXYZ;
        fresh = s->fresh;
        gpuItem_ = s;
    }
    if ((!t.pointTexture && !t.positions) || t.pointCount == 0) {
        gpuDeferred_ = true;  // nothing uploaded yet: after the upload
        return;
    }
    if (fresh) {
        gpuDeferred_ = true;
        return;
    }
    std::string why;
    if (!gpuPreview_->run(rhi, t, &why)) {
        if (why != gpuWhy_) qWarning("GroomLab: the GPU preview did not run: %s", why.c_str());
        gpuWhy_ = why;
        return;
    }
    gpuWhy_.clear();
    gpuRan_ = true;
    if (lookdev) {
        const gpu::LookBounds& b = gpuPreview_->bounds();
        Bounds bounds;
        bounds.valid = b.valid;
        bounds.lo = b.lo;
        bounds.hi = b.hi;
        lookdev_.hairWritten(bounds);
    } else if (gpuItem_) {
        // The buffers hold the GPU's strands, not the builder's: the next build writes them whole.
        gpuItem_->holds = false;
    }
}

void GroomRenderer::setMaterial(const HairMaterial& material) { lookdev_.setMaterial(material); }

void GroomRenderer::setLighting(const LightingSetup& lighting) { lookdev_.setLighting(lighting); }

void GroomRenderer::setLookdev(bool on) {
    lookdevWanted_ = on;
    lookdev_.invalidate();
}

void GroomRenderer::clear() {
    lod_->release();
    for (auto& item : items_) {
        item->release();
        item->pending.reset();
        item->pendingColors.reset();
        item->shown = false;
    }
    dots_->release();
    dots_->ownIndices.clear();
    dots_->shown = false;
    tool_->release();
    tool_->pending.reset();
    tool_->shown = false;
    hairStrands_.reset();
    hairVersion_ = 0;
    lookdev_.setHair(nullptr, nullptr, 0);
}

void GroomRenderer::releaseResources() {
    for (auto& item : items_) {
        item->release();
        // What was uploaded must go up again on the next render: keep nothing pending twice.
    }
    scalp_->release();
    scalp_->ownPending = !scalp_->ownIndices.empty();
    dots_->release();
    dots_->ownPending = !dots_->ownIndices.empty();
    tool_->release();
    for (auto& item : propItems_) {
        item->release();
        item->ownPending = !item->ownIndices.empty();
    }
    for (std::size_t i = 0; i < otherItems_.size(); ++i) {
        otherItems_[i]->release();
        otherItems_[i]->pending = otherGeometry_[i];  // written again on the next render
    }
    main_ = PipelineSet();
    lod_->release();
    hdr_ = PipelineSet();
    rhi_ = nullptr;
    photoGpu_ = PhotoGpu();
    lookdev_.releaseResources();
    if (gpuPreview_) gpuPreview_->releaseResources();
}

// ---- The reference photo ------------------------------------------------------------------------

void GroomRenderer::setPhoto(const Photo& photo) { photo_ = photo; }

QRectF GroomRenderer::photoRect(const QSize& size, double photoAspect) {
    const double w = std::max(size.width(), 1), h = std::max(size.height(), 1);
    if (!(photoAspect > 0.0)) return QRectF(0.0, 0.0, w, h);
    if (w / h >= photoAspect) {
        const double pw = h * photoAspect;  // as tall as the frame, bars left and right
        return QRectF((w - pw) * 0.5, 0.0, pw, h);
    }
    const double ph = w / photoAspect;  // as wide as the frame, bars above and below
    return QRectF(0.0, (h - ph) * 0.5, w, ph);
}

bool GroomRenderer::preparePhoto(QRhi* rhi, QRhiCommandBuffer* cb, QRhiRenderTarget* target) {
    if (photo_.image.isNull()) return false;
    PhotoGpu& g = photoGpu_;
    QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();
    if (!g.uniforms) {
        g.uniforms.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 32));
        if (!g.uniforms->create()) g.uniforms.reset();
    }
    if (!g.sampler) {
        g.sampler.reset(rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge,
                                        QRhiSampler::ClampToEdge));
        if (!g.sampler->create()) g.sampler.reset();
    }
    if (g.uniforms && g.sampler && (g.key != photo_.key || !g.texture)) {
        // The image as uploaded: its first row at v 0 (the quad's top), at most the device's size.
        QImage image = photo_.image.convertToFormat(QImage::Format_RGBA8888);
        const int limit = std::max(rhi->resourceLimit(QRhi::TextureSizeMax), 256);
        if (image.width() > limit || image.height() > limit) image = image.scaled(limit, limit, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        if (!g.texture || g.texture->pixelSize() != image.size()) {
            g.texture.reset(rhi->newTexture(QRhiTexture::RGBA8, image.size()));
            if (!g.texture->create()) g.texture.reset();
            g.bindings.reset();
        }
        if (g.texture) batch->uploadTexture(g.texture.get(), image);
        g.key = photo_.key;
    }
    if (!g.texture) {
        batch->release();
        return false;
    }
    if (!g.bindings) {
        g.bindings.reset(rhi->newShaderResourceBindings());
        g.bindings->setBindings({QRhiShaderResourceBinding::uniformBuffer(
                                     0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, g.uniforms.get()),
                                 QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, g.texture.get(),
                                                                           g.sampler.get())});
        if (!g.bindings->create()) g.bindings.reset();
        g.pipeline.reset();
    }
    const QVector<quint32> format = target->renderPassDescriptor()->serializedFormat();
    if (g.bindings && (!g.pipeline || g.format != format || g.sampleCount != target->sampleCount())) {
        g.pipeline.reset(rhi->newGraphicsPipeline());
        g.pipeline->setShaderStages({{QRhiShaderStage::Vertex, loadShader("photo.vert")}, {QRhiShaderStage::Fragment, loadShader("photo.frag")}});
        g.pipeline->setVertexInputLayout({});
        g.pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        QRhiGraphicsPipeline::TargetBlend blend;
        blend.enable = true;
        blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
        blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        blend.srcAlpha = QRhiGraphicsPipeline::One;
        blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        g.pipeline->setTargetBlends({blend});
        g.pipeline->setCullMode(QRhiGraphicsPipeline::None);
        g.pipeline->setDepthTest(false);
        g.pipeline->setDepthWrite(false);
        g.pipeline->setSampleCount(target->sampleCount());
        g.pipeline->setShaderResourceBindings(g.bindings.get());
        g.pipeline->setRenderPassDescriptor(target->renderPassDescriptor());
        if (!g.pipeline->create()) {
            qWarning("GroomLab: the reference photo's pipeline could not be made");
            g.pipeline.reset();
        }
        g.format = format;
        g.sampleCount = target->sampleCount();
    }
    // The photo's rectangle in normalized device coordinates (y up, or down where the backend's
    // NDC has it so).
    const QSize size = target->pixelSize();
    const QRectF r = photoRect(size, static_cast<double>(photo_.image.width()) / std::max(photo_.image.height(), 1));
    const float w = static_cast<float>(std::max(size.width(), 1)), h = static_cast<float>(std::max(size.height(), 1));
    float top = 1.0f - 2.0f * static_cast<float>(r.top()) / h, bottom = 1.0f - 2.0f * static_cast<float>(r.bottom()) / h;
    if (!rhi->isYUpInNDC()) {
        top = -top;
        bottom = -bottom;
    }
    const float u[8] = {2.0f * static_cast<float>(r.left()) / w - 1.0f, top, 2.0f * static_cast<float>(r.right()) / w - 1.0f, bottom,
                        std::clamp(photo_.opacity, 0.0f, 1.0f), 0.0f, 0.0f, 0.0f};
    batch->updateDynamicBuffer(g.uniforms.get(), 0, sizeof(u), u);
    cb->resourceUpdate(batch);
    return g.pipeline != nullptr;
}

void GroomRenderer::setScalpMap(const ScalpMapView& view) {
    map_ = view;
    mapFileKey_.clear();
    if (map_.kind == "texture" && !map_.preview && !map_.texture.empty()) {
        mapFileKey_ = "file:" + map_.texture + "@" + std::to_string(fileStamp(map_.texture)) + "#" +
                      (map_.channels == 4 ? map_.channel : std::string("gray"));
    }
}

bool GroomRenderer::prepareMapTexture(QRhi* rhi, QRhiResourceUpdateBatch* batch) {
    MapGpu& g = mapGpu_;
    if (map_.kind != "texture") return false;
    if (!g.sampler) {
        g.sampler.reset(rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge,
                                        QRhiSampler::ClampToEdge));
        if (!g.sampler->create()) {
            g.sampler.reset();
            return false;
        }
    }
    // The image: the brush's live one (each revision), else the file's (as the map shows it).
    const TexturePreview* preview = map_.preview && map_.preview->width() > 0 && map_.preview->height() > 0 ? map_.preview.get() : nullptr;
    const std::string key = preview ? "preview:" + std::to_string(preview->layout()) + ":" + std::to_string(preview->revision()) : mapFileKey_;
    if (key.empty()) return false;
    if (key == g.key && g.texture) return true;
    int width = 0, height = 0;
    const std::uint8_t* pixels = nullptr;
    std::vector<std::uint8_t> own;
    if (preview) {
        width = preview->width();
        height = preview->height();
        pixels = preview->rgba().data();
    } else {
        TexturePtr texture;
        try {
            texture = loadTexture(map_.texture);
        } catch (const std::exception&) {
        }
        const Texture::Image* image = texture ? texture->image() : nullptr;
        if (!image || image->width <= 0 || image->height <= 0) return false;
        own = scalpMapCachedPixels(*image, map_.channels, scalpMapPick(map_.channel));
        width = image->width;
        height = image->height;
        pixels = own.data();
    }
    if (!g.texture || g.texture->pixelSize() != QSize(width, height)) {
        g.texture.reset(rhi->newTexture(QRhiTexture::RGBA8, QSize(width, height)));
        if (!g.texture->create()) {
            g.texture.reset();
            return false;
        }
        // The scalp map item's bindings held the old texture.
        GpuItem& item = *items_[static_cast<std::size_t>(DisplayItemId::ScalpMap)];
        item.bindings.reset();
        item.bindingsHdr.reset();
    }
    // Rows bottom-up as they come: the first row at v 0, the bottom of the scalp's uv space.
    QRhiTextureSubresourceUploadDescription sub(pixels, static_cast<quint32>(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4));
    batch->uploadTexture(g.texture.get(), QRhiTextureUploadDescription({0, 0, sub}));
    g.key = key;
    return true;
}

void GroomRenderer::drawPhoto(QRhiCommandBuffer* cb) {
    const PhotoGpu& g = photoGpu_;
    if (!g.pipeline) return;
    cb->setGraphicsPipeline(g.pipeline.get());
    cb->setViewport(passViewport_);
    cb->setShaderResources(g.bindings.get());
    cb->draw(4);
    ++drawCalls_;
}

void GroomRenderer::ensurePipelines(QRhi* rhi, QRhiRenderPassDescriptor* pass, int sampleCount, PipelineSet& set) {
    const QVector<quint32> format = pass->serializedFormat();
    if (rhi == rhi_ && format == set.format && sampleCount == set.sampleCount && set.ready()) return;
    set.format = format;
    set.sampleCount = sampleCount;

    // The strand lines' coverage: by coverage masks on a multisampled target (LINES_SAMPLE_MASK).
    const bool masks = sampleCount > 1;
    const QShader linesLitVert = loadShader("lines_lit.vert");
    const QShader linesLitFrag = loadShader(masks ? "lines_lit_mask.frag" : "lines_lit.frag");
    const QShader flatVert = loadShader("flat.vert"), flatColoredVert = loadShader("flat_colored.vert");
    const QShader strandsVert = loadShader("strands.vert"), strandsColoredVert = loadShader("strands_colored.vert");
    const QShader strandsFrag = loadShader(masks ? "strands_mask.frag" : "strands.frag");
    const QShader flatFrag = loadShader("flat.frag");
    const QShader surfaceVert = loadShader("surface.vert"), surfaceFrag = loadShader("surface.frag");
    const QShader dotsVert = loadShader("dots.vert");
    const QShader mapVert = loadShader("map.vert"), mapFrag = loadShader("map.frag");

    // A kind's pipeline (`depthWrite` false: the strand lines' blended pass, depth tested alone).
    auto make = [&](Kind kind, bool depthWrite) {
        auto p = std::make_unique<Pipeline>();
        p->layout.reset(rhi->newShaderResourceBindings());
        if (kind == Kind::MapTextured) {
            p->layout->setBindings({QRhiShaderResourceBinding::uniformBuffer(
                                        0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, nullptr),
                                    QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, nullptr, nullptr)});
        } else {
            p->layout->setBindings({QRhiShaderResourceBinding::uniformBuffer(
                0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, nullptr)});
        }
        p->layout->create();

        auto* ps = rhi->newGraphicsPipeline();
        QRhiVertexInputLayout layout;
        bool triangles = false, depthTest = true;
        // The strand lines: width as coverage, hashed (opaque fragments, or coverage masks), its
        // top blended over what is behind (depth written: the head and the guides keep hiding
        // what they hide).
        bool blended = false;
        switch (kind) {
            case Kind::LinesLit:
                ps->setShaderStages({{QRhiShaderStage::Vertex, linesLitVert}, {QRhiShaderStage::Fragment, linesLitFrag}});
                layout.setBindings({{kStrideXYZ}, {kStrideXYZ}, {kStrideXYZ}, {kStrideUV}});
                layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0},
                                      {1, 1, QRhiVertexInputAttribute::Float3, 0},
                                      {2, 2, QRhiVertexInputAttribute::Float3, 0},
                                      {3, 3, QRhiVertexInputAttribute::Float2, 0}});  // u root to tip, width
                blended = true;
                break;
            case Kind::StrandLines:
                ps->setShaderStages({{QRhiShaderStage::Vertex, strandsVert}, {QRhiShaderStage::Fragment, strandsFrag}});
                layout.setBindings({{kStrideXYZ}, {kStrideUV}});
                layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0},
                                      {1, 1, QRhiVertexInputAttribute::Float2, 0}});
                blended = true;
                break;
            case Kind::StrandLinesColored:
                ps->setShaderStages({{QRhiShaderStage::Vertex, strandsColoredVert}, {QRhiShaderStage::Fragment, strandsFrag}});
                layout.setBindings({{kStrideXYZ}, {kStrideRGBA}, {kStrideUV}});
                layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0},
                                      {1, 1, QRhiVertexInputAttribute::Float4, 0},
                                      {2, 2, QRhiVertexInputAttribute::Float2, 0}});
                blended = true;
                break;
            case Kind::Lines:
            case Kind::Overlay:
            case Kind::Triangles:
                ps->setShaderStages({{QRhiShaderStage::Vertex, flatVert}, {QRhiShaderStage::Fragment, flatFrag}});
                layout.setBindings({{kStrideXYZ}});
                layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0}});
                triangles = kind == Kind::Triangles;
                depthTest = kind != Kind::Overlay;
                break;
            case Kind::LinesColored:
            case Kind::OverlayColored:
            case Kind::TrianglesColored:
                ps->setShaderStages({{QRhiShaderStage::Vertex, flatColoredVert}, {QRhiShaderStage::Fragment, flatFrag}});
                layout.setBindings({{kStrideXYZ}, {kStrideRGBA}});
                layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0},
                                      {1, 1, QRhiVertexInputAttribute::Float4, 0}});
                triangles = kind == Kind::TrianglesColored;
                depthTest = kind != Kind::OverlayColored;
                break;
            case Kind::Surface:
                ps->setShaderStages({{QRhiShaderStage::Vertex, surfaceVert}, {QRhiShaderStage::Fragment, surfaceFrag}});
                layout.setBindings({{kStrideXYZ}, {kStrideXYZ}});
                layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0},
                                      {1, 1, QRhiVertexInputAttribute::Float3, 0}});
                triangles = true;
                break;
            case Kind::Dots:
                ps->setShaderStages({{QRhiShaderStage::Vertex, dotsVert}, {QRhiShaderStage::Fragment, flatFrag}});
                layout.setBindings({{kStrideXYZ}, {kStrideUV}, {kStrideRGBA}});
                layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0},
                                      {1, 1, QRhiVertexInputAttribute::Float2, 0},
                                      {2, 2, QRhiVertexInputAttribute::Float4, 0}});
                triangles = true;
                depthTest = false;  // over the guides, as the guides are over the hair
                break;
            case Kind::MapTextured:
                ps->setShaderStages({{QRhiShaderStage::Vertex, mapVert}, {QRhiShaderStage::Fragment, mapFrag}});
                layout.setBindings({{kStrideXYZ}, {kStrideUV}});
                layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0},
                                      {1, 1, QRhiVertexInputAttribute::Float2, 0}});
                triangles = true;
                break;
            case Kind::Count:
                break;
        }
        ps->setVertexInputLayout(layout);
        if (blended) {
            QRhiGraphicsPipeline::TargetBlend blend;
            blend.enable = true;
            blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
            blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            blend.srcAlpha = QRhiGraphicsPipeline::One;
            blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            ps->setTargetBlends({blend});
        }
        ps->setTopology(triangles ? QRhiGraphicsPipeline::Triangles : QRhiGraphicsPipeline::Lines);
        ps->setCullMode(QRhiGraphicsPipeline::None);
        ps->setDepthTest(depthTest);
        ps->setDepthWrite(depthTest && depthWrite);
        ps->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
        ps->setSampleCount(sampleCount);
        ps->setShaderResourceBindings(p->layout.get());
        ps->setRenderPassDescriptor(pass);
        if (!ps->create()) qWarning("GroomLab: a viewport pipeline could not be made (kind %d)", static_cast<int>(kind));
        p->pipeline.reset(ps);
        return p;
    };
    for (std::size_t k = 0; k < static_cast<std::size_t>(Kind::Count); ++k) {
        const Kind kind = static_cast<Kind>(k);
        set.pipelines[k] = make(kind, true);
        const bool strandLines = kind == Kind::LinesLit || kind == Kind::StrandLines || kind == Kind::StrandLinesColored;
        set.noDepthWrite[k] = strandLines ? make(kind, false) : nullptr;
    }
    lod_->release();  // (its bindings reference the old layouts: made again)
    // The items' bindings reference the old layouts: made again on upload.
    for (auto& item : items_) {
        item->bindings.reset();
        item->bindingsHdr.reset();
    }
    for (GpuItem* item : {scalp_.get(), dots_.get(), tool_.get()}) {
        item->bindings.reset();
        item->bindingsHdr.reset();
    }
    for (auto& item : propItems_) {
        item->bindings.reset();
        item->bindingsHdr.reset();
    }
    for (auto& item : otherItems_) {
        item->bindings.reset();
        item->bindingsHdr.reset();
    }
}

void GroomRenderer::upload(QRhi* rhi, QRhiResourceUpdateBatch* batch, GpuItem& item) {
    // Strand lines are Static with the storage usage where the device computes: the changed
    // strands' ranges are written again in place (uploadStaticBuffer at an offset), and the GPU
    // look preview's kernels may write them (QRhi's StorageBuffer: unordered access on Direct3D
    // 11, shader storage on OpenGL); everything else is uploaded once.
    const bool lines = item.pending && item.pending->lines != nullptr;
    const bool storage = lines && gpuPreview_ && rhi->isFeatureSupported(QRhi::Compute);
    auto makeBuffer = [&](std::unique_ptr<QRhiBuffer>& buffer, const void* data, std::size_t bytes, bool index) {
        if (bytes == 0) {
            buffer.reset();
            return;
        }
        QRhiBuffer::UsageFlags usage = index ? QRhiBuffer::IndexBuffer : QRhiBuffer::VertexBuffer;
        if (storage && !index) usage |= QRhiBuffer::StorageBuffer;
        const QRhiBuffer::Type type = lines ? QRhiBuffer::Static : QRhiBuffer::Immutable;
        // A buffer of the same size and kind is written again in place: a tweak that rebuilds
        // the strands then allocates nothing (tens of MB made and freed each frame cost the
        // driver more than the write, and showed as spikes), and straight through the device
        // where it can be (writeBufferNative).
        if (!buffer || buffer->size() != bytes || buffer->type() != type || buffer->usage() != usage) {
            buffer.reset();
            buffer.reset(rhi->newBuffer(type, usage, static_cast<quint32>(bytes)));
            if (!buffer->create()) {
                qWarning("GroomLab: a vertex buffer of %zu bytes could not be made", bytes);
                buffer.reset();
                return;
            }
        } else if (lines && writeBufferNative(rhi, buffer.get(), data, bytes)) {
            return;
        }
        batch->uploadStaticBuffer(buffer.get(), data);
    };
    auto makeFloats = [&](std::unique_ptr<QRhiBuffer>& buffer, const std::vector<float>& v) {
        makeBuffer(buffer, v.data(), v.size() * sizeof(float), false);
    };
    if (item.ownPending) {
        makeFloats(item.positions, item.ownPositions);
        makeFloats(item.normals, item.ownNormals);
        makeFloats(item.offsets, item.ownOffsets);
        makeFloats(item.colors, item.ownColors);
        makeBuffer(item.indices, item.ownIndices.data(), item.ownIndices.size() * sizeof(std::uint32_t), true);
        item.indexCount = static_cast<std::uint32_t>(item.ownIndices.size());
        item.ownPending = false;
    }
    if (item.pending && lines) {
        const StrandLines& l = *item.pending->lines;
        const std::size_t n = l.vertexCount();
        QElapsedTimer timer;
        timer.start();
        // Only some strands moved from the ones the buffers hold (DisplayItem::updated, the same
        // point counts): their vertex ranges alone are written, in runs; the indices, the uvs
        // and every other strand stay. Past a third of the strands (a release after a wide
        // stroke) the whole write wins: thousands of small writes cost more than one.
        const std::size_t strandCount = l.strands ? l.strands->strandCount() : 0;
        const bool partial = partialUploads_ && item.pendingUpdated && !item.pendingUpdated->all && item.holds && item.positions &&
                             item.heldVertices == n && n > 0 && item.pendingUpdated->strands.size() * 3 < strandCount;
        if (partial) {
            const StrandSet& s = *l.strands;
            std::vector<float> xyz, nrm, tan, rgba, uv;
            std::size_t strands = 0;
            for (const auto& [first, end] : strandRuns(item.pendingUpdated->strands, kRunGap)) {
                const std::size_t v0 = s.strandBegin(first), v1 = s.offsets()[end], count = v1 - v0;
                if (count == 0) continue;
                xyz.resize(count * 3);
                l.writePositions(xyz.data(), first, end);
                batch->uploadStaticBuffer(item.positions.get(), static_cast<quint32>(v0 * kStrideXYZ), static_cast<quint32>(count * kStrideXYZ), xyz.data());
                if (l.lit && item.normals && item.tangents) {
                    nrm.resize(count * 3);
                    tan.resize(count * 3);
                    l.writeFrames(nrm.data(), tan.data(), first, end);
                    batch->uploadStaticBuffer(item.normals.get(), static_cast<quint32>(v0 * kStrideXYZ), static_cast<quint32>(count * kStrideXYZ), nrm.data());
                    batch->uploadStaticBuffer(item.tangents.get(), static_cast<quint32>(v0 * kStrideXYZ), static_cast<quint32>(count * kStrideXYZ), tan.data());
                }
                if (l.colors && item.colors) {
                    rgba.resize(count * 4);
                    l.writeColors(rgba.data(), first, end);
                    batch->uploadStaticBuffer(item.colors.get(), static_cast<quint32>(v0 * kStrideRGBA), static_cast<quint32>(count * kStrideRGBA), rgba.data());
                }
                if (item.uvs) {  // (u, width): these strands' widths (the others' stay)
                    uv.resize(count * 2);
                    l.writeUvs(uv.data(), 2, first, end);
                    batch->uploadStaticBuffer(item.uvs.get(), static_cast<quint32>(v0 * kStrideUV), static_cast<quint32>(count * kStrideUV), uv.data());
                }
                strands += end - first;
            }
            item.heldWidths = l.widthsKey();
            lineUploadPartial_ = true;
            lineUploadStrands_ = strands;
        } else {
            // Written whole, into scratch arrays kept between uploads: tens of MB allocated
            // anew each time would be page-faulted in as they are first written, which costs
            // more than the writes themselves (the core recycles its arrays for the same reason).
            Scratch& s = scratch_;
            // The positions are the strands' own array, as they lie (three floats a point): a
            // buffer written through the device takes them from there, no copy at all.
            static_assert(sizeof(Vec3f) == 3 * sizeof(float), "the strands' positions are the vertex stream");
            const bool direct = l.strands && l.strands->positions().size() == n && item.positions && item.positions->size() == n * kStrideXYZ &&
                                item.positions->type() == QRhiBuffer::Static && writeBufferNative(rhi, item.positions.get(), l.strands->positions().data(), n * kStrideXYZ);
            if (!direct) {
                s.positions.resize(n * 3);
                l.writePositions(s.positions.data());
                makeFloats(item.positions, s.positions);
            }
            // The indices depend on the strands' point counts alone, the uvs (u, width) on those and
            // their widths: kept while they hold (a tweak that moves every point writes neither).
            const bool sameLayout = item.holds && item.heldLayout == l.layoutKey() && item.indices && item.indexCount == l.indexCount();
            const bool sameUvs = sameLayout && item.uvs && item.heldWidths == l.widthsKey();
            if (!sameLayout) {
                s.indices.resize(l.indexCount());
                l.writeIndices(s.indices.data());
                makeBuffer(item.indices, s.indices.data(), s.indices.size() * sizeof(std::uint32_t), true);
            }
            if (l.lit && n > 0) {
                s.normals.resize(n * 3);
                s.tangents.resize(n * 3);
                l.writeFrames(s.normals.data(), s.tangents.data());
                makeFloats(item.normals, s.normals);
                makeFloats(item.tangents, s.tangents);
            } else {
                item.normals.reset();
                item.tangents.reset();
            }
            if (!sameUvs && n > 0) {  // u root to tip and the width, two floats a point
                s.uvs.resize(n * 2);
                l.writeUvs(s.uvs.data(), 2);
                makeFloats(item.uvs, s.uvs);
            }
            if (l.colors) {
                s.colors.resize(n * 4);
                l.writeColors(s.colors.data());
                makeFloats(item.colors, s.colors);
            } else {
                item.colors.reset();
            }
            item.indexCount = static_cast<std::uint32_t>(l.indexCount());
            item.holds = item.positions != nullptr;
            item.heldVertices = n;
            item.heldLayout = l.layoutKey();
            item.heldWidths = l.widthsKey();
            lineUploadPartial_ = false;
            lineUploadStrands_ = l.strands ? l.strands->strandCount() : 0;
        }
        lineUploadMs_ = timer.nsecsElapsed() / 1e6;
        ++lineUploads_;
        item.fresh = true;
        item.lines = item.pending->lines;
        item.pending.reset();
        item.pendingUpdated.reset();
    } else if (item.pending) {
        const DisplayGeometry& g = *item.pending;
        makeFloats(item.positions, g.positions);
        makeFloats(item.normals, g.normals);
        makeFloats(item.tangents, g.tangents);
        if (item.kind == Kind::LinesLit && g.uvFloats == 1) {
            // Lit lines in vectors (u alone): the pipeline's (u, width), drawn opaque.
            std::vector<float> uv(g.uvs.size() * 2);
            for (std::size_t v = 0; v < g.uvs.size(); ++v) {
                uv[v * 2] = g.uvs[v];
                uv[v * 2 + 1] = kNoLineWidth;
            }
            makeFloats(item.uvs, uv);
        } else {
            makeFloats(item.uvs, g.uvs);
        }
        makeFloats(item.colors, g.colors);
        makeBuffer(item.indices, g.indices.data(), g.indices.size() * sizeof(std::uint32_t), true);
        item.indexCount = static_cast<std::uint32_t>(g.indices.size());
        item.holds = false;
        item.fresh = true;
        item.pending.reset();
        item.pendingUpdated.reset();
    }
    if (item.pendingColors) {
        const std::vector<float> rgba = item.pendingColors->rgba();
        if (rgba.size() == static_cast<std::size_t>(item.positions ? item.positions->size() / kStrideXYZ * 4 : 0)) {
            makeFloats(item.colors, rgba);
        }
        item.pendingColors.reset();
    }
    auto ensure = [&](std::unique_ptr<QRhiBuffer>& uniforms, std::unique_ptr<QRhiShaderResourceBindings>& bindings) {
        if (!uniforms) {
            uniforms.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(Uniforms)));
            uniforms->create();
        }
        if (!bindings) {
            if (item.kind == Kind::MapTextured) {
                // With the map's texture (none yet: made when it is, prepareMapTexture).
                if (!mapGpu_.texture || !mapGpu_.sampler) return;
                bindings.reset(rhi->newShaderResourceBindings());
                bindings->setBindings({QRhiShaderResourceBinding::uniformBuffer(
                                           0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, uniforms.get()),
                                       QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                                                 mapGpu_.texture.get(), mapGpu_.sampler.get())});
            } else {
                bindings.reset(rhi->newShaderResourceBindings());
                bindings->setBindings({QRhiShaderResourceBinding::uniformBuffer(
                    0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, uniforms.get())});
            }
            bindings->create();
        }
    };
    ensure(item.uniforms, item.bindings);
    ensure(item.uniformsHdr, item.bindingsHdr);
}

void GroomRenderer::writeUniforms(QRhiResourceUpdateBatch* batch, GpuItem& item, const QMatrix4x4& mvp, const QVector3D& eye,
                                  const QSize& size, bool hdr, float pixelScale) {
    QRhiBuffer* uniforms = hdr ? item.uniformsHdr.get() : item.uniforms.get();
    if (uniforms) fillUniforms(batch, uniforms, item, mvp, eye, size, hdr, pixelScale);
}

void GroomRenderer::fillUniforms(QRhiResourceUpdateBatch* batch, QRhiBuffer* uniforms, const GpuItem& item, const QMatrix4x4& mvp,
                                 const QVector3D& eye, const QSize& size, bool hdr, float pixelScale, float lod, float pass,
                                 float density) {
    const HairHighlight highlight = hairHighlight(look_);
    Uniforms u{};
    std::memcpy(u.mvp, mvp.constData(), sizeof(u.mvp));
    u.viewport[0] = static_cast<float>(std::max(size.width(), 1));
    u.viewport[1] = static_cast<float>(std::max(size.height(), 1));
    u.viewport[2] = widthCoverage_ ? pixelScale : 0.0f;  // the strand lines' coverage (0: off, opaque)
    u.viewport[3] = widthScale_;
    u.lines[0] = density >= 0.0f ? density : coverageDensity_;
    u.lines[1] = static_cast<float>(std::max(hdr ? hdr_.sampleCount : main_.sampleCount, 1));
    u.lines[2] = lod;   // the level of detail's class exponent (strand_lod.h; 0: none)
    u.lines[3] = pass;  // 0 every fragment, 1 the opaque ones, 2 the blended ones
    for (int k = 0; k < 4; ++k) u.color[k] = item.color[k];
    u.highlight[0] = highlight.color[0];
    u.highlight[1] = highlight.color[1];
    u.highlight[2] = highlight.color[2];
    u.highlight[3] = highlight.power;
    u.camera[0] = eye.x();
    u.camera[1] = eye.y();
    u.camera[2] = eye.z();
    u.camera[3] = 1.0f;
    u.light[0] = lightDirection_.x();
    u.light[1] = lightDirection_.y();
    u.light[2] = lightDirection_.z();
    u.light[3] = ambient_;
    batch->updateDynamicBuffer(uniforms, 0, sizeof(Uniforms), &u);
}

void GroomRenderer::draw(QRhiCommandBuffer* cb, const GpuItem& item, const PipelineSet& set, bool hdr) {
    const Pipeline* p = set.pipelines[static_cast<std::size_t>(item.kind)].get();
    if (!p || !p->pipeline || !item.positions || !item.indices || item.indexCount == 0) return;
    QRhiShaderResourceBindings* bindings = hdr ? item.bindingsHdr.get() : item.bindings.get();
    if (!bindings) return;
    cb->setGraphicsPipeline(p->pipeline.get());
    cb->setViewport(passViewport_);  // after the pipeline: see passViewport_
    cb->setShaderResources(bindings);
    if (!setInputs(cb, item)) return;
    cb->drawIndexed(item.indexCount);
    ++drawCalls_;
}

bool GroomRenderer::setInputs(QRhiCommandBuffer* cb, const GpuItem& item, QRhiBuffer* indices) {
    QVarLengthArray<QRhiCommandBuffer::VertexInput, 4> inputs;
    inputs.append({item.positions.get(), 0});
    switch (item.kind) {
        case Kind::LinesLit:
            if (!item.normals || !item.tangents || !item.uvs) return false;
            inputs.append({item.normals.get(), 0});
            inputs.append({item.tangents.get(), 0});
            inputs.append({item.uvs.get(), 0});
            break;
        case Kind::StrandLines:
            if (!item.uvs) return false;
            inputs.append({item.uvs.get(), 0});
            break;
        case Kind::StrandLinesColored:
            if (!item.colors || !item.uvs) return false;
            inputs.append({item.colors.get(), 0});
            inputs.append({item.uvs.get(), 0});
            break;
        case Kind::LinesColored:
        case Kind::OverlayColored:
        case Kind::TrianglesColored:
            if (!item.colors) return false;
            inputs.append({item.colors.get(), 0});
            break;
        case Kind::Surface:
            if (!item.normals) return false;
            inputs.append({item.normals.get(), 0});
            break;
        case Kind::Dots:
            if (!item.offsets || !item.colors) return false;
            inputs.append({item.offsets.get(), 0});
            inputs.append({item.colors.get(), 0});
            break;
        case Kind::MapTextured:
            if (!item.uvs) return false;
            inputs.append({item.uvs.get(), 0});
            break;
        default:
            break;
    }
    cb->setVertexInput(0, static_cast<int>(inputs.size()), inputs.constData(), indices ? indices : item.indices.get(), 0,
                       QRhiCommandBuffer::IndexUInt32);
    return true;
}

void GroomRenderer::prepareLod(QRhi* rhi, QRhiResourceUpdateBatch* batch, const OrbitCamera& camera, const QMatrix4x4& mvp,
                               const QSize& size, float pixelScale) {
    LodDraw& d = *lod_;
    d.item = nullptr;
    lodStats_.on = false;
    lodStats_.draws = 0;
    if (!lodOn_ || !widthCoverage_) {
        d.view = StrandLodView{};
        return;
    }
    // The strand lines drawn as lines with width as coverage: the strands', else the colored strands'.
    GpuItem* s = nullptr;
    for (const DisplayItemId id : {DisplayItemId::Strands, DisplayItemId::ColoredStrands}) {
        GpuItem& item = *items_[static_cast<std::size_t>(id)];
        const bool lines = item.kind == Kind::LinesLit || item.kind == Kind::StrandLines || item.kind == Kind::StrandLinesColored;
        if (item.shown && lines && item.lines && item.lines->strands && item.positions && item.indices &&
            item.positions->size() == item.lines->vertexCount() * kStrideXYZ) {
            s = &item;
            break;
        }
    }
    if (!s) return;
    d.lod.setStrands(s->lines->strands, "clumpId");
    const QVector3D eye = camera.eye(), look = camera.target() - eye;
    // (The camera orbits its target: the view keeps what an orbit about it at this distance shows.)
    const QVector3D target = camera.target();
    const StrandLodView view = lodView(Vec3f(eye.x(), eye.y(), eye.z()), Vec3f(look.x(), look.y(), look.z()), pixelScale, false,
                                       d.lod.center(), Vec3f(target.x(), target.y(), target.z()), d.view);
    d.view = view;
    StrandLodSettings settings;
    settings.error = lodError_;
    settings.widthScale = widthScale_;
    settings.density = coverageDensity_;
    // Impostors are the CPU's strands' axes: not while a GPU preview moves the strands.
    settings.impostors = !(gpuPreview_ && (gpuPreview_->pending() || gpuRan_));
    settings.splitBlended = main_.sampleCount <= 1;  // (a multisampled target's lines write masks: nothing blends)
    const StrandLod::Selection& sel = d.lod.select(view, settings);
    lodStats_.level = view.level;
    lodStats_.totalStrands = sel.totalStrands;
    if (!sel.active) return;
    // The indices, written when the selection changed.
    if (sel.key != d.key || !d.indices) {
        const std::vector<std::uint32_t> pairs = d.lod.classSegments(d.firsts);
        const auto bytes = static_cast<quint32>(std::max<std::size_t>(pairs.size(), 2) * sizeof(std::uint32_t));
        if (!d.indices || d.indices->size() != bytes) {
            d.indices.reset(rhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::IndexBuffer, bytes));
            if (!d.indices->create()) {
                d.indices.reset();
                return;
            }
        }
        if (!pairs.empty()) batch->uploadStaticBuffer(d.indices.get(), 0, static_cast<quint32>(pairs.size() * sizeof(std::uint32_t)), pairs.data());
        d.key = sel.key;
        ++lodStats_.rebuilds;
    }
    // Each slot's uniforms: the item's, with its class's exponent and pass.
    const bool split = d.firsts[kLodClassCount + 1] > d.firsts[kLodClassCount];
    for (std::size_t slot = 0; slot < LodDraw::kSlots; ++slot) {
        const bool used = slot < static_cast<std::size_t>(kLodClassCount) ? d.firsts[slot + 1] > d.firsts[slot] : split;
        if (!used) continue;
        if (!d.uniforms[slot]) {
            d.uniforms[slot].reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(Uniforms)));
            d.uniforms[slot]->create();
            d.bindings[slot].reset(rhi->newShaderResourceBindings());
            d.bindings[slot]->setBindings({QRhiShaderResourceBinding::uniformBuffer(
                0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, d.uniforms[slot].get())});
            d.bindings[slot]->create();
        }
        const float lod = slot > 0 && slot < static_cast<std::size_t>(kLodClassCount) ? lodClassExponent(static_cast<int>(slot)) : 0.0f;
        const float pass = slot == LodDraw::kBlendOpaque ? 1.0f : slot == LodDraw::kBlended ? 2.0f : 0.0f;
        fillUniforms(batch, d.uniforms[slot].get(), *s, mvp, eye, size, false, pixelScale, lod, pass);
    }
    // The impostors: their own item, as the strands are drawn, hashed alone (exponent 1) at their own
    // widths (the band's ink at the render's density: density 1).
    const StrandsPtr axes = d.lod.impostors();
    if (axes) {
        const std::shared_ptr<const StrandColors> colors = s->lines->colors ? d.lod.impostorColors(*s->lines->colors) : nullptr;
        const std::uint64_t colorsKey = colors ? colors->key : 0;
        GpuItem& imp = d.impostors;
        if (sel.impostorKey != d.impostorKey || colorsKey != d.impostorColors || !imp.positions) {
            auto geometry = std::make_shared<DisplayGeometry>();
            geometry->lines = std::make_shared<StrandLines>(axes, s->lines->scalp, s->lines->lit, colors);
            imp.pending = geometry;
            imp.pendingUpdated.reset();
            imp.kind = kindOf(*geometry, false);
            upload(rhi, batch, imp);
            d.impostorKey = sel.impostorKey;
            d.impostorColors = colorsKey;
        }
        imp.color = s->color;
        imp.shown = imp.positions != nullptr;
        if (imp.uniforms) fillUniforms(batch, imp.uniforms.get(), imp, mvp, eye, size, false, pixelScale, 1.0f, 0.0f, 1.0f);
    } else {
        d.impostors.shown = false;
    }
    d.item = s;
    lodStats_.on = true;
    lodStats_.drawnStrands = sel.drawnStrands;
    lodStats_.impostors = axes ? axes->strandCount() : 0;
    lodStats_.blendSegments = sel.blendSegments;
}

void GroomRenderer::drawLod(QRhiCommandBuffer* cb, bool blended) {
    LodDraw& d = *lod_;
    if (!d.item || !d.indices) return;
    const GpuItem& item = *d.item;
    const std::size_t k = static_cast<std::size_t>(item.kind);
    const Pipeline* p = (blended ? main_.noDepthWrite[k] : main_.pipelines[k]).get();
    if (!p || !p->pipeline) return;
    cb->setGraphicsPipeline(p->pipeline.get());
    cb->setViewport(passViewport_);
    if (!setInputs(cb, item, d.indices.get())) return;
    auto range = [&](std::size_t slot, std::size_t first, std::size_t end) {
        if (end <= first || !d.bindings[slot]) return;
        cb->setShaderResources(d.bindings[slot].get());
        cb->drawIndexed(static_cast<quint32>(end - first), 1, static_cast<quint32>(first));
        ++drawCalls_;
        ++lodStats_.draws;
    };
    const std::size_t blendFirst = d.firsts[kLodClassCount], blendEnd = d.firsts[kLodClassCount + 1];
    if (blended) {
        range(LodDraw::kBlended, blendFirst, blendEnd);
        return;
    }
    for (std::size_t c = 0; c < static_cast<std::size_t>(kLodClassCount); ++c) range(c, d.firsts[c], d.firsts[c + 1]);
    range(LodDraw::kBlendOpaque, blendFirst, blendEnd);
    if (d.impostors.shown) draw(cb, d.impostors, main_, false);
}

void GroomRenderer::render(QRhi* rhi, QRhiCommandBuffer* cb, QRhiRenderTarget* target, const OrbitCamera& camera) {
    if (rhi != rhi_) {
        // Another device: every object belonged to the old one.
        for (auto& item : items_) {
            const bool hadGeometry = item->indexCount > 0;
            item->release();
            if (hadGeometry && !item->pending) item->shown = false;  // nothing to draw until rebuilt
        }
        scalp_->release();
        scalp_->ownPending = !scalp_->ownIndices.empty();
        dots_->release();
        dots_->ownPending = !dots_->ownIndices.empty();
        tool_->release();
        tool_->shown = tool_->pending != nullptr;
        for (auto& item : propItems_) {
            item->release();
            item->ownPending = !item->ownIndices.empty();
        }
        for (std::size_t i = 0; i < otherItems_.size(); ++i) {
            otherItems_[i]->release();
            otherItems_[i]->pending = otherGeometry_[i];
        }
        main_ = PipelineSet();
        hdr_ = PipelineSet();
        photoGpu_ = PhotoGpu();
        mapGpu_ = MapGpu();
        if (gpuPreview_) gpuPreview_->releaseResources();
    }
    ensurePipelines(rhi, target->renderPassDescriptor(), target->sampleCount(), main_);
    // The reference photo (looking through it): its texture and quad first, in a batch of its own.
    const bool photo = preparePhoto(rhi, cb, target);
    lod_->item = nullptr;  // (the line path sets it: prepareLod)
    lodStats_.on = false;
    rhi_ = rhi;
    const QSize size = target->pixelSize();
    const float aspect = size.height() > 0 ? static_cast<float>(size.width()) / static_cast<float>(size.height()) : 1.0f;
    const QMatrix4x4 mvp = camera.viewProjection(aspect, rhi->clipSpaceCorrMatrix());
    const QVector3D eye = camera.eye();
    // Width as coverage: pixels a scene unit spans at clip w 1 (the perspective's [1][1] is
    // 1 / tan(fov / 2)).
    const float pixelScale = linePixelScale(1.0f / std::tan(0.5f * camera.fovDegrees() * 3.14159265f / 180.0f),
                                            static_cast<float>(std::max(size.height(), 1)));
    // The lookdev path: its targets and pipelines first (the HDR pass's descriptor is known
    // then, and the items' pipelines for it follow), the strands its own to draw.
    // (A photo behind: the line path, over it; the lookdev's backdrop is opaque.)
    bool lookdev = lookdevWanted_ && !(photo && !photo_.front) && LookdevRenderer::supported(rhi) && lookdev_.prepare(rhi, cb, target);
    if (lookdev) ensurePipelines(rhi, lookdev_.hdrPassDescriptor(), lookdev_.hdrSampleCount(), hdr_);
    const auto strandsIndex = static_cast<std::size_t>(DisplayItemId::Strands);
    const QMatrix4x4 hdrMvp = lookdev ? lookdev_.jitter(size) * mvp : mvp;

    QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();
    prepareMapTexture(rhi, batch);
    auto prepare = [&](GpuItem& item, bool skip) {
        item.fresh = false;
        if (!item.shown || skip) return;
        upload(rhi, batch, item);
        writeUniforms(batch, item, mvp, eye, size, false, pixelScale);
        if (lookdev) writeUniforms(batch, item, hdrMvp, eye, size, true, pixelScale);
    };
    // The scalp and the props are the lookdev path's own surfaces: the line path alone draws
    // these copies.
    prepare(*scalp_, !showScalp_ || lookdev);
    for (auto& item : propItems_) prepare(*item, lookdev);
    // (The hair hidden: none of its items; the guides, their dots and the overlays as ever.)
    const bool hair = hairShown_;
    auto hidden = [&](std::size_t i) { return !hair && isHairItem(static_cast<DisplayItemId>(i)); };
    for (std::size_t i = 0; i < items_.size(); ++i) prepare(*items_[i], (lookdev && i == strandsIndex) || hidden(i));
    // The other grooms' strands: lines in both paths. The flat shader encodes its color to sRGB for
    // the window; the lookdev's HDR scene is linear, then exposed and tone mapped
    // (LightingSetup::exposure): their color goes in divided by the exposure and decoded from sRGB,
    // so they read as the same dark gray as in the line path, not lit up.
    const float unexpose = std::pow(2.0f, -lookdev_.lighting().exposure);
    auto fromSrgb = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    for (auto& item : otherItems_) {
        item->fresh = false;
        if (!item->shown || !hair) continue;
        upload(rhi, batch, *item);
        writeUniforms(batch, *item, mvp, eye, size, false, pixelScale);
        if (lookdev) {
            const std::array<float, 4> color = item->color;
            for (int k = 0; k < 3; ++k) item->color[k] = fromSrgb(color[k] * unexpose);
            writeUniforms(batch, *item, hdrMvp, eye, size, true, pixelScale);
            item->color = color;
        }
    }
    prepare(*dots_, false);
    prepare(*tool_, false);
    // The GPU preview's run, once this frame's uploads are recorded (its targets exist) and
    // before anything draws them.
    runGpuPreview(rhi, lookdev);
    // The overlays over everything, in order: the frame's, the guide dots, the tools'.
    auto drawOverlays = [&](QRhiCommandBuffer* passCb) {
        for (std::size_t i = 0; i < items_.size(); ++i) {
            const GpuItem& item = *items_[i];
            if (!hidden(i) && item.shown && isOverlay(item.kind)) draw(passCb, item, main_, false);  // (a Scatter's root dots: hair)
        }
        if (dots_->shown) draw(passCb, *dots_, main_, false);
        if (tool_->shown) draw(passCb, *tool_, main_, false);
    };

    drawCalls_ = 0;
    lookdevActive_ = false;
    if (lookdev) {
        // The batch goes first: the lookdev passes draw these buffers.
        cb->resourceUpdate(batch);
        batch = nullptr;
        auto depthItems = [&](QRhiCommandBuffer* passCb) {
            passViewport_ = lookdev_.passViewport();  // the HDR pass's (a reduced frame's corner)
            for (std::size_t i = 0; i < items_.size(); ++i) {
                const GpuItem& item = *items_[i];
                if (!hidden(i) && item.shown && i != strandsIndex && !isOverlay(item.kind)) draw(passCb, item, hdr_, true);
            }
            for (const auto& item : otherItems_) {
                if (hair && item->shown) draw(passCb, *item, hdr_, true);
            }
        };
        lookdevActive_ = lookdev_.render(rhi, cb, target, camera, hdrMvp, depthItems);
    }
    if (lookdevActive_) {
        cb->beginPass(target, Qt::black, {1.0f, 0});
        passViewport_ = QRhiViewport(0, 0, static_cast<float>(size.width()), static_cast<float>(size.height()));
        cb->setViewport(passViewport_);
        lookdev_.composite(cb, passViewport_);
        ++drawCalls_;
        if (photo) drawPhoto(cb);  // (in front: behind takes the line path)
        drawOverlays(cb);
        cb->endPass();
        return;
    }
    // The line path (or the lookdev could not run: everything, strands included, as lines).
    if (lookdev) {
        // The strands, the scalp and the props were skipped above: upload and draw them now
        // (the GPU preview's run, if any, waits for a frame where the lines are drawn as such).
        batch = rhi->nextResourceUpdateBatch();
        GpuItem& strands = *items_[strandsIndex];
        if (strands.shown && hair) {
            upload(rhi, batch, strands);
            writeUniforms(batch, strands, mvp, eye, size, false, pixelScale);
        }
        if (gpuPreview_ && gpuPreview_->pending()) gpuDeferred_ = true;
        if (showScalp_ && scalp_->shown) {
            upload(rhi, batch, *scalp_);
            writeUniforms(batch, *scalp_, mvp, eye, size, false, pixelScale);
        }
        for (auto& item : propItems_) {
            if (!item->shown) continue;
            upload(rhi, batch, *item);
            writeUniforms(batch, *item, mvp, eye, size, false, pixelScale);
        }
    }
    // The level of detail: the strand lines by class (its uploads with this frame's; none while the
    // hair is hidden).
    if (hair) prepareLod(rhi, batch, camera, mvp, size, pixelScale);
    const QColor bg = background_;
    cb->beginPass(target, QColor::fromRgbF(bg.redF(), bg.greenF(), bg.blueF(), 1.0f), {1.0f, 0}, batch);
    passViewport_ = QRhiViewport(0, 0, static_cast<float>(size.width()), static_cast<float>(size.height()));
    cb->setViewport(passViewport_);
    if (photo && !photo_.front) drawPhoto(cb);  // the backdrop
    if (showScalp_ && scalp_->shown) draw(cb, *scalp_, main_, false);
    for (auto& item : propItems_) {
        if (item->shown) draw(cb, *item, main_, false);
    }
    // The builder's order is the drawing order: hair, instances, then the overlays over them.
    for (std::size_t i = 0; i < items_.size(); ++i) {
        GpuItem& item = *items_[i];
        if (hidden(i) || !item.shown || isOverlay(item.kind)) continue;
        if (&item == lod_->item) {
            drawLod(cb, false);
        } else {
            draw(cb, item, main_, false);
        }
    }
    for (const auto& item : otherItems_) {
        if (hair && item->shown) draw(cb, *item, main_, false);
    }
    if (hair) drawLod(cb, true);  // (the blendable segments' blended fragments, after everything, depth not written)
    if (photo && photo_.front) drawPhoto(cb);  // over the strands and the meshes, under the overlays
    drawOverlays(cb);
    cb->endPass();
}

QImage imageFromReadback(const QRhiReadbackResult& result, bool yUpInFramebuffer) {
    if (result.data.isEmpty() || result.pixelSize.isEmpty()) return {};
    const QImage::Format format = result.format == QRhiTexture::BGRA8 ? QImage::Format_ARGB32 : QImage::Format_RGBA8888;
    const QImage wrapped(reinterpret_cast<const uchar*>(result.data.constData()), result.pixelSize.width(),
                         result.pixelSize.height(), format);
    QImage image = wrapped.copy();
    return yUpInFramebuffer ? image.mirrored() : image;
}

}  // namespace groomlab::app
