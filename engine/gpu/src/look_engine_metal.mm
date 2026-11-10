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
#include "metal_internal.h"

// The kernels as Metal Shading Language, compiled from shaders/look.slang by slangc when the
// module builds (engine/gpu/CMakeLists.txt): kLookMetalNoiseCS and its group kLookMetalNoiseCSGroup...
#include "look_kernels_metal.h"

namespace groomlab::gpu {

namespace {

using namespace metal;

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

const KernelSource kSources[] = {
#define GROOMLAB_KERNEL(Name) {kLookMetal##Name##CS, kLookMetal##Name##CSGroup, #Name "CS"},
#include "groomlab/look/look_kernels.inc"
#undef GROOMLAB_KERNEL
};
constexpr int kKernelCount = static_cast<int>(LookEngine::Kernel::Count);
static_assert(sizeof kSources / sizeof kSources[0] == kKernelCount, "a source a kernel");


}  // namespace

namespace metal {

MetalKernels& kernelsFor(id<MTLDevice> device, const KernelSource* sources, int count) {
    static std::mutex mutex;
    static auto* all = new std::map<std::pair<void*, const void*>, MetalKernels*>;  // (never freed: see MetalKernels)
    std::lock_guard<std::mutex> lock(mutex);
    MetalKernels*& k = (*all)[{(__bridge void*)device, sources}];
    if (!k) k = new MetalKernels(device, sources, count);
    return *k;
}

id<MTLDevice> deviceOf(void* device) {
    if (device) return (__bridge id<MTLDevice>)device;
    static id<MTLDevice> fallback = MTLCreateSystemDefaultDevice();
    return fallback;
}

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

std::shared_ptr<Stream> streamFor(id<MTLDevice> device) {
    static std::mutex mutex;
    static auto* all = new std::map<void*, std::shared_ptr<Stream>>;
    std::lock_guard<std::mutex> lock(mutex);
    std::shared_ptr<Stream>& s = (*all)[(__bridge void*)device];
    if (!s) s = std::make_shared<Stream>(device);
    return s;
}

id<MTLCounterSampleBuffer> timestampSamples(id<MTLDevice> device, std::size_t count) {
    if (![device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) return nil;
    for (id<MTLCounterSet> set in device.counterSets) {
        if (![set.name isEqualToString:MTLCommonCounterSetTimestamp]) continue;
        MTLCounterSampleBufferDescriptor* d = [MTLCounterSampleBufferDescriptor new];
        d.counterSet = set;
        d.storageMode = MTLStorageModeShared;
        d.sampleCount = count;
        return [device newCounterSampleBufferWithDescriptor:d error:nil];
    }
    return nil;
}

}  // namespace metal

namespace {

class MetalLookEngine final : public LookEngine {
public:
    MetalLookEngine(id<MTLDevice> device, std::shared_ptr<Stream> stream)
        : device_(device), kernels_(kernelsFor(device, kSources, kKernelCount)), stream_(std::move(stream)) {
        kernels_.start();
        zero_ = [device_ newBufferWithLength:65536 options:MTLResourceStorageModeShared];  // (zeros: an input not bound)
        // Timestamps at the compute encoders' starts (Apple's GPUs sample at stage boundaries).
        samples_ = timestampSamples(device_, kSampleSlots);
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
    // Timestamps: each encoder of a run samples at its start and end (two slots of samples_), and a
    // stamp is the end of the encoder before it, or (none open) the start of the next one.
    void beginRun() override {
        slotsUsed_ = 0;
        pendingStamp_ = -1;
        ours_ = nil;
        oursEnd_ = lastEnd_ = MTLCounterDontSample;
        std::fill(std::begin(stampSlot_), std::end(stampSlot_), MTLCounterDontSample);
        stampCount_ = 0;
    }
    void stamp(std::size_t index) override {
        if (!samples_ || index >= kMaxStamps) return;
        if (pendingStamp_ >= 0) stampSlot_[pendingStamp_] = lastEnd_;  // (two stamps with nothing between)
        pendingStamp_ = -1;
        if (ours_ && stream_->currentCompute() == ours_) {
            stampSlot_[index] = oursEnd_;
            stream_->endEncoders();  // the next dispatch's encoder is another
            ours_ = nil;
        } else {
            pendingStamp_ = static_cast<int>(index);
        }
    }
    void endRun(std::size_t stamps) override {
        if (pendingStamp_ >= 0) stampSlot_[pendingStamp_] = lastEnd_;  // nothing after it: the last encoder's end
        pendingStamp_ = -1;
        ours_ = nil;
        run_ = stream_->commit();
        stampCount_ = samples_ ? std::min(stamps, kMaxStamps) : 0;  // (stamps' slots: stampSlot_)
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
        NSData* data = slotsUsed_ ? [samples_ resolveCounterRange:NSMakeRange(0, slotsUsed_)] : nil;
        if (!data || data.length < slotsUsed_ * sizeof(MTLCounterResultTimestamp)) {
            *frequency = 0.0;  // (as DirectX's disjoint: no times this run)
            return true;
        }
        const auto* samples = static_cast<const MTLCounterResultTimestamp*>(data.bytes);
        std::uint64_t last = 0;
        for (std::size_t i = 0; i < stampCount_; ++i) {
            const NSUInteger slot = stampSlot_[i];
            std::uint64_t t = slot < slotsUsed_ ? samples[slot].timestamp : last;  // (none: the stamp before's)
            if (t == MTLCounterErrorValue || t == 0) {
                *frequency = 0.0;
                t = last;
            }
            (*ticks)[i] = last = t;
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
    // The encoder this engine dispatches into: its own still open, else a new one sampled at its
    // start and end where there are slots left (a stamp waiting takes its start).
    id<MTLComputeCommandEncoder> encoderForDispatch() {
        if (ours_ && stream_->currentCompute() == ours_) return ours_;
        if (!samples_ || slotsUsed_ + 2 > kSampleSlots) {
            ours_ = nil;
            return stream_->compute();
        }
        const NSUInteger start = slotsUsed_++, end = slotsUsed_++;
        ours_ = stream_->compute(samples_, start, end);
        oursEnd_ = lastEnd_ = end;
        if (pendingStamp_ >= 0) stampSlot_[pendingStamp_] = start;
        pendingStamp_ = -1;
        return ours_;
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
    static constexpr std::size_t kSampleSlots = 4 * kMaxStamps;  // two an encoder
    id<MTLCounterSampleBuffer> samples_ = nil;
    NSUInteger slotsUsed_ = 0;
    NSUInteger stampSlot_[kMaxStamps] = {};
    int pendingStamp_ = -1;
    id<MTLComputeCommandEncoder> ours_ = nil;  // the encoder this engine opened (its end sample: oursEnd_)
    NSUInteger oursEnd_ = MTLCounterDontSample, lastEnd_ = MTLCounterDontSample;
    id<MTLCommandBuffer> run_ = nil;  // the last run's (its stamps)
    std::size_t stampCount_ = 0;
    bool ticksPending_ = false;
};

}  // namespace

std::unique_ptr<LookEngine> makeMetalLookEngine(void* device) {
    @autoreleasepool {
        id<MTLDevice> d = deviceOf(device);
        if (!d) fail("no device");
        return std::make_unique<MetalLookEngine>(d, streamFor(d));
    }
}

void* metalSystemDevice() { return (__bridge void*)deviceOf(nullptr); }

bool prepareMetalLookKernels(void* device) {
    @autoreleasepool {
        id<MTLDevice> d = deviceOf(device);
        if (!d) return false;
        MetalKernels& k = kernelsFor(d, kSources, kKernelCount);
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
