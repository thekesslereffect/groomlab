// SPDX-License-Identifier: Apache-2.0
#pragma once

// The groom editor's tool overlay (engine/editor), every host's: what the brush tool draws over the
// groom while hovering or stroking, worked out once. The ring is described in screen pixels
// (brushRing: Maya draws it in 2D); hosts drawing lines in the world (the app, Blender) take the
// whole overlay as colored lines (brushOverlay).

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "groomlab/display.h"
#include "groomlab/stroke.h"
#include "groomlab/view_projection.h"

namespace groomlab {

// One circle of the brush ring, in screen pixels.
struct RingCircle {
    float radius = 0.0f;               // pixels from the cursor
    std::array<float, 3> color{1.0f, 1.0f, 1.0f};
    bool dashed = false;               // long dashes, short gaps
    bool halo = false;                 // the dark (or light) edge the ring reads on any hair against
};

// What a held key does to the next stroke, shown on the ring.
enum class RingHint { None, Reverse, Smooth };

// The brush ring every host draws, as circles in pixels around the cursor: a dot for the tools
// that make guides (Place, Draw, Length Keys); else the ring at the brush's screen radius in its
// color (brushRingColor; `tone` for the shaping brushes when the host has a theme tone), thickened
// to about two pixels with a halo just outside and inside it (dark under a light ring, light under
// a dark one), dashed and red while Ctrl reverses, green while Shift smooths; and the falloff, a
// dimmer dashed ring where full strength ends (hardness of the radius).
std::vector<RingCircle> brushRing(const BrushToolSettings& settings, RingHint hint = RingHint::None,
                                  const std::array<float, 3>* tone = nullptr);

// The scalp's line of symmetry kept while the scalp and symmetry stay the same (symmetrySeam).
struct SeamCache {
    std::uint64_t key = 0;
    std::vector<Vec3f> points;  // segment pairs
};

// The overlay as asked.
struct OverlayRequest {
    float x = 0.0f, y = 0.0f;          // the cursor (the view's pixels)
    bool withLines = true;             // the guides the brush would touch (hovering, not while stroking)
    RingHint hint = RingHint::None;
    const std::array<float, 3>* tone = nullptr;
    // Where the ring sits off the scalp (the host's camera target depth under the pixel); null: no ring there.
    std::function<bool(float x, float y, Vec3f& at)> offScalp;
};

// The overlay drawn.
struct BrushOverlay {
    DisplayGeometry lines;             // colored line strips (overlay)
    bool ring = false, ringOnScalp = false;
    Vec3f ringCenter, ringNormal;
    float ringRadius = 0.0f;           // the ring's world radius
};

// The brush overlay at the cursor, as every world-drawing host shows it: the guides the brush would
// touch (brushHoverColor), the scalp's line of symmetry (red, with symmetry on), the ring (brushRing
// laid on the scalp under the cursor, else where `offScalp` puts it, facing the camera, as wide as
// the brush there: worldRadiusAt) and a Part or Flow stroke's path so far (brushPathColor). Not the
// root tool's outlines nor the Length Keys' keys (the host adds them). `host` and the scalp may be
// missing: what needs them is left out.
BrushOverlay brushOverlay(BrushTool& tool, StrokeHost* host, const ToolView& view, const OverlayRequest& request, SeamCache& seam);

// Points appended to an overlay as one colored line strip (the overlay's own geometry).
void appendColoredLine(DisplayGeometry& g, const std::vector<Vec3f>& points, const std::array<float, 3>& color);

}  // namespace groomlab
