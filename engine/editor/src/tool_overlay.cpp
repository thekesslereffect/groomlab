// SPDX-License-Identifier: Apache-2.0
#include "groomlab/editor/tool_overlay.h"

#include <algorithm>
#include <cmath>

#include "groomlab/brush.h"
#include "groomlab/random.h"

namespace groomlab {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kRingSegments = 96;
constexpr float kDotPixels = 6.0f;  // the guide-making tools' dot

bool makesGuides(const BrushToolSettings& s) {
    using Tool = BrushToolSettings::Tool;
    return s.tool == Tool::Place || s.tool == Tool::Draw || s.tool == Tool::Lengths;  // (Lengths: the keys' rings)
}

}  // namespace

void appendColoredLine(DisplayGeometry& g, const std::vector<Vec3f>& points, const std::array<float, 3>& color) {
    if (points.size() < 2) return;
    const auto first = static_cast<std::uint32_t>(g.positions.size() / 3);
    for (const Vec3f& p : points) {
        g.positions.insert(g.positions.end(), {p.x, p.y, p.z});
        g.colors.insert(g.colors.end(), {color[0], color[1], color[2], 1.0f});
    }
    for (std::uint32_t k = 0; k + 1 < points.size(); ++k) g.indices.insert(g.indices.end(), {first + k, first + k + 1});
}

std::vector<RingCircle> brushRing(const BrushToolSettings& s, RingHint hint, const std::array<float, 3>* tone) {
    std::vector<RingCircle> out;
    if (makesGuides(s)) {
        out.push_back({kDotPixels, brushRingColor(s), false, false});
        return out;
    }
    // The shaping brushes' ring in the host's tone (the Part, Flow and erasing tools keep their own);
    // a held key's state on the ring itself.
    const bool shaping = s.tool == BrushToolSettings::Tool::Brush && s.brush.mode != BrushMode::Erase;
    std::array<float, 3> color = shaping && tone ? *tone : brushRingColor(s);
    if (hint == RingHint::Reverse) color = {0.89f, 0.09f, 0.10f};  // Ctrl: the error red, dashed
    if (hint == RingHint::Smooth) color = {0.12f, 0.66f, 0.32f};   // Shift: smoothing's green
    const bool dashed = hint == RingHint::Reverse;
    // A ring that reads on any hair: a halo just outside and inside it, then the color thickened to
    // about two pixels.
    const auto stroke = [&](float r, const std::array<float, 3>& c, bool dash) {
        const float lum = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
        const std::array<float, 3> halo = lum > 0.18f ? std::array<float, 3>{0.0f, 0.0f, 0.0f} : std::array<float, 3>{1.0f, 1.0f, 1.0f};
        out.push_back({r + 1.6f, halo, dash, true});
        out.push_back({std::max(0.0f, r - 1.6f), halo, dash, true});
        out.push_back({r, c, dash, false});
        out.push_back({r + 0.6f, c, dash, false});
    };
    const float r = s.screenRadius;
    stroke(r, color, dashed);
    // The falloff: a dashed inner ring where the full strength ends, a step dimmer.
    const float hard = s.brush.hardness;
    if (hard > 0.04f && hard < 0.96f) {
        std::array<float, 3> inner = color;
        for (float& c : inner) c *= 0.7f;
        stroke(r * hard, inner, true);
    }
    return out;
}

BrushOverlay brushOverlay(BrushTool& tool, StrokeHost* host, const ToolView& view, const OverlayRequest& request, SeamCache& seam) {
    BrushOverlay out;
    out.lines.overlay = true;
    const BrushToolSettings& s = tool.settings;
    using Tool = BrushToolSettings::Tool;
    const float x = request.x, y = request.y;
    if (request.withLines && host) {
        // The guides the brush would touch, as the core draws them for the hover (smoothed).
        std::vector<std::vector<Vec3f>> lines;
        try {
            lines = tool.affectedLines(*host, view, x, y);
        } catch (const std::exception&) {
            lines.clear();
        }
        for (const std::vector<Vec3f>& line : lines) appendColoredLine(out.lines, line, brushHoverColor(s));
    }
    const MeshPtr scalp = host ? host->scalp() : nullptr;
    // Symmetry on: the scalp's line of symmetry in red.
    if (scalp && scalp->triangleCount() > 0) {
        const StrokeSymmetry sym = StrokeSymmetry::of(s.brush, scalp.get());
        if (sym.on()) {
            const std::uint64_t key = hashCombine(hashCombine(scalp->contentHash(), static_cast<std::uint64_t>(sym.axis + 1)),
                                                  hashCombine(hashFloat(hash64(0x5EA4), sym.center), sym.topology() ? 1u : 0u));
            if (key != seam.key) {
                seam.points = symmetrySeam(*scalp, sym);
                seam.key = key;
            }
            for (std::size_t k = 0; k + 1 < seam.points.size(); k += 2) {
                appendColoredLine(out.lines, {seam.points[k], seam.points[k + 1]}, {1.0f, 0.15f, 0.15f});
            }
        }
    }
    // The ring: on the scalp under the cursor, else where the host puts it, facing the camera.
    Vec3f origin, direction;
    if (view.ray(x, y, origin, direction)) {
        Vec3f center;
        bool have = false;
        SurfacePoint hit;
        if (scalp && scalp->triangleCount() > 0 && scalp->intersectRay(origin, direction, &hit)) {
            center = hit.position;
            have = out.ringOnScalp = true;
        } else if (request.offScalp) {
            have = request.offScalp(x, y, center);
        }
        const float screen = makesGuides(s) ? kDotPixels : s.screenRadius;
        const float radius = have ? worldRadiusAt(view, center, x, y, screen) : 0.0f;
        if (radius > 0.0f) {
            Vec3f eye, along;
            bool ortho = false;
            view.camera(eye, along, ortho);
            const Vec3f n = ortho ? along : (center - eye).normalized();
            const Vec3f u = (std::abs(n.y) < 0.9f ? Vec3f{0.0f, 1.0f, 0.0f} : Vec3f{1.0f, 0.0f, 0.0f}).cross(n).normalized();
            const Vec3f v = n.cross(u);
            const float px = radius / std::max(1.0f, screen);  // one screen pixel at the ring's depth
            for (const RingCircle& c : brushRing(s, request.hint, request.tone)) {
                const float r = c.radius * px;
                std::vector<Vec3f> pts;
                for (int k = 0; k <= kRingSegments; ++k) {
                    const float a = 2.0f * kPi * static_cast<float>(k) / kRingSegments;
                    pts.push_back(center + u * (std::cos(a) * r) + v * (std::sin(a) * r));
                }
                if (!c.dashed) {
                    appendColoredLine(out.lines, pts, c.color);
                } else {
                    for (std::size_t k = 0; k + 3 < pts.size(); k += 4) appendColoredLine(out.lines, {pts[k], pts[k + 1], pts[k + 2], pts[k + 3]}, c.color);
                }
            }
            out.ring = true;
            out.ringCenter = center;
            out.ringRadius = radius;
            out.ringNormal = n;
        }
    }
    // A Part or Flow stroke's path so far, until it is committed.
    if (tool.stroking() && (s.tool == Tool::Part || s.tool == Tool::Flow)) appendColoredLine(out.lines, tool.drawnPath(), brushPathColor(s));
    return out;
}

}  // namespace groomlab
