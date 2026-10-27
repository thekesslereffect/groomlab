// SPDX-License-Identifier: Apache-2.0
// Braid (Strands + Guides -> Strands): braids strands along braid paths.
//
// Each path (a guide, e.g. from a "braids" guide group) gathers the strands
// rooted within `region` of its root. Without a braids input it picks its own
// paths among the strands, about `region` apart over the scalp (as a Clump
// picks its centers by size): box braids all over the head with nothing drawn. Along the path, the strands split into
// lobes that weave around it in the path's rotation-minimizing frame:
//   three     classic three-strand braid (lobes trace figure-eights)
//   fishtail  two sections exchanging fine strips
//   rope      two lobes twisted around each other
//   cornrow   a three-strand braid flattened onto the scalp (Dutch: raised)
// `dutch` crosses the lobes under instead of over (a raised braid). Strands
// leave their roots over the first `gather` of their length, the braid
// narrows along `widthProfile`, and over the last `tail` the lobes let go and the
// ends fan out. A share of strands (`strays`) stay loose.
//
// The hair is parted into the sections as it lies: a strand takes the section
// on its side of the braid where it joins (the region's roots cut into wedges
// side to side, one a section), so the sections leave the scalp without
// crossing each other.
//
// The weave is drawn with `resolution` points per repeat: a braided strand
// with fewer points (strands interpolated from a few guide points, say) gets
// points added along it (up to maxSegments), so the braid stays a round,
// woven volume rather than a few flat zigzags. `display` paths shows each
// braid's path and its sections' center curves in place of the hair.
//
// With `feed`, strands rooted anywhere along a path (within `feedDistance` of
// its first `feedUntil` share) join the braid where they are and run along it
// from there, as hair is picked up into cornrows and French braids; past
// `feedUntil` the braid hangs free. A strand picked up joins the outer section
// on its side, as a braider adds hair, and with `feedGrowth` the braid
// thickens with the hair it holds. The root region (`region`) joins in both
// modes. With `endAtPath` (the default) each fed strand is fitted to the path:
// it ends within the braid's last stretch (`tail`), wherever it joined;
// otherwise longer strands hang straight on past the path's end.
//
// Strands drawn in from a distant root (a ponytail's, or fed from the root)
// would cut straight through the head on their way: with `alongScalp` the
// gathering stretch is pressed onto the scalp (plus a clearance), so the hair
// sweeps over the head into the braid.
//
// The look: `sections` weaves more strands (a four- or five-strand braid, a
// three-ply rope), `depth` rounds or flattens the braid's cross-section,
// `phase` slides the weave along it, `crossingsProfile` varies the crossings along
// its length, and `sectionTwist` twists each section's hair around itself as
// it weaves (as real sections do).
//
// Output channels, per strand: `braidId` (the path's index among the guides, or for paths of its own a
// number from the picked strand's ID; -1 when not
// braided) and `braidFlat` (1 for a cornrow, lying on the scalp), for nodes
// downstream (Cards builds braid cards from them); with display paths, `braidSection`
// (-1 the path, else the section).

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>

#include "groomlab/curve.h"
#include "groomlab/frames.h"
#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/point_index.h"
#include "groomlab/portable_math.h"
#include "groomlab/random.h"
#include "incremental.h"
#include "internal.h"

namespace groomlab {

namespace {

constexpr float kTwoPi = 6.2831853f;

enum class Style { Three, Fishtail, Rope, Cornrow };

Style styleFrom(const std::string& name) {
    if (name == "three") return Style::Three;
    if (name == "fishtail") return Style::Fishtail;
    if (name == "rope") return Style::Rope;
    if (name == "cornrow") return Style::Cornrow;
    throw GraphError("Braid: style must be three, fishtail, rope or cornrow");
}

// Sections: `sections` (0: the style's own), fishtail always two.
int lobeCount(Style s, std::int64_t sections) {
    if (s == Style::Fishtail) return 2;
    const int base = s == Style::Rope ? 2 : 3;
    return sections <= 0 ? base : static_cast<int>(std::clamp<std::int64_t>(sections, 2, 8));
}

// A lobe's center across the braid ([0]: side to side, [1]: depth, out of the
// scalp), in braid widths, at weave phase `theta`. Woven braids send each of
// `lobes` sections across in turn: two sections meeting side to side are
// always at opposite depths, so one crosses over the other.
std::array<float, 2> lobeCenter(Style style, int lobe, int lobes, float theta, float depthSign) {
    switch (style) {
        case Style::Three:
        case Style::Cornrow: {
            const float phase = theta + kTwoPi * static_cast<float>(lobe) / static_cast<float>(lobes);
            float depth = 0.35f * pm::sin(2.0f * phase) * depthSign;
            if (style == Style::Cornrow) depth = 0.25f * (0.5f + 0.5f * pm::sin(2.0f * phase));  // above the scalp
            return {0.55f * pm::sin(phase), depth};
        }
        case Style::Fishtail: {
            // Two sections side by side; each sends a strip across, alternating.
            const float side = lobe == 0 ? -1.0f : 1.0f;
            const float phase = 3.0f * theta + 3.14159265f * static_cast<float>(lobe);
            return {side * (0.28f - 0.12f * pm::sin(phase)), 0.18f * pm::sin(2.0f * phase) * depthSign};
        }
        case Style::Rope: {  // plies twisted around each other: wider with more of them
            const float phase = theta + kTwoPi * static_cast<float>(lobe) / static_cast<float>(lobes);
            const float r = 0.3f + 0.06f * static_cast<float>(lobes - 2);
            return {r * pm::cos(phase), r * pm::sin(phase) * depthSign};
        }
    }
    return {0.0f, 0.0f};
}

// How much faster than the weave a style's lobes move (a fishtail's strips cross three times a
// repeat): the points a repeat needs scale with it.
float styleRate(Style s) { return s == Style::Fishtail ? 3.0f : 1.0f; }

struct BraidPath {
    std::vector<Vec3f> points;  // resampled evenly
    std::vector<Frame> frames;
    float length = 0.0f;
    Vec3f root;
    StrandId id = 0;           // the guide's (a picked strand's) ID
    std::vector<float> turns;  // weave travelled at each point (tightness integrated); empty: the distance
    float repeats = 0.0f;      // weave repeats along the whole path
    std::vector<float> grown;  // feed growth: the width's factor at each point (empty: 1)

