// SPDX-License-Identifier: Apache-2.0
#include "gpu_preview.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>

// DirectX 11 and OpenGL through WGL: Windows only. Elsewhere the engine has no backend (engine/gpu,
// look_engine_none.cpp): mayaDevice() finds none, so every drag and stroke previews on the CPU.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <GL/gl.h>
#endif

#include <maya/M3dView.h>
#include <maya/MArgDatabase.h>
#include <maya/MArgList.h>
#include <maya/MDagPath.h>
#include <maya/MFnDagNode.h>
#include <maya/MGlobal.h>
#include <maya/MHWGeometry.h>
#include <maya/MSelectionList.h>
#include <maya/MStringArray.h>
#include <maya/MViewport2Renderer.h>
#include <nlohmann/json.hpp>

#include "gpu_lines.h"
#include "groom_draw_override.h"
#include "groom_shape.h"
#include "groomlab/display.h"
#include "guarded.h"

namespace groomlab::maya {

namespace {

#ifndef _WIN32
using GLuint = unsigned;  // (an OpenGL buffer's name: none is handed out here)
using UINT = unsigned;
#endif

std::map<const GroomShape*, GpuPreviewRequest>& requests() {
    static std::map<const GroomShape*, GpuPreviewRequest> all;
    return all;
}

std::map<const GroomShape*, GpuLookPreview*>& previews() {
    static std::map<const GroomShape*, GpuLookPreview*> all;
    return all;
}

// GROOMLAB_GPU_PREVIEW_COPY=1: the results copied into the viewport's own buffers (DirectX 11)
// rather than drawn from the preview's: for comparisons.
bool copyIntoViewportBuffers() {
    static const bool on = [] {
#ifdef _WIN32
        char* v = nullptr;
        std::size_t size = 0;
        const bool set = _dupenv_s(&v, &size, "GROOMLAB_GPU_PREVIEW_COPY") == 0 && v && std::string(v) == "1";
        std::free(v);
#else
        const char* v = std::getenv("GROOMLAB_GPU_PREVIEW_COPY");
        const bool set = v && std::string(v) == "1";
#endif
        return set;
    }();
    return on;
}

const char* apiName(MHWRender::DrawAPI api) {
    switch (api) {
        case MHWRender::kDirectX11: return "DirectX11";
        case MHWRender::kOpenGLCoreProfile: return "OpenGLCoreProfile";
        case MHWRender::kOpenGL: return "OpenGL";
        default: return "none";
    }
}

std::string drawApi() {
    MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer(false);
    return renderer ? apiName(renderer->drawAPI()) : "none";
}

// The device Viewport 2.0 draws with: DirectX 11's, or the OpenGL context current on the main
// thread (Maya's viewports share theirs); a null handle without a renderer or a context.
struct MayaDevice {
    gpu::GpuApi api = gpu::GpuApi::D3D11;
    void* handle = nullptr;
};
MayaDevice mayaDevice() {
#ifdef _WIN32
    MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer(false);
    if (!renderer) return {};
    if (renderer->drawAPI() == MHWRender::kDirectX11) return {gpu::GpuApi::D3D11, renderer->GPUDeviceHandle()};
    if (renderer->drawAPIIsOpenGL()) return {gpu::GpuApi::OpenGL, wglGetCurrentContext()};
#endif
    return {};
}

// An OpenGL vertex buffer's name: Viewport 2.0's resource handle there points at it.
void* glBufferOf(MHWRender::MVertexBuffer* buffer) {
    const auto* name = buffer ? static_cast<const GLuint*>(buffer->resourceHandle()) : nullptr;
    return name ? reinterpret_cast<void*>(static_cast<std::uintptr_t>(*name)) : nullptr;
}

// Waits until Maya's device has done everything submitted (timing a tweak to its drawn frame):
// an event query on DirectX 11, glFinish on OpenGL (Maya's context is current on the main thread).
bool waitForDevice() {
#ifndef _WIN32
    return false;  // no device of the preview's
#else
    MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer(false);
    if (renderer && renderer->drawAPIIsOpenGL()) {
        if (!wglGetCurrentContext()) return false;
        glFinish();
        return true;
    }
    const MayaDevice maya = mayaDevice();
    auto* device = static_cast<ID3D11Device*>(maya.handle);
    if (maya.api != gpu::GpuApi::D3D11 || !device) return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    D3D11_QUERY_DESC q{};
    q.Query = D3D11_QUERY_EVENT;
    ID3D11Query* event = nullptr;
    bool ok = SUCCEEDED(device->CreateQuery(&q, &event));
    if (ok) {
        context->End(event);
        context->Flush();
        BOOL done = FALSE;
        while (context->GetData(event, &done, sizeof done, 0) != S_OK || !done) {
        }
        event->Release();
    }
    context->Release();
    return ok;
#endif
}

std::string bufferDescription(ID3D11Buffer* buffer) {
    if (!buffer) return "null";
#ifndef _WIN32
    return "a buffer";
#else
    D3D11_BUFFER_DESC d{};
    buffer->GetDesc(&d);
    char text[200];
    std::snprintf(text, sizeof text, "bytes %u usage %u bind 0x%x cpu 0x%x misc 0x%x", d.ByteWidth, static_cast<unsigned>(d.Usage),
                  d.BindFlags, d.CPUAccessFlags, d.MiscFlags);
    return text;
#endif
}

bool& gpuPreviewOn() {
    static bool on = true;
    return on;
}

}  // namespace

bool waitForGpu() { return waitForDevice(); }

void prepareGpuKernels() {
#ifdef _WIN32
    MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer(false);
    if (renderer && renderer->drawAPIIsOpenGL() && wglGetCurrentContext()) gpu::prepareGLLookKernels();
#endif
}

namespace {

// groomLabGpuPreview -kernels: whether the kernels are ready (OpenGL: started and compiled, on the
// context current on the main thread, as the groom's draw finds them); `wait`: once they are (tests
// before their first drag; five minutes at most).
bool gpuKernelsReady(bool wait) {
#ifndef _WIN32
    (void)wait;
    return true;  // no kernels to wait for: no backend
#else
    MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer(false);
    if (!renderer || !renderer->drawAPIIsOpenGL()) return true;
    if (!wglGetCurrentContext()) return false;
    const auto start = std::chrono::steady_clock::now();
    while (!gpu::prepareGLLookKernels()) {
        if (!wait || std::chrono::steady_clock::now() - start > std::chrono::minutes(5)) return false;
        Sleep(10);
    }
    return true;
#endif
}

}  // namespace

bool gpuPreviewEnabled() { return gpuPreviewOn(); }

void setGpuPreviewEnabled(bool on) {
    gpuPreviewOn() = on;
    if (on) return;
    // Off in the middle of a drag: its values go, the strands are drawn from the CPU again.
    for (auto& [shape, request] : requests()) {
        if (!request.active()) continue;
        request.overrides.clear();
        ++request.version;
    }
}

void loadGpuPreviewPreference() {
#ifdef _WIN32
    char* v = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&v, &size, "GROOMLAB_GPU_PREVIEW") == 0 && v && (std::string(v) == "0" || std::string(v) == "1")) {
        gpuPreviewOn() = std::string(v) == "1";
        std::free(v);
        return;
    }
    std::free(v);
