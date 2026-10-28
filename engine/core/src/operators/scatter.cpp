// SPDX-License-Identifier: Apache-2.0
// Scatter: places strand roots on the scalp by area density.
//
// Stability: sample k on face f always gets the same ID and position, so
// raising density only appends samples per face, and preview density filters
// by ID. Neither reshuffles existing strands.
//
// Randomness: every root has two spots, worked out apart from the randomness: its random one
// (white noise, from the seed) and its even one (blue noise: the roots relaxed apart on the scalp's
// rest pose, each until the spacing around it settles, so an edit of the scalp moves roots only
// near it). Each root sits `randomness` of the way from its even spot (0) to its random one (1),
// on the surface: a small change of the randomness moves every root a little and keeps the same
// strands, at every value. At exactly 1 only the random spots show, and no
// even ones are worked out (the fast default). The even spots are always every root's, relaxed
// together whatever a preview holds of them, so a preview's strands sit where the render's do.
// The placements with even spots are kept (the memo, without the randomness or the preview), and
// the last mixes: a randomness tweak only mixes again the roots the preview holds.
//
// Preview: a thinned preview holds the strands whose hashed ID falls under its density, and the
// core (preview_core.h: under the core share, coreShare of the strands it makes at full density)
// besides, those it does not show marked hidden in the roles channel; at every density, the render
// included, each strand's role says whether it is core. With previewAll it is never thinned: every
// strand core, no roles.
//
// Layout: random (the default: the randomness above; 0 spreads the roots evenly), or uvGrid: one
// root per cell of a rows x columns grid over the scalp's UVs (0-1), at the cell's center (or
// moved at random in it, gridRandom), on the first face (in face order) whose UVs hold it. Its IDs come
// from the cell, so a grid keeps its strands while the scalp deforms; a density map thins it.
//
// Around the guides (guideRadius): roots within guideRadius of a guide's root, or, for a guide
// whose tube was set (tube.h: TubeProfile::edited: a shape, or a sized round tube; whether or not
// an Interpolate fills it), on its root footprint instead (the root section laid flat on the scalp, TubeSpine::setRootSurface,
// as Interpolate's fillTubes finds the strands it fills) or within tubeMargin of its outline (in
// half-widths, Interpolate's tubeBlend's measure): so a flat blade's or a ribbon's footprint is
// filled, at the scatter's density, and not a disc round it.
//
// Subdivision: on a refined scalp (the Scalp node's subdivision, smooth preview, match render) the
// roots are placed on its cage, as on the scalp without subdivision, and each is carried onto the
// refined surface (CageBinding): the same strands, with the same IDs, whatever the level; only
// where they sit follows the surface drawn.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/point_index.h"
#include "groomlab/preview_core.h"
#include "groomlab/random.h"
#include "groomlab/scalp_pieces.h"
#include "groomlab/subdivision.h"
#include "groomlab/texture.h"
#include "groomlab/tube.h"
#include "internal.h"

namespace groomlab {

namespace {

constexpr double kMaxStrands = 50.0e6;
// Past this many strands (the render's, every one) the Scatter says it is heavy: a human head has
// about 100-150k hairs, and renders, exports and the viewport at full preview density hold them all.
constexpr double kHeavyStrands = 2.0e6;
constexpr double kMaxPreviewPoints = 60.0e6;  // about 1 GB per cached copy
constexpr double kMaxPoints = 400.0e6;         // any evaluation (renders too): past this, memory runs out
constexpr int kRelaxIterations = 24;
constexpr std::size_t kRelaxNeighbors = 24;    // itself and every neighbor within reach, about (two rings hold 12)
constexpr int kRelaxBlock = 4;                 // iterations per neighbor search and surface snap
constexpr float kRelaxReach = 2.0f;            // neighbors push within this many spacings
constexpr float kRelaxStep = 0.6f;             // spacings moved per unit of push
constexpr float kRelaxQuantum = 1.0f / 1024.0f;  // spacings: the lattice the relaxed roots land on
constexpr std::size_t kPlacedMemo = 2;         // placements a Scatter keeps (a preview and a render, say)
// The randomness's maximum: white noise (gaps and clusters past it are a density map's or a Clump's).
constexpr float kMaxRandomness = 1.0f;
// A scatter making this many strands or fewer is never thinned in a preview (previewAll's).
constexpr double kNeverThinned = 250.0;

std::atomic<std::size_t> relaxCount{0};
std::atomic<std::size_t> relaxIterationCount{0};

// The scalp's open edges (on one triangle only), each with the direction into
// the surface, and points along them every `step` to find the nearest within `reach` (binned in
// cells that wide: a root far from every edge looks through a few empty cells, not the edges).
struct OpenEdges {
    struct Edge {
        Vec3f a, b, inward;
    };
    std::vector<Edge> edges;
    std::vector<Vec3f> samples;
    std::vector<std::uint32_t> sampleEdge;
    std::unique_ptr<CellGrid> cells;

    OpenEdges(const ScalpMesh& mesh, float step, float reach) {
        std::map<std::pair<std::int32_t, std::int32_t>, std::pair<int, std::size_t>> uses;  // count, triangle
        const auto& tris = mesh.triangles();
        for (std::size_t f = 0; f < tris.size(); ++f) {
            for (int e = 0; e < 3; ++e) {
                const std::int32_t u = tris[f][static_cast<std::size_t>(e)], v = tris[f][static_cast<std::size_t>((e + 1) % 3)];
                auto& use = uses[{std::min(u, v), std::max(u, v)}];
                ++use.first;
                use.second = f;
            }
        }
        const auto& pts = mesh.points();
        for (const auto& [key, use] : uses) {
            if (use.first != 1) continue;
            const auto& tri = tris[use.second];
            std::int32_t other = tri[0];
            for (const std::int32_t v : tri) {
                if (v != key.first && v != key.second) other = v;
            }
            const Vec3f a = pts[static_cast<std::size_t>(key.first)], b = pts[static_cast<std::size_t>(key.second)];
            const Vec3f c = pts[static_cast<std::size_t>(other)];
            const Vec3f along = b - a;
            const float len2 = along.dot(along);
            if (!(len2 > 0.0f)) continue;
            Vec3f inward = (c - a) - along * ((c - a).dot(along) / len2);  // toward the triangle, in its plane
            if (!(inward.length() > 0.0f)) continue;
            inward = inward.normalized();
            const std::size_t count = std::min<std::size_t>(
                static_cast<std::size_t>(std::sqrt(len2) / std::max(step, 1e-9f)) + 1, 4096);
            for (std::size_t k = 0; k <= count; ++k) {
                samples.push_back(a + along * (static_cast<float>(k) / static_cast<float>(count)));
                sampleEdge.push_back(static_cast<std::uint32_t>(edges.size()));
            }
            edges.push_back({a, b, inward});
        }
        if (!samples.empty()) cells = std::make_unique<CellGrid>(samples, reach);
    }

