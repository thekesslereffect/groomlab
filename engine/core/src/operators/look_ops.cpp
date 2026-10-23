// SPDX-License-Identifier: Apache-2.0
// Procedural look operators (M3): Clump, ToGuides, Noise,
// Attribute, Merge, Switch, Blend (Curl: curl_op.cpp).
//
// "Along the length" parameters are Ramps over t = point index / (n - 1),
// root (0) to tip (1). Random choices hash the stable strand ID with the seed,
// so results never depend on thread order or strand order.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "groomlab/curve.h"
#include "groomlab/export_view.h"
#include "groomlab/field.h"
#include "groomlab/frames.h"
#include "groomlab/graph.h"
#include "groomlab/hair_color.h"
#include "groomlab/noise.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/point_index.h"
#include "groomlab/portable_math.h"
#include "groomlab/preview_core.h"
#include "groomlab/random.h"
#include "incremental.h"
#include "internal.h"

namespace groomlab {

namespace {

constexpr std::size_t kCenterSamples = 32;
constexpr float kTwoPi = 6.28318530718f;
// The randomness default of a Clump picking its own centers (and Braid its paths).
constexpr double kCenterRandomness = 0.3;
// The randomness's hard maximum (past 1 each center goes on beyond its random spot: twice as far
// from its even one at 2; farther, centers would cross most of their neighbours').
constexpr double kMaxCenterRandomness = 2.0;
// The even centers are kept this share of the spacing apart (planClumpCenters): below hexagonal
// packing's 1.07 and what darts thrown at random fill before jamming (about 0.83), so about one
// center per spacing squared still fits (kDartsPerClump).
constexpr float kEvenShare = 0.7f;
// How far (in spacings) a center's random spot lies from its even one at most: a spot anywhere in
// the disc that far around it, so at randomness 1 two neighbours may land side by side.
constexpr float kRandomReach = 1.0f;

// A hash of a strand channel's values, chunk by chunk side by side and the chunks combined in
// order: the same whichever threads ran.
std::uint64_t hashChannel(const std::vector<float>& values) {
    constexpr std::size_t kChunk = 4096;
    const std::size_t chunks = (values.size() + kChunk - 1) / kChunk;
    std::vector<std::uint64_t> hashes(chunks);
    parallelFor(0, chunks, [&](std::size_t c) {
        std::uint64_t h = hash64(c + 1);
        const std::size_t end = std::min(values.size(), (c + 1) * kChunk);
        for (std::size_t i = c * kChunk; i < end; ++i) h = hashFloat(h, values[i]);
        hashes[c] = h;
    }, 1);
    std::uint64_t h = hashCombine(hash64(0x2E610u), values.size());
    for (const std::uint64_t c : hashes) h = hashCombine(h, c);
    return h;
}

// The normal a curve's rotation-minimizing frames start from (Clump's locks and ribbons): the scalp's normal at its root (`normal`; zero: none), taken square
// to the curve's first segment, so the frames turn with the scalp and neighbours' agree.
// anyPerpendicular of the segment, which flips where the segment crosses a diagonal of the
// world axes (a head turning a few degrees curls in another plane), only without a normal or
// where the segment runs along it. (look.slang frameSeed repeats this.)
Vec3f frameSeed(const Vec3f& normal, const Vec3f& first, const Vec3f& second) {
    const Vec3f t = second - first;
    const float tt = t.dot(t);
    if (tt > 0.0f) {
        const Vec3f across = normal - t * (normal.dot(t) / tt);
        if (across.dot(across) > 1e-8f) return across.normalized();
    }
    return anyPerpendicular(t);
}
// The scalp's normal at a root on it; zero without a scalp or for a root off it.
Vec3f rootNormalOr(const ScalpMesh* scalp, const StrandRoot& root) {
    return scalp && scalp->contains(root) ? scalp->normal(root) : Vec3f{};
}

// The frame between a curve's sample frames `a` and `b` at f (0 to 1): tangent and normal
// blended and made orthonormal again, so an offset held in it moves smoothly along the curve
// rather than jumping at each sample (a lock's strands on a tight bend). (look.slang
// frameBetween repeats this.)
Frame frameBetween(const Frame& a, const Frame& b, float f) {
    if (f <= 0.0f) return a;
    if (f >= 1.0f) return b;
    Frame out;
    const Vec3f t = a.tangent * (1.0f - f) + b.tangent * f;
    out.tangent = t.dot(t) > 0.0f ? t.normalized() : a.tangent;
    Vec3f nrm = a.normal * (1.0f - f) + b.normal * f;
    nrm = nrm - out.tangent * nrm.dot(out.tangent);
    out.normal = nrm.dot(nrm) > 1e-12f ? nrm.normalized() : anyPerpendicular(out.tangent);
    out.binormal = out.tangent.cross(out.normal);
    return out;
}

// Where a strand's point `arc` from its root lies on a center of kCenterSamples points with
// cumulative arc lengths `arcs`: the sample `k` at or before it (walked on from the last, as
// arc grows along the strand) and the share `f` of the way to the next; past the center's tip,
// the tip (k = kCenterSamples - 2, f = 1). (look.slang centerAt repeats this.)
void centerSampleAt(const float* arcs, float arc, std::size_t& k, float* f) {
    while (k + 2 < kCenterSamples && arcs[k + 1] < arc) ++k;
    const float span = arcs[k + 1] - arcs[k];
    *f = span > 0.0f ? std::clamp((arc - arcs[k]) / span, 0.0f, 1.0f) : 0.0f;
}

// ---------------------------------------------------------------------------

// Feathers: Propagate's barbs carry featherId and side (and isShaft on the shafts). Every barb of
// a feather starts at its shaft's root on the scalp, and both sides of a shaft start at the same
// points on it, so clumping by root would gather whole feathers, across the shaft. Barbs clump
// within their own vane instead, by where they start on the shaft, toward centers that carry the
// same featherId and side (a Clump's own on barbs, or ToGuides of barbs); shafts do not clump.
constexpr const char* kFeatherChannel = "featherId";
constexpr const char* kSideChannel = "side";
constexpr const char* kIsShaftChannel = "isShaft";
constexpr const char* kBarbIndexChannel = "barbIndex";

using VaneKey = std::pair<std::int64_t, std::int64_t>;  // (featherId, side)

bool hasVanes(const StrandSet& s) { return s.strandChannel(kFeatherChannel) && s.strandChannel(kSideChannel); }

bool isShaft(const StrandSet& s, std::size_t i) {
    const std::vector<float>* shafts = s.strandChannel(kIsShaftChannel);
    return shafts && (*shafts)[i] > 0.5f;
}

VaneKey vaneOf(const StrandSet& s, std::size_t i) {
    return {std::llround((*s.strandChannel(kFeatherChannel))[i]), std::llround((*s.strandChannel(kSideChannel))[i])};
}

// Clump's centers grouped by vane, when the strands are barbs and every center names its vane.
class FeatherVanes {
public:
    // `starts`: per center, its first point.
    FeatherVanes(const StrandSet& strands, const std::vector<const Guide*>& centers, const std::vector<Vec3f>& starts)
        : feathers_(strands.strandChannel(kFeatherChannel)), sides_(strands.strandChannel(kSideChannel)),
          shafts_(strands.strandChannel(kIsShaftChannel)) {
        if (!hasVanes(strands) || centers.empty()) return;
        std::vector<std::vector<Vec3f>> byVane;
        for (std::size_t c = 0; c < centers.size(); ++c) {
            const auto f = centers[c]->attributes.find(kFeatherChannel);
            const auto side = centers[c]->attributes.find(kSideChannel);
            if (f == centers[c]->attributes.end() || side == centers[c]->attributes.end()) return;  // not vane centers
            const auto [it, fresh] = index_.emplace(VaneKey{std::llround(f->second), std::llround(side->second)},
                                                    members_.size());
            if (fresh) {
                members_.emplace_back();
                byVane.emplace_back();
            }
            members_[it->second].push_back(c);
            byVane[it->second].push_back(starts[c]);
        }
        for (std::vector<Vec3f>& s : byVane) trees_.emplace_back(std::move(s));
        active_ = true;
    }
    bool active() const { return active_; }
    // The center barb s clumps toward: the nearest start on its vane; SIZE_MAX for a shaft or a
    // vane without centers.
    std::size_t nearest(std::size_t s, const Vec3f& start) const {
        if (shafts_ && (*shafts_)[s] > 0.5f) return SIZE_MAX;  // isShaft, its channel looked up once
        const auto it = index_.find(VaneKey{std::llround((*feathers_)[s]), std::llround((*sides_)[s])});  // vaneOf
        if (it == index_.end()) return SIZE_MAX;
        const std::size_t k = trees_[it->second].nearest(start);
        return k == SIZE_MAX ? SIZE_MAX : members_[it->second][k];
    }

private:
    const std::vector<float>* feathers_;  // the strands' featherId, side and isShaft (null: none)
    const std::vector<float>* sides_;
    const std::vector<float>* shafts_;
    bool active_ = false;
    std::map<VaneKey, std::size_t> index_;
    std::vector<std::vector<std::size_t>> members_;
    std::vector<PointIndex> trees_;
};

// How a Clump picking centers of its own sizes its clumps: clumpBy "count": about
// `count` clumps over the strands; "size": clumps about `size` apart (a length), as many as fit.
// The other is 0 (planClumpCenters, vaneCenters); both 0: no clumps.
struct ClumpSizing {
    std::size_t count = 0;
    float size = 0.0f;
    float randomness = static_cast<float>(kCenterRandomness);  // 0 evenly spread, 1 anywhere
    bool none() const { return count == 0 && !(size > 0.0f && std::isfinite(size)); }
};
ClumpSizing clumpSizing(const ParamSet& params) {
    ClumpSizing s;
    if (params.getString("clumpBy") == "size") s.size = static_cast<float>(params.getDouble("size"));
    else s.count = static_cast<std::size_t>(params.getInt("count"));
    s.randomness = std::clamp(static_cast<float>(params.getDouble("randomness")), 0.0f, static_cast<float>(kMaxCenterRandomness));
    return s;
}

// A Clump's own centers on barbs: by count, about `count` centers shared out over the vanes by their barbs (at
// least one each); by size, as many on each vane as fit `size` apart along it (its first barb's
// root to its last's; at least one). Spread evenly along each vane by barb index with a little
// jitter, among the vane's core barbs (preview_core.h), so every preview picks the render's
// centers. Each center names its vane (featherId, side) for Clump.
GuidesPtr vaneCenters(const StrandSet& in, ClumpSizing sizing, const ScalarField& weight, std::uint64_t seed) {
    const std::size_t count = std::max<std::size_t>(sizing.count, 1);
    auto centers = std::make_shared<GuideSet>();
    std::map<VaneKey, std::size_t> index;
    std::vector<VaneKey> keys;
    std::vector<std::vector<std::size_t>> vanes;  // barbs per vane, in input order
    const std::vector<float>* barbIndex = in.strandChannel(kBarbIndexChannel);
    std::size_t barbs = 0;
    const PreviewCore core(in);  // the barbs every preview holds (preview_core.h)
    for (std::size_t s = 0; s < in.strandCount(); ++s) {
        if (isShaft(in, s) || in.strandPointCount(s) < 2 || !core.core(s)) continue;
        const VaneKey key = vaneOf(in, s);
        const auto [it, fresh] = index.emplace(key, vanes.size());
        if (fresh) {
            vanes.emplace_back();
            keys.push_back(key);
        }
        vanes[it->second].push_back(s);
        ++barbs;
    }
    const bool weighted = !weight.isConstant() || weight.maximum() < 1.0f;
    for (std::size_t v = 0; v < vanes.size(); ++v) {
        std::vector<std::size_t>& members = vanes[v];
        // Each barb's place along the vane: its barbIndex (the same whether or not the preview
        // thinned its neighbours), else its order.
        std::vector<float> places(members.size());
        for (std::size_t m = 0; m < members.size(); ++m) places[m] = barbIndex ? (*barbIndex)[members[m]] : static_cast<float>(m);
        if (barbIndex) {
            std::vector<std::size_t> order(members.size());
            for (std::size_t m = 0; m < order.size(); ++m) order[m] = m;
            std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return places[a] < places[b]; });
            std::vector<std::size_t> sortedMembers(members.size());
            std::vector<float> sortedPlaces(members.size());
            for (std::size_t m = 0; m < order.size(); ++m) {
                sortedMembers[m] = members[order[m]];
                sortedPlaces[m] = places[order[m]];
            }
            members.swap(sortedMembers);
            places.swap(sortedPlaces);
        }
        const float span = barbIndex ? places.back() + 1.0f : static_cast<float>(members.size());
        std::size_t wanted = 1;
        if (sizing.size > 0.0f) {
            const Vec3f first = in.strandPoints(members.front())[0], last = in.strandPoints(members.back())[0];
            const double along = static_cast<double>((last - first).length());
            if (std::isfinite(along)) {
                wanted = static_cast<std::size_t>(std::clamp(std::llround(along / static_cast<double>(sizing.size)), 1LL, 1000000LL));
            }
        } else {
            wanted = std::max<std::size_t>(
                1, static_cast<std::size_t>(std::llround(static_cast<double>(count) * static_cast<double>(members.size()) /
                                                         static_cast<double>(barbs))));
        }
        const std::uint64_t vaneHash = hashCombine(hashCombine(hash64(static_cast<std::uint64_t>(keys[v].first)),
                                                               static_cast<std::uint64_t>(keys[v].second + 2)),
                                                   seed ^ 0xFEA7C);
        std::size_t previous = SIZE_MAX;
        for (std::size_t j = 0; j < wanted; ++j) {
            const float jitter = 0.8f * (unitFloat(hashCombine(vaneHash, j)) - 0.5f);
            const float target = (static_cast<float>(j) + 0.5f + jitter) * span / static_cast<float>(wanted);
            // The member whose place is nearest the target (places are sorted).
            const auto above = std::lower_bound(places.begin(), places.end(), target);
            std::size_t best = std::min(static_cast<std::size_t>(above - places.begin()), members.size() - 1);
            if (best > 0 && std::abs(places[best - 1] - target) <= std::abs(places[best] - target)) --best;
            if (best == previous) continue;
            previous = best;
            const std::size_t s = members[best];
            if (weighted && unitFloat(hashCombine(in.ids()[s], seed ^ 0xC3E7ull)) >= weight.at(in, s)) continue;
            const Vec3f* pts = in.strandPoints(s);
            Guide& g = centers->add(in.ids()[s], std::vector<Vec3f>(pts, pts + in.strandPointCount(s)));
            g.root = in.roots()[s];
            g.attributes[kFeatherChannel] = static_cast<float>(keys[v].first);
            g.attributes[kSideChannel] = static_cast<float>(keys[v].second);
        }
    }
    return GuidesPtr(centers);
}

// A Clump's choice (without a centers input) of clump centers among the strands of `in`, about
// `spacing` apart, in two parts, so that a change of the randomness moves the same centers a
// little instead of picking others: planClumpCenters works out (without the randomness) the even
// centers and each one's way to its random spot, placeClumpCenters moves each center `randomness`
// of its way (past its random spot above 1) and takes the strand rooted nearest there. A center
// keeps its ID (its even strand's) wherever it lands, so its clump keeps its number, and a small
// change moves only the borders between clumps a little. Local, so an edit of the scalp (or of the
// strands) in one place changes the centers only near it. Only the preview core's strands
// (preview_core.h: those every preview holds) may be centers, so preview and render pick the same,
// and strands of fewer than 2 points never are; the count's box off a scalp is the core's too. Positions are the roots on the rest scalp (CageBinding:
// the same whatever the subdivision; without a scalp, the first points).
// 1. Darts, kDartsPerClump per spacing squared: on a scalp, points on its rest surface, each
//    face's from its own area and a hash of the face and the seed, each landing on the candidate
//    rooted nearest (within the spacing; none: no strands grow there); off one, the candidates
//    themselves, each one with that many over the candidates within the spacing of it (on a pi
//    spacing^2 patch), by a hash of its ID and the seed. Either way a dart depends only on what lies
//    around it. `weight` (centerWeight) lets only some candidates through (by ID).
// 2. Each dart in turn by its priority (a hash) makes its candidate an even center unless a center
//    already taken lies within kEvenShare spacings. Whether a dart is taken depends only on the
//    darts within that of higher priority (and theirs on theirs): the same as a search of the
//    neighborhood would decide, so an edit's effect fades within a few spacings. The darts'
//    density is fitted (by simulation) so that about one center falls per spacing squared.
// 3. Each even center's random spot: anywhere within kRandomReach spacings of it, along the scalp,
//    by a hash of its ID (a disc around it: at randomness 1 neighbours may land side by side).
// clumpBy size: the spacing is the size, wherever the scalp and however big. clumpBy count: about
// `count` clumps over the scalp, so the spacing is its rest area (without a scalp, the roots' box)
// over the count, square-rooted: global by nature; it is rounded to 1% steps so that an edit
// that changes the area a little keeps it (and the centers away from the edit).
struct CenterPlan {
    std::vector<std::size_t> even;  // per center: the strand of `in` it is at randomness 0
    std::vector<StrandId> ids;      // its ID (its even strand's)
    std::vector<Vec3f> evenAt, way; // its root there (rest) and the way to its random spot
    std::vector<std::size_t> allowed;          // the strands a moved center may land on
    std::shared_ptr<const PointIndex> index;   // their roots
    // The choice depends on the strands' roots and IDs alone (every strand rooted on the scalp, no
    // weight left out): the same strands while no root moves.
    bool byRoots = false;
    float spacing = 0.0f;  // how far apart the centers were planned (the size, or the count's spacing)
};
// Darts per clump (per spacing squared) for kEvenShare 0.7: the density of darts whose priority
// pick, kept kEvenShare apart, leaves one center per spacing squared (random sequential
// adsorption, measured on a plane).
constexpr float kDartsPerClump = 4.98f;