    // Weave travelled `at` a distance along the path (past its end: at the end's rate).
    float weaveAt(float at, const Ramp& tightness) const {
        if (turns.empty()) return at;
        const std::size_t last = points.size() - 1;
        if (at >= length) return turns.back() + (at - length) * tightness(1.0f);
        const float f = std::max(at, 0.0f) / length * static_cast<float>(last);
        const std::size_t k = std::min(static_cast<std::size_t>(f), last - 1);
        const float w = f - static_cast<float>(k);
        return turns[k] * (1.0f - w) + turns[k + 1] * w;
    }
    // The width's factor for the hair held `at` a distance along the path.
    float grownAt(float at) const {
        if (grown.empty()) return 1.0f;
        const std::size_t last = points.size() - 1;
        const float f = std::clamp(at / length, 0.0f, 1.0f) * static_cast<float>(last);
        const std::size_t k = std::min(static_cast<std::size_t>(f), last - 1);
        const float w = f - static_cast<float>(k);
        return grown[k] * (1.0f - w) + grown[k + 1] * w;
    }
};

// Everything that shapes the weave, shared by both modes.
struct BraidLook {
    Style style;
    int lobes;
    float depthSign, depth, phase, sectionTwist;
    Ramp tightness;

    // A strand's offset from the path, in the frame's (depth, side) plane, before the fan-out.
    Vec3f offset(int lobe, const Vec3f& inLobe, float weave, float wid) const {
        const float theta = kTwoPi * (weave + phase);
        const auto lc = lobeCenter(style, lobe, lobes, theta, depthSign);
        Vec3f spot = inLobe;
        if (sectionTwist != 0.0f) {  // the section's hair turning around its own center
            const float a = sectionTwist * theta, c = pm::cos(a), s = pm::sin(a);
            spot = {inLobe.x * c - inLobe.y * s, inLobe.x * s + inLobe.y * c, 0.0f};
        }
        // Cornrows lie on the scalp: their strands only ever stack outward.
        const float lobeDepth = style == Style::Cornrow ? std::abs(spot.y) * 0.6f : spot.y;
        return {(lc[0] + spot.x) * wid, (lc[1] + lobeDepth) * depth * wid, 0.0f};
    }

    // The lobe at `share` of the way across the braid (0: its one side, 1: the other, along the
    // frame's binormal) at `weave`: the lobes ranked side to side there, an equal share each.
    int lobeAcross(float share, float weave) const {
        const float theta = kTwoPi * (weave + phase);
        std::array<int, 8> order{};
        std::array<float, 8> side{};
        for (int k = 0; k < lobes; ++k) {
            order[static_cast<std::size_t>(k)] = k;
            side[static_cast<std::size_t>(k)] = lobeCenter(style, k, lobes, theta, depthSign)[0];
        }
        std::sort(order.begin(), order.begin() + lobes, [&](int a, int b) {
            const float sa = side[static_cast<std::size_t>(a)], sb = side[static_cast<std::size_t>(b)];
            return sa < sb || (sa == sb && a < b);
        });
        const int rank = std::clamp(static_cast<int>(share * static_cast<float>(lobes)), 0, lobes - 1);
        return order[static_cast<std::size_t>(rank)];
    }
    // The lobe a strand whose root lies `offset` from where it joins the braid (frame `fr`) takes:
    // the root region cut into equal wedges side to side, around the join, one a lobe (`fallback`:
    // a root on the path itself).
    int lobeOfRoot(const Vec3f& offset, const Frame& fr, float weave, int fallback) const {
        const float d = offset.length();
        if (!(d > 0.0f)) return fallback;
        const float t = std::clamp(offset.dot(fr.binormal) / d, -1.0f, 1.0f);
        return lobeAcross(pm::acos(-t) / 3.14159265f, weave);
    }
};

// The gathering stretch pressed over the scalp: a point on its way into the
// braid (blend strictly between 0 and 1) is held `clearance` above the scalp,
// rising to where the braid has it as the blend completes; a point that would
// cut through the head is lifted out. Only these points query the scalp.
struct ScalpGather {
    const ScalpMesh* scalp = nullptr;  // null: straight lines
    float clearance = 0.0f;

    void apply(Vec3f& p, float blend) const {
        if (!scalp || !(blend > 0.0f) || !(blend < 1.0f)) return;
        const SurfacePoint sp = scalp->closestPoint(p);
        if (sp.root.face < 0) return;
        const float height = (p - sp.position).dot(sp.normal);
        const float held = clearance + std::max(height - clearance, 0.0f) * blend;
        p = p + sp.normal * (held - height);
    }
};

// Per-strand output channels for nodes downstream.
struct BraidChannels {
    std::vector<float>* id;    // the path braided into, -1 when loose
    std::vector<float>* flat;  // 1 when the braid lies on the scalp (cornrow)
    int first = 0;             // this node's ids start here (after an earlier Braid node's)
    // Paths of its own: each path's number, from its strand's ID (stableGroupNumbers), so an edit
    // of the scalp in one place renumbers no braid elsewhere; null: the path's index (the guides').
    const std::vector<float>* numbers = nullptr;
    float number(int path) const {
        return static_cast<float>(first) + (numbers ? (*numbers)[static_cast<std::size_t>(path)] : static_cast<float>(path));
    }
    void set(std::size_t s, int path, Style style) const {
        (*id)[s] = number(path);
        (*flat)[s] = style == Style::Cornrow ? 1.0f : 0.0f;
    }
};

// A strand's own draws, by its ID: whether it stays loose, a lobe for a root with no side, its
// spot within its lobe, how its end fans out and wanders.
struct StrandDraws {
    bool stray = false;
    int lobe = 0;
    Vec3f inLobe;
    float fanAngle = 0.0f, wanderPhase = 0.0f;
    float endDraw = 0.0f;  // with endAtPath: where in the tail it ends
    float fan = 0.9f;      // the fan-out's reach, in braid widths
    float fuzz = 0.0f;     // the wander, scene units

