// SPDX-License-Identifier: Apache-2.0
// Interpolate: shapes each strand from its nearest guides.
//
// Blends guide offsets (point - root) between roots with modified Shepard
// weights: every guide has its own reach R_g (`reach` times the distance to
// its `neighbors`-th nearest guide, so dense guides reach less far than sparse
// ones), and a strand within it weighs the guide ((R_g - d) / (R_g d))^p. A
// weight is 0 exactly where the strand leaves the guide's reach, and a
// guide's reach does not depend on the strand, so the blend is smooth across
// the scalp. Plain inverse distance over the k nearest shows seams and
// pops wherever the neighbour set changes.
//
// The binding keeps each strand's guides (16-bit indices up to 65535 guides) and their weights in
// 16 bits, as shares of 65536 summing to it exactly (operator_state.h kInterpolateWeightUnit; a
// guide whose share rounds to nothing is left out), and the blend reads those: the GPU preview
// blends the same weights from records a sixth the size. Against the unrounded weights a strand on
// a head moves by a few micrometres at most.
//
// With a scalp connected, each guide's shape is carried from its root to the
// strand's along the skin (surface_paths.h): a front from each guide root
// walks the rest scalp out to the guide's reach, and a strand reads the path
// to it at its root's triangle. Where the straight line between the roots
// hugs the skin (almost everywhere), the offsets are turned from the surface
// normal at the guide's root to the normal at the strand's root: the turn
// parallel transport along that line makes, which keeps the guide's direction
// along the skin as well as its lean out of it (a guide combed back on one
// side of a curved scalp stays combed back on the other, and does not push
// strands into the surface). Where the path bends round a fold, a thin flap or
// the rim of a shell, the guide's frame (normal and a direction along the skin)
// is carried through each bend, so its shape arrives as the skin it travelled
// over turned it, not by one turn between normals that may face apart. A
// field of directions over the whole scalp (its UVs' tangents, or any smooth
// field) would not do: on a curved scalp such a field must turn against the
// skin by as much as the curvature it encloses (a few degrees a centimetre at
// a head's hairline), or wind round a singular point, and every guide would be
// twisted by that on its way to its strands; UVs add seams and poles of their
// own. With `followSurface` (the default), the weights measure the distance
// along those paths too, not the straight line, so a guide does not reach
// across an ear, a fold or a thin shell to strands that are close in space but
// far along the skin; where the straight line hugs the skin the two are the
// same to the bit, so a smooth scalp's strands do not change. Without a scalp,
// or for a guide or strand not rooted on it, the straight line and no turn.
//
// Guides are resampled to a common count that is never below the strands'
// own resolution, so detailed guides (curls) keep their shape. Blending
// offsets of guides that point different ways shortens the result;
// `preserveLength` scales it back to the guides' blended length.
//
// A shape carried rigidly from a guide's root to a strand's does not know the skin under the
// strand: where that bulges more than the skin under the guide (an ear, a cheek, the neck, the
// back of the skull), the strand goes into it. With `keepClearance` (and a scalp), the strand is
// walked from the root through the scalp's signed distance field (distance_field.h, made once per
// scalp and kept): a point lower than the clearance (kClearanceFloor of the scalp's size, about a
// hair's width on a head, times the guides' influence; along the field's normal, as Collide
// measures it) is lifted to it, just onto the skin, and the points after it follow it, each kept
// its segment's length (follow the leader, as Collide does): the root and the lengths stay, the
// strand comes out of the skin and lies on it. Past the bulge the strand eases back to its shaped
// path: each point is led toward where it came plus what is left of the point before's offset
// from where that one came, the offset eased by kClearanceEase / (kClearanceEase + d) over the d
// between the two as they came (kClearanceEase of the scalp's size, about 1 cm on a head: an
// offset is halved over that much hair), its length still kept and still lifted out of the skin
// where that is under it. So hair crossing an ear drapes over it, touching it, and comes back to
// its groomed path below it, where carrying the offset rigidly on would leave it standing off the
// head. The walk asks at most kClearanceAsked points of a strand (every step-th and the tip,
// interpolateClearanceStep: every point of a strand of up to 17), each led from the one asked
// before it at their distance as they came; the points between move by the moves of the two asked
// around them, blended by index (fur of 50 segments asks 13 points, not 50). Where either of two
// asked points was lifted, the strand touches the skin there, and where they are farther apart
// than kClearanceSpan of the scalp's size a blend of moves would cut into the skin's curve: the
// points between them are walked one by one instead (the later of the two, once it was the one
// lifted, walked again). A point already clear of the skin is not moved, nor is a
// strand none of whose points is below the clearance (to the bit); a point the last one asked
// shows clear of the clearance (the field's bounds, the height changing at most about twice as
// fast as the point moves) is not asked. Points deeper than kClearanceDepth of the scalp's size
// are on the far side of an open scalp, not in it, as with Collide; points beside it (past an open
// edge, lower than half their distance from it either way) are clear of it. Strands not rooted on
// the scalp are left as they are. The walk takes the field's answers alone (Probe::fieldOnly:
// where the field does not blend, the closest point among the triangles around the closest points
// it holds there; Probe::relaxed: blended across creases up to about 26 degrees), so the GPU
// preview, walking the field's tables (look.slang ClearanceCS), makes the same strands; over a scalp deformed away from its rest pose
// the walk asks the rest pose's field vertex by vertex, the CPU's alone. The field's grid is the
// world's and the distances go by the scalp's size rounded to an eighth of an octave: an edit of
// one side of the scalp leaves the strands on the other side as they were, to the bit.
//
// With a `regionMap` (regions painted on the scalp in colors, XGen's region maps), a strand blends
// only the guides rooted in its own region, as if they were the only guides: their reaches among
// themselves, the strand's candidates among them, so a region's blend is as smooth as a whole
// scalp's and the edge between two regions is hard, like a part. The regions are the distinct
// colors at the guides' roots (colors within kRegionTolerance of each other one region; a guide's
// color between two regions' colors, soft paint or a vertex map blending them across a face, joins
// the nearer of the two), so no exact colors need painting; a strand joins the region whose color
// is nearest its root's, when that is close, or its color lies close to the line between two
// regions' colors (soft paint between them); a strand of a color no guide is rooted on (a region
// without a guide), or not
// rooted on the scalp, blends the nearest guides of any region. Each strand's region number goes
// to `regionChannel` (-1: none) for Clump, BlendNeighbors and Tangle to keep within. A brush dab
// binds again as without regions while every guide and strand keeps its region; a guide rooted
// into another region binds every strand again, and a strand changing region (a guide rooted
// in a color none was) evaluates in full. The GPU preview blends from this binding as from any.
//
// With a `limitDistance`, guides only reach the strands around them: a strand whose
// nearest guide root is farther keeps its incoming shape (from Flow, Bend,
// Scatter), and closer ones blend toward the guides by `falloff` over the
// distance. `take` "length" keeps each strand's own shape and only brings its
// length to the guides'. Each strand's influence (0-1) goes to `influenceChannel`.
//
// Guide attributes come along: per-guide values blend into strand channels
// of the same name, and per-point layers (painted along the guides) into
// point channels, each guide's layer resampled by arc length to the common
// resolution, blended with the same weights, and read at each strand point's
// arc-length position.
//
// With `fillTubes` (tube guides, tube.h: guides from a hierarchy carry their tubes), a strand
// whose root lies inside a guide's tube at its root rides that tube instead of blending guides:
// its root is taken onto the tube's footprint, the root section laid flat on the scalp round the
// guide's root (TubeSpine::setRootSurface: turned from the guide's root direction onto the
// scalp's normal there, so a guide leaving the scalp at any angle, even lying along it, has a
// footprint of the section's whole area), which gives it section coordinates (u, v), and while
// those lie inside the tube's shape (and the root lies within kTubeAlongReach of the section's
// larger half-extent off the footprint's plane), the strand's point at t (0 root, 1 tip, by point
// index) is the tube's point of (u, v) at t (the guide taken smooth and evenly along its length,
// as the blend takes it), its root kept where it grows and the difference faded out by
// (1 - t / join)^2 (join: TubeSpine::rootJoin, a few root extents along the guide), as a child
// guide's root is snapped onto the scalp. So
// the strand stays inside the shape to the tip, flattening, twisting and tapering with the tube,
// and as long as the guide (a Length or a cut after it still works). Where tubes overlap a strand
// takes the deepest one (the tubes of every level of the hierarchy fill, not only the level the
// node gets: GuideSet::levelTubes, so a child guide's tube inside its parent's takes the strands
// rooted in it and the parent's tube the rest of its footprint), then the one it lies most
// centrally in (tubeShapeDepth: the smallest normalized radius), then the first guide of equals.
// Such a strand ignores limitDistance (influence 1), parts and regions (its tube says where it
// belongs); keepClearance still lifts it out of the skin; its guide attributes and layers blend
// as ever. It carries tubeId (its guide's number, as clumpId numbers clumps: stableGroupNumbers
// of the guide's ID), tubeU and tubeV (its section coordinates).
//
// `tubeBlend` gives the tubes a soft edge, so the seam between a tube and the strands round it
// vanishes: a strand rooted outside a tube but within tubeBlend of its outline (on the tube's
// footprint, in half-widths of the root section: tubeOutsideDistance) leans from the shape
// it has without that tube (the blend of the nearest guides, or the shallower tube it lies in)
// toward riding the tube at its own section coordinates (outside the outline, the tube's sections
// carried on past it), by 1 - smoothstep(distance / tubeBlend): all of the way on the outline,
// none at the band's edge. The nearest such tube deeper than the strand's own takes it. Strands
// in no tube and no band carry -1, 0, 0 and blend exactly as without fillTubes. Off by default.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <new>
#include <utility>
#include <vector>

#if defined(_M_X64) || defined(__SSE2__)
#define GROOMLAB_INTERPOLATE_SSE2 1
#include <emmintrin.h>
#else
#define GROOMLAB_INTERPOLATE_SSE2 0
#endif

#include "groomlab/curve.h"
#include "groomlab/distance_field.h"
#include "groomlab/frames.h"
#include "groomlab/graph.h"
#include "groomlab/look/look_cpu.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/paths.h"
#include "groomlab/point_index.h"
#include "groomlab/portable_math.h"
#include "groomlab/ramp.h"
#include "groomlab/random.h"
#include "groomlab/scalp_pieces.h"
#include "groomlab/surface_paths.h"
#include "groomlab/texture.h"
#include "incremental.h"
#include "internal.h"

namespace groomlab {

namespace {

constexpr std::size_t kMinGuideSamples = 32;
constexpr std::size_t kMaxNeighbors = 16;
constexpr std::size_t kMaxCandidates = kMaxNeighbors * 4 + 2;  // candidateCount never exceeds this

// keepClearance, as shares of the scalp's size (its rest pose's bounding box diagonal: about 35 cm
// for a head, rounded to an eighth of an octave, so an edit of the scalp changes none of these
// unless it steps): the clearance a strand keeps (about 0.17 mm on a head, a hair's width or two:
// a strand drawn at its width lies on the skin, its middle clear of it by more than the depth
// buffer tells apart at any working distance, and a segment of a few millimetres over a head's
// curve sags less than that between its ends), how far an offset eases back over (an offset is
// halved over that much of a strand: about 1 cm on a head), how far apart two asked points on the
// skin walk the points between them one by one (about 3 mm on a head: a span that long over skin
// curving as tightly as the ease's length, an ear's rim, sags about the clearance between its
// ends, (size / 128)^2 / (8 size / 32) = size / 2048, so fur's short spans keep the blend of
// moves and walk no more points than they ask), how far above the skin the field's
// fine band reaches (the cap: about 1 cm on a head; the field shared with the viewport's hair
// grid, hair_grid.cpp, and a strand's height known that far above the skin), how deep under the
// skin a point is still in it (deeper: the far side of an open scalp), and the field's cells (the
// power of two nearest that share: the world's grid). The field's fine band (in cells) reaches far
// enough above the cap that a point it does not blend is settled by its brick's bounds (a cap of 8
// cells, bricks of 4: their bounds hold to about 3.5 cells), not answered from closest faces; its
// cells are coarser than Collide's, fine enough that corners of a cell over a head's curve stay
// within the field's crease test (2.6 degrees).
constexpr float kClearanceFloor = 1.0f / 2048.0f;
constexpr float kClearanceEase = 1.0f / 32.0f;
constexpr float kClearanceSpan = 1.0f / 128.0f;
constexpr float kClearanceCap = 1.0f / 32.0f;
constexpr float kClearanceDepth = 1.0f / 8.0f;
constexpr float kClearanceCell = 1.0f / 256.0f;
constexpr int kClearanceBandAboveCap = 4;

// What keepClearance asks of the scalp: its surface to push against and the distances above.
struct Clearance {
    CollisionSurface surface;
    float floor = 0.0f, ease = 0.0f, span = 0.0f, cap = 0.0f, depth = 0.0f, searchRadius = 0.0f, beside = 0.0f;

