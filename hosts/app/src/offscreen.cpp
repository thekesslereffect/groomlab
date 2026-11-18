// SPDX-License-Identifier: Apache-2.0
#include "offscreen.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <rhi/qrhi_platform.h>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#endif
#include <cstdio>

#include "groomlab/profile.h"

namespace groomlab::app {

namespace {

// Waits until the device has done everything submitted: a benchmark's frame time is then the
// whole frame's, as an artist sees it. QRhi::finish executes what was recorded and flushes
// but does not wait on Direct3D 11 or 12, so the wait is the device's own: an event query on 11,
// a fence signaled on the queue on 12, glFinish on OpenGL; elsewhere a readback would (the Null
// backend has nothing to wait for).
void waitForGpu(QRhi* rhi) {
    rhi->finish();
#ifdef Q_OS_WIN
    if (rhi->backend() == QRhi::D3D11) {
        const auto* handles = static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
        auto* device = handles ? static_cast<ID3D11Device*>(handles->dev) : nullptr;
        auto* context = handles ? static_cast<ID3D11DeviceContext*>(handles->context) : nullptr;
        if (!device || !context) return;
        D3D11_QUERY_DESC q{};
        q.Query = D3D11_QUERY_EVENT;
        ID3D11Query* event = nullptr;
        if (FAILED(device->CreateQuery(&q, &event))) return;
        context->End(event);
        context->Flush();
        BOOL done = FALSE;
        while (context->GetData(event, &done, sizeof done, 0) != S_OK || !done) {
        }
        event->Release();
        return;
    }
    if (rhi->backend() == QRhi::D3D12) {
        // A fence signaled on QRhi's queue after what it submitted, waited for.
        const auto* handles = static_cast<const QRhiD3D12NativeHandles*>(rhi->nativeHandles());
        auto* device = handles ? static_cast<ID3D12Device*>(handles->dev) : nullptr;
        auto* queue = handles ? static_cast<ID3D12CommandQueue*>(handles->commandQueue) : nullptr;
        if (!device || !queue) return;
        ID3D12Fence* fence = nullptr;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence)))) return;
        HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (done && SUCCEEDED(queue->Signal(fence, 1)) && fence->GetCompletedValue() < 1 && SUCCEEDED(fence->SetEventOnCompletion(1, done))) {
            WaitForSingleObject(done, INFINITE);
        }
        if (done) CloseHandle(done);
        fence->Release();
        return;
    }
#endif
    if (rhi->backend() == QRhi::OpenGLES2) {
        if (QOpenGLContext* context = QOpenGLContext::currentContext()) context->functions()->glFinish();
    }
}

// With GROOMLAB_APP_RHI_DEBUG (the debug layer on): the layer's messages since the last call on
// stderr (they go to the debugger's output otherwise, which a scripted run does not see).
void printDebugMessages(QRhi* rhi) {
#ifdef Q_OS_WIN
    static const bool on = qEnvironmentVariableIntValue("GROOMLAB_APP_RHI_DEBUG") != 0;
    if (!on) return;
    auto drain = [](auto* queue, auto getMessage) {
        if (!queue) return;
        const UINT64 count = queue->GetNumStoredMessages();
        for (UINT64 i = 0; i < count && i < 64; ++i) {
            SIZE_T length = 0;
            if (FAILED(getMessage(queue, i, nullptr, &length)) || length == 0) continue;
            QByteArray storage(static_cast<qsizetype>(length), Qt::Uninitialized);
            if (SUCCEEDED(getMessage(queue, i, storage.data(), &length))) {
                const auto* m = reinterpret_cast<const D3D12_MESSAGE*>(storage.constData());
                std::fprintf(stderr, "GroomLab: debug layer: %.*s\n", static_cast<int>(m->DescriptionByteLength), m->pDescription);
            }
        }
        queue->ClearStoredMessages();
    };
    if (rhi->backend() == QRhi::D3D12) {
        const auto* handles = static_cast<const QRhiD3D12NativeHandles*>(rhi->nativeHandles());
        auto* device = handles ? static_cast<ID3D12Device*>(handles->dev) : nullptr;
        ID3D12InfoQueue* queue = nullptr;
        if (device && SUCCEEDED(device->QueryInterface(__uuidof(ID3D12InfoQueue), reinterpret_cast<void**>(&queue)))) {
            drain(queue, [](ID3D12InfoQueue* q, UINT64 i, void* m, SIZE_T* n) { return q->GetMessage(i, static_cast<D3D12_MESSAGE*>(m), n); });
            queue->Release();
        }
    } else if (rhi->backend() == QRhi::D3D11) {
        const auto* handles = static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
        auto* device = handles ? static_cast<ID3D11Device*>(handles->dev) : nullptr;
        ID3D11InfoQueue* queue = nullptr;
        if (device && SUCCEEDED(device->QueryInterface(__uuidof(ID3D11InfoQueue), reinterpret_cast<void**>(&queue)))) {
            // (D3D11_MESSAGE has the same leading layout as D3D12_MESSAGE.)
            drain(queue, [](ID3D11InfoQueue* q, UINT64 i, void* m, SIZE_T* n) { return q->GetMessage(i, static_cast<D3D11_MESSAGE*>(m), n); });
            queue->Release();
        }
    }
#else
    (void)rhi;
#endif
}

}  // namespace

