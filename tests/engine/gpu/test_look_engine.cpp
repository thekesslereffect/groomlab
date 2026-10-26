// SPDX-License-Identifier: Apache-2.0
// The GPU look preview's kernels (engine/gpu/, look_engine.h) against the CPU operators: the same
// strands in, the same params, the results read back and compared, on every backend. Each runs on a
// device of its own (DirectX 11: a hardware device; OpenGL: a hidden window's 4.5 context); a
// backend without one skips (CI without a GPU, a remote session).

#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <tuple>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>
#include <d3d11.h>
#undef near  // (windows.h: view_projection.h names a method so)
#undef far

#include "../core/test_helpers.h"
#include "groomlab/display.h"
#include "groomlab/flow.h"
#include "groomlab/frames.h"
#include "groomlab/gpu/look_engine.h"
#include "groomlab/gpu/look_preview.h"
#include "groomlab/look/look_cpu.h"
#include "groomlab/groom.h"
#include "groomlab/presets.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom_session.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "groomlab/noise.h"
#include "groomlab/parts.h"
#include "groomlab/sculpt.h"
#include "groomlab/stroke.h"

namespace groomlab {
namespace {

// A backend's device for the tests, and target buffers on it.
class Backend {
public:
    virtual ~Backend() = default;
    virtual bool available() const = 0;
    virtual std::unique_ptr<gpu::LookEngine> engine() = 0;
    // A vertex buffer the kernels write in place.
    virtual void* displayBuffer(std::size_t bytes) = 0;
    // A vertex buffer they cannot (copied into), or null where every buffer can be written.
    virtual void* plainBuffer(std::size_t bytes) = 0;
    virtual std::vector<Vec3f> read(void* buffer, std::size_t count) = 0;
    // `count` floats of a buffer, and `bytes` written into one (a host's upload).
    virtual std::vector<float> readFloats(void* buffer, std::size_t count) = 0;
    virtual void fill(void* buffer, const void* data, std::size_t bytes) = 0;
    virtual void release(void* buffer) = 0;
};

class D3D11Backend final : public Backend {
public:
    D3D11Backend() {
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device_, nullptr,
                          &context_);
    }
    ~D3D11Backend() override {
        if (context_) context_->Release();
        if (device_) device_->Release();
    }
    bool available() const override { return device_ != nullptr; }
    std::unique_ptr<gpu::LookEngine> engine() override { return gpu::makeD3D11LookEngine(device_); }
    void* displayBuffer(std::size_t bytes) override { return gpu::createD3D11DisplayBuffer(device_, bytes); }
    void* plainBuffer(std::size_t bytes) override {  // as Maya's own vertex buffers are made
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(bytes);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        ID3D11Buffer* b = nullptr;
        device_->CreateBuffer(&desc, nullptr, &b);
        return b;
    }
    std::vector<Vec3f> read(void* buffer, std::size_t count) override {
        std::vector<Vec3f> out(count);
        const std::vector<float> floats = readFloats(buffer, count * 3);
        if (count) std::memcpy(out.data(), floats.data(), count * 12);
        return out;
    }
    void fill(void* buffer, const void* data, std::size_t bytes) override {
        const D3D11_BOX box{0, 0, 0, static_cast<UINT>(bytes), 1, 1};
        context_->UpdateSubresource(static_cast<ID3D11Buffer*>(buffer), 0, &box, data, 0, 0);
    }
    std::vector<float> readFloats(void* buffer, std::size_t count) override {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(count * 4);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Buffer* staging = nullptr;
        device_->CreateBuffer(&desc, nullptr, &staging);
        const D3D11_BOX box{0, 0, 0, desc.ByteWidth, 1, 1};
        context_->CopySubresourceRegion(staging, 0, 0, 0, 0, static_cast<ID3D11Buffer*>(buffer), 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        std::vector<float> out(count);
        if (SUCCEEDED(context_->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            std::memcpy(out.data(), mapped.pData, count * 4);
            context_->Unmap(staging, 0);
        }
        staging->Release();
        return out;
    }
    void release(void* buffer) override {
        if (buffer) static_cast<ID3D11Buffer*>(buffer)->Release();
    }

private:
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
};

// OpenGL: a hidden window's 4.5 core profile context (as Maya's Viewport 2.0 makes), current on
// the test's thread.
class GLBackend final : public Backend {
public:
    GLBackend() {
        WNDCLASSA wc{};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "GroomLabGpuTest";
        RegisterClassA(&wc);  // fails harmlessly when a test before registered it
        window_ = CreateWindowA("GroomLabGpuTest", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
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
        getBufferSubData_ = reinterpret_cast<decltype(getBufferSubData_)>(wglGetProcAddress("glGetBufferSubData"));
        bufferSubData_ = reinterpret_cast<decltype(bufferSubData_)>(wglGetProcAddress("glBufferSubData"));
    }
    ~GLBackend() override {
        if (context_) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(context_);
        }
        if (dc_) ReleaseDC(window_, dc_);
        if (window_) DestroyWindow(window_);
    }
    bool available() const override { return context_ != nullptr; }
    std::unique_ptr<gpu::LookEngine> engine() override { return gpu::makeGLLookEngine(); }
    void* displayBuffer(std::size_t bytes) override {  // as Maya's own vertex buffers are made
        GLuint b = 0;
        genBuffers_(1, &b);
        bindBuffer_(0x8892, b);  // GL_ARRAY_BUFFER
        bufferData_(0x8892, static_cast<std::ptrdiff_t>(bytes), nullptr, 0x88E4);  // GL_STATIC_DRAW
        bindBuffer_(0x8892, 0);
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(b));
    }
    void* plainBuffer(std::size_t) override { return nullptr; }  // every buffer is written in place
    std::vector<Vec3f> read(void* buffer, std::size_t count) override {
        std::vector<Vec3f> out(count);
        bindBuffer_(0x8892, static_cast<GLuint>(reinterpret_cast<std::uintptr_t>(buffer)));
        getBufferSubData_(0x8892, 0, static_cast<std::ptrdiff_t>(count * 12), out.data());
        bindBuffer_(0x8892, 0);
        return out;
    }
    std::vector<float> readFloats(void* buffer, std::size_t count) override {
        std::vector<float> out(count);
        bindBuffer_(0x8892, static_cast<GLuint>(reinterpret_cast<std::uintptr_t>(buffer)));
        getBufferSubData_(0x8892, 0, static_cast<std::ptrdiff_t>(count * 4), out.data());
        bindBuffer_(0x8892, 0);
        return out;
    }
    void fill(void* buffer, const void* data, std::size_t bytes) override {
        bindBuffer_(0x8892, static_cast<GLuint>(reinterpret_cast<std::uintptr_t>(buffer)));
        bufferSubData_(0x8892, 0, static_cast<std::ptrdiff_t>(bytes), data);
        bindBuffer_(0x8892, 0);
    }
    void release(void* buffer) override {
        const auto b = static_cast<GLuint>(reinterpret_cast<std::uintptr_t>(buffer));
        if (b) deleteBuffers_(1, &b);
    }

private:
    HWND window_ = nullptr;
    HDC dc_ = nullptr;
    HGLRC context_ = nullptr;
    void(APIENTRY* genBuffers_)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* deleteBuffers_)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* bindBuffer_)(GLenum, GLuint) = nullptr;
    void(APIENTRY* bufferData_)(GLenum, std::ptrdiff_t, const void*, GLenum) = nullptr;
    void(APIENTRY* getBufferSubData_)(GLenum, std::ptrdiff_t, std::ptrdiff_t, void*) = nullptr;
    void(APIENTRY* bufferSubData_)(GLenum, std::ptrdiff_t, std::ptrdiff_t, const void*) = nullptr;
};

// The CPU engine (look.slang's kernels compiled to C++): its targets are float3 arrays in memory.
class CpuBackend final : public Backend {
public:
    bool available() const override { return true; }
    std::unique_ptr<gpu::LookEngine> engine() override { return gpu::makeCpuLookEngine(); }
    void* displayBuffer(std::size_t bytes) override { return new float[(bytes + 3) / 4](); }
    void* plainBuffer(std::size_t) override { return nullptr; }
    std::vector<Vec3f> read(void* buffer, std::size_t count) override {
        std::vector<Vec3f> out(count);
        std::memcpy(out.data(), buffer, count * 12);
        return out;
    }
    std::vector<float> readFloats(void* buffer, std::size_t count) override {
        std::vector<float> out(count);
        std::memcpy(out.data(), buffer, count * 4);
        return out;
    }
    void fill(void* buffer, const void* data, std::size_t bytes) override { std::memcpy(buffer, data, bytes); }
    void release(void* buffer) override { delete[] static_cast<float*>(buffer); }
};

std::unique_ptr<Backend> makeBackend(const std::string& name) {
    if (name == "DirectX11") return std::make_unique<D3D11Backend>();
    if (name == "OpenGL") return std::make_unique<GLBackend>();
    if (name == "CPU") return std::make_unique<CpuBackend>();
    return nullptr;
}

// The benchmark's chain (tools/bench/xgen_compare groomlab_graph): roots straight out of the scalp
// (or, with `guides`, shaped by Interpolate from guides), clumped, then Noise, Curl and Width,
// then a Resample with nothing to do. Each node's params (and anything else of its JSON) and
// extra nodes and connections can be given.
struct LookGraph {
    bool guides = false;   // Guides -> Interpolate between Scatter and the clumping
    bool length = false;   // a Length after the clumping (lengthNode: its JSON)
    std::string interpolate = R"("params": {})";
    std::string scatter = R"("params": {"density": 40.0, "length": 3.0})";
    std::string clump = R"("params": {"strength": 0.8})";
    std::string lengthNode = R"("params": {"keep": 0.9, "fraction": 0.7, "trimMin": 0.1, "trimMax": 0.3})";
    std::string noise = R"("params": {"amplitude": 0.3, "frequency": 1.0})";
    std::string curl = R"("params": {"radius": 0.1, "frequency": 0.8})";
    std::string width = R"("params": {"width": 0.01})";
    std::string nodes, connections;  // ", {...}" each
    bool uneven = false;  // strands of different point counts: a Resample by length before Noise, none after
    std::string after;    // a node after Width (its type): the CPU's
    std::string afterParams;  // its JSON past the type (`"params": {...}`), if any
    // The Clump's params with about 60 centers of its own (unless they say a count).
    static std::string ownCenters(std::string params) {
        if (params.find("\"count\"") == std::string::npos) params.insert(params.find('{') + 1, R"("count": 60, )");
        return params;
    }
    // A Curl's params adding no points (tolerance 0, unless they say one): the GPU runs a Curl at
    // the points it is given (one that added points is the CPU's: CurlAddingPointsIsTheCpus), so a
    // drag whose curls would want more a turn compares with the CPU's at the same points.
    static std::string noPointsAdded(std::string params) {
        if (params.find("\"tolerance\"") != std::string::npos) return params;
        const std::size_t open = params.find('{');
        const bool empty = params.find_first_not_of(" ", open + 1) == params.find('}', open);
        params.insert(open + 1, empty ? R"("tolerance": 0.0)" : R"("tolerance": 0.0, )");
        return params;
    }

    std::string json() const {
        std::string j = R"({"nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", )" + scatter + R"(},
        {"name": "clump", "type": "Clump", )" + ownCenters(clump) + "},\n";
        if (guides) {
            j += R"({"name": "guides", "type": "Guides"}, {"name": "interpolate", "type": "Interpolate", )" + interpolate +
                 "},\n";
        }
        if (length) j += R"({"name": "length", "type": "Length", )" + lengthNode + "},\n";
        if (uneven) j += R"({"name": "even", "type": "Resample", "params": {"segments": 15, "mode": "length", "minSegments": 4}},)";
        j += R"({"name": "noise", "type": "Noise", )" + noise + "},\n" + R"({"name": "curl", "type": "Curl", )" + noPointsAdded(curl) + "},\n" +
             R"({"name": "width", "type": "Width", )" + width + "},\n";
        if (!uneven) j += R"({"name": "resample", "type": "Resample", "params": {"segments": 15, "mode": "even"}},)";
        if (!after.empty()) j += R"({"name": "after", "type": ")" + after + "\"" + (afterParams.empty() ? "" : ", " + afterParams) + "},";
        j += R"({"name": "output", "type": "Output"})" + nodes + R"(],
      "connections": [
        {"from": "scalp", "to": "scatter", "input": "scalp"},)";
        const std::string grown = guides ? "interpolate" : "scatter";
        if (guides) {
            j += R"({"from": "scatter", "to": "interpolate", "input": "strands"}, {"from": "guides", "to": "interpolate", "input": "guides"},
                {"from": "scalp", "to": "interpolate", "input": "scalp"},)";
        }
        j += R"({"from": ")" + grown + R"(", "to": "clump", "input": "strands"},
        )";
        std::string last = "clump";
        if (length) {
            j += R"({"from": "clump", "to": "length", "input": "strands"},)";
            last = "length";
        }
        if (uneven) {
            j += R"({"from": ")" + last + R"(", "to": "even", "input": "strands"},)";
            last = "even";
        }
        j += R"({"from": ")" + last + R"(", "to": "noise", "input": "strands"},)";
        j += R"({"from": "noise", "to": "curl", "input": "strands"}, {"from": "curl", "to": "width", "input": "strands"},)";
        last = uneven ? "width" : "resample";
        if (!uneven) j += R"({"from": "width", "to": "resample", "input": "strands"},)";
        if (!after.empty()) {
            j += R"({"from": ")" + last + R"(", "to": "after", "input": "strands"}, {"from": "after", "to": "output", "input": "strands"})";
        } else {
            j += R"({"from": ")" + last + R"(", "to": "output", "input": "strands"})";
        }
        return j + connections + R"(], "output": "output"})";
    }
};

std::string graph(double amplitude) {
    LookGraph g;
    g.noise = R"("params": {"amplitude": )" + std::to_string(amplitude) + R"(, "frequency": 1.0})";
    return g.json();
}

// Curved guides around the upper sphere (of `radius`): out along the normal, bent sideways by `bend`.
GuidesPtr guideSet(const ScalpMesh& scalp, float bend, float radius = 4.0f) {
    auto guides = std::make_shared<GuideSet>();
    std::uint64_t id = 1;
    for (int ring = 1; ring <= 4; ++ring) {
        const float theta = 0.25f * static_cast<float>(ring);
        for (int k = 0; k < 10; ++k) {
            const float phi = 6.2831853f * (static_cast<float>(k) + 0.5f * static_cast<float>(ring % 2)) / 10.0f;
            const Vec3f n(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
            const Vec3f side = n.cross(Vec3f(0, 1, 0)).length() > 1e-3f ? n.cross(Vec3f(0, 1, 0)).normalized() : Vec3f(1, 0, 0);
            std::vector<Vec3f> pts;
            for (int i = 0; i <= 6; ++i) {
                const float t = static_cast<float>(i) / 6.0f;
                pts.push_back(n * (radius + 3.0f * t) + side * (bend * t * t * (1.0f + 0.1f * static_cast<float>(k))));
            }
            guides->add(id++, std::move(pts));
        }
    }
    guides->rootOnMesh(scalp);
    return guides;
}

GroomData groom(const std::string& graphJson, float bend = 0.6f, float radius = 4.0f) {
    GroomData d;
    d.graphJson = graphJson;
    d.scalp = test::makeSphere(24, 48, radius);
    d.guides = guideSet(*d.scalp, bend, radius);
    return d;
}

// The sphere with UVs (u around, v up) and a painted vertex map "mask" (a color set, red rising
// toward the top), as Groom Tools > Paint leaves a scalp.
GroomData paintedGroom(const std::string& graphJson) {
    GroomData d = groom(graphJson);
    auto mesh = std::make_shared<ScalpMesh>(d.scalp->points(), d.scalp->triangles());
    std::vector<ScalpMesh::UV> uvs;
    std::vector<ScalpMesh::Color> mask;
    for (const auto& tri : mesh->triangles()) {
        for (const std::int32_t v : tri) {
            const Vec3f& p = mesh->points()[v];
            uvs.push_back({0.5f + 0.5f * std::atan2(p.z, p.x) / 3.14159265f, 0.5f + p.y / 8.0f});
            mask.push_back({std::clamp(0.5f + p.y / 6.0f, 0.0f, 1.0f), 0.0f, 0.0f, 1.0f});
        }
    }
    mesh->setCornerUVs(uvs);
    mesh->setCornerColors("mask", mask);
    d.scalp = mesh;
    return d;
}

// A gray image (PGM) of stripes across u, written to a temporary file of its own per caller
// (`tag`: the backend) and process: ctest -j runs the DirectX 11 and OpenGL instances side by
// side, and one would remove the file while the other reads it.
std::string stripes(const std::string& tag) {
    char dir[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    const std::string path =
        std::string(dir) + "groomlab_gpu_stripes_" + tag + "_" + std::to_string(GetCurrentProcessId()) + ".pgm";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return path;
    std::fprintf(f, "P5\n64 16\n255\n");
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 64; ++x) std::fputc((x / 8) % 2 ? 230 : 40, f);
    }
    std::fclose(f);
    return path;
}

// Distances under which the GPU's results count as the CPU's: float rounding (fused multiply-adds,
// the GPU's sqrt, division, sin and cos) on strands 3 to 7 units long, far below a pixel at any
// viewing distance.
constexpr double kPointTolerance = 1e-3, kMeanTolerance = 1e-5, kFrameTolerance = 1e-3;
// The bitangent is cross(tangent, root normal) normalized: where a strand runs nearly along its
// root normal a rounding of the tangent turns it by that rounding over |cross| (at least 1e-4, as
// strandFrame tests it), so it gets a wider allowance (its normal and tangent stay within 1e-3): a
// tangent rounding of 1e-5 turns it by 0.02 where |cross| is 5e-4 (one point of hair_basics' Clump
// alone, on DirectX 11, comes to 0.013). Past that, a point's own: its tangent's error (the GPU's
// from the CPU's) times kBitangentTurn over |tangent x normal| (the CPU's), what that tangent
// error can turn the bitangent by there (|tangent x root normal| is at least half |tangent x
// normal|, the normal being the root normal less half its along-the-strand part: a turn of at most
// 2 x 2 |dt| / |tangent x normal|). curly's tight curls: points within 3.3e-5 of the CPU's, tangents
// within 1e-4 on both APIs; one point, 0.15 degrees off its root normal (|tangent x normal| 0.0027),
// its tangent 6.6e-5 apart, its bitangent 0.041 apart on DirectX 11 (of its allowance 0.099).
constexpr double kBitangentTolerance = 2e-2, kBitangentTurn = 4.0;
// A width goes by its point's share of its strand's length (Width's profile): a point within
// kPointTolerance of the CPU's (a rounding apart deciding the clearance walk's push of one point
// the other way) moves the widths after it by a few millionths.
constexpr double kWidthTolerance = 4e-6;

// The bitangents the kernel writes beside the normals and tangents, from the CPU's frames: the
// viewport carries none (display.h: a host's shader takes the cross product), so the CPU's are
// made here as strandFrame makes them: tangent x root normal, normalized, which tangent x normal
// is along (the normal is the root normal less half its along-the-strand part); a strand along
// its root normal (|tangent x root normal| under 1e-4, which is |tangent x normal| under about
// 2e-4) takes any direction across it, as anyPerpendicular gives.
std::vector<Vec3f> cpuBitangents(const std::vector<Vec3f>& normals, const std::vector<Vec3f>& tangents) {
    std::vector<Vec3f> out(normals.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        Vec3f side = tangents[i].cross(normals[i]);
        if (side.length() < 2e-4f) side = anyPerpendicular(tangents[i]);
        out[i] = side.normalized();
    }
    return out;
}

struct Compared {
    gpu::PointError positions, normals, tangents, bitangents;
    double widthMax = 0;
    std::size_t over = 0;  // points more than 1e-4 apart
    double bitangentShare = 0;  // the most of a point's bitangent allowance its error takes (bitangentShare)
};

// The most of its allowance any point's bitangent error takes (kBitangentTolerance, or what its
// tangent's error turns it by where it runs near its root normal: kBitangentTurn).
double bitangentShare(const std::vector<Vec3f>& got, const std::vector<Vec3f>& want, const std::vector<Vec3f>& gotTangents,
                      const std::vector<Vec3f>& tangents, const std::vector<Vec3f>& normals) {
    double share = 0;
    for (std::size_t i = 0; i < got.size() && i < want.size() && i < gotTangents.size() && i < tangents.size(); ++i) {
        const double across = tangents[i].cross(normals[i]).length();
        const double turn = across > 0 ? kBitangentTurn * (gotTangents[i] - tangents[i]).length() / across : 0.0;
        share = std::max(share, (got[i] - want[i]).length() / std::max(kBitangentTolerance, turn));
    }
    return share;
}

// Whether two arrays hold the same bits.
template <typename T>
bool sameBits(const std::vector<T>& a, const std::vector<T>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}

// What the engine runs for `session`'s graph (with `overrides`): from the stage of node `from`
// (else the first overridden, else the first of the chain) to the output.
struct Prepared {
    gpu::LookChain chain;
    gpu::LookRun run;
    StrandsPtr base, output;
    std::string why;  // why not, when it is not
};
bool prepareRun(GroomSession& session, const gpu::Overrides& overrides, const std::string& from, Prepared* p,
                gpu::LookInputs* kept = nullptr, const std::set<NodeId>& changed = {}) {
    const GraphDocument* doc = session.viewDocument();
    if (!doc) {
        p->why = "no document";
        return false;
    }
    gpu::LookChain full;
    if (!gpu::findLookChain(*doc, &full, &p->why)) return false;
    std::size_t first = 0;
    if (!from.empty()) {
        first = full.indexOf(doc->nodesByName.at(from));
    } else if (gpu::firstOverridden(*doc, full, overrides) != gpu::kNoStage) {
        first = gpu::firstOverridden(*doc, full, overrides);
    }
    if (first == gpu::kNoStage) {
        p->why = from + " is not in the chain";
        return false;
    }
    p->chain = gpu::chainFrom(*doc, full, first);
    gpu::LookInputs own;
    gpu::LookInputs& inputs = kept ? *kept : own;
    return gpu::readLookParams(*doc, p->chain, overrides, &p->run.params, &p->why) &&
           gpu::lookStrands(*doc, p->chain, &p->base, &p->output, &p->why) &&
           gpu::readLookFields(*doc, p->chain, session.last().scalp.get(), &p->run.fields, &p->why) &&
           gpu::lookFieldsHold(*doc, p->chain, p->run.fields, &p->why) &&
           inputs.read(*doc, p->chain, p->run.params, session.last().parts.get(), &p->run.inputs, &p->why, changed,
                       session.last().scalp.get(), &p->run.fields, session.last().scalp);
}
Prepared prepare(GroomSession& session, const gpu::Overrides& overrides = {}, const std::string& from = "",
                 const std::set<NodeId>& changed = {}) {
    Prepared p;
    EXPECT_TRUE(prepareRun(session, overrides, from, &p, nullptr, changed)) << p.why;
    return p;
}

class LookEngineTest : public ::testing::TestWithParam<std::string> {
protected:
    void SetUp() override {
        backend_ = makeBackend(GetParam());
        if (!backend_ || !backend_->available()) GTEST_SKIP() << "no " << GetParam() << " device";
    }

    // The GPU's run of `session`'s graph (with `overrides`, from node `from`) against `cpu`'s
    // output strands.
    Compared runAndCompare(GroomSession& session, const gpu::Overrides& overrides, GroomSession& cpu, const std::string& from = "") {
        const Prepared p = prepare(session, overrides, from, changed_);
        const auto made = std::chrono::steady_clock::now();
        auto engine = backend_->engine();  // the kernels made: from bytecode, or GLSL by the driver
        const double makeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - made).count();
        if (configure_) configure_(*engine);
        EXPECT_TRUE(engine->setBase(p.base, session.last().scalp.get()));
        EXPECT_FALSE(engine->setBase(p.base, session.last().scalp.get()));  // held
        engine->run(p.run, true);
        const gpu::GpuTimes times = engine->times(true);
        EXPECT_TRUE(times.valid);

        const StrandsPtr want = cpu.last().strands;
        Compared c;
        const std::vector<Vec3f> points = engine->readPositions();
        c.positions = gpu::compareVectors(points, want->positions());
        std::size_t over = 0;  // points more than 1e-4 apart
        for (std::size_t i = 0; i < points.size() && i < want->pointCount(); ++i) over += (points[i] - want->positions()[i]).length() > 1e-4f;
        c.over = over;
        const StrandLines lines(want, cpu.last().scalp, true, nullptr);
        const std::size_t n = want->pointCount();
        std::vector<Vec3f> frames[3] = {std::vector<Vec3f>(n), std::vector<Vec3f>(n), {}};
        lines.writeFrames(&frames[0][0].x, &frames[1][0].x);
        frames[2] = cpuBitangents(frames[0], frames[1]);
        std::vector<Vec3f> got[3];
        engine->readFrames(&got[0], &got[1], &got[2]);
        c.normals = gpu::compareVectors(got[0], frames[0]);
        c.tangents = gpu::compareVectors(got[1], frames[1]);
        c.bitangents = gpu::compareVectors(got[2], frames[2]);
        c.bitangentShare = bitangentShare(got[2], frames[2], got[1], frames[1], frames[0]);
        const std::vector<float> widths = engine->readWidths();
        EXPECT_EQ(widths.size(), want->widths().size());
        for (std::size_t i = 0; i < widths.size() && i < want->widths().size(); ++i) {
            c.widthMax = std::max(c.widthMax, static_cast<double>(std::abs(widths[i] - want->widths()[i])));
        }
        // The same run dispatched as rows of 3 groups (as one past 65535 groups runs): every kernel's
        // results to the bit, the last row's groups past the end leaving as threads past it do.
        if (rowsChecked_) {
            engine->setDispatchLimit(3);
            engine->run(p.run, true);
            std::vector<Vec3f> rows[3];
            engine->readFrames(&rows[0], &rows[1], &rows[2]);
            EXPECT_TRUE(sameBits(engine->readPositions(), points)) << "positions as rows";
            for (int i = 0; i < 3; ++i) EXPECT_TRUE(sameBits(rows[i], got[i])) << "frames " << i << " as rows";
            EXPECT_TRUE(sameBits(engine->readWidths(), widths)) << "widths as rows";
        }
        std::string stages;
        for (const gpu::LookStage& s : p.chain.stages) stages += std::string(stages.empty() ? "" : " ") + gpu::stageName(s.kind);
        std::printf("[ %-8s ] %s: %zu strands, %zu points: positions max %.3g mean %.3g (%zu over 1e-4), normals max %.3g, tangents max %.3g, "
                    "bitangents max %.3g, widths max %.3g; GPU ms interpolate %.3f (clearance %.3f) clump %.3f length %.3f noise %.3f curl %.3f "
                    "width %.3f selfCollide %.3f static %.3f tie %.3f lines %.3f; engine made in %.1f ms\n",
                    GetParam().c_str(), stages.c_str(), want->strandCount(), n, c.positions.max, c.positions.mean, over, c.normals.max,
                    c.tangents.max, c.bitangents.max, c.widthMax, times.interpolateMs, times.clearanceMs, times.clumpMs, times.lengthMs, times.noiseMs,
                    times.curlMs, times.widthMs, times.selfCollideMs, times.staticMs, times.tieMs, times.linesMs, makeMs);
        return c;
    }

    std::unique_ptr<Backend> backend_;
    std::function<void(gpu::LookEngine&)> configure_;  // runAndCompare's engine set up so (threads, dispatch rows)
    bool rowsChecked_ = true;  // runAndCompare's run again as rows of 3 groups, to the bit
    std::set<NodeId> changed_;  // nodes runAndCompare's run stands for anew besides its stages (a stroke's guides)
};

void expectClose(const Compared& c) {
    EXPECT_GT(c.positions.count, 1000u);
    EXPECT_LT(c.positions.max, kPointTolerance);
    EXPECT_LT(c.positions.mean, kMeanTolerance);
    EXPECT_LT(c.normals.max, kFrameTolerance);
    EXPECT_LT(c.tangents.max, kFrameTolerance);
    EXPECT_LT(c.bitangentShare, 1.0) << "bitangents max " << c.bitangents.max;
    EXPECT_LT(c.widthMax, kWidthTolerance);
}
// The same but the widths: a run without a Width (its widths are the strands', which it does not write).
void expectPointsClose(const Compared& c) {
    EXPECT_GT(c.positions.count, 1000u);
    EXPECT_LT(c.positions.max, kPointTolerance);
    EXPECT_LT(c.positions.mean, kMeanTolerance);
    EXPECT_LT(c.normals.max, kFrameTolerance);
    EXPECT_LT(c.tangents.max, kFrameTolerance);
    EXPECT_LT(c.bitangentShare, 1.0) << "bitangents max " << c.bitangents.max;
}

// The graphs a groom starts from: the default graph, hair_basics (fitted to the test's scalp, as a
// host applies it), and hair_basics with its Clump alone (it picks its own centers).
std::string startingGraph(const std::string& which) {
    if (which == "default") return defaultGraphJson();
    std::ifstream file(std::filesystem::u8path(std::string(GROOMLAB_SHIPPED_PRESETS) + "/hair_basics.json"));
    std::stringstream text;
    text << file.rdbuf();
    nlohmann::json graph = nlohmann::json::parse(presetGraphJson(text.str(), presetScalpSize(*groom("{}").scalp)));
    if (which == "clumpAlone") {
        nlohmann::json nodes = nlohmann::json::array(), links = nlohmann::json::array();
        for (const nlohmann::json& n : graph["nodes"]) {
            if (n["name"] != "clumpCenters") nodes.push_back(n);
        }
        for (const nlohmann::json& c : graph["connections"]) {
            if (c["from"] != "clumpCenters" && c["to"] != "clumpCenters") links.push_back(c);
        }
        graph["nodes"] = nodes;
        graph["connections"] = links;
    }
    return graph.dump();
}

// A graph with its Interpolate not keeping clearance.
std::string withoutClearance(const std::string& graphJson) {
    nlohmann::json graph = nlohmann::json::parse(graphJson);
    for (nlohmann::json& n : graph["nodes"]) {
        if (n["type"] == "Interpolate") n["params"]["keepClearance"] = false;
    }
    return graph.dump();
}

GroomData deformedGroom(const std::string& graphJson, float bend = 0.6f);

TEST(GpuPreview, EngagesOnTheGraphsGroomsStartFrom) {
    // The default graph (its Resample even) and hair_basics run on the GPU from Interpolate, keeping
    // its strands clear of the scalp (the scalp's field as tables) or not. A Clump picking its own
    // centers too (from the strands the run moves).
    for (const bool clearance : {true, false}) {
        for (const char* which : {"default", "hairBasics", "clumpAlone"}) {
            SCOPED_TRACE(std::string(which) + (clearance ? " keeping clearance" : ""));
            GroomSession session(groom(clearance ? startingGraph(which) : withoutClearance(startingGraph(which))));
            ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
            Prepared p;
            ASSERT_TRUE(prepareRun(session, {}, "", &p)) << p.why;
            ASSERT_EQ(p.chain.stages.front().kind, gpu::StageKind::Interpolate);
            const gpu::InterpolateInputs& in = *p.run.inputs.front().interpolate;
            EXPECT_EQ(in.clearance != nullptr, clearance);
            if (clearance) EXPECT_TRUE(in.clearance->tables && in.clearance->tables->size() > 1000u);
            const bool clumps = std::string(which) != "default";
            EXPECT_EQ(p.chain.node(gpu::StageKind::Clump) != 0, clumps);
            EXPECT_TRUE(p.chain.node(gpu::StageKind::Noise) && p.chain.node(gpu::StageKind::Width));
        }
    }
    // Over a scalp deformed away from its rest pose, the walk asks the rest pose's field through
    // each vertex's frame: the GPU runs Interpolate too, with the rest field's tables and the walk.
    // Where the strands come near the scalp's degenerate faces (the sphere's poles: no frame at
    // their vertices, where the walk asks the mesh), the CPU's.
    GroomData deformed = groom(startingGraph("default"));
    auto moved = std::make_shared<ScalpMesh>(*deformed.scalp);
    std::vector<Vec3f> rest = moved->points();
    for (Vec3f& q : rest) q = q * 0.98f;
    moved->setRestPoints(rest);
    deformed.scalp = moved;
    GroomSession session(deformed);
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    Prepared p;
    EXPECT_FALSE(prepareRun(session, {}, "", &p));
    EXPECT_NE(p.why.find("degenerate"), std::string::npos) << p.why;
    GroomSession capless(deformedGroom(startingGraph("default")));
    ASSERT_TRUE(capless.evaluate(ViewRequest{}).strands);
    Prepared q;
    ASSERT_TRUE(prepareRun(capless, {}, "", &q)) << q.why;
    ASSERT_NE(q.chain.node(gpu::StageKind::Interpolate), 0u);
    const gpu::InterpolateInputs& in = *q.run.inputs.front().interpolate;
    ASSERT_TRUE(in.clearance);
    EXPECT_TRUE(in.clearance->walk && in.clearance->rest && in.cursors);
}

// A sphere scalp deformed away from its rest pose: the rest pose sheared, squashed, turned and
// moved a little from the deformed points (the strands and guides on the deformed one).
// (Without the sphere's pole caps, whose triangles are degenerate: their vertices have no frame,
// where the CPU's walk asks the mesh.)
GroomData deformedGroom(const std::string& graphJson, float bend) {
    GroomData data;
    data.graphJson = graphJson;
    const int rings = 24, segments = 48;
    const MeshPtr sphere = test::makeSphere(rings, segments, 4.0f);
    std::vector<ScalpMesh::Triangle> tris;
    for (std::size_t t = 0; t < sphere->triangleCount(); ++t) {
        const std::size_t row = t / (2 * segments);
        if (row != 0 && row + 1 != static_cast<std::size_t>(rings)) tris.push_back(sphere->triangles()[t]);
    }
    data.scalp = std::make_shared<ScalpMesh>(sphere->points(), tris);
    data.guides = guideSet(*data.scalp, bend);
    auto moved = std::make_shared<ScalpMesh>(*data.scalp);
    std::vector<Vec3f> rest = moved->points();
    const float c = std::cos(0.15f), s = std::sin(0.15f);
    for (Vec3f& q : rest) {
        const Vec3f sheared(q.x * 0.97f + 0.06f * q.y, q.y * 1.03f, q.z * 0.98f);
        q = Vec3f(c * sheared.x - s * sheared.z, sheared.y, s * sheared.x + c * sheared.z) + Vec3f(0.3f, -0.2f, 0.1f);
    }
    moved->setRestPoints(rest);
    data.scalp = moved;
    return data;
}

TEST_P(LookEngineTest, InterpolateKeepsClearanceOverADeformedScalpAsTheCpuDoes) {
    // The walk through the rest pose's field, each point taken there by the frame of the deformed
    // scalp's vertex nearest it (walked over the edges from its root's), its push turned back: on
    // the graphs a groom starts from, then a dab bending the guides (the walk again for the strands
    // of the guides it changed), against the CPU.
    for (const char* which : {"default", "hairBasics"}) {
        SCOPED_TRACE(which);
        GroomData data = deformedGroom(startingGraph(which));
        GroomSession session(data);
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        {
            const Prepared p = prepare(session, {}, "interpolate");
            ASSERT_FALSE(p.run.inputs.empty());
            ASSERT_TRUE(p.run.inputs.front().interpolate && p.run.inputs.front().interpolate->clearance);
            ASSERT_TRUE(p.run.inputs.front().interpolate->clearance->walk);
        }
        expectClose(runAndCompare(session, {}, session, "interpolate"));
        const StrandsPtr before = session.last().strands;
        const GuidesPtr bent = guideSet(*data.scalp, 1.4f);
        session.setGuides(bent);
        ViewRequest dab;
        dab.stopAt = "interpolate";
        ASSERT_EQ(session.evaluate(dab).strands, before);
        GroomData after = data;
        after.guides = bent;
        GroomSession fresh(after);
        ASSERT_TRUE(fresh.evaluate(ViewRequest{}).strands);
        expectClose(runAndCompare(session, {}, fresh, "interpolate"));
    }
}
TEST_P(LookEngineTest, TheGraphsGroomsStartFromGiveTheCpusResults) {
    for (const bool clearance : {true, false}) {
        for (const char* which : {"default", "hairBasics", "clumpAlone"}) {
            SCOPED_TRACE(std::string(which) + (clearance ? " keeping clearance" : ""));
            GroomSession session(groom(clearance ? startingGraph(which) : withoutClearance(startingGraph(which))));
            ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
            expectClose(runAndCompare(session, {}, session));
        }
    }
}

TEST_P(LookEngineTest, KernelsGiveTheCpuOperatorsResults) {
    // From Noise (Phase 1's chain), and from Clump with its centers as the CPU chose and made them.
    GroomSession session(groom(graph(0.3)));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    expectClose(runAndCompare(session, {}, session, "noise"));
    expectClose(runAndCompare(session, {}, session, "clump"));
}

TEST_P(LookEngineTest, ADraggedValueGivesWhatTheCpuEvaluatesForIt) {
    // The drag's values over the graph's, against a CPU evaluation of the graph holding them.
    GroomSession session(groom(graph(0.3)));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    GroomSession committed(groom(graph(0.55)));
    ASSERT_TRUE(committed.evaluate(ViewRequest{}).strands);
    expectClose(runAndCompare(session, {{"noise", {{"amplitude", 0.55}}}}, committed));
}

TEST_P(LookEngineTest, EachKernelGivesItsOperatorsResult) {
    GroomSession session(groom(graph(0.3)));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    const GraphDocument& doc = *session.viewDocument();
    // Noise alone, then Noise and Curl, then Width alone: each against its node's cached result;
    // Noise and the frames a thread per point (the strands all have 16 points) and a thread per
    // strand.
    const Prepared p = prepare(session, {}, "noise");
    auto engine = backend_->engine();
    engine->setBase(p.base, session.last().scalp.get());
    const StrandSet& noiseOut = *valueAs<StrandsPtr>(doc.graph.cachedValue(doc.nodesByName.at("noise")));
    const StrandSet& curlOut = *valueAs<StrandsPtr>(doc.graph.cachedValue(doc.nodesByName.at("curl")));
    const StrandLines lines(p.output, session.last().scalp, true, nullptr);
    const std::size_t count = p.output->pointCount();
    std::vector<Vec3f> normals(count), tangents(count);
    lines.writeFrames(&normals[0].x, &tangents[0].x);
    const std::vector<Vec3f> bitangents = cpuBitangents(normals, tangents);
    auto only = [&](std::size_t stages) {
        gpu::LookRun run = p.run;
        run.params.stages.resize(stages);
        run.fields.stages.resize(stages);
        run.inputs.resize(stages);
        return run;
    };
    for (const bool pointThreads : {true, false}) {
        engine->setPointThreads(pointThreads);
        engine->run(only(1), false);
        const gpu::PointError n = gpu::compareVectors(engine->readPositions(), noiseOut.positions());
        engine->run(only(2), true);
        const gpu::PointError c = gpu::compareVectors(engine->readPositions(), curlOut.positions());
        std::vector<Vec3f> got[3];
        engine->readFrames(&got[0], &got[1], &got[2]);
        const gpu::PointError f = gpu::compareVectors(got[0], normals);
        std::printf("[ %-8s ] %s: noise max %.3g mean %.3g; noise + curl max %.3g mean %.3g; normals max %.3g\n",
                    GetParam().c_str(), pointThreads ? "a thread a point" : "a thread a strand", n.max, n.mean, c.max, c.mean,
                    f.max);
        EXPECT_LT(n.max, kPointTolerance);
        EXPECT_LT(c.max, kPointTolerance);
        EXPECT_LT(f.max, kFrameTolerance);
        EXPECT_LT(gpu::compareVectors(got[1], tangents).max, kFrameTolerance);
        EXPECT_LT(gpu::compareVectors(got[2], bitangents).max, kBitangentTolerance);
    }
    // Width alone: the points as they came (Curl's), the widths by the profile.
    const Prepared w = prepare(session, {}, "width");
    ASSERT_EQ(w.chain.stages.size(), 1u);
    engine->setBase(w.base, session.last().scalp.get());
    engine->run(w.run, false);
    EXPECT_EQ(gpu::compareVectors(engine->readPositions(), w.base->positions()).max, 0.0);
}

TEST_P(LookEngineTest, APreviewHoldingHiddenCoreStrandsGivesTheCpusShownStrands) {
    // A preview under its core share (preview_core.h: 8k strands, all core, at 0.5) holds core
    // strands it does not show: the GPU runs every strand (Clump's centers among the hidden ones)
    // and writes the shown ones alone, as the viewport's buffers hold them, into the host's buffers
    // in place or copied: what the CPU gives, a drag's value too.
    ViewRequest half;
    half.density = 0.5f;
    GroomSession session(groom(graph(0.3)));
    const StrandsPtr shown = session.evaluate(half).strands;
    ASSERT_TRUE(shown);
    GroomSession full(groom(graph(0.3)));
    ASSERT_GT(full.evaluate(ViewRequest{}).strands->strandCount(), shown->strandCount() * 3 / 2);
    expectClose(runAndCompare(session, {}, session, "noise"));
    expectClose(runAndCompare(session, {}, session, "clump"));
    GroomSession committed(groom(graph(0.55)));
    ASSERT_TRUE(committed.evaluate(half).strands);
    expectClose(runAndCompare(session, {{"noise", {{"amplitude", 0.55}}}}, committed));

    const Prepared p = prepare(session);
    EXPECT_EQ(p.output, shown);
    EXPECT_GT(p.base->strandCount(), shown->strandCount());
    auto engine = backend_->engine();
    engine->setBase(p.base, session.last().scalp.get());
    engine->run(p.run, true);
    const std::vector<Vec3f> positions = engine->readPositions();
    ASSERT_EQ(positions.size(), shown->pointCount());
    std::vector<Vec3f> frames[3];
    engine->readFrames(&frames[0], &frames[1], &frames[2]);
    const std::size_t n = shown->pointCount();
    void* buffers[4];
    for (auto& b : buffers) b = backend_->displayBuffer(n * 12);
    engine->run(p.run, true, {buffers[0], buffers[1], buffers[2], buffers[3]});
    EXPECT_TRUE(engine->wroteInPlace());
    EXPECT_EQ(gpu::compareVectors(backend_->read(buffers[0], n), positions).max, 0.0);
    for (int i = 0; i < 3; ++i) EXPECT_EQ(gpu::compareVectors(backend_->read(buffers[i + 1], n), frames[i]).max, 0.0);
    engine->dropTargets();
    for (void* b : buffers) backend_->release(b);
    void* plain[4] = {backend_->plainBuffer(n * 12), backend_->plainBuffer(n * 12), backend_->plainBuffer(n * 12),
                      backend_->plainBuffer(n * 12)};
    if (plain[0]) {
        engine->run(p.run, true, {plain[0], plain[1], plain[2], plain[3]});
        EXPECT_FALSE(engine->wroteInPlace());
        EXPECT_EQ(gpu::compareVectors(backend_->read(plain[0], n), positions).max, 0.0);
        for (int i = 0; i < 3; ++i) EXPECT_EQ(gpu::compareVectors(backend_->read(plain[i + 1], n), frames[i]).max, 0.0);
        engine->dropTargets();
    }
    for (void* b : plain) backend_->release(b);
}

TEST_P(LookEngineTest, WritesTheHostsBuffersInPlace) {
    // The results written straight into vertex buffers made for it (what the viewport draws), the
    // same as in the engine's own; buffers it cannot write are copied into on the GPU.
    GroomSession session(groom(graph(0.3)));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    const Prepared p = prepare(session);
    auto engine = backend_->engine();
    engine->setBase(p.base, session.last().scalp.get());
    engine->run(p.run, true);
    const std::vector<Vec3f> positions = engine->readPositions();
    std::vector<Vec3f> frames[3];
    engine->readFrames(&frames[0], &frames[1], &frames[2]);
    const std::size_t n = p.base->pointCount();
    const std::size_t ownBytes = engine->videoBytes();

    void* buffers[4];
    for (auto& b : buffers) b = backend_->displayBuffer(n * 12);
    engine->run(p.run, true, {buffers[0], buffers[1], buffers[2], buffers[3]});
    EXPECT_TRUE(engine->wroteInPlace());
    EXPECT_EQ(gpu::compareVectors(backend_->read(buffers[0], n), positions).max, 0.0);
    for (int i = 0; i < 3; ++i) EXPECT_EQ(gpu::compareVectors(backend_->read(buffers[i + 1], n), frames[i]).max, 0.0);
    EXPECT_EQ(gpu::compareVectors(engine->readPositions(), positions).max, 0.0);  // read from the target
    std::printf("[ %-8s ] engine's own buffers %.1f MB, with the results in the host's %.1f MB\n", GetParam().c_str(),
                static_cast<double>(ownBytes) / 1e6, static_cast<double>(engine->videoBytes()) / 1e6);
    engine->dropTargets();
    for (void* b : buffers) backend_->release(b);

    void* plain[4] = {backend_->plainBuffer(n * 12), backend_->plainBuffer(n * 12), backend_->plainBuffer(n * 12),
                      backend_->plainBuffer(n * 12)};
    if (plain[0]) {
        engine->run(p.run, true, {plain[0], plain[1], plain[2], plain[3]});
        EXPECT_FALSE(engine->wroteInPlace());
        EXPECT_EQ(gpu::compareVectors(backend_->read(plain[0], n), positions).max, 0.0);
        for (int i = 0; i < 3; ++i) EXPECT_EQ(gpu::compareVectors(backend_->read(plain[i + 1], n), frames[i]).max, 0.0);
        engine->dropTargets();
    }
    for (void* b : plain) backend_->release(b);
}

TEST_P(LookEngineTest, WritesWidthsIntoTheLinesUvs) {
    // The lines' uvs (u, width: StrandLines::writeUvs) as the host wrote them, then a run with a
    // Width: its widths in each point's second float, the u left alone; the shown strands' alone
    // for a preview holding hidden core strands. Without a uvs target nothing changes.
    for (const float density : {1.0f, 0.5f}) {
        SCOPED_TRACE(density);
        ViewRequest request;
        request.density = density;
        LookGraph g;
        g.width = R"("params": {"width": 0.02, "profile": {"keys": [[0.0, 1.0], [1.0, 0.0]]}})";
        GroomSession session(groom(g.json()));
        ASSERT_TRUE(session.evaluate(request).strands);
        const Prepared p = prepare(session, {{"width", {{"width", 0.03}}}});
        const std::size_t n = p.output->pointCount();
        // The CPU's uvs of the output (its own widths), then each width as 1.5 times it (the drag).
        const StrandLines lines(p.output, nullptr, false, nullptr);
        std::vector<float> written(n * 2);
        lines.writeUvs(written.data(), 2);
        void* uvs = backend_->displayBuffer(n * 8);
        backend_->fill(uvs, written.data(), n * 8);
        auto engine = backend_->engine();
        engine->setBase(p.base, session.last().scalp.get());
        gpu::LookTargets targets;
        targets.uvs = uvs;
        engine->run(p.run, false, targets);
        EXPECT_TRUE(engine->wroteUvs());
        const std::vector<float> got = backend_->readFloats(uvs, n * 2);
        double uOff = 0, widthOff = 0;
        for (std::size_t i = 0; i < n; ++i) {
            uOff = std::max(uOff, static_cast<double>(std::abs(got[i * 2] - written[i * 2])));
            widthOff = std::max(widthOff, static_cast<double>(std::abs(got[i * 2 + 1] - 1.5f * p.output->widths()[i])));
        }
        EXPECT_EQ(uOff, 0.0);
        EXPECT_LT(widthOff, 1e-6);
        engine->run(p.run, false);  // no uvs target: none written
        EXPECT_FALSE(engine->wroteUvs());
        engine->dropTargets();
        backend_->release(uvs);
    }
}

// The GPU's run of `graph` (with `overrides`) against the CPU's evaluation of `committed` (the same
// graph with the overrides' values): what a drag shows against what its release commits.
#define EXPECT_GPU_MATCHES(graphJson, overrides, committedJson)                         \
    do {                                                                               \
        GroomSession dragged(groom(graphJson));                                        \
        ASSERT_TRUE(dragged.evaluate(ViewRequest{}).strands) << dragged.last().errors.size(); \
        GroomSession committed(groom(committedJson));                                  \
        ASSERT_TRUE(committed.evaluate(ViewRequest{}).strands);                        \
        expectClose(runAndCompare(dragged, overrides, committed));                     \
    } while (0)
// ... of a node after the graph's Width (its run writes no widths).
#define EXPECT_GPU_POINTS_MATCH(graphJson, overrides, committedJson)                   \
    do {                                                                               \
        GroomSession dragged(groom(graphJson));                                        \
        ASSERT_TRUE(dragged.evaluate(ViewRequest{}).strands);                          \
        GroomSession committed(groom(committedJson));                                  \
        ASSERT_TRUE(committed.evaluate(ViewRequest{}).strands);                        \
        expectPointsClose(runAndCompare(dragged, overrides, committed));               \
    } while (0)

TEST_P(LookEngineTest, WorldSpaceNoise) {
    LookGraph g;
    g.noise = R"("params": {"amplitude": 0.3, "frequency": 1.5, "space": "world", "octaves": 3})";
    LookGraph c = g;
    c.noise = R"("params": {"amplitude": 0.45, "frequency": 1.5, "space": "world", "octaves": 3})";
    EXPECT_GPU_MATCHES(g.json(), (gpu::Overrides{{"noise", {{"amplitude", 0.45}}}}), c.json());
}

TEST_P(LookEngineTest, AnInstanceEndingTheGraphHandsTheGpusPointsOn) {
    // An Instance after the look (snow near the tips): a drag upstream runs on the GPU, its points
    // the CPU's for the dragged value, the instances kept as the last evaluation placed them.
    LookGraph g;
    g.after = "Instance";
    g.afterParams = R"("params": {"probability": 0.4, "tMin": 0.5, "scale": 0.05})";
    LookGraph c = g;
    c.noise = R"("params": {"amplitude": 0.45, "frequency": 1.0})";
    {
        GroomSession session(groom(g.json()));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        gpu::LookChain chain;
        std::string why;
        ASSERT_TRUE(gpu::findLookChain(*session.viewDocument(), &chain, &why)) << why;
        const NodeId instance = session.viewDocument()->nodesByName.at("after");
        EXPECT_NE(std::find(chain.passOn.begin(), chain.passOn.end(), instance), chain.passOn.end());
        StrandsPtr base, output;
        EXPECT_TRUE(gpu::lookStrands(*session.viewDocument(), chain, &base, &output, &why)) << why;
        ASSERT_TRUE(output && output->instances());
        EXPECT_FALSE(output->instances()->empty());
    }
    EXPECT_GPU_POINTS_MATCH(g.json(), (gpu::Overrides{{"noise", {{"amplitude", 0.45}}}}), c.json());
}

TEST_P(LookEngineTest, WidthRandomness) {
    // Each strand's width varied by its draw, from Width alone; then a drag of the randomness.
    LookGraph g;
    g.width = R"("params": {"width": 0.01, "randomness": 0.4, "seed": 3})";
    GroomSession session(groom(g.json()));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    expectClose(runAndCompare(session, {}, session, "width"));
    LookGraph c = g;
    c.width = R"("params": {"width": 0.01, "randomness": 0.7, "seed": 3})";
    EXPECT_GPU_MATCHES(g.json(), (gpu::Overrides{{"width", {{"randomness", 0.7}}}}), c.json());
}

TEST_P(LookEngineTest, KinkyNoise) {
    // Kinky in strand space (a thread a point where the strands have one count) and world space (a
    // thread a strand: its arc length), from Noise; then a drag of its frequency.
    for (const std::string space : {"strand", "world"}) {
        LookGraph g;
        g.noise = R"("params": {"amplitude": 0.3, "frequency": 3.0, "mode": "kinky", "space": ")" + space + R"("})";
        GroomSession session(groom(g.json()));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        expectClose(runAndCompare(session, {}, session, "noise"));
    }
    LookGraph g;
    g.noise = R"("params": {"amplitude": 0.3, "frequency": 3.0, "mode": "kinky"})";
    LookGraph c = g;
    c.noise = R"("params": {"amplitude": 0.3, "frequency": 4.5, "mode": "kinky"})";
    EXPECT_GPU_MATCHES(g.json(), (gpu::Overrides{{"noise", {{"frequency", 4.5}}}}), c.json());
}

TEST_P(LookEngineTest, CurlWavesZigzagsAndPhase) {
    // A wave and a zigzag from a phase of their own (half of a turn's randomness), from Noise; then
    // a drag of the phase.
    for (const std::string shape : {"wave", "zigzag"}) {
        LookGraph g;
        g.curl = R"("params": {"radius": 0.1, "frequency": 0.8, "shape": ")" + shape + R"(", "phase": 0.2, "phaseRandom": 0.5})";
        GroomSession session(groom(g.json()));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        expectClose(runAndCompare(session, {}, session, "noise"));
    }
    LookGraph g;
    g.curl = R"("params": {"radius": 0.1, "frequency": 0.8, "phase": 0.2, "phaseRandom": 0.0})";
    LookGraph c = g;
    c.curl = R"("params": {"radius": 0.1, "frequency": 0.8, "phase": 0.45, "phaseRandom": 0.0})";
    EXPECT_GPU_MATCHES(g.json(), (gpu::Overrides{{"curl", {{"phase", 0.45}}}}), c.json());
}

TEST_P(LookEngineTest, CurlSpacingAsTheCpuDoes) {
    // Curls tightening toward the tip (frequencyProfile: smooth, and straight with a key between
    // the axis's samples) and spaced at random along each strand (spacingRandom), each strand on
    // its own and by clump: a helix, a wave, a zigzag and a coil (on the strands' unit widths), from
    // Noise and from Clump; then drags of spacingRandom and of frequencyProfile.
    const std::string profiles[] = {R"([[0, 0.2], [0.5, 0.55], [1, 1.0]])", R"({"keys": [[0, 0.3], [0.37, 1.0], [1, 0.6]], "linear": true})"};
    for (const char* coherence : {"strand", "clump"}) {
        for (const char* shape : {"helix", "wave", "zigzag", "coil"}) {
            for (const std::string& profile : profiles) {
                SCOPED_TRACE(std::string(coherence) + " " + shape + " " + profile);
                LookGraph g;
                g.curl = std::string(R"("params": {"radius": 0.1, "coilWidths": 0.08, "frequency": 1.6, "randomness": 0.3, "spacingRandom": 0.7, "coherence": ")") +
                         coherence + R"(", "shape": ")" + shape + R"(", "frequencyProfile": )" + profile + "}";
                GroomSession session(groom(g.json()));
                ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
                expectClose(runAndCompare(session, {}, session, "noise"));
                expectClose(runAndCompare(session, {}, session, "curl"));
            }
        }
    }
    LookGraph g;
    g.curl = R"("params": {"radius": 0.1, "frequency": 0.8, "spacingRandom": 0.2})";
    LookGraph c = g;
    c.curl = R"("params": {"radius": 0.1, "frequency": 0.8, "spacingRandom": 0.9})";
    EXPECT_GPU_MATCHES(g.json(), (gpu::Overrides{{"curl", {{"spacingRandom", 0.9}}}}), c.json());
    LookGraph tightened = g;
    tightened.curl = R"("params": {"radius": 0.1, "frequency": 0.8, "spacingRandom": 0.2, "frequencyProfile": [[0, 0.25], [1, 1.0]]})";
    EXPECT_GPU_MATCHES(g.json(), (gpu::Overrides{{"curl", {{"frequencyProfile", Ramp{{0.0f, 0.25f}, {1.0f, 1.0f}}}}}}), tightened.json());
}

TEST_P(LookEngineTest, WeightedCurlAsTheCpuDoes) {
    // Weighted curls (Curl's weight: the turns spread where hair hangs below them, the radius
    // narrowing to keep each turn's wire length) run on the GPU: each strand on its own and by
    // clump, a helix and a coil (on the strands' unit widths), plain and with a frequencyProfile and
    // spacingRandom, from Noise and from Curl.
    const char* spacings[] = {"", R"(, "spacingRandom": 0.6, "frequencyProfile": [[0, 0.3], [0.37, 1.0], [1, 0.7]])"};
    for (const char* coherence : {"strand", "clump"}) {
        for (const char* shape : {"helix", "coil"}) {
            for (const char* spacing : spacings) {
                SCOPED_TRACE(std::string(coherence) + " " + shape + spacing);
                LookGraph g;
                g.curl = std::string(R"("params": {"radius": 0.3, "coilWidths": 0.15, "frequency": 1.2, "randomness": 0.3, "weight": 1.5, "coherence": ")") +
                         coherence + R"(", "shape": ")" + shape + "\"" + spacing + "}";
                GroomSession session(groom(g.json()));
                ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
                expectClose(runAndCompare(session, {}, session, "noise"));
                expectClose(runAndCompare(session, {}, session, "curl"));
            }
        }
    }

    // Overstretched (a weight far past what the curls' turns can take): the curls hanging below
    // the most turns go straight, on the GPU as on the CPU.
    {
        LookGraph g;
        g.curl = R"("params": {"radius": 0.1, "frequency": 0.8, "weight": 6.0})";
        GroomSession session(groom(g.json()));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const GraphDocument& doc = *session.viewDocument();
        const StrandsPtr in = valueAs<StrandsPtr>(doc.graph.cachedValue(doc.nodesByName.at("noise")));
        const StrandsPtr out = valueAs<StrandsPtr>(doc.graph.cachedValue(doc.nodesByName.at("curl")));
        ASSERT_TRUE(in && out);
        ASSERT_EQ(in->pointCount(), out->pointCount());
        std::size_t straight = 0;  // points past the root the curl left where they came in
        for (std::size_t s = 0; s < in->strandCount(); ++s) {
            for (std::size_t i = 1; i < in->strandPointCount(s); ++i) straight += in->strandPoints(s)[i] == out->strandPoints(s)[i];
        }
        EXPECT_GT(straight, 1000u);
        expectClose(runAndCompare(session, {}, session, "curl"));
    }

    // Strands pointing up (against weightDirection) carry nothing: their curls as unweighted, on
    // the CPU exactly and on the GPU within its rounding; the hanging ones change.
    {
        LookGraph g, plain;
        g.curl = R"("params": {"radius": 0.2, "frequency": 1.0, "weight": 2.0})";
        plain.curl = R"("params": {"radius": 0.2, "frequency": 1.0})";
        GroomSession session(groom(g.json())), unweighted(groom(plain.json()));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        ASSERT_TRUE(unweighted.evaluate(ViewRequest{}).strands);
        const Prepared p = prepare(session, {}, "curl");
        auto engine = backend_->engine();
        ASSERT_TRUE(engine->setBase(p.base, session.last().scalp.get()));
        engine->run(p.run, true);
        const std::vector<Vec3f> gpuPoints = engine->readPositions();
        const StrandsPtr weighed = session.last().strands, want = unweighted.last().strands;
        ASSERT_EQ(gpuPoints.size(), want->pointCount());
        std::size_t up = 0, hanging = 0;
        for (std::size_t s = 0; s < want->strandCount(); ++s) {
            const std::size_t begin = want->strandBegin(s), n = want->strandPointCount(s);
            float cpuApart = 0.0f, gpuApart = 0.0f;
            for (std::size_t i = 0; i < n; ++i) {
                cpuApart = std::max(cpuApart, (weighed->positions()[begin + i] - want->positions()[begin + i]).length());
                gpuApart = std::max(gpuApart, (gpuPoints[begin + i] - want->positions()[begin + i]).length());
            }
            if (want->positions()[begin].y > 3.0f) {
                ++up;
                EXPECT_EQ(cpuApart, 0.0f) << "strand " << s;
                EXPECT_LT(gpuApart, kPointTolerance) << "strand " << s;
            } else if (want->positions()[begin].y < -3.0f) {
                hanging += gpuApart > 0.05f;
            }
        }
        EXPECT_GT(up, 50u);
        EXPECT_GT(hanging, 50u);
    }

    // Drags: weight from 0 (the curl as before) and from another weight, and weightDirection.
    LookGraph g;
    g.curl = R"("params": {"radius": 0.2, "frequency": 1.0})";
    LookGraph c = g;
    c.curl = R"("params": {"radius": 0.2, "frequency": 1.0, "weight": 1.2})";
    EXPECT_GPU_MATCHES(g.json(), (gpu::Overrides{{"curl", {{"weight", 1.2}}}}), c.json());
    LookGraph some = g;
    some.curl = R"("params": {"radius": 0.2, "frequency": 1.0, "weight": 0.4, "coherence": "clump"})";
    LookGraph more = g;
    more.curl = R"("params": {"radius": 0.2, "frequency": 1.0, "weight": 2.5, "coherence": "clump"})";
    EXPECT_GPU_MATCHES(some.json(), (gpu::Overrides{{"curl", {{"weight", 2.5}}}}), more.json());
    LookGraph down = g, across = g;
    down.curl = R"("params": {"radius": 0.2, "frequency": 1.0, "weight": 0.8})";
    across.curl = R"("params": {"radius": 0.2, "frequency": 1.0, "weight": 0.8, "weightDirection": [1, -1, 0]})";
    EXPECT_GPU_MATCHES(down.json(), (gpu::Overrides{{"curl", {{"weightDirection", Vec3f(1.0f, -1.0f, 0.0f)}}}}), across.json());
    // Each a param the GPU previews, a vector for the vector.
    GroomSession session(groom(down.json()));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    std::string why;
    EXPECT_TRUE(gpu::previewableOverrides(*session.viewDocument(), {{"curl", {{"weight", 0.5}}}}, &why)) << why;
    EXPECT_TRUE(gpu::previewableOverrides(*session.viewDocument(), {{"curl", {{"weightDirection", Vec3f(0.0f, 0.0f, 1.0f)}}}}, &why)) << why;
    EXPECT_FALSE(gpu::previewableOverrides(*session.viewDocument(), {{"curl", {{"weightDirection", 1.0}}}}, &why));
    EXPECT_NE(why.find("takes a vector"), std::string::npos) << why;
    EXPECT_FALSE(gpu::previewableOverrides(*session.viewDocument(), {{"curl", {{"radius", Vec3f(0.0f, 0.0f, 1.0f)}}}}, &why));
}

TEST_P(LookEngineTest, WeightedCurlAfterADab) {
    // A brush dab's guides under a weighted Curl (each strand's and by clump): evaluated up to
    // Interpolate alone, the GPU runs on to the output as a fresh CPU session of those guides.
    for (const char* coherence : {"strand", "clump"}) {
        SCOPED_TRACE(coherence);
        LookGraph g;
        g.guides = true;
        g.curl = std::string(R"("params": {"radius": 0.2, "frequency": 1.0, "weight": 1.5, "spacingRandom": 0.4, "coherence": ")") + coherence + "\"}";
        GroomData data = groom(g.json(), 0.6f);
        GroomSession session(data);
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const StrandsPtr before = session.last().strands;
        const GuidesPtr bent = guideSet(*data.scalp, 1.4f);
        session.setGuides(bent);
        ViewRequest dab;
        dab.stopAt = "interpolate";
        ASSERT_EQ(session.evaluate(dab).strands, before);
        GroomData after = data;
        after.guides = bent;
        GroomSession fresh(after);
        ASSERT_TRUE(fresh.evaluate(ViewRequest{}).strands);
        expectClose(runAndCompare(session, {}, fresh, "interpolate"));
    }
}

TEST_P(LookEngineTest, MappedParamsPerStrandAndPerPoint) {
    // Noise's amplitude by an expression per strand, its fraction by another; Curl's radius per
    // point (by point index: it reads no positions); Width's by t (arc length: read on the points
    // Width takes). From Noise (the first stage the GPU changes): the graph's own values.
    LookGraph g;
    g.noise = R"("params": {"amplitude": {"expression": "0.2 + 0.4 * rand", "min": 0.0, "max": 1.0}, "frequency": 1.0,
                            "fraction": {"expression": "u", "min": 0.3, "max": 1.0}})";
    g.curl = R"("params": {"radius": {"expression": "0.05 + 0.1 * pt", "min": 0.0, "max": 1.0}, "frequency": 0.8})";
    g.width = R"("params": {"width": {"expression": "0.02 - 0.015 * t", "min": 0.0, "max": 1.0}})";
    GroomSession session(groom(g.json()));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    Prepared from;
    EXPECT_FALSE(prepareRun(session, {}, "noise", &from));  // Width's t moves with what runs before it
    EXPECT_NE(from.why.find("width.width"), std::string::npos) << from.why;
    expectClose(runAndCompare(session, {}, session, "width"));
    g.width = R"("params": {"width": {"expression": "0.01 + 0.01 * rand", "min": 0.0, "max": 1.0}})";
    GroomSession plain(groom(g.json()));
    ASSERT_TRUE(plain.evaluate(ViewRequest{}).strands);
    expectClose(runAndCompare(plain, {}, plain, "noise"));
}

TEST_P(LookEngineTest, ADragKeepsTheMaps) {
    // A number dragged while other params are maps that read no positions: the maps' values as
    // read once, the dragged number's.
    LookGraph g;
    g.noise = R"("params": {"amplitude": {"expression": "0.2 + 0.4 * rand", "min": 0.0, "max": 1.0}, "frequency": 1.0})";
    g.curl = R"("params": {"radius": {"expression": "0.05 + 0.1 * pt", "min": 0.0, "max": 1.0}, "frequency": 0.8})";
    g.width = R"("params": {"width": {"expression": "0.01 + 0.01 * rand", "min": 0.0, "max": 1.0}})";
    LookGraph c = g;
    c.curl = R"("params": {"radius": {"expression": "0.05 + 0.1 * pt", "min": 0.0, "max": 1.0}, "frequency": 1.3})";
    EXPECT_GPU_MATCHES(g.json(), (gpu::Overrides{{"curl", {{"frequency", 1.3}}}}), c.json());
}

TEST_P(LookEngineTest, PaintedMapsAndTextures) {
    // Noise's amplitude from a painted vertex map (a color set), Curl's radius from a texture, read
    // at each root as the operators read them; a drag of Curl's frequency keeps both.
    const std::string texture = stripes(GetParam());
    std::string path = texture;
    std::replace(path.begin(), path.end(), '\\', '/');
    LookGraph g;
    g.noise = R"("params": {"amplitude": {"colorSet": "mask", "min": 0.0, "max": 0.6}, "frequency": 1.0})";
    g.curl = R"("params": {"radius": {"texture": ")" + path + R"(", "min": 0.02, "max": 0.2}, "frequency": 0.8})";
    LookGraph c = g;
    c.curl = R"("params": {"radius": {"texture": ")" + path + R"(", "min": 0.02, "max": 0.2}, "frequency": 1.2})";
    GroomSession dragged(paintedGroom(g.json()));
    ASSERT_TRUE(dragged.evaluate(ViewRequest{}).strands);
    ASSERT_TRUE(dragged.last().errors.empty()) << dragged.last().errors.front();
    GroomSession committed(paintedGroom(c.json()));
    ASSERT_TRUE(committed.evaluate(ViewRequest{}).strands);
    // From Noise: its amplitude and Curl's radius read at the roots.
    const Prepared p = prepare(dragged, {{"curl", {{"frequency", 1.2}}}}, "noise");
    EXPECT_EQ(p.run.fields.stages[0].field[0].mode, gpu::FieldValues::kPerStrand);
    EXPECT_EQ(p.run.fields.stages[1].field[0].mode, gpu::FieldValues::kPerStrand);
    EXPECT_FALSE(p.run.fields.stages[1].field[0].readsPositions);
    EXPECT_TRUE(p.run.fields.stages[1].field[0].fixed);
    expectClose(runAndCompare(dragged, {{"curl", {{"frequency", 1.2}}}}, committed, "noise"));
    std::remove(texture.c_str());
}

TEST_P(LookEngineTest, APluggedFieldTimesTheDraggedNumber) {
    // A field node plugged into Width's width: the CPU multiplies it by the param's own number,
    // the GPU by the dragged one.
    LookGraph g;
    g.width = R"("params": {"width": 0.02}, "paramInputs": ["width"])";
    g.nodes = R"(, {"name": "mask", "type": "FieldNoise", "params": {"frequency": 0.7, "min": 0.2, "max": 1.0}})";
    g.connections = R"(, {"from": "mask", "to": "width", "input": "width"})";
    LookGraph c = g;
    c.width = R"("params": {"width": 0.035}, "paramInputs": ["width"])";
    EXPECT_GPU_MATCHES(g.json(), (gpu::Overrides{{"width", {{"width", 0.035}}}}), c.json());
}

TEST_P(LookEngineTest, AValueInputSetsAParamTheGpuReads) {
    // A Value node setting Noise's octaves (a Value port): the GPU reads the node's params as the
    // CPU set them. A drag of the param it sets is the CPU's (the value wins there).
    LookGraph g;
    g.noise = R"("params": {"amplitude": 0.3, "frequency": 1.0, "octaves": 1}, "paramInputs": ["octaves"])";
    g.nodes = R"(, {"name": "octaves", "type": "Value", "params": {"kind": "integer", "integer": 4}})";
    g.connections = R"(, {"from": "octaves", "to": "noise", "input": "octaves"})";
    LookGraph c;
    c.noise = R"("params": {"amplitude": 0.4, "frequency": 1.0, "octaves": 4})";
    EXPECT_GPU_MATCHES(g.json(), (gpu::Overrides{{"noise", {{"amplitude", 0.4}}}}), c.json());
    GroomSession session(groom(g.json()));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    std::string why;
    EXPECT_FALSE(gpu::previewableOverrides(*session.viewDocument(), {{"noise", {{"octaves", 2.0}}}}, &why));
    EXPECT_NE(why.find("Value input"), std::string::npos) << why;
}

TEST_P(LookEngineTest, PluggedCurvesAndManyKeys) {
    // A Curve node plugged into Noise's profile, a profile of twelve keys on Curl.
    LookGraph g;
    g.noise = R"("params": {"amplitude": 0.3, "frequency": 1.0}, "paramInputs": ["profile"])";
    g.curl = R"("params": {"radius": 0.1, "frequency": 0.8, "profile": {"keys": [[0, 0], [0.1, 0.4], [0.2, 0.2], [0.3, 0.9],
        [0.4, 0.5], [0.5, 1.0], [0.6, 0.3], [0.7, 0.8], [0.8, 0.6], [0.9, 1.0], [0.95, 0.2], [1.0, 0.7]]}})";
    g.nodes = R"(, {"name": "shape", "type": "Curve", "params": {"curve": {"keys": [[0, 0], [0.3, 1], [1, 0.4]]}, "interpolation": "ease"}})";
    g.connections = R"(, {"from": "shape", "to": "noise", "input": "profile"})";
    LookGraph c = g;
    c.noise = R"("params": {"amplitude": 0.4, "frequency": 1.0}, "paramInputs": ["profile"])";
    EXPECT_GPU_MATCHES(g.json(), (gpu::Overrides{{"noise", {{"amplitude", 0.4}}}}), c.json());
}

TEST_P(LookEngineTest, StrandsOfDifferentLengths) {
    // Point counts that differ from strand to strand: a thread a strand and the profiles' keys (no
    // table), mapped values per point at each strand's own offsets.
    LookGraph g;
    g.uneven = true;
    g.curl = R"("params": {"radius": {"expression": "0.05 + 0.1 * pt", "min": 0.0, "max": 1.0}, "frequency": 0.8})";
    LookGraph c = g;
    c.noise = R"("params": {"amplitude": 0.5, "frequency": 1.0})";
    GroomSession check(groom(g.json()));
    ASSERT_TRUE(check.evaluate(ViewRequest{}).strands);
    EXPECT_EQ(check.last().strands->uniformPointCount(), 0u);
    EXPECT_GPU_MATCHES(g.json(), (gpu::Overrides{{"noise", {{"amplitude", 0.5}}}}), c.json());
}

TEST_P(LookEngineTest, StrandsOfDifferentLengthsRunAThreadAPointOrAStrand) {
    // Point counts that differ from strand to strand: Noise and the frames a thread a point (each
    // point finding its strand in the table made once per layout) and a thread a strand, both the
    // CPU's; and either dispatched as rows of a few groups (the 65535 groups a row lowered to 3).
    LookGraph g;
    g.uneven = true;
    g.noise = R"("params": {"amplitude": 0.3, "frequency": 1.2, "octaves": 3})";
    g.curl = R"("params": {"radius": {"expression": "0.05 + 0.1 * pt", "min": 0.0, "max": 1.0}, "frequency": 0.8})";
    LookGraph c = g;
    c.noise = R"("params": {"amplitude": 0.45, "frequency": 1.2, "octaves": 3})";
    for (const bool pointThreads : {true, false}) {
        for (const std::size_t limit : {gpu::LookEngine::kMaxGroups, std::size_t{3}}) {
            SCOPED_TRACE(std::string(pointThreads ? "a thread a point" : "a thread a strand") + (limit < 100 ? ", rows of 3 groups" : ""));
            configure_ = [&](gpu::LookEngine& e) {
                e.setPointThreads(pointThreads);
                e.setDispatchLimit(limit);
            };
            EXPECT_GPU_MATCHES(g.json(), (gpu::Overrides{{"noise", {{"amplitude", 0.45}}}}), c.json());
        }
    }
    configure_ = nullptr;
}

TEST_P(LookEngineTest, DispatchesPastAGroupRowRunAsRows) {
    // A dispatch of more groups than a row takes (65535: past 16.78M points a thread a point, 4.19M
    // strands a thread a strand) runs as rows of them, its last row's groups past the end leaving
    // (a whole group of them: ClearanceCS's, that read its window's points past the offsets' end and
    // wrote them back over the other groups' walks). The limit lowered to 2, 3 and 5 groups, every
    // kernel (Interpolate keeping clearance, Clump making its centers, Noise, Curl, Width, the
    // frames, a preview's gather of its shown strands) gives what one row gives, to the bit; strands
    // of one point count and of several, at several counts: last rows of every size, strand and
    // point counts that are not a multiple of a group (the last group part full). (runAndCompare
    // checks every other test's graph as rows of 3 groups.)
    LookGraph uneven;
    uneven.uneven = true;
    auto scattered = [](double density) {
        nlohmann::json g = nlohmann::json::parse(startingGraph("default"));
        for (nlohmann::json& n : g["nodes"]) {
            if (n["type"] == "Scatter") n["params"]["density"] = density;
        }
        return g.dump();
    };
    const std::pair<std::string, std::string> graphs[] = {{"default", startingGraph("default")},
                                                          {"default at density 45", scattered(45.0)},
                                                          {"default at density 57", scattered(57.0)},
                                                          {"hairBasics", startingGraph("hairBasics")},
                                                          {"uneven", uneven.json()}};
    const std::size_t limits[] = {2, 3, 5};
    bool partStrandGroup = false, partPointGroup = false;  // a run whose last group is part full
    std::set<std::pair<std::size_t, std::size_t>> lastRows;  // (limit, groups in the last row) run
    for (const auto& [which, json] : graphs) {
        for (const float density : {1.0f, 0.5f}) {  // all shown, then a preview holding hidden core strands
            SCOPED_TRACE(which + (density < 1.0f ? ", hidden core strands" : ""));
            GroomSession session(groom(json));
            ViewRequest request;
            request.density = density;
            ASSERT_TRUE(session.evaluate(request).strands);
            const Prepared p = prepare(session);
            ASSERT_FALSE(p.chain.stages.empty());
            auto engine = backend_->engine();
            engine->setBase(p.base, session.last().scalp.get());
            struct Results {
                std::vector<Vec3f> positions, frames[3];
                std::vector<float> widths;
            };
            auto runOnce = [&](std::size_t limit) {
                engine->setDispatchLimit(limit);
                engine->run(p.run, true);
                Results r;
                r.positions = engine->readPositions();
                engine->readFrames(&r.frames[0], &r.frames[1], &r.frames[2]);
                r.widths = engine->readWidths();
                return r;
            };
            const Results one = runOnce(gpu::LookEngine::kMaxGroups);
            ASSERT_GT(one.positions.size(), 1000u);
            const std::size_t strands = p.base->strandCount(), points = p.base->pointCount();
            partStrandGroup = partStrandGroup || strands % 64 != 0;
            partPointGroup = partPointGroup || points % 256 != 0;
            for (const std::size_t limit : limits) {
                SCOPED_TRACE("rows of " + std::to_string(limit) + " groups, " + std::to_string(strands) + " strands, " +
                             std::to_string(points) + " points");
                for (const std::size_t groups : {(strands + 63) / 64, (points + 255) / 256}) {
                    lastRows.insert({limit, groups % limit == 0 ? limit : groups % limit});
                }
                const Results rows = runOnce(limit);
                EXPECT_TRUE(sameBits(one.positions, rows.positions));
                for (int i = 0; i < 3; ++i) EXPECT_TRUE(sameBits(one.frames[i], rows.frames[i])) << "frames " << i;
                EXPECT_TRUE(sameBits(one.widths, rows.widths));
            }
            // And the CPU's (the shown strands').
            EXPECT_LT(gpu::compareVectors(one.positions, p.output->positions()).max, kPointTolerance);
        }
    }
    EXPECT_TRUE(partStrandGroup && partPointGroup);
    for (const std::size_t limit : limits) {
        for (std::size_t last = 1; last <= limit; ++last) EXPECT_TRUE(lastRows.count({limit, last})) << limit << " groups a row, " << last << " in the last";
    }
}
TEST_P(LookEngineTest, AMillionRootGridTakesWidthDrags) {
    // A tester's crash report: Scatter's uvGrid at 1000 x 1000 (a million roots), then the Width
    // node's width dragged (the GPU look preview's path). Thinned as the viewport shows it and in
    // full (17M points): every drag's widths written, each the dragged value at the root.
    nlohmann::json g = nlohmann::json::parse(defaultGraphJson());
    for (nlohmann::json& n : g["nodes"]) {
        if (n["type"] == "Scatter") {
            n["params"]["layout"] = "uvGrid";
            n["params"]["gridRows"] = 1000;
            n["params"]["gridColumns"] = 1000;
        }
    }
    for (const float density : {0.1f, 1.0f}) {
        SCOPED_TRACE(density);
        GroomSession session(paintedGroom(g.dump()));
        ViewRequest request;
        request.density = density;
        ASSERT_TRUE(session.evaluate(request).strands);
        const std::size_t points = session.last().strands->pointCount();
        EXPECT_GT(session.last().strands->strandCount(), density < 1.0f ? 50000u : 500000u);
        for (const double w : {0.02, 0.0, 0.5}) {
            const Prepared p = prepare(session, {{"width", {{"width", w}}}});
            auto engine = backend_->engine();
            engine->setBase(p.base, session.last().scalp.get());
            engine->run(p.run, true);
            const std::vector<float> widths = engine->readWidths();
            ASSERT_EQ(widths.size(), points);
            EXPECT_NEAR(widths.front(), w, 1e-6);
        }
    }
}

TEST(GpuPreview, AnOpenGLEngineKeepsItsKernelsCompiled) {
    // The kernels' program binaries kept (GROOMLAB_GL_CACHE: a directory of the test's own), a file
    // a kernel: the first engine's kernels compile (in the background where the driver can: a host
    // asks kernelsReady, a run waits) and are kept, the next context's load them, and both run
    // alike; a damaged file is compiled again. Engines of one context share them.
    GLBackend backend;
    if (!backend.available()) GTEST_SKIP() << "no OpenGL device";
    char dir[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    const std::filesystem::path cache =
        std::filesystem::path(dir) / ("groomlab_glcache_test_" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(cache);
    std::string kept;  // the variable as it was (empty: unset)
    char* before = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&before, &length, "GROOMLAB_GL_CACHE") == 0 && before) {
        kept = before;
        std::free(before);
    }
    _putenv_s("GROOMLAB_GL_CACHE", cache.string().c_str());
    GroomSession session(groom(graph(0.3)));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    const Prepared p = prepare(session);
    auto positions = [&](gpu::LookEngine& e) {
        e.setBase(p.base, session.last().scalp.get());
        e.run(p.run, true);
        return e.readPositions();
    };
    // Every kernel ready (asked now and then, as a host drawing does).
    auto allReady = [] {
        const auto start = std::chrono::steady_clock::now();
        while (!gpu::prepareGLLookKernels()) {
            if (std::chrono::steady_clock::now() - start > std::chrono::minutes(5)) return false;
            Sleep(10);
        }
        return true;
    };
    gpu::forgetGLLookKernels();
    auto first = backend.engine();
    std::string why;
    const bool readyAtOnce = first->kernelsReady(p.run, &why);
    std::printf("[ OpenGL   ] the first engine's kernels ready at once: %s (%s)\n", readyAtOnce ? "yes" : "no", why.c_str());
    EXPECT_EQ(readyAtOnce, why.empty());
    const std::vector<Vec3f> a = positions(*first);  // (waits for the kernels it runs)
    EXPECT_TRUE(first->kernelsReady(p.run));
    EXPECT_TRUE(gpu::glLookKernelsReady(p.run));
    ASSERT_TRUE(allReady());
    EXPECT_FALSE(first->kernelsFromCache());
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(cache)) files.push_back(e.path());
    ASSERT_EQ(files.size(), static_cast<std::size_t>(gpu::LookEngine::Kernel::Count));
    // Another engine of the context: the same programs.
    auto shared = backend.engine();
    EXPECT_TRUE(shared->kernelsReady(p.run));
    EXPECT_FALSE(shared->kernelsFromCache());
    first.reset();
    shared.reset();
    gpu::forgetGLLookKernels();  // (as a new context: loaded from the cache)
    auto second = backend.engine();
    ASSERT_TRUE(allReady());
    EXPECT_TRUE(second->kernelsFromCache());
    const std::vector<Vec3f> b = positions(*second);
    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(std::memcmp(a.data(), b.data(), a.size() * sizeof(Vec3f)), 0);
    second.reset();
    {
        std::ofstream damaged(files[0], std::ios::binary | std::ios::trunc);
        damaged << "GLPB not a binary";
    }
    gpu::forgetGLLookKernels();
    auto third = backend.engine();
    ASSERT_TRUE(allReady());
    EXPECT_FALSE(third->kernelsFromCache());
    EXPECT_EQ(positions(*third).size(), a.size());
    third.reset();
    gpu::forgetGLLookKernels();
    auto fourth = backend.engine();  // (the damaged one compiled again and kept)
    ASSERT_TRUE(allReady());
    EXPECT_TRUE(fourth->kernelsFromCache());
    fourth.reset();
    gpu::forgetGLLookKernels();
    _putenv_s("GROOMLAB_GL_CACHE", kept.c_str());
    std::error_code error;
    std::filesystem::remove_all(cache, error);
}

// ---- Clump, Length, Interpolate (Phase 2) -------------------------------------------------------

TEST_P(LookEngineTest, ClumpDragsGiveWhatTheCpuClumps) {
    // Clump's numbers dragged (strength, a lock radius): its centers as the CPU chose and
    // resampled them, its strength per point by point index.
    LookGraph g;
    g.clump = R"("params": {"strength": 0.8})";
    LookGraph c = g;
    c.clump = R"("params": {"strength": 0.55})";
    EXPECT_GPU_MATCHES(g.json(), (gpu::Overrides{{"clump", {{"strength", 0.55}}}}), c.json());
    LookGraph lock = g;
    lock.clump = R"("params": {"strength": 0.9, "lockRadius": 0.15})";
    LookGraph locked = g;
    locked.clump = R"("params": {"strength": 0.9, "lockRadius": 0.25})";
    EXPECT_GPU_MATCHES(lock.json(), (gpu::Overrides{{"clump", {{"lockRadius", 0.25}}}}), locked.json());
    LookGraph mapped = g;
    mapped.clump = R"("params": {"strength": {"expression": "0.3 + 0.05 * pt", "min": 0.0, "max": 1.0}})";
    EXPECT_GPU_MATCHES(mapped.json(), gpu::Overrides{}, mapped.json());
}

TEST_P(LookEngineTest, ANegativeClumpRepelsAsTheCpuDoes) {
    // A negative strength pushes the strands near each center away from it (off the skin, lengths
    // kept): as the graph holds it, dragged from a pull across 0 into a push, the repel radius
    // dragged, and a strength per point that turns negative toward the tips.
    LookGraph g;
    g.clump = R"("params": {"strength": -0.7, "repelRadius": 0.6})";
    EXPECT_GPU_MATCHES(g.json(), gpu::Overrides{}, g.json());
    LookGraph pull = g;
    pull.clump = R"("params": {"strength": 0.8, "repelRadius": 0.6})";
    LookGraph push = g;
    push.clump = R"("params": {"strength": -0.5, "repelRadius": 0.6})";
    EXPECT_GPU_MATCHES(pull.json(), (gpu::Overrides{{"clump", {{"strength", -0.5}}}}), push.json());
    LookGraph wider = g;
    wider.clump = R"("params": {"strength": -0.7, "repelRadius": 0.9})";
    EXPECT_GPU_MATCHES(g.json(), (gpu::Overrides{{"clump", {{"repelRadius", 0.9}}}}), wider.json());
    LookGraph mapped = g;
    mapped.clump = R"("params": {"strength": {"expression": "0.5 - 0.1 * pt", "min": -1.0, "max": 1.0}, "lockRadius": 0.2,
                                 "repelRadius": 0.5})";
    EXPECT_GPU_MATCHES(mapped.json(), gpu::Overrides{}, mapped.json());
}

TEST_P(LookEngineTest, ClumpOnScatteredCentersGivesWhatTheCpuClumps) {
    // Centers from a sparse Scatter's spikes fed straight in (centersFrom inputPoints: each axis the
    // strand nearest its center, extended past its tip): dragged as the graph holds it, and with
    // the strands made by Interpolate on the GPU (the axes resampled again from the strands it moves).
    LookGraph g;
    g.clump = R"("params": {"centersFrom": "inputPoints", "strength": 0.8, "pastTip": "extend"})";
    g.nodes = R"(, {"name": "spots", "type": "Scatter", "params": {"density": 0.3, "seed": 4, "previewAll": true}})";
    g.connections = R"(, {"from": "scalp", "to": "spots", "input": "scalp"}, {"from": "spots", "to": "clump", "input": "centers"})";
    LookGraph c = g;
    c.clump = R"("params": {"centersFrom": "inputPoints", "strength": 0.55, "pastTip": "extend"})";
    EXPECT_GPU_MATCHES(g.json(), (gpu::Overrides{{"clump", {{"strength", 0.55}}}}), c.json());
    LookGraph grown = g;
    grown.guides = true;
    GroomSession session(groom(grown.json()));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    const Prepared p = prepare(session, {}, "interpolate");
    ASSERT_TRUE(p.run.inputs[1].clump);
    EXPECT_TRUE(p.run.inputs[1].clump->chosen) << "the axes move with the strands Interpolate makes";
    expectClose(runAndCompare(session, {}, session, "interpolate"));
}

TEST_P(LookEngineTest, ClumpOnCentersABranchShapesFromTheGuidesGivesWhatTheCpuClumps) {
    // Centers of a branch of their own (centersFrom inputCurves), as a creature groom's medium and
    // big clumps are made: a sparse Scatter's strands shaped by the same guides through an
    // Interpolate of their own, waved by a Noise. A dab's guides move them too: the branch runs on
    // the GPU into the centers (resampled, shared, framed there) before the Clump, against a fresh
    // CPU session of the same guides; with copyAmount and without, keeping clearance or not.
    for (const bool shared : {false, true}) {
        for (const bool clearance : {false, true}) {
            SCOPED_TRACE(std::string(shared ? "shared" : "alone") + (clearance ? ", keeping clearance" : ""));
            LookGraph g;
            g.guides = true;
            g.clump = std::string(R"("params": {"centersFrom": "inputCurves", "strength": 0.8, "lockRadius": 0.1)") +
                      (shared ? R"(, "copyAmount": 0.4})" : "}");
            g.nodes = std::string(R"(, {"name": "spots", "type": "Scatter", "params": {"density": 0.4, "seed": 4, "length": 3.0}},
                {"name": "spotsShaped", "type": "Interpolate", "params": {"keepClearance": )") +
                      (clearance ? "true" : "false") + R"(}},
                {"name": "spotsWave", "type": "Noise", "params": {"amplitude": 0.4, "frequency": 0.7}})";
            g.connections = R"(, {"from": "scalp", "to": "spots", "input": "scalp"}, {"from": "spots", "to": "spotsShaped", "input": "strands"},
                {"from": "guides", "to": "spotsShaped", "input": "guides"}, {"from": "scalp", "to": "spotsShaped", "input": "scalp"},
                {"from": "spotsShaped", "to": "spotsWave", "input": "strands"}, {"from": "spotsWave", "to": "clump", "input": "centers"})";
            GroomData data = groom(g.json(), 0.6f);
            GroomSession session(data);
            ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
            // As the graph holds it, a drag of the Clump's strength: its centers as the CPU made them.
            {
                const Prepared held = prepare(session, {{"clump", {{"strength", 0.6}}}});
                ASSERT_EQ(held.run.inputs.size(), held.chain.stages.size());
                ASSERT_TRUE(held.run.inputs.front().clump);
                EXPECT_TRUE(held.run.inputs.front().clump->curves);
            }
            const StrandsPtr before = session.last().strands;
            const GuidesPtr bent = guideSet(*data.scalp, 1.4f);
            session.setGuides(bent);
            ViewRequest dab;
            dab.stopAt = "interpolate";
            ASSERT_EQ(session.evaluate(dab).strands, before);
            GroomData after = data;
            after.guides = bent;
            GroomSession fresh(after);
            ASSERT_TRUE(fresh.evaluate(ViewRequest{}).strands);
            NodeId guides = 0;
            const GraphDocument& doc = *session.viewDocument();
            ASSERT_TRUE(doc.graph.inputSource(doc.nodesByName.at("interpolate"), 1, &guides));
            changed_ = {guides};
            const Prepared p = prepare(session, {}, "interpolate", changed_);
            ASSERT_GE(p.run.inputs.size(), 2u);
            ASSERT_TRUE(p.run.inputs[1].clump);
            const gpu::ClumpInputs& clump = *p.run.inputs[1].clump;
            ASSERT_TRUE(clump.centersRun) << "the centers move with the guides their branch's Interpolate shapes";
            EXPECT_FALSE(clump.curves);
            ASSERT_EQ(clump.centersRun->chain.stages.size(), 2u);
            EXPECT_EQ(clump.centersRun->chain.stages[0].kind, gpu::StageKind::Interpolate);
            EXPECT_EQ(clump.centersRun->chain.stages[1].kind, gpu::StageKind::Noise);
            expectClose(runAndCompare(session, {}, fresh, "interpolate"));
            // Without the guides among the changed nodes the branch is taken as the cache holds it:
            // the centers before the dab, which the CPU's after it are not.
            changed_.clear();
            EXPECT_GT(runAndCompare(session, {}, fresh, "interpolate").positions.max, 10 * kPointTolerance);
        }
    }
}

TEST_P(LookEngineTest, HairColorHandsTheGpusPointsOn) {
    // A HairColor at the end (channels alone, the points shared with what it takes): the run goes
    // through it, the points drawn are the CPU's output's.
    LookGraph g;
    g.after = "HairColor";
    LookGraph c = g;
    c.noise = R"("params": {"amplitude": 0.45, "frequency": 1.0})";
    EXPECT_GPU_MATCHES(g.json(), (gpu::Overrides{{"noise", {{"amplitude", 0.45}}}}), c.json());
}

std::string withParam(const std::string& graphJson, const std::string& name, const std::string& param, const nlohmann::json& value);

TEST_P(LookEngineTest, LengthGivesWhatTheCpuCuts) {
    LookGraph g;
    g.length = true;
    // Kept, and a share of the strands trimmed at random by a share in a range: drags of each
    // (the range given high to low: either way round); the fraction by a map per strand.
    LookGraph cut = g;
    cut.lengthNode = R"("params": {"keep": 0.8, "fraction": 0.6, "trimMin": 0.1, "trimMax": 0.4})";
    const std::pair<const char*, double> drags[] = {{"keep", 0.6}, {"fraction", 0.3}, {"trimMin", 0.5}, {"trimMax", 0.2}};
    for (const auto& [param, value] : drags) {
        SCOPED_TRACE(param);
        EXPECT_GPU_MATCHES(cut.json(), (gpu::Overrides{{"length", {{param, value}}}}),
                           withParam(cut.json(), "length", param, value));
    }
    LookGraph mapped = g;
    mapped.lengthNode = R"("params": {"fraction": {"expression": "rand", "min": 0.0, "max": 1.0}, "trimMax": 0.5})";
    EXPECT_GPU_MATCHES(mapped.json(), gpu::Overrides{}, mapped.json());
}

TEST_P(LookEngineTest, InterpolateGivesWhatTheCpuBlends) {
    // Strands shaped from curved guides (turned to each strand's normal, scaled back to the
    // guides' length), clumped toward the Clump's own centers made from them on the GPU; a radius
    // dragged (guides reach only the strands near them, the rest blend toward their own shape).
    LookGraph g;
    g.guides = true;
    GroomSession session(groom(g.json()));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    const Prepared p = prepare(session, {}, "interpolate");
    ASSERT_EQ(p.chain.stages.size(), 5u);
    ASSERT_TRUE(p.run.inputs[1].clump);
    EXPECT_TRUE(p.run.inputs[1].clump->chosen) << "the centers move with the strands Interpolate makes";
    expectClose(runAndCompare(session, {}, session, "interpolate"));
    LookGraph reach = g;
    reach.interpolate = R"("params": {"limitDistance": 0.9})";
    EXPECT_GPU_MATCHES(g.json(), (gpu::Overrides{{"interpolate", {{"limitDistance", 0.9}}}}), reach.json());
    LookGraph flat = g;
    flat.interpolate = R"("params": {"preserveLength": false, "smoothGuides": false})";
    EXPECT_GPU_MATCHES(flat.json(), gpu::Overrides{}, flat.json());
}

// ---- Ramps and seeds dragged ---------------------------------------------------------------------

// A drag of one ramp param: its keys as a host sends them (the graph's JSON, rampFromJson).
gpu::Overrides rampDrag(const std::string& node, const std::string& param, const std::string& keys) {
    return gpu::Overrides{{node, {{param, rampFromJson(keys)}}}};
}

TEST_P(LookEngineTest, ARampDragGivesWhatTheCpuEvaluatesForIt) {
    // Each stage's profile (Interpolate's falloff) dragged to another curve, smooth and straight:
    // the kernels take the keys and their tangents (and RampAlong's table) as the CPU evaluates them.
    const std::string bump = R"([[0, 0.2], [0.45, 1.0], [1, 0.35]])";
    const std::string straight = R"({"keys": [[0, 1.0], [0.6, 0.3], [1, 0.9]], "linear": true})";
    {
        LookGraph g;
        LookGraph c = g;
        c.noise = R"("params": {"amplitude": 0.3, "frequency": 1.0, "profile": )" + bump + "}";
        EXPECT_GPU_MATCHES(g.json(), rampDrag("noise", "profile", bump), c.json());
        c.noise = R"("params": {"amplitude": 0.3, "frequency": 1.0, "profile": )" + straight + "}";
        EXPECT_GPU_MATCHES(g.json(), rampDrag("noise", "profile", straight), c.json());
    }
    {
        LookGraph g;
        LookGraph c = g;
        c.curl = R"("params": {"radius": 0.1, "frequency": 0.8, "profile": )" + bump + "}";
        EXPECT_GPU_MATCHES(g.json(), rampDrag("curl", "profile", bump), c.json());
    }
    {
        LookGraph g;
        LookGraph c = g;
        c.width = R"("params": {"width": 0.01, "profile": )" + straight + "}";
        EXPECT_GPU_MATCHES(g.json(), rampDrag("width", "profile", straight), c.json());
    }
    {
        LookGraph g;
        LookGraph c = g;
        c.clump = R"("params": {"strength": 0.8, "profile": )" + bump + "}";
        EXPECT_GPU_MATCHES(g.json(), rampDrag("clump", "profile", bump), c.json());
    }
    {
        LookGraph g;
        g.guides = true;
        g.interpolate = R"("params": {"limitDistance": 0.9})";
        LookGraph c = g;
        const std::string falloff = R"([[0, 1.0], [0.5, 0.85], [1, 0.0]])";
        c.interpolate = R"("params": {"limitDistance": 0.9, "falloff": )" + falloff + "}";
        EXPECT_GPU_MATCHES(g.json(), rampDrag("interpolate", "falloff", falloff), c.json());
    }
}

TEST_P(LookEngineTest, ASeedDragGivesWhatTheCpuDrawsForIt) {
    // Each stage's seed dragged: the strands' draws made again from it, as the CPU hashes them.
    LookGraph g;
    g.length = true;
    g.clump = R"("params": {"strength": 0.8, "lockRadius": 0.1, "seed": 1})";
    g.lengthNode = R"("params": {"keep": 0.9, "randomness": 0.3, "seed": 1})";
    g.noise = R"("params": {"amplitude": 0.3, "frequency": 1.0, "seed": 1})";
    g.curl = R"("params": {"radius": 0.1, "frequency": 0.8, "randomness": 0.5, "seed": 1})";
    g.width = R"("params": {"width": 0.01, "randomness": 0.4, "seed": 1})";
    struct Case {
        const char* node;
        std::string LookGraph::*field;
        std::string params;
    };
    const Case cases[] = {
        {"length", &LookGraph::lengthNode, R"("params": {"keep": 0.9, "randomness": 0.3, "seed": 17})"},
        {"noise", &LookGraph::noise, R"("params": {"amplitude": 0.3, "frequency": 1.0, "seed": 17})"},
        {"curl", &LookGraph::curl, R"("params": {"radius": 0.1, "frequency": 0.8, "randomness": 0.5, "seed": 17})"},
        {"width", &LookGraph::width, R"("params": {"width": 0.01, "randomness": 0.4, "seed": 17})"},
    };
    for (const Case& k : cases) {
        SCOPED_TRACE(k.node);
        LookGraph c = g;
        c.*k.field = k.params;
        EXPECT_GPU_MATCHES(g.json(), (gpu::Overrides{{k.node, {{"seed", 17.0}}}}), c.json());
    }
}

TEST(GpuPreview, AScriptNodeRunsOnTheCpu) {
    // Script nodes (script.h) are not GPU stages: a graph ending in one is refused, saying so, and
    // drags over it evaluate on the CPU.
    const GraphDocument doc = loadGraphJson(R"({"version": 1, "output": "out",
      "nodes": [{"name": "scalp", "type": "Scalp"}, {"name": "scatter", "type": "Scatter"},
                {"name": "wave", "type": "Script", "params": {"definition": {"glnode": 1, "id": "test.wave", "version": 1,
                 "label": "Wave", "writes": ["P"], "script": "per point { P += N * 0.1 * t; }"}}},
                {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"}, {"from": "scatter", "to": "wave", "input": "strands"},
                      {"from": "wave", "to": "out", "input": "strands"}]})");
    gpu::LookChain chain;
    std::string why;
    EXPECT_FALSE(gpu::findLookChain(doc, &chain, &why));
    EXPECT_NE(why.find("script node runs on the CPU"), std::string::npos) << why;
}

TEST(GpuPreview, ASubdivideOfNoViewportLevelsHandsOn) {
    // A Subdivide at the end: of no viewport levels it hands its input on in the viewport, so the
    // nodes before it run on the GPU; of some, its smooth points are the CPU's.
    const auto doc = [](int viewportLevels) {
        return loadGraphJson(R"({"version": 1, "output": "out",
          "nodes": [{"name": "scalp", "type": "Scalp"}, {"name": "scatter", "type": "Scatter"},
                    {"name": "clump", "type": "Clump"},
                    {"name": "smooth", "type": "Subdivide", "params": {"viewportLevels": )" + std::to_string(viewportLevels) + R"(}},
                    {"name": "out", "type": "Output"}],
          "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"}, {"from": "scatter", "to": "clump", "input": "strands"},
                          {"from": "clump", "to": "smooth", "input": "strands"}, {"from": "smooth", "to": "out", "input": "strands"}]})");
    };
    gpu::LookChain chain;
    std::string why;
    ASSERT_TRUE(gpu::findLookChain(doc(0), &chain, &why)) << why;
    EXPECT_EQ(chain.stages.size(), 1u);
    gpu::LookChain smooth;
    EXPECT_FALSE(gpu::findLookChain(doc(1), &smooth, &why) && !smooth.stages.empty());
}

TEST(GpuPreview, AClumpVaryingEachStrandsPullRunsOnTheCpu) {
    // strandStrengthRandom is the CPU's for now: a Clump using it refuses the GPU its params, saying so.
    const auto doc = [](double random) {
        return loadGraphJson(R"({"version": 1, "output": "out",
          "nodes": [{"name": "scalp", "type": "Scalp"}, {"name": "scatter", "type": "Scatter"},
                    {"name": "clump", "type": "Clump", "params": {"strandStrengthRandom": )" + std::to_string(random) + R"(}},
                    {"name": "out", "type": "Output"}],
          "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"}, {"from": "scatter", "to": "clump", "input": "strands"},
                          {"from": "clump", "to": "out", "input": "strands"}]})");
    };
    std::string why;
    for (const double random : {0.0, 0.5}) {
        const GraphDocument d = doc(random);
        gpu::LookChain chain;
        ASSERT_TRUE(gpu::findLookChain(d, &chain, &why)) << why;
        gpu::LookParams params;
        EXPECT_EQ(gpu::readLookParams(d, chain, {}, &params, &why), random == 0.0) << why;
    }
    EXPECT_NE(why.find("strandStrengthRandom"), std::string::npos) << why;
}

TEST(GpuPreview, StageTableNamesRealTypesAndParams) {
    // Every row of look_stages.inc names a registered node type, and every param it lists (mapped,
    // ramp, vector, number) is one of that type's, of the kind it says: a typo there would keep a
    // stage from running, or let a drag through that the kernel never reads.
    std::map<std::string, std::map<std::string, std::string>> kinds;  // type -> param -> kind
    for (const nlohmann::json& s : nlohmann::json::parse(operatorSpecsJson())) {
        for (const nlohmann::json& p : s["params"]) kinds[s["type"]][p["name"]] = p["kind"];
    }
    auto check = [&](const char* type, const char* list, const char* what, std::set<std::string> allowed) {
        SCOPED_TRACE(std::string(type) + " " + what + " \"" + list + "\" (look_stages.inc)");
        ASSERT_TRUE(kinds.count(type)) << type << " is not a registered node type";
        std::istringstream in(list);
        for (std::string name; in >> name;) {
            const auto it = kinds[type].find(name);
            ASSERT_NE(it, kinds[type].end()) << type << " has no param '" << name << "'";
            if (!allowed.empty()) EXPECT_TRUE(allowed.count(it->second)) << type << "." << name << " is a " << it->second;
            EXPECT_TRUE(std::string(what) == "mapped" || gpu::previewableParam(type, name)) << type << "." << name;
        }
    };
    const std::set<std::string> numbers = {"float", "int", "bool"};
#define GROOMLAB_STAGE(Kind, name, mapped0, mapped1, ramps, vectors, nums) \
    check(#Kind, mapped0 " " mapped1, "mapped", {});                       \
    check(#Kind, ramps, "ramps", {"ramp"});                                \
    check(#Kind, vectors, "vectors", {"vec3"});                            \
    check(#Kind, nums, "numbers", numbers);
#define GROOMLAB_STAGE_NODE(Type, Kind, ramps, vectors, nums) \
    check(#Type, ramps, "ramps", {"ramp"});                   \
    check(#Type, vectors, "vectors", {"vec3"});               \
    check(#Type, nums, "numbers", numbers);
#define GROOMLAB_GPU_PARAMS(Type, ramps, vectors, nums) \
    check(#Type, ramps, "ramps", {"ramp"});             \
    check(#Type, vectors, "vectors", {"vec3"});         \
    check(#Type, nums, "numbers", numbers);
#define GROOMLAB_PASS_THROUGH(Type) check(#Type, "", "pass-through", {});
#include "groomlab/look/look_stages.inc"
#undef GROOMLAB_STAGE
#undef GROOMLAB_STAGE_NODE
#undef GROOMLAB_GPU_PARAMS
#undef GROOMLAB_PASS_THROUGH
    EXPECT_STREQ(gpu::stageName(gpu::StageKind::SelfCollide), "selfCollide");
}

TEST(GpuPreview, RefusesDragsTheKernelsDoNotRead) {
    // A param the kernels do not read (it would show its old value), a ramp for a number or a
    // number for a ramp, and a Clump's seed where it picks its own centers by it: the CPU's.
    GroomSession session(groom(startingGraph("clumpAlone")));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    const GraphDocument& doc = *session.viewDocument();
    std::string clump;
    for (const auto& [name, id] : doc.nodesByName) {
        if (doc.graph.node(id).typeName() == "Clump") clump = name;
    }
    ASSERT_FALSE(clump.empty());
    std::string why;
    EXPECT_FALSE(gpu::previewableOverrides(doc, {{clump, {{"count", 50.0}}}}, &why));
    EXPECT_NE(why.find("count"), std::string::npos) << why;
    EXPECT_FALSE(gpu::previewableOverrides(doc, {{clump, {{"profile", 0.5}}}}, &why));
    EXPECT_FALSE(gpu::previewableOverrides(doc, {{clump, {{"strength", Ramp::linear(0.0f, 1.0f)}}}}, &why));
    EXPECT_TRUE(gpu::previewableOverrides(doc, {{clump, {{"profile", Ramp::linear(0.0f, 1.0f)}, {"seed", 3.0}}}}, &why)) << why;
    gpu::LookChain chain;
    ASSERT_TRUE(gpu::findLookChain(doc, &chain, &why)) << why;
    gpu::LookParams params;
    EXPECT_FALSE(gpu::readLookParams(doc, chain, {{clump, {{"seed", 3.0}}}}, &params, &why));
    EXPECT_NE(why.find("own centers"), std::string::npos) << why;
    EXPECT_TRUE(gpu::readLookParams(doc, chain, {{clump, {{"profile", Ramp::linear(0.2f, 0.9f)}}}}, &params, &why)) << why;
    ASSERT_TRUE(params.find(gpu::StageKind::Clump));
    EXPECT_EQ(params.find(gpu::StageKind::Clump)->ramp.keys.size(), 2u);
    EXPECT_FLOAT_EQ(params.find(gpu::StageKind::Clump)->ramp(0.0f), 0.2f);
}

TEST_P(LookEngineTest, InterpolateTurnsGuidesAsTheCpuDoesWhereNormalsNearlyAgree) {
    // A large, gently curved scalp with many strands: those rooted near a guide's root have
    // nearly its normal, where Rotation takes the identity (a cosine above 1 - 1e-6) or a slight
    // turn, apart by more than rounding on long strands: the GPU decides from the cosine summed as
    // the CPU sums it.
    // (Not keeping clearance: on this coarse sphere the walk's answers near its poles, where many
    // degenerate triangles meet at copies of one point each with a normal of its own, turn on the
    // last bit of a point's place, on the CPU as on the GPU. InterpolateKeepsClearanceAsTheCpuDoes
    // walks a welded scalp.)
    LookGraph g;
    g.guides = true;
    g.interpolate = R"("params": {"keepClearance": false})";
    g.scatter = R"("params": {"density": 3.0, "length": 3.0})";
    GroomSession session(groom(g.json(), 0.6f, 40.0f));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    ASSERT_GT(session.last().strands->strandCount(), 50000u);
    expectClose(runAndCompare(session, {}, session, "interpolate"));
}

TEST_P(LookEngineTest, InterpolateKeepsClearanceAsTheCpuDoes) {
    // A head-like scalp (a sphere of radius 10 with an ear standing out of its side and the back of
    // the skull bulging 1 out) and guides combed over the crown and down the sides: carried as
    // they are, strands crossing the ear and the bulge go into them; keeping clearance (the
    // default), the walk lifts them out through the scalp's field (blended, and from the closest
    // faces where the field does not blend: the creases round the ear), as the CPU walks them.
    // (A welded sphere, an icosahedron's faces split five times: a UV sphere's poles, where many
    // degenerate triangles meet at copies of one point each with a normal of its own, make the
    // walk there turn on the last bit of a point's place, on the CPU as on the GPU. Open below
    // y = -6, as a scalp is: strands hanging past its edge are beside it.)
    std::vector<Vec3f> unit = {{-1, 1.618034f, 0}, {1, 1.618034f, 0}, {-1, -1.618034f, 0}, {1, -1.618034f, 0},
                               {0, -1, 1.618034f}, {0, 1, 1.618034f}, {0, -1, -1.618034f}, {0, 1, -1.618034f},
                               {1.618034f, 0, -1}, {1.618034f, 0, 1}, {-1.618034f, 0, -1}, {-1.618034f, 0, 1}};
    std::vector<ScalpMesh::Triangle> faces = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7},  {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
                                              {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4},  {3, 4, 2},   {3, 2, 6}, {3, 6, 8},
                                              {3, 8, 9},  {4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},   {9, 8, 1}};
    for (Vec3f& v : unit) v = v.normalized();
    for (int level = 0; level < 5; ++level) {
        std::map<std::pair<std::int32_t, std::int32_t>, std::int32_t> middles;
        auto middle = [&](std::int32_t a, std::int32_t b) {
            const auto key = std::minmax(a, b);
            const auto it = middles.find(key);
            if (it != middles.end()) return it->second;
            unit.push_back((unit[a] + unit[b]).normalized());
            return middles[key] = static_cast<std::int32_t>(unit.size() - 1);
        };
        std::vector<ScalpMesh::Triangle> split;
        for (const auto& t : faces) {
            const std::int32_t ab = middle(t[0], t[1]), bc = middle(t[1], t[2]), ca = middle(t[2], t[0]);
            split.push_back({t[0], ab, ca});
            split.push_back({t[1], bc, ab});
            split.push_back({t[2], ca, bc});
            split.push_back({ab, bc, ca});
        }
        faces = std::move(split);
    }
    auto direction = [](float polarDegrees, float azimuthDegrees) {
        const float t = polarDegrees * 3.14159265f / 180.0f, a = azimuthDegrees * 3.14159265f / 180.0f;
        return Vec3f(std::sin(t) * std::cos(a), std::cos(t), std::sin(t) * std::sin(a));
    };
    const Vec3f ear = direction(80.0f, 40.0f), back = direction(60.0f, -60.0f);
    auto bump = [](const Vec3f& n, const Vec3f& at, float height, float width) {
        const float a = std::acos(std::clamp(n.dot(at), -1.0f, 1.0f)) / width;
        return height * std::exp(-a * a);
    };
    std::vector<Vec3f> points;
    for (const Vec3f& n : unit) points.push_back(n * (10.0f + bump(n, ear, 2.5f, 0.12f) + bump(n, back, 1.0f, 0.35f)));
    std::vector<ScalpMesh::Triangle> kept;
    for (const auto& t : faces) {
        if (points[t[0]].y > -6.0f || points[t[1]].y > -6.0f || points[t[2]].y > -6.0f) kept.push_back(t);
    }
    GroomData data;
    data.scalp = std::make_shared<const ScalpMesh>(points, kept);
    auto guides = std::make_shared<GuideSet>();
    std::uint64_t id = 1;
    for (int k = 0; k < 8; ++k) {  // over the skin from the crown 100 degrees down, then hanging
        const float azimuth = 45.0f * static_cast<float>(k);
        std::vector<Vec3f> along;
        for (int i = 0; i <= 20; ++i) {
            const SurfacePoint at = data.scalp->closestPoint(direction(10.0f + 4.5f * static_cast<float>(i), azimuth) * 10.0f);
            along.push_back(i == 0 ? at.position : at.position + at.normal * 0.05f);
        }
        for (int i = 1; i <= 8; ++i) along.push_back(along[20] + Vec3f(0.0f, -0.5f * static_cast<float>(i), 0.0f));
        guides->add(id++, along);
    }
    guides->rootOnMesh(*data.scalp);
    data.guides = guides;
    // Interpolate's strands as they are (a Width after it, which moves no point). Strands of 9
    // points (the walk asks each of them) and, resampled to 41 before Interpolate, of 41 (it asks
    // every third and the tip, the points between moved by the moves of those around them).
    for (const bool fine : {false, true}) {
        SCOPED_TRACE(fine ? "strands of 41 points" : "strands of 9 points");
        const std::string resample = fine ? R"({"name": "fine", "type": "Resample", "params": {"segments": 40, "mode": "even"}},)" : "";
        const std::string toInterpolate = fine ? R"({"from": "scatter", "to": "fine", "input": "strands"}, {"from": "fine", "to": "interpolate", "input": "strands"},)"
                                               : R"({"from": "scatter", "to": "interpolate", "input": "strands"},)";
        data.graphJson = R"({"nodes": [
            {"name": "scalp", "type": "Scalp"}, {"name": "scatter", "type": "Scatter", "params": {"density": 30.0, "length": 3.0}},)" + resample + R"(
            {"name": "guides", "type": "Guides"}, {"name": "interpolate", "type": "Interpolate", "params": {}},
            {"name": "width", "type": "Width", "params": {"width": 0.01}}, {"name": "output", "type": "Output"}],
          "connections": [
            {"from": "scalp", "to": "scatter", "input": "scalp"},)" + toInterpolate + R"(
            {"from": "guides", "to": "interpolate", "input": "guides"}, {"from": "scalp", "to": "interpolate", "input": "scalp"},
            {"from": "interpolate", "to": "width", "input": "strands"}, {"from": "width", "to": "output", "input": "strands"}],
          "output": "output"})";
        GroomSession session(data);
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        GroomData off = data;
        off.graphJson = withoutClearance(data.graphJson);
        GroomSession carried(off);
        ASSERT_TRUE(carried.evaluate(ViewRequest{}).strands);
        const auto interpolated = [](GroomSession& s) {
            const GraphDocument& d = *s.viewDocument();
            return valueAs<StrandsPtr>(d.graph.cachedValue(d.nodesByName.at("interpolate")));
        };
        const StrandsPtr lifted = interpolated(session), asCarried = interpolated(carried);
        ASSERT_TRUE(lifted && asCarried);
        std::size_t moved = 0;
        for (std::size_t s = 0; s < lifted->strandCount(); ++s) {
            moved += std::memcmp(lifted->strandPoints(s), asCarried->strandPoints(s), lifted->strandPointCount(s) * sizeof(Vec3f)) != 0;
        }
        std::printf("[ %-8s ] keeping clearance lifted %zu of %zu strands of %zu points\n", GetParam().c_str(), moved, lifted->strandCount(),
                    lifted->strandPointCount(0));
        EXPECT_GT(moved, lifted->strandCount() / 4) << "the test's premise: the walk lifts strands";
        EXPECT_EQ(lifted->strandPointCount(0), fine ? 41u : 9u);
        const auto start = std::chrono::steady_clock::now();
        const Prepared p = prepare(session, {}, "interpolate");
        const double readMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        ASSERT_EQ(p.chain.stages.front().kind, gpu::StageKind::Interpolate);
        ASSERT_TRUE(p.run.inputs.front().interpolate && p.run.inputs.front().interpolate->clearance);
        std::printf("[ %-8s ] the scalp's field as tables: %.1f MB (the run read in %.0f ms, the tables made with it)\n",
                    GetParam().c_str(), static_cast<double>(p.run.inputs.front().interpolate->clearance->tables->size()) * 4e-6, readMs);
        expectClose(runAndCompare(session, {}, session, "interpolate"));
    }
}

TEST_P(LookEngineTest, InterpolateCarriesGuidesRoundAFoldAsTheCpuDoes) {
    // A thin flap standing on a floor, guides on both its faces, along its top and on the floor:
    // strands near the top take guides from the other face carried over it (their binding's
    // twists), the GPU turns them as the CPU does; following the surface and not.
    for (const bool followSurface : {true, false}) {
        LookGraph g;
        g.guides = true;
        g.scatter = R"("params": {"density": 600.0, "length": 1.0})";
        g.interpolate = std::string(R"("params": {"followSurface": )") + (followSurface ? "true" : "false") + "}";
        GroomData data;
        data.graphJson = g.json();
        data.scalp = test::makeFin(1.2f, 0.08f, 2.0f, 0.05f, true, 1.0f);
        auto guides = std::make_shared<GuideSet>();
        std::uint64_t id = 1;
        for (int i = 0; i < 6; ++i) {
            const float x = 0.17f + 0.33f * static_cast<float>(i);
            for (int j = 0; j < 4; ++j) {
                const float y = 0.2f + 0.3f * static_cast<float>(j);
                guides->add(id++, test::straightCurve({x, y, -0.04f}, {0.3f, 0.1f, -0.6f}, 6));
                guides->add(id++, test::straightCurve({x, y, 0.04f}, {0.6f, -0.2f, 0.2f}, 6));
            }
            for (const float z : {-0.7f, -0.3f, 0.3f, 0.7f}) guides->add(id++, test::straightCurve({x, 0.0f, z}, {0.2f, 0.7f, 0.1f * z}, 6));
        }
        guides->rootOnMesh(*data.scalp);
        data.guides = guides;
        GroomSession session(data);
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const GraphDocument* doc = session.viewDocument();
        InterpolateState state;
        ASSERT_TRUE(interpolateState(doc->graph.node(doc->nodesByName.at("interpolate")),
                                     valueAs<StrandsPtr>(doc->graph.cachedValue(doc->nodesByName.at("interpolate"))), &state));
        InterpolateRecords records;
        interpolateRecords(state, &records);
        std::size_t twisted = 0;
        for (const float t : records.twists) twisted += t != 0.0f;
        EXPECT_GT(twisted, 0u) << "no guide came over the flap's top";
        expectClose(runAndCompare(session, {}, session, "interpolate"));
    }
}

TEST_P(LookEngineTest, AGuideEditRunsFromInterpolateAsABrushDabWould) {
    // A brush dab's guides (their shapes changed, not their roots): evaluated up to Interpolate
    // alone (ViewRequest::stopAt), the GPU blends, clumps (centers made again from its strands,
    // with a lock radius: their frames too) and shapes them; against a fresh CPU session of the
    // same guides. Then roots moved: the binding is the CPU's, the GPU says no.
    LookGraph g;
    g.guides = true;
    g.clump = R"("params": {"strength": 0.8, "lockRadius": 0.1})";
    GroomData data = groom(g.json(), 0.6f);
    GroomSession session(data);
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    const StrandsPtr before = session.last().strands;
    const GuidesPtr bent = guideSet(*data.scalp, 1.4f);
    session.setGuides(bent);
    ViewRequest dab;
    dab.stopAt = "interpolate";
    ASSERT_EQ(session.evaluate(dab).strands, before);
    GroomData after = data;
    after.guides = bent;
    GroomSession fresh(after);
    ASSERT_TRUE(fresh.evaluate(ViewRequest{}).strands);
    expectClose(runAndCompare(session, {}, fresh, "interpolate"));

    // A guide's root moved: the strands bound again on the CPU near it (the rest of the binding
    // shared), the GPU blends with that.
    auto moved = std::make_shared<GuideSet>(*bent);
    for (Vec3f& p : moved->guides()[3].points) p = p + Vec3f(0.3f, 0.0f, 0.0f);
    moved->rootOnMesh(*data.scalp);
    session.setGuides(moved);
    ASSERT_EQ(session.evaluate(dab).strands, before);
    GroomData movedData = data;
    movedData.guides = moved;
    GroomSession movedFresh(movedData);
    ASSERT_TRUE(movedFresh.evaluate(ViewRequest{}).strands);
    expectClose(runAndCompare(session, {}, movedFresh, "interpolate"));
    // A guide more: the CPU binds again from scratch (the GPU says no).
    auto more = std::make_shared<GuideSet>(*moved);
    more->add(999, test::straightCurve(Vec3f(0.0f, 4.0f, 0.0f), Vec3f(0.0f, 3.0f, 0.0f), 6));
    more->rootOnMesh(*data.scalp);
    session.setGuides(more);
    session.evaluate(dab);
    Prepared refused;
    EXPECT_FALSE(prepareRun(session, {}, "interpolate", &refused));
    EXPECT_NE(refused.why.find("added or removed"), std::string::npos) << refused.why;
}

TEST_P(LookEngineTest, InterpolateWithRegionsBlendsAsTheCpuDoes) {
    // Regions painted on the scalp (a color set: three sectors around it), Interpolate's regionMap
    // naming them: the GPU blends from the CPU's binding within regions; then a dab bending the
    // guides, and one moving a guide's root within its region (the CPU binds again near it), give
    // a fresh CPU session's strands.
    LookGraph g;
    g.guides = true;
    g.interpolate = R"("params": {"regionMap": "regions"})";
    GroomData data = groom(g.json());
    auto painted = std::make_shared<ScalpMesh>(data.scalp->points(), data.scalp->triangles());
    std::vector<ScalpMesh::Color> regions;
    for (const auto& tri : painted->triangles()) {
        for (const std::int32_t v : tri) {
            const Vec3f& p = painted->points()[v];
            const float a = std::atan2(p.z, p.x);
            regions.push_back(a < -1.0472f ? ScalpMesh::Color{1, 0, 0, 1} : a < 1.0472f ? ScalpMesh::Color{0, 1, 0, 1} : ScalpMesh::Color{0, 0, 1, 1});
        }
    }
    painted->setCornerColors("regions", regions);
    data.scalp = painted;
    data.guides = guideSet(*data.scalp, 0.6f);
    GroomSession session(data);
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    ASSERT_TRUE(session.last().strands->strandChannel("region"));
    expectClose(runAndCompare(session, {}, session, "interpolate"));
    const StrandsPtr before = session.last().strands;
    ViewRequest dab;
    dab.stopAt = "interpolate";
    const GuidesPtr bent = guideSet(*data.scalp, 1.4f);
    auto moved = std::make_shared<GuideSet>(*bent);
    for (Vec3f& p : moved->guides()[3].points) p = p + Vec3f(0.3f, 0.0f, 0.0f);
    moved->rootOnMesh(*data.scalp);
    for (const GuidesPtr& guides : {bent, GuidesPtr(moved)}) {
        session.setGuides(guides);
        ASSERT_EQ(session.evaluate(dab).strands, before);
        GroomData after = data;
        after.guides = guides;
        GroomSession fresh(after);
        ASSERT_TRUE(fresh.evaluate(ViewRequest{}).strands);
        expectClose(runAndCompare(session, {}, fresh, "interpolate"));
    }
}

TEST_P(LookEngineTest, AStrokeOnGuidesWithChildrenBindsWhereTheirRootsMoved) {
    // Guides with children generated in their tubes: bending a parent at its root moves its
    // children's roots on the scalp, so the strands near them are bound again (on the CPU, as
    // Interpolate binds them, sharing the rest) and the GPU blends from that binding; against a
    // fresh CPU session of the same preview, dab after dab.
    LookGraph g;
    g.guides = true;
    GroomData data = groom(g.json());
    data.guides = nullptr;
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    TubeProfile tube;
    tube.width = {0.5f, 0.3f};
    const GuidesPtr primaries = guideSet(*data.scalp, 0.6f);
    for (const Guide& guide : primaries->guides()) {
        if (guide.id % 3 == 0) h.addGuide(primary, guide.points, tube);
    }
    h.evaluate(data.scalp.get());
    ChildGenerationParams children;
    children.count = 4;
    h.generateChildren(primary, "children", children);
    h.evaluate(data.scalp.get());
    data.hierarchyJson = h.toJson();
    GroomSession session(data);
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    const StrandsPtr before = session.last().strands;
    auto preview = std::make_shared<GuideHierarchy>(*session.authoredHierarchy());
    gpu::LookInputs inputs;  // as a host keeps it from dab to dab
    for (int dab = 1; dab <= 3; ++dab) {
        for (const HierarchyGuide& guide : preview->guides()) {
            if (guide.isChild()) continue;
            std::vector<Vec3f> points = guide.points;
            for (std::size_t i = 1; i < points.size(); ++i) {
                points[i] = points[i] + Vec3f(0.25f * static_cast<float>(dab) * static_cast<float>(i), 0.0f, 0.1f * static_cast<float>(i));
            }
            preview->setPoints(guide.id, points);
        }
        preview->evaluate(data.scalp.get());  // the children follow (as a brush's stamp leaves them)
        session.setPreviewHierarchy(preview);
        ViewRequest stopped;
        stopped.stopAt = "interpolate";
        ASSERT_EQ(session.evaluate(stopped).strands, before);
        GroomSession fresh(data);
        fresh.setPreviewHierarchy(std::make_shared<GuideHierarchy>(*preview));
        ASSERT_TRUE(fresh.evaluate(ViewRequest{}).strands);
        Prepared p;
        ASSERT_TRUE(prepareRun(session, {}, "interpolate", &p, &inputs)) << p.why;
        expectClose(runAndCompare(session, {}, fresh, "interpolate"));
    }
    EXPECT_GT(inputs.rebinds(), 0u) << "the children's roots moved with their parents'";
}

TEST_P(LookEngineTest, GuideRootsMovedDabAfterDabRewriteOnlyTheirRecords) {
    // Guide roots moved dab after dab: the strands near them bound again (their chunks' records
    // growing and shrinking), one engine keeping the binding writes only those chunks (where they
    // were when they fit, else after the others, else all again); each run gives what a new
    // engine given the whole binding gives, to the bit, and what a fresh CPU session gives.
    LookGraph g;
    g.guides = true;
    GroomData data = groom(g.json());
    GroomSession session(data);
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    const StrandsPtr before = session.last().strands;
    const ScalpMesh* scalp = session.last().scalp.get();
    gpu::LookInputs inputs;  // as a host keeps it from dab to dab
    auto kept = backend_->engine();
    GuidesPtr guides = data.guides;
    ViewRequest dab;
    dab.stopAt = "interpolate";
    std::size_t lighter = 0;  // dabs that wrote less than the whole binding
    for (int d = 1; d <= 8; ++d) {
        SCOPED_TRACE("dab " + std::to_string(d));
        auto moved = std::make_shared<GuideSet>(*guides);
        // (A stroke's small steps; every third dab a long way.)
        const float step = d % 3 == 0 ? 0.3f : 0.03f;
        for (std::size_t k = 0; k < 3; ++k) {
            Guide& guide = moved->guides()[(static_cast<std::size_t>(d) * 7 + k * 11) % moved->guides().size()];
            const Vec3f by(step * static_cast<float>((d + static_cast<int>(k)) % 3 - 1), 0.0f, step * static_cast<float>(k % 2 ? 1 : -1));
            for (Vec3f& p : guide.points) p = p + by;
        }
        moved->rootOnMesh(*data.scalp);
        guides = moved;
        session.setGuides(moved);
        ASSERT_EQ(session.evaluate(dab).strands, before);
        Prepared p;
        ASSERT_TRUE(prepareRun(session, {}, "interpolate", &p, &inputs)) << p.why;
        kept->setBase(p.base, scalp);
        kept->run(p.run, true);
        const double keptMb = kept->runUploadMegabytes();
        auto whole = backend_->engine();
        whole->setBase(p.base, scalp);
        whole->run(p.run, true);
        lighter += keptMb < whole->runUploadMegabytes() ? 1 : 0;
        const std::vector<Vec3f> points = kept->readPositions();
        EXPECT_EQ(gpu::compareVectors(points, whole->readPositions()).max, 0.0);
        GroomData after = data;
        after.guides = moved;
        GroomSession fresh(after);
        ASSERT_TRUE(fresh.evaluate(ViewRequest{}).strands);
        const gpu::PointError e = gpu::compareVectors(points, fresh.last().strands->positions());
        EXPECT_LT(e.max, kPointTolerance);
        EXPECT_LT(e.mean, kMeanTolerance);
    }
    EXPECT_GT(inputs.rebinds(), 0u);
    EXPECT_GT(lighter, 0u) << "every dab wrote the whole binding";
}

TEST_P(LookEngineTest, ADabWalksOnlyTheStrandsOfTheGuidesItChanged) {
    // Keeping clearance: a dab reshaping a few guides (their roots where they were) walks only the
    // strands bound to them, the others keeping the last run's walks (look_engine.cpp
    // walkOnlyChanged). Dab after dab, one engine keeping its walks gives what a new engine gives,
    // to the bit, and what a fresh CPU session gives.
    LookGraph g;
    g.guides = true;
    GroomData data = groom(g.json());
    GroomSession session(data);
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    const StrandsPtr before = session.last().strands;
    const ScalpMesh* scalp = session.last().scalp.get();
    gpu::LookInputs inputs;  // as a host keeps it from dab to dab
    auto kept = backend_->engine();
    GuidesPtr guides = data.guides;
    ViewRequest dab;
    dab.stopAt = "interpolate";
    std::size_t partial = 0;
    for (int d = 0; d <= 4; ++d) {
        SCOPED_TRACE("dab " + std::to_string(d));
        if (d > 0) {
            auto moved = std::make_shared<GuideSet>(*guides);
            for (std::size_t k = 0; k < 2; ++k) {  // two guides bent down onto the scalp, their roots kept
                Guide& guide = moved->guides()[(static_cast<std::size_t>(d) * 5 + k * 13) % moved->guides().size()];
                const Vec3f root = guide.points.front();
                for (std::size_t i = 1; i < guide.points.size(); ++i) {
                    guide.points[i] = guide.points[i] + (root - guide.points[i]) * 0.2f + Vec3f(0.05f * static_cast<float>(d), -0.1f, 0.0f) * static_cast<float>(i);
                }
            }
            moved->rootOnMesh(*data.scalp);
            guides = moved;
            session.setGuides(moved);
            ASSERT_EQ(session.evaluate(dab).strands, before);
        }
        Prepared p;
        ASSERT_TRUE(prepareRun(session, {}, "interpolate", &p, &inputs)) << p.why;
        ASSERT_TRUE(p.run.inputs.front().interpolate && p.run.inputs.front().interpolate->clearance);
        kept->setBase(p.base, scalp);
        kept->run(p.run, true);
        if (d > 0) {
            const gpu::LookEngine::WalkStats& walk = kept->lastWalk();
            partial += walk.partial ? 1 : 0;
            EXPECT_TRUE(walk.partial);
            EXPECT_GT(walk.changedGuides, 0u);
            EXPECT_LT(walk.changedGuides, walk.guides);
        }
        auto whole = backend_->engine();
        whole->setBase(p.base, scalp);
        whole->run(p.run, true);
        EXPECT_FALSE(whole->lastWalk().partial);
        const std::vector<Vec3f> points = kept->readPositions();
        EXPECT_EQ(gpu::compareVectors(points, whole->readPositions()).max, 0.0);
        GroomData after = data;
        after.guides = guides;
        GroomSession fresh(after);
        ASSERT_TRUE(fresh.evaluate(ViewRequest{}).strands);
        const gpu::PointError e = gpu::compareVectors(points, fresh.last().strands->positions());
        EXPECT_LT(e.max, kPointTolerance);
        EXPECT_LT(e.mean, kMeanTolerance);
    }
    EXPECT_EQ(partial, 4u);
}

TEST_P(LookEngineTest, InterpolateBlendsPastSixtyFiveThousandGuides) {
    // More guides than 16 bits index (65535): the binding keeps 32-bit indices, the GPU's records
    // two words each; the GPU blends what the CPU blends.
    LookGraph g;
    g.guides = true;
    g.interpolate = R"("params": {"keepClearance": false})";
    GroomData data = groom(g.json());
    auto guides = std::make_shared<GuideSet>();
    const float radius = 4.0f;
    for (std::uint64_t id = 1; id <= 70000; ++id) {
        // Spread over the sphere (a Fibonacci spiral), each leaning a little its own way.
        const float i = static_cast<float>(id) - 0.5f;
        const float y = 1.0f - 2.0f * i / 70000.0f, r = std::sqrt(std::max(0.0f, 1.0f - y * y)), a = 2.39996323f * i;
        const Vec3f n(r * std::cos(a), y, r * std::sin(a));
        guides->add(id, test::straightCurve(n * radius, n * 2.0f + Vec3f(0.3f * std::sin(a * 3.0f), 0.0f, 0.3f * std::cos(a * 5.0f)), 6));
    }
    guides->rootOnMesh(*data.scalp);
    data.guides = guides;
    GroomSession session(data);
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    expectClose(runAndCompare(session, {}, session, "interpolate"));
}

TEST_P(LookEngineTest, EveryPointStaysInTheBounds) {
    // What the viewport culls by during a drag: every point the GPU makes, in strand and world
    // space, at values far past the graph's; from Interpolate (its guides bent far); and clumped
    // onto a sparse Scatter's short spikes with the strands carried on past them (pastTip extend).
    for (const char* space : {"strand", "world", "spikes"}) {
        LookGraph g;
        g.guides = true;
        const bool spikes = std::string(space) == "spikes";
        g.noise = std::string(R"("params": {"amplitude": 0.3, "frequency": 1.0, "octaves": 4, "space": ")") +
                  (spikes ? "strand" : space) + R"("})";
        g.curl = R"("params": {"radius": 0.1, "frequency": 0.8, "randomness": 1.0})";
        if (spikes) {
            g.clump = R"("params": {"strength": 0.8, "centersFrom": "inputCurves", "pastTip": "extend"})";
            g.nodes = R"(, {"name": "spots", "type": "Scatter", "params": {"density": 0.3, "seed": 4, "previewAll": true}})";
            g.connections = R"(, {"from": "scalp", "to": "spots", "input": "scalp"}, {"from": "spots", "to": "clump", "input": "centers"})";
        }
        GroomSession session(groom(g.json()));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const gpu::Overrides drag{{"noise", {{"amplitude", 2.5}}}, {"curl", {{"radius", 0.8}}}, {"clump", {{"lockRadius", 0.6}}}};
        const Prepared p = prepare(session, drag, "interpolate");
        auto engine = backend_->engine();
        engine->setBase(p.base, session.last().scalp.get());
        engine->run(p.run, false);
        const gpu::LookBounds b = gpu::lookBounds(p.run.params, p.run.fields, p.run.inputs, *p.base);
        ASSERT_TRUE(b.valid);
        std::size_t outside = 0;
        for (const Vec3f& v : engine->readPositions()) {
            outside += v.x < b.lo.x || v.y < b.lo.y || v.z < b.lo.z || v.x > b.hi.x || v.y > b.hi.y || v.z > b.hi.z;
        }
        const auto cpu = p.base->extent();
        std::printf("[ %-8s ] %s space: bounds grown %.3g past the base's, %zu points outside\n", GetParam().c_str(), space,
                    static_cast<double>(cpu.lo.x - b.lo.x), outside);
        EXPECT_EQ(outside, 0u);
    }
}

TEST(GpuPreview, AnOpenGLEngineKnowsWhenItsContextIsGone) {
    // What the Maya side asks before every drag (reachable()): an engine made in a context another
    // context does not share with (a new renderer, a device lost) is made again, never used.
    auto first = std::make_unique<GLBackend>();
    if (!first->available()) GTEST_SKIP() << "no OpenGL device";
    auto engine = first->engine();
    EXPECT_TRUE(engine->reachable());
    GLBackend second;  // current now, sharing nothing with the first
    ASSERT_TRUE(second.available());
    EXPECT_FALSE(engine->reachable());
    GroomSession session(groom(graph(0.3)));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    const Prepared p = prepare(session);
    EXPECT_THROW(engine->setBase(p.base, session.last().scalp.get()), std::runtime_error);
    auto again = second.engine();
    EXPECT_TRUE(again->reachable());
    EXPECT_TRUE(again->setBase(p.base, session.last().scalp.get()));
    again.reset();
    engine.reset();  // made in a context gone from the thread: lets go without touching the current one
}

TEST(GpuPreview, NoiseStaysUnderItsReach) {
    // lookBounds' kNoiseReach against the noise itself: its largest component over many samples.
    float largest = 0.0f;
    std::uint64_t h = 12345;
    for (int i = 0; i < 200000; ++i) {
        auto next = [&] {
            h = h * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<float>((h >> 33) % 100000) / 1000.0f - 50.0f;
        };
        const Vec3f q(next(), next(), next());
        const Vec3f n = vectorNoise(q, static_cast<std::uint64_t>(i % 7), 1 + i % 4);
        largest = std::max({largest, std::abs(n.x), std::abs(n.y), std::abs(n.z)});
    }
    std::printf("[ GPU      ] VectorNoise's largest component over 200000 samples: %.3f (reach %.2f)\n", largest,
                static_cast<double>(gpu::kNoiseReach));
    EXPECT_LT(largest, gpu::kNoiseReach * 0.8f);
}

TEST_P(LookEngineTest, AStrokeUploadsOnlyItsStrands) {
    // The base changed in a few strands (a brush stroke's incremental evaluation says which): only
    // their points go up, and the run gives what a full upload of the new base gives, to the bit.
    // (The CPU engine uploads nothing: it shares the base's own arrays, LookEngine::sharesBase.)
    if (GetParam() == "CPU") GTEST_SKIP() << "the CPU engine shares the base";
    GroomSession session(groom(graph(0.3)));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    const Prepared p = prepare(session, {}, "noise");
    auto stroked = std::make_shared<StrandSet>(*p.base);
    stroked->mutablePositions();
    std::vector<std::uint32_t> changed;
    for (std::uint32_t s = 100; s < 160; ++s) changed.push_back(s);
    for (std::uint32_t s = 3000; s < 3400; s += 3) changed.push_back(s);
    for (const std::uint32_t s : changed) {
        Vec3f* pts = stroked->strandPoints(s);
        for (std::size_t i = 1; i < stroked->strandPointCount(s); ++i) pts[i] += Vec3f(0.05f * static_cast<float>(i), 0.0f, -0.02f);
    }
    const StrandsPtr after(stroked);
    const StrandChanges changes = StrandChanges::listed(changed);

    auto incremental = backend_->engine();
    incremental->setBase(p.base, session.last().scalp.get());
    EXPECT_TRUE(incremental->setBase(after, session.last().scalp.get(), &changes));
    EXPECT_TRUE(incremental->lastUploadPartial());
    EXPECT_EQ(incremental->lastUploadStrands(), changed.size());
    const double partialMB = incremental->uploadMegabytes();
    gpu::LookRun run = p.run;
    run.fields = gpu::LookFields{};  // (none mapped)
    incremental->run(run, true);
    auto full = backend_->engine();
    full->setBase(after, session.last().scalp.get());
    EXPECT_FALSE(full->lastUploadPartial());
    full->run(run, true);
    std::printf("[ %-8s ] a stroke of %zu strands: %.3f MB up instead of %.3f MB\n", GetParam().c_str(), changed.size(), partialMB,
                full->uploadMegabytes());
    EXPECT_EQ(gpu::compareVectors(incremental->readPositions(), full->readPositions()).max, 0.0);
    std::vector<Vec3f> a, b;
    incremental->readFrames(&a, nullptr, nullptr);
    full->readFrames(&b, nullptr, nullptr);
    EXPECT_EQ(gpu::compareVectors(a, b).max, 0.0);
    // The graph cannot say (results came in between): the points compared find the same strands.
    auto compared = backend_->engine();
    compared->setBase(p.base, session.last().scalp.get());
    EXPECT_TRUE(compared->setBase(after, session.last().scalp.get()));
    EXPECT_TRUE(compared->lastUploadPartial());
    EXPECT_EQ(compared->lastUploadStrands(), changed.size());
    compared->run(run, true);
    EXPECT_EQ(gpu::compareVectors(compared->readPositions(), full->readPositions()).max, 0.0);
    // Other strands altogether (another count: a sparser Scatter, all of it shown): the whole base
    // goes up.
    LookGraph sparse;
    sparse.scatter = R"("params": {"density": 20.0, "length": 3.0})";
    GroomSession sparser(groom(sparse.json()));
    ASSERT_TRUE(sparser.evaluate(ViewRequest{}).strands);
    const Prepared q = prepare(sparser, {}, "noise");
    ASSERT_NE(q.base->strandCount(), p.base->strandCount());
    const StrandChanges all;
    compared->setBase(q.base, sparser.last().scalp.get(), &all);
    EXPECT_FALSE(compared->lastUploadPartial());
}

// ---- Clump's variation, curls by clump, fields after Interpolate, Part (B4a) ---------------------

// A part down the sphere's +z side from near its top (of `radius`).
std::string meridianParts(const ScalpMesh& scalp, float radius = 4.0f) {
    PartSet set;
    std::vector<Vec3f> meridian;
    for (int i = 0; i <= 10; ++i) {
        const float a = 0.15f + 1.2f * static_cast<float>(i) / 10.0f;
        meridian.push_back({0.0f, radius * std::cos(a), radius * std::sin(a)});
    }
    set.parts.push_back(PartSet::fromPoints("meridian", meridian, scalp));
    return set.toJson();
}

TEST_P(LookEngineTest, ClumpVariesItsClumpsAsTheCpuDoes) {
    // Each clump's strength and lock radius varied by its center's ID, volumize (a number, then a
    // map, along its profile), flatness lying along the scalp, turned at random or along a
    // direction: from Clump as the CPU chose its centers, and from Interpolate with its centers
    // made again (and given frames) from the strands it moves; then drags of each.
    const std::string varied = R"("strength": 0.8, "strengthRandom": 0.6, "lockRadius": 0.12, "lockRadiusRandom": 0.5, "seed": 3)";
    const std::string clumps[] = {
        "{" + varied + "}",
        "{" + varied + R"(, "volumize": 0.4, "volumizeProfile": [[0, 1], [0.6, 0.4], [1, 0]]})",
        R"({"strength": 0.9, "volumize": {"expression": "0.2 + 0.3 * rand", "min": 0.0, "max": 1.0}})",
        R"({"strength": 0.9, "flatness": 0.7})",
        R"({"strength": 0.9, "flatness": 0.6, "flatten": "random", "lockRadius": 0.1})",
        R"({"strength": 0.9, "flatness": 0.8, "flatten": "direction", "flattenDirection": [0, 1, 1]})"};
    for (const std::string& params : clumps) {
        for (const bool guides : {false, true}) {
            // (A flat clump lying along the scalp, on strands straight out of it: its thin way is
            // the scalp's normal less its part along the strand, a rounding's length, whose way is
            // the rounding's on the CPU as on the GPU. Bent by Interpolate, it is the CPU's.)
            if (!guides && params == R"({"strength": 0.9, "flatness": 0.7})") continue;
            SCOPED_TRACE(params + (guides ? " from Interpolate" : ""));
            LookGraph g;
            g.guides = guides;
            g.clump = R"("params": )" + params;
            GroomSession session(groom(g.json()));
            ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
            expectClose(runAndCompare(session, {}, session, guides ? "interpolate" : "clump"));
        }
    }
    const std::pair<const char*, double> drags[] = {{"strengthRandom", 0.3}, {"lockRadiusRandom", 0.9}, {"volumize", 0.25},
                                                    {"flatness", 0.4}, {"seed", 7.0}};
    for (const auto& [param, value] : drags) {
        SCOPED_TRACE(param);
        LookGraph g;
        g.clump = R"("params": {"centersFrom": "inputPoints", "strength": 0.8, "strengthRandom": 0.6, "lockRadius": 0.12, "lockRadiusRandom": 0.5,
                                "volumize": 0.2, "flatness": 0.3, "seed": 3})";
        g.nodes = R"(, {"name": "spots", "type": "Scatter", "params": {"density": 0.3, "seed": 4, "previewAll": true}})";
        g.connections = R"(, {"from": "scalp", "to": "spots", "input": "scalp"}, {"from": "spots", "to": "clump", "input": "centers"})";
        nlohmann::json want = nlohmann::json::parse(g.json());
        for (auto& n : want["nodes"]) {
            if (n["name"] == "clump") n["params"][param] = std::string(param) == "seed" ? nlohmann::json(7) : nlohmann::json(value);
        }
        EXPECT_GPU_MATCHES(g.json(), (gpu::Overrides{{"clump", {{param, value}}}}), want.dump());
    }
}

TEST_P(LookEngineTest, ClumpSharesItsNeighboursShapesAsTheCpuDoes) {
    // copyAmount: each clump takes on its nearest clumps' shapes. Its own centers made from the
    // strands Interpolate moves (resampled, shared and given frames on the GPU), centers fed in with
    // centersFrom inputPoints (axes gathering no strand neither give nor take), and as the CPU made them.
    LookGraph own;
    own.guides = true;
    own.clump = R"("params": {"strength": 0.85, "copyAmount": 0.6, "copyNeighbors": 3, "lockRadius": 0.08})";
    LookGraph fed = own;
    fed.clump = R"("params": {"centersFrom": "inputPoints", "strength": 0.85, "copyAmount": 0.5, "copyNeighbors": 4, "flatness": 0.4})";
    fed.nodes = R"(, {"name": "spots", "type": "Scatter", "params": {"density": 0.3, "seed": 4, "previewAll": true}})";
    fed.connections = R"(, {"from": "scalp", "to": "spots", "input": "scalp"}, {"from": "spots", "to": "clump", "input": "centers"})";
    for (const LookGraph* g : {&own, &fed}) {
        GroomSession session(groom(g->json()));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const Prepared p = prepare(session, {}, "interpolate");
        ASSERT_TRUE(p.run.inputs[1].clump);
        EXPECT_TRUE(p.run.inputs[1].clump->chosen) << "the centers move with the strands Interpolate makes";
        EXPECT_TRUE(p.run.inputs[1].clump->neighbors);
        expectClose(runAndCompare(session, {}, session, "interpolate"));
        expectClose(runAndCompare(session, {}, session, "clump"));
    }
}

TEST_P(LookEngineTest, CurlsEachClumpAsOneAsTheCpuDoes) {
    // Curls coherent by clump (the clumps' middles from their core strands): a helix and a wave,
    // from Clump, from Curl and from Interpolate; a phase dragged; and in a preview holding hidden
    // core strands (the middles of every core strand, shown or not).
    for (const char* shape : {"helix", "wave"}) {
        LookGraph g;
        g.curl = std::string(R"("params": {"radius": 0.12, "frequency": 0.7, "randomness": 0.4, "coherence": "clump", "shape": ")") +
                 shape + R"("})";
        for (const bool guides : {false, true}) {
            SCOPED_TRACE(std::string(shape) + (guides ? " from Interpolate" : ""));
            g.guides = guides;
            GroomSession session(groom(g.json()));
            ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
            expectClose(runAndCompare(session, {}, session, "curl"));
            expectClose(runAndCompare(session, {}, session, guides ? "interpolate" : "clump"));
        }
    }
    LookGraph g;
    g.curl = R"("params": {"radius": 0.12, "frequency": 0.7, "phase": 0.1, "coherence": "clump"})";
    LookGraph c = g;
    c.curl = R"("params": {"radius": 0.12, "frequency": 0.7, "phase": 0.35, "coherence": "clump"})";
    EXPECT_GPU_MATCHES(g.json(), (gpu::Overrides{{"curl", {{"phase", 0.35}}}}), c.json());
    GroomSession preview(groom(g.json()));
    ViewRequest half;
    half.density = 0.5f;
    ASSERT_TRUE(preview.evaluate(half).strands);
    expectClose(runAndCompare(preview, {}, preview, "clump"));
}

TEST_P(LookEngineTest, FieldsAfterInterpolateAsTheCpuMakesThem) {
    // Mapped params of nodes after a running Interpolate: of the strands' shapes ($length /
    // $longest: made on the GPU from the run's points, the longest of the core strands), of the
    // guides' influence (Interpolate's channel: made by its kernel), of the roots alone (they hold);
    // a Clump picking its centers by the influence (its choice holds while the binding does).
    LookGraph shaped;
    shaped.guides = true;
    shaped.clump = R"("params": {"strength": {"expression": "$length / $longest", "min": 0.3, "max": 1.0}})";
    shaped.length = true;
    shaped.lengthNode = R"("params": {"keep": {"expression": "mix(0.6, 1, smoothstep(-0.2, 0.8, $ny)) - 0.05 * $straightness", "min": 0.0, "max": 1.0}})";
    LookGraph influenced;
    influenced.guides = true;
    influenced.interpolate = R"("params": {"limitDistance": 1.6, "falloff": [[0, 1], [0.35, 0.85], [1, 0]]})";
    influenced.clump = R"("params": {"strength": {"attribute": "guideInfluence", "min": 0.0, "max": 0.85},
                                     "centerWeight": {"attribute": "guideInfluence"}})";
    for (const LookGraph* g : {&shaped, &influenced}) {
        GroomData data = groom(g->json());
        GroomSession session(data);
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const Prepared p = prepare(session, {}, "interpolate");
        ASSERT_GE(p.run.fields.stages.size(), 2u);
        EXPECT_TRUE(p.run.fields.stages[1].field[0].program) << "Clump's strength made on the GPU";
        expectClose(runAndCompare(session, {}, session, "interpolate"));
        // A dab: the guides bent (their roots kept).
        const StrandsPtr before = session.last().strands;
        const GuidesPtr bent = guideSet(*data.scalp, 1.4f);
        session.setGuides(bent);
        ViewRequest dab;
        dab.stopAt = "interpolate";
        ASSERT_EQ(session.evaluate(dab).strands, before);
        GroomData after = data;
        after.guides = bent;
        GroomSession fresh(after);
        ASSERT_TRUE(fresh.evaluate(ViewRequest{}).strands);
        expectClose(runAndCompare(session, {}, fresh, "interpolate"));
    }
    // The influence's radius dragged: the strength read from it made again (no centerWeight).
    LookGraph reach = influenced;
    reach.clump = R"("params": {"strength": {"attribute": "guideInfluence", "min": 0.0, "max": 0.85}})";
    LookGraph reached = reach;
    reached.interpolate = R"("params": {"limitDistance": 1.1, "falloff": [[0, 1], [0.35, 0.85], [1, 0]]})";
    EXPECT_GPU_MATCHES(reach.json(), (gpu::Overrides{{"interpolate", {{"limitDistance", 1.1}}}}), reached.json());
    // ... where it picks the centers too, the CPU's.
    GroomSession picked(groom(influenced.json()));
    ASSERT_TRUE(picked.evaluate(ViewRequest{}).strands);
    Prepared refused;
    EXPECT_FALSE(prepareRun(picked, {{"interpolate", {{"limitDistance", 1.1}}}}, "", &refused));
    EXPECT_NE(refused.why.find("centerWeight"), std::string::npos) << refused.why;
}

TEST_P(LookEngineTest, PartLeansAwayAsTheCpuDoes) {
    // Part after the look (each strand's side from its root: the CPU's), from Part, from Clump and
    // from Interpolate; its strength and width dragged.
    for (const bool guides : {false, true}) {
        SCOPED_TRACE(guides ? "from Interpolate" : "from Clump");
        LookGraph g;
        g.guides = guides;
        g.after = "Part";
        GroomData data = groom(g.json());
        data.partsJson = meridianParts(*data.scalp);
        GroomSession session(data);
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const Prepared p = prepare(session, {}, "after");
        ASSERT_EQ(p.chain.stages.size(), 1u);
        EXPECT_EQ(p.chain.stages[0].kind, gpu::StageKind::Part);
        std::size_t parted = 0;
        for (std::size_t s = 0; s < p.base->strandCount(); ++s) parted += (*p.run.inputs[0].part->sides)[s * 4 + 3] >= 0.0f;
        EXPECT_GT(parted, 50u);
        const Compared alone = runAndCompare(session, {}, session, "after");  // (no Width runs: the positions)
        EXPECT_LT(alone.positions.max, kPointTolerance);
        EXPECT_LT(alone.positions.mean, kMeanTolerance);
        expectClose(runAndCompare(session, {}, session, guides ? "interpolate" : "clump"));
    }
    const std::pair<const char*, double> drags[] = {{"strength", 0.4}, {"width", 0.8}};
    for (const auto& [param, value] : drags) {
        LookGraph g;
        g.after = "Part";
        nlohmann::json committed = nlohmann::json::parse(g.json());
        for (auto& n : committed["nodes"]) {
            if (n["name"] == "after") n["params"][param] = value;
        }
        GroomData dragged = groom(g.json()), made = groom(committed.dump());
        dragged.partsJson = made.partsJson = meridianParts(*dragged.scalp);
        GroomSession a(dragged), b(made);
        ASSERT_TRUE(a.evaluate(ViewRequest{}).strands);
        ASSERT_TRUE(b.evaluate(ViewRequest{}).strands);
        const Compared c = runAndCompare(a, {{"after", {{param, value}}}}, b);
        EXPECT_LT(c.positions.max, kPointTolerance);
        EXPECT_LT(c.positions.mean, kMeanTolerance);
        EXPECT_LT(c.normals.max, kFrameTolerance);
    }
}

std::string withParam(const std::string& graphJson, const std::string& name, const std::string& param, const nlohmann::json& value);

// Guides around the upper sphere (of `radius`) combed apart at x = `x0`: straight, each along the
// scalp toward -x or +x by its side (and a little back), lifted by `lift` along the normal: a
// parting an auto Part finds.
GuidesPtr partedGuides(const ScalpMesh& scalp, float x0 = 0.0f, float lift = 0.4f, float radius = 4.0f) {
    auto guides = std::make_shared<GuideSet>();
    std::uint64_t id = 1;
    for (int ring = 1; ring <= 4; ++ring) {
        const float theta = 0.25f * static_cast<float>(ring);
        for (int k = 0; k < 10; ++k) {
            const float phi = 6.2831853f * (static_cast<float>(k) + 0.5f * static_cast<float>(ring % 2)) / 10.0f;
            const Vec3f n(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
            const Vec3f comb(n.x * radius < x0 ? -1.0f : 1.0f, 0.0f, 0.3f);
            const Vec3f along = (comb - n * n.dot(comb)).normalized();
            const Vec3f dir = (along + n * lift).normalized();
            std::vector<Vec3f> pts;
            for (int i = 0; i <= 6; ++i) pts.push_back(n * radius + dir * (3.0f * static_cast<float>(i) / 6.0f));
            guides->add(id++, std::move(pts));
        }
    }
    guides->rootOnMesh(scalp);
    return guides;
}

// LookGraph's chain from guides combed apart (partedGuides), then an auto Part (`partParams`: its
// params past partsFrom) after the look; Interpolate and Clump not respecting parts (so a painted
// copy of the lines the Part found changes nothing before it).
GroomData autoPartGroom(const std::string& partParams = "", float x0 = 0.0f) {
    LookGraph g;
    g.guides = true;
    g.interpolate = R"("params": {"respectParts": false})";
    g.clump = R"("params": {"strength": 0.8, "respectParts": false})";
    g.after = "Part";
    g.afterParams = R"("params": {"partsFrom": "auto")" + (partParams.empty() ? "" : ", " + partParams) + "}";
    GroomData data = groom(g.json());
    data.guides = partedGuides(*data.scalp, x0);
    return data;
}

// The lines the auto Part "after" kept for its cached result (false: none kept).
bool keptAutoParts(GroomSession& session, PartsPtr* lines) {
    const GraphDocument& doc = *session.viewDocument();
    const NodeId part = doc.nodesByName.at("after");
    return autoPartState(doc.graph.node(part), valueAs<StrandsPtr>(doc.graph.cachedValue(part)), lines);
}

TEST_P(LookEngineTest, AnAutoPartOpensTheLinesItFoundAsTheCpuDoes) {
    // An auto Part after Interpolate: the lines its evaluation found kept with its result, each
    // strand's side of them as the CPU opens them; from Part, and from Interpolate.
    GroomData data = autoPartGroom();
    GroomSession session(data);
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    PartsPtr lines;
    ASSERT_TRUE(keptAutoParts(session, &lines));
    ASSERT_TRUE(lines && !lines->empty()) << "the parting found";
    {
        const Prepared p = prepare(session, {}, "after");
        ASSERT_EQ(p.chain.stages.size(), 1u);
        EXPECT_EQ(p.chain.stages[0].kind, gpu::StageKind::Part);
        std::size_t parted = 0;
        for (std::size_t s = 0; s < p.base->strandCount(); ++s) parted += (*p.run.inputs[0].part->sides)[s * 4 + 3] >= 0.0f;
        EXPECT_GT(parted, 50u);
    }
    expectPointsClose(runAndCompare(session, {}, session, "after"));  // (no Width runs: the positions)
    expectClose(runAndCompare(session, {}, session, "interpolate"));

    // Its width and strength dragged: the lines stay (found from the same strands).
    const std::pair<const char*, double> drags[] = {{"strength", 0.4}, {"width", 0.8}, {"width", 2.5}};
    for (const auto& [param, value] : drags) {
        SCOPED_TRACE(param);
        GroomData committed = autoPartGroom(std::string("\"") + param + "\": " + std::to_string(value));
        GroomSession a(data), b(committed);
        ASSERT_TRUE(a.evaluate(ViewRequest{}).strands);
        ASSERT_TRUE(b.evaluate(ViewRequest{}).strands);
        expectPointsClose(runAndCompare(a, {{"after", {{param, value}}}}, b));
    }

    // Finding none (minLength the whole scalp): the strands handed on as they came, as on the CPU.
    GroomSession none(autoPartGroom(R"("minLength": 1.0)"));
    ASSERT_TRUE(none.evaluate(ViewRequest{}).strands);
    PartsPtr nothing;
    ASSERT_TRUE(keptAutoParts(none, &nothing));
    EXPECT_FALSE(nothing);
    expectPointsClose(runAndCompare(none, {}, none, "after"));
    expectClose(runAndCompare(none, {}, none, "interpolate"));
}

TEST_P(LookEngineTest, ADabUpstreamOfAnAutoPartOpensTheLinesItFound) {
    // A dab combing the guides' parting over: Interpolate's strands blended again on the GPU, the
    // Part opening the lines its last evaluation found (the release finds them again). Against a
    // CPU evaluation of the dabbed guides opening those same lines (painted).
    GroomData data = autoPartGroom();
    GroomSession dabbed(data);
    ASSERT_TRUE(dabbed.evaluate(ViewRequest{}).strands);
    PartsPtr kept;
    ASSERT_TRUE(keptAutoParts(dabbed, &kept));
    ASSERT_TRUE(kept && !kept->empty());
    const GraphDocument& doc = *dabbed.viewDocument();
    const StrandsPtr partIn = valueAs<StrandsPtr>(doc.graph.cachedValue(doc.nodesByName.at("resample")));
    ASSERT_TRUE(partIn);
    const PartSet found = findAutoParts(*partIn, *dabbed.last().scalp, 90.0f, 0.15f);
    EXPECT_EQ(PartLines(found, *dabbed.last().scalp).contentHash(), kept->contentHash());

    const StrandsPtr before = dabbed.last().strands;
    const GuidesPtr combed = partedGuides(*data.scalp, 0.6f);  // (the same roots)
    dabbed.setGuides(combed);
    ViewRequest dab;
    dab.stopAt = "interpolate";
    ASSERT_EQ(dabbed.evaluate(dab).strands, before);

    GroomData painted = data;
    painted.guides = combed;
    painted.graphJson = withParam(data.graphJson, "after", "partsFrom", "painted");
    painted.partsJson = found.toJson();
    GroomSession fresh(painted);
    ASSERT_TRUE(fresh.evaluate(ViewRequest{}).strands);
    expectClose(runAndCompare(dabbed, {}, fresh, "interpolate"));

    // (The release finds other lines: the preview's were stale.)
    GroomData released = data;
    released.guides = combed;
    GroomSession again(released);
    ASSERT_TRUE(again.evaluate(ViewRequest{}).strands);
    PartsPtr refound;
    ASSERT_TRUE(keptAutoParts(again, &refound));
    EXPECT_TRUE(!refound || refound->contentHash() != kept->contentHash());
}

// ---- Gravity, Flyaway, Collide, Curl's coil (B4b) ----------------------------------------------------

// Scatter's strands shaped by Interpolate from the guides, then `nodes` in order (type, then its
// JSON past the type), then (with `width`) a Width, then Output. Node k is named "n<k>"; a Collide
// takes the scalp and, with `collider`, the ColliderMesh "ball" (GroomData::meshes "collider").
std::string chainGraph(const std::vector<std::pair<std::string, std::string>>& nodes, bool width = true, bool collider = false) {
    std::string j = R"({"nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 40.0, "length": 3.0}},
        {"name": "guides", "type": "Guides"}, {"name": "interpolate", "type": "Interpolate"},)";
    if (collider) j += R"({"name": "ball", "type": "ColliderMesh", "params": {"name": "collider"}},)";
    for (std::size_t k = 0; k < nodes.size(); ++k) {
        // (A Curl adding no points: LookGraph::noPointsAdded.)
        const std::string params = nodes[k].first == "Curl" ? LookGraph::noPointsAdded(nodes[k].second.empty() ? R"("params": {})" : nodes[k].second)
                                                            : nodes[k].second;
        j += R"({"name": "n)" + std::to_string(k) + R"(", "type": ")" + nodes[k].first + "\"" + (params.empty() ? "" : ", " + params) + "},";
    }
    if (width) j += R"({"name": "width", "type": "Width", "params": {"width": 0.01, "randomness": 0.2}},)";
    j += R"({"name": "output", "type": "Output"}], "connections": [
        {"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "interpolate", "input": "strands"}, {"from": "guides", "to": "interpolate", "input": "guides"},
        {"from": "scalp", "to": "interpolate", "input": "scalp"})";
    std::string last = "interpolate";
    for (std::size_t k = 0; k < nodes.size(); ++k) {
        const std::string name = "n" + std::to_string(k);
        j += R"(, {"from": ")" + last + R"(", "to": ")" + name + R"(", "input": "strands"})";
        if (nodes[k].first == "Collide") {
            j += R"(, {"from": "scalp", "to": ")" + name + R"(", "input": "scalp"})";
            if (collider) j += R"(, {"from": "ball", "to": ")" + name + R"(", "input": "collider"})";
        }
        last = name;
    }
    if (width) {
        j += R"(, {"from": ")" + last + R"(", "to": "width", "input": "strands"})";
        last = "width";
    }
    return j + R"(, {"from": ")" + last + R"(", "to": "output", "input": "strands"}], "output": "output"})";
}

// `graphJson` with node `name`'s param set to `value` (what a drag's release commits).
std::string withParam(const std::string& graphJson, const std::string& name, const std::string& param, const nlohmann::json& value) {
    nlohmann::json g = nlohmann::json::parse(graphJson);
    for (auto& n : g["nodes"]) {
        if (n["name"] == name) n["params"][param] = value;
    }
    return g.dump();
}

// A dab on `data`'s guides (bent further, their roots kept), evaluated up to Interpolate alone, run
// on the GPU from it against a fresh CPU evaluation of the same guides.
#define EXPECT_DAB_MATCHES(data)                                          \
    do {                                                                 \
        GroomSession dabbed(data);                                       \
        ASSERT_TRUE(dabbed.evaluate(ViewRequest{}).strands);             \
        const StrandsPtr before = dabbed.last().strands;                 \
        const GuidesPtr bent = guideSet(*(data).scalp, 1.4f);            \
        dabbed.setGuides(bent);                                          \
        ViewRequest dab;                                                 \
        dab.stopAt = "interpolate";                                      \
        ASSERT_EQ(dabbed.evaluate(dab).strands, before);                 \
        GroomData after = (data);                                        \
        after.guides = bent;                                             \
        GroomSession fresh(after);                                       \
        ASSERT_TRUE(fresh.evaluate(ViewRequest{}).strands);              \
        expectClose(runAndCompare(dabbed, {}, fresh, "interpolate"));    \
    } while (0)

TEST_P(LookEngineTest, GravityHangsAsTheCpuDoes) {
    // Gravity after the look (its strength a number, then by a map per strand), from Gravity and
    // from Interpolate; a dab; drags of its strength, stiffness and stiffness profile.
    const std::string params[] = {R"("params": {"strength": 0.8, "stiffness": 0.6})",
                                  R"("params": {"strength": {"expression": "0.4 + 0.8 * rand", "min": 0.0, "max": 2.0},
                                                "stiffness": 0.3, "direction": [0.3, -1.0, 0.2]})"};
    for (const std::string& p : params) {
        SCOPED_TRACE(p);
        LookGraph g;
        g.guides = true;
        g.after = "Gravity";
        g.afterParams = p;
        GroomData data = groom(withoutClearance(g.json()));
        GroomSession session(data);
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        expectPointsClose(runAndCompare(session, {}, session, "after"));
        expectClose(runAndCompare(session, {}, session, "interpolate"));
        EXPECT_DAB_MATCHES(data);
    }
    LookGraph g;
    g.after = "Gravity";
    g.afterParams = R"("params": {"strength": 0.8, "stiffness": 0.6})";
    const std::string graph = g.json();
    EXPECT_GPU_POINTS_MATCH(graph, (gpu::Overrides{{"after", {{"strength", 1.4}}}}), withParam(graph, "after", "strength", 1.4));
    EXPECT_GPU_POINTS_MATCH(graph, (gpu::Overrides{{"after", {{"stiffness", 0.2}}}}), withParam(graph, "after", "stiffness", 0.2));
    EXPECT_GPU_POINTS_MATCH(graph, (rampDrag("after", "stiffnessProfile", "[[0, 1], [0.5, 0.1], [1, 0]]")),
                       withParam(graph, "after", "stiffnessProfile", nlohmann::json::parse("[[0, 1], [0.5, 0.1], [1, 0]]")));
}

TEST_P(LookEngineTest, FlyawaysStrayAsTheCpuDoes) {
    // Strays (a fraction by ID, then by a map per strand) wandering, standing out and lengthened,
    // from Flyaway and from Interpolate; a dab; drags of each of its numbers and its seed.
    const std::string params[] = {
        R"("params": {"fraction": 0.3, "stray": 0.9, "standOut": 0.4, "lengthen": 0.3, "start": 0.2, "seed": 5})",
        R"("params": {"fraction": {"expression": "0.5 * rand", "min": 0.0, "max": 1.0}, "stray": {"expression": "0.5 + rand", "min": 0.0, "max": 2.0},
                      "frequency": 2.5})"};
    for (const std::string& p : params) {
        SCOPED_TRACE(p);
        LookGraph g;
        g.guides = true;
        g.after = "Flyaway";
        g.afterParams = p;
        GroomData data = groom(withoutClearance(g.json()));
        GroomSession session(data);
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const std::vector<float>* strays = session.last().strands->strandChannel("flyaway");
        ASSERT_TRUE(strays);
        EXPECT_GT(std::count(strays->begin(), strays->end(), 1.0f), 50);
        expectPointsClose(runAndCompare(session, {}, session, "after"));
        expectClose(runAndCompare(session, {}, session, "interpolate"));
        EXPECT_DAB_MATCHES(data);
    }
    LookGraph g;
    g.after = "Flyaway";
    g.afterParams = params[0];
    const std::string graph = g.json();
    const std::pair<const char*, double> drags[] = {{"stray", 1.6},   {"standOut", 0.8}, {"lengthen", 0.6}, {"start", 0.5},
                                                    {"frequency", 3.0}, {"fraction", 0.5}, {"seed", 9.0}};
    for (const auto& [param, value] : drags) {
        SCOPED_TRACE(param);
        const nlohmann::json stored = std::string(param) == "seed" ? nlohmann::json(9) : nlohmann::json(value);
        EXPECT_GPU_POINTS_MATCH(graph, (gpu::Overrides{{"after", {{param, value}}}}), withParam(graph, "after", param, stored));
    }
}

TEST_P(LookEngineTest, CollideKeepsStrandsOutAsTheCpuDoes) {
    // Strands run through a ball over the head (a ColliderMesh), or hung into the scalp by Gravity,
    // pushed out by Collide through its surfaces' fields as tables: the GPU's walk is the CPU's
    // `method` field, which takes the fields' answers alone (where the field would ask the mesh, the
    // closest point among the triangles around the ones it holds there).
    struct Case {
        const char* name;
        std::vector<std::pair<std::string, std::string>> nodes;
        bool collider;
    };
    const std::string shallow = R"("params": {"offset": 0.12, "maxDepth": 0.6})", deep = R"("params": {"offset": 0.12, "maxDepth": 3.0})";
    const std::string hang = R"("params": {"strength": 1.6, "stiffness": 0.2})";
    const Case cases[] = {{"through a ball", {{"Collide", shallow}}, true},
                          {"hung into the scalp", {{"Gravity", hang}, {"Collide", deep}}, false},
                          {"hung into the scalp and through a ball", {{"Gravity", hang}, {"Collide", shallow}}, true}};
    for (const Case& k : cases) {
        SCOPED_TRACE(k.name);
        GroomData data = groom(withoutClearance(chainGraph(k.nodes, true, k.collider)));
        if (k.collider) {
            const MeshPtr sphere = test::makeSphere(16, 32, 1.2f);
            std::vector<Vec3f> points = sphere->points();
            for (Vec3f& q : points) q = q + Vec3f(0.5f, 6.0f, 0.0f);
            data.meshes["collider"] = std::make_shared<ScalpMesh>(points, sphere->triangles());
        }
        GroomSession session(data);
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const GraphDocument& doc = *session.viewDocument();
        const std::string collideNode = "n" + std::to_string(k.nodes.size() - 1);
        const NodeId node = doc.nodesByName.at(collideNode);
        NodeId source = 0;
        ASSERT_TRUE(doc.graph.inputSource(node, 0, &source));
        const StrandsPtr taken = valueAs<StrandsPtr>(doc.graph.cachedValue(source));
        const StrandsPtr pushed = valueAs<StrandsPtr>(doc.graph.cachedValue(node));
        ASSERT_TRUE(taken && pushed);
        std::size_t moved = 0;
        for (std::size_t i = 0; i < taken->pointCount(); ++i) moved += (taken->positions()[i] - pushed->positions()[i]).length() > 1e-3f;
        EXPECT_GT(moved, 500u);
        const ParamSet& params = doc.graph.node(node).params();
        const auto offset = static_cast<float>(params.getDouble("offset")), maxDepth = static_cast<float>(params.getDouble("maxDepth"));
        std::vector<Value> inputs{taken, session.last().scalp};
        std::vector<MeshPtr> meshes{session.last().scalp};
        if (k.collider) {
            inputs.push_back(data.meshes.at("collider"));
            meshes.push_back(data.meshes.at("collider"));
        }
        auto collide = [&](const StrandsPtr& in, const char* method) {
            auto op = OperatorRegistry::instance().create(op::kCollide);
            for (const char* name : {"offset", "maxDepth"}) op->params().set(name, params.getDouble(name));
            op->params().set("iterations", params.getInt("iterations"));
            op->params().set("method", std::string(method));
            std::vector<Value> with = inputs;
            with[0] = in;
            return valueAs<StrandsPtr>(op->evaluate(EvalContext{}, with));
        };
        auto over = [](const std::vector<Vec3f>& a, const std::vector<Vec3f>& b, float apart) {
            std::size_t n = 0;
            for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) n += (a[i] - b[i]).length() > apart;
            return n;
        };
        // The CPU's field against its exact method on the same strands: a small fraction of the
        // offset apart on average (a strand pushed out from deep inside folds along the surface, and
        // where it folds the two may part); no point left deeper under the offset than a few percent
        // of it (the field's blend near the surface), each surface's own closest point measuring
        // them within the depth Collide pushes from, nor inside a surface.
        {
            const StrandsPtr exact = collide(taken, "exact");
            ASSERT_TRUE(exact);
            struct Under {
                std::size_t count = 0, inside = 0;
                float deepest = 0.0f;
            };
            auto under = [&](const StrandSet& s) {
                Under u;
                for (std::size_t j = 0; j < s.strandCount(); ++j) {
                    for (std::size_t i = 1; i < s.strandPointCount(j); ++i) {
                        const Vec3f q = s.strandPoints(j)[i];
                        for (const MeshPtr& mesh : meshes) {
                            const SurfacePoint sp = mesh->closestPoint(q);
                            const float h = (q - sp.position).dot(sp.normal);
                            if (h <= -maxDepth) continue;
                            u.count += h < offset * 0.97f;
                            u.inside += h < 0.0f;
                            u.deepest = std::max(u.deepest, offset - h);
                        }
                    }
                }
                return u;
            };
            const Under uField = under(*pushed), uExact = under(*exact);
            const gpu::PointError e = gpu::compareVectors(pushed->positions(), exact->positions());
            std::printf("[ %-8s ] %s: the CPU's field against its exact method: max %.3g, mean %.3g, %zu points over 1e-3 apart; "
                        "under the offset by over 3%% of it: field %zu (deepest %.4f under it, %zu inside), exact %zu (%.4f, %zu)\n",
                        GetParam().c_str(), k.name, e.max, e.mean, over(pushed->positions(), exact->positions(), 1e-3f),
                        uField.count, uField.deepest, uField.inside, uExact.count, uExact.deepest, uExact.inside);
            EXPECT_LT(e.mean, 0.005 * offset);
            EXPECT_LT(uField.deepest, 0.05f * offset);
            EXPECT_EQ(uField.inside, 0u);
        }
        // How far the CPU's own Collide moves for its input rounded otherwise by about as much as
        // the GPU's Gravity rounds it (GravityHangsAsTheCpuDoes: 2e-5): a point on the edge of one of
        // the walk's decisions (a push or not, the field's blend or its answer from closest faces)
        // falls the other way, by the field's own error, and its strand after it.
        auto nudged = std::make_shared<StrandSet>(*taken);
        Rng rng(3);
        for (Vec3f& q : nudged->mutablePositions()) {
            q += Vec3f(rng.uniform(-2e-5f, 2e-5f), rng.uniform(-2e-5f, 2e-5f), rng.uniform(-2e-5f, 2e-5f));
        }
        const StrandsPtr renudged = collide(nudged, "field");
        ASSERT_TRUE(renudged);
        const gpu::PointError sensitive = gpu::compareVectors(renudged->positions(), pushed->positions());
        const std::size_t sensitiveOver = over(renudged->positions(), pushed->positions(), 1e-4f);
        std::printf("[ %-8s ] %s: the CPU's Collide with its input nudged by 2e-5: max %.3g, mean %.3g, %zu points over 1e-4 apart\n",
                    GetParam().c_str(), k.name, sensitive.max, sensitive.mean, sensitiveOver);
        const Prepared p = prepare(session, {}, "interpolate");
        ASSERT_EQ(p.chain.stages[k.nodes.size()].kind, gpu::StageKind::Collide);
        EXPECT_EQ(p.run.inputs[k.nodes.size()].collide->tables.size(), k.collider ? 2u : 1u);
        // From Collide: the CPU's Collide, to the bit.
        {
            const Prepared q = prepare(session, {}, collideNode);
            auto engine = backend_->engine();
            engine->setBase(q.base, session.last().scalp.get());
            engine->run(q.run, false);
            const gpu::PointError e = gpu::compareVectors(engine->readPositions(), pushed->positions());
            std::printf("[ %-8s ] %s: Collide moves %zu of %zu points; the GPU against the CPU: max %.3g\n", GetParam().c_str(),
                        k.name, moved, taken->pointCount(), e.max);
            // (OpenGL: the driver's compiler rounds a step or two of the walk an ulp otherwise.)
            EXPECT_LE(e.max, GetParam() == "DirectX11" ? 0.0 : 2e-6);
        }
        // Against the CPU's Collide (`method` field: the same walk), from Collide; from Interpolate;
        // and a dab (the binding from the press a rounding from a fresh one's). After Gravity, whose
        // turns the GPU rounds otherwise than the CPU, Collide takes points rounded otherwise: apart
        // no more than the CPU's own Collide for such a rounding of its input (above).
        const bool rounded = k.nodes.size() > 1;
        auto expectCollided = [&](const Compared& c, bool afterRounding) {
            if (!afterRounding) {
                expectPointsClose(c);
            } else {
                EXPECT_GT(c.positions.count, 1000u);
                EXPECT_LE(c.positions.max, std::max(kPointTolerance, 2.0 * sensitive.max));
                EXPECT_LE(c.positions.mean, std::max(kMeanTolerance, 2.0 * sensitive.mean));
                EXPECT_LE(c.over, sensitiveOver);
            }
            EXPECT_LT(c.widthMax, 1e-5);
        };
        expectCollided(runAndCompare(session, {}, session, collideNode), false);
        expectCollided(runAndCompare(session, {}, session, "interpolate"), rounded);
        GroomSession dabbed(data);
        ASSERT_TRUE(dabbed.evaluate(ViewRequest{}).strands);
        const StrandsPtr before = dabbed.last().strands;
        const GuidesPtr bent = guideSet(*data.scalp, 1.4f);
        dabbed.setGuides(bent);
        ViewRequest dab;
        dab.stopAt = "interpolate";
        ASSERT_EQ(dabbed.evaluate(dab).strands, before);
        GroomData moved2 = data;
        moved2.guides = bent;
        GroomSession fresh(moved2);
        ASSERT_TRUE(fresh.evaluate(ViewRequest{}).strands);
        expectCollided(runAndCompare(dabbed, {}, fresh, "interpolate"), rounded);
    }
    // Over a scalp deformed away from its rest pose (asked through each vertex's frame): the CPU's,
    // and the GPU runs from the node after Collide. Asked of the mesh at every point: refused.
    GroomData deformed = groom(withoutClearance(chainGraph({{"Collide", R"("params": {"offset": 0.12})"}})));
    auto moved = std::make_shared<ScalpMesh>(*deformed.scalp);
    std::vector<Vec3f> rest = moved->points();
    for (Vec3f& q : rest) q = q * 0.98f;
    moved->setRestPoints(rest);
    deformed.scalp = moved;
    GroomSession session(deformed);
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    gpu::LookChain chain;
    std::string why;
    ASSERT_TRUE(gpu::findLookChain(*session.viewDocument(), &chain, &why)) << why;
    EXPECT_EQ(session.viewDocument()->nameOf(chain.base), "n0");
    GroomSession exact(groom(chainGraph({{"Collide", R"("params": {"offset": 0.12, "method": "exact"})"}})));
    ASSERT_TRUE(exact.evaluate(ViewRequest{}).strands);
    Prepared refused;
    EXPECT_FALSE(prepareRun(exact, {}, "", &refused));
    EXPECT_NE(refused.why.find("exact"), std::string::npos) << refused.why;
}

TEST_P(LookEngineTest, CollideEasesAndSmoothsAsTheCpuDoes) {
    // Collide's softness (the push eased in over a band above the offset) and smoothing (the
    // correction spread toward the root, the strand bent segment by segment), alone and together,
    // through a ball over the head and hung into the scalp: from Collide, and through a ball from
    // Interpolate and a dab, against the CPU.
    const char* const params[] = {R"("params": {"offset": 0.12, "maxDepth": 0.6, "softness": 0.3})",
                                  R"("params": {"offset": 0.12, "maxDepth": 0.6, "smoothing": 1.5})",
                                  R"("params": {"offset": 0.12, "maxDepth": 3.0, "softness": 0.2, "smoothing": 1.0})"};
    const std::string hang = R"("params": {"strength": 1.6, "stiffness": 0.2})";
    for (const char* p : params) {
        for (const bool hung : {false, true}) {
            SCOPED_TRACE(std::string(p) + (hung ? ", hung into the scalp" : ", through a ball"));
            const auto nodes = hung ? std::vector<std::pair<std::string, std::string>>{{"Gravity", hang}, {"Collide", p}}
                                    : std::vector<std::pair<std::string, std::string>>{{"Collide", p}};
            GroomData data = groom(withoutClearance(chainGraph(nodes, true, !hung)));
            if (!hung) {
                const MeshPtr sphere = test::makeSphere(16, 32, 1.2f);
                std::vector<Vec3f> points = sphere->points();
                for (Vec3f& q : points) q = q + Vec3f(0.5f, 6.0f, 0.0f);
                data.meshes["collider"] = std::make_shared<ScalpMesh>(points, sphere->triangles());
            }
            GroomSession session(data);
            ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
            const std::string collideNode = "n" + std::to_string(nodes.size() - 1);
            const Compared c = runAndCompare(session, {}, session, collideNode);
            expectPointsClose(c);
            EXPECT_LT(c.widthMax, 1e-5);
            if (hung) continue;  // (after Gravity's rounding, as CollideKeepsStrandsOutAsTheCpuDoes allows)
            expectClose(runAndCompare(session, {}, session, "interpolate"));
            EXPECT_DAB_MATCHES(data);
        }
    }
}

TEST_P(LookEngineTest, ACollideOffsetDragGivesWhatTheCpuPushesForIt) {
    // Collide's offset dragged (through a ball over the head, softened): the GPU runs it, the offset a
    // number of the kernel's, the surfaces' fields' tables made once for every offset of the fields the
    // CPU asks (cellSizeFor's cell, reachFor's reach): kept while the dragged offsets share them, made
    // again for another field. Each value the CPU's Collide for it, to the bit (OpenGL: an ulp or two).
    const std::string graph =
        withoutClearance(chainGraph({{"Collide", R"("params": {"offset": 0.12, "maxDepth": 0.6, "softness": 0.1})"}}, true, true));
    auto withBall = [](const std::string& json) {
        GroomData data = groom(json);
        const MeshPtr sphere = test::makeSphere(16, 32, 1.2f);
        std::vector<Vec3f> points = sphere->points();
        for (Vec3f& q : points) q = q + Vec3f(0.5f, 6.0f, 0.0f);
        data.meshes["collider"] = std::make_shared<ScalpMesh>(points, sphere->triangles());
        return data;
    };
    const GroomData data = withBall(graph);
    GroomSession session(data);
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    EXPECT_TRUE(gpu::previewableParam("Collide", "offset"));
    std::string why;
    ASSERT_TRUE(gpu::previewableOverrides(*session.viewDocument(), {{"n0", {{"offset", 0.13}}}}, &why)) << why;
    // The fields the CPU's Collide asks at an offset: each surface's cell size and reach (its search
    // radius as CollideOp makes it).
    auto fieldsAt = [&](double offset) {
        const float radius = std::max(static_cast<float>(offset) + 0.1f, 0.6f) * 1.001f + 1e-6f;
        std::vector<float> keys;
        for (const MeshPtr& mesh : {session.last().scalp, data.meshes.at("collider")}) {
            keys.push_back(MeshDistanceField::cellSizeFor(*mesh, static_cast<float>(offset)));
            keys.push_back(MeshDistanceField::reachFor(radius));
        }
        return keys;
    };
    gpu::LookInputs kept;
    const double offsets[] = {0.12, 0.13, 0.125, 0.08, 0.3};
    std::map<double, std::vector<MeshDistanceField::Tables>> tablesAt;  // (held: compared by address)
    std::map<double, double> readMs;
    for (const double offset : offsets) {
        SCOPED_TRACE(offset);
        const gpu::Overrides drag{{"n0", {{"offset", offset}}}};
        Prepared p;
        const auto started = std::chrono::steady_clock::now();
        ASSERT_TRUE(prepareRun(session, drag, "", &p, &kept)) << "the GPU previews it: " << p.why;
        readMs[offset] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        ASSERT_EQ(p.chain.stages.front().kind, gpu::StageKind::Collide);
        const gpu::CollideInputs& in = *p.run.inputs.front().collide;
        ASSERT_EQ(in.tables.size(), 2u);
        tablesAt[offset] = in.tables;
        GroomSession committed(withBall(withParam(graph, "n0", "offset", offset)));
        ASSERT_TRUE(committed.evaluate(ViewRequest{}).strands);
        auto engine = backend_->engine();
        engine->setBase(p.base, session.last().scalp.get());
        engine->run(p.run, false);
        const gpu::PointError e = gpu::compareVectors(engine->readPositions(), committed.last().strands->positions());
        std::printf("[ %-8s ] Collide's offset dragged to %.3f: the GPU against the CPU: max %.3g; read in %.2f ms\n", GetParam().c_str(),
                    offset, e.max, readMs[offset]);
        EXPECT_LE(e.max, GetParam() == "DirectX11" ? 0.0 : 2e-6);
        expectPointsClose(runAndCompare(session, drag, committed));
    }
    // The tables kept from one value to the next of the same fields, made again for others.
    for (std::size_t i = 1; i < std::size(offsets); ++i) {
        SCOPED_TRACE(offsets[i]);
        EXPECT_EQ(tablesAt[offsets[i]] == tablesAt[offsets[i - 1]], fieldsAt(offsets[i]) == fieldsAt(offsets[i - 1]));
    }
    EXPECT_TRUE(fieldsAt(0.13) == fieldsAt(0.12) && fieldsAt(0.125) == fieldsAt(0.12)) << "the test's premise: nearby offsets share the fields";
    EXPECT_FALSE(fieldsAt(0.08) == fieldsAt(0.12)) << "the test's premise: a farther offset asks other fields";
    // What the drag cost on the CPU: the graph evaluated again for a new offset (ms).
    const auto started = std::chrono::steady_clock::now();
    session.setGraphJson(withParam(graph, "n0", "offset", 0.135));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    std::printf("[ %-8s ] Collide's offset: the CPU's evaluation for a new value %.2f ms\n", GetParam().c_str(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
}

TEST_P(LookEngineTest, CurlCoilsByTheStrandsWidthsAsTheCpuDoes) {
    // A coil coilWidths strand widths across: by the widths the strands carry (from Curl), by those
    // a Width before it makes in the run (from Width, from Interpolate), coiling each clump as one
    // (from Curl); a drag of coilWidths.
    const std::string coil = R"("params": {"shape": "coil", "coilWidths": 6.0, "frequency": 2.0, "randomness": 0.3})";
    const std::string graph = withoutClearance(chainGraph({{"Width", R"("params": {"width": 0.02, "randomness": 0.4})"}, {"Curl", coil}}, false));
    GroomSession session(groom(graph));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    for (const char* from : {"n1", "n0", "interpolate"}) {
        SCOPED_TRACE(from);
        const Compared c = runAndCompare(session, {}, session, from);
        if (std::string(from) == "n1") {
            expectPointsClose(c);  // (no Width runs)
        } else {
            expectClose(c);
        }
    }
    {
        GroomSession dragged(groom(graph)), committed(groom(withParam(graph, "n1", "coilWidths", 3.5)));
        ASSERT_TRUE(dragged.evaluate(ViewRequest{}).strands);
        ASSERT_TRUE(committed.evaluate(ViewRequest{}).strands);
        expectPointsClose(runAndCompare(dragged, {{"n1", {{"coilWidths", 3.5}}}}, committed));
    }
    const std::string byClump = R"("params": {"shape": "coil", "coilWidths": 6.0, "frequency": 2.0, "coherence": "clump"})";
    const std::string clumped = withoutClearance(chainGraph(
        {{"Width", R"("params": {"width": 0.02})"}, {"Clump", R"("params": {"count": 60, "strength": 0.8})"}, {"Curl", byClump}}, false));
    GroomSession ringlets(groom(clumped));
    ASSERT_TRUE(ringlets.evaluate(ViewRequest{}).strands);
    expectPointsClose(runAndCompare(ringlets, {}, ringlets, "n2"));
    expectPointsClose(runAndCompare(ringlets, {}, ringlets, "n1"));
    Prepared refused;  // by clump on widths the run makes: the CPU's
    EXPECT_FALSE(prepareRun(ringlets, {}, "n0", &refused));
    EXPECT_NE(refused.why.find("coils each clump"), std::string::npos) << refused.why;
}

TEST_P(LookEngineTest, GravityFlyawaysAndCollideStayInTheBounds) {
    // lookBounds grown by Gravity, Flyaway and Collide, at values far past the graph's.
    const std::string graph = chainGraph({{"Gravity", R"("params": {"strength": 0.5})"},
                                          {"Flyaway", R"("params": {"fraction": 0.4, "stray": 0.8, "lengthen": 0.3})"},
                                          {"Collide", R"("params": {"offset": 0.2})"}});
    GroomSession session(groom(graph));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    const gpu::Overrides drag{{"n0", {{"strength", 2.0}}}, {"n1", {{"stray", 2.5}, {"lengthen", 0.9}, {"standOut", 1.0}}}};
    const Prepared p = prepare(session, drag, "interpolate");
    auto engine = backend_->engine();
    engine->setBase(p.base, session.last().scalp.get());
    engine->run(p.run, false);
    const gpu::LookBounds b = gpu::lookBounds(p.run.params, p.run.fields, p.run.inputs, *p.base);
    ASSERT_TRUE(b.valid);
    std::size_t outside = 0;
    for (const Vec3f& v : engine->readPositions()) {
        outside += v.x < b.lo.x || v.y < b.lo.y || v.z < b.lo.z || v.x > b.hi.x || v.y > b.hi.y || v.z > b.hi.z;
    }
    EXPECT_EQ(outside, 0u);
}

// ---- Switch, Wind ------------------------------------------------------------------------------------

// chainGraph's look (Noise and Width after Interpolate) as Switch input a, Interpolate's strands
// hung by a Gravity ("hung") as input b, the Switch ("pick", picking `index`) to the output.
std::string switchGraph(int index) {
    nlohmann::json g = nlohmann::json::parse(chainGraph({{"Noise", R"("params": {"amplitude": 0.3})"}}));
    g["nodes"].push_back({{"name", "hung"}, {"type", "Gravity"}, {"params", {{"strength", 0.8}}}});
    g["nodes"].push_back({{"name", "pick"}, {"type", "Switch"}, {"params", {{"index", index}}}});
    nlohmann::json links = nlohmann::json::array();
    for (const nlohmann::json& c : g["connections"]) {
        if (c["to"] != "output") links.push_back(c);
    }
    for (const auto& [from, to, input] : {std::tuple{"width", "pick", "a"}, std::tuple{"interpolate", "hung", "strands"},
                                          std::tuple{"hung", "pick", "b"}, std::tuple{"pick", "output", "strands"}}) {
        links.push_back({{"from", from}, {"to", to}, {"input", input}});
    }
    g["connections"] = links;
    return withoutClearance(g.dump());
}

TEST_P(LookEngineTest, ASwitchHandsOnTheInputItPicks) {
    // The GPU's run goes through a Switch to the input it picks (its strands handed on as they
    // came), from Interpolate; a dab through it; bypassed, it hands on input a.
    for (const int index : {0, 1}) {
        SCOPED_TRACE(index);
        GroomData data = groom(switchGraph(index));
        GroomSession session(data);
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const Prepared p = prepare(session);
        const GraphDocument& doc = *session.viewDocument();
        ASSERT_EQ(p.chain.stages.front().kind, gpu::StageKind::Interpolate);
        EXPECT_EQ(p.chain.stages.back().kind, index == 0 ? gpu::StageKind::Width : gpu::StageKind::Gravity);
        EXPECT_NE(std::find(p.chain.passOn.begin(), p.chain.passOn.end(), doc.nodesByName.at("pick")), p.chain.passOn.end());
        if (index == 0) {
            expectClose(runAndCompare(session, {}, session));
            EXPECT_DAB_MATCHES(data);
        } else {
            expectPointsClose(runAndCompare(session, {}, session));
        }
    }
    GroomSession bypassed(groom(switchGraph(1)));
    ASSERT_TRUE(bypassed.evaluate(ViewRequest{}).strands);
    GraphDocument doc = bypassed.viewDocument()->clone();
    doc.graph.setBypass(doc.nodesByName.at("pick"), true);
    gpu::LookChain chain;
    std::string why;
    ASSERT_TRUE(gpu::findLookChain(doc, &chain, &why)) << why;
    EXPECT_EQ(chain.stages.back().kind, gpu::StageKind::Width);
}

TEST_P(LookEngineTest, WindBlowsAsTheCpuDoes) {
    // Wind after the look (gusting, swayed and fluttering; then steady, its strength by a map per
    // strand), at a time well past 0 (the gusts travelled, the flutter turned), from Wind and from
    // Interpolate; a dab; drags of each of its numbers, its seed and its profile.
    ViewRequest late;
    late.time = 2.7;
    const std::string params[] = {
        R"("params": {"strength": 0.6, "gust": 0.7, "frequency": 0.4, "speed": 20.0, "turbulence": 0.5, "direction": [1.0, 0.2, 0.3], "seed": 3})",
        R"("params": {"strength": {"expression": "0.2 + 0.6 * rand", "min": 0.0, "max": 2.0}, "gust": 0.0, "turbulence": 0.0,
                      "direction": [0.0, -1.0, 1.0]})",
        // Looped (the time 2.7 s past the range, repeating it): the noises on the loop's circle.
        R"("params": {"strength": 0.6, "gust": 0.7, "frequency": 0.4, "speed": 20.0, "turbulence": 0.5, "direction": [1.0, 0.2, 0.3],
                      "loop": true, "loopStart": 1, "loopEnd": 37})"};
    for (const std::string& p : params) {
        SCOPED_TRACE(p);
        LookGraph g;
        g.guides = true;
        g.after = "Wind";
        g.afterParams = p;
        GroomData data = groom(withoutClearance(g.json()));
        GroomSession session(data);
        ASSERT_TRUE(session.evaluate(late).strands);
        const GraphDocument& doc = *session.viewDocument();
        const StrandsPtr still = valueAs<StrandsPtr>(doc.graph.cachedValue(doc.nodesByName.at("resample")));
        const StrandsPtr blown = valueAs<StrandsPtr>(doc.graph.cachedValue(doc.nodesByName.at("after")));
        ASSERT_TRUE(still && blown);
        EXPECT_GT(gpu::compareVectors(still->positions(), blown->positions()).max, 0.3);  // it blew
        expectPointsClose(runAndCompare(session, {}, session, "after"));
        expectClose(runAndCompare(session, {}, session, "interpolate"));
        EXPECT_DAB_MATCHES(data);
    }
    LookGraph g;
    g.after = "Wind";
    g.afterParams = params[0];
    const std::string graph = g.json();
    auto dragMatches = [&](const gpu::Overrides& overrides, const std::string& committedJson) {
        GroomSession dragged(groom(graph));
        ASSERT_TRUE(dragged.evaluate(late).strands);
        GroomSession committed(groom(committedJson));
        ASSERT_TRUE(committed.evaluate(late).strands);
        expectPointsClose(runAndCompare(dragged, overrides, committed));
    };
    const std::pair<const char*, double> drags[] = {{"strength", 0.9}, {"gust", 0.3},       {"frequency", 0.8},
                                                    {"speed", 45.0},   {"turbulence", 0.1}, {"seed", 7.0}};
    for (const auto& [param, value] : drags) {
        SCOPED_TRACE(param);
        const nlohmann::json stored = std::string(param) == "seed" ? nlohmann::json(7) : nlohmann::json(value);
        dragMatches(gpu::Overrides{{"after", {{param, value}}}}, withParam(graph, "after", param, stored));
    }
    dragMatches(rampDrag("after", "profile", "[[0, 0], [0.3, 0.6], [1, 0.8]]"),
                withParam(graph, "after", "profile", nlohmann::json::parse("[[0, 0], [0.3, 0.6], [1, 0.8]]")));
    // Looped: drags of the numbers its circle comes from.
    LookGraph looped = g;
    looped.afterParams = params[2];
    for (const auto& [param, value] : {std::pair<const char*, double>{"speed", 45.0}, {"frequency", 0.8}}) {
        SCOPED_TRACE(std::string("looped ") + param);
        GroomSession dragged(groom(looped.json()));
        ASSERT_TRUE(dragged.evaluate(late).strands);
        GroomSession committed(groom(withParam(looped.json(), "after", param, value)));
        ASSERT_TRUE(committed.evaluate(late).strands);
        expectPointsClose(runAndCompare(dragged, gpu::Overrides{{"after", {{param, value}}}}, committed));
    }
    // Blowing nowhere at its own strength (a plain 0) it stays cached whatever the time: a drag of
    // its strength is the CPU's.
    GroomSession calm(groom(withParam(graph, "after", "strength", 0.0)));
    ASSERT_TRUE(calm.evaluate(late).strands);
    Prepared refused;
    EXPECT_TRUE(prepareRun(calm, {}, "after", &refused)) << refused.why;
    EXPECT_FALSE(prepareRun(calm, {{"after", {{"strength", 0.5}}}}, "after", &refused));
    EXPECT_NE(refused.why.find("blows nowhere"), std::string::npos) << refused.why;
}

// Interpolate's strands clumped ("sleek", look a) and curled ("curls", look b), blended ("blend",
// its JSON past the type `blend`), then a Width. `lookWidth`: look b's Curl followed by a Width;
// `curlsFrom`: the node look b's Curl takes ("smooth": a Group of Interpolate's strands, which the
// GPU does not run).
std::string blendGraph(const std::string& blend, const std::string& extraNodes = "", const std::string& extraLinks = "",
                       bool lookWidth = false, const std::string& curlsFrom = "interpolate") {
    std::string j = R"({"nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 40.0, "length": 3.0}},
        {"name": "guides", "type": "Guides"}, {"name": "interpolate", "type": "Interpolate", "params": {"keepClearance": false}},
        {"name": "sleek", "type": "Clump", "params": {"count": 60, "strength": 0.7}},
        {"name": "curls", "type": "Curl", "params": {"radius": 0.1, "frequency": 0.8, "tolerance": 0.0}},
        {"name": "curlWidth", "type": "Width", "params": {"width": 0.02}}, {"name": "smooth", "type": "Group"},
        {"name": "blend", "type": "Blend", )" + blend + R"(},
        {"name": "width", "type": "Width", "params": {"width": 0.01, "randomness": 0.2}},
        {"name": "output", "type": "Output"})" + extraNodes + R"(], "connections": [
        {"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "interpolate", "input": "strands"}, {"from": "guides", "to": "interpolate", "input": "guides"},
        {"from": "scalp", "to": "interpolate", "input": "scalp"},
        {"from": "interpolate", "to": "sleek", "input": "strands"}, {"from": "interpolate", "to": "smooth", "input": "strands"},
        {"from": ")" + curlsFrom + R"(", "to": "curls", "input": "strands"},
        {"from": "sleek", "to": "blend", "input": "a"},)";
    j += lookWidth ? R"({"from": "curls", "to": "curlWidth", "input": "strands"}, {"from": "curlWidth", "to": "blend", "input": "b"},)"
                   : R"({"from": "curls", "to": "blend", "input": "b"},)";
    return j + R"({"from": "blend", "to": "width", "input": "strands"}, {"from": "width", "to": "output", "input": "strands"})" + extraLinks +
           R"(], "output": "output"})";
}

TEST_P(LookEngineTest, BlendMixesItsLooksAsTheCpuDoes) {
    // A Blend of two looks made from Interpolate's strands, each look and the Blend on the GPU (a
    // mix by number along a profile, by a map per strand, by a Control), from Interpolate, from look
    // a (look b from where they part too); a dab; a drag of its mix and its profile.
    const std::string control = R"(, {"name": "wet", "type": "Control", "params": {"name": "wet", "default": 0.4}})";
    const std::string controlLink = R"(, {"from": "wet", "to": "blend", "input": "mix"})";
    const std::string graphs[] = {blendGraph(R"("params": {"mix": 0.6, "profile": [[0, 0.2], [1, 1]]})"),
                                  blendGraph(R"("params": {"mix": {"expression": "0.2 + 0.7 * rand", "min": 0.0, "max": 1.0}})"),
                                  blendGraph(R"("params": {"mix": 1.0}, "paramInputs": ["mix"])", control, controlLink)};
    for (const std::string& graph : graphs) {
        SCOPED_TRACE(graph.substr(graph.find("\"Blend\""), 120));
        GroomData data = groom(graph);
        GroomSession session(data);
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const Prepared p = prepare(session);
        ASSERT_EQ(p.chain.stages.size(), 5u);
        const gpu::StageKind kinds[] = {gpu::StageKind::Interpolate, gpu::StageKind::Clump, gpu::StageKind::Curl, gpu::StageKind::Blend,
                                        gpu::StageKind::Width};
        const std::size_t from[] = {gpu::kNoStage, 0, 0, 1, 3};
        for (std::size_t k = 0; k < 5; ++k) {
            EXPECT_EQ(p.chain.stages[k].kind, kinds[k]) << k;
            EXPECT_EQ(p.chain.stages[k].from, from[k]) << k;
        }
        EXPECT_EQ(p.chain.stages[3].from2, 2u);
        expectClose(runAndCompare(session, {}, session));
        expectClose(runAndCompare(session, {}, session, "sleek"));
        const Prepared lookB = prepare(session, {}, "curls");  // back to where the looks part
        EXPECT_EQ(lookB.chain.stages.front().kind, gpu::StageKind::Clump);
        EXPECT_EQ(lookB.chain.stages[1].from, gpu::kNoStage);
        expectClose(runAndCompare(session, {}, session, "curls"));
        EXPECT_DAB_MATCHES(data);
    }
    const std::string graph = graphs[0];
    {
        // A thread a strand.
        GroomSession session(groom(graph));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        configure_ = [](gpu::LookEngine& engine) { engine.setPointThreads(false); };
        expectClose(runAndCompare(session, {}, session));
        configure_ = nullptr;
    }
    EXPECT_GPU_MATCHES(graph, (gpu::Overrides{{"blend", {{"mix", 0.25}}}}), withParam(graph, "blend", "mix", 0.25));
    EXPECT_GPU_MATCHES(graph, (rampDrag("blend", "profile", "[[0, 1], [0.5, 0.3], [1, 0.9]]")),
                       withParam(graph, "blend", "profile", nlohmann::json::parse("[[0, 1], [0.5, 0.3], [1, 0.9]]")));
    // Looks parting at the base (look a from Interpolate, look b from Scatter's strands).
    {
        GroomSession session(groom(blendGraph(R"("params": {"mix": 0.5})", "", "", false, "scatter")));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const Prepared p = prepare(session);
        ASSERT_EQ(p.chain.stages.size(), 5u);
        EXPECT_EQ(session.viewDocument()->nameOf(p.chain.base), "scatter");
        EXPECT_EQ(p.chain.stages[2].kind, gpu::StageKind::Curl);
        EXPECT_EQ(p.chain.stages[2].from, gpu::kNoStage);
        expectClose(runAndCompare(session, {}, session));
    }
    // A look with a Width (its widths unlike the other's), or one not run on the GPU before they
    // meet: the Blend is the CPU's, the GPU running after it.
    for (const bool lookWidth : {true, false}) {
        GroomSession other(groom(blendGraph(R"("params": {"mix": 0.5})", "", "", lookWidth, lookWidth ? "interpolate" : "smooth")));
        ASSERT_TRUE(other.evaluate(ViewRequest{}).strands);
        gpu::LookChain chain;
        std::string why;
        ASSERT_TRUE(gpu::findLookChain(*other.viewDocument(), &chain, &why)) << why;
        EXPECT_EQ(other.viewDocument()->nameOf(chain.base), "blend");
        ASSERT_EQ(chain.stages.size(), 1u);
        EXPECT_EQ(chain.stages[0].kind, gpu::StageKind::Width);
    }
}

// A shipped preset's file.
std::string presetText(const std::string& name) {
    std::ifstream file(std::filesystem::u8path(std::string(GROOMLAB_SHIPPED_PRESETS) + "/" + name + ".json"));
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

// A shipped preset fitted to the test's scalp, as a host applies it.
std::string presetGraph(const std::string& name) {
    return presetGraphJson(presetText(name), presetScalpSize(*groom("{}").scalp));
}

gpu::PointError cpuShake(GroomSession& session, const std::string& name, float by);
void expectFlowClose(const Compared& c, const gpu::PointError& shake);

TEST_P(LookEngineTest, TheLookPresetsRunOnTheGpu) {
    // The shipped looks whose clumps vary, curl by clump, read fields after Interpolate or part:
    // the GPU from their chain's first node gives the CPU's, in a preview showing a quarter of the
    // strands and in one below its core share (hidden core strands held); a brush dab on their
    // guides (from Interpolate) gives a fresh CPU evaluation's. (The dab without Interpolate keeping
    // clearance: its walk decides at each point on the binding, which the GPU holds from the press,
    // a rounding from a fresh one's, and the field's own error, about 1e-3, separates the two sides
    // of a decision.)
    // (B4b: gravity, flyaways, cinematic_hair and everyday_sim: Gravity, Flyaway and Collide;
    // wind_and_jiggle: Wind, its Jiggle with no motion handed on; blend_two_looks: a Blend of two
    // looks of Interpolate's strands, by a Control. creature_fur and flow_with_guides lay Scatter's
    // strands with a Flow, in the latter as look a of a Blend whose look b is Interpolate's, by its
    // guideAreas map (painted varying, so both looks show): within what a rounding moves the CPU's
    // own Flow, expectFlowClose; the test's groom has no arrows, and Scatter's strands stand straight
    // out of the skin, where which way a strand goes is a rounding's choice.)
    for (const char* name : {"creature_fur", "curly", "expression_driven", "flow_with_guides", "parted_style", "gravity", "flyaways",
                             "cinematic_hair", "everyday_sim", "wind_and_jiggle", "blend_two_looks"}) {
        for (const float density : {0.25f, 0.003f}) {
            SCOPED_TRACE(std::string(name) + (density < 0.01f ? ", below the core share" : ""));
            GroomData data = groom(presetGraph(name));
            test::paintRequiredMaps(presetText(name), data);  // (flow_with_guides' guideAreas: both looks)
            data.partsJson = meridianParts(*data.scalp);
            GroomSession session(data);
            ViewRequest request;
            request.density = density;
            ASSERT_TRUE(session.evaluate(request).strands);
            Prepared p;
            ASSERT_TRUE(prepareRun(session, {}, "", &p)) << p.why;
            const bool shaped = p.chain.node(gpu::StageKind::Interpolate) != 0;
            EXPECT_EQ(shaped, std::string(name) != "creature_fur");
            if (density < 0.01f) EXPECT_GT(p.base->strandCount(), p.output->strandCount()) << "no hidden core strands";
            // (gravity and flyaways have no Width: their widths are the strands', not the run's.)
            const bool widths = p.run.params.find(gpu::StageKind::Width) != nullptr;
            auto expectRun = [&](const Compared& c) { widths ? expectClose(c) : expectPointsClose(c); };
            const NodeId flow = p.chain.node(gpu::StageKind::Flow);
            if (flow) {
                const Compared c = runAndCompare(session, {}, session);
                expectFlowClose(c, cpuShake(session, session.viewDocument()->nameOf(flow), 1e-5f));
            } else {
                expectRun(runAndCompare(session, {}, session));
            }
            if (!shaped || density < 0.01f) continue;
            GroomData plain = data;
            plain.graphJson = withoutClearance(data.graphJson);
            GroomSession dabbed(plain);
            ASSERT_TRUE(dabbed.evaluate(request).strands);
            const StrandsPtr before = dabbed.last().strands;
            const GuidesPtr bent = guideSet(*data.scalp, 1.4f);
            dabbed.setGuides(bent);
            ViewRequest dab = request;
            dab.stopAt = "interpolate";
            ASSERT_EQ(dabbed.evaluate(dab).strands, before);
            GroomData after = plain;
            after.guides = bent;
            GroomSession fresh(after);
            ASSERT_TRUE(fresh.evaluate(request).strands);
            expectRun(runAndCompare(dabbed, {}, fresh, "interpolate"));
        }
    }
}

// Two branches merged (a's strands of 9 points, b's resampled to 13: the base's point counts differ),
// a Noise or a Collide after the Merge: a drag up its input a runs a's stages on a's strands (the
// base's first), b's handed on as the Merge made them, then the stages after the Merge on all.
std::string mergedGraph(double amplitude, double after, bool collide) {
    const std::string tail = collide ? R"({"name": "after", "type": "Collide", "params": {"offset": 0.08}},)"
                                     : R"({"name": "after", "type": "Noise", "params": {"amplitude": )" + std::to_string(after) + R"(, "seed": 9}},)";
    return R"({"nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "scatter", "type": "Scatter", "params": {"density": 40.0, "length": 3.0}},
        {"name": "guides", "type": "Guides"}, {"name": "interpolate", "type": "Interpolate", "params": {"keepClearance": false}},
        {"name": "noise", "type": "Noise", "params": {"amplitude": )" + std::to_string(amplitude) + R"(, "frequency": 1.0}},
        {"name": "width", "type": "Width", "params": {"width": 0.01, "randomness": 0.2}},
        {"name": "scatterB", "type": "Scatter", "params": {"density": 12.0, "length": 2.0, "seed": 5}},
        {"name": "resampleB", "type": "Resample", "params": {"segments": 12, "mode": "even"}},
        {"name": "noiseB", "type": "Noise", "params": {"amplitude": 0.2}},
        {"name": "widthB", "type": "Width", "params": {"width": 0.02}},
        {"name": "merge", "type": "Merge"},)" + tail + R"(
        {"name": "output", "type": "Output"}],
      "connections": [
        {"from": "scalp", "to": "scatter", "input": "scalp"}, {"from": "scatter", "to": "interpolate", "input": "strands"},
        {"from": "guides", "to": "interpolate", "input": "guides"}, {"from": "scalp", "to": "interpolate", "input": "scalp"},
        {"from": "interpolate", "to": "noise", "input": "strands"}, {"from": "noise", "to": "width", "input": "strands"},
        {"from": "scalp", "to": "scatterB", "input": "scalp"}, {"from": "scatterB", "to": "resampleB", "input": "strands"},
        {"from": "resampleB", "to": "noiseB", "input": "strands"}, {"from": "noiseB", "to": "widthB", "input": "strands"},
        {"from": "width", "to": "merge", "input": "a"}, {"from": "widthB", "to": "merge", "input": "b"},)" +
           (collide ? R"({"from": "scalp", "to": "after", "input": "scalp"},)" : "") + R"(
        {"from": "merge", "to": "after", "input": "strands"}, {"from": "after", "to": "output", "input": "strands"}],
      "output": "output"})";
}

TEST_P(LookEngineTest, MergeRunsItsInputAAsTheCpuDoes) {
    for (const bool collide : {false, true}) {
        SCOPED_TRACE(collide ? "a Collide after the Merge" : "a Noise after the Merge");
        GroomSession session(groom(mergedGraph(0.3, 0.1, collide)));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const StrandsPtr out = session.last().strands;
        const GraphDocument& doc = *session.viewDocument();
        const StrandsPtr a = valueAs<StrandsPtr>(doc.graph.cachedValue(doc.nodesByName.at("width")));
        ASSERT_TRUE(out && a);
        ASSERT_LT(a->strandCount(), out->strandCount()) << "the test's premise: b has strands";
        ASSERT_NE(a->strandPointCount(0), out->strandPointCount(out->strandCount() - 1)) << "the test's premise: point counts differ";
        gpu::LookChain chain;
        std::string why;
        ASSERT_TRUE(gpu::findLookChain(doc, &chain, &why)) << why;
        const std::size_t merge = chain.indexOf(doc.nodesByName.at("merge"));
        ASSERT_NE(merge, gpu::kNoStage);
        EXPECT_EQ(chain.stages[merge].kind, gpu::StageKind::Merge);
        for (std::size_t k = 0; k < chain.stages.size(); ++k) EXPECT_EQ(chain.stages[k].prefix, k < merge) << k;
        EXPECT_EQ(chain.indexOf(doc.nodesByName.at("noiseB")), gpu::kNoStage) << "b's side: a run of its own";
        // A drag up a: a's Noise and Width, then the Merge, then the node after it.
        GroomSession committed(groom(mergedGraph(0.55, 0.1, collide)));
        ASSERT_TRUE(committed.evaluate(ViewRequest{}).strands);
        expectClose(runAndCompare(session, {{"noise", {{"amplitude", 0.55}}}}, committed));
        // A drag of a's Width alone: its widths up to a's last strand, b's as they were.
        GroomSession wider(groom(mergedGraph(0.3, 0.1, collide)));
        {
            nlohmann::json g = nlohmann::json::parse(mergedGraph(0.3, 0.1, collide));
            for (auto& n : g["nodes"]) {
                if (n["name"] == "width") n["params"]["width"] = 0.016;
            }
            GroomSession made(groom(g.dump()));
            ASSERT_TRUE(made.evaluate(ViewRequest{}).strands);
            expectClose(runAndCompare(session, {{"width", {{"width", 0.016}}}}, made));
        }
        if (!collide) {  // a preview holding hidden core strands (preview_core.h): every strand runs, the shown ones written
            ViewRequest half;
            half.density = 0.5f;
            GroomSession thinned(groom(mergedGraph(0.3, 0.1, false)));
            const StrandsPtr shown = thinned.evaluate(half).strands;
            ASSERT_TRUE(shown);
            GroomSession thinnedCommitted(groom(mergedGraph(0.55, 0.1, false)));
            ASSERT_TRUE(thinnedCommitted.evaluate(half).strands);
            const Prepared p = prepare(thinned, {{"noise", {{"amplitude", 0.55}}}});
            EXPECT_GT(p.base->strandCount(), shown->strandCount()) << "the test's premise: hidden strands";
            expectClose(runAndCompare(thinned, {{"noise", {{"amplitude", 0.55}}}}, thinnedCommitted));
        }
        if (!collide) {  // a drag after the Merge: b's strands run too
            GroomSession after(groom(mergedGraph(0.3, 0.25, false)));
            ASSERT_TRUE(after.evaluate(ViewRequest{}).strands);
            expectPointsClose(runAndCompare(session, {{"after", {{"amplitude", 0.25}}}}, after));  // (no Width runs)
        }
    }
}

// `graphJson` with each of `params` set (node, param, value): what a drag's release commits.
std::string withParams(std::string graphJson, const std::vector<std::tuple<std::string, std::string, nlohmann::json>>& params) {
    for (const auto& [node, param, value] : params) graphJson = withParam(graphJson, node, param, value);
    return graphJson;
}

TEST_P(LookEngineTest, MergeRunsADragUpItsInputBAsTheCpuDoes) {
    // A drag up a Merge's input b (b's Noise, b's Width): the GPU runs from the Merge, b's own run (its
    // chain up b from the dragged node) on a sibling before it, a's strands as cached; then the node
    // after the Merge on all. A drag on both sides: a's stages on a's strands, b's run, then the rest.
    for (const bool collide : {false, true}) {
        SCOPED_TRACE(collide ? "a Collide after the Merge" : "a Noise after the Merge");
        const std::string graph = mergedGraph(0.3, 0.1, collide);
        GroomSession session(groom(graph));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const GraphDocument& doc = *session.viewDocument();
        struct Drag {
            const char* name;
            gpu::Overrides overrides;
            std::vector<std::tuple<std::string, std::string, nlohmann::json>> committed;
            const char* first;   // the node the main run starts from
            const char* firstB;  // ... b's run
        };
        const Drag drags[] = {
            {"b's Noise", {{"noiseB", {{"amplitude", 0.35}}}}, {{"noiseB", "amplitude", 0.35}}, "merge", "noiseB"},
            {"b's Width", {{"widthB", {{"width", 0.03}}}}, {{"widthB", "width", 0.03}}, "merge", "widthB"},
            {"both sides", {{"noise", {{"amplitude", 0.55}}}, {"noiseB", {{"amplitude", 0.35}}}},
             {{"noise", "amplitude", 0.55}, {"noiseB", "amplitude", 0.35}}, "noise", "noiseB"}};
        for (const Drag& d : drags) {
            SCOPED_TRACE(d.name);
            Prepared p;
            ASSERT_TRUE(prepareRun(session, d.overrides, "", &p)) << "the GPU previews it: " << p.why;
            EXPECT_EQ(doc.nameOf(p.chain.stages.front().node), d.first);
            const std::size_t merge = p.chain.indexOf(doc.nodesByName.at("merge"));
            ASSERT_NE(merge, gpu::kNoStage);
            ASSERT_TRUE(p.run.params.stages[merge].branch) << "b's own run";
            ASSERT_TRUE(p.run.inputs[merge].merged);
            const gpu::LookChain& b = p.run.params.stages[merge].branch->chain;
            EXPECT_EQ(doc.nameOf(b.stages.front().node), d.firstB);
            EXPECT_EQ(doc.nameOf(b.stages.back().node), "widthB");
            GroomSession committed(groom(withParams(graph, d.committed)));
            ASSERT_TRUE(committed.evaluate(ViewRequest{}).strands);
            expectClose(runAndCompare(session, d.overrides, committed));
        }
        // A drag up b through a node the GPU does not run there (b's Scatter's strands): the CPU's.
        Prepared refused;
        EXPECT_FALSE(prepareRun(session, {{"resampleB", {{"segments", 8.0}}}}, "", &refused));
        if (!collide) {  // a preview holding hidden core strands (preview_core.h): b's run holds them too
            ViewRequest half;
            half.density = 0.5f;
            GroomSession thinned(groom(graph));
            ASSERT_TRUE(thinned.evaluate(half).strands);
            GroomSession thinnedCommitted(groom(withParam(graph, "noiseB", "amplitude", 0.35)));
            ASSERT_TRUE(thinnedCommitted.evaluate(half).strands);
            expectClose(runAndCompare(thinned, {{"noiseB", {{"amplitude", 0.35}}}}, thinnedCommitted));
        }
    }
    // A denser groom: b's drag on the GPU against the CPU's evaluation of the same value (ms).
    {
        nlohmann::json g = nlohmann::json::parse(mergedGraph(0.3, 0.1, false));
        for (auto& n : g["nodes"]) {
            if (n["name"] == "scatter") n["params"]["density"] = 400.0;
            if (n["name"] == "scatterB") n["params"]["density"] = 400.0;
        }
        const std::string dense = g.dump();
        GroomSession session(groom(dense));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const gpu::Overrides drag{{"noiseB", {{"amplitude", 0.35}}}};
        Prepared p;
        ASSERT_TRUE(prepareRun(session, drag, "", &p)) << p.why;
        auto engine = backend_->engine();
        engine->setBase(p.base, session.last().scalp.get());
        engine->run(p.run, true);
        engine->finish();
        std::vector<double> gpuMs;
        gpu::LookInputs kept;
        for (int i = 0; i < 5; ++i) {
            const auto start = std::chrono::steady_clock::now();
            Prepared q;
            ASSERT_TRUE(prepareRun(session, {{"noiseB", {{"amplitude", 0.3 + 0.01 * i}}}}, "", &q, &kept)) << q.why;
            engine->setBase(q.base, session.last().scalp.get());
            engine->run(q.run, true);
            engine->finish();
            gpuMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        std::vector<double> cpuMs;
        for (int i = 0; i < 5; ++i) {
            session.setGraphJson(withParam(dense, "noiseB", "amplitude", 0.3 + 0.01 * (i + 1)));
            const auto start = std::chrono::steady_clock::now();
            ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
            cpuMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        std::sort(gpuMs.begin(), gpuMs.end());
        std::sort(cpuMs.begin(), cpuMs.end());
        std::printf("[ %-8s ] b's Noise dragged on %zu strands: GPU plan and run %.2f ms (median of 5), the CPU's evaluation %.2f ms\n",
                    GetParam().c_str(), p.base->strandCount(), gpuMs[2], cpuMs[2]);
    }
}
INSTANTIATE_TEST_SUITE_P(Backends, LookEngineTest, ::testing::Values(std::string("DirectX11"), std::string("OpenGL"), std::string("CPU")),
                         [](const ::testing::TestParamInfo<std::string>& info) { return info.param; });

TEST(GpuPreview, MaskedNodesAreStagesAndRenderOnlyOnesAreHandedOn) {
    // A stage restricted by its affect (a mask) runs on the GPU, its result mixed back by it
    // (StageParams::masked); of none, its input handed on; one set to run in renders only is off in
    // the viewport (handed on, as bypassed); one set to run in the viewport only runs there as ever.
    LookGraph g;
    GroomSession session(groom(g.json()));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    GraphDocument doc = session.viewDocument()->clone();
    const NodeId noise = doc.nodesByName.at("noise");
    auto staged = [&](const gpu::LookChain& c) {
        return std::any_of(c.stages.begin(), c.stages.end(), [&](const gpu::LookStage& s) { return s.node == noise; });
    };
    std::string why;
    gpu::LookChain chain;
    ASSERT_TRUE(gpu::findLookChain(doc, &chain, &why)) << why;
    ASSERT_TRUE(staged(chain));

    MapRef mask;
    mask.source = MapRef::Source::Expression;
    mask.name = "x > 0";
    doc.graph.node(noise).params().set(kAffectParam, mask);
    gpu::LookChain masked;
    ASSERT_TRUE(gpu::findLookChain(doc, &masked, &why)) << why;
    EXPECT_TRUE(staged(masked));
    gpu::LookParams params;
    ASSERT_TRUE(gpu::readLookParams(doc, masked, {}, &params, &why)) << why;
    ASSERT_TRUE(params.find(gpu::StageKind::Noise));
    EXPECT_TRUE(params.find(gpu::StageKind::Noise)->masked);
    EXPECT_FALSE(params.find(gpu::StageKind::Curl)->masked);
    doc.graph.node(noise).params().set(kAffectParam, 0.0);
    gpu::LookChain none;
    ASSERT_TRUE(gpu::findLookChain(doc, &none, &why)) << why;
    EXPECT_FALSE(staged(none));
    EXPECT_NE(std::find(none.passOn.begin(), none.passOn.end(), noise), none.passOn.end());
    EXPECT_FALSE(gpu::previewableOverrides(doc, gpu::Overrides{{"noise", {{"affect", 0.5}}}}, &why));
    doc.graph.node(noise).params().set(kAffectParam, 1.0);
    EXPECT_TRUE(gpu::previewableOverrides(doc, gpu::Overrides{{"noise", {{"affect", 0.5}}}}, &why)) << why;

    doc.graph.setEvaluationMode(noise, EvaluationMode::RenderOnly);
    gpu::LookChain off;
    ASSERT_TRUE(gpu::findLookChain(doc, &off, &why)) << why;
    EXPECT_FALSE(staged(off));
    EXPECT_NE(std::find(off.passOn.begin(), off.passOn.end(), noise), off.passOn.end());

    doc.graph.setEvaluationMode(noise, EvaluationMode::ViewportOnly);
    gpu::LookChain on;
    ASSERT_TRUE(gpu::findLookChain(doc, &on, &why)) << why;
    EXPECT_TRUE(staged(on));
}

// ---- SelfCollide and Static: the hair grid --------------------------------------------------------

// The CPU's own node `name` (of `session`'s graph) on the strands it takes, and on them shaken by a
// float rounding (each point moved `by` in a direction of its own): how far apart its two results
// are.
gpu::PointError cpuShake(GroomSession& session, const std::string& name, float by = 1e-5f) {
    gpu::PointError e;
    e.max = -1.0;
    const GraphDocument& doc = *session.viewDocument();
    const NodeId id = doc.nodesByName.at(name);
    NodeId source = 0;
    doc.graph.inputSource(id, 0, &source);
    const StrandsPtr in = valueAs<StrandsPtr>(doc.graph.cachedValue(source));
    if (!in) return e;
    auto shaken = std::make_shared<StrandSet>(*in);
    std::vector<Vec3f>& points = shaken->mutablePositions();
    for (std::size_t i = 0; i < points.size(); ++i) {
        const std::uint64_t h = hash64(i * 3 + 1);
        const Vec3f d(unitFloat(h) - 0.5f, unitFloat(hash64(h)) - 0.5f, unitFloat(hash64(h + 7)) - 0.5f);
        if (d.length() > 0.0f) points[i] += d.normalized() * by;
    }
    auto node = OperatorRegistry::instance().create(doc.graph.node(id).typeName());
    node->params() = doc.graph.node(id).params();
    ExternalInputs ext;
    if (session.last().scalp) ext.set("scalp", session.last().scalp, 1);
    FlowState flow;  // (a Flow's field, as its evaluation took it)
    if (flowState(doc.graph.node(id), valueAs<StrandsPtr>(doc.graph.cachedValue(id)), &flow) && flow.flow) {
        ext.set("flow", flow.flow, flow.flow->contentHash());
    }
    EvalContext ctx;
    ctx.externals = &ext;
    const StrandsPtr a = valueAs<StrandsPtr>(node->evaluate(ctx, {Value(in)}));
    const StrandsPtr b = valueAs<StrandsPtr>(node->evaluate(ctx, {Value(StrandsPtr(shaken))}));
    return a && b ? gpu::compareVectors(a->positions(), b->positions()) : e;
}

// The hair grid's results against the CPU's (look.slang's hair grid: the samples and the cells they
// fall in are the CPU's to the bit, the sums the same integers; the solve, the slabs' eigenvectors
// and the pushes in float where the CPU works in double). From the node itself, on a grid of the
// test's size (the strands it takes the CPU's to the bit): the usual tolerances.
void expectGridClose(const Compared& c, bool widths) {
    if (widths) {
        expectClose(c);
    } else {
        expectPointsClose(c);
    }
}
// Where the strands the node takes are the GPU's (a run from a node before it, a dab: within the
// usual roundings of the CPU's, about 2e-5), or where SelfCollide's later passes take what its first
// moved (a rounding of the CPU's in float), the node's own result is not continuous at a rounding:
// a sample's cell, a node overpacked or not, a slab or not, the grid's origin, each decided on the
// points, flip. The CPU's own result moves as far for what it takes shaken by 1e-5 (`shake`,
// cpuShake; printed): the GPU's is within twice that of the CPU's (and the usual tolerances where
// that is less). Its frames, from those points, within 10 times as much (a tangent turns by a
// point's move over a segment), and the bitangents (cross(tangent, root normal): a tangent near
// the root normal turns them by up to 2) 100 times as much.
void expectGridShakenClose(const Compared& c, const gpu::PointError& shake, bool widths) {
    ASSERT_GE(shake.max, 0.0);
    const double points = std::max(kPointTolerance, 2.0 * shake.max), mean = std::max(kMeanTolerance, 2.0 * shake.mean);
    EXPECT_GT(c.positions.count, 1000u);
    EXPECT_LT(c.positions.max, points);
    EXPECT_LT(c.positions.mean, mean);
    EXPECT_LT(c.normals.max, std::max(kFrameTolerance, 10.0 * points));
    EXPECT_LT(c.tangents.max, std::max(kFrameTolerance, 10.0 * points));
    EXPECT_LT(c.bitangents.max, std::max(kBitangentTolerance, 100.0 * points));
    if (widths) EXPECT_LT(c.widthMax, kWidthTolerance);
}
void printShake(const std::string& api, const char* type, const gpu::PointError& shake) {
    std::printf("[ %-8s ] %s's own CPU result for its strands shaken by 1e-5: max %.3g mean %.3g apart\n", api.c_str(), type, shake.max,
                shake.mean);
}
// A dab on the guides (EXPECT_DAB_MATCHES) for a graph with a hair grid node `node` after Interpolate.
#define EXPECT_GRID_DAB_MATCHES(data, node)                                                          \
    do {                                                                                             \
        GroomSession dabbed(data);                                                                   \
        ASSERT_TRUE(dabbed.evaluate(ViewRequest{}).strands);                                         \
        const StrandsPtr before = dabbed.last().strands;                                             \
        const GuidesPtr bent = guideSet(*(data).scalp, 1.4f);                                        \
        dabbed.setGuides(bent);                                                                      \
        ViewRequest dab;                                                                             \
        dab.stopAt = "interpolate";                                                                  \
        ASSERT_EQ(dabbed.evaluate(dab).strands, before);                                             \
        GroomData after = (data);                                                                    \
        after.guides = bent;                                                                         \
        GroomSession fresh(after);                                                                   \
        ASSERT_TRUE(fresh.evaluate(ViewRequest{}).strands);                                          \
        const gpu::PointError shaken = cpuShake(fresh, node);                                        \
        printShake(GetParam(), node, shaken);                                                        \
        expectGridShakenClose(runAndCompare(dabbed, {}, fresh, "interpolate"), shaken, true);        \
    } while (0)

// What the hair grid's run takes: its stage's layout and walls.
const gpu::HairGridInputs* gridInputs(const Prepared& p, gpu::StageKind kind) {
    for (std::size_t k = 0; k < p.chain.stages.size() && k < p.run.inputs.size(); ++k) {
        if (p.chain.stages[k].kind == kind) return p.run.inputs[k].grid.get();
    }
    return nullptr;
}

TEST_P(LookEngineTest, SelfCollidePushesAsTheCpuDoes) {
    // SelfCollide after the look, the strands it takes clumped tight and thick (overpacked: pushed
    // apart, by the pressure and the slabs' spread), packed looser than its default, then looser
    // still with its strength by a map per strand; from SelfCollide and from Interpolate (its widths
    // a Width's before it); a dab on the guides.
    const std::string params[] = {R"("params": {"packing": 0.2})",
                                  R"("params": {"packing": 0.15, "strength": {"expression": "0.4 + 0.6 * rand", "min": 0.0, "max": 1.0}})"};
    for (const std::string& p : params) {
        SCOPED_TRACE(p);
        LookGraph g;
        g.guides = true;
        g.width = R"("params": {"width": 0.08})";
        g.after = "SelfCollide";
        g.afterParams = p;
        GroomData data = groom(withoutClearance(g.json()));
        GroomSession session(data);
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const GraphDocument& doc = *session.viewDocument();
        const StrandsPtr packed = valueAs<StrandsPtr>(doc.graph.cachedValue(doc.nodesByName.at("resample")));
        const StrandsPtr pushed = valueAs<StrandsPtr>(doc.graph.cachedValue(doc.nodesByName.at("after")));
        ASSERT_TRUE(packed && pushed);
        EXPECT_GT(gpu::compareVectors(packed->positions(), pushed->positions()).max, 0.05);  // it pushed
        {
            const Prepared prepared = prepare(session, {}, "after");
            const gpu::HairGridInputs* grid = gridInputs(prepared, gpu::StageKind::SelfCollide);
            ASSERT_TRUE(grid && grid->layout && grid->walls);
            EXPECT_TRUE(grid->layout->widths);  // (a Width before it)
            EXPECT_GT(grid->layout->segments.size(), 1000u);
            EXPECT_TRUE(grid->layout->tables);
        }
        const gpu::PointError shake = cpuShake(session, "after");
        printShake(GetParam(), "SelfCollide", shake);
        expectGridClose(runAndCompare(session, {}, session, "after"), false);
        expectGridShakenClose(runAndCompare(session, {}, session, "interpolate"), shake, true);
        EXPECT_GRID_DAB_MATCHES(data, "after");
    }
}

TEST_P(LookEngineTest, SelfCollideDragsAsTheCpuDoes) {
    // Its strength, packing and profile dragged (values on the grid it lays: the GPU's), against a
    // CPU evaluation of the graph holding them; then after a Clump, a Noise and a Width after it (the
    // GPU's too), from it and from Interpolate, and a dab.
    LookGraph g;
    g.width = R"("params": {"width": 0.08})";
    g.after = "SelfCollide";
    const std::string graph = g.json();
    EXPECT_GPU_POINTS_MATCH(graph, (gpu::Overrides{{"after", {{"strength", 0.5}}}}), withParam(graph, "after", "strength", 0.5));
    EXPECT_GPU_POINTS_MATCH(graph, (gpu::Overrides{{"after", {{"packing", 0.2}}}}), withParam(graph, "after", "packing", 0.2));
    EXPECT_GPU_POINTS_MATCH(graph, (rampDrag("after", "profile", "[[0, 0], [0.4, 0.5], [1, 1]]")),
                            withParam(graph, "after", "profile", nlohmann::json::parse("[[0, 0], [0.4, 0.5], [1, 1]]")));
    const std::string clumped = R"("params": {"strength": 0.9})";
    GroomData data = groom(withoutClearance(chainGraph({{"Clump", LookGraph::ownCenters(clumped)}, {"SelfCollide", ""},
                                                        {"Noise", R"("params": {"amplitude": 0.1})"}})));
    GroomSession session(data);
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    {
        const Prepared p = prepare(session, {}, "n1");
        ASSERT_EQ(p.chain.stages.size(), 3u);
        EXPECT_EQ(p.chain.stages[0].kind, gpu::StageKind::SelfCollide);
        EXPECT_EQ(p.chain.stages[1].kind, gpu::StageKind::Noise);
        EXPECT_EQ(p.chain.stages[2].kind, gpu::StageKind::Width);
    }
    expectClose(runAndCompare(session, {}, session, "n1"));
    const gpu::PointError shake = cpuShake(session, "n1");
    printShake(GetParam(), "SelfCollide", shake);
    expectGridShakenClose(runAndCompare(session, {}, session, "interpolate"), shake, true);
    EXPECT_GRID_DAB_MATCHES(data, "n1");
}

TEST_P(LookEngineTest, StaticChargesAsTheCpuDoes) {
    // Static after the look (its charge by ID, then even and stronger with its strength by a map per
    // strand), from Static and from Interpolate; a dab; drags of its strength, randomness and
    // profile; a Noise and Width after it.
    const std::string params[] = {R"("params": {})",
                                  R"("params": {"randomness": 0.0, "strength": {"expression": "0.5 + rand", "min": 0.0, "max": 2.0}})"};
    for (const std::string& p : params) {
        SCOPED_TRACE(p);
        LookGraph g;
        g.guides = true;
        g.after = "Static";
        g.afterParams = p;
        GroomData data = groom(withoutClearance(g.json()));
        GroomSession session(data);
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const GraphDocument& doc = *session.viewDocument();
        const StrandsPtr still = valueAs<StrandsPtr>(doc.graph.cachedValue(doc.nodesByName.at("resample")));
        const StrandsPtr charged = valueAs<StrandsPtr>(doc.graph.cachedValue(doc.nodesByName.at("after")));
        ASSERT_TRUE(still && charged);
        EXPECT_GT(gpu::compareVectors(still->positions(), charged->positions()).max, 0.2);  // it puffed out
        const gpu::PointError shake = cpuShake(session, "after");
        printShake(GetParam(), "Static", shake);
        expectGridClose(runAndCompare(session, {}, session, "after"), false);
        expectGridShakenClose(runAndCompare(session, {}, session, "interpolate"), shake, true);
        EXPECT_GRID_DAB_MATCHES(data, "after");
    }
    LookGraph g;
    g.after = "Static";
    const std::string graph = g.json();
    EXPECT_GPU_POINTS_MATCH(graph, (gpu::Overrides{{"after", {{"strength", 0.9}}}}), withParam(graph, "after", "strength", 0.9));
    EXPECT_GPU_POINTS_MATCH(graph, (gpu::Overrides{{"after", {{"randomness", 0.9}}}}), withParam(graph, "after", "randomness", 0.9));
    EXPECT_GPU_POINTS_MATCH(graph, (rampDrag("after", "profile", "[[0, 0.2], [0.5, 0.3], [1, 1]]")),
                            withParam(graph, "after", "profile", nlohmann::json::parse("[[0, 0.2], [0.5, 0.3], [1, 1]]")));
    GroomData data = groom(withoutClearance(chainGraph({{"Static", ""}, {"Noise", R"("params": {"amplitude": 0.1})"}})));
    GroomSession session(data);
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    expectClose(runAndCompare(session, {}, session, "n0"));
    expectClose(runAndCompare(session, {}, session, "interpolate"));  // (Interpolate alone before it: the CPU's to the bit)
}

TEST_P(LookEngineTest, TheHairGridsRunHiddenCoreStrandsAndAHundredThousand) {
    // A preview below the core share: the core strands it holds hidden run (and are the ones
    // splatted) on the GPU too. Then about 100k strands (the grid splats at most 8192 of their core:
    // it costs what it does at 8k; the pushes run a thread a strand), each node's GPU time printed
    // (the median of 5 runs after one). (SelfCollide's later passes splat what its first moved: a
    // rounding of the CPU's, and at 100k strands one decision of the CPU's flips with a rounding:
    // within the CPU's own shake.)
    for (const char* type : {"SelfCollide", "Static"}) {
        SCOPED_TRACE(type);
        const bool collide = std::string(type) == "SelfCollide";
        LookGraph g;
        g.width = collide ? R"("params": {"width": 0.08})" : g.width;
        g.after = type;
        g.afterParams = collide ? R"("params": {"packing": 0.2})" : R"("params": {})";
        {
            GroomSession session(groom(withoutClearance(g.json())));
            ViewRequest request;
            request.density = 0.02f;  // (under the core share: its hidden core strands held)
            ASSERT_TRUE(session.evaluate(request).strands);
            const Prepared p = prepare(session, {}, "after");
            ASSERT_TRUE(p.base && p.output);
            EXPECT_GT(p.base->strandCount(), p.output->strandCount()) << "no hidden core strands";
            expectGridClose(runAndCompare(session, {}, session, "after"), false);
        }
        g.scatter = R"("params": {"density": 500.0, "length": 3.0})";
        g.width = collide ? R"("params": {"width": 0.03})" : g.width;
        GroomSession session(groom(withoutClearance(g.json())));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        EXPECT_GT(session.last().strands->strandCount(), 90000u);
        const gpu::PointError shake = cpuShake(session, "after");
        printShake(GetParam(), type, shake);
        expectGridShakenClose(runAndCompare(session, {}, session, "after"), shake, false);
        const Prepared p = prepare(session, {}, "after");
        auto engine = backend_->engine();
        engine->setBase(p.base, session.last().scalp.get());
        engine->run(p.run, true);
        engine->times(true);
        std::vector<double> ms;
        for (int run = 0; run < 5; ++run) {
            engine->run(p.run, true);
            const gpu::GpuTimes t = engine->times(true);
            ms.push_back(collide ? t.selfCollideMs : t.staticMs);
        }
        std::sort(ms.begin(), ms.end());
        std::printf("[ %-8s ] %s at %zu strands, %zu points: %.3f ms on the GPU (median of 5)\n", GetParam().c_str(), type,
                    p.base->strandCount(), p.base->pointCount(), ms[2]);
    }
}

// ---- Sculpt --------------------------------------------------------------------------------------

// LookGraph's chain with its guides from a Sculpt node (Sculpt <- ToGuides <- strands, a groom's
// guide levels): the hand guides through an even Scatter's Interpolate made into dense guides
// (ToGuides), sculpted, then the look's Interpolate, Clump, Noise, Curl and Width. `between`: a
// node of that type between the Sculpt node and the look's Interpolate ("": none).
std::string sculptGraph(double strength = 1.0, const std::string& between = "") {
    LookGraph g;
    g.guides = true;
    g.nodes = R"(, {"name": "even", "type": "Scatter", "params": {"density": 3.0, "randomness": 0.0, "length": 3.0, "previewAll": true}},
        {"name": "dense", "type": "Interpolate"}, {"name": "toGuides", "type": "ToGuides"},
        {"name": "sculpt", "type": "Sculpt", "params": {"strength": )" + std::to_string(strength) + "}}";
    g.connections = R"(, {"from": "scalp", "to": "even", "input": "scalp"}, {"from": "even", "to": "dense", "input": "strands"},
        {"from": "guides", "to": "dense", "input": "guides"}, {"from": "scalp", "to": "dense", "input": "scalp"},
        {"from": "dense", "to": "toGuides", "input": "strands"}, {"from": "toGuides", "to": "sculpt", "input": "guides"},
        {"from": "scalp", "to": "sculpt", "input": "scalp"})";
    std::string fed = "sculpt";
    if (!between.empty()) {
        g.nodes += R"(, {"name": "between", "type": ")" + between + "\"}";
        g.connections += R"(, {"from": "sculpt", "to": "between", "input": "strands"})";
        fed = "between";
    }
    std::string json = g.json();
    const std::string link = R"({"from": "guides", "to": "interpolate", "input": "guides"})";
    const std::size_t at = json.find(link);
    if (at != std::string::npos) json.replace(at, link.size(), R"({"from": ")" + fed + R"(", "to": "interpolate", "input": "guides"})");
    return json;
}

// Edits hooking every third guide the Sculpt node is fed: its tip half swung sideways.
std::string hookedSculpts(GroomSession& session) {
    const SculptInput in = session.sculptInput("sculpt");
    SculptSet set;
    for (std::size_t i = 0; i < in.guides->guides().size(); i += 3) {
        const Guide& g = in.guides->guides()[i];
        std::vector<Vec3f> hooked = g.points;
        for (std::size_t k = hooked.size() / 2; k < hooked.size(); ++k) {
            hooked[k] = hooked[k] + Vec3f(0.15f, 0.0f, 0.1f) * static_cast<float>(k + 1 - hooked.size() / 2);
        }
        set.nodes["sculpt"].guides[g.id] = recordSculpt(g, hooked, 1.0f, in.scalp.get());
    }
    return set.toJson();
}

TEST_P(LookEngineTest, ASculptStrokesDabsRunDownstreamOnTheGpu) {
    // A comb on a Sculpt node's guides (the core stroke API, as a host's brush tool drives it):
    // after the press (the CPU's), each dab's evaluation stops at the look's Interpolate, which
    // evaluates the Sculpt node alone (its edits laid on the guides it is fed, cached), and the GPU
    // runs Interpolate, Clump, Noise, Curl and Width from those guides: as the CPU evaluates the
    // same dab. Sculpt edits turn segments, never roots: the binding holds (no strand bound again).
    // The release commits the node's edits on the CPU, one undo step. With Interpolate keeping its
    // strands clear of the scalp, and not.
    for (const bool clearance : {false, true}) {
        SCOPED_TRACE(clearance ? "keeping clearance" : "no clearance");
        GroomSession session(groom(clearance ? sculptGraph() : withoutClearance(sculptGraph())));
        SessionStrokeHost host(session);
        session.setSculptTarget("sculpt");
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const SculptInput in = session.sculptInput("sculpt");
        ASSERT_GT(in.guides->size(), 100u);
        // Across the guide rooted highest, from above and in front.
        const Guide* top = &in.guides->guides()[0];
        for (const Guide& g : in.guides->guides()) {
            if (g.points[0].y > top->points[0].y) top = &g;
        }
        const ToolView view = ToolView::fromMatrix(test::lookAtProjection({0.0f, 14.0f, 10.0f}, {0.0f, 4.0f, 0.0f}, 40.0, 1.0), 800, 800);
        float cx = 0.0f, cy = 0.0f;
        ASSERT_TRUE(view.project(top->points[top->points.size() / 2], cx, cy));
        std::vector<std::array<float, 2>> line;
        for (int k = 0; k < 10; ++k) line.push_back({cx - 90.0f + 20.0f * static_cast<float>(k), cy + 4.0f * static_cast<float>(k)});
        BrushTool tool;
        tool.settings.setMode("comb");
        tool.settings.screenRadius = 90.0f;
        tool.settings.brush.strength = 1.0f;

        gpu::LookPreview preview;
        const gpu::LookPreview::Shown shown = [&] { return session.last().strands; };
        auto engine = backend_->engine();
        bool armed = false;
        int gpuDabs = 0;
        std::size_t cpuNodes = 0, guidesMade = 0;
        double worst = 0.0, moved = 0.0, meanWorst = 0.0;
        std::vector<Vec3f> lastGpu;
        ASSERT_TRUE(tool.stroke(host, view, line, 1.0f, 0, [&] {
            if (!armed) {
                // The press shown (the CPU's): the GPU takes the next dabs.
                EXPECT_TRUE(session.hasPreviewSculpt());
                EXPECT_TRUE(gpu::strokeShapesGuides(session));
                ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
                std::string why;
                armed = preview.armStroke(session, shown, &why);
                ASSERT_TRUE(armed) << why;
                EXPECT_EQ(preview.stopAt(), "interpolate");
                return;
            }
            const StrandsPtr before = session.last().strands;
            const std::size_t evaluations = session.evaluationCount();
            ViewRequest dab;
            dab.stopAt = preview.stopAt();
            EXPECT_EQ(session.evaluate(dab).strands, before);  // the view keeps its strands: the GPU draws
            cpuNodes = std::max(cpuNodes, session.evaluationCount() - evaluations);
            std::string why;
            ASSERT_TRUE(preview.strokeHolds(session, shown, &why)) << why;
            const gpu::LookPlan& plan = *preview.strokePlan();
            EXPECT_EQ(plan.chain.stages.front().kind, gpu::StageKind::Interpolate);
            guidesMade = std::max(guidesMade, preview.inputs().lastGuidesMade());
            preview.upload(*engine, plan, session.last().scalp.get());
            engine->run(plan.run, true);
            EXPECT_TRUE(engine->times(true).valid);
            ++gpuDabs;
            lastGpu = engine->readPositions();
            // The same dab evaluated whole on the CPU.
            ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
            const gpu::PointError e = gpu::compareVectors(lastGpu, session.last().strands->positions());
            worst = std::max(worst, e.max);
            meanWorst = std::max(meanWorst, e.mean);
            moved = std::max(moved, gpu::compareVectors(before->positions(), session.last().strands->positions()).max);
        }));
        preview.endStroke();
        std::printf("[ %-8s ] sculpt stroke (%s): %d dabs on the GPU, the CPU evaluating %zu node(s) a dab, %zu guide shapes "
                    "made, %zu strands bound again; vs the CPU's dab: max %.3g mean %.3g; the dabs moved the hair by %.3g\n",
                    GetParam().c_str(), clearance ? "clearance" : "no clearance", gpuDabs, cpuNodes, guidesMade,
                    preview.inputs().rebinds(), worst, meanWorst, moved);
        EXPECT_GE(gpuDabs, 4);
        EXPECT_LE(cpuNodes, 1u) << "a dab evaluates the Sculpt node alone on the CPU";
        EXPECT_EQ(guidesMade, in.guides->size());
        EXPECT_EQ(preview.inputs().rebinds(), 0u) << "sculpt edits move no roots: the binding holds";
        EXPECT_LT(worst, kPointTolerance);
        EXPECT_LT(meanWorst, kMeanTolerance);
        EXPECT_GT(moved, 0.05) << "the dabs reshaped the hair";
        // The release: the node's edits committed on the CPU, one undo step, the hair the last dab's.
        EXPECT_FALSE(session.hasPreviewSculpt());
        EXPECT_FALSE(session.hasPreview());
        ASSERT_EQ(host.edits().size(), 1u);
        EXPECT_TRUE(host.edits()[0].text(StrokeField::Sculpts).set);
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        EXPECT_LT(gpu::compareVectors(lastGpu, session.last().strands->positions()).max, kPointTolerance);
    }
}

TEST_P(LookEngineTest, ASculptStrengthDragRunsDownstreamOnTheGpu) {
    // A drag of a Sculpt node's strength: its guides made on the CPU at each value (its stored
    // edits laid on the guides it is fed, cached; nothing of the graph evaluated), the GPU running
    // from the look's Interpolate: as the CPU evaluates the graph holding the value (half, past
    // full, none).
    const std::string graph = withoutClearance(sculptGraph());
    GroomData data = groom(graph);
    {
        GroomSession probe(data);
        ASSERT_TRUE(probe.evaluate(ViewRequest{}).strands);
        data.sculptsJson = hookedSculpts(probe);
    }
    for (const double strength : {0.5, 1.5, 0.0}) {
        SCOPED_TRACE(strength);
        GroomSession dragged(data);
        ASSERT_TRUE(dragged.evaluate(ViewRequest{}).strands);
        const StrandsPtr before = dragged.last().strands;
        gpu::LookPreview preview;
        const gpu::LookPreview::Shown shown = [&] { return dragged.last().strands; };
        gpu::LookPlan plan;
        std::string why;
        const std::size_t evaluations = dragged.evaluationCount();
        ASSERT_TRUE(preview.plan(dragged, shown, {{"sculpt", {{"strength", strength}}}}, {}, &plan, &why)) << why;
        EXPECT_EQ(dragged.evaluationCount(), evaluations) << "the drag evaluates nothing of the graph";
        ASSERT_FALSE(plan.chain.stages.empty());
        EXPECT_EQ(plan.chain.stages.front().node, dragged.viewDocument()->nodesByName.at("interpolate"));
        auto engine = backend_->engine();
        preview.upload(*engine, plan, dragged.last().scalp.get(), false);
        engine->run(plan.run, true);
        EXPECT_TRUE(engine->times(true).valid);
        const std::vector<Vec3f> points = engine->readPositions();
        const std::vector<float> widths = engine->readWidths();

        GroomData held = data;
        held.graphJson = withParam(graph, "sculpt", "strength", strength);
        GroomSession committed(held);
        ASSERT_TRUE(committed.evaluate(ViewRequest{}).strands);
        const StrandsPtr want = committed.last().strands;
        const gpu::PointError e = gpu::compareVectors(points, want->positions());
        const double changed = gpu::compareVectors(before->positions(), want->positions()).max;
        double widthMax = 0.0;
        ASSERT_EQ(widths.size(), want->widths().size());
        for (std::size_t i = 0; i < widths.size(); ++i) widthMax = std::max(widthMax, static_cast<double>(std::abs(widths[i] - want->widths()[i])));
        std::printf("[ %-8s ] Sculpt strength dragged to %.1f: %zu points vs the CPU's: max %.3g mean %.3g, widths max %.3g; the hair "
                    "moved by %.3g\n",
                    GetParam().c_str(), strength, points.size(), e.max, e.mean, widthMax, changed);
        EXPECT_GT(e.count, 1000u);
        EXPECT_LT(e.max, kPointTolerance);
        EXPECT_LT(e.mean, kMeanTolerance);
        EXPECT_LT(widthMax, kWidthTolerance);
        EXPECT_GT(changed, 0.01) << "the value changes the hair";
    }
}

TEST_P(LookEngineTest, ACompoundsNodesRunOnTheGpu) {
    // A Noise grouped into a compound (its amplitude exposed as "amount"): the session holds the
    // compound as its nodes inline, so a drag of the compound's param runs its Noise on the GPU,
    // as the CPU evaluates the graph holding the value.
    nlohmann::json g = nlohmann::json::parse(withoutClearance(chainGraph({{"Noise", R"("params": {"amplitude": 0.1, "frequency": 2.0})"}})));
    for (auto& n : g["nodes"]) {
        if (n["name"] != "n0") continue;
        n = nlohmann::json::parse(R"j({"name": "n0", "type": "Compound", "params": {"amount": 0.1, "definition": {
              "graph": {"version": 1, "output": "noise",
                "nodes": [{"name": "in", "type": "Input", "params": {"name": "strands", "type": "Strands"}},
                          {"name": "noise", "type": "Noise", "params": {"amplitude": 0.1, "frequency": 2.0}}],
                "connections": [{"from": "in", "to": "noise", "input": "strands"}]},
              "inputs": [{"name": "strands", "type": "Strands"}],
              "expose": {"amount": "noise.amplitude"}}}})j");
    }
    const std::string graph = g.dump();
    GroomSession dragged(groom(graph));
    ASSERT_TRUE(dragged.evaluate(ViewRequest{}).strands);
    const StrandsPtr before = dragged.last().strands;
    gpu::LookPreview preview;
    const gpu::LookPreview::Shown shown = [&] { return dragged.last().strands; };
    gpu::LookPlan plan;
    std::string why;
    ASSERT_TRUE(preview.plan(dragged, shown, {{"n0", {{"amount", 0.3}}}}, {}, &plan, &why)) << why;
    ASSERT_FALSE(plan.chain.stages.empty());
    EXPECT_EQ(dragged.viewDocument()->nameOf(plan.chain.stages.front().node), "n0/noise");
    auto engine = backend_->engine();
    preview.upload(*engine, plan, dragged.last().scalp.get(), false);
    engine->run(plan.run, true);
    const std::vector<Vec3f> points = engine->readPositions();
    GroomSession committed(groom(withParam(graph, "n0", "amount", 0.3)));
    ASSERT_TRUE(committed.evaluate(ViewRequest{}).strands);
    const gpu::PointError e = gpu::compareVectors(points, committed.last().strands->positions());
    std::printf("[ %-8s ] a compound's Noise dragged: %zu points vs the CPU's: max %.3g mean %.3g\n", GetParam().c_str(), points.size(),
                e.max, e.mean);
    EXPECT_GT(e.count, 1000u);
    EXPECT_LT(e.max, kPointTolerance);
    EXPECT_GT(gpu::compareVectors(before->positions(), committed.last().strands->positions()).max, 0.01) << "the value changes the hair";
}

TEST_P(LookEngineTest, ASubdivideAtTheEndHandsTheStrandsOnForADrag) {
    // A Subdivide refining the output for the viewport ends the GPU's run: a drag of the Noise
    // before it is the CPU's. During a live drag (GroomSession::setLiveDrag) the view hands the
    // strands on unrefined and the GPU runs the Noise, as the CPU evaluates the graph holding the
    // value under the same drag.
    nlohmann::json g = nlohmann::json::parse(withoutClearance(chainGraph({{"Noise", R"("params": {"amplitude": 0.1, "frequency": 2.0})"}})));
    g["nodes"].push_back(nlohmann::json::parse(R"({"name": "subdivide", "type": "Subdivide", "params": {"viewportLevels": 1}})"));
    for (auto& c : g["connections"]) {
        if (c["to"] == "output") c["to"] = "subdivide";
    }
    g["connections"].push_back(nlohmann::json::parse(R"({"from": "subdivide", "to": "output", "input": "strands"})"));
    const std::string graph = g.dump();
    ViewRequest view;
    view.viewport = true;
    GroomSession dragged(groom(graph));
    ASSERT_TRUE(dragged.evaluate(view).strands);
    EXPECT_TRUE(dragged.viewRefinesStrands());
    gpu::LookPreview preview;
    const gpu::LookPreview::Shown shown = [&] { return dragged.last().strands; };
    gpu::LookPlan plan;
    std::string why;
    EXPECT_FALSE(preview.plan(dragged, shown, {{"n0", {{"amplitude", 0.3}}}}, {}, &plan, &why));
    dragged.setLiveDrag(true);
    ASSERT_TRUE(dragged.evaluate(view).strands);
    ASSERT_TRUE(preview.plan(dragged, shown, {{"n0", {{"amplitude", 0.3}}}}, {}, &plan, &why)) << why;
    auto engine = backend_->engine();
    preview.upload(*engine, plan, dragged.last().scalp.get(), false);
    engine->run(plan.run, true);
    const std::vector<Vec3f> points = engine->readPositions();
    GroomSession committed(groom(withParam(graph, "n0", "amplitude", 0.3)));
    committed.setLiveDrag(true);
    ASSERT_TRUE(committed.evaluate(view).strands);
    const gpu::PointError e = gpu::compareVectors(points, committed.last().strands->positions());
    std::printf("[ %-8s ] a Noise before a Subdivide dragged: %zu points vs the CPU's: max %.3g mean %.3g\n", GetParam().c_str(),
                points.size(), e.max, e.mean);
    EXPECT_GT(e.count, 1000u);
    EXPECT_LT(e.max, kPointTolerance);
    // The release: refined again.
    committed.setLiveDrag(false);
    EXPECT_GT(committed.evaluate(view).strands->pointCount(), points.size());
}

TEST(GpuPreview, SculptsGuidesThroughNodesTheGpuDoesNotRunAreTheCpus) {
    // A Sculpt node's strength drags on the GPU where its guides feed the look's Interpolate as it
    // makes them; through another node (a Noise of the guides) the guides Interpolate takes are not
    // the ones made at the dragged strength: the CPU's. The node's other params do not drag.
    EXPECT_TRUE(gpu::previewableParam("Sculpt", "strength"));
    EXPECT_FALSE(gpu::previewableParam("Sculpt", "seed"));
    GroomData data = groom(withoutClearance(sculptGraph(1.0, "Noise")));
    {
        GroomSession probe(data);
        ASSERT_TRUE(probe.evaluate(ViewRequest{}).strands);
        data.sculptsJson = hookedSculpts(probe);
    }
    GroomSession session(data);
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    gpu::LookPreview preview;
    const gpu::LookPreview::Shown shown = [&] { return session.last().strands; };
    gpu::LookPlan plan;
    std::string why;
    EXPECT_FALSE(preview.plan(session, shown, {{"sculpt", {{"strength", 0.5}}}}, {}, &plan, &why));
    EXPECT_NE(why.find("through nodes the GPU does not run"), std::string::npos) << why;
    // A stroke on it is the GPU's all the same: each dab's evaluation makes those guides, the Noise's
    // too, up to Interpolate.
    session.setSculptTarget("sculpt");
    session.setPreviewSculpt("sculpt", std::make_shared<const SculptEdits>(*session.sculpts().node("sculpt")), nullptr);
    session.setLiveStroke(true);
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    why.clear();
    EXPECT_TRUE(preview.armStroke(session, shown, &why)) << why;
    EXPECT_EQ(preview.stopAt(), "interpolate");
}

TEST(GpuPreview, TheHairGridsLayoutIsTheCpus) {
    // Their params change values on the grid alone: each drags on the GPU. A node with a scalp
    // deformed away from its rest pose (the walk asks the rest pose's field) is the CPU's.
    std::string why;
    EXPECT_TRUE(gpu::previewableParam("SelfCollide", "strength") && gpu::previewableParam("SelfCollide", "packing") &&
                gpu::previewableParam("SelfCollide", "profile"));
    EXPECT_TRUE(gpu::previewableParam("Static", "strength") && gpu::previewableParam("Static", "randomness") &&
                gpu::previewableParam("Static", "profile"));
    for (const char* type : {"SelfCollide", "Static"}) {
        SCOPED_TRACE(type);
        LookGraph g;
        g.after = type;
        GroomData data = deformedGroom(g.json());
        GroomSession session(data);
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        Prepared p;
        EXPECT_FALSE(prepareRun(session, {}, "after", &p));
        EXPECT_NE(p.why.find("deformed"), std::string::npos) << p.why;
    }
}

// ---- Tie -----------------------------------------------------------------------------------------

// A Tie's params: its ring behind the test's head (a sphere of radius 4 about the origin, strands
// about 3 long: those rooted near the back pass the ring, the rest stop on the way), then `extra`
// (JSON object items, a leading comma).
std::string tieParams(const std::string& extra = R"(, "spread": 15.0, "twist": 0.3, "ease": 0.3)",
                      const std::string& center = "[0.0, 1.0, -5.0]", double radius = 0.5) {
    return R"("params": {"center": )" + center + R"(, "radius": )" + std::to_string(radius) + extra + "}";
}

// The GPU's Tie against the CPU's: within the kernels' rounding (their sin, cos and tan, a strand's
// point found on its path in float where the CPU solves in double, the field's answers where the
// CPU's push asks the mesh), a few thousandths at most on a head 8 across, but where a route's
// closest point under its arc ties between two faces across the test sphere's seam: its split
// vertices' normals are each their own side's, so the skin point lifted off it is lifted one way or
// the other as the CPU's tree or the GPU's walk down the mesh meets the faces first, and the route
// over it moves by a few hundredths (a strand or two of a groom). Few points apart at all, a mean
// of millionths.
constexpr double kTiePointTolerance = 2e-2, kTieMeanTolerance = 1e-5;
void expectTieClose(const Compared& c, bool widths = true) {
    EXPECT_GT(c.positions.count, 1000u);
    EXPECT_LT(c.positions.max, kTiePointTolerance);
    EXPECT_LT(c.positions.mean, kTieMeanTolerance);
    EXPECT_LT(c.over, c.positions.count / 100);  // (more than 1e-4 apart)
    EXPECT_LT(c.normals.max, 5e-2);
    EXPECT_LT(c.tangents.max, 5e-2);
    if (widths) EXPECT_LT(c.widthMax, 1e-5);
}
#define EXPECT_GPU_TIES(graphJson, overrides, committedJson)                           \
    do {                                                                               \
        GroomSession dragged(groom(graphJson));                                        \
        ASSERT_TRUE(dragged.evaluate(ViewRequest{}).strands);                          \
        GroomSession committed(groom(committedJson));                                  \
        ASSERT_TRUE(committed.evaluate(ViewRequest{}).strands);                        \
        expectTieClose(runAndCompare(dragged, overrides, committed));                  \
    } while (0)

// How many of a node's points its last evaluation moved more than `by` from what it took.
std::size_t movedBy(GroomSession& session, const std::string& name, float by = 1e-3f) {
    const GraphDocument& doc = *session.viewDocument();
    const NodeId id = doc.nodesByName.at(name);
    NodeId source = 0;
    if (!doc.graph.inputSource(id, 0, &source)) return 0;
    const StrandsPtr taken = valueAs<StrandsPtr>(doc.graph.cachedValue(source)), made = valueAs<StrandsPtr>(doc.graph.cachedValue(id));
    if (!taken || !made || taken->pointCount() != made->pointCount()) return 0;
    std::size_t moved = 0;
    for (std::size_t i = 0; i < taken->pointCount(); ++i) moved += (taken->positions()[i] - made->positions()[i]).length() > by;
    return moved;
}

TEST_P(LookEngineTest, TieTiesAsTheCpuDoes) {
    // Tie on the GPU: its frame and ring's bins from the core strands, each strand's route over the
    // scalp to the ring, its path pushed out of the scalp and the strand laid along it (strands too
    // short stopping on the way): alone and after Interpolate (a run from it), with its tail channel
    // and without, tightly and loosely, with its direction its own, against the CPU's.
    const std::string extras[] = {R"(, "spread": 15.0, "twist": 0.3, "ease": 0.3)",
                                  R"(, "spread": 30.0, "tightness": 0.4, "ease": 0.0, "tailChannel": "")",
                                  R"(, "direction": [0.3, -1.0, -0.6], "twist": -0.5, "ease": 1.0)"};
    for (const std::string& extra : extras) {
        SCOPED_TRACE(extra);
        GroomSession session(groom(withoutClearance(chainGraph({{"Tie", tieParams(extra)}}))));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        EXPECT_GT(movedBy(session, "n0"), 20000u);
        const Prepared p = prepare(session, {}, "interpolate");
        ASSERT_EQ(p.chain.stages.size(), 3u);
        EXPECT_EQ(p.chain.stages[1].kind, gpu::StageKind::Tie);
        expectTieClose(runAndCompare(session, {}, session, "n0"));
        expectTieClose(runAndCompare(session, {}, session, "interpolate"));
    }
}

TEST_P(LookEngineTest, TieDragsGiveWhatTheCpuTies) {
    // Drags of each of its numbers and of its center and direction (the ring's bins made again from
    // the core strands), against a CPU evaluation of the graph holding the value.
    const std::string graph = withoutClearance(chainGraph({{"Tie", tieParams()}}));
    const std::pair<const char*, double> drags[] = {{"radius", 0.8}, {"spread", 35.0}, {"twist", -0.6}, {"tightness", 0.5}, {"ease", 0.8}};
    for (const auto& [param, value] : drags) {
        SCOPED_TRACE(param);
        EXPECT_GPU_TIES(graph, (gpu::Overrides{{"n0", {{param, value}}}}), withParam(graph, "n0", param, value));
    }
    {
        SCOPED_TRACE("center");
        EXPECT_GPU_TIES(graph, (gpu::Overrides{{"n0", {{"center", Vec3f(1.5f, 2.0f, -4.75f)}}}}),
                        withParam(graph, "n0", "center", nlohmann::json::array({1.5, 2.0, -4.75})));
    }
    {
        SCOPED_TRACE("direction");
        EXPECT_GPU_TIES(graph, (gpu::Overrides{{"n0", {{"direction", Vec3f(0.25f, -1.0f, -0.5f)}}}}),
                        withParam(graph, "n0", "direction", nlohmann::json::array({0.25, -1.0, -0.5})));
    }
}

TEST_P(LookEngineTest, ADabUpstreamOfTieGivesWhatTheCpuTies) {
    // A brush dab on the guides: Interpolate's strands blended again, then tied on the GPU (its bins
    // from the strands the dab moved).
    GroomData data = groom(withoutClearance(chainGraph({{"Tie", tieParams()}})));
    GroomSession dabbed(data);
    ASSERT_TRUE(dabbed.evaluate(ViewRequest{}).strands);
    const StrandsPtr before = dabbed.last().strands;
    const GuidesPtr bent = guideSet(*data.scalp, 1.4f);
    dabbed.setGuides(bent);
    ViewRequest dab;
    dab.stopAt = "interpolate";
    ASSERT_EQ(dabbed.evaluate(dab).strands, before);
    GroomData after = data;
    after.guides = bent;
    GroomSession fresh(after);
    ASSERT_TRUE(fresh.evaluate(ViewRequest{}).strands);
    expectTieClose(runAndCompare(dabbed, {}, fresh, "interpolate"));
}

TEST_P(LookEngineTest, ACurlByTheTailAfterTieAsTheCpuDoes) {
    // A Curl masked by the tail (its affect mapped to it, per point) runs on the GPU, mixed by it:
    // from the Curl (the tail as the CPU wrote it), from the Tie and from Interpolate (the tail as the
    // Tie writes it on the GPU, read after it); drags of the Tie's radius and the Curl's, a dab. A
    // Curl whose radius is mapped to the tail runs on the GPU, the tail as the CPU wrote it, where the
    // run starts after the Tie; a run of the Tie (which writes the tail the Curl reads) is the CPU's.
    const std::string curl = R"("params": {"radius": 0.08, "frequency": 1.5)";
    {
        const std::string graph = withoutClearance(chainGraph({{"Tie", tieParams()}, {"Curl", curl + R"(, "affect": {"attribute": "tail"}})"}}));
        GroomData data = groom(graph);
        GroomSession session(data);
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        EXPECT_GT(movedBy(session, "n1"), 2000u);
        const Prepared p = prepare(session, {}, "n0");
        ASSERT_EQ(p.chain.stages.size(), 3u);
        EXPECT_TRUE(p.run.params.stages[1].masked);
        EXPECT_EQ(p.run.fields.stages[1].affect.channelFrom, 0u);  // (the Tie's tail, written on the GPU)
        expectTieClose(runAndCompare(session, {}, session, "n1"));
        expectTieClose(runAndCompare(session, {}, session, "n0"));
        expectTieClose(runAndCompare(session, {}, session, "interpolate"));
        EXPECT_GPU_TIES(graph, (gpu::Overrides{{"n0", {{"radius", 0.8}}}}), withParam(graph, "n0", "radius", 0.8));
        EXPECT_GPU_TIES(graph, (gpu::Overrides{{"n1", {{"radius", 0.12}}}}), withParam(graph, "n1", "radius", 0.12));
        GroomSession dabbed(data);
        ASSERT_TRUE(dabbed.evaluate(ViewRequest{}).strands);
        const StrandsPtr before = dabbed.last().strands;
        const GuidesPtr bent = guideSet(*data.scalp, 1.4f);
        dabbed.setGuides(bent);
        ViewRequest dab;
        dab.stopAt = "interpolate";
        ASSERT_EQ(dabbed.evaluate(dab).strands, before);
        GroomData after = data;
        after.guides = bent;
        GroomSession fresh(after);
        ASSERT_TRUE(fresh.evaluate(ViewRequest{}).strands);
        expectTieClose(runAndCompare(dabbed, {}, fresh, "interpolate"));
    }
    GroomSession session(groom(withoutClearance(chainGraph({{"Tie", tieParams()}, {"Curl", curl + R"(, "radius": {"attribute": "tail"}})"}}))));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    EXPECT_GT(movedBy(session, "n1"), 2000u);
    expectTieClose(runAndCompare(session, {}, session, "n1"));
    Prepared refused;
    EXPECT_FALSE(prepareRun(session, {}, "n0", &refused));
    EXPECT_NE(refused.why.find("tail"), std::string::npos) << refused.why;
}

TEST_P(LookEngineTest, TwoTiesAsTheCpuDoes) {
    // A double ponytail (the left half of the head to a tie on the left, the right half to one on the
    // right: each Tie's affect a mask by the roots, read per strand), and a second Tie down the
    // first's tail (it starts where the first's tail does: the tail the first writes on the GPU when
    // both run). From the second, from the first, from Interpolate; drags of each one's radius.
    const std::string left = tieParams(R"(, "spread": 10.0, "affect": {"expression": "x < 0 ? 1 : 0"})", "[-5.0, 1.0, -1.5]");
    const std::string right = tieParams(R"(, "spread": 10.0, "twist": 0.2, "affect": {"expression": "x < 0 ? 0 : 1"})", "[5.0, 1.0, -1.5]");
    const std::string first = tieParams(R"(, "spread": 5.0)"), second = tieParams(R"(, "spread": 20.0)", "[0.0, 1.0, -6.0]", 0.3);
    const std::pair<const char*, std::string> graphs[] = {{"a double ponytail", withoutClearance(chainGraph({{"Tie", left}, {"Tie", right}}))},
                                                          {"a second Tie down the tail", withoutClearance(chainGraph({{"Tie", first}, {"Tie", second}}))}};
    for (const auto& [name, graph] : graphs) {
        SCOPED_TRACE(name);
        GroomSession session(groom(graph));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        EXPECT_GT(movedBy(session, "n0"), 5000u);
        EXPECT_GT(movedBy(session, "n1"), 2000u);
        const Prepared p = prepare(session, {}, "n0");
        ASSERT_EQ(p.chain.stages.size(), 3u);
        ASSERT_TRUE(p.run.inputs[1].tie);
        EXPECT_EQ(p.run.inputs[1].tie->channelFrom, 0u);  // (the first's tail, written on the GPU)
        expectTieClose(runAndCompare(session, {}, session, "n1"));
        expectTieClose(runAndCompare(session, {}, session, "n0"));
        expectTieClose(runAndCompare(session, {}, session, "interpolate"));
        EXPECT_GPU_TIES(graph, (gpu::Overrides{{"n1", {{"radius", 0.4}}}}), withParam(graph, "n1", "radius", 0.4));
        EXPECT_GPU_TIES(graph, (gpu::Overrides{{"n0", {{"radius", 0.7}}}}), withParam(graph, "n0", "radius", 0.7));
    }
}

TEST_P(LookEngineTest, TieAtAHundredThousandStrands) {
    // About 100k strands: the GPU's Tie against the CPU's, and its time on the GPU (the median of 5
    // runs after one; noisy where something else uses the GPU).
    LookGraph g;
    g.scatter = R"("params": {"density": 500.0, "length": 3.0})";
    g.after = "Tie";
    g.afterParams = tieParams();
    GroomSession session(groom(g.json()));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    EXPECT_GT(session.last().strands->strandCount(), 90000u);
    expectTieClose(runAndCompare(session, {}, session, "after"), false);
    const Prepared p = prepare(session, {}, "after");
    auto engine = backend_->engine();
    engine->setBase(p.base, session.last().scalp.get());
    engine->run(p.run, true);
    engine->times(true);
    std::vector<double> ms;
    for (int run = 0; run < 5; ++run) {
        engine->run(p.run, true);
        ms.push_back(engine->times(true).tieMs);
    }
    std::sort(ms.begin(), ms.end());
    std::printf("[ %-8s ] Tie at %zu strands, %zu points: %.3f ms on the GPU (median of 5)\n", GetParam().c_str(), p.base->strandCount(),
                p.base->pointCount(), ms[2]);
}

TEST(GpuPreview, CurlAddingPointsIsTheCpus) {
    // A Curl whose strands have too few points a turn for its tolerance adds points (curl_op.cpp):
    // its result is laid out unlike its input, which a run keeps, so the GPU's run starts after it.
    // At the points it was given (tolerance 0) it runs on the GPU.
    for (const double tolerance : {0.3, 0.0}) {
        LookGraph g;
        g.curl = R"("params": {"radius": 0.1, "frequency": 3.0, "tolerance": )" + std::to_string(tolerance) + "}";  // 1.7 points a turn
        GroomSession session(groom(g.json()));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const GraphDocument& doc = *session.viewDocument();
        const NodeId curl = doc.nodesByName.at("curl");
        const StrandsPtr in = valueAs<StrandsPtr>(doc.graph.cachedValue(doc.nodesByName.at("noise")));
        const StrandsPtr out = valueAs<StrandsPtr>(doc.graph.cachedValue(curl));
        ASSERT_TRUE(in && out);
        std::string why;
        gpu::LookChain chain;
        ASSERT_TRUE(gpu::findLookChain(doc, &chain, &why)) << why;
        if (tolerance > 0.0) {
            EXPECT_GT(out->pointCount(), in->pointCount() * 2);
            EXPECT_EQ(chain.base, curl);
            EXPECT_EQ(chain.node(gpu::StageKind::Curl), 0u);
            EXPECT_NE(chain.node(gpu::StageKind::Width), 0u);
        } else {
            EXPECT_EQ(out->pointCount(), in->pointCount());
            EXPECT_EQ(chain.node(gpu::StageKind::Curl), curl);
        }
    }
}

// ---- Wet -----------------------------------------------------------------------------------------

// A Wet's params (its levels and wetness, a clumpSize clumping the test's strands at every level).
std::string wetParams(int levels, double wetness) {
    return R"("params": {"levels": )" + std::to_string(levels) + R"(, "wetness": )" + std::to_string(wetness) + R"(, "clumpSize": 2.0})";
}

TEST_P(LookEngineTest, WetClumpsItsLevelsAsTheCpuDoes) {
    // Wet's levels run as Clump stages back to back, the largest first, each with its level's
    // params, profile and centers as the CPU's levels picked them (the smaller levels' curves made
    // on the GPU from the strands the level before moved): levels 1, 3 and 5 at wetness 0.3 and 1,
    // from Wet and from Interpolate (Wet's largest level's centers made on the GPU too).
    for (const int levels : {1, 3, 5}) {
        for (const double wetness : {0.3, 1.0}) {
            SCOPED_TRACE(std::to_string(levels) + " levels, wetness " + std::to_string(wetness));
            GroomSession session(groom(withoutClearance(chainGraph({{"Wet", wetParams(levels, wetness)}}))));
            ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
            const GraphDocument& doc = *session.viewDocument();
            const NodeId wet = doc.nodesByName.at("n0");
            const StrandsPtr dry = valueAs<StrandsPtr>(doc.graph.cachedValue(doc.nodesByName.at("interpolate")));
            const StrandsPtr wetted = valueAs<StrandsPtr>(doc.graph.cachedValue(wet));
            ASSERT_TRUE(dry && wetted);
            EXPECT_GT(gpu::compareVectors(dry->positions(), wetted->positions()).max, 0.2);  // (it clumps)
            {
                const Prepared p = prepare(session, {}, "n0");
                ASSERT_EQ(p.chain.stages.size(), static_cast<std::size_t>(levels) + 1);  // and Width
                for (int j = 0; j < levels; ++j) {
                    const gpu::LookStage& s = p.chain.stages[static_cast<std::size_t>(j)];
                    EXPECT_EQ(s.kind, gpu::StageKind::Clump);
                    EXPECT_EQ(s.node, wet);
                    EXPECT_EQ(s.level, j);
                    EXPECT_EQ(s.from, j == 0 ? gpu::kNoStage : static_cast<std::size_t>(j - 1));
                    // The largest level's centers as the CPU made them (nothing before moves
                    // them); the smaller ones' made from the strands the run moves.
                    const gpu::ClumpInputs& in = *p.run.inputs[static_cast<std::size_t>(j)].clump;
                    EXPECT_GT(in.centers, 0u);
                    EXPECT_EQ(in.curves != nullptr, j == 0);
                }
            }
            expectClose(runAndCompare(session, {}, session, "n0"));
            expectClose(runAndCompare(session, {}, session, "interpolate"));
        }
    }
}

TEST_P(LookEngineTest, WetAfterAndBeforeNoiseAndCurlAsTheCpuDoes) {
    // Wet after a Noise (its largest level's centers made from the strands the Noise moves) and
    // before a Curl coherent by Wet's biggest clumps; Wet after a Curl and before a Noise.
    const std::string noise = R"("params": {"amplitude": 0.2, "frequency": 1.2})";
    const std::string curl = R"("params": {"radius": 0.08, "frequency": 1.0, "coherence": "clump"})";
    const std::vector<std::vector<std::pair<std::string, std::string>>> chains = {
        {{"Noise", noise}, {"Wet", wetParams(3, 0.7)}, {"Curl", curl}},
        {{"Curl", R"("params": {"radius": 0.08, "frequency": 1.0})"}, {"Wet", wetParams(4, 0.5)}, {"Noise", noise}},
    };
    for (const auto& nodes : chains) {
        SCOPED_TRACE(nodes.front().first + " first");
        GroomSession session(groom(withoutClearance(chainGraph(nodes))));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        expectClose(runAndCompare(session, {}, session, "n1"));
        expectClose(runAndCompare(session, {}, session, "n0"));
        expectClose(runAndCompare(session, {}, session, "interpolate"));
    }
}

TEST_P(LookEngineTest, AWetnessDragGivesWhatTheCpuWetsForIt) {
    // Dragging wetness changes the levels' profiles alone (their centers stay): wetter, drier and
    // to 0 (the strands as they came), against a CPU evaluation of the graph holding the value.
    for (const int levels : {1, 3, 5}) {
        SCOPED_TRACE(levels);
        const std::string graph = withoutClearance(chainGraph({{"Noise", R"("params": {"amplitude": 0.2})"}, {"Wet", wetParams(levels, 0.5)}}));
        for (const double wetness : {0.3, 1.0, 0.0}) {
            SCOPED_TRACE(wetness);
            EXPECT_GPU_MATCHES(graph, (gpu::Overrides{{"n1", {{"wetness", wetness}}}}), withParam(graph, "n1", "wetness", wetness));
        }
    }
}

TEST_P(LookEngineTest, ADabUpstreamOfWetGivesWhatTheCpuWets) {
    // A brush dab on the guides: Interpolate's strands blended again, then Wet's levels clumping
    // them about centers made from them, on the GPU.
    GroomData data = groom(withoutClearance(chainGraph({{"Wet", wetParams(3, 0.8)}})));
    EXPECT_DAB_MATCHES(data);
    GroomData curled = groom(withoutClearance(chainGraph({{"Wet", wetParams(5, 1.0)}, {"Curl", R"("params": {"radius": 0.08})"}})));
    EXPECT_DAB_MATCHES(curled);
}

TEST(GpuPreview, WetsCentersAreTheCpus) {
    // Wet's clumpSize, levels, randomness and seed pick its levels' centers: dragging them is the
    // CPU's (wetness the GPU's). At a wetness of 0 it hands its input on as it came: no stage, the
    // nodes around it the GPU's. A field after it reading its wetness (which a drag changes) is
    // the CPU's.
    GroomSession session(groom(chainGraph({{"Wet", wetParams(3, 0.6)}})));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    const GraphDocument& doc = *session.viewDocument();
    std::string why;
    EXPECT_TRUE(gpu::previewableOverrides(doc, {{"n0", {{"wetness", 0.9}}}}, &why)) << why;
    for (const char* param : {"clumpSize", "levels", "randomness", "seed"}) {
        SCOPED_TRACE(param);
        EXPECT_FALSE(gpu::previewableOverrides(doc, {{"n0", {{param, 2.0}}}}, &why));
        EXPECT_NE(why.find(std::string("n0.") + param), std::string::npos) << why;
    }

    GroomSession dry(groom(chainGraph({{"Noise", R"("params": {"amplitude": 0.2})"}, {"Wet", wetParams(3, 0.0)}})));
    ASSERT_TRUE(dry.evaluate(ViewRequest{}).strands);
    gpu::LookChain chain;
    ASSERT_TRUE(gpu::findLookChain(*dry.viewDocument(), &chain, &why)) << why;
    EXPECT_EQ(chain.node(gpu::StageKind::Clump), 0u);
    EXPECT_NE(chain.node(gpu::StageKind::Noise), 0u);
    EXPECT_EQ(chain.node(gpu::StageKind::Interpolate), dry.viewDocument()->nodesByName.at("interpolate"));
    EXPECT_EQ(gpu::firstOverridden(*dry.viewDocument(), chain, {{"n1", {{"wetness", 0.5}}}}), gpu::kNoStage);  // no stage of it to drag
    Prepared p;
    EXPECT_TRUE(prepareRun(dry, {}, "interpolate", &p)) << p.why;

    GroomSession reading(groom(chainGraph(
        {{"Wet", wetParams(3, 0.6)}, {"Noise", R"("params": {"amplitude": {"expression": "wetness", "min": 0.0, "max": 0.3}})"}})));
    ASSERT_TRUE(reading.evaluate(ViewRequest{}).strands);
    Prepared q;
    EXPECT_FALSE(prepareRun(reading, {}, "n0", &q));
    EXPECT_NE(q.why.find("wetness"), std::string::npos) << q.why;
}

TEST(GpuPreview, AnAutoPartsLinesAreTheCpus) {
    // An auto Part's splitAngle and minLength pick which lines it finds: dragging them is the
    // CPU's (its width and strength the GPU's). One whose last evaluation found no lines with
    // partsFrom auto (not evaluated so) keeps none for its result: the CPU's.
    GroomSession session(autoPartGroom());
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    const GraphDocument& doc = *session.viewDocument();
    std::string why;
    for (const char* param : {"width", "strength"}) {
        EXPECT_TRUE(gpu::previewableOverrides(doc, {{"after", {{param, 0.5}}}}, &why)) << param << ": " << why;
    }
    for (const char* param : {"splitAngle", "minLength"}) {
        SCOPED_TRACE(param);
        EXPECT_FALSE(gpu::previewableOverrides(doc, {{"after", {{param, 0.5}}}}, &why));
        EXPECT_NE(why.find(std::string("after.") + param), std::string::npos) << why;
    }

    GroomData data = autoPartGroom();
    data.graphJson = withParam(data.graphJson, "after", "partsFrom", "painted");
    GroomSession painted(data);
    ASSERT_TRUE(painted.evaluate(ViewRequest{}).strands);
    PartsPtr lines;
    EXPECT_FALSE(keptAutoParts(painted, &lines));
    GraphDocument flipped = painted.viewDocument()->clone();
    const NodeId part = flipped.nodesByName.at("after");
    flipped.graph.node(part).params().set("partsFrom", std::string("auto"));
    gpu::LookChain full;
    ASSERT_TRUE(gpu::findLookChain(flipped, &full, &why)) << why;
    const gpu::LookChain chain = gpu::chainFrom(flipped, full, full.indexOf(part));
    gpu::LookParams params;
    EXPECT_FALSE(gpu::readLookParams(flipped, chain, {}, &params, &why));
    EXPECT_NE(why.find("keeps no parts"), std::string::npos) << why;
}

TEST(GpuPreview, TakesOnlyWhatItRuns) {
    std::string why;
    gpu::LookChain chain;
    {
        LookGraph g;
        g.after = "Group";  // Group after Width: the CPU's
        GroomSession session(groom(g.json()));
        session.evaluate(ViewRequest{});
        EXPECT_FALSE(gpu::findLookChain(*session.viewDocument(), &chain, &why));
    }
    {
        // A map read from the points (t, by arc length) holds only on the first stage of a run, and
        // Noise and Curl read each point after the ones before it moved.
        LookGraph g;
        g.width = R"("params": {"width": {"expression": "0.02 - 0.015 * t", "min": 0.0, "max": 1.0}})";
        g.curl = R"("params": {"radius": {"expression": "0.1 * t", "min": 0.0, "max": 1.0}, "frequency": 0.8})";
        GroomSession session(groom(g.json()));
        session.evaluate(ViewRequest{});
        const GraphDocument& doc = *session.viewDocument();
        gpu::LookChain full;
        ASSERT_TRUE(gpu::findLookChain(doc, &full, &why)) << why;
        chain = gpu::chainFrom(doc, full, full.indexOf(doc.nodesByName.at("curl")));
        gpu::LookFields fields;
        ASSERT_TRUE(gpu::readLookFields(doc, chain, session.last().scalp.get(), &fields, &why)) << why;
        EXPECT_EQ(fields.stages[1].field[0].mode, gpu::FieldValues::kPerPoint);
        EXPECT_TRUE(fields.stages[1].field[0].readsPositions);
        EXPECT_TRUE(gpu::sameLookFields(doc, chain, fields));
        EXPECT_FALSE(gpu::lookFieldsHold(doc, chain, fields, &why));  // Curl's radius by t, per point
        EXPECT_NE(why.find("curl.radius"), std::string::npos) << why;
        g.curl = R"("params": {"radius": 0.1, "frequency": 0.8})";
        GroomSession plain(groom(g.json()));
        plain.evaluate(ViewRequest{});
        const GraphDocument& doc2 = *plain.viewDocument();
        ASSERT_TRUE(gpu::findLookChain(doc2, &full, &why));
        const gpu::LookChain widthOnly = gpu::chainFrom(doc2, full, full.indexOf(doc2.nodesByName.at("width")));
        ASSERT_TRUE(gpu::readLookFields(doc2, widthOnly, plain.last().scalp.get(), &fields, &why)) << why;
        EXPECT_TRUE(gpu::lookFieldsHold(doc2, widthOnly, fields, &why)) << why;
        const gpu::LookChain fromNoise = gpu::chainFrom(doc2, full, full.indexOf(doc2.nodesByName.at("noise")));
        ASSERT_TRUE(gpu::readLookFields(doc2, fromNoise, plain.last().scalp.get(), &fields, &why)) << why;
        EXPECT_FALSE(gpu::lookFieldsHold(doc2, fromNoise, fields, &why));
        EXPECT_FALSE(gpu::sameLookFields(doc, fromNoise, fields));  // another evaluation's
    }
    {
        // Curl's coil (the strands' widths), curls by clump, Clump's flatness and per-clump
        // variation, Gravity, Flyaway and Collide are the GPU's; Gravity taking its direction per
        // strand (an expression) and a Collide asking the mesh at every point the CPU's, saying why.
        gpu::LookChain full;
        gpu::LookParams params;
        const std::pair<std::string, std::string> taken[] = {
            {"curl", R"("params": {"shape": "coil", "coilWidths": 4.0})"},
            {"curl", R"("params": {"coherence": "clump"})"},
            {"clump", R"("params": {"strength": 0.8, "flatness": 0.6, "strengthRandom": 0.5, "volumize": 0.3})"},
            {"Gravity", R"("params": {"strength": 0.8})"},
            {"Flyaway", R"("params": {"fraction": 0.2})"},
            {"Collide", R"("params": {"offset": 0.1})"}};
        for (const auto& [node, json] : taken) {
            SCOPED_TRACE(node + " " + json);
            LookGraph g;
            if (node == "curl") {
                g.curl = json;
            } else if (node == "clump") {
                g.clump = json;
            } else {
                g.after = node;
                g.afterParams = json;
                if (node == "Collide") g.connections = R"(, {"from": "scalp", "to": "after", "input": "scalp"})";
            }
            GroomSession other(groom(g.json()));
            other.evaluate(ViewRequest{});
            ASSERT_TRUE(gpu::findLookChain(*other.viewDocument(), &full, &why)) << why;
            EXPECT_TRUE(gpu::readLookParams(*other.viewDocument(), full, {}, &params, &why)) << why;
            if (node != "curl" && node != "clump") EXPECT_EQ(other.viewDocument()->nameOf(full.stages.back().node), "after");
        }
        const std::pair<std::string, std::string> refused[] = {
            {"Gravity", R"("params": {"direction": {"expression": "[x, -1, 0]"}})"},
            {"Collide", R"("params": {"method": "exact"})"}};
        for (const auto& [node, json] : refused) {
            SCOPED_TRACE(node + " " + json);
            LookGraph g;
            g.after = node;
            g.afterParams = json;
            if (node == "Collide") g.connections = R"(, {"from": "scalp", "to": "after", "input": "scalp"})";
            GroomSession other(groom(g.json()));
            ASSERT_TRUE(other.evaluate(ViewRequest{}).strands);
            ASSERT_TRUE(gpu::findLookChain(*other.viewDocument(), &full, &why)) << why;
            EXPECT_FALSE(gpu::readLookParams(*other.viewDocument(), full, {}, &params, &why));
            EXPECT_NE(why.find("after"), std::string::npos) << why;
        }
        // A Jiggle with no motion of the scalp to lag behind hands its input on as it came.
        LookGraph jiggled;
        jiggled.after = "Jiggle";
        GroomSession still(groom(jiggled.json()));
        ASSERT_TRUE(still.evaluate(ViewRequest{}).strands);
        ASSERT_TRUE(gpu::findLookChain(*still.viewDocument(), &full, &why)) << why;
        const NodeId jiggle = still.viewDocument()->nodesByName.at("after");
        EXPECT_NE(std::find(full.passOn.begin(), full.passOn.end(), jiggle), full.passOn.end());
        EXPECT_TRUE(full.node(gpu::StageKind::Width));
        // Cards and RenderData ending the graph hand the GPU's points on (their cards and channels
        // as the last evaluation made them: the commit makes them again), cards built or not.
        for (const char* node : {"Cards", "RenderData"}) {
            for (const bool built : {false, true}) {
                SCOPED_TRACE(std::string(node) + (built ? " built" : ""));
                LookGraph ended;
                ended.after = node;
                GroomSession made(groom(ended.json()));
                ViewRequest request;
                request.buildCards = built;
                ASSERT_TRUE(made.evaluate(request).strands);
                ASSERT_TRUE(gpu::findLookChain(*made.viewDocument(), &full, &why)) << why;
                const NodeId after = made.viewDocument()->nodesByName.at("after");
                EXPECT_NE(std::find(full.passOn.begin(), full.passOn.end(), after), full.passOn.end());
                StrandsPtr base, output;
                EXPECT_TRUE(gpu::lookStrands(*made.viewDocument(), full, &base, &output, &why)) << why;
            }
        }
    }
    {
        // HairColor after Width hands the GPU's points on, Bands too (bands by point index); Bands
        // picking guard hairs by length follows the run's lengths: the CPU's, saying why.
        LookGraph g;
        g.after = "HairColor";
        std::string json = g.json();
        GroomSession banded(groom(json));
        banded.evaluate(ViewRequest{});
        EXPECT_TRUE(gpu::findLookChain(*banded.viewDocument(), &chain, &why)) << why;
        const std::string plainColor = R"("type": "HairColor"})";
        json.replace(json.find(plainColor), plainColor.size(),
                     R"("type": "Bands", "params": {"guards": 0.5, "guardsBy": "longest"}})");
        GroomSession longest(groom(json));
        longest.evaluate(ViewRequest{});
        EXPECT_FALSE(gpu::findLookChain(*longest.viewDocument(), &chain, &why));
        EXPECT_NE(why.find("guard hairs by length"), std::string::npos) << why;
    }
    {
        GroomSession session(groom(graph(0.3)));
        session.evaluate(ViewRequest{});
        ASSERT_TRUE(gpu::findLookChain(*session.viewDocument(), &chain, &why)) << why;
        ASSERT_EQ(chain.stages.size(), 4u);
        EXPECT_EQ(chain.stages[0].kind, gpu::StageKind::Clump);
        EXPECT_TRUE(chain.node(gpu::StageKind::Noise) && chain.node(gpu::StageKind::Curl) && chain.node(gpu::StageKind::Width));
        EXPECT_EQ(session.viewDocument()->nameOf(chain.base), "scatter");
        EXPECT_TRUE(gpu::previewableParam("Noise", "amplitude"));
        EXPECT_TRUE(gpu::previewableParam("Noise", "seed"));
        EXPECT_TRUE(gpu::previewableParam("Noise", "profile"));
        EXPECT_TRUE(gpu::previewableParam("Clump", "strength"));
        EXPECT_TRUE(gpu::previewableParam("Clump", "seed"));  // refused where it picks its own centers
        EXPECT_FALSE(gpu::previewableParam("Clump", "count"));
        EXPECT_TRUE(gpu::previewableParam("Clump", "volumizeProfile"));
        EXPECT_FALSE(gpu::previewableParam("Clump", "copyAmount"));  // the centers' shapes: the CPU's
        EXPECT_TRUE(gpu::previewableParam("Interpolate", "falloff"));
        EXPECT_FALSE(gpu::previewableParam("Scatter", "seed"));      // the roots: the CPU's
        EXPECT_FALSE(gpu::previewableParam("Resample", "segments"));  // the counts: the CPU's
        EXPECT_TRUE(gpu::previewableParam("Curl", "phase"));
        EXPECT_TRUE(gpu::previewableParam("Interpolate", "limitDistance"));
        EXPECT_FALSE(gpu::previewableParam("Interpolate", "neighbors"));  // the binding: the CPU's
        EXPECT_TRUE(gpu::previewableParam("Gravity", "stiffnessProfile"));
        EXPECT_TRUE(gpu::previewableParam("Flyaway", "seed"));
        EXPECT_TRUE(gpu::previewableParam("Curl", "coilWidths"));
        EXPECT_TRUE(gpu::previewableParam("Curl", "spacingRandom"));
        EXPECT_TRUE(gpu::previewableParam("Curl", "frequencyProfile"));
        EXPECT_TRUE(gpu::previewableParam("Curl", "weight"));
        EXPECT_TRUE(gpu::previewableParam("Curl", "weightDirection"));
        EXPECT_TRUE(gpu::previewableParam("Collide", "offset"));     // a number of the kernel's (its fields' tables made for it)
        EXPECT_FALSE(gpu::previewableParam("Collide", "softness"));  // its fields' tables: the CPU's
        EXPECT_FALSE(gpu::previewableParam("Gravity", "direction"));
    }
}

// ---- The shapers working on each strand alone: Bend, Direction, Transform, Displace, Twist ---------

// A shaper of `type` (`params`) between Interpolate (not keeping clearance) and a Noise, then the
// Width: the node is "n0", or "n1" after a Clump (`clump`: Twist's clumps) or another node `before`
// (its type and params).
std::string shaperGraph(const std::string& type, const std::string& params, bool clump = false,
                        const std::pair<std::string, std::string>& before = {}) {
    std::vector<std::pair<std::string, std::string>> nodes;
    if (clump) nodes.push_back({"Clump", R"("params": {"count": 60, "strength": 0.5})"});
    if (!before.first.empty()) nodes.push_back(before);
    nodes.push_back({type, params});
    nodes.push_back({"Noise", R"("params": {"amplitude": 0.15, "frequency": 1.3})"});
    return withoutClearance(chainGraph(nodes));
}
std::string shaperName(bool clump, bool before = false) { return clump || before ? "n1" : "n0"; }

// `graphJson` with the guides also feeding node `name`'s axes (Twist around the guides' curves).
std::string withAxes(const std::string& graphJson, const std::string& name) {
    nlohmann::json g = nlohmann::json::parse(graphJson);
    g["connections"].push_back({{"from", "guides"}, {"to", name}, {"input", "axes"}});
    return g.dump();
}

// The shaper (node `node` of `data`'s graph) on the GPU from it, and from Interpolate (the node then
// a Noise after it), against the CPU; then a dab on the guides.
#define EXPECT_SHAPER_MATCHES(data, node)                              \
    do {                                                               \
        GroomSession shaped(data);                                     \
        ASSERT_TRUE(shaped.evaluate(ViewRequest{}).strands);           \
        expectClose(runAndCompare(shaped, {}, shaped, node));          \
        expectClose(runAndCompare(shaped, {}, shaped, "interpolate")); \
        EXPECT_DAB_MATCHES(data);                                      \
    } while (0)

// A drag of node `node`'s `param` to `value` (a number or a vector) against the CPU's evaluation of it
// committed.
#define EXPECT_SHAPER_DRAG(graphJson, node, param, value, stored)                                                  \
    do {                                                                                                         \
        SCOPED_TRACE(param);                                                                                     \
        EXPECT_GPU_MATCHES(graphJson, (gpu::Overrides{{node, {{param, value}}}}), withParam(graphJson, node, param, stored)); \
    } while (0)

TEST_P(LookEngineTest, BendBendsAsTheCpuDoes) {
    // Along the normal (stood up, laid down, bent over from straight up), toward a direction laid on
    // the scalp (a comb's 180 with start and end 0), along the scalp's U (its UVs), its angle by a map
    // per strand; from Bend and from Interpolate (Noise after it); a dab; drags of its numbers, seed and
    // direction.
    const std::pair<std::string, bool> cases[] = {
        {R"("params": {"angle": 40.0, "start": 0.2, "end": 0.8, "randomness": 0.3, "seed": 3})", false},
        {R"("params": {"angle": -50.0, "end": 0.0})", false},
        {R"("params": {"angle": 180.0, "end": 0.0, "along": "direction", "direction": [0.0, -1.0, 0.3]})", false},
        {R"("params": {"angle": {"expression": "rand", "min": -60.0, "max": 60.0}, "along": "direction", "direction": [1.0, -1.0, 0.3], "end": 0.6})",
         false},
        {R"("params": {"angle": 70.0, "along": "tangent", "direction": [0.0, 0.0, 1.0], "start": 0.1})", true}};
    for (const auto& [params, uvs] : cases) {
        SCOPED_TRACE(params);
        const std::string graph = shaperGraph("Bend", params);
        GroomData data = uvs ? paintedGroom(graph) : groom(graph);
        EXPECT_SHAPER_MATCHES(data, "n0");
    }
    const std::string graph = shaperGraph("Bend", R"("params": {"angle": 40.0, "along": "direction", "direction": [1.0, -1.0, 0.0], "randomness": 0.2})");
    EXPECT_SHAPER_DRAG(graph, "n0", "angle", 75.0, 75.0);
    EXPECT_SHAPER_DRAG(graph, "n0", "start", 0.3, 0.3);
    EXPECT_SHAPER_DRAG(graph, "n0", "end", 0.7, 0.7);
    EXPECT_SHAPER_DRAG(graph, "n0", "randomness", 0.6, 0.6);
    EXPECT_SHAPER_DRAG(graph, "n0", "seed", 7.0, 7);
    EXPECT_SHAPER_DRAG(graph, "n0", "direction", Vec3f(0.0f, -1.0f, 1.0f), nlohmann::json::array({0.0, -1.0, 1.0}));
}

TEST_P(LookEngineTest, DirectionTurnsAsTheCpuDoes) {
    // Its inclination and roll with random spreads (roll 0 along a direction laid on the scalp, then
    // along the scalp's U), part of the way, two of its params by maps per strand; from Direction and
    // from Interpolate; a dab; drags.
    const std::pair<std::string, bool> cases[] = {
        {R"("params": {"inclination": 60.0, "roll": 30.0, "rollRandom": 20.0, "inclinationRandom": 5.0, "seed": 2, "along": "direction"})", false},
        {R"("params": {"inclination": {"expression": "rand", "min": 20.0, "max": 80.0}, "strength": {"expression": "rand", "min": 0.2, "max": 1.0},
                       "roll": 45.0, "along": "direction", "direction": [1.0, 0.0, 0.0]})",
         false},
        {R"("params": {"inclination": 70.0, "roll": {"expression": "rand", "min": -90.0, "max": 90.0}, "strength": 0.6})", true}};
    for (const auto& [params, uvs] : cases) {
        SCOPED_TRACE(params);
        const std::string graph = shaperGraph("Direction", params);
        GroomData data = uvs ? paintedGroom(graph) : groom(graph);
        EXPECT_SHAPER_MATCHES(data, "n0");
    }
    const std::string graph = shaperGraph("Direction", R"("params": {"inclination": 50.0, "roll": 20.0, "along": "direction", "rollRandom": 10.0})");
    EXPECT_SHAPER_DRAG(graph, "n0", "inclination", 75.0, 75.0);
    EXPECT_SHAPER_DRAG(graph, "n0", "roll", -40.0, -40.0);
    EXPECT_SHAPER_DRAG(graph, "n0", "strength", 0.5, 0.5);
    EXPECT_SHAPER_DRAG(graph, "n0", "inclinationRandom", 12.0, 12.0);
    EXPECT_SHAPER_DRAG(graph, "n0", "rollRandom", 60.0, 60.0);
    EXPECT_SHAPER_DRAG(graph, "n0", "seed", 4.0, 4);
    EXPECT_SHAPER_DRAG(graph, "n0", "direction", Vec3f(0.0f, 0.0f, 1.0f), nlohmann::json::array({0.0, 0.0, 1.0}));
}

TEST_P(LookEngineTest, TransformMovesAsTheCpuDoes) {
    // Each strand about its root in its root's frame (the scalp's U from its UVs) with randoms, the
    // set about a pivot with its scale by a map, nothing to do; from Transform and from Interpolate; a
    // dab; drags of its numbers and vectors.
    const std::pair<std::string, bool> cases[] = {
        {R"("params": {"translate": [0.01, 0.02, 0.0], "rotate": [10.0, 30.0, -5.0], "scale": 0.8, "translateRandom": [0.02, 0.0, 0.02],
                       "rotateRandom": [3.0, 15.0, 0.0], "scaleRandom": 0.15, "seed": 2})",
         true},
        {R"("params": {"rotate": [0.0, 20.0, 10.0], "scale": 1.1})", false},
        {R"("params": {"space": "world", "translate": [0.1, 0.0, 0.0], "rotate": [0.0, 25.0, 0.0], "pivot": [0.0, 1.0, 0.0],
                       "scale": {"expression": "rand", "min": 0.8, "max": 1.2}})",
         false},
        {R"("params": {})", false}};
    for (const auto& [params, uvs] : cases) {
        SCOPED_TRACE(params);
        const std::string graph = shaperGraph("Transform", params);
        GroomData data = uvs ? paintedGroom(graph) : groom(graph);
        EXPECT_SHAPER_MATCHES(data, "n0");
    }
    const std::string graph = shaperGraph("Transform", R"("params": {"rotate": [10.0, 0.0, 5.0], "rotateRandom": [5.0, 5.0, 0.0], "seed": 1})");
    EXPECT_SHAPER_DRAG(graph, "n0", "scale", 1.2, 1.2);
    EXPECT_SHAPER_DRAG(graph, "n0", "scaleRandom", 0.3, 0.3);
    EXPECT_SHAPER_DRAG(graph, "n0", "seed", 5.0, 5);
    EXPECT_SHAPER_DRAG(graph, "n0", "translate", Vec3f(0.05f, 0.1f, 0.0f), nlohmann::json::array({0.05, 0.1, 0.0}));
    EXPECT_SHAPER_DRAG(graph, "n0", "rotate", Vec3f(20.0f, 0.0f, 10.0f), nlohmann::json::array({20.0, 0.0, 10.0}));
    EXPECT_SHAPER_DRAG(graph, "n0", "translateRandom", Vec3f(0.03f, 0.0f, 0.03f), nlohmann::json::array({0.03, 0.0, 0.03}));
    EXPECT_SHAPER_DRAG(graph, "n0", "rotateRandom", Vec3f(0.0f, 40.0f, 0.0f), nlohmann::json::array({0.0, 40.0, 0.0}));
    const std::string world = shaperGraph("Transform", R"("params": {"space": "world", "rotate": [0.0, 15.0, 0.0], "scale": 0.9})");
    EXPECT_SHAPER_DRAG(world, "n0", "pivot", Vec3f(0.5f, 2.0f, 0.0f), nlohmann::json::array({0.5, 2.0, 0.0}));
}

TEST_P(LookEngineTest, DisplaceMovesAsTheCpuDoes) {
    // Along the scalp's normal, along a direction by a painted map (per strand), by a map per point
    // (by point index); from Displace and from Interpolate; a dab; drags of its distance, profile and
    // direction.
    const std::pair<std::string, bool> cases[] = {
        {R"("params": {"distance": 0.2})", false},
        {R"("params": {"distance": {"colorSet": "mask", "min": 0.0, "max": 0.3}, "along": "direction", "direction": [0.0, 1.0, 1.0]})", true},
        {R"("params": {"distance": {"expression": "0.05 + 0.2 * pt", "min": 0.0, "max": 1.0}, "profile": [[0, 0], [1, 1]]})", false}};
    for (const auto& [params, uvs] : cases) {
        SCOPED_TRACE(params);
        const std::string graph = shaperGraph("Displace", params);
        GroomData data = uvs ? paintedGroom(graph) : groom(graph);
        EXPECT_SHAPER_MATCHES(data, "n0");
    }
    const std::string graph = shaperGraph("Displace", R"("params": {"distance": 0.15, "along": "direction", "direction": [1.0, 0.5, 0.0]})");
    EXPECT_SHAPER_DRAG(graph, "n0", "distance", -0.25, -0.25);
    EXPECT_SHAPER_DRAG(graph, "n0", "direction", Vec3f(0.0f, -1.0f, 0.0f), nlohmann::json::array({0.0, -1.0, 0.0}));
    EXPECT_GPU_MATCHES(graph, (rampDrag("n0", "profile", "[[0, 0], [0.5, 1], [1, 0.3]]")),
                       withParam(graph, "n0", "profile", nlohmann::json::parse("[[0, 0], [0.5, 1], [1, 0.3]]")));
}

TEST_P(LookEngineTest, TwistTurnsAsTheCpuDoes) {
    // Around each clump's middle (a Clump before it; its turns by a map per strand, a profile), and
    // around the guides' curves (its axes input; after a Transform moving the roots, each strand's
    // axis found again for its moved root); from Twist and from Interpolate; a dab (around the guides:
    // the axes the dab's guides); drags of its turns and profile.
    for (const std::string& params : {std::string(R"("params": {"turns": 1.5})"),
                                      std::string(R"("params": {"turns": {"expression": "rand", "min": 0.5, "max": 2.0}, "profile": [[0, 0], [1, 1]]})")}) {
        SCOPED_TRACE(params);
        EXPECT_SHAPER_MATCHES(groom(shaperGraph("Twist", params, true)), "n1");
    }
    {
        SCOPED_TRACE("around the guides");
        EXPECT_SHAPER_MATCHES(groom(withAxes(shaperGraph("Twist", R"("params": {"turns": 0.8})"), "n0")), "n0");
        const std::pair<std::string, std::string> moved{"Transform", R"("params": {"translate": [0.0, 0.3, 0.0], "rotate": [0.0, 10.0, 0.0]})"};
        GroomData data = groom(withAxes(shaperGraph("Twist", R"("params": {"turns": 0.8})", false, moved), "n1"));
        GroomSession session(data);
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        expectClose(runAndCompare(session, {}, session, "n0"));  // (the roots the Transform moves)
        expectClose(runAndCompare(session, {}, session, "n1"));
    }
    {
        // A stroke's dab on the guides the axes are (read as the press read them before): the axes as
        // the dab made them, each strand's nearest found on the GPU.
        SCOPED_TRACE("a dab on the axes");
        GroomData data = groom(withAxes(shaperGraph("Twist", R"("params": {"turns": 0.8})"), "n0"));
        GroomSession dabbed(data);
        ASSERT_TRUE(dabbed.evaluate(ViewRequest{}).strands);
        gpu::LookInputs kept;
        Prepared press;
        ASSERT_TRUE(prepareRun(dabbed, {}, "interpolate", &press, &kept)) << press.why;
        const GuidesPtr bent = guideSet(*data.scalp, 1.4f);
        dabbed.setGuides(bent);
        ViewRequest dab;
        dab.stopAt = "interpolate";
        ASSERT_TRUE(dabbed.evaluate(dab).strands);
        Prepared p;
        ASSERT_TRUE(prepareRun(dabbed, {}, "interpolate", &p, &kept)) << p.why;
        const gpu::TwistInputs* twist = nullptr;
        for (const gpu::StageInputs& in : p.run.inputs) twist = in.twist ? in.twist.get() : twist;
        ASSERT_TRUE(twist && twist->strands && twist->axes);
        EXPECT_EQ((*twist->strands)[1], 0xffffffffu);  // (no root found on the CPU)
        GroomData after = data;
        after.guides = bent;
        GroomSession fresh(after);
        ASSERT_TRUE(fresh.evaluate(ViewRequest{}).strands);
        auto engine = backend_->engine();
        engine->setBase(p.base, dabbed.last().scalp.get());
        engine->run(p.run, false);
        const gpu::PointError e = gpu::compareVectors(engine->readPositions(), fresh.last().strands->positions());
        std::printf("[ %-8s ] twist around the guides a dab made: positions max %.3g mean %.3g\n", GetParam().c_str(), e.max, e.mean);
        EXPECT_LT(e.max, kPointTolerance);
        EXPECT_LT(e.mean, kMeanTolerance);
    }
    const std::string graph = shaperGraph("Twist", R"("params": {"turns": 1.0})", true);
    EXPECT_SHAPER_DRAG(graph, "n1", "turns", -2.0, -2.0);
    EXPECT_GPU_MATCHES(graph, (rampDrag("n1", "profile", "[[0, 0.2], [1, 1]]")),
                       withParam(graph, "n1", "profile", nlohmann::json::parse("[[0, 0.2], [1, 1]]")));
}

TEST_P(LookEngineTest, ShapersStayInTheBounds) {
    // Every point a run of the shapers makes (dragged further than their params) inside lookBounds.
    const std::pair<std::string, std::string> nodes[] = {
        {"Bend", R"("params": {"angle": 90.0})"},
        {"Direction", R"("params": {"inclination": 80.0})"},
        {"Transform", R"("params": {"scale": 1.5, "translate": [0.2, 0.3, 0.0], "rotate": [30.0, 0.0, 0.0]})"},
        {"Transform", R"("params": {"space": "world", "scale": 1.3, "pivot": [2.0, 5.0, 0.0], "rotate": [0.0, 0.0, 40.0]})"},
        {"Displace", R"("params": {"distance": 0.6})"},
        {"Twist", R"("params": {"turns": 3.0})"}};
    for (const auto& [type, params] : nodes) {
        SCOPED_TRACE(type + " " + params);
        GroomSession session(groom(shaperGraph(type, params, type == "Twist")));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const Prepared p = prepare(session, {}, shaperName(type == "Twist"));
        const gpu::LookBounds b = gpu::lookBounds(p.run.params, p.run.fields, p.run.inputs, *p.base);
        ASSERT_TRUE(b.valid);
        auto engine = backend_->engine();
        engine->setBase(p.base, session.last().scalp.get());
        engine->run(p.run, false);
        for (const Vec3f& q : engine->readPositions()) {
            ASSERT_TRUE(q.x >= b.lo.x && q.y >= b.lo.y && q.z >= b.lo.z && q.x <= b.hi.x && q.y <= b.hi.y && q.z <= b.hi.z);
        }
    }
}

TEST_P(LookEngineTest, ShapersAtAHundredThousandStrands) {
    // About 100k strands through each shaper (and Noise and Width after it), from the shaper: against
    // the CPU, and each one's GPU time printed (the median of 5 runs after one).
    const std::pair<std::string, std::string> nodes[] = {
        {"Bend", R"("params": {"angle": 50.0, "end": 0.7, "randomness": 0.3})"},
        {"Direction", R"("params": {"inclination": 60.0, "roll": 30.0, "rollRandom": 20.0, "along": "direction"})"},
        {"Transform", R"("params": {"rotate": [10.0, 30.0, 0.0], "scale": 0.9, "rotateRandom": [3.0, 15.0, 0.0], "scaleRandom": 0.1})"},
        {"Displace", R"("params": {"distance": 0.2})"},
        {"Twist", R"("params": {"turns": 1.5})"}};
    for (const auto& [type, params] : nodes) {
        SCOPED_TRACE(type);
        nlohmann::json g = nlohmann::json::parse(shaperGraph(type, params, type == "Twist"));
        for (auto& n : g["nodes"]) {
            if (n["name"] == "scatter") n["params"]["density"] = 500.0;
        }
        GroomSession session(groom(g.dump()));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        EXPECT_GT(session.last().strands->strandCount(), 90000u);
        const std::string node = shaperName(type == "Twist");
        expectClose(runAndCompare(session, {}, session, node));
        const Prepared p = prepare(session, {}, node);
        auto engine = backend_->engine();
        engine->setBase(p.base, session.last().scalp.get());
        engine->run(p.run, true);
        engine->times(true);
        std::vector<double> ms;
        for (int run = 0; run < 5; ++run) {
            engine->run(p.run, true);
            const gpu::GpuTimes t = engine->times(true);
            ms.push_back(t.bendMs + t.directionMs + t.transformMs + t.displaceMs + t.twistMs);
        }
        std::sort(ms.begin(), ms.end());
        std::printf("[ %-8s ] %s at %zu strands, %zu points: %.3f ms on the GPU (median of 5)\n", GetParam().c_str(), type.c_str(),
                    p.base->strandCount(), p.base->pointCount(), ms[2]);
    }
}

TEST(GpuPreview, ShapersAreTheGpusAndMirrorStartsARun) {
    // Bend, Direction, Transform, Displace and Twist are stages, their numbers, vectors, seeds and
    // profiles previewable; a direction per strand (an expression) and a Direction mapping all three
    // of its params are the CPU's, saying why. Mirror adds and drops strands (new IDs, roots found
    // again on the scalp): a run's first stage, its result the base (the nodes up its input a run of
    // their own: CullSplitAndMirrorRunADragUpTheirInputAsTheCpuDoes).
    std::string why;
    for (const char* type : {"Bend", "Direction", "Transform", "Displace", "Twist"}) {
        SCOPED_TRACE(type);
        GroomSession session(groom(shaperGraph(type, "", std::string(type) == "Twist")));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        gpu::LookChain chain;
        ASSERT_TRUE(gpu::findLookChain(*session.viewDocument(), &chain, &why)) << why;
        EXPECT_EQ(session.viewDocument()->nameOf(chain.base), "scatter");
        EXPECT_EQ(chain.stages.front().kind, gpu::StageKind::Interpolate);
    }
    EXPECT_TRUE(gpu::previewableParam("Bend", "angle") && gpu::previewableParam("Bend", "direction") && gpu::previewableParam("Bend", "seed"));
    EXPECT_FALSE(gpu::previewableParam("Bend", "along"));
    EXPECT_TRUE(gpu::previewableParam("Direction", "roll") && gpu::previewableParam("Direction", "direction"));
    EXPECT_TRUE(gpu::previewableParam("Transform", "pivot") && gpu::previewableParam("Transform", "scaleRandom"));
    EXPECT_FALSE(gpu::previewableParam("Transform", "space"));
    EXPECT_TRUE(gpu::previewableParam("Displace", "profile") && gpu::previewableParam("Displace", "distance"));
    EXPECT_TRUE(gpu::previewableParam("Twist", "turns") && gpu::previewableParam("Twist", "profile"));
    EXPECT_FALSE(gpu::previewableParam("Twist", "clumpChannel"));
    EXPECT_FALSE(gpu::previewableParam("Mirror", "offset"));
    const std::pair<std::string, std::string> refused[] = {
        {"Bend", R"("params": {"along": "direction", "direction": {"expression": "[x, -1, 0]"}})"},
        {"Displace", R"("params": {"along": "direction", "direction": {"expression": "[0, 1, z]"}})"},
        {"Direction", R"("params": {"inclination": {"expression": "40 * rand", "min": 0.0, "max": 1.0}, "roll": {"expression": "rand", "min": 0.0, "max": 1.0},
                                    "strength": {"expression": "rand", "min": 0.0, "max": 1.0}})"}};
    for (const auto& [type, params] : refused) {
        SCOPED_TRACE(type);
        GroomSession session(groom(shaperGraph(type, params)));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        Prepared p;
        EXPECT_FALSE(prepareRun(session, {}, "n0", &p));
        EXPECT_NE(p.why.find("n0"), std::string::npos) << p.why;
    }
    for (const char* mode : {"replace", "add"}) {
        SCOPED_TRACE(mode);
        GroomSession session(groom(shaperGraph("Mirror", std::string(R"("params": {"mode": ")") + mode + R"("})")));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        gpu::LookChain chain;
        ASSERT_TRUE(gpu::findLookChain(*session.viewDocument(), &chain, &why)) << why;
        EXPECT_EQ(session.viewDocument()->nameOf(chain.base), "n0");
        EXPECT_EQ(chain.stages.front().kind, gpu::StageKind::Mirror);
        ASSERT_GE(chain.stages.size(), 2u);
        EXPECT_EQ(chain.stages[1].kind, gpu::StageKind::Noise);
    }
}

// Tangle ---------------------------------------------------------------------------------------------

// Noise's params kinking the strands before a shaper (the GPU's Noise ahead of it in a run).
const std::string kKinks = R"("params": {"amplitude": 0.25, "frequency": 2.5})";

// The shaper `type` (its JSON past the type) after a Noise, then Gravity: its run from itself, from
// the Noise before it and from Interpolate (a dab's), and a dab on the guides, against the CPU's.
// `connect`: more connections (a RestoreLength's reference).
void expectNeighbourShaperRuns(LookEngineTest& test, const std::string& graph, std::size_t minMoved,
                      const std::function<Compared(GroomSession&, const std::string&)>& run) {
    GroomSession session(groom(graph));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    EXPECT_GT(movedBy(session, "n1", 1e-4f), minMoved);
    for (const char* from : {"n1", "n0", "interpolate"}) {
        SCOPED_TRACE(from);
        expectClose(run(session, from));
    }
    (void)test;
}
std::string neighbourGraph(const std::string& type, const std::string& params, bool gravity = true) {
    std::vector<std::pair<std::string, std::string>> nodes{{"Noise", kKinks}, {type, params}};
    if (gravity) nodes.push_back({"Gravity", R"("params": {"strength": 0.5, "stiffness": 0.5})"});
    return withoutClearance(chainGraph(nodes));
}
// `graph` with a RestoreLength's (node `name`) reference taken from node `from`.
std::string withReference(const std::string& graph, const std::string& name, const std::string& from) {
    nlohmann::json g = nlohmann::json::parse(graph);
    g["connections"].push_back({{"from", from}, {"to", name}, {"input", "reference"}});
    return g.dump();
}
#define EXPECT_NEIGHBOUR_SHAPER_RUNS(graphJson, minMoved)                                                                       \
    do {                                                                                                              \
        const std::string graph_ = (graphJson);                                                                       \
        expectNeighbourShaperRuns(*this, graph_, minMoved,                                                                     \
                         [&](GroomSession& s, const std::string& from) { return runAndCompare(s, {}, s, from); });    \
        GroomData data_ = groom(graph_);                                                                              \
        EXPECT_DAB_MATCHES(data_);                                                                                    \
    } while (0)

TEST_P(LookEngineTest, SmoothSmoothsAsTheCpuDoes) {
    // Smooth after kinks (Noise), then Gravity: small and large scales, a strength by a map per
    // strand and a profile, an affect mapping half the head; from itself, from the Noise, from
    // Interpolate, a dab; drags of its strength, scale and profile.
    const std::string params[] = {R"("params": {"strength": 0.8, "scale": 0.09})",
                                  R"("params": {"strength": {"expression": "0.3 + 0.7 * rand", "min": 0.0, "max": 1.0}, "scale": 0.4,
                                                "profile": [[0, 0], [0.5, 1], [1, 0.6]]})",
                                  R"("params": {"strength": 1.0, "scale": 1.0, "affect": {"expression": "x < 0 ? 1 : 0.35"}})",
                                  R"("params": {"strength": 0.7, "scale": 0.002})"};
    for (const std::string& p : params) {
        SCOPED_TRACE(p);
        EXPECT_NEIGHBOUR_SHAPER_RUNS(neighbourGraph("Smooth", p), 10000u);
    }
    const std::string graph = neighbourGraph("Smooth", params[0]);
    EXPECT_GPU_MATCHES(graph, (gpu::Overrides{{"n1", {{"strength", 0.4}}}}), withParam(graph, "n1", "strength", 0.4));
    EXPECT_GPU_MATCHES(graph, (gpu::Overrides{{"n1", {{"scale", 0.3}}}}), withParam(graph, "n1", "scale", 0.3));
    EXPECT_GPU_MATCHES(graph, (rampDrag("n1", "profile", "[[0, 1], [1, 0.2]]")),
                       withParam(graph, "n1", "profile", nlohmann::json::parse("[[0, 1], [1, 0.2]]")));
    // A Noise drag upstream: Smooth and Gravity after it on the GPU.
    EXPECT_GPU_MATCHES(graph, (gpu::Overrides{{"n0", {{"amplitude", 0.4}}}}), withParam(graph, "n0", "amplitude", 0.4));
}

TEST_P(LookEngineTest, RestoreLengthRestoresAsTheCpuDoes) {
    // RestoreLength after kinks (Noise, which stretches the strands), its reference Interpolate's
    // strands: segments and scale, and even (no reference read); from itself and from the Noise (the
    // reference as cached), from Interpolate (the reference the run's own Interpolate makes on the
    // GPU), a dab; masked by an affect; a Noise drag upstream.
    const std::string modes[] = {R"("params": {"mode": "segments"})", R"("params": {"mode": "scale"})", R"("params": {"mode": "even"})",
                                 R"("params": {"mode": "segments", "affect": {"expression": "y > 2 ? 1 : 0.5"}})"};
    for (const std::string& p : modes) {
        SCOPED_TRACE(p);
        const std::string graph = withReference(neighbourGraph("RestoreLength", p), "n1", "interpolate");
        EXPECT_NEIGHBOUR_SHAPER_RUNS(graph, 1000u);
        GroomSession session(groom(graph));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const Prepared all = prepare(session, {}, "interpolate");
        ASSERT_EQ(all.chain.stages.size(), 5u);
        EXPECT_EQ(all.chain.stages[2].kind, gpu::StageKind::RestoreLength);
        EXPECT_EQ(all.chain.stages[2].from2, 0u);  // (Interpolate's result, on the GPU)
        const Prepared own = prepare(session, {}, "n1");
        EXPECT_EQ(own.chain.stages[0].from2, gpu::kNoStage);  // (as cached)
        EXPECT_GPU_MATCHES(graph, (gpu::Overrides{{"n0", {{"amplitude", 0.4}}}}), withParam(graph, "n0", "amplitude", 0.4));
    }
    // Without a reference (segments, scale): handed on.
    GroomSession session(groom(neighbourGraph("RestoreLength", modes[0])));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    const Prepared p = prepare(session, {}, "n0");
    EXPECT_EQ(p.chain.stages.size(), 3u);  // Noise, Gravity, Width
    expectClose(runAndCompare(session, {}, session, "n0"));
}

TEST_P(LookEngineTest, BlendNeighborsBlendsAsTheCpuDoes) {
    // BlendNeighbors after kinks: its neighbours the CPU's (by root), their shapes the run's; its
    // strength by a map, a profile, preserveLength off, masked; from itself, from the Noise, from
    // Interpolate, a dab; drags of its strength and profile.
    const std::string params[] = {R"("params": {"strength": 0.8, "neighbors": 6})",
                                  R"("params": {"strength": {"expression": "0.2 + 0.8 * rand", "min": 0.0, "max": 1.0}, "neighbors": 3,
                                                "profile": [[0, 0], [1, 1]], "preserveLength": false})",
                                  R"("params": {"strength": 1.0, "affect": {"expression": "z < 0 ? 1 : 0"}})"};
    for (const std::string& p : params) {
        SCOPED_TRACE(p);
        EXPECT_NEIGHBOUR_SHAPER_RUNS(neighbourGraph("BlendNeighbors", p), 10000u);
    }
    const std::string graph = neighbourGraph("BlendNeighbors", params[0]);
    EXPECT_GPU_MATCHES(graph, (gpu::Overrides{{"n1", {{"strength", 0.3}}}}), withParam(graph, "n1", "strength", 0.3));
    EXPECT_GPU_MATCHES(graph, (rampDrag("n1", "profile", "[[0, 0.2], [1, 1]]")),
                       withParam(graph, "n1", "profile", nlohmann::json::parse("[[0, 0.2], [1, 1]]")));
    EXPECT_GPU_MATCHES(graph, (gpu::Overrides{{"n0", {{"amplitude", 0.4}}}}), withParam(graph, "n0", "amplitude", 0.4));
}

TEST_P(LookEngineTest, BlendNeighborsOfOtherPointCountsAsTheCpuDoes) {
    // Strands of different point counts (a Resample by length before): each neighbour resampled to
    // the strand's count on the GPU.
    LookGraph g;
    g.uneven = true;
    g.after = "BlendNeighbors";
    g.afterParams = R"("params": {"strength": 0.9})";
    GroomSession session(groom(g.json()));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    EXPECT_GT(movedBy(session, "after", 1e-4f), 10000u);
    expectPointsClose(runAndCompare(session, {}, session, "after"));
    expectClose(runAndCompare(session, {}, session, "noise"));
}

TEST_P(LookEngineTest, TangleSticksAsTheCpuDoes) {
    // Tangle after kinks: its partners the CPU's (by root and ID), the spans and pulls the GPU's; a
    // fraction by a map of the IDs, its strength by a map, masked; from itself, from the Noise, from
    // Interpolate, a dab; drags of its strength, start, span and reach.
    const std::string params[] = {R"("params": {"fraction": 0.6, "strength": 0.9, "radius": 0.4, "reach": 0.5})",
                                  R"("params": {"fraction": {"expression": "rand", "min": 0.0, "max": 1.0}, "strength":
                                                {"expression": "0.5 + 0.5 * rand", "min": 0.0, "max": 1.0}, "span": 0.6, "start": 0.1, "reach": 0.0})",
                                  R"("params": {"fraction": 0.8, "strength": 1.0, "reach": 1.0, "affect": {"expression": "x < 0 ? 0.6 : 1"}})"};
    for (const std::string& p : params) {
        SCOPED_TRACE(p);
        EXPECT_NEIGHBOUR_SHAPER_RUNS(neighbourGraph("Tangle", p), 5000u);
    }
    const std::string graph = neighbourGraph("Tangle", params[0]);
    const std::pair<const char*, double> drags[] = {{"strength", 0.5}, {"start", 0.5}, {"span", 0.2}, {"reach", 0.2}};
    for (const auto& [param, value] : drags) {
        SCOPED_TRACE(param);
        EXPECT_GPU_MATCHES(graph, (gpu::Overrides{{"n1", {{param, value}}}}), withParam(graph, "n1", param, value));
    }
    EXPECT_GPU_MATCHES(graph, (gpu::Overrides{{"n0", {{"amplitude", 0.4}}}}), withParam(graph, "n0", "amplitude", 0.4));
}

// The test's groom with flow arrows down the sides of its sphere (radius 4) and, with `sink`, a
// whorl at the crown.
// Flow's trace is not continuous everywhere: where the field's baked corners about cancel (it asks
// the field itself there), at a whorl's middle, where a strand's own segment stands about straight
// out of the skin (no way along it: it carries on), a rounding decides, and the strand goes on
// another way from there, on the CPU as on the GPU (its own result for its strands shaken by 1e-5,
// cpuShake: printed, up to most of a strand without arrows). So: within twice that, or 2e-2 (a
// strand or two of a groom passing such a place a rounding apart), the mean within twice the
// shake's or the usual, under 1% of the points more than 1e-4 apart, the frames as far as the points
// allow (expectGridShakenClose's), and no widths (a strand gone another way has its widths by its
// own lengths).
void expectFlowClose(const Compared& c, const gpu::PointError& shake) {
    ASSERT_GE(shake.max, 0.0);
    const double points = std::max(2e-2, 2.0 * shake.max), mean = std::max(kMeanTolerance, 2.0 * shake.mean);
    EXPECT_GT(c.positions.count, 1000u);
    EXPECT_LT(c.positions.max, points);
    EXPECT_LT(c.positions.mean, mean);
    EXPECT_LT(c.over, c.positions.count / 100);
    EXPECT_LT(c.normals.max, 10.0 * points);
    EXPECT_LT(c.tangents.max, 10.0 * points);
    EXPECT_LT(c.bitangents.max, std::max(kBitangentTolerance, 100.0 * points));
}

// Hair is grown over the upper part of the head alone (Scatter's density mapped by height): no field
// of arrows on a sphere is smooth everywhere, and below them, where their directions on the skin
// cancel (the arrows all point down to the bottom pole), which way a strand goes is a rounding's
// choice, on the CPU as on the GPU.
GroomData flowGroom(const std::string& graphJson, bool arrows = true, bool sink = true, double density = 60.0) {
    nlohmann::json upper = nlohmann::json::parse(R"({"expression": "y > -1 ? 1 : 0", "min": 0.0})");
    upper["max"] = density;
    GroomData d = groom(withParam(graphJson, "scatter", "density", upper));
    if (!arrows) return d;
    FlowSet set;
    for (int ring = 0; ring < 3; ++ring) {
        for (int k = 0; k < 12; ++k) {
            const float a = 6.2831853f * (static_cast<float>(k) + 0.5f * static_cast<float>(ring)) / 12.0f;
            const float y = 3.4f - 1.1f * static_cast<float>(ring), r = std::sqrt(16.0f - y * y);
            const Vec3f at{r * std::cos(a), y, r * std::sin(a)};
            set.pins.push_back(FlowSet::makePin(*d.scalp, d.scalp->closestPoint(at), Vec3f{-std::sin(a) * 0.4f, -1.0f, std::cos(a) * 0.4f}));
        }
    }
    if (sink) {
        FlowSink whorl;
        whorl.root = d.scalp->closestPoint({0.3f, 4.0f, 0.2f}).root;
        whorl.angle = 70.0f;
        whorl.radius = 1.5f;
        set.sinks.push_back(whorl);
    }
    d.flowJson = set.toJson();
    return d;
}

TEST_P(LookEngineTest, FlowLaysStrandsAsTheCpuDoes) {
    // Flow after Interpolate, then kinks (Noise): laid along arrows down the sides and a whorl at the
    // crown (the field baked at the vertices, the field itself where they disagree), along their own
    // way without arrows, its lift by a map, masked; from itself and from Interpolate, a dab; drags of
    // its lift, bend, rollRandom and seed.
    const std::string params[] = {R"("params": {"lift": 12.0, "bend": 0.6, "rollRandom": 6.0})",
                                  R"("params": {"lift": {"expression": "10 + 20 * rand", "min": 0.0, "max": 60.0}, "bend": 1.0, "rollRandom": 0.0})",
                                  R"("params": {"lift": 25.0, "bend": 0.3, "rollRandom": 10.0, "seed": 3, "affect": {"expression": "x < 0 ? 1 : 0.4"}})"};
    // (Within what a rounding moves the CPU's own trace: expectFlowClose.)
    for (const bool arrows : {true, false}) {
        for (const std::string& p : params) {
            SCOPED_TRACE(p + (arrows ? " (arrows)" : " (no arrows)"));
            const std::string graph = withoutClearance(chainGraph({{"Flow", p}, {"Noise", kKinks}}));
            const GroomData data = flowGroom(graph, arrows);
            GroomSession session(data);
            ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
            EXPECT_GT(movedBy(session, "n0", 1e-2f), 20000u);
            const gpu::PointError shake = cpuShake(session, "n0");
            printShake(GetParam(), "Flow", shake);
            expectFlowClose(runAndCompare(session, {}, session, "n0"), shake);
            expectFlowClose(runAndCompare(session, {}, session, "interpolate"), shake);
            GroomSession dabbed(data);
            ASSERT_TRUE(dabbed.evaluate(ViewRequest{}).strands);
            const StrandsPtr before = dabbed.last().strands;
            const GuidesPtr bent = guideSet(*data.scalp, 1.4f);
            dabbed.setGuides(bent);
            ViewRequest dab;
            dab.stopAt = "interpolate";
            ASSERT_EQ(dabbed.evaluate(dab).strands, before);
            GroomData after = data;
            after.guides = bent;
            GroomSession fresh(after);
            ASSERT_TRUE(fresh.evaluate(ViewRequest{}).strands);
            const gpu::PointError freshShake = cpuShake(fresh, "n0");
            expectFlowClose(runAndCompare(dabbed, {}, fresh, "interpolate"), freshShake);
        }
    }
    const std::string graph = withoutClearance(chainGraph({{"Flow", params[0]}, {"Noise", kKinks}}));
    const std::pair<const char*, double> drags[] = {{"lift", 30.0}, {"bend", 0.2}, {"rollRandom", 20.0}, {"seed", 7.0}};
    for (const auto& [param, value] : drags) {
        SCOPED_TRACE(param);
        const nlohmann::json stored = std::string(param) == "seed" ? nlohmann::json(7) : nlohmann::json(value);
        GroomSession dragged(flowGroom(graph));
        ASSERT_TRUE(dragged.evaluate(ViewRequest{}).strands);
        GroomSession committed(flowGroom(withParam(graph, "n0", param, stored)));
        ASSERT_TRUE(committed.evaluate(ViewRequest{}).strands);
        const gpu::PointError shake = cpuShake(committed, "n0");
        expectFlowClose(runAndCompare(dragged, gpu::Overrides{{"n0", {{param, value}}}}, committed), shake);
    }
}

TEST(GpuPreview, TheShapersChoicesAreTheCpus) {
    // Whom a strand reads is the CPU's last evaluation's: BlendNeighbors' neighbors, Tangle's
    // fraction, radius and seed are not previewed; a Tangle whose fraction reads the points a run moves
    // before it is the CPU's.
    EXPECT_TRUE(gpu::previewableParam("Smooth", "scale"));
    EXPECT_TRUE(gpu::previewableParam("Smooth", "profile"));
    EXPECT_TRUE(gpu::previewableParam("BlendNeighbors", "strength"));
    EXPECT_FALSE(gpu::previewableParam("BlendNeighbors", "neighbors"));
    EXPECT_TRUE(gpu::previewableParam("Tangle", "reach"));
    EXPECT_FALSE(gpu::previewableParam("Tangle", "fraction"));
    EXPECT_FALSE(gpu::previewableParam("Tangle", "radius"));
    EXPECT_FALSE(gpu::previewableParam("Tangle", "seed"));
    EXPECT_FALSE(gpu::previewableParam("RestoreLength", "mode"));
    EXPECT_TRUE(gpu::previewableParam("Flow", "lift"));
    EXPECT_TRUE(gpu::previewableParam("Flow", "seed"));
    {
        GroomSession session(groom(neighbourGraph("Tangle", R"("params": {"fraction": {"expression": "length > 3.2 ? 1 : 0", "min": 0.0, "max": 1.0}})")));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        Prepared own, upstream;
        EXPECT_TRUE(prepareRun(session, {}, "n1", &own)) << own.why;
        EXPECT_FALSE(prepareRun(session, {}, "n0", &upstream));
        EXPECT_NE(upstream.why.find("fraction"), std::string::npos) << upstream.why;
    }
}

TEST_P(LookEngineTest, TheShapersAtAHundredThousandStrands) {
    // About 100k strands: each shaper on the GPU against the CPU, and its time (the median of 5 runs
    // after one; noisy where something else uses the GPU).
    const std::pair<const char*, std::string> shapers[] = {{"Smooth", R"("params": {"strength": 0.8, "scale": 0.09})"},
                                                           {"BlendNeighbors", R"("params": {"strength": 0.8})"},
                                                           {"Tangle", R"("params": {"fraction": 0.6, "strength": 0.9})"},
                                                           {"RestoreLength", R"("params": {"mode": "even"})"},
                                                           {"Flow", R"("params": {"lift": 12.0, "bend": 0.6})"}};
    for (const auto& [type, params] : shapers) {
        SCOPED_TRACE(type);
        LookGraph g;
        g.scatter = R"("params": {"density": 500.0, "length": 3.0})";
        g.after = type;
        g.afterParams = params;
        GroomSession session(std::string(type) == "Flow" ? flowGroom(g.json(), true, true, 800.0) : groom(g.json()));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        EXPECT_GT(session.last().strands->strandCount(), 90000u);
        if (std::string(type) == "Flow") {
            expectFlowClose(runAndCompare(session, {}, session, "after"), cpuShake(session, "after"));  // (FlowLaysStrandsAsTheCpuDoes)
        } else {
            expectPointsClose(runAndCompare(session, {}, session, "after"));
        }
        const Prepared p = prepare(session, {}, "after");
        auto engine = backend_->engine();
        engine->setBase(p.base, session.last().scalp.get());
        engine->run(p.run, true);
        engine->times(true);
        std::vector<double> ms;
        for (int run = 0; run < 5; ++run) {
            engine->run(p.run, true);
            const gpu::GpuTimes t = engine->times(true);
            ms.push_back(t.smoothMs + t.blendNeighborsMs + t.tangleMs + t.restoreLengthMs + t.flowMs);
        }
        std::sort(ms.begin(), ms.end());
        std::printf("[ %-8s ] %s at %zu strands, %zu points: %.3f ms on the GPU (median of 5)\n", GetParam().c_str(), type,
                    p.base->strandCount(), p.base->pointCount(), ms[2]);
    }
}

// ---- Affect masks --------------------------------------------------------------------------------

// A Group of the strands about the crown (its channel "crown", soft at the edge).
const char* const kCrownGroup = R"("params": {"name": "crown", "center": [0.0, 4.0, 0.0], "radius": 2.5, "falloff": 1.0})";

TEST_P(LookEngineTest, MaskedStagesMixAsTheCpuDoes) {
    // Stages restricted by their affect run on the GPU, each result mixed back toward what the node
    // took (look.slang AffectCS) as the graph mixes it: Noise masked by a Group (an attribute map of
    // its strand channel), Bend per point by a map along the strand, Curl by a number, Noise by the
    // strand's length (made on the GPU from the run's points), Gravity per strand, a Wet's levels
    // (mixed after the last), a Width's widths (after a Width: those it took kept for the mix). From
    // the masked node, from Interpolate where the run can start there, then a dab.
    struct Case {
        const char* name;
        std::string graph;
        bool fromInterpolate;
    };
    const std::string curl = R"("params": {"radius": 0.1, "frequency": 1.0, "affect": 0.4})";
    const Case cases[] = {
        {"Noise masked by a Group",
         withoutClearance(chainGraph({{"Group", kCrownGroup}, {"Noise", R"("params": {"amplitude": 0.4, "affect": {"attribute": "crown"}})"}})), false},
        {"Bend masked per point", withoutClearance(chainGraph({{"Bend", R"("params": {"angle": 60.0, "affect": {"expression": "x > 0 ? pt : 0.4"}})"}})),
         true},
        {"Curl masked by a number", withoutClearance(chainGraph({{"Curl", curl}})), true},
        {"Noise masked by the strands' lengths",
         withoutClearance(chainGraph({{"Noise", R"("params": {"amplitude": 0.4, "affect": {"expression": "length / 4"}})"}})), true},
        {"Gravity masked per strand",
         withoutClearance(chainGraph({{"Gravity", R"("params": {"strength": 0.8, "affect": {"expression": "z > 0 ? 1 : 0.2"}})"}})), true},
        {"Wet masked", withoutClearance(chainGraph({{"Wet", R"("params": {"levels": 3, "wetness": 0.6, "clumpSize": 2.0, "affect": {"expression": "x > 0 ? 1 : 0"}})"}})),
         true},
        {"Clump masked", withoutClearance(chainGraph({{"Clump", R"("params": {"strength": 0.8, "count": 60, "affect": {"expression": "y > 2 ? 1 : 0.3"}})"}})),
         true},
        {"Wind masked",
         withoutClearance(chainGraph({{"Wind", R"("params": {"strength": 0.6, "direction": [1.0, 0.0, 0.2], "affect": {"expression": "z > 0 ? 1 : 0.5"}})"}})),
         true},
        {"Width masked", withParam(withoutClearance(chainGraph({{"Width", R"("params": {"width": 0.02})"}})), "width", "affect",
                                   nlohmann::json::parse(R"({"expression": "x > 0 ? 1 : 0.25"})")),
         true},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        GroomData data = groom(c.graph);
        GroomSession session(data);
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const std::string masked = std::string(c.name) == "Width masked" ? "width" : c.name == std::string("Noise masked by a Group") ? "n1" : "n0";
        const Prepared p = prepare(session, {}, masked);
        ASSERT_FALSE(p.run.params.stages.empty());
        EXPECT_TRUE(p.run.params.stages.front().masked);
        expectClose(runAndCompare(session, {}, session, masked));
        if (!c.fromInterpolate) continue;
        expectClose(runAndCompare(session, {}, session, "interpolate"));
        EXPECT_DAB_MATCHES(data);
    }
}

TEST_P(LookEngineTest, MaskedStageDragsGiveWhatTheCpuMixes) {
    // Drags of a masked node's params and of its affect (a number, or 1 of its own dragged under it),
    // against a CPU evaluation of the graph holding the value.
    const std::string group = withoutClearance(chainGraph({{"Group", kCrownGroup}, {"Noise", R"("params": {"amplitude": 0.4, "affect": {"attribute": "crown"}})"}}));
    EXPECT_GPU_MATCHES(group, (gpu::Overrides{{"n1", {{"amplitude", 0.8}}}}), withParam(group, "n1", "amplitude", 0.8));
    EXPECT_GPU_MATCHES(group, (gpu::Overrides{{"n1", {{"seed", 7.0}}}}), withParam(group, "n1", "seed", 7));
    const std::string bend = withoutClearance(chainGraph({{"Bend", R"("params": {"angle": 60.0, "affect": {"expression": "x > 0 ? pt : 0.4"}})"}}));
    EXPECT_GPU_MATCHES(bend, (gpu::Overrides{{"n0", {{"angle", -30.0}}}}), withParam(bend, "n0", "angle", -30.0));
    const std::string curl = withoutClearance(chainGraph({{"Curl", R"("params": {"radius": 0.1, "frequency": 1.0, "affect": 0.4})"}}));
    EXPECT_GPU_MATCHES(curl, (gpu::Overrides{{"n0", {{"affect", 0.7}}}}), withParam(curl, "n0", "affect", 0.7));
    EXPECT_GPU_MATCHES(curl, (gpu::Overrides{{"n0", {{"radius", 0.2}}}}), withParam(curl, "n0", "radius", 0.2));
    const std::string whole = withoutClearance(chainGraph({{"Noise", R"("params": {"amplitude": 0.4})"}}));
    EXPECT_GPU_MATCHES(whole, (gpu::Overrides{{"n0", {{"affect", 0.3}}}}), withParam(whole, "n0", "affect", 0.3));
    EXPECT_GPU_MATCHES(whole, (gpu::Overrides{{"n0", {{"affect", 0.0}}}}), withParam(whole, "n0", "affect", 0.0));
}

TEST_P(LookEngineTest, AMaskedStageAtAHundredThousandStrands) {
    // About 100k strands through Noise masked by a Group (and Width after it), from the Noise:
    // against the CPU, and its GPU time masked and not (the median of 5 runs after one).
    for (const bool masked : {false, true}) {
        nlohmann::json g = nlohmann::json::parse(
            chainGraph({{"Group", kCrownGroup}, {"Noise", masked ? R"("params": {"amplitude": 0.4, "affect": {"attribute": "crown"}})" : R"("params": {"amplitude": 0.4})"}}));
        for (auto& n : g["nodes"]) {
            if (n["name"] == "scatter") n["params"]["density"] = 500.0;
        }
        GroomSession session(groom(withoutClearance(g.dump())));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        EXPECT_GT(session.last().strands->strandCount(), 90000u);
        expectClose(runAndCompare(session, {}, session, "n1"));
        const Prepared p = prepare(session, {}, "n1");
        auto engine = backend_->engine();
        engine->setBase(p.base, session.last().scalp.get());
        engine->run(p.run, true);
        engine->times(true);
        std::vector<double> ms;
        for (int run = 0; run < 5; ++run) {
            engine->run(p.run, true);
            ms.push_back(engine->times(true).noiseMs);
        }
        std::sort(ms.begin(), ms.end());
        std::printf("[ %-8s ] Noise%s at %zu strands, %zu points: %.3f ms on the GPU (median of 5)\n", GetParam().c_str(),
                    masked ? " masked by a Group" : "", p.base->strandCount(), p.base->pointCount(), ms[2]);
    }
}

// ---- Jiggle --------------------------------------------------------------------------------------

// The test's head at frame f: turned about y by up to 0.3 radians and moved along x by up to 2,
// eased over frames 0 to 8, still after.
Vec3f movedPoint(const Vec3f& q, int f) {
    const float t = std::clamp(static_cast<float>(f) / 8.0f, 0.0f, 1.0f), s = t * t * (3.0f - 2.0f * t);
    const float a = 0.3f * s, c = std::cos(a), sn = std::sin(a);
    return Vec3f(c * q.x + sn * q.z, q.y, -sn * q.x + c * q.z) + Vec3f(2.0f * s, 0.0f, 0.0f);
}
// A groom of `graphJson` at frame `frame` of that motion, its scalp's last `frames` frames its
// history (a frame apart, at 24 a second), the guides moved with the head.
GroomData jiggleGroom(const std::string& graphJson, int frame, int frames = 12) {
    GroomData d;
    d.graphJson = graphJson;
    const MeshPtr rest = test::makeSphere(24, 48, 4.0f);
    auto at = [&](int f) {
        std::vector<Vec3f> pts = rest->points();
        for (Vec3f& q : pts) q = movedPoint(q, f);
        return pts;
    };
    d.scalp = std::make_shared<ScalpMesh>(at(frame), rest->triangles());
    auto guides = std::make_shared<GuideSet>();
    const GuidesPtr still = guideSet(*rest, 0.6f);
    for (const Guide& g : still->guides()) {
        std::vector<Vec3f> pts = g.points;
        for (Vec3f& q : pts) q = movedPoint(q, frame);
        guides->add(g.id, std::move(pts));
    }
    guides->rootOnMesh(*d.scalp);
    d.guides = guides;
    std::vector<std::vector<Vec3f>> past;
    for (int k = 1; k <= frames; ++k) past.push_back(at(frame - k));
    d.scalpHistory = ScalpHistory::make(1.0 / 24.0, std::move(past));
    return d;
}
std::string jiggleGraph(const std::string& params = R"("params": {"strength": 1.0, "frequency": 2.0, "damping": 0.3})") {
    return withoutClearance(chainGraph({{"Jiggle", params}}));
}
#define EXPECT_GPU_JIGGLES(graphJson, frame, overrides, committedJson)                  \
    do {                                                                                \
        GroomSession dragged(jiggleGroom(graphJson, frame));                            \
        ASSERT_TRUE(dragged.evaluate(ViewRequest{}).strands);                           \
        GroomSession committed(jiggleGroom(committedJson, frame));                      \
        ASSERT_TRUE(committed.evaluate(ViewRequest{}).strands);                         \
        expectClose(runAndCompare(dragged, overrides, committed));                      \
    } while (0)

TEST_P(LookEngineTest, JiggleSwingsAsTheCpuDoes) {
    // Jiggle on a head turning and moving (then still): its strands trailing mid-move (frame 4),
    // overshooting after the stop (9), settling (13), each strand's lag the CPU's (its root's face's
    // vertices' and turn's), turned about its root on the GPU: from Jiggle and from Interpolate, its
    // strength a number and by a map per strand, and masked.
    const std::string graphs[] = {jiggleGraph(),
                                  jiggleGraph(R"("params": {"strength": {"expression": "x > 2 ? 1.5 : 0.5"}, "frequency": 3.0, "damping": 0.1})"),
                                  jiggleGraph(R"("params": {"frequency": 1.5, "affect": {"expression": "z > 0 ? 1 : 0.3"}})")};
    for (const std::string& graph : graphs) {
        for (const int frame : {4, 9, 13}) {
            SCOPED_TRACE("frame " + std::to_string(frame) + ": " + graph.substr(graph.find("Jiggle"), 120));
            GroomSession session(jiggleGroom(graph, frame));
            ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
            EXPECT_GT(movedBy(session, "n0", 1e-2f), 1000u);
            const Prepared p = prepare(session, {}, "interpolate");
            ASSERT_EQ(p.chain.stages.size(), 3u);
            EXPECT_EQ(p.chain.stages[1].kind, gpu::StageKind::Jiggle);
            expectClose(runAndCompare(session, {}, session, "n0"));
            expectClose(runAndCompare(session, {}, session, "interpolate"));
        }
    }
    // A still head (no motion in its history): the strands handed on as they came.
    GroomSession still(jiggleGroom(jiggleGraph(), 30));
    ASSERT_TRUE(still.evaluate(ViewRequest{}).strands);
    const Prepared p = prepare(still, {}, "interpolate");
    EXPECT_EQ(p.chain.node(gpu::StageKind::Jiggle), 0u);
}

TEST_P(LookEngineTest, JiggleDragsGiveWhatTheCpuSwings) {
    // Drags of its strength, frequency, damping and (fewer frames) history, the lags made again for
    // them; a history past the frames sampled for it is the CPU's.
    const std::string graph = jiggleGraph();
    const std::pair<const char*, double> drags[] = {{"strength", 1.7}, {"frequency", 3.5}, {"damping", 0.9}, {"history", 6.0}};
    for (const int frame : {5, 10}) {
        for (const auto& [param, value] : drags) {
            SCOPED_TRACE(std::string(param) + " at frame " + std::to_string(frame));
            const nlohmann::json stored = std::string(param) == "history" ? nlohmann::json(static_cast<int>(value)) : nlohmann::json(value);
            const std::string release = withParam(graph, "n0", param, stored);
            EXPECT_GPU_JIGGLES(graph, frame, (gpu::Overrides{{"n0", {{param, value}}}}), release);
        }
    }
    GroomSession session(jiggleGroom(graph, 6));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    Prepared refused;
    EXPECT_FALSE(prepareRun(session, gpu::Overrides{{"n0", {{"history", 20.0}}}}, "", &refused));
    EXPECT_NE(refused.why.find("history"), std::string::npos) << refused.why;
}

TEST_P(LookEngineTest, JiggleAtAHundredThousandStrands) {
    // About 100k strands through Jiggle (and Width after it), from Jiggle: against the CPU, and its
    // GPU time (the median of 5 runs after one).
    nlohmann::json g = nlohmann::json::parse(jiggleGraph());
    for (auto& n : g["nodes"]) {
        if (n["name"] == "scatter") n["params"]["density"] = 500.0;
    }
    GroomSession session(jiggleGroom(g.dump(), 6));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    EXPECT_GT(session.last().strands->strandCount(), 90000u);
    expectClose(runAndCompare(session, {}, session, "n0"));
    const Prepared p = prepare(session, {}, "n0");
    auto engine = backend_->engine();
    engine->setBase(p.base, session.last().scalp.get());
    engine->run(p.run, true);
    engine->times(true);
    std::vector<double> ms;
    for (int run = 0; run < 5; ++run) {
        engine->run(p.run, true);
        ms.push_back(engine->times(true).jiggleMs);
    }
    std::sort(ms.begin(), ms.end());
    std::printf("[ %-8s ] Jiggle at %zu strands, %zu points: %.3f ms on the GPU (median of 5)\n", GetParam().c_str(), p.base->strandCount(),
                p.base->pointCount(), ms[2]);
}

// ---- MeshCut -------------------------------------------------------------------------------------

// A MeshCut after Interpolate (node n0) cutting by the ColliderMesh "ball" (GroomData::meshes "collider").
std::string meshCutGraph(const std::string& params) {
    nlohmann::json g = nlohmann::json::parse(withoutClearance(chainGraph({{"MeshCut", params}}, true, true)));
    g["connections"].push_back({{"from", "ball"}, {"to", "n0"}, {"input", "mesh"}});
    return g.dump();
}
// A groom whose collider is a sphere of `radius` about `center`.
GroomData meshCutGroom(const std::string& graphJson, const Vec3f& center, float radius) {
    GroomData data = groom(graphJson);
    const MeshPtr sphere = test::makeSphere(16, 32, radius);
    std::vector<Vec3f> points = sphere->points();
    for (Vec3f& q : points) q = q + center;
    data.meshes["collider"] = std::make_shared<ScalpMesh>(points, sphere->triangles());
    return data;
}

TEST_P(LookEngineTest, MeshCutCutsAsTheCpuDoes) {
    // MeshCut keeps each strand's point count (the kept part spread over its points again), so it
    // runs on the GPU: each segment a ray down the mesh's tree, the root's side by its closest point.
    // An obstacle over the crown (keep outside), one holding some roots (those cut to nothing), a hull
    // round the head (keep inside); with an offset; from MeshCut and from Interpolate, then a dab;
    // drags of the offset.
    struct Case {
        const char* name;
        std::string params;
        Vec3f center;
        float radius;
    };
    const Case cases[] = {
        {"an obstacle", R"("params": {"keep": "outside"})", Vec3f(0.5f, 6.0f, 0.0f), 1.5f},
        {"an obstacle holding roots", R"("params": {"keep": "outside", "offset": 0.2})", Vec3f(0.0f, 4.0f, 0.0f), 2.5f},
        {"a hull", R"("params": {"keep": "inside", "offset": 0.3})", Vec3f(0.3f, 0.5f, 0.0f), 6.0f},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        const std::string graph = meshCutGraph(c.params);
        GroomData data = meshCutGroom(graph, c.center, c.radius);
        GroomSession session(data);
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const std::size_t moved = movedBy(session, "n0", 1e-2f);
        std::printf("[ %-8s ] MeshCut by %s: %zu points cut\n", GetParam().c_str(), c.name, moved);
        EXPECT_GT(moved, 500u);
        const Prepared p = prepare(session, {}, "interpolate");
        ASSERT_EQ(p.chain.stages.size(), 3u);
        EXPECT_EQ(p.chain.stages[1].kind, gpu::StageKind::MeshCut);
        expectClose(runAndCompare(session, {}, session, "n0"));
        expectClose(runAndCompare(session, {}, session, "interpolate"));
        EXPECT_DAB_MATCHES(data);
        GroomSession dragged(data);
        ASSERT_TRUE(dragged.evaluate(ViewRequest{}).strands);
        GroomSession committed(meshCutGroom(withParam(graph, "n0", "offset", 0.6), c.center, c.radius));
        ASSERT_TRUE(committed.evaluate(ViewRequest{}).strands);
        expectClose(runAndCompare(dragged, gpu::Overrides{{"n0", {{"offset", 0.6}}}}, committed));
    }
    // Nothing on its mesh input: the strands handed on as they came.
    GroomSession bare(groom(withoutClearance(chainGraph({{"MeshCut", R"("params": {})"}}))));
    ASSERT_TRUE(bare.evaluate(ViewRequest{}).strands);
    const Prepared p = prepare(bare, {}, "interpolate");
    EXPECT_EQ(p.chain.node(gpu::StageKind::MeshCut), 0u);
}

TEST_P(LookEngineTest, MeshCutAtAHundredThousandStrands) {
    // About 100k strands cut by an obstacle over the crown, from MeshCut: against the CPU, and its
    // GPU time (the median of 5 runs after one).
    nlohmann::json g = nlohmann::json::parse(meshCutGraph(R"("params": {"keep": "outside", "offset": 0.1})"));
    for (auto& n : g["nodes"]) {
        if (n["name"] == "scatter") n["params"]["density"] = 500.0;
    }
    GroomSession session(meshCutGroom(g.dump(), Vec3f(0.5f, 6.0f, 0.0f), 1.5f));
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    EXPECT_GT(session.last().strands->strandCount(), 90000u);
    expectClose(runAndCompare(session, {}, session, "n0"));
    const Prepared p = prepare(session, {}, "n0");
    auto engine = backend_->engine();
    engine->setBase(p.base, session.last().scalp.get());
    engine->run(p.run, true);
    engine->times(true);
    std::vector<double> ms;
    for (int run = 0; run < 5; ++run) {
        engine->run(p.run, true);
        ms.push_back(engine->times(true).meshCutMs);
    }
    std::sort(ms.begin(), ms.end());
    std::printf("[ %-8s ] MeshCut at %zu strands, %zu points: %.3f ms on the GPU (median of 5)\n", GetParam().c_str(), p.base->strandCount(),
                p.base->pointCount(), ms[2]);
}

// A Cull, Split or Mirror after Interpolate, a Noise and a Width, a Noise after it (taking a Split's
// output `output`).
std::string gatherGraph(const std::string& type, const std::string& params, const std::string& output = "") {
    return R"({"nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "scatter", "type": "Scatter", "params": {"density": 40.0, "length": 3.0}},
        {"name": "guides", "type": "Guides"}, {"name": "interpolate", "type": "Interpolate", "params": {"keepClearance": false}},
        {"name": "noise", "type": "Noise", "params": {"amplitude": 0.3, "frequency": 1.0}},
        {"name": "width", "type": "Width", "params": {"width": 0.01, "randomness": 0.2}},
        {"name": "picked", "type": ")" + type + R"(", "params": )" + params + R"(},
        {"name": "after", "type": "Noise", "params": {"amplitude": 0.1, "seed": 9}},
        {"name": "output", "type": "Output"}],
      "connections": [
        {"from": "scalp", "to": "scatter", "input": "scalp"}, {"from": "scatter", "to": "interpolate", "input": "strands"},
        {"from": "guides", "to": "interpolate", "input": "guides"}, {"from": "scalp", "to": "interpolate", "input": "scalp"},
        {"from": "interpolate", "to": "noise", "input": "strands"}, {"from": "noise", "to": "width", "input": "strands"},
        {"from": "width", "to": "picked", "input": "strands"},
        {"from": "picked", "to": "after", "input": "strands")" + (output.empty() ? "" : R"(, "output": ")" + output + "\"") + R"(},
        {"from": "after", "to": "output", "input": "strands"}],
      "output": "output"})";
}

TEST_P(LookEngineTest, CullSplitAndMirrorRunADragUpTheirInputAsTheCpuDoes) {
    // A drag of a node before a Cull, Split or Mirror (the Noise, the Width): the GPU runs from it,
    // the nodes up its input a run of their own on a sibling, their strands laid out as its last
    // evaluation laid them out (kept, or mirrored and moved onto the scalp), then the Noise after it:
    // as the CPU evaluates the dragged value. A drag after it: from there, as before.
    struct Case {
        const char* name;
        const char* type;
        const char* params;
        const char* output = "";  // a Split's output the Noise after it takes
    };
    const Case cases[] = {{"Cull", "Cull", R"({"keep": 0.6, "seed": 3})"},
                          {"Split a", "Split", R"({"weightA": 1.0, "weightB": 1.0})"},
                          {"Split b", "Split", R"({"weightA": 1.0, "weightB": 1.0, "weightC": 1.0})", "b"},
                          {"Split d", "Split", R"({"weightA": 1.0, "weightB": 1.0, "weightD": 1.0})", "d"},
                          {"Mirror (replace)", "Mirror", R"({"axis": "x", "mode": "replace"})"},
                          {"Mirror (add)", "Mirror", R"({"axis": "x", "mode": "add"})"}};
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        const std::string graph = gatherGraph(c.type, c.params, c.output);
        GroomSession session(groom(graph));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const GraphDocument& doc = *session.viewDocument();
        const NodeId picked = doc.nodesByName.at("picked");
        const StrandsPtr in = valueAs<StrandsPtr>(doc.graph.cachedValue(doc.nodesByName.at("width")));
        const std::size_t port = *c.output ? doc.graph.outputIndex(picked, c.output) : 0;
        const StrandsPtr made = valueAs<StrandsPtr>(doc.graph.cachedValue(picked, port));
        ASSERT_TRUE(in && made);
        EXPECT_NE(in->strandCount(), made->strandCount()) << "the test's premise: it keeps or adds strands";
        gpu::LookChain chain;
        std::string why;
        ASSERT_TRUE(gpu::findLookChain(doc, &chain, &why)) << why;
        ASSERT_EQ(chain.stages.size(), 2u);
        EXPECT_EQ(chain.stages.front().node, picked);
        EXPECT_TRUE(gpu::gathers(chain.stages.front().kind));
        EXPECT_EQ(chain.base, picked) << "its result the base";
        EXPECT_EQ(chain.baseOutput, port);
        EXPECT_EQ(chain.stages[1].from, 0u) << "the Noise after it takes its result";
        for (const auto& [node, value] : {std::pair<const char*, double>{"noise", 0.55}, {"width", 0.016}}) {
            SCOPED_TRACE(node);
            const std::string param = std::string(node) == "noise" ? "amplitude" : "width";
            const gpu::Overrides drag{{node, {{param, value}}}};
            Prepared p;
            ASSERT_TRUE(prepareRun(session, drag, "", &p)) << "the GPU previews it: " << p.why;
            EXPECT_EQ(p.chain.stages.front().node, picked);
            ASSERT_TRUE(p.run.params.stages.front().branch) << "the run up its input";
            ASSERT_TRUE(p.run.inputs.front().merged && p.run.inputs.front().gather);
            EXPECT_EQ(doc.nameOf(p.run.params.stages.front().branch->chain.stages.front().node), node);
            GroomSession committed(groom(withParam(graph, node, param, value)));
            ASSERT_TRUE(committed.evaluate(ViewRequest{}).strands);
            expectClose(runAndCompare(session, drag, committed));
        }
        {  // after it: its result as cached
            GroomSession committed(groom(withParam(graph, "after", "amplitude", 0.25)));
            ASSERT_TRUE(committed.evaluate(ViewRequest{}).strands);
            const gpu::Overrides drag{{"after", {{"amplitude", 0.25}}}};
            const Prepared p = prepare(session, drag);
            EXPECT_EQ(doc.nameOf(p.chain.stages.front().node), "after");
            expectPointsClose(runAndCompare(session, drag, committed));
        }
        {  // a preview holding hidden core strands (preview_core.h): the run up its input holds them too
            ViewRequest half;
            half.density = 0.5f;
            GroomSession thinned(groom(graph));
            ASSERT_TRUE(thinned.evaluate(half).strands);
            GroomSession thinnedCommitted(groom(withParam(graph, "noise", "amplitude", 0.55)));
            ASSERT_TRUE(thinnedCommitted.evaluate(half).strands);
            expectClose(runAndCompare(thinned, {{"noise", {{"amplitude", 0.55}}}}, thinnedCommitted));
        }
    }
    // A Cull keeping by an expression the run could change: the CPU's (the run ends after it).
    {
        nlohmann::json g = nlohmann::json::parse(gatherGraph("Cull", R"({"seed": 3})"));
        for (auto& n : g["nodes"]) {
            if (n["name"] == "picked") n["params"]["keep"] = {{"expression", "0.2 * length"}, {"min", 0.0}, {"max", 1.0}};
        }
        GroomSession session(groom(g.dump()));
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const GraphDocument& doc = *session.viewDocument();
        gpu::LookChain chain;
        std::string why;
        ASSERT_TRUE(gpu::findLookChain(doc, &chain, &why)) << why;
        EXPECT_EQ(chain.indexOf(doc.nodesByName.at("picked")), gpu::kNoStage);
        EXPECT_EQ(chain.base, doc.nodesByName.at("picked"));
        EXPECT_EQ(gpu::firstOverridden(doc, chain, {{"noise", {{"amplitude", 0.55}}}}), gpu::kNoStage);
    }
}

TEST_P(LookEngineTest, ASculptStrokeThroughACullRunsOnTheGpu) {
    // A comb on a Sculpt node's guides with a Cull after the look: the stroke armed from the look's
    // Interpolate (the run up the Cull's input, its stopAt), the Cull laying its strands out as it
    // kept them: as the CPU evaluates each dab.
    nlohmann::json g = nlohmann::json::parse(withoutClearance(sculptGraph()));
    for (auto& c : g["connections"]) {
        if (c["to"] == "output") c["to"] = "thin";
    }
    g["nodes"].push_back({{"name", "thin"}, {"type", "Cull"}, {"params", {{"keep", 0.6}, {"seed", 2}}}});
    g["connections"].push_back({{"from", "thin"}, {"to", "output"}, {"input", "strands"}});
    GroomSession session(groom(g.dump()));
    SessionStrokeHost host(session);
    session.setSculptTarget("sculpt");
    ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
    const SculptInput in = session.sculptInput("sculpt");
    const Guide* top = &in.guides->guides()[0];
    for (const Guide& guide : in.guides->guides()) {
        if (guide.points[0].y > top->points[0].y) top = &guide;
    }
    const ToolView view = ToolView::fromMatrix(test::lookAtProjection({0.0f, 14.0f, 10.0f}, {0.0f, 4.0f, 0.0f}, 40.0, 1.0), 800, 800);
    float cx = 0.0f, cy = 0.0f;
    ASSERT_TRUE(view.project(top->points[top->points.size() / 2], cx, cy));
    std::vector<std::array<float, 2>> line;
    for (int k = 0; k < 10; ++k) line.push_back({cx - 90.0f + 20.0f * static_cast<float>(k), cy + 4.0f * static_cast<float>(k)});
    BrushTool tool;
    tool.settings.setMode("comb");
    tool.settings.screenRadius = 90.0f;
    tool.settings.brush.strength = 1.0f;
    gpu::LookPreview preview;
    const gpu::LookPreview::Shown shown = [&] { return session.last().strands; };
    auto engine = backend_->engine();
    bool armed = false;
    int gpuDabs = 0;
    double worst = 0.0, moved = 0.0;
    ASSERT_TRUE(tool.stroke(host, view, line, 1.0f, 0, [&] {
        if (!armed) {
            ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
            std::string why;
            armed = preview.armStroke(session, shown, &why);
            ASSERT_TRUE(armed) << why;
            EXPECT_EQ(preview.stopAt(), "interpolate");
            return;
        }
        const StrandsPtr before = session.last().strands;
        ViewRequest dab;
        dab.stopAt = preview.stopAt();
        EXPECT_EQ(session.evaluate(dab).strands, before);
        std::string why;
        ASSERT_TRUE(preview.strokeHolds(session, shown, &why)) << why;
        const gpu::LookPlan& plan = *preview.strokePlan();
        EXPECT_EQ(plan.chain.stages.front().kind, gpu::StageKind::Cull);
        ASSERT_TRUE(plan.run.params.stages.front().branch);
        preview.upload(*engine, plan, session.last().scalp.get());
        engine->run(plan.run, true);
        ++gpuDabs;
        const std::vector<Vec3f> got = engine->readPositions();
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        worst = std::max(worst, gpu::compareVectors(got, session.last().strands->positions()).max);
        moved = std::max(moved, gpu::compareVectors(before->positions(), session.last().strands->positions()).max);
    }));
    preview.endStroke();
    EXPECT_GE(gpuDabs, 4);
    EXPECT_LT(worst, kPointTolerance);
    EXPECT_GT(moved, 0.05) << "the dabs reshaped the hair";
}


TEST(CpuEngine, BitsAgainstTheOperatorsStageByStage) {
    // The CPU engine (look.slang's kernels as C++) run stage by stage from each chain's first node,
    // each stage's points against its node's cached result: how many floats differ in their bits
    // (none: the CPU operators and the kernels give the same bits, the kernels' transcendentals the
    // core's portable ones, cmake/portable_cpu_kernels.cmake). The operators run their own C++ (the
    // engine off for them: else those switched to it make what they are compared against).
    gpu::setCpuEngineOn(false);
    struct EngineBack {
        ~EngineBack() { gpu::setCpuEngineOn(true); }
    } engineBack;
    struct Graph {
        std::string name, json, from;
        std::function<GroomData(const std::string&)> make = nullptr;  // (null: groom)
    };
    std::vector<Graph> graphs = {{"noise curl width", graph(0.3), "noise"}, {"hair_basics", startingGraph("hairBasics"), ""},
                                 {"default", startingGraph("default"), ""}};
    for (const char* preset : {"curly", "parted_style", "gravity", "flyaways", "cinematic_hair", "everyday_sim", "wind_and_jiggle"}) {
        graphs.push_back({preset, presetGraph(preset), ""});
    }
    // The stages the presets above leave out: the shapers, the neighbours' shapers, the hair grids,
    // Tie, Wet, Blend, Jiggle, MeshCut.
    const std::pair<const char*, const char*> shapers[] = {
        {"Bend", R"("params": {"angle": 35.0, "randomness": 0.3})"},
        {"Direction", R"("params": {"inclination": 30.0, "strength": 0.7, "roll": 20.0})"},
        {"Transform", R"("params": {"scale": 1.2, "translate": [0.0, 0.05, 0.0], "rotate": [10.0, 0.0, 5.0]})"},
        {"Displace", R"("params": {"distance": 0.1})"},
        {"Twist", R"("params": {"turns": 0.6})"}};
    for (const auto& [type, params] : shapers) graphs.push_back({type, shaperGraph(type, params, true), ""});
    for (const auto& [type, params] : {std::pair<const char*, const char*>{"Smooth", R"("params": {"strength": 0.8, "scale": 0.3})"},
                                       {"BlendNeighbors", R"("params": {"strength": 0.6})"},
                                       {"Tangle", R"("params": {"strength": 0.6, "fraction": 0.5})"}}) {
        graphs.push_back({type, neighbourGraph(type, params), ""});
    }
    graphs.push_back({"Collide", withoutClearance(chainGraph({{"Collide", R"("params": {"offset": 0.12, "softness": 0.1})"}}, true, true)), ""});
    graphs.push_back({"SelfCollide", withoutClearance(chainGraph({{"Clump", R"("params": {"count": 60, "strength": 0.6})"}, {"SelfCollide", ""}})), ""});
    graphs.push_back({"Static", withoutClearance(chainGraph({{"Static", ""}, {"Noise", R"("params": {"amplitude": 0.1})"}})), ""});
    graphs.push_back({"Tie", withoutClearance(chainGraph({{"Tie", R"("params": {"radius": 0.4})"}})), ""});
    graphs.push_back({"Wet", withoutClearance(chainGraph({{"Wet", wetParams(3, 0.6)}})), ""});
    graphs.push_back({"Blend", blendGraph(R"("params": {"mix": 0.6, "profile": [[0, 0.2], [1, 1]]})"), ""});
    graphs.push_back({"Jiggle", jiggleGraph(), ""});
    graphs.push_back({"MeshCut", meshCutGraph(R"("params": {"offset": 0.02})"), "",
                      [](const std::string& j) { return meshCutGroom(j, Vec3f(0.0f, 4.6f, 0.0f), 1.6f); }});
    graphs.push_back({"Jiggle moving", jiggleGraph(), "", [](const std::string& j) { return jiggleGroom(j, 4); }});
    graphs.push_back({"Jiggle settling", jiggleGraph(), "", [](const std::string& j) { return jiggleGroom(j, 13); }});
    for (const char* mode : {"segments", "scale", "even"}) {
        graphs.push_back({std::string("RestoreLength ") + mode,
                          withReference(neighbourGraph("RestoreLength", std::string(R"("params": {"mode": ")") + mode + R"("})"), "n1", "interpolate"), ""});
    }
    graphs.push_back({"Flow", withoutClearance(chainGraph({{"Flow", R"("params": {"lift": 12.0, "bend": 0.6, "rollRandom": 6.0})"}, {"Noise", kKinks}})), "",
                      [](const std::string& j) { return flowGroom(j, true); }});
    std::size_t differing = 0;
    for (const Graph& g : graphs) {
        GroomData data = g.make ? g.make(g.json) : groom(g.json);
        test::paintRequiredMaps(g.json, data);
        data.partsJson = meridianParts(*data.scalp);
        GroomSession session(data);
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands) << g.name;
        Prepared p;
        if (!prepareRun(session, {}, g.from, &p)) {
            std::printf("[ CPU bits ] %s: not run (%s)\n", g.name.c_str(), p.why.c_str());
            continue;
        }
        const GraphDocument& doc = *session.viewDocument();
        auto engine = gpu::makeCpuLookEngine();
        engine->setBase(p.base, session.last().scalp.get());
        std::string line;
        // Tie is not its operator's arithmetic yet: it and the stages after it are reported, not
        // counted.
        bool counted = true;
        for (std::size_t k = 1; k <= p.chain.stages.size(); ++k) {
            if (k < p.chain.stages.size() && p.chain.stages[k].node == p.chain.stages[k - 1].node) continue;  // (a Wet's levels)
            const gpu::LookStage& stage = p.chain.stages[k - 1];
            if (stage.kind == gpu::StageKind::Tie) counted = false;
            const StrandsPtr want = valueAs<StrandsPtr>(doc.graph.cachedValue(stage.node));
            gpu::LookRun run = p.run;
            run.params.stages.resize(k);
            run.fields.stages.resize(k);
            run.inputs.resize(k);
            engine->run(run, false);
            const std::vector<Vec3f> got = engine->readPositions();
            if (!want || got.size() != want->pointCount()) {
                line += " " + doc.nameOf(stage.node) + "(?)";
                continue;
            }
            std::size_t bits = 0;
            double most = 0.0;
            for (std::size_t i = 0; i < got.size(); ++i) {
                for (int a = 0; a < 3; ++a) {
                    const float x = (&got[i].x)[a], y = (&want->positions()[i].x)[a];
                    if (std::memcmp(&x, &y, 4) != 0) {
                        ++bits;
                        most = std::max(most, static_cast<double>(std::abs(x - y)));
                    }
                }
            }
            if (counted) differing += bits;
            char part[160];
            std::snprintf(part, sizeof part, " %s %zu/%zu (%.2g)", gpu::stageName(stage.kind), bits, got.size() * 3, most);
            line += part;
        }
        std::printf("[ CPU bits ] %s:%s\n", g.name.c_str(), line.c_str());
    }
    std::printf("[ CPU bits ] floats differing in all: %zu\n", differing);
    EXPECT_EQ(differing, 0u);
}

TEST(CpuEngine, TheOperatorsOnItGiveTheirOwnBits) {
    // The stages the CPU engine runs for their operators (look_cpu.h runStage), and the operators as
    // they run now (on it where they switched: Part's sides are the core's, its turns still its C++):
    // each against the operator's own C++ (the engine off), on the strands it took, bit for bit,
    // points and widths; mapped params, per point and per strand, root frames with UVs and without.
    struct Case {
        std::string type, params;
        bool uvs = false;
        bool cpus = false;  // the operator's own C++ (a per-point value read after the points before it moved)
        bool adds = false;  // adds points (Curl): the operator's own C++, its stage alone not compared
        std::pair<std::string, std::string> before = {};  // a node before it (after the Clump)
    };
    const Case cases[] = {
        {"Width", R"("params": {"width": 0.02, "randomness": 0.3, "profile": [[0, 1], [1, 0.2]]})"},
        {"Width", R"("params": {"width": {"expression": "0.02 - 0.015 * t", "min": 0.0, "max": 1.0}})"},
        {"Width", R"("params": {"width": {"expression": "0.01 + 0.01 * rand", "min": 0.0, "max": 1.0}})"},
        {"Bend", R"("params": {"angle": 40.0, "start": 0.2, "end": 0.8, "randomness": 0.3, "seed": 3})"},
        {"Bend", R"("params": {"angle": 180.0, "end": 0.0, "along": "direction", "direction": [0.0, -1.0, 0.3]})"},
        {"Bend", R"("params": {"angle": {"expression": "rand", "min": -60.0, "max": 60.0}, "along": "direction", "direction": [1.0, -1.0, 0.3], "end": 0.6})"},
        {"Bend", R"("params": {"angle": 70.0, "along": "tangent", "direction": [0.0, 0.0, 1.0], "start": 0.1})", true},
        {"Direction", R"("params": {"inclination": 60.0, "roll": 30.0, "rollRandom": 20.0, "inclinationRandom": 5.0, "seed": 2, "along": "direction"})"},
        {"Direction", R"("params": {"inclination": {"expression": "rand", "min": 20.0, "max": 80.0}, "strength": {"expression": "rand", "min": 0.2, "max": 1.0},
                                    "roll": 45.0, "along": "direction", "direction": [1.0, 0.0, 0.0]})"},
        {"Direction", R"("params": {"inclination": 70.0, "roll": {"expression": "rand", "min": -90.0, "max": 90.0}, "strength": 0.6})", true},
        {"Transform", R"("params": {"scale": 1.2, "translate": [0.0, 0.05, 0.0], "rotate": [10.0, 0.0, 5.0]})"},
        {"Transform", R"("params": {"scale": {"expression": "0.8 + 0.4 * rand", "min": 0.0, "max": 2.0}, "scaleRandom": 0.2,
                                    "translateRandom": [0.02, 0.0, 0.01], "rotateRandom": [5.0, 0.0, 5.0], "seed": 4})"},
        {"Transform", R"("params": {"space": "world", "pivot": [0.0, 1.0, 0.0], "rotate": [0.0, 20.0, 0.0], "scale": 0.9})"},
        {"Displace", R"("params": {"distance": 0.1, "profile": [[0, 0], [1, 1]]})"},
        {"Displace", R"("params": {"distance": {"expression": "0.05 * t", "min": 0.0, "max": 1.0}, "along": "direction", "direction": [0.0, 1.0, 0.0]})",
         false, true},
        {"Displace", R"("params": {"distance": {"expression": "0.02 + 0.06 * rand", "min": 0.0, "max": 1.0}})"},
        {"Part", R"("params": {"strength": 0.8, "width": 0.3})"},
        {"Part", R"("params": {"strength": {"expression": "0.4 + 0.6 * rand", "min": 0.0, "max": 1.0}, "width": 0.5, "profile": [[0, 0], [1, 1]]})"},
        {"Flyaway", R"("params": {"stray": 0.1, "fraction": 0.3, "frequency": 3.0, "start": 0.2, "standOut": 0.3, "lengthen": 0.2, "seed": 5})"},
        {"Flyaway", R"("params": {"stray": {"expression": "0.05 + 0.1 * rand", "min": 0.0, "max": 1.0}, "fraction": {"expression": "rand", "min": 0.0, "max": 0.6}})"},
        {"Twist", R"("params": {"turns": 0.6})"},
        {"Twist", R"("params": {"turns": {"expression": "0.3 + rand", "min": -2.0, "max": 2.0}, "profile": [[0, 0], [1, 1]]})"},
        {"Tangle", R"("params": {"strength": 0.6, "fraction": 0.5})"},
        {"Tangle", R"("params": {"strength": {"expression": "rand", "min": 0.0, "max": 1.0}, "fraction": 0.7, "sameClump": false, "reach": 0.0})"},
        {"BlendNeighbors", R"("params": {"strength": 0.6})"},
        {"BlendNeighbors", R"("params": {"strength": {"expression": "rand", "min": 0.0, "max": 1.0}, "preserveLength": true, "neighbors": 5})"},
        {"Length", R"("params": {"keep": 0.8, "fraction": 0.5, "trimMin": 0.1, "trimMax": 0.4, "seed": 2})"},
        {"Length", R"("params": {"keep": {"expression": "0.5 + 0.5 * rand", "min": 0.0, "max": 1.0}})"},
        {"Wind", R"("params": {"strength": 0.5, "gust": 0.6, "turbulence": 0.3, "direction": [1.0, 0.0, 0.3]})"},
        {"Wind", R"("params": {"strength": {"expression": "rand", "min": 0.0, "max": 1.0}, "loop": true, "loopStart": 1, "loopEnd": 25})"},
        {"Noise", R"("params": {"amplitude": 0.15, "frequency": 1.3})"},
        {"Noise", R"("params": {"amplitude": 0.1, "frequency": 4.0, "octaves": 4, "fraction": 0.6, "seed": 3})"},
        {"Noise", R"("params": {"amplitude": {"expression": "0.05 + 0.1 * rand", "min": 0.0, "max": 1.0}, "space": "world", "frequency": 2.0})"},
        {"Noise", R"("params": {"amplitude": 0.1, "mode": "kinky", "frequency": 3.0})"},
        {"Noise", R"("params": {"amplitude": 0.1, "mode": "kinky", "space": "world", "frequency": 3.0})"},
        {"Collide", R"("params": {"offset": 0.12})"},
        {"Collide", R"("params": {"offset": 0.08, "softness": 0.1, "smoothing": 0.2, "iterations": 3})"},
        {"Clump", R"("params": {"count": 80, "strength": 0.7})"},
        {"Clump", R"("params": {"count": 50, "strength": {"expression": "0.3 + 0.7 * t", "min": 0.0, "max": 1.0}, "volumize": 0.2, "flatness": 0.5,
                                "lockRadius": 0.05, "lockRadiusRandom": 0.3, "strengthRandom": 0.4, "copyAmount": 0.3, "pastTip": "extend", "seed": 6})",
         false, true},
        {"Clump", R"("params": {"count": 50, "strength": {"expression": "0.3 + 0.7 * rand", "min": 0.0, "max": 1.0}, "volumize": 0.2, "flatness": 0.5,
                                "lockRadius": 0.05, "lockRadiusRandom": 0.3, "strengthRandom": 0.4, "copyAmount": 0.3, "pastTip": "extend", "seed": 6})"},
        {"Clump", R"("params": {"count": 40, "strength": -0.3, "repelRadius": 0.2, "flatten": "random", "flatness": 0.7})"},
        {"Interpolate", R"("params": {"keepClearance": true})", false, true},
        {"Interpolate", R"("params": {"keepClearance": false, "limitDistance": 0.6, "preserveLength": true})", false, true},
        {"Interpolate", R"("params": {"keepClearance": true, "limitDistance": 1.5, "smoothGuides": true})", true, true},
        {"Smooth", R"("params": {"strength": 0.8, "scale": 0.3})"},
        {"Smooth", R"("params": {"strength": {"expression": "0.3 + 0.7 * rand", "min": 0.0, "max": 1.0}, "scale": 0.1, "profile": [[0, 0], [1, 1]]})"},
        {"Smooth", R"("params": {"strength": 1.0, "scale": 1.0})"},
        {"Gravity", R"("params": {"strength": 0.8, "stiffness": 0.3})"},
        {"Gravity", R"("params": {"strength": {"expression": "rand", "min": 0.0, "max": 1.0}, "direction": [0.3, -1.0, 0.0]})"},
        {"Curl", R"("params": {"radius": 0.1, "frequency": 1.0, "tolerance": 0.0})"},
        {"Curl", R"("params": {"radius": 0.05, "frequency": 0.3})"},
        {"Curl", R"("params": {"radius": 0.05, "frequency": 3.0, "tolerance": 0.1})", false, false, true},
        {"Curl", R"("params": {"radius": 0.08, "frequency": 1.5, "shape": "zigzag", "tolerance": 0.2})", false, false, true},
        {"Curl", R"("params": {"radius": 0.08, "frequency": 1.2, "shape": "wave", "randomness": 0.6, "spacingRandom": 0.5, "tolerance": 0.0})"},
        {"Curl", R"("params": {"radius": 0.08, "frequency": 1.2, "shape": "zigzag", "frequencyProfile": [[0, 0.5], [1, 1]], "tolerance": 0.0})"},
        {"Curl", R"("params": {"radius": 0.1, "frequency": 0.8, "coherence": "clump", "tolerance": 0.0})"},
        {"Curl", R"("params": {"radius": 0.1, "frequency": 0.8, "weight": 1.0, "tolerance": 0.0})"},
        {"Curl", R"("params": {"radius": {"expression": "0.05 + 0.1 * rand", "min": 0.0, "max": 1.0}, "frequency": 1.0, "phase": 0.3, "tolerance": 0.0})"},
        {"Curl", R"("params": {"shape": "coil", "coilWidths": 3.0, "frequency": 2.0, "tolerance": 0.0})", false, false, false,
         {"Width", R"("params": {"width": 0.02, "randomness": 0.4})"}},
        {"SelfCollide", R"("params": {"packing": 0.02})", false, false, false, {"Width", R"("params": {"width": 0.02, "randomness": 0.4})"}},
        {"SelfCollide", R"("params": {"packing": 0.03, "strength": {"expression": "0.5 + 0.5 * rand", "min": 0.0, "max": 1.0}, "profile": [[0, 0], [1, 1]]})",
         false, false, false, {"Width", R"("params": {"width": 0.04})"}},
        {"SelfCollide", R"("params": {"packing": 0.01})"},
        {"Static", R"("params": {"strength": 0.5})"},
        {"Static", R"("params": {"strength": {"expression": "0.3 + 0.7 * rand", "min": 0.0, "max": 1.0}, "randomness": 0.8, "profile": [[0, 0.2], [1, 1]]})"},
    };
    std::size_t differing = 0, compared = 0;
    for (const Case& c : cases) {
        SCOPED_TRACE(c.type + " " + c.params);
        // (Interpolate: the graph's own, its params these.)
        const bool interpolate = c.type == "Interpolate";
        std::string json = shaperGraph(interpolate ? "Bend" : c.type, interpolate ? R"("params": {"angle": 10.0})" : c.params, true, c.before);
        if (interpolate) {
            nlohmann::json j = nlohmann::json::parse(json);
            const nlohmann::json own = nlohmann::json::parse("{" + c.params + "}")["params"];
            for (auto& n : j["nodes"]) {
                if (n["type"] == "Interpolate") n["params"].update(own);
            }
            json = j.dump();
        }
        GroomData data = c.uvs ? paintedGroom(json) : groom(json);
        data.partsJson = meridianParts(*data.scalp);
        GroomSession session(data);
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const GraphDocument& doc = *session.viewDocument();
        NodeId id = 0;
        if (interpolate) {
            for (const auto& [name, node] : doc.nodesByName) {
                if (doc.graph.node(node).typeName() == "Interpolate") id = node;
            }
        } else {
            id = doc.nodesByName.at(c.before.first.empty() ? shaperName(true) : "n2");
        }
        ExternalInputs ext;
        setGroomExternals(ext, session.last().scalp, data.guides, session.last().hierarchy, session.last().parts, session.last().flow);
        EvalContext ctx;
        ctx.externals = &ext;
        ctx.time = 1.25;  // (Wind's gusts somewhere along)
        const Operator& op = doc.graph.node(id);
        std::vector<Value> inputs(op.inputs().size());
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            NodeId src = 0;
            std::size_t output = 0;
            if (doc.graph.inputSource(id, i, &src, &output)) inputs[i] = doc.graph.cachedValue(src, output);
        }
        // (Run there, not handed to its C++.)
        const std::map<std::string, gpu::StageKind> kinds = {{"Width", gpu::StageKind::Width},         {"Bend", gpu::StageKind::Bend},
                                                             {"Direction", gpu::StageKind::Direction}, {"Transform", gpu::StageKind::Transform},
                                                             {"Displace", gpu::StageKind::Displace},   {"Gravity", gpu::StageKind::Gravity},
                                                             {"Part", gpu::StageKind::Part},           {"Flyaway", gpu::StageKind::Flyaway},
                                                             {"Twist", gpu::StageKind::Twist},         {"Tangle", gpu::StageKind::Tangle},
                                                             {"BlendNeighbors", gpu::StageKind::BlendNeighbors},
                                                             {"Length", gpu::StageKind::Length},       {"Wind", gpu::StageKind::Wind},
                                                             {"Noise", gpu::StageKind::Noise},         {"Collide", gpu::StageKind::Collide},
                                                             {"Clump", gpu::StageKind::Clump},         {"Interpolate", gpu::StageKind::Interpolate},
                                                             {"Smooth", gpu::StageKind::Smooth},       {"Curl", gpu::StageKind::Curl},
                                                             {"SelfCollide", gpu::StageKind::SelfCollide}, {"Static", gpu::StageKind::Static}};
        const gpu::StageKind kind = kinds.at(c.type);
        gpu::StageInputs stageInputs;
        std::string whyNot;
        if (kind == gpu::StageKind::Twist) stageInputs.twist = gpu::twistInputs(*valueAs<StrandsPtr>(inputs[0]), valueAs<GuidesPtr>(inputs[1]).get(), "clumpId", false, &whyNot);
        // (Tangle's partners, BlendNeighbors' neighbours and Clump's centers: their operators' own, as their C++ just made them.)
        if (kind == gpu::StageKind::Clump) {
            gpu::setCpuEngineOn(false);
            const Value cpu = op.evaluate(ctx, inputs);
            gpu::setCpuEngineOn(true);
            ClumpState state;
            ASSERT_TRUE(clumpState(op, valueAs<StrandsPtr>(cpu), &state));
            stageInputs.clump = gpu::clumpInputs(state.assigned, state.curves, state.centerNormals, state.centerIds,
                                                 static_cast<std::uint64_t>(op.params().getInt("seed")));
        }
        if (kind == gpu::StageKind::Tangle || kind == gpu::StageKind::BlendNeighbors) {
            gpu::setCpuEngineOn(false);
            const Value cpu = op.evaluate(ctx, inputs);
            gpu::setCpuEngineOn(true);
            if (kind == gpu::StageKind::Tangle) {
                std::shared_ptr<const TangleState> state;
                ASSERT_TRUE(tangleState(op, valueAs<StrandsPtr>(cpu), &state));
                stageInputs.tangle = gpu::tangleInputs(*valueAs<StrandsPtr>(inputs[0]), *state);
            } else {
                std::shared_ptr<const BlendNeighborsState> state;
                ASSERT_TRUE(blendNeighborsState(op, valueAs<StrandsPtr>(cpu), &state));
                stageInputs.neighbors = state;
            }
        }
        if (kind == gpu::StageKind::Collide) {
            // Its surfaces' fields as CollideOp asks them (collisionField at its offset and search radius).
            std::vector<MeshPtr> meshes;
            std::vector<DistanceFieldPtr> fields;
            const float offset = static_cast<float>(op.params().getDouble("offset")), softness = static_cast<float>(op.params().getDouble("softness"));
            const float radius = std::max(offset + softness, static_cast<float>(op.params().getDouble("maxDepth"))) * 1.001f + 1e-6f;
            for (std::size_t slot = 1; slot < inputs.size(); ++slot) {
                if (const MeshPtr mesh = valueAs<MeshPtr>(inputs[slot])) {
                    meshes.push_back(mesh);
                    fields.push_back(collisionField(mesh, offset, radius));
                }
            }
            stageInputs.collide = gpu::collideInputs(meshes, 0u, fields, offset, softness, &whyNot);
        }
        if (kind == gpu::StageKind::Curl && op.params().getString("coherence") == "clump") {
            stageInputs.curl = gpu::curlInputs(*valueAs<StrandsPtr>(inputs[0]), session.last().scalp.get(), "clumpId",
                                               static_cast<std::uint64_t>(op.params().getInt("seed")));
        }
        if (kind == gpu::StageKind::SelfCollide || kind == gpu::StageKind::Static) {
            auto layout = std::make_shared<HairGridLayout>();
            ASSERT_TRUE(hairGridLayout(op.typeName(), *valueAs<StrandsPtr>(inputs[0]), session.last().scalp, layout.get(), &whyNot)) << whyNot;
            auto grid = std::make_shared<gpu::HairGridInputs>();
            grid->walls = hairGridWalls(*session.last().scalp, layout->h, layout->wallReach);
            grid->layout = layout;
            stageInputs.grid = grid;
        }
        if (kind == gpu::StageKind::Part) stageInputs.part = gpu::partInputs(*valueAs<StrandsPtr>(inputs[0]), session.last().parts.get(), session.last().scalp.get(), "");
        // Its C++ (the engine off), the stage on the engine, the operator as it runs now.
        StrandsPtr made[3];
        gpu::setCpuEngineOn(false);
        made[0] = valueAs<StrandsPtr>(op.evaluate(ctx, inputs));
        gpu::setCpuEngineOn(true);
        if (c.adds) EXPECT_GT(made[0]->pointCount(), valueAs<StrandsPtr>(inputs[0])->pointCount());
        if (kind == gpu::StageKind::SelfCollide || kind == gpu::StageKind::Static) {
            EXPECT_TRUE(made[0] != valueAs<StrandsPtr>(inputs[0])) << "pushes nothing";  // (its input: none moved)
        }
        if (interpolate) {
            gpu::setCpuEngineOn(false);
            const StrandsPtr cpu = valueAs<StrandsPtr>(op.evaluate(ctx, inputs));
            gpu::setCpuEngineOn(true);
            const StrandsPtr now = valueAs<StrandsPtr>(op.evaluate(ctx, inputs));
            ASSERT_TRUE(cpu && now);
            ASSERT_EQ(cpu->pointCount(), now->pointCount());
            std::size_t bits = 0;
            for (std::size_t i = 0; i < cpu->pointCount(); ++i) bits += std::memcmp(&cpu->positions()[i], &now->positions()[i], sizeof(Vec3f)) != 0;
            for (const std::string name : {"guideInfluence", "region"}) {
                const std::vector<float>* a = cpu->strandChannel(name);
                const std::vector<float>* b = now->strandChannel(name);
                EXPECT_EQ(a == nullptr, b == nullptr) << name;
                if (a && b) EXPECT_TRUE(*a == *b) << name;
            }
            EXPECT_EQ(bits, 0u) << "the operator now";
            differing += bits;
            compared += cpu->pointCount();
            continue;
        }
        gpu::StageParamReader reader(op.params(), "");
        reader.time = ctx.time;
        reader.framesPerSecond = ctx.framesPerSecond;
        if (!c.adds) {
            made[1] = gpu::runStage(kind, reader, valueAs<StrandsPtr>(inputs[0]), session.last().scalp.get(), stageInputs);
            EXPECT_EQ(made[1] != nullptr, !c.cpus);
        }
        made[2] = valueAs<StrandsPtr>(op.evaluate(ctx, inputs));
        for (int m = 1; m < 3; ++m) {
            if (!made[m]) continue;
            ASSERT_TRUE(made[0]);
            ASSERT_EQ(made[0]->pointCount(), made[m]->pointCount());
            ASSERT_EQ(made[0]->widths().size(), made[m]->widths().size());
            std::size_t bits = 0;
            for (std::size_t i = 0; i < made[0]->pointCount(); ++i) {
                bits += std::memcmp(&made[0]->positions()[i], &made[m]->positions()[i], sizeof(Vec3f)) != 0;
            }
            for (std::size_t i = 0; i < made[0]->widths().size(); ++i) {
                bits += std::memcmp(&made[0]->widths()[i], &made[m]->widths()[i], sizeof(float)) != 0;
            }
            // (Flyaway's strays flagged as its C++ flags them.)
            if (m == 2 && made[0]->strandChannel("flyaway")) {
                const std::vector<float>* got = made[m]->strandChannel("flyaway");
                EXPECT_TRUE(got && *got == *made[0]->strandChannel("flyaway"));
            }
            EXPECT_EQ(bits, 0u) << (m == 1 ? "the stage on the engine" : "the operator now");
            differing += bits;
            compared += made[0]->pointCount();
        }
    }
    std::printf("[ CPU ops ] points differing: %zu of %zu\n", differing, compared);
}

TEST(CpuEngine, SpeedAgainstTheOperators) {
    // Each stage of dense grooms (400 strands a unit): the CPU engine's run of it against its
    // operator's evaluation of the same input, its own C++ (the engine off) and as it runs now (ms,
    // best of 3). A report: which kernels are at their operators' speed, which want tuning.
    struct Graph {
        std::string name, json;
        std::function<GroomData(const std::string&)> make = nullptr;  // (null: groom)
    };
    std::vector<Graph> graphs = {{"look", graph(0.3)}, {"hair_basics", startingGraph("hairBasics")}};
    for (const char* preset : {"parted_style", "gravity", "flyaways", "cinematic_hair", "wind_and_jiggle"}) graphs.push_back({preset, presetGraph(preset)});
    const std::pair<const char*, const char*> shapers[] = {
        {"Bend", R"("params": {"angle": 35.0, "randomness": 0.3})"},
        {"Direction", R"("params": {"inclination": 30.0, "strength": 0.7, "roll": 20.0})"},
        {"Transform", R"("params": {"scale": 1.2, "translate": [0.0, 0.05, 0.0], "rotate": [10.0, 0.0, 5.0]})"},
        {"Displace", R"("params": {"distance": 0.1})"},
        {"Twist", R"("params": {"turns": 0.6})"}};
    for (const auto& [type, params] : shapers) graphs.push_back({type, shaperGraph(type, params, true)});
    for (const auto& [type, params] : {std::pair<const char*, const char*>{"Smooth", R"("params": {"strength": 0.8, "scale": 0.3})"},
                                       {"BlendNeighbors", R"("params": {"strength": 0.6})"},
                                       {"Tangle", R"("params": {"strength": 0.6, "fraction": 0.5})"}}) {
        graphs.push_back({type, neighbourGraph(type, params)});
    }
    graphs.push_back({"Collide", withoutClearance(chainGraph({{"Collide", R"("params": {"offset": 0.12})"}}, true, true))});
    graphs.push_back({"SelfCollide", withoutClearance(chainGraph({{"Clump", R"("params": {"count": 60, "strength": 0.6})"}, {"SelfCollide", ""}}))});
    graphs.push_back({"Static", withoutClearance(chainGraph({{"Static", R"("params": {"strength": 0.5})"}}))});
    graphs.push_back({"RestoreLength", withReference(neighbourGraph("RestoreLength", R"("params": {"mode": "segments"})"), "n1", "interpolate")});
    graphs.push_back({"MeshCut", meshCutGraph(R"("params": {"offset": 0.02})"),
                      [](const std::string& j) { return meshCutGroom(j, Vec3f(0.0f, 4.6f, 0.0f), 1.6f); }});
    std::set<std::string> seen;
    for (const Graph& g : graphs) {
        nlohmann::json j = nlohmann::json::parse(g.json);
        for (auto& n : j["nodes"]) {
            if (n["type"] == "Scatter") n["params"]["density"] = 400.0;
        }
        GroomData data = g.make ? g.make(j.dump()) : groom(j.dump());
        test::paintRequiredMaps(g.json, data);
        data.partsJson = meridianParts(*data.scalp);
        GroomSession session(data);
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands) << g.name;
        Prepared p;
        if (!prepareRun(session, {}, "", &p)) continue;
        const GraphDocument& doc = *session.viewDocument();
        ExternalInputs ext;
        setGroomExternals(ext, session.last().scalp, data.guides, session.last().hierarchy, session.last().parts, session.last().flow);
        setMapExternals(ext, data.maps);
        EvalContext ctx;
        ctx.externals = &ext;
        auto engine = gpu::makeCpuLookEngine();
        for (std::size_t k = 0; k < p.chain.stages.size(); ++k) {
            const gpu::LookStage& stage = p.chain.stages[k];
            const std::string kind = gpu::stageName(stage.kind);
            if (stage.level >= 0 || !seen.insert(kind).second) continue;  // (a Wet's levels; each kind once)
            NodeId src = 0;
            std::size_t output = 0;
            if (!doc.graph.inputSource(stage.node, 0, &src, &output)) continue;
            const StrandsPtr input = valueAs<StrandsPtr>(doc.graph.cachedValue(src, output));
            if (!input) continue;
            gpu::LookRun run = p.run;
            run.params.stages = {p.run.params.stages[k]};
            run.params.stages[0].stage.from = gpu::kNoStage;
            run.params.stages[0].stage.from2 = gpu::kNoStage;
            run.fields.stages = k < p.run.fields.stages.size() ? std::vector<gpu::StageFields>{p.run.fields.stages[k]} : std::vector<gpu::StageFields>{};
            run.inputs = k < p.run.inputs.size() ? std::vector<gpu::StageInputs>{p.run.inputs[k]} : std::vector<gpu::StageInputs>{};
            engine->setBase(input, session.last().scalp.get());
            std::vector<Vec3f> out(input->pointCount());
            gpu::LookTargets targets;
            targets.positions = out.data();
            double engineMs = 1e30, opMs = 1e30, onMs = 1e30;
            try {
                for (int r = 0; r < 3; ++r) {
                    const auto t0 = std::chrono::steady_clock::now();
                    engine->run(run, false, targets);
                    engineMs = std::min(engineMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                }
            } catch (const std::exception& e) {
                std::printf("[ CPU speed ] %s: not run alone (%s)\n", kind.c_str(), e.what());
                continue;
            }
            engine->dropTargets();
            const Operator& op = doc.graph.node(stage.node);
            std::vector<Value> inputs(op.inputs().size());
            for (std::size_t i = 0; i < inputs.size(); ++i) {
                if (doc.graph.inputSource(stage.node, i, &src, &output)) inputs[i] = doc.graph.cachedValue(src, output);
            }
            for (const bool on : {false, true}) {
                gpu::setCpuEngineOn(on);
                for (int r = 0; r < 3; ++r) {
                    // (Another set each time, as a graph's evaluations hand it: the engine takes it anew.)
                    std::vector<Value> fresh = inputs;
                    fresh[0] = Value(StrandsPtr(std::make_shared<StrandSet>(*input)));
                    const auto t0 = std::chrono::steady_clock::now();
                    const Value made = op.evaluate(ctx, fresh);
                    double& best = on ? onMs : opMs;
                    best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                }
            }
            std::printf("[ CPU speed ] %-15s %8zu strands %9zu points: engine %8.2f ms, operator's C++ %8.2f ms (x%5.2f), operator now %8.2f ms\n",
                        kind.c_str(), input->strandCount(), input->pointCount(), engineMs, opMs, engineMs / opMs, onMs);
        }
    }
    gpu::setCpuEngineOn(true);
}
}  // namespace
}  // namespace groomlab
