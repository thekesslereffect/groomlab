// SPDX-License-Identifier: Apache-2.0
// The lit strand lines' frames on the GPU (engine/gpu line_frames.h) against the CPU's
// (display.h StrandLines::writeFrames, writeUvs): the points written into a vertex buffer, the
// frames and widths written by the kernels, read back and compared, on every backend (DirectX 11:
// a hardware device; OpenGL: a hidden window's 4.5 context). A backend without a device skips.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>
#include <d3d11.h>
#endif

#include "../core/test_helpers.h"
#include "groomlab/display.h"
#include "groomlab/follow_scalp.h"
#include "groomlab/gpu/line_frames.h"
#ifdef GROOMLAB_HAS_METAL
#include "groomlab/gpu/look_engine.h"  // createMetalBuffer
#endif

namespace groomlab {
namespace {

// A device and its vertex buffers.
class Device {
public:
    virtual ~Device() = default;
    virtual bool available() const = 0;
    virtual std::unique_ptr<gpu::LineFrames> frames() = 0;
    virtual void* buffer(std::size_t bytes) = 0;  // a vertex buffer the kernels may read and write
    virtual void release(void* buffer) = 0;
    // A 3D texture of x * y * z texels of two floats (runDensity's target), read back and released.
    virtual void* volume(unsigned x, unsigned y, unsigned z) = 0;
    virtual std::vector<float> readVolume(void* volume, unsigned x, unsigned y, unsigned z) = 0;
    virtual void releaseVolume(void* volume) = 0;
};

#ifdef _WIN32
class D3D11Device final : public Device {
public:
    D3D11Device() {
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device_, nullptr, nullptr);
    }
    ~D3D11Device() override {
        if (device_) device_->Release();
    }
    bool available() const override { return device_ != nullptr; }
    std::unique_ptr<gpu::LineFrames> frames() override { return gpu::makeD3D11LineFrames(device_); }
    void* buffer(std::size_t bytes) override { return gpu::createD3D11DisplayBuffer(device_, bytes); }
    void release(void* buffer) override { static_cast<ID3D11Buffer*>(buffer)->Release(); }
    void* volume(unsigned x, unsigned y, unsigned z) override {
        D3D11_TEXTURE3D_DESC t{};
        t.Width = x;
        t.Height = y;
        t.Depth = z;
        t.MipLevels = 1;
        t.Format = DXGI_FORMAT_R32G32_FLOAT;
        t.Usage = D3D11_USAGE_DEFAULT;
        t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture3D* texture = nullptr;
        device_->CreateTexture3D(&t, nullptr, &texture);
        return texture;
    }
    std::vector<float> readVolume(void* volume, unsigned x, unsigned y, unsigned z) override {
        D3D11_TEXTURE3D_DESC t{};
        static_cast<ID3D11Texture3D*>(volume)->GetDesc(&t);
        t.Usage = D3D11_USAGE_STAGING;
        t.BindFlags = 0;
        t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture3D* staging = nullptr;
        device_->CreateTexture3D(&t, nullptr, &staging);
        ID3D11DeviceContext* context = nullptr;
        device_->GetImmediateContext(&context);
        context->CopyResource(staging, static_cast<ID3D11Texture3D*>(volume));
        D3D11_MAPPED_SUBRESOURCE mapped{};
        std::vector<float> out(std::size_t{x} * y * z * 2);
        if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            for (unsigned k = 0; k < z; ++k) {
                for (unsigned j = 0; j < y; ++j) {
                    const auto* row = static_cast<const unsigned char*>(mapped.pData) + k * mapped.DepthPitch + j * mapped.RowPitch;
                    std::memcpy(out.data() + (std::size_t{k} * y + j) * x * 2, row, std::size_t{x} * 8);
                }
            }
            context->Unmap(staging, 0);
        }
        staging->Release();
        context->Release();
        return out;
    }
    void releaseVolume(void* volume) override { static_cast<ID3D11Texture3D*>(volume)->Release(); }

private:
    ID3D11Device* device_ = nullptr;
};

class GLDevice final : public Device {
public:
    GLDevice() {
        WNDCLASSA wc{};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "GroomLabLineFramesTest";
        RegisterClassA(&wc);
        window_ = CreateWindowA("GroomLabLineFramesTest", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
        if (!window_) return;
        dc_ = GetDC(window_);
        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof pfd;
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        if (!SetPixelFormat(dc_, ChoosePixelFormat(dc_, &pfd), &pfd)) return;
        HGLRC legacy = wglCreateContext(dc_);
        if (!legacy || !wglMakeCurrent(dc_, legacy)) return;
        using CreateAttribs = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
        auto create = reinterpret_cast<CreateAttribs>(wglGetProcAddress("wglCreateContextAttribsARB"));
        const int attribs[] = {0x2091, 4, 0x2092, 5, 0x9126, 0x00000001, 0};  // 4.5, core profile
        context_ = create ? create(dc_, nullptr, attribs) : nullptr;
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(legacy);
        if (!context_ || !wglMakeCurrent(dc_, context_)) {
            context_ = nullptr;
            return;
        }
        genBuffers_ = reinterpret_cast<decltype(genBuffers_)>(wglGetProcAddress("glGenBuffers"));
        deleteBuffers_ = reinterpret_cast<decltype(deleteBuffers_)>(wglGetProcAddress("glDeleteBuffers"));
        bindBuffer_ = reinterpret_cast<decltype(bindBuffer_)>(wglGetProcAddress("glBindBuffer"));
        bufferData_ = reinterpret_cast<decltype(bufferData_)>(wglGetProcAddress("glBufferData"));
        texImage3D_ = reinterpret_cast<decltype(texImage3D_)>(wglGetProcAddress("glTexImage3D"));
    }
    ~GLDevice() override {
        if (context_) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(context_);
        }
        if (dc_) ReleaseDC(window_, dc_);
        if (window_) DestroyWindow(window_);
    }
    bool available() const override { return context_ != nullptr; }
    std::unique_ptr<gpu::LineFrames> frames() override { return gpu::makeGLLineFrames(); }
    void* buffer(std::size_t bytes) override {  // as Maya's own vertex buffers are made
        GLuint b = 0;
        genBuffers_(1, &b);
        bindBuffer_(0x8892, b);  // GL_ARRAY_BUFFER
        bufferData_(0x8892, static_cast<std::ptrdiff_t>(bytes), nullptr, 0x88E4);  // GL_STATIC_DRAW
        bindBuffer_(0x8892, 0);
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(b));
    }
    void release(void* buffer) override {
        const auto b = static_cast<GLuint>(reinterpret_cast<std::uintptr_t>(buffer));
        if (b) deleteBuffers_(1, &b);
    }
    void* volume(unsigned x, unsigned y, unsigned z) override {  // GL_RG32F, as Maya's MTexture
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(0x806F, t);  // GL_TEXTURE_3D
        texImage3D_(0x806F, 0, 0x8230, static_cast<GLsizei>(x), static_cast<GLsizei>(y), static_cast<GLsizei>(z), 0, 0x8227, GL_FLOAT, nullptr);
        glTexParameteri(0x806F, GL_TEXTURE_MIN_FILTER, GL_LINEAR);  // (one level: complete, as an image unit needs)
        glBindTexture(0x806F, 0);
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(t));
    }
    std::vector<float> readVolume(void* volume, unsigned x, unsigned y, unsigned z) override {
        std::vector<float> out(std::size_t{x} * y * z * 2);
        glBindTexture(0x806F, static_cast<GLuint>(reinterpret_cast<std::uintptr_t>(volume)));
        glGetTexImage(0x806F, 0, 0x8227, GL_FLOAT, out.data());  // GL_RG
        glBindTexture(0x806F, 0);
        return out;
    }
    void releaseVolume(void* volume) override {
        const auto t = static_cast<GLuint>(reinterpret_cast<std::uintptr_t>(volume));
        glDeleteTextures(1, &t);
    }

