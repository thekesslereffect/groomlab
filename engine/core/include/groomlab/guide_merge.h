// SPDX-License-Identifier: Apache-2.0
#pragma once

// Pieces of locks (3D polylines, each a lock seen over part of its length: a photo's trace lifted
// or fitted) joined into longer chains end to end, and pieces that are the same lock twice dropped.
// A clean-room reading of the merge HairGS describes (arXiv 2509.07774: greedy endpoint matching
// with relaxing thresholds; no code copied), at lock scale:
//
// 1. Candidates: both free ends of every chain (valence 1), each with its inward tangent (from the
//    end into its chain: the direction over its last `tangentLength`, not HairGS's chord to the far
//    end, which a long curved lock bends away from).
// 2. Search: every other chain's ends within the level's distance d (a brute radius search: a
//    photo set gives a few hundred ends). A pair (a, b) is kept only when chain a, leaving through
//    a, carries on into chain b: dot(-t_a, t_b) >= cos(theta), signed (HairGS's default: no
//    hairpins, two pieces meeting tip to tip the same way are not one lock). Ours, for lock-sized
//    gaps: the gap b - a runs along the joined direction (u = normalize(-t_a + t_b)): its part
//    across u at most `lateralShare` d (two neighbouring locks side by side are not one), and it
//    goes back along u no more than that (pieces overlapping lengthwise are duplicates, not a join).
// 3. Greedy: pairs sorted by distance, nearest first; each end used once a pass and each chain
//    joined at most once a pass (HairGS disables both chains' far ends after a join: no loops, no
//    chain growing at both ends at once).
// 4. Fuse: the two ends replaced by their midpoint.
// 5. Repeat: passes at a level until none joins (at most `passesPerLevel`), then the next level,
//    the thresholds relaxing log-linearly from (distanceStart, angleStart) to (distanceEnd,
//    angleEnd) over `levels` levels. HairGS re-fits its joints to the photos between passes; here
//    the caller scores the chains against the photos afterwards (PhotoScorer).
//
// The thresholds' scale. HairGS's 2 -> 4 mm and 20 -> 40 degrees are at its Gaussians' scale: its
// strands have 80 points, so 2-4 mm is about one segment, a gap between neighbouring Gaussians of
// one strand. Lock pieces are a lock wide (about a tenth of the head's radius R) with points a
// twentieth of R apart, and a piece lifted from one photo carries a depth error of about a tenth
// of R, so the gaps between pieces of one lock are a lock's width or more. The caller's defaults
// (reference_guides.h LockMergeOptions) are 0.1 R -> 0.3 R (one lock width to three) and the
// angles unchanged (angles do not scale): 20 -> 40 degrees.

#include <vector>

#include "groomlab/math.h"

namespace groomlab {

struct PieceMergeOptions {
    float distanceStart = 0.01f;  // scene units: the first level's search radius
    float distanceEnd = 0.03f;    // the last level's
    float angleStart = 20.0f;     // degrees: the first level's largest turn from one piece into the next
    float angleEnd = 40.0f;
    int levels = 4;               // levels from start to end (1: the end's thresholds alone)
    int passesPerLevel = 4;       // passes at a level, at most (stops early when one joins nothing)
    float tangentLength = 0.0f;   // scene units: an end's tangent read over this much of its chain (0: 2 distanceEnd)
    float lateralShare = 0.5f;    // the gap across the joined direction (and back along it), at most this share of d
};

struct PieceMergeResult {
    std::vector<std::vector<Vec3f>> chains;    // the joined chains (pieces left alone too), each in its first piece's direction
    std::vector<std::vector<int>> members;     // each chain's pieces, in order along it
    std::vector<int> mergesPerPass;            // joins each pass made (passes with none included, one a level at least)
    std::vector<float> passDistance, passAngle;  // each pass's thresholds (scene units, degrees)
    int merges = 0;
};

// The pieces (2 points or more each; shorter ones are passed through alone) joined.
PieceMergeResult mergePieces(const std::vector<std::vector<Vec3f>>& pieces, const PieceMergeOptions& options = {});

struct DuplicatePieceOptions {
    float distance = 0.01f;  // scene units: a point this close to the other piece lies on it
    float share = 0.6f;      // a piece with at least this share of its points on a longer one is that one again
    float angle = 30.0f;     // degrees: and running along it there (either way: a line traced from two photos)
};

// For each piece, the longer piece it repeats (its points on that one, running along it), else -1.
// Longest first: a piece repeats only a piece that is kept (repeats nothing itself).
std::vector<int> duplicatePieces(const std::vector<std::vector<Vec3f>>& pieces, const DuplicatePieceOptions& options = {});

}  // namespace groomlab
