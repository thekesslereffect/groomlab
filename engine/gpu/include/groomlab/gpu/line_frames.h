// SPDX-License-Identifier: Apache-2.0
#pragma once

// The lit strand lines' shading frames on the GPU, from their points (display.h StrandLines).
//
// After a CPU evaluation (a param's commit, a brush stroke's release, a playback frame) a host's
// lit lines took three streams a point from the CPU: the points, the normals and the tangents
// (36 bytes a point: ~300 MB at 500k strands of 17 points), and the uvs (u, width) when the widths
// changed. With this, the host sends the points alone (write) into a vertex buffer the kernels can
// read, and run() writes the normals and tangents into the host's other two (shaders/lines.slang
// FramesCS: look.slang's lineFrame, the GPU look preview's own lines arithmetic, so the frames are
// the CPU's, StrandLines::writeFrames, to float rounding); new widths go up alone, a float a point,
// into the uvs' second float (writeWidths). What a run needs beside the points goes up when it
// changes: the strands' point offsets (their layout) and each strand's root normal
// (StrandLines::writeRootNormals), each by a key the host gives.
//
// Targets: DirectX 11, ID3D11Buffer* made for compute (createD3D11DisplayBuffer: vertex buffer,
// raw views, unordered access); OpenGL, buffer names (GLuint, cast through std::uintptr_t), any of
// them. Runs on the caller's thread with the device's context (Maya: the draw override's update on
// the main thread) and leaves the compute state it touches as it found it. Throws
// std::runtime_error.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "groomlab/gpu/look_engine.h"

struct ID3D11Buffer;
struct ID3D11Device;

namespace groomlab::gpu {

class LineFrames {
public:
    virtual ~LineFrames() = default;
    LineFrames(const LineFrames&) = delete;
    LineFrames& operator=(const LineFrames&) = delete;

    virtual GpuApi api() const = 0;
    // ID3D11Device*, or the OpenGL context (HGLRC) it was made in.
    virtual void* device() const = 0;
    // OpenGL: whether its objects can be used from the context current now (false once its share
    // group is gone).
    virtual bool reachable() const { return true; }

