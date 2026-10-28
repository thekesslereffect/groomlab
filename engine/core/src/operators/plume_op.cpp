// SPDX-License-Identifier: Apache-2.0
// Plume (Yeti's Plume node): feathers laid over one another instead of through one another. What
// a Propagate node grows is a set of feathers (its strands grouped by featherId: each shaft and
// its barbs), each grown on its own, so neighbouring vanes cut through each other and drooping
// barbs dip into the body. Plume stacks them: the feathers are put in an order (rankBy), and each
// feather is turned up about its root, off the body, until it lies `offset` above every lower-
// ranked feather under it and `bodyOffset` above the body (the scalp, and the collider input).
//
// How:
// - A feather is its strands (shaft and barbs) about its root: the scalp point under its shaft,
//   the scalp's normal there its "up" (the vane's normal without a scalp). It turns about the
//   axis across it in the skin's plane (its side), raising its tip: its root stays on the skin.
//   Points within kRootZone of its length from the root are never lifted for (they could only
//   move by leaving the skin); a turn of up to maxAngle resolves the rest.
// - stiffness 1 turns the feather as a rigid whole; 0 bends it, each part along it turned only
//   as much as the contacts at or before it need (rising smoothly from the root to each contact);
//   between, a mix. The turn of every point is a rotation about the root (lengths from the root
//   kept).
// - The feather under another is its vane as a height map in its own frame (the top and bottom
//   of its strands over a grid across the feather): a point of a higher feather over it (in the
//   map, within `depth` under its bottom) lies at least `offset` above its top. Feathers facing
//   another way (their ups more than ~70 degrees apart: the other side of a wing) never stack.
// - Order: each feather depends on the lower-ranked feathers it can meet: wherever either turns
//   (0 to maxAngle), their boxes (each feather's points swept through its turn, in its own frame)
//   overlap. rankBy rootDistance without a direction ranks each two such feathers against their
//   own mean direction (a ring of feathers so ranked, where their ways turn, against the ring's
//   mean direction): what lies over what is decided among neighbours alone, and an edit of the
//   feathers in one place leaves the order elsewhere as it was. A feather is pushed out of the
//   body from as deep as half its own length. A feather is solved once every feather it depends
//   on is final, so the feathers are
//   solved in waves (every feather of a wave in parallel, each reading only the finished feathers
//   under it). The result does not depend on the threads. (About 0.4 s for 3000 feathers of 180
//   points each on 16 threads: Budget.PlumeBird.)
// - `iterations` linearised passes per feather: each finds what every point still needs and turns
//   the feather by that much more.
//
// Every feather's turn is final before the feathers over it are solved: a single sweep, no
// global relaxation. The strands of a feather keep their IDs, roots and point counts; their
// vane normals (vaneNormalX/Y/Z) turn with them, and a rachis (Propagate's shaftWidth) turns
// with its feather.

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <unordered_map>

#include "groomlab/color_patterns.h"
#include "groomlab/distance_field.h"
#include "groomlab/feathers.h"
#include "groomlab/field.h"
#include "groomlab/frames.h"
#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/portable_math.h"
#include "groomlab/random.h"
#include "internal.h"

namespace groomlab {

namespace {

constexpr float kDegrees = 0.017453293f;
constexpr float kRootZone = 0.1f;    // points nearer the root than this share of the feather's reach are not lifted for
constexpr float kSameSide = 0.35f;   // feathers whose ups' dot is below this never stack (about 70 degrees)
constexpr float kMaxStep = 0.35f;    // radians a feather turns at most per pass (the linearisation's range)
constexpr std::size_t kTable = 17;   // the turn along a feather, sampled 0 root .. 1 tip
constexpr int kMapAlong = 32;        // a height map's cells along its feather (across: by its aspect)

const char* kFeatherId = "featherId";
const char* kIsShaft = "isShaft";
const char* kShaftU = "shaftU";

// The smooth ramp a bend rises by from the root to a contact at 1 (and stays at past it).
float ramp(float x) { return smoothstep(0.0f, 1.0f, x); }

// The strongly connected parts of a graph (node b's edges: each of edges[b]): each node's part
// number (*count of them). Tarjan's, without recursion.
std::vector<int> stronglyConnected(const std::vector<std::vector<std::uint32_t>>& edges, int* count) {
    const std::size_t n = edges.size();
    std::vector<int> index(n, -1), low(n, 0), part(n, -1);
    std::vector<char> stacked(n, 0);
    std::vector<std::uint32_t> stack;
    std::vector<std::pair<std::uint32_t, std::size_t>> calls;  // node, its next edge
    int counter = 0, parts = 0;
    auto visit = [&](std::uint32_t v) {
        index[v] = low[v] = counter++;
        stack.push_back(v);
        stacked[v] = 1;
        calls.emplace_back(v, 0);
    };
    for (std::size_t s = 0; s < n; ++s) {
        if (index[s] >= 0) continue;
        visit(static_cast<std::uint32_t>(s));
        while (!calls.empty()) {
            const std::uint32_t v = calls.back().first;
            const std::size_t e = calls.back().second;
            if (e < edges[v].size()) {
                ++calls.back().second;
                const std::uint32_t w = edges[v][e];
                if (index[w] < 0) {
                    visit(w);
                } else if (stacked[w]) {
                    low[v] = std::min(low[v], index[w]);
                }
                continue;
            }
            calls.pop_back();
            if (!calls.empty()) low[calls.back().first] = std::min(low[calls.back().first], low[v]);
            if (low[v] != index[v]) continue;
            std::uint32_t w = 0;
            do {
                w = stack.back();
                stack.pop_back();
                stacked[w] = 0;
                part[w] = parts;
            } while (w != v);
            ++parts;
        }
    }
    *count = parts;
    return part;
}

// A lower feather as the feathers over it read it: its strands' top and bottom over a grid in
// its frame (origin its root; x along it, y across, z its up turned with it).
struct HeightMap {
    Vec3f origin, x, y, z;
    float x0 = 0.0f, y0 = 0.0f, cellX = 1.0f, cellY = 1.0f;
    float depth = 0.0f;  // how far under its bottom a point is still lifted over it
    int nx = 0, ny = 0;
    std::vector<float> top, bottom;  // per cell; empty cells have top < bottom
    Vec3f boxMin, boxMax;            // its points' bounds, grown by what can reach them
    bool valid() const { return nx > 0 && ny > 0; }