    // The open edge sampled nearest p among those within `reach` (and some a little farther); -1
    // when there are none.
    std::int64_t nearest(const Vec3f& p) const {
        if (!cells) return -1;
        std::int64_t best = -1;
        float bestD2 = std::numeric_limits<float>::max();
        cells->around(p, [&](std::uint32_t k) {
            const Vec3f d = samples[k] - p;
            const float d2 = d.dot(d);
            if (d2 < bestD2 || (d2 == bestD2 && static_cast<std::int64_t>(sampleEdge[k]) < best)) {
                bestD2 = d2;
                best = sampleEdge[k];
            }
            return true;
        });
        return best;
    }
};

// Spreads roots evenly on the rest surface (blue noise): each pushes away from
// its neighbors within kRelaxReach spacings, harder the closer they are, for kRelaxIterations
// steps. Open edges of the scalp push back like a mirror image of the root across them, so roots
// don't pile up along a hairline. `spacing(i)` is the ideal distance around root i (from the
// local density); `edgeStep` the smallest (at the density's maximum), how finely the open edges
// are sampled. Roots keep their order (and so their IDs).
// Smooth: every root takes every step (none stops early), its push fading to nothing at the reach
// from every neighbor within it (kRelaxNeighbors hold them all, about), so a root placed a rounding
// apart (the same scalp in other units) ends a rounding apart: nothing it does is a choice a
// rounding could tip (a strand near the border of two clumps after it would change clumps). The
// roots land on a lattice of kRelaxQuantum spacings at the end only.
// Local: a root moves by its neighbors alone, so an edit of the scalp moves roots only within the
// reach the iterations carry it (kRelaxIterations x kRelaxReach spacings at most); farther, every
// root makes the same steps from the same neighbors, to the bit.
// `allowed(root)`: where a root may sit (density above 0, near the guides with guideRadius). A
// root pushed outside stays where it last was: the edge of a density map holds roots like a wall,
// instead of the roots along it, pushed only from inside, drifting out into the zero area.
template <typename Spacing, typename Allowed>
void relaxRoots(std::vector<StrandRoot>& roots, const ScalpMesh& rest, float edgeStep, Spacing spacing, Allowed allowed) {
    const std::size_t n = roots.size();
    if (n < 2) return;
    std::vector<Vec3f> pos(n), nrm(n);
    std::vector<float> space(n);
    parallelFor(0, n, [&](std::size_t i) {
        pos[i] = rest.position(roots[i]);
        nrm[i] = rest.normal(roots[i]);
        space[i] = spacing(i);
    });
    if (std::none_of(space.begin(), space.end(), [](float s) { return s > 0.0f; })) return;
    // An edge pushes a root within half its reach (its mirror image within the reach), the widest
    // spacing at most; a root moves up to half its spacing a step, so over a block (whose edges are
    // found at its start) one that can push it is within that plus kRelaxBlock half spacings, and a
    // sample of it within half a step more.
    const float widest = *std::max_element(space.begin(), space.end());
    const OpenEdges open(rest, 0.5f * edgeStep, widest * (1.0f + 0.5f * static_cast<float>(kRelaxBlock)) + edgeStep);
    std::vector<std::int64_t> wall(n, -1);  // each root's nearest open edge
    // Neighbors are found once per block and reused (roots move less than a
    // spacing per step); roots settle back onto the surface at each block's end.
    std::vector<std::uint32_t> near(n * kRelaxNeighbors);
    std::vector<Vec3f> moved(n);
    for (int iteration = 0; iteration < kRelaxIterations; ++iteration) {
        ++relaxIterationCount;
        if (iteration % kRelaxBlock == 0) {
            const PointIndex index(pos);
            parallelFor(0, n, [&](std::size_t i) {
                std::size_t found[kRelaxNeighbors];
                float d2[kRelaxNeighbors];
                const std::size_t count = index.kNearest(pos[i], kRelaxNeighbors, found, d2);
                for (std::size_t k = 0; k < kRelaxNeighbors; ++k) {
                    near[i * kRelaxNeighbors + k] = static_cast<std::uint32_t>(k < count ? found[k] : i);
                }
                wall[i] = open.nearest(pos[i]);
            });
        }
        parallelFor(0, n, [&](std::size_t i) {
            moved[i] = pos[i];
            if (!(space[i] > 0.0f)) return;
            Vec3f push{0.0f, 0.0f, 0.0f};
            for (std::size_t k = 0; k < kRelaxNeighbors; ++k) {
                const std::size_t j = near[i * kRelaxNeighbors + k];
                if (j == i || !(space[j] > 0.0f)) continue;
                const float reach = kRelaxReach * 0.5f * (space[i] + space[j]);
                const Vec3f between = pos[i] - pos[j];
                const float d2 = between.dot(between);
                if (d2 >= reach * reach) continue;
                const float d = std::sqrt(d2);
                const Vec3f away = d > 1e-6f * reach ? between * (1.0f / d)
                                                     : Vec3f{unitFloat(hash64(i)) - 0.5f, unitFloat(hash64(i + n)) - 0.5f, 0.0f};
                const float w = 1.0f - d / reach;
                push = push + away * (w * w);
            }
            if (wall[i] >= 0) {  // the mirror image across the nearest open edge
                const OpenEdges::Edge& e = open.edges[static_cast<std::size_t>(wall[i])];
                const Vec3f along = e.b - e.a;
                const float t = std::clamp((pos[i] - e.a).dot(along) / along.dot(along), 0.0f, 1.0f);
                const float d = 2.0f * (pos[i] - (e.a + along * t)).length();
                const float reach = kRelaxReach * space[i];
                if (d < reach) {
                    const float w = 1.0f - d / reach;
                    push = push + e.inward * (w * w);
                }
            }
            push = push - nrm[i] * push.dot(nrm[i]);  // along the surface
            // At most half a spacing a step, eased into (no corner where the step is cut).
            const Vec3f step = push * (kRelaxStep * space[i]);
            const float limit = 0.5f * space[i], len = step.length();
            moved[i] = pos[i] + step * (limit / std::sqrt(len * len + limit * limit));
            if (iteration + 1 == kRelaxIterations) {
                // On a lattice of kRelaxQuantum spacings: an edit's ripple, a rounding by then where
                // it reaches at all, lands where it would have.
                const float q = kRelaxQuantum * space[i];
                const Vec3f to = moved[i];
                moved[i] = {std::round(to.x / q) * q, std::round(to.y / q) * q, std::round(to.z / q) * q};
            }
        });
        if ((iteration + 1) % kRelaxBlock != 0 && iteration + 1 != kRelaxIterations) {
            pos.swap(moved);
            continue;
        }
        // Onto the surface.
        parallelFor(0, n, [&](std::size_t i) {
            if (!(space[i] > 0.0f)) return;
            const SurfacePoint hit = rest.closestPoint(moved[i], 4.0f * space[i]);
            if (hit.root.face < 0) return;  // off the surface: stays where it last was
            if (!allowed(hit.root)) {  // outside the density (or the guides' reach): back where it last was
                pos[i] = rest.position(roots[i]);
                return;
            }
            roots[i] = hit.root;
            pos[i] = hit.position;
            nrm[i] = hit.normal;
        });
    }
}

// Each root `randomness` of the way from its even spot to its random one (past it above 1), settled
// onto the surface. 0 and 1 are the spots themselves.
std::vector<StrandRoot> mixRoots(const std::vector<StrandRoot>& even, const std::vector<StrandRoot>& random,
                                 const ScalpMesh& rest, float randomness) {
    if (randomness == 0.0f) return even;
    if (randomness == 1.0f) return random;
    std::vector<StrandRoot> mixed(even.size());
    parallelFor(0, even.size(), [&](std::size_t i) {
        const Vec3f a = rest.position(even[i]), b = rest.position(random[i]);
        const Vec3f at = a + (b - a) * randomness;
        const SurfacePoint hit = rest.closestPoint(at, 2.0f * std::max(randomness, 1.0f) * (b - a).length() + 1e-6f);
        mixed[i] = hit.root.face >= 0 ? hit.root : (randomness < 0.5f ? even[i] : random[i]);
    });
    return mixed;
}


// Scatter's guideRadius for guides with a shaped tube: their root footprints (TubeSpine laid on
// the scalp), each holding the roots on it or within `margin` half-widths of its outline.
class GuideFootprints {
public:
    explicit GuideFootprints(float margin) : margin_(margin) {}
    // A guide's tube, its root moved to `root` (where the scatter reads it: the rest pose), laid
    // on the surface of normal `normal` there.
    void add(const Guide& g, const Vec3f& root, const Vec3f& normal) {
        std::vector<Vec3f> pts = g.points;
        const Vec3f move = root - pts.front();
        for (Vec3f& p : pts) p += move;
        TubeSpine spine(pts.data(), pts.size(), g.frameUp, g.tube);
        spine.setRootSurface(normal);
        if (spine.empty() || !(spine.rootWidth() > 0.0f) || !(spine.rootHeight() > 0.0f)) return;
        hash_ = hashTube(hashCombine(hash_, g.tube.roll.size()), g.tube);
        for (const float r : g.tube.roll) hash_ = hashFloat(hash_, r);
        for (const Vec3f& p : pts) hash_ = hashFloat(hashFloat(hashFloat(hash_, p.x), p.y), p.z);
        for (const Vec3f& v : {g.frameUp, normal}) hash_ = hashFloat(hashFloat(hashFloat(hash_, v.x), v.y), v.z);
        spines_.push_back(std::move(spine));
    }
    bool empty() const { return spines_.empty(); }
    std::uint64_t hash() const { return hashFloat(hashCombine(hash_, spines_.size()), margin_); }
    void index() {
        std::vector<Vec3f> centers;
        float cell = 0.0f;
        for (const TubeSpine& s : spines_) {
            const float extent = std::max(s.rootWidth(), s.rootHeight());
            cell = std::max(cell, extent * std::sqrt((1.0f + margin_) * (1.0f + margin_) + kAlong * kAlong));
            centers.push_back(s.atSample(0, 0.0f, 0.0f));
        }
        grid_ = std::make_unique<CellGrid>(centers, cell);
    }
    bool covers(const Vec3f& p) const {
        bool in = false;
        grid_->around(p, [&](std::uint32_t i) {
            const TubeSpine& s = spines_[i];
            float along = 0.0f;
            const SectionPoint at = s.rootCoords(p, &along);
            if (std::abs(along) > kAlong * std::max(s.rootWidth(), s.rootHeight())) return true;
            in = tubeOutsideDistance(s.tube(), s.rootHeight() / s.rootWidth(), at.u, at.v, margin_) <= margin_;
            return !in;
        });
        return in;
    }

private:
    static constexpr float kAlong = 4.0f;  // how far off the footprint's plane a root may lie, in root extents (Interpolate's)
    float margin_;
    std::vector<TubeSpine> spines_;
    std::unique_ptr<CellGrid> grid_;
    std::uint64_t hash_ = 0x7EBEu;
};

class ScatterOp : public Operator {
public:
    std::string typeName() const override { return op::kScatter; }
    std::string category() const override { return "grow"; }
    // The segments it grows its strands with (its branch's, EvalContext::strandSegments): a
    // Resample's edit makes only the Scatters it sets again.
    std::uint64_t contextSalt(const EvalContext& ctx) const override {
        return hashCombine(hash64(0x5E6), static_cast<std::uint64_t>(segmentsFor(ctx)));
    }
    static int segmentsFor(const EvalContext& ctx) { return std::clamp(ctx.strandSegments > 0 ? ctx.strandSegments : 8, 1, 256); }
    bool thinnedLastRun() const override { return thinned_.load() != 0; }
    // The same node in another copy of the graph (its cached result taken on): whether that thinned,
    // and its kept placements and mixes (each keyed on all it comes from: a graph parsed again for a
    // randomness edit mixes the relaxed roots again, it does not relax them again).
    void adoptKeptWork(const Operator& previous) override {
        const auto* other = dynamic_cast<const ScatterOp*>(&previous);
        if (!other || other == this) return;
        thinned_ = other->thinned_.load();
        std::vector<std::pair<std::uint64_t, std::shared_ptr<const Placed>>> memo;
        std::vector<Mixed> mixed;
        {
            const std::lock_guard<std::mutex> lock(other->memoMutex_);
            memo = other->memo_;
            mixed = other->mixed_;
        }
        const std::lock_guard<std::mutex> lock(memoMutex_);
        memo_ = std::move(memo);
        mixed_ = std::move(mixed);
    }
    // Guides for density from a guide layer (guide map), and for guideRadius.
    std::vector<PortSpec> inputs() const override { return {{"scalp", PortType::Mesh}, {"guides", PortType::Guides, true}}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            notAnimatable({"density", 80.0, "Strands per unit area (mappable: texture, color set, expression, guide layer)", "density"})
                .atLeast(0)
                .soft(0, 2000),
            notAnimatable({"seed", std::int64_t{0}, "Random seed"}).adv(),
            ParamSpec{"length", 1.0, "Length of the straight strands grown along the scalp's normal. An Interpolate after "
                                     "it gives the strands the guides' shapes and lengths, so with guides this shows only "
                                     "where no guide reaches; to make hair longer or shorter, brush the guides or trim "
                                     "with the Length node", "length"}
                .soft(0, 10),
            ParamSpec{"randomness", 1.0, "How random the roots are: 0 evenly spread (blue noise), 1 at random (white "
                                         "noise); in between each root moves smoothly from its even spot to its random "
                                         "one. The same strands at every value", "fraction"}
                .range(0, kMaxRandomness)
                .when("layout", {"random"}),
            notAnimatable({"layout", std::string("random"),
                           "random: roots at random (randomness 0 spreads them evenly); uvGrid: one per cell of a grid "
                           "over the scalp's UVs (rows x columns; density maps thin it)", "",
                           {"random", "uvGrid"}}),
            notAnimatable({"gridRows", std::int64_t{100}, "uvGrid: rows of the grid, along V"})
                .range(1, 20000)
                .soft(1, 1000)
                .when("layout", {"uvGrid"}),
            notAnimatable({"gridColumns", std::int64_t{100}, "uvGrid: columns of the grid, along U"})
                .range(1, 20000)
                .soft(1, 1000)
                .when("layout", {"uvGrid"}),
            notAnimatable({"gridRandom", 0.0, "uvGrid: each root moved at random up to this share of its cell", "fraction"})
                .range(0, 1)
                .when("layout", {"uvGrid"}),
            notAnimatable({"previewAll", false, "Every root in the viewport too, whatever the groom's preview density: for "
                                                "big pieces (instances, scales) that a thinned preview would leave bald. "
                                                "A scatter making 250 strands or fewer is never thinned anyway"})
                .adv(),
            notAnimatable({"guideRadius", 0.0, "Only within this distance of a guide's root (the guides input, every level of "
                                               "its groups: primaries and their children): fur along drawn guides, a "
                                               "patch, a stripe. 0, or nothing connected to guides: the whole scalp", "length"})
                .atLeast(0)
                .soft(0, 5)
                .adv(),
            notAnimatable({"tubeMargin", 0.0, "With guideRadius: guides whose tube has a shape keep the roots on their "
                                               "tube's root footprint (its outline laid on the scalp) instead of a disc, "
                                               "and within this share of the tube's half-width round it (as Interpolate's "
                                               "tubeBlend measures)", "fraction"})
                .atLeast(0)
                .soft(0, 1)
                .adv(),
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        auto out = std::make_shared<StrandSet>();
        const MeshPtr mesh = valueAs<MeshPtr>(inputs.at(0));
        if (!mesh || mesh->triangleCount() == 0) return StrandsPtr(out);
        // Roots are placed on the cage (the scalp itself unless it is refined from one) and
        // carried onto the scalp at the end.
        const CageBinding binding(mesh.get());
        const ScalpMesh& surface = *binding.cage();

        // A density map scatters at its maximum and keeps each root with
        // probability map / maximum, decided by the strand ID: stable IDs, and
        // painting more density only adds strands.
        GuidesPtr guides = inputs.size() > 1 ? valueAs<GuidesPtr>(inputs[1]) : nullptr;
        // One of several scalps (scalp_pieces.h): the guides' roots (on the whole mesh) read on it,
        // and each root placed on it lands on the whole mesh at the end (toWholeRoot).
        if (mesh->isPiece() && guides) {
            auto local = std::make_shared<GuideSet>(*guides);
            for (Guide& g : local->guides()) g.root = toPieceRoot(*mesh, g.root);
            if (local->levelTubes()) {
                auto tubes = std::make_shared<std::vector<Guide>>(*local->levelTubes());
                for (Guide& g : *tubes) g.root = toPieceRoot(*mesh, g.root);
                local->setLevelTubes(std::move(tubes));
            }
            guides = std::move(local);
        }
        const ScalarField densityField(params(), "density", mesh.get(), guides.get());
        double density = std::max(0.0f, densityField.maximum());
        if (!densityField.isConstant()) {
            // An expression (not clamped to 0-1) or a plugged field can go past the maximum it reports,
            // where value / maximum would clip at 1: its largest at the cage's corners and face middles
            // when past it, rounded up to a step of a log lattice (an edit of the scalp moving it a
            // little keeps it, and so the roots far from the edit).
            float sampled = 0.0f;
            constexpr float kAt[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f / 3.0f, 1.0f / 3.0f}};
            for (std::size_t f = 0; f < surface.triangleCount(); ++f) {
                for (const auto& uv : kAt) {
                    const float v = densityField.atCageRoot(StrandRoot{static_cast<std::int32_t>(f), uv[0], uv[1]});
                    if (std::isfinite(v)) sampled = std::max(sampled, v);
                }
            }
            if (sampled > density) density = roundToDecadeStep(sampled, 8, true);
        }
        const auto seed = static_cast<std::uint64_t>(params().getInt("seed"));
        const float length = static_cast<float>(params().getDouble("length"));
        // Strand resolution comes from the Resample node (EvalContext::strandSegments).
        const auto segments = static_cast<std::size_t>(segmentsFor(ctx));
        const std::string layout = params().getString("layout");
        const bool grid = layout == "uvGrid";
        const float randomness = std::clamp(static_cast<float>(params().getDouble("randomness")), 0.0f, kMaxRandomness);
        const auto rows = static_cast<std::size_t>(params().getInt("gridRows"));
        const auto columns = static_cast<std::size_t>(params().getInt("gridColumns"));
        if (grid && !surface.hasUVs()) throw GraphError("Scatter: the uvGrid layout needs the scalp's UVs");
        // How many strands the scatter makes at most: the grid's cells, else the area's.
        const double strandsMade = grid ? static_cast<double>(rows) * static_cast<double>(columns) : surface.restTotalArea() * density;
        // Around the guides only: roots farther than guideRadius from every guide root (on the rest
        // scalp) drop, like a density map that is 1 near the guides and 0 elsewhere.
        const float guideRadius = static_cast<float>(params().getDouble("guideRadius"));
        std::unique_ptr<PointIndex> guideRoots;
        std::unique_ptr<GuideFootprints> footprints;
        std::uint64_t guideRootsHash = 0;
        if (guideRadius > 0.0f) {
            if (!guides || guides->empty()) {
                throw GraphError("Scatter: guideRadius needs guides on the guides input (a Guides node)");
            }
            const float margin = static_cast<float>(std::max(params().getDouble("tubeMargin"), 0.0));
            std::vector<Vec3f> positions;
            positions.reserve(guides->size());
            auto shaped = std::make_unique<GuideFootprints>(margin);
            auto around = [&](const Guide& g) {
                if (g.points.empty()) return;
                const Vec3f root = binding.stablePosition(g.root, g.points[0]);
                if (g.hasTube && g.tube.edited() && g.points.size() >= 2) {
                    shaped->add(g, root, surface.contains(g.root) ? surface.normal(g.root) : Vec3f{});
                } else {
                    positions.push_back(root);
                }
            };
            for (const Guide& g : guides->guides()) around(g);
            // The family's other levels too (a group's primaries with its children: GuideSet::levelTubes).
            if (guides->levelTubes()) {
                for (const Guide& g : *guides->levelTubes()) around(g);
            }
            guideRootsHash = hashFloat(hash64(positions.size()), guideRadius);
            if (!shaped->empty()) guideRootsHash = hashCombine(guideRootsHash, shaped->hash());  // (none: as it always was)
            for (const Vec3f& p : positions) guideRootsHash = hashFloat(hashFloat(hashFloat(guideRootsHash, p.x), p.y), p.z);
            if (!positions.empty()) guideRoots = std::make_unique<PointIndex>(std::move(positions));
            if (!shaped->empty()) {
                shaped->index();
                footprints = std::move(shaped);
            }
        }
        // A root kept by guideRadius: near a guide's root, or on a shaped tube's footprint.
        auto nearGuides = [&](const Vec3f& at) {
            float d2 = 0.0f;
            if (guideRoots && guideRoots->nearest(at, &d2) != SIZE_MAX && d2 <= guideRadius * guideRadius) return true;
            return footprints && footprints->covers(at);
        };
        const bool aroundGuides = guideRoots || footprints;

