// SPDX-License-Identifier: Apache-2.0
// The look engine on Metal (look_engine.h makeMetalLookEngine; macOS), mirroring
// look_engine_d3d11.cpp. The kernels are look.slang's in Metal Shading Language (slangc -target
// metal, embedded by cmake/embed_kernel.cmake), compiled by the system's Metal compiler in the
// background and cached by it. Their buffers have Slang's indices, the resources' order in
// look.slang and the same in every kernel: 0 the constants (set inline), 1-9 the inputs (DirectX's
// t0-t8), 10-12 the outputs (u0-u2); the app's point texture is texture 0.
//
// Buffers live in memory the CPU shares (Apple silicon's unified memory): an upload is a copy into
// them where the GPU has nothing pending, else a copy staged through a blit in order with the
// dispatches (as DirectX 11's UpdateSubresource is ordered on its context). An engine and its
// siblings encode into one command stream (as DirectX 11's share the immediate context), so what
// one writes is there for the next to read; a run is committed at its end and runs while the host
// carries on, finish() waits for it.

#import <Metal/Metal.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "groomlab/gpu/look_engine.h"
#include "groomlab/look/look_engine_internal.h"

// The kernels as Metal Shading Language, compiled from shaders/look.slang by slangc when the
// module builds (engine/gpu/CMakeLists.txt): kLookMetalNoiseCS and its group kLookMetalNoiseCSGroup...
#include "look_kernels_metal.h"

namespace groomlab::gpu {

namespace {

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("GPU preview (Metal): " + what); }

// The buffers' indices in every kernel (look.slang's resources in order: Slang's layout).
enum : NSUInteger {
    kArgConstants = 0,
    kArgOffsets,
    kArgStrandData,
    kArgIn,
    kArgRamps,
    kArgFields,
    kArgAux0,
    kArgAux1,
    kArgAux2,
    kArgPointStrands,
    kArgOut0,
    kArgOut1,
    kArgOut2
};

struct KernelSource {
    const char* source;
    const unsigned* group;
    const char* entry;
};

const KernelSource kSources[] = {
#define GROOMLAB_KERNEL(Name) {kLookMetal##Name##CS, kLookMetal##Name##CSGroup, #Name "CS"},
#include "groomlab/look/look_kernels.inc"
#undef GROOMLAB_KERNEL
};
constexpr int kKernelCount = static_cast<int>(LookEngine::Kernel::Count);
static_assert(sizeof kSources / sizeof kSources[0] == kKernelCount, "a source a kernel");

MTLCompileOptions* compileOptions() {
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
    explicit MetalKernels(id<MTLDevice> device) : device_(device) {}

    void start() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (started_) return;
            started_ = true;
        }
        // (Unlocked: a compile the system has cached may call back before newLibrary returns.)
        MTLCompileOptions* options = compileOptions();
        for (int k = 0; k < kKernelCount; ++k) {
            // No fused multiply-adds either (Metal contracts them even without fast math): each
            // product and sum rounded on its own, as the CPU and DirectX 11 round them (look.slang's
            // exactDot and the clearance walk decide on them; Slang has no `precise` for Metal).
            NSString* source = [@"#pragma METAL fp contract(off)\n" stringByAppendingString:[NSString stringWithUTF8String:kSources[k].source]];
            NSString* entry = [NSString stringWithUTF8String:kSources[k].entry];
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
        if (slots_[k].done && !slots_[k].pipeline) fail(std::string(kSources[k].entry) + " did not compile: " + slots_[k].error);
        return slots_[k].done;
    }
    bool allReady() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) return false;
        return std::all_of(std::begin(slots_), std::end(slots_), [](const Slot& s) { return s.done && s.pipeline; });
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
    std::mutex mutex_;
    std::condition_variable changed_;
    bool started_ = false;
    Slot slots_[kKernelCount];
};

MetalKernels& kernelsFor(id<MTLDevice> device) {
    static std::mutex mutex;
    static auto* all = new std::map<void*, MetalKernels*>;  // (never freed: see MetalKernels)
    std::lock_guard<std::mutex> lock(mutex);
    MetalKernels*& k = (*all)[(__bridge void*)device];
    if (!k) k = new MetalKernels(device);
    return *k;
}