std::unique_ptr<QRhi> createOffscreenRhi() {
    const QByteArray name = qgetenv("GROOMLAB_APP_RHI").toLower();
    // GROOMLAB_APP_RHI_DEBUG=1: the API's validation layer (Direct3D's debug layer), its messages
    // on the debugger's output and stderr.
    const bool debugLayer = qEnvironmentVariableIntValue("GROOMLAB_APP_RHI_DEBUG") != 0;
    std::unique_ptr<QRhi> rhi;
#ifdef Q_OS_WIN
    if (name.isEmpty() || name == "d3d11" || name == "direct3d11") {
        QRhiD3D11InitParams params;
        params.enableDebugLayer = debugLayer;
        rhi.reset(QRhi::create(QRhi::D3D11, &params, QRhi::EnableTimestamps));
    } else if (name == "d3d12" || name == "direct3d12") {
        QRhiD3D12InitParams params;
        params.enableDebugLayer = debugLayer;
        if (qEnvironmentVariableIntValue("GROOMLAB_APP_RHI_DEBUG") >= 2) {
            // GPU-based validation too (the descriptors and resource states each draw reads).
            ID3D12Debug1* debug = nullptr;
            if (SUCCEEDED(D3D12GetDebugInterface(__uuidof(ID3D12Debug1), reinterpret_cast<void**>(&debug)))) {
                debug->EnableDebugLayer();
                debug->SetEnableGPUBasedValidation(TRUE);
                debug->Release();
            }
        }
        rhi.reset(QRhi::create(QRhi::D3D12, &params, QRhi::EnableTimestamps));
    }
#endif
#ifdef Q_OS_MACOS
    if (name.isEmpty() || name == "metal") {  // macOS: Metal, as the viewport's QRhiWidget picks
        QRhiMetalInitParams params;
        rhi.reset(QRhi::create(QRhi::Metal, &params, QRhi::EnableTimestamps));
    }
#endif
    if (!rhi && (name == "opengl" || name == "gl")) {
        // OpenGL without a window: a fallback surface of QRhi's own (an offscreen one, or a
        // hidden window where the platform has no offscreen surfaces).
        static QOffscreenSurface* fallback = QRhiGles2InitParams::newFallbackSurface();
        QRhiGles2InitParams params;
        params.fallbackSurface = fallback;
        rhi.reset(QRhi::create(QRhi::OpenGLES2, &params, QRhi::EnableTimestamps));
    }
    if (!rhi) {
        QRhiNullInitParams params;
        rhi.reset(QRhi::create(QRhi::Null, &params));
    }
    return rhi;
}