    // The top and bottom under `p` (its height in the map's z: *h); false where the map has none.
    bool at(const Vec3f& p, float* h, float* t, float* b) const {
        if (p.x < boxMin.x || p.y < boxMin.y || p.z < boxMin.z || p.x > boxMax.x || p.y > boxMax.y || p.z > boxMax.z) {
            return false;
        }
        const Vec3f d = p - origin;
        const float fx = (d.dot(x) - x0) / cellX, fy = (d.dot(y) - y0) / cellY;
        if (!(fx >= 0.0f) || !(fy >= 0.0f)) return false;
        const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
        if (ix >= nx || iy >= ny) return false;
        const std::size_t c = static_cast<std::size_t>(iy) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(ix);
        if (top[c] < bottom[c]) return false;
        *h = d.dot(z);
        *t = top[c];
        *b = bottom[c];
        return true;
    }
};

// One feather: its strands, its frame about the root, its rank and its solved turn.
struct Feather {
    int id = 0;
    std::vector<std::uint32_t> strands;
    std::size_t rep = 0;  // the strand that speaks for it: its shaft when kept, else its first
    Vec3f root, up, along, side;  // side = along x up: the axis it turns about (+: its tip up)
    float reach = 0.0f;       // the farthest point from the root
    float alongLength = 0.0f; // the farthest along `along`
    float rank = 0.0f;
    float strength = 1.0f;
    // In the preview core (preview_core.h: its strand speaking for it is): only core feathers lie
    // under others or size the body's fields, so every feather is laid the same at every preview
    // density.
    bool core = true;
    std::array<float, kTable> turn{};  // radians at u = k / (kTable - 1)
    bool moved = false;
    // Where it can be, turned anywhere from 0 to maxAngle: a box in its frame (along, side, up).
    Vec3f boxCenter, boxHalf;

    Vec3f axisOf(int k) const { return k == 0 ? along : k == 1 ? side : up; }