private:
    HWND window_ = nullptr;
    HDC dc_ = nullptr;
    HGLRC context_ = nullptr;
    void(APIENTRY* genBuffers_)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* deleteBuffers_)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* bindBuffer_)(GLenum, GLuint) = nullptr;
    void(APIENTRY* bufferData_)(GLenum, std::ptrdiff_t, const void*, GLenum) = nullptr;
    void(APIENTRY* texImage3D_)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
};

#endif  // _WIN32

#ifdef GROOMLAB_HAS_METAL
// Metal (macOS): the system's device; buffers in memory the CPU shares.
class MetalDevice final : public Device {
public:
    bool available() const override { return gpu::metalSystemDevice() != nullptr; }
    std::unique_ptr<gpu::LineFrames> frames() override { return gpu::makeMetalLineFrames(gpu::metalSystemDevice()); }
    void* buffer(std::size_t bytes) override {
        void* b = gpu::createMetalBuffer(gpu::metalSystemDevice(), bytes);
        std::memset(gpu::metalBufferContents(b), 0, bytes);
        return b;
    }
    void release(void* buffer) override { gpu::releaseMetalBuffer(buffer); }
    void* volume(unsigned x, unsigned y, unsigned z) override { return gpu::createMetalVolume(gpu::metalSystemDevice(), x, y, z); }
    std::vector<float> readVolume(void* volume, unsigned, unsigned, unsigned) override {
        return gpu::makeMetalLineFrames(gpu::metalSystemDevice())->readVolume(volume);  // (on the device's one stream)
    }
    void releaseVolume(void* volume) override { gpu::releaseMetalTexture(volume); }
};
#endif

std::unique_ptr<Device> makeDevice(const std::string& api) {
#ifdef _WIN32
    if (api == "DirectX11") return std::make_unique<D3D11Device>();
    return std::make_unique<GLDevice>();
#elif defined(GROOMLAB_HAS_METAL)
    (void)api;
    return std::make_unique<MetalDevice>();
#else
    (void)api;
    return nullptr;
#endif
}

// Wavy strands rooted on a sphere (roots on its faces, a few unrooted), `even`: every strand
// `points` points, else from 1 to `points`; widths tapering.
StrandsPtr wavyStrands(const std::shared_ptr<const ScalpMesh>& scalp, std::size_t count, std::size_t points, bool even) {
    auto s = std::make_shared<StrandSet>();
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t n = even ? points : 1 + i % points;
        StrandRoot root;
        if (i % 17 != 0) {
            root.face = static_cast<std::int32_t>(i % scalp->triangleCount());
            root.u = 0.3f;
            root.v = 0.3f;
        }
        const Vec3f base = root.face >= 0 ? scalp->position(root) : Vec3f{unit(rng), unit(rng), unit(rng)};
        const Vec3f up = root.face >= 0 ? scalp->normal(root) : Vec3f{0, 1, 0};
        std::vector<Vec3f> p(n);
        std::vector<float> w(n);
        for (std::size_t k = 0; k < n; ++k) {
            const float t = static_cast<float>(k) * 0.05f;
            p[k] = base + up * t + Vec3f{std::sin(t * 9.0f + i), std::cos(t * 7.0f), std::sin(t * 5.0f)} * (0.02f * t);
            w[k] = 0.01f * (1.0f - 0.8f * static_cast<float>(k) / static_cast<float>(n));
        }
        if (i % 29 == 3 && n > 2) p[1] = p[0];  // a zero-length segment
        s->addStrand(static_cast<StrandId>(1000 + 3 * i), p, w, root);
    }
    return s;
}

float worst(const std::vector<float>& a, const std::vector<float>& b, std::size_t stride = 1, std::size_t offset = 0) {
    float m = 0.0f;
    for (std::size_t k = offset; k < a.size() && k < b.size(); k += stride) m = std::max(m, std::abs(a[k] - b[k]));
    return a.size() == b.size() ? m : 1e30f;
}

class LineFramesTest : public ::testing::TestWithParam<std::string> {};

TEST_P(LineFramesTest, FramesAndWidthsAreTheCpus) {
    auto device = makeDevice(GetParam());
    if (!device->available()) GTEST_SKIP() << GetParam() << ": no device";
    auto frames = device->frames();
    const auto scalp = test::makeSphere(12, 24);
    for (const bool even : {true, false}) {
        SCOPED_TRACE(even ? "even strands" : "strands of 1 to 17 points");
        const StrandsPtr strands = wavyStrands(scalp, 3000, 17, even);
        const StrandLines lines(strands, scalp, true, nullptr);
        const std::size_t n = strands->pointCount();
        void* buffers[4] = {device->buffer(n * 12), device->buffer(n * 12), device->buffer(n * 12), device->buffer(n * 8)};
        frames->write(buffers[0], strands->positions().data(), 0, n * 12);
        EXPECT_GT(frames->setLayout(strands->offsets().data(), strands->strandCount(), lines.layoutKey()), 0u);
        EXPECT_EQ(frames->setLayout(strands->offsets().data(), strands->strandCount(), lines.layoutKey()), 0u);  // held
        std::vector<float> up(strands->strandCount() * 4);
        const std::uint64_t key = lines.writeRootNormals(up.data());
        EXPECT_GT(frames->setRootNormals(up.data(), key), 0u);
        EXPECT_EQ(frames->setRootNormals(up.data(), key), 0u);
        frames->run(buffers[0], buffers[1], buffers[2]);
        std::vector<float> normals(n * 3), tangents(n * 3), uvs(n * 2);
        lines.writeFrames(normals.data(), tangents.data());
        EXPECT_LT(worst(frames->read(buffers[1], n * 3), normals), 1e-5f);
        EXPECT_LT(worst(frames->read(buffers[2], n * 3), tangents), 1e-5f);
        // The uvs: the CPU's u, then the widths alone by the GPU.
        lines.writeUvs(uvs.data(), 2);
        std::vector<float> stale = uvs;
        for (std::size_t k = 1; k < stale.size(); k += 2) stale[k] = -5.0f;
        frames->write(buffers[3], stale.data(), 0, n * 8);
        frames->writeWidths(buffers[3], strands->widths().data());
        EXPECT_EQ(frames->read(buffers[3], n * 2), uvs);
        EXPECT_GE(frames->gpuMs(true), 0.0);
        for (void* b : buffers) device->release(b);
    }
}