        if (!std::isfinite(density) || !std::isfinite(surface.restTotalArea())) {
            throw GraphError("Scatter: the density (or the scalp's area) is not a finite number");
        }
        if (strandsMade > kMaxStrands) {
            throw GraphError(grid ? "Scatter: the grid would create more than 50M strands"
                                  : "Scatter: density would create more than 50M strands");
        }
        if (strandsMade > kHeavyStrands) {
            char message[256];
            std::snprintf(message, sizeof message,
                          "%.1fM strands: heavy (a human head has about 0.1-0.15M); renders, exports and a full "
                          "preview hold every one. Lower the %s, or thin it with a density map",
                          strandsMade / 1e6, grid ? "grid's rows and columns" : "density");
            ctx.note(message);
        }
        // Memory goes with points (strands x points each), and every node downstream keeps a copy
        // while previewing: a preview past this stops with a message rather than exhausting memory.
        // The viewport's point budget: thin the preview further to fit (the same strands, fewer of them).
        const double fullPoints = strandsMade * static_cast<double>(segments + 1);
        // A small scatter (kNeverThinned strands or fewer: feather shafts, scales) is never thinned
        // either: a few of them thinned leave a part bald, and showing them all costs next to nothing.
        const bool previewAll = params().getBool("previewAll") || !(strandsMade > kNeverThinned);
        float keepFraction = previewAll ? std::max(ctx.previewDensity, 1.0f) : ctx.previewDensity;
        const bool thinned = ctx.maxPoints > 0.0 && fullPoints * keepFraction > ctx.maxPoints;
        if (thinned) keepFraction = static_cast<float>(ctx.maxPoints / fullPoints);
        thinned_ = thinned ? 1 : 0;
        // The preview core (preview_core.h): the strands every preview holds, shown or not, so the
        // nodes reading other strands read the render's. All of them with previewAll (never thinned).
        const float core = previewAll ? 1.0f : coreShare(strandsMade);
        const float emitted = keepFraction >= 1.0f ? 1.0f : std::max(keepFraction, core);
        const double previewPoints = fullPoints * emitted;
        if (ctx.maxPoints <= 0.0 && ctx.previewDensity < 1.0f && previewPoints > kMaxPreviewPoints) {
            char message[256];
            std::snprintf(message, sizeof message,
                          "Scatter: the preview would have %.0fM points (%.0fk strands x %zu): lower the preview "
                          "density, density or the Resample's segments",
                          previewPoints / 1e6, strandsMade * ctx.previewDensity / 1e3, segments + 1);
            throw GraphError(message);
        }
        if (previewPoints > kMaxPoints) {
            char message[256];
            std::snprintf(message, sizeof message,
                          "Scatter: %.0fM points (strands x %zu) is more than memory holds: lower the density or "
                          "the Resample's segments", previewPoints / 1e6, segments + 1);
            throw GraphError(message);
        }