    float uOf(const Vec3f& p) const {
        return alongLength > 0.0f ? std::clamp((p - root).dot(along) / alongLength, 0.0f, 1.0f) : 0.0f;
    }
    float turnAt(float u) const {
        const float f = std::clamp(u, 0.0f, 1.0f) * static_cast<float>(kTable - 1);
        const auto i = std::min(static_cast<std::size_t>(f), kTable - 2);
        const float t = f - static_cast<float>(i);
        return turn[i] + (turn[i + 1] - turn[i]) * t;
    }
    Vec3f turned(const Vec3f& p, float angle) const { return root + rotateAxisAngle(p - root, side, angle); }
};

class PlumeOp : public Operator {
public:
    std::string typeName() const override { return op::kPlume; }
    std::string category() const override { return "feathers"; }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::vector<PortSpec> inputs() const override {
        return {{"strands", PortType::Strands}, {"collider", PortType::Mesh, true}};
    }
    PortSpec output() const override { return {"out", PortType::Strands}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"offset", 0.01, "Gap kept between a feather and each lower-ranked feather under it", "length"}
                .atLeast(0)
                .soft(0, 0.1),
            ParamSpec{"bodyOffset", 0.005, "Gap kept between the feathers and the body (the scalp, and the collider input)",
                      "length"}
                .atLeast(0)
                .soft(0, 0.1),
            {"rankBy", std::string("rootDistance"),
             "Which feathers lie on top: rootDistance (roots farther along `direction`), length (shorter feathers over longer "
             "ones: coverts over flight feathers), channel (a higher rankChannel value) or random",
             "", {"rootDistance", "length", "channel", "random"}},
            ParamSpec{"direction", Vec3f{0.0f, 0.0f, 0.0f},
                      "rootDistance: feathers rooted farther along this lie on top. (0, 0, 0): each two neighbouring "
                      "feathers against their mean direction, so each feather's tip lies over the base of the one "
                      "behind it (roof tiles), wherever the feathers point"}
                .world()
                .when("rankBy", {"rootDistance"}),
            ParamSpec{"rankChannel", std::string(""), "channel: the strand channel ranking the feathers (its shaft's value)"}
                .when("rankBy", {"channel"})
                .readsChannel(),
            notAnimatable({"invert", false, "Turn the order over: the feathers that would lie on top lie underneath"}),
            notAnimatable({"iterations", std::int64_t{4}, "Passes per feather: each turns it by what its points still need"})
                .range(0, 32)
                .soft(1, 8)
                .adv(),
            ParamSpec{"stiffness", 1.0, "1: each feather turns as a rigid whole about its root; 0: it bends, each part only "
                                        "as much as the contacts up to it need", "fraction"}
                .range(0, 1),
            ParamSpec{"maxAngle", 60.0, "The most a feather turns up off the body, degrees", "angle"}.range(0, 180).soft(0, 90),
            ParamSpec{"depth", 0.0, "How far under a lower feather a point is still lifted over it (scene units; 0: a "
                                    "quarter of that feather's length)", "length"}
                .atLeast(0)
                .soft(0, 1)
                .adv(),
            ParamSpec{"strength", 1.0, "How much of its turn each feather takes, 0-1 (mappable, per feather: its shaft's "
                                       "value)", "fraction"}
                .range(0, 1),
            notAnimatable({"seed", std::int64_t{0}, "rankBy random: the seed"}).when("rankBy", {"random"}).adv(),
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, 0, op::kPlume);
        const std::vector<float>* featherIds = in->strandChannel(kFeatherId);
        const auto iterations = static_cast<int>(params().getInt("iterations"));
        if (!featherIds || featherIds->size() != in->strandCount() || iterations <= 0) {
            if (!featherIds) ctx.note("Plume: no feathers (a featherId channel: Propagate's output) to stack");
            return in;
        }
        const MeshPtr scalpPtr = valueAs<MeshPtr>(ctx.external("scalp"));
        const ScalpMesh* scalp = scalpPtr && scalpPtr->triangleCount() > 0 ? scalpPtr.get() : nullptr;
        const ScalarField strengthField(params(), "strength", scalp);
        strengthField.validate(*in);
        const float offset = std::max(0.0f, static_cast<float>(params().getDouble("offset")));
        const float bodyOffset = std::max(0.0f, static_cast<float>(params().getDouble("bodyOffset")));
        const std::string rankBy = params().getString("rankBy");
        if (rankBy != "rootDistance" && rankBy != "length" && rankBy != "channel" && rankBy != "random") {
            throw GraphError("Plume: rankBy must be rootDistance, length, channel or random");
        }
        const float stiffness = std::clamp(static_cast<float>(params().getDouble("stiffness")), 0.0f, 1.0f);
        const float maxAngle = std::max(0.0f, static_cast<float>(params().getDouble("maxAngle")) * kDegrees);
        const float depthParam = std::max(0.0f, static_cast<float>(params().getDouble("depth")));
        const bool invert = params().getBool("invert");
        const auto seed = static_cast<std::uint64_t>(params().getInt("seed"));
        const std::vector<float>* rankChannel = nullptr;
        if (rankBy == "channel") {
            const std::string name = params().getString("rankChannel");
            rankChannel = in->strandChannel(name);
            if (!rankChannel || rankChannel->size() != in->strandCount()) {
                throw GraphError("Plume: no strand channel '" + name + "' to rank the feathers by");
            }
        }

        // The feathers: strands grouped by featherId (in ID order; -1: none, passed through).
        std::vector<std::pair<int, std::uint32_t>> members;
        members.reserve(in->strandCount());
        for (std::size_t s = 0; s < in->strandCount(); ++s) {
            const float f = (*featherIds)[s];
            if (f >= 0.0f && std::isfinite(f) && in->strandPointCount(s) > 0) {
                members.emplace_back(static_cast<int>(f), static_cast<std::uint32_t>(s));
            }
        }
        std::sort(members.begin(), members.end());
        std::vector<Feather> feathers;
        for (std::size_t i = 0; i < members.size();) {
            Feather f;
            f.id = members[i].first;
            while (i < members.size() && members[i].first == f.id) f.strands.push_back(members[i++].second);
            feathers.push_back(std::move(f));
        }
        if (feathers.empty()) return in;
        const std::vector<float>* isShaft = in->strandChannel(kIsShaft);
        const std::vector<float>* shaftU = in->strandChannel(kShaftU);
        std::array<const std::vector<float>*, 3> vaneNormals{};
        bool hasVaneNormals = true;
        for (int k = 0; k < 3; ++k) {
            vaneNormals[static_cast<std::size_t>(k)] = in->strandChannel(kVaneNormalChannels[k]);
            hasVaneNormals = hasVaneNormals && vaneNormals[static_cast<std::size_t>(k)] &&
                             vaneNormals[static_cast<std::size_t>(k)]->size() == in->strandCount();
        }