TEST_P(LineFramesTest, PartlyWrittenPointsGiveTheFramesOfAllOfThem) {
    auto device = makeDevice(GetParam());
    if (!device->available()) GTEST_SKIP() << GetParam() << ": no device";
    auto frames = device->frames();
    const auto scalp = test::makeSphere(12, 24);
    const StrandsPtr before = wavyStrands(scalp, 2000, 12, false);
    auto after = std::make_shared<StrandSet>(*before);
    // Strands 100-139 move (a stroke's), the others stay.
    for (std::size_t i = 100; i < 140; ++i) {
        Vec3f* p = after->strandPoints(i);
        for (std::size_t k = 1; k < after->strandPointCount(i); ++k) p[k] = p[k] + Vec3f{0.01f * k, 0.0f, -0.02f * k};
    }
    const std::size_t n = before->pointCount();
    void* buffers[3] = {device->buffer(n * 12), device->buffer(n * 12), device->buffer(n * 12)};
    const StrandLines lines(after, scalp, true, nullptr);
    frames->write(buffers[0], before->positions().data(), 0, n * 12);
    const std::size_t begin = after->strandBegin(100), end = after->strandBegin(140);
    frames->write(buffers[0], after->positions().data() + begin, begin * 12, (end - begin) * 12);
    frames->setLayout(after->offsets().data(), after->strandCount(), lines.layoutKey());
    std::vector<float> up(after->strandCount() * 4);
    frames->setRootNormals(up.data(), lines.writeRootNormals(up.data()));
    frames->run(buffers[0], buffers[1], buffers[2]);
    std::vector<float> normals(n * 3), tangents(n * 3);
    lines.writeFrames(normals.data(), tangents.data());
    EXPECT_LT(worst(frames->read(buffers[1], n * 3), normals), 1e-5f);
    EXPECT_LT(worst(frames->read(buffers[2], n * 3), tangents), 1e-5f);
    for (void* b : buffers) device->release(b);
}

// Ribbons expanded from the lines (writeRibbonLayout, setRibbonColors, runRibbons) against the CPU's
// strandRibbons: every vertex of a strand of two points or more (the CPU leaves out strands of one
// point; the GPU makes two vertices a point whatever the strand), its uvs, colors and triangles.
TEST_P(LineFramesTest, RibbonsAreTheCpus) {
    auto device = makeDevice(GetParam());
    if (!device->available()) GTEST_SKIP() << GetParam() << ": no device";
    auto frames = device->frames();
    const auto scalp = test::makeSphere(12, 24);
    const Vec3f key = ribbonKeyLight();
    const float light[3] = {key.x, key.y, key.z};
    for (const bool even : {true, false}) {
        for (const bool lit : {true, false}) {
            SCOPED_TRACE(std::string(even ? "even strands" : "strands of 1 to 17 points") + (lit ? ", lit" : ", unlit"));
            const StrandsPtr strands = wavyStrands(scalp, 2500, 17, even);
            const std::size_t n = strands->pointCount(), count = strands->strandCount();
            std::vector<std::array<float, 3>> byStrand(count);
            for (std::size_t i = 0; i < count; ++i) byStrand[i] = {0.2f + 0.0003f * i, 0.5f, 0.9f - 0.0002f * i};
            const StrandColors colors{strands, byStrand};
            const StrandLines lines(strands, scalp, lit, nullptr);
            const float scale = 1.5f;
            const DisplayGeometry cpu = strandRibbons(*strands, scalp.get(), byStrand, scale, lit, lit ? &colors : nullptr);
            // The lines on the GPU: points, uvs (u, width), the layout and root normals.
            void* points = device->buffer(n * 12);
            void* lineUvs = device->buffer(n * 8);
            void* out[6] = {device->buffer(n * 24), device->buffer(n * 24), device->buffer(n * 24), device->buffer(n * 16),
                            device->buffer(n * 32), device->buffer(n * 24)};  // positions, normals, tangents, uvs, colors, indices
            frames->write(points, strands->positions().data(), 0, n * 12);
            std::vector<float> uvs(n * 2);
            lines.writeUvs(uvs.data(), 2);
            frames->write(lineUvs, uvs.data(), 0, n * 8);
            frames->setLayout(strands->offsets().data(), count, lines.layoutKey());
            std::vector<float> up(count * 4);
            frames->setRootNormals(up.data(), lines.writeRootNormals(up.data()));
            frames->writeRibbonLayout(out[5], lit ? out[3] : nullptr, lit ? lineUvs : nullptr);  // (the uvs' u with the strand's number)
            gpu::LineFrames::RibbonTargets targets;
            targets.positions = out[0];
            if (lit) {
                std::vector<float> rgba(n * 4);
                colors.write(rgba.data());
                EXPECT_GT(frames->setRibbonColors(rgba.data(), n, 7), 0u);
                EXPECT_EQ(frames->setRibbonColors(rgba.data(), n, 7), 0u);  // held
                frames->writeRibbonColors(out[4]);
                targets.normals = out[1];
                targets.tangents = out[2];
            } else {
                std::vector<float> rgba(count * 4);
                for (std::size_t i = 0; i < count; ++i) {
                    for (int c = 0; c < 3; ++c) rgba[i * 4 + c] = byStrand[i][c];
                    rgba[i * 4 + 3] = 1.0f;
                }
                frames->setRibbonColors(rgba.data(), count, 9);
                targets.colors = out[4];
            }
            frames->runRibbons(points, lineUvs, scale, lit, targets, light);
            const std::vector<float> positions = frames->read(out[0], n * 6);
            const std::vector<float> normals = lit ? frames->read(out[1], n * 6) : std::vector<float>();
            const std::vector<float> tangents = lit ? frames->read(out[2], n * 6) : std::vector<float>();
            const std::vector<float> gpuUvs = lit ? frames->read(out[3], n * 4) : std::vector<float>();
            const std::vector<float> gpuColors = frames->read(out[4], n * 8);
            const std::vector<float> indexBits = frames->read(out[5], n * 6);
            // Each CPU vertex against the GPU's for the same point and edge.
            float worstPosition = 0, worstFrame = 0, worstUv = 0, worstColor = 0;
            std::size_t cpuVertex = 0, cpuIndex = 0, badTriangles = 0;
            for (std::size_t i = 0; i < count; ++i) {
                const std::size_t pts = strands->strandPointCount(i), begin = strands->strandBegin(i);
                if (pts < 2) {
                    // Nothing drawn: every triangle past it names its first vertex alone.
                    for (std::size_t k = 0; k < 6; ++k) {
                        std::uint32_t index;
                        std::memcpy(&index, &indexBits[begin * 6 + k], 4);
                        if (index != 2 * begin) ++badTriangles;
                    }
                    continue;
                }
                for (std::size_t k = 0; k < pts; ++k) {
                    for (std::size_t e = 0; e < 2; ++e, ++cpuVertex) {
                        const std::size_t g = 2 * (begin + k) + e;
                        for (int c = 0; c < 3; ++c) {
                            worstPosition = std::max(worstPosition, std::abs(positions[g * 3 + c] - cpu.positions[cpuVertex * 3 + c]));
                            if (lit) {
                                worstFrame = std::max(worstFrame, std::abs(normals[g * 3 + c] - cpu.normals[cpuVertex * 3 + c]));
                                worstFrame = std::max(worstFrame, std::abs(tangents[g * 3 + c] - cpu.tangents[cpuVertex * 3 + c]));
                            }
                        }
                        for (int c = 0; c < 4; ++c) worstColor = std::max(worstColor, std::abs(gpuColors[g * 4 + c] - cpu.colors[cpuVertex * 4 + c]));
                        if (lit) {
                            worstUv = std::max(worstUv, std::abs(gpuUvs[g * 2] - cpu.uvs[cpuVertex]));
                            worstUv = std::max(worstUv, std::abs(gpuUvs[g * 2 + 1] - 0.5f));
                        }
                    }
                }
                // The triangles: the CPU's, its vertices the GPU's for the same points.
                const std::size_t cpuFirst = cpuVertex - 2 * pts;
                for (std::size_t k = 0; k < pts; ++k) {
                    for (std::size_t j = 0; j < 6; ++j) {
                        std::uint32_t index;
                        std::memcpy(&index, &indexBits[(begin + k) * 6 + j], 4);
                        if (k + 1 == pts) {
                            if (index != 2 * (begin + k)) ++badTriangles;
                            continue;
                        }
                        const std::uint32_t want = cpu.indices[cpuIndex + 6 * k + j];
                        if (index - 2 * begin != want - cpuFirst) ++badTriangles;
                    }
                }
                cpuIndex += 6 * (pts - 1);
            }
            EXPECT_EQ(cpuVertex, cpu.positions.size() / 3);
            EXPECT_EQ(cpuIndex, cpu.indices.size());
            EXPECT_EQ(badTriangles, 0u);
            EXPECT_LT(worstPosition, 1e-5f);
            EXPECT_LT(worstFrame, 1e-5f);
            EXPECT_LT(worstUv, 1e-6f);
            EXPECT_LT(worstColor, 1e-4f);  // (the unlit shade: a face normal from a cross product, to rounding; 1/255 is 4e-3)
            EXPECT_GE(frames->gpuMs(true), 0.0);
            device->release(points);
            device->release(lineUvs);
            for (void* b : out) device->release(b);
        }
    }
}