    StrandDraws() = default;
    StrandDraws(std::uint64_t id, std::uint64_t seed, float strays, int lobes, float lobeFill, float wander) : fuzz(wander) {
        Rng rng(hashCombine(id, seed ^ 0xB4A1D5ull));
        stray = rng.nextFloat() < strays;
        lobe = static_cast<int>(rng.nextU64() % static_cast<std::uint64_t>(lobes));
        const float a = kTwoPi * rng.nextFloat();
        const float r = lobeFill * std::sqrt(rng.nextFloat());
        inLobe = {r * pm::cos(a), r * pm::sin(a), 0.0f};
        fanAngle = kTwoPi * rng.nextFloat();
        wanderPhase = kTwoPi * rng.nextFloat();
        endDraw = rng.nextFloat();
    }
};

// The points a braided strand gets: enough for `perRepeat` a weave repeat over the share of it
// that weaves, at most `most` segments, never fewer than it came with.
struct Sampling {
    float perRepeat = 0.0f;  // 0: the strands' own points
    std::size_t most = 512;

    std::size_t count(std::size_t n, float repeats, float share) const {
        if (!(perRepeat > 0.0f) || n < 2 || !(repeats > 0.0f)) return n;
        const float want = std::ceil(perRepeat * repeats / std::max(share, 1e-3f));
        const auto segments = static_cast<std::size_t>(std::min(want, static_cast<float>(most)));
        return std::max(n, segments + 1);
    }
};

// Where a fed strand joins its braid and how it runs along it (Braider::planFed).
struct FedPlan {
    int path = -1;            // -1: stays as it is
    std::size_t joinK = 0;    // the path point it joins at
    float joinAt = 0.0f;      // that distance along the path
    float approach = 0.0f;    // the distance along the strand it reaches the braid at
    float blendFrom = 0.0f, blendTo = 0.0f;
    float squeeze = 1.0f;     // its length fitted to the path
    bool drawnIn = false;     // drawn from its root (not following its own shape in)
    bool region = false;      // joined from the root region (not picked up along the path)
    float strandLength = 0.0f;
    StrandDraws draws;
};

bool isConstant(const Ramp& r) {
    for (const auto& key : r.keys) {
        if (key.second != r.keys.front().second) return false;
    }
    return true;
}

// The braid's settings and paths: where each strand joins and where its points go.
struct Braider {
    BraidLook look;
    float width = 0.0f;
    Ramp taper;
    float crossings = 0.0f, lobeFill = 0.0f, region2 = 0.0f, gather = 0.0f, tail = 0.0f, strays = 0.0f, fuzz = 0.0f;
    std::uint64_t seed = 0;
    Sampling sampling;
    ScalpGather overScalp;
    float feedDistance2 = 0.0f, feedUntil = 0.0f;
    bool endAtPath = true;
    std::vector<BraidPath> paths;

    float weaveAt(const BraidPath& path, float at) const { return crossings * path.weaveAt(at, look.tightness); }

    // The braid's point `at` a distance along `path` for a strand in `lobe` (-1: the path itself)
    // with draws `d`: the lobe's offset in the path's frame, letting go and fanning out over the tail.
    Vec3f braidedAt(const BraidPath& path, float at, int lobe, const StrandDraws& d) const {
        const std::size_t last = path.points.size() - 1;
        Vec3f center;
        const Frame* fr;
        if (at < path.length) {
            const float f = at / path.length * static_cast<float>(last);
            const std::size_t k = std::min(static_cast<std::size_t>(f), last - 1);
            const float w = f - static_cast<float>(k);
            center = path.points[k] * (1.0f - w) + path.points[k + 1] * w;
            fr = &path.frames[w < 0.5f ? k : k + 1];
        } else {  // hanging on past the end
            center = path.points[last] + (path.points[last] - path.points[last - 1]).normalized() * (at - path.length);
            fr = &path.frames[last];
        }
        if (lobe < 0) return center;
        const float u = std::min(at / path.length, 1.0f);
        const float wid = width * std::max(taper(u), 0.0f) * path.grownAt(at);
        const float letGo = tail > 0.0f ? smoothstep((1.0f - tail) * path.length, path.length, at) : 0.0f;
        const float wander = d.fuzz * pm::sin(d.wanderPhase + 7.0f * u);
        const Vec3f local = look.offset(lobe, d.inLobe, weaveAt(path, at), wid) * (1.0f - letGo);
        const float fanDepth = look.style == Style::Cornrow ? std::abs(pm::sin(d.fanAngle)) : pm::sin(d.fanAngle);
        const Vec3f fan = Vec3f{pm::cos(d.fanAngle), fanDepth, 0.0f} * (letGo * wid * d.fan);
        // Frame x is the normal (depth, out of the scalp), y the binormal (side to side).
        return center + fr->toWorld({local.y + fan.y, local.x + fan.x + wander, 0.0f});
    }

