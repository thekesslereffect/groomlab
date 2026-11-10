// SPDX-License-Identifier: Apache-2.0
// The Metal backends' shared parts (look_engine_metal.mm, line_frames_metal.mm; macOS, Objective-C++
// with ARC): the kernels compiled in the background from Slang's Metal Shading Language, a device's
// command stream, the runs pending.
#pragma once

#import <Metal/Metal.h>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace groomlab::gpu::metal {

// A kernel: its Metal Shading Language (NUL-terminated), its threadgroup (x, y, z) and entry point.
struct KernelSource {
    const char* source;
    const unsigned* group;
    const char* entry;
};

[[noreturn]] inline void fail(const std::string& what) { throw std::runtime_error("GPU preview (Metal): " + what); }

inline MTLCompileOptions* compileOptions() {
    // The kernels decide as the CPU does where they must (look.slang's exactDot): no fast math.
    MTLCompileOptions* options = [MTLCompileOptions new];
    if (@available(macOS 15.0, *)) {
        options.mathMode = MTLMathModeSafe;
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        options.fastMathEnabled = NO;
#pragma clang diagnostic pop
    }
    return options;
}

// A device's kernels, compiled once a process, in the background (each its own library: Slang's
// sources repeat their helpers' names). Never freed (the compiler's callbacks may outlive anything
// else at exit).
class MetalKernels {
public:
    MetalKernels(id<MTLDevice> device, const KernelSource* sources, int count)
        : device_(device), sources_(sources), slots_(static_cast<std::size_t>(count)) {}

    void start() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (started_) return;
            started_ = true;
        }
        // (Unlocked: a compile the system has cached may call back before newLibrary returns.)
        MTLCompileOptions* options = compileOptions();
        for (int k = 0; k < static_cast<int>(slots_.size()); ++k) {
            // No fused multiply-adds either (Metal contracts them even without fast math): each
            // product and sum rounded on its own, as the CPU and DirectX 11 round them (look.slang's
            // exactDot and the clearance walk decide on them; Slang has no `precise` for Metal).
            NSString* source = [@"#pragma METAL fp contract(off)\n" stringByAppendingString:[NSString stringWithUTF8String:sources_[k].source]];
            NSString* entry = [NSString stringWithUTF8String:sources_[k].entry];
            [device_ newLibraryWithSource:source
                                  options:options
                        completionHandler:^(id<MTLLibrary> library, NSError* error) {
                          if (!library) {
                              done(k, nil, error ? error.localizedDescription.UTF8String : "the library did not compile");
                              return;
                          }
                          id<MTLFunction> function = [library newFunctionWithName:entry];
                          if (!function) {
                              done(k, nil, "no entry point");
                              return;
                          }
                          [device_ newComputePipelineStateWithFunction:function
                                                     completionHandler:^(id<MTLComputePipelineState> pipeline, NSError* e) {
                                                       done(k, pipeline, pipeline ? "" : e ? e.localizedDescription.UTF8String : "no pipeline");
                                                     }];
                        }];
        }
    }
    // Whether kernel k is ready (`wait`: once it is); throws when it did not compile.
    bool ready(int k, bool wait) {
        start();
        std::unique_lock<std::mutex> lock(mutex_);
        if (wait) changed_.wait(lock, [&] { return slots_[k].done; });
        if (slots_[k].done && !slots_[k].pipeline) fail(std::string(sources_[k].entry) + " did not compile: " + slots_[k].error);
        return slots_[k].done;
    }
    bool allReady() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) return false;
        return std::all_of(slots_.begin(), slots_.end(), [](const Slot& s) { return s.done && s.pipeline; });
    }
    id<MTLComputePipelineState> pipeline(int k) {
        ready(k, true);
        std::lock_guard<std::mutex> lock(mutex_);
        return slots_[k].pipeline;
    }

private:
    void done(int k, id<MTLComputePipelineState> pipeline, const char* error) {
        std::lock_guard<std::mutex> lock(mutex_);
        slots_[k].pipeline = pipeline;
        slots_[k].error = error ? error : "";
        slots_[k].done = true;
        changed_.notify_all();
    }

    struct Slot {
        id<MTLComputePipelineState> pipeline = nil;
        std::string error;
        bool done = false;
    };
    id<MTLDevice> device_;
    const KernelSource* sources_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool started_ = false;
    std::vector<Slot> slots_;
};