// Ribbons facing the viewer (runRibbons with RibbonFacing: the strand lines at their true width):
// each point's two vertices about it, across both its tangent and the way to the viewer, half its
// width times the scale apart from it, or half the least pixels at its distance where that is more
// (the pixel's size picked so that about half the points are widened), seen from an eye and along
// an orthographic view.
TEST_P(LineFramesTest, FacingRibbonsFaceTheViewer) {
    auto device = makeDevice(GetParam());
    if (!device->available()) GTEST_SKIP() << GetParam() << ": no device";
    auto frames = device->frames();
    const auto scalp = test::makeSphere(12, 24);
    const float light[3] = {0.0f, 1.0f, 0.0f};
    const StrandsPtr strands = wavyStrands(scalp, 2000, 17, false);
    const std::size_t n = strands->pointCount(), count = strands->strandCount();
    const StrandLines lines(strands, scalp, true, nullptr);
    const float scale = 1.5f;
    void* points = device->buffer(n * 12);
    void* lineUvs = device->buffer(n * 8);
    void* out[3] = {device->buffer(n * 24), device->buffer(n * 24), device->buffer(n * 24)};
    frames->write(points, strands->positions().data(), 0, n * 12);
    std::vector<float> uvs(n * 2);
    lines.writeUvs(uvs.data(), 2);
    frames->write(lineUvs, uvs.data(), 0, n * 8);
    frames->setLayout(strands->offsets().data(), count, lines.layoutKey());
    std::vector<float> up(count * 4);
    frames->setRootNormals(up.data(), lines.writeRootNormals(up.data()));
    gpu::LineFrames::RibbonTargets targets;
    targets.positions = out[0];
    targets.normals = out[1];
    targets.tangents = out[2];
    const auto& p = strands->positions();
    auto at = [&](std::size_t i) { return Vec3f(p[i]); };
    auto halfWidthOf = [&](std::size_t i) { return 0.5f * scale * (uvs[i * 2 + 1] < 0.0f ? 0.01f : uvs[i * 2 + 1]); };
    for (const bool orthographic : {false, true}) {
        SCOPED_TRACE(orthographic ? "orthographic" : "from an eye");
        gpu::LineFrames::RibbonFacing facing;
        const Vec3f eye{0.5f, 0.8f, 6.0f}, toward = Vec3f{0.2f, 0.3f, 1.0f}.normalized();
        const Vec3f viewer = orthographic ? toward : eye;
        facing.eye[0] = viewer.x;
        facing.eye[1] = viewer.y;
        facing.eye[2] = viewer.z;
        facing.orthographic = orthographic;
        auto distanceOf = [&](std::size_t i) { return orthographic ? 1.0f : (eye - at(i)).length(); };
        // The pixel: half of it at each point's distance as wide as the median half width.
        std::vector<float> ratios(n);
        for (std::size_t i = 0; i < n; ++i) ratios[i] = halfWidthOf(i) / distanceOf(i);
        std::nth_element(ratios.begin(), ratios.begin() + n / 2, ratios.end());
        facing.pixelWorld = 2.0f * ratios[n / 2];
        facing.minPixels = 1.0f;
        frames->runRibbons(points, lineUvs, scale, true, targets, light, &facing);
        const std::vector<float> v = frames->read(out[0], n * 6);
        float worstMiddle = 0, worstHalf = 0, worstAlong = 0, worstToward = 0;
        std::size_t widened = 0, kept = 0;
        for (std::size_t s = 0; s < count; ++s) {
            const std::size_t pts = strands->strandPointCount(s), begin = strands->strandBegin(s);
            if (pts < 2) continue;
            for (std::size_t k = 0; k < pts; ++k) {
                const std::size_t i = begin + k;
                const Vec3f a{v[i * 6], v[i * 6 + 1], v[i * 6 + 2]}, b{v[i * 6 + 3], v[i * 6 + 4], v[i * 6 + 5]};
                const Vec3f q = at(i), across = b - a;
                const Vec3f along = at(begin + std::min(k + 1, pts - 1)) - at(begin + (k > 0 ? k - 1 : 0));
                if (along.length() < 1e-6f) continue;  // (points on each other: no tangent, no side, as the frames' own ribbons)
                const Vec3f t = along.normalized();
                const Vec3f view = (orthographic ? toward : eye - q).normalized();
                const float least = 0.5f * facing.pixelWorld * distanceOf(i), half = std::max(halfWidthOf(i), least);
                (least > halfWidthOf(i) ? widened : kept) += 1;
                worstMiddle = std::max(worstMiddle, (0.5f * (a + b) - q).length());
                worstHalf = std::max(worstHalf, std::abs(0.5f * across.length() - half) / half);
                const Vec3f unit = across.normalized();
                worstAlong = std::max(worstAlong, std::abs(unit.dot(t)));
                // (A strand pointing at the viewer keeps its frame's side: across the view only where it can be.)
                if (t.cross(view).length() > 0.05f) worstToward = std::max(worstToward, std::abs(unit.dot(view)));
            }
        }
        EXPECT_GT(widened, n / 4);
        EXPECT_GT(kept, n / 4);
        EXPECT_LT(worstMiddle, 1e-5f);
        EXPECT_LT(worstHalf, 1e-4f);
        EXPECT_LT(worstAlong, 1e-4f);
        EXPECT_LT(worstToward, 1e-4f);
    }
    // Split for the view: each segment drawn once, as the ribbon's two triangles where it is a pixel or
    // more wide at either end (its width times the scale over a pixel at its distance), else as its
    // line (the ribbon's vertices on one side); a strand's last point draws nothing.
    {
        gpu::LineFrames::RibbonFacing facing;
        const Vec3f eye{0.5f, 0.8f, 6.0f};
        facing.eye[0] = eye.x;
        facing.eye[1] = eye.y;
        facing.eye[2] = eye.z;
        std::vector<float> ratios(n);
        for (std::size_t i = 0; i < n; ++i) ratios[i] = 2.0f * halfWidthOf(i) / (eye - at(i)).length();
        std::nth_element(ratios.begin(), ratios.begin() + n / 2, ratios.end());
        facing.pixelWorld = ratios[n / 2];  // (about half the points a pixel or more)
        void* triangles = device->buffer(n * 24);
        void* lineIndices = device->buffer(n * 8);
        frames->writeFacingSplit(triangles, lineIndices, points, lineUvs, scale, facing, 1.0f);
        const std::vector<float> tri = frames->read(triangles, n * 6), seg = frames->read(lineIndices, n * 2);
        auto index = [](const std::vector<float>& v, std::size_t k) {
            std::uint32_t u;
            std::memcpy(&u, &v[k], 4);
            return u;
        };
        auto pixels = [&](std::size_t i) { return 2.0f * halfWidthOf(i) / (facing.pixelWorld * (eye - at(i)).length()); };
        std::size_t wrong = 0, wide = 0, thin = 0, nearEdge = 0;
        for (std::size_t s = 0; s < count; ++s) {
            const std::size_t pts = strands->strandPointCount(s), begin = strands->strandBegin(s);
            for (std::size_t k = 0; k < pts; ++k) {
                const std::size_t i = begin + k;
                const auto a = static_cast<std::uint32_t>(2 * i);
                const bool drawnAsTriangles = index(tri, i * 6) != index(tri, i * 6 + 1);
                const bool drawnAsLine = index(seg, i * 2) != index(seg, i * 2 + 1);
                if (k + 1 == pts) {
                    if (drawnAsTriangles || drawnAsLine) ++wrong;
                    continue;
                }
                const float px = std::max(pixels(i), pixels(i + 1));
                if (std::abs(px - 1.0f) < 1e-3f) {  // (at the edge: rounding may go either way, once)
                    ++nearEdge;
                    if (drawnAsTriangles == drawnAsLine) ++wrong;
                    continue;
                }
                const bool want = px >= 1.0f;
                (want ? wide : thin) += 1;
                if (drawnAsTriangles != want || drawnAsLine == want) ++wrong;
                if (want && (index(tri, i * 6) != a || index(tri, i * 6 + 1) != a + 1 || index(tri, i * 6 + 2) != a + 2 ||
                             index(tri, i * 6 + 3) != a + 1 || index(tri, i * 6 + 4) != a + 3 || index(tri, i * 6 + 5) != a + 2)) {
                    ++wrong;
                }
                if (!want && (index(seg, i * 2) != a || index(seg, i * 2 + 1) != a + 2)) ++wrong;
            }
        }
        EXPECT_EQ(wrong, 0u);
        EXPECT_GT(wide, n / 8);
        EXPECT_GT(thin, n / 8);
        EXPECT_LT(nearEdge, n / 100);
        device->release(triangles);
        device->release(lineIndices);
    }
    // Their coverage's uvs: the lines' (u with the strand's number, width) on both vertices, to the bit.
    void* ribbonUvs = device->buffer(n * 16);
    frames->writeRibbonUvs(ribbonUvs, lineUvs);
    const std::vector<float> copied = frames->read(ribbonUvs, n * 4);
    std::size_t differ = 0;
    for (std::size_t i = 0; i < n; ++i) {
        for (int e = 0; e < 2; ++e) {
            if (std::memcmp(&copied[i * 4 + e * 2], &uvs[i * 2], 8) != 0) ++differ;
        }
    }
    EXPECT_EQ(differ, 0u);
    // Unlit and unshaded: each vertex its strand's color as it is.
    std::vector<float> rgba(count * 4, 1.0f);
    for (std::size_t s = 0; s < count; ++s) {
        rgba[s * 4] = 0.1f + 0.0004f * static_cast<float>(s);
        rgba[s * 4 + 1] = 0.6f;
        rgba[s * 4 + 2] = 0.3f;
    }
    frames->setRibbonColors(rgba.data(), count, 11);
    void* colors = device->buffer(n * 32);
    gpu::LineFrames::RibbonTargets flat;
    flat.positions = out[0];
    flat.colors = colors;
    gpu::LineFrames::RibbonFacing facing;
    facing.eye[2] = 6.0f;
    facing.pixelWorld = 1e-3f;
    facing.unshaded = true;
    frames->runRibbons(points, lineUvs, scale, false, flat, light, &facing);
    const std::vector<float> drawn = frames->read(colors, n * 8);
    float worstColor = 0;
    for (std::size_t s = 0; s < count; ++s) {
        for (std::size_t i = strands->strandBegin(s); i < strands->strandBegin(s) + strands->strandPointCount(s); ++i) {
            for (int c = 0; c < 4; ++c) {
                worstColor = std::max(worstColor, std::abs(drawn[i * 8 + c] - rgba[s * 4 + c]));
                worstColor = std::max(worstColor, std::abs(drawn[i * 8 + 4 + c] - rgba[s * 4 + c]));
            }
        }
    }
    EXPECT_EQ(worstColor, 0.0f);
    device->release(ribbonUvs);
    device->release(colors);
    device->release(points);
    device->release(lineUvs);
    for (void* b : out) device->release(b);
}