    // Without feed: the path a strand rooted at `root` joins, the nearest root within the region
    // (-1: none).
    int pathOf(const Vec3f& root) const {
        int best = -1;
        float bestD2 = region2;
        for (std::size_t b = 0; b < paths.size(); ++b) {
            const float d2 = (paths[b].root - root).dot(paths[b].root - root);
            if (d2 <= bestD2) {
                bestD2 = d2;
                best = static_cast<int>(b);
            }
        }
        return best;
    }
    // Without feed, the points strand s of `in` is braided with: as many as it has, or for one
    // braided (not loose, not a stray) the points its path's weave needs.
    std::size_t pointsFor(const StrandSet& in, std::size_t s) const {
        const std::size_t n = in.strandPointCount(s);
        const int path = n < 2 ? -1 : pathOf(in.strandPoints(s)[0]);
        if (path < 0 || StrandDraws(in.ids()[s], seed, strays, look.lobes, lobeFill, fuzz).stray) return n;
        return sampling.count(n, paths[static_cast<std::size_t>(path)].repeats, 1.0f);
    }
    // Without feed, the strand's points along its path from root to tip (pts[0], its root, stays).
    void braidAlong(Vec3f* pts, std::size_t n, const BraidPath& path, const StrandDraws& d) const {
        // The section on its side where it comes in, halfway through the gather.
        const float in = 0.5f * gather * path.length;
        const int lobe = look.lobeOfRoot(pts[0] - path.root, path.frames.front(), weaveAt(path, in), d.lobe);
        for (std::size_t i = 1; i < n; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(n - 1);
            const Vec3f braided = braidedAt(path, u * path.length, lobe, d);
            const float blend = smoothstep(0.0f, gather, u);  // from the root into the braid
            pts[i] = pts[i] * (1.0f - blend) + braided * blend;
            overScalp.apply(pts[i], blend);
        }
    }
};

// Feed mode's lookups: the path samples a strand may be picked up at (the first `feedUntil` of
// each path, past the root: that is the region's) in one index, the roots in another, and each
// path's own (where a strand first reaches its braid).
struct FeedIndex {
    std::vector<std::pair<std::size_t, std::size_t>> sampleOf;  // (path, sample)
    std::vector<PointIndex> pathIndex;
    PointIndex pickUp, rootIndex;
    std::size_t rootCount = 0;

