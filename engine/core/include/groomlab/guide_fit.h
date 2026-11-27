// SPDX-License-Identifier: Apache-2.0
#pragma once

// Guides fitted to strokes drawn on several lined-up photos: each guide a 3D polyline whose points
// are free, pulled so that, projected through each photo's camera, it lies on that photo's stroke
// for it, with the rest of its shape (above all its depth, which one photo cannot see) left to a
// rod's stiffness, gravity, the head and the hair's shell. A clean-room reading of the published
// math of EnergyHair (Graphics Interface 2022: screen-space B-spline residuals plus a discrete rod,
// Newton per guide); no paper code. Ours: the start from the shell ray-cast lift (lock_guides.h
// liftStroke), the prior toward the hair's outside each photo sees, the robust screen cost, the
// rooting again, the matching of strokes across photos (the paper makes the user pick the guide)
// and its checks, and every weight (the paper gives none).
//
// The energy of one guide x_0..x_n (x_0 the root, fixed on the scalp; h its point spacing; R the
// head's radius; every term a sum of squared residuals, each made unitless by a length that costs 1,
// all in head radii so the fit is the same at any scale):
//
// - screen: for each photo with a stroke for the guide and each point that photo sees (in its frame,
//   not behind the head), the distance in pixels from the point's projection to the stroke's curve
//   (a quadratic open-uniform B-spline through the stroke resampled every `splineSpacing` of the
//   photo's larger side: a polyline's creases trap points), as a length at the head's depth in that
//   photo (pixels x depth / focal), d, over screenSigma R. A point whose nearest place on the curve is
//   one of its ends costs nothing: the stroke does not cover it (the paper's rule), so a stroke drawn
//   over part of a lock, in one photo, and one over another part, in another, do not fight. Ours:
//   the cost is robust, tau^2 ln(1 + (d / tau)^2) with tau = screenRobust R (in screenSigmas), the
//   square near the stroke, a pull that fades far off it (a point of a curly lock whose nearest place
//   on another photo's partial stroke is mid-stroke but far: the paper's plain square let it drag the
//   guide over).
// - seen (ours, in place of a plain pull to the shell): a point a stroke covers lies no deeper along
//   that photo's ray than where the ray first meets the shell: deeper costs (depth / shellSigma R)^2,
//   faded in over the first `shellFade` R from the root (the root is on the scalp, under the hair). A
//   stroke traces hair its photo sees, the hair's outside from there, so depth one photo leaves free
//   stays at that outside (the lift's depth, without its zig-zags: the bend smooths them) instead of
//   collapsing to the stiffest line; two photos outweigh it 100 to 1.
// - stretch: (|x_i+1 - x_i| / h - 1)^2 x `stretch`: the guide keeps its length (the paper's rod).
// - bend: |x_i+1 - 2 x_i + x_i-1|^2 / h^2 x `bend` (the turning angle squared at each point; a
//   straight rest shape): depth zig-zags a photo cannot see cost much, slow depth changes little.
// - gravity: `gravity` x (x_i - x_0) . up / R: a mild sag where nothing else holds the guide.
// - head: a point closer to the head than `clearance` R (by the head mesh's closest point and normal
//   within a head radius, else the shell's head along the direction from its middle) costs
//   (its height - clearance)^2 / (headSigma R)^2.
// - shell: out past the shell's outer surface (along the direction from the head's middle) costs
//   (out / shellOutSigma R)^2.
//
// Solve: Levenberg-Marquardt (Gauss-Newton on the residuals, the gravity's gradient added) on the
// free points; the matrix is block-pentadiagonal (3 x 3 blocks, a point coupled to two each side by
// the bend), solved by a banded LDL^T. A step is taken only when it lowers the energy; no point moves
// more than maxStep R a step. Guides fit side by side.
//
// Rooting again (when `scalp` is given), after the first fit: the guide is cut back to its first
// point on a stroke (within 3 screenSigma of it), carried back along its direction by the length of
// stroke before that point (the most of any photo; not into the head), and, when that start is more
// than carryShare R off the head (a lock drawn from where it comes out from under other hair),
// carried on back along the lock, turning toward the head's middle over 0.4 R, until it meets the
// head (at most carryLength R; else it is rooted straight under); then rooted on the scalp
// (guide_rooting.h, as the lift roots: under its start toward the head's middle) and fitted on
// (twice more at most, carried only the first time). With two photos the start of the lock is known
// in 3D, where one photo's lift guessed its depth; and a long straight spike from the scalp out to a
// stroke drawn on the hair's outside becomes a lock coming from the scalp.
//
// Then the length (lengthRounds rounds): a guide whose strokes run on past its tip in some photo (by
// more than two points) grows along its last direction by what is left (at most half its length a
// round); one whose last points no stroke covers ends at the last point one does; then it is fitted
// again.
//
// Matching strokes across photos (associateStrokes; ours, the paper makes the user pick the guide):
// each stroke fitted alone first. Two strokes on different photos are a candidate when one's guide,
// projected into the other's photo (the points it sees), lies near the other's curve: it covers
// minCovered of the points, at a mean distance within nearDistance (head radii at the head's depth:
// loose, one photo's depth may be off) and runs the same way (minAgreement, the mean cosine, root to
// tip). Each candidate pair is then fitted as one guide (from its better rooted member's lone fit);
// it is a match when that guide fits both strokes (each photo's mean distance within
// maxFusedResidual, no covered point farther off than maxFusedOff, at least minStrokeCovered of each
// stroke covered), stays in the shell (out past it no more than maxOutside) and each photo would
// see it (no deeper than maxHidden behind the hair's outside from there: two photos' strokes of
// different locks can often be triangulated into some curve, but one inside the hair neither photo
// could have seen). Matches are joined best fitting first, a group holding at most one stroke a
// photo; each group of several is fitted to all of its strokes and, should that fit fail the same
// test, split back into its strokes, each fitted alone.

