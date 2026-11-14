// SPDX-License-Identifier: Apache-2.0
#include "root_outlines.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <maya/MColor.h>
#include <maya/MDagPath.h>
#include <maya/MDoubleArray.h>
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
MObject RootOutlines::aDotColor;
MObject RootOutlines::aDotSize;
MObject RootOutlines::aFillOpacity;

MStatus RootOutlines::initialize() {
    MFnTypedAttribute typed;
    MFnNumericAttribute numeric;
    MFnVectorArrayData vectors;
    MFnIntArrayData ints;

    aPoints = typed.create("points", "pts", MFnData::kVectorArray, vectors.create());
    typed.setStorable(false);
    aCounts = typed.create("counts", "cnt", MFnData::kIntArray, ints.create());
    typed.setStorable(false);
    aColors = typed.create("colors", "cls", MFnData::kVectorArray, vectors.create());
    typed.setStorable(false);
    aNormals = typed.create("normals", "nrm", MFnData::kVectorArray, vectors.create());
    typed.setStorable(false);
    aStyles = typed.create("styles", "sty", MFnData::kIntArray, ints.create());
    typed.setStorable(false);
    aDotColor = numeric.createColor("dotColor", "dcl");
    numeric.setDefault(1.0f, 0.62f, 0.2f);
    aDotSize = numeric.create("dotSize", "dsz", MFnNumericData::kFloat, 9.0);
    numeric.setMin(1.0);
    aFillOpacity = numeric.create("fillOpacity", "fop", MFnNumericData::kFloat, 1.0);
    numeric.setMin(0.0);
    numeric.setMax(1.0);

    for (MObject* a : {&aPoints, &aCounts, &aColors, &aNormals, &aStyles, &aDotColor, &aDotSize, &aFillOpacity}) {
        const MStatus status = addAttribute(*a);
        CHECK_MSTATUS_AND_RETURN_IT(status);
    }
    return MS::kSuccess;
}

namespace {

struct Outline {
    std::vector<MPoint> points;
    MColor color;
    MVector normal;
    int style = 0;
};

class RootOutlinesData : public MUserData {
public:
    RootOutlinesData() = default;
    std::vector<Outline> outlines;
    MColor dotColor;
    double dotSize = 9.0;
    float fillOpacity = 1.0f;
};

MVectorArray vectorPlug(const MObject& node, const MObject& attr) {
    const MObject value = MPlug(node, attr).asMObject();
    return value.isNull() ? MVectorArray() : MFnVectorArrayData(value).array();
}

MIntArray intPlug(const MObject& node, const MObject& attr) {
    const MObject value = MPlug(node, attr).asMObject();
    return value.isNull() ? MIntArray() : MFnIntArrayData(value).array();
}

}  // namespace

MUserData* RootOutlinesDrawOverride::prepareForDraw(const MDagPath& objPath, const MDagPath&,
                                                    const MHWRender::MFrameContext&, MUserData* oldData) {
    auto* data = dynamic_cast<RootOutlinesData*>(oldData);
    if (!data) data = new RootOutlinesData();
    data->outlines.clear();
    try {
        const MObject node = objPath.node();
        const MVectorArray points = vectorPlug(node, RootOutlines::aPoints);
        const MVectorArray colors = vectorPlug(node, RootOutlines::aColors);
        const MVectorArray normals = vectorPlug(node, RootOutlines::aNormals);
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
            o.normal = k < normals.length() ? normals[k] : MVector();
            o.style = k < styles.length() ? styles[k] : 0;
            data->outlines.push_back(std::move(o));
        }
        const MPlug dot(node, RootOutlines::aDotColor);
        data->dotColor = MColor(dot.child(0).asFloat(), dot.child(1).asFloat(), dot.child(2).asFloat(), 1.0f);
        data->dotSize = std::max(1.0, static_cast<double>(MPlug(node, RootOutlines::aDotSize).asFloat()));
        data->fillOpacity = std::clamp(MPlug(node, RootOutlines::aFillOpacity).asFloat(), 0.0f, 1.0f);
    } catch (...) {
        data->outlines.clear();  // nothing thrown into Maya's draw
    }
    return data;
}

