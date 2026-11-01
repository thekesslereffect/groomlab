// SPDX-License-Identifier: Apache-2.0
#pragma once

// Morphing a mesh to photos with pins (KeenTools-style): each pin is a point on the mesh and where
// that feature sits in one or more photos whose cameras are lined up (camera_solve.h). A pin's
// target is where its mesh point must go to land on its photo points: with one photo, the point on
// the photo point's ray at the pin's own depth from that camera (it slides across the view, its
// depth kept); with two or more, the point nearest all the rays (triangulated, depth too). The
// mesh then follows the pins smoothly: each point moves by a radial basis function of the pins'
// moves (the biharmonic kernel |r| plus a shift, so the same move of every pin moves the whole mesh
// rigidly), `stiffness` easing the fit toward that shift (0: every pin lands exactly; higher keeps
// the mesh's shape, the pins falling short).
//
// Space-free: points, pins and cameras in any one space (the host's, the groom's). The camera
// model is camera_solve.h's (looks down -Z, +Y up, pixels from the top-left, v down).

#include <string>
#include <vector>

#include "groomlab/camera_solve.h"

namespace groomlab {

struct PinView {
    CameraPose pose;
    double focalPx = 0.0;
    double width = 0.0, height = 0.0;  // the photo's pixels
};

struct PinSight {  // where a pin sits in one photo
    int view = 0;  // index into the views
    double u = 0.0, v = 0.0;
};

struct MorphPin {
    CameraVec3 mesh{0, 0, 0};       // the pin's point on the mesh, before the morph
    std::vector<PinSight> sights;   // none: the pin holds its point still
};

struct PinMorphOptions {
    double stiffness = 0.0;  // 0..1: how much the fit gives way (0: pins exact)
    // Symmetry about the plane where coordinate `axis` (0 x, 1 y, 2 z) is 0 (a head's middle):
    // each pin's mirror image joins with the mirrored target, unless a pin lies there already; a
    // pin on the plane (within 1% of the pins' extent) keeps its target on it.
    bool symmetric = false;
    int axis = 0;
};

struct PinMorphResult {
    bool ok = false;
    std::string error;
    std::vector<CameraVec3> targets;  // per pin
    std::vector<CameraVec3> points;   // the mesh's points, morphed
    double rmsBeforePx = 0.0;         // the pins' reprojection error before and after, pixels
    double rmsAfterPx = 0.0;
};

// A pin's target (false when no sight is usable: behind its camera, parallel rays).
bool pinTarget(const MorphPin& pin, const std::vector<PinView>& views, CameraVec3& target);

// Moves `points` so each pin's mesh point goes to its target (pinTarget), the rest following.
PinMorphResult morphToPins(const std::vector<CameraVec3>& points, const std::vector<MorphPin>& pins,
                           const std::vector<PinView>& views, const PinMorphOptions& options = {});

}  // namespace groomlab