#include <array>
#include <vector>

#include "groomlab/guide_rooting.h"
#include "groomlab/hair_shell.h"
#include "groomlab/reference_guides.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {

// A stroke on a lined-up photo.
struct FitStroke {
    RefCamera camera;                          // the photo's, at its size
    std::vector<std::array<float, 2>> pixels;  // the photo's pixels, root first (2 or more)
    int photo = 0;                             // the caller's labels, for reports
    int stroke = 0;
    float rootGap = 0.0f;                      // how far its lift's root was off the scalp (the best rooted member roots a group)
};

struct GuideFitOptions {
    float headRadius = 0.0f;         // the scale R (scene units); 0: the shell's frame's
    float splineSpacing = 0.01f;     // the stroke's B-spline control points this share of the photo's larger side apart
    float screenSigma = 0.01f;       // a point this many R (at the head's depth) off a stroke costs 1
    float screenRobust = 0.1f;       // R: farther off a stroke a point pulls less and less (Cauchy; 0: plain squares)
    float stretch = 100.0f;
    float bend = 1.0f;
    float gravity = 2.0f;
    float clearance = 0.005f;        // R kept off the head
    float headSigma = 0.005f;        // R inside the head that cost 1
    float shellOutSigma = 0.02f;     // R outside the shell that cost 1
    float shellSigma = 0.1f;         // R deeper than where a stroke's photo first sees the shell that cost 1 (seen)
    float shellFade = 0.3f;          // R from the root over which that fades in
    float maxStep = 0.2f;            // R a point may move in one step
    int iterations = 60;             // a round's most steps
    int lengthRounds = 3;            // rounds of fit, grow or trim (1: no length change)
    // Where hair grows (the hair region's scalp): after the first fit, the guide from its first point
    // a stroke covers is rooted again (rootGuide with `rooting`, never flipped) and fitted on. Null:
    // the root stays where the guide started.
    const ScalpMesh* scalp = nullptr;
    GuideRootingOptions rooting;
    float carryShare = 0.25f;        // R: a start farther off the head is carried back along the lock onto it (0: never)
    float carryLength = 0.8f;        // R: carried at most this far, turning toward the head over 0.4 R (else rooted straight under)
};

// How a fitted guide lies on one stroke.
struct FitViewResidual {
    int photo = 0, stroke = 0;
    float meanPx = 0.0f, maxPx = 0.0f;  // the covered points' distance to the stroke's curve, pixels
    float meanR = 0.0f, maxR = 0.0f;    // the mean and largest as head radii at the head's depth
    float covered = 0.0f;               // the share of the points the photo sees that the stroke covers
    float strokeCovered = 0.0f;         // the share of the stroke's length the guide covers
    float hidden = 0.0f;                // R: how far the points on the stroke lie behind the hair's outside from this photo
                                        // (beyond where their ray first meets the shell), mean
    int points = 0;                     // points the photo sees
};