// `scale`: the groom's (EvalContext::scale), the spacing by count rounds at.
CenterPlan planClumpCenters(const StrandSet& in, const ScalpMesh* scalp, std::size_t count, float size,
                            const ScalarField& weight, std::uint64_t seed, double scale = 1.0) {
    CenterPlan plan;
    const bool bySize = size > 0.0f && std::isfinite(size);
    if (in.empty() || (count == 0 && !bySize)) return plan;
    // Roots on the rest scalp when it has one, so centers stay put while it deforms; on a refined
    // scalp, on its cage (CageBinding), so the same centers whatever the subdivision.
    const CageBinding binding(scalp);
    std::vector<Vec3f> rootPositions(in.strandCount());
    // The strands in chunks side by side, each chunk's counts, candidates and box merged in
    // chunk order after (a count and a box are the same in any order; the candidates keep the
    // strands' order): the same whatever the thread count. Only the preview core counts
    // (preview_core.h: the strands every preview holds), so the centers, and with them every
    // clump, are the same at every preview density.
    const PreviewCore core(in);
    constexpr std::size_t kChunk = 4096;
    const std::size_t chunks = (in.strandCount() + kChunk - 1) / kChunk;
    struct Chunk {
        std::size_t rooted = 0, counted = 0;
        std::vector<std::size_t> candidates;  // the core strands with a curve
        Vec3f lo, hi;                         // the box of the roots of its core strands with points
        bool any = false;
    };
    std::vector<Chunk> parts(chunks);
    parallelFor(0, chunks, [&](std::size_t c) {
        Chunk& part = parts[c];
        const std::size_t end = std::min(in.strandCount(), (c + 1) * kChunk);
        for (std::size_t s = c * kChunk; s < end; ++s) {
            const StrandRoot& root = in.roots()[s];
            const std::size_t n = in.strandPointCount(s);
            const Vec3f r = binding.stablePosition(root, n ? in.strandPoints(s)[0] : Vec3f{});
            rootPositions[s] = r;
            if (!core.core(s)) continue;
            ++part.counted;
            part.rooted += scalp && scalp->contains(root) ? 1 : 0;
            if (n >= 2) part.candidates.push_back(s);
            if (n == 0) continue;  // where no strand grows
            if (!part.any) {
                part.lo = part.hi = r;
                part.any = true;
            } else {
                part.lo = {std::min(part.lo.x, r.x), std::min(part.lo.y, r.y), std::min(part.lo.z, r.z)};
                part.hi = {std::max(part.hi.x, r.x), std::max(part.hi.y, r.y), std::max(part.hi.z, r.z)};
            }
        }
    }, 1);
    std::size_t rootedCount = 0, coreCount = 0;
    std::vector<std::size_t> candidateStrands;
    candidateStrands.reserve(in.strandCount());
    Vec3f lo, hi;
    bool anyRoot = false;
    for (const Chunk& part : parts) {
        rootedCount += part.rooted;
        coreCount += part.counted;
        candidateStrands.insert(candidateStrands.end(), part.candidates.begin(), part.candidates.end());
        if (!part.any) continue;
        if (!anyRoot) {
            lo = part.lo;
            hi = part.hi;
            anyRoot = true;
        } else {
            lo = {std::min(lo.x, part.lo.x), std::min(lo.y, part.lo.y), std::min(lo.z, part.lo.z)};
            hi = {std::max(hi.x, part.hi.x), std::max(hi.y, part.hi.y), std::max(hi.z, part.hi.z)};
        }
    }
    if (candidateStrands.empty()) return plan;  // no core strand with a curve
    const ScalpMesh* cage = binding.cage();
    const bool onScalp = scalp && scalp->triangleCount() > 0 && rootedCount == coreCount &&
                         std::isfinite(cage->restTotalArea()) && cage->restTotalArea() > 0.0f;

    // The spacing: the size, or the scalp's rest area (without one, the roots' box: its largest
    // two extents) over the count, in 1% steps.
    float spacing = size;
    if (!bySize) {
        float area = 0.0f;
        if (onScalp) {
            area = cage->restTotalArea();
        } else {
            const Vec3f ext = hi - lo;
            const float e[3] = {ext.x, ext.y, ext.z};
            float a = std::max({e[0], e[1], e[2]}), b = 0.0f;
            for (const float v : e) {
                if (v < a) b = std::max(b, v);
            }
            area = std::max(a * std::max(b, a * 0.1f), 1e-12f);
        }
        spacing = roundToPercentStep(std::sqrt(area / static_cast<float>(count)), scale);
    }
    if (!(spacing > 0.0f) || !std::isfinite(spacing)) return plan;
    plan.spacing = spacing;
    const float apart = kEvenShare * spacing;

    // 1. The darts, each with its priority and the candidate it lands on.
    const std::size_t n = candidateStrands.size();
    std::vector<Vec3f> positions(n);
    for (std::size_t i = 0; i < n; ++i) positions[i] = rootPositions[candidateStrands[i]];
    const float spacing2 = spacing * spacing;
    const bool weighted = !weight.isConstant() || weight.maximum() < 1.0f;
    auto letThrough = [&](std::size_t i) {  // centerWeight's chance, by the strand's ID
        const std::size_t s = candidateStrands[i];
        return !weighted || unitFloat(hashCombine(in.ids()[s], seed ^ 0xC3E7ull)) < weight.at(in, s);
    };
    struct Dart {
        std::uint64_t priority;
        std::uint32_t candidate;
    };
    std::vector<Dart> darts;
    std::shared_ptr<const PointIndex> rooted;  // the candidates' roots (on the scalp)
    if (onScalp) {
        // On the scalp: points on its rest surface, kDartsPerClump per spacing squared, each face's
        // from its own area and hash (at most kMostDarts on the scalp: clumps finer than that are
        // below any strand spacing); each lands on the candidate rooted nearest, when one is within
        // the spacing (none: no strands grow there).
        constexpr double kMostDarts = 2e6;
        const double area = cage->restTotalArea();
        const double density = std::min(static_cast<double>(kDartsPerClump) / static_cast<double>(spacing2), kMostDarts / area);
        const std::uint64_t seedHash = hash64(seed ^ 0xA07C1u);
        const std::size_t faces = cage->triangleCount();
        std::vector<std::size_t> first(faces + 1, 0);
        for (std::size_t f = 0; f < faces; ++f) {
            const double expected = static_cast<double>(cage->restTriangleArea(f)) * density;
            std::size_t k = 0;
            if (expected > 0.0) {
                k = static_cast<std::size_t>(std::min(expected, 1e7));
                if (unitFloat(hash64(hashCombine(seedHash, f))) < static_cast<float>(expected - static_cast<double>(k))) ++k;
            }
            first[f + 1] = first[f] + k;
        }
        rooted = std::make_shared<const PointIndex>(positions);
        const PointIndex& nearest = *rooted;
        darts.resize(first[faces]);
        parallelFor(0, faces, [&](std::size_t f) {
            const std::uint64_t faceHash = hashCombine(seedHash, f);
            for (std::size_t at = first[f]; at < first[f + 1]; ++at) {
                const std::uint64_t id = hashCombine(faceHash, at - first[f]);
                Rng rng(id);
                const float su = std::sqrt(rng.nextFloat());
                const float r2 = rng.nextFloat();
                const StrandRoot root{static_cast<std::int32_t>(f), su * (1.0f - r2), su * r2};
                float d2 = 0.0f;
                const std::size_t c = nearest.nearest(cage->restPositionOr(root, cage->position(root)), &d2);
                const bool lands = c != SIZE_MAX && d2 <= spacing2 && letThrough(c);
                darts[at] = {hash64(id), lands ? static_cast<std::uint32_t>(c) : std::numeric_limits<std::uint32_t>::max()};
            }
        }, 64);
    } else {
        // Off a scalp: the candidates themselves, each one with the chance that makes about as many
        // per spacing squared: pi kDartsPerClump over the candidates within the spacing of it (itself
        // among them; counted only as far as decides it), by a hash of its ID and the seed.
        const CellGrid cells(positions, spacing);
        const float wanted = 3.14159265f * kDartsPerClump;
        std::vector<char> dart(n, 0);
        parallelFor(0, n, [&](std::size_t i) {
            const float u = unitFloat(hash64(hashCombine(in.ids()[candidateStrands[i]] ^ 0xDA27Full, seed)));
            std::size_t found = 0;
            bool is = true;
            const Vec3f& p = positions[i];
            cells.around(p, [&](std::uint32_t j) {
                const Vec3f d = positions[j] - p;
                if (d.dot(d) > spacing2) return true;
                ++found;
                is = static_cast<float>(found) * u < wanted;
                return is;
            });
            dart[i] = is && letThrough(i) ? 1 : 0;
        }, 256);
        for (std::size_t i = 0; i < n; ++i) {
            if (dart[i]) darts.push_back({hash64(hashCombine(in.ids()[candidateStrands[i]], seed ^ 0xA07C1ull)), static_cast<std::uint32_t>(i)});
        }
    }

    // 2. The darts by priority (then by the candidate, for equals), each making its candidate a
    // center unless it is one already or one lies within `apart`.
    std::sort(darts.begin(), darts.end(),
              [](const Dart& a, const Dart& b) { return a.priority != b.priority ? a.priority < b.priority : a.candidate < b.candidate; });
    std::vector<char> taken(n, 0);
    GrowingCellGrid centersNear(apart);
    // A hair under `apart`: strands laid out on a lattice (a uvGrid) can sit exactly that far
    // apart, and a distance at the very edge would go either way with the rounding of the scene's
    // scale; this keeps such a pair on one side at every scale.
    const float barred = apart * (1.0f - 1e-4f);
    for (const Dart& dart : darts) {
        const std::uint32_t i = dart.candidate;
        if (i >= n || taken[i]) continue;  // no strands grow here, or a center already
        const Vec3f& r = positions[i];
        bool tooClose = false;
        centersNear.around(r, [&](const Vec3f& q, std::uint32_t) {
            tooClose = (q - r).length() < barred;
            return !tooClose;
        });
        if (tooClose) continue;
        centersNear.add(r, i);
        taken[i] = 1;
        plan.even.push_back(candidateStrands[i]);
        plan.ids.push_back(in.ids()[candidateStrands[i]]);
        plan.evenAt.push_back(r);
    }
    // 3. Each center's way to its random spot: anywhere in the disc kRandomReach spacings around it
    // on the scalp (square to its rest face's normal; off one, in the ball), by a hash of its ID.
    plan.way.resize(plan.even.size());
    parallelFor(0, plan.even.size(), [&](std::size_t c) {
        const std::uint64_t h = hashCombine(plan.ids[c], seed ^ 0x5A7D0Full);
        const float reach = kRandomReach * spacing;
        // In the plane of its rest face, from the face's first edge: the same spot however the
        // scalp is turned (a groom read in another up axis grows the same clumps, turned).
        Vec3f normal{}, edge{};
        if (onScalp) {
            const StrandRoot root = binding.toCage(in.roots()[plan.even[c]]);
            const auto& tri = cage->triangles()[static_cast<std::size_t>(root.face)];
            const std::vector<Vec3f>& pts = cage->hasRest() ? cage->restPoints() : cage->points();
            const Vec3f a = pts[static_cast<std::size_t>(tri[0])];
            edge = pts[static_cast<std::size_t>(tri[1])] - a;
            normal = edge.cross(pts[static_cast<std::size_t>(tri[2])] - a);
        }
        if (normal.dot(normal) > 0.0f && edge.dot(edge) > 0.0f) {
            normal = normal.normalized();
            const Vec3f side = edge.normalized(), other = normal.cross(side);
            const float angle = kTwoPi * unitFloat(hashCombine(h, 1)), radius = std::sqrt(unitFloat(hashCombine(h, 2)));
            plan.way[c] = (side * pm::cos(angle) + other * pm::sin(angle)) * (radius * reach);
        } else {
            const float z = 2.0f * unitFloat(hashCombine(h, 3)) - 1.0f, angle = kTwoPi * unitFloat(hashCombine(h, 1));
            const float across = std::sqrt(std::max(0.0f, 1.0f - z * z));
            const float radius = pm::cbrt(unitFloat(hashCombine(h, 2)));
            plan.way[c] = Vec3f{across * pm::cos(angle), across * pm::sin(angle), z} * (radius * reach);
        }
    });
    // The strands a moved center may land on: the candidates centerWeight lets through (all of
    // them without a weight: the darts' index over their roots again).
    if (!weighted && rooted) {
        plan.allowed = candidateStrands;
        plan.index = rooted;
    } else {
        std::vector<Vec3f> allowedAt;
        for (std::size_t i = 0; i < n; ++i) {
            if (!letThrough(i)) continue;
            plan.allowed.push_back(candidateStrands[i]);
            allowedAt.push_back(positions[i]);
        }
        plan.index = std::make_shared<const PointIndex>(std::move(allowedAt));
    }
    plan.byRoots = onScalp && weight.isConstant() && weight.maximum() >= 1.0f;
    return plan;
}

// The strand each center of `plan` is at `randomness`: the one rooted nearest its spot there, and
// (with `at`) the spot itself, `randomness` of its way from its even one (on the rest scalp), which
// moves smoothly where the strand taken steps from one to the next.
std::vector<std::size_t> placeClumpCenters(const CenterPlan& plan, float randomness, std::vector<Vec3f>* at = nullptr) {
    std::vector<std::size_t> chosen(plan.even.size());
    if (at) at->resize(plan.even.size());
    const float r = std::clamp(randomness, 0.0f, static_cast<float>(kMaxCenterRandomness));
    parallelFor(0, chosen.size(), [&](std::size_t c) {
        const Vec3f spot = plan.evenAt[c] + plan.way[c] * r;
        if (at) (*at)[c] = r == 0.0f ? plan.evenAt[c] : spot;
        if (r == 0.0f || !plan.index || plan.index->empty()) {
            chosen[c] = plan.even[c];
            return;
        }
        const std::size_t k = plan.index->nearest(spot);
        chosen[c] = k == SIZE_MAX ? plan.even[c] : plan.allowed[k];
    }, 64);
    return chosen;
}

// Clump's followTubes: the centers of its own for strands filling tube guides (an Interpolate's
// fillTubes: tubeId, tubeU, tubeV). Of the centers `strands` (their IDs `ids`, their spots `at`)
// picked over the whole scalp, those on a strand in a tube give way; then each tube picks its own
// among its core strands (those `weight` lets through, by ID as planClumpCenters lets them),
// `spacing` apart (the whole scalp's: clumpBy size's size or count's spacing), as planClumpCenters
// picks off a scalp (their roots on the rest scalp, CageBinding), moved `randomness` of their way
// as ever, appended. Each tube's centers are its own strands, so they lie inside its shape, and
// since a strand in a tube is the tube's point of its section coordinates at every t (fillTubes),
// gathering it toward one at the same distance along gathers it in section space: the clumps
// flatten, twist and taper with the tube. Tubes in their numbers' order, each picked on its own:
// the same whatever the thread count.
void addTubeCenters(const StrandSet& in, const std::vector<float>& tubeIds, const ScalpMesh* scalp, float spacing,
                    float randomness, const ScalarField& weight, std::uint64_t seed, double scale,
                    std::vector<std::size_t>& strands, std::vector<StrandId>& ids, std::vector<Vec3f>& at) {
    std::size_t kept = 0;
    for (std::size_t c = 0; c < strands.size(); ++c) {
        if (tubeIds[strands[c]] >= 0.0f) continue;
        strands[kept] = strands[c];
        ids[kept] = ids[c];
        if (c < at.size()) at[kept] = at[c];
        ++kept;
    }
    strands.resize(kept);
    ids.resize(kept);
    at.resize(kept);
    if (!(spacing > 0.0f) || !std::isfinite(spacing)) return;
    const PreviewCore core(in);
    const bool weighted = !weight.isConstant() || weight.maximum() < 1.0f;
    std::map<float, std::vector<std::size_t>> byTube;
    for (std::size_t s = 0; s < in.strandCount(); ++s) {
        if (!(tubeIds[s] >= 0.0f) || !core.core(s) || in.strandPointCount(s) < 2) continue;
        if (weighted && !(unitFloat(hashCombine(in.ids()[s], seed ^ 0xC3E7ull)) < weight.at(in, s))) continue;
        byTube[tubeIds[s]].push_back(s);
    }
    std::vector<const std::vector<std::size_t>*> tubes;
    for (const auto& [number, members] : byTube) tubes.push_back(&members);
    ParamSet every;
    every.set("weight", 1.0);
    const ScalarField all(every, "weight", nullptr);
    const CageBinding binding(scalp);
    struct Picked {
        std::vector<std::size_t> strands;
        std::vector<StrandId> ids;
        std::vector<Vec3f> at;
    };
    std::vector<Picked> picked(tubes.size());
    parallelFor(0, tubes.size(), [&](std::size_t t) {
        const std::vector<std::size_t>& members = *tubes[t];
        StrandSet sub;
        sub.reserve(members.size(), 2 * members.size());
        for (const std::size_t s : members) {
            const Vec3f q = binding.stablePosition(in.roots()[s], in.strandPoints(s)[0]);
            sub.addStrand(in.ids()[s], {q, q});
        }
        const CenterPlan plan = planClumpCenters(sub, nullptr, 0, spacing, all, seed, scale);
        std::vector<Vec3f> spots;
        const std::vector<std::size_t> chosen = placeClumpCenters(plan, randomness, &spots);
        for (std::size_t c = 0; c < chosen.size(); ++c) {
            picked[t].strands.push_back(members[chosen[c]]);
            picked[t].ids.push_back(plan.ids[c]);
            picked[t].at.push_back(spots[c]);
        }
    }, 1);
    for (const Picked& p : picked) {
        strands.insert(strands.end(), p.strands.begin(), p.strands.end());
        ids.insert(ids.end(), p.ids.begin(), p.ids.end());
        at.insert(at.end(), p.at.begin(), p.at.end());
    }
}

// The region number followTubes gives a strand in tube n (kTubeRegionBase + n): apart from any
// region an Interpolate's regionMap numbers (small whole numbers), and whole in a float (tube
// numbers are below kGroupNumbers).
constexpr float kTubeRegionBase = static_cast<float>(std::int64_t{1} << 23);

// Each center's number for the clump channel (clumpId): from its ID (stableGroupNumbers), so an
// edit (or a center picked anew) in one place renumbers no clump elsewhere.
std::vector<float> clumpNumbers(const std::vector<const Guide*>& centers) {
    std::vector<std::uint64_t> ids(centers.size());
    for (std::size_t c = 0; c < centers.size(); ++c) ids[c] = centers[c]->id;
    return stableGroupNumbers(ids);
}