        // Where the strands grow, as IDs and roots (face and barycentrics): from the rest pose (the
        // scalp itself without one), never the current points. Relaxed roots are kept, keyed on all
        // they come from, so a deforming scalp moves them along without relaxing them again.
        // uvGrid: one root per cell, on the first face whose UVs hold the cell's spot.
        auto placeGrid = [&]() {
            const std::size_t cells = rows * columns;
            std::vector<std::int32_t> owner(cells, -1);
            std::vector<std::array<float, 2>> at(cells);
            const auto gridRandom = static_cast<float>(params().getDouble("gridRandom"));
            const std::uint64_t gridHash = hash64(seed ^ 0x6121D5EEDull);
            auto cellId = [&](std::size_t cell) { return hashCombine(gridHash, cell); };
            parallelFor(0, cells, [&](std::size_t cell) {
                const std::size_t r = cell / columns, c = cell % columns;
                float u = (static_cast<float>(c) + 0.5f) / static_cast<float>(columns);
                float v = (static_cast<float>(r) + 0.5f) / static_cast<float>(rows);
                if (gridRandom > 0.0f) {
                    const std::uint64_t id = cellId(cell);
                    u += gridRandom * (unitFloat(hashCombine(id, 1)) - 0.5f) / static_cast<float>(columns);
                    v += gridRandom * (unitFloat(hashCombine(id, 2)) - 0.5f) / static_cast<float>(rows);
                }
                at[cell] = {u, v};
            });
            const std::vector<ScalpMesh::UV>& uvs = surface.cornerUVs();
            std::vector<StrandRoot> rootOf(cells);
            for (std::size_t f = 0; f < surface.triangleCount() && 3 * f + 2 < uvs.size(); ++f) {
                const ScalpMesh::UV& a = uvs[3 * f];
                const ScalpMesh::UV& b = uvs[3 * f + 1];
                const ScalpMesh::UV& c = uvs[3 * f + 2];
                const float e0u = b[0] - a[0], e0v = b[1] - a[1], e1u = c[0] - a[0], e1v = c[1] - a[1];
                const float det = e0u * e1v - e1u * e0v;
                if (!(std::fabs(det) > 1e-14f)) continue;  // no area in UV space
                const float lo[2] = {std::min({a[0], b[0], c[0]}), std::min({a[1], b[1], c[1]})};
                const float hi[2] = {std::max({a[0], b[0], c[0]}), std::max({a[1], b[1], c[1]})};
                // The cells whose spots may fall inside (a cell of slack each way for the jitter).
                const auto cellOf = [](float x, std::size_t n) {
                    return static_cast<std::int64_t>(std::floor(x * static_cast<float>(n)));
                };
                const std::int64_t c0 = std::max<std::int64_t>(0, cellOf(lo[0], columns) - 1);
                const std::int64_t c1 = std::min<std::int64_t>(static_cast<std::int64_t>(columns) - 1, cellOf(hi[0], columns) + 1);
                const std::int64_t r0 = std::max<std::int64_t>(0, cellOf(lo[1], rows) - 1);
                const std::int64_t r1 = std::min<std::int64_t>(static_cast<std::int64_t>(rows) - 1, cellOf(hi[1], rows) + 1);
                for (std::int64_t r = r0; r <= r1; ++r) {
                    for (std::int64_t col = c0; col <= c1; ++col) {
                        const std::size_t cell = static_cast<std::size_t>(r) * columns + static_cast<std::size_t>(col);
                        if (owner[cell] >= 0) continue;  // an earlier face has it
                        const float pu = at[cell][0] - a[0], pv = at[cell][1] - a[1];
                        const float bu = (pu * e1v - e1u * pv) / det;  // corner b's weight
                        const float bv = (e0u * pv - pu * e0v) / det;  // corner c's
                        constexpr float kEdge = 1e-6f;
                        if (bu < -kEdge || bv < -kEdge || bu + bv > 1.0f + kEdge) continue;
                        owner[cell] = static_cast<std::int32_t>(f);
                        const float u = std::clamp(bu, 0.0f, 1.0f);
                        rootOf[cell] = {static_cast<std::int32_t>(f), u, std::clamp(bv, 0.0f, 1.0f - u)};
                    }
                }
            }
            std::vector<std::size_t> taken;
            for (std::size_t cell = 0; cell < cells; ++cell) {
                if (owner[cell] >= 0 && keepAtDensity(cellId(cell), emitted)) taken.push_back(cell);
            }
            std::vector<char> keep(taken.size(), 1);
            parallelFor(0, taken.size(), [&](std::size_t i) {
                const StrandRoot& root = rootOf[taken[i]];
                const std::uint64_t id = cellId(taken[i]);
                if (!densityField.isConstant() && density > 0.0) {
                    const float p = densityField.atCageRoot(root) / static_cast<float>(density);
                    keep[i] = unitFloat(hashCombine(id, 0xD3A5)) < p ? 1 : 0;
                }
                if (keep[i] && aroundGuides && !nearGuides(surface.restPositionOr(root, surface.position(root)))) keep[i] = 0;
            });
            auto placed = std::make_shared<Placed>();
            for (std::size_t i = 0; i < taken.size(); ++i) {
                if (!keep[i]) continue;
                placed->ids.push_back(cellId(taken[i]));
                placed->random.push_back(rootOf[taken[i]]);
            }
            return std::shared_ptr<const Placed>(std::move(placed));
        };
        // The even spots are worked out unless only the random ones show (randomness 1): every root
        // relaxed, whatever the preview holds of them, so its strands sit where the render's do.
        const bool needEven = !grid && randomness != 1.0f;
        // On the rest pose: the same spacing every frame.
        std::unique_ptr<ScalpMesh> restMesh;
        auto restOf = [&]() -> const ScalpMesh& {
            if (!surface.hasRest()) return surface;
            if (!restMesh) restMesh = std::make_unique<ScalpMesh>(surface.restPoints(), surface.triangles());
            return *restMesh;
        };
        auto place = [&]() {
            if (grid) return placeGrid();
            struct Sample {
                std::uint64_t id;
                std::int32_t face;
            };
            std::vector<Sample> samples;
            samples.reserve(static_cast<std::size_t>(surface.restTotalArea() * density * (needEven ? 1.0f : emitted) * 1.1) + 16);

            const std::uint64_t seedHash = hash64(seed);
            for (std::size_t f = 0; f < surface.triangleCount(); ++f) {
                const std::uint64_t faceHash = hashCombine(seedHash, f);
                const double expected = surface.restTriangleArea(f) * density;  // rest: same strands every frame
                if (!(expected > 0.0)) continue;  // none (or not a number)
                auto count = static_cast<std::uint64_t>(std::min(expected, kMaxStrands));
                if (unitFloat(hash64(faceHash)) < static_cast<float>(expected - static_cast<double>(count))) ++count;

                for (std::uint64_t k = 0; k < count; ++k) {
                    const std::uint64_t id = hashCombine(faceHash, k);
                    // Relaxed, every root (the preview's are taken after); else those it holds.
                    if (needEven || keepAtDensity(id, emitted)) samples.push_back({id, static_cast<std::int32_t>(f)});
                }
            }

            std::vector<StrandRoot> roots(samples.size());
            std::vector<char> keep(samples.size(), 1);
            parallelFor(0, samples.size(), [&](std::size_t i) {
                const Sample& s = samples[i];
                Rng rng(s.id);
                const float su = std::sqrt(rng.nextFloat());
                const float r2 = rng.nextFloat();
                roots[i] = {s.face, su * (1.0f - r2), su * r2};
                if (!densityField.isConstant() && density > 0.0) {
                    const float p = densityField.atCageRoot(roots[i]) / static_cast<float>(density);
                    keep[i] = unitFloat(hashCombine(s.id, 0xD3A5)) < p ? 1 : 0;
                }
                if (keep[i] && aroundGuides && !nearGuides(surface.restPositionOr(roots[i], surface.position(roots[i])))) {
                    keep[i] = 0;
                }
            });
            std::size_t kept = 0;
            for (std::size_t i = 0; i < samples.size(); ++i) {
                if (!keep[i]) continue;
                samples[kept] = samples[i];
                roots[kept] = roots[i];
                ++kept;
            }
            samples.resize(kept);
            roots.resize(kept);

            auto placed = std::make_shared<Placed>();
            if (needEven && !roots.empty()) {
                constexpr float kHexSpacing = 1.0746f;  // sqrt(2 / sqrt(3)): hexagonal packing at 1 per unit area
                const float fullSpacing = kHexSpacing / std::sqrt(static_cast<float>(density));
                const auto spacingAt = [&](const StrandRoot& root) {
                    if (densityField.isConstant()) return fullSpacing;
                    const float local = densityField.atCageRoot(root);
                    return local > 0.0f ? kHexSpacing / std::sqrt(local) : 0.0f;
                };
                // Where an even spot may be: where its random one could be kept.
                const auto allowed = [&](const StrandRoot& root) {
                    if (!densityField.isConstant() && !(densityField.atCageRoot(root) > 0.0f)) return false;
                    return !aroundGuides || nearGuides(surface.restPositionOr(root, surface.position(root)));
                };
                placed->even = roots;
                relaxRoots(placed->even, restOf(), fullSpacing, [&](std::size_t i) { return spacingAt(roots[i]); }, allowed);
                // A root that still ended outside (its first step over the edge, between two
                // checks) takes its random spot, which the density kept.
                parallelFor(0, roots.size(), [&](std::size_t i) {
                    if (!allowed(placed->even[i])) placed->even[i] = roots[i];
                });
                ++relaxCount;
            }
            placed->ids.reserve(samples.size());
            for (const Sample& sample : samples) placed->ids.push_back(sample.id);
            placed->random = std::move(roots);
            return std::shared_ptr<const Placed>(std::move(placed));
        };
        // Kept: placements with even spots (every root relaxed), keyed on all they come from but the
        // randomness and the preview, and the last mixes of what a preview holds of them, keyed on
        // that, the randomness and the preview's share.
        std::uint64_t memoKey = 0;
        const bool memoize = needEven && placementKey(binding, guides.get(), seed, density, guideRootsHash, &memoKey);
        std::shared_ptr<const Placed> placed = memoize ? findPlaced(memoKey) : nullptr;
        if (!placed) {
            placed = place();
            if (memoize) keepPlaced(memoKey, placed);
        }
        // What the preview holds of them, by ID, and each one's role in it (preview_core.h).
        std::vector<std::size_t> held;
        std::vector<float> roles;
        held.reserve(placed->ids.size());
        roles.reserve(placed->ids.size());
        bool allCoreShown = true;
        for (std::size_t i = 0; i < placed->ids.size(); ++i) {
            float role = kCoreShown;
            if (!previewRole(placed->ids[i], keepFraction >= 1.0f ? 2.0f : keepFraction, core, &role)) continue;
            held.push_back(i);
            roles.push_back(role);
            allCoreShown = allCoreShown && role == kCoreShown;
        }
        const bool all = held.size() == placed->ids.size();
        auto heldOf = [&](const std::vector<StrandRoot>& every) {
            std::vector<StrandRoot> out(held.size());
            for (std::size_t k = 0; k < held.size(); ++k) out[k] = every[held[k]];
            return out;
        };
        // The placement's own arrays when the preview holds every root (the render): no copy.
        auto rootsOf = [&](const std::vector<StrandRoot>& every) {
            return all ? std::shared_ptr<const std::vector<StrandRoot>>(placed, &every)
                       : std::make_shared<const std::vector<StrandRoot>>(heldOf(every));
        };
        std::vector<StrandId> ids(held.size());
        for (std::size_t k = 0; k < held.size(); ++k) ids[k] = placed->ids[held[k]];
        std::shared_ptr<const std::vector<StrandRoot>> rootsHeld;
        if (grid || randomness == 1.0f) {
            rootsHeld = rootsOf(placed->random);
        } else if (randomness == 0.0f) {
            rootsHeld = rootsOf(placed->even);
        } else {
            // Each root's mix is its own: those the preview holds alone are mixed.
            const std::uint64_t mixKey = hashFloat(hashFloat(memoKey, randomness), keepFraction);
            rootsHeld = memoize ? findMixed(mixKey, placed) : nullptr;
            if (!rootsHeld) {
                rootsHeld = std::make_shared<const std::vector<StrandRoot>>(
                    all ? mixRoots(placed->even, placed->random, restOf(), randomness)
                        : mixRoots(heldOf(placed->even), heldOf(placed->random), restOf(), randomness));
                if (memoize) keepMixed(mixKey, placed, rootsHeld);
            }
        }
        const std::vector<StrandRoot>& roots = *rootsHeld;

