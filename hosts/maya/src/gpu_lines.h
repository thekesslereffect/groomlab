// SPDX-License-Identifier: Apache-2.0
#pragma once

// The lit strand lines' frames on the GPU in Maya (groomlab/gpu/line_frames.h).
//
// After a CPU evaluation (a param's commit, a brush stroke's release, a playback frame) the draw
// override sends the lit lines' points alone and the GPU writes their normals and tangents from
// them (FramesCS), and, where only the widths changed, the widths alone into the uvs' second float
// (WidthsCS): 12 bytes a point (16 with new widths) where the CPU's frames took 36 (44). On by
// default where Viewport 2.0 draws with DirectX 11 or OpenGL 4.5; GROOMLAB_GPU_FRAMES=0 in the
// environment (or `groomLabGpuPreview -frames off`) writes them on the CPU as before, as does a
// device the kernels cannot run on (software, remote: everything uploaded as before).
//
// Where they go. OpenGL: Maya's own vertex buffers (GPU-only there, written first by the CPU when
// made), the points written into them with glBufferSubData, the frames and widths by the kernels
// in place. DirectX 11: Maya 2022 keeps a system-memory copy of its vertex buffers and gives none
// compute may write (gpu_preview.h), so the lit lines' points, normals, tangents and uvs are
// buffers of the override's own (GpuLineStreams: createD3D11DisplayBuffer), handed to Viewport 2.0
// as custom resource handles (MVertexBuffer::resourceHandle), as the GPU preview's are: the points
// go in with UpdateSubresource, the kernels write the rest. The indices and the lit lines' colors
// stay Maya's (written when they change).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/gpu/line_frames.h"

struct ID3D11Buffer;

#include <maya/MApiNamespace.h>  // Maya's classes declared (MHWRender's are using-declarations of a
                                  // versioned namespace's: a plain forward declaration conflicts, Clang/GCC)