// Clump's repel (a negative strength): point p, `arc` from its strand's root (along the strand as
// it came), pushed away from the center's point as far from the center's root (its curve of
// kCenterSamples points, `centerLength` long; past its tip, the tip), across the center there.
// Points within `radius` of it go out toward the radius by amount (|strength| there) times
// radius (1 - x)^2 (1 + x), x their distance over the radius: a point on the center goes all the
// way out at amount 1, the push easing off to nothing at the radius (the strands beyond it as they
// came). Past the center's tip the distance grows by how far past it the point is, so hair longer
// than the center is let go within a radius of its tip. A point on (or very near) the center goes
// the way its root lies from the center's root, else a way of its own (`angle` about the center),
// blended in smoothly. With a skin normal (zero: none) the push never goes into the skin and lies
// along it at the root, lifting free of it toward the tip (t). (look.slang ClumpCS repeats this.)
Vec3f repelled(const Vec3f& p, const Vec3f* curve, float centerLength, float arc, float amount, float radius,
               const Vec3f& rootOffset, float angle, const Vec3f& skin, float t) {
    const float along = centerLength > 0.0f ? std::min(arc / centerLength, 1.0f) : 0.0f;
    const Vec3f center = pointAlong(curve, kCenterSamples, along);
    const auto k = static_cast<std::size_t>(std::lround(along * static_cast<float>(kCenterSamples - 1)));
    const Vec3f tangent = (curve[std::min(k + 1, kCenterSamples - 1)] - curve[k > 0 ? k - 1 : 0]).normalized();
    const Vec3f d = p - center;
    const Vec3f across = d - tangent * d.dot(tangent);
    const float past = std::max(arc - centerLength, 0.0f);  // beyond the center's tip
    const float r = std::sqrt(across.dot(across) + past * past);
    if (!(r < radius)) return p;
    // The way out: across, blended (within a hundredth of the radius) into the root's way, itself
    // blended into the strand's own turn, so it never jumps (nor turns on a rounding) near the center.
    const float near = 0.01f * radius;
    const Vec3f side = anyPerpendicular(tangent);
    const Vec3f spun = side * pm::cos(angle) + tangent.cross(side) * pm::sin(angle);
    const Vec3f fromRoot = rootOffset - tangent * rootOffset.dot(tangent);
    const Vec3f own = (fromRoot + spun * near).normalized();
    Vec3f way = (across + own * near).normalized();
    const float up = way.dot(skin);
    way = way - skin * (up < 0.0f ? up : up * (1.0f - t));
    const float x = r / radius;
    const float push = amount * radius * ((1.0f - x) * (1.0f - x) * (1.0f + x));
    return p + way * push;
}

// Pulls each strand toward the nearest clump center (by root), more strongly
// toward the tip as the profile ramp says, onto the clump's axis at the same
// arc length from the root. Chain Clumps (with different sizes, or guide levels
// as centers) for multi-level clumping. centersFrom says where its centers come from: pick
// (the default) picks them among its strands (clumpBy: about `count`, or `size` apart, the strands
// `centerWeight` lets through; planClumpCenters, placeClumpCenters), so a Clump alone clumps; each
// axis is the picked strand. The others take the centers input, guides or strands
// (a low-density Scatter, ToGuides, guide levels): inputPoints, they
// only place the clumps (a single point will do) and each axis is the core strand the
// clump gathers nearest its center's root (XGen's clump guides: the hair as grown
// there); inputCurves, each axis is the center curve itself (groomed
// clump curves, Yeti's clump input). Whether the input is linked never decides it: pick ignores
// a linked one, and the input modes with nothing linked leave the strands as they came (a note
// says which to change). pastTip says what a strand longer than its
// axis does past the axis's tip: gathers at the tip, or extends on along it. With a radius the
// strands gather into a round lock of that radius instead of onto the center
// (dreads, wet strings). With `flatness` each clump is squashed across into a ribbon
// (wet hair). What primitive nodes do is theirs: a Twist after it spirals the
// clumps (ropes, locs), a Noise on guides as its centers waves each clump as one, a
// RestoreLength after it keeps the strands' lengths, a strength that varies per
// strand (an expression of rand) lets some strands escape their clump. A negative strength
// pushes the strands within `repelRadius` of their center away from it instead (repelled):
// fur parting around whiskers, horns or eyes, the guides of those as the centers. With regions (the
// `regionChannel` an Interpolate's regionMap writes) a strand clumps only toward a center of its own
// region, as it never clumps across a part. With followTubes (the default) and strands filling tube
// guides (an Interpolate's fillTubes writes tubeId, tubeU and tubeV), a Clump picking its own
// centers picks each tube's among its strands (addTubeCenters) and a strand in a tube clumps only
// toward its tube's centers, as toward its region's, so clumps keep inside the tube and flatten,
// twist and taper with it; strands in no tube clump as ever among the centers off the tubes.
class ClumpOp : public Operator {
public:
    // The rest scalp places roots for the nearest-center search; parts limit it.
    std::vector<std::string> externalInputs() const override { return {"scalp", "parts"}; }
    std::string typeName() const override { return op::kClump; }
    std::string category() const override { return "look"; }
    std::vector<PortSpec> inputs() const override {
        return {{"strands", PortType::Strands}, {"centers", PortType::Curves, true}};  // guides or strands
    }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"strength", 0.7, "Overall pull toward the clump center, -1 to 1 (mappable, per point). Negative "
                                       "pushes strands away from the centers (part fur around whiskers, horns, eyes): "
                                       "those within repelRadius, out toward it"}
                .soft(-1, 1),
            {"profile", Ramp{{0.0f, 0.0f}, {0.4f, 0.5f}, {1.0f, 1.0f}}, "Pull along the length, root to tip"},
            ParamSpec{"centersFrom", std::string("pick"),
                      "Where the clumps' centers come from: pick: it picks them among the strands (clumpBy), each axis "
                      "the picked strand, and a centers input is ignored; inputPoints: the centers input (guides or "
                      "strands: a Scatter, guides, single points) only says where the clumps are (their roots), each "
                      "axis the strand it gathers nearest its center's root (the hair as grown there); inputCurves: each "
                      "axis is the center curve itself (groomed clump curves, a Noise on the centers waving each clump "
                      "as one). Connecting centers in the Graph Editor sets inputPoints",
                      "", {"pick", "inputPoints", "inputCurves"}},
            ParamSpec{"clumpBy", std::string("count"),
                      "The clumps it picks among the strands: count: about `count` of them, whatever the scalp's size "
                      "(re-fitted to the scalp's area: an edit that changes it by more than 1% moves every clump); size: "
                      "clumps about `size` apart, as many as the scalp holds (a whole body gets more than a head, of the "
                      "same size; an edit of the scalp changes the clumps only near it)",
                      "", {"count", "size"}}
                .when("centersFrom", {"pick"}),
            notAnimatable({"count", std::int64_t{300}, "About how many clumps (clumpBy count); 0: none (the strands as they "
                                                       "came)"})
                .atLeast(0)
                .soft(0, 1000)
                .when("clumpBy", {"count"}),
            notAnimatable({"size", 1.0, "Clump size (clumpBy size): about how far apart the centers are, so the count "
                                        "follows the scalp's area; 0: none", "length"})
                .atLeast(0)
                .soft(0, 5)
                .when("clumpBy", {"size"}),
            notAnimatable({"randomness", kCenterRandomness,
                           "How random the centers it picks are: 0 evenly spread (blue noise, every one about the average "
                           "spacing from its neighbors), 1 at random (some side by side); in between each center moves "
                           "smoothly from its even spot toward its random one (the same clumps, their borders shifting), "
                           "past 1 on beyond it, up to 2. As Scatter's", "fraction"})
                .range(0, kMaxCenterRandomness)
                .soft(0, 1)
                .when("centersFrom", {"pick"}),
            ParamSpec{"centerWeight", 1.0, "Chance each strand can be a center it picks, 0-1 (mappable: e.g. an Expression "
                                           "favoring long strands, or guideInfluence)", "fraction"}
                .soft(0, 1)
                .when("centersFrom", {"pick"}),
            ParamSpec{"pastTip", std::string("gather"),
                      "A strand longer than its clump's axis: gather: its points past the axis's tip gather at the tip; "
                      "extend: they go on past it, along the axis's last direction, so long hair keeps its length",
                      "", {"gather", "extend"}},
            ParamSpec{"clumpChannel", std::string("clumpId"),
                      "Per-strand channel receiving the clump's number, from its center's ID (the same while that center "
                      "is; empty: none). Chained Clumps each want their own (lockId, then clumpId), for a Twist, Cards or "
                      "HairColor's clumpShare to follow the one it names"}
                .writesChannel(),
            notAnimatable({"seed", std::int64_t{0}, "Random seed for the centers it picks and the per-clump variation"}).adv(),
            {"respectParts", true, "Never clump toward a center on the other side of a part"},
            ParamSpec{"regionChannel", std::string("region"),
                      "Channel numbering each strand's region (an Interpolate's regionMap writes region): strands clump "
                      "only toward centers of their own region (a center's region is its strand's, its own value of the "
                      "channel, or else the nearest strand's), and a region without a center stays unclumped; empty, or "
                      "a channel the strands lack: across regions"}
                .adv()
                .readsChannel(true),
            ParamSpec{"followTubes", true,
                      "Strands filling tube guides (an Interpolate's fillTubes: tubeId, tubeU and tubeV) clump inside "
                      "their own tube: each tube's centers are picked among its strands (as far apart as clumpBy says "
                      "for the whole scalp) and a strand gathers toward its tube's nearest, so the clumps flatten, "
                      "twist and taper with the tube; strands in no tube clump as ever. Off: by root across tubes. "
                      "Strands without those channels clump as ever either way"}
                .when("centersFrom", {"pick"}),
            ParamSpec{"lockRadius", 0.0, "Lock radius: strands gather into a round lock this thick around the center "
                                         "(dreads, wet strings; a Twist node after it turns them into ropes); 0: onto the "
                                         "center", "length"}
                .atLeast(0)
                .soft(0, 1),
            ParamSpec{"repelRadius", 0.5, "With a negative strength: how far from its center a strand is pushed out to (the "
                                          "part's half width; strands farther away stay as they are)", "length"}
                .atLeast(0)
                .soft(0, 2),
            ParamSpec{"flatness", 0.0, "Squashes each clump across into a ribbon, 0-1, as much as it clumps there (wet hair)",
                      "fraction"}
                .range(0, 1),
            {"flatten", std::string("surface"),
             "Which way a flat clump lies: surface: flat along the scalp; random: turned at random per clump; direction: "
             "its width along flattenDirection", "", {"surface", "random", "direction"}},
            ParamSpec{"flattenDirection", Vec3f{1.0f, 0.0f, 0.0f}, "Flatten direction: the way a ribbon's width runs (world space)"}
                .world()
                .when("flatten", {"direction"}),
            // XGen's and Yeti's clump controls, each per clump (by its center's ID), 0: none.
            ParamSpec{"volumize", 0.0, "Pushes each clump's strands out from its axis by this share of how far they are from "
                                       "it, most near the root, filling the clump's volume (mappable)"}
                .soft(0, 1)
                .in("Shape"),
            ParamSpec{"volumizeProfile", Ramp::linear(1.0f, 0.0f), "Volumize along the length, root to tip"}.in("Shape"),
            ParamSpec{"strengthRandom", 0.0, "Each clump's strength scaled down by up to this share, at random per clump",
                      "fraction"}
                .range(0, 1)
                .in("Random"),
            ParamSpec{"strandStrengthRandom", 0.0, "Each strand's strength scaled down by up to this share, at random per "
                                                   "strand: some hug the clump, some hang loose", "fraction"}
                .range(0, 1)
                .in("Random"),
            ParamSpec{"lockRadiusRandom", 0.0, "Each clump's lock radius up to this share bigger or smaller, at random per "
                                               "clump", "fraction"}
                .range(0, 1)
                .in("Random"),
            ParamSpec{"copyAmount", 0.0, "Each clump takes on this share of its neighbouring clumps' shape (their center "
                                         "curves, from their roots), so clumps share one flow", "fraction"}
                .range(0, 1)
                .in("Shape"),
            notAnimatable({"copyNeighbors", std::int64_t{1}, "How many of the nearest clumps a clump copies (their mean)"})
                .range(1, 8)
                .in("Shape")
                .adv(),
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    // A dab moves some strands, and with them some centers (guides made from strands): the strands
    // that moved and the members of the centers that moved are clumped again; the rest keep the
    // last result's points. The nearest center is by root, so it stays while no center's root moves.
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

    // The center choice the same node made in another copy of the graph (Operator::adoptKeptWork).
    void adoptKeptWork(const Operator& previous) override {
        const auto* from = dynamic_cast<const ClumpOp*>(&previous);
        if (!from || from == this) return;
        Made made;
        {
            const std::lock_guard<std::mutex> lock(from->madeMutex_);
            made = from->made_;
        }
        {
            const std::lock_guard<std::mutex> lock(madeMutex_);
            if (!made_.assigned) made_ = std::move(made);
        }
        std::vector<std::pair<std::uint64_t, std::shared_ptr<const Pick>>> picks;
        {
            const std::lock_guard<std::mutex> lock(from->pickMutex_);
            picks = from->pickMemo_;
        }
        const std::lock_guard<std::mutex> lock(pickMutex_);
        if (pickMemo_.empty()) pickMemo_ = std::move(picks);
    }

    // What the last result was made from, when it is `output` (operator_state.h).
    bool state(const StrandsPtr& output, ClumpState* state) const {
        const std::lock_guard<std::mutex> lock(madeMutex_);
        if (!output || !sameOrShown(made_.output.lock(), output) || !made_.assigned) return false;
        state->curves = made_.curves;
        state->centerNormals = made_.centerNormals;
        state->assigned = made_.assigned;
        state->centerGuides = made_.centerGuides;
        state->chosen = made_.chosen;
        state->axes = made_.axes;
        state->centerIds = made_.ids;
        state->centerRoots = made_.roots;
        state->picked = made_.picked;
        return true;
    }
    // Masked: its state answers for the mixed result the graph keeps (Operator::resultMixed).
    void resultMixed(const StrandsPtr& made, const StrandsPtr& mixed) const override {
        const std::lock_guard<std::mutex> lock(madeMutex_);
        if (made && made_.output.lock() == made) made_.output = mixed;
    }

