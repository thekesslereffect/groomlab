// SPDX-License-Identifier: Apache-2.0
#pragma once

// GPU look preview in Maya. A preference, on by default: the optionVar groomLabGpuPreview (the
// GroomLab menu's Preview Look Drags on the GPU, groomlab.gpu_preview()), read when the plugin
// loads; GROOMLAB_GPU_PREVIEW=0 or 1 in the environment forces it. `groomLabGpuPreview -enable`
// turns it on or off for the session. Off, every drag and stroke previews on the CPU as before.
//
// Drags. While a param of a node the GPU runs (Interpolate's radius, Length, Clump, Noise, Curl,
// Width, their seeds and profiles: groomlab/gpu/look_chain.h previewableParam) is dragged,
// `groomLabGpuPreview -node n -param p -value v <groom>` (a profile: `-ramp <its keys' JSON>`)
// holds the value outside the graph (no evaluation, no undo); the draw override's next update runs
// that node and every one after it on the GPU from the strands entering it (the last evaluation's
// cached result, uploaded when it changes) on Viewport 2.0's own device, and the strands draw from
// the results: nothing comes back to the CPU. `-end` drops the values and the strands are drawn
// from the CPU again; the caller commits the drag as ever (set_param), which evaluates on the CPU.
//
// Brush strokes. Once a stroke's press is drawn (the CPU's evaluation of the stroke's document),
// when the graph from Interpolate to the output is the GPU's and Interpolate's binding, the center
// choice and the item drawing the strands allow it, the shape's evaluation of each later dab stops
// at Interpolate (ViewRequest::stopAt: the guides alone are evaluated) and the GPU blends the dab's
// guides into the strands, clumps and shapes them into what the strands item draws. A dab the GPU
// cannot take (guide roots moved, guides added) is evaluated by the CPU, and the rest of the stroke
// with it. The release evaluates on the CPU (incrementally, as ever) and replaces the preview.
//
// Anything the kernels do not do answers false and the caller falls back to the CPU preview (the
// graph written, evaluated). See engine/gpu/ (look_chain.h, look_engine.h, and look_preview.h:
// the planning, the stroke's arming and the base the engine holds, shared with the app).
//
// Where the results go. DirectX 11: Maya 2022 keeps a system-memory copy of every vertex buffer
// there (GPU-only ones cannot be written: groom_draw_override.cpp softwareStaged) and gives no
// buffer compute may write, so the preview makes its own (vertex buffers with unordered access,
// createD3D11DisplayBuffer), hands them to Viewport 2.0 as custom resource handles
// (MVertexBuffer::resourceHandle(handle, size)) and binds them to the strands item for the drag:
// the kernels write what the viewport draws, nothing is copied. When the drag ends the item
// draws its own buffers again (written by the CPU's evaluation) and the preview's go.
// GROOMLAB_GPU_PREVIEW_COPY=1 copies into the viewport's own buffers instead (comparisons).

#include <cstdint>
#include <memory>
#include <string>

#include <maya/MPxCommand.h>
#include <maya/MSyntax.h>

#include "groomlab/gpu/look_engine.h"
#include "groomlab/gpu/look_preview.h"

#include <maya/MApiNamespace.h>  // Maya's classes declared (MHWRender's are using-declarations of a
                                  // versioned namespace's: a plain forward declaration conflicts, Clang/GCC)

namespace groomlab::maya {

class GroomShape;

// Waits until Maya's device has done everything submitted (groomLabGpuPreview -sync); false without one.
bool waitForGpu();

// Whether drags preview on the GPU now (the preference, or the session's -enable).
bool gpuPreviewEnabled();
void setGpuPreviewEnabled(bool on);
// At plugin load: GROOMLAB_GPU_PREVIEW (0 or 1) if set, else the optionVar groomLabGpuPreview,
// else on.
void loadGpuPreviewPreference();

// A drag's values for one groom, and a version bumped by every change of them.
struct GpuPreviewRequest {
    gpu::Overrides overrides;
    std::uint64_t version = 0;
    bool active() const { return !overrides.empty(); }
};
// The groom's request (null when it never had one and `create` is false).
GpuPreviewRequest* gpuPreviewRequest(const GroomShape* shape, bool create = false);

// From the shape's evaluation of a live stroke's dab that stopped at the GPU's node: whether the
// GPU draws this dab (its guides fit what the GPU holds); false: the shape evaluates it on the CPU.
bool gpuStrokeHolds(GroomShape& shape);

// The vertex buffers of the item the strands draw as (lines): positions, and the lit lines'
// frames (null when unlit; no bitangent: the strands carry none, display.h); the uvs (u, width)
// with width as coverage (null without), where a Width the GPU runs writes its widths.
struct GpuItemBuffers {
    MHWRender::MVertexBuffer* positions = nullptr;
    MHWRender::MVertexBuffer* normals = nullptr;
    MHWRender::MVertexBuffer* tangents = nullptr;
    unsigned vertexCount = 0;
    bool remade = false;  // the preview's own buffers were made anew: the item must be bound to them
    MHWRender::MVertexBuffer* uvs = nullptr;
    // OpenGL: the buffers positions and uvs are, where their MVertexBuffers' handles do not name
    // them (custom resource handles: the ribbons' lines, gpu_lines.h GpuRibbonStreams).
    void* glPositions = nullptr;
    void* glUvs = nullptr;
};

// What a groom's draw override keeps for the preview: the engine on Maya's device, the preview's
// own vertex buffers (DirectX 11) and what the last run took. Its update calls apply() (a drag)
// or applyStroke() (a stroke's dab) after uploading its items.
class GpuLookPreview {
public:
    explicit GpuLookPreview(const GroomShape* shape);
    ~GpuLookPreview();