    // The strands' layout: `strands` + 1 point offsets, uploaded unless `key` is the one held (a
    // host's StrandLines::layoutKey; 0: always). Returns the bytes sent.
    std::size_t setLayout(const std::size_t* offsets, std::size_t strands, std::uint64_t key);
    // Each strand's root normal (xyzw, 4 floats a strand: StrandLines::writeRootNormals) for the
    // layout's strands, uploaded unless `key` (a hash of them) is the one held. Returns the bytes sent.
    std::size_t setRootNormals(const float* xyzw, std::uint64_t key);
    // `bytes` of `data` into `target` at byte `offset` (a host's own vertex buffer: the points).
    virtual void write(void* target, const void* data, std::size_t offset, std::size_t bytes) = 0;
    // The same for data the caller writes itself (the uvs, written by StrandLines::writeUvs): memory
    // for `bytes` that endWrite() then sends into `target` at `offset`; null where there is none to
    // give (OpenGL: write() then). One at a time.
    virtual void* beginWrite(void* target, std::size_t offset, std::size_t bytes) = 0;
    virtual void endWrite() = 0;
    // The normals and tangents of every point of the layout from the points in `positions`, into
    // `normals` and `tangents`, each at least the layout's points x 12 bytes.
    void run(void* positions, void* normals, void* tangents);
    // The layout's points' widths (a float a point) into the second float of each point's uvs
    // (`uvs`: float2 a point, u and width, StrandLines::writeUvs with two floats). Returns the
    // bytes sent.
    std::size_t writeWidths(void* uvs, const float* widths);
    // ---- Ribbons (display.h DisplayGeometry::ribbons: the CPU's strandRibbons) ----
    //
    // A host drawing the strands as ribbons sends the lines alone (their points, as above, and their
    // uvs' widths: writeWidths, or the uvs StrandLines::writeUvs writes with two floats) and these
    // expand them: two vertices a point (2p, 2p + 1), across each point's frame by half its width
    // times a scale, the very triangles strandRibbons builds (to float rounding) but for strands of
    // one point, whose vertices no triangle uses. 12 bytes a point go up where the CPU's ribbons
    // took 88 (positions, normals, tangents, uvs of two vertices) and 48 of indices.
    //
    // The layout's triangles (`indices`: six uint32 a point, at least points x 24 bytes) and, when
    // `uvs` is given, the lit ribbons' uvs (float2 a vertex: u root to tip, 0.5; points x 16 bytes):
    // what the point counts alone give, written again only when they change.
    void writeRibbonLayout(void* indices, void* uvs, void* lineUvs = nullptr);
    // The colors the ribbons take, `count` RGBA floats x 4: lit, the strands' own colors one a point
    // (StrandColors::write), then writeRibbonColors; unlit, one a strand (each run shades them).
    // Uploaded unless `key` is the one held (0: always). Returns the bytes sent.
    std::size_t setRibbonColors(const float* rgba, std::size_t count, std::uint64_t key);
    bool ribbonColorsHeld(std::uint64_t key) const { return key != 0 && key == colorsKey_; }
    // Lit: the colors setRibbonColors holds (one a point) onto both vertices of each point
    // (`colors`: float4 a vertex, points x 32 bytes).
    void writeRibbonColors(void* colors);
    // The vertices that move with the points: `points` (float3 a point) and `lineUvs` (float2 a
    // point: their second float the width, StrandLines::writeUvs; kNoLineWidth: kRibbonDefaultWidth)
    // into `positions` (float3 a vertex) and, lit, `normals` and `tangents` (the point's frame on both
    // its vertices); unlit, `colors` (float4 a vertex: the strand's color setRibbonColors holds,
    // shaded by `light`, display.h ribbonKeyLight, across the ribbon's face).
    struct RibbonTargets {
        void* positions = nullptr;
        void* normals = nullptr;
        void* tangents = nullptr;
        void* colors = nullptr;
    };
    // With `facing`, the ribbons face the viewer instead (the strand lines drawn at their true width):
    // across each point by cross(its tangent, toward the viewer), half its width times the scale but
    // never less than half of `minPixels` pixels at its distance, so a strand thinner than a pixel is
    // a pixel wide (its coverage then says how much of it it covers). Run again when the view moves.
    struct RibbonFacing {
        float eye[3] = {0.0f, 0.0f, 0.0f};  // the eye (world); orthographic: the direction toward the viewer
        bool orthographic = false;
        float pixelWorld = 0.0f;            // world units a pixel spans at distance 1 (orthographic: a pixel)
        float minPixels = 1.0f;
        bool unshaded = false;  // unlit: the strands' colors as they are, not shaded by `light`
    };
    // Facing ribbons whose coverage says how much of their pixel a thin strand covers: the lines'
    // uvs (`lineUvs`, float2 a point: u with the strand's number, width) onto both vertices of each
    // point (`uvs`: float2 a vertex, points x 16 bytes), each time the widths change.
    void writeRibbonUvs(void* uvs, void* lineUvs);
    // Facing ribbons split for the view (`facing`, as runRibbons takes it): a segment `splitPixels`
    // or more wide at either end (its width times the scale, on screen) keeps its two triangles in
    // `indices` (six a point, the ribbons' triangles written over), a thinner one is drawn as its line
    // instead, into `lineIndices` (two uint32 a point: the ribbon's vertices on one side, so half a
    // pixel off the strand), each degenerate where the other draws it: lines draw thin strands far
    // faster than triangles a pixel wide. `points` and `lineUvs` as runRibbons reads them. Each view.
    void writeFacingSplit(void* indices, void* lineIndices, void* points, void* lineUvs, float widthScale, const RibbonFacing& facing,
                          float splitPixels);
    void runRibbons(void* points, void* lineUvs, float widthScale, bool lit, const RibbonTargets& targets, const float light[3],
                    const RibbonFacing* facing = nullptr);