// Following the scalp (setFollowRest, setFollowRoots, setFollowScalp, runFollow) against the CPU's
// carryStrands: the strands at rest carried to a deformed scalp (turned, moved and bent; one cap of
// it left where it was), point for point, a still root's points to the bit; then the lines' frames
// and ribbons from the carried points with the follow's root normals, as the CPU's from the carried
// strands on the scalp now.
TEST_P(LineFramesTest, FollowIsTheCpusCarry) {
    auto device = makeDevice(GetParam());
    if (!device->available()) GTEST_SKIP() << GetParam() << ": no device";
    auto frames = device->frames();
    const auto rest = test::makeSphere(12, 24);
    std::vector<Vec3f> moved = rest->points();
    for (Vec3f& p : moved) {
        if (p.y > 0.8f) continue;  // the cap stays
        const float a = 0.4f + 0.3f * p.y;  // turned about z by more lower down: bent
        p = Vec3f{p.x * std::cos(a) - p.y * std::sin(a), p.x * std::sin(a) + p.y * std::cos(a), p.z} + Vec3f{0.3f, -0.1f, 0.2f};
    }
    const auto now = std::make_shared<const ScalpMesh>(rest->withPoints(moved));
    const Vec3f key = ribbonKeyLight();
    const float light[3] = {key.x, key.y, key.z};
    for (const bool even : {true, false}) {
        SCOPED_TRACE(even ? "even strands" : "strands of 1 to 17 points");
        const StrandsPtr strands = wavyStrands(rest, 3000, 17, even);
        const StrandsPtr carried = carryStrands(strands, *rest, *now);
        ASSERT_NE(carried, strands);
        const std::size_t n = strands->pointCount(), count = strands->strandCount();
        void* buffers[3] = {device->buffer(n * 12), device->buffer(n * 12), device->buffer(n * 12)};
        const StrandLines lines(strands, rest, true, nullptr);
        frames->setLayout(strands->offsets().data(), count, lines.layoutKey());
        EXPECT_GT(frames->setFollowRest(&strands->positions()[0].x, 11), 0u);
        EXPECT_EQ(frames->setFollowRest(&strands->positions()[0].x, 11), 0u);  // held
        std::vector<FollowRoot> roots(count);
        const std::uint64_t rootsKey = writeFollowRoots(*strands, *rest, roots.data());
        EXPECT_GT(frames->setFollowRoots(roots.data(), rootsKey), 0u);
        EXPECT_EQ(frames->setFollowRoots(roots.data(), rootsKey), 0u);
        frames->setFollowScalp(false, &rest->points()[0].x, &rest->vertexNormals()[0].x, rest->pointCount(), 21);
        frames->setFollowScalp(true, &now->points()[0].x, &now->vertexNormals()[0].x, now->pointCount(), 22);
        frames->runFollow(buffers[0]);
        const std::vector<float> got = frames->read(buffers[0], n * 3);
        std::vector<float> want(n * 3);
        std::memcpy(want.data(), carried->positions().data(), n * 12);
        EXPECT_LT(worst(got, want), 2e-5f);
        // A root whose frame did not move (on the cap, or unrooted): its points as they were, to the bit.
        std::size_t still = 0, stillDiffer = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (!rootMotion(*rest, *now, strands->roots()[i]).still()) continue;
            ++still;
            for (std::size_t p = strands->strandBegin(i); p < strands->strandBegin(i) + strands->strandPointCount(i); ++p) {
                for (int c = 0; c < 3; ++c) {
                    if (std::memcmp(&got[p * 3 + c], &want[p * 3 + c], 4) != 0) ++stillDiffer;
                }
            }
        }
        EXPECT_GT(still, 100u);
        EXPECT_EQ(stillDiffer, 0u);
        // The frames from the carried points with the follow's root normals: the CPU's on the scalp now.
        frames->run(buffers[0], buffers[1], buffers[2]);
        const StrandLines placed(carried, now, true, nullptr);
        std::vector<float> normals(n * 3), tangents(n * 3);
        placed.writeFrames(normals.data(), tangents.data());
        EXPECT_LT(worst(frames->read(buffers[1], n * 3), normals), 1e-4f);
        EXPECT_LT(worst(frames->read(buffers[2], n * 3), tangents), 1e-4f);
        // The ribbons as well (lit).
        void* lineUvs = device->buffer(n * 8);
        void* out[3] = {device->buffer(n * 24), device->buffer(n * 24), device->buffer(n * 24)};
        std::vector<float> uvs(n * 2);
        placed.writeUvs(uvs.data(), 2);
        frames->write(lineUvs, uvs.data(), 0, n * 8);
        gpu::LineFrames::RibbonTargets targets;
        targets.positions = out[0];
        targets.normals = out[1];
        targets.tangents = out[2];
        frames->runRibbons(buffers[0], lineUvs, 1.5f, true, targets, light);
        const std::vector<std::array<float, 3>> white(count, std::array<float, 3>{1.0f, 1.0f, 1.0f});
        const DisplayGeometry cpu = strandRibbons(*carried, now.get(), white, 1.5f, true);
        const std::vector<float> positions = frames->read(out[0], n * 6), ribbonNormals = frames->read(out[1], n * 6);
        float worstPosition = 0, worstNormal = 0;
        std::size_t v = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t pts = strands->strandPointCount(i), begin = strands->strandBegin(i);
            if (pts < 2) continue;
            for (std::size_t k = 0; k < 2 * pts; ++k, ++v) {
                for (int c = 0; c < 3; ++c) {
                    worstPosition = std::max(worstPosition, std::abs(positions[(2 * begin + k) * 3 + c] - cpu.positions[v * 3 + c]));
                    worstNormal = std::max(worstNormal, std::abs(ribbonNormals[(2 * begin + k) * 3 + c] - cpu.normals[v * 3 + c]));
                }
            }
        }
        EXPECT_EQ(v, cpu.positions.size() / 3);
        EXPECT_LT(worstPosition, 2e-5f);
        EXPECT_LT(worstNormal, 1e-4f);
        // A strand set's own root normals again after it (setRootNormals sends them, whatever its key).
        std::vector<float> up(count * 4);
        EXPECT_GT(frames->setRootNormals(up.data(), lines.writeRootNormals(up.data())), 0u);
        device->release(lineUvs);
        for (void* b : out) device->release(b);
        for (void* b : buffers) device->release(b);
    }
}