#else
    if (const char* v = std::getenv("GROOMLAB_GPU_PREVIEW"); v && (std::string(v) == "0" || std::string(v) == "1")) {
        gpuPreviewOn() = std::string(v) == "1";
        return;
    }
#endif
    int exists = 0;
    MGlobal::executeCommand("optionVar -exists groomLabGpuPreview", exists);
    if (exists) {
        int value = 1;
        MGlobal::executeCommand("optionVar -query groomLabGpuPreview", value);
        gpuPreviewOn() = value != 0;
    } else {
        gpuPreviewOn() = true;
    }
}

GpuPreviewRequest* gpuPreviewRequest(const GroomShape* shape, bool create) {
    auto& all = requests();
    auto it = all.find(shape);
    if (it != all.end()) return &it->second;
    return create ? &all[shape] : nullptr;
}

GpuLookPreview* gpuLookPreviewFor(const GroomShape* shape) {
    auto it = previews().find(shape);
    return it == previews().end() ? nullptr : it->second;
}

bool gpuStrokeHolds(GroomShape& shape) {
    GpuLookPreview* preview = gpuLookPreviewFor(&shape);
    return preview && gpuPreviewEnabled() && preview->strokeHolds(shape);
}

// The preview's own vertex buffers (DirectX 11): made for compute to write, handed to Viewport 2.0
// as custom resource handles. The MVertexBuffers go before the buffers they name.
struct GpuLookPreview::Display {
    ID3D11Device* device = nullptr;
    unsigned count = 0;
    bool lit = false;
    ID3D11Buffer* buffers[4] = {};  // positions, normals, tangents (float3), the lines' uvs (float2: u, width)
    std::unique_ptr<MHWRender::MVertexBuffer> vertices[4];

    // The uvs as the CPU writes the lines' (u, width: the drawn strands' u and widths), the
    // kernels' Width writing its own widths over them.
    void fillUvs(const StrandsPtr& strands) {
        if (!buffers[3] || !strands || strands->pointCount() != count) return;
#ifdef _WIN32
        std::vector<float> uvs(std::size_t{count} * 2);
        StrandLines(strands, nullptr, false, nullptr).writeUvs(uvs.data(), 2);
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        const D3D11_BOX box{0, 0, 0, static_cast<UINT>(uvs.size() * sizeof(float)), 1, 1};
        context->UpdateSubresource(buffers[3], 0, &box, uvs.data(), 0, 0);
        context->Release();
#endif
    }

    ~Display() { clear(); }
    void clear() {
        for (auto& v : vertices) v.reset();
        for (auto*& b : buffers) {
#ifdef _WIN32
            if (b) b->Release();
#endif
            b = nullptr;
        }
        count = 0;
    }
    std::size_t bytes() const {
        std::size_t n = 0;
        for (int i = 0; i < 4; ++i) n += buffers[i] ? std::size_t{count} * (i < 3 ? 12 : 8) : 0;
        return n;
    }
};

