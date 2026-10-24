// SPDX-License-Identifier: Apache-2.0
#pragma once

// Private to the draw override's files (groom_draw_override*.cpp): the render items' names, the
// report-only timings and the helpers more than one of them uses.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

#include <maya/MString.h>

#include "groom_draw_override.h"

#include <maya/MApiNamespace.h>  // Maya's classes declared (MHWRender's are using-declarations of a
                                  // versioned namespace's: a plain forward declaration conflicts, Clang/GCC)

namespace groomlab::maya {

// The draw overrides drawing each groom (test hooks: gpuFramesCheckJson, gpuFramesSwitched).
std::multimap<const GroomShape*, GroomSubSceneOverride*>& drawnOverrides();

inline const MString kStrandsItem("groomLabStrands");
inline const MString kDimStrandsItem("groomLabStrandsDim");
inline const MString kGuidesItem("groomLabGuides");
inline const MString kInstancesItem("groomLabInstances");
inline const MString kInstanceMeshesItem("groomLabInstanceMeshes");
inline const MString kScalesItem("groomLabScales");
inline const MString kColoredStrandsItem("groomLabStrandsColored");
inline const MString kRibbonsItem("groomLabRibbons");
// Facing ribbons' (true width) segments thinner than a pixel: lines drawn from the ribbons' own
// buffers with width as coverage (updateThinRibbons), the ribbons drawing the rest opaque.
inline const MString kThinRibbonsItem("groomLabRibbonsThin");
inline const MString kTubesItem("groomLabTubes");
inline const MString kSelectedItem("groomLabSelectedGuides");
inline const MString kPartsItem("groomLabParts");
inline const MString kFlowItem("groomLabFlowArrows");
inline const MString kCardsItem("groomLabCards");
inline const MString kCardWiresItem("groomLabCardWires");  // (not the builder's: updateCardWires)
inline const MString kScalpMapItem("groomLabScalpMap");
inline const MString kStrandMeshItem("groomLabStrandMesh");
inline const MString kFlowFieldItem("groomLabFlowField");
inline const MString kSoftSelectedItem("groomLabSoftSelectedGuides");
// The level of detail's items (updateLod): a class's strands, class 0's blendable segments, the
// impostors.
inline const MString kLodItemPrefix("groomLabStrandsLod");
inline const MString kLodBlendItem("groomLabStrandsLodBlend");
inline const MString kLodBlendOpaqueItem("groomLabStrandsLodBlendOpaque");
inline const MString kLodImpostorsItem("groomLabStrandsLodImpostors");

// The render item of each of the builder's items (DisplayItemId order).
inline const MString kItemNames[kDisplayItemCount] = {
    kDimStrandsItem, kStrandsItem,       kColoredStrandsItem, kRibbonsItem, kInstancesItem,
    kInstanceMeshesItem, kScalesItem,    kGuidesItem,         kTubesItem,   kSelectedItem, kSoftSelectedItem,
    kPartsItem,      kFlowItem,          kFlowFieldItem,      kCardsItem,   kScalpMapItem, kStrandMeshItem};

// ---- Shaders (groom_draw_override_fragments.cpp, groom_draw_override_shaders.cpp) ----

// Maya's hair graph adapted for the cards, the lit strands in their own colors, in one color, and
// the scales: registered once; false when this Maya's hair graph can't be adapted (said once).
bool cardShaderRegistered();
bool hairColorShaderRegistered();
bool plainHairShaderRegistered();
bool scaleShaderRegistered();
// The overlay effect (guides, parts, flow over the hair); null when it does not compile (said once).
MHWRender::MShaderInstance* acquireOverlayShader(const MHWRender::MShaderManager& shaders);
bool overlayShaderAvailable(const MHWRender::MShaderManager& shaders);
// The flat strand lines' effect with width as coverage; null when it does not compile (said once).
MHWRender::MShaderInstance* acquireLinesShader(const MHWRender::MShaderManager& shaders, bool colored);
bool linesShaderAvailable(const MHWRender::MShaderManager& shaders);

// The blend state of the lines' draws in a color pass (made once: alpha blending, for the
// fragments the top of the coverage range draws with an alpha of their own; the hashed ones are
// opaque) and the state each draw found (put back after); the draws by kind (stats): hashed (one
// sample a pixel) or sampled (a multisampled target).
struct CoverageBlend {
    const MHWRender::MBlendState* blend = nullptr;
    const MHWRender::MBlendState* kept = nullptr;
    bool set = false;
    long long hashed = 0, sampled = 0;
    // The level of detail's blend item (kLodBlendItem: class 0's segments that may blend, drawn
    // after the rest): depth tested, not written, so a blended fragment hides nothing drawn after
    // it; the depth state each such draw found, put back after.
    const MHWRender::MDepthStencilState* noDepthWrite = nullptr;
    const MHWRender::MDepthStencilState* keptDepth = nullptr;
    bool depthSet = false;
    long long blendItemDraws = 0;
};
CoverageBlend& coverageBlend();

// ---- Timings (groom_draw_override.cpp) ----

// Report-only timings of a viewport update (lastDrawTimesJson).
struct DrawTimes {
    struct Item {
        double acquireMs = 0, writeMs = 0, commitMs = 0, bindMs = 0, megabytes = 0;
        std::size_t partialStrands = 0;  // strands written alone (updateItemPartly); 0: the item written whole
        int refaced = 0;                 // facing ribbons made again for a new view alone (refaceRibbons)
        bool gpuFrames = false;          // lit lines whose frames the GPU wrote (gpu_lines.h)
        // ... the CPU's milliseconds of writeGpuLines: the points sent, the layout, the root normals
        // made, sent, the frames' kernel dispatched, the uvs.
        double gpuParts[6] = {};
        double gpuMegabytes = 0;  // ... and the megabytes it sent (of `megabytes`)
        bool follow = false;      // points the GPU carried to the scalp (carryOnGpu)
        // The streams it was bound to (bindGeometry): each name with its vertices, a * where the
        // buffer is the device's own (a custom handle: the GPU's lines, a GPU preview's).
        std::string streams;
    };
    double updateMs = 0, buildMs = 0, uploadMs = 0;
    // ... of updateMs: the groom's evaluation pulled (a dab's), the GPU preview's run (applyGpuPreview)
    // and the hair shading's density grid (updateHairGrid).
    double pullMs = 0, previewMs = 0, gridMs = 0, settingsMs = 0, afterMs = 0, lodMs = 0;
    std::map<std::string, Item> items;
    // Each item's shader as style() left it this update (lastDrawTimesJson "shaders"): its instance,
    // its kind, GroomLab's hair or not, width as coverage, the coverage the hair shader's draw
    // callbacks know it by (-1: not one of them), the coverage density and width scale given, and
    // how many items share the instance.
    struct Shader {
        std::uintptr_t instance = 0;
        int kind = -1;
        bool hair = false, coverage = false;
        int registered = -1;
        float lineDensity = 0.0f, widthScale = 0.0f;
    };
    std::map<std::string, Shader> shaders;
    std::uint64_t serial = 0;  // updates that wrote or ran something, counted (tests: a drag draws every step)
    bool facing = false;  // true width as this update left it: the strands as facing ribbons
};
DrawTimes& currentDraw();
// The level of detail as the last viewport update drew it (any groom's; lastDrawTimesJson "lod").
struct LodStats {
    bool on = false;          // the classes drew the strand lines
    int level = kNoLodLevel;  // the view's level
    std::size_t drawnStrands = 0, totalStrands = 0, groups = 0, thinnedGroups = 0, impostors = 0, blendSegments = 0;
    std::size_t items = 0;    // the classes' items drawn (the blend item counted)
    bool clumped = false;     // grouped by the clump channel (else by cells)
    double ms = 0.0;          // updateLod's milliseconds
    double indicesMs = 0.0;   // ... of it writing the indices (0: kept)
    double selectMs = 0.0;    // ... of it choosing the strands (StrandLod::select)
    std::size_t slotsWritten = 0;  // the index lists the last rebuild wrote (of kLodSlotCount)
    int farLevel = kNoLodLevel;  // the view's level at the farthest
    double megabytes = 0.0;   // the indices written
    long long updates = 0;    // updates that drew with it, and of those that wrote indices
    long long rebuilds = 0;
    long long slotsBound = 0;      // the classes' items bound to the strands' streams, all told
    std::uint64_t baseSerial = 0;  // the strands' item's last binding the classes drew (boundSerials_)
    std::size_t staleSlots = 0;    // drawn classes' items bound before it (0)
};
LodStats& lastLod();
DrawTimes& lastDraw();

// Milliseconds since the last lap (or the start).
struct Lap {
    std::chrono::steady_clock::time_point at = std::chrono::steady_clock::now();
    double operator()() {
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - at).count();
        at = now;
        return ms;
    }
};

// Whether the items' buffers keep a system-memory copy (DirectX 11) rather than living on the GPU
// alone (OpenGL); groom_draw_override.cpp says why.
bool softwareStaged();

}  // namespace groomlab::maya
