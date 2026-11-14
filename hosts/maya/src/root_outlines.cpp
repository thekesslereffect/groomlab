// SPDX-License-Identifier: Apache-2.0
#include "root_outlines.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <maya/MColor.h>
#include <maya/MColorArray.h>
#include <maya/MDagPath.h>
#include <maya/MFnIntArrayData.h>
#include <maya/MFnNumericAttribute.h>
#include <maya/MFnTypedAttribute.h>
#include <maya/MFnVectorArrayData.h>
#include <maya/MFrameContext.h>
#include <maya/MHWGeometry.h>
#include <maya/MIntArray.h>
#include <maya/MMatrix.h>
#include <maya/MPlug.h>
#include <maya/MPoint.h>
#include <maya/MPointArray.h>
#include <maya/MUIDrawManager.h>
#include <maya/MUserData.h>
#include <maya/MVectorArray.h>

#include "node_ids.h"

namespace groomlab::maya {

const MTypeId RootOutlines::kTypeId(kGroomLabRootOutlinesTypeId);
const MString RootOutlines::kTypeName("groomLabRootOutlines");
const MString RootOutlines::kDrawDbClassification("drawdb/geometry/groomLabRootOutlines");
const MString RootOutlines::kRegistrantId("groomLabRootOutlinesOverride");

MObject RootOutlines::aPoints;
MObject RootOutlines::aCounts;
MObject RootOutlines::aColors;
MObject RootOutlines::aNormals;
MObject RootOutlines::aStyles;
MObject RootOutlines::aFillPoints;
MObject RootOutlines::aFillOwners;
MObject RootOutlines::aDots;
MObject RootOutlines::aDotOwners;
MObject RootOutlines::aDotColor;
MObject RootOutlines::aEdgeColor;
MObject RootOutlines::aDotSize;
MObject RootOutlines::aEdgeWidth;
MObject RootOutlines::aFillOpacity;

MStatus RootOutlines::initialize() {
    MFnTypedAttribute typed;
    MFnNumericAttribute numeric;
    MFnVectorArrayData vectors;
    MFnIntArrayData ints;

    auto vectorArray = [&](const char* name, const char* brief) {
        MObject a = typed.create(name, brief, MFnData::kVectorArray, vectors.create());
        typed.setStorable(false);
        return a;
    };
    auto intArray = [&](const char* name, const char* brief) {
        MObject a = typed.create(name, brief, MFnData::kIntArray, ints.create());
        typed.setStorable(false);
        return a;
    };
    aPoints = vectorArray("points", "pts");
    aCounts = intArray("counts", "cnt");
    aColors = vectorArray("colors", "cls");
    aNormals = vectorArray("normals", "nrm");
    aStyles = intArray("styles", "sty");
    aFillPoints = vectorArray("fillPoints", "fpt");
    aFillOwners = intArray("fillOwners", "fow");
    aDots = vectorArray("dots", "dts");
    aDotOwners = intArray("dotOwners", "dow");
    aDotColor = numeric.createColor("dotColor", "dcl");
    numeric.setDefault(1.0f, 0.62f, 0.2f);
    aEdgeColor = numeric.createColor("edgeColor", "ecl");
    numeric.setDefault(0.3f, 1.0f, 0.4f);
    aDotSize = numeric.create("dotSize", "dsz", MFnNumericData::kFloat, 9.0);
    numeric.setMin(1.0);
    aEdgeWidth = numeric.create("edgeWidth", "ewd", MFnNumericData::kFloat, 3.0);
    numeric.setMin(0.5);
    aFillOpacity = numeric.create("fillOpacity", "fop", MFnNumericData::kFloat, 0.5);
    numeric.setMin(0.0);
    numeric.setMax(1.0);

    for (MObject* a : {&aPoints, &aCounts, &aColors, &aNormals, &aStyles, &aFillPoints, &aFillOwners, &aDots, &aDotOwners,
                       &aDotColor, &aEdgeColor, &aDotSize, &aEdgeWidth, &aFillOpacity}) {
        const MStatus status = addAttribute(*a);
        CHECK_MSTATUS_AND_RETURN_IT(status);
    }
    return MS::kSuccess;
}

namespace {

struct Outline {
    std::vector<MPoint> points;
    MColor color;
    int style = 0;
};

class RootOutlinesData : public MUserData {
public:
    RootOutlinesData() = default;
    std::vector<Outline> outlines;
    std::vector<MPoint> fills;      // three a triangle
    std::vector<int> fillOwners;    // a triangle's outline
    std::vector<MPoint> dots;
    std::vector<int> dotOwners;
    MColor dotColor, edgeColor;
    double dotSize = 9.0, edgeWidth = 3.0;
    float fillOpacity = 0.5f;
};

MVectorArray vectorPlug(const MObject& node, const MObject& attr) {
    const MObject value = MPlug(node, attr).asMObject();
    return value.isNull() ? MVectorArray() : MFnVectorArrayData(value).array();
}

MIntArray intPlug(const MObject& node, const MObject& attr) {
    const MObject value = MPlug(node, attr).asMObject();
    return value.isNull() ? MIntArray() : MFnIntArrayData(value).array();
}

MColor colorPlug(const MObject& node, const MObject& attr) {
    const MPlug plug(node, attr);
    return MColor(plug.child(0).asFloat(), plug.child(1).asFloat(), plug.child(2).asFloat(), 1.0f);
}

}  // namespace

MUserData* RootOutlinesDrawOverride::prepareForDraw(const MDagPath& objPath, const MDagPath&,
                                                    const MHWRender::MFrameContext&, MUserData* oldData) {
    auto* data = dynamic_cast<RootOutlinesData*>(oldData);
    if (!data) data = new RootOutlinesData();
    data->outlines.clear();
    data->fills.clear();
    data->fillOwners.clear();
    data->dots.clear();
    data->dotOwners.clear();
    try {
        const MObject node = objPath.node();
        const MVectorArray points = vectorPlug(node, RootOutlines::aPoints);
        const MVectorArray colors = vectorPlug(node, RootOutlines::aColors);
        const MIntArray counts = intPlug(node, RootOutlines::aCounts);
        const MIntArray styles = intPlug(node, RootOutlines::aStyles);
        unsigned int at = 0;
        for (unsigned int k = 0; k < counts.length(); ++k) {
            const unsigned int n = static_cast<unsigned int>(std::max(0, counts[k]));
            if (at + n > points.length()) break;
            Outline o;
            o.points.reserve(n);
            for (unsigned int i = 0; i < n; ++i) o.points.emplace_back(points[at + i]);
            at += n;
            const MVector c = k < colors.length() ? colors[k] : MVector(1.0, 1.0, 1.0);
            o.color = MColor(static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z), 1.0f);
            o.style = k < styles.length() ? styles[k] : 0;
            data->outlines.push_back(std::move(o));
        }
        const MVectorArray fills = vectorPlug(node, RootOutlines::aFillPoints);
        const MIntArray fillOwners = intPlug(node, RootOutlines::aFillOwners);
        for (unsigned int t = 0; t < fillOwners.length() && 3 * t + 2 < fills.length(); ++t) {
            const int owner = fillOwners[t];
            if (owner < 0 || static_cast<std::size_t>(owner) >= data->outlines.size()) continue;
            for (unsigned int c = 0; c < 3; ++c) data->fills.emplace_back(fills[3 * t + c]);
            data->fillOwners.push_back(owner);
        }
        const MVectorArray dots = vectorPlug(node, RootOutlines::aDots);
        const MIntArray dotOwners = intPlug(node, RootOutlines::aDotOwners);
        for (unsigned int i = 0; i < dots.length(); ++i) {
            data->dots.emplace_back(dots[i]);
            data->dotOwners.push_back(i < dotOwners.length() ? dotOwners[i] : -1);
        }
        data->dotColor = colorPlug(node, RootOutlines::aDotColor);
        data->edgeColor = colorPlug(node, RootOutlines::aEdgeColor);
        data->dotSize = std::max(1.0, static_cast<double>(MPlug(node, RootOutlines::aDotSize).asFloat()));
        data->edgeWidth = std::max(0.5, static_cast<double>(MPlug(node, RootOutlines::aEdgeWidth).asFloat()));
        data->fillOpacity = std::clamp(MPlug(node, RootOutlines::aFillOpacity).asFloat(), 0.0f, 1.0f);
    } catch (...) {
        data->outlines.clear();  // nothing thrown into Maya's draw
        data->fills.clear();
        data->dots.clear();
    }
    return data;
}

