// SPDX-License-Identifier: Apache-2.0
#pragma once

// Photos of one shoot solved together (align_photos, and align_photo's priors): every photo's
// camera from its landmark pairs, assuming what a photo session around one subject is like.
//   - One focal length for all (solved once from every photo's pairs, or given), with a gentle
//     pull toward a portrait lens (`preferredFocalMm`, 36 mm film back): a near-orthographic
//     portrait's pairs cannot tell 60 mm from 135 mm, and a free solve then drifts short.
//   - Each camera round the head near its view's angle (yaw about the head's up axis: front 0,
//     the subject's left positive; three-quarters +-45, sides +-90, three-quarter backs +-135,
//     back 180), level (pitch and roll near 0): soft priors, so a real photo may differ a little.
//   - The cameras at about the same distance from the head's middle (soft).
// Each prior is a residual beside the pairs' pixel errors (`priorWeight` scales them all: 0 off),
// the pixel errors in units of how good the picks are (pickSigma), so pairs that clearly say
// otherwise win: exact picks keep their camera, rough ones (another person's head) lean on the
// priors. A free focal that still ends short of 70 mm is tried at the preferred one: kept there
// unless the pairs fit clearly worse than the picks are good. Levenberg-Marquardt over each camera's yaw, pitch, roll,
// distance and the head middle's place in its picture, and the shared focal length, from a few
// focal lengths; the best kept.

#include <optional>
#include <string>
#include <vector>

#include "groomlab/camera_solve.h"
#include "groomlab/reference_guides.h"

namespace groomlab::agent {

struct ShootView {
    std::vector<CameraCorrespondence> pairs;
    double width = 0.0, height = 0.0;
    std::optional<double> yawDegrees;  // the view's angle round the head (none: free)
    bool level = true;                 // pitch and roll pulled toward 0
    // A fixed camera (a photo lined up already that only lends the shoot its focal and distance).
    std::optional<RefCamera> fixed;
};

struct ShootOptions {
    Vec3f center, up{0, 1, 0}, front{0, 0, 1};  // the head's middle and directions
    std::optional<double> focalMm;   // given: kept
    double preferredFocalMm = 100.0;  // where a free focal leans when the pairs cannot tell
    double priorWeight = 1.0;        // scales every prior (0: pairs alone)
    double yawSigmaDegrees = 10.0, pitchSigmaDegrees = 6.0, rollSigmaDegrees = 4.0;
    double focalSigma = 1.4;         // the free focal's spread about the preferred one (a factor)
    double distanceSigma = 0.15;     // the distances' spread, in log units
    std::optional<double> distance;  // the shoot's distance from the head's middle (a prior), when known
    // How good the picks are (pixels): what a pair's error is weighed in against the priors. None:
    // what the pairs alone fit to (their RMS, for the parameters they fix), or 1.5% of the photo's
    // width when they are too few to say (another person's head is no better).
    std::optional<double> pickSigma;
    bool lensFallback = true;  // a short free focal tried at the preferred one (below)
};

struct ShootCamera {
    RefCamera camera;
    double rms = 0.0;
    std::vector<double> errors;
    double yaw = 0.0, pitch = 0.0, roll = 0.0;  // degrees, in the head's frame
    double fromPrior = 0.0;                     // degrees between its look and its view's prior look
    double distance = 0.0;
    bool fights = false;  // its pairs disagree with the rest of the shoot (rms far over the others')
};

struct ShootSolve {
    bool ok = false;
    std::string why;
    std::string focalNote;  // when a short focal the pairs leaned to was given up for a portrait lens
    double focalMm = 0.0;
    std::vector<ShootCamera> cameras;  // one a view, in order
    double rms = 0.0;                  // over every pair
};

ShootSolve solveShoot(const std::vector<ShootView>& views, const ShootOptions& options);

// The yaw of a view's name (front 0, three_quarter_front_left 45, left 90, ...; three_quarter: 35;
// top: none). False for a name it does not know.
bool viewYaw(const std::string& view, double& yawDegrees, bool* level = nullptr);

}  // namespace groomlab::agent
