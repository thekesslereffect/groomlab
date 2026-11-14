// SPDX-License-Identifier: Apache-2.0
#pragma once

// groomLabRootOutlines: the Tube tool's root outlines (groomlab.ui.root_tube_tool, issue 61), drawn
// in the scene, on the scalp, so the hair and guides in front of them hide them:
//
//   - each outline a polyline of `counts[k]` of `points` (its edges laid along the scalp), in
//     `colors[k]` (its region's color, groomlab.root_tubes.root_colors), styled by `styles[k]`
//     (Style bits): closed, its edges green and thicker (the root being edited or selected:
//     `highlight`), or a marker (a large dot at its first point: a root's middle);
//   - the fills: triangles of `fillPoints` (three points each, on the scalp), each in its outline's
//     color (`fillOwners`) at `fillOpacity`;
//   - the CVs: dots at `dots` (`dotOwners`: their outline), `dotColor`, `dotSize` pixels across.
//
// Each layer lifted toward the camera a little more than the one under it (the scalp, the fills,
// the edges, the dots), so the dots draw over the edges and the edges over the fills; each outline's
// edges a hair more than the outline before it's (the edited or selected ones most), so two
// neighbours' shared edge never flickers between them. Unselectable and unbounded.

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

    static MObject aPoints;       // vectorArray: every outline's edge points, one outline after another (world)
    static MObject aCounts;       // Int32Array: each outline's point count
    static MObject aColors;       // vectorArray: each outline's color
    static MObject aNormals;      // vectorArray: each outline's facing (unused: the scene's depth hides the far side)
    static MObject aStyles;       // Int32Array: each outline's Style bits
    static MObject aFillPoints;   // vectorArray: the fills' triangles, three points each (world)
    static MObject aFillOwners;   // Int32Array: each triangle's outline
    static MObject aDots;         // vectorArray: the CVs (world)
    static MObject aDotOwners;    // Int32Array: each CV's outline
    static MObject aDotColor;     // float3: the CV dots' color
    static MObject aEdgeColor;    // float3: a highlighted outline's edges
    static MObject aDotSize;      // float: the CV dots' diameter in pixels
    static MObject aEdgeWidth;    // float: the edges' width in pixels (highlighted: 1.5 times)
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