    // For `scalp` (null: none, when it has no triangles).
    static Clearance of(const std::shared_ptr<const ScalpMesh>& scalp) {
        Clearance c;
        if (!scalp || scalp->triangleCount() == 0) return c;
        const std::vector<Vec3f>& points = scalp->hasRest() ? scalp->restPoints() : scalp->points();
        Vec3f lo = points.empty() ? Vec3f{} : points.front(), hi = lo;
        for (const Vec3f& p : points) {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        const float diagonal = (hi - lo).length();
        if (!(diagonal > 0.0f) || !std::isfinite(diagonal)) return c;
        const float size = pm::exp2(std::round(pm::log2(diagonal) * 8.0f) / 8.0f);
        const float cell = pm::exp2(std::round(pm::log2(size * kClearanceCell)));
        c.floor = size * kClearanceFloor;
        c.ease = size * kClearanceEase;
        c.span = size * kClearanceSpan;
        c.cap = size * kClearanceCap;
        c.depth = size * kClearanceDepth;
        c.searchRadius = std::max(c.cap, c.depth) * 1.001f + 1e-6f;  // (as Collide's)
        c.beside = 2.0f * cell;
        const int band = static_cast<int>(std::ceil(c.cap / cell)) + kClearanceBandAboveCap;
        c.surface = CollisionSurface::of(scalp, c.floor, c.depth, c.searchRadius, FieldGrid{cell, band});
        return c;
    }
    explicit operator bool() const { return static_cast<bool>(surface); }
    // A strand's push's probe: points beside the scalp (past an open edge of it, farther than two
    // of the field's cells) are left alone, as clear of it; the field's answers alone (the GPU's
    // walk has them too: look.slang).
    MeshDistanceField::Probe probe() const {
        MeshDistanceField::Probe p;
        p.beside = beside;
        p.fieldOnly = true;
        p.relaxed = true;  // a soft target: blended across gentle creases
        return p;
    }
};

// interpolateClearance's inputs from the scalp's Clearance at hand (below).
std::shared_ptr<const InterpolateClearance> clearanceInputs(const Clearance& c, const MeshPtr& scalp, std::string* why, bool host);

// Tests: the binding as first written (setInterpolateReferencePaths), to compare.
std::atomic<bool> referencePaths{false};

// A polyline held as rows (x, y and z arrays) below: each operation is Vec3f's, in its order,
// four points at a time in SSE lanes where the CPU has them, so every point comes out as the
// Vec3f code makes it, bit for bit.

// len[j] = |p[j] - p[j - 1]| for j in [1, n): (p[j] - p[j - 1]).length() for each.
void segmentLengths(const float* x, const float* y, const float* z, std::size_t n, float* len) {
    std::size_t j = 1;
#if GROOMLAB_INTERPOLATE_SSE2
    for (; j + 4 <= n; j += 4) {
        const __m128 dx = _mm_sub_ps(_mm_loadu_ps(x + j), _mm_loadu_ps(x + j - 1));
        const __m128 dy = _mm_sub_ps(_mm_loadu_ps(y + j), _mm_loadu_ps(y + j - 1));
        const __m128 dz = _mm_sub_ps(_mm_loadu_ps(z + j), _mm_loadu_ps(z + j - 1));
        const __m128 d2 = _mm_add_ps(_mm_add_ps(_mm_mul_ps(dx, dx), _mm_mul_ps(dy, dy)), _mm_mul_ps(dz, dz));
        _mm_storeu_ps(len + j, _mm_sqrt_ps(d2));
    }
#endif
    for (; j < n; ++j) len[j] = (Vec3f(x[j], y[j], z[j]) - Vec3f(x[j - 1], y[j - 1], z[j - 1])).length();
}

constexpr std::size_t kBindingMemo = 2;  // bindings an Interpolate keeps (a preview and a render, say)
constexpr std::size_t kBindChunk = 256;  // strands bound per task
constexpr std::size_t kSmallGuides = 65535;  // kept guides at most whose indices a binding keeps in 16 bits

// A strand's weights w[0, n) (its guides idx and twists alongside) as Interpolate keeps them
// (operator_state.h kInterpolateWeightUnit): each its share of their sum in 65536ths, the largest
// remainders rounded up and the others down so the shares sum to 65536 exactly (Hamilton's
// method); a guide whose share is 0 is dropped (the others keep their order). The codes (share - 1) go to `codes`; returns the guides kept, 0 when
// the weights sum to nothing (or not to a number).
std::size_t quantizeWeights(std::size_t* idx, const float* w, float* twist, std::size_t n, std::uint16_t* codes) {
    // The guides with a weight, in order, and their sum.
    std::array<std::uint8_t, kMaxCandidates + 1> live;
    std::size_t m = 0;
    double sum = 0.0;
    for (std::size_t j = 0; j < n; ++j) {
        if (!(w[j] > 0.0f)) continue;
        live[m++] = static_cast<std::uint8_t>(j);
        sum += static_cast<double>(w[j]);
    }
    if (m == 0 || !std::isfinite(sum)) return 0;
    constexpr std::uint32_t kWhole = 65536;
    const double scale = static_cast<double>(kWhole) / sum;
    std::array<std::uint32_t, kMaxCandidates + 1> share;
    std::array<double, kMaxCandidates + 1> rest;  // what rounding added (-0.5 to 0.5)
    std::uint32_t given = 0;
    for (std::size_t k = 0; k < m; ++k) {
        const double x = std::min(static_cast<double>(w[live[k]]) * scale, static_cast<double>(kWhole));
        share[k] = static_cast<std::uint32_t>(x + 0.5);  // (x >= 0: to the nearest)
        rest[k] = x - static_cast<double>(share[k]);
        given += share[k];
    }
    // Rounded to the nearest, the shares sum to 65536 within a few: each short one to the largest
    // remainder left (the first of equals), each over one taken back from the smallest, so the
    // shares rounded up are those of the largest remainders, as rounding down and handing out the
    // rest to them would make them.
    while (given < kWhole) {
        std::size_t best = 0;
        for (std::size_t k = 1; k < m; ++k) best = rest[k] > rest[best] ? k : best;
        ++share[best];
        rest[best] -= 1.0;
        ++given;
    }
    while (given > kWhole) {
        std::size_t best = m;
        for (std::size_t k = 0; k < m; ++k) {
            if (share[k] > 0 && (best == m || rest[k] < rest[best])) best = k;
        }
        --share[best];
        rest[best] += 1.0;
        --given;
    }
    std::size_t kept = 0;
    for (std::size_t k = 0; k < m; ++k) {
        if (share[k] == 0) continue;
        const std::size_t j = live[k];
        idx[kept] = idx[j];
        twist[kept] = twist[j];
        codes[kept] = static_cast<std::uint16_t>(share[k] - 1);
        ++kept;
    }
    return kept;
}

std::atomic<std::size_t> bindCount{0};
std::atomic<std::size_t> strandBindCount{0};

// A hash of points, chunk by chunk side by side and the chunks combined in order: the same
// whichever threads ran. Two coordinates per step.
std::uint64_t hashPoints(std::uint64_t seed, const std::vector<Vec3f>& points) {
    constexpr std::size_t kChunk = 4096;
    const std::size_t chunks = (points.size() + kChunk - 1) / kChunk;
    std::vector<std::uint64_t> hashes(chunks);
    parallelFor(0, chunks, [&](std::size_t c) {
        std::uint64_t h = hash64(c + 1);
        const std::size_t end = std::min(points.size(), (c + 1) * kChunk);
        for (std::size_t i = c * kChunk; i < end; ++i) {
            std::uint32_t xyz[3];
            std::memcpy(xyz, &points[i].x, sizeof(float));
            std::memcpy(xyz + 1, &points[i].y, sizeof(float));
            std::memcpy(xyz + 2, &points[i].z, sizeof(float));
            h = hashCombine(hashCombine(h, (static_cast<std::uint64_t>(xyz[0]) << 32) | xyz[1]), xyz[2]);
        }
        hashes[c] = h;
    }, 1);
    std::uint64_t h = hashCombine(seed, points.size());
    for (const std::uint64_t c : hashes) h = hashCombine(h, c);
    return h;
}

// The records of kBindChunk strands in a row (the last chunk fewer), strand i of the chunk's
// guides and their weights at [offsets[i], offsets[i + 1]).
struct BindingChunk {
    std::vector<std::uint32_t> offsets;
    // Per record, its guide (index into the guides kept: 2 points or more), in 16 bits while they
    // are kSmallGuides at most (`wide`: 32), and its weight (a code: interpolateWeight).
    bool wide = false;
    std::vector<std::uint16_t> guides16;
    std::vector<std::uint32_t> guides32;
    std::vector<std::uint16_t> weights;
    // The records with a twist (what their path's bends add, SurfacePath::twist; the others: the
    // plain turn), in order, and their twists: few chunks have any.
    std::vector<std::uint32_t> twistAt;
    std::vector<float> twists;
    std::vector<float> nearest2;  // per strand: squared distance to the nearest guide on its side
    std::vector<char> bound;      // per strand: 0 when it has no points or no guide (left as it is)
    // Per strand: the squared distance of its farthest candidate guide (infinite when every guide
    // was one), and whether its candidates and the next guide out were strictly ordered by
    // distance (no ties: the candidates and their order are the distances' alone, whatever tree
    // found them). A strict strand whose candidates did not move, change reach or side, and
    // that no moved guide came within reach of, keeps its guides and weights when roots move.
    std::vector<float> reach2;
    std::vector<char> strict;
    // Over the chunk's strands with points: their roots' box, the largest reach2, whether every
    // one is strict. When nothing that changed comes within that of the box, binding again keeps
    // the whole chunk (shared, not copied).
    Vec3f lo, hi;
    float farthest2 = 0.0f;
    bool allStrict = true;
    bool any = false;
};

struct RegionSource;
struct RegionSplit;

// Each strand's guides and their weights, as the blend reads them: what depends only on where the
// strands and guides are rooted, not on the guides' shapes. In chunks, so binding again after a
// few guide roots moved (a brush dab) makes again only the chunks around them.
struct Binding {
    std::vector<std::shared_ptr<const BindingChunk>> chunks;
    std::size_t strandCount = 0;
    // What it was made from, for binding again when only some guide roots moved: the strands'
    // search positions and whether each has points, everything but the guide roots (baseKey),
    // the guide roots and their reaches.
    std::shared_ptr<const std::vector<Vec3f>> queries;
    std::shared_ptr<const std::vector<char>> hasPoints;  // 0: none; 1: points; 2: points, a core strand (preview_core.h)
    std::uint64_t baseKey = 0;
    std::vector<Vec3f> roots;
    std::vector<float> reaches;
    // With a region map: what the regions are read from, the regions, and each guide's reach
    // among its own region's guides (as `reaches`; infinite for a guide in none).
    std::shared_ptr<const RegionSource> regionSource;
    std::shared_ptr<const RegionSplit> regions;
    std::vector<float> regionReaches;
    // On a scalp: the strands' roots, the guides' and the paths along the skin from the guides
    // (their fronts, shared with a binding again where a guide's root and reach stayed).
    std::shared_ptr<const std::vector<StrandRoot>> strandRoots;
    std::vector<StrandRoot> guideRoots;
    std::shared_ptr<const SurfaceFronts> fronts;

    // Strand s's record.
    struct Record {
        const BindingChunk* chunk;
        std::uint32_t begin, count;
        float nearest2;
        bool bound;
        std::uint32_t guide(std::uint32_t i) const { return chunk->wide ? chunk->guides32[begin + i] : chunk->guides16[begin + i]; }
        std::uint16_t code(std::uint32_t i) const { return chunk->weights[begin + i]; }
        float weight(std::uint32_t i) const { return interpolateWeight(chunk->weights[begin + i]); }
        // Its twists into to[0, count); false when it has none (to left as it was).
        bool twists(float* to) const {
            const auto first = std::lower_bound(chunk->twistAt.begin(), chunk->twistAt.end(), begin);
            if (first == chunk->twistAt.end() || *first >= begin + count) return false;
            std::fill(to, to + count, 0.0f);
            for (auto at = first; at != chunk->twistAt.end() && *at < begin + count; ++at) {
                to[*at - begin] = chunk->twists[static_cast<std::size_t>(at - chunk->twistAt.begin())];
            }
            return true;
        }
    };
    Record record(std::size_t s) const {
        const BindingChunk& c = *chunks[s / kBindChunk];
        const std::size_t i = s % kBindChunk;
        return {&c, c.offsets[i], c.offsets[i + 1] - c.offsets[i], c.nearest2[i], c.bound[i] != 0};
    }
};

// The nearest guide roots of many strands, as PointIndex::kNearest finds them, without walking the
// tree for each. The strands' roots fall into the cells of a grid (as wide as the guides' mean
// distance to their nearest other guide, or wider where the strands are sparse); each cell holds,
// found once, every guide that can be among the k nearest of any point in it (those within its
// center's k-th nearest distance plus its diagonal, with room for rounding), nearest its center
// first. A strand's k nearest are picked from its cell's guides by the squared distances the tree
// computes, kept as the tree keeps them (sorted on insertion). Where those are strictly ordered
// they and their order are the distances' alone, whichever search found them; where they tie, the
// tree's order among equals decides, and the binding asks the tree again (as it does for every
// strand that is not strict). A strand whose root is not a finite point, or whose cell holds too
// many guides, asks the tree; so do all when few search, or few to each guide.
class CandidateCells {
public:
    static constexpr std::size_t kMaxGuides = 128;  // a cell's guides at most (else the tree answers)

    // Cells for the k-nearest searches of the strands `wanted(s)` says will search.
    template <class Wanted>
    CandidateCells(const std::vector<Vec3f>& roots, const PointIndex& index, const std::vector<Vec3f>& queries,
                   const Wanted& wanted, std::size_t k)
        : k_(k) {
        const std::size_t guideCount = roots.size();
        if (guideCount == 0 || k == 0 || queries.empty()) return;
        for (const Vec3f& r : roots) {
            if (!finite(r)) return;  // a NaN distance would sort unlike the tree's
        }
        // The searching strands' box and count, chunk by chunk.
        constexpr std::size_t kChunk = 4096;
        const std::size_t chunks = (queries.size() + kChunk - 1) / kChunk;
        std::vector<Vec3f> chunkLo(chunks, Vec3f(kHuge, kHuge, kHuge)), chunkHi(chunks, Vec3f(-kHuge, -kHuge, -kHuge));
        std::vector<std::size_t> chunkCount(chunks, 0);
        parallelFor(0, chunks, [&](std::size_t c) {
            Vec3f lo = chunkLo[c], hi = chunkHi[c];
            const std::size_t end = std::min(queries.size(), (c + 1) * kChunk);
            for (std::size_t s = c * kChunk; s < end; ++s) {
                const Vec3f& q = queries[s];
                if (!wanted(s) || !finite(q)) continue;
                lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
                hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
                ++chunkCount[c];
            }
            chunkLo[c] = lo;
            chunkHi[c] = hi;
        }, 1);
        Vec3f lo(kHuge, kHuge, kHuge), hi(-kHuge, -kHuge, -kHuge);
        std::size_t searching = 0;
        for (std::size_t c = 0; c < chunks; ++c) {
            lo = {std::min(lo.x, chunkLo[c].x), std::min(lo.y, chunkLo[c].y), std::min(lo.z, chunkLo[c].z)};
            hi = {std::max(hi.x, chunkHi[c].x), std::max(hi.y, chunkHi[c].y), std::max(hi.z, chunkHi[c].z)};
            searching += chunkCount[c];
        }
        // A few strands (a dab binding again near the guides it moved), or few to each guide, ask
        // the tree: cells for them would cost more than they save.
        if (searching < kMinSearching || searching < kStrandsPerGuide * guideCount) return;
        // The guides' spacing: their mean distance to the nearest other one.
        std::vector<float> spacing(guideCount, 0.0f);
        parallelFor(0, guideCount, [&](std::size_t g) {
            std::size_t idx[2];
            float d2[2];
            if (index.kNearest(roots[g], 2, idx, d2) == 2) spacing[g] = std::sqrt(d2[1]);
        });
        double meanSpacing = 0.0;
        for (const float d : spacing) meanSpacing += d;
        meanSpacing /= static_cast<double>(guideCount);
        const double extent = std::max({double(hi.x) - lo.x, double(hi.y) - lo.y, double(hi.z) - lo.z});
        double size = meanSpacing;
        if (!(size > 0.0)) size = extent / 32.0;  // one guide, or all at one place
        if (!(size > 0.0)) size = 1.0;            // every strand at one place
        // The cells some strand searches from, numbered: at most kMaxCells in the box, and wider
        // ones while the strands are too few to share them (each cell's guides are found once,
        // for its strands to share; dense guides and sparse strands make many cells of few).
        lo_ = lo;
        std::vector<std::size_t> slotCells;
        auto count = [&](double extentAxis) { return static_cast<std::size_t>(std::max(1.0, std::ceil(extentAxis / size))); };
        for (int widen = 0;; ++widen) {
            nx_ = count(double(hi.x) - lo.x);
            ny_ = count(double(hi.y) - lo.y);
            nz_ = count(double(hi.z) - lo.z);
            if (static_cast<double>(nx_) * ny_ * nz_ > kMaxCells) {
                size *= 1.26;  // twice the volume
                continue;
            }
            inverseSize_ = 1.0 / size;
            const std::size_t cells = nx_ * ny_ * nz_;
            std::unique_ptr<std::atomic<std::uint8_t>[]> used(new std::atomic<std::uint8_t>[cells]());
            parallelFor(0, queries.size(), [&](std::size_t s) {
                if (!wanted(s) || !finite(queries[s])) return;
                std::atomic<std::uint8_t>& cell = used[cellOf(queries[s])];
                if (!cell.load(std::memory_order_relaxed)) cell.store(1, std::memory_order_relaxed);  // (read first: no line bounced)
            });
            slotCells.clear();
            for (std::size_t c = 0; c < cells; ++c) {
                if (used[c].load(std::memory_order_relaxed)) slotCells.push_back(c);
            }
            if (slotCells.size() * kStrandsPerCell <= searching || widen == 8) break;
            size *= 1.5;
        }
        slotOf_.assign(nx_ * ny_ * nz_, -1);
        for (std::size_t slot = 0; slot < slotCells.size(); ++slot) slotOf_[slotCells[slot]] = static_cast<std::int32_t>(slot);
        // Each cell's guides, cells side by side in groups, the groups laid end to end.
        const std::size_t slots = slotCells.size();
        constexpr std::size_t kSlotGroup = 64;
        const std::size_t groups = (slots + kSlotGroup - 1) / kSlotGroup;
        struct Group {
            std::vector<std::uint32_t> counts;  // per slot: its guides (kOverflow: too many)
            std::vector<std::uint32_t> guides;
        };
        std::vector<Group> gathered(groups);
        const double halfDiagonal = 0.5 * size * std::sqrt(3.0);
        parallelFor(0, groups, [&](std::size_t grp) {
            Group& group = gathered[grp];
            std::array<std::size_t, kMaxGuides> idx{};
            std::array<float, kMaxGuides> d2{};
            const std::size_t last = std::min(slots, (grp + 1) * kSlotGroup);
            for (std::size_t slot = grp * kSlotGroup; slot < last; ++slot) {
                const std::size_t c = slotCells[slot];
                const std::size_t ix = c % nx_, iy = (c / nx_) % ny_, iz = c / (nx_ * ny_);
                const Vec3f center(static_cast<float>(lo.x + (static_cast<double>(ix) + 0.5) * size),
                                   static_cast<float>(lo.y + (static_cast<double>(iy) + 0.5) * size),
                                   static_cast<float>(lo.z + (static_cast<double>(iz) + 0.5) * size));
                // A point of the cell has its k nearest within (the center's k-th nearest + the half
                // diagonal), so each of them is within that + the half diagonal of the center.
                std::size_t m = std::min({guideCount, std::max<std::size_t>(2 * k, 16), kMaxGuides});
                std::uint32_t kept = kOverflow;
                for (;;) {
                    const std::size_t found = index.kNearest(center, m, idx.data(), d2.data());
                    const double kth = std::sqrt(static_cast<double>(d2[std::min(found, k) - 1]));
                    const double reach = (kth + 2.0 * halfDiagonal) * 1.001 + 1e-6 * (size + kth);
                    const double reach2 = reach * reach;
                    if (found == guideCount || static_cast<double>(d2[found - 1]) > reach2) {
                        kept = 0;
                        while (kept < found && static_cast<double>(d2[kept]) <= reach2) ++kept;
                        break;
                    }
                    if (m == kMaxGuides) break;  // too many: the tree answers this cell's strands
                    m = std::min({guideCount, 2 * m, kMaxGuides});
                }
                group.counts.push_back(kept);
                if (kept == kOverflow) continue;
                for (std::uint32_t i = 0; i < kept; ++i) group.guides.push_back(static_cast<std::uint32_t>(idx[i]));
            }
        }, 1);
        // Laid end to end; each cell's guide roots as x, y and z rows of a multiple of 4 floats.
        begin_.assign(slots + 1, 0);
        rowBegin_.assign(slots + 1, 0);
        std::size_t slot = 0;
        for (const Group& group : gathered) {
            for (const std::uint32_t n : group.counts) {
                const std::uint32_t real = n == kOverflow ? 0 : n;
                counts_.push_back(n);
                begin_[slot + 1] = begin_[slot] + real;
                rowBegin_[slot + 1] = rowBegin_[slot] + 3 * ((real + 3) & ~3u);
                ++slot;
            }
        }
        guides_.resize(begin_[slots]);
        rows_.assign(rowBegin_[slots], 0.0f);
        parallelFor(0, groups, [&](std::size_t grp) {
            const Group& group = gathered[grp];
            std::size_t at = 0;
            const std::size_t last = std::min(slots, (grp + 1) * kSlotGroup);
            for (std::size_t s = grp * kSlotGroup; s < last; ++s) {
                const std::uint32_t n = begin_[s + 1] - begin_[s];
                const std::uint32_t padded = (n + 3) & ~3u;
                float* x = rows_.data() + rowBegin_[s];
                for (std::uint32_t i = 0; i < n; ++i) {
                    const std::uint32_t g = group.guides[at + i];
                    guides_[begin_[s] + i] = g;
                    x[i] = roots[g].x;
                    x[padded + i] = roots[g].y;
                    x[2 * padded + i] = roots[g].z;
                }
                at += n;
            }
        }, 1);
        ready_ = true;
    }