namespace groomlab::maya {

// Whether lit strand lines take their frames from the GPU (the default; GROOMLAB_GPU_FRAMES=0 in
// the environment: the CPU's), and the session's switch.
bool gpuFramesEnabled();
void setGpuFramesEnabled(bool on);

// DirectX 11: the vertex buffers a lit lines item draws (made for compute), and the
// MVertexBuffers naming them, which must go first: the item's own (ItemBuffers), so this is
// destroyed after them (declared before them).
struct GpuLineStreams {
    void* device = nullptr;
    unsigned count = 0;     // the points drawn
    unsigned capacity = 0;  // the points the buffers hold (a little more: other counts reuse them)
    ID3D11Buffer* buffers[4] = {};  // positions, normals, tangents (float3 a point), uvs (float2: u, width)
    GpuLineStreams() = default;
    GpuLineStreams(const GpuLineStreams&) = delete;
    GpuLineStreams& operator=(const GpuLineStreams&) = delete;
    ~GpuLineStreams();
    std::size_t bytes() const { return std::size_t{capacity} * (12 * 3 + 8); }
};

// Ribbons (displayRibbons) made by the GPU from the lines (line_frames.h runRibbons): the lines'
// points and uvs (u, width) sent as the lit lines' are, the ribbons' vertices (two a point:
// positions, and lit normals, tangents and uvs, or colors) and triangles written by the kernels.
// Every buffer the override's own, handed to Viewport 2.0 as custom resource handles: DirectX 11,
// made for compute (the indices by createD3D11IndexBuffer); OpenGL, buffer objects made with no
// data (Maya's own buffers, made through acquire and commit, sent their whole size to the GPU: at a
// million strands half a second). The MVertexBuffers and MIndexBuffer naming them go first
// (declared after this in ItemBuffers).
struct GpuRibbonStreams {
    // kLineIndices: facing ribbons' thin segments drawn as lines (line_frames.h writeFacingSplit).
    enum Buffer { kPoints, kLineUvs, kPositions, kNormals, kTangents, kUvs, kColors, kIndices, kLineIndices, kBufferCount };
    bool gl = false;
    void* device = nullptr;  // ID3D11Device*, or the OpenGL context they were made in
    unsigned count = 0;      // the lines' points (the ribbons draw twice as many vertices)
    unsigned capacity = 0;   // the points the buffers hold (a little more: other counts reuse them)
    bool lit = false, colors = false;
    bool uvs = false;  // a uvs stream (lit; unlit facing: its thin segments' coverage)
    ID3D11Buffer* buffers[kBufferCount] = {};
    unsigned names[kBufferCount] = {};  // OpenGL: the buffers' names (the handles Viewport 2.0 holds point at them)
    // What the kernels take as buffer `i` (line_frames.h targets).
    void* target(int i) const { return gl ? reinterpret_cast<void*>(static_cast<std::uintptr_t>(names[i])) : buffers[i]; }
    GpuRibbonStreams() = default;
    GpuRibbonStreams(const GpuRibbonStreams&) = delete;
    GpuRibbonStreams& operator=(const GpuRibbonStreams&) = delete;
    ~GpuRibbonStreams();
    std::size_t bytes() const;
};

// A draw override's engine on Viewport 2.0's device: made at first use, again for another device.
class GpuLines {
public:
    GpuLines();
    ~GpuLines();
    // The engine on the device the viewport draws with now; null (and why()) where there is none:
    // the CPU writes the frames.
    gpu::LineFrames* engine();
    const std::string& why() const { return why_; }
    // The engine failed (`why`): none on this device from now on.
    void fail(const std::string& why);
#ifdef __APPLE__
    // macOS, where Viewport 2.0 draws with OpenGL 4.1 (no compute: engine() has none): the hair's
    // density grid on the system's Metal device. `count` points (`points`: float3 a point; `uvs`:
    // float2, the width second) copied into buffers the GPU shares, splatted into `grid` and its
    // texels read back (rg a cell, x fastest then y then z: what a texture of the grid's size
    // takes); `gpuMs` the splat's milliseconds on the GPU. Empty where it could not (densityWhy()).
    std::vector<float> densityOnMetal(const float* points, const float* uvs, std::size_t count, const gpu::LineFrames::DensityGrid& grid,
                                      double* gpuMs);
    const std::string& densityWhy() const { return densityWhy_; }
#endif
    // DirectX 11: buffers for `count` points on the engine's device (`held`'s where they hold them
    // and are not more than twice as long), and MVertexBuffers naming them (positions, normals,
    // tangents, uvs) put into `vertices` (what they held goes first).
    std::shared_ptr<GpuLineStreams> makeStreams(unsigned count, std::shared_ptr<GpuLineStreams> held,
                                                std::unique_ptr<MHWRender::MVertexBuffer>* vertices[4]);
    // DirectX 11: the ribbons' buffers for lines of `count` points (`held`'s where they hold them:
    // as lit, as colored, not more than twice as long), and what names them put into `vertices`
    // (the lines' points and uvs, then positions, normals, tangents, uvs and colors: those the
    // ribbons draw, the others reset) and `indices`.
    std::shared_ptr<GpuRibbonStreams> makeRibbonStreams(unsigned count, bool lit, bool colors, bool uvs, std::shared_ptr<GpuRibbonStreams> held,
                                                        std::unique_ptr<MHWRender::MVertexBuffer>* vertices[7],
                                                        std::unique_ptr<MHWRender::MIndexBuffer>* indices,
                                                        std::unique_ptr<MHWRender::MIndexBuffer>* lineIndices);

private:
#ifdef __APPLE__
    std::unique_ptr<gpu::LineFrames> metal_;  // the density grid's (densityOnMetal)
    void* metalPoints_ = nullptr;             // shared MTLBuffers: the points, their uvs
    void* metalUvs_ = nullptr;
    std::size_t metalCapacity_ = 0;           // the points they hold
    void* metalVolume_ = nullptr;             // the grid's MTLTexture
    unsigned metalVolumeSize_[3] = {0, 0, 0};
    std::string densityWhy_;
    void releaseMetal();
#endif
    std::unique_ptr<gpu::LineFrames> engine_;
    void* failedOn_ = nullptr;  // the device an engine could not be made on (not tried again)
    std::string why_;
};

}  // namespace groomlab::maya