GpuLookPreview::GpuLookPreview(const GroomShape* shape) : shape_(shape) { previews()[shape] = this; }

GpuLookPreview::~GpuLookPreview() {
    auto it = previews().find(shape_);
    if (it != previews().end() && it->second == this) previews().erase(it);
    // The groom's drag goes with its drawing (a new scene, the node deleted): a node made later at
    // the same address starts without one.
    requests().erase(shape_);
    if (engine_) engine_->dropTargets();
    display_.reset();
}

void GpuLookPreview::setDrawable(bool drawable, const std::string& why) {
    drawable_ = drawable;
    notDrawable_ = why;
}

bool GpuLookPreview::wroteItemBuffers() const {
    return engine_ && !display_ && (engine_->api() == gpu::GpuApi::OpenGL ? engine_->wroteInPlace() : copyIntoViewportBuffers());
}

bool GpuLookPreview::wroteItemUvs() const {
    return engine_ && !display_ && engine_->api() == gpu::GpuApi::OpenGL && engine_->wroteUvs();
}

void GpuLookPreview::releaseDisplay() {
    if (!display_) return;
    if (engine_) engine_->dropTargets();
    display_.reset();
}

void* GpuLookPreview::displayBuffer(int stream) const {
    return display_ && stream >= 0 && stream < 4 ? display_->buffers[stream] : nullptr;
}

bool GpuLookPreview::ensureDisplay(unsigned count, bool lit, const StrandsPtr& uvs) {
    auto* device = static_cast<ID3D11Device*>(mayaDevice().handle);
    const bool withUvs = uvs != nullptr;
    if (display_ && display_->device == device && display_->count == count && display_->lit == lit &&
        (display_->buffers[3] != nullptr) == withUvs) {
        return false;
    }
    releaseDisplay();
    auto d = std::make_unique<Display>();
    d->device = device;
    d->count = count;
    d->lit = lit;
    // The lit lines' streams: positions, normals, tangents (no bitangent: the strands carry none).
    static const MHWRender::MGeometry::Semantic kSemantics[3] = {MHWRender::MGeometry::kPosition, MHWRender::MGeometry::kNormal,
                                                                  MHWRender::MGeometry::kTangent};
    for (int i = 0; i < (lit ? 3 : 1); ++i) {
        d->buffers[i] = gpu::createD3D11DisplayBuffer(device, std::size_t{count} * 12);
        d->vertices[i] = std::make_unique<MHWRender::MVertexBuffer>(
            MHWRender::MVertexBufferDescriptor("", kSemantics[i], MHWRender::MGeometry::kFloat, 3));
        d->vertices[i]->resourceHandle(d->buffers[i], count);  // Viewport 2.0 draws it; the preview owns it
    }
    if (withUvs) {  // the lines' uvs: the kernels write the widths, the u written here
        d->buffers[3] = gpu::createD3D11DisplayBuffer(device, std::size_t{count} * 8);
        d->vertices[3] = std::make_unique<MHWRender::MVertexBuffer>(
            MHWRender::MVertexBufferDescriptor("", MHWRender::MGeometry::kTexture, MHWRender::MGeometry::kFloat, 2));
        d->vertices[3]->resourceHandle(d->buffers[3], count);
        d->fillUvs(uvs);
    }
    display_ = std::move(d);
    return true;
}

namespace {

// The node names of a plan's chain (stats).
std::string stageNames(const gpu::LookPlan& p) {
    std::string stages;
    for (const gpu::LookStage& s : p.chain.stages) stages += std::string(stages.empty() ? "" : " ") + gpu::stageName(s.kind);
    return stages;
}

// From the draw: a device the viewport draws with. (From the shape's evaluation the device is not
// asked for: it may run on another thread; a stroke was armed on the draw's, which asked.)
bool hasDevice(std::string* why) {
    if (mayaDevice().handle) return true;
    *why = "no device the viewport draws with (" + drawApi() + ")";
    return false;
}

}  // namespace

gpu::LookPreview::Shown GpuLookPreview::shown(GroomShape& shape, bool inCompute) {
    GroomShape* s = &shape;
    // (Strands handed on at rest, ViewResult::follow: not those drawn, which the scalp carries.)
    auto drawn = [s] {
        const ViewResult& view = s->session().last();
        return view.follow ? StrandsPtr() : view.strands;
    };
    if (inCompute) return drawn;
    return [s, drawn] {
        s->ensureEvaluated();  // evaluates when dirty, or when the frame shown is a cached one
        return drawn();
    };
}

bool GpuLookPreview::plan(GroomShape& shape, const gpu::Overrides& overrides, const gpu::LookStart& start, gpu::LookPlan* out,
                          std::string* why) {
    if (!hasDevice(why) || !core_.plan(shape.session(), shown(shape, false), overrides, start, out, why)) return false;
    stages_ = stageNames(*out);
    return true;
}

