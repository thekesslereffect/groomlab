// SPDX-License-Identifier: Apache-2.0
#pragma once

// A small CPU renderer for looking at a groom without a GPU (the agent tools' render_preview,
// agent_tools.h): the scalp shaded flat with a key light, the strands drawn as z-buffered lines in
// the hair's own color (its appearance color, export_view.h) with a simple hair highlight, from one
// or several fixed views laid out in a grid. Orthographic, rendered at twice the size and filtered down.
// Deterministic: the same groom and options give the same pixels on any machine and thread count.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "groomlab/vec3.h"

namespace groomlab {

class ScalpMesh;
class StrandSet;

namespace agent {

// The views render_preview takes: "front", "side" (from +X), "back", "three_quarter", "top",
// "left" (the head's left side: from +X, as "side"), "right" (from -X). A Y-up head faces +Z (a
// Z-up one -Y: the views turn with it).
const std::vector<std::string>& previewViewNames();

struct PreviewOptions {
    int size = 512;                                 // the image's width (its height fits the grid); 64-1024
    std::vector<std::string> views{"three_quarter"};  // laid out left to right, top to bottom
    std::size_t maxStrands = 50000;                 // more than this: every k-th strand is drawn
    bool zUp = false;                               // the points are Z-up (Blender, Houdini)
    // Guide curves drawn over the hair (world points, root first), bright, hidden only by the scalp.
    std::vector<std::vector<Vec3f>> guides;
};

struct PreviewImage {
    int width = 0, height = 0;
    std::vector<std::uint8_t> png;  // the encoded image
    std::size_t strandsTotal = 0, strandsDrawn = 0, stride = 1;
    int columns = 1;
};

// Throws std::invalid_argument for an unknown view or a size out of range.
PreviewImage renderPreview(const ScalpMesh* scalp, const StrandSet* strands, const PreviewOptions& options);

// Standard base64 (for an MCP image's data).
std::string base64Encode(const std::vector<std::uint8_t>& bytes);

}  // namespace agent
}  // namespace groomlab