    // The k nearest guides of query q, closest first, as PointIndex::kNearest gives them where they
    // are strictly ordered; false when the cells cannot answer (the tree does).
    bool nearest(const Vec3f& q, std::size_t* indices, float* distances2, std::size_t* found) const {
        if (!ready_ || !finite(q)) return false;
        const std::int32_t slot = slotOf_[cellOf(q)];
        if (slot < 0 || counts_[static_cast<std::size_t>(slot)] == kOverflow) return false;
        const std::uint32_t begin = begin_[slot], n = begin_[slot + 1] - begin;
        const std::uint32_t padded = (n + 3) & ~3u;
        const float* x = rows_.data() + rowBegin_[slot];
        const float* y = x + padded;
        const float* z = y + padded;
        // Each guide's squared distance as the tree computes it: d = guide - q, d.dot(d).
        alignas(16) float d2[kMaxGuides];
        std::uint32_t i = 0;
#if GROOMLAB_INTERPOLATE_SSE2
        const __m128 qx = _mm_set1_ps(q.x), qy = _mm_set1_ps(q.y), qz = _mm_set1_ps(q.z);
        for (; i < padded; i += 4) {
            const __m128 dx = _mm_sub_ps(_mm_loadu_ps(x + i), qx);
            const __m128 dy = _mm_sub_ps(_mm_loadu_ps(y + i), qy);
            const __m128 dz = _mm_sub_ps(_mm_loadu_ps(z + i), qz);
            _mm_store_ps(d2 + i, _mm_add_ps(_mm_add_ps(_mm_mul_ps(dx, dx), _mm_mul_ps(dy, dy)), _mm_mul_ps(dz, dz)));
        }
#endif
        for (; i < n; ++i) {
            const Vec3f d = Vec3f(x[i], y[i], z[i]) - q;
            d2[i] = d.dot(d);
        }
        // The k smallest, sorted on insertion as the tree's search keeps them.
        std::size_t kept = 0;
        for (i = 0; i < n; ++i) {
            const float d = d2[i];
            if (kept == k_ && d >= distances2[k_ - 1]) continue;
            std::size_t pos = std::min(kept, k_ - 1);
            while (pos > 0 && distances2[pos - 1] > d) {
                distances2[pos] = distances2[pos - 1];
                indices[pos] = indices[pos - 1];
                --pos;
            }
            distances2[pos] = d;
            indices[pos] = guides_[begin + i];
            if (kept < k_) ++kept;
        }
        *found = kept;
        return true;
    }

private:
    static constexpr float kHuge = std::numeric_limits<float>::max();
    static constexpr double kMaxCells = 1 << 18;
    static constexpr std::size_t kMinSearching = 4096;    // fewer searching strands ask the tree
    static constexpr std::size_t kStrandsPerGuide = 32;  // and fewer to each guide too
    static constexpr std::size_t kStrandsPerCell = 8;    // searching strands to a cell, on average, at least
    static constexpr std::uint32_t kOverflow = std::numeric_limits<std::uint32_t>::max();
    static bool finite(const Vec3f& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
    std::size_t cellOf(const Vec3f& q) const {
        // (Truncated, as floor for t >= 0: the grid starts at the strands' lowest corner, so t is
        // below 0 only by rounding, and past the last cell only by rounding: clamped.)
        auto axis = [&](float v, float lo, std::size_t n) {
            const double t = (static_cast<double>(v) - lo) * inverseSize_;
            const std::size_t i = t > 0.0 ? static_cast<std::size_t>(t) : 0;
            return i < n ? i : n - 1;
        };
        return (axis(q.z, lo_.z, nz_) * ny_ + axis(q.y, lo_.y, ny_)) * nx_ + axis(q.x, lo_.x, nx_);
    }

    std::size_t k_ = 0;
    bool ready_ = false;
    Vec3f lo_;
    double inverseSize_ = 0.0;
    std::size_t nx_ = 0, ny_ = 0, nz_ = 0;
    std::vector<std::int32_t> slotOf_;    // per cell: its slot, -1 when no strand searches from it
    std::vector<std::uint32_t> counts_;   // per slot: its guides, kOverflow when too many
    std::vector<std::uint32_t> begin_;    // per slot: its guides at [begin_[s], begin_[s + 1])
    std::vector<std::uint32_t> guides_;
    std::vector<std::size_t> rowBegin_;   // per slot: its guides' roots as rows (x, y, z of 4n floats)
    std::vector<float> rows_;
};