    // Runs the groom's graph from the first node the request's values change, with them, into the
    // viewport's buffers, or into the preview's own (*draw: what the item must draw from, bound by
    // the caller; all null: the viewport's own, written). False (and why()) when it cannot:
    // nothing was written.
    bool apply(GroomShape& shape, const GpuPreviewRequest& request, const GpuItemBuffers& item, GpuItemBuffers* draw);
    // The same for a live stroke's dab (from Interpolate on, the dab's guides).
    bool applyStroke(GroomShape& shape, const GpuItemBuffers& item, GpuItemBuffers* draw);
    // The preview's own vertex buffers go (call once the item draws from the viewport's again).
    void releaseDisplay();
    // DirectX 11: the preview's own buffer of `stream` (0 positions, 1 normals, 2 tangents, 3 the
    // lines' uvs: ID3D11Buffer*, made for compute), null where it has none: what a host reads its
    // run's lines from (the ribbons made from them).
    void* displayBuffer(int stream) const;
    const std::string& why() const { return why_; }
    // A box around everything the last run can have drawn (lookBounds): the item's bounds for the
    // drag, so the viewport culls none of the hair it pushed past the CPU's bounds.
    const gpu::LookBounds& bounds() const { return bounds_; }
    // The same with twice the reach: what the item is bound with when its bounds must grow, so a
    // drag going on further binds it again rarely (binding costs Viewport 2.0 more than a run).
    const gpu::LookBounds& roomyBounds() const { return roomyBounds_; }
    // Whether the last run wrote into the item's own buffers (OpenGL in place, or a copy into
    // DirectX 11's), not into the preview's; and of those, in place (OpenGL).
    bool wroteItemBuffers() const;
    // Whether the last run wrote widths into the item's own uvs (OpenGL, in place).
    bool wroteItemUvs() const;
    bool writesItemInPlace() const;
    // The override's word on whether the strands are drawn so a run can write them (lines, the
    // output's point count), with why not.
    void setDrawable(bool drawable, const std::string& why);
    // Whether the GPU can draw the groom as it is with these values (a device it runs on, the
    // graph's chain, plain params, the cached strands it starts from, an item to draw them);
    // false with *why.
    bool accepts(GroomShape& shape, const gpu::Overrides& overrides, std::string* why);

    // A live stroke (the override calls these as the shape's preview comes and goes). arm: after
    // the press is drawn, whether the GPU takes the next dabs (the shape's evaluation then stops at
    // Interpolate: GroomShape::setGpuStroke); end: the stroke is over (or the GPU gave it up).
    bool armStroke(GroomShape& shape);
    void endStroke(GroomShape& shape);
    bool strokeArmed() const { return core_.strokeArmed(); }
    // The shape's evaluation stopped at the GPU's node for a dab: whether the GPU takes it (the
    // plan is kept for the draw), counted in strokeDabs().
    bool strokeHolds(GroomShape& shape);
    std::uint64_t strokeDabs() const { return strokeDabs_; }