    static std::vector<Vec3f> samplesOf(const std::vector<BraidPath>& paths, float feedUntil,
                                        std::vector<std::pair<std::size_t, std::size_t>>& sampleOf) {
        std::vector<Vec3f> samples;
        for (std::size_t b = 0; b < paths.size(); ++b) {
            const std::size_t last = paths[b].points.size() - 1;
            const std::size_t until = static_cast<std::size_t>(feedUntil * static_cast<float>(last) + 1e-3f);
            for (std::size_t k = 1; k <= std::min(until, last); ++k) {
                samples.push_back(paths[b].points[k]);
                sampleOf.emplace_back(b, k);
            }
        }
        return samples;
    }
    static std::vector<Vec3f> rootsOf(const std::vector<BraidPath>& paths) {
        std::vector<Vec3f> roots;
        for (const BraidPath& p : paths) roots.push_back(p.root);
        return roots;
    }
    FeedIndex(const std::vector<BraidPath>& paths, float feedUntil)
        : pickUp(samplesOf(paths, feedUntil, sampleOf)), rootIndex(rootsOf(paths)), rootCount(paths.size()) {
        for (const BraidPath& p : paths) pathIndex.emplace_back(p.points);
    }
};

// Feed mode: where strand `id` (points `pts`) joins a path: where it reaches it, or at the point
// nearest its root; `along` gets the arc length from its root to each point.
FedPlan planFed(const Braider& br, const FeedIndex& index, const Vec3f* pts, std::size_t n, std::uint64_t id,
                std::vector<float>& along) {
    FedPlan plan;
    if (n < 2) return plan;
    // Picked up at the nearest path point within feedDistance, or from the nearest root within
    // region: whichever is closer.
    int best = -1;
    std::size_t bestK = 0;
    float bestD2 = std::numeric_limits<float>::infinity();
    {
        float d2 = 0.0f;
        const std::size_t q = index.pickUp.nearest(pts[0], &d2);
        if (q < index.sampleOf.size() && d2 <= br.feedDistance2) {
            best = static_cast<int>(index.sampleOf[q].first);
            bestK = index.sampleOf[q].second;
            bestD2 = d2;
        }
        const std::size_t r = index.rootIndex.nearest(pts[0], &d2);
        if (r < index.rootCount && d2 <= br.region2 && d2 < bestD2) {
            best = static_cast<int>(r);
            bestK = 0;
            bestD2 = d2;
        }
    }
    if (best < 0) return plan;
    plan.draws = StrandDraws(id, br.seed, br.strays, br.look.lobes, br.lobeFill, br.fuzz);
    if (plan.draws.stray) return plan;
    const BraidPath& path = br.paths[static_cast<std::size_t>(best)];
    const std::size_t last = path.points.size() - 1;
    const float step = path.length / static_cast<float>(last);
    along.resize(n);
    cumulativeArcLength(pts, n, along.data());
    plan.strandLength = along[n - 1];
    const float gatherLength = std::max(std::max(br.gather, 1e-4f) * plan.strandLength, 1e-30f);  // shares of the strand

    // Where the strand joins: where its own shape (e.g. guides combed toward the braid) first
    // comes within the braid, at the path point there; failing that, from its root straight to
    // the nearest point.
    float reach = -1.0f;  // distance along the strand
    std::size_t joinK = bestK;
    {
        const float within = 0.6f * br.width * std::max(br.taper(0.0f), 0.1f);
        const PointIndex& own = index.pathIndex[static_cast<std::size_t>(best)];
        for (std::size_t i = 0; i < n && reach < 0.0f; ++i) {
            float d2 = 0.0f;
            const std::size_t k = own.nearest(pts[i], &d2);
            if (k <= last && d2 <= within * within) {
                reach = along[i];
                joinK = k;
            }
        }
    }
    plan.joinK = joinK;
    plan.joinAt = static_cast<float>(joinK) * step;
    plan.region = bestK == 0;
    plan.drawnIn = reach < 0.0f;
    if (!plan.drawnIn) {  // following its own shape into the braid
        plan.approach = reach;
        plan.blendFrom = std::max(0.0f, reach - 0.5f * gatherLength);
        plan.blendTo = reach + 0.5f * gatherLength;
    } else {  // drawn in from the root
        plan.approach = std::sqrt(bestD2);
        plan.blendFrom = 0.0f;
        plan.blendTo = std::max(gatherLength, plan.approach);
    }
    // Fitted to the path: the strand's length is shared out so it ends within the braid's last
    // stretch (a random spot in the tail, so the end isn't cut square), never past it. A strand
    // joining at or past that end has no braid to run along: it stays loose.
    if (br.endAtPath) {
        const float endAt = path.length * (1.0f - 0.5f * br.tail * plan.draws.endDraw);
        if (plan.joinAt >= endAt) return plan;
        const float room = plan.approach + endAt - plan.joinAt;
        if (plan.strandLength > room && plan.strandLength > 1e-6f) plan.squeeze = room / plan.strandLength;
    }
    plan.path = best;
    return plan;
}

class BraidOp : public Operator {
public:
    std::string typeName() const override { return op::kBraid; }
    std::vector<ChannelWrite> channelWrites() const override {
        return {{"braidId", ""}, {"braidFlat", ""}, {"braidSection", ""}};
    }
    std::string category() const override { return "look"; }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::vector<PortSpec> inputs() const override {
        return {{"strands", PortType::Strands}, {"braids", PortType::Guides, true}};
    }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            {"style", std::string("three"), "three, fishtail, rope or cornrow", "", {"three", "fishtail", "rope", "cornrow"}},
            notAnimatable({"sections", std::int64_t{0}, "Sections woven, 2-8 (0: the style's own: three for braids and cornrows, two "
                                                        "plies for rope; fishtail is always two)"})
                .range(0, 8)
                .when("style", {"three", "rope", "cornrow"}),
            ParamSpec{"dutch", false, "Cross the lobes under instead of over (a raised braid)"}
                .when("style", {"three", "fishtail", "rope"}),
            ParamSpec{"depth", 1.0, "The braid's thickness out of the scalp against its width: below 1 flatter, above "
                                    "rounder"}
                .atLeast(0)
                .soft(0, 2)
                .in("Weave"),
            ParamSpec{"phase", 0.0, "Slides the weave along the braid, in repeats (0.5: half a crossing)", "turns"}
                .soft(0, 1)
                .in("Weave"),
            ParamSpec{"crossingsProfile", Ramp::constant(1.0f), "Crossings along the braid, start to end, times `crossings`"}
                .in("Weave"),
            ParamSpec{"sectionTwist", 0.0, "Turns of each section's hair around its own center per weave repeat "
                                           "(signed)", "turns"}
                .soft(-2, 2)
                .in("Weave"),
            notAnimatable(ParamSpec{"resolution", 12.0, "Points per weave repeat: a braided strand with fewer gets points "
                                                        "added, so the weave is drawn smoothly however few points the "
                                                        "strands came with (0: their own points)"})
                .atLeast(0)
                .soft(0, 32)
                .in("Weave"),
            notAnimatable({"maxSegments", std::int64_t{512}, "The most segments a braided strand may have with the points "
                                                             "Braid adds"})
                .range(1, 4096)
                .in("Weave")
                .adv(),
            ParamSpec{"width", 0.12, "Braid width at its start, scene units", "length"}.soft(0, 0.5),
            {"widthProfile", Ramp::linear(1.0f, 0.55f), "Width along the braid, start to end"},
            ParamSpec{"crossings", 2.5, "Weave repeats per unit of braid length (tighter: more)", "perLength"}.soft(0, 10),
            ParamSpec{"lobeFill", 0.6, "How much of a lobe's space its strands fill, 0 to 1", "fraction"}
                .range(0, 1)
                .in("Weave"),
            ParamSpec{"region", 0.25, "Strands rooted within this distance of a braid's root join it (large: a "
                                      "ponytail's); without a braids input, the braids it picks among the strands are "
                                      "about this far apart", "length"}
                .soft(0, 2),
            ParamSpec{"feed", false, "Pick up strands all along the path, where they grow (cornrows, French braids)"}
                .in("Feed"),
            ParamSpec{"feedDistance", 0.25, "With feed: strands rooted within this distance of the path join it where "
                                            "they grow", "length"}
                .soft(0, 1)
                .in("Feed")
                .when("feed", {"true"}),
            ParamSpec{"feedUntil", 1.0, "With feed: pick up strands along this share of the path only; past it the "
                                        "braid hangs free (0: only the root region, a ponytail braid)", "fraction"}
                .range(0, 1)
                .in("Feed")
                .when("feed", {"true"}),
            ParamSpec{"feedGrowth", 1.0, "With feed: the braid thickens with the hair it has picked up (1: its width "
                                         "follows the share of its hair held so far; 0: widthProfile alone)", "fraction"}
                .range(0, 1)
                .in("Feed")
                .when("feed", {"true"}),
            ParamSpec{"endAtPath", true, "With feed: strands end where the braid path ends (staggered over the tail) "
                                         "instead of hanging on past it"}
                .in("Feed")
                .when("feed", {"true"}),
            ParamSpec{"alongScalp", true, "Strands drawn in from afar sweep over the scalp into the braid instead of "
                                          "cutting through the head"}
                .in("Strands"),
            ParamSpec{"gather", 0.15, "Share of the strand spent leaving the scalp into the braid", "fraction"}
                .range(0, 0.9)
                .in("Strands"),
            ParamSpec{"tail", 0.12, "Share at the end where the lobes let go and the ends fan out", "fraction"}
                .range(0, 0.9)
                .in("Strands"),
            ParamSpec{"strays", 0.03, "Share of the region's strands left loose", "fraction"}.range(0, 1).in("Strands"),
            ParamSpec{"fuzz", 0.02, "Random wander within the lobes, scene units", "length"}.soft(0, 0.1).in("Strands"),
            notAnimatable({"seed", std::int64_t{0}, "Random seed"}).in("Strands").adv(),
            {"display", std::string("hair"), "hair: the braided strands; paths: each braid's path and its sections' "
                                             "center curves in their place, to see the weave",
             "", {"hair", "paths"}},
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    // Without feed each strand is braided by its own root alone (the nearest path's root within
    // the region): while the paths stay, the strands that changed are braided again (each to as
    // many points as before, or it evaluates in full) and the rest keep the last result's points.
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

private:
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = valueAs<StrandsPtr>(inputs.at(0));
        if (!in) throw GraphError("Braid: strands input not connected");
        GuidesPtr braids = valueAs<GuidesPtr>(inputs.at(1));
        auto handOn = [&] {  // nothing to braid along: the graph passes the input's changes on
            if (incremental) return false;
            *result = in;
            return true;
        };
        const ScalpMesh* scalp = valueAs<MeshPtr>(ctx.external("scalp")).get();
        const bool own = !braids;
        if (own) {  // no braids input: paths of its own, strands about `region` apart
            if (incremental) return false;
            const float spacing = static_cast<float>(params().getDouble("region"));
            if (!(spacing > 0.0f) || in->empty()) return handOn();
            auto picked = std::make_shared<GuideSet>();
            for (const std::size_t s : pickSpreadStrands(*in, scalp, spacing, static_cast<std::uint64_t>(params().getInt("seed")))) {
                const Vec3f* pts = in->strandPoints(s);
                picked->add(in->ids()[s], std::vector<Vec3f>(pts, pts + in->strandPointCount(s))).root = in->roots()[s];
            }
            braids = GuidesPtr(picked);
        }
        if (braids->empty()) return handOn();