private:
    static constexpr std::uint32_t kNoCenter = kNoClumpCenter;
    static_assert(kCenterSamples == kClumpCenterSamples, "operator_state.h says how Clump samples its centers");
    // What the last result was made from (evaluateIncremental): the result (by identity), the
    // centers' search roots and curves, and each strand's center (kNoCenter: not clumped).
    struct Made {
        std::weak_ptr<const StrandSet> output;
        std::vector<Vec3f> roots, curves, centerNormals;
        std::shared_ptr<const std::vector<std::uint32_t>> assigned;
        // What the assignment was searched from besides the centers' roots: each strand's query
        // (its root on the rest scalp, else its first point; NaN without points) and the parts'
        // sides (0: none). A full run with the same reuses it (a tweak of anything but these).
        std::shared_ptr<const std::vector<Vec3f>> queries;
        std::uint64_t partsKey = 0;
        std::vector<std::size_t> centerGuides;  // per center: its guide's index in the centers input
        std::vector<std::size_t> chosen;        // centers of its own: the strand each guide is (ClumpState)
        std::vector<std::uint32_t> axes;        // centersFrom inputPoints: each center's axis strand (ClumpState)
        std::vector<StrandId> ids;              // per center: its ID (ClumpState::centerIds)
        std::vector<std::size_t> picked;        // centers of its own: the strand each guide is (ClumpState)
        // Regions (regionChannel): the strands' regions' hash (0: none), each center's region, and
        // whether some center's is its nearest strand's.
        std::uint64_t regionHash = 0;
        std::vector<float> centerRegions;
        bool regionsFromStrands = false;
    };
    // A plan of its own centers (planClumpCenters: the even centers and their ways to their random
    // spots). Kept by pickKey, so a run that reads the same picks nothing again (three passes over
    // every strand and the darts, for a strength or randomness tweak).
    using Pick = CenterPlan;
    static constexpr std::size_t kPickMemo = 2;  // picks kept (a preview and a render, say)

    // Everything a plan of its own centers depends on: the scalp's rest surface (and whether there
    // is one; a deforming scalp keeps it), the strands' IDs, roots, point counts and first points
    // (all planClumpCenters reads of them: a stroke moves no root), the sizing (not its randomness:
    // only where the centers are moved to) and the seed. The strands' part is hashed in chunks side
    // by side, the chunks' hashes combined in order: the same whatever the thread count.
    static std::uint64_t pickKey(const StrandSet& in, const ScalpMesh* scalp, const ClumpSizing& sizing, float weight,
                                 std::uint64_t seed) {
        constexpr std::size_t kChunk = 4096;
        const std::size_t count = in.strandCount();
        std::vector<std::uint64_t> parts((count + kChunk - 1) / kChunk);
        parallelFor(0, parts.size(), [&](std::size_t c) {
            std::uint64_t h = hash64(c);
            const std::size_t end = std::min(count, (c + 1) * kChunk);
            for (std::size_t s = c * kChunk; s < end; ++s) {
                const StrandRoot& r = in.roots()[s];
                h = hashCombine(h, in.ids()[s]);
                h = hashCombine(h, static_cast<std::uint32_t>(r.face));
                h = hashFloat(hashFloat(h, r.u), r.v);
                const std::size_t n = in.strandPointCount(s);
                h = hashCombine(h, n);
                if (n == 0) continue;
                const Vec3f& p = in.strandPoints(s)[0];
                h = hashFloat(hashFloat(hashFloat(h, p.x), p.y), p.z);
            }
            parts[c] = h;
        }, 1);
        std::uint64_t key = hashCombine(hash64(0xC1A7Cu), scalp ? scalp->restSurfaceHash() : 0);
        key = hashCombine(key, scalp ? 1u : 0u);
        key = hashFloat(hashCombine(key, sizing.count), sizing.size);
        key = hashFloat(key, weight);
        key = hashCombine(hashCombine(key, seed), count);
        for (const std::uint64_t h : parts) key = hashCombine(key, h);
        return key;
    }
    std::shared_ptr<const Pick> findPick(std::uint64_t key) const {
        const std::lock_guard<std::mutex> lock(pickMutex_);
        for (const auto& [k, pick] : pickMemo_) {
            if (k == key) return pick;
        }
        return nullptr;
    }
    void keepPick(std::uint64_t key, std::shared_ptr<const Pick> pick) const {
        const std::lock_guard<std::mutex> lock(pickMutex_);
        pickMemo_.insert(pickMemo_.begin(), {key, std::move(pick)});
        if (pickMemo_.size() > kPickMemo) pickMemo_.resize(kPickMemo);
    }

    // In full, or (with `incremental`) only the strands that moved and the members of the centers
    // that moved, into the last result's copy; false when that cannot be done.
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, 0, op::kClump);
        const std::string from = params().getString("centersFrom");
        const bool own = from == "pick";
        if (!own && from != "inputPoints" && from != "inputCurves") {
            throw GraphError("Clump: unknown centersFrom '" + from + "' (pick, inputPoints or inputCurves)");
        }
        const bool linked = inputs.at(1).index() != 0;
        GuidesPtr centers;
        if (own && linked) {
            ctx.note("centers are connected but centersFrom is pick (it picks its own): set centersFrom to inputPoints "
                     "or inputCurves to clump by them");
        } else if (!own) {
            centers = valueAs<GuidesPtr>(inputs.at(1));
            // Strands as centers: their preview core (preview_core.h), the same at every preview density.
            if (const StrandsPtr strands = valueAs<StrandsPtr>(inputs.at(1))) centers = strandsAsGuides(*strands, true);
            if (!linked) ctx.note("no clumps: connect centers (guides or strands) or set centersFrom to pick; the strands pass through");
        }
        // centersFrom pick: centers of its own, picked among the strands (the ones centerWeight
        // lets through), each the strand `chosen` names (kept when the choice is by roots).
        std::vector<std::size_t> chosen;
        std::vector<std::size_t> picked;  // centers of its own: the strand each is (whether by roots or not)
        std::vector<Vec3f> pickedAt;      // and where each is (its spot on the rest scalp): what strands clump by
        const std::string regionName = params().getString("regionChannel");
        const ClumpSizing sizing = own ? clumpSizing(params()) : ClumpSizing{};
        // followTubes: strands filling tube guides clump inside their own tube (addTubeCenters,
        // their tubes as regions below); not on barbs.
        const std::vector<float>* tubeIds = nullptr;
        if (own && params().getBool("followTubes") && in->strandChannel("tubeU") && in->strandChannel("tubeV") && !hasVanes(*in)) {
            tubeIds = in->strandChannel("tubeId");
            if (tubeIds && (tubeIds->size() != in->strandCount() ||
                            std::none_of(tubeIds->begin(), tubeIds->end(), [](float t) { return t >= 0.0f; }))) {
                tubeIds = nullptr;
            }
            if (tubeIds && incremental) return false;  // (each tube's centers picked anew)
        }
        if (own && !in->empty() && !sizing.none()) {
            const auto seed = static_cast<std::uint64_t>(params().getInt("seed"));
            const ScalarField weight(params(), "centerWeight", scalpOf(ctx));
            weight.validate(*in);
            if (hasVanes(*in)) {
                if (incremental) return false;
                centers = vaneCenters(*in, sizing, weight, seed);
            } else {
                // The choice kept from the last run that read the same (a tweak of the strength,
                // profile, radius... picks again from nothing new); the centers are the chosen
                // strands as they come now. A weight that varies per strand picks anew.
                const bool keyed = weight.isConstant();
                const std::uint64_t key =
                    keyed ? pickKey(*in, scalpOf(ctx), sizing, weight.maximum(), seed) : 0;
                std::shared_ptr<const Pick> pick = keyed ? findPick(key) : nullptr;
                // A dab moves no root: incrementally, only a kept pick by roots (the same centers,
                // as they come now); anything else picks in full.
                if (incremental && !(pick && pick->byRoots)) return false;
                if (!pick) {
                    pick = std::make_shared<const Pick>(
                        planClumpCenters(*in, scalpOf(ctx), sizing.count, sizing.size, weight, seed, ctx.scale));
                    if (keyed) keepPick(key, pick);
                }
                // Each center moved `randomness` of its way to its random spot, the strand rooted
                // nearest there its axis; it keeps its ID (its even strand's) whichever strand it
                // lands on, and the strands nearest its spot clump to it, so a small change of the
                // randomness moves the borders between clumps only a little.
                std::vector<std::size_t> now = placeClumpCenters(*pick, sizing.randomness, &pickedAt);
                std::vector<StrandId> nowIds = pick->ids;
                if (tubeIds) {
                    addTubeCenters(*in, *tubeIds, scalpOf(ctx), pick->spacing, sizing.randomness, weight, seed, ctx.scale, now,
                                   nowIds, pickedAt);
                }
                auto pickedCenters = std::make_shared<GuideSet>();
                for (std::size_t c = 0; c < now.size(); ++c) {
                    const std::size_t s = now[c];
                    const Vec3f* pts = in->strandPoints(s);
                    pickedCenters->add(nowIds[c], std::vector<Vec3f>(pts, pts + in->strandPointCount(s))).root = in->roots()[s];
                }
                if (pick->byRoots && !tubeIds) chosen = now;
                picked = now;
                centers = GuidesPtr(pickedCenters);
            }
        }
        if (!centers || centers->empty() || in->empty()) {
            if (incremental) return false;
            if (own && sizing.none()) {
                ctx.note("no clumps: set a count or size above 0 (or centersFrom inputPoints with centers connected); the "
                         "strands pass through");
            }
            *result = unclumped(in);
            return true;
        }

        const ScalarField strength(params(), "strength", scalpOf(ctx));
        strength.validate(*in);
        const bool strengthPerPoint = strength.perPoint();
        const Ramp profile = params().getRamp("profile");
        const std::string channel = params().getString("clumpChannel");
        const auto seed = static_cast<std::uint64_t>(params().getInt("seed"));
        const float radius = static_cast<float>(params().getDouble("lockRadius"));
        const float repelRadius = static_cast<float>(params().getDouble("repelRadius"));
        const float flatness = static_cast<float>(params().getDouble("flatness"));
        const bool shaped = flatness > 0.0f;  // a clump's cross-section squashed
        const std::string flatten = shaped ? params().getString("flatten") : std::string();
        const Vec3f flattenDirection = flatten == "direction" ? params().getVec3("flattenDirection") : Vec3f{};
        // Each clump's axis: the core strand it gathers nearest its center's root (centersFrom
        // inputPoints), else the center's own curve (a center of its own is a strand).
        const bool fromStrands = from == "inputPoints";
        const bool extend = params().getString("pastTip") == "extend";
        if (fromStrands && strength.isConstant() && strength.maximum() < 0.0f) {
            ctx.note("parts the strands around the strands nearest its centers (centersFrom inputPoints); inputCurves "
                     "parts them around the center curves themselves (whiskers, horns)");
        }

        // Nearest center by root, on the rest scalp when it has one (stable while it deforms), on
        // a refined scalp's cage (CageBinding: each strand the same center whatever the subdivision).
        const ScalpMesh* scalp = scalpOf(ctx);
        const CageBinding binding(scalp);
        std::vector<Vec3f> roots;
        std::vector<Vec3f> starts;  // per center: its first point (what vanes search from)
        // Center c occupies [c * kCenterSamples, (c + 1) * kCenterSamples); from the strands, made
        // once the strands are assigned (axisCurves).
        std::vector<Vec3f> curves;
        std::vector<const Guide*> used;  // the centers, in `roots` order
        std::vector<std::size_t> centerGuides;
        for (const Guide& g : centers->guides()) {
            // From the strands, a center gives only its root: a single point will do.
            if (g.points.size() < (fromStrands ? 1u : 2u)) continue;
            if (fromStrands) {
                starts.push_back(g.points.front());
            } else {
                const auto resampled = resampleByArcLength(g.points.data(), g.points.size(), kCenterSamples);
                starts.push_back(resampled.front());
                curves.insert(curves.end(), resampled.begin(), resampled.end());
            }
            const auto guide = static_cast<std::size_t>(&g - centers->guides().data());
            roots.push_back(guide < pickedAt.size() ? pickedAt[guide] : binding.stablePosition(g.root, starts.back()));
            centerGuides.push_back(static_cast<std::size_t>(&g - centers->guides().data()));
            used.push_back(&g);
        }
        if (roots.empty()) {
            if (incremental) return false;
            *result = unclumped(in);
            return true;
        }
        if (!fromStrands) shareCenters(roots, curves, nullptr);
        // From the strands: each center's axis strand (kNoCenter: it gathers none; its curve is its
        // first point), resampled as its curve, then shared (copyAmount) among those with one.
        std::vector<std::uint32_t> axes;
        auto axisCurves = [&] {
            curves.resize(roots.size() * kCenterSamples);
            parallelFor(0, roots.size(), [&](std::size_t c) {
                Vec3f* curve = curves.data() + c * kCenterSamples;
                if (axes[c] == kNoCenter) {
                    std::fill(curve, curve + kCenterSamples, starts[c]);
                    return;
                }
                const auto resampled = resampleByArcLength(in->strandPoints(axes[c]), in->strandPointCount(axes[c]), kCenterSamples);
                std::copy(resampled.begin(), resampled.end(), curve);
            }, 64);
            shareCenters(roots, curves, &axes);
        };
        // Per clump (by its center's ID): its strength and lock radius scaled at random.
        const auto strengthRandom = static_cast<float>(params().getDouble("strengthRandom"));
        const auto radiusRandom = static_cast<float>(params().getDouble("lockRadiusRandom"));
        auto clumpStrength = [&](std::size_t c) {
            return 1.0f - strengthRandom * unitFloat(hashCombine(used[c]->id, seed ^ 0x57E4Aull));
        };
        // Per strand (by its ID), on top: how far it follows its clump's pull.
        const auto strandRandom = static_cast<float>(params().getDouble("strandStrengthRandom"));
        auto clumpRadius = [&](std::size_t c) {
            return radius * (1.0f + radiusRandom * (2.0f * unitFloat(hashCombine(used[c]->id, seed ^ 0x2AD1Bull)) - 1.0f));
        };
        const ScalarField volumize(params(), "volumize", scalp);
        volumize.validate(*in);
        const bool volumizing = !(volumize.isConstant() && volumize.maximum() == 0.0f);
        const Ramp volumizeProfile = params().getRamp("volumizeProfile");
        const FeatherVanes vanes(*in, used, starts);
        // Regions (regionChannel, which Interpolate's regionMap writes): a strand clumps only toward
        // a center of its own region. A center's region: a center of its own, its strand's; else its
        // own value of the channel (centers that are strands carry it); else the region of the
        // strand rooted nearest it. A region without a center leaves its strands unclumped.
        const std::vector<float>* strandRegions = vanes.active() ? nullptr : regionsOf(*in, regionName);
        // followTubes: each tube a region of its own (kTubeRegionBase + its number), the strands in
        // no tube in theirs (the channel's, else one region), so a strand clumps toward its tube's
        // centers and the others toward the centers off the tubes.
        std::vector<float> tubeRegions;
        if (tubeIds && !picked.empty()) {
            tubeRegions.resize(in->strandCount());
            const std::vector<float>* given = strandRegions;
            parallelFor(0, tubeRegions.size(), [&](std::size_t s) {
                const float t = (*tubeIds)[s];
                tubeRegions[s] = t >= 0.0f ? kTubeRegionBase + t : given ? (*given)[s] : 0.0f;
            }, 4096);
            strandRegions = &tubeRegions;
        }
        const PreviewCore core(*in);  // what a center's region and axis come from (preview_core.h)
        const std::uint64_t regionHash = strandRegions ? hashChannel(*strandRegions) : 0;
        std::vector<float> centerRegions;
        bool regionsFromStrands = false;  // some center's region is its nearest strand's
        // Each clump's number in the clump channel (clumpNumbers).
        const std::vector<float> numbers = channel.empty() ? std::vector<float>() : clumpNumbers(used);
        // Incrementally: the last result made here, from centers rooted where these are (each
        // strand's center the same), with a strength that reads each strand alone and no vanes.
        const StrandChanges* changes = nullptr;
        StrandsPtr last;
        Made made;
        std::vector<std::uint32_t> dirty;
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            {
                const std::lock_guard<std::mutex> lock(madeMutex_);
                made = made_;
            }
            auto sameBits = [](const std::vector<Vec3f>& a, const std::vector<Vec3f>& b) {
                return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(Vec3f)) == 0);
            };
            if (!last || made.output.lock() != last || !made.assigned || made.assigned->size() != in->strandCount() ||
                !sameBits(made.roots, roots) || vanes.active() || !strength.strandLocal() || !volumize.strandLocal() ||
                made.regionHash != regionHash || made.centerRegions.size() != (strandRegions ? roots.size() : 0)) {
                return false;
            }
            centerRegions = made.centerRegions;  // the same centers and strands' regions: the same
            regionsFromStrands = made.regionsFromStrands;
            if (fromStrands) {
                // The same assignment (no root moves), so the same axis strands, as they come now.
                if (made.axes.size() != roots.size()) return false;
                for (const std::uint32_t a : made.axes) {
                    if (a != kNoCenter && (a >= in->strandCount() || in->strandPointCount(a) < 2)) return false;
                }
                axes = made.axes;
                axisCurves();
            }
            if (made.curves.size() != curves.size()) return false;
            // The centers whose curves moved, then the strands to clump again: those that moved and
            // those clumped toward a center that did.
            std::vector<char> movedCenter(roots.size(), 0);
            bool anyMoved = false;
            for (std::size_t c = 0; c < roots.size(); ++c) {
                movedCenter[c] = std::memcmp(curves.data() + c * kCenterSamples, made.curves.data() + c * kCenterSamples,
                                             kCenterSamples * sizeof(Vec3f)) != 0;
                anyMoved = anyMoved || movedCenter[c];
            }
            std::vector<char> again(in->strandCount(), 0);
            for (const std::uint32_t s : changes->strands) again[s] = 1;
            if (anyMoved) {
                const std::vector<std::uint32_t>& assigned = *made.assigned;
                parallelFor(0, again.size(), [&](std::size_t s) {
                    if (assigned[s] != kNoCenter && movedCenter[assigned[s]]) again[s] = 1;
                }, 4096);
            }
            for (std::size_t s = 0; s < again.size(); ++s) {
                if (again[s]) dirty.push_back(static_cast<std::uint32_t>(s));
            }
            if (dirty.empty()) {  // the same strands and centers: the same result
                *result = last;
                *changed = StrandChanges::listed({});
                return true;
            }
        }
        const PointIndex index(roots);
        const PartsPtr parts = params().getBool("respectParts") ? valueAs<PartsPtr>(ctx.external("parts")) : nullptr;
        // Parts lie on the scalp drawn: sides are found from the roots there (the queries are on
        // the cage when it is refined).
        std::vector<PartLines::Side> centerSides;
        if (parts) {
            for (std::size_t c = 0; c < roots.size(); ++c) {
                centerSides.push_back(binding.refined() ? parts->sideOf(restRootOr(scalp, used[c]->root, starts[c]))
                                                        : parts->sideOf(roots[c]));
            }
        }

        // Each strand's query for the nearest center: its root on the rest scalp, else its first
        // point (NaN without points).
        auto queryOf = [&](std::size_t s) {
            if (in->strandPointCount(s) == 0) return Vec3f(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f);
            return binding.stablePosition(in->roots()[s], in->strandPoints(s)[0]);
        };
        // The centers' regions (kept incrementally), then each region's centers and an index over them.
        if (strandRegions && !last) {
            const float none = std::numeric_limits<float>::quiet_NaN();
            centerRegions.assign(roots.size(), none);
            std::vector<std::size_t> unknown;
            for (std::size_t c = 0; c < roots.size(); ++c) {
                if (!picked.empty()) {
                    centerRegions[c] = (*strandRegions)[picked[centerGuides[c]]];
                    continue;
                }
                const auto it = used[c]->attributes.find(regionName);
                if (it != used[c]->attributes.end()) {
                    centerRegions[c] = it->second;
                } else {
                    unknown.push_back(c);
                }
            }
            if (!unknown.empty()) {
                regionsFromStrands = true;
                std::vector<std::uint32_t> rooted;
                std::vector<Vec3f> at;
                for (std::size_t s = 0; s < in->strandCount(); ++s) {
                    if (in->strandPointCount(s) == 0 || !core.core(s)) continue;  // the core's nearest
                    rooted.push_back(static_cast<std::uint32_t>(s));
                    at.push_back(queryOf(s));
                }
                if (!rooted.empty()) {
                    const PointIndex nearestStrand(std::move(at));
                    parallelFor(0, unknown.size(), [&](std::size_t k) {
                        const std::size_t c = unknown[k];
                        centerRegions[c] = (*strandRegions)[rooted[nearestStrand.nearest(roots[c])]];
                    });
                }
            }
        }
        struct RegionCenters {
            std::vector<std::uint32_t> centers;
            std::optional<PointIndex> index;
        };
        std::map<float, RegionCenters> regionCenters;  // by region (a center in none, NaN, in no entry)
        if (strandRegions) {
            for (std::size_t c = 0; c < centerRegions.size(); ++c) {
                if (centerRegions[c] == centerRegions[c]) regionCenters[centerRegions[c]].centers.push_back(static_cast<std::uint32_t>(c));
            }
            for (auto& [region, group] : regionCenters) {
                std::vector<Vec3f> at;
                for (const std::uint32_t c : group.centers) at.push_back(roots[c]);
                group.index.emplace(std::move(at));
            }
        }
        // What the assignment is searched from besides the queries and the centers' roots.
        std::uint64_t partsKey = parts ? hashCombine(parts->sideHash(), 1) : 0;
        if (strandRegions) {
            partsKey = hashCombine(hashCombine(partsKey, regionHash), centerRegions.size());
            for (const float r : centerRegions) partsKey = hashFloat(partsKey, r);
        }
        // In full: the last full run's assignment when it was searched from these queries, centers'
        // roots and parts (a tweak of the strength, profile, flatness...), else searched again.
        std::shared_ptr<const std::vector<Vec3f>> queries;
        std::shared_ptr<const std::vector<std::uint32_t>> reuse;
        if (!last && !vanes.active()) {
            auto mine = std::make_shared<std::vector<Vec3f>>(in->strandCount());
            parallelFor(0, mine->size(), [&](std::size_t s) { (*mine)[s] = queryOf(s); }, 4096);
            queries = mine;
            const std::lock_guard<std::mutex> lock(madeMutex_);
            if (made_.assigned && made_.queries && made_.partsKey == partsKey && made_.assigned->size() == mine->size() &&
                made_.queries->size() == mine->size() && made_.roots.size() == roots.size() &&
                (roots.empty() || std::memcmp(made_.roots.data(), roots.data(), roots.size() * sizeof(Vec3f)) == 0) &&
                (mine->empty() || std::memcmp(made_.queries->data(), mine->data(), mine->size() * sizeof(Vec3f)) == 0)) {
                reuse = made_.assigned;
                queries = made_.queries;
            }
        } else if (last && made.queries && made.partsKey == partsKey && made.queries->size() == in->strandCount()) {
            // The queries of the strands made again: the kept ones unless one moved.
            queries = made.queries;
            std::shared_ptr<std::vector<Vec3f>> moved;
            for (const std::uint32_t s : dirty) {
                const Vec3f q = queryOf(s);
                if (std::memcmp(&q, &(*queries)[s], sizeof(Vec3f)) == 0) continue;
                if (!moved) moved = std::make_shared<std::vector<Vec3f>>(*queries);
                (*moved)[s] = q;
            }
            if (moved) queries = moved;
            if (moved && fromStrands) return false;  // a strand moved center: the axes in full
            if (moved && regionsFromStrands) return false;  // a center's nearest strand may be another
        }
        if (last && fromStrands && !queries) return false;

        auto out = std::make_shared<StrandSet>(*in);
        std::vector<float>* clumpIds = nullptr;
        std::shared_ptr<std::vector<std::uint32_t>> assigned;
        if (last) {
            // The strands left alone as the last result made them: its points, clump IDs and centers.
            positionsOfLast(*out, last, *incremental);
            if (!channel.empty()) {
                clumpIds = out->copyStrandChannelFrom(channel, *last);
                if (!clumpIds) return false;
            }
            assigned = std::make_shared<std::vector<std::uint32_t>>(*made.assigned);
        } else {
            // Every point written: each strand's copied in from the input before it is clumped.
            out->overwritePositions();
            clumpIds = channel.empty() ? nullptr : &out->addStrandChannel(channel, -1.0f);
            assigned = std::make_shared<std::vector<std::uint32_t>>(out->strandCount(), kNoCenter);
        }

        // Each strand's center (kNoCenter: not clumped), searched from these queries, centers and
        // parts before (reuse), or now.
        auto centerOf = [&](std::size_t s) -> std::uint32_t {
            if (in->strandPointCount(s) == 0) return kNoCenter;
            if (reuse) return (*reuse)[s];
            const Vec3f* pts = in->strandPoints(s);
            if (vanes.active()) {
                const std::size_t c = vanes.nearest(s, pts[0]);
                return c == SIZE_MAX ? kNoCenter : static_cast<std::uint32_t>(c);  // a shaft, or a vane without a center
            }
            const Vec3f query = queries ? (*queries)[s] : binding.stablePosition(in->roots()[s], pts[0]);
            // The centers it may clump toward: all, or its region's (none: not clumped).
            const PointIndex* search = &index;
            const std::vector<std::uint32_t>* region = nullptr;
            if (strandRegions) {
                const float mine = (*strandRegions)[s];
                const auto it = mine == mine ? regionCenters.find(mine) : regionCenters.end();
                if (it == regionCenters.end()) return kNoCenter;
                search = &*it->second.index;
                region = &it->second.centers;
            }
            auto centerAt = [&](std::size_t i) { return region ? static_cast<std::size_t>((*region)[i]) : i; };
            if (!parts) return static_cast<std::uint32_t>(centerAt(search->nearest(query)));
            // The nearest center on this strand's side of any part.
            std::array<std::size_t, 16> near{};
            std::array<float, 16> d2{};
            const std::size_t found = search->kNearest(query, near.size(), near.data(), d2.data());
            std::size_t c = centerAt(near[0]);  // everything across: the nearest
            const PartLines::Side side = parts->sideOf(binding.refined() ? restRootOr(scalp, in->roots()[s], pts[0]) : query);
            for (std::size_t i = 0; i < found; ++i) {
                if (!PartLines::separated(side, centerSides[centerAt(near[i])])) {
                    c = centerAt(near[i]);
                    break;
                }
            }
            return static_cast<std::uint32_t>(c);
        };
        if (last) {
            forEachListed(dirty, [&](std::size_t s) { (*assigned)[s] = centerOf(s); });
            // From the strands, the axes hold while no strand changes its center.
            for (const std::uint32_t s : dirty) {
                if (fromStrands && (*assigned)[s] != (*made.assigned)[s]) return false;
            }
        } else {
            parallelFor(0, assigned->size(), [&](std::size_t s) { (*assigned)[s] = centerOf(s); }, 1024);
            if (fromStrands) {
                // Each center's axis: of the core strands (of 2 points or more) it gathers, the one
                // whose query lies nearest its root (with vanes: whose start nearest its start), the
                // lowest index of equals; packed (squared distance's bits, strand) to take the least.
                // A center gathering no core strand takes the core strand rooted nearest it.
                std::unique_ptr<std::atomic<std::uint64_t>[]> best(new std::atomic<std::uint64_t>[roots.size()]);
                for (std::size_t c = 0; c < roots.size(); ++c) best[c].store(~std::uint64_t{0}, std::memory_order_relaxed);
                parallelFor(0, assigned->size(), [&](std::size_t s) {
                    const std::uint32_t c = (*assigned)[s];
                    if (c == kNoCenter || in->strandPointCount(s) < 2 || !core.core(s)) return;
                    const Vec3f d = vanes.active() ? in->strandPoints(s)[0] - starts[c] : (*queries)[s] - roots[c];
                    const float d2 = d.dot(d);
                    std::uint32_t bits = 0;
                    std::memcpy(&bits, &d2, sizeof bits);
                    const std::uint64_t key = (static_cast<std::uint64_t>(bits) << 32) | static_cast<std::uint64_t>(s);
                    std::uint64_t seen = best[c].load(std::memory_order_relaxed);
                    while (key < seen && !best[c].compare_exchange_weak(seen, key, std::memory_order_relaxed)) {
                    }
                }, 4096);
                axes.resize(roots.size());
                std::vector<std::size_t> lacking;
                for (std::size_t c = 0; c < roots.size(); ++c) {
                    const std::uint64_t b = best[c].load(std::memory_order_relaxed);
                    axes[c] = b == ~std::uint64_t{0} ? kNoCenter : static_cast<std::uint32_t>(b & 0xffffffffu);
                    if (axes[c] == kNoCenter && !vanes.active()) lacking.push_back(c);
                }
                if (!lacking.empty()) {
                    std::vector<std::uint32_t> curved;
                    std::vector<Vec3f> at;
                    for (std::size_t s = 0; s < in->strandCount(); ++s) {
                        if (in->strandPointCount(s) < 2 || !core.core(s)) continue;
                        curved.push_back(static_cast<std::uint32_t>(s));
                        at.push_back((*queries)[s]);
                    }
                    if (!curved.empty()) {
                        const PointIndex nearestCore(std::move(at));
                        for (const std::size_t c : lacking) {
                            const std::size_t k = nearestCore.nearest(roots[c]);
                            if (k != SIZE_MAX) axes[c] = curved[k];
                        }
                    }
                }
                axisCurves();
            }
        }

        // Round locks and ribbons: each center's cross-section frames, from the scalp's normal at
        // its root (frameSeed), so the locks turn with the scalp.
        std::vector<Vec3f> centerNormals(roots.size());
        for (std::size_t c = 0; c < roots.size(); ++c) centerNormals[c] = rootNormalOr(scalp, used[c]->root);
        std::vector<Frame> frames;
        if (radius > 0.0f || shaped) {
            frames.resize(curves.size());
            parallelFor(0, roots.size(), [&](std::size_t c) {
                thread_local std::vector<Frame> one;  // scratch, reused across centers
                const Vec3f* pts = curves.data() + c * kCenterSamples;
                rotationMinimizingFramesInto(pts, kCenterSamples, frameSeed(centerNormals[c], pts[0], pts[1]), one);
                std::copy(one.begin(), one.end(), frames.begin() + static_cast<std::ptrdiff_t>(c * kCenterSamples));
            }, 64);
        }
        // Each center's cumulative arc length at its samples: a strand's point is pulled toward
        // the center's point as far from the root as the point is along its strand, so short
        // hairs stay by the clump's root (as Ornatrix's and Yeti's) rather than reaching its tip.
        std::vector<float> centerArcs(curves.size());
        parallelFor(0, roots.size(), [&](std::size_t c) {
            cumulativeArcLength(curves.data() + c * kCenterSamples, kCenterSamples, centerArcs.data() + c * kCenterSamples);
        }, 256);
        // Flat clumps: each center's way across that its ribbon is thin in, before it is taken
        // square to the center at each sample: the scalp's normal at its root (lying on the
        // scalp), a random turn about its frame (random), or across the width's direction.
        std::vector<Vec3f> thinWay;
        if (flatness > 0.0f) {
            thinWay.resize(roots.size());
            for (std::size_t c = 0; c < roots.size(); ++c) {
                const Frame& first = frames[c * kCenterSamples];
                Vec3f way = first.normal;
                if (flatten == "random") {
                    const float a = kTwoPi * unitFloat(hashCombine(used[c]->id, seed ^ 0xF1A7));
                    way = first.normal * pm::cos(a) + first.binormal * pm::sin(a);
                } else if (flatten == "direction") {
                    way = first.tangent.cross(flattenDirection);  // across the tangent and the width's way
                } else if (scalp && scalp->contains(used[c]->root)) {
                    way = scalp->normal(used[c]->root);
                }
                thinWay[c] = way;
            }
        }

        const RampAlong along(profile, *out);
        const RampAlong volumizeAlong(volumizeProfile, *out);
        // A clumped point's offset from its center's point `center` (its frame there `fr`)
        // squashed across by the flatness (as much as the point clumps, w).
        auto shape = [&](Vec3f& p, std::size_t c, const Vec3f& center, const Frame& fr, float w) {
            Vec3f offset = p - center;
            Vec3f thin = thinWay[c] - fr.tangent * thinWay[c].dot(fr.tangent);
            thin = thin.length() > 1e-6f ? thin.normalized() : fr.normal;  // along the center: its frame's
            offset -= thin * (offset.dot(thin) * flatness * w);
            p = center + offset;
        };

        auto clumpStrand = [&](std::size_t s) {
            const std::size_t c = (*assigned)[s];
            if (c == kNoCenter) return;
            const std::size_t n = out->strandPointCount(s);
            Vec3f* pts = out->strandPoints(s);
            if (clumpIds) (*clumpIds)[s] = numbers[c];

            float escape = strengthRandom > 0.0f ? clumpStrength(c) : 1.0f;
            if (strandRandom > 0.0f) escape *= 1.0f - strandRandom * unitFloat(hashCombine(out->ids()[s], seed ^ 0x5E7A9ull));
            // Volumize: read on the strand as it came (its points are still the input's here).
            const float volume = volumizing ? volumize.at(*out, s) : 0.0f;
            // Point i pushed out from the center's axis (its point `axisPoint`, running along
            // `tangent` there) by the volume there (across the axis).
            auto fill = [&](std::size_t i, const Vec3f& axisPoint, const Vec3f& tangent) {
                const float v = volume * volumizeAlong(i, n);
                if (v == 0.0f) return;
                const Vec3f d = pts[i] - axisPoint;
                pts[i] += (d - tangent * d.dot(tangent)) * v;
            };
            const float strandAmount = strengthPerPoint ? 0.0f : strength.at(*out, s) * escape;
            auto amountAt = [&](std::size_t i) {
                return strengthPerPoint ? strength.atPoint(*out, s, i) * escape : strandAmount;
            };
            const Vec3f* center = curves.data() + c * kCenterSamples;
            const float* centerArc = centerArcs.data() + c * kCenterSamples;
            const float centerLength = centerArc[kCenterSamples - 1];
            const Frame* fr = frames.empty() ? nullptr : frames.data() + c * kCenterSamples;
            // pastTip extend: past the center's tip, on along its last segment's way.
            const Vec3f tipWay =
                extend ? (center[kCenterSamples - 1] - center[kCenterSamples - 2]).normalized() : Vec3f{};
            // Each point is measured along the strand as it came (`arc`, from the root): pulled
            // toward (or, at a negative strength, repelled from) the center's point as far along
            // the center, past its tip the tip (or on past it: extend).
            const Vec3f* src = in->strandPoints(s);
            float arc = 0.0f;
            std::size_t k = 0;  // the center's sample at or before `arc` (centerSampleAt)
            bool repelling = false;
            Vec3f skin;
            float turn = 0.0f;  // its way round a center it sits on: the lock's angle
            auto repel = [&](std::size_t i, float raw) {
                if (!(raw < 0.0f) || repelRadius <= 0.0f) return false;
                if (!repelling) {
                    const StrandRoot& root = out->roots()[s];
                    skin = scalp && scalp->contains(root) ? scalp->normal(root) : Vec3f{};
                    turn = kTwoPi * unitFloat(hashCombine(out->ids()[s], seed ^ 0x10C4));
                    repelling = true;
                }
                pts[i] = repelled(pts[i], center, centerLength, arc, std::min(-raw, 1.0f), repelRadius, src[0] - center[0], turn,
                                  skin, paramAt(i, n));
                return true;
            };
            // A fixed spot in the lock's disk (uniform over its area), in the center's frame there.
            float angle = 0.0f, r = 0.0f;
            if (radius > 0.0f) {
                const std::uint64_t h = hashCombine(out->ids()[s], seed ^ 0x10C4);
                angle = kTwoPi * unitFloat(h);
                r = (radiusRandom > 0.0f ? clumpRadius(c) : radius) * std::sqrt(unitFloat(hash64(h)));
            }
            for (std::size_t i = 1; i < n; ++i) {
                arc += (src[i] - src[i - 1]).length();
                const float raw = amountAt(i) * along(i, n);
                if (repel(i, raw)) continue;
                const float w = std::clamp(raw, 0.0f, 1.0f);
                float f = 0.0f;
                centerSampleAt(centerArc, arc, k, &f);
                Vec3f at = center[k] * (1.0f - f) + center[k + 1] * f;
                if (extend && arc > centerLength) at += tipWay * (arc - centerLength);
                if (radius > 0.0f) {
                    const Frame frame = frameBetween(fr[k], fr[k + 1], f);
                    const Vec3f target = at + frame.toWorld({r * pm::cos(angle), r * pm::sin(angle), 0.0f});
                    pts[i] += (target - pts[i]) * w;
                    if (shaped) shape(pts[i], c, at, frame, w);
                } else {
                    pts[i] += (at - pts[i]) * w;
                    if (shaped) shape(pts[i], c, at, frameBetween(fr[k], fr[k + 1], f), w);
                }
                if (volume != 0.0f) fill(i, at, (center[k + 1] - center[k]).normalized());
            }
            if (!repelling) return;
            // Lengths kept, as Tangle keeps them, after a push out: each point back at its
            // segment's length from the one before, along where the pull or push put it (follow the
            // leader). A segment nothing moved stays as it came, bit for bit (the strands beyond the
            // repel radius). (Every strand's: a RestoreLength after the Clump.)
            for (std::size_t i = 1; i < n; ++i) {
                const Vec3f d = pts[i] - pts[i - 1];
                const float len = d.length();
                const float l = (src[i] - src[i - 1]).length();
                if (len == l) continue;
                pts[i] = len > 1e-12f ? pts[i - 1] + d * (l / len) : pts[i - 1] + (src[i] - src[i - 1]);
            }
        };
        if (last) {
            // Each from its incoming points and no clump ID, as in a full run.
            forEachListed(dirty, [&](std::size_t s) {
                copyStrandPoints(*out, *in, s);
                if (clumpIds) (*clumpIds)[s] = -1.0f;
                clumpStrand(s);
            });
            *changed = StrandChanges::listed(std::move(dirty));
        } else {
            parallelFor(0, out->strandCount(), [&](std::size_t s) {
                copyStrandPoints(*out, *in, s);
                clumpStrand(s);
            });
        }
        {
            const std::lock_guard<std::mutex> lock(madeMutex_);
            made_.output = out;
            made_.roots = std::move(roots);
            made_.curves = std::move(curves);
            made_.centerNormals = std::move(centerNormals);
            made_.assigned = std::move(assigned);
            made_.centerGuides = std::move(centerGuides);
            made_.chosen = std::move(chosen);
            made_.axes = std::move(axes);
            made_.ids.resize(used.size());
            for (std::size_t c = 0; c < used.size(); ++c) made_.ids[c] = used[c]->id;
            made_.picked = std::move(picked);
            made_.queries = std::move(queries);
            made_.partsKey = partsKey;
            made_.regionHash = regionHash;
            made_.centerRegions = std::move(centerRegions);
            made_.regionsFromStrands = regionsFromStrands;
        }
        *result = StrandsPtr(out);
        return true;
    }

    // Copy (XGen's and Yeti's), on the centers' curves (kCenterSamples points each, root first)
    // before any strand clumps toward them: each curve takes on `copyAmount` of the mean shape (its
    // points less its root) of its `copyNeighbors` nearest centers by root. Roots stay; nothing
    // changes at the default (0). (Clump noise: a Noise node on the centers before the Clump.)
    // With `axes` (centersFrom inputPoints), a center without an axis strand neither gives nor takes.
    void shareCenters(const std::vector<Vec3f>& roots, std::vector<Vec3f>& curves,
                      const std::vector<std::uint32_t>* axes) const {
        const std::size_t count = roots.size();
        const auto copyAmount = static_cast<float>(params().getDouble("copyAmount"));
        if (copyAmount > 0.0f && count > 1) {
            const auto k = std::min<std::size_t>(static_cast<std::size_t>(params().getInt("copyNeighbors")), count - 1);
            const PointIndex index(roots);
            const std::vector<Vec3f> own = curves;
            auto shapeless = [&](std::size_t c) { return axes && (*axes)[c] == kNoCenter; };
            parallelFor(0, count, [&](std::size_t c) {
                if (shapeless(c)) return;
                std::array<std::size_t, 9> near{};
                std::array<float, 9> d2{};
                const std::size_t found = index.kNearest(roots[c], k + 1, near.data(), d2.data());
                std::array<Vec3f, kCenterSamples> shape{};
                std::size_t taken = 0;
                for (std::size_t j = 0; j < found && taken < k; ++j) {
                    if (near[j] == c || shapeless(near[j])) continue;
                    const Vec3f* other = own.data() + near[j] * kCenterSamples;
                    for (std::size_t i = 0; i < kCenterSamples; ++i) shape[i] += other[i] - other[0];
                    ++taken;
                }
                if (taken == 0) return;
                const Vec3f* mine = own.data() + c * kCenterSamples;
                Vec3f* curve = curves.data() + c * kCenterSamples;
                for (std::size_t i = 1; i < kCenterSamples; ++i) {
                    const Vec3f offset = mine[i] - mine[0];
                    curve[i] = mine[0] + offset + (shape[i] / static_cast<float>(taken) - offset) * copyAmount;
                }
            }, 16);
        }
    }

    // The strands as they came, every one in no clump (the channel's -1), when there is nothing
    // to clump toward: what reads the channel after (Curl by clump, Merge) finds it either way.
    StrandsPtr unclumped(const StrandsPtr& in) const {
        const std::string channel = params().getString("clumpChannel");
        if (channel.empty()) return in;
        auto out = std::make_shared<StrandSet>(*in);
        out->addStrandChannel(channel, -1.0f);
        return StrandsPtr(out);
    }

    mutable std::mutex madeMutex_;
    mutable Made made_;
    mutable std::mutex pickMutex_;
    mutable std::vector<std::pair<std::uint64_t, std::shared_ptr<const Pick>>> pickMemo_;  // newest first
};