        // Each feather's frame: its root (the shaft's base), up, the way it points and its reach.
        parallelFor(0, feathers.size(), [&](std::size_t fi) {
            Feather& f = feathers[fi];
            f.rep = f.strands[0];
            bool shaft = false;
            if (isShaft) {
                for (const std::uint32_t s : f.strands) {
                    if ((*isShaft)[s] > 0.5f) {
                        f.rep = s;
                        shaft = true;
                        break;
                    }
                }
            }
            const StrandRoot& root = in->roots()[f.rep];
            const bool onScalp = scalp && scalp->contains(root);
            if (shaft) {
                f.root = in->strandPoints(f.rep)[0];
            } else if (onScalp) {
                f.root = scalp->position(root);
            } else {  // barbs alone: the one nearest the shaft's root starts there
                std::uint32_t first = f.strands[0];
                for (const std::uint32_t s : f.strands) {
                    if (shaftU && (*shaftU)[s] < (*shaftU)[first]) first = s;
                }
                f.root = in->strandPoints(first)[0];
            }
            Vec3f up{0.0f, 1.0f, 0.0f};
            if (onScalp) {
                up = scalp->normal(root);
            } else if (hasVaneNormals) {
                const Vec3f v{(*vaneNormals[0])[f.rep], (*vaneNormals[1])[f.rep], (*vaneNormals[2])[f.rep]};
                if (v.length() > 1e-6f) up = v;
            }
            f.up = up.length() > 1e-12f ? up.normalized() : Vec3f{0.0f, 1.0f, 0.0f};
            Vec3f mean{};
            std::size_t count = 0;
            for (const std::uint32_t s : f.strands) {
                const Vec3f* pts = in->strandPoints(s);
                for (std::size_t i = 0; i < in->strandPointCount(s); ++i) {
                    mean += pts[i] - f.root;
                    ++count;
                }
            }
            mean = count ? mean / static_cast<float>(count) : mean;
            Vec3f along = mean - f.up * mean.dot(f.up);
            f.along = along.length() > 1e-9f ? along.normalized() : anyPerpendicular(f.up).normalized();
            f.side = f.along.cross(f.up).normalized();
            for (const std::uint32_t s : f.strands) {
                const Vec3f* pts = in->strandPoints(s);
                for (std::size_t i = 0; i < in->strandPointCount(s); ++i) {
                    f.reach = std::max(f.reach, (pts[i] - f.root).length());
                    f.alongLength = std::max(f.alongLength, (pts[i] - f.root).dot(f.along));
                }
            }
            f.strength = std::clamp(strengthField.at(*in, f.rep), 0.0f, 1.0f);
            if (!std::isfinite(f.strength)) f.strength = 0.0f;
            // Its swept box: every point turned through 0 .. maxAngle about the side (sampled, and
            // grown by the most an arc between samples bulges past its chord).
            constexpr int kSweep = 4;
            const float big = std::numeric_limits<float>::max();
            Vec3f lo{big, big, big}, hi{-big, -big, -big};
            const float slice = maxAngle / static_cast<float>(kSweep);
            const float bulge = f.reach * (1.0f - pm::cos(0.5f * slice));
            for (const std::uint32_t s : f.strands) {
                const Vec3f* pts = in->strandPoints(s);
                for (std::size_t i = 0; i < in->strandPointCount(s); ++i) {
                    const Vec3f d = pts[i] - f.root;
                    const float a = d.dot(f.along), w = d.dot(f.side), n = d.dot(f.up);
                    for (int k = 0; k <= kSweep; ++k) {
                        const float t = slice * static_cast<float>(k), c = pm::cos(t), sn = pm::sin(t);
                        const Vec3f q{a * c - n * sn, w, a * sn + n * c};
                        lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
                        hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
                    }
                }
            }
            lo -= Vec3f{bulge, bulge, bulge};
            hi += Vec3f{bulge, bulge, bulge};
            const Vec3f mid = (lo + hi) * 0.5f;
            f.boxCenter = f.root + f.along * mid.x + f.side * mid.y + f.up * mid.z;
            f.boxHalf = (hi - lo) * 0.5f;
        }, 16);

        // The order between two feathers that can meet (a under b, or b under a): by rank, then
        // featherId (the lower underneath). rankBy rootDistance without a direction ranks each such
        // pair along its own way: against the two feathers' mean direction (the one rooted farther
        // back on top: each feather's tip over the base of the one behind it, roof tiles), so a
        // feather's order among its neighbours depends on them alone, not on feathers elsewhere.
        const Vec3f direction = params().getVec3("direction");
        const bool pairwise = rankBy == "rootDistance" && !(direction.length() > 1e-9f);
        const Vec3f axis = pairwise ? Vec3f{} : direction;
        for (Feather& f : feathers) {
            float r = 0.0f;
            if (rankBy == "rootDistance") r = f.root.dot(axis);
            else if (rankBy == "length") r = -f.reach;
            else if (rankBy == "channel") r = (*rankChannel)[f.rep];
            else r = unitFloat(hashCombine(hash64(static_cast<std::uint64_t>(f.id)), seed ^ 0x9101E5ull));
            f.rank = invert ? -r : r;
            if (!std::isfinite(f.rank)) f.rank = 0.0f;
        }
        // How far b ranks above a (0: equal), each pair's the other's negated, to the bit.
        auto above = [&](std::size_t a, std::size_t b) {
            const Feather& fa = feathers[a];
            const Feather& fb = feathers[b];
            if (!pairwise) return fb.rank - fa.rank;
            const Vec3f way = -(fa.along + fb.along);
            const float d = way.length() > 1e-9f ? (fb.root - fa.root).dot(way.normalized()) : 0.0f;
            return std::isfinite(d) ? (invert ? -d : d) : 0.0f;
        };
        auto lowerOf = [&](std::size_t a, std::size_t b) {
            const float d = above(a, b);
            return d != 0.0f ? d > 0.0f : feathers[a].id < feathers[b].id;
        };

        auto depthOf = [&](const Feather& f) { return depthParam > 0.0f ? depthParam : 0.25f * f.reach; };