        const Style style = styleFrom(params().getString("style"));
        Braider br;
        br.look = BraidLook{style,
                            lobeCount(style, params().getInt("sections")),
                            params().getBool("dutch") ? -1.0f : 1.0f,
                            static_cast<float>(params().getDouble("depth")),
                            static_cast<float>(params().getDouble("phase")),
                            static_cast<float>(params().getDouble("sectionTwist")),
                            params().getRamp("crossingsProfile")};
        br.width = static_cast<float>(params().getDouble("width"));
        br.taper = params().getRamp("widthProfile");
        br.crossings = static_cast<float>(params().getDouble("crossings"));
        br.lobeFill = static_cast<float>(params().getDouble("lobeFill"));
        const float region = static_cast<float>(params().getDouble("region"));
        br.region2 = region * region;
        br.gather = static_cast<float>(params().getDouble("gather"));
        br.tail = static_cast<float>(params().getDouble("tail"));
        br.strays = static_cast<float>(params().getDouble("strays"));
        br.fuzz = static_cast<float>(params().getDouble("fuzz"));
        br.seed = static_cast<std::uint64_t>(params().getInt("seed"));
        // Points a repeat: the resolution, for the fastest-moving part of the weave.
        br.sampling.perRepeat = static_cast<float>(params().getDouble("resolution")) * styleRate(style) *
                                std::max(1.0f, std::abs(br.look.sectionTwist));
        br.sampling.most = static_cast<std::size_t>(std::max<std::int64_t>(1, params().getInt("maxSegments")));
        const bool feed = params().getBool("feed");
        if (feed) {
            const float feedDistance = static_cast<float>(params().getDouble("feedDistance"));
            br.feedDistance2 = feedDistance * feedDistance;
            br.feedUntil = static_cast<float>(params().getDouble("feedUntil"));
            br.endAtPath = params().getBool("endAtPath");
        }
        const bool showPaths = params().getString("display") == "paths";

        // Braid paths, evenly resampled, with frames facing out of the scalp.
        for (const Guide& g : braids->guides()) {
            if (g.points.size() < 2) continue;
            BraidPath p;
            const std::size_t samples = std::max<std::size_t>(64, g.points.size() * 4);
            p.points = resampleByArcLength(g.points.data(), g.points.size(), samples);
            p.length = arcLength(p.points.data(), p.points.size());
            if (!(p.length > 1e-6f)) continue;  // also NaN
            p.root = p.points.front();
            p.id = g.id;
            const bool rooted = scalp && scalp->contains(g.root);
            // Out of the scalp: at the root, or where the path starts when it is not bound.
            Vec3f up = rooted ? scalp->normal(g.root)
                              : (scalp && scalp->triangleCount() > 0 ? scalp->closestPoint(p.root).normal
                                                                     : anyPerpendicular((p.points[1] - p.points[0]).normalized()));
            // The frame's normal: the scalp normal, made perpendicular to the path's start.
            const Vec3f t0 = (p.points[1] - p.points[0]).normalized();
            up = up - t0 * up.dot(t0);
            if (up.length() < 1e-4f) up = anyPerpendicular(t0);
            p.frames = rotationMinimizingFrames(p.points.data(), p.points.size(), up.normalized());
            if (!isConstant(br.look.tightness) || br.look.tightness(0.0f) != 1.0f) {
                // The weave travelled along the path: tightness integrated over its length.
                const std::size_t count = p.points.size();
                const float step = p.length / static_cast<float>(count - 1);
                p.turns.assign(count, 0.0f);
                for (std::size_t k = 1; k < count; ++k) {
                    const float u0 = static_cast<float>(k - 1) / static_cast<float>(count - 1);
                    const float u1 = static_cast<float>(k) / static_cast<float>(count - 1);
                    p.turns[k] = p.turns[k - 1] + 0.5f * (std::max(br.look.tightness(u0), 0.0f) +
                                                          std::max(br.look.tightness(u1), 0.0f)) * step;
                }
            }
            p.repeats = br.weaveAt(p, p.length);
            br.paths.push_back(std::move(p));
        }
        if (br.paths.empty()) return handOn();

        // Incrementally: the same paths, no feed, no earlier Braid's ids (numbered on from their
        // highest), the hair shown.
        const StrandChanges* changes = nullptr;
        StrandsPtr last;
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = valueAs<StrandsPtr>(incremental->previous);
            if (!changes || !last || last->strandCount() != in->strandCount() || feed || showPaths ||
                !inputUnchanged(*incremental, 1) || in->strandChannel("braidId")) {
                return false;
            }
            if (changes->none()) {
                *result = last;
                *changed = *changes;
                return true;
            }
        }

