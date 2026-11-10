// SPDX-License-Identifier: Apache-2.0
// The lit strand lines' frames, ribbons, following the scalp and the hair's density grid on Metal
// (line_frames.h makeMetalLineFrames; macOS), mirroring line_frames.cpp's DirectX 11 backend: the
// kernels of shaders/lines.slang, follow.slang and density.slang in Metal Shading Language
// (metal_internal.h compiles them), on the device's one command stream (so a look engine's points
// are written before these read them). Targets are MTLBuffers, any of them, written in place; each
// call's work is committed as it ends, so a host drawing from the targets finds it under way.
//
// Buffers in lines.slang and follow.slang have look.slang's indices (1 gOffsets, 2 gStrandData, 3
// gIn, 6 gAux0, 7 gAux1, 10-12 gOut-gOut3); density.slang's own: 1 gIn, 2 gAux1, 3 gOut, the
// volume texture 0.

#import <Metal/Metal.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "groomlab/gpu/line_frames.h"
#include "groomlab/look/look_engine_internal.h"
#include "metal_internal.h"

// FramesCS, WidthsCS, RibbonsCS, RibbonLayoutCS, RibbonColorsCS (lines.slang), FollowCS
// (follow.slang), DensityClearCS, DensitySplatCS, DensityResolveCS (density.slang) as Metal Shading
// Language (engine/gpu/CMakeLists.txt).
#include "line_kernels_metal.h"
#include "follow_kernels_metal.h"
#include "density_kernels_metal.h"

namespace groomlab::gpu {

namespace {

using namespace metal;

// By LineFrames::Kernel, then the density's three.
const KernelSource kLineSources[] = {
    {kLookMetalFramesCS, kLookMetalFramesCSGroup, "FramesCS"},
    {kLookMetalWidthsCS, kLookMetalWidthsCSGroup, "WidthsCS"},
    {kLookMetalRibbonsCS, kLookMetalRibbonsCSGroup, "RibbonsCS"},
    {kLookMetalRibbonLayoutCS, kLookMetalRibbonLayoutCSGroup, "RibbonLayoutCS"},
    {kLookMetalRibbonColorsCS, kLookMetalRibbonColorsCSGroup, "RibbonColorsCS"},
    {kLookMetalFollowCS, kLookMetalFollowCSGroup, "FollowCS"},
    {kLookMetalDensityClearCS, kLookMetalDensityClearCSGroup, "DensityClearCS"},
    {kLookMetalDensitySplatCS, kLookMetalDensitySplatCSGroup, "DensitySplatCS"},
    {kLookMetalDensityResolveCS, kLookMetalDensityResolveCSGroup, "DensityResolveCS"},
};
constexpr int kLineKernelCount = sizeof kLineSources / sizeof kLineSources[0];
constexpr int kDensityFirst = 6;

enum : NSUInteger { kArgConstants = 0, kArgOffsets = 1, kArgStrandData = 2, kArgIn = 3, kArgAux0 = 6, kArgAux1 = 7, kArgOut0 = 10 };
enum : NSUInteger { kDensityIn = 1, kDensityUvs = 2, kDensityGrid = 3 };

id<MTLBuffer> bufferOf(void* handle) { return (__bridge id<MTLBuffer>)handle; }

class MetalLineFrames final : public LineFrames {
public:
    explicit MetalLineFrames(id<MTLDevice> device)
        : device_(device), kernels_(kernelsFor(device, kLineSources, kLineKernelCount)), stream_(streamFor(device)) {
        kernels_.start();
        samples_ = timestampSamples(device_, 2);
    }

    GpuApi api() const override { return GpuApi::Metal; }
    void* device() const override { return (__bridge void*)device_; }

    void write(void* target, const void* data, std::size_t offset, std::size_t bytes) override {
        if (!target || !bytes) return;
        put(bufferOf(target), data, offset, bytes);
        stream_->commit();
    }

