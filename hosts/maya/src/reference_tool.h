// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <string>
#include <utility>
#include <vector>

#include <maya/MApiNamespace.h>  // M3dView (a versioned namespace's class: no plain forward declaration)
#include <maya/MDagPath.h>
#include <maya/MPxCommand.h>
#include <maya/MPxContext.h>
#include <maya/MPxContextCommand.h>
#include <maya/MSyntax.h>

namespace groomlab::maya {

// Grooming from photos (Groom Tools > Reference): a photo is a camera with an image plane
// (groomlab.reference); lining the camera up with the mesh is a solve from point pairs.

// groomLabCameraSolve: the camera that sees the world points where the image points are
// (groomlab/camera_solve.h: DLT + Levenberg-Marquardt; principal point at the image's center,
// square pixels, no distortion).
//
//   -imagePoint/-ip u v     (multi-use) pixels, origin at the image's top-left, v down
//   -worldPoint/-wp x y z   (multi-use) the same points in world space, in the same order
//   -imageSize/-is w h      the image's size in pixels (required)
//   -filmBack/-fb h v       the camera's film aperture in inches (default 1.417 x h*1.417/w)
//   -focal/-fl mm           the focal length to start from, or keep with -fixFocal
//   -fixFocal/-ff           solve the pose only (4 pairs are enough; 6 otherwise)
//   -camera/-cam name       start from this camera's pose (and its focal and film back
//                           when -focal and -filmBack are not given)
//
// Returns [tx ty tz rx ry rz focalMm rmsPx e1 ... eN]: the camera transform in world space
// (Maya's camera: looking down -Z, Y up; rotation order xyz, degrees), the focal length, the
// RMS reprojection error and each pair's error in pixels. Changes nothing in the scene.
class CameraSolveCmd : public MPxCommand {
public:
    static const char* kName;
    static void* creator();
    static MSyntax newSyntax();
    MStatus doIt(const MArgList& args) override;
};

// The Reference Flow tool (guides from reference photos' hints): looking through a reference
// camera, a drag over the photo draws a flow stroke, root end first; Ctrl+drag (or Ctrl+click on
// one) erases the strokes it touches. Strokes are kept in the photo's pixels (they stay on the
// photo whatever the camera does later) in the groom's glReferenceViews attribute, through
// groomlab.from_reference (one undo step each), and draw as curved arrows on the image plane, as
// the Flow tool draws its strokes on the scalp. guides_from_reference reads them as hint strokes.
class ReferenceFlowContext : public MPxContext {
public:
    ReferenceFlowContext();
    void getClassName(MString& name) const override;  // "groomLabReferenceFlow"
    void toolOnSetup(MEvent& event) override;

    MStatus doPress(MEvent& event, MHWRender::MUIDrawManager& draw, const MHWRender::MFrameContext& ctx) override;
    MStatus doDrag(MEvent& event, MHWRender::MUIDrawManager& draw, const MHWRender::MFrameContext& ctx) override;
    MStatus doRelease(MEvent& event, MHWRender::MUIDrawManager& draw, const MHWRender::MFrameContext& ctx) override;
    MStatus drawFeedback(MHWRender::MUIDrawManager& draw, const MHWRender::MFrameContext& ctx) override;

    // A stroke as the mouse would draw it, through view coordinates (x right, y up from the view's
    // bottom-left) in `view`: press at the first point, drag through the rest, release. `erase`:
    // as with Ctrl held. Returns the strokes now kept for the camera (-1: not looking through it).
    int stroke(M3dView& view, const std::vector<std::pair<short, short>>& points, bool erase);
    int strokeCount() const;

    std::string camera;  // the reference camera drawn over (its transform)
    std::string groom;   // the groom keeping the strokes

private:
    bool begin(M3dView& view, short x, short y, bool erase);
    void extend(M3dView& view, short x, short y);
    void end();

    bool active_ = false, erase_ = false;
    std::vector<std::array<double, 2>> pixels_;  // the stroke being drawn, photo pixels
};

// groomLabReferenceFlowCtx: creates and edits Reference Flow contexts.
//   groomLabReferenceFlowCtx -e -camera <reference camera> -groom <groom> ctxName
//   groomLabReferenceFlowCtx -e -debugStroke "x0 y0 x1 y1 ..." [-debugErase] ctxName   (tests: view coordinates)
//   groomLabReferenceFlowCtx -q -strokeCount ctxName
class ReferenceFlowContextCmd : public MPxContextCommand {
public:
    static const char* kName;
    static void* creator();
    MPxContext* makeObj() override;
    MStatus appendSyntax() override;
    MStatus doEditFlags() override;
    MStatus doQueryFlags() override;

private:
    ReferenceFlowContext* context_ = nullptr;
};

}  // namespace groomlab::maya
