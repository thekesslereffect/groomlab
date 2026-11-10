// SPDX-License-Identifier: Apache-2.0
#pragma once

// GPU look preview in the standalone app: the host glue for engine/gpu/'s LookEngine (look_engine.h,
// look_chain.h) on the device QRhi draws with (QRhi::nativeHandles: the Direct3D 11 device, the
// OpenGL context current while the viewport renders; Vulkan and Metal engines would plug in
// here the same way once engine/gpu/ has them). The planning, the stroke's arming and the base
// the engine holds are look_preview.h's, as in Maya's.
//
// Drags. While a param of a node the GPU runs (Interpolate's radius and falloff, Length, Clump,
// Noise, Curl, Width, their seeds and profiles: look_chain.h previewableParam) is dragged, the document holds the value outside the
// graph (GroomDocument::livePreview: no evaluation, no undo) and `drag` keeps it as an override;
// the renderer's next frame runs that node and every one after it on the GPU from the strands
// entering it (the last evaluation's cached result, uploaded once; only the changed strands
// after a stroke) straight into what the viewport draws: the lookdev path's point texture
// (the kernels write its RGBA32F layout) or the line path's vertex buffers. The release commits
// the value on the CPU (bit-exact, the reference) and endDrag drops the overrides.
//
// Strokes. Once a stroke's press is evaluated and shown (the CPU's, at the stroke's density),
// armStroke plans the run from Interpolate; each later dab's evaluation stops at Interpolate
// (ViewRequest::stopAt: the guides alone are evaluated, the view keeps its strands) and
// strokeHolds checks the dab's guides fit what the GPU holds (else the dab, and the rest of the
// stroke, is the CPU's); the renderer runs the plan into the buffers it keeps. The release
// evaluates on the CPU as ever and replaces the preview.
//
// A preference: the Display panel's "GPU preview" (on by default where the device has an
// engine), GROOMLAB_APP_GPU_PREVIEW=0|1 forcing it; the app's --gpu-preview on|off.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <rhi/qrhi.h>

#include "groomlab/gpu/look_chain.h"
#include "groomlab/gpu/look_engine.h"
#include "groomlab/gpu/look_preview.h"
#include "groomlab/groom_session.h"

namespace groomlab::app {

class GroomDocument;

// A param's dragged value (GroomDocument::livePreview's JSON) as the GPU preview takes it: a
// number, three numbers (a vec3: Curl's weightDirection) or a profile's keys (a ramp). False for
// anything else (the CPU previews it).
bool gpuOverrideFromJson(const nlohmann::json& value, gpu::OverrideValue* out);

class GpuLookPreview {
public:
    GpuLookPreview();
    ~GpuLookPreview();
    GpuLookPreview(const GpuLookPreview&) = delete;
    GpuLookPreview& operator=(const GpuLookPreview&) = delete;

    // Whether engine/gpu/ has an engine for this QRhi's backend (Direct3D 11, OpenGL on Windows).
    static bool supported(QRhi* rhi);
    // GROOMLAB_APP_GPU_PREVIEW in the environment (0 or 1), else `fallback`.
    static bool preference(bool fallback);

    void setEnabled(bool on);
    bool enabled() const { return enabled_; }
    // The document the previews read (null: none); a drag or stroke in flight is dropped.
    void setDocument(GroomDocument* document);
    // The session took another copy of its graph document (a commit off the UI thread landed:
    // GroomSession::adopt), and the plans name the one it let go: everything planned goes, a
    // drag in flight plans again from the new one (its overrides kept), an armed stroke arms
    // again at its next dab.
    void sessionDocumentSwapped();

    // ---- Drags -----------------------------------------------------------------------------
    // A param's live value (a number, a seed, a profile's ramp, a vector: look_chain.h previewableParam):
    // held as an override (the first value of a node plans the run: the chain, its params and
    // inputs) and previewed by the next frame. False with *why when the GPU cannot (off, no
    // engine, a node or param it does not run, a stroke live, ...): the caller previews on the
    // CPU as before.
    bool drag(const std::string& node, const std::string& param, const gpu::OverrideValue& value, std::string* why);
    bool dragging() const { return !overrides_.empty(); }
    // The last successful run wrote the drag's latest value into a point texture (the lookdev
    // path's): a release committing that value finds those points already drawn.
    bool drewLatestValue() const { return !overrides_.empty() && wroteTexture_ && wrote_ == version_; }
    // The drag ended: the overrides go (the caller committed on the CPU; its evaluation
    // replaces the preview).
    void endDrag();

    // ---- Strokes ---------------------------------------------------------------------------
    // After a stroke's press was evaluated and shown: whether the GPU takes the next dabs.
    bool armStroke(std::string* why);
    bool strokeArmed() const { return core_.strokeArmed(); }
    // The node a dab's evaluation stops at while armed (the Interpolate the GPU runs from).
    const std::string& stopAt() const { return core_.stopAt(); }
    // After an evaluation that stopped there: whether the GPU draws this dab (the plan is kept
    // for the frame). False: the dab is the CPU's, and so is the rest of the stroke.
    bool strokeHolds();
    // The stroke is over (released, or dropped): the plan goes, the next press may arm again.
    void endStroke();