bool GpuLookPreview::accepts(GroomShape& shape, const gpu::Overrides& overrides, std::string* why) {
    gpu::LookPlan p;
    if (!plan(shape, overrides, {}, &p, why)) {
        why_ = *why;  // (stats say why the drag is the CPU's)
        return false;
    }
    if (!drawable_) {
        *why = why_ = notDrawable_.empty() ? "the strands are not drawn" : notDrawable_;
        return false;
    }
    // OpenGL: the kernels the drag runs compiled (prepareGpuKernels, as the groom draws); until
    // they are, the CPU previews it (its next values ask again).
    if (mayaDevice().api == gpu::GpuApi::OpenGL && !gpu::glLookKernelsReady(p.run, why)) {
        why_ = *why;
        return false;
    }
    return true;
}

bool GpuLookPreview::ensureEngine() {
    // The engine on Viewport 2.0's device: made at the first drag, and again for another device (a
    // new draw API, a device lost and made again) or an OpenGL context it cannot reach.
    const MayaDevice device = mayaDevice();
    const bool sameDevice = engine_ && engine_->api() == device.api &&
                            (device.api == gpu::GpuApi::OpenGL || engine_->device() == device.handle) && engine_->reachable();
    if (sameDevice) return true;
    releaseDisplay();
    engine_.reset();
    core_.forgetBase();
    try {
        engine_ = device.api == gpu::GpuApi::OpenGL ? gpu::makeGLLookEngine()
                                                    : gpu::makeD3D11LookEngine(static_cast<ID3D11Device*>(device.handle));
    } catch (const std::exception& e) {
        why_ = e.what();
        return false;
    }
    return true;
}

bool GpuLookPreview::runPlan(GroomShape& shape, const gpu::LookPlan& p, const GpuItemBuffers& item, GpuItemBuffers* draw) {
    *draw = GpuItemBuffers{};
    if (!ensureEngine()) return false;
    if (item.vertexCount != p.output->pointCount()) {
        why_ = "the strands drawn are not the graph's output";
        return false;
    }
    const MeshPtr scalp = shape.session().last().scalp;
    const bool lit = item.normals && item.tangents;
    // The lines' widths change with a Width the GPU runs: written into their uvs.
    const bool widths = item.uvs && gpu::runsWidth(p.run.params);
    try {
        // OpenGL's kernels still compiling: the CPU's this time (nothing waits for them).
        if (!engine_->kernelsReady(p.run, &why_)) return false;
        // After a brush stroke's incremental evaluation only its strands go up.
        const auto based = std::chrono::steady_clock::now();
        const ChangesPtr changes = core_.upload(*engine_, p, scalp.get());
        lastRun_.setBaseMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - based).count();
        baseChanges_ = !changes ? "unknown" : changes->all ? "all" : std::to_string(changes->strands.size()) + " strands";
        bounds_ = gpu::lookBounds(p.run.params, p.run.fields, p.run.inputs, *p.base);
        roomyBounds_ = gpu::lookBounds(p.run.params, p.run.fields, p.run.inputs, *p.base, 2.0f);
        gpu::LookTargets targets;
        if (engine_->api() == gpu::GpuApi::OpenGL) {
            // OpenGL: the viewport's own buffers (GPU-only there), bound as shader storage and
            // written in place; the item keeps drawing them.
            // (No bitangent target: the engine's own buffer takes the kernel's, nobody draws it.)
            targets = {item.glPositions ? item.glPositions : glBufferOf(item.positions), lit ? glBufferOf(item.normals) : nullptr,
                       lit ? glBufferOf(item.tangents) : nullptr, nullptr};
            targets.uvs = !widths ? nullptr : item.glUvs ? item.glUvs : glBufferOf(item.uvs);
            if (!targets.positions || (lit && (!targets.normals || !targets.tangents))) {
                why_ = "the viewport's buffers have no OpenGL names";
                return false;
            }
            vp2Buffer_ = "OpenGL buffer " + std::to_string(reinterpret_cast<std::uintptr_t>(targets.positions));
        } else if (copyIntoViewportBuffers()) {
            // Copied into the viewport's own buffers (system-memory staged under DirectX 11: their
            // GPU copy is written, which holds until Maya uploads the staged copy again).
            auto handle = [](MHWRender::MVertexBuffer* b) { return b ? b->resourceHandle() : nullptr; };
            targets = {handle(item.positions), lit ? handle(item.normals) : nullptr, lit ? handle(item.tangents) : nullptr, nullptr};
            const UINT need = static_cast<UINT>(p.output->pointCount() * 12);
            void* const want[3] = {targets.positions, targets.normals, targets.tangents};
            for (int i = 0; i < (lit ? 3 : 1); ++i) {
#ifdef _WIN32
                D3D11_BUFFER_DESC d{};
                if (want[i]) static_cast<ID3D11Buffer*>(want[i])->GetDesc(&d);
                if (!want[i] || d.ByteWidth < need || d.Usage == D3D11_USAGE_IMMUTABLE) {
#else
                if (!want[i] || need == 0) {  // (never here: no engine without a backend)
#endif
                    why_ = "the viewport's buffer cannot take a GPU copy (" + bufferDescription(static_cast<ID3D11Buffer*>(want[i])) + ")";
                    return false;
                }
            }
            vp2Buffer_ = bufferDescription(static_cast<ID3D11Buffer*>(targets.positions));
        } else {
            // The preview's own buffers, drawn by the item for the drag (the uvs too where a Width
            // runs: its widths go there; else the item draws its own).
            const bool remade = ensureDisplay(item.vertexCount, lit, widths ? p.output : nullptr);
            targets = {display_->buffers[0], display_->buffers[1], display_->buffers[2], nullptr};
            targets.uvs = display_->buffers[3];
            *draw = GpuItemBuffers{display_->vertices[0].get(), display_->vertices[1].get(), display_->vertices[2].get(),
                                   item.vertexCount, remade};
            draw->uvs = display_->vertices[3].get();
            vp2Buffer_ = bufferDescription(display_->buffers[0]);
        }
        const auto ran = std::chrono::steady_clock::now();
        engine_->run(p.run, lit, targets);
        lastRun_.runMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ran).count();
        lastRun_.prepareMs = engine_->runUploadMs();
        for (int i = 0; i < 3; ++i) lastRun_.parts[i] = engine_->prepareParts()[i];
        lastRun_.runUploadMB = engine_->runUploadMegabytes();
        lastRun_.baseUploaded = engine_->uploads();
    } catch (const std::exception& e) {
        why_ = e.what();
        *draw = GpuItemBuffers{};
        return false;
    }
    ++runs_;
    why_.clear();
    return true;
}