    void* beginWrite(void* target, std::size_t offset, std::size_t bytes) override {
        if (!target || !bytes || filling_) return nullptr;
        id<MTLBuffer> b = bufferOf(target);
        if (offset + bytes > b.length) return nullptr;
        filling_ = b;
        fillOffset_ = offset;
        fillBytes_ = bytes;
        if (b.storageMode == MTLStorageModeShared && !stream_->busy()) {  // straight in
            fillStaging_ = nil;
            return static_cast<char*>(b.contents) + offset;
        }
        fillStaging_ = [device_ newBufferWithLength:rounded(bytes) options:MTLResourceStorageModeShared];
        return fillStaging_ ? fillStaging_.contents : nullptr;
    }
    void endWrite() override {
        if (!filling_) return;
        if (fillStaging_) {
            [stream_->blit() copyFromBuffer:fillStaging_ sourceOffset:0 toBuffer:filling_ destinationOffset:fillOffset_ size:fillBytes_];
            stream_->commit();
        }
        filling_ = nil;
        fillStaging_ = nil;
    }

    double gpuMs(bool wait) override {
        if (!timed_) return lastMs_;
        if (timed_.status != MTLCommandBufferStatusCompleted && timed_.status != MTLCommandBufferStatusError) {
            if (!wait) return -1.0;
            [timed_ waitUntilCompleted];
        }
        timed_ = nil;
        lastMs_ = -1.0;
        NSData* data = samples_ ? [samples_ resolveCounterRange:NSMakeRange(0, 2)] : nil;
        if (data && data.length >= 2 * sizeof(MTLCounterResultTimestamp)) {
            const auto* t = static_cast<const MTLCounterResultTimestamp*>(data.bytes);
            if (t[0].timestamp != MTLCounterErrorValue && t[1].timestamp != MTLCounterErrorValue && t[1].timestamp >= t[0].timestamp) {
                lastMs_ = static_cast<double>(t[1].timestamp - t[0].timestamp) / 1e6;  // (nanoseconds)
            }
        }
        return lastMs_;
    }

    std::vector<float> read(void* target, std::size_t count) override {
        std::vector<float> out(count);
        if (!target || !count) return out;
        id<MTLBuffer> b = bufferOf(target);
        const std::size_t bytes = std::min<std::size_t>(count * 4, b.length);
        if (b.storageMode == MTLStorageModeShared) {
            stream_->waitIdle();
            std::memcpy(out.data(), b.contents, bytes);
            return out;
        }
        id<MTLBuffer> staging = [device_ newBufferWithLength:rounded(bytes) options:MTLResourceStorageModeShared];
        [stream_->blit() copyFromBuffer:b sourceOffset:0 toBuffer:staging destinationOffset:0 size:bytes];
        stream_->waitIdle();
        std::memcpy(out.data(), staging.contents, bytes);
        return out;
    }

    std::vector<float> readVolume(void* texture) override {
        id<MTLTexture> source = texture ? (__bridge id<MTLTexture>)texture : volume_;
        if (!source || source.textureType != MTLTextureType3D || source.pixelFormat != MTLPixelFormatRG32Float) return {};
        const std::size_t row = source.width * 2, image = row * source.height;
        std::vector<float> out(image * source.depth);
        id<MTLBuffer> staging = [device_ newBufferWithLength:rounded(out.size() * 4) options:MTLResourceStorageModeShared];
        [stream_->blit() copyFromTexture:source
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(0, 0, 0)
                              sourceSize:MTLSizeMake(source.width, source.height, source.depth)
                                toBuffer:staging
                       destinationOffset:0
                  destinationBytesPerRow:row * 4
                destinationBytesPerImage:image * 4];
        stream_->waitIdle();
        std::memcpy(out.data(), staging.contents, out.size() * 4);
        return out;
    }