id<MTLDevice> deviceOf(void* device) {
    if (device) return (__bridge id<MTLDevice>)device;
    static id<MTLDevice> fallback = MTLCreateSystemDefaultDevice();
    return fallback;
}

// The runs committed and not finished, on every stream (finishMetalRuns).
std::mutex& runsMutex() {
    static std::mutex m;
    return m;
}
std::condition_variable& runsDone() {
    static std::condition_variable c;
    return c;
}
std::size_t& runsPending() {
    static std::size_t n = 0;
    return n;
}

// The command stream an engine and its siblings encode into, in call order: the command buffer
// being filled, its encoder, and the last one committed.
class Stream {
public:
    explicit Stream(id<MTLDevice> device) : queue_([device newCommandQueue]) {
        if (!queue_) fail("no command queue");
    }

    id<MTLComputeCommandEncoder> compute(id<MTLCounterSampleBuffer> samples, NSUInteger startSample) {
        if (compute_ && startSample == MTLCounterDontSample) return compute_;
        endEncoders();
        if (samples && startSample != MTLCounterDontSample) {
            MTLComputePassDescriptor* pass = [MTLComputePassDescriptor computePassDescriptor];
            pass.dispatchType = MTLDispatchTypeSerial;
            pass.sampleBufferAttachments[0].sampleBuffer = samples;
            pass.sampleBufferAttachments[0].startOfEncoderSampleIndex = startSample;
            pass.sampleBufferAttachments[0].endOfEncoderSampleIndex = MTLCounterDontSample;
            compute_ = [buffer() computeCommandEncoderWithDescriptor:pass];
        } else {
            compute_ = [buffer() computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
        }
        if (!compute_) fail("no compute encoder");
        return compute_;
    }
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

std::size_t rounded(std::size_t bytes) { return std::max<std::size_t>((bytes + 15) & ~std::size_t{15}, 16); }

class MetalLookEngine final : public LookEngine {
public:
    MetalLookEngine(id<MTLDevice> device, std::shared_ptr<Stream> stream)
        : device_(device), kernels_(kernelsFor(device)), stream_(std::move(stream)) {
        kernels_.start();
        zero_ = [device_ newBufferWithLength:65536 options:MTLResourceStorageModeShared];  // (zeros: an input not bound)
        // Timestamps at the compute encoders' starts (Apple's GPUs sample at stage boundaries).
        if ([device_ supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
            for (id<MTLCounterSet> set in device_.counterSets) {
                if (![set.name isEqualToString:MTLCommonCounterSetTimestamp]) continue;
                MTLCounterSampleBufferDescriptor* d = [MTLCounterSampleBufferDescriptor new];
                d.counterSet = set;
                d.storageMode = MTLStorageModeShared;
                d.sampleCount = kMaxStamps;
                samples_ = [device_ newCounterSampleBufferWithDescriptor:d error:nil];
            }
        }
    }

    GpuApi api() const override { return GpuApi::Metal; }
    void* device() const override { return (__bridge void*)device_; }
    bool kernelReady(Kernel kernel, bool wait) override { return kernels_.ready(static_cast<int>(kernel), wait); }

    std::unique_ptr<LookEngine> sibling() override { return std::make_unique<MetalLookEngine>(device_, stream_); }
    void borrow(int n, LookEngine& from, int fromId) override {
        auto& other = dynamic_cast<MetalLookEngine&>(from);
        if (!other.has(fromId)) fail("a sibling's buffer to borrow is missing");
        buffers_[n] = other.buffer(fromId);
    }

    std::size_t videoBytes() const override {
        std::size_t n = constants_.size();
        for (const auto& [key, b] : buffers_) n += b.bytes;
        return n;
    }

    void dropTargets() override {
        for (int i = 0; i < 4; ++i) {
            if (outIsTarget_[i]) {
                out_[i] = Held{};
                outBound_[i] = false;
                outIsTarget_[i] = false;
            }
        }
        uvs_ = Held{};
        uvsBound_ = false;
        texture_ = nil;
    }

    void finish() override { stream_->waitIdle(); }

protected:
    void write(int n, const void* data, std::size_t bytes, bool) override {
        Held& b = buffers_[n];
        const bool fresh = make(b, bytes);
        upload(b, 0, data, bytes, fresh);
    }
    void writeRange(int n, std::size_t offset, const void* data, std::size_t bytes) override {
        auto it = buffers_.find(n);
        if (it == buffers_.end() || !bytes) return;
        if (offset + bytes > it->second.buffer.length) fail("a write past a buffer's end");
        upload(it->second, offset, data, bytes, false);
    }
    void allocate(int n, std::size_t bytes) override { make(buffers_[n], bytes); }
    bool has(int n) const override {
        if (n >= kOut0 && n <= kOut3) return outBound_[n - kOut0];
        if (n == kOutUvs) return uvsBound_;
        return buffers_.count(n) != 0;
    }
    void release(int n) override {
        if (n >= kOut0 && n <= kOut3) {
            out_[n - kOut0] = Held{};
            outBound_[n - kOut0] = false;
            outIsTarget_[n - kOut0] = false;
        }
        buffers_.erase(n);
    }

    bool bindTargets(const LookTargets& targets, bool frames, std::size_t bytes, std::size_t uvBytes) override {
        // The lines' uvs: written in place or not at all.
        id<MTLBuffer> uvs = (__bridge id<MTLBuffer>)targets.uvs;
        uvsBound_ = uvs && uvs.length >= uvBytes;
        uvs_ = uvsBound_ ? Held{uvs, uvBytes} : Held{};
        // The app's point texture, checked to be the RGBA32Float it reads back.
        id<MTLTexture> texture = (__bridge id<MTLTexture>)targets.pointTexture;
        if (texture != texture_) {
            texture_ = nil;
            if (texture) {
                if (texture.pixelFormat != MTLPixelFormatRGBA32Float || !(texture.usage & MTLTextureUsageShaderWrite)) {
                    fail("the point texture is not an RGBA32Float texture compute may write");
                }
                if (texture.width != targets.textureWidth) fail("the point texture is not as wide as said");
                texture_ = texture;
            }
        }
        void* const want[4] = {targets.positions, targets.normals, targets.tangents, targets.bitangents};
        bool inPlace = targets.positions != nullptr;
        for (int i = 0; i < 4; ++i) {
            outIsTarget_[i] = false;
            if (i > 0 && !frames) {
                out_[i] = Held{};
                outBound_[i] = false;
                continue;
            }
            id<MTLBuffer> target = (__bridge id<MTLBuffer>)want[i];
            if (target && target.length >= bytes) {
                out_[i] = Held{target, bytes};
                outIsTarget_[i] = true;
            } else {
                // The engine's own (a target too short for the run's points is not written).
                Held& own = buffers_[kOut0 + i];
                make(own, bytes);
                out_[i] = own;
                if (target) inPlace = false;
            }
            outBound_[i] = true;
        }
        return inPlace;
    }
    void copyToTargets(bool, std::size_t) override {}  // (every target is written in place)
    void copy(int from, int to, std::size_t bytes, std::size_t fromOffset, std::size_t toOffset) override {
        if (!bytes) return;
        [stream_->blit() copyFromBuffer:buffer(from).buffer
                           sourceOffset:fromOffset
                               toBuffer:buffer(to).buffer
                      destinationOffset:toOffset
                                   size:bytes];
    }
    void setConstants(const void* slots, std::size_t count, std::size_t slotBytes) override {
        constants_.resize(count * kConstantSlotBytes);
        for (std::size_t i = 0; i < count; ++i) {
            std::memcpy(constants_.data() + i * kConstantSlotBytes, static_cast<const char*>(slots) + i * slotBytes,
                        std::min(slotBytes, kConstantSlotBytes));
        }
    }
    void dispatch(Kernel kernel, std::size_t slot, const Binding& b, std::size_t groupsX, std::size_t groupsY) override {
        if (!groupsX || !groupsY) return;
        const int k = static_cast<int>(kernel);
        id<MTLComputePipelineState> pipeline = kernels_.pipeline(k);
        const unsigned* group = kSources[k].group;
        if (static_cast<NSUInteger>(group[0]) * group[1] * group[2] > pipeline.maxTotalThreadsPerThreadgroup) {
            fail(std::string(kSources[k].entry) + "'s group is larger than the device runs");
        }
        if ((slot + 1) * kConstantSlotBytes > constants_.size()) fail("a dispatch's constants were not set");
        id<MTLComputeCommandEncoder> encoder = encoderForDispatch();
        [encoder setComputePipelineState:pipeline];
        [encoder setBytes:constants_.data() + slot * kConstantSlotBytes length:kConstantSlotBytes atIndex:kArgConstants];
        bind(encoder, kArgOffsets, b.offsets >= 0 ? b.offsets : static_cast<int>(kOffsets));
        bind(encoder, kArgStrandData, kStrandData);
        bind(encoder, kArgIn, b.in);
        bind(encoder, kArgRamps, kRamps);
        bind(encoder, kArgFields, b.out[0] == kFields ? -1 : static_cast<int>(kFields));
        bind(encoder, kArgAux0, b.aux[0]);
        bind(encoder, kArgAux1, b.aux[1]);
        bind(encoder, kArgAux2, b.aux[2]);
        bind(encoder, kArgPointStrands, b.out[0] == kPointStrands ? -1 : static_cast<int>(kPointStrands));
        bind(encoder, kArgOut0, b.out[0]);
        bind(encoder, kArgOut1, b.out[1]);
        bind(encoder, kArgOut2, b.out[2]);
        if (b.texture) {
            if (!texture_) fail("no point texture bound");
            [encoder setTexture:texture_ atIndex:0];
        }
        [encoder dispatchThreadgroups:MTLSizeMake(groupsX, groupsY, 1) threadsPerThreadgroup:MTLSizeMake(group[0], group[1], group[2])];
    }
    void beginRun() override {
        pendingStamp_ = MTLCounterDontSample;
        stampCount_ = 0;
    }
    void stamp(std::size_t index) override {
        if (!samples_ || index >= kMaxStamps) return;
        if (pendingStamp_ != MTLCounterDontSample) sampleNow();  // (two stamps with nothing between)
        stream_->endEncoders();  // the next dispatch's encoder takes the stamp at its start
        pendingStamp_ = index;
    }
    void endRun(std::size_t stamps) override {
        if (pendingStamp_ != MTLCounterDontSample) sampleNow();
        run_ = stream_->commit();
        stampCount_ = samples_ ? std::min(stamps, kMaxStamps) : 0;
        ticksPending_ = samples_ != nil;
    }
    bool stampTicks(bool wait, std::vector<std::uint64_t>* ticks, double* frequency) override {
        if (!ticksPending_) return false;
        if (run_ && run_.status != MTLCommandBufferStatusCompleted && run_.status != MTLCommandBufferStatusError) {
            if (!wait) return false;
            [run_ waitUntilCompleted];
        }
        ticksPending_ = false;
        ticks->assign(stampCount_, 0);
        *frequency = 1e9;  // (Apple's GPU timestamps are nanoseconds, the CPU's clock)
        if (!stampCount_) return true;
        NSData* data = [samples_ resolveCounterRange:NSMakeRange(0, stampCount_)];
        if (!data || data.length < stampCount_ * sizeof(MTLCounterResultTimestamp)) {
            *frequency = 0.0;  // (as DirectX's disjoint: no times this run)
            return true;
        }
        const auto* stamps = static_cast<const MTLCounterResultTimestamp*>(data.bytes);
        for (std::size_t i = 0; i < stampCount_; ++i) {
            if (stamps[i].timestamp == MTLCounterErrorValue) *frequency = 0.0;
            (*ticks)[i] = stamps[i].timestamp;
        }
        return true;
    }
    std::vector<std::uint8_t> readBack(int n, std::size_t size) override {
        std::vector<std::uint8_t> bytes(size);
        if (!has(n) || size == 0) return bytes;
        const Held b = buffer(n);
        if (!b.buffer) return bytes;
        size = std::min<std::size_t>(size, b.buffer.length);
        if (b.buffer.storageMode == MTLStorageModeShared) {
            stream_->waitIdle();
            std::memcpy(bytes.data(), b.buffer.contents, size);
            return bytes;
        }
        id<MTLBuffer> staging = [device_ newBufferWithLength:rounded(size) options:MTLResourceStorageModeShared];
        [stream_->blit() copyFromBuffer:b.buffer sourceOffset:0 toBuffer:staging destinationOffset:0 size:size];
        stream_->waitIdle();
        std::memcpy(bytes.data(), staging.contents, size);
        return bytes;
    }

private:
    // `b` at least `bytes` long, a new buffer where it was not; whether it is new.
    bool make(Held& b, std::size_t bytes) {
        if (b.buffer && b.bytes == bytes) return false;
        b.buffer = [device_ newBufferWithLength:rounded(bytes) options:MTLResourceStorageModeShared];
        if (!b.buffer) fail("out of memory for a buffer");
        b.bytes = bytes;
        return true;
    }
    void upload(const Held& b, std::size_t offset, const void* data, std::size_t bytes, bool fresh) {
        if (!bytes || !data) return;
        if (fresh || !stream_->busy()) {  // the GPU touches it nowhere: straight in
            std::memcpy(static_cast<char*>(b.buffer.contents) + offset, data, bytes);
            return;
        }
        // In order with what is encoded: through a staging copy.
        id<MTLBuffer> staging = [device_ newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared];
        [stream_->blit() copyFromBuffer:staging sourceOffset:0 toBuffer:b.buffer destinationOffset:offset size:bytes];
    }
    void bind(id<MTLComputeCommandEncoder> encoder, NSUInteger index, int n) {
        id<MTLBuffer> b = n >= 0 && has(n) ? buffer(n).buffer : nil;
        [encoder setBuffer:b ? b : zero_ offset:0 atIndex:index];
    }
    id<MTLComputeCommandEncoder> encoderForDispatch() {
        const NSUInteger start = pendingStamp_;
        pendingStamp_ = MTLCounterDontSample;
        return stream_->compute(samples_, start);
    }
    void sampleNow() {  // an encoder of its own for a stamp nothing followed
        encoderForDispatch();
        stream_->endEncoders();
    }
    Held buffer(int n) {
        if (n == kOutUvs) {
            if (!uvsBound_) fail("the uvs not bound");
            return uvs_;
        }
        if (n >= kOut0 && n <= kOut3) {
            if (!outBound_[n - kOut0]) fail("an output not bound");
            return out_[n - kOut0];
        }
        return buffers_[n];
    }

    id<MTLDevice> device_;
    MetalKernels& kernels_;
    std::shared_ptr<Stream> stream_;
    id<MTLBuffer> zero_ = nil;
    std::vector<std::uint8_t> constants_;  // a slot a dispatch of a run, set inline
    std::map<int, Held> buffers_;
    Held out_[4];  // kOut0-3 this run: a target, or the engine's own
    bool outBound_[4] = {};
    bool outIsTarget_[4] = {};
    Held uvs_;  // the lines' uvs target (kOutUvs), written in place
    bool uvsBound_ = false;
    id<MTLTexture> texture_ = nil;  // the app's point texture
    id<MTLCounterSampleBuffer> samples_ = nil;
    NSUInteger pendingStamp_ = MTLCounterDontSample;
    id<MTLCommandBuffer> run_ = nil;  // the last run's (its stamps)
    std::size_t stampCount_ = 0;
    bool ticksPending_ = false;
};

}  // namespace

std::unique_ptr<LookEngine> makeMetalLookEngine(void* device) {
    @autoreleasepool {
        id<MTLDevice> d = deviceOf(device);
        if (!d) fail("no device");
        return std::make_unique<MetalLookEngine>(d, std::make_shared<Stream>(d));
    }
}

void* metalSystemDevice() { return (__bridge void*)deviceOf(nullptr); }

bool prepareMetalLookKernels(void* device) {
    @autoreleasepool {
        id<MTLDevice> d = deviceOf(device);
        if (!d) return false;
        MetalKernels& k = kernelsFor(d);
        k.start();
        return k.allReady();
    }
}

void* createMetalBuffer(void* device, std::size_t bytes) {
    @autoreleasepool {
        id<MTLDevice> d = deviceOf(device);
        if (!d) fail("no device");
        id<MTLBuffer> b = [d newBufferWithLength:rounded(bytes) options:MTLResourceStorageModeShared];
        if (!b) fail("out of memory for a buffer");
        return (__bridge_retained void*)b;
    }
}

void finishMetalRuns() {
    std::unique_lock<std::mutex> lock(runsMutex());
    runsDone().wait(lock, [] { return runsPending() == 0; });
}

void releaseMetalBuffer(void* buffer) {
    if (buffer) CFRelease(buffer);
}

void* metalBufferContents(void* buffer) { return buffer ? [(__bridge id<MTLBuffer>)buffer contents] : nullptr; }

}  // namespace groomlab::gpu
