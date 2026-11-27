// SPDX-License-Identifier: Apache-2.0
#pragma once

// A smooth 3D field of hair directions through the hair's volume, filled in between a few guides,
// and guides grown root first through it: the fill between key guides that the lock tools
// (guides_from_strokes, comb_guides, fill_guides) use in place of copying the nearest guides'
// shapes. A clean-room reading of the published math (Fu et al. 2007 "Sketching Hairstyles" and
// Yu et al. I3D 2014 for the soft-constraint Laplacian field, Dr.Hair CVPR 2024 for the scalp's
// pore direction, DeepSketchHair 2020 for the growth's turn rules); no paper code.
//
// The field: a vector per cell of a regular grid round the head, the cells limited to the hair's
// shell (hair_shell.h: between the head and the hull the photos carve, less what a photo shows
// bare). It is the smoothest field (the sum over neighbouring cells of |t_i - t_j|^2, a harmonic
// field) that keeps, softly (weight lambda^2, lambda = 100), to:
//
// - each guide's tangent, root to tip (so its sign is never in doubt), splatted into the cells
//   round points half a cell apart along it;
// - past each guide's tip, its last direction carried on a few cells, fading from 1 to 0, so the
//   field flows on past where a guide stops and dies out there (Fu's "empty segments");
// - near the scalp (a ring of cells within 1.5 cells of the head), a weak (0.3) direction: the
//   field there from a first solve, its inward part removed (Fu's near-scalp ring), leaned toward
//   the scalp's pore direction normalize(n + down * min(n . down + 1, 1)) (Dr.Hair), so roots leave
//   the scalp and never point into it. The first solve holds that pore direction weakly at the scalp's
//   cells, so a field with few guides still flows somewhere.
//
// The smoothness is the harmonic (Dirichlet) energy, not Fu's squared Laplacian: the same as Dr.Hair's
// harmonic field and I3D's Poisson solve, one sparse SPD matrix for the three components, solved by
// Jacobi-preconditioned conjugate gradients without a factorisation.
//
// A part (a plane: PartLine) is a seam: neighbouring cells on its two sides are not coupled over the
// top and front of the head (above 0.2 and in front of -0.25 of the head's radius in its frame), so
// each side keeps its own sweep instead of the two cancelling. Hair sweeping away from a seam leaves
// the scalp bare along it, so it is for a real part. A smooth field cannot hold crossing flows
// elsewhere (braids, a cowlick, hair crossing over a part at the back): those blend.
//
// Growth (grow): from a root on the scalp, steps of half a cell along the trilinearly sampled field;
// a turn of more than 60 degrees from the last step takes their mean, more than 150 stops (the
// DeepSketchHair rules); it stops where the field fades below 0.05 (past the guides' tips), where it
// leaves the cells (the shell's hull: hair the photos do not show) or at the length given; it is
// kept off the head, and (FieldGrowOptions::ride) steered toward a height in the shell: a field's
// streamlines keep to their layer, so without it a guide rooted on the scalp stays low under keys
// that ride the hair's outside.

#include <cstdint>
#include <vector>

#include "groomlab/hair_shell.h"
#include "groomlab/lock_guides.h"

namespace groomlab {

// A guide the field keeps to, root first, and how strongly (1: a drawn key; less: a guess, such
// as a combed guide where no key is near).
struct FieldGuide {
    std::vector<Vec3f> points;
    float weight = 1.0f;
};

struct GuideFieldOptions {
    float cell = 0.0f;            // the grid's cell (scene units; 0: a 14th of the head's radius)
    int maxCells = 128;           // cells along the grid's longest side at most (the cell grows to fit)
    float lambda = 100.0f;        // the guides' weight (squared in the energy), Fu's
    float tipFade = 3.0f;         // each guide carried on past its tip this many cells, fading 1 to 0
    float scalpMagnitude = 0.3f;  // the near-scalp ring's strength (Fu's 0.3)
    float ringCells = 1.5f;       // the ring: cells whose middle is this many cells or less off the head
    float poreLean = 0.5f;        // the ring's direction leaned this much toward the scalp's pore direction
    PartLine part;                // a seam over the top and front (off: none)
    int maxIterations = 3000;     // conjugate gradient iterations a solve, at most
    float tolerance = 1e-6f;      // stop when a Jacobi step would move the cells less (root mean square, field units)
};

struct FieldGrowOptions {
    float step = 0.5f;               // cells a step
    float stopMagnitude = 0.05f;     // the field fading below this stops a guide
    float averageTurnDegrees = 60.0f;
    float stopTurnDegrees = 150.0f;
    float keepOut = 0.0f;            // kept this far off the head (scene units; 0: 0.02 head radii)
    float pointSpacing = 0.0f;       // the grown guide's points this far apart (0: a 20th of the head's radius)
    // Steered toward this height in the shell (a share of its thickness off the head, reached over the
    // first 0.3 head radii; 0: the field alone). A field's guides follow its layers: one rooted on the
    // scalp stays low where the guides it fills between ride the hair's outside.
    float ride = 0.0f;
    float rideGain = 2.5f;           // how hard (a head radius off: this much of a step's direction)
};

// Why a grown guide stopped.
enum class GrowStop { Length, Faded, Hull, Turn };

struct GrownGuide {
    std::vector<Vec3f> points;  // root first; the root as given
    GrowStop stop = GrowStop::Length;
    float length = 0.0f;
};

class GuideField {
public:
    struct Stats {
        int nx = 0, ny = 0, nz = 0;  // the grid
        float cell = 0.0f;
        int cells = 0;               // cells in the hair's volume (the unknowns)
        int guideCells = 0;          // cells a guide (or its tip) holds
        int ringCells = 0;           // cells in the near-scalp ring
        int partSeams = 0;           // neighbour pairs not blended across the part
        int iterations = 0;          // both solves'
        float residual = 0.0f;       // the last solve's, relative
        double seconds = 0.0;
    };