// Procedural guide curves (Yeti-style): strands grown and shaped in the graph
// become guides, for Clump centers (secondary clumping), Interpolate or
// anything else taking guides. Keeps a stable fraction of the core strands (preview_core.h: the
// strands every preview holds, so the guides are the same at every preview density; all of a
// small scatter's, or of one never thinned); their channels come along as guide attributes (point
// channels as per-point layers).
class ToGuidesOp : public Operator {
public:
    std::string typeName() const override { return op::kToGuides; }
    std::string category() const override { return "grow"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    PortSpec output() const override { return {"guides", PortType::Guides}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            notAnimatable({"fraction", 1.0, "Fraction of the strands that become guides, chosen by ID (stable), of those every "
                                            "preview holds (all of a small or unthinned set)", "fraction"})
                .range(0, 1),
            notAnimatable({"seed", std::int64_t{0}, "Random seed for the choice"}).adv(),
            {"channels", true, "Carry strand channels over as guide attributes (point channels as per-point layers)"},
            ParamSpec{"color", Vec3f(0.25f, 0.85f, 0.55f), "The color these guides draw in when they are shown as guides "
                                                          "(a Sculpt node after this one), linear RGB", "color"},
        };
    }

    Value evaluate(const EvalContext&, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, 0, op::kToGuides);
        auto out = std::make_shared<GuideSet>();
        out->setDisplayColor(params().getVec3("color"));
        const float fraction = static_cast<float>(params().getDouble("fraction"));
        const auto seed = static_cast<std::uint64_t>(params().getInt("seed"));
        const bool channels = params().getBool("channels");
        std::vector<std::string> names = channels ? in->strandChannelNames() : std::vector<std::string>{};
        names.erase(std::remove(names.begin(), names.end(), std::string(kPreviewChannel)), names.end());
        const std::vector<std::string> layers = channels ? in->pointChannelNames() : std::vector<std::string>{};
        // Guides of the core strands (preview_core.h): the same guides at every preview density.
        const PreviewCore core(*in);
        for (std::size_t s = 0; s < in->strandCount(); ++s) {
            if (!core.core(s)) continue;
            if (fraction < 1.0f && unitFloat(hashCombine(in->ids()[s], seed ^ 0x70C0)) >= fraction) continue;
            const Vec3f* pts = in->strandPoints(s);
            const std::size_t begin = in->strandBegin(s), n = in->strandPointCount(s);
            Guide& g = out->add(in->ids()[s], std::vector<Vec3f>(pts, pts + n));
            g.root = in->roots()[s];
            for (const std::string& name : names) g.attributes[name] = (*in->strandChannel(name))[s];
            // The guide has the strand's points: a point channel is its layer as it is.
            for (const std::string& name : layers) {
                const std::vector<float>& values = *in->pointChannel(name);
                g.pointAttributes[name].assign(values.begin() + static_cast<std::ptrdiff_t>(begin),
                                               values.begin() + static_cast<std::ptrdiff_t>(begin + n));
            }
        }
        return GuidesPtr(out);
    }
};