  // strands blended per task

// fillTubes: how far off the plane of a tube's root footprint a strand's root may lie and still
// be in the tube, in the root section's larger half-extents.
constexpr float kTubeAlongReach = 4.0f;

// fillTubes' work for one evaluation: every tube filled (the kept guides' first, in their order,
// then the other levels': GuideSet::levelTubes) laid along its guide (empty: none), its level and
// its number for tubeId; each strand's tube (an index into them; -1: none) and section
// coordinates; and the tube whose soft edge (tubeBlend) it lies in (-1: none), its coordinates
// there and how far it leans toward riding it (0 to 1).
struct TubeFill {
    std::vector<TubeSpine> spines;
    std::vector<int> depths;
    std::vector<std::uint64_t> ids;
    std::vector<float> numbers;
    std::vector<std::int32_t> tubeOf;
    std::vector<SectionPoint> at;
    std::vector<std::int32_t> edgeOf;
    std::vector<SectionPoint> edgeAt;
    std::vector<float> edgeWeight;
};

// The scalp's normal under a guide's root: at its root on the scalp, else at the scalp point
// nearest its first point (guides not rooted on this scalp); zero with neither.
Vec3f scalpNormalAt(const ScalpMesh& scalp, const StrandRoot& root, const Vec3f& first) {
    if (scalp.contains(root)) return scalp.normal(root);
    const SurfacePoint sp = scalp.closestPoint(first);
    return sp.root.face >= 0 ? sp.normal : Vec3f{};
}

// A thread's working memory for the blend, kept from strand to strand.
struct Scratch {
    std::vector<float> rows;  // a strand's x, y and z rows, its segments' lengths, its arc lengths
    // Per guide of a strand: its root's normal, whether it turns, its rows, its weight and its twist.
    std::vector<Vec3f> from;
    std::vector<char> turned;
    std::vector<const float*> guideRows;
    std::vector<std::uint32_t> guides;
    std::vector<float> weights, twists;
    std::vector<Vec3f> guided, blended, edged;
    std::vector<float> blendedLayer, strandArc;
    void reserve(std::size_t stride) {
        if (rows.size() < 5 * stride) rows.resize(5 * stride);
        if (from.size() < kMaxCandidates) {
            from.resize(kMaxCandidates);
            turned.resize(kMaxCandidates);
            guideRows.resize(kMaxCandidates);
            guides.resize(kMaxCandidates);
            weights.resize(kMaxCandidates);
            twists.resize(kMaxCandidates);
        }
    }
};

// The same guide to the bit: ID, root, points, painted values and layers (what a guide's rows,
// normal, length and attributes in the blend come from).
bool sameGuide(const Guide& a, const Guide& b) {
    auto sameFloats = [](const float* x, const float* y, std::size_t n) { return n == 0 || std::memcmp(x, y, n * sizeof(float)) == 0; };
    if (a.id != b.id || a.points.size() != b.points.size() || a.attributes.size() != b.attributes.size() ||
        a.pointAttributes.size() != b.pointAttributes.size() || std::memcmp(&a.root, &b.root, sizeof(StrandRoot)) != 0 ||
        !sameFloats(reinterpret_cast<const float*>(a.points.data()), reinterpret_cast<const float*>(b.points.data()), 3 * a.points.size())) {
        return false;
    }
    for (auto x = a.attributes.begin(), y = b.attributes.begin(); x != a.attributes.end(); ++x, ++y) {
        if (x->first != y->first || !sameFloats(&x->second, &y->second, 1)) return false;
    }
    for (auto x = a.pointAttributes.begin(), y = b.pointAttributes.begin(); x != a.pointAttributes.end(); ++x, ++y) {
        if (x->first != y->first || x->second.size() != y->second.size() ||
            !sameFloats(x->second.data(), y->second.data(), x->second.size())) {
            return false;
        }
    }
    return true;
}

// Strand s's guides, weights and the rest of its record the same to the bit in both bindings.
bool sameRecord(const Binding& a, const Binding& b, std::size_t s) {
    const Binding::Record x = a.record(s), y = b.record(s);
    if (x.bound != y.bound || x.count != y.count || std::memcmp(&x.nearest2, &y.nearest2, sizeof(float)) != 0) return false;
    if (x.count == 0) return true;
    for (std::uint32_t i = 0; i < x.count; ++i) {
        if (x.guide(i) != y.guide(i) || x.code(i) != y.code(i)) return false;
    }
    std::array<float, kMaxCandidates + 1> tx{}, ty{};
    x.twists(tx.data());
    y.twists(ty.data());
    return std::memcmp(tx.data(), ty.data(), x.count * sizeof(float)) == 0;
}

// Guide g's shape as the blend reads it: the smooth curve through its points (with
// `smoothGuides`, fine enough that sampling it at the common resolution follows the curve, not
// chords), resampled by arc length to `samples` points into `resampled`. Returns its length.
float guideShape(const Guide& g, std::size_t samples, bool smoothGuides, std::vector<Vec3f>& resampled) {
    thread_local std::vector<Vec3f> curve;  // a guide as the smooth curve through its points
    const Vec3f* along = g.points.data();
    std::size_t alongCount = g.points.size();
    if (smoothGuides && alongCount >= 3) {
        const int perSpan = std::max(4, static_cast<int>((2 * samples + alongCount - 2) / (alongCount - 1)));
        curve = smoothPolyline(along, alongCount, perSpan);
        along = curve.data();
        alongCount = curve.size();
    }
    resampled.resize(samples);
    resampleByArcLengthInto(along, alongCount, resampled.data(), samples);
    return arcLength(along, alongCount);
}

// A guide weighed by (the modified Shepard) `base` to the power `power`:
// nothing where base is 0 (out of reach), even at power 0.
// The outer share of a guide's reach over which the reaches' blend fades to the fringe's (a
// strand's depth inside its best-covering guide's reach, 0 at the fringe).
constexpr float kFringeBand = 0.25f;

float shepardWeight(float base, float power) {
    if (base <= 0.0f) return 0.0f;
    return power == 2.0f ? base * base : pm::pow(base, power);
}

// ---- Regions (regionMap) ------------------------------------------------------------------------

// How far apart (in RGB, 0-1 a channel) two colors of a region map may be and still be one region's:
// the guide-root colors closer than this are one region, and a strand's color this close to a
// region's (or to the line between two regions' colors: a soft edge between them) is in it.
constexpr float kRegionTolerance = 0.1f;

// A region map: the painted map regionMap names, read as RGB (a color as x y z) at a root on the
// scalp. Found as HairColor finds its map: the host's catalog (a vertex map is a color set of the
// scalp, a texture map an image at its UVs), else a color set of the name, else an image file.
class RegionMap {
public:
    RegionMap(const EvalContext& ctx, const std::string& name, MeshPtr scalp) : scalp_(std::move(scalp)) {
        const std::string about = std::string(op::kInterpolate) + ": regionMap '" + name + "': ";
        if (!scalp_ || scalp_->triangleCount() == 0) throw GraphError(about + "regions are painted on the scalp: connect the scalp input");
        const MapInfo* info = catalogMap(ctx, name);
        std::string kind = info ? info->kind : "";
        if (kind.empty()) kind = scalp_->hasColorSet(name) ? "vertices" : isImagePath(name) ? "image" : "";
        if (kind == "texture" || kind == "image") {
            const std::string file = kind == "image" ? name : info->texture;
            if (file.empty()) throw GraphError(about + "the texture map has no image file yet");
            if (!scalp_->hasUVs()) throw GraphError(about + "an image needs a scalp with UVs");
            try {
                texture_ = loadTexture(resolvePath(file));
            } catch (const TextureError& e) {
                throw GraphError(about + e.what());
            }
        } else if (kind == "vertices") {
            if (!scalp_->hasColorSet(name)) {
                throw GraphError(about + "the vertex map does not fit this scalp (paint it again, or convert it)");
            }
            colorSet_ = name;
        } else if (kind == "guides") {
            throw GraphError(about + "a guide layer lies on the guides: paint the regions on the scalp (a vertex or texture map)");
        } else {
            throw GraphError(about + "no map, color set or image of that name (Groom Tools > Paint > New... makes a color map)");
        }
    }
    // The map's color at `root`; false when the root is not on the scalp.
    bool at(const StrandRoot& root, Vec3f* rgb) const {
        if (!scalp_->contains(root)) return false;
        if (texture_) {
            const ScalpMesh::UV uv = scalp_->uvAt(root);
            const std::array<float, 4> t = texture_->sample(uv[0], uv[1]);
            *rgb = {t[0], t[1], t[2]};
        } else {
            const ScalpMesh::Color c = scalp_->colorAt(colorSet_, root);
            *rgb = {c[0], c[1], c[2]};
        }
        return true;
    }

private:
    MeshPtr scalp_;
    TexturePtr texture_;
    std::string colorSet_;
};

// What regions are read from: the map, and each strand's color at its root (NaN: not on the scalp),
// which depend on the strands' roots alone (kept while the strands are; `name` and `salt`: the
// param and what the map was read from, contextSalt).
struct RegionSource {
    std::shared_ptr<const RegionMap> map;
    std::shared_ptr<const std::vector<Vec3f>> strandColors;
    std::string name;
    std::uint64_t salt = 0;
};

// The regions: each one's color, each kept guide's region (-1: not rooted on the scalp) and each
// strand's (-1: no region's color: any guide).
struct RegionSplit {
    std::vector<Vec3f> palette;
    std::vector<std::int32_t> guides;
    std::shared_ptr<const std::vector<std::int32_t>> strands;
};

bool sameStrandRegions(const RegionSplit& a, const RegionSplit& b) {
    if (a.strands == b.strands) return true;
    return a.strands && b.strands && *a.strands == *b.strands;
}

// The region of a strand of color c (NaN: none): the region whose color is nearest, when that is
// within kRegionTolerance, or c lies that close to the line from it to another region's color
// (a soft edge between the two, where the paint blends them); else -1 (an area no guide's root is
// in: the strand blends the nearest guides of any region).
std::int32_t regionOfColor(const std::vector<Vec3f>& palette, const Vec3f& c) {
    if (palette.empty() || !(c.x == c.x)) return -1;
    std::size_t best = 0;
    float best2 = std::numeric_limits<float>::infinity();
    for (std::size_t r = 0; r < palette.size(); ++r) {
        const Vec3f d = c - palette[r];
        if (d.dot(d) < best2) {
            best2 = d.dot(d);
            best = r;
        }
    }
    const float tolerance2 = kRegionTolerance * kRegionTolerance;
    if (best2 <= tolerance2) return static_cast<std::int32_t>(best);
    const Vec3f& p = palette[best];
    for (std::size_t r = 0; r < palette.size(); ++r) {
        if (r == best) continue;
        const Vec3f e = palette[r] - p;
        const float t = std::clamp((c - p).dot(e) / std::max(e.dot(e), 1e-12f), 0.0f, 1.0f);
        const Vec3f d = c - (p + e * t);
        if (d.dot(d) <= tolerance2) return static_cast<std::int32_t>(best);
    }
    return -1;
}

// The regions of guides rooted at `guideRoots` (the kept guides'): the distinct colors of the map
// at their roots (in RGB order, colors within kRegionTolerance of a region's first one joining it,
// each region the mean of its colors; one lying between two others, a blend of them, is not a
// region of its own), each guide in the nearest; each strand's from its color.
// The strands' regions of `previous` are kept when the regions' colors are the same to the bit
// (`previous` made from the same source).
RegionSplit splitRegions(const RegionSource& source, const std::vector<StrandRoot>& guideRoots, const RegionSplit* previous) {
    RegionSplit out;
    const std::size_t count = guideRoots.size();
    std::vector<Vec3f> colors(count);
    std::vector<std::uint32_t> order;
    for (std::size_t g = 0; g < count; ++g) {
        if (source.map->at(guideRoots[g], &colors[g])) order.push_back(static_cast<std::uint32_t>(g));
    }
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        const Vec3f &x = colors[a], &y = colors[b];
        if (x.x != y.x) return x.x < y.x;
        if (x.y != y.y) return x.y < y.y;
        if (x.z != y.z) return x.z < y.z;
        return a < b;
    });
    struct Cluster {
        Vec3f first, sum;
        float n = 0.0f;
    };
    std::vector<Cluster> clusters;
    for (const std::uint32_t g : order) {
        const Vec3f& c = colors[g];
        std::size_t into = clusters.size();
        float nearest2 = kRegionTolerance * kRegionTolerance;
        for (std::size_t k = 0; k < clusters.size(); ++k) {
            const Vec3f d = c - clusters[k].first;
            if (d.dot(d) <= nearest2) {
                nearest2 = d.dot(d);
                into = k;
            }
        }
        if (into == clusters.size()) clusters.push_back({c, Vec3f{}, 0.0f});
        clusters[into].sum += c;
        clusters[into].n += 1.0f;
    }
    // The regions: the colors, most guides first, but those lying between two regions' colors (a
    // guide rooted in soft paint between two regions, or where a vertex map blends them across a
    // face) join the nearer of the two, as a strand of that color does.
    std::vector<std::size_t> byCount(clusters.size());
    for (std::size_t k = 0; k < byCount.size(); ++k) byCount[k] = k;
    std::stable_sort(byCount.begin(), byCount.end(), [&](std::size_t a, std::size_t b) { return clusters[a].n > clusters[b].n; });
    std::vector<char> region(clusters.size(), 0);
    std::vector<Vec3f> regionColors;
    const float tolerance2 = kRegionTolerance * kRegionTolerance;
    for (const std::size_t k : byCount) {
        const Vec3f c = clusters[k].sum * (1.0f / clusters[k].n);
        bool between = false;
        for (std::size_t a = 0; a < regionColors.size() && !between; ++a) {
            for (std::size_t b = a + 1; b < regionColors.size() && !between; ++b) {
                const Vec3f e = regionColors[b] - regionColors[a];
                const float t = std::clamp((c - regionColors[a]).dot(e) / std::max(e.dot(e), 1e-12f), 0.0f, 1.0f);
                const Vec3f d = c - (regionColors[a] + e * t);
                between = d.dot(d) <= tolerance2;
            }
        }
        if (between) continue;
        region[k] = 1;
        regionColors.push_back(c);
    }
    for (std::size_t k = 0; k < clusters.size(); ++k) {
        if (region[k]) out.palette.push_back(clusters[k].sum * (1.0f / clusters[k].n));  // in RGB order
    }
    out.guides.assign(count, -1);
    for (const std::uint32_t g : order) {
        float best2 = std::numeric_limits<float>::infinity();
        for (std::size_t r = 0; r < out.palette.size(); ++r) {
            const Vec3f d = colors[g] - out.palette[r];
            if (d.dot(d) < best2) {
                best2 = d.dot(d);
                out.guides[g] = static_cast<std::int32_t>(r);
            }
        }
    }
    const std::vector<Vec3f>& strandColors = *source.strandColors;
    auto samePalette = [&] {
        return previous && previous->strands && previous->strands->size() == strandColors.size() &&
               previous->palette.size() == out.palette.size() &&
               (out.palette.empty() || std::memcmp(previous->palette.data(), out.palette.data(), out.palette.size() * sizeof(Vec3f)) == 0);
    };
    if (samePalette()) {
        out.strands = previous->strands;
    } else {
        auto strands = std::make_shared<std::vector<std::int32_t>>(strandColors.size());
        parallelFor(0, strandColors.size(), [&](std::size_t s) { (*strands)[s] = regionOfColor(out.palette, strandColors[s]); }, 4096);
        out.strands = std::move(strands);
    }
    return out;
}

class InterpolateOp : public Operator {
public:
    std::string typeName() const override { return op::kInterpolate; }
    // The influence with a limitDistance, the regions with a regionMap; and the guides' painted maps
    // (their guides input carries them).
    std::vector<ChannelWrite> channelWrites() const override {
        std::vector<ChannelWrite> out;
        const std::string influence = params().getString("influenceChannel");
        if (params().getDouble("limitDistance") > 0.0 && !influence.empty()) out.push_back({influence, "influenceChannel"});
        const std::string region = params().getString("regionChannel");
        if (!params().getString("regionMap").empty() && !region.empty()) out.push_back({region, "regionChannel"});
        if (fillsTubes()) {
            for (const char* name : {"tubeId", "tubeU", "tubeV"}) out.push_back({name, ""});
        }
        return out;
    }
    bool carriesChannels(std::size_t slot) const override { return slot <= 1; }
    std::string category() const override { return "grow"; }
    std::vector<PortSpec> inputs() const override {
        return {{"strands", PortType::Strands}, {"guides", PortType::Guides, true}, {"scalp", PortType::Mesh, true}};
    }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"neighbors", std::int64_t{3}, "How far each guide reaches: to its neighbors-th nearest guide (times "
                                                    "reach). 1: every strand takes its nearest guide alone, with hard edges"}
                .range(1, static_cast<double>(kMaxNeighbors))
                .soft(1, 8),
            ParamSpec{"reach", 1.5, "Each guide's reach, as a multiple of the distance to its neighbors-th nearest guide; "
                                    "more blends more guides into each strand"}
                .atLeast(1)
                .soft(1, 4),
            ParamSpec{"power", 2.0, "Falloff power of the guide weights over distance (2 neighbors or more)"}
                .atLeast(0)
                .soft(0, 4)
                .adv()
                .when("neighbors", {"2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16"}),
            ParamSpec{"smoothGuides", true, "Follow each guide as the smooth curve the viewport draws through its points (a "
                                            "centripetal Catmull-Rom spline), not straight lines between them, so a bend "
                                            "drawn with few points stays round in the hair"}
                .adv(),
            ParamSpec{"preserveLength", true, "Scale each blended strand to its guides' blended length (blending guides that "
                                              "point different ways would shorten it; take shape)"}
                .adv()
                .when("take", {"shape"}),
            ParamSpec{"keepClearance", true, "Keep strands out of the skin: where the skin under a strand bulges more "
                                             "than under its guides (an ear, a cheek, the neck), the part that would "
                                             "sink in is lifted just onto the skin, its length kept, and past the bulge "
                                             "the strand eases back to its groomed path, so hair drapes over an ear "
                                             "instead of standing off it (take shape; needs the scalp input)"}
                .when("take", {"shape"}),
            ParamSpec{"respectParts", true, "Never blend guides from the other side of a part"},
            ParamSpec{"followSurface", true, "Measure how far guides reach along the scalp (the scalp input), not in a "
                                              "straight line, so they do not reach across an ear, a fold or a thin shell "
                                              "(the same on a smooth scalp)"},
            ParamSpec{"limitDistance", 0.0, "How far guides reach: strands farther from every guide root keep their "
                                            "incoming shape (e.g. from Flow); 0: every strand follows the guides (and "
                                            "falloff and influenceChannel do nothing)", "length"}
                .atLeast(0)
                .soft(0, 10)
                .in("Limit"),
            ParamSpec{"falloff", Ramp::linear(1.0f, 0.0f), "With limitDistance above 0: the guides' influence over it, at "
                                                           "the guide to limitDistance away"}
                .in("Limit"),
            ParamSpec{"take", std::string("shape"), "shape: the guides' shape and length; length: each strand keeps its own "
                                                    "shape and takes the guides' length", "", {"shape", "length"}}
                .adv(),
            ParamSpec{"influenceChannel", std::string("guideInfluence"), "With limitDistance above 0: per-strand channel "
                                                                       "receiving the guides' influence, 0-1 (empty: none)"}
                .in("Limit")
                .adv()
                .writesChannel(),
            ParamSpec{"regionMap", std::string(),
                      "Regions painted on the scalp in colors (empty: none): a strand blends only the guides rooted in "
                      "its own region, with a hard edge between regions, like a part. Each color guides are rooted on "
                      "is a region (soft paint between two joins the nearer); a strand in a color no guide is rooted "
                      "on blends the nearest guides of any region. A map painted in Groom Tools > Paint by name, a "
                      "color set of that name on the scalp, or an image file at the scalp's UVs"}
                .in("Regions"),
            ParamSpec{"regionChannel", std::string("region"),
                      "With a regionMap: per-strand channel receiving each strand's region number (-1: none), which "
                      "Clump, BlendNeighbors and Tangle keep within (empty: none)"}
                .in("Regions")
                .adv()
                .writesChannel(),
            ParamSpec{"fillTubes", false,
                      "Fill the guides' tubes (tube guides: a hierarchy's guides with their sections and shape): a "
                      "strand rooted inside a guide's tube takes its place in the tube's section there and keeps it to "
                      "the tip, so it stays inside the tube as it flattens, twists and tapers, and is as long as the "
                      "guide, instead of blending the nearest guides. Where tubes overlap, the one it lies most "
                      "centrally in; strands rooted in no tube blend as ever. Writes tubeId, tubeU and tubeV, which "
                      "Clump's followTubes clumps inside each tube by (take shape)"}
                .in("Tubes")
                .when("take", {"shape"}),
            ParamSpec{"tubeBlend", 0.25,
                      "With fillTubes: the tubes' soft edge, as a share of each tube's half-width at its root. Strands "
                      "rooted outside a tube but within this of its outline lean from the guides' blend toward riding "
                      "the tube, all the way at the outline and not at all at the edge, so no seam shows round a "
                      "tube; 0: a hard edge", "fraction"}
                .range(0, 1)
                .in("Tubes")
                .when("fillTubes", {"true"}),
        };
    }
    // fillTubes on, where it shows (take shape).
    bool fillsTubes() const { return params().getBool("fillTubes") && params().getString("take") == "shape"; }
    std::vector<std::string> externalInputs() const override {
        std::vector<std::string> names;
        if (params().getBool("respectParts")) names.push_back("parts");
        if (!params().getString("regionMap").empty()) {  // read on the scalp, found in the host's catalog
            names.push_back("scalp");
            names.push_back("maps");
        }
        return names;
    }
    // A region map's source joins the key, as HairColor's map's does: a texture map's or an image's
    // file stamp, a color set's hash (the scalp's key leaves color sets out).
    std::uint64_t contextSalt(const EvalContext& ctx) const override { return regionSalt(ctx, params().getString("regionMap")); }
    static std::uint64_t regionSalt(const EvalContext& ctx, const std::string& name) {
        if (name.empty()) return 0;
        const MapInfo* info = catalogMap(ctx, name);
        if (info && info->kind == "texture") return hashString(info->texture.empty() ? 1 : fileStamp(resolvePath(info->texture)), name);
        std::uint64_t salt = hashString(colorSetSalt(ctx, name), name);
        if (!info && isImagePath(name)) salt = hashCombine(salt, fileStamp(resolvePath(name)));
        return salt;
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    // A brush dab reshapes a few guides: only the strands bound to them (and, when guide roots
    // moved, those whose guides or weights changed) are blended again, into a copy of the last
    // result. Each guide is told from the one the last result was made with, to the bit.
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

    // The binding and what the last result was made from, from the same node in another copy of
    // the graph (each names the results it serves; see Operator::adoptKeptWork).
    void adoptKeptWork(const Operator& previous) override {
        const auto* from = dynamic_cast<const InterpolateOp*>(&previous);
        if (!from || from == this) return;
        Made made;
        decltype(memo_) memo;
        {
            const std::lock_guard<std::mutex> lock(from->madeMutex_);
            made = from->made_;
        }
        {
            const std::lock_guard<std::mutex> lock(from->memoMutex_);
            memo = from->memo_;
        }
        {
            const std::lock_guard<std::mutex> lock(madeMutex_);
            if (!made_.binding) made_ = std::move(made);
        }
        const std::lock_guard<std::mutex> lock(memoMutex_);
        if (memo_.empty()) memo_ = std::move(memo);
    }

    // What the last result was made from, when it is `output` (operator_state.h).
    bool state(const StrandsPtr& output, InterpolateState* state) const {
        Made made;
        {
            const std::lock_guard<std::mutex> lock(madeMutex_);
            made = made_;
        }
        if (!output || !sameOrShown(made.output.lock(), output) || !made.binding || !made.guides) return false;
        state->binding = made.binding;
        state->baseKey = made.binding->baseKey;
        state->strandCount = made.binding->strandCount;
        state->roots = made.binding->roots;
        state->guides = made.guides;
        state->strands = made.strands.lock();
        state->channels = !made.attrNames.empty() || !made.layerNames.empty();
        return true;
    }

    // `state`'s strands bound again to guides rooted at `roots` (operator_state.h): as run()
    // binds them, from the same strands, params and parts (the binding's base key), the held
    // binding passed as the previous one; kept in the memo as run() keeps its own.
    bool rebind(const PartLines* groomParts, const std::vector<Vec3f>& roots, const std::vector<StrandRoot>& guideRoots,
                InterpolateState* state) const {
        const auto* held = static_cast<const Binding*>(state->binding.get());
        if (!held || roots.size() != held->roots.size() || roots.empty() || guideRoots.size() != roots.size()) return false;
        BindParams b = bindParams(roots.size(), groomParts, held->fronts != nullptr);
        const std::uint64_t key = hashPoints(held->baseKey, roots);
        std::shared_ptr<const Binding> previous;
        std::shared_ptr<const Binding> binding = findBinding(key, held->baseKey, &previous);
        if (!binding) {
            // With regions, the guides' at their roots now; a strand changing region (a guide
            // rooted in a color none was) is the CPU's to evaluate (its region channel changes).
            std::shared_ptr<const RegionSplit> regions;
            if (held->regionSource) {
                regions = std::make_shared<const RegionSplit>(splitRegions(*held->regionSource, guideRoots, held->regions.get()));
                if (!held->regions || !sameStrandRegions(*regions, *held->regions)) return false;
            }
            binding = bind(held->queries, held->hasPoints, held->strandRoots, roots, guideRoots,
                           held->fronts ? held->fronts->graphPointer() : nullptr, b, held->baseKey, held, held->regionSource, regions);
            keepBinding(key, binding);
        }
        state->binding = binding;
        state->roots = roots;
        return true;
    }

private:
    // What a binding is made with besides the strands and guide roots, from the params.
    struct BindParams {
        std::size_t requested = 0;  // the neighbors param
        std::size_t neighborCount = 0, candidateCount = 0;
        bool nearestOnly = false;
        float reach = 1.5f, power = 2.0f;
        bool followSurface = false;  // (on a scalp)
        float radius = 0.0f;         // (with followSurface: the paths reach that far at least)
        const PartLines* parts = nullptr;
    };
    BindParams bindParams(std::size_t guideCount, const PartLines* groomParts, bool onScalp) const {
        BindParams b;
        b.requested = static_cast<std::size_t>(params().getInt("neighbors"));
        b.power = static_cast<float>(params().getDouble("power"));
        b.reach = static_cast<float>(params().getDouble("reach"));
        b.parts = params().getBool("respectParts") ? groomParts : nullptr;
        b.nearestOnly = b.requested == 1;
        b.followSurface = onScalp && params().getBool("followSurface");
        if (b.followSurface) b.radius = static_cast<float>(params().getDouble("limitDistance"));
        return sizedFor(b, guideCount);
    }
    // `b` for a blend among `guideCount` guides (all of them, or a region's).
    static BindParams sizedFor(BindParams b, std::size_t guideCount) {
        b.neighborCount = std::min(b.requested, guideCount);
        // Candidates per strand: the guides whose reach may cover it. Their reaches are from the
        // local spacing, so a few rings of neighbours suffice. The nearest alone needs one, unless
        // parts or the skin may rule it out.
        b.candidateCount = b.nearestOnly && !b.parts && !b.followSurface
                               ? 1
                               : std::min(guideCount, std::max<std::size_t>(b.neighborCount * 4 + 2, 8));
        return b;
    }

    // The kept guides' search roots, root normals, offset rows (`stride` floats a row) and lengths at
    // `samples` samples.
    struct GuideRows {
        std::size_t samples = 0;
        std::vector<Vec3f> roots, normals;
        std::vector<float> offsets, lengths;
    };
    // What the last result was made from (evaluateIncremental): the result and its strands (by
    // identity, not kept alive), the guides (the kept ones in order), the binding, the guide
    // attribute and layer names, the scalp's surface and the binding's base key.
    struct Made {
        std::weak_ptr<const StrandSet> output, strands;
        GuidesPtr guides;
        std::vector<const Guide*> kept;  // into `guides`
        std::shared_ptr<const Binding> binding;
        std::vector<std::string> attrNames, layerNames;
        std::uint64_t scalp = 0;
        std::uint64_t baseKey = 0;
        std::shared_ptr<const GuideRows> rows;  // the kept guides' as made
        // regionMap: what the regions were read from (the strands' colors, kept while the strands
        // and the map are) and the regions.
        std::shared_ptr<const RegionSource> regionSource;
        std::shared_ptr<const RegionSplit> regions;
    };

    // In full, or (with `incremental`) only the strands a change of the guides reaches, into the
    // last result's copy; false when that cannot be done.
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr strands = valueAs<StrandsPtr>(inputs.at(0));
        if (!strands) throw GraphError("Interpolate: strands input not connected");
        const GuidesPtr guides = valueAs<GuidesPtr>(inputs.at(1));
        // With a limitDistance, the influence channel is always there (0 without guides), so
        // params reading it (a Clump's strength, say) work before any guide is placed.
        auto untouched = [&]() -> Value {
            const std::string name = params().getString("influenceChannel");
            if (params().getDouble("limitDistance") <= 0.0 || name.empty()) return strands;
            auto out = std::make_shared<StrandSet>(*strands);
            out->addStrandChannel(name, 0.0f);
            return StrandsPtr(out);
        };
        if (!guides || guides->empty()) {
            if (incremental) return false;
            ctx.note("no guides: place some with the guide tools; the strands keep their incoming shape");
            forget();
            *result = untouched();
            return true;
        }

        // fillTubes: each strand's tube is worked out anew from the guides (a dab evaluates in full).
        const bool fillTubes = fillsTubes();
        if (incremental && fillTubes) return false;
        const float radius = static_cast<float>(params().getDouble("limitDistance"));
        const Ramp falloff = params().getRamp("falloff");
        const std::string take = params().getString("take");
        if (take != "shape" && take != "length") throw GraphError("Interpolate: take must be shape or length");
        const bool lengthOnly = take == "length";
        const std::string influenceName = params().getString("influenceChannel");
        const std::string regionMapName = params().getString("regionMap");
        const std::string regionName = params().getString("regionChannel");

        // Normal alignment needs the scalp and rooted strands and guides.
        // Roots index the whole mesh: a scalp's piece (scalp_pieces.h) reads as the groom's scalps together.
        const MeshPtr scalp = wholeScalp(valueAs<MeshPtr>(inputs.at(2)));
        const bool align = scalp && scalp->triangleCount() > 0;
        // The region map, read on the scalp (checked before anything is made).
        const std::shared_ptr<const RegionMap> regionMap =
            regionMapName.empty() ? nullptr : std::make_shared<const RegionMap>(ctx, regionMapName, scalp);

        // Guides at a common resolution: at least the strands' own (the core strands', preview_core.h:
        // the same at every preview density), so detail survives.
        const std::size_t samples = interpolateSamples(*strands);

        // Guide roots, root normals and offset curves. The offsets are held as rows, x then y then z
        // (`stride` floats each: the samples rounded up to four, the rest 0), guide g's at
        // [3 g stride, 3 (g + 1) stride), so the blend reads four samples at a time. The guides
        // kept (2 points or more) are each made on their own, side by side.
        const std::size_t stride = (samples + 3) & ~std::size_t{3};
        std::vector<const Guide*> kept;
        for (const Guide& g : guides->guides()) {
            if (g.points.size() >= 2) kept.push_back(&g);
        }
        if (kept.empty()) {
            if (incremental) return false;
            forget();
            *result = untouched();
            return true;
        }
        // Incrementally: the last result, when it was made here from these strands on this scalp
        // with the same guides kept (their IDs in order). Its params, parts and context are these
        // (the graph asks only when the node's own key is the last result's).
        const std::uint64_t scalpKey = scalp ? scalp->surfaceHash() : 0;
        Made made;
        StrandsPtr last;
        if (incremental) {
            {
                const std::lock_guard<std::mutex> lock(madeMutex_);
                made = made_;
            }
            last = previousStrandsFor(*incremental, *strands);
            const bool same = last && made.output.lock() == last && made.strands.lock() == strands && made.scalp == scalpKey &&
                              made.kept.size() == kept.size() && made.binding;
            if (!same) return false;
            for (std::size_t g = 0; g < kept.size(); ++g) {
                if (kept[g]->id != made.kept[g]->id) return false;  // guides added, erased or reordered
            }
        }
        // Incrementally, the guides that changed (the others take their rows as made).
        std::vector<char> guideChanged;
        const GuideRows* before = last && made.rows && made.rows->samples == samples ? made.rows.get() : nullptr;
        if (last) {
            guideChanged.resize(kept.size());
            parallelFor(0, kept.size(), [&](std::size_t g) { guideChanged[g] = sameGuide(*kept[g], *made.kept[g]) ? 0 : 1; });
        }
        auto rowsMade = std::make_shared<GuideRows>();
        rowsMade->samples = samples;
        std::vector<Vec3f>& roots = rowsMade->roots;      // search positions: on the rest scalp when it has one
        std::vector<Vec3f>& normals = rowsMade->normals;  // zero when a guide is not rooted on the scalp
        std::vector<float>& offsets = rowsMade->offsets;
        std::vector<float>& guideLengths = rowsMade->lengths;
        roots.resize(kept.size());
        normals.resize(kept.size());
        offsets.assign(kept.size() * 3 * stride, 0.0f);
        guideLengths.resize(kept.size());
        const bool smoothGuides = params().getBool("smoothGuides");
        // keepClearance: the scalp's field.
        const Clearance clearance = align && !lengthOnly && params().getBool("keepClearance") ? clearanceOf(scalp) : Clearance{};
        // fillTubes: each kept guide's tube laid along its shape as the blend reads it.
        std::unique_ptr<TubeFill> fill;
        const std::vector<Guide>* levelTubes = fillTubes && guides->levelTubes() ? guides->levelTubes().get() : nullptr;
        if (fillTubes) {
            const std::size_t tubes = kept.size() + (levelTubes ? levelTubes->size() : 0);
            fill = std::make_unique<TubeFill>();
            fill->spines.resize(tubes);
            fill->numbers.assign(tubes, -1.0f);
            fill->depths.resize(tubes);
            fill->ids.resize(tubes);
            for (std::size_t k = 0; k < tubes; ++k) {
                const Guide& g = k < kept.size() ? *kept[k] : (*levelTubes)[k - kept.size()];
                fill->depths[k] = g.depth;
                fill->ids[k] = g.id;
            }
            if (levelTubes) {  // the other levels' tubes, along their guides as the kept ones' below
                parallelFor(0, levelTubes->size(), [&](std::size_t i) {
                    const Guide& g = (*levelTubes)[i];
                    if (!g.hasTube || samples < 2) return;
                    thread_local std::vector<Vec3f> resampled;
                    guideShape(g, samples, smoothGuides, resampled);
                    TubeSpine& spine = fill->spines[kept.size() + i];
                    spine = TubeSpine(resampled.data(), samples, g.frameUp, g.tube);
                    if (align) spine.setRootSurface(scalpNormalAt(*scalp, g.root, resampled.front()));
                }, 8);
            }
        }
        parallelFor(0, kept.size(), [&](std::size_t k) {
            const Guide& g = *kept[k];
            if (before && !guideChanged[k]) {  // (no tubes: a fill evaluates in full)
                roots[k] = before->roots[k];
                normals[k] = before->normals[k];
                guideLengths[k] = before->lengths[k];
                std::copy_n(before->offsets.begin() + static_cast<std::ptrdiff_t>(k * 3 * stride), 3 * stride,
                            offsets.begin() + static_cast<std::ptrdiff_t>(k * 3 * stride));
                return;
            }
            thread_local std::vector<Vec3f> resampled;
            guideLengths[k] = guideShape(g, samples, smoothGuides, resampled);
            roots[k] = restRootOr(align ? scalp.get() : nullptr, g.root, resampled.front());
            normals[k] = align && scalp->contains(g.root) ? scalp->normal(g.root) : Vec3f{};
            if (fill && g.hasTube && samples >= 2) {
                // The tube along the guide, its root footprint laid on the scalp.
                fill->spines[k] = TubeSpine(resampled.data(), samples, g.frameUp, g.tube);
                fill->spines[k].setRootSurface(align ? scalpNormalAt(*scalp, g.root, resampled.front()) : Vec3f{});
            }
            float* rows = offsets.data() + k * 3 * stride;
            for (std::size_t j = 0; j < samples; ++j) {
                const Vec3f o = resampled[j] - resampled.front();
                rows[j] = o.x;
                rows[stride + j] = o.y;
                rows[2 * stride + j] = o.z;
            }
        }, 8);
        std::vector<char> guideTurns(normals.size());  // a guide whose offsets turn to the strand's normal
        for (std::size_t g = 0; g < normals.size(); ++g) guideTurns[g] = normals[g].length() == 0.0f ? 0 : 1;

        // Guide attributes (GH-7), one row per name over the guides kept above.
        std::vector<std::string> attrNames;
        for (const Guide& g : guides->guides()) {
            for (const auto& [name, value] : g.attributes) {
                if (std::find(attrNames.begin(), attrNames.end(), name) == attrNames.end()) attrNames.push_back(name);
            }
        }
        std::vector<std::vector<float>> attrValues(attrNames.size());
        for (const Guide* guide : kept) {
            const Guide& g = *guide;
            for (std::size_t a = 0; a < attrNames.size(); ++a) {
                auto it = g.attributes.find(attrNames[a]);
                attrValues[a].push_back(it == g.attributes.end() ? 0.0f : it->second);
            }
        }
        // Per-point layers, one row per name: each guide's layer at the common
        // resolution, guide g in [g * samples, (g + 1) * samples). A guide
        // without the layer carries its scalar attribute of that name, else 0.
        std::vector<std::string> layerNames;
        for (const Guide& g : guides->guides()) {
            for (const auto& [name, values] : g.pointAttributes) {
                if (!values.empty() && std::find(layerNames.begin(), layerNames.end(), name) == layerNames.end()) {
                    layerNames.push_back(name);
                }
            }
        }
        std::vector<std::vector<float>> layerValues(layerNames.size(), std::vector<float>(kept.size() * samples));
        if (!layerNames.empty()) {
            parallelFor(0, kept.size(), [&](std::size_t k) {
                const Guide& g = *kept[k];
                for (std::size_t a = 0; a < layerNames.size(); ++a) {
                    float* row = layerValues[a].data() + k * samples;
                    auto it = g.pointAttributes.find(layerNames[a]);
                    if (it != g.pointAttributes.end() && !it->second.empty()) {
                        // Evenly by arc length along the guide, as its offsets are.
                        resampleValuesByArcLength(g.points.data(), g.points.size(), it->second.data(),
                                                  it->second.size(), row, samples);
                    } else {
                        auto scalar = g.attributes.find(layerNames[a]);
                        std::fill(row, row + samples, scalar == g.attributes.end() ? 0.0f : scalar->second);
                    }
                }
            }, 8);
        }
        if (last && (attrNames != made.attrNames || layerNames != made.layerNames)) return false;
        // Parts: strands skip guides across a part.
        const PartsPtr parts = params().getBool("respectParts") ? valueAs<PartsPtr>(ctx.external("parts")) : nullptr;
        const BindParams b = bindParams(roots.size(), parts.get(), align);

        // Where each strand looks for its guides: its root on the rest scalp when there is one.
        const std::size_t strandCount = strands->strandCount();
        std::shared_ptr<const std::vector<Vec3f>> queries;
        std::shared_ptr<const std::vector<char>> hasPoints;
        std::shared_ptr<const std::vector<StrandRoot>> strandRoots;
        auto findQueries = [&] {
            auto where = std::make_shared<std::vector<Vec3f>>(strandCount);
            auto any = std::make_shared<std::vector<char>>(strandCount);
            const PreviewCore core(*strands);
            parallelFor(0, strandCount, [&](std::size_t s) {
                (*any)[s] = strands->strandPointCount(s) > 0 ? (core.core(s) ? 2 : 1) : 0;
                (*where)[s] = (*any)[s] ? restRootOr(align ? scalp.get() : nullptr, strands->roots()[s], strands->strandPoints(s)[0])
                                        : Vec3f{};
            });
            queries = std::move(where);
            hasPoints = std::move(any);
        };
        std::vector<StrandRoot> guideRoots(kept.size());
        for (std::size_t g = 0; g < kept.size(); ++g) guideRoots[g] = kept[g]->root;
        // Regions: the strands' colors on the map (the last result's while the strands and the map
        // are the same), the guides' at their roots now, and from those the regions. Incrementally
        // every strand keeps its region (else it is evaluated in full: its region channel changes).
        std::shared_ptr<const RegionSource> regionSource;
        std::shared_ptr<const RegionSplit> regions;
        if (regionMap) {
            const std::uint64_t salt = regionSalt(ctx, regionMapName);
            std::shared_ptr<const RegionSource> heldSource;
            std::shared_ptr<const RegionSplit> heldRegions;
            {
                const std::lock_guard<std::mutex> lock(madeMutex_);
                if (made_.regionSource && made_.strands.lock() == strands && made_.regionSource->name == regionMapName &&
                    made_.regionSource->salt == salt) {
                    heldSource = made_.regionSource;
                    heldRegions = made_.regions;
                }
            }
            if (heldSource) {
                regionSource = heldSource;
            } else {
                if (last) return false;
                auto colors = std::make_shared<std::vector<Vec3f>>(strandCount);
                parallelFor(0, strandCount, [&](std::size_t s) {
                    const float none = std::numeric_limits<float>::quiet_NaN();
                    Vec3f c(none, none, none);
                    regionMap->at(strands->roots()[s], &c);
                    (*colors)[s] = c;
                }, 4096);
                auto source = std::make_shared<RegionSource>();
                source->map = regionMap;
                source->strandColors = std::move(colors);
                source->name = regionMapName;
                source->salt = salt;
                regionSource = std::move(source);
            }
            regions = std::make_shared<const RegionSplit>(splitRegions(*regionSource, guideRoots, heldRegions.get()));
            if (last && (!made.regions || !sameStrandRegions(*regions, *made.regions))) return false;
        }
        // The binding is kept while everything it is made from stays (a brush dab changes the
        // guides' shapes only): the strands' and guides' roots, the parts, the scalp's rest surface
        // and the params it reads. When only guide roots moved, the strands they cannot reach keep
        // theirs. Incrementally, the strands, scalp, parts and params are the last result's: so is
        // the base key.
        std::uint64_t baseKey = made.baseKey;
        if (!last) {
            findQueries();
            baseKey = hashPoints(hash64(0xB1AD), *queries);
            for (std::size_t s = 0; s < strandCount; ++s) {  // strands without points (rare)
                if (!(*hasPoints)[s]) baseKey = hashCombine(baseKey, s + 0xE0u);
            }
            baseKey = hashCombine(hashCombine(hashCombine(baseKey, b.neighborCount), b.nearestOnly), b.candidateCount);
            baseKey = hashFloat(hashFloat(baseKey, b.reach), b.power);
            baseKey = hashCombine(hashCombine(baseKey, parts ? parts->sideHash() : 0), roots.size());
            if (align) baseKey = hashFloat(hashCombine(hashCombine(baseKey, scalp->restSurfaceHash()), b.followSurface), b.radius);
            if (regionSource) baseKey = hashPoints(hashCombine(baseKey, regionSource->salt), *regionSource->strandColors);
        }
        const std::uint64_t key = hashPoints(baseKey, roots);
        std::shared_ptr<const Binding> previous;
        std::shared_ptr<const Binding> binding = findBinding(key, baseKey, &previous);
        if (!binding) {
            if (!queries) {
                // The last result's strands: their search positions are the last binding's.
                if (last && made.binding->queries && made.binding->queries->size() == strandCount) {
                    queries = made.binding->queries;
                    hasPoints = made.binding->hasPoints;
                    strandRoots = made.binding->strandRoots;
                } else {
                    findQueries();
                }
            }
            if (align && !strandRoots) strandRoots = std::make_shared<const std::vector<StrandRoot>>(strands->roots());
            binding = bind(queries, hasPoints, strandRoots, roots, guideRoots, align ? sharedSurfaceGraph(*scalp) : nullptr, b,
                           baseKey, previous.get(), regionSource, regions);
            keepBinding(key, binding);
        }
        const Binding& bindings = *binding;
        if (fill) findTubes(*strands, *fill, std::clamp(static_cast<float>(params().getDouble("tubeBlend")), 0.0f, 1.0f));

        // Incrementally, the strands to blend again: those bound to a guide that changed, and
        // (another binding: guide roots moved) those whose guides or weights did.
        std::vector<std::uint32_t> dirty;
        if (last) {
            const Binding& boundBefore = *made.binding;
            const std::size_t chunks = bindings.chunks.size();
            std::vector<std::vector<std::uint32_t>> found(chunks);
            parallelFor(0, chunks, [&](std::size_t c) {
                // A chunk bound again (guide roots moved near it): its strands whose records changed too.
                const bool rebound = c >= boundBefore.chunks.size() || boundBefore.chunks[c] != bindings.chunks[c];
                const std::size_t end = std::min(strandCount, (c + 1) * kBindChunk);
                for (std::size_t s = c * kBindChunk; s < end; ++s) {
                    bool again = rebound && !sameRecord(boundBefore, bindings, s);
                    const Binding::Record r = bindings.record(s);
                    if (!again && r.bound) {
                        for (std::uint32_t i = 0; i < r.count && !again; ++i) again = guideChanged[r.guide(i)] != 0;
                    }
                    if (again) found[c].push_back(static_cast<std::uint32_t>(s));
                }
            }, 1);
            for (const auto& list : found) dirty.insert(dirty.end(), list.begin(), list.end());
            if (dirty.empty()) {  // nothing reaches a strand: the last result as it is
                remember(last, strands, guides, kept, binding, attrNames, layerNames, scalpKey, baseKey,
                         regionSource, regions, rowsMade);
                *result = last;
                *changed = StrandChanges::listed({});
                return true;
            }
        }

        auto out = std::make_shared<StrandSet>(*strands);
        // The influence varies only with a radius (else it is 1 everywhere): only then is it written.
        const bool influenceWritten = !influenceName.empty() && radius > 0.0f;
        std::vector<std::vector<float>*> attrChannels;
        std::vector<std::vector<float>*> layerChannels;
        std::vector<float>* influenceChannel = nullptr;
        // With a region map, each strand's region (written last: it wins over a guide attribute of its name).
        const bool regionWritten = regionSource && !regionName.empty();
        if (last) {
            // The strands left alone as the last result made them: its points and channels, copied
            // (every strand's region as it was).
            if (influenceWritten && std::find(attrNames.begin(), attrNames.end(), influenceName) != attrNames.end()) return false;
            if (regionWritten && (std::find(attrNames.begin(), attrNames.end(), regionName) != attrNames.end() ||
                                  !out->copyStrandChannelFrom(regionName, *last))) {
                return false;
            }
            positionsOfLast(*out, last, *incremental);
            for (const std::string& name : attrNames) attrChannels.push_back(out->copyStrandChannelFrom(name, *last));
            for (const std::string& name : layerNames) layerChannels.push_back(out->copyPointChannelFrom(name, *last));
            if (influenceWritten) influenceChannel = out->copyStrandChannelFrom(influenceName, *last);
            for (const auto* channel : attrChannels) {
                if (!channel) return false;
            }
            for (const auto* channel : layerChannels) {
                if (!channel) return false;
            }
            if (influenceWritten && !influenceChannel) return false;
        } else {
            // (Every strand's points written by the stage, StrandSet::overwritePositions.)
            for (const std::string& name : attrNames) attrChannels.push_back(&out->addStrandChannel(name, 0.0f));
            for (const std::string& name : layerNames) layerChannels.push_back(&out->addPointChannel(name, 0.0f));
            if (influenceWritten) influenceChannel = &out->addStrandChannel(influenceName, 0.0f);
        }

        // How much the guides take a strand nearest2 from its nearest guide: all of it, or by distance
        // within the radius (the ramp's value, brought to 0 at the radius whatever its last key, over
        // its last twentieth: a ramp ending above 0 would pop there).
        auto influenceAt = [&](float nearest2) {
            if (!(radius > 0.0f)) return 1.0f;
            const float t = std::sqrt(nearest2) / radius;
            return t >= 1.0f ? 0.0f : std::clamp(falloff(t), 0.0f, 1.0f) * std::min((1.0f - t) * 20.0f, 1.0f);
        };
        // look.slang's InterpolateCS (and ClearanceCS keeping clearance), the GPU preview's own
        // (look_cpu.h), on this binding and these guides: its inputs for every strand, or (`which`)
        // for those strands alone (a view of them). The channels here.
        // A bound strand's layers: each blended over its guides like the shape, then read at each of
        // its points, once the shape is final, by where it sits along the strand (arc length).
        auto blendLayers = [&](std::size_t s, Scratch& scratch) {
            if (layerChannels.empty()) return;
            const Binding::Record record = bindings.record(s);
            const std::size_t n = out->strandPointCount(s);
            std::vector<float>& blendedLayer = scratch.blendedLayer;
            std::vector<float>& strandArc = scratch.strandArc;
            const Vec3f* shaped = static_cast<const StrandSet&>(*out).strandPoints(s);
            strandArc.resize(n);
            arcParams(shaped, n, strandArc.data());
            blendedLayer.resize(samples);
            const std::size_t begin = out->strandBegin(s);
            for (std::size_t a = 0; a < layerChannels.size(); ++a) {
                std::fill(blendedLayer.begin(), blendedLayer.end(), 0.0f);
                for (std::uint32_t i = 0; i < record.count; ++i) {
                    const float* row = layerValues[a].data() + record.guide(i) * samples;
                    const float wi = record.weight(i);
                    for (std::size_t k = 0; k < samples; ++k) blendedLayer[k] += row[k] * wi;
                }
                float* channel = layerChannels[a]->data() + begin;
                for (std::size_t j = 0; j < n; ++j) {
                    const float x = strandArc[j] * static_cast<float>(samples - 1);
                    const std::size_t k = std::min(static_cast<std::size_t>(x), samples - 2);
                    const float f = x - static_cast<float>(k);
                    channel[j] = blendedLayer[k] * (1.0f - f) + blendedLayer[k + 1] * f;
                }
            }
        };
        auto engineInputs = [&](const std::vector<std::uint32_t>* which, gpu::StageInputs* blend) {
            const std::size_t count = which ? which->size() : strandCount;
            auto strandAt = [&](std::size_t k) { return which ? static_cast<std::size_t>((*which)[k]) : k; };
            std::shared_ptr<const InterpolateClearance> field;
            std::shared_ptr<const std::vector<std::uint32_t>> cursors;
            if (clearance) {
                std::string whyNot;
                field = clearanceInputs(clearance, scalp, &whyNot, true);
                if (!field) throw GraphError("Interpolate: " + whyNot);
                if (field->rest) {  // over a deformed scalp: each strand's walk's first vertex
                    float longest = 0.0f;
                    for (const float length : guideLengths) longest = std::max(longest, length);
                    cursors = interpolateRestCursors(*field, *strands, longest, &whyNot, true);
                    if (!cursors) throw GraphError("Interpolate: " + whyNot);
                    if (which) {
                        auto listed = std::make_shared<std::vector<std::uint32_t>>(count);
                        for (std::size_t k = 0; k < count; ++k) (*listed)[k] = (*cursors)[strandAt(k)];
                        cursors = std::move(listed);
                    }
                }
            }
            auto data = std::make_shared<gpu::InterpolateInputs>();
            {
                const std::lock_guard<std::mutex> lock(madeMutex_);
                // (The binding alone: kept while it is, whatever strands and guides come with it,
                // so its records stay uploaded while the strands deform.)
                if (!engineState_ || engineState_->binding != binding || engineState_->strandCount != strandCount) {
                    auto state = std::make_shared<InterpolateState>();
                    state->binding = binding;
                    state->baseKey = baseKey;
                    state->strandCount = strandCount;
                    state->roots = roots;
                    state->channels = !attrNames.empty();
                    engineState_ = std::move(state);
                }
                data->binding = which ? interpolateStateOf(*engineState_, *which) : engineState_;
            }
            auto rows = std::make_shared<InterpolateGuideRows>();
            rows->samples = samples;
            rows->roots = roots;
            rows->normals = normals;
            rows->lengths = guideLengths;
            rows->offsets.resize(kept.size() * samples);
            for (std::size_t g = 0; g < kept.size(); ++g) {
                const float* o = offsets.data() + g * 3 * stride;
                for (std::size_t j = 0; j < samples; ++j) rows->offsets[g * samples + j] = Vec3f(o[j], o[stride + j], o[2 * stride + j]);
            }
            data->rows = std::move(rows);
            auto strandNormals = std::make_shared<std::vector<Vec3f>>(count);
            parallelFor(0, count, [&](std::size_t k) {
                const StrandRoot& root = strands->roots()[strandAt(k)];
                (*strandNormals)[k] = align && scalp->contains(root) ? scalp->normal(root) : Vec3f{};
            }, 4096);
            data->strandNormals = std::move(strandNormals);
            data->clearance = std::move(field);
            data->cursors = std::move(cursors);
            if (fill) {
                // The tubes and each strand's as the kernel reads them (look_run.h InterpolateInputs::tubes).
                const std::size_t tubes = fill->spines.size(), perTube = 1 + 9 * samples;
                auto words = std::make_shared<std::vector<std::uint32_t>>(2 + tubes * perTube + count * 8, 0u);
                auto put = [&](std::size_t at, float v) { std::memcpy(words->data() + at, &v, 4); };
                (*words)[0] = static_cast<std::uint32_t>(samples);
                (*words)[1] = static_cast<std::uint32_t>(tubes);
                for (std::size_t t = 0; t < tubes; ++t) {
                    const TubeSpine& spine = fill->spines[t];
                    const std::size_t at = 2 + t * perTube;
                    put(at, spine.empty() ? 1.0f : spine.rootJoin());
                    for (std::size_t i = 0; i < spine.samples() && i < samples; ++i) {
                        const Vec3f parts[3] = {spine.center(i), spine.axisU(i), spine.axisV(i)};
                        for (int a = 0; a < 3; ++a) {
                            put(at + 1 + i * 9 + a * 3, parts[a].x);
                            put(at + 1 + i * 9 + a * 3 + 1, parts[a].y);
                            put(at + 1 + i * 9 + a * 3 + 2, parts[a].z);
                        }
                    }
                }
                parallelFor(0, count, [&](std::size_t k) {
                    const std::size_t s = strandAt(k), at = 2 + tubes * perTube + k * 8;
                    std::memcpy(words->data() + at, &fill->tubeOf[s], 4);
                    put(at + 1, fill->at[s].u);
                    put(at + 2, fill->at[s].v);
                    std::memcpy(words->data() + at + 3, &fill->edgeOf[s], 4);
                    put(at + 4, fill->edgeAt[s].u);
                    put(at + 5, fill->edgeAt[s].v);
                    put(at + 6, fill->edgeWeight[s]);
                }, 4096);
                data->tubes = std::move(words);
            }
            blend->interpolate = std::move(data);
            return true;
        };
        // A bound strand's channels from its guides.
        auto blendChannels = [&](std::size_t s) {
            const Binding::Record record = bindings.record(s);
            if (!record.bound) return;
            for (std::size_t a = 0; a < attrChannels.size(); ++a) {
                float v = 0.0f;
                for (std::uint32_t i = 0; i < record.count; ++i) v += attrValues[a][record.guide(i)] * record.weight(i);
                (*attrChannels[a])[s] = v;
            }
            if (influenceChannel) (*influenceChannel)[s] = fill && fill->tubeOf[s] >= 0 ? 1.0f : influenceAt(record.nearest2);
        };
        // look.slang's InterpolateCS (keeping clearance: ClearanceCS after it), the GPU preview's own
        // (look_cpu.h): in full, or the strands to blend again on a view of them (each one's
        // channels from their defaults, as a fresh output's); the channels, layers, regions and tubes
        // written here.
        gpu::StageInputs blend;
        engineInputs(last ? &dirty : nullptr, &blend);
        gpu::StageParamReader reader(params(), "");
        reader.samples = samples;
        reader.lengthAlone = lengthOnly;
        reader.tubes = true;  // (its tubes handed in, StageInputs::interpolate; none filled: none)
        if (!last) {
            gpu::runStageInto(gpu::StageKind::Interpolate, reader, strands, scalp.get(), *out, std::move(blend));
            parallelFor(0, strandCount, blendChannels, 1024);
            if (!layerChannels.empty()) {
                parallelFor(0, strandCount, [&](std::size_t s) {
                    thread_local Scratch scratch;
                    if (bindings.record(s).bound) blendLayers(s, scratch);
                }, 256);
            }
            if (regionWritten) {
                std::vector<float>& channel = out->addStrandChannel(regionName, -1.0f);
                const std::vector<std::int32_t>& strandRegions = *regions->strands;
                parallelFor(0, channel.size(), [&](std::size_t s) { channel[s] = static_cast<float>(strandRegions[s]); }, 4096);
            }
            if (fill) writeTubeChannels(*out, *fill);
        } else {
            gpu::runStageOnStrands(gpu::StageKind::Interpolate, reader, strands, dirty, scalp.get(), *out, std::move(blend));
            forEachListed(dirty, [&](std::size_t s) {
                thread_local Scratch scratch;
                for (std::vector<float>* channel : attrChannels) (*channel)[s] = 0.0f;
                const std::size_t begin = out->strandBegin(s), n = out->strandPointCount(s);
                for (std::vector<float>* channel : layerChannels) std::fill(channel->begin() + begin, channel->begin() + begin + n, 0.0f);
                if (influenceChannel) (*influenceChannel)[s] = 0.0f;
                blendChannels(s);
                if (bindings.record(s).bound) blendLayers(s, scratch);
            });
            *changed = StrandChanges::listed(std::move(dirty));
        }
        const StrandsPtr blended = out;
        remember(blended, strands, guides, kept, binding, attrNames, layerNames, scalpKey, baseKey,
                 regionSource, regions, rowsMade);
        *result = blended;
        return true;
    }

    // fillTubes' channels, written last: each strand's tube wins over a guide attribute of its name.
    static void writeTubeChannels(StrandSet& out, const TubeFill& fill) {
        std::vector<float>& ids = out.addStrandChannel("tubeId", -1.0f);
        std::vector<float>& us = out.addStrandChannel("tubeU", 0.0f);
        std::vector<float>& vs = out.addStrandChannel("tubeV", 0.0f);
        parallelFor(0, ids.size(), [&](std::size_t s) {
            const std::int32_t t = fill.tubeOf[s];
            if (t < 0) return;
            ids[s] = fill.numbers[static_cast<std::size_t>(t)];
            us[s] = fill.at[s].u;
            vs[s] = fill.at[s].v;
        }, 4096);
    }

    // fillTubes: each strand's tube (TubeFill::tubeOf, at) from its root (its first point), among
    // the tubes laid out (`fill.spines`), the soft edge it lies in (edgeOf, edgeAt, edgeWeight;
    // `blend`: tubeBlend), and each tube's number. The strands each on their own, side by side; of
    // the tubes holding a root the deepest wins, then the most central, then the first; of the
    // deeper tubes whose edge holds it the nearest, then the deepest, then the first: the same
    // whichever threads ran.
    static void findTubes(const StrandSet& strands, TubeFill& fill, float blend) {
        const std::size_t count = strands.strandCount();
        fill.tubeOf.assign(count, -1);
        fill.at.assign(count, SectionPoint{});
        fill.edgeOf.assign(count, -1);
        fill.edgeAt.assign(count, SectionPoint{});
        fill.edgeWeight.assign(count, 0.0f);
        std::vector<std::uint32_t> tubes;
        std::vector<Vec3f> centers;
        std::vector<std::uint64_t> ids;
        float cell = 0.0f;
        for (std::size_t k = 0; k < fill.spines.size(); ++k) {
            const TubeSpine& spine = fill.spines[k];
            if (spine.empty() || !(spine.rootWidth() > 0.0f) || !(spine.rootHeight() > 0.0f)) continue;
            const float extent = std::max(spine.rootWidth(), spine.rootHeight());
            // Reaches across by the extent and its soft edge, along by kTubeAlongReach of it.
            const float across = 1.0f + blend;
            cell = std::max(cell, extent * std::sqrt(across * across + kTubeAlongReach * kTubeAlongReach));
            tubes.push_back(static_cast<std::uint32_t>(k));
            centers.push_back(spine.atSample(0, 0.0f, 0.0f));
            ids.push_back(fill.ids[k]);
        }
        if (tubes.empty() || !(cell > 0.0f) || !std::isfinite(cell)) return;
        const std::vector<float> numbers = stableGroupNumbers(ids);
        for (std::size_t i = 0; i < tubes.size(); ++i) fill.numbers[tubes[i]] = numbers[i];
        const CellGrid grid(centers, cell);
        parallelFor(0, count, [&](std::size_t s) {
            if (strands.strandPointCount(s) < 2) return;
            const Vec3f root = strands.strandPoints(s)[0];
            if (!std::isfinite(root.x) || !std::isfinite(root.y) || !std::isfinite(root.z)) return;
            std::int32_t best = -1;
            float bestDepth = 0.0f;
            SectionPoint bestAt;
            // The tubes holding the root.
            grid.around(root, [&](std::uint32_t i) {
                const std::uint32_t k = tubes[i];
                const TubeSpine& spine = fill.spines[k];
                float along = 0.0f;
                const SectionPoint at = spine.rootCoords(root, &along);
                if (std::abs(along) > kTubeAlongReach * std::max(spine.rootWidth(), spine.rootHeight())) return true;
                const float depth = tubeShapeDepth(spine.tube(), at.u, at.v);
                if (!(depth >= 0.0f)) return true;
                const int level = fill.depths[k];
                const int bestLevel = best < 0 ? -1 : fill.depths[static_cast<std::size_t>(best)];
                if (best < 0 || level > bestLevel ||
                    (level == bestLevel && (depth > bestDepth || (depth == bestDepth && static_cast<std::int32_t>(k) < best)))) {
                    best = static_cast<std::int32_t>(k);
                    bestDepth = depth;
                    bestAt = at;
                }
                return true;
            });
            fill.tubeOf[s] = best;
            fill.at[s] = bestAt;
            if (!(blend > 0.0f)) return;
            // The soft edges of the deeper tubes not holding it.
            const int ownLevel = best < 0 ? std::numeric_limits<int>::min() : fill.depths[static_cast<std::size_t>(best)];
            std::int32_t edge = -1;
            float edgeDistance = blend;
            SectionPoint edgeAt;
            grid.around(root, [&](std::uint32_t i) {
                const std::uint32_t k = tubes[i];
                const int level = fill.depths[k];
                if (level <= ownLevel) return true;
                const TubeSpine& spine = fill.spines[k];
                float along = 0.0f;
                const SectionPoint at = spine.rootCoords(root, &along);
                if (std::abs(along) > kTubeAlongReach * std::max(spine.rootWidth(), spine.rootHeight())) return true;
                if (tubeShapeDepth(spine.tube(), at.u, at.v) >= 0.0f) return true;
                const float d = tubeOutsideDistance(spine.tube(), spine.rootHeight() / spine.rootWidth(), at.u, at.v, blend);
                if (!(d < blend)) return true;
                const int edgeLevel = edge < 0 ? -1 : fill.depths[static_cast<std::size_t>(edge)];
                if (edge < 0 || d < edgeDistance ||
                    (d == edgeDistance && (level > edgeLevel || (level == edgeLevel && static_cast<std::int32_t>(k) < edge)))) {
                    edge = static_cast<std::int32_t>(k);
                    edgeDistance = d;
                    edgeAt = at;
                }
                return true;
            });
            if (edge < 0) return;
            const float x = edgeDistance / blend;
            fill.edgeOf[s] = edge;
            fill.edgeAt[s] = edgeAt;
            fill.edgeWeight[s] = 1.0f - x * x * (3.0f - 2.0f * x);
        }, 1024);
    }

    void remember(const StrandsPtr& output, const StrandsPtr& strands, const GuidesPtr& guides,
                  const std::vector<const Guide*>& kept, const std::shared_ptr<const Binding>& binding,
                  const std::vector<std::string>& attrNames, const std::vector<std::string>& layerNames,
                  std::uint64_t scalp, std::uint64_t baseKey, std::shared_ptr<const RegionSource> regionSource,
                  std::shared_ptr<const RegionSplit> regions, std::shared_ptr<const GuideRows> rows) const {
        const std::lock_guard<std::mutex> lock(madeMutex_);
        made_.rows = std::move(rows);
        made_.regionSource = std::move(regionSource);
        made_.regions = std::move(regions);
        made_.output = output;
        made_.strands = strands;
        made_.guides = guides;
        made_.kept = kept;
        made_.binding = binding;
        made_.attrNames = attrNames;
        made_.layerNames = layerNames;
        made_.scalp = scalp;
        made_.baseKey = baseKey;
    }
    void forget() const {
        const std::lock_guard<std::mutex> lock(madeMutex_);
        made_ = Made{};
    }

    // Each strand's guides and weights (Binding), from the strands' roots (`queries`) and the guides'.
    // On a scalp (`graph`): the strands' and guides' roots on it, for the paths along it.
    // `previous`: a binding of the same strands, params and parts whose guide roots may differ
    // (as many guides): strands no change reaches keep its records, guides whose root and reach
    // stayed their paths. `regions` (with `regionSource`, what they were read from): each strand
    // blends only the guides of its own region (a strand in none: any guide), weighed as if they
    // were the only guides, each guide's reach among its region's (null: none).
    static std::shared_ptr<const Binding> bind(const std::shared_ptr<const std::vector<Vec3f>>& queriesPtr,
                                               const std::shared_ptr<const std::vector<char>>& hasPointsPtr,
                                               const std::shared_ptr<const std::vector<StrandRoot>>& strandRoots,
                                               const std::vector<Vec3f>& roots, const std::vector<StrandRoot>& guideRoots,
                                               const std::shared_ptr<const SurfaceGraph>& graph, const BindParams& b,
                                               std::uint64_t baseKey, const Binding* previous,
                                               const std::shared_ptr<const RegionSource>& regionSource,
                                               const std::shared_ptr<const RegionSplit>& regions) {
        ++bindCount;
        const bool nearestOnly = b.nearestOnly;
        const float reach = b.reach, power = b.power;
        const PartLines* parts = b.parts;
        const std::vector<Vec3f>& queries = *queriesPtr;
        const std::vector<char>& hasPoints = *hasPointsPtr;
        auto out = std::make_shared<Binding>();
        out->baseKey = baseKey;
        out->roots = roots;
        out->queries = queriesPtr;
        out->hasPoints = hasPointsPtr;
        out->strandCount = queries.size();
        out->regionSource = regionSource;
        out->regions = regions;
        const PointIndex rootIndex(roots);
        constexpr float kInfinite = std::numeric_limits<float>::infinity();

        // The guides a strand searches: all of them (subset 0), or with regions its region's
        // (subset r + 1; a strand in none: all), each subset searched and weighed on its own.
        const std::size_t regionCount = regions ? regions->palette.size() : 0;
        const std::vector<std::int32_t>* strandRegions = regionCount ? regions->strands.get() : nullptr;
        auto subsetOf = [&](std::size_t s) { return strandRegions ? static_cast<std::size_t>((*strandRegions)[s] + 1) : std::size_t{0}; };
        struct Subset {
            std::vector<std::uint32_t> guides;  // a region's guides (indices into `roots`); empty: all
            std::vector<Vec3f> roots;           // theirs
            std::optional<PointIndex> own;      // a region's index over them
            const PointIndex* index = nullptr;
            BindParams p;
            const std::vector<float>* reaches = nullptr;  // per guide, as this subset's strands weigh them
            std::optional<CandidateCells> cells;
            bool used = false;  // some strand with points searches it
        };
        std::vector<Subset> subsets(regionCount + 1);
        subsets[0].index = &rootIndex;
        subsets[0].p = b;
        subsets[0].reaches = &out->reaches;
        subsets[0].used = strandRegions == nullptr;
        bool noneByCore = subsets[0].used;
        if (regionCount) {
            for (std::size_t g = 0; g < roots.size(); ++g) {
                const std::int32_t r = regions->guides[g];
                if (r < 0) continue;
                subsets[static_cast<std::size_t>(r) + 1].guides.push_back(static_cast<std::uint32_t>(g));
                subsets[static_cast<std::size_t>(r) + 1].roots.push_back(roots[g]);
            }
            std::unique_ptr<std::atomic<std::uint8_t>[]> used(new std::atomic<std::uint8_t>[regionCount + 1]());
            std::atomic<std::uint8_t> coreUsesNone{0};  // a core strand (preview_core.h) searches subset 0
            parallelFor(0, queries.size(), [&](std::size_t s) {
                if (!hasPoints[s]) return;
                const std::size_t k = subsetOf(s);
                std::atomic<std::uint8_t>& u = used[k];
                if (!u.load(std::memory_order_relaxed)) u.store(1, std::memory_order_relaxed);
                if (k == 0 && hasPoints[s] == 2 && !coreUsesNone.load(std::memory_order_relaxed)) coreUsesNone.store(1, std::memory_order_relaxed);
            }, 4096);
            subsets[0].used = used[0].load() != 0;
            noneByCore = coreUsesNone.load() != 0;
            out->regionReaches.assign(roots.size(), kInfinite);
            for (std::size_t k = 1; k <= regionCount; ++k) {
                Subset& sub = subsets[k];
                sub.own.emplace(sub.roots);
                sub.index = &*sub.own;
                sub.p = sizedFor(b, sub.guides.size());
                sub.reaches = &out->regionReaches;
                sub.used = used[k].load() != 0;
            }
        }
        // Each guide's reach: `reach` times the distance to its neighbors-th nearest other guide
        // (infinite with too few guides: plain inverse distance); with regions, among its own
        // region's guides too (regionReaches).
        // How far each guide's paths along the skin go: its reach (with followSurface, the radius
        // at least); taking the nearest guide alone, `reach` times the distance to its nearest
        // other one (a strand farther from every guide takes the straight line's nearest). Rounded
        // up to an eighth of an octave: a reshaped guide's root moves by a rounding (the smooth
        // curve's first point), which changes the reaches about it by as little, and keeps its paths.
        // With regions, the farthest of the reaches its strands weigh it by.
        out->reaches.assign(roots.size(), kInfinite);
        std::vector<float> limits(graph ? roots.size() : 0, kInfinite);
        std::vector<float> regionLimits(graph && regionCount ? roots.size() : 0, kInfinite);
        auto reachesOf = [&](const Subset& sub, std::vector<float>& reachOut, std::vector<float>& limitOut) {
            const std::size_t count = sub.guides.empty() ? roots.size() : sub.guides.size();
            const std::size_t neighborCount = sub.p.neighborCount;
            if (count <= neighborCount || (nearestOnly && !graph)) return;
            parallelFor(0, count, [&](std::size_t i) {
                const std::size_t g = sub.guides.empty() ? i : sub.guides[i];
                std::array<std::size_t, kMaxNeighbors + 1> idx{};
                std::array<float, kMaxNeighbors + 1> d2{};
                const std::size_t found = sub.index->kNearest(roots[g], neighborCount + 1, idx.data(), d2.data());
                if (found <= neighborCount) return;
                const float at = reach * std::sqrt(d2[neighborCount]);  // [0] is the guide itself
                if (!nearestOnly) reachOut[g] = at;
                if (graph) limitOut[g] = pm::exp2(std::ceil(pm::log2(std::max(at, b.radius)) * 8.0f) / 8.0f);
            });
        };
        reachesOf(subsets[0], out->reaches, limits);
        if (regionCount) {
            for (std::size_t k = 1; k <= regionCount; ++k) reachesOf(subsets[k], out->regionReaches, regionLimits);
            if (graph) {
                for (std::size_t g = 0; g < roots.size(); ++g) {
                    if (regions->guides[g] < 0) continue;  // all subset 0's
                    // Whether strands in no region weigh it too, as the core strands say (the same
                    // at every preview density).
                    limits[g] = noneByCore ? std::max(limits[g], regionLimits[g]) : regionLimits[g];
                }
            }
        }
        // Each guide's side of the parts.
        std::vector<PartLines::Side> guideSides;
        if (parts) {
            guideSides.resize(roots.size());
            parallelFor(0, roots.size(), [&](std::size_t g) { guideSides[g] = parts->sideOf(roots[g]); });
        }
        // The paths along the skin from each guide root (those of `previous` kept where the guide's
        // root and limit stayed). A first binding makes the strands' search cells meanwhile (they
        // do not depend on the paths; binding again, which strands search does).
        const bool referenced = referencePaths.load(std::memory_order_relaxed);
        auto makeCells = [&](auto&& searches) {
            for (std::size_t k = 0; k < subsets.size(); ++k) {
                Subset& sub = subsets[k];
                if (!sub.used) continue;
                sub.cells.emplace(sub.guides.empty() ? roots : sub.roots, *sub.index, queries,
                                  [&](std::size_t s) { return hasPoints[s] && subsetOf(s) == k && searches(s); },
                                  referenced ? 0 : sub.p.candidateCount + 1);
            }
        };
        auto makeFronts = [&] {
            if (!graph) return;
            out->strandRoots = strandRoots;
            out->guideRoots = guideRoots;
            out->fronts = std::make_shared<const SurfaceFronts>(graph, guideRoots, limits, previous ? previous->fronts.get() : nullptr);
        };
        if (previous) {
            makeFronts();
        } else {
            parallelInvoke(makeFronts, [&] { makeCells([](std::size_t) { return true; }); });
        }
        const SurfaceFronts* fronts = out->fronts.get();

        // Only some guide roots moved: what changed, where it was and is (the moved guides,
        // before and after, and the guides whose reach or paths changed). A strand none of these
        // is a candidate of, before or now, keeps its record. Most moved: everything is bound again.
        // With regions, only while every guide and strand keeps its region.
        auto same = [](float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; };
        const bool sameRegions = (previous && previous->regions) == (regions != nullptr) &&
                                 (!regions || (previous->regions->guides == regions->guides &&
                                               previous->regions->palette.size() == regions->palette.size() &&
                                               sameStrandRegions(*previous->regions, *regions)));
        const bool partial = previous && previous->roots.size() == roots.size() && previous->strandCount == queries.size() &&
                             (previous->fronts != nullptr) == (fronts != nullptr) && sameRegions;
        std::vector<Vec3f> changed;
        if (partial) {
            for (std::size_t g = 0; g < roots.size(); ++g) {
                const Vec3f& was = previous->roots[g];
                const bool moved = !same(was.x, roots[g].x) || !same(was.y, roots[g].y) || !same(was.z, roots[g].z);
                if (moved) changed.push_back(was);
                const bool pathsChanged = fronts && previous->fronts->front(g) != fronts->front(g);
                const bool reachChanged = !same(previous->reaches[g], out->reaches[g]) ||
                                          (regionCount && !same(previous->regionReaches[g], out->regionReaches[g]));
                if (moved || pathsChanged || reachChanged) changed.push_back(roots[g]);
            }
        }
        const bool reuse = partial && changed.size() <= roots.size() / 2;
        // Their box first (a strand farther from it than its farthest candidate is untouched),
        // then the nearest of them.
        Vec3f lo{0.0f, 0.0f, 0.0f}, hi = lo;
        if (!changed.empty()) lo = hi = changed[0];
        for (const Vec3f& p : changed) {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        const PointIndex changes(reuse ? changed : std::vector<Vec3f>{});
        auto untouched = [&](const Vec3f& q, float reach2) {
            if (changes.empty()) return true;
            const float dx = std::max({lo.x - q.x, 0.0f, q.x - hi.x});
            const float dy = std::max({lo.y - q.y, 0.0f, q.y - hi.y});
            const float dz = std::max({lo.z - q.z, 0.0f, q.z - hi.z});
            // (The box test is rounded: only a clear miss by it skips the exact search.)
            if (dx * dx + dy * dy + dz * dz > reach2 * 1.001f + 1e-12f) return true;
            float d2 = 0.0f;
            changes.nearest(q, &d2);
            return d2 > reach2;
        };

        // Strands in chunks, each chunk's records made on its own. Binding again, a chunk whose
        // strands are all strict and whose roots are clear of everything that changed by more
        // than its farthest candidate (with room for rounding: each of its strands passes the test
        // below) is the previous binding's.
        const std::size_t strandCount = queries.size();
        const std::size_t chunks = (strandCount + kBindChunk - 1) / kBindChunk;
        const bool wide = roots.size() > kSmallGuides;  // guide indices in 32 bits
        out->chunks.resize(chunks);
        std::vector<char> shared(chunks, 0);
        if (reuse) {
            parallelFor(0, chunks, [&](std::size_t c) {
                const BindingChunk& was = *previous->chunks[c];
                if (!was.any || changes.empty()) {
                    shared[c] = 1;
                    return;
                }
                if (!was.allStrict || !std::isfinite(was.farthest2)) return;
                const float dx = std::max({was.lo.x - hi.x, 0.0f, lo.x - was.hi.x});
                const float dy = std::max({was.lo.y - hi.y, 0.0f, lo.y - was.hi.y});
                const float dz = std::max({was.lo.z - hi.z, 0.0f, lo.z - was.hi.z});
                shared[c] = dx * dx + dy * dy + dz * dz > was.farthest2 * 1.01f + 1e-9f ? 1 : 0;
            }, 16);
        }
        // In the other chunks, the strands that keep their records: strict ones that nothing that
        // changed is or was a candidate of. The others search, from cells (the tree as first
        // written, for tests).
        std::vector<char> keeps;
        if (reuse) {
            keeps.assign(strandCount, 0);
            parallelFor(0, chunks, [&](std::size_t c) {
                if (shared[c]) return;
                const BindingChunk& was = *previous->chunks[c];
                const std::size_t first = c * kBindChunk, end = std::min(strandCount, first + kBindChunk);
                for (std::size_t s = first; s < end; ++s) {
                    keeps[s] = hasPoints[s] && was.strict[s - first] && untouched(queries[s], was.reach2[s - first]) ? 1 : 0;
                }
            }, 4);
        }
        if (previous) makeCells([&](std::size_t s) { return !(reuse && (shared[s / kBindChunk] || keeps[s])); });
        std::atomic<std::size_t> searchedTotal{0};
        parallelFor(0, chunks, [&](std::size_t c) {
            if (reuse && shared[c]) {
                out->chunks[c] = previous->chunks[c];
                return;
            }
            const std::size_t first = c * kBindChunk, last = std::min(strandCount, first + kBindChunk), n = last - first;
            auto chunk = std::make_shared<BindingChunk>();
            chunk->offsets.assign(n + 1, 0);
            chunk->nearest2.assign(n, 0.0f);
            chunk->bound.assign(n, 0);
            chunk->reach2.assign(n, 0.0f);
            chunk->strict.assign(n, 0);
            chunk->wide = wide;
            chunk->weights.reserve(n * 8);  // (about 7.5 guides a strand)
            if (wide) {
                chunk->guides32.reserve(n * 8);
            } else {
                chunk->guides16.reserve(n * 8);
            }
            // A record: its guide, weight (code) and twist (0: none).
            auto keep = [&](std::size_t guide, std::uint16_t code, float twist) {
                const auto at = static_cast<std::uint32_t>(chunk->weights.size());
                if (wide) {
                    chunk->guides32.push_back(static_cast<std::uint32_t>(guide));
                } else {
                    chunk->guides16.push_back(static_cast<std::uint16_t>(guide));
                }
                chunk->weights.push_back(code);
                if (twist != 0.0f) {
                    chunk->twistAt.push_back(at);
                    chunk->twists.push_back(twist);
                }
            };
            std::size_t searched = 0;
            for (std::size_t s = first; s < last; ++s) {
                const std::size_t i = s - first;
                chunk->offsets[i + 1] = static_cast<std::uint32_t>(chunk->weights.size());
                if (!hasPoints[s]) continue;
                const Vec3f& query = queries[s];
                if (reuse && keeps[s]) {
                    // Nothing that changed is or was among its candidates: as before.
                    const BindingChunk& was = *previous->chunks[c];
                    chunk->nearest2[i] = was.nearest2[i];
                    chunk->bound[i] = was.bound[i];
                    chunk->reach2[i] = was.reach2[i];
                    chunk->strict[i] = 1;
                    const Binding::Record record = previous->record(s);
                    std::array<float, kMaxCandidates + 1> twist{};
                    record.twists(twist.data());
                    for (std::uint32_t r = 0; r < record.count; ++r) keep(record.guide(r), record.code(r), twist[r]);
                    chunk->offsets[i + 1] = static_cast<std::uint32_t>(chunk->weights.size());
                    continue;
                }

                ++searched;
                // The guides it searches (with regions, its region's) and their sizes.
                const Subset& sub = subsets[subsetOf(s)];
                const std::size_t neighborCount = sub.p.neighborCount, candidateCount = sub.p.candidateCount;
                const std::vector<float>& reaches = *sub.reaches;
                // The nearest guide roots, nearest first, and the next one out: strictly ordered,
                // the first candidateCount are the candidates (as a search for that many finds them).
                std::array<std::size_t, kMaxCandidates + 1> idx;  // (read only as far as found)
                std::array<float, kMaxCandidates + 1> dist2;
                std::size_t found = 0;
                if (!sub.cells || !sub.cells->nearest(query, idx.data(), dist2.data(), &found)) {
                    found = sub.index->kNearest(query, candidateCount + 1, idx.data(), dist2.data());
                }
                bool strict = true;
                for (std::size_t j = 1; j < found; ++j) strict = strict && dist2[j - 1] < dist2[j];
                if (strict) {
                    chunk->reach2[i] = found > candidateCount ? dist2[candidateCount - 1] : std::numeric_limits<float>::infinity();
                    found = std::min(found, candidateCount);
                } else {
                    found = sub.index->kNearest(query, candidateCount, idx.data(), dist2.data());
                }
                chunk->strict[i] = strict ? 1 : 0;
                if (!sub.guides.empty()) {  // a region's: their indices among all the guides
                    for (std::size_t j = 0; j < found; ++j) idx[j] = sub.guides[idx[j]];
                }
                if (parts) {
                    // Keep the guides on this strand's side of any part.
                    const PartLines::Side side = parts->sideOf(query);
                    std::size_t kept = 0;
                    for (std::size_t j = 0; j < found; ++j) {
                        if (PartLines::separated(side, guideSides[idx[j]])) continue;
                        idx[kept] = idx[j];
                        dist2[kept] = dist2[j];
                        ++kept;
                    }
                    if (kept > 0) found = kept;  // every guide across: blend as before
                }
                if (found == 0) continue;

                // The paths along the skin from the candidates that may weigh (within their reach,
                // or the radius), each found once: how far each is along the skin and the twist its
                // bends add (none where the straight line hugs the skin).
                const SurfacePathEnd end(fronts ? (*strandRoots)[s] : StrandRoot{}, query);
                // A candidate within its limit on a triangle every front reaches straight there
                // (almost the whole of a scalp) comes by the straight line: nothing to look up.
                // The others' paths are looked up once each (raw: only those looked up are made
                // and read; one not looked up is the straight line).
                const bool straightHere = fronts && fronts->straightAt(end.root);
                auto plain = [&](std::size_t j) { return straightHere && dist2[j] < fronts->straightWithin2(idx[j]); };
                alignas(SurfacePath) unsigned char pathBytes[sizeof(SurfacePath) * (kMaxCandidates + 1)];
                SurfacePath* paths = reinterpret_cast<SurfacePath*>(pathBytes);
                std::uint64_t asked[2] = {0, 0};
                auto wasAsked = [&](std::size_t j) { return (asked[j >> 6] >> (j & 63)) & 1u; };
                auto pathOf = [&](std::size_t j) -> const SurfacePath& {
                    if (!wasAsked(j)) {
                        new (&paths[j]) SurfacePath(fronts->path(idx[j], end, dist2[j]));
                        asked[j >> 6] |= std::uint64_t{1} << (j & 63);
                    }
                    return paths[j];
                };
                auto twistOf = [&](std::size_t j) {
                    return wasAsked(j) && paths[j].kind == SurfacePath::Kind::Bent ? paths[j].twist : 0.0f;
                };
                // The distance candidate j is weighed by: along the skin with followSurface (out
                // of its reach where the skin never brings it within), else the straight line.
                auto weighed = [&](std::size_t j, float R) {
                    const float d = std::sqrt(dist2[j]);
                    if (!fronts || !(d < R || d < b.radius) || plain(j)) return d;
                    const SurfacePath& p = pathOf(j);
                    if (!b.followSurface) return d;
                    if (p.kind == SurfacePath::Kind::Bent) return p.distance;
                    return p.kind == SurfacePath::Kind::Beyond ? std::numeric_limits<float>::infinity() : d;
                };

                // Modified Shepard weights over the guides whose reach covers the
                // strand; an exact hit takes the guide's shape. Outside every reach
                // (beyond the guides' fringe): the nearest guide's, with plain inverse
                // distance among the candidates in its own ring.
                std::array<float, kMaxCandidates + 1> w;
                std::array<float, kMaxCandidates + 1> twist;
                float wsum = 0.0f;
                float nearest2 = dist2[0];
                if (nearestOnly) {
                    // Following the skin, the nearest along it (none within reach along it: the
                    // straight line's nearest).
                    std::size_t pick = 0;
                    if (b.followSurface) {
                        nearest2 = std::numeric_limits<float>::infinity();
                        for (std::size_t j = 0; j < found; ++j) {
                            if (plain(j)) {
                                if (dist2[j] < nearest2) {
                                    nearest2 = dist2[j];
                                    pick = j;
                                }
                                continue;
                            }
                            const SurfacePath& p = pathOf(j);
                            const float d2 = p.kind == SurfacePath::Kind::Bent     ? p.distance * p.distance
                                             : p.kind == SurfacePath::Kind::Beyond ? std::numeric_limits<float>::infinity()
                                                                                   : dist2[j];
                            if (d2 < nearest2) {
                                nearest2 = d2;
                                pick = j;
                            }
                        }
                    } else if (fronts && !plain(0)) {
                        pathOf(0);
                    }
                    idx[0] = idx[pick];
                    twist[0] = twistOf(pick);
                    w[0] = 1.0f;
                    wsum = 1.0f;
                    found = 1;
                } else {
                    // Each weight is the guide's base over the largest base among the candidates,
                    // raised to the power: the same blend (the weights are normalised by their sum)
                    // with the largest weight 1, so no power overflows a float (a strand root on a
                    // guide root has a base of a million) and no blend underflows to nothing.
                    std::array<float, kMaxCandidates + 1> bases;
                    float largest = 0.0f;
                    // How deep inside its best-covering guide's reach the strand is (0 at the
                    // fringe, 1 at the root): the blend fades to the fringe's over the outer band.
                    float depth = 0.0f;
                    for (std::size_t j = 0; j < found; ++j) {
                        const float R = reaches[idx[j]];
                        const float d = weighed(j, R);
                        float base;
                        if (std::isfinite(R)) {
                            base = d < R ? (R - d) / (R * std::max(d, 1e-6f)) : 0.0f;
                            if (d < R) depth = std::max(depth, (R - d) / R);
                        } else {
                            base = 1.0f / std::max(d, 1e-6f);
                            depth = 1.0f;
                        }
                        bases[j] = base;
                        largest = std::max(largest, base);
                    }
                    for (std::size_t j = 0; j < found; ++j) {
                        w[j] = largest > 0.0f ? shepardWeight(bases[j] / largest, power) : 0.0f;
                        wsum += w[j];
                    }
                    // The nearest guide's distance (the influence, with a radius): along the skin
                    // with followSurface (those not looked up are straight, or beyond the radius).
                    if (b.followSurface && b.radius > 0.0f) {
                        nearest2 = std::numeric_limits<float>::infinity();
                        for (std::size_t j = 0; j < found; ++j) {
                            float d2 = dist2[j];
                            if (wasAsked(j) && paths[j].kind == SurfacePath::Kind::Bent) d2 = paths[j].distance * paths[j].distance;
                            if (wasAsked(j) && paths[j].kind == SurfacePath::Kind::Beyond) d2 = std::numeric_limits<float>::infinity();
                            nearest2 = std::min(nearest2, d2);
                        }
                    }
                    // The fringe's blend: past every guide's reach (along the skin with
                    // followSurface), inverse distance over the nearest few by the straight line
                    // (the paths go no farther than the reaches; their twists where they reached).
                    // A strand just inside the last guide's reach would take that guide alone
                    // while one just outside blends the nearest few: so over the outer band of
                    // the reaches (kFringeBand of the best-covering guide's reach) the two blends
                    // are mixed, and the shape is continuous across the fringe.
                    const float mix = std::min(depth * (1.0f / kFringeBand), 1.0f);  // 1: the reaches' blend alone
                    if (mix < 1.0f) {
                        // Over the nearest few, each weighed to zero at the next candidate out (the
                        // Shepard-Franke ring), so a guide joining or leaving the few as the strand
                        // moves along the fringe weighs nothing where it does: no seam there either.
                        const std::size_t few = std::min(found, neighborCount);
                        const float ring = found > few ? std::sqrt(dist2[few]) : std::numeric_limits<float>::infinity();
                        std::array<float, kMaxCandidates + 1> fringe;
                        float fringeSum = 0.0f, largestBase = 0.0f;
                        for (std::size_t j = 0; j < few; ++j) {
                            if (fronts && !plain(j)) pathOf(j);
                            const float d = std::max(std::sqrt(dist2[j]), 1e-6f);
                            fringe[j] = std::isfinite(ring) ? (d < ring ? (ring - d) / (ring * d) : 0.0f) : 1.0f / d;
                            largestBase = std::max(largestBase, fringe[j]);
                        }
                        for (std::size_t j = 0; j < few; ++j) {
                            fringe[j] = largestBase > 0.0f ? shepardWeight(fringe[j] / largestBase, power) : 0.0f;
                            fringeSum += fringe[j];
                        }
                        if (wsum > 0.0f) {
                            // Both normalised, mixed by the depth: the sum is 1.
                            const float inReach = mix / wsum, atFringe = fringeSum > 0.0f ? (1.0f - mix) / fringeSum : 0.0f;
                            for (std::size_t j = 0; j < found; ++j) w[j] = w[j] * inReach + (j < few ? fringe[j] * atFringe : 0.0f);
                            wsum = 1.0f;
                        } else {
                            for (std::size_t j = 0; j < few; ++j) w[j] = fringe[j];
                            wsum = fringeSum;
                            found = few;
                        }
                    }
                    if (asked[0] | asked[1]) {
                        for (std::size_t j = 0; j < found; ++j) twist[j] = twistOf(j);
                    } else {
                        std::fill(twist.begin(), twist.begin() + static_cast<std::ptrdiff_t>(found), 0.0f);
                    }
                }
                // The nearest guide's distance (the influence), then the guides with a share of
                // the weights (kept in 16 bits: quantizeWeights): the others add nothing to any
                // blend (a guide past its reach weighs 0). Every weight 0 (underflowed): left as it is.
                std::array<std::uint16_t, kMaxCandidates + 1> codes;
                found = quantizeWeights(idx.data(), w.data(), twist.data(), found, codes.data());
                if (found == 0) continue;
                chunk->nearest2[i] = nearest2;
                chunk->bound[i] = 1;
                for (std::size_t j = 0; j < found; ++j) keep(idx[j], codes[j], twist[j]);
                chunk->offsets[i + 1] = static_cast<std::uint32_t>(chunk->weights.size());
            }
            // The chunk's box, farthest candidate and strictness, for binding again.
            for (std::size_t s = first; s < last; ++s) {
                if (!hasPoints[s]) continue;
                const Vec3f& q = queries[s];
                const std::size_t i = s - first;
                if (!chunk->any) chunk->lo = chunk->hi = q;
                chunk->lo = {std::min(chunk->lo.x, q.x), std::min(chunk->lo.y, q.y), std::min(chunk->lo.z, q.z)};
                chunk->hi = {std::max(chunk->hi.x, q.x), std::max(chunk->hi.y, q.y), std::max(chunk->hi.z, q.z)};
                chunk->farthest2 = std::max(chunk->farthest2, chunk->reach2[i]);
                chunk->allStrict = chunk->allStrict && chunk->strict[i];
                chunk->any = true;
            }
            out->chunks[c] = std::move(chunk);
            searchedTotal += searched;
        }, 1);
        strandBindCount += searchedTotal.load();
        return out;
    }
    // The binding of that key; else null, with `*previous` the newest one of the same base key
    // (the same strands, params and parts) if any.
    std::shared_ptr<const Binding> findBinding(std::uint64_t key, std::uint64_t baseKey,
                                               std::shared_ptr<const Binding>* previous) const {
        const std::lock_guard<std::mutex> lock(memoMutex_);
        for (const auto& [k, binding] : memo_) {
            if (k == key) return binding;
        }
        for (const auto& [k, binding] : memo_) {
            if (binding->baseKey == baseKey) {
                *previous = binding;
                break;
            }
        }
        return nullptr;
    }
    void keepBinding(std::uint64_t key, std::shared_ptr<const Binding> binding) const {
        const std::lock_guard<std::mutex> lock(memoMutex_);
        memo_.insert(memo_.begin(), {key, std::move(binding)});
        if (memo_.size() > kBindingMemo) memo_.resize(kBindingMemo);
    }

    mutable std::mutex memoMutex_;
    mutable std::vector<std::pair<std::uint64_t, std::shared_ptr<const Binding>>> memo_;  // newest first
    mutable std::mutex madeMutex_;
    mutable Made made_;
    // The last scalp's Clearance (its bounds and field asked again only for another scalp).
    mutable std::mutex clearanceMutex_;
    mutable MeshPtr clearanceScalp_;
    mutable Clearance clearance_;
    Clearance clearanceOf(const MeshPtr& scalp) const {
        const std::lock_guard<std::mutex> lock(clearanceMutex_);
        if (clearanceScalp_ != scalp) {
            clearance_ = Clearance::of(scalp);
            clearanceScalp_ = scalp;
        }
        return clearance_;
    }
    // The binding as the CPU engine takes it (InterpolateState), kept while the binding is: its
    // records stay uploaded (look_engine.cpp compares them by address).
    mutable std::shared_ptr<const InterpolateState> engineState_;
};

}  // namespace