        // What each feather lies over: the core feathers within reach facing its way (their ups less
        // than ~70 degrees apart: `near`, the same both ways) ranked under it whose boxes meet its.
        const PreviewCore core(*in);
        for (Feather& f : feathers) f.core = core.core(f.rep);
        float longest = 0.0f;  // of the core feathers
        for (const Feather& f : feathers) {
            if (f.core) longest = std::max(longest, f.reach);
        }
        const float margin = offset + bodyOffset + 1e-6f;
        const float meet = offset + margin;
        const float cell = std::max(2.0f * longest + margin, 1e-6f);
        auto cellOf = [&](const Vec3f& p) {
            return std::array<std::int64_t, 3>{static_cast<std::int64_t>(std::floor(p.x / cell)),
                                               static_cast<std::int64_t>(std::floor(p.y / cell)),
                                               static_cast<std::int64_t>(std::floor(p.z / cell))};
        };
        auto key = [](std::int64_t x, std::int64_t y, std::int64_t z) {
            return hashCombine(hashCombine(hash64(static_cast<std::uint64_t>(x)), static_cast<std::uint64_t>(y)),
                               static_cast<std::uint64_t>(z));
        };
        std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> grid;
        for (std::size_t fi = 0; fi < feathers.size(); ++fi) {
            const auto c = cellOf(feathers[fi].root);
            grid[key(c[0], c[1], c[2])].push_back(static_cast<std::uint32_t>(fi));
        }
        std::vector<std::vector<std::uint32_t>> near(feathers.size()), under(feathers.size());
        parallelFor(0, feathers.size(), [&](std::size_t b) {
            const Feather& fb = feathers[b];
            const auto c = cellOf(fb.root);
            std::vector<std::uint32_t>& list = near[b];
            for (std::int64_t dx = -1; dx <= 1; ++dx) {
                for (std::int64_t dy = -1; dy <= 1; ++dy) {
                    for (std::int64_t dz = -1; dz <= 1; ++dz) {
                        const auto it = grid.find(key(c[0] + dx, c[1] + dy, c[2] + dz));
                        if (it == grid.end()) continue;
                        for (const std::uint32_t a : it->second) {
                            const Feather& fa = feathers[a];
                            if (a == b || !fa.core || fa.up.dot(fb.up) < kSameSide) continue;
                            if ((fa.root - fb.root).length() > fa.reach + fb.reach + margin) continue;
                            list.push_back(a);
                        }
                    }
                }
            }
            std::sort(list.begin(), list.end());
            for (const std::uint32_t a : list) {
                if (lowerOf(a, b) && sweptBoxesMeet(feathers[a], fb, depthOf(feathers[a]), meet)) under[b].push_back(a);
            }
        }, 16);
        // Pairs ranked each by their own way can go round in a ring (a under b under c under a,
        // where the feathers' ways turn): each ring of feathers (a strongly connected part of what
        // lies over what) is ordered as one, along against its own feathers' mean way.
        if (pairwise) {
            int count = 0;
            const std::vector<int> part = stronglyConnected(under, &count);
            std::vector<std::vector<std::uint32_t>> rings(static_cast<std::size_t>(count));
            for (std::size_t f = 0; f < feathers.size(); ++f) rings[static_cast<std::size_t>(part[f])].push_back(static_cast<std::uint32_t>(f));
            for (const std::vector<std::uint32_t>& ring : rings) {
                if (ring.size() < 2) continue;
                Vec3f mean{};
                for (const std::uint32_t f : ring) mean += feathers[f].along;
                const Vec3f way = mean.length() > 1e-9f ? -mean.normalized() : Vec3f{0.0f, 0.0f, 1.0f};
                auto lowerInRing = [&](std::uint32_t a, std::uint32_t b) {
                    float d = (feathers[b].root - feathers[a].root).dot(way);
                    d = std::isfinite(d) ? (invert ? -d : d) : 0.0f;
                    return d != 0.0f ? d > 0.0f : feathers[a].id < feathers[b].id;
                };
                for (const std::uint32_t b : ring) {
                    std::vector<std::uint32_t>& list = under[b];
                    list.erase(std::remove_if(list.begin(), list.end(), [&](std::uint32_t a) { return part[a] == part[b]; }),
                               list.end());
                    for (const std::uint32_t a : near[b]) {
                        if (part[a] == part[b] && lowerInRing(a, b) && sweptBoxesMeet(feathers[a], feathers[b], depthOf(feathers[a]), meet)) {
                            list.push_back(a);
                        }
                    }
                    std::sort(list.begin(), list.end());
                }
            }
        }
        // Waves: a feather's is one past the highest of those it lies over.
        std::vector<std::vector<std::uint32_t>> over(feathers.size());
        std::vector<std::size_t> waiting(feathers.size());
        std::vector<std::uint32_t> ready;
        for (std::size_t b = 0; b < feathers.size(); ++b) {
            for (const std::uint32_t a : under[b]) over[a].push_back(static_cast<std::uint32_t>(b));
            waiting[b] = under[b].size();
            if (waiting[b] == 0) ready.push_back(static_cast<std::uint32_t>(b));
        }
        std::vector<int> wave(feathers.size(), 0);
        int waves = 0;
        for (std::size_t k = 0; k < ready.size(); ++k) {
            const std::uint32_t a = ready[k];
            waves = std::max(waves, wave[a] + 1);
            for (const std::uint32_t b : over[a]) {
                wave[b] = std::max(wave[b], wave[a] + 1);
                if (--waiting[b] == 0) ready.push_back(b);
            }
        }
        if (ready.size() != feathers.size()) throw GraphError("Plume: the feathers' order goes round in a ring");
        std::vector<std::vector<std::size_t>> byWave(static_cast<std::size_t>(waves));
        for (std::size_t b = 0; b < feathers.size(); ++b) byWave[static_cast<std::size_t>(wave[b])].push_back(b);