// The ribbons from strands as the core's builder gives them to a host expanding them
// (setRibbonLines): lines, two vertices a point; materialized, the CPU's triangles.
TEST(RibbonLines, MaterializeAsTheCpuBuildsThem) {
    const auto scalp = test::makeSphere(12, 24);
    for (const bool lighting : {true, false}) {
        SCOPED_TRACE(lighting ? "lit" : "unlit");
        DisplaySettings settings;
        settings.ribbons = true;
        settings.ribbonWidthScale = 2.0f;
        settings.lighting = lighting;
        DisplayInputs in;
        in.strands = wavyStrands(scalp, 400, 9, false);
        in.scalp = scalp;
        in.strandsVersion = 1;
        DisplayBuilder cpu, host;
        host.setWriteLines(true);
        host.setRibbonLines(true);
        const DisplayGeometry& built = *cpu.update(in, settings).item(DisplayItemId::Ribbons).geometry;
        const DisplayGeometry& lines = *host.update(in, settings).item(DisplayItemId::Ribbons).geometry;
        ASSERT_TRUE(lines.lines);
        EXPECT_TRUE(lines.ribbons);
        EXPECT_EQ(lines.vertexCount(), 2 * in.strands->pointCount());
        EXPECT_EQ(lines.lit(), lighting);
        EXPECT_EQ(lines.materialized().digest(), built.digest());
        EXPECT_EQ(lines.digest(), built.digest());
    }
}

