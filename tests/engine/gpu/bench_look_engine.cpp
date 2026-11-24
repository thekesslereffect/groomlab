// SPDX-License-Identifier: Apache-2.0
// GPU look engine benchmark (ctest label "bench": GROOMLAB_BENCH=1 ctest --preset ninja-maya2022 -L bench).
// Not a check: numbers for the performance audit, printed as "[ bench ] ..." lines (and as JSON
// lines to GROOMLAB_BENCH_OUT when set).
//
// Per backend (DirectX 11, OpenGL 4.5) and strand count (GROOMLAB_BENCH_COUNTS, default
// 100000,500000; 16 points a strand): the benchmark chain (Scatter -> Interpolate from guides,
// keeping clearance -> Clump with its own 400 centers -> Noise -> Curl -> Width -> Resample even)
//   - the CPU: the first evaluation, a Noise tweak and a Clump tweak re-evaluated (what a drag
//     costs without the GPU preview), and a brush dab's evaluation stopping at Interpolate;
//   - the GPU, from Interpolate, Clump, Noise and Width: the engine made, the base uploaded (CPU ms,
//     MB), the first run (per-strand numbers, inputs: CPU ms, MB), then warm drags (a new value
//     each run): CPU ms of run() alone, CPU ms to the GPU done, GPU timestamps per kernel, and the
//     effective bandwidth of Noise (bytes read and written over its time);
//   - Noise and the frames a thread per strand against a thread per point;
//   - results written in place into display buffers against the engine's own;
//   - a brush dab end to end without a host: a guide moved, the CPU's evaluation up to
//     Interpolate, LookInputs read again, the run, the GPU done.
// A sculpt stroke's dabs on a dense groom's Sculpt node, on the CPU and on the GPU (SculptDabs).
// And, once: which shipped presets the GPU previews from where (findLookChain, readLookParams,
// lookStrands, readLookFields, LookInputs::read), with why it stops.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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
#include "groomlab/gpu/look_engine.h"
#include "groomlab/gpu/look_preview.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/groom_session.h"
#include "groomlab/parts.h"
#include "groomlab/presets.h"
#include "groomlab/sculpt.h"
#include "groomlab/stroke.h"

namespace groomlab {
namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

bool benchOn() {
    const char* v = std::getenv("GROOMLAB_BENCH");
    return v && *v && std::strcmp(v, "0") != 0;
}

std::vector<std::size_t> benchCounts() {
    std::vector<std::size_t> counts;
    const char* v = std::getenv("GROOMLAB_BENCH_COUNTS");
    std::stringstream in(v && *v ? v : "100000,500000");
    std::string item;
    while (std::getline(in, item, ',')) {
        if (!item.empty()) counts.push_back(static_cast<std::size_t>(std::stod(item)));
    }
    return counts;
}

void record(const nlohmann::json& row) {
    std::printf("[ bench ] %s\n", row.dump().c_str());
    std::fflush(stdout);
    if (const char* out = std::getenv("GROOMLAB_BENCH_OUT"); out && *out) {
        std::ofstream file(out, std::ios::app);
        file << row.dump() << "\n";
    }
}

// --- devices -------------------------------------------------------------------------------

class Device {
public:
    virtual ~Device() = default;
    virtual bool available() const = 0;
    virtual std::string name() const = 0;
    virtual std::unique_ptr<gpu::LookEngine> engine() = 0;
    virtual void* displayBuffer(std::size_t bytes) = 0;
    virtual void release(void* buffer) = 0;
};

class D3D11Device final : public Device {
public:
    D3D11Device() {
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device_, nullptr,
                          &context_);
    }
    ~D3D11Device() override {
        if (context_) context_->Release();
        if (device_) device_->Release();
    }
    bool available() const override { return device_ != nullptr; }
    std::string name() const override { return "DirectX11"; }
    std::unique_ptr<gpu::LookEngine> engine() override { return gpu::makeD3D11LookEngine(device_); }
    void* displayBuffer(std::size_t bytes) override { return gpu::createD3D11DisplayBuffer(device_, bytes); }
    void release(void* buffer) override {
        if (buffer) static_cast<ID3D11Buffer*>(buffer)->Release();
    }

private:
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
};