bool interpolateState(const Operator& op, const StrandsPtr& output, InterpolateState* state) {
    const auto* interpolate = dynamic_cast<const InterpolateOp*>(&op);
    return interpolate && interpolate->state(output, state);
}

void interpolateRecords(const InterpolateState& state, InterpolateRecords* records) {
    const auto* binding = static_cast<const Binding*>(state.binding.get());
    interpolateRecords(state, 0, binding ? binding->strandCount : 0, records);
}

void interpolateRecords(const InterpolateState& state, std::size_t first, std::size_t count, InterpolateRecords* records) {
    const auto* binding = static_cast<const Binding*>(state.binding.get());
    InterpolateRecords& r = *records;
    const std::size_t n = binding ? std::min(count, binding->strandCount - std::min(first, binding->strandCount)) : 0;
    r.offsets.assign(n + 1, 0);
    r.nearest2.assign(n, 0.0f);
    r.bound.assign(n, 0);
    for (std::size_t s = 0; s < n; ++s) r.offsets[s + 1] = r.offsets[s] + binding->record(first + s).count;
    r.guides.resize(r.offsets[n]);
    r.weights.resize(r.offsets[n]);
    r.twists.assign(r.offsets[n], 0.0f);
    parallelFor(0, n, [&](std::size_t s) {
        const Binding::Record record = binding->record(first + s);
        const std::uint32_t at = r.offsets[s];
        for (std::uint32_t i = 0; i < record.count; ++i) {
            r.guides[at + i] = record.guide(i);
            r.weights[at + i] = record.code(i);
        }
        record.twists(r.twists.data() + at);
        r.nearest2[s] = record.nearest2;
        r.bound[s] = record.bound ? 1 : 0;
    }, 4096);
}