OffscreenView::OffscreenView(const QSize& size, QRhi* rhi, int samples) : rhi_(rhi), size_(size) {
    if (!rhi_) {
        own_ = createOffscreenRhi();
        rhi_ = own_.get();
    }
    if (!rhi_) return;
    texture_.reset(rhi_->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!texture_->create()) return;
    samples = std::max(samples, 1);
    depth_.reset(rhi_->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size, samples));
    if (!depth_->create()) return;
    QRhiColorAttachment color(texture_.get());
    if (samples > 1) {
        color_.reset(rhi_->newRenderBuffer(QRhiRenderBuffer::Color, size, samples, {}, QRhiTexture::RGBA8));
        if (!color_->create()) return;
        color = QRhiColorAttachment(color_.get());
        color.setResolveTexture(texture_.get());
    }
    QRhiTextureRenderTargetDescription description(color);
    description.setDepthStencilBuffer(depth_.get());
    std::unique_ptr<QRhiTextureRenderTarget> target(rhi_->newTextureRenderTarget(description));
    pass_.reset(target->newCompatibleRenderPassDescriptor());
    target->setRenderPassDescriptor(pass_.get());
    if (target->create()) target_ = std::move(target);
}

OffscreenView::~OffscreenView() = default;

OffscreenResult OffscreenView::frame(GroomRenderer& renderer, const OrbitCamera& camera, bool readback) {
    OffscreenResult result;
    if (!rhi_ || !target_) return result;
    result.api = QString::fromLatin1(rhi_->backendName());
    QRhiReadbackResult read;
    QElapsedTimer timer;
    timer.start();
    QRhiCommandBuffer* cb = nullptr;
    if (rhi_->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) return result;
    // The profiler's rows (profile.h): the renderer's CPU side (its uploads and the passes it
    // records, "app.render.*" inside it), the frame handed to the device, the wait for the GPU to
    // finish it (everything it ran: the passes, and what a strand change makes it build again).
    ProfilePhases phase("app.frame.render");
    renderer.render(rhi_, cb, target_.get(), camera);
    phase.next("app.frame.submit");
    if (readback) {
        QRhiResourceUpdateBatch* batch = rhi_->nextResourceUpdateBatch();
        batch->readBackTexture(QRhiReadbackDescription(texture_.get()), &read);
        cb->resourceUpdate(batch);
    }
    rhi_->endOffscreenFrame();
    phase.next("app.frame.waitGpu");
    waitForGpu(rhi_);  // the time is the whole frame's
    phase.stop();
    printDebugMessages(rhi_);
    result.firstFrameMs = result.msPerFrame = timer.nsecsElapsed() / 1e6;
    // (The last frame whose timestamps came back: this one, or on some backends the one before.)
    result.gpuMsPerFrame = cb->lastCompletedGpuTime() * 1000.0;
    result.frames = 1;
    if (readback && rhi_->backend() != QRhi::Null) {
        const ProfileScope scope("app.frame.readImage");
        result.image = imageFromReadback(read, rhi_->isYUpInFramebuffer());
    }
    return result;
}

OffscreenResult renderOffscreen(GroomRenderer& renderer, const OrbitCamera& camera, const QSize& size, int frames, QRhi* given,
                                int samples) {
    OffscreenResult result;
    OffscreenView view(size, given, samples);
    if (!view.rhi()) return result;
    result.api = QString::fromLatin1(view.rhi()->backendName());
    if (!view.ready()) return result;
    double total = 0.0, gpuTotal = 0.0;
    int gpuFrames = 0;
    const int count = std::max(frames, 1);
    for (int frame = 0; frame < count; ++frame) {
        const OffscreenResult one = view.frame(renderer, camera, frame + 1 == count);
        if (one.frames == 0) break;
        if (frame == 0) {
            result.firstFrameMs = one.firstFrameMs;
        } else {
            total += one.firstFrameMs;
            // (From the third frame: the second's report may still be the first frame's.)
            if (frame >= 2 && one.gpuMsPerFrame > 0.0) {
                gpuTotal += one.gpuMsPerFrame;
                ++gpuFrames;
            }
        }
        ++result.frames;
        if (frame + 1 == count) result.image = one.image;
    }
    result.msPerFrame = result.frames > 1 ? total / (result.frames - 1) : result.firstFrameMs;
    result.gpuMsPerFrame = gpuFrames > 0 ? gpuTotal / gpuFrames : 0.0;
    // The renderer's objects belong to this QRhi: released before it goes (a given one stays).
    if (!given) renderer.releaseResources();
    return result;
}

}  // namespace groomlab::app