struct FittedGuide {
    std::vector<Vec3f> points;            // root first, on the scalp
    std::vector<FitViewResidual> views;   // one per stroke, in the order given
    float energyBefore = 0.0f, energyAfter = 0.0f;
    float lengthBefore = 0.0f, lengthAfter = 0.0f;
    int iterations = 0;                   // steps taken, every round
};

// One guide fitted to its strokes (any number: none leaves rod, shell and head). `initial` root first
// (2 points or more), its root on the scalp; `head` the head mesh it stays out of (null: none).
FittedGuide fitGuide(const std::vector<Vec3f>& initial, const std::vector<FitStroke>& strokes, const HairShell& shell, const ScalpMesh* head,
                     const GuideFitOptions& options = {});

// Many at once, side by side: guide i to strokes[i].
std::vector<FittedGuide> fitGuides(const std::vector<std::vector<Vec3f>>& initial, const std::vector<std::vector<FitStroke>>& strokes,
                                   const HairShell& shell, const ScalpMesh* head, const GuideFitOptions& options = {});

// How a guide (root first) projects onto a stroke: the points the stroke's photo sees (not behind
// `head`, may be null), the share whose nearest place on the stroke's curve is not an end (covered),
// their mean distance (pixels, and head radii at the head's depth: `center` the head's middle, `headRadius` R), and the mean
// cosine between the guide's projected direction and the stroke's there (both root to tip).
struct StrokeMatch {
    int points = 0, coveredPoints = 0;
    float covered = 0.0f;
    float meanPx = 0.0f, meanR = 0.0f;
    float agreement = 0.0f;
    float strokeCovered = 0.0f;  // the share of the stroke's length the covered points span
};
StrokeMatch matchStroke(const std::vector<Vec3f>& guide, const FitStroke& stroke, const ScalpMesh* head, const Vec3f& center, float headRadius,
                        float splineSpacing = 0.01f);

struct StrokeAssociationOptions {
    float nearDistance = 0.3f;       // R: a candidate pair's closer way's mean distance at most (one photo's depth may be off)
    float minCovered = 0.3f;         // its covered share at least
    float minAgreement = 0.2f;       // its direction agreement at least (the mean cosine: the same way, loosely)
    float maxFusedResidual = 0.04f;  // R: one guide fitted to both (all) strokes is this close to each (mean), or no match
    float maxFusedOff = 0.15f;       // R: and no point it covers farther off
    float minStrokeCovered = 0.3f;   // and covers this share of each stroke's length
    float maxOutside = 0.05f;        // R: and is no farther out past the shell
    float maxHidden = 0.2f;          // R: and lies no deeper behind the hair's outside from any of its photos (hidden, mean): a
                                     // stroke traces hair its photo sees, so a "lock" each photo would see only through other hair is
                                     // two locks
};

// Groups of strokes (indices) that are one lock: each stroke in exactly one group, a group at most
// one stroke a photo; `guides[i]` stroke i's guide fitted alone (empty: it stays alone). `forced[i]`
// (when given, same size) >= 0 puts strokes with the same value in one group whatever they match,
// and keeps them out of the matching. `fit`: how the candidate pairs are fitted (fitGuide).
std::vector<std::vector<int>> associateStrokes(const std::vector<FitStroke>& strokes, const std::vector<std::vector<Vec3f>>& guides,
                                               const HairShell& shell, const ScalpMesh* head, const GuideFitOptions& fit = {},
                                               const StrokeAssociationOptions& options = {}, const std::vector<int>* forced = nullptr);

// The whole pass: `lifts[i]` stroke i lifted onto the shell (liftStroke: root first, on the scalp;
// empty when it was dropped). Each stroke fitted alone; with `fuse`, matched across photos and each
// group fitted to all of its strokes (split back when it fits one badly). One guide a group, the
// groups in the order of their first stroke.
struct StrokeFitGroup {
    std::vector<int> strokes;  // the strokes it fits, the root's (primary) first
    FittedGuide guide;
    bool split = false;        // matched with others, then fitted alone (the joint fit left one far off)
};
struct StrokeFitResult {
    std::vector<StrokeFitGroup> groups;
    int fused = 0;  // groups of 2 or more strokes
    int split = 0;  // strokes of matched groups fitted alone after all
};
StrokeFitResult fitStrokes(const std::vector<FitStroke>& strokes, const std::vector<std::vector<Vec3f>>& lifts, const HairShell& shell,
                           const ScalpMesh* head, bool fuse, const GuideFitOptions& options = {},
                           const StrokeAssociationOptions& association = {}, const std::vector<int>* forced = nullptr);

}  // namespace groomlab