class GLDevice final : public Device {
public:
    GLDevice() {
        WNDCLASSA wc{};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "GroomLabGpuBench";
        RegisterClassA(&wc);
        window_ = CreateWindowA("GroomLabGpuBench", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
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
        const int attribs[] = {0x2091, 4, 0x2092, 5, 0x9126, 0x00000001, 0};
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
    std::string name() const override { return "OpenGL"; }
    std::unique_ptr<gpu::LookEngine> engine() override { return gpu::makeGLLookEngine(); }
    void* displayBuffer(std::size_t bytes) override {
        GLuint b = 0;
        genBuffers_(1, &b);
        bindBuffer_(0x8892, b);
        bufferData_(0x8892, static_cast<std::ptrdiff_t>(bytes), nullptr, 0x88E4);
        bindBuffer_(0x8892, 0);
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(b));
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
};

std::unique_ptr<Device> makeDevice(const std::string& name) {
    if (name == "DirectX11") return std::make_unique<D3D11Device>();
    return std::make_unique<GLDevice>();
}

// --- the scene -----------------------------------------------------------------------------

constexpr float kRadius = 10.0f;  // a head-sized sphere

// The benchmark chain (tools/bench/xgen_compare's, on a sphere): `density` strands a unit area.
std::string benchGraph(double density, double noiseAmplitude = 0.5, double clumpStrength = 0.8) {
    nlohmann::json g = nlohmann::json::parse(R"({"nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "guides", "type": "Guides"},
        {"name": "scatter", "type": "Scatter", "params": {"length": 15.0}},
        {"name": "interpolate", "type": "Interpolate", "params": {}},
        {"name": "clump", "type": "Clump", "params": {"count": 400, "strength": 0.8}},
        {"name": "noise", "type": "Noise", "params": {"amplitude": 0.5, "frequency": 1.0}},
        {"name": "curl", "type": "Curl", "params": {"radius": 0.3, "frequency": 0.5, "tolerance": 0.0}},
        {"name": "width", "type": "Width", "params": {"width": 0.01}},
        {"name": "resample", "type": "Resample", "params": {"segments": 15, "mode": "even"}},
        {"name": "output", "type": "Output"}],
      "connections": [
        {"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "interpolate", "input": "strands"},
        {"from": "guides", "to": "interpolate", "input": "guides"},
        {"from": "scalp", "to": "interpolate", "input": "scalp"},
        {"from": "interpolate", "to": "clump", "input": "strands"},
        {"from": "clump", "to": "noise", "input": "strands"},
        {"from": "noise", "to": "curl", "input": "strands"},
        {"from": "curl", "to": "width", "input": "strands"},
        {"from": "width", "to": "resample", "input": "strands"},
        {"from": "resample", "to": "output", "input": "strands"}],
      "output": "output"})");
    for (auto& n : g["nodes"]) {
        if (n["name"] == "scatter") n["params"]["density"] = density;
        if (n["name"] == "noise") n["params"]["amplitude"] = noiseAmplitude;
        if (n["name"] == "clump") n["params"]["strength"] = clumpStrength;
    }
    return g.dump();
}

