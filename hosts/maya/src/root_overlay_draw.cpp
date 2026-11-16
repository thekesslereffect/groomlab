// SPDX-License-Identifier: Apache-2.0
#include "root_overlay_draw.h"

#include <algorithm>
#include <cmath>

#include <maya/MColor.h>
#include <maya/MColorArray.h>
#include <maya/MHWGeometry.h>
#include <maya/MMatrix.h>
#include <maya/MPoint.h>
#include <maya/MPointArray.h>
#include <maya/MVector.h>

namespace groomlab::maya {

void drawRootOverlay(MHWRender::MUIDrawManager& draw, const MHWRender::MFrameContext& context,
                     const std::vector<RootTubeTool::Item>& items, float pixelScale) {
    using MHWRender::MUIDrawManager;
    using Item = RootTubeTool::Item;
    if (items.empty()) return;
    int ox = 0, oy = 0, width = 0, height = 0;
    context.getViewportDimensions(ox, oy, width, height);
    if (height <= 0) return;
    const MMatrix camera = context.getMatrix(MHWRender::MFrameContext::kViewInverseMtx);
    const MMatrix projection = context.getMatrix(MHWRender::MFrameContext::kProjectionMtx);
    const MVector right(camera(0, 0), camera(0, 1), camera(0, 2)), up(camera(1, 0), camera(1, 1), camera(1, 2));
    const MVector back(camera(2, 0), camera(2, 1), camera(2, 2));  // cameras look down -Z
    const MPoint eye(camera(3, 0), camera(3, 1), camera(3, 2));
    const bool ortho = std::fabs(projection(3, 3) - 1.0) < 1e-9;
    // World units a pixel at view depth d: 2 d / (P11 h) in perspective, 2 / (P11 h) orthographic.
    const double perPixel = 2.0 / (projection(1, 1) * static_cast<double>(height));
    auto depthOf = [&](const MPoint& p) { return ortho ? 1.0 : std::max(1e-6, -(p - eye) * back); };
    const double kStep = 0.0012;  // a layer: this share of the view's height there, toward the camera
    auto lifted = [&](const Vec3f& v, double steps) {
        const MPoint p(v.x, v.y, v.z);
        const MVector toEye = ortho ? back : (eye - p).normal();
        const double size = depthOf(p) * perPixel * static_cast<double>(height);
        return p + toEye * (kStep * steps * size);
    };
    auto color = [](const std::array<float, 3>& c, float a = 1.0f) { return MColor(c[0], c[1], c[2], a); };
    const MColor dark(0.08f, 0.08f, 0.08f, 1.0f);
    constexpr double kFill = 2.5, kEdge = 4.0, kDot = 6.5;
    const double dotSize = RootTubeTool::kDotPixels * pixelScale, edgeWidth = RootTubeTool::kEdgePixels * pixelScale;

    draw.beginDrawable(MUIDrawManager::kNonSelectable);
    draw.setDepthPriority(MHWRender::MRenderItem::sActiveWireDepthPriority);
    // The fills (see-through).
    MPointArray fill;
    MColorArray fillColors;
    for (const Item& item : items) {
        if (!(item.style & RootTubeTool::Filled)) continue;
        for (const Vec3f& p : item.triangles) {
            fill.append(lifted(p, kFill));
            fillColors.append(color(item.color, RootTubeTool::kFillOpacity));
        }
    }
    if (fill.length() > 0) draw.mesh(MUIDrawManager::kTriangles, fill, nullptr, &fillColors);
    // The edges (each a hair over the one before it, the highlighted over all) and the drawn path.
    for (std::size_t k = 0; k < items.size(); ++k) {
        const Item& item = items[k];
        if ((item.style & RootTubeTool::Marker) || item.edge.size() < 2) continue;
        const bool high = (item.style & RootTubeTool::Highlight) != 0;
        const bool stroke = (item.style & RootTubeTool::Stroke) != 0;
        const double steps = kEdge + 0.08 * static_cast<double>(k % 16) + (high ? 1.3 : 0.0);
        MPointArray strip;
        for (const Vec3f& p : item.edge) strip.append(stroke ? MPoint(p.x, p.y, p.z) : lifted(p, steps));
        if ((item.style & RootTubeTool::Closed) && item.edge.size() >= 3) strip.append(strip[0]);
        draw.setColor(stroke ? color(item.color) : color(RootTubeTool::edgeColor(item)));
        draw.setLineWidth(static_cast<float>(high ? 1.5 * edgeWidth : edgeWidth));
        draw.lineStrip(strip, false);
    }
    // The CVs and the middles: discs facing the camera, a constant size on the screen, a dark ring.
    constexpr int kSegments = 16;
    auto disc = [&](const Vec3f& at, double pixels, const MColor& inside, const MColor& ring, float ringWidth) {
        const MPoint c = lifted(at, kDot);
        const double r = 0.5 * pixels * perPixel * depthOf(c);
        MPointArray fan, edge;
        MColorArray colors;
        for (int s = 0; s < kSegments; ++s) {
            const double a0 = 6.283185307179586 * s / kSegments, a1 = 6.283185307179586 * (s + 1) / kSegments;
            fan.append(c);
            fan.append(c + (right * std::cos(a0) + up * std::sin(a0)) * r);
            fan.append(c + (right * std::cos(a1) + up * std::sin(a1)) * r);
            for (int v = 0; v < 3; ++v) colors.append(inside);
            edge.append(c + (right * std::cos(a0) + up * std::sin(a0)) * (r + 0.75 * perPixel * depthOf(c)));
        }
        edge.append(edge[0]);
        draw.mesh(MUIDrawManager::kTriangles, fan, nullptr, &colors);
        draw.setColor(ring);
        draw.setLineWidth(ringWidth);
        draw.lineStrip(edge, false);
    };
    for (const Item& item : items) {
        if (!(item.style & RootTubeTool::Marker) || item.edge.empty()) continue;
        const bool high = (item.style & RootTubeTool::Highlight) != 0;
        disc(item.edge.front(), std::max(1.6 * dotSize, 12.0), color(item.color), high ? color(RootTubeTool::kHighlight) : dark,
             high ? 2.5f : 1.5f);
    }
    for (const Item& item : items) {
        if (!(item.style & RootTubeTool::Dots)) continue;
        const bool dim = (item.style & RootTubeTool::Dim) != 0;
        const std::array<float, 3> d = RootTubeTool::kDotColor;
        const MColor c = dim ? MColor(d[0] * 0.45f, d[1] * 0.45f, d[2] * 0.45f, 1.0f) : color(d);
        for (const Vec3f& p : item.dots) disc(p, dim ? 0.8 * dotSize : dotSize, c, dark, 1.5f);
    }
    draw.endDrawable();
}

}  // namespace groomlab::maya