// The hair's density grid (runDensity: the splat, then the ambient occlusion's sweeps along each
// axis) against the same on the CPU: each cell's density (each point's piece of fiber, trilinear
// into the eight cells around it, as the kernel's fixed point adds) and its occlusion (the mean over
// the six axis directions of the light through the hair within the reach, its own cell's half).
TEST_P(LineFramesTest, DensityGridIsTheCpus) {
    auto device = makeDevice(GetParam());
    if (!device->available()) GTEST_SKIP() << GetParam() << ": no device";
    auto frames = device->frames();
    const auto scalp = test::makeSphere(12, 24);
    const StrandsPtr strands = wavyStrands(scalp, 3000, 17, true);
    const StrandLines lines(strands, scalp, true, nullptr);
    const std::size_t n = strands->pointCount();
    void* points = device->buffer(n * 12);
    void* lineUvs = device->buffer(n * 8);
    frames->write(points, strands->positions().data(), 0, n * 12);
    std::vector<float> uvs(n * 2);
    lines.writeUvs(uvs.data(), 2);
    frames->write(lineUvs, uvs.data(), 0, n * 8);
    for (const float reach : {6.0f, 2.0f}) {
        SCOPED_TRACE("reach " + std::to_string(reach));
        gpu::LineFrames::DensityGrid grid;
        const unsigned size[3] = {23, 19, 27};  // (unequal sides: each sweep's axis its own)
        grid.cell = 0.12f;
        for (int k = 0; k < 3; ++k) {
            grid.size[k] = size[k];
            grid.origin[k] = -0.5f * grid.cell * static_cast<float>(size[k]);
        }
        grid.pieceWeight = 0.785398f * 0.05f / (grid.cell * grid.cell * grid.cell);
        grid.occlusionReach = reach;
        void* volume = device->volume(size[0], size[1], size[2]);
        frames->runDensity(points, lineUvs, n, grid, volume);
        EXPECT_GE(frames->gpuMs(true), 0.0);
        const std::vector<float> gpu = device->readVolume(volume, size[0], size[1], size[2]);
        device->releaseVolume(volume);
        // The CPU's: the kernel's fixed point words, then the occlusion.
        const std::size_t cells = std::size_t{size[0]} * size[1] * size[2];
        std::vector<std::uint64_t> words(cells, 0);
        auto index = [&](int x, int y, int z) { return std::size_t(x) + size[0] * (std::size_t(y) + size[1] * std::size_t(z)); };
        auto inside = [&](int x, int y, int z) { return x >= 0 && y >= 0 && z >= 0 && x < int(size[0]) && y < int(size[1]) && z < int(size[2]); };
        for (std::size_t p = 0; p < n; ++p) {
            const Vec3f q = strands->positions()[p];
            const float width = uvs[p * 2 + 1] >= 0.0f ? uvs[p * 2 + 1] : grid.defaultWidth;
            const float amount = width * grid.pieceWeight * 65536.0f;
            const float g[3] = {(q.x - grid.origin[0]) / grid.cell - 0.5f, (q.y - grid.origin[1]) / grid.cell - 0.5f,
                                (q.z - grid.origin[2]) / grid.cell - 0.5f};
            const float lo[3] = {std::floor(g[0]), std::floor(g[1]), std::floor(g[2])};
            for (int k = 0; k < 8; ++k) {
                const int o[3] = {k & 1, (k >> 1) & 1, (k >> 2) & 1};
                const int c[3] = {int(lo[0]) + o[0], int(lo[1]) + o[1], int(lo[2]) + o[2]};
                if (!inside(c[0], c[1], c[2])) continue;
                float t = 1.0f;
                for (int a = 0; a < 3; ++a) t *= o[a] ? g[a] - lo[a] : 1.0f - (g[a] - lo[a]);
                words[index(c[0], c[1], c[2])] += static_cast<std::uint32_t>(amount * t + 0.5f);
            }
        }
        float worstSigma = 0.0f, worstOcclusion = 0.0f, largestSigma = 0.0f, lowestOcclusion = 1.0f;
        for (int z = 0; z < int(size[2]); ++z) {
            for (int y = 0; y < int(size[1]); ++y) {
                for (int x = 0; x < int(size[0]); ++x) {
                    const float sigma = static_cast<float>(words[index(x, y, z)]) / 65536.0f;
                    double through = 0.0;
                    for (int d = 0; d < 6; ++d) {
                        const int step[3] = {d == 0 ? 1 : d == 1 ? -1 : 0, d == 2 ? 1 : d == 3 ? -1 : 0, d == 4 ? 1 : d == 5 ? -1 : 0};
                        double tau = 0.5 * sigma;
                        for (int k = 1; k <= int(reach); ++k) {
                            const int c[3] = {x + step[0] * k, y + step[1] * k, z + step[2] * k};
                            if (!inside(c[0], c[1], c[2])) break;
                            tau += static_cast<double>(words[index(c[0], c[1], c[2])]) / 65536.0;
                        }
                        through += std::exp(-tau * grid.cell);
                    }
                    const std::size_t v = index(x, y, z);
                    worstSigma = std::max(worstSigma, std::abs(gpu[v * 2] - sigma) / std::max(sigma, 1.0f));
                    worstOcclusion = std::max(worstOcclusion, std::abs(gpu[v * 2 + 1] - static_cast<float>(through / 6.0)));
                    largestSigma = std::max(largestSigma, sigma);
                    lowestOcclusion = std::min(lowestOcclusion, static_cast<float>(through / 6.0));
                }
            }
        }
        EXPECT_GT(largestSigma, 1.0f);       // (hair in it)
        EXPECT_LT(lowestOcclusion, 0.9f);    // (and occluding)
        EXPECT_LT(worstSigma, 1e-4f);
        EXPECT_LT(worstOcclusion, 1e-5f);
    }
    device->release(points);
    device->release(lineUvs);
}