    bool writeVolume(void* texture, const std::vector<float>& texels) override {
        id<MTLTexture> target = texture ? (__bridge id<MTLTexture>)texture : nil;
        if (!target || target.textureType != MTLTextureType3D || target.pixelFormat != MTLPixelFormatRG32Float) return false;
        const std::size_t row = target.width * 2, image = row * target.height;
        if (texels.size() != image * target.depth) return false;
        id<MTLBuffer> staging = [device_ newBufferWithBytes:texels.data() length:texels.size() * 4 options:MTLResourceStorageModeShared];
        [stream_->blit() copyFromBuffer:staging
                           sourceOffset:0
                      sourceBytesPerRow:row * 4
                    sourceBytesPerImage:image * 4
                             sourceSize:MTLSizeMake(target.width, target.height, target.depth)
                              toTexture:target
                       destinationSlice:0
                       destinationLevel:0
                      destinationOrigin:MTLOriginMake(0, 0, 0)];
        stream_->commit();
        return true;
    }

protected:
    std::vector<std::uint32_t> readDensityGrid(std::size_t cells) override {
        std::vector<std::uint32_t> out(cells);
        if (!grid_ || cells == 0 || cells > gridCells_) return out;
        const std::vector<float> raw = read((__bridge void*)grid_, cells);
        std::memcpy(out.data(), raw.data(), cells * 4);
        return out;
    }

    void upload(Buffer which, const void* data, std::size_t bytes) override {
        Held& b = buffers_[which];
        const std::size_t size = rounded(bytes);
        if (b.bytes < size || b.bytes > 2 * size) {  // (room to grow: other counts, a playback's, reuse it)
            const std::size_t room = rounded(size + size / 8);
            b.buffer = [device_ newBufferWithLength:room options:MTLResourceStorageModeShared];
            if (!b.buffer) fail("out of memory for a buffer");
            b.bytes = room;
            if (bytes && data) std::memcpy(b.buffer.contents, data, bytes);  // (new: the GPU touches it nowhere)
            return;
        }
        if (bytes && data) put(b.buffer, data, 0, bytes);
    }

    void dispatch(Kernel kernel, const void* constants, std::size_t constantBytes, const Binding& binding, std::uint32_t groupsX,
                  std::uint32_t groupsY) override {
        const bool timed = kernel == Kernel::Frames || kernel == Kernel::Ribbons;
        const bool framed = kernel == Kernel::Frames || kernel == Kernel::Ribbons;  // the root normals read
        const bool follow = kernel == Kernel::Follow;  // the follow buffers read, the root normals written
        if (!buffers_[kOffsets].buffer || ((framed || follow) && !buffers_[kRootNormals].buffer)) fail("no layout or root normals");
        if (follow && (!buffers_[kFollowRoots].buffer || !buffers_[kFollowRest].buffer || !buffers_[kFollowScalpRest].buffer ||
                       !buffers_[kFollowScalpNow].buffer)) {
            fail("the strands at rest, their roots or the scalps are missing (follow)");
        }
        if (kernel == Kernel::Widths && !buffers_[kWidths].buffer) fail("no widths");
        const bool colored = kernel == Kernel::RibbonColors || (kernel == Kernel::Ribbons && binding.out[2] == nullptr);
        if (colored && !buffers_[kRibbonColors].buffer) fail("no ribbon colors");
        const std::size_t points = pointCount();
        auto usable = [](void* handle, std::size_t bytes) {
            if (bufferOf(handle).length < bytes) fail("a target is shorter than the points need");
        };
        if (binding.in) usable(binding.in, points * 12);
        if (binding.lineUvs) usable(binding.lineUvs, points * 8);
        for (int i = 0; i < 3; ++i) {
            if (binding.out[i]) usable(binding.out[i], binding.outBytes[i]);
        }
        const int k = static_cast<int>(kernel);
        id<MTLComputePipelineState> pipeline = kernels_.pipeline(k);
        id<MTLComputeCommandEncoder> encoder = timed ? timedEncoder() : stream_->compute();
        [encoder setComputePipelineState:pipeline];
        [encoder setBytes:constants length:constantBytes atIndex:kArgConstants];
        [encoder setBuffer:buffers_[kOffsets].buffer offset:0 atIndex:kArgOffsets];
        [encoder setBuffer:follow ? buffers_[kFollowRoots].buffer : buffers_[kRootNormals].buffer offset:0 atIndex:kArgStrandData];
        id<MTLBuffer> in = follow ? buffers_[kFollowRest].buffer : kernel == Kernel::Widths ? buffers_[kWidths].buffer : bufferOf(binding.in);
        [encoder setBuffer:in offset:0 atIndex:kArgIn];
        [encoder setBuffer:follow ? buffers_[kFollowScalpRest].buffer : colored ? buffers_[kRibbonColors].buffer : nil offset:0 atIndex:kArgAux0];
        [encoder setBuffer:follow ? buffers_[kFollowScalpNow].buffer : bufferOf(binding.lineUvs) offset:0 atIndex:kArgAux1];
        for (int i = 0; i < 3; ++i) [encoder setBuffer:bufferOf(binding.out[i]) offset:0 atIndex:kArgOut0 + i];
        if (follow) [encoder setBuffer:buffers_[kRootNormals].buffer offset:0 atIndex:kArgOut0 + 1];  // (gOut2)
        const unsigned* group = kLineSources[k].group;
        [encoder dispatchThreadgroups:MTLSizeMake(groupsX, groupsY, 1) threadsPerThreadgroup:MTLSizeMake(group[0], group[1], group[2])];
        if (timed) {
            stream_->endEncoders();
            timed_ = stream_->commit();
        }
        stream_->commit();
    }