// The spacing of a kinky Noise's knots in the noise's space: apart by more than a lattice cell, so
// neighbouring kinks are drawn about independently.
constexpr float kKinkSpacing = 1.618034f;

// Smooth random displacement. "world" space is coherent between neighbors
// (waves through the groom); "strand" space is independent per strand. On a
// fraction of the strands, with a high frequency, it is frizz. In `kinky` mode
// the displacement is jagged instead: random offsets at knots `frequency` to
// the strand's length (strand space) or to a unit of length (world space),
// straight between them, so the strand breaks into sharp kinks (Yeti's
// Scraggle, Ornatrix's Frizz); in world space the knots' offsets come from the
// noise at the root, so neighbours kink alike.
class NoiseOp : public Operator {
public:
    std::string typeName() const override { return op::kNoise; }
    std::string category() const override { return "look"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"amplitude", 0.2, "Displacement size, scene units (mappable, per point)", "length"}.soft(0, 1),
            ParamSpec{"frequency", 3.0, "Features per scene unit (world) or per strand length (strand)", "perLength"}
                .unitsWhen("space", {"world"})
                .soft(0, 20),
            ParamSpec{"octaves", std::int64_t{2}, "Fractal octaves"}.range(1, 8).adv(),
            {"profile", Ramp::linear(0.0f, 1.0f), "Amplitude along the length, root to tip"},
            {"space", std::string("strand"), "strand: independent per strand; world: coherent across strands", "",
             {"strand", "world"}},
            ParamSpec{"fraction", 1.0, "Share of the strands that get it, 0-1 (mappable): a few, at a high frequency, "
                                       "for frizz", "fraction"}
                .soft(0, 1),
            notAnimatable({"seed", std::int64_t{0}, "Random seed"}).adv(),
            {"mode", std::string("smooth"),
             "smooth: waves; kinky: sharp kinks, straight between random offsets frequency apart (scraggle, frizz)", "",
             {"smooth", "kinky"}},
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

private:
    // In full, or (with `incremental`) only the strands the input changed in, into the last
    // result's copy; false when that cannot be done.
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, 0, op::kNoise);
        const ScalarField amplitudeField(params(), "amplitude", scalpOf(ctx));
        amplitudeField.validate(*in);
        const ScalarField fractionField(params(), "fraction", scalpOf(ctx));
        fractionField.validate(*in);
        const bool perPoint = amplitudeField.perPoint();
        const float frequency = static_cast<float>(params().getDouble("frequency"));
        const int octaves = static_cast<int>(params().getInt("octaves"));
        const Ramp profile = params().getRamp("profile");
        const bool strandSpace = params().getString("space") != "world";
        const auto seed = static_cast<std::uint64_t>(params().getInt("seed"));
        const bool kinky = params().getString("mode") == "kinky";

        const StrandChanges* changes = nullptr;
        StrandsPtr last;  // the last result, to make again only the strands the input changed in
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            if (!last || !amplitudeField.strandLocal() || !fractionField.strandLocal()) return false;
            if (changes->none()) {  // the same strands in: the same result
                *result = last;
                *changed = *changes;
                return true;
            }
        }
        auto out = std::make_shared<StrandSet>(*in);
        if (last) {
            positionsOfLast(*out, last, *incremental);  // the strands left alone, as they were made
        } else {
            out->overwritePositions();  // every point written: each strand copied in before it is shaped
        }
        const RampAlong along(profile, *out);
        // Smooth noise is read at the strand's points: the octaves finer than they draw fade out
        // (octaveWeights), by the point spacing in the noise's space: in strand space a strand
        // spans `frequency` cells whatever its length, in world space a unit of length does.
        // (Kinky noise draws its knots `frequency` apart itself: nothing to fade.)
        auto spacingOf = [&](const Vec3f* pts, std::size_t n) {
            if (kinky || n < 2) return 0.0f;
            const auto segments = static_cast<float>(n - 1);
            return strandSpace ? frequency / segments : arcLength(pts, n) * frequency / segments;
        };
        if (!kinky && strandSpace && !last) {
            const std::size_t uniform = in->uniformPointCount();
            float base = 1.0f;
            if (uniform >= 2) octaveWeights(spacingOf(nullptr, uniform), 1, &base);
            if (base == 0.0f) {
                ctx.note("frequency " + std::to_string(frequency) + " is finer than the strands' " + std::to_string(uniform - 1) +
                         " segments draw: the noise fades out; raise Resample's segments or lower the frequency");
            }
        }
        auto shape = [&](std::size_t s) {  // strand s's points: the incoming ones, shaped
            const float fraction = fractionField.at(*out, s);
            if (fraction < 1.0f && unitFloat(hashCombine(out->ids()[s], seed ^ 0xF822)) >= fraction) return;
            const std::size_t n = out->strandPointCount(s);
            Vec3f* pts = out->strandPoints(s);
            // Strand space: a random far-apart offset per strand, walked along the length.
            Rng rng(hashCombine(out->ids()[s], seed));
            const Vec3f strandOffset = randomOffsetZYX(rng);
            const float amplitude = perPoint ? 0.0f : amplitudeField.at(*out, s);
            float weights[kMaxNoiseOctaves];
            octaveWeights(spacingOf(pts, n), octaves, weights);
            VectorNoise noise(seed, octaves, weights);
            if (kinky) {
                // Knot k's offset: the noise at the strand's offset (strand space) or its root
                // (world), k spacings along z; each point straight between the knots around it,
                // by its share of the length (strand space) or its arc length (world), both read
                // on the points as they came.
                const Vec3f base = strandSpace ? strandOffset : pts[0] * frequency;
                auto knot = [&](float k) {
                    const Vec3f q = base + Vec3f(0.0f, 0.0f, k * kKinkSpacing);
                    return strandSpace ? noise(q) : vectorNoise(q, seed, octaves);
                };
                bool holding = false;  // a knot pair below
                float held = 0.0f;     // the knot it starts at
                Vec3f below, above;
                float arc = 0.0f;
                Vec3f previous = pts[0];
                for (std::size_t i = 1; i < n; ++i) {
                    const Vec3f original = pts[i];
                    arc += (original - previous).length();
                    previous = original;
                    const float a = (perPoint ? amplitudeField.atPoint(*out, s, i) : amplitude) * along(i, n);
                    if (a == 0.0f) continue;
                    const float u = (strandSpace ? paramAt(i, n) : arc) * frequency;
                    const float k = std::floor(u);
                    if (!holding || k != held) {
                        below = holding && k == held + 1.0f ? above : knot(k);
                        above = knot(k + 1.0f);
                        held = k;
                        holding = true;
                    }
                    pts[i] = original + (below + (above - below) * (u - k)) * a;
                }
                return;
            }
            for (std::size_t i = 1; i < n; ++i) {
                const float t = paramAt(i, n);
                const float a = (perPoint ? amplitudeField.atPoint(*out, s, i) : amplitude) * along(i, n);
                if (a == 0.0f) continue;
                // Each point is sampled at its own position before it moves.
                const Vec3f q = strandSpace ? strandOffset + Vec3f(0, 0, t * frequency) : pts[i] * frequency;
                // World space: neighbours rarely share a lattice cell, so the cache would only cost.
                pts[i] += (strandSpace ? noise(q) : vectorNoise(q, seed, octaves, weights)) * a;
            }
        };
        if (last) {
            forEachListed(changes->strands, [&](std::size_t s) {
                copyStrandPoints(*out, *in, s);
                shape(s);
            });
            *changed = *changes;
        } else {
            parallelFor(0, out->strandCount(), [&](std::size_t s) {
                copyStrandPoints(*out, *in, s);
                shape(s);
            });
        }
        *result = StrandsPtr(out);
        return true;
    }
};

// Writes a per-strand channel, read downstream through attribute maps (and by
// renderers): a number, or any map read once (texture, painted color set, painted
// guide map, an expression: fit(rand, 0.2, 0.8) for a random value per strand): paint
// one mask, drive clump, length and noise from it.
class AttributeOp : public Operator {
public:
    std::string typeName() const override { return op::kAttribute; }
    std::string category() const override { return "utility"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"name", std::string("attr"),
                      "The attribute's name: the per-strand channel this node writes (an attribute map, an expression, "
                      "a renderer reads it by name; a name made upstream is overwritten)"}
                .writesChannel(),
            ParamSpec{"value", 1.0,
                      "The value per strand (mappable: a number, a map, or an expression such as fit(rand, 0.2, 0.8) for "
                      "a random value per strand)"}
                .soft(0, 1),
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    // Each strand's value comes from that strand alone: read again for the strands the input
    // changed in (their channels or points may have), the last result's for every other strand.
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

private:
    // In full, or (with `incremental`) only the strands the input changed in, into the last
    // result's copy; false when that cannot be done.
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const StrandsPtr in = requireStrands(inputs, 0, op::kAttribute);
        const std::string name = params().getString("name");
        if (name.empty()) throw GraphError("Attribute: channel name is empty");
        const StrandChanges* changes = nullptr;
        StrandsPtr last;  // the last result, to make again only the strands the input changed in
        if (incremental) {
            changes = someStrandsChanged(*incremental, 0);
            last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            if (!last) return false;
        }
        const ScalarField field(params(), "value", scalpOf(ctx));
        field.validate(*in);
        if (last && !field.strandLocal()) return false;
        if (last && changes->none()) {  // the same strands in: the same result
            *result = last;
            *changed = *changes;
            return true;
        }
        auto out = std::make_shared<StrandSet>(*in);
        if (field.isConstant()) {
            out->addStrandChannel(name, field.at(*in, 0));
            if (last) *changed = *changes;
            *result = StrandsPtr(out);
            return true;
        }
        std::vector<float>& channel = out->addStrandChannel(name, 0.0f);
        if (last) {
            // Each strand made again reads the set a full evaluation reads (its own entry of
            // the channel not written yet); then the last result's values for the rest.
            const std::vector<std::uint32_t>& strands = changes->strands;
            std::vector<float> made(strands.size());
            parallelFor(0, strands.size(), [&](std::size_t k) { made[k] = field.at(*out, strands[k]); }, 128);
            std::vector<float>* kept = out->copyStrandChannelFrom(name, *last);
            if (!kept) return false;
            for (std::size_t k = 0; k < strands.size(); ++k) (*kept)[strands[k]] = made[k];
            *changed = *changes;
        } else {
            parallelFor(0, out->strandCount(), [&](std::size_t s) { channel[s] = field.at(*out, s); });
        }
        *result = StrandsPtr(out);
        return true;
    }
};

// Where two sub-grooms meet, each strand within `blend` of the other side's
// roots leans toward the shape of its nearest strands there (of the other side's core strands,
// preview_core.h: the same at every preview density; root-relative, resampled to its own point
// count): halfway at the seam, so both sides meet in the same shape, fading to its own shape at
// `blend`. Writes into `out`.
void matchAcrossSeam(const StrandSet& own, const StrandSet& other, float blend, float match, StrandSet& out) {
    // The other side's core strands with a shape to lean toward (2 points or more), by their roots.
    std::vector<Vec3f> otherRoots;
    std::vector<std::size_t> shaped;
    const PreviewCore core(other);
    for (std::size_t s = 0; s < other.strandCount(); ++s) {
        if (other.strandPointCount(s) < 2 || !core.core(s)) continue;
        otherRoots.push_back(other.strandPoints(s)[0]);
        shaped.push_back(s);
    }
    const PointIndex index(otherRoots);
    constexpr std::size_t kNear = 4;
    parallelFor(0, own.strandCount(), [&](std::size_t s) {
        const std::size_t n = own.strandPointCount(s);
        if (n < 2) return;
        const Vec3f* pts = own.strandPoints(s);
        std::size_t near[kNear];
        float d2[kNear];
        const std::size_t found = index.kNearest(pts[0], kNear, near, d2);
        if (found == 0) return;
        const float d = std::sqrt(d2[0]);
        if (d >= blend) return;
        const float t = 1.0f - d / blend;
        const float w = 0.5f * match * t * t * (3.0f - 2.0f * t);  // smooth: half at the seam
        thread_local std::vector<Vec3f> shape, resampled;  // scratch, reused across strands
        shape.assign(n, Vec3f{});
        resampled.resize(n);
        float total = 0.0f;
        for (std::size_t k = 0; k < found; ++k) {
            const std::size_t j = shaped[near[k]];
            const std::size_t m = other.strandPointCount(j);
            const float wk = 1.0f / (d2[k] + 1e-4f * blend * blend);
            resampleByArcLengthInto(other.strandPoints(j), m, resampled.data(), n);
            for (std::size_t p = 0; p < n; ++p) shape[p] += (resampled[p] - resampled[0]) * wk;
            total += wk;
        }
        if (total <= 0.0f) return;
        Vec3f* dst = out.strandPoints(s);
        for (std::size_t p = 1; p < n; ++p) {
            const Vec3f target = pts[0] + shape[p] / total;
            dst[p] = pts[p] + (target - pts[p]) * w;
        }
    });
}

// Channels numbering groups of strands (-1: none), which every Clump, Braid and
// Propagate (its feathers) in parallel branches counts from 0: Merge numbers b's
// groups on from a's, so two merged looks' clumps, braids and feathers stay apart.
constexpr const char* kGroupIdChannels[] = {kClumpIdChannel, "braidId", kFeatherChannel};

// Makes b's strand IDs unique against a's (and b's own earlier ones): an ID
// already taken is rehashed until it is new, deterministically, so two
// Scatters with the same seed still give strands of their own. IDs that do
// not collide stay as they are. Returns old -> new for the IDs changed.
std::unordered_map<StrandId, StrandId> makeIdsUnique(const StrandSet& a, StrandSet& b) {
    // The IDs taken, in one flat table (open addressing, at most half full: every ID either side
    // adds fits), 0 apart as the empty slot's mark; a node per ID cost most of a Merge's time.
    class Taken {
    public:
        explicit Taken(std::size_t most) {
            std::size_t size = 16;
            while (size < 2 * most) size <<= 1;
            slots_.assign(size, 0);
            mask_ = size - 1;
        }
        bool insert(StrandId id) {
            if (id == 0) return !std::exchange(zero_, true);
            for (std::size_t i = static_cast<std::size_t>(hash64(id)) & mask_;; i = (i + 1) & mask_) {
                if (slots_[i] == id) return false;
                if (slots_[i] == 0) {
                    slots_[i] = id;
                    return true;
                }
            }
        }

    private:
        std::vector<StrandId> slots_;
        std::size_t mask_ = 0;
        bool zero_ = false;
    };
    std::unordered_map<StrandId, StrandId> renamed;
    Taken taken(a.strandCount() + b.strandCount());
    for (const StrandId id : a.ids()) taken.insert(id);
    for (std::size_t s = 0; s < b.strandCount(); ++s) {
        const StrandId id = b.ids()[s];
        if (taken.insert(id)) continue;
        StrandId fresh = id;
        do {
            fresh = hashCombine(fresh, 0x9E3779B97F4A7C15ull);
        } while (!taken.insert(fresh));
        b.mutableIds()[s] = fresh;
        renamed.emplace(id, fresh);
    }
    return renamed;
}