bool GpuLookPreview::apply(GroomShape& shape, const GpuPreviewRequest& request, const GpuItemBuffers& item, GpuItemBuffers* draw) {
    const auto start = std::chrono::steady_clock::now();
    *draw = GpuItemBuffers{};
    if (!item.positions) {
        why_ = "the strands are not drawn as lines";
        return false;
    }
    gpu::LookPlan p;
    if (!plan(shape, request.overrides, {}, &p, &why_) || !runPlan(shape, p, item, draw)) return false;
    cpuMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return true;
}

bool GpuLookPreview::armStroke(GroomShape& shape) {
    if (core_.strokeArmed()) return true;
    if (core_.strokeRefused() || !gpuPreviewEnabled()) return false;
    // A stroke of the guides alone (the hand guides, or a Sculpt node's; else core_ says it is
    // not), drawn through a device.
    if (gpu::strokeShapesGuides(shape.session()) && !hasDevice(&strokeWhy_)) {
        core_.refuseStroke();
        return false;
    }
    if (!core_.armStroke(shape.session(), shown(shape, false), &strokeWhy_)) return false;
    if (!drawable_) {
        strokeWhy_ = notDrawable_.empty() ? "the strands are not drawn" : notDrawable_;
        core_.refuseStroke();
        return false;
    }
    // OpenGL: the kernels the stroke runs compiled (prepareGpuKernels, as the groom draws); until
    // they are, the stroke is the CPU's whole (its evaluation would stop at the GPU's node).
    if (mayaDevice().api == gpu::GpuApi::OpenGL && !gpu::glLookKernelsReady(core_.strokePlan()->run, &strokeWhy_)) {
        core_.refuseStroke();
        return false;
    }
    strokeWhy_.clear();
    stages_ = stageNames(*core_.strokePlan());
    shape.setGpuStroke(core_.stopAt());
    return true;
}

void GpuLookPreview::endStroke(GroomShape& shape) {
    core_.endStroke();
    shape.setGpuStroke(std::string());
}

bool GpuLookPreview::strokeHolds(GroomShape& shape) {
    // From the shape's evaluation: the CPU's from here when it does not (this dab, and the rest of
    // the stroke).
    if (!core_.strokeArmed() || !core_.strokeHolds(shape.session(), shown(shape, true), &strokeWhy_)) return false;
    stages_ = stageNames(*core_.strokePlan());
    ++strokeDabs_;
    return true;
}

bool GpuLookPreview::applyStroke(GroomShape& shape, const GpuItemBuffers& item, GpuItemBuffers* draw) {
    const auto start = std::chrono::steady_clock::now();
    *draw = GpuItemBuffers{};
    if (!item.positions) {
        why_ = "the strands are not drawn as lines";
        return false;
    }
    // The plan the dab's evaluation made, while it is the document's as it is (else made again).
    const GraphDocument* doc = shape.session().viewDocument();
    if (!core_.strokePlan() || core_.strokePlan()->doc != doc) {
        if (!hasDevice(&why_) || !core_.replanStroke(shape.session(), shown(shape, false), &why_)) return false;
        stages_ = stageNames(*core_.strokePlan());
    }
    if (!runPlan(shape, *core_.strokePlan(), item, draw)) return false;
    ++strokeRuns_;
    cpuMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    lastRun_.cpuMs = cpuMs_;
    if (lastRun_.cpuMs > slowestStroke_.cpuMs) slowestStroke_ = lastRun_;
    return true;
}