        const std::size_t pointsPerStrand = segments + 1;
        // Every point is written below (the widths stay 1): the positions come as they are.
        out->resizeUniform(ids.size(), pointsPerStrand, StrandSet::kLeavePositions);
        std::vector<StrandId>& outIds = out->mutableIds();
        std::vector<StrandRoot>& outRoots = out->mutableRoots();
        if (!allCoreShown) out->addStrandChannel(kPreviewChannel, kCoreShown) = std::move(roles);

        parallelFor(0, ids.size(), [&](std::size_t i) {
            const StrandRoot root = binding.toScalp(roots[i]);  // on the cage until here

            const Vec3f base = mesh->position(root);
            const Vec3f dir = mesh->normal(root) * length;

            outIds[i] = ids[i];
            outRoots[i] = toWholeRoot(*mesh, root);
            Vec3f* pts = out->strandPoints(i);
            for (std::size_t k = 0; k < pointsPerStrand; ++k) {
                pts[k] = base + dir * (static_cast<float>(k) / static_cast<float>(segments));
            }
        });

        return StrandsPtr(out);
    }

private:
    // Strand IDs and each one's random and even spots (empty: not worked out), as placed by an
    // evaluation. uvGrid: the grid's roots as the random ones.
    struct Placed {
        std::vector<StrandId> ids;
        std::vector<StrandRoot> random, even;
    };