    // ---- Following the scalp (follow_scalp.h; shaders/follow.slang) ----
    //
    // Strands evaluated on the scalp's rest pose (GroomSession following, ViewResult::follow) held
    // here, carried to the scalp as it is now by their roots' frames (the CPU's carryStrands, to
    // float rounding) into a host's points: a frame of playback sends the scalp's points and normals
    // alone (24 bytes a scalp vertex) where the carried strands took 12 bytes a point, and nothing of
    // the strands is carried on the CPU. Each strand's root normal on the scalp now is written too,
    // in place of setRootNormals' (the scalp's smooth normal under its root, up when unrooted: the
    // lit lines' StrandLines::writeRootNormals without feathers' vanes), for run() or runRibbons()
    // on the carried points after it.
    //
    // The strands at rest: the layout's points (float3 a point), uploaded unless `key` (the host's
    // name for them, never 0) is the one held. Returns the bytes sent.
    std::size_t setFollowRest(const float* points, std::uint64_t key);
    bool followRestHeld(std::uint64_t key) const { return key != 0 && key == followRestKey_; }
    // Each strand's root on the rest pose (follow_scalp.h writeFollowRoots, 32 bytes a strand) for
    // the layout's strands, unless `key` (its hash) is the one held.
    std::size_t setFollowRoots(const void* roots, std::uint64_t key);
    bool followRootsHeld(std::uint64_t key) const { return key != 0 && key == followRootsKey_; }
    // A scalp's `vertices` points and vertex normals (float3 each: ScalpMesh::points, vertexNormals):
    // the rest pose (`now` false) or the scalp now, unless `key` is the one held for it. The two
    // have the same vertices (the rest pose of the same mesh).
    std::size_t setFollowScalp(bool now, const float* points, const float* normals, std::size_t vertices, std::uint64_t key);
    // The strands at rest carried to the scalp now into `positions` (float3 a point, at least the
    // layout's points x 12 bytes), their root normals held for run() or runRibbons().
    void runFollow(void* positions);

    // ---- The hair's density (shaders/density.slang) ----
    //
    // The viewport's hair shading shadows the hair with itself (hosts/maya hair_shading.h): the
    // strand lines' points, as a host draws them, splatted into a grid of the hair's optical density
    // (how much of the light a unit of length through each cell blocks), and each cell's ambient
    // occlusion from it. No layout needed: each point stands for `pieceWeight` of its strand.
    struct DensityGrid {
        float origin[3] = {0.0f, 0.0f, 0.0f};  // the first cell's corner (world)
        float cell = 1.0f;                     // a cell's size (world)
        std::uint32_t size[3] = {1, 1, 1};     // cells along x, y, z
        // What a point of width 1 adds to its cells' density: (pi / 4) x its piece of strand (the mean
        // segment) / cell^3, over the preview's density (the render's strands it stands for).
        float pieceWeight = 0.0f;
        float defaultWidth = 0.01f;  // a point's width where its uvs hold none
        float occlusionReach = 6.0f; // cells the ambient occlusion looks along each axis
    };
    // `count` points (`positions`: float3 a point; `lineUvs`: float2, the width second) splatted
    // into `grid` and (density, occlusion) written into `volume`: DirectX 11, an ID3D11Texture3D of the
    // grid's size in R32G32_FLOAT, copied into (any bind flags); OpenGL, a 3D texture's name (GL_RG32F),
    // written as an image. The milliseconds on the GPU: gpuMs.
    void runDensity(void* positions, void* lineUvs, std::size_t count, const DensityGrid& grid, void* volume);

    // The last run's (and writeWidths') milliseconds on the GPU (timestamps); `wait` blocks until
    // known, else -1 when not known yet.
    virtual double gpuMs(bool wait) = 0;
    // `count` floats of a target (checks only: blocks on the GPU).
    virtual std::vector<float> read(void* target, std::size_t count) = 0;
    // The last runDensity's grid summed (each cell's optical density, the sigma it holds), and the
    // points it splatted (checks only: blocks on the GPU). A grid built again from the same points
    // sums the same: each run clears it first.
    double densitySum();
    std::size_t densityPoints() const { return densityPoints_; }
    // A density texture's texels (two floats each, rg, x fastest then y then z): `texture` the
    // host's (a runDensity target, or a view of one), null the engine's own the kernels wrote (and
    // runDensity copied into the target). Empty where the backend cannot read one (checks only:
    // blocks on the GPU).
    virtual std::vector<float> readVolume(void* texture) { (void)texture; return {}; }
    // A host's density texture written whole from `texels` (as readVolume gives them); false where
    // the backend cannot (or the sizes differ).
    virtual bool writeVolume(void* texture, const std::vector<float>& texels) { (void)texture; (void)texels; return false; }