bool hasAll(const StrandSet& s, bool perPoint) {
    for (const char* name : kColorChannels) {
        if (!(perPoint ? s.pointChannel(name) : s.strandChannel(name))) return false;
    }
    return true;
}

// A strand's color on a side of a Merge or Blend: its HairColor RGB, else its melanin's
// color, else HairColor's default look.
// Its channels are looked up once, then read per strand.
class SideColor {
public:
    SideColor() = default;
    explicit SideColor(const StrandSet& side) {
        if (hasAll(side, false)) {
            for (int k = 0; k < 3; ++k) rgb_[k] = side.strandChannel(kColorChannels[k]);
            return;
        }
        melanin_ = side.strandChannel(kMelaninChannel);
        redness_ = side.strandChannel(kMelaninRednessChannel);
    }
    std::array<float, 3> operator()(std::size_t s) const {
        if (rgb_[0]) return {(*rgb_[0])[s], (*rgb_[1])[s], (*rgb_[2])[s]};
        if (!melanin_) return defaultHairColor();
        return melaninColor((*melanin_)[s], redness_ ? (*redness_)[s] : kDefaultMelaninRedness);
    }

private:
    std::array<const std::vector<float>*, 3> rgb_{};
    const std::vector<float>* melanin_ = nullptr;
    const std::vector<float>* redness_ = nullptr;
};

// HairColor's pigment channels with the values of HairColor's default look.
constexpr std::pair<const char*, float> kPigmentDefaults[] = {{kMelaninChannel, kDefaultMelanin},
                                                              {kMelaninRednessChannel, kDefaultMelaninRedness}};

// The value of look channel `name` (kLookChannels) on a strand of a side that lacks it, when
// `other` has it: HairColor's default pigment, else the value `other` added it with (what
// strands without it take when merged in).
float missingLookValue(const std::string& name, const StrandSet& other) {
    for (const auto& [pigment, value] : kPigmentDefaults) {
        if (name == pigment) return value;
    }
    return other.strandChannelDefault(name);
}

// Where only one side of a Merge has HairColor's channels, the other side's
// strands get HairColor's default look (or their own melanin's color), not
// the zero that would export as black; per-point colors on one side only get
// each of the other side's strands' color along it (StrandSet::append already
// spreads a side's own per-strand RGB along its points; this also covers a
// side colored by melanin alone). `out` is a then b.
void fillMissingColor(const StrandSet& a, const StrandSet& b, StrandSet& out) {
    const std::size_t strands[2] = {0, a.strandCount()}, points[2] = {0, a.pointCount()};
    const StrandSet* sides[2] = {&a, &b};
    for (int side = 0; side < 2; ++side) {
        const StrandSet& own = *sides[side];
        const StrandSet& other = *sides[1 - side];
        const std::size_t first = strands[side], count = own.strandCount();
        for (const auto& [name, value] : kPigmentDefaults) {
            if (own.strandChannel(name) || !other.strandChannel(name)) continue;
            std::vector<float>& values = *out.mutableStrandChannel(name);
            std::fill(values.begin() + first, values.begin() + first + count, value);
        }
        const SideColor sideColor(own);
        if (!hasAll(own, false) && hasAll(other, false)) {
            std::array<std::vector<float>*, 3> rgb{};
            for (int k = 0; k < 3; ++k) rgb[k] = out.mutableStrandChannel(kColorChannels[k]);
            for (std::size_t s = 0; s < count; ++s) {
                const std::array<float, 3> c = sideColor(s);
                for (int k = 0; k < 3; ++k) (*rgb[k])[first + s] = c[k];
            }
        }
        if (!hasAll(own, true) && hasAll(other, true)) {
            std::array<std::vector<float>*, 3> rgb{};
            for (int k = 0; k < 3; ++k) rgb[k] = out.mutablePointChannel(kColorChannels[k]);
            for (std::size_t s = 0; s < count; ++s) {
                const std::array<float, 3> c = sideColor(s);
                const std::size_t begin = points[side] + own.strandBegin(s), end = begin + own.strandPointCount(s);
                for (int k = 0; k < 3; ++k) std::fill(rgb[k]->begin() + begin, rgb[k]->begin() + end, c[k]);
            }
        }
    }
}

bool sameInstance(const InstanceSet& x, const InstanceSet::Instance& i, const InstanceSet& y,
                  const InstanceSet::Instance& j) {
    auto name = [](const InstanceSet& set, std::uint32_t p) {
        return p < set.prototypes.size() ? set.prototypes[p] : std::string();
    };
    return i.id == j.id && i.strand == j.strand && name(x, i.prototype) == name(y, j.prototype) && i.t == j.t &&
           i.orient == j.orient && i.direction == j.direction && i.spin == j.spin && i.scale == j.scale &&
           i.offset == j.offset;
}

// Both sides' instances, once: an Instance node upstream of both sides (its
// instances the same set, or the start of both, as each Instance node adds
// to the ones before it) is not doubled. b's instances follow its renamed strands.
std::shared_ptr<const InstanceSet> mergeInstances(const std::shared_ptr<const InstanceSet>& a,
                                                  const std::shared_ptr<const InstanceSet>& b,
                                                  const std::unordered_map<StrandId, StrandId>& renamed) {
    if (!b || b == a) return a;
    std::size_t shared = 0;
    if (a) {
        const std::size_t most = std::min(a->size(), b->size());
        while (shared < most && sameInstance(*a, a->instances[shared], *b, b->instances[shared])) ++shared;
    }
    InstanceSet added;
    added.prototypes = b->prototypes;
    added.instances.assign(b->instances.begin() + static_cast<std::ptrdiff_t>(shared), b->instances.end());
    for (InstanceSet::Instance& i : added.instances) {
        auto it = renamed.find(i.strand);
        if (it != renamed.end()) i.strand = it->second;
    }
    if (added.empty()) return a;
    auto merged = std::make_shared<InstanceSet>(a ? *a : InstanceSet{});
    merged->append(added);
    return merged;
}

// Concatenates two strand sets with their channels (a channel only one side
// has takes its default on the other side's strands), optionally blending
// their shapes where they meet so there is no seam. b's strands get IDs of
// their own where a has them already, its clumps and braids are numbered
// after a's, and HairColor's channels on one side only get HairColor's
// default look on the other. Instances and cards both sides have from one
// node upstream are kept once.
class MergeOp : public Operator {
public:
    std::string typeName() const override { return op::kMerge; }
    bool carriesChannels(std::size_t) const override { return true; }
    std::string category() const override { return "utility"; }
    std::vector<PortSpec> inputs() const override {
        return {{"a", PortType::Strands}, {"b", PortType::Strands, true}};
    }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"blend", 0.0, "Where a and b meet, strands this close to the other side take on its shape, "
                                    "meeting halfway at the seam (0: no blending; nothing on b: a passes through)", "length"}
                .soft(0, 2),
            ParamSpec{"match", 1.0, "How far the shapes meet at the seam, 0-1", "fraction"}.range(0, 1),
        };
    }

    Value evaluate(const EvalContext&, const std::vector<Value>& inputs) const override {
        const StrandsPtr a = requireStrands(inputs, 0, op::kMerge);
        const StrandsPtr b = valueAs<StrandsPtr>(inputs.at(1));
        if (!b || b->empty()) return a;
        const float blend = static_cast<float>(params().getDouble("blend"));
        const float match = static_cast<float>(params().getDouble("match"));
        auto out = std::make_shared<StrandSet>(*a);
        auto bOut = std::make_shared<StrandSet>(*b);  // shares b's arrays until written
        if (blend > 0.0f && match > 0.0f && !a->empty()) {
            // Both sides lean toward the other as it was (not as already blended).
            matchAcrossSeam(*a, *b, blend, match, *out);
            matchAcrossSeam(*b, *a, blend, match, *bOut);
        }
        const auto renamed = makeIdsUnique(*a, *bOut);
        offsetGroupIds(*a, *bOut);
        out->append(*bOut);  // both sides' channels
        fillMissingColor(*a, *bOut, *out);
        out->setInstances(mergeInstances(a->instances(), b->instances(), renamed));
        for (const auto& layer : b->cardLayers()) {  // a Cards node upstream of both sides: once
            const auto& have = out->cardLayers();
            if (std::find(have.begin(), have.end(), layer) == have.end()) out->addCardLayer(layer);
        }
        for (const auto& layer : b->featherLayers()) {  // likewise a Propagate node upstream of both
            const auto& have = out->featherLayers();
            if (std::find(have.begin(), have.end(), layer) == have.end()) out->addFeatherLayer(layer);
        }
        return StrandsPtr(out);
    }
    // Without a seam to blend each merged strand is its side's strand alone (its IDs, renamed or
    // not, stay with the IDs; b's groups numbered on from a's highest, which stays while a's
    // changed strands keep their groups): a's changed strands and b's after a's count, merged
    // again on their own and written over the last result's.
    bool evaluateIncremental(const EvalContext&, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* result, StrandChanges* changed) const override {
        const StrandsPtr a = requireStrands(inputs, 0, op::kMerge);
        const StrandsPtr b = valueAs<StrandsPtr>(inputs.at(1));
        if (!b || b->empty() || a->empty()) return false;  // a handed on: the graph passes its changes on
        const float blend = static_cast<float>(params().getDouble("blend"));
        const float match = static_cast<float>(params().getDouble("match"));
        if (blend > 0.0f && match > 0.0f) return false;
        const StrandChanges* changesA = someStrandsChanged(incremental, 0);
        const StrandChanges* changesB = someStrandsChanged(incremental, 1);
        const StrandsPtr last = valueAs<StrandsPtr>(incremental.previous);
        if (!last || last->strandCount() != a->strandCount() + b->strandCount() ||
            last->pointCount() != a->pointCount() + b->pointCount()) {
            return false;
        }
        // Only widths or channels changed on a side (a Width or HairColor edit on a branch), the
        // other as it was or so too: the last result's points, each changed side's widths and
        // channels written over its range.
        if (inputPointsKept(incremental, 0) && inputPointsKept(incremental, 1) &&
            (!inputUnchanged(incremental, 0) || !inputUnchanged(incremental, 1))) {
            if (StrandsPtr made = attributesOver(*a, *b, last, !inputUnchanged(incremental, 0), !inputUnchanged(incremental, 1))) {
                *result = std::move(made);
                *changed = StrandChanges{};
                return true;
            }
        }
        // One side made anew (a branch evaluated in full: a whisker group's guides edited) while the
        // other's changes are known: that side's every strand, when it holds the strands it held
        // (each strand's IDs, roots and point count, its channels and their defaults, no instances,
        // cards or feathers of its own), so the nodes after go on incrementally.
        StrandChanges wholeA, wholeB;
        if (!changesA && changesB && sameLayoutAs(*a, *last, 0, 0, true)) {
            wholeA = everyStrand(a->strandCount());
            changesA = &wholeA;
        } else if (changesA && !changesB && sameLayoutAs(*b, *last, a->strandCount(), a->pointCount(), false)) {
            wholeB = everyStrand(b->strandCount());
            changesB = &wholeB;
        }
        if (!changesA || !changesB) return false;
        // A side made anew holding most strands: no faster than the full merge.
        if ((changesA == &wholeA || changesB == &wholeB) &&
            2 * (changesA->strands.size() + changesB->strands.size()) > last->strandCount()) {
            return false;
        }
        if (changesA->none() && changesB->none()) {
            *result = last;
            *changed = StrandChanges::listed({});
            return true;
        }
        std::vector<std::size_t> pickA(changesA->strands.begin(), changesA->strands.end());
        std::vector<std::size_t> pickB(changesB->strands.begin(), changesB->strands.end());
        // b's groups go on from a's highest: a's changed strands keep their groups as merged before.
        for (const char* name : kGroupIdChannels) {
            const std::vector<float>* mine = a->strandChannel(name);
            const std::vector<float>* before = last->strandChannel(name);
            if (!mine || !b->strandChannel(name)) continue;
            if (!before) return false;
            for (const std::size_t s : pickA) {
                if (std::memcmp(&(*mine)[s], &(*before)[s], sizeof(float)) != 0) return false;
            }
        }
        // The changed strands merged as the full merge merges them: b's groups numbered on from
        // all of a's, the colors one side lacks filled in the same way.
        const StrandSet someA = a->subset(pickA);
        StrandSet someB = b->subset(pickB);
        offsetGroupIds(*a, someB);
        StrandSet merged = someA;
        merged.append(someB);
        fillMissingColor(someA, someB, merged);
        std::vector<std::uint32_t> at;
        at.reserve(pickA.size() + pickB.size());
        at.insert(at.end(), changesA->strands.begin(), changesA->strands.end());
        for (const std::uint32_t s : changesB->strands) at.push_back(static_cast<std::uint32_t>(a->strandCount() + s));
        if (merged.strandChannelNames() != last->strandChannelNames() || merged.pointChannelNames() != last->pointChannelNames()) {
            return false;
        }
        auto out = copyOfLast(last, incremental);
        writeStrands(*out, merged, at);
        *result = StrandsPtr(out);
        *changed = StrandChanges::listed(std::move(at));
        return true;
    }

private:
    // The merge of a and b where each side holds the points it held when `last` was made
    // (StrandChanges::pointsKept), `changedA`/`changedB` its widths or channels changed: `last`'s
    // arrays, the changed sides' widths and channel values written over their ranges, as the full
    // merge lays them (a side's own values; b's groups numbered on from a's). Null where a changed
    // side lacks a channel the merge has (filled in from the other side's: the full merge's), or a's
    // groups changed (b's numbering would).
    static StrandsPtr attributesOver(const StrandSet& a, const StrandSet& b, const StrandsPtr& last, bool changedA, bool changedB) {
        const bool perPoint = a.widths().size() == a.pointCount() && b.widths().size() == b.pointCount() &&
                              last->widths().size() == last->pointCount();
        if (!perPoint) return nullptr;
        const std::vector<std::string> strandNames = last->strandChannelNames(), pointNames = last->pointChannelNames();
        auto holdsAll = [&](const StrandSet& side) {
            if (side.strandChannelNames() != strandNames || side.pointChannelNames() != pointNames) return false;
            for (const std::string& name : strandNames) {
                if (side.strandChannelDefault(name) != last->strandChannelDefault(name)) return false;
            }
            for (const std::string& name : pointNames) {
                if (side.pointChannelDefault(name) != last->pointChannelDefault(name)) return false;
            }
            return true;
        };
        if ((changedA && !holdsAll(a)) || (changedB && !holdsAll(b))) return nullptr;
        if (changedA) {
            for (const char* name : kGroupIdChannels) {
                const std::vector<float>* now = a.strandChannel(name);
                const std::vector<float>* before = last->strandChannel(name);
                if (now && (!before || std::memcmp(now->data(), before->data(), now->size() * sizeof(float)) != 0)) return nullptr;
            }
        }
        // b as the full merge appends it: its groups numbered on from a's.
        StrandSet bOut = b;
        if (changedB) offsetGroupIds(a, bOut);
        auto out = std::make_shared<StrandSet>(*last);
        std::vector<float>* widths = nullptr;
        std::map<std::string, std::vector<float>*> strandWritten, pointWritten;
        // `from` over `to` from element `at`, where it differs (each array taken for writing once).
        auto over = [](const std::vector<float>& from, std::size_t at, const std::vector<float>& before, const auto& writable) {
            if (from.empty() || std::memcmp(from.data(), before.data() + at, from.size() * sizeof(float)) == 0) return;
            std::vector<float>& to = writable();
            detail::copyBytesParallel(to.data() + at, from.data(), from.size() * sizeof(float));
        };
        const StrandSet* sides[2] = {&a, &bOut};
        const bool changedSide[2] = {changedA, changedB};
        const std::size_t firstStrand[2] = {0, a.strandCount()}, firstPoint[2] = {0, a.pointCount()};
        for (int side = 0; side < 2; ++side) {
            if (!changedSide[side]) continue;
            const StrandSet& own = *sides[side];
            over(own.widths(), firstPoint[side], last->widths(), [&]() -> std::vector<float>& {
                if (!widths) widths = &out->copyWidthsFrom(*last);
                return *widths;
            });
            for (const std::string& name : strandNames) {
                over(*own.strandChannel(name), firstStrand[side], *last->strandChannel(name), [&]() -> std::vector<float>& {
                    std::vector<float>*& to = strandWritten[name];
                    if (!to) to = out->copyStrandChannelFrom(name, *last);
                    return *to;
                });
            }
            for (const std::string& name : pointNames) {
                over(*own.pointChannel(name), firstPoint[side], *last->pointChannel(name), [&]() -> std::vector<float>& {
                    std::vector<float>*& to = pointWritten[name];
                    if (!to) to = out->copyPointChannelFrom(name, *last);
                    return *to;
                });
            }
        }
        return out;
    }
    static StrandChanges everyStrand(std::size_t count) {
        std::vector<std::uint32_t> all(count);
        for (std::size_t s = 0; s < count; ++s) all[s] = static_cast<std::uint32_t>(s);
        return StrandChanges::listed(std::move(all));
    }
    // Whether `side` holds the strands `last` holds from strand `first` (point `firstPoint`) on, as
    // a merge of it would: the same IDs (b's: kept as they come, none renamed), roots and point
    // counts, the channels' defaults `last` has, no instances, card or feather layers of its own;
    // a's groups (kGroupIdChannels) as before, so b's numbered on from them stay.
    static bool sameLayoutAs(const StrandSet& side, const StrandSet& last, std::size_t first, std::size_t firstPoint, bool isA) {
        const std::size_t n = side.strandCount();
        if (first + n > last.strandCount() || (side.instances() && !side.instances()->empty()) || !side.cardLayers().empty() ||
            !side.featherLayers().empty()) {
            return false;
        }
        if (n == 0) return true;
        if (std::memcmp(side.ids().data(), last.ids().data() + first, n * sizeof(StrandId)) != 0 ||
            std::memcmp(side.roots().data(), last.roots().data() + first, n * sizeof(StrandRoot)) != 0) {
            return false;
        }
        const std::vector<std::size_t>& mine = side.offsets();
        const std::vector<std::size_t>& theirs = last.offsets();
        for (std::size_t s = 0; s <= n; ++s) {
            if (theirs[first + s] - firstPoint != mine[s]) return false;
        }
        for (const std::string& name : side.strandChannelNames()) {
            if (!last.strandChannel(name) || last.strandChannelDefault(name) != side.strandChannelDefault(name)) return false;
        }
        for (const std::string& name : side.pointChannelNames()) {
            if (!last.pointChannel(name) || last.pointChannelDefault(name) != side.pointChannelDefault(name)) return false;
        }
        if (isA) {
            for (const char* name : kGroupIdChannels) {
                const std::vector<float>* now = side.strandChannel(name);
                const std::vector<float>* before = last.strandChannel(name);
                if (!now) continue;
                if (!before || std::memcmp(now->data(), before->data(), n * sizeof(float)) != 0) return false;
            }
        }
        return true;
    }
};