        // The body: the scalp, and the collider. Each feather is pushed out of it from as deep as
        // half its own length (and its search reaches that far); the surfaces' fields are made for
        // the longest feather's (their reach that search rounded up to a power of two: the same
        // fields while feathers elsewhere change, unless the longest crosses one).
        const float fieldDepth = std::max(0.5f * longest, 2.0f * bodyOffset);
        const float fieldSearch = std::max(bodyOffset, fieldDepth) * 1.001f + 1e-6f;
        std::vector<CollisionSurface> bodies;
        if (scalp) {
            if (CollisionSurface surface = CollisionSurface::of(scalpPtr, bodyOffset, fieldDepth, fieldSearch)) {
                bodies.push_back(std::move(surface));
            }
        }
        if (inputs.size() > 1) {
            const MeshPtr collider = valueAs<MeshPtr>(inputs[1]);
            if (collider && collider->triangleCount() > 0) {
                if (CollisionSurface surface = CollisionSurface::of(collider, bodyOffset, fieldDepth, fieldSearch)) {
                    bodies.push_back(std::move(surface));
                }
            }
        }

        auto out = std::make_shared<StrandSet>(*in);
        std::vector<Vec3f>& positions = out->mutablePositions();
        std::vector<HeightMap> maps(feathers.size());

        auto solve = [&](std::size_t b) {
            Feather& f = feathers[b];
            // The points lifted for (past the root zone), each with its place along the feather.
            struct Sample {
                Vec3f p;
                float u;
            };
            thread_local std::vector<Sample> samples;
            samples.clear();
            const float zone = kRootZone * f.reach;
            const float bodyDepth = std::max(0.5f * f.reach, 2.0f * bodyOffset);
            const float bodySearch = std::max(bodyOffset, bodyDepth) * 1.001f + 1e-6f;
            for (const std::uint32_t s : f.strands) {
                const Vec3f* pts = in->strandPoints(s);
                for (std::size_t i = 0; i < in->strandPointCount(s); ++i) {
                    if ((pts[i] - f.root).length() >= zone) samples.push_back({pts[i], f.uOf(pts[i])});
                }
            }
            std::vector<const HeightMap*> lower;
            for (const std::uint32_t a : under[b]) {
                if (maps[a].valid()) lower.push_back(&maps[a]);
            }
            struct Contact {
                float u, angle;
            };
            thread_local std::vector<Contact> contacts;
            thread_local std::vector<Vec3f> turnedNow;
            thread_local std::vector<const HeightMap*> near;
            for (int pass = 0; pass < iterations && maxAngle > 0.0f; ++pass) {
                contacts.clear();
                std::vector<RestPoseCollision::Cursor> cursors(bodies.size());
                for (std::size_t m = 0; m < bodies.size(); ++m) {
                    cursors[m] = bodies[m].cursor(in->roots()[f.rep], f.root);
                }
                // The lower maps this pass can meet: their boxes against the feather's points now.
                const float big = std::numeric_limits<float>::max();
                Vec3f lo{big, big, big}, hi{-big, -big, -big};
                turnedNow.resize(samples.size());
                for (std::size_t k = 0; k < samples.size(); ++k) {
                    const Vec3f p = f.turned(samples[k].p, f.turnAt(samples[k].u));
                    turnedNow[k] = p;
                    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
                }
                near.clear();
                for (const HeightMap* map : lower) {
                    if (hi.x < map->boxMin.x || hi.y < map->boxMin.y || hi.z < map->boxMin.z || lo.x > map->boxMax.x ||
                        lo.y > map->boxMax.y || lo.z > map->boxMax.z) {
                        continue;
                    }
                    near.push_back(map);
                }
                for (std::size_t k = 0; k < samples.size(); ++k) {
                    const Sample& sample = samples[k];
                    const Vec3f p = turnedNow[k];
                    const Vec3f arm = p - f.root;
                    const float radius = arm.length();
                    const Vec3f swing = f.side.cross(arm);  // where a turn moves p, per radian
                    float need = 0.0f;
                    auto lift = [&](float distance, const Vec3f& normal) {
                        const float lever = std::max(swing.dot(normal), 0.1f * radius);
                        if (lever > 0.0f) need = std::max(need, distance / lever);
                    };
                    for (const HeightMap* map : near) {
                        float h = 0.0f, t = 0.0f, bottom = 0.0f;
                        if (!map->at(p, &h, &t, &bottom)) continue;
                        if (h < t + offset && h > bottom - map->depth) {
                            lift(t + offset - h, map->z);
                        }
                    }
                    for (std::size_t m = 0; m < bodies.size(); ++m) {
                        Vec3f q = p;
                        if (bodies[m].pushOut(q, bodyOffset, bodyDepth, bodySearch, cursors[m])) {
                            const Vec3f delta = q - p;
                            const float d = delta.length();
                            if (d > 0.0f) lift(d, delta / d);
                        }
                    }
                    if (need > 0.0f) contacts.push_back({sample.u, need});
                }
                if (contacts.empty()) break;
                float rigid = 0.0f;
                for (const Contact& c : contacts) rigid = std::max(rigid, c.angle);
                bool turned = false;
                for (std::size_t k = 0; k < kTable; ++k) {
                    const float u = static_cast<float>(k) / static_cast<float>(kTable - 1);
                    float bend = 0.0f;
                    if (stiffness < 1.0f) {
                        for (const Contact& c : contacts) bend = std::max(bend, c.angle * ramp(c.u > 0.0f ? u / c.u : 1.0f));
                    }
                    const float step = std::min(stiffness * rigid + (1.0f - stiffness) * bend, kMaxStep);
                    const float next = std::min(f.turn[k] + step, maxAngle);
                    turned = turned || next != f.turn[k];
                    f.turn[k] = next;
                }
                if (!turned) break;
            }
            for (float& t : f.turn) t *= f.strength;
            f.moved = std::any_of(f.turn.begin(), f.turn.end(), [](float t) { return t != 0.0f; });
            // Its strands where it now lies.
            if (f.moved) {
                for (const std::uint32_t s : f.strands) {
                    const std::size_t begin = in->strandBegin(s), n = in->strandPointCount(s);
                    const Vec3f* pts = in->strandPoints(s);
                    for (std::size_t i = 0; i < n; ++i) {
                        const float angle = f.turnAt(f.uOf(pts[i]));
                        if (angle != 0.0f) positions[begin + i] = f.turned(pts[i], angle);
                    }
                }
            }
            maps[b] = heightMapOf(f, *out, depthOf(f), offset);
        };
        for (const std::vector<std::size_t>& wavefront : byWave) {
            parallelFor(0, wavefront.size(), [&](std::size_t k) { solve(wavefront[k]); }, 1);
        }