std::size_t interpolateChunkStrands() { return kBindChunk; }

std::size_t interpolateRecordBytes(const InterpolateState& state) {
    const auto* binding = static_cast<const Binding*>(state.binding.get());
    std::size_t bytes = 0;
    if (!binding) return 0;
    for (const auto& chunk : binding->chunks) {
        const BindingChunk& c = *chunk;
        bytes += c.offsets.size() * 4 + c.guides16.size() * 2 + c.guides32.size() * 4 + c.weights.size() * 2 +
                 c.twistAt.size() * 4 + c.twists.size() * 4 + c.nearest2.size() * 4 + c.bound.size();
    }
    return bytes;
}

std::vector<const void*> interpolateChunks(const InterpolateState& state) {
    const auto* binding = static_cast<const Binding*>(state.binding.get());
    std::vector<const void*> chunks;
    if (binding) {
        for (const auto& chunk : binding->chunks) chunks.push_back(chunk.get());
    }
    return chunks;
}

std::shared_ptr<const InterpolateState> interpolateStateOf(const InterpolateState& state, const std::vector<std::uint32_t>& which) {
    const auto* binding = static_cast<const Binding*>(state.binding.get());
    auto made = std::make_shared<Binding>();
    made->strandCount = which.size();
    // (Each chunk made on its own, side by side.)
    made->chunks.resize((which.size() + kBindChunk - 1) / kBindChunk);
    parallelFor(0, made->chunks.size(), [&](std::size_t c) {
        const std::size_t first = c * kBindChunk, n = std::min(kBindChunk, which.size() - first);
        thread_local std::vector<float> twists;
        auto chunk = std::make_shared<BindingChunk>();
        chunk->wide = true;
        chunk->offsets.assign(n + 1, 0u);
        chunk->nearest2.resize(n);
        chunk->bound.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            const Binding::Record r = binding->record(which[first + i]);
            const std::uint32_t at = chunk->offsets[i];
            chunk->offsets[i + 1] = at + r.count;
            chunk->nearest2[i] = r.nearest2;
            chunk->bound[i] = r.bound ? 1 : 0;
            for (std::uint32_t g = 0; g < r.count; ++g) {
                chunk->guides32.push_back(r.guide(g));
                chunk->weights.push_back(r.code(g));
            }
            twists.resize(r.count);
            if (!r.twists(twists.data())) continue;
            for (std::uint32_t g = 0; g < r.count; ++g) {
                if (twists[g] == 0.0f) continue;
                chunk->twistAt.push_back(at + g);
                chunk->twists.push_back(twists[g]);
            }
        }
        made->chunks[c] = std::move(chunk);
    }, 1);
    auto out = std::make_shared<InterpolateState>(state);
    out->binding = std::move(made);
    out->strandCount = which.size();
    return out;
}