    std::size_t strandCount() const { return strands_; }
    std::size_t pointCount() const { return points_; }

protected:
    LineFrames() = default;
    enum Buffer { kOffsets, kRootNormals, kWidths, kRibbonColors, kFollowRest, kFollowRoots, kFollowScalpRest, kFollowScalpNow, kBufferCount };
    enum class Kernel { Frames, Widths, Ribbons, RibbonLayout, RibbonColors, Follow, Count };
    // A buffer the kernels read, made (or made again when another size) to hold `bytes` of `data`
    // (null: made, nothing written). kRootNormals is one runFollow writes too.
    virtual void upload(Buffer buffer, const void* data, std::size_t bytes) = 0;
    // What a dispatch reads and writes (look.slang's resources): gIn `in` (the points; Widths: the
    // widths, kWidths), gAux0 kRibbonColors (the ribbons' kernels), gAux1 `lineUvs`; gOut, gOut2 and
    // gOut3 `out`, each at least `outBytes` long (null: not bound).
    struct Binding {
        void* in = nullptr;
        void* lineUvs = nullptr;
        void* out[3] = {};
        std::size_t outBytes[3] = {};
    };
    // One dispatch over `groupsX` x `groupsY` groups with these constants (look.slang's cbuffer:
    // look_engine_internal.h StageConstants). Frames and Ribbons are timed (gpuMs). Follow reads
    // the follow buffers (follow.slang: gStrandData the roots, gIn the points at rest, gAux0 and
    // gAux1 the scalps) and writes `out[0]` and the root normals (kRootNormals).
    virtual void dispatch(Kernel kernel, const void* constants, std::size_t constantBytes, const Binding& binding,
                          std::uint32_t groupsX, std::uint32_t groupsY) = 0;
    // The density's three dispatches (clear, splat, resolve), each `groups[k]` groups (x, y) with
    // `constants` (the threads in a row set per dispatch by the backend from groups' x), on a grid
    // of `cells` cells.
    virtual void dispatchDensity(void* constants, void* positions, void* lineUvs, std::size_t points, std::size_t cells,
                                 const std::uint32_t groups[3][2], void* volume) = 0;
    // The last density grid's cells (16.16 fixed point), `cells` of them (densitySum).
    virtual std::vector<std::uint32_t> readDensityGrid(std::size_t cells) = 0;

private:
    std::size_t densityCells_ = 0, densityPoints_ = 0;
    void dispatchPoints(Kernel kernel, const Binding& binding, const float* values = nullptr, std::uint32_t flag = 0,
                        std::uint32_t flagZ = 0, const float* values2 = nullptr);
    std::size_t strands_ = 0, points_ = 0, uniform_ = 0;
    std::uint64_t layoutKey_ = 0, normalsKey_ = 0, colorsKey_ = 0;
    bool hasLayout_ = false, hasNormals_ = false;
    std::uint64_t followRestKey_ = 0, followRootsKey_ = 0, followScalpKeys_[2] = {0, 0};
    std::size_t followVertices_[2] = {0, 0};
};

// On a DirectX 11 device (Maya's, or a test's).
std::unique_ptr<LineFrames> makeD3D11LineFrames(ID3D11Device* device);
// On the OpenGL context current on the calling thread (4.5).
std::unique_ptr<LineFrames> makeGLLineFrames();
// A DirectX 11 index buffer the ribbons' layout kernel may write (raw views, unordered access),
// `bytes` long: what a host hands Viewport 2.0 as its ribbons' indices.
ID3D11Buffer* createD3D11IndexBuffer(ID3D11Device* device, std::size_t bytes);

#ifdef __APPLE__
// On a Metal device (id<MTLDevice>; null: the system's default), on its one command stream with the
// look engines of it (look_engine.h makeMetalLookEngine). Targets are MTLBuffers (look_engine.h
// createMetalBuffer, or a host's), any of them; runDensity's an RG32Float 3D MTLTexture, written in
// place where it takes shader writes, else copied into.
std::unique_ptr<LineFrames> makeMetalLineFrames(void* device);
// A 3D texture of x * y * z RG32Float texels on `device` (null: the default) that compute may write:
// a density target. Retained for the caller (releaseMetalTexture).
void* createMetalVolume(void* device, unsigned x, unsigned y, unsigned z);
void releaseMetalTexture(void* texture);
#endif

}  // namespace groomlab::gpu
