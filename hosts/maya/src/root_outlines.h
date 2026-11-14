// SPDX-License-Identifier: Apache-2.0
#pragma once

// groomLabRootOutlines: the Tube tool's root outlines (groomlab.ui.root_tube_tool, issue 61), drawn
// in screen space over everything (a constant size whatever the zoom):
//
//   - each outline a polyline of `counts[k]` of `points`, in `colors[k]` (its region's color,
//     groomlab.root_tubes.root_colors), styled by `styles[k]` (Style bits): closed, filled (a fan
//     round its middle, `fillOpacity`), its CVs as dots (`dotColor`, `dotSize` pixels across, a
//     dark ring), its edge white and thicker (the root being edited or selected), or a marker
//     (a large dot at its first point: a root's middle);
//   - the edges drawn over the fills, the dots over the edges;
//   - an outline whose `normals[k]` faces away from the camera (behind the head): faint, unfilled.
//
// Unselectable and unbounded (no bounding box).

#include <maya/MPxDrawOverride.h>
#include <maya/MPxLocatorNode.h>
#include <maya/MString.h>
#include <maya/MTypeId.h>

namespace groomlab::maya {

class RootOutlines : public MPxLocatorNode {
public:
    static void* creator() { return new RootOutlines(); }
    static MStatus initialize();

    bool isBounded() const override { return false; }
    bool excludeAsLocator() const override { return false; }

    // styles[k]'s bits.
    enum Style : int { kClosed = 1, kFilled = 2, kDots = 4, kHighlight = 8, kMarker = 16 };

    static const MTypeId kTypeId;
    static const MString kTypeName;
    static const MString kDrawDbClassification;
    static const MString kRegistrantId;

    static MObject aPoints;       // vectorArray: every outline's points, one outline after another (world space)
    static MObject aCounts;       // Int32Array: each outline's point count
    static MObject aColors;       // vectorArray: each outline's color
    static MObject aNormals;      // vectorArray: each outline's facing (the scalp's normal at its middle; zero: always faces)
    static MObject aStyles;       // Int32Array: each outline's Style bits
    static MObject aDotColor;     // float3: the CV dots' color
    static MObject aDotSize;      // float: the CV dots' diameter in pixels
    static MObject aFillOpacity;  // float: the fills' opacity
};

class RootOutlinesDrawOverride : public MHWRender::MPxDrawOverride {
public:
    static MHWRender::MPxDrawOverride* creator(const MObject& obj) { return new RootOutlinesDrawOverride(obj); }

    MHWRender::DrawAPI supportedDrawAPIs() const override { return MHWRender::kAllDevices; }
    bool hasUIDrawables() const override { return true; }
    bool isBounded(const MDagPath&, const MDagPath&) const override { return false; }
    bool disableInternalBoundingBoxDraw() const override { return true; }
    MUserData* prepareForDraw(const MDagPath& objPath, const MDagPath& cameraPath,
                              const MHWRender::MFrameContext& frameContext, MUserData* oldData) override;
    void addUIDrawables(const MDagPath& objPath, MHWRender::MUIDrawManager& draw,
                        const MHWRender::MFrameContext& frameContext, const MUserData* data) override;

private:
    explicit RootOutlinesDrawOverride(const MObject& obj) : MHWRender::MPxDrawOverride(obj, nullptr, true) {}
};

}  // namespace groomlab::maya