        const bool anyMoved = std::any_of(feathers.begin(), feathers.end(), [](const Feather& f) { return f.moved; });
        if (!anyMoved) return in;

        // The vane normals turn with their feathers (each by the turn where its strand starts).
        if (hasVaneNormals) {
            std::array<std::vector<float>*, 3> normals{};
            for (int k = 0; k < 3; ++k) normals[static_cast<std::size_t>(k)] = out->mutableStrandChannel(kVaneNormalChannels[k]);
            parallelFor(0, feathers.size(), [&](std::size_t fi) {
                const Feather& f = feathers[fi];
                if (!f.moved) return;
                for (const std::uint32_t s : f.strands) {
                    const float angle = f.turnAt(f.uOf(in->strandPoints(s)[0]));
                    if (angle == 0.0f) continue;
                    const Vec3f v{(*normals[0])[s], (*normals[1])[s], (*normals[2])[s]};
                    const Vec3f r = rotateAxisAngle(v, f.side, angle);
                    (*normals[0])[s] = r.x;
                    (*normals[1])[s] = r.y;
                    (*normals[2])[s] = r.z;
                }
            }, 64);
        }
        // A rachis turns with its feather (layers numbered as buildRachisMesh and Merge number them).
        if (!in->featherLayers().empty()) {
            std::unordered_map<int, std::size_t> byId;
            for (std::size_t fi = 0; fi < feathers.size(); ++fi) byId.emplace(feathers[fi].id, fi);
            std::vector<FeatherLayerPtr> layers;
            int base = 0;
            for (const FeatherLayerPtr& layer : in->featherLayers()) {
                int highest = -1;
                for (const int id : layer->featherIds) highest = std::max(highest, id);
                if (!layer->hasRachis()) {
                    layers.push_back(layer);
                    base += static_cast<int>(groupNumberBase(highest));
                    continue;
                }
                auto turned = std::make_shared<FeatherLayer>(*layer);
                for (std::size_t k = 0; k < turned->featherIds.size(); ++k) {
                    const auto it = byId.find(base + turned->featherIds[k]);
                    if (it == byId.end() || !feathers[it->second].moved) continue;
                    const Feather& f = feathers[it->second];
                    for (std::size_t r = turned->offsets[k]; r < turned->offsets[k + 1]; ++r) {
                        RachisRing& ring = turned->rings[r];
                        const float angle = f.turnAt(f.uOf(ring.center));
                        if (angle == 0.0f) continue;
                        ring.center = f.turned(ring.center, angle);
                        ring.tangent = rotateAxisAngle(ring.tangent, f.side, angle);
                        ring.normal = rotateAxisAngle(ring.normal, f.side, angle);
                        ring.side = rotateAxisAngle(ring.side, f.side, angle);
                    }
                }
                layers.push_back(std::move(turned));
                base += static_cast<int>(groupNumberBase(highest));
            }
            out->setFeatherLayers(std::move(layers));
        }
        return StrandsPtr(out);
    }

private:
    // Whether feather b (anywhere it can turn to) can come within reach of feather a's height map
    // (a's swept box grown by `below` under it and `above` over it): their boxes overlap on each
    // of their faces' axes (a conservative test: boxes it keeps may still miss each other).
    static bool sweptBoxesMeet(const Feather& a, const Feather& b, float below, float above) {
        // a's box grown along its up: `below` down, `above` up.
        const Vec3f aCenter = a.boxCenter + a.up * (0.5f * (above - below));
        const Vec3f aHalf = a.boxHalf + Vec3f{above, above, 0.5f * (above + below)};
        const Vec3f between = b.boxCenter - aCenter;
        auto separated = [&](const Vec3f& axis) {
            float ra = 0.0f, rb = 0.0f;
            for (int k = 0; k < 3; ++k) {
                ra += (k == 0 ? aHalf.x : k == 1 ? aHalf.y : aHalf.z) * std::fabs(a.axisOf(k).dot(axis));
                rb += (k == 0 ? b.boxHalf.x : k == 1 ? b.boxHalf.y : b.boxHalf.z) * std::fabs(b.axisOf(k).dot(axis));
            }
            return std::fabs(between.dot(axis)) > ra + rb;
        };
        for (int k = 0; k < 3; ++k) {
            if (separated(a.axisOf(k)) || separated(b.axisOf(k))) return false;
        }
        return true;
    }