std::string GpuLookPreview::statsJson(bool wait) {
    nlohmann::json j;
    j["api"] = drawApi();
    j["runs"] = runs_;
    j["cpuMs"] = cpuMs_;
    j["why"] = why_;
    j["stages"] = stages_;
    j["vp2Buffer"] = vp2Buffer_;
    j["displayMB"] = display_ ? static_cast<double>(display_->bytes()) / 1e6 : 0.0;
    j["stroke"] = {{"armed", core_.strokeArmed()}, {"dabs", strokeDabs_}, {"runs", strokeRuns_}, {"why", strokeWhy_},
                   {"slowest", {{"cpuMs", slowestStroke_.cpuMs}, {"setBaseMs", slowestStroke_.setBaseMs}, {"runMs", slowestStroke_.runMs},
                                  {"prepareMs", slowestStroke_.prepareMs}, {"runUploadMB", slowestStroke_.runUploadMB},
                                  {"prepareParts", {slowestStroke_.parts[0], slowestStroke_.parts[1], slowestStroke_.parts[2]}}}},
                   {"last", {{"cpuMs", lastRun_.cpuMs}, {"setBaseMs", lastRun_.setBaseMs}, {"runMs", lastRun_.runMs},
                             {"prepareMs", lastRun_.prepareMs}, {"runUploadMB", lastRun_.runUploadMB},
                             {"prepareParts", {lastRun_.parts[0], lastRun_.parts[1], lastRun_.parts[2]}}}}};
    if (bounds_.valid) {
        j["bounds"] = {bounds_.lo.x, bounds_.lo.y, bounds_.lo.z, bounds_.hi.x, bounds_.hi.y, bounds_.hi.z};
    }
    if (display_ && display_->buffers[0]) {
#ifdef _WIN32
        // References to the preview's buffer: the preview's own and its views' (Viewport 2.0 takes
        // none: it draws the handle it was given).
        display_->buffers[0]->AddRef();
        j["displayRefs"] = display_->buffers[0]->Release();
#endif
        j["displayVertices"] = display_->vertices[0] ? display_->vertices[0]->vertexCount() : 0u;
        j["displayCustomHandle"] = display_->vertices[0] && display_->vertices[0]->hasCustomResourceHandle();
    }
    if (engine_) {
        j["inPlace"] = engine_->wroteInPlace();
#ifdef _WIN32
        if (engine_->api() == gpu::GpuApi::OpenGL) j["sameContext"] = engine_->device() == wglGetCurrentContext();
#endif
        const gpu::GpuTimes t = engine_->times(wait);
        if (t.valid) {
            j["gpu"] = {{"interpolateMs", t.interpolateMs}, {"lengthMs", t.lengthMs}, {"clumpMs", t.clumpMs}, {"noiseMs", t.noiseMs},
                        {"curlMs", t.curlMs}, {"widthMs", t.widthMs}, {"clearanceMs", t.clearanceMs}, {"partMs", t.partMs},
                        {"gravityMs", t.gravityMs}, {"flyawayMs", t.flyawayMs}, {"collideMs", t.collideMs},
                        {"textureMs", t.textureMs}, {"linesMs", t.linesMs}, {"copyMs", t.copyMs}, {"totalMs", t.totalMs}};
        }
        const gpu::LookEngine::WalkStats& walk = engine_->lastWalk();
        j["walk"] = {{"partial", walk.partial}, {"changedGuides", walk.changedGuides}, {"guides", walk.guides}};
        j["uploadMs"] = engine_->uploadMs();
        j["uploadMB"] = engine_->uploadMegabytes();
        j["uploads"] = engine_->uploads();
        j["uploadPartial"] = engine_->lastUploadPartial();
        j["baseChanges"] = baseChanges_;  // what the graph said of the base at the last upload
        j["uploadStrands"] = engine_->lastUploadStrands();
        j["runUploadMB"] = engine_->runUploadMegabytes();
        j["runUploadMs"] = engine_->runUploadMs();
        j["fieldsMB"] = engine_->fieldMegabytes();
        j["fieldsReadMs"] = core_.fieldsReadMs();
        j["inputsReadMs"] = core_.inputs().lastReadMs();
        j["guidesMade"] = core_.inputs().lastGuidesMade();
        j["videoMB"] = static_cast<double>(engine_->videoBytes()) / 1e6;
    }
    return j.dump();
}