    // Everything the placement of relaxed roots depends on (not the randomness: only their mix
    // does), the scalp's current pose aside: its rest surface, the density and what its map reads,
    // the seed, and the guides' roots with a guideRadius; not the preview (every root is relaxed).
    // False when the density is a plugged field or an expression (they may read anything, the pose
    // too): those are not kept.
    bool placementKey(const CageBinding& binding, const GuideSet* guides, std::uint64_t seed,
                      double density, std::uint64_t guideRootsHash, std::uint64_t* key) const {
        if (params().field("density")) return false;
        const ScalpMesh& mesh = *binding.cage();  // where the roots are placed
        std::uint64_t k = hashCombine(hash64(0x5CA77E2u), mesh.restSurfaceHash());
        if (params().has("density")) {
            const ParamValue& value = params().get("density");
            if (const MapRef* map = std::get_if<MapRef>(&value)) {
                if (map->source == MapRef::Source::Expression) return false;
                k = hashString(hashCombine(k, static_cast<std::uint64_t>(map->source) + 1), map->name);
                k = hashFloat(hashFloat(hashCombine(k, static_cast<std::uint64_t>(map->component)), map->min), map->max);
                if (map->source == MapRef::Source::Texture) k = hashCombine(k, fileStamp(resolvePath(map->name)));
                if (map->source == MapRef::Source::ColorSet) k = hashCombine(k, mesh.colorSetHash(map->name));
                if (map->source == MapRef::Source::Guide || map->source == MapRef::Source::Attribute) {
                    // The guides' layer, blended from their roots on the rest pose.
                    k = hashCombine(k, guides ? guides->size() + 1 : 0);
                    if (guides) {
                        for (const Guide& g : guides->guides()) {
                            if (g.points.empty()) continue;
                            const Vec3f p = binding.stablePosition(g.root, g.points.front());
                            k = hashFloat(hashFloat(hashFloat(k, p.x), p.y), p.z);
                            const auto it = g.attributes.find(map->name);
                            k = it == g.attributes.end() ? hashCombine(k, 0xAB5u) : hashFloat(k, it->second);
                        }
                    }
                }
            } else {
                k = hashDouble(k, params().getDouble("density"));
            }
        }
        k = hashDouble(hashCombine(k, seed), density);
        *key = hashCombine(k, guideRootsHash);
        return true;
    }
    std::shared_ptr<const Placed> findPlaced(std::uint64_t key) const {
        const std::lock_guard<std::mutex> lock(memoMutex_);
        for (const auto& [k, placed] : memo_) {
            if (k == key) return placed;
        }
        return nullptr;
    }
    void keepPlaced(std::uint64_t key, std::shared_ptr<const Placed> placed) const {
        const std::lock_guard<std::mutex> lock(memoMutex_);
        memo_.insert(memo_.begin(), {key, std::move(placed)});
        if (memo_.size() > kPlacedMemo) memo_.resize(kPlacedMemo);
    }