void RootOutlinesDrawOverride::addUIDrawables(const MDagPath&, MHWRender::MUIDrawManager& draw,
                                              const MHWRender::MFrameContext& context, const MUserData* userData) {
    using MHWRender::MUIDrawManager;
    const auto* data = dynamic_cast<const RootOutlinesData*>(userData);
    if (!data || (data->outlines.empty() && data->dots.empty())) return;
    try {
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
        // Lifted toward the camera by `steps` of a small share of the view's size there (layer over
        // layer: the scalp, the fills, the edges, the dots).
        const double kStep = 0.0012;
        auto lifted = [&](const MPoint& p, double steps) {
            const MVector toEye = ortho ? back : (eye - p).normal();
            const double size = depthOf(p) * perPixel * static_cast<double>(height);  // the view's height there
            return p + toEye * (kStep * steps * size);
        };
        const MColor dark(0.08f, 0.08f, 0.08f, 1.0f);
        // (The fills well clear of the scalp's own depth: never fighting it, wherever the scalp
        // curves between the fill's rings; the edges over the fills, the dots over the edges.)
        constexpr double kFill = 2.5, kEdge = 4.0, kDot = 6.5;

        draw.beginDrawable(MUIDrawManager::kNonSelectable);
        draw.setDepthPriority(MHWRender::MRenderItem::sActiveWireDepthPriority);
        // The fills (see-through).
        if (!data->fills.empty() && data->fillOpacity > 0.0f) {
            MPointArray positions;
            MColorArray colors;
            for (std::size_t t = 0; t < data->fillOwners.size(); ++t) {
                MColor c = data->outlines[static_cast<std::size_t>(data->fillOwners[t])].color;
                c.a = data->fillOpacity;
                for (std::size_t v = 0; v < 3; ++v) {
                    positions.append(lifted(data->fills[3 * t + v], kFill));
                    colors.append(c);
                }
            }
            draw.mesh(MUIDrawManager::kTriangles, positions, nullptr, &colors);
        }
        // The edges: the root's color lightened, or green and thicker (edited or selected); each
        // outline a hair over the one before it, the highlighted over all.
        for (std::size_t k = 0; k < data->outlines.size(); ++k) {
            const Outline& o = data->outlines[k];
            if ((o.style & RootOutlines::kMarker) || o.points.size() < 2) continue;
            const bool high = (o.style & RootOutlines::kHighlight) != 0;
            const double steps = kEdge + 0.08 * static_cast<double>(k % 16) + (high ? 1.3 : 0.0);
            MPointArray strip;
            for (const MPoint& p : o.points) strip.append(lifted(p, steps));
            if ((o.style & RootOutlines::kClosed) && o.points.size() >= 3) strip.append(strip[0]);
            const MColor c = o.color;
            draw.setColor(high ? data->edgeColor : MColor(0.6f * c.r + 0.4f, 0.6f * c.g + 0.4f, 0.6f * c.b + 0.4f, 1.0f));
            draw.setLineWidth(static_cast<float>(high ? 1.5 * data->edgeWidth : data->edgeWidth));
            draw.lineStrip(strip, false);
        }
        // The CVs and the middles: discs facing the camera, a constant size on the screen, a dark ring.
        constexpr int kSegments = 16;
        auto disc = [&](const MPoint& at, double pixels, const MColor& fill, const MColor& ring, float ringWidth) {
            const MPoint c = lifted(at, kDot);
            const double r = 0.5 * pixels * perPixel * depthOf(c);
            MPointArray fan, edge;
            MColorArray colors;
            for (int s = 0; s < kSegments; ++s) {
                const double a0 = 6.283185307179586 * s / kSegments, a1 = 6.283185307179586 * (s + 1) / kSegments;
                fan.append(c);
                fan.append(c + (right * std::cos(a0) + up * std::sin(a0)) * r);
                fan.append(c + (right * std::cos(a1) + up * std::sin(a1)) * r);
                for (int v = 0; v < 3; ++v) colors.append(fill);
                edge.append(c + (right * std::cos(a0) + up * std::sin(a0)) * (r + 0.75 * perPixel * depthOf(c)));
            }
            edge.append(edge[0]);
            draw.mesh(MUIDrawManager::kTriangles, fan, nullptr, &colors);
            draw.setColor(ring);
            draw.setLineWidth(ringWidth);
            draw.lineStrip(edge, false);
        };
        for (const Outline& o : data->outlines) {
            if (!(o.style & RootOutlines::kMarker) || o.points.empty()) continue;
            const bool high = (o.style & RootOutlines::kHighlight) != 0;
            disc(o.points[0], std::max(1.6 * data->dotSize, 12.0), o.color, high ? data->edgeColor : dark, high ? 2.5f : 1.5f);
        }
        for (const MPoint& p : data->dots) disc(p, data->dotSize, data->dotColor, dark, 1.5f);
        draw.endDrawable();
    } catch (...) {
    }
}

}  // namespace groomlab::maya