bool interpolateRebind(const Operator& op, const PartLines* parts, const std::vector<Vec3f>& roots,
                       const std::vector<StrandRoot>& guideRoots, InterpolateState* state) {
    const auto* interpolate = dynamic_cast<const InterpolateOp*>(&op);
    return interpolate && interpolate->rebind(parts, roots, guideRoots, state);
}

std::size_t interpolateSamples(const StrandSet& strands) {
    const PreviewCore core(strands);
    if (core.all()) return std::max(kMinGuideSamples, strands.maxPointCount());
    // (In blocks side by side, each its most, then theirs.)
    constexpr std::size_t kBlock = 4096;
    const std::size_t count = strands.strandCount(), blocks = (count + kBlock - 1) / kBlock;
    std::vector<std::size_t> most(blocks, 0);
    parallelFor(0, blocks, [&](std::size_t b) {
        for (std::size_t s = b * kBlock; s < std::min(count, (b + 1) * kBlock); ++s) {
            if (core.core(s)) most[b] = std::max(most[b], strands.strandPointCount(s));
        }
    }, 1);
    std::size_t all = 0;
    for (const std::size_t m : most) all = std::max(all, m);
    return std::max(kMinGuideSamples, all);
}

void interpolateGuideRows(const ParamSet& params, const std::vector<const Guide*>& kept, const MeshPtr& scalpPtr,
                          std::size_t samples, InterpolateGuideRows* rows) {
    // As InterpolateOp::run makes them: the same shapes, roots and normals.
    const ScalpMesh* scalp = scalpPtr.get();
    const bool align = scalp && scalp->triangleCount() > 0;
    const bool smoothGuides = params.getBool("smoothGuides");
    rows->samples = samples;
    rows->offsets.assign(kept.size() * samples, Vec3f{});
    rows->roots.assign(kept.size(), Vec3f{});
    rows->normals.assign(kept.size(), Vec3f{});
    rows->lengths.assign(kept.size(), 0.0f);
    parallelFor(0, kept.size(), [&](std::size_t k) {
        const Guide& g = *kept[k];
        thread_local std::vector<Vec3f> resampled;
        rows->lengths[k] = guideShape(g, samples, smoothGuides, resampled);
        rows->roots[k] = restRootOr(align ? scalp : nullptr, g.root, resampled.front());
        rows->normals[k] = align && scalp->contains(g.root) ? scalp->normal(g.root) : Vec3f{};
        Vec3f* row = rows->offsets.data() + k * samples;
        for (std::size_t j = 0; j < samples; ++j) row[j] = resampled[j] - resampled.front();
    }, 8);
}