    // A mix kept for `key` (the placement's and the randomness), made from `placed`.
    std::shared_ptr<const std::vector<StrandRoot>> findMixed(std::uint64_t key, const std::shared_ptr<const Placed>& placed) const {
        const std::lock_guard<std::mutex> lock(memoMutex_);
        for (const Mixed& m : mixed_) {
            if (m.key == key && m.placed == placed) return m.roots;
        }
        return nullptr;
    }
    void keepMixed(std::uint64_t key, std::shared_ptr<const Placed> placed, std::shared_ptr<const std::vector<StrandRoot>> roots) const {
        const std::lock_guard<std::mutex> lock(memoMutex_);
        mixed_.insert(mixed_.begin(), {key, std::move(placed), std::move(roots)});
        if (mixed_.size() > kPlacedMemo) mixed_.resize(kPlacedMemo);
    }

    struct Mixed {
        std::uint64_t key = 0;
        std::shared_ptr<const Placed> placed;
        std::shared_ptr<const std::vector<StrandRoot>> roots;
    };
    mutable std::mutex memoMutex_;
    mutable std::vector<std::pair<std::uint64_t, std::shared_ptr<const Placed>>> memo_;  // newest first
    mutable std::vector<Mixed> mixed_;                                                   // newest first
    // thinnedLastRun: 1 thinned, 0 not, -1 not known (never run here nor taken on: as thinned).
    mutable std::atomic<int> thinned_{-1};
};

}  // namespace

std::size_t scatterRelaxCount() { return relaxCount.load(); }
std::size_t scatterRelaxIterationCount() { return relaxIterationCount.load(); }

void registerScatterOperator(OperatorRegistry& registry) {
    registry.registerType(op::kScatter,
                          {"grow", "Roots over the scalp: how many hairs, and where.", "density count roots number hairs"},
                          [] { return std::make_unique<ScatterOp>(); });
}

}  // namespace groomlab