    // ---- Rendering -------------------------------------------------------------------------
    // A run is wanted: a drag value or a dab since the last run.
    bool pending() const { return version_ != ran_; }
    // The points of the strands the pending run stands for (the graph's output as shown): the
    // host picks the item (or texture) holding that many.
    std::size_t outputPoints() const;
    // Where the next run writes: the lookdev path's point texture (an RGBA32F texel a point,
    // `textureWidth` wide), or the line path's vertex buffers (float3 a point; the frames when
    // lit). `pointCount` says what they hold (the run's output must match).
    struct Targets {
        QRhiBuffer* positions = nullptr;
        QRhiBuffer* normals = nullptr;
        QRhiBuffer* tangents = nullptr;
        QRhiTexture* pointTexture = nullptr;
        int textureWidth = 0;
        std::size_t pointCount = 0;
        QRhiBuffer* uvs = nullptr;  // the strand lines' (u, width): a Width the run has writes its widths there
    };
    // Runs the pending plan on `rhi`'s device into `targets` (the engine made on first use, and
    // again for another device). Inside the host's frame on the render thread (OpenGL: the
    // context current). False with *why when it could not (nothing was written): the caller
    // keeps drawing the CPU's strands.
    bool run(QRhi* rhi, const Targets& targets, std::string* why);
    // Each frame, before any run (the render thread; OpenGL: the context current): OpenGL's
    // kernels started, from the program cache or compiled by the driver in the background, so the
    // first drag or stroke finds them ready (a stroke armed before is the CPU's whole).
    void prepare(QRhi* rhi);
    // Whether every kernel is ready on the device the last frame drew with (OpenGL: compiled, as
    // prepare last found them; DirectX 11: always).
#ifdef Q_OS_WIN
    bool kernelsReady() const { return !gl_ || gpu::glLookKernelsAllReady(); }
#elif defined(GROOMLAB_HAS_METAL)
    bool kernelsReady() const { return !metalDevice_ || gpu::prepareMetalLookKernels(metalDevice_); }
#else
    bool kernelsReady() const { return true; }
#endif
    // The box around everything the last run can have drawn (lookBounds): the host's shadow
    // maps fit it, so hair the run pushed past the CPU's bounds keeps its shadows.
    const gpu::LookBounds& bounds() const { return bounds_; }
    // The last run's CPU side (planning, uploads, dispatch) in milliseconds, and the GPU's own
    // times (`wait`: blocks until the GPU has them).
    double lastRunMs() const { return runMs_; }
    gpu::GpuTimes gpuTimes(bool wait);
    std::size_t runs() const { return runs_; }
    // The last run's base upload: partial, and how many strands went up.
    bool lastUploadPartial() const { return uploadPartial_; }
    std::size_t lastUploadStrands() const { return uploadStrands_; }
    double lastUploadMs() const { return uploadMs_; }
    // Why the last drag, stroke or run was refused ("" when it was not).
    const std::string& why() const { return why_; }
    // The API the engine runs on ("DirectX11", "OpenGL"), "" without one.
    std::string api() const;
    // The engine goes (the QRhi is going away: the host's releaseResources).
    void releaseResources();

private:
    // The run from the first stage `overrides` change (a drag's).
    bool plan(const gpu::Overrides& overrides, gpu::LookPlan* out, std::string* why);
    // The strands the viewport shows (the document's last view).
    gpu::LookPreview::Shown shown() const;
    // What a run takes now: the armed stroke's plan, else the drag's.
    const gpu::LookPlan* current() const { return core_.strokeArmed() ? core_.strokePlan() : plan_.get(); }
    bool ensureEngine(QRhi* rhi, std::string* why);
    void bump() { ++version_; }

    GroomDocument* document_ = nullptr;
    bool enabled_ = true;
    std::unique_ptr<gpu::LookEngine> engine_;
    void* device_ = nullptr;  // the QRhi device the engine was made on
    std::string engineWhy_;   // why no engine could be made on it ("" while one could, or none was tried)
    std::atomic<bool> gl_{false};  // the viewport draws through OpenGL (prepare)
    std::atomic<void*> metalDevice_{nullptr};  // the viewport's Metal device (prepare; null: not Metal)
    gpu::LookPreview core_;   // planning, the stroke, the base the engine holds
    gpu::Overrides overrides_;
    std::unique_ptr<gpu::LookPlan> plan_;  // the drag's
    std::uint64_t version_ = 0, ran_ = 0;
    std::uint64_t wrote_ = 0;    // the version the last successful run stood for
    bool wroteTexture_ = false;  // it wrote a point texture (not the line path's buffers)
    gpu::LookBounds bounds_;
    std::string why_;
    double runMs_ = 0.0, uploadMs_ = 0.0;
    std::size_t runs_ = 0, uploadStrands_ = 0;
    bool uploadPartial_ = false;
};

}  // namespace groomlab::app