    GuideField() = default;
    // The field round `shell` from `guides`; `hairScalp` (when given) where hair grows: the pore
    // direction is held only at cells over it. `shell` must outlive the field (grow reads the head).
    GuideField(const HairShell& shell, const std::vector<FieldGuide>& guides, const GuideFieldOptions& options = {},
               const ScalpMesh* hairScalp = nullptr);

    bool empty() const { return shell_ == nullptr; }
    // The field at `p` (trilinear over the cells in the volume, zero outside); `inside`: the share of
    // the eight cells round p that are in it.
    Vec3f at(const Vec3f& p, float* inside = nullptr) const;
    // A guide grown from `root` (on the scalp), at most `maxLength` long.
    GrownGuide grow(const Vec3f& root, float maxLength, const FieldGrowOptions& options = {}) const;
    const Stats& stats() const { return stats_; }
    float cell() const { return cell_; }

private:
    const HairShell* shell_ = nullptr;
    Vec3f origin_;
    float cell_ = 1.0f;
    int nx_ = 0, ny_ = 0, nz_ = 0;
    std::vector<int> index_;    // grid cell -> unknown (-1: not in the volume)
    std::vector<Vec3f> field_;  // by unknown
    Stats stats_;
};

// The part the guides show: among guides rooted over the top front of the head, those whose first
// stretch heads toward the character's left and those heading right, split by a plane square to
// `frame.left` (the offset that misplaces fewest; at least two a side, at most a fifth misplaced).
// Off when they do not split so. `votes`: the guides that took part.
PartLine partFromGuides(const HeadFrame& frame, const std::vector<std::vector<Vec3f>>& guides, int* votes = nullptr);

struct FieldFillOptions {
    GuideFieldOptions field;
    FieldGrowOptions grow;
    int nearest = 4;             // a root's length from this many nearest guides' (by 1 / distance^2 of their roots)
    float lengthSlack = 0.15f;   // grown at most that length times 1 + this (the field ends most first)
    float lengthRandom = 0.15f;  // and times 1 +- this, by root
    // Each grown guide steered toward its nearest keys' riding height (FieldGrowOptions::ride: their
    // median height in the shell, weighted as the length), at most maxRide.
    bool ride = true;
    float maxRide = 0.9f;
    std::uint64_t seed = 1;
};

struct FieldFill {
    std::vector<std::vector<Vec3f>> guides;  // one a root, root first (fewer than 2 points: nothing grew)
    int byLength = 0, byFade = 0, byHull = 0, byTurn = 0;  // why they stopped
    int empty = 0;                                         // roots nothing grew from
    GuideField::Stats field;
};

// Guides grown from `roots` through the field of `keys` (see GuideField), each at most its
// nearest keys' length (FieldFillOptions). Throws std::invalid_argument for an empty shell or no keys.
FieldFill fieldFill(const std::vector<Vec3f>& roots, const std::vector<FieldGuide>& keys, const HairShell& shell,
                    const FieldFillOptions& options = {}, const ScalpMesh* hairScalp = nullptr);

// A guide given the comb's S-wave across its flow and its tip's flip (combGuide's, CombOptions'
// amplitude, wavelength, coherence and flip, by `seed`): the detail a field's smooth guides lack.
std::vector<Vec3f> wavedGuide(const std::vector<Vec3f>& points, const HeadFrame& frame, const CombOptions& options, std::uint64_t seed);

}  // namespace groomlab