    void dispatchDensity(void* constants, void* positions, void* lineUvs, std::size_t points, std::size_t cells,
                         const std::uint32_t groups[3][2], void* volume) override {
        auto* c = static_cast<StageConstants*>(constants);
        id<MTLTexture> target = (__bridge id<MTLTexture>)volume;
        if (!target || target.textureType != MTLTextureType3D) fail("the density's target is not a 3D texture");
        if (target.width != c->flag[0] || target.height != c->flag[1] || target.depth != c->flag[2] || target.pixelFormat != MTLPixelFormatRG32Float) {
            fail("the density's target texture is not the grid's size and format");
        }
        if (bufferOf(positions).length < points * 12 || bufferOf(lineUvs).length < points * 8) fail("the density's points are shorter than said");
        // The grid (a word a cell), and the texture the kernels write: the host's where it takes
        // shader writes, else one of our own copied into it.
        if (gridCells_ != cells) {
            grid_ = [device_ newBufferWithLength:rounded(cells * 4) options:MTLResourceStorageModeShared];
            if (!grid_) fail("out of memory for the density grid");
            gridCells_ = cells;
        }
        const bool inPlace = (target.usage & MTLTextureUsageShaderWrite) != 0;
        if (!inPlace && (!volume_ || volume_.width != target.width || volume_.height != target.height || volume_.depth != target.depth)) {
            volume_ = makeVolume(target.width, target.height, target.depth);
        }
        id<MTLTexture> written = inPlace ? target : volume_;
        id<MTLComputeCommandEncoder> encoder = timedEncoder();  // (the three in it, timed together)
        for (int k = 0; k < 3; ++k) {
            c->dispatch[0] = groups[k][0] * kLineSources[kDensityFirst + k].group[0];  // the threads in a row
            [encoder setComputePipelineState:kernels_.pipeline(kDensityFirst + k)];
            [encoder setBytes:c length:sizeof(StageConstants) atIndex:kArgConstants];
            [encoder setBuffer:bufferOf(positions) offset:0 atIndex:kDensityIn];
            [encoder setBuffer:bufferOf(lineUvs) offset:0 atIndex:kDensityUvs];
            [encoder setBuffer:grid_ offset:0 atIndex:kDensityGrid];
            [encoder setTexture:written atIndex:0];
            const unsigned* group = kLineSources[kDensityFirst + k].group;
            [encoder dispatchThreadgroups:MTLSizeMake(groups[k][0], groups[k][1], 1)
                    threadsPerThreadgroup:MTLSizeMake(group[0], group[1], group[2])];
        }
        stream_->endEncoders();
        timed_ = stream_->commit();
        if (!inPlace) {
            [stream_->blit() copyFromTexture:volume_ toTexture:target];
        }
        stream_->commit();
    }

private:
    // An encoder of its own for timed work: sample 0 at its start, 1 at its end.
    id<MTLComputeCommandEncoder> timedEncoder() {
        stream_->endEncoders();
        return samples_ ? stream_->compute(samples_, 0, 1) : stream_->compute();
    }
    // `bytes` of `data` into `b` at `offset`: straight in where the GPU touches nothing, else
    // staged and copied in order with what is encoded.
    void put(id<MTLBuffer> b, const void* data, std::size_t offset, std::size_t bytes) {
        if (offset + bytes > b.length) fail("a write past a buffer's end");
        if (b.storageMode == MTLStorageModeShared && !stream_->busy()) {
            std::memcpy(static_cast<char*>(b.contents) + offset, data, bytes);
            return;
        }
        id<MTLBuffer> staging = [device_ newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared];
        [stream_->blit() copyFromBuffer:staging sourceOffset:0 toBuffer:b destinationOffset:offset size:bytes];
    }
    id<MTLTexture> makeVolume(NSUInteger x, NSUInteger y, NSUInteger z) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor new];
        d.textureType = MTLTextureType3D;
        d.pixelFormat = MTLPixelFormatRG32Float;
        d.width = x;
        d.height = y;
        d.depth = z;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        d.storageMode = MTLStorageModePrivate;
        id<MTLTexture> t = [device_ newTextureWithDescriptor:d];
        if (!t) fail("out of memory for the density texture");
        return t;
    }