    // Feather f's strands (where they now lie in `strands`) as a height map in its turned frame.
    static HeightMap heightMapOf(const Feather& f, const StrandSet& strands, float depth, float offset) {
        HeightMap map;
        const float mid = f.turnAt(0.5f);
        map.origin = f.root;
        map.x = rotateAxisAngle(f.along, f.side, mid);
        map.z = rotateAxisAngle(f.up, f.side, mid);
        map.y = f.side;
        map.depth = depth;
        float x0 = std::numeric_limits<float>::max(), x1 = -x0, y0 = x0, y1 = -x0;
        Vec3f lo{x0, x0, x0}, hi{-x0, -x0, -x0};
        for (const std::uint32_t s : f.strands) {
            const Vec3f* pts = strands.strandPoints(s);
            for (std::size_t i = 0; i < strands.strandPointCount(s); ++i) {
                const Vec3f d = pts[i] - map.origin;
                x0 = std::min(x0, d.dot(map.x));
                x1 = std::max(x1, d.dot(map.x));
                y0 = std::min(y0, d.dot(map.y));
                y1 = std::max(y1, d.dot(map.y));
                lo = {std::min(lo.x, pts[i].x), std::min(lo.y, pts[i].y), std::min(lo.z, pts[i].z)};
                hi = {std::max(hi.x, pts[i].x), std::max(hi.y, pts[i].y), std::max(hi.z, pts[i].z)};
            }
        }
        if (!(x1 > x0) && !(y1 > y0)) return map;
        const float lengthX = std::max(x1 - x0, 1e-6f), lengthY = std::max(y1 - y0, 1e-6f);
        map.nx = kMapAlong;
        map.ny = std::clamp(static_cast<int>(std::ceil(static_cast<float>(kMapAlong) * lengthY / lengthX)), 2, 2 * kMapAlong);
        map.cellX = lengthX / static_cast<float>(map.nx) * (1.0f + 1e-5f);
        map.cellY = lengthY / static_cast<float>(map.ny) * (1.0f + 1e-5f);
        map.x0 = x0;
        map.y0 = y0;
        const std::size_t cells = static_cast<std::size_t>(map.nx) * static_cast<std::size_t>(map.ny);
        map.top.assign(cells, -std::numeric_limits<float>::max());
        map.bottom.assign(cells, std::numeric_limits<float>::max());
        auto mark = [&](const Vec3f& p) {
            const Vec3f d = p - map.origin;
            const int ix = std::clamp(static_cast<int>((d.dot(map.x) - map.x0) / map.cellX), 0, map.nx - 1);
            const int iy = std::clamp(static_cast<int>((d.dot(map.y) - map.y0) / map.cellY), 0, map.ny - 1);
            const std::size_t c = static_cast<std::size_t>(iy) * static_cast<std::size_t>(map.nx) + static_cast<std::size_t>(ix);
            const float h = d.dot(map.z);
            map.top[c] = std::max(map.top[c], h);
            map.bottom[c] = std::min(map.bottom[c], h);
        };
        // Every segment, in steps of at most half a cell: the vane covered, not dotted.
        const float step = 0.5f * std::min(map.cellX, map.cellY);
        for (const std::uint32_t s : f.strands) {
            const Vec3f* pts = strands.strandPoints(s);
            const std::size_t n = strands.strandPointCount(s);
            mark(pts[0]);
            for (std::size_t i = 1; i < n; ++i) {
                const Vec3f d = pts[i] - pts[i - 1];
                const int steps = std::clamp(static_cast<int>(std::ceil(d.length() / step)), 1, 256);
                for (int k = 1; k <= steps; ++k) mark(pts[i - 1] + d * (static_cast<float>(k) / static_cast<float>(steps)));
            }
        }
        // Gaps between barbs a cell wide: filled from the cells either side (the vane is whole).
        const std::vector<float> top = map.top, bottom = map.bottom;
        auto filled = [&](std::size_t c) { return top[c] >= bottom[c]; };
        for (int iy = 0; iy < map.ny; ++iy) {
            for (int ix = 0; ix < map.nx; ++ix) {
                const std::size_t c = static_cast<std::size_t>(iy) * static_cast<std::size_t>(map.nx) + static_cast<std::size_t>(ix);
                if (filled(c)) continue;
                auto take = [&](std::size_t a, std::size_t b) {
                    if (!filled(a) || !filled(b)) return;
                    map.top[c] = std::max(map.top[c], std::max(top[a], top[b]));
                    map.bottom[c] = std::min(map.bottom[c], std::min(bottom[a], bottom[b]));
                };
                if (ix > 0 && ix + 1 < map.nx) take(c - 1, c + 1);
                if (iy > 0 && iy + 1 < map.ny) take(c - static_cast<std::size_t>(map.nx), c + static_cast<std::size_t>(map.nx));
            }
        }
        // What can reach it: its bounds grown by the gap above and the depth below.
        const float grow = depth + offset + 1e-6f;
        map.boxMin = lo - Vec3f{grow, grow, grow};
        map.boxMax = hi + Vec3f{grow, grow, grow};
        return map;
    }
};

}  // namespace

void registerPlumeOperator(OperatorRegistry& registry) {
    registry.registerType(op::kPlume,
                          {"feathers",
                           "Lays feathers over one another (by a rank) and off the body: no vane through another.",
                           "feather layers overlap"},
                          [] { return std::make_unique<PlumeOp>(); });
}

}  // namespace groomlab