        // A Braid node after another keeps the earlier braids' ids and numbers its own on from
        // them, so every braid in the groom has an id of its own (the Cards node's card sets).
        int firstId = 0;
        std::vector<float> earlierIds, earlierFlat;
        if (const std::vector<float>* had = in->strandChannel("braidId")) {
            earlierIds = *had;
            if (const std::vector<float>* flat = in->strandChannel("braidFlat")) earlierFlat = *flat;
            float highest = -1.0f;  // of the core strands (preview_core.h): the same at every preview density
            const PreviewCore core(*in);
            for (std::size_t s = 0; s < earlierIds.size(); ++s) {
                if (core.core(s)) highest = std::max(highest, std::floor(earlierIds[s]));
            }
            firstId = static_cast<int>(groupNumberBase(static_cast<std::int64_t>(highest)));
        }
        BraidChannels channels{nullptr, nullptr, firstId};
        std::vector<float> numbers;
        if (own) {
            std::vector<std::uint64_t> ids(br.paths.size());
            for (std::size_t b = 0; b < br.paths.size(); ++b) ids[b] = br.paths[b].id;
            numbers = stableGroupNumbers(ids);
            channels.numbers = &numbers;
        }
        if (params().getBool("alongScalp") && scalp && scalp->triangleCount() > 0) {
            br.overScalp.scalp = scalp;
            // (at least a hundredth of the width: a share of the braid, whatever the scene's scale)
            br.overScalp.clearance = 0.25f * br.width * std::max(br.taper(0.0f), 0.04f);
        }

        const std::size_t count = in->strandCount();
        std::vector<std::size_t> counts(count);
        if (feed) {
            // Where each strand joins (on its points as they came), for the points it needs and
            // how much of each braid's hair it holds where.
            const FeedIndex index(br.paths, br.feedUntil);
            std::vector<FedPlan> plans(count);
            parallelFor(0, count, [&](std::size_t s) {
                thread_local std::vector<float> along;
                const std::size_t n = in->strandPointCount(s);
                plans[s] = planFed(br, index, in->strandPoints(s), n, in->ids()[s], along);
                counts[s] = n;
                const FedPlan& p = plans[s];
                if (p.path < 0) return;
                const BraidPath& path = br.paths[static_cast<std::size_t>(p.path)];
                const float braided = std::max(p.strandLength * p.squeeze - p.approach, 0.0f);
                const float repeats = br.weaveAt(path, p.joinAt + braided) - br.weaveAt(path, p.joinAt);
                const float share = p.strandLength * p.squeeze > 0.0f ? braided / (p.strandLength * p.squeeze) : 1.0f;
                counts[s] = br.sampling.count(n, repeats, share);
            });
            const float growth = static_cast<float>(params().getDouble("feedGrowth"));
            if (growth > 0.0f) {
                // The share of each braid's hair (its core strands: the same at every preview
                // density) joined by each of its points.
                const PreviewCore core(*in);
                std::vector<std::vector<double>> held(br.paths.size());
                for (std::size_t b = 0; b < br.paths.size(); ++b) held[b].assign(br.paths[b].points.size(), 0.0);
                for (std::size_t s = 0; s < count; ++s) {
                    if (plans[s].path >= 0 && core.core(s)) held[static_cast<std::size_t>(plans[s].path)][plans[s].joinK] += 1.0;
                }
                for (std::size_t b = 0; b < br.paths.size(); ++b) {
                    std::vector<double>& h = held[b];
                    for (std::size_t k = 1; k < h.size(); ++k) h[k] += h[k - 1];
                    if (!(h.back() > 0.0)) continue;
                    std::vector<float>& grown = br.paths[b].grown;
                    grown.resize(h.size());
                    for (std::size_t k = 0; k < h.size(); ++k) {
                        const float share = static_cast<float>(h[k] / h.back());
                        grown[k] = 1.0f - growth + growth * std::sqrt(std::max(share, 0.05f));
                    }
                }
            }
        }
        if (showPaths) {
            *result = pathStrands(br, channels);
            return true;
        }

        if (incremental) {
            // Each changed strand braided again into as many points as it had (as the rest keep
            // theirs, their input as it was), else in full.
            std::atomic<bool> moved{false};
            forEachListed(changes->strands, [&](std::size_t s) {
                if (br.pointsFor(*in, s) != last->strandPointCount(s)) moved = true;
            });
            if (moved) return false;
            auto made = copyOfLast(last, *incremental);
            const bool widths = in->widths().size() == in->pointCount();
            std::vector<float>& outWidths = made->mutableWidths();
            std::vector<std::vector<float>*> outPoint;
            std::vector<const std::vector<float>*> inPoint;
            for (const std::string& name : in->pointChannelNames()) {
                outPoint.push_back(made->copyPointChannelFrom(name, *last));
                inPoint.push_back(in->pointChannel(name));
                if (!outPoint.back()) return false;
            }
            std::vector<std::pair<const std::vector<float>*, std::vector<float>*>> strandValues;
            for (const std::string& name : in->strandChannelNames()) {
                std::vector<float>* to = made->copyStrandChannelFrom(name, *last);
                if (!to) return false;
                strandValues.emplace_back(in->strandChannel(name), to);
            }
            channels.id = made->copyStrandChannelFrom("braidId", *last);
            channels.flat = made->copyStrandChannelFrom("braidFlat", *last);
            if (!channels.id || !channels.flat) return false;
            forEachListed(changes->strands, [&](std::size_t s) {
                for (const auto& [from, to] : strandValues) (*to)[s] = (*from)[s];
                resampleStrand(*in, *made, s, false, widths, inPoint, outPoint, outWidths);
                (*channels.id)[s] = -1.0f;
                (*channels.flat)[s] = 0.0f;
                braidOne(br, *made, s, channels);
            });
            *result = StrandsPtr(made);
            *changed = *changes;
            return true;
        }