void RootOutlinesDrawOverride::addUIDrawables(const MDagPath&, MHWRender::MUIDrawManager& draw,
                                              const MHWRender::MFrameContext& context, const MUserData* userData) {
    using MHWRender::MUIDrawManager;
    const auto* data = dynamic_cast<const RootOutlinesData*>(userData);
    if (!data || data->outlines.empty()) return;
    try {
        const MMatrix projection = context.getMatrix(MHWRender::MFrameContext::kProjectionMtx);
        const bool ortho = std::abs(projection(3, 3) - 1.0) < 1e-9 && std::abs(projection(2, 3)) < 1e-9;
        const MDoubleArray eyeTuple = context.getTuple(MHWRender::MFrameContext::kViewPosition);
        const MDoubleArray dirTuple = context.getTuple(MHWRender::MFrameContext::kViewDirection);
        const MPoint eye = eyeTuple.length() >= 3 ? MPoint(eyeTuple[0], eyeTuple[1], eyeTuple[2]) : MPoint();
        const MVector viewDir = dirTuple.length() >= 3 ? MVector(dirTuple[0], dirTuple[1], dirTuple[2]) : MVector(0, 0, -1);

        // Each outline on the screen (none with a point behind the eye), and whether it faces away.
        struct Shown {
            const Outline* outline;
            MPointArray screen;
            bool faint;
        };
        std::vector<Shown> shown;
        for (const Outline& o : data->outlines) {
            if (o.points.empty()) continue;
            Shown s{&o, MPointArray(), false};
            bool ok = true;
            MPoint middle;
            for (const MPoint& p : o.points) {
                if (!ortho && (p - eye) * viewDir <= 0.0) {
                    ok = false;
                    break;
                }
                double x = 0.0, y = 0.0;
                context.worldToViewport(p, x, y);
                if (!std::isfinite(x) || !std::isfinite(y)) {
                    ok = false;
                    break;
                }
                s.screen.append(MPoint(x, y, 0.0));
                middle += MVector(p);
            }
            if (!ok) continue;
            middle = MPoint(MVector(middle) / static_cast<double>(o.points.size()));
            if (o.normal.length() > 1e-12) {
                const MVector toCamera = ortho ? -viewDir : (eye - middle);
                s.faint = o.normal * toCamera < 0.0;
            }
            shown.push_back(std::move(s));
        }
        const auto faded = [](MColor c, bool faint) {
            if (faint) c.a *= 0.3f;
            return c;
        };
        const MColor dark(0.08f, 0.08f, 0.08f, 1.0f), white(1.0f, 1.0f, 1.0f, 1.0f);
        const double r = 0.5 * data->dotSize;

        draw.beginDrawable(MUIDrawManager::kNonSelectable);
        draw.beginDrawInXray();
        draw.setDepthPriority(MHWRender::MRenderItem::sActiveWireDepthPriority + 10);
        // The fills: a fan round each outline's middle (on the screen).
        for (const Shown& s : shown) {
            const int style = s.outline->style;
            const unsigned int n = s.screen.length();
            if (!(style & RootOutlines::kFilled) || s.faint || n < 3 || data->fillOpacity <= 0.0f) continue;
            MPoint c;
            for (unsigned int i = 0; i < n; ++i) c += MVector(s.screen[i]);
            c = MPoint(MVector(c) / static_cast<double>(n));
            MPointArray fan;
            for (unsigned int i = 0; i < n; ++i) {
                fan.append(c);
                fan.append(s.screen[i]);
                fan.append(s.screen[(i + 1) % n]);
            }
            MColor color = s.outline->color;
            color.a = data->fillOpacity;
            draw.setColor(color);
            draw.mesh2d(MUIDrawManager::kTriangles, fan);
        }
        // The edges: the root's color lightened, or white and thicker (edited or selected).
        for (const Shown& s : shown) {
            const int style = s.outline->style;
            if (style & RootOutlines::kMarker) continue;
            const unsigned int n = s.screen.length();
            if (n < 2) continue;
            MPointArray strip(s.screen);
            if ((style & RootOutlines::kClosed) && n >= 3) strip.append(s.screen[0]);
            const bool high = (style & RootOutlines::kHighlight) != 0;
            const MColor c = s.outline->color;
            const MColor edge = high ? white : MColor(0.55f * c.r + 0.45f, 0.55f * c.g + 0.45f, 0.55f * c.b + 0.45f, 1.0f);
            draw.setColor(faded(edge, s.faint));
            draw.setLineWidth(high ? 3.0f : 2.0f);
            draw.lineStrip(strip, true);
        }
        // The CVs and the middles, over the edges.
        for (const Shown& s : shown) {
            const int style = s.outline->style;
            const bool high = (style & RootOutlines::kHighlight) != 0;
            if (style & RootOutlines::kMarker) {
                const MPoint& at = s.screen[0];
                const double big = std::max(r * 1.6, 6.0);
                draw.setColor(faded(s.outline->color, s.faint));
                draw.circle2d(at, big, 32, true);
                draw.setColor(faded(high ? white : dark, s.faint));
                draw.setLineWidth(high ? 2.5f : 1.5f);
                draw.circle2d(at, big + (high ? 1.5 : 0.75), 32, false);
                continue;
            }
            if (!(style & RootOutlines::kDots)) continue;
            for (unsigned int i = 0; i < s.screen.length(); ++i) {
                draw.setColor(faded(data->dotColor, s.faint));
                draw.circle2d(s.screen[i], r, 24, true);
                draw.setColor(faded(dark, s.faint));
                draw.setLineWidth(1.5f);
                draw.circle2d(s.screen[i], r + 0.75, 24, false);
            }
        }
        draw.endDrawInXray();
        draw.endDrawable();
    } catch (...) {
    }
}

}  // namespace groomlab::maya