// The kernels of `sources` (a table of `count`) on `device`, made once a process.
MetalKernels& kernelsFor(id<MTLDevice> device, const KernelSource* sources, int count);
// `device`, or the system's default where null.
id<MTLDevice> deviceOf(void* device);
// The runs committed and not finished, on every stream (finishMetalRuns).
std::mutex& runsMutex();
std::condition_variable& runsDone();
std::size_t& runsPending();

// A device's command stream, which every engine and line frames on it encode into in call order (as
// DirectX 11's immediate context orders them): the command buffer being filled, its encoder, and the
// last one committed.
class Stream {
public:
    explicit Stream(id<MTLDevice> device) : queue_([device newCommandQueue]) {
        if (!queue_) fail("no command queue");
    }

    // The compute encoder open, or a new one: one taking timestamps at its start and end into
    // `samples` (a sample nothing ran between is 0 on Apple's GPUs: an encoder sampled must dispatch).
    id<MTLComputeCommandEncoder> compute(id<MTLCounterSampleBuffer> samples = nil, NSUInteger startSample = MTLCounterDontSample,
                                         NSUInteger endSample = MTLCounterDontSample) {
        const bool sampled = samples && (startSample != MTLCounterDontSample || endSample != MTLCounterDontSample);
        if (compute_ && !sampled) return compute_;
        endEncoders();
        if (sampled) {
            MTLComputePassDescriptor* pass = [MTLComputePassDescriptor computePassDescriptor];
            pass.dispatchType = MTLDispatchTypeSerial;
            pass.sampleBufferAttachments[0].sampleBuffer = samples;
            pass.sampleBufferAttachments[0].startOfEncoderSampleIndex = startSample;
            pass.sampleBufferAttachments[0].endOfEncoderSampleIndex = endSample;
            compute_ = [buffer() computeCommandEncoderWithDescriptor:pass];
        } else {
            compute_ = [buffer() computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
        }
        if (!compute_) fail("no compute encoder");
        return compute_;
    }
    id<MTLComputeCommandEncoder> currentCompute() const { return compute_; }
    id<MTLBlitCommandEncoder> blit() {
        if (blit_) return blit_;
        endEncoders();
        blit_ = [buffer() blitCommandEncoder];
        if (!blit_) fail("no blit encoder");
        return blit_;
    }
    void endEncoders() {
        if (compute_) [compute_ endEncoding];
        if (blit_) [blit_ endEncoding];
        compute_ = nil;
        blit_ = nil;
    }
    // Commits what is encoded (it runs while the caller goes on); returns the command buffer.
    id<MTLCommandBuffer> commit() {
        endEncoders();
        if (open_) {
            {
                std::lock_guard<std::mutex> lock(runsMutex());
                ++runsPending();
            }
            [open_ addCompletedHandler:^(id<MTLCommandBuffer>) {
              std::lock_guard<std::mutex> lock(runsMutex());
              --runsPending();
              runsDone().notify_all();
            }];
            [open_ commit];
            last_ = open_;
            open_ = nil;
        }
        return last_;
    }
    void waitIdle() {
        commit();
        if (last_) [last_ waitUntilCompleted];
        if (last_ && last_.status == MTLCommandBufferStatusError) {
            NSError* error = last_.error;
            last_ = nil;
            fail(std::string("a run failed on the GPU: ") + (error ? error.localizedDescription.UTF8String : "?"));
        }
    }
    // Whether the GPU may still touch the buffers: work encoded, or committed and not done.
    bool busy() const {
        if (open_) return true;
        return last_ && last_.status != MTLCommandBufferStatusCompleted && last_.status != MTLCommandBufferStatusError;
    }

private:
    id<MTLCommandBuffer> buffer() {
        if (!open_) open_ = [queue_ commandBuffer];
        if (!open_) fail("no command buffer");
        return open_;
    }

    id<MTLCommandQueue> queue_;
    id<MTLCommandBuffer> open_ = nil;
    id<MTLCommandBuffer> last_ = nil;
    id<MTLComputeCommandEncoder> compute_ = nil;
    id<MTLBlitCommandEncoder> blit_ = nil;
};

// A buffer by number: the engine's own, or a target the host gave.
struct Held {
    id<MTLBuffer> buffer = nil;
    std::size_t bytes = 0;
};

inline std::size_t rounded(std::size_t bytes) { return std::max<std::size_t>((bytes + 15) & ~std::size_t{15}, 16); }

// The stream of `device` (one a device, made once).
std::shared_ptr<Stream> streamFor(id<MTLDevice> device);
// A timestamp sample buffer of `count` samples (null where the device samples none at encoder starts).
id<MTLCounterSampleBuffer> timestampSamples(id<MTLDevice> device, std::size_t count);

}  // namespace groomlab::gpu::metal