    struct Held {
        id<MTLBuffer> buffer = nil;
        std::size_t bytes = 0;
    };

    id<MTLDevice> device_;
    MetalKernels& kernels_;
    std::shared_ptr<Stream> stream_;
    Held buffers_[kBufferCount];
    id<MTLBuffer> grid_ = nil;
    std::size_t gridCells_ = 0;
    id<MTLTexture> volume_ = nil;  // the density's own where the host's takes no shader writes
    id<MTLCounterSampleBuffer> samples_ = nil;
    id<MTLCommandBuffer> timed_ = nil;  // the last timed work's (gpuMs)
    double lastMs_ = -1.0;
    id<MTLBuffer> filling_ = nil;  // beginWrite's target, until endWrite
    id<MTLBuffer> fillStaging_ = nil;
    std::size_t fillOffset_ = 0, fillBytes_ = 0;
};

}  // namespace

std::unique_ptr<LineFrames> makeMetalLineFrames(void* device) {
    @autoreleasepool {
        id<MTLDevice> d = metal::deviceOf(device);
        if (!d) metal::fail("no device");
        return std::make_unique<MetalLineFrames>(d);
    }
}

void* createMetalVolume(void* device, unsigned x, unsigned y, unsigned z) {
    @autoreleasepool {
        id<MTLDevice> d = metal::deviceOf(device);
        if (!d) metal::fail("no device");
        MTLTextureDescriptor* t = [MTLTextureDescriptor new];
        t.textureType = MTLTextureType3D;
        t.pixelFormat = MTLPixelFormatRG32Float;
        t.width = x;
        t.height = y;
        t.depth = z;
        t.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        t.storageMode = MTLStorageModePrivate;
        id<MTLTexture> texture = [d newTextureWithDescriptor:t];
        if (!texture) metal::fail("out of memory for a 3D texture");
        return (__bridge_retained void*)texture;
    }
}

void releaseMetalTexture(void* texture) {
    if (texture) CFRelease(texture);
}

}  // namespace groomlab::gpu