// The grid made again (the viewport makes it after every update that moves the strands) holds what
// one fresh build holds: each run clears it first, nothing adds up. Also after the strands were
// scaled down and back up (a groom scaled with its scalp, frozen), the same cells: the same sum.
TEST_P(LineFramesTest, DensityGridMadeAgainSumsAsOnce) {
    auto device = makeDevice(GetParam());
    if (!device->available()) GTEST_SKIP() << GetParam() << ": no device";
    auto frames = device->frames();
    const auto scalp = test::makeSphere(12, 24);
    const StrandsPtr strands = wavyStrands(scalp, 3000, 17, true);
    const StrandLines lines(strands, scalp, true, nullptr);
    const std::size_t n = strands->pointCount();
    void* points = device->buffer(n * 12);
    void* lineUvs = device->buffer(n * 8);
    std::vector<float> uvs(n * 2);
    lines.writeUvs(uvs.data(), 2);
    gpu::LineFrames::DensityGrid grid;
    const unsigned size[3] = {23, 19, 27};
    grid.cell = 0.12f;
    for (int k = 0; k < 3; ++k) {
        grid.size[k] = size[k];
        grid.origin[k] = -0.5f * grid.cell * static_cast<float>(size[k]);
    }
    grid.pieceWeight = 0.785398f * 0.05f / (grid.cell * grid.cell * grid.cell);
    void* volume = device->volume(size[0], size[1], size[2]);
    auto put = [&](float scale) {
        std::vector<Vec3f> p = strands->positions();
        std::vector<float> u = uvs;
        for (Vec3f& q : p) q = q * scale;
        for (std::size_t i = 0; i < n; ++i) u[i * 2 + 1] *= scale;
        frames->write(points, p.data(), 0, n * 12);
        frames->write(lineUvs, u.data(), 0, n * 8);
    };
    put(1.0f);
    frames->runDensity(points, lineUvs, n, grid, volume);
    const double once = frames->densitySum();
    EXPECT_GT(once, 1.0);
    EXPECT_EQ(frames->densityPoints(), n);
    for (int again = 0; again < 4; ++again) {
        frames->runDensity(points, lineUvs, n, grid, volume);
        EXPECT_DOUBLE_EQ(frames->densitySum(), once) << "made " << again + 2 << " times";
    }
    put(0.1f);
    frames->runDensity(points, lineUvs, n, grid, volume);
    EXPECT_NE(frames->densitySum(), once);
    put(1.0f);
    frames->runDensity(points, lineUvs, n, grid, volume);
    EXPECT_DOUBLE_EQ(frames->densitySum(), once) << "scaled down and back up";
    // The host's texture holds what the kernels made (where the backend reads textures back).
    const std::vector<float> own = frames->readVolume(nullptr);
    if (!own.empty()) {
        EXPECT_EQ(own.size(), std::size_t{size[0]} * size[1] * size[2] * 2);
        EXPECT_EQ(frames->readVolume(volume), own) << "the texture copied into";
    }
    device->releaseVolume(volume);
    device->release(points);
    device->release(lineUvs);
}

// runDensity's GPU time at the viewport bench's 1M strands (16 points each on a radius-10 sphere,
// rows of strands side by side, 159 cells a side): the median of 15 runs, with the occlusion's reach
// and without. A benchmark, run by hand (--gtest_also_run_disabled_tests).
TEST_P(LineFramesTest, DISABLED_DensityGridTimes) {
    auto device = makeDevice(GetParam());
    if (!device->available()) GTEST_SKIP();
    auto frames = device->frames();
    const std::size_t strands = 1000000, per = 16, n = strands * per;
    std::vector<float> xyz(n * 3), uv(n * 2);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    for (std::size_t i = 0; i < strands; ++i) {
        // (Strands side by side in a row, rows down the sphere, as a scalp's are made.)
        const float z = 1.0f - 2.0f * (static_cast<float>(i / 1000) + 0.5f) / 1000.0f + 0.0005f * unit(rng);
        const float phi = 6.2831853f * (static_cast<float>(i % 1000) + 0.5f + 0.3f * unit(rng)) / 1000.0f;
        const float rho = std::sqrt(std::max(0.0f, 1.0f - z * z));
        float d[3] = {rho * std::cos(phi), z, rho * std::sin(phi)};
        for (std::size_t k = 0; k < per; ++k) {
            const float r = 10.0f + 0.12f * static_cast<float>(k);
            for (int c = 0; c < 3; ++c) xyz[(i * per + k) * 3 + c] = d[c] * r + (c == 1 ? -0.1f * static_cast<float>(k) : 0.0f);
            uv[(i * per + k) * 2] = static_cast<float>(k) / (per - 1);
            uv[(i * per + k) * 2 + 1] = 0.007f;
        }
    }
    void* points = device->buffer(n * 12);
    void* lineUvs = device->buffer(n * 8);
    frames->write(points, xyz.data(), 0, n * 12);
    frames->write(lineUvs, uv.data(), 0, n * 8);
    gpu::LineFrames::DensityGrid grid;
    grid.cell = 0.15f;
    for (int k = 0; k < 3; ++k) {
        grid.size[k] = 159;
        grid.origin[k] = -11.9f;
    }
    grid.pieceWeight = 0.785398f * 0.12f / (grid.cell * grid.cell * grid.cell);
    void* volume = device->volume(159, 159, 159);
    for (const float reach : {6.0f, 0.0f}) {
        grid.occlusionReach = reach;
        std::vector<double> ms;
        for (int r = 0; r < 16; ++r) {
            frames->runDensity(points, lineUvs, n, grid, volume);
            const double t = frames->gpuMs(true);
            if (r > 0) ms.push_back(t);
        }
        std::sort(ms.begin(), ms.end());
        std::printf("[ density ] %s reach %.0f: %.3f ms (min %.3f)\n", GetParam().c_str(), reach, ms[ms.size() / 2], ms.front());
    }
    device->releaseVolume(volume);
    device->release(points);
    device->release(lineUvs);
}

// The APIs, picked outside the macro (MSVC takes no #ifdef inside a macro's arguments).
#ifdef _WIN32
#define GROOMLAB_LINE_FRAME_APIS ::testing::Values("DirectX11", "OpenGL")
#else
#define GROOMLAB_LINE_FRAME_APIS ::testing::Values("Metal")
#endif
INSTANTIATE_TEST_SUITE_P(Apis, LineFramesTest, GROOMLAB_LINE_FRAME_APIS,
                         [](const ::testing::TestParamInfo<std::string>& info) { return info.param; });

}  // namespace
}  // namespace groomlab