std::string GpuLookPreview::checkJson(GroomShape& shape, const std::string& from) {
    gpu::LookPlan p;
    nlohmann::json j;
    bool planned = false;
    if (!from.empty()) {
        planned = plan(shape, {}, {gpu::LookStart::From::Node, from}, &p, &why_);
    } else {
        // From the first node of the chain the GPU can run now (Interpolate without its binding
        // kept, say, is left to the CPU's cache).
        const GraphDocument* doc = shape.session().viewDocument();
        gpu::LookChain full;
        std::string why;
        if (doc && gpu::findLookChain(*doc, &full, &why)) {
            for (std::size_t k = 0; k < full.stages.size() && !planned; ++k) {
                planned = plan(shape, {}, {gpu::LookStart::From::Node, doc->nameOf(full.stages[k].node)}, &p, &why_);
            }
        } else {
            why_ = why;
        }
    }
    if (!planned || !ensureEngine()) {
        j["why"] = why_;
        return j.dump();
    }
    const std::size_t n = p.output->pointCount();
    if (n == 0) {
        j["why"] = "no points";
        return j.dump();
    }
    try {
        core_.upload(*engine_, p, shape.session().last().scalp.get(), false);  // all of it
        engine_->run(p.run, true);  // into the engine's own buffers (the viewport's are left as drawn)
    } catch (const std::exception& e) {
        j["why"] = e.what();
        return j.dump();
    }
    // The CPU's: the output strands and the lit lines' frames the viewport writes from them (the
    // normals and tangents: the strands carry no bitangent).
    const StrandLines lines(p.output, shape.evaluatedScalp(), true, nullptr);
    std::vector<Vec3f> cpu[2] = {std::vector<Vec3f>(n), std::vector<Vec3f>(n)};
    lines.writeFrames(&cpu[0][0].x, &cpu[1][0].x);
    std::vector<Vec3f> gpuFrames[2];
    engine_->readFrames(&gpuFrames[0], &gpuFrames[1], nullptr);
    auto put = [&](const char* name, const gpu::PointError& e) { j[name] = {{"max", e.max}, {"mean", e.mean}, {"count", e.count}}; };
    put("positions", gpu::compareVectors(engine_->readPositions(), p.output->positions()));
    put("normals", gpu::compareVectors(gpuFrames[0], cpu[0]));
    put("tangents", gpu::compareVectors(gpuFrames[1], cpu[1]));
    if (gpu::runsWidth(p.run.params) && p.output->widths().size() == n) {
        const std::vector<float> widths = engine_->readWidths();
        double max = 0, sum = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const double d = std::abs(static_cast<double>(widths[i]) - p.output->widths()[i]);
            max = std::max(max, d);
            sum += d;
        }
        j["widths"] = {{"max", max}, {"mean", n ? sum / static_cast<double>(n) : 0.0}};
    }
    j["stages"] = stages_;
    j["strands"] = p.output->strandCount();
    j["points"] = n;
    return j.dump();
}

// ---- groomLabGpuPreview -------------------------------------------------------------------------

const char* GpuPreviewCmd::kName = "groomLabGpuPreview";

void* GpuPreviewCmd::creator() { return new GpuPreviewCmd(); }

MSyntax GpuPreviewCmd::newSyntax() {
    MSyntax s;
    s.addFlag("-n", "-node", MSyntax::kString);
    s.addFlag("-p", "-param", MSyntax::kString);
    s.addFlag("-v", "-value", MSyntax::kDouble);
    s.addFlag("-r", "-ramp", MSyntax::kString);  // a profile's keys, as the graph's JSON holds them
    s.addFlag("-vc", "-vector", MSyntax::kDouble, MSyntax::kDouble, MSyntax::kDouble);  // a vec3 param (Curl's weightDirection)
    s.addFlag("-end", "-endDrag");
    s.addFlag("-st", "-stats");
    s.addFlag("-w", "-wait");
    s.addFlag("-c", "-check");
    s.addFlag("-fr", "-from", MSyntax::kString);
    s.addFlag("-sy", "-sync");
    s.addFlag("-api", "-drawApi");
    s.addFlag("-en", "-enable", MSyntax::kBoolean);
    s.addFlag("-ie", "-isEnabled");
    s.addFlag("-frm", "-frames", MSyntax::kBoolean);  // the lit lines' frames from the GPU (gpu_lines.h)
    s.addFlag("-cf", "-checkFrames");
    s.addFlag("-k", "-kernels");  // whether OpenGL's kernels are ready (-wait: once they are)
    s.setObjectType(MSyntax::kStringObjects, 0, 1);
    return s;
}