        if (!feed) {
            parallelFor(0, count, [&](std::size_t s) { counts[s] = br.pointsFor(*in, s); });
        }
        // The strands given the points they need (each evenly along itself, as Resample does).
        bool adds = false;
        for (std::size_t s = 0; s < count && !adds; ++s) adds = counts[s] != in->strandPointCount(s);
        // (A set of its own when made here: nobody else holds it.)
        const std::shared_ptr<StrandSet> out =
            adds ? std::const_pointer_cast<StrandSet>(resampleStrands(*in, counts)) : std::make_shared<StrandSet>(*in);
        channels.id = &out->addStrandChannel("braidId", -1.0f);
        channels.flat = &out->addStrandChannel("braidFlat", 0.0f);
        if (!earlierIds.empty()) {
            *channels.id = earlierIds;
            if (earlierFlat.size() == channels.flat->size()) *channels.flat = earlierFlat;
        }
        if (feed) {
            const FeedIndex index(br.paths, br.feedUntil);
            parallelFor(0, count, [&](std::size_t s) {
                thread_local std::vector<float> along;
                Vec3f* pts = out->strandPoints(s);
                const std::size_t n = out->strandPointCount(s);
                const FedPlan plan = planFed(br, index, pts, n, out->ids()[s], along);
                if (plan.path < 0) return;
                channels.set(s, plan.path, style);
                feedOne(br, plan, pts, n, along);
            });
        } else {
            parallelFor(0, count, [&](std::size_t s) { braidOne(br, *out, s, channels); });
        }
        *result = StrandsPtr(out);
        return true;
    }

    // Without feed: strand s of `out` braided along the path it joins, if any.
    static void braidOne(const Braider& br, StrandSet& out, std::size_t s, const BraidChannels& channels) {
        Vec3f* pts = out.strandPoints(s);
        const std::size_t n = out.strandPointCount(s);
        if (n < 2) return;
        const int best = br.pathOf(pts[0]);
        if (best < 0) return;
        const StrandDraws draws(out.ids()[s], br.seed, br.strays, br.look.lobes, br.lobeFill, br.fuzz);
        if (draws.stray) return;
        channels.set(s, best, br.look.style);
        br.braidAlong(pts, n, br.paths[static_cast<std::size_t>(best)], draws);
    }

    // Feed mode: a strand joins the path where it reaches it (or at the point nearest its root)
    // and runs along it by its own length.
    static void feedOne(const Braider& br, const FedPlan& plan, Vec3f* pts, std::size_t n, const std::vector<float>& along) {
        const BraidPath& path = br.paths[static_cast<std::size_t>(plan.path)];
        // Picked up along the path: the outer section on its side, as a braider adds hair; from
        // the root region: the section on its side of the region.
        const Vec3f offset = pts[0] - path.points[plan.joinK];
        const Frame& fr = path.frames[plan.joinK];
        const float weave = br.weaveAt(path, plan.joinAt);
        int lobe = plan.draws.lobe;
        if (plan.region) {
            lobe = br.look.lobeOfRoot(offset, fr, weave, lobe);
        } else if (const float side = offset.dot(fr.binormal); side != 0.0f) {
            lobe = br.look.lobeAcross(side < 0.0f ? 0.0f : 1.0f, weave);
        }
        for (std::size_t i = 1; i < n; ++i) {
            const float fitted = along[i] * plan.squeeze;
            const float at = plan.joinAt + std::max(0.0f, fitted - plan.approach);  // distance along the path
            const Vec3f braided = br.braidedAt(path, at, lobe, plan.draws);
            const float blend = smoothstep(plan.blendFrom, plan.blendTo, fitted);
            pts[i] = pts[i] * (1.0f - blend) + braided * blend;
            if (plan.drawnIn) br.overScalp.apply(pts[i], blend);
        }
    }

    // display paths: each braid's path and the center curve of each of its sections, as strands
    // (braidId the braid's, braidSection -1 for the path, else the section).
    static StrandsPtr pathStrands(const Braider& br, const BraidChannels& numbering) {
        const std::size_t perPath = static_cast<std::size_t>(br.look.lobes) + 1;
        std::vector<std::size_t> counts;
        for (const BraidPath& p : br.paths) {
            const Sampling fine{std::max(br.sampling.perRepeat, 12.0f * styleRate(br.look.style)), br.sampling.most};
            counts.insert(counts.end(), perPath, fine.count(p.points.size(), p.repeats, 1.0f));
        }
        auto out = std::make_shared<StrandSet>();
        out->resize(counts, StrandSet::kLeavePositions);
        std::vector<StrandId>& ids = out->mutableIds();
        std::vector<float>& id = out->addStrandChannel("braidId", -1.0f);
        std::vector<float>& flat = out->addStrandChannel("braidFlat", 0.0f);
        std::vector<float>& section = out->addStrandChannel("braidSection", -1.0f);
        StrandDraws centered;  // a section's center: no spot within it, no fan, no wander
        centered.fan = 0.0f;
        parallelFor(0, counts.size(), [&](std::size_t s) {
            const std::size_t b = s / perPath;
            const int lobe = static_cast<int>(s % perPath) - 1;
            const BraidPath& path = br.paths[b];
            ids[s] = hashCombine(path.id, static_cast<std::uint64_t>(lobe + 1));
            id[s] = numbering.number(static_cast<int>(b));
            flat[s] = br.look.style == Style::Cornrow ? 1.0f : 0.0f;
            section[s] = static_cast<float>(lobe);
            Vec3f* pts = out->strandPoints(s);
            const std::size_t m = out->strandPointCount(s);
            for (std::size_t j = 0; j < m; ++j) {
                pts[j] = br.braidedAt(path, path.length * static_cast<float>(j) / static_cast<float>(m - 1), lobe, centered);
            }
        });
        return StrandsPtr(out);
    }
};

}  // namespace

void registerBraidOperator(OperatorRegistry& registry) {
    registry.registerType(op::kBraid,
                          {"look",
                           "Braids strands along paths: three-strand, fishtail, rope, cornrows.",
                           "plait cornrow fishtail rope"},
                          [] { return std::make_unique<BraidOp>(); });
}

}  // namespace groomlab