bool interpolateKeepsClearance(const ParamSet& params, const ScalpMesh* scalp) {
    return scalp && scalp->triangleCount() > 0 && params.getString("take") == "shape" && params.getBool("keepClearance");
}

std::shared_ptr<const InterpolateClearance> interpolateClearance(const ParamSet& params, const MeshPtr& scalp, std::string* why,
                                                                 bool host) {
    if (!interpolateKeepsClearance(params, scalp.get())) {
        *why = "it does not keep clearance";
        return nullptr;
    }
    return clearanceInputs(Clearance::of(scalp), scalp, why, host);
}

namespace {

std::shared_ptr<const InterpolateClearance> clearanceInputs(const Clearance& c, const MeshPtr& scalp, std::string* why, bool host) {
    if (!c) {
        *why = "its scalp has no size";
        return nullptr;
    }
    auto out = std::make_shared<InterpolateClearance>();
    out->floor = c.floor;
    out->ease = c.ease;
    out->span = c.span;
    out->depth = c.depth;
    out->searchRadius = c.searchRadius;
    out->beside = c.beside;
    out->mesh = scalp;
    if (host) {
        // The CPU engine's: every push asked of the surface itself, its field making only the
        // bricks the strands come near (tables are made whole); over a deformed scalp each strand's
        // walk from its root's vertex (interpolateRestCursors).
        out->host = true;
        out->surface = c.surface;
        out->rest = c.surface.rest;
        out->tables = std::make_shared<const std::vector<std::uint32_t>>(MeshDistanceField::kTableHeaderWords, 0u);
        return out;
    }
    if (c.surface.field) {
        out->tables = c.surface.field->tables(c.cap, c.searchRadius);
    } else if (c.surface.rest) {  // a deformed scalp's walk goes through its rest pose, vertex by vertex
        out->tables = c.surface.rest->restField().tables(c.cap, c.searchRadius);
        out->walk = c.surface.rest->walkWords();
        out->rest = c.surface.rest;
    }
    if (!out->tables) {
        *why = c.surface.field || c.surface.rest ? "its scalp is too big for the clearance field's grid"
                                                 : "it keeps clearance over a deformed scalp its rest field cannot push from";
        return nullptr;
    }
    return out;
}

}  // namespace

std::shared_ptr<const std::vector<std::uint32_t>> interpolateRestCursors(const InterpolateClearance& clearance, const StrandSet& in,
                                                                         float longestGuide, std::string* why, bool meshAsked) {
    if (!clearance.rest || (!clearance.walk && !meshAsked)) {
        *why = "no deformed scalp's walk";
        return nullptr;
    }
    auto cursors = std::make_shared<std::vector<std::uint32_t>>(in.strandCount(), 0xFFFFFFFFu);
    const RestPoseCollision& rest = *clearance.rest;
    parallelFor(0, in.strandCount(), [&](std::size_t s) {
        if (in.strandPointCount(s) == 0) return;
        const RestPoseCollision::Cursor c = rest.cursor(in.roots()[s], in.strandPoints(s)[0]);
        if (c.vertex != RestPoseCollision::Cursor::kNone) (*cursors)[s] = static_cast<std::uint32_t>(c.vertex);
    }, 4096);
    if (meshAsked) return cursors;  // (the CPU engine asks the mesh at them)
    // How near the strands' roots the scalp's degenerate faces' vertices come (which push asks the
    // mesh at: the CPU engine's alone). A walk from a root reaches no vertex farther from it than twice the
    // strand's reach (twice the longest guide) and two of the longest edge, and asks a vertex's
    // motion only within the longest edge and the search of it.
    const std::vector<std::uint32_t>& w = *clearance.walk;
    std::vector<Vec3f> degenerate;
    for (std::uint32_t v = 0; v < w[RestPoseCollision::kWalkVertices]; ++v) {
        if (w[w[RestPoseCollision::kWalkMotions] + v * RestPoseCollision::kWalkMotionWords] != RestPoseCollision::kWalkNoMotion) continue;
        Vec3f q;
        std::memcpy(&q.x, &w[w[RestPoseCollision::kWalkPoints] + v * 3], sizeof(Vec3f));
        degenerate.push_back(q);
    }
    float apart = std::numeric_limits<float>::infinity();
    if (!degenerate.empty()) {
        const PointIndex index(degenerate);
        for (std::size_t s = 0; s < in.strandCount(); ++s) {
            if (in.strandPointCount(s) == 0) continue;
            float d2 = 0.0f;
            index.nearest(in.strandPoints(s)[0], &d2);
            apart = std::min(apart, std::sqrt(d2));
        }
    }
    float edge = 0.0f;
    std::memcpy(&edge, &w[RestPoseCollision::kWalkLongestEdge], sizeof(float));
    const float reach = 4.0f * longestGuide + 3.0f * edge + clearance.searchRadius;
    if (!(apart > reach * 1.01f)) {
        *why = "its strands come near a degenerate face of the deformed scalp";
        return nullptr;
    }
    return cursors;
}

std::size_t interpolateBindCount() { return bindCount.load(); }
std::size_t interpolateStrandBindCount() { return strandBindCount.load(); }
void setInterpolateReferencePaths(bool reference) { referencePaths = reference; }

void registerInterpolateOperator(OperatorRegistry& registry) {
    registry.registerType(op::kInterpolate,
                          {"grow",
                           "Grows each root into a strand shaped like the guides around it.",
                           "children guides blend strands"},
                          [] { return std::make_unique<InterpolateOp>(); });
}

}  // namespace groomlab