    // {"api", "runs", "cpuMs" (apply on the CPU: params, uploads when needed, dispatch and copy
    // submitted), "gpu": {stage ms by timestamps}, "inPlace", "uploadMs", "uploadMB", "uploads",
    // "uploadPartial", "uploadStrands", "runUploadMB", "runUploadMs", "fieldsMB", "fieldsReadMs",
    // "inputsReadMs", "stages" (the last run's nodes), "stroke" (armed, dabs, GPU runs, why not),
    // "bounds" (the drawn item's, grown: x y z lo, x y z hi), "videoMB" (the engine's buffers),
    // "displayMB" (the preview's own vertex buffers), "vp2Buffer": the drawn buffer's description,
    // "why"}. `wait`: the GPU times waited for.
    std::string statsJson(bool wait);
    // Runs the graph's GPU nodes (from `from`: a node's name, else the first the GPU runs) with
    // the graph's own values on the CPU's last result and compares (reads back: checks only):
    // {"positions": {"max", "mean"}, "normals", "tangents", "widths", "stages"}.
    std::string checkJson(GroomShape& shape, const std::string& from = {});

private:
    // accepts()'s checks, giving what a run takes (from the first stage `overrides` change, or
    // from `start`'s node): a device the viewport draws with, then look_preview.h's plan.
    bool plan(GroomShape& shape, const gpu::Overrides& overrides, const gpu::LookStart& start, gpu::LookPlan* plan,
              std::string* why);
    // Where the strands the viewport shows come from: the shape, evaluated when dirty (from the
    // draw), or the session's last evaluation (from the shape's evaluation, which may run on
    // another thread).
    static gpu::LookPreview::Shown shown(GroomShape& shape, bool inCompute);
    bool runPlan(GroomShape& shape, const gpu::LookPlan& plan, const GpuItemBuffers& item, GpuItemBuffers* draw);
    bool ensureEngine();
    // DirectX 11: the preview's own vertex buffers for `count` vertices (lit: the frames too;
    // `uvs`: the lines' uvs, their u from `output`'s point counts), made and handed to Viewport 2.0
    // when missing or of another size; true when made anew.
    bool ensureDisplay(unsigned count, bool lit, const StrandsPtr& uvs);
    const GroomShape* shape_;
    std::unique_ptr<gpu::LookEngine> engine_;
    gpu::LookPreview core_;    // planning, the stroke, the base the engine holds
    std::string baseChanges_;  // what changesSince said at the last setBase (stats)
    gpu::LookBounds bounds_, roomyBounds_;
    struct Display;
    std::unique_ptr<Display> display_;
#ifdef GROOMLAB_HAS_METAL
    // macOS (Metal): the run's buffers in memory the GPU shares (positions, normals, tangents
    // float3; the lines' uvs float2), copied into the viewport's own after it.
    struct MetalCopy {
        void* buffers[4] = {};
        unsigned capacity = 0, count = 0;
        bool copied = false, copiedUvs = false;  // the last run's results went into the item's own (uvs: widths)
        MetalCopy() = default;
        MetalCopy(const MetalCopy&) = delete;
        MetalCopy& operator=(const MetalCopy&) = delete;
        ~MetalCopy();
        void release();
        // Buffers for `count` points (`uvs`: the lines' uvs written from these strands' point counts).
        void ensure(unsigned count, const StrandsPtr& uvs);
        void copyInto(const GpuItemBuffers& item, bool lit, bool uvs);
    };
    MetalCopy metal_;
#endif
    std::string why_, vp2Buffer_, notDrawable_, stages_;
    bool drawable_ = false;
    std::size_t runs_ = 0, strokeRuns_ = 0;
    double cpuMs_ = 0;
    // A live stroke (core_'s): the dabs it held, why it is not the GPU's.
    std::uint64_t strokeDabs_ = 0;
    std::string strokeWhy_;
    // The last run's CPU milliseconds by part (stats), and the slowest of a stroke's dabs.
    struct RunTimes {
        double cpuMs = 0, setBaseMs = 0, runMs = 0, prepareMs = 0, runUploadMB = 0, parts[3] = {};
        std::size_t baseUploaded = 0;
    };
    RunTimes lastRun_, slowestStroke_;
};

// The preview of a groom whose draw override has one (null otherwise).
GpuLookPreview* gpuLookPreviewFor(const GroomShape* shape);
// OpenGL: the GPU preview's kernels started on Viewport 2.0's context (from the program cache, else
// compiled by the driver in the background), and looked at again while they compile: a groom's
// draw calls it, so the first drag or stroke finds them ready (until then the CPU draws them).
// Cheap once they are; nothing on DirectX 11.
void prepareGpuKernels();

// groomLabGpuPreview [-node n -param p -value v | -endDrag | -stats | -check [-from node] | -sync |
//                     -drawApi | -enable on|off | -isEnabled | -frames on|off | -checkFrames] <groom>
// (-frames: the lit lines' frames from the GPU, gpu_lines.h; -checkFrames: the drawn strands' buffers against
// the CPU's, groom_draw_override.h gpuFramesCheckJson)
class GpuPreviewCmd : public MPxCommand {
public:
    static const char* kName;
    static void* creator();
    static MSyntax newSyntax();
    MStatus doIt(const MArgList& args) override;
};

}  // namespace groomlab::maya
