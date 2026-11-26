// SPDX-License-Identifier: Apache-2.0
#pragma once

// What the agent tools (agent_tools.h) groom with, worked out once and tested on their own: named
// places on a scalp (an agent says "comb from the crown to the nape" without making coordinates),
// stroke paths along the surface, which guides a sphere takes, what a stroke changed, and numbers
// that describe a set of guides (lengths, direction, curl) so an agent can check a result.

#include <cstddef>
#include <string>
#include <vector>

#include "groomlab/guide_hierarchy.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab::agent {

// The directions of a groom's world: up (Y, or Z for a Z-up groom), the way the face looks (+Z for
// Y-up, -Y for Z-up: GroomLab's sample heads, Maya's and Blender's conventions) and the
// character's left (up x front).
struct ScalpFrame {
    Vec3f up, front, left;
    static ScalpFrame of(const std::string& upAxis);
};

// A named place on the scalp and the surface's outward normal there.
struct Landmark {
    std::string name;
    Vec3f point, normal;
};

// The scalp's landmarks, approximate: the head taken as a ball under the scalp's top as wide as
// the scalp across (`middle` its middle: a head with its neck, or a cap, has its ball's middle
// where a skull's would be), each landmark the scalp point nearest the ball's surface in the
// landmark's direction, so on a head they fall where the names say; on other shapes they are only
// the surface that way.
//   top             straight up
//   crown           up and back (the whorl's usual place)
//   front_hairline  up and forward
//   nape            back and a little down
//   left_temple, right_temple   to the side, forward and up
//   left_ear, right_ear         the side of the head, about the ears' height
//   back            straight back
struct ScalpLandmarks {
    ScalpFrame frame;
    Vec3f lo, hi;          // the scalp's bounds
    Vec3f middle;          // the head's middle the landmarks (and scalpPath's arcs) are taken about
    float size = 0.0f;     // the bounds' largest side
    std::vector<Landmark> points;
    const Landmark* find(const std::string& name) const;  // null for none
};
ScalpLandmarks scalpLandmarks(const ScalpMesh& scalp, const std::string& upAxis);
// The landmarks' names, in the order scalpLandmarks gives them.
const std::vector<std::string>& landmarkNames();

// A stroke's path along the scalp through `stops` (at least two): between each two the direction
// from `middle` turns evenly (a great circle about it) and the distance from it blends, each sample
// then put on the scalp at its nearest point and lifted `offset` along the normal there. Samples at
// most `spacing` apart (at least one per stretch), the stops among them.
std::vector<Vec3f> scalpPath(const ScalpMesh& scalp, const Vec3f& middle, const std::vector<Vec3f>& stops, float spacing,
                             float offset);

// The guides of `h` (of `level`, -1 any, and group `group`, "" any) with a root (rootsOnly) or any
// point within `radius` of `center`.
std::vector<GuideId> guidesInSphere(const GuideHierarchy& h, const Vec3f& center, float radius, bool rootsOnly, int level = -1,
                                    const std::string& group = {});

// What changed between two states of the guides: guides whose points moved (or changed count),
// guides added and removed, and how far the moved guides' points went (point by point, the shorter
// guide's count; the mean over every point compared, the largest).
struct GuideChange {
    std::size_t touched = 0, added = 0, removed = 0;
    float meanMove = 0.0f, maxMove = 0.0f;
    float lengthBefore = 0.0f, lengthAfter = 0.0f;  // the touched guides' mean length
};
GuideChange guideChange(const GuideHierarchy& before, const GuideHierarchy& after);

// Numbers that describe guides `ids` of `h` (those with points).
struct GuideStats {
    std::size_t count = 0;
    float lengthMin = 0.0f, lengthMean = 0.0f, lengthMax = 0.0f;
    Vec3f direction;            // the mean root-to-tip direction (unit; zero when they cancel out)
    float straightness = 0.0f;  // mean of root-to-tip distance over length: 1 straight, lower bent or curled
    float curlDegrees = 0.0f;   // mean of the angles turned along each guide, root to tip (one turn: 360)
    float rootAngleDegrees = 0.0f;  // mean angle of the first segment from the scalp's normal: 0 standing out, 90 lying flat
    Vec3f rootMiddle, tipMiddle;    // the roots' and the tips' mean point
};
// `scalp` (may be null) gives the normals at the roots.
GuideStats measureGuides(const GuideHierarchy& h, const std::vector<GuideId>& ids, const ScalpMesh* scalp);
// A direction in words in the frame ("down, back", "out to the left"...): its parts past a third.
std::string directionWords(const Vec3f& direction, const ScalpFrame& frame);

}  // namespace groomlab::agent