MStatus GpuPreviewCmd::doIt(const MArgList& args) {
    return guarded("GpuPreviewCmd::doIt", [&]() -> MStatus {
        MStatus parsed;
        MArgDatabase db(syntax(), args, &parsed);
        if (!parsed) return MS::kFailure;
        if (db.isFlagSet("-enable")) {  // the session's switch (the menu stores the preference)
            bool on = true;
            db.getFlagArgument("-enable", 0, on);
            setGpuPreviewEnabled(on);
            setResult(on);
            return MS::kSuccess;
        }
        if (db.isFlagSet("-isEnabled")) {
            setResult(gpuPreviewEnabled());
            return MS::kSuccess;
        }
        if (db.isFlagSet("-frames")) {  // GPU frames on or off for the session (every groom's strands written again)
            bool on = true;
            db.getFlagArgument("-frames", 0, on);
            if (on != gpuFramesEnabled()) {
                setGpuFramesEnabled(on);
                gpuFramesSwitched();
                M3dView::scheduleRefreshAllViews();
            }
            setResult(on);
            return MS::kSuccess;
        }
        if (db.isFlagSet("-drawApi")) {
            setResult(MString(drawApi().c_str()));
            return MS::kSuccess;
        }
        if (db.isFlagSet("-kernels")) {  // no groom needed
            setResult(gpuKernelsReady(db.isFlagSet("-wait")));
            return MS::kSuccess;
        }
        if (db.isFlagSet("-sync")) {  // no groom needed
            setResult(waitForDevice());
            return MS::kSuccess;
        }
        MStringArray names;
        MSelectionList sel;
        MDagPath path;
        if (db.getObjects(names) != MS::kSuccess || names.length() < 1 || sel.add(names[0]) != MS::kSuccess ||
            sel.getDagPath(0, path) != MS::kSuccess) {
            MGlobal::displayError("groomLabGpuPreview: name a groomLabShape");
            return MS::kFailure;
        }
        path.extendToShape();
        auto* shape = dynamic_cast<GroomShape*>(MFnDagNode(path).userNode());
        if (!shape) {
            MGlobal::displayError("groomLabGpuPreview: " + names[0] + " is not a groomLabShape");
            return MS::kFailure;
        }
        GpuLookPreview* preview = gpuLookPreviewFor(shape);
        if (db.isFlagSet("-stats")) {
            setResult(MString(preview ? preview->statsJson(db.isFlagSet("-wait")).c_str() : "{}"));
            return MS::kSuccess;
        }
        if (db.isFlagSet("-checkFrames")) {
            setResult(MString(gpuFramesCheckJson(shape).c_str()));
            return MS::kSuccess;
        }
        if (db.isFlagSet("-check")) {
            MString from;
            if (db.isFlagSet("-from")) db.getFlagArgument("-from", 0, from);
            setResult(MString(preview ? preview->checkJson(*shape, from.asChar()).c_str() : "{\"why\": \"not drawn\"}"));
            return MS::kSuccess;
        }
        if (db.isFlagSet("-endDrag")) {
            if (GpuPreviewRequest* request = gpuPreviewRequest(shape)) {
                if (request->active()) {
                    // The viewport draws the drag's last values from the GPU: the commit that follows
                    // (set_param) may evaluate off the UI thread, the view keeping this frame meanwhile.
                    if (gpuPreviewDrawn(shape, request->version)) shape->expectCommit(GroomShape::CommitKind::Graph);
                    request->overrides.clear();
                    ++request->version;
                    MHWRender::MRenderer::setGeometryDrawDirty(shape->thisMObject());
                    M3dView::scheduleRefreshAllViews();
                }
            }
            return MS::kSuccess;
        }
        MString node, param, rampText;
        double number = 0;
        double xyz[3] = {0.0, 0.0, 0.0};
        const bool ramp = db.isFlagSet("-ramp");
        const bool vector = !ramp && db.isFlagSet("-vector");
        auto readValue = [&] {
            if (ramp) return db.getFlagArgument("-ramp", 0, rampText) == MS::kSuccess;
            if (!vector) return db.getFlagArgument("-value", 0, number) == MS::kSuccess;
            for (unsigned k = 0; k < 3; ++k) {
                if (db.getFlagArgument("-vector", k, xyz[k]) != MS::kSuccess) return false;
            }
            return true;
        };
        if (db.getFlagArgument("-node", 0, node) != MS::kSuccess || db.getFlagArgument("-param", 0, param) != MS::kSuccess ||
            !readValue()) {
            MGlobal::displayError("groomLabGpuPreview: -node, -param and -value (or -ramp, -vector), or -end, -stats, -check, -sync");
            return MS::kFailure;
        }
        gpu::OverrideValue value = number;
        if (vector) value = Vec3f(static_cast<float>(xyz[0]), static_cast<float>(xyz[1]), static_cast<float>(xyz[2]));
        bool readable = true;
        if (ramp) {
            try {
                value = rampFromJson(rampText.asChar());
            } catch (const std::exception&) {
                readable = false;  // not a ramp's keys: the CPU's
            }
        }
        // Whether the GPU takes this drag: a previewable param of a node the GPU runs, on the groom
        // as it is, drawn through a device it runs on. False: the caller previews on the CPU (and
        // the drag's earlier values here are dropped, so the CPU's show).
        if (!preview || !gpuPreviewEnabled()) {  // not drawn, or the preview is off: the CPU previews
            setResult(false);
            return MS::kSuccess;
        }
        GpuPreviewRequest* request = gpuPreviewRequest(shape, true);
        gpu::Overrides overrides = request->overrides;
        overrides[node.asChar()][param.asChar()] = value;
        std::string why;
        const GraphDocument* doc = shape->session().viewDocument();
        bool takes = false;
        if (doc && readable) {
            // (A compound's exposed param: the inner node's param it sets, the compound held inline.)
            const auto [inner, innerParam] = shape->session().flatCompounds().param(node.asChar(), param.asChar());
            const auto it = doc->nodesByName.find(inner);
            takes = it != doc->nodesByName.end() && gpu::previewableParam(doc->graph.node(it->second).typeName(), innerParam) &&
                    preview->accepts(*shape, overrides, &why);
        }
        if (takes) {
            request->overrides = std::move(overrides);
            ++request->version;
        } else if (request->active()) {
            request->overrides.clear();
            ++request->version;
        }
        // Nothing of the scene changed (the graph is left alone during the drag), so nothing would
        // redraw the views: the groom's drawing is marked dirty and a redraw scheduled (Maya's idle
        // work), so every step of a drag draws its value, whoever sends it.
        MHWRender::MRenderer::setGeometryDrawDirty(shape->thisMObject());
        M3dView::scheduleRefreshAllViews();
        setResult(takes);
        return MS::kSuccess;
    });
}

}  // namespace groomlab::maya
