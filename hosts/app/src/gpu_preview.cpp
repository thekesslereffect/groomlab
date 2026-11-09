// SPDX-License-Identifier: Apache-2.0
#include "gpu_preview.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

#include <QByteArray>
#include <QtGlobal>
#include <rhi/qrhi_platform.h>

#include "groom_document.h"
#include "groomlab/graph_io.h"

namespace groomlab::app {

bool gpuOverrideFromJson(const nlohmann::json& value, gpu::OverrideValue* out) {
    if (value.is_number() && !value.is_boolean()) {
        *out = value.get<double>();
        return true;
    }
    // Three numbers: a vec3 (a profile's keys are pairs, or an object).
    if (value.is_array() && value.size() == 3 &&
        std::all_of(value.begin(), value.end(), [](const nlohmann::json& c) { return c.is_number(); })) {
        *out = Vec3f(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
        return true;
    }
    try {
        *out = rampFromJson(value.dump());
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

namespace {

double since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// The native handle a QRhi buffer or texture wraps, as the engine takes it (look_engine.h
// LookTargets): the objects QRhi hands out are pointers to the native handles.
void* nativeBufferOf(QRhi* rhi, QRhiBuffer* buffer) {
    if (!buffer) return nullptr;
    const QRhiBuffer::NativeBuffer nb = buffer->nativeBuffer();
    if (nb.slotCount < 1 || !nb.objects[0]) return nullptr;
    switch (rhi->backend()) {
        case QRhi::D3D11: return *static_cast<void* const*>(nb.objects[0]);
        case QRhi::OpenGLES2: return reinterpret_cast<void*>(static_cast<std::uintptr_t>(*static_cast<const unsigned*>(nb.objects[0])));
        default: return nullptr;
    }
}

void* nativeTextureOf(QRhi* rhi, QRhiTexture* texture) {
    if (!texture) return nullptr;
    const QRhiTexture::NativeTexture nt = texture->nativeTexture();
    switch (rhi->backend()) {
        case QRhi::D3D11: return reinterpret_cast<void*>(static_cast<std::uintptr_t>(nt.object));
        case QRhi::OpenGLES2: return reinterpret_cast<void*>(static_cast<std::uintptr_t>(nt.object));
        default: return nullptr;
    }
}

// The device the engine is made on: the Direct3D 11 device, or the OpenGL context (the one
// current: QRhi makes it current for the frame).
void* deviceOf(QRhi* rhi) {
    if (!rhi) return nullptr;
    switch (rhi->backend()) {
#ifdef Q_OS_WIN
        case QRhi::D3D11: {
            const auto* handles = static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
            return handles ? handles->dev : nullptr;
        }
#endif
        case QRhi::OpenGLES2: {
            const auto* handles = static_cast<const QRhiGles2NativeHandles*>(rhi->nativeHandles());
            return handles ? handles->context : nullptr;
        }
        default: return nullptr;
    }
}

}  // namespace

GpuLookPreview::GpuLookPreview() = default;

GpuLookPreview::~GpuLookPreview() = default;

bool GpuLookPreview::supported(QRhi* rhi) {
#ifdef Q_OS_WIN
    return rhi && (rhi->backend() == QRhi::D3D11 || rhi->backend() == QRhi::OpenGLES2);
#else
    (void)rhi;
    return false;
#endif
}

bool GpuLookPreview::preference(bool fallback) {
    const QByteArray v = qgetenv("GROOMLAB_APP_GPU_PREVIEW");
    if (v == "0") return false;
    if (v == "1") return true;
    return fallback;
}

void GpuLookPreview::setEnabled(bool on) {
    if (enabled_ == on) return;
    enabled_ = on;
    if (!on) {
        endDrag();
        endStroke();
    }
}

void GpuLookPreview::setDocument(GroomDocument* document) {
    if (document_ != document) {
        // Another groom: nothing of the old one's cache holds (the plans and the base).
        overrides_.clear();
        plan_.reset();
        core_.reset();
        bounds_ = gpu::LookBounds{};
    }
    document_ = document;
}

void GpuLookPreview::sessionDocumentSwapped() {
    plan_.reset();
    core_.reset();  // (the stroke's plan, the base and the inputs read from the old document)
    if (overrides_.empty() || !document_) return;
    auto p = std::make_unique<gpu::LookPlan>();
    std::string why;
    if (plan(overrides_, p.get(), &why)) {
        plan_ = std::move(p);
    } else {
        overrides_.clear();
        why_ = why;
    }
    bump();
}

std::string GpuLookPreview::api() const { return engine_ ? gpu::apiName(engine_->api()) : std::string(); }

std::size_t GpuLookPreview::outputPoints() const {
    const gpu::LookPlan* p = current();
    return p && p->output ? p->output->pointCount() : 0;
}

// ---- Planning -----------------------------------------------------------------------------------

gpu::LookPreview::Shown GpuLookPreview::shown() const {
    // (The view must show the graph's output, as a run stands for it: not a node viewed, not
    // strands carried elsewhere.)
    const GroomDocument* document = document_;
    return [document] { return document->lastView().strands; };
}

bool GpuLookPreview::plan(const gpu::Overrides& overrides, gpu::LookPlan* out, std::string* why) {
    if (!document_) {
        *why = "no groom open";
        return false;
    }
    return core_.plan(document_->session(), shown(), overrides, {}, out, why);
}

// ---- Drags ---------------------------------------------------------------------------------------

bool GpuLookPreview::drag(const std::string& node, const std::string& param, const gpu::OverrideValue& value, std::string* why) {
    if (!enabled_) {
        *why = why_ = "the GPU preview is off";
        return false;
    }
    if (!engineWhy_.empty()) {
        // The device has no engine (a kernel its driver refused): the CPU's, until another device.
        *why = why_ = engineWhy_;
        return false;
    }
    if (core_.strokeArmed()) {
        *why = why_ = "a brush stroke is live";
        return false;
    }
    gpu::Overrides overrides = overrides_;
    overrides[node][param] = value;
    auto p = std::make_unique<gpu::LookPlan>();
    if (!plan(overrides, p.get(), why)) {
        why_ = *why;
        return false;
    }
#ifdef Q_OS_WIN
    // OpenGL: the kernels the drag runs compiled (prepare); until they are, the CPU previews it
    // (its next values ask again).
    if (gl_ && !gpu::glLookKernelsReady(p->run, why)) {
        why_ = *why;
        return false;
    }
#endif
    overrides_ = std::move(overrides);
    plan_ = std::move(p);
    why_.clear();
    bump();
    return true;
}

void GpuLookPreview::endDrag() {
    if (overrides_.empty()) return;
    overrides_.clear();
    plan_.reset();
    ran_ = version_;  // nothing pending: the CPU's commit draws next
}

// ---- Strokes -------------------------------------------------------------------------------------

bool GpuLookPreview::armStroke(std::string* why) {
    if (core_.strokeArmed()) return true;
    if (!enabled_) {
        *why = "the GPU preview is off";
        return false;
    }
    if (core_.strokeRefused()) {
        *why = why_;
        return false;
    }
    if (!engineWhy_.empty()) {
        *why = why_ = engineWhy_;
        core_.refuseStroke();
        return false;
    }
    if (!document_) {
        *why = why_ = "the stroke does not shape guides";
        core_.refuseStroke();
        return false;
    }
    if (!core_.armStroke(document_->session(), shown(), why)) {
        why_ = *why;
        return false;
    }
#ifdef Q_OS_WIN
    // OpenGL: the kernels the stroke runs compiled (prepare); until they are, the stroke is the
    // CPU's whole (its dabs' evaluation would stop at Interpolate).
    if (gl_ && !gpu::glLookKernelsReady(core_.strokePlan()->run, why)) {
        why_ = *why;
        core_.refuseStroke();
        return false;
    }
#endif
    overrides_.clear();
    plan_.reset();
    why_.clear();
    return true;
}

bool GpuLookPreview::strokeHolds() {
    if (!core_.strokeArmed() || !document_) return false;
    std::string why;
    if (!core_.strokeHolds(document_->session(), shown(), &why)) {
        // The CPU's from here: this dab, and the rest of the stroke.
        why_ = why;
        ran_ = version_;
        return false;
    }
    bump();
    return true;
}

void GpuLookPreview::endStroke() {
    if (!core_.strokeArmed() && !core_.strokeRefused()) return;
    core_.endStroke();
    plan_.reset();
    ran_ = version_;
}

// ---- Rendering -----------------------------------------------------------------------------------

bool GpuLookPreview::ensureEngine(QRhi* rhi, std::string* why) {
    // The engine on the viewport's device: made on the first run, and again for another device
    // (another QRhi) or an OpenGL context it cannot reach.
    void* device = deviceOf(rhi);
    if (!device) {
        *why = "no device the viewport draws with";
        return false;
    }
    const bool sameDevice = engine_ && device_ == device && engine_->reachable();
    if (sameDevice) return true;
    engine_.reset();
    core_.forgetBase();
    try {
#ifdef Q_OS_WIN
        engine_ = rhi->backend() == QRhi::OpenGLES2 ? gpu::makeGLLookEngine()
                                                    : gpu::makeD3D11LookEngine(static_cast<ID3D11Device*>(device));
#else
        *why = "no engine for this platform yet";
        return false;
#endif
    } catch (const std::exception& e) {
        // Remembered: the next drag or stroke is the CPU's at once, rather than a frame drawn
        // with stale strands and the kernels compiled again each time.
        *why = engineWhy_ = e.what();
        return false;
    }
    device_ = device;
    engineWhy_.clear();
    return true;
}

bool GpuLookPreview::run(QRhi* rhi, const Targets& targets, std::string* why) {
    const auto start = std::chrono::steady_clock::now();
    if (!current() || !document_) {
        *why = why_ = "nothing to run";
        ran_ = version_;
        return false;
    }
    if (!ensureEngine(rhi, why)) {
        why_ = *why;
        ran_ = version_;
        return false;
    }
    const gpu::LookPlan& p = *current();
    if (targets.pointCount != p.output->pointCount()) {
        *why = why_ = "the strands drawn are not the graph's output";
        ran_ = version_;
        return false;
    }
    if (!targets.pointTexture && !targets.positions) {
        *why = why_ = "nothing to write";
        ran_ = version_;
        return false;
    }
    const MeshPtr scalp = document_->session().last().scalp;
    try {
        // OpenGL's kernels still compiling: the CPU's this time (nothing waits for them).
        if (!engine_->kernelsReady(p.run, why)) {
            why_ = *why;
            ran_ = version_;
            return false;
        }
        core_.upload(*engine_, p, scalp.get());  // after a brush stroke's incremental evaluation: its strands alone
        uploadPartial_ = engine_->lastUploadPartial();
        uploadStrands_ = engine_->lastUploadStrands();
        uploadMs_ = engine_->uploadMs();
        bounds_ = gpu::lookBounds(p.run.params, p.run.fields, p.run.inputs, *p.base);
        gpu::LookTargets t;
        const bool frames = targets.normals && targets.tangents;
        t.positions = nativeBufferOf(rhi, targets.positions);
        t.normals = frames ? nativeBufferOf(rhi, targets.normals) : nullptr;
        t.tangents = frames ? nativeBufferOf(rhi, targets.tangents) : nullptr;
        t.pointTexture = nativeTextureOf(rhi, targets.pointTexture);
        t.uvs = targets.uvs ? nativeBufferOf(rhi, targets.uvs) : nullptr;
        t.textureWidth = static_cast<std::uint32_t>(targets.textureWidth);
        if ((targets.positions && !t.positions) || (targets.pointTexture && !t.pointTexture)) {
            *why = why_ = "the viewport's buffers have no native handles";
            ran_ = version_;
            return false;
        }
        engine_->run(p.run, frames, t);
    } catch (const std::exception& e) {
        // A run that cannot happen on this device (a target it cannot write): the rest of the
        // drag or stroke is the CPU's.
        *why = why_ = e.what();
        if (!overrides_.empty()) overrides_.clear();
        core_.refuseStroke();
        plan_.reset();
        ran_ = version_;
        return false;
    }
    ran_ = version_;
    wrote_ = version_;
    wroteTexture_ = targets.pointTexture != nullptr && !core_.strokeArmed();
    ++runs_;
    runMs_ = since(start);
    why_.clear();
    return true;
}

void GpuLookPreview::prepare(QRhi* rhi) {
#ifdef Q_OS_WIN
    gl_ = rhi && rhi->backend() == QRhi::OpenGLES2;
    if (gl_ && enabled_ && deviceOf(rhi)) gpu::prepareGLLookKernels();
#else
    (void)rhi;
#endif
}

gpu::GpuTimes GpuLookPreview::gpuTimes(bool wait) { return engine_ ? engine_->times(wait) : gpu::GpuTimes{}; }

void GpuLookPreview::releaseResources() {
    if (engine_) engine_->dropTargets();
    engine_.reset();
    device_ = nullptr;
    engineWhy_.clear();  // another device may have an engine
    core_.forgetBase();
}

}  // namespace groomlab::app
