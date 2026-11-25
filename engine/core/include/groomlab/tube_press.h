// SPDX-License-Identifier: Apache-2.0
#pragma once

// Tubes pressed against each other instead of passing through one another: SelfCollide's locks
// (operators/self_collide_op.cpp, mode locks: hair in locks, each lock a tube) and a version 3
// Braid's sections (operators/braid_op.cpp).
//
// A tube is rings along a center curve: at each station a center, a frame square to the course and
// how far out each of `around` rays reaches. Where two tubes would overlap, each ray is squashed
// in toward its center (never slid round or along its tube) to where its point lies as deep in its
// own tube as in the other, each depth its distance from that tube's center curve over the reach
// there: the two meet on one surface between them, pressed flat against each other, whatever the
// angle or depth they cross at: a version 2 Braid's press (operators/braid_op.cpp pressSections)
// on any tubes. First, centers that pass closer than half their sections' reach move apart
// (`separate`: one lies over the other, as a braid's sections do, instead of both pinching to a
// sliver where their courses run through each other). Then on each tube's grid (station along it,
// ray round it):
//
//     volume      0-1: free, a ring grows (at most a quarter past its reach) to hold the area a
//                 contact pressed out of it (1: all of it, as a braid's sections do)
//     distance    that growth eased along the tube over about this far (scene units)
//     smoothness  0-1: the outline rounded round the ring and along the tube (up to four passes),
//                 never past a contact, so a contact is a flat facet with a rounded edge
//     clearance   scene units the pressed sides stop short of each other
//     separate    0-1: how far centers too close move apart first (1: to half their reach)
//
//     pressed = pressTubes(tubes, settings)  // per tube, each station's shift and each ray's reach after
//     out = pressLocks(strands, lockOf, settings, amount)  // hair in locks, each lock a tube

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "groomlab/vec3.h"

namespace groomlab {

class StrandSet;

struct PressTube {
    std::vector<Vec3f> centers;             // station by station, root to tip
    // Each station's ring axes, square to the course and to each other: ray j runs along
    // normal cos(2 pi j / around) + binormal sin(2 pi j / around), and its reach is in that ray's
    // length (unit axes: scene units; a braid's: its sections' half axes, the reach a scale).
    std::vector<Vec3f> normals, binormals;
    int around = 0;
    std::vector<float> reach;  // [station * around + j]: how far out ray j reaches (the tube open at both ends)

    std::size_t stations() const { return centers.size(); }
};

struct PressSettings {
    float distance = 1.0f;
    float smoothness = 0.5f;
    float volume = 1.0f;
    float clearance = 0.0f;
    float separate = 1.0f;
};

struct PressResult {
    std::vector<std::vector<Vec3f>> shift;  // [tube][station]: how far its center moved apart
    std::vector<std::vector<float>> reach;  // [tube][station * around + j]: each ray's reach about the moved center
};

// The tubes pressed (a tube that meets no other, or is not a grid of at least 2 stations and 3
// rays: no shift, its reach as it came).
PressResult pressTubes(const std::vector<PressTube>& tubes, const PressSettings& settings);

// Strands in locks pressed against each other. `lockOf`: each strand's lock (a strand channel's
// value, as whole numbers; below 0: in none, left as it is). Each lock is read as a tube: stations
// evenly along its strands (by the share of each one's length), its center the mean of its hair
// there, each ray's reach as far as its hair lies that way (the outline round all of it). Each
// point then moves with its station's shift and in or out toward its lock's center with the ray it
// lies on, by its ray's press
// (its hair stays inside the pressed outline, as far out in it as it was), times `amount(strand,
// t)` (t 0 root to 1 tip). Null `amount`: all of it. The input as it came when nothing is pressed.
std::shared_ptr<const StrandSet> pressLocks(const std::shared_ptr<const StrandSet>& strands, const std::vector<float>& lockOf,
                                            const PressSettings& settings,
                                            const std::function<float(std::size_t strand, float t)>& amount = {});

}  // namespace groomlab
