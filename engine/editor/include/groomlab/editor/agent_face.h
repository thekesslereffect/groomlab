// SPDX-License-Identifier: Apache-2.0
#pragma once

// A head's facial landmarks found from its mesh's shape (agent tools: mesh_landmarks names them,
// align_photo pairs photo pixels with them by name, new_groom's scalp from the landmarks): where a
// whole head is the scalp (face and neck included), the features an agent can also find on a
// photo. Heuristic, not learned: the face is read off depth maps of the mesh (seen from the front
// and from each side) with a head's usual proportions as the starting guesses:
//   nose_tip      the most forward point of the face's middle line
//   chin          the lowest point of the face's front before the neck falls back
//   left_eye, right_eye                 the eye sockets' deepest point near the usual eye height
//   left_eye_inner/outer, right_eye_inner/outer   the eye corners, half an eye's width either side
//   left_mouth_corner, right_mouth_corner         at the mouth's usual height and width
//   left_ear_top, left_earlobe, right_ear_top, right_earlobe   the top and bottom of what sticks
//                 out from the side of the head at the ears' height (the side depth map's bump)
//   hairline_middle   the forehead's hairline at the middle (about as far above the eyes as the nose
//                 tip is above the chin)
//   crown, nape   the scalp landmarks' (agent_grooming.h)
// Each with a confidence 0-1: how clearly the shape showed it (a nose that stands out, a socket, an
// ear's bump); proportions alone (mouth corners, eye corners, hairline) stay low. On a mesh with no
// face (a cap, a ball) none are found and `why` says so. "left" is the character's.

#include <string>
#include <vector>

#include "groomlab/editor/agent_grooming.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab::agent {

struct FaceLandmark {
    std::string name;
    Vec3f point, normal;
    float confidence = 0.0f;
};

struct FaceLandmarks {
    bool head = false;  // a face was found: the mesh is a head with its face
    std::string why;    // when not
    std::vector<FaceLandmark> points;
    // The face's measures the guesses use: the nose's height above the chin (the face's lower
    // third), the head's width across (without the ears).
    float third = 0.0f, width = 0.0f;
    const FaceLandmark* find(const std::string& name) const;  // null for none
};

FaceLandmarks faceLandmarks(const ScalpMesh& mesh, const std::string& upAxis);
const std::vector<std::string>& faceLandmarkNames();

// How much a point of the head is scalp (1: hair grows there) rather than face, ears or neck (0):
// above a hairline through the forehead's middle, the temples, over the ears and down behind them
// to the nape, `softness` (a share of the face's third) the blend's width. On a head with no face:
// 1 everywhere.
float hairRegionWeight(const FaceLandmarks& face, const ScalpLandmarks& marks, const Vec3f& point, float softness = 0.15f);
// The hairline's height (along up, from marks.middle) `angleDegrees` round the head from the front
// (positive: toward its left), and the head's surface there: on a whole head, where the scalp's
// front_hairline and temples are (scalpLandmarks puts them on a ball, low on a face).
float hairlineHeight(const FaceLandmarks& face, const ScalpLandmarks& marks, float angleDegrees);
Vec3f hairlinePoint(const ScalpMesh& mesh, const FaceLandmarks& face, const ScalpLandmarks& marks, float angleDegrees, Vec3f* normal = nullptr);

}  // namespace groomlab::agent
