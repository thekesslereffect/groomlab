// SPDX-License-Identifier: Apache-2.0
#pragma once

// Lining a photo's camera up better than the agent's pairs alone (align_photo's refine): the
// camera from the pairs, then tightened so the mesh's outline through it lies on the photo's
// outline, where the two are the same thing. A photo's outline includes the hair while the mesh is
// a bald head, so only the parts that are not hair count: the mesh's outline where it is face,
// ears or neck (`usable`: the head below its hairline) and the photo's where no hair is (its hair
// mask, when one is given). The pairs stay in the fit, so it cannot slide off them.
//
// The photo's outline: the subject against a plain backdrop (the corners' color), its edge pixels'
// distance map (how far each pixel is from the nearest). The mesh's: its silhouette edges (one face
// toward the camera, one away) on the outer outline (outside them the mesh does not cover the image),
// sampled. Levenberg-Marquardt over the pose (the focal length kept) minimizes the pairs'
// reprojection error plus the outline samples' distances (capped: an outline part with no partner
// in the photo does not pull).

#include <functional>
#include <string>
#include <vector>

#include "groomlab/camera_solve.h"
#include "groomlab/reference_guides.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab::agent {

struct OutlineFit {
    bool ok = false;
    std::string why;
    RefCamera camera;                            // the refined camera (the start when !ok)
    double pairsBefore = 0.0, pairsAfter = 0.0;  // the pairs' RMS, pixels
    double outlineBefore = 0.0, outlineAfter = 0.0;  // the outline samples' mean distance to the photo's outline, pixels
    int samples = 0;     // outline samples used at the end
    int edgePixels = 0;  // the photo's outline pixels that count (not hair)
};

// The photo's subject against its backdrop (one channel, 1 subject): the pixels that differ from
// the corners' color.
RefImage subjectMask(const RefImage& photo);

// `hairMask`: one channel 0-1 at the photo's size (empty: none known); `usable`: whether a point of
// the mesh may be matched (null: all).
OutlineFit refineWithOutline(const ScalpMesh& mesh, const RefImage& photo, const RefImage& hairMask, const RefCamera& start,
                             const std::vector<CameraCorrespondence>& pairs, const std::function<bool(const Vec3f&)>& usable,
                             int rounds = 8);

}  // namespace groomlab::agent
