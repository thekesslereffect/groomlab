// SPDX-License-Identifier: Apache-2.0
#include "gpu_lines.h"

#include <cstdlib>

// DirectX 11 and OpenGL through WGL: Windows only. Elsewhere the engine has no backend (engine/gpu,
// look_engine_none.cpp): no device is found, so no engine, and the CPU writes the frames.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <GL/gl.h>
#endif

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#ifdef __APPLE__
#include "groomlab/gpu/look_engine.h"  // createMetalBuffer, finishMetalRuns
#endif

#include <maya/MHWGeometry.h>
#include <maya/MViewport2Renderer.h>

namespace groomlab::maya {

namespace {

bool& framesOn() {
    static bool on = [] {
#ifdef _WIN32
        char* v = nullptr;
        std::size_t size = 0;
        const bool off = _dupenv_s(&v, &size, "GROOMLAB_GPU_FRAMES") == 0 && v && std::string(v) == "0";
        std::free(v);
#else
        const char* v = std::getenv("GROOMLAB_GPU_FRAMES");
        const bool off = v && std::string(v) == "0";
#endif
        return !off;
    }();
    return on;
}

// The device Viewport 2.0 draws with: DirectX 11's, or the OpenGL context current on the main
// thread (Maya's viewports share theirs); null without a renderer, or on another API.
struct Device {
    gpu::GpuApi api = gpu::GpuApi::D3D11;
    void* handle = nullptr;
};
Device viewportDevice() {
#ifdef _WIN32
    MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer(false);
    if (!renderer) return {};
    if (renderer->drawAPI() == MHWRender::kDirectX11) return {gpu::GpuApi::D3D11, renderer->GPUDeviceHandle()};
    if (renderer->drawAPIIsOpenGL()) return {gpu::GpuApi::OpenGL, wglGetCurrentContext()};
#endif
    return {};
}

}  // namespace

bool gpuFramesEnabled() { return framesOn(); }
void setGpuFramesEnabled(bool on) { framesOn() = on; }

GpuLineStreams::~GpuLineStreams() {
#ifdef _WIN32
    for (ID3D11Buffer*& b : buffers) {
        if (b) b->Release();
        b = nullptr;
    }
#endif
}

#ifdef _WIN32
namespace {

// The few OpenGL calls the ribbons' buffers take (made with no data, deleted), from the context
// current (Maya's, on the main thread).
struct GlBuffers {
    using GenBuffers = void(APIENTRY*)(GLsizei, GLuint*);
    using DeleteBuffers = void(APIENTRY*)(GLsizei, const GLuint*);
    using BindBuffer = void(APIENTRY*)(GLenum, GLuint);
    using BufferData = void(APIENTRY*)(GLenum, std::ptrdiff_t, const void*, GLenum);
    GenBuffers gen = nullptr;
    DeleteBuffers del = nullptr;
    BindBuffer bind = nullptr;
    BufferData data = nullptr;
    bool load() {
        gen = reinterpret_cast<GenBuffers>(wglGetProcAddress("glGenBuffers"));
        del = reinterpret_cast<DeleteBuffers>(wglGetProcAddress("glDeleteBuffers"));
        bind = reinterpret_cast<BindBuffer>(wglGetProcAddress("glBindBuffer"));
        data = reinterpret_cast<BufferData>(wglGetProcAddress("glBufferData"));
        return gen && del && bind && data;
    }
};
constexpr GLenum kCopyWriteBuffer = 0x8F37, kCopyWriteBinding = 0x8F37, kStaticDraw = 0x88E4;

}  // namespace
#endif

GpuRibbonStreams::~GpuRibbonStreams() {
#ifdef _WIN32
    for (ID3D11Buffer*& b : buffers) {
        if (b) b->Release();
        b = nullptr;
    }
    GlBuffers gl;
    if (wglGetCurrentContext() && gl.load()) {  // (else gone with the context)
        for (unsigned& n : names) {
            const GLuint name = n;
            if (name) gl.del(1, &name);
            n = 0;
        }
    }
#endif
}

std::size_t GpuRibbonStreams::bytes() const {
    // Per point: the lines' 12 + 8, two vertices' 24 (+ 24 + 24 + 16 lit, + 32 colored), 24 of indices, 8 of line indices.
    return std::size_t{capacity} * (12 + 8 + 24 + (lit ? 48 : 0) + (uvs ? 16 : 0) + (colors ? 32 : 0) + 24 + 8);
}

GpuLines::GpuLines() = default;
#ifdef __APPLE__
GpuLines::~GpuLines() { releaseMetal(); }

void GpuLines::releaseMetal() {
    gpu::finishMetalRuns();  // (nothing the GPU still reads)
    gpu::releaseMetalBuffer(metalPoints_);
    gpu::releaseMetalBuffer(metalUvs_);
    gpu::releaseMetalTexture(metalVolume_);
    metalPoints_ = metalUvs_ = metalVolume_ = nullptr;
    metalCapacity_ = 0;
    metalVolumeSize_[0] = metalVolumeSize_[1] = metalVolumeSize_[2] = 0;
    metal_.reset();
}

std::vector<float> GpuLines::densityOnMetal(const float* points, const float* uvs, std::size_t count, const gpu::LineFrames::DensityGrid& grid,
                                            double* gpuMs) {
    if (!gpuFramesEnabled()) {
        densityWhy_ = "off (GROOMLAB_GPU_FRAMES=0, or groomLabGpuPreview -frames off)";
        return {};
    }
    try {
        if (!metal_) metal_ = gpu::makeMetalLineFrames(nullptr);
        gpu::finishMetalRuns();  // (the last grid's splat done with the buffers)
        if (count > metalCapacity_) {  // room for a little more: other counts reuse them
            gpu::releaseMetalBuffer(metalPoints_);
            gpu::releaseMetalBuffer(metalUvs_);
            metalPoints_ = metalUvs_ = nullptr;
            metalCapacity_ = 0;
            const std::size_t room = count + count / 8 + 1;
            metalPoints_ = gpu::createMetalBuffer(nullptr, room * 12);
            metalUvs_ = gpu::createMetalBuffer(nullptr, room * 8);
            metalCapacity_ = room;
        }
        std::memcpy(gpu::metalBufferContents(metalPoints_), points, count * 12);
        std::memcpy(gpu::metalBufferContents(metalUvs_), uvs, count * 8);
        if (metalVolumeSize_[0] != grid.size[0] || metalVolumeSize_[1] != grid.size[1] || metalVolumeSize_[2] != grid.size[2]) {
            gpu::releaseMetalTexture(metalVolume_);
            metalVolume_ = gpu::createMetalVolume(nullptr, grid.size[0], grid.size[1], grid.size[2]);
            for (int k = 0; k < 3; ++k) metalVolumeSize_[k] = grid.size[k];
        }
        metal_->runDensity(metalPoints_, metalUvs_, count, grid, metalVolume_);
        std::vector<float> texels = metal_->readVolume(metalVolume_);
        if (gpuMs) *gpuMs = metal_->gpuMs(true);
        densityWhy_.clear();
        return texels;
    } catch (const std::exception& e) {
        densityWhy_ = e.what();
        releaseMetal();
        return {};
    }
}
#else
GpuLines::~GpuLines() = default;
#endif

gpu::LineFrames* GpuLines::engine() {
    if (!gpuFramesEnabled()) {
        why_ = "off (GROOMLAB_GPU_FRAMES=0, or groomLabGpuPreview -frames off)";
        return nullptr;
    }
    const Device device = viewportDevice();
    if (!device.handle) {
        engine_.reset();
        why_ = "no device the kernels run on (Viewport 2.0 draws with neither DirectX 11 nor OpenGL)";
        return nullptr;
    }
    const bool same = engine_ && engine_->api() == device.api && (device.api == gpu::GpuApi::OpenGL || engine_->device() == device.handle) &&
                      engine_->reachable();
    if (same) {
        why_.clear();
        return engine_.get();
    }
    engine_.reset();
    if (failedOn_ == device.handle) return nullptr;  // (why_ says why)
    try {
        engine_ = device.api == gpu::GpuApi::OpenGL ? gpu::makeGLLineFrames()
                                                    : gpu::makeD3D11LineFrames(static_cast<ID3D11Device*>(device.handle));
        why_.clear();
    } catch (const std::exception& e) {
        failedOn_ = device.handle;
        why_ = e.what();
        return nullptr;
    }
    return engine_.get();
}

void GpuLines::fail(const std::string& why) {
    failedOn_ = viewportDevice().handle;
    engine_.reset();
    why_ = why;
}

std::shared_ptr<GpuLineStreams> GpuLines::makeStreams(unsigned count, std::shared_ptr<GpuLineStreams> held,
                                                      std::unique_ptr<MHWRender::MVertexBuffer>* vertices[4]) {
    auto* device = engine_ && engine_->api() == gpu::GpuApi::D3D11 ? static_cast<ID3D11Device*>(engine_->device()) : nullptr;
    if (!device) throw std::runtime_error("GPU frames: no DirectX 11 device");
    // Another count (a deforming scalp's playback, say) reuses the buffers while they hold it.
    const bool reuse = held && held->device == device && held->capacity >= count && held->capacity / 2 <= count;
    std::shared_ptr<GpuLineStreams> streams = reuse ? std::move(held) : std::make_shared<GpuLineStreams>();
    streams->device = device;
    streams->count = count;
    if (!reuse) streams->capacity = count + count / 8;
    static const MHWRender::MGeometry::Semantic kSemantics[4] = {MHWRender::MGeometry::kPosition, MHWRender::MGeometry::kNormal,
                                                                  MHWRender::MGeometry::kTangent, MHWRender::MGeometry::kTexture};
    // Every buffer made before any MVertexBuffer is given (a buffer the device refuses throws with
    // the item's as they were).
    for (int i = 0; i < 4 && !reuse; ++i) {
        streams->buffers[i] = gpu::createD3D11DisplayBuffer(device, std::size_t{streams->capacity} * 4 * (i < 3 ? 3 : 2));
    }
    for (int i = 0; i < 4; ++i) {
        const int dimension = i < 3 ? 3 : 2;
        auto vertex = std::make_unique<MHWRender::MVertexBuffer>(
            MHWRender::MVertexBufferDescriptor("", kSemantics[i], MHWRender::MGeometry::kFloat, dimension));
        // (Its size in floats: Viewport 2.0 takes the vertex count from it and the dimension.)
        vertex->resourceHandle(streams->buffers[i], count * static_cast<unsigned>(dimension));  // drawn by Viewport 2.0, owned by the streams
        *vertices[i] = std::move(vertex);  // (the one it held, naming older buffers, goes first)
    }
    return streams;
}

std::shared_ptr<GpuRibbonStreams> GpuLines::makeRibbonStreams(unsigned count, bool lit, bool colors, bool uvs, std::shared_ptr<GpuRibbonStreams> held,
                                                              std::unique_ptr<MHWRender::MVertexBuffer>* vertices[7],
                                                              std::unique_ptr<MHWRender::MIndexBuffer>* indices,
                                                              std::unique_ptr<MHWRender::MIndexBuffer>* lineIndices) {
    if (!engine_) throw std::runtime_error("GPU ribbons: no device");
#ifdef _WIN32
    const bool gl = engine_->api() == gpu::GpuApi::OpenGL;
    void* device = engine_->device();
    GlBuffers glCalls;
    if (gl && !glCalls.load()) throw std::runtime_error("GPU ribbons: OpenGL buffer functions missing");
    uvs = uvs || lit;
    const bool reuse = held && held->gl == gl && held->device == device && held->lit == lit && held->colors == colors && held->uvs == uvs &&
                       held->capacity >= count && held->capacity / 2 <= count;
    std::shared_ptr<GpuRibbonStreams> streams = reuse ? std::move(held) : std::make_shared<GpuRibbonStreams>();
    streams->gl = gl;
    streams->device = device;
    streams->count = count;
    streams->lit = lit;
    streams->colors = colors;
    streams->uvs = uvs;
    using MG = MHWRender::MGeometry;
    // Per buffer: the floats a point (or vertex) holds, the semantic, the dimension, the vertices a
    // point (1: the lines', 2: the ribbons'), whether the ribbons have it.
    struct Stream {
        MG::Semantic semantic;
        int dimension, perPoint;
        bool used;
    };
    const Stream kStreams[7] = {{MG::kPosition, 3, 1, true}, {MG::kTexture, 2, 1, true},  {MG::kPosition, 3, 2, true},
                                {MG::kNormal, 3, 2, lit},    {MG::kTangent, 3, 2, lit},  {MG::kTexture, 2, 2, uvs},
                                {MG::kColor, 4, 2, colors}};
    if (!reuse) {
        streams->capacity = count + count / 8;
        const std::size_t room = streams->capacity;
        GLint bound = 0;
        if (gl) glGetIntegerv(kCopyWriteBinding, &bound);
        for (int i = 0; i < GpuRibbonStreams::kBufferCount; ++i) {
            const bool index = i == GpuRibbonStreams::kIndices || i == GpuRibbonStreams::kLineIndices;
            if (!index && !kStreams[i].used) continue;
            const std::size_t bytes = i == GpuRibbonStreams::kIndices ? room * 24
                                    : i == GpuRibbonStreams::kLineIndices ? room * 8
                                    : room * 4 * kStreams[i].dimension * kStreams[i].perPoint;
            if (!gl) {
                auto* d3d = static_cast<ID3D11Device*>(device);
                streams->buffers[i] = index ? gpu::createD3D11IndexBuffer(d3d, bytes) : gpu::createD3D11DisplayBuffer(d3d, bytes);
                continue;
            }
            // (No data: nothing goes up; the kernels write it all.)
            GLuint name = 0;
            glCalls.gen(1, &name);
            glCalls.bind(kCopyWriteBuffer, name);
            glCalls.data(kCopyWriteBuffer, static_cast<std::ptrdiff_t>(bytes), nullptr, kStaticDraw);
            streams->names[i] = name;
        }
        if (gl) glCalls.bind(kCopyWriteBuffer, static_cast<GLuint>(bound));
    }
    // OpenGL: Viewport 2.0 takes a pointer to the buffer's name (the streams keep it).
    auto handle = [&](int i) -> void* { return gl ? static_cast<void*>(&streams->names[i]) : streams->buffers[i]; };
    for (int i = 0; i < 7; ++i) {
        if (!kStreams[i].used) {
            vertices[i]->reset();
            continue;
        }
        auto vertex = std::make_unique<MHWRender::MVertexBuffer>(
            MHWRender::MVertexBufferDescriptor("", kStreams[i].semantic, MG::kFloat, kStreams[i].dimension));
        vertex->resourceHandle(handle(i), count * static_cast<unsigned>(kStreams[i].perPoint * kStreams[i].dimension));
        *vertices[i] = std::move(vertex);
    }
    auto index = std::make_unique<MHWRender::MIndexBuffer>(MG::kUnsignedInt32);
    index->resourceHandle(handle(GpuRibbonStreams::kIndices), count * 6);
    *indices = std::move(index);
    auto lines = std::make_unique<MHWRender::MIndexBuffer>(MG::kUnsignedInt32);
    lines->resourceHandle(handle(GpuRibbonStreams::kLineIndices), count * 2);
    *lineIndices = std::move(lines);
    return streams;
#else
    (void)count, (void)lit, (void)colors, (void)uvs, (void)held, (void)vertices, (void)indices, (void)lineIndices;
    throw std::runtime_error("GPU ribbons: no backend on this platform");
#endif
}

}  // namespace groomlab::maya