// Passes one of two strand sets through, by index (variants, A/B looks).
class SwitchOp : public Operator {
public:
    std::string typeName() const override { return op::kSwitch; }
    bool carriesChannels(std::size_t slot) const override { return inputActive(slot); }
    std::string category() const override { return "utility"; }
    std::vector<PortSpec> inputs() const override {
        return {{"a", PortType::Strands, true}, {"b", PortType::Strands, true}};
    }
    std::vector<ParamSpec> paramSpecs() const override {
        return {ParamSpec{"index", std::int64_t{0}, "0: pass a, 1: pass b"}.soft(0, 1)};
    }
    bool inputActive(std::size_t slot) const override {
        return slot == (params().getInt("index") == 0 ? 0u : 1u);
    }

    Value evaluate(const EvalContext&, const std::vector<Value>& inputs) const override {
        const std::size_t slot = params().getInt("index") == 0 ? 0 : 1;
        StrandsPtr picked = valueAs<StrandsPtr>(inputs.at(slot));
        if (!picked) throw GraphError(std::string("Switch: input ") + (slot == 0 ? "a" : "b") + " not connected");
        return picked;
    }
    // The same index as for the last result (the node's own key): the picked input, changed as
    // it changed.
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* result, StrandChanges* changed) const override {
        const std::size_t slot = params().getInt("index") == 0 ? 0 : 1;
        if (slot >= incremental.inputs.size() || !incremental.inputs[slot]) return false;
        *result = evaluate(ctx, inputs);
        *changed = *incremental.inputs[slot];
        return true;
    }
};

// Two looks of the same strands mixed, e.g. dry (a) and wet (b): each point
// moves from a toward b by `mix` (mappable, per point, or a Control node for
// a keyable slider) times profile (along the length: wet ends, dry roots).
// Widths blend too, and with `include` shapeAndLook (the default) so does the
// look, as a lerp by the same weights: HairColor's RGB per point (whenever a
// side has per-point colors or the weight varies along the strands, so a
// painted per-point amount or a profile shows in the color), its per-strand
// RGB and the other look channels (kLookChannels) by the strand's mean weight
// along it. A side without HairColor's channels counts as its melanin's color
// or HairColor's default look, as in Merge. IDs and every other channel come
// from a. Both inputs must hold the same strands, so branch them after one
// Interpolate (or any shared node).
class BlendOp : public Operator {
public:
    std::string typeName() const override { return op::kBlend; }
    std::string category() const override { return "utility"; }
    std::vector<PortSpec> inputs() const override { return {{"a", PortType::Strands}, {"b", PortType::Strands}}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            // 1 by default: a plugged mask multiplies it, so the mask alone is the mix (0.5 halved it)
            ParamSpec{"mix", 1.0, "0: a, 1: b (mappable, per point)", "fraction"}.soft(0, 1),
            {"profile", Ramp::constant(1.0f), "Mix along the length, root to tip"},
            {"include", std::string("shapeAndLook"),
             "What moves toward b: the shape (points, widths) and the look (HairColor's color, melanin, redness, "
             "roughness), or the shape alone; IDs and other channels always stay a's",
             "", {"shapeAndLook", "shape"}},
            ParamSpec{"mixChannel", std::string("blend"), "Per-strand channel receiving the mix, e.g. for the shader (empty: "
                                                          "none)"}
                .adv()
                .writesChannel(),
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    // Each strand is mixed from its own a and b strands: the strands either side changed in are
    // mixed again (with a mix that reads each strand alone), the rest keep the last result's.
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

private:
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        StrandsPtr a = requireStrands(inputs, 0, op::kBlend);
        StrandsPtr b = requireStrands(inputs, 1, op::kBlend);
        if (a->strandCount() != b->strandCount() || a->ids() != b->ids()) {
            throw GraphError("Blend: a and b must be the same strands (branch both from one node, e.g. Interpolate; "
                             "no Scatter, Length cut or Merge in between)");
        }
        // The same strands with points of their own (a Curl adding points to tight curls on one
        // side): each strand resampled by arc length to the more points either side has.
        bool layoutsDiffer = false;
        for (std::size_t s = 0; s < a->strandCount() && !layoutsDiffer; ++s) {
            layoutsDiffer = a->strandPointCount(s) != b->strandPointCount(s);
        }
        if (layoutsDiffer) {
            if (incremental) return false;
            std::vector<std::size_t> points(a->strandCount());
            for (std::size_t s = 0; s < points.size(); ++s) points[s] = std::max(a->strandPointCount(s), b->strandPointCount(s));
            a = resampleStrands(*a, points);
            b = resampleStrands(*b, points);
        }
        const ScalarField amount(params(), "mix", scalpOf(ctx));
        amount.validate(*a);
        const bool perPoint = amount.perPoint();
        const Ramp ramp = params().getRamp("profile");
        const RampAlong profile(ramp, *a);
        const std::string channel = params().getString("mixChannel");
        const bool look = params().getString("include") != "shape";

        // Incrementally: the strands either side changed in, into the last result's copy.
        StrandsPtr last;
        std::vector<std::uint32_t> remade;
        if (incremental) {
            const StrandChanges* changesA = someStrandsChanged(*incremental, 0);
            const StrandChanges* changesB = someStrandsChanged(*incremental, 1);
            last = changesA && changesB ? previousStrandsFor(*incremental, *a) : nullptr;
            if (!last || !amount.strandLocal()) return false;
            std::set_union(changesA->strands.begin(), changesA->strands.end(), changesB->strands.begin(),
                           changesB->strands.end(), std::back_inserter(remade));
            if (remade.empty()) {
                *result = last;
                *changed = StrandChanges::listed({});
                return true;
            }
        }

        auto out = std::make_shared<StrandSet>(*a);
        // An array the mix writes: a's own copy (`own`: a has it) or a new one of `fallback`;
        // incrementally the last result's copy (null when it has none).
        auto writable = [&](bool point, const char* name, bool own, float fallback) -> std::vector<float>* {
            if (last) return point ? out->copyPointChannelFrom(name, *last) : out->copyStrandChannelFrom(name, *last);
            if (own) return point ? out->mutablePointChannel(name) : out->mutableStrandChannel(name);
            return point ? &out->addPointChannel(name, fallback) : &out->addStrandChannel(name, fallback);
        };
        std::vector<float>* amounts = channel.empty() ? nullptr : writable(false, channel.c_str(), false, 0.0f);
        if (last && !channel.empty() && !amounts) return false;
        const bool widths = a->widths().size() == a->pointCount() && b->widths().size() == b->pointCount();
        std::vector<float>& outWidths = last ? widthsOfLast(*out, last, *incremental) : out->mutableWidths();
        if (last) positionsOfLast(*out, last, *incremental);
        const bool flat = std::all_of(ramp.keys.begin(), ramp.keys.end(),
                                      [&](const auto& key) { return key.second == ramp.keys.front().second; });
        const LookMix mix = look ? LookMix(*a, *b, perPoint || !flat, writable) : LookMix();
        if (!mix.complete) return false;
        auto blendStrand = [&](std::size_t s) {
            if (last) {  // made again from a's strand, as a full evaluation's copy of a starts it
                copyStrandPoints(*out, *a, s);
                if (outWidths.size() == a->widths().size()) {
                    const auto begin = static_cast<std::ptrdiff_t>(a->strandBegin(s));
                    const auto end = begin + static_cast<std::ptrdiff_t>(a->strandPointCount(s));
                    std::copy(a->widths().begin() + begin, a->widths().begin() + end, outWidths.begin() + begin);
                }
            }
            const float w = std::clamp(amount.at(*a, s), 0.0f, 1.0f);  // per point: the root's, for the channel
            if (amounts) (*amounts)[s] = w;
            const std::size_t begin = out->strandBegin(s), n = out->strandPointCount(s);
            // Same totals, different split: the strand stays a's (shape and look).
            const bool same = begin == b->strandBegin(s) && n == b->strandPointCount(s);
            const auto weightAt = [&](std::size_t k) {
                if (!same) return 0.0f;
                return (perPoint ? std::clamp(amount.atPoint(*a, s, k), 0.0f, 1.0f) : w) * profile(k, n);
            };
            float sum = 0.0f;
            if (same) {
                Vec3f* pts = out->strandPoints(s);
                const Vec3f* to = b->strandPoints(s);
                for (std::size_t k = 0; k < n; ++k) {
                    const float f = weightAt(k);
                    sum += f;
                    pts[k] = pts[k] + (to[k] - pts[k]) * f;
                    if (widths) {
                        float& width = outWidths[begin + k];
                        width += (b->widths()[begin + k] - width) * f;
                    }
                }
            }
            if (look) mix.strand(s, n > 0 ? sum / static_cast<float>(n) : 0.0f, begin, n, weightAt);
        };
        if (last) {
            forEachListed(remade, blendStrand);
            *changed = StrandChanges::listed(std::move(remade));
        } else {
            parallelFor(0, out->strandCount(), blendStrand);
        }
        *result = StrandsPtr(out);
        return true;
    }

    // Blend's look: where the mixed channels go in the output and where they come from.
    struct LookMix {
        bool rgb = false;                                // HairColor's per-strand RGB is mixed
        std::array<std::vector<float>*, 3> outRgb{};     // the output's per-strand RGB
        std::array<std::vector<float>*, 3> outPoints{};  // its per-point RGB (null: none)
        std::array<const std::vector<float>*, 3> aPoints{}, bPoints{};  // a side's own (null: its strand color)
        struct Channel {
            std::vector<float>* out;
            const std::vector<float>* a;  // null: the side lacks it and takes `aMissing`
            const std::vector<float>* b;
            float aMissing, bMissing;
        };
        std::vector<Channel> channels;  // the kLookChannels either side has
        SideColor aColor, bColor;       // each side's strand colors
        bool complete = true;           // every array to write was had (incrementally: the last result has each)

        LookMix() = default;
        // Sets up the output (a copy of a) to receive the mix, each array taken by
        // writable(point, name, own, fallback) (Blend::run): channels only b has are added with
        // b's default; per-point RGB when a side has it or `varies` (the weight changes along
        // the strands) and there is RGB to mix.
        template <class Writable>
        LookMix(const StrandSet& a, const StrandSet& b, bool varies, const Writable& writable) : aColor(a), bColor(b) {
            auto take = [&](bool point, const char* name, bool own, float fallback) {
                std::vector<float>* to = writable(point, name, own, fallback);
                complete = complete && to;
                return to;
            };
            const bool aRgb = hasAll(a, false), bRgb = hasAll(b, false);
            rgb = aRgb || bRgb;
            if (rgb) {
                for (int k = 0; k < 3; ++k) {
                    const char* name = kColorChannels[k];
                    outRgb[k] = take(false, name, aRgb, b.strandChannelDefault(name));
                }
                const bool aPts = hasAll(a, true), bPts = hasAll(b, true);
                if (aPts || bPts || varies) {
                    for (int k = 0; k < 3; ++k) {
                        const char* name = kColorChannels[k];
                        aPoints[k] = aPts ? a.pointChannel(name) : nullptr;
                        bPoints[k] = bPts ? b.pointChannel(name) : nullptr;
                        // Without b's point channel: the output's strand channel's default (a's,
                        // else b's, added just above).
                        const float strandDefault = aRgb ? a.strandChannelDefault(name) : b.strandChannelDefault(name);
                        outPoints[k] = take(true, name, aPts, bPts ? b.pointChannelDefault(name) : strandDefault);
                    }
                }
            }
            for (const char* name : kLookChannels) {
                const std::vector<float>* fromA = a.strandChannel(name);
                const std::vector<float>* fromB = b.strandChannel(name);
                if (!fromA && !fromB) continue;
                std::vector<float>* to = take(false, name, fromA != nullptr, b.strandChannelDefault(name));
                channels.push_back({to, fromA, fromB, missingLookValue(name, b), missingLookValue(name, a)});
            }
        }

        // Strand s mixed: its per-strand channels by `mean` (its mean weight along it), its
        // per-point colors by weightAt(i) at point i (of n, from `begin`, the same on both sides
        // wherever the weight is above 0).
        template <class Weight>
        void strand(std::size_t s, float mean, std::size_t begin, std::size_t n, const Weight& weightAt) const {
            for (const Channel& c : channels) {
                const float from = c.a ? (*c.a)[s] : c.aMissing, to = c.b ? (*c.b)[s] : c.bMissing;
                (*c.out)[s] = from + (to - from) * mean;
            }
            if (!rgb) return;
            const std::array<float, 3> from = aColor(s), to = bColor(s);
            for (int k = 0; k < 3; ++k) (*outRgb[k])[s] = from[k] + (to[k] - from[k]) * mean;
            if (!outPoints[0]) return;
            for (std::size_t i = 0; i < n; ++i) {
                const float f = weightAt(i);
                for (int k = 0; k < 3; ++k) {
                    const float x = aPoints[k] ? (*aPoints[k])[begin + i] : from[k];
                    if (f == 0.0f) {
                        (*outPoints[k])[begin + i] = x;
                        continue;
                    }
                    const float y = bPoints[k] ? (*bPoints[k])[begin + i] : to[k];
                    (*outPoints[k])[begin + i] = x + (y - x) * f;
                }
            }
        }
    };
};

}  // namespace

// Numbers b's groups (clumpId, braidId, featherId) after a's highest; -1 stays -1 (internal.h).
void offsetGroupIds(const StrandSet& a, StrandSet& b) {
    const PreviewCore core(a);  // the highest of a's core strands: the same at every preview density
    for (const char* name : kGroupIdChannels) {
        const std::vector<float>* mine = a.strandChannel(name);
        if (!mine || !b.strandChannel(name)) continue;
        float top = -1.0f;
        for (std::size_t s = 0; s < mine->size(); ++s) {
            if (core.core(s)) top = std::max(top, std::floor((*mine)[s]));
        }
        if (top < 0.0f) continue;
        for (float& v : *b.mutableStrandChannel(name)) {
            if (v >= 0.0f) v += static_cast<float>(groupNumberBase(static_cast<std::int64_t>(top)));
        }
    }
}
std::vector<float> stableGroupNumbers(const std::vector<std::uint64_t>& keys) {
    std::vector<std::tuple<std::uint64_t, std::uint64_t, std::size_t>> order(keys.size());
    for (std::size_t k = 0; k < keys.size(); ++k) order[k] = {hash64(keys[k] ^ 0xC1D5ull) % static_cast<std::uint64_t>(kGroupNumbers), keys[k], k};
    std::sort(order.begin(), order.end());
    std::vector<float> numbers(keys.size());
    std::unordered_set<std::uint64_t> used;
    used.reserve(keys.size());
    for (const auto& [wanted, key, k] : order) {
        std::uint64_t n = wanted;
        while (!used.insert(n).second) n = (n + 1) % static_cast<std::uint64_t>(kGroupNumbers);
        numbers[k] = static_cast<float>(n);
    }
    return numbers;
}

std::vector<std::size_t> pickSpreadStrands(const StrandSet& in, const ScalpMesh* scalp, float spacing, std::uint64_t seed) {
    ParamSet everyStrand;
    everyStrand.set("weight", 1.0);
    const ScalarField weight(everyStrand, "weight", nullptr);
    return placeClumpCenters(planClumpCenters(in, scalp, 0, spacing, weight, seed),
                             static_cast<float>(kCenterRandomness));
}

bool clumpState(const Operator& op, const StrandsPtr& output, ClumpState* state) {
    const auto* clump = dynamic_cast<const ClumpOp*>(&op);
    return clump && clump->state(output, state);
}

void registerClumpOperator(OperatorRegistry& registry) {
    registry.registerType(op::kClump,
                          {"look",
                           "Pulls strands toward their clump's center (or into round locks; Twist after it for ropes).",
                           "clumps groups locks"},
                          [] { return std::make_unique<ClumpOp>(); });
}

void registerToGuidesOperator(OperatorRegistry& registry) {
    registry.registerType(op::kToGuides,
                          {"convert", "Strands become guides, for nodes that follow guides.", "strands to guides"},
                          [] { return std::make_unique<ToGuidesOp>(); });
}

void registerNoiseOperator(OperatorRegistry& registry) {
    registry.registerType(op::kNoise,
                          {"look",
                           "Random displacement; on a fraction of the strands at a high frequency, frizz.",
                           "frizz random jitter"},
                          [] { return std::make_unique<NoiseOp>(); });
}

void registerAttributeOperator(OperatorRegistry& registry) {
    registry.registerType(op::kAttribute,
                          {"utility",
                           "Writes a per-strand channel for later nodes or the shader: a number, a map or an expression.",
                           "channel primvar data"},
                          [] { return std::make_unique<AttributeOp>(); });
}

void registerMergeOperator(OperatorRegistry& registry) {
    registry.registerType(op::kMerge,
                          {"utility", "Combines two sets of strands.", "combine join"},
                          [] { return std::make_unique<MergeOp>(); });
}

void registerSwitchOperator(OperatorRegistry& registry) {
    registry.registerType(op::kSwitch,
                          {"utility", "Picks one of several inputs: look variants.", "variant choose pick"},
                          [] { return std::make_unique<SwitchOp>(); });
}

void registerBlendOperator(OperatorRegistry& registry) {
    registry.registerType(op::kBlend,
                          {"utility", "Mixes two looks of the same strands (dry to wet).", "mix looks wet dry"},
                          [] { return std::make_unique<BlendOp>(); });
}

}  // namespace groomlab