// Guides straight out along the normals, about one per 1.6 units of the sphere (~500 guides on
// radius 10), 15 long, 8 points.
GuidesPtr benchGuides(const ScalpMesh& scalp, float bend = 0.0f) {
    auto guides = std::make_shared<GuideSet>();
    std::uint64_t id = 1;
    const int rings = 16;
    for (int r = 1; r < rings; ++r) {
        const float theta = 3.14159265f * static_cast<float>(r) / rings;
        const int around = std::max(6, static_cast<int>(std::round(32.0f * std::sin(theta))));
        for (int k = 0; k < around; ++k) {
            const float phi = 6.2831853f * (static_cast<float>(k) + 0.5f * static_cast<float>(r % 2)) / static_cast<float>(around);
            const Vec3f n(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
            const Vec3f side = n.cross(Vec3f(0, 1, 0)).length() > 1e-3f ? n.cross(Vec3f(0, 1, 0)).normalized() : Vec3f(1, 0, 0);
            std::vector<Vec3f> pts;
            for (int i = 0; i <= 7; ++i) {
                const float t = static_cast<float>(i) / 7.0f;
                pts.push_back(n * (kRadius + 15.0f * t) + side * (bend * t * t));
            }
            guides->add(id++, std::move(pts));
        }
    }
    guides->rootOnMesh(scalp);
    return guides;
}

GroomData benchGroom(std::size_t count) {
    GroomData d;
    d.scalp = test::makeSphere(64, 128, kRadius);
    const double area = 4.0 * 3.14159265 * kRadius * kRadius;
    d.graphJson = benchGraph(static_cast<double>(count) / area);
    d.guides = benchGuides(*d.scalp);
    return d;
}

struct Prepared {
    gpu::LookChain chain;
    gpu::LookRun run;
    StrandsPtr base, output;
    std::string why;
};
bool prepareRun(GroomSession& session, const gpu::Overrides& overrides, const std::string& from, Prepared* p,
                gpu::LookInputs& inputs, const std::set<NodeId>& changed = {}) {
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
    return gpu::readLookParams(*doc, p->chain, overrides, &p->run.params, &p->why) &&
           gpu::lookStrands(*doc, p->chain, &p->base, &p->output, &p->why) &&
           gpu::readLookFields(*doc, p->chain, session.last().scalp.get(), &p->run.fields, &p->why) &&
           gpu::lookFieldsHold(*doc, p->chain, p->run.fields, &p->why) &&
           inputs.read(*doc, p->chain, p->run.params, session.last().parts.get(), &p->run.inputs, &p->why, changed,
                       session.last().scalp.get(), &p->run.fields, session.last().scalp);
}

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

nlohmann::json timesJson(const gpu::GpuTimes& t) {
    return {{"interpolate", t.interpolateMs}, {"clearance", t.clearanceMs}, {"length", t.lengthMs}, {"clump", t.clumpMs},
            {"noise", t.noiseMs},             {"curl", t.curlMs},           {"width", t.widthMs},   {"lines", t.linesMs},
            {"copy", t.copyMs},               {"total", t.totalMs}};
}

// The per-kernel medians of several runs' times.
nlohmann::json medianTimes(const std::vector<gpu::GpuTimes>& all) {
    nlohmann::json out;
    auto pick = [&](const char* name, double gpu::GpuTimes::*field) {
        std::vector<double> v;
        for (const auto& t : all) v.push_back(t.*field);
        out[name] = median(v);
    };
    pick("interpolate", &gpu::GpuTimes::interpolateMs);
    pick("clearance", &gpu::GpuTimes::clearanceMs);
    pick("clump", &gpu::GpuTimes::clumpMs);
    pick("noise", &gpu::GpuTimes::noiseMs);
    pick("curl", &gpu::GpuTimes::curlMs);
    pick("width", &gpu::GpuTimes::widthMs);
    pick("length", &gpu::GpuTimes::lengthMs);
    pick("part", &gpu::GpuTimes::partMs);
    pick("gravity", &gpu::GpuTimes::gravityMs);
    pick("flyaway", &gpu::GpuTimes::flyawayMs);
    pick("collide", &gpu::GpuTimes::collideMs);
    pick("wind", &gpu::GpuTimes::windMs);
    pick("blend", &gpu::GpuTimes::blendMs);
    pick("selfCollide", &gpu::GpuTimes::selfCollideMs);
    pick("static", &gpu::GpuTimes::staticMs);
    pick("lines", &gpu::GpuTimes::linesMs);
    pick("copy", &gpu::GpuTimes::copyMs);
    pick("total", &gpu::GpuTimes::totalMs);
    return out;
}

// --- the benchmark ---------------------------------------------------------------------------

class GpuBench : public ::testing::TestWithParam<std::string> {};

TEST_P(GpuBench, LookChainKernelsAndDragLatency) {
    if (!benchOn()) GTEST_SKIP() << "set GROOMLAB_BENCH=1 to run the benchmark";
    auto device = makeDevice(GetParam());
    if (!device->available()) GTEST_SKIP() << "no " << GetParam() << " device";
    for (const std::size_t count : benchCounts()) {
        GroomData data = benchGroom(count);
        const double area = 4.0 * 3.14159265 * kRadius * kRadius;
        GroomSession session(data);
        auto t0 = Clock::now();
        ASSERT_TRUE(session.evaluate(ViewRequest{}).strands);
        const double firstEvalMs = msSince(t0);
        const StrandsPtr out = session.last().strands;
        const std::size_t strands = out->strandCount(), points = out->pointCount();

        // The CPU: a Noise tweak and a Clump tweak evaluated (as a drag without the GPU preview).
        std::vector<double> noiseCpu, clumpCpu;
        for (int i = 0; i < 4; ++i) {
            session.setGraphJson(benchGraph(static_cast<double>(count) / area, 0.5 + 0.01 * (i + 1), 0.8));
            t0 = Clock::now();
            session.evaluate(ViewRequest{});
            noiseCpu.push_back(msSince(t0));
        }
        for (int i = 0; i < 4; ++i) {
            session.setGraphJson(benchGraph(static_cast<double>(count) / area, 0.54, 0.8 - 0.01 * (i + 1)));
            t0 = Clock::now();
            session.evaluate(ViewRequest{});
            clumpCpu.push_back(msSince(t0));
        }
        session.setGraphJson(benchGraph(static_cast<double>(count) / area));
        session.evaluate(ViewRequest{});
        record({{"bench", "cpu"}, {"api", GetParam()}, {"strands", strands}, {"points", points}, {"firstEvaluationMs", firstEvalMs},
                {"noiseTweakMs", median(noiseCpu)}, {"clumpTweakMs", median(clumpCpu)}});

        // The GPU from each stage: drags of a value of that node.
        struct From {
            const char* node;
            const char* param;
            double base;
        };
        const From froms[] = {{"interpolate", "limitDistance", 4.0}, {"clump", "strength", 0.8}, {"noise", "amplitude", 0.5},
                              {"width", "width", 0.01}};
        // GROOMLAB_BENCH_ONLY=dab: the dab alone.
        const char* only = std::getenv("GROOMLAB_BENCH_ONLY");
        const bool dabOnly = only && std::strcmp(only, "dab") == 0;
        for (const From& f : froms) {
            if (dabOnly) break;
            gpu::LookInputs inputs;
            Prepared p;
            const gpu::Overrides first{{f.node, {{f.param, f.base * 1.01}}}};
            if (!prepareRun(session, first, f.node, &p, inputs)) {
                record({{"bench", "gpu_from"}, {"api", GetParam()}, {"strands", strands}, {"from", f.node}, {"refused", p.why}});
                continue;
            }
            std::unique_ptr<gpu::LookEngine> engine;
            for (const bool inPlace : {true, false}) {
                t0 = Clock::now();
                engine = device->engine();  // a fresh one: the base uploaded in full
                const double makeMs = msSince(t0);
                std::vector<void*> buffers;
                gpu::LookTargets targets;
                if (inPlace) {
                    for (int b = 0; b < 3; ++b) buffers.push_back(device->displayBuffer(points * 12));
                    targets.positions = buffers[0];
                    targets.normals = buffers[1];
                    targets.tangents = buffers[2];
                }
                t0 = Clock::now();
                const bool uploaded = engine->setBase(p.base, session.last().scalp.get());
                const double baseMs = msSince(t0);
                const double baseMb = engine->uploadMegabytes();
                (void)uploaded;
                t0 = Clock::now();
                engine->run(p.run, true, targets);
                const double firstRunCpu = msSince(t0);
                engine->finish();
                const double firstRunDone = msSince(t0);
                const double firstUploadMb = engine->runUploadMegabytes();
                const gpu::GpuTimes firstTimes = engine->times(true);
                // Warm drags: a new value each run, params read again (as a drag step does).
                std::vector<double> prepareMs, runCpu, runDone, uploadMb;
                std::vector<gpu::GpuTimes> times;
                for (int i = 0; i < 20; ++i) {
                    const gpu::Overrides o{{f.node, {{f.param, f.base * (1.0 + 0.002 * (i + 2))}}}};
                    t0 = Clock::now();
                    gpu::LookParams params;
                    std::string why;
                    ASSERT_TRUE(gpu::readLookParams(*session.viewDocument(), p.chain, o, &params, &why)) << why;
                    gpu::LookRun run = p.run;
                    run.params = std::move(params);
                    prepareMs.push_back(msSince(t0));
                    t0 = Clock::now();
                    engine->run(run, true, targets);
                    runCpu.push_back(msSince(t0));
                    engine->finish();
                    runDone.push_back(msSince(t0));
                    uploadMb.push_back(engine->runUploadMegabytes());
                    times.push_back(engine->times(true));
                }
                // Noise: bytes per point read and written (points in, points out: 24) plus the
                // per-strand numbers; a lower bound on the traffic.
                nlohmann::json row{{"bench", "gpu_from"},
                                   {"api", GetParam()},
                                   {"strands", strands},
                                   {"points", points},
                                   {"from", f.node},
                                   {"stages", p.chain.stages.size()},
                                   {"inPlace", inPlace},
                                   {"wroteInPlace", engine->wroteInPlace()},
                                   {"engineMadeMs", makeMs},
                                   {"baseUploadMs", baseMs},
                                   {"baseUploadMb", baseMb},
                                   {"firstRunCpuMs", firstRunCpu},
                                   {"firstRunDoneMs", firstRunDone},
                                   {"firstRunUploadMb", firstUploadMb},
                                   {"firstRunGpu", timesJson(firstTimes)},
                                   {"paramsReadMs", median(prepareMs)},
                                   {"runCpuMs", median(runCpu)},
                                   {"runDoneMs", median(runDone)},
                                   {"runUploadMb", median(uploadMb)},
                                   {"gpu", medianTimes(times)},
                                   {"videoMb", static_cast<double>(engine->videoBytes()) / 1e6}};
                const double noiseMs = row["gpu"]["noise"].get<double>();
                if (noiseMs > 0) row["noiseGBs"] = static_cast<double>(points) * 24.0 / (noiseMs * 1e-3) / 1e9;
                const double linesMs = row["gpu"]["lines"].get<double>();
                if (linesMs > 0) row["linesGBs"] = static_cast<double>(points) * (12.0 + 24.0) / (linesMs * 1e-3) / 1e9;
                record(row);
                engine->dropTargets();
                for (void* b : buffers) device->release(b);
            }
            // Noise and the frames a thread per strand against a thread per point.
            if (std::string(f.node) == "noise") {
                for (const bool pointThreads : {true, false}) {
                    engine->setPointThreads(pointThreads);
                    std::vector<gpu::GpuTimes> times;
                    for (int i = 0; i < 10; ++i) {
                        engine->run(p.run, true);
                        times.push_back(engine->times(true));
                    }
                    record({{"bench", "threads"}, {"api", GetParam()}, {"strands", strands}, {"pointThreads", pointThreads},
                            {"gpu", medianTimes(times)}});
                }
            }
        }

        // A brush dab end to end without a host: one guide moved, the CPU evaluates what feeds
        // Interpolate (stopAt), the inputs are read again (the guides' shapes), the GPU runs from
        // Interpolate to the output and is waited for.
        {
            gpu::LookInputs inputs;
            Prepared p;
            ASSERT_TRUE(prepareRun(session, {}, "interpolate", &p, inputs)) << p.why;
            auto engine = device->engine();
            engine->setBase(p.base, session.last().scalp.get());
            engine->run(p.run, true);
            engine->finish();
            std::vector<double> evalMs, readMs, runCpu, runDone, total;
            std::vector<gpu::GpuTimes> times;
            const StrandsPtr before = session.last().strands;
            for (int i = 0; i < 10; ++i) {
                auto moved = std::make_shared<GuideSet>(*data.guides);
                for (std::size_t g = 0; g < 8 && g < moved->guides().size(); ++g) {
                    // A comb: the points past the root move, the root stays (no rebinding).
                    std::vector<Vec3f>& pts = moved->guides()[(g * 37 + i) % moved->guides().size()].points;
                    for (std::size_t k = 1; k < pts.size(); ++k) pts[k] = pts[k] + Vec3f(0.05f * (i + 1) * static_cast<float>(k), 0, 0);
                }
                moved->rootOnMesh(*data.scalp);
                ViewRequest dab;
                dab.stopAt = "interpolate";
                const auto start = Clock::now();
                session.setGuides(moved);
                t0 = Clock::now();
                session.evaluate(dab);
                evalMs.push_back(msSince(t0));
                const NodeId guidesNode = session.viewDocument()->nodesByName.at("guides");
                t0 = Clock::now();
                Prepared q;
                if (!prepareRun(session, {}, "interpolate", &q, inputs, {guidesNode})) {
                    record({{"bench", "dab"}, {"api", GetParam()}, {"strands", strands}, {"refused", q.why}});
                    break;
                }
                readMs.push_back(msSince(t0));
                engine->setBase(q.base, session.last().scalp.get());
                t0 = Clock::now();
                engine->run(q.run, true);
                runCpu.push_back(msSince(t0));
                engine->finish();
                runDone.push_back(msSince(t0));
                total.push_back(msSince(start));
                times.push_back(engine->times(true));
            }
            if (!total.empty()) {
                record({{"bench", "dab"},
                        {"api", GetParam()},
                        {"strands", strands},
                        {"guidesMovedPerDab", 8},
                        {"cpuEvaluationToInterpolateMs", median(evalMs)},
                        {"inputsReadMs", median(readMs)},
                        {"inputsRebinds", inputs.rebinds()},
                        {"guidesMade", inputs.lastGuidesMade()},
                        {"runUploadMb", engine->runUploadMegabytes()},
                        {"runUploadMs", engine->runUploadMs()},
                        {"runCpuMs", median(runCpu)},
                        {"runDoneMs", median(runDone)},
                        {"totalMs", median(total)},
                        {"gpu", medianTimes(times)},
                        {"viewKept", session.last().strands == before}});
            }
            session.setGuides(data.guides);
        }
    }
}

// A shipped preset on the bench's head-sized sphere (fitted to it, as a host applies it), its
// guides bent, a part down one side, its Scatter's density set for about `strands` strands.
GroomData presetGroom(const std::string& name, std::size_t strands) {
    namespace fs = std::filesystem;
    std::ifstream in(fs::u8path(std::string(GROOMLAB_SHIPPED_PRESETS) + "/" + name + ".json"));
    std::stringstream text;
    text << in.rdbuf();
    GroomData d;
    d.scalp = test::makeSphere(64, 128, kRadius);
    d.guides = benchGuides(*d.scalp, 2.0f);
    PartSet parts;
    std::vector<Vec3f> meridian;
    for (int i = 0; i <= 10; ++i) {
        const float a = 0.15f + 1.2f * static_cast<float>(i) / 10.0f;
        meridian.push_back({0.0f, kRadius * std::cos(a), kRadius * std::sin(a)});
    }
    parts.parts.push_back(PartSet::fromPoints("meridian", meridian, *d.scalp));
    d.partsJson = parts.toJson();
    d.graphJson = presetGraphJson(text.str(), presetScalpSize(*d.scalp));
    test::paintRequiredMaps(text.str(), d);  // the maps a host makes on applying it, painted
    // The Scatter's density scaled to the count wanted (strands grow with it).
    GroomSession probe(d);
    const StrandsPtr made = probe.evaluate(ViewRequest{}).strands;
    if (!made || made->strandCount() == 0) return d;
    nlohmann::json g = nlohmann::json::parse(d.graphJson);
    for (auto& n : g["nodes"]) {
        if (n["type"] != "Scatter") continue;
        const double density = n["params"].value("density", 1.0);
        n["params"]["density"] = density * static_cast<double>(strands) / static_cast<double>(made->strandCount());
        break;
    }
    d.graphJson = g.dump();
    return d;
}

// One brush dab: `count` guides combed (their points past the root moved, the roots kept).
GuidesPtr combed(const GuideSet& guides, int dab, std::size_t count = 8) {
    auto moved = std::make_shared<GuideSet>(guides);
    for (std::size_t g = 0; g < count && g < moved->guides().size(); ++g) {
        std::vector<Vec3f>& pts = moved->guides()[(g * 37 + static_cast<std::size_t>(dab)) % moved->guides().size()].points;
        for (std::size_t k = 1; k < pts.size(); ++k) pts[k] = pts[k] + Vec3f(0.05f * static_cast<float>(dab + 1) * static_cast<float>(k), 0, 0);
    }
    return moved;
}

// The looks B4a and B4b moved onto the GPU (and hair_basics, already there): a brush dab on each at about
// GROOMLAB_BENCH_PRESET_STRANDS strands (500000 by default), on the CPU (a full incremental
// evaluation, what a dab costs where the GPU cannot take it) and on the GPU where it takes it (the
// evaluation up to Interpolate, the inputs read again, the run, the GPU done); a look without
// guides (creature_fur) its Clump's strength dragged instead. GROOMLAB_BENCH_PRESETS: a comma list.
TEST_P(GpuBench, PresetDabs) {
    if (!benchOn()) GTEST_SKIP() << "set GROOMLAB_BENCH=1 to run the benchmark";
    auto device = makeDevice(GetParam());
    if (!device->available()) GTEST_SKIP() << "no " << GetParam() << " device";
    std::size_t target = 500000;
    if (const char* v = std::getenv("GROOMLAB_BENCH_PRESET_STRANDS"); v && *v) target = static_cast<std::size_t>(std::stod(v));
    std::vector<std::string> names;
    {
        const char* v = std::getenv("GROOMLAB_BENCH_PRESETS");
        std::stringstream in(v && *v ? v
                                     : "creature_fur,curly,expression_driven,flow_with_guides,parted_style,hair_basics,gravity,flyaways,"
                                       "cinematic_hair,everyday_sim");
        std::string item;
        while (std::getline(in, item, ',')) {
            if (!item.empty()) names.push_back(item);
        }
    }
    constexpr int kDabs = 7;
    for (const std::string& name : names) {
        const GroomData data = presetGroom(name, target);
        nlohmann::json row{{"bench", "preset_dab"}, {"api", GetParam()}, {"preset", name}};
        // The CPU: each dab evaluated in full (incrementally), as without the GPU.
        {
            GroomSession session(data);
            const StrandsPtr out = session.evaluate(ViewRequest{}).strands;
            if (!out) {
                row["error"] = "no strands";
                record(row);
                continue;
            }
            row["strands"] = out->strandCount();
            std::vector<double> cpu;
            if (data.guides) {
                for (int i = 0; i < kDabs; ++i) {
                    session.setGuides(combed(*data.guides, i));
                    const auto t0 = Clock::now();
                    session.evaluate(ViewRequest{});
                    cpu.push_back(msSince(t0));
                }
            }
            row["cpuDabMs"] = median(cpu);
        }
        // The GPU: from Interpolate, each dab's guides evaluated up to it and run.
        GroomSession session(data);
        session.evaluate(ViewRequest{});
        gpu::LookInputs inputs;
        Prepared p;
        if (!prepareRun(session, {}, "", &p, inputs)) {
            row["refused"] = p.why;
            record(row);
            continue;
        }
        std::vector<std::string> stages;
        for (const auto& s : p.chain.stages) stages.push_back(gpu::stageName(s.kind));
        row["stages"] = stages;
        auto engine = device->engine();
        const bool guided = p.chain.stages.front().kind == gpu::StageKind::Interpolate;
        if (!guided) {
            // No guides shape it: its first node's strength dragged (a GPU run against a CPU tweak).
            const std::string node = session.viewDocument()->nameOf(p.chain.stages.front().node);
            engine->setBase(p.base, session.last().scalp.get());
            std::vector<double> gpuMs, cpuMs;
            std::vector<gpu::GpuTimes> times;
            for (int i = 0; i < kDabs; ++i) {
                const gpu::Overrides o{{node, {{"strength", 0.3 + 0.01 * i}}}};
                Prepared q;
                const auto t0 = Clock::now();
                if (!prepareRun(session, o, node, &q, inputs)) {
                    row["refused"] = q.why;
                    break;
                }
                engine->run(q.run, true);
                engine->finish();
                gpuMs.push_back(msSince(t0));
                times.push_back(engine->times(true));
            }
            GroomSession tweaked(data);
            tweaked.evaluate(ViewRequest{});
            for (int i = 0; i < kDabs; ++i) {
                nlohmann::json g = nlohmann::json::parse(data.graphJson);
                for (auto& n : g["nodes"]) {
                    if (n["name"] == node) n["params"]["strength"] = 0.3 + 0.01 * i;
                }
                tweaked.setGraphJson(g.dump());
                const auto t0 = Clock::now();
                tweaked.evaluate(ViewRequest{});
                cpuMs.push_back(msSince(t0));
            }
            row["dragNode"] = node;
            row["gpuDragMs"] = median(gpuMs);
            row["cpuDragMs"] = median(cpuMs);
            if (!times.empty()) row["gpu"] = medianTimes(times);
            record(row);
            continue;
        }
        // As a host does (look_preview.h): the plan made again for each dab (the mapped values read
        // once per evaluation, the inputs kept), the base uploaded where it changed, the run.
        gpu::LookPreview preview;
        const gpu::LookPreview::Shown shown = [&] { return session.last().strands; };
        const gpu::LookStart fromInterpolate{gpu::LookStart::From::Interpolate, {}};
        gpu::LookPlan plan;
        std::string why;
        if (!preview.plan(session, shown, {}, fromInterpolate, &plan, &why)) {
            row["refused"] = why;
            record(row);
            continue;
        }
        preview.upload(*engine, plan, session.last().scalp.get());
        engine->run(plan.run, true);
        engine->finish();
        std::vector<double> evalMs, readMs, total;
        std::vector<gpu::GpuTimes> times;
        for (int i = 0; i < kDabs; ++i) {
            ViewRequest dab;
            dab.stopAt = session.viewDocument()->nameOf(p.chain.stages.front().node);
            const auto start = Clock::now();
            session.setGuides(combed(*data.guides, i));
            auto t0 = Clock::now();
            session.evaluate(dab);
            evalMs.push_back(msSince(t0));
            t0 = Clock::now();
            gpu::LookPlan q;
            if (!preview.plan(session, shown, {}, fromInterpolate, &q, &why)) {
                row["refused"] = why;
                break;
            }
            readMs.push_back(msSince(t0));
            preview.upload(*engine, q, session.last().scalp.get());
            engine->run(q.run, true);
            engine->finish();
            total.push_back(msSince(start));
            times.push_back(engine->times(true));
        }
        if (!total.empty()) {
            row["gpuDabMs"] = median(total);
            row["cpuToInterpolateMs"] = median(evalMs);
            row["inputsReadMs"] = median(readMs);
            row["gpu"] = medianTimes(times);
        }
        record(row);
    }
}

// A dense groom's guide levels (GROOMLAB_BENCH_SCULPT_STRANDS strands, 50000 by default, from
// GROOMLAB_BENCH_SCULPT_GUIDES dense guides, 2000 by default): the hand guides through an even
// Scatter's Interpolate made into dense guides (ToGuides), a Sculpt node, then the benchmark's look.
std::string sculptBenchGraph(std::size_t strands, std::size_t guides) {
    const double area = 4.0 * 3.14159265 * kRadius * kRadius;
    nlohmann::json g = nlohmann::json::parse(benchGraph(static_cast<double>(strands) / area));
    g["nodes"].push_back({{"name", "even"}, {"type", "Scatter"},
                          {"params", {{"density", static_cast<double>(guides) / area}, {"randomness", 0.0}, {"length", 15.0}, {"previewAll", true}}}});
    g["nodes"].push_back({{"name", "dense"}, {"type", "Interpolate"}});
    g["nodes"].push_back({{"name", "toGuides"}, {"type", "ToGuides"}});
    g["nodes"].push_back({{"name", "sculpt"}, {"type", "SculptGuides"}});
    for (auto& c : g["connections"]) {
        if (c["from"] == "guides" && c["to"] == "interpolate") c["from"] = "sculpt";
    }
    auto link = [&](const char* from, const char* to, const char* input) {
        g["connections"].push_back({{"from", from}, {"to", to}, {"input", input}});
    };
    link("scalp", "even", "scalp");
    link("even", "dense", "strands");
    link("guides", "dense", "guides");
    link("scalp", "dense", "scalp");
    link("dense", "toGuides", "strands");
    link("toGuides", "sculpt", "guides");
    link("scalp", "sculpt", "scalp");
    return g.dump();
}

// A comb on the Sculpt node's guides (the core stroke API, as a host's brush tool drives it), its
// dabs on the CPU (each a full incremental evaluation of the stroke's edits: what a sculpt dab cost
// before the GPU took them; at the full density and at the hosts' default sculptDensity, 0.35) and
// on the GPU (the evaluation up to Interpolate, the Sculpt node alone; the plan with the guides'
// shapes; the upload; the run; the GPU done), with the GPU's last dab against the CPU's.
TEST_P(GpuBench, SculptDabs) {
    if (!benchOn()) GTEST_SKIP() << "set GROOMLAB_BENCH=1 to run the benchmark";
    auto device = makeDevice(GetParam());
    if (!device->available()) GTEST_SKIP() << "no " << GetParam() << " device";
    auto count = [](const char* name, std::size_t fallback) {
        const char* v = std::getenv(name);
        return v && *v ? static_cast<std::size_t>(std::stod(v)) : fallback;
    };
    const std::size_t strands = count("GROOMLAB_BENCH_SCULPT_STRANDS", 50000), guides = count("GROOMLAB_BENCH_SCULPT_GUIDES", 2000);
    GroomData data;
    data.scalp = test::makeSphere(64, 128, kRadius);
    data.graphJson = sculptBenchGraph(strands, guides);
    data.guides = benchGuides(*data.scalp, 3.0f);
    const ToolView view = ToolView::fromMatrix(test::lookAtProjection({0.0f, 45.0f, 30.0f}, {0.0f, 12.0f, 0.0f}, 40.0, 1.0), 1000, 1000);
    struct Mode {
        const char* name;
        bool gpu;
        float sculptDensity;
    };
    // A brush of 100 and of 300 pixels (of 1000): the guides a dab changes, and the strands they shape.
    for (const float radius : {100.0f, 300.0f})
    for (const Mode mode : {Mode{"cpu", false, 1.0f}, Mode{"cpu", false, 0.35f}, Mode{"gpu", true, 1.0f}}) {
        GroomSession session(data);
        SessionStrokeHost host(session);
        session.setSculptTarget("sculpt");
        ViewRequest request;
        request.sculptDensity = mode.sculptDensity;
        ASSERT_TRUE(session.evaluate(request).strands);
        const SculptInput in = session.sculptInput("sculpt");
        const Guide* top = &in.guides->guides()[0];
        for (const Guide& g : in.guides->guides()) {
            if (g.points[0].y > top->points[0].y) top = &g;
        }
        float cx = 0.0f, cy = 0.0f;
        ASSERT_TRUE(view.project(top->points[top->points.size() / 2], cx, cy));
        std::vector<std::array<float, 2>> line;
        for (int k = 0; k < 24; ++k) line.push_back({cx - 120.0f + 10.0f * static_cast<float>(k), cy + 2.0f * static_cast<float>(k)});
        BrushTool tool;
        tool.settings.setMode("comb");
        tool.settings.screenRadius = radius;
        tool.settings.brush.strength = 1.0f;
        gpu::LookPreview preview;
        const gpu::LookPreview::Shown shown = [&] { return session.last().strands; };
        auto engine = mode.gpu ? device->engine() : nullptr;
        bool pressed = false;
        std::vector<double> dabMs, brushMs, stopMs, planMs, uploadMs, runMs, changedGuides;
        std::vector<gpu::GpuTimes> times;
        std::vector<Vec3f> lastGpu;
        Clock::time_point left = Clock::now();
        ASSERT_TRUE(tool.stroke(host, view, line, 1.0f, 0, [&] {
            const auto t0 = Clock::now();
            if (!pressed) {  // the press shown (the CPU's); the GPU arms
                pressed = true;
                ASSERT_TRUE(session.evaluate(request).strands);
                if (mode.gpu) {
                    std::string why;
                    ASSERT_TRUE(preview.armStroke(session, shown, &why)) << why;
                    const gpu::LookPlan& plan = *preview.strokePlan();
                    preview.upload(*engine, plan, session.last().scalp.get());
                    engine->run(plan.run, true);  // (the first run uploads what the dabs keep)
                    engine->finish();
                }
                left = Clock::now();
                return;
            }
            brushMs.push_back(std::chrono::duration<double, std::milli>(t0 - left).count());
            if (!mode.gpu) {
                ASSERT_TRUE(session.evaluate(request).strands);
            } else {
                ViewRequest dab = request;
                dab.stopAt = preview.stopAt();
                session.evaluate(dab);
                const auto t1 = Clock::now();
                std::string why;
                ASSERT_TRUE(preview.strokeHolds(session, shown, &why)) << why;
                const auto t2 = Clock::now();
                const gpu::LookPlan& plan = *preview.strokePlan();
                preview.upload(*engine, plan, session.last().scalp.get());
                const auto t3 = Clock::now();
                engine->run(plan.run, true);
                engine->finish();
                const auto t4 = Clock::now();
                stopMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
                planMs.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
                uploadMs.push_back(std::chrono::duration<double, std::milli>(t3 - t2).count());
                runMs.push_back(std::chrono::duration<double, std::milli>(t4 - t3).count());
                times.push_back(engine->times(true));
                changedGuides.push_back(static_cast<double>(engine->lastWalk().changedGuides));
            }
            dabMs.push_back(msSince(t0));
            left = Clock::now();
        }));
        nlohmann::json row = {{"bench", "sculptDabs"}, {"api", GetParam()}, {"mode", mode.name}, {"sculptDensity", mode.sculptDensity},
                              {"brushRadius", radius},
                              {"guides", in.guides->size()}, {"dabs", dabMs.size()}, {"dabMs", median(dabMs)},
                              {"brushMs", median(brushMs)}};
        if (mode.gpu) {
            // The last dab's strands against the CPU's evaluation of the same edits (still live: the
            // stroke's are the committed ones now, the same).
            lastGpu = engine->readPositions();
            ASSERT_TRUE(session.evaluate(request).strands);
            const gpu::PointError e = gpu::compareVectors(lastGpu, session.last().strands->positions());
            row["stopAtMs"] = median(stopMs);
            row["planMs"] = median(planMs);
            row["uploadMs"] = median(uploadMs);
            row["runMs"] = median(runMs);
            row["gpu"] = medianTimes(times);
            row["errorMax"] = e.max;
            row["errorMean"] = e.mean;
            row["rebinds"] = preview.inputs().rebinds();
            row["changedGuides"] = median(changedGuides);  // (Interpolate's clearance walk: the guides a dab changed)
        }
        row["strands"] = session.last().strands->strandCount();
        row["points"] = session.last().strands->pointCount();
        record(row);
        EXPECT_EQ(host.edits().size(), 1u);
    }
}

INSTANTIATE_TEST_SUITE_P(Backends, GpuBench, ::testing::Values("DirectX11", "OpenGL"),
                         [](const ::testing::TestParamInfo<std::string>& info) { return info.param; });

// Which shipped presets the GPU previews from where, and why it stops (no device needed).
TEST(GpuBenchPresets, WhereEachPresetRunsOnTheGpu) {
    if (!benchOn()) GTEST_SKIP() << "set GROOMLAB_BENCH=1 to run the benchmark";
    namespace fs = std::filesystem;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(fs::u8path(GROOMLAB_SHIPPED_PRESETS))) {
        if (e.path().extension() == ".json") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& file : files) {
        std::ifstream in(file);
        std::stringstream text;
        text << in.rdbuf();
        GroomData d;
        d.scalp = test::makeSphere(32, 64, kRadius);
        d.guides = benchGuides(*d.scalp, 2.0f);
        nlohmann::json row{{"bench", "preset"}, {"preset", file.stem().u8string()}};
        try {
            d.graphJson = presetGraphJson(text.str(), presetScalpSize(*d.scalp));
            test::paintRequiredMaps(text.str(), d);
        } catch (const std::exception& e) {
            row["error"] = e.what();
            record(row);
            continue;
        }
        GroomSession session(d);
        ViewRequest request;
        request.density = 0.1f;
        request.viewport = true;
        const auto t0 = Clock::now();
        session.evaluate(request);
        row["evaluationMs"] = msSince(t0);
        const GraphDocument* doc = session.viewDocument();
        if (!doc || !session.last().strands) {
            row["error"] = "no strands";
            record(row);
            continue;
        }
        row["strands"] = session.last().strands->strandCount();
        gpu::LookChain chain;
        std::string why;
        if (!gpu::findLookChain(*doc, &chain, &why)) {
            row["gpu"] = false;
            row["why"] = why;
            record(row);
            continue;
        }
        std::vector<std::string> stages;
        for (const auto& s : chain.stages) stages.push_back(std::string(gpu::stageName(s.kind)) + ":" + doc->nameOf(s.node));
        row["stages"] = stages;
        row["base"] = doc->nameOf(chain.base);
        row["baseType"] = doc->graph.node(chain.base).typeName();
        // The whole chain (a brush dab needs it from Interpolate), then from each stage on.
        nlohmann::json from = nlohmann::json::object();
        for (std::size_t k = 0; k < chain.stages.size(); ++k) {
            gpu::LookChain c = gpu::chainFrom(*doc, chain, k);
            gpu::LookParams params;
            gpu::LookFields fields;
            std::vector<gpu::StageInputs> inputs;
            gpu::LookInputs reader;
            StrandsPtr base, output;
            std::string w;
            const bool ok = gpu::readLookParams(*doc, c, {}, &params, &w) && gpu::lookStrands(*doc, c, &base, &output, &w) &&
                            gpu::readLookFields(*doc, c, session.last().scalp.get(), &fields, &w) &&
                            gpu::lookFieldsHold(*doc, c, fields, &w) &&
                            reader.read(*doc, c, params, session.last().parts.get(), &inputs, &w, {}, session.last().scalp.get(),
                                        &fields);
            from[doc->nameOf(chain.stages[k].node)] = ok ? std::string("gpu") : w;
        }
        row["from"] = from;
        record(row);
    }
}

}  // namespace
}  // namespace groomlab
