// SPDX-License-Identifier: Apache-2.0
// Instance (Strands -> Strands): geometry on the strands, e.g. snowflakes near
// the tips, dandruff near the roots, dew, burrs or beads. Each strand gets up
// to `count` instances, each kept with the (mappable) probability, somewhere
// between tMin and tMax along it; orientation, size, spin and offset vary per
// instance. The strands pass through unchanged, carrying the instances (see
// instances.h); hosts place them on the final strands. sizeMode fitStrand sizes
// each to its strand instead (leaves, feathers spanning it): a fraction of the
// strand's rest length along the prototype's +Y, `aspect` of that across.
// Several prototypes (names separated by spaces) are variants, one per
// instance, picked by its ID or by a map. With strands on its `along` input
// (a Braid node's ties) the instances go on those instead, which are carried
// with the instances (InstanceSet::anchors) and never drawn as hair: beads,
// cuffs and hair ties round braids (fitWidth: as thick as the braid there).
// Where on the hair they go: `exposure` keeps them near the hair's outer surface (external), away from
// it (internal) or anywhere; `axisFacing` keeps them on the side of the hair facing an axis (snow on
// top: +Y), each within its falloff's depth under the surface (HairVolume).
// Scales (Strands -> Strands): one scale at each strand's root instead, lying on the skin along the
// strand, as long as the strand and as wide as its root (times `size`; scales.h makes their
// meshes: tilted like roof tiles, bent onto the skin).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>

#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/instances.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/point_index.h"
#include "groomlab/portable_math.h"
#include "groomlab/random.h"
#include "groomlab/scalp_mesh.h"
#include "incremental.h"
#include "internal.h"

namespace groomlab {

namespace {

constexpr float kTwoPi = 6.2831853f;
constexpr float kDegrees = 0.017453293f;

// The hair as a volume on a grid (the strands' segments in cells, closed by a cell), for how deep a
// point lies under its outer surface: straight in (depth) or along an axis from its side
// (depthAlong). Outside is the empty space reaching the grid's border (a cavity inside is hair).
class HairVolume {
public:
    static constexpr int kCells = 96;  // along the longest side

    // axis 0-2 (x, y, z) and sign +1 / -1: the side depthAlong measures from; axis -1: none.
    HairVolume(const StrandSet& hair, bool depthIn, int axis, int sign) {
        const std::vector<Vec3f>& points = hair.positions();
        if (points.empty()) return;
        Vec3f lo = points[0], hi = points[0];
        for (const Vec3f& p : points) {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        const float extent = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
        // The gap between neighbouring strands: twice the mean distance from a root to its nearest
        // (a sample of them). Cells are at least half of it, and the strands grow by it.
        const float gap = 2.0f * meanRootSpacing(hair);
        cell_ = std::max(extent / static_cast<float>(kCells), 0.5f * gap);
        if (!(cell_ > 0.0f)) cell_ = 1.0f;
        grow_ = std::clamp(static_cast<int>(std::ceil(gap / cell_)), 1, 3);
        const int kPad = grow_ + 1;
        origin_ = lo - Vec3f{1, 1, 1} * (cell_ * kPad);
        n_[0] = static_cast<int>((hi.x - lo.x) / cell_) + 1 + 2 * kPad;
        n_[1] = static_cast<int>((hi.y - lo.y) / cell_) + 1 + 2 * kPad;
        n_[2] = static_cast<int>((hi.z - lo.z) / cell_) + 1 + 2 * kPad;
        const std::size_t total = static_cast<std::size_t>(n_[0]) * n_[1] * n_[2];
        // The segments, sampled every half cell.
        std::vector<std::uint8_t> hit(total, 0);
        for (std::size_t s = 0; s < hair.strandCount(); ++s) {
            const Vec3f* p = hair.strandPoints(s);
            const std::size_t count = hair.strandPointCount(s);
            for (std::size_t i = 0; i < count; ++i) {
                hit[indexOf(p[i])] = 1;
                if (i + 1 == count) break;
                const Vec3f d = p[i + 1] - p[i];
                const int steps = static_cast<int>(d.length() / (0.5f * cell_));
                for (int k = 1; k <= steps; ++k) hit[indexOf(p[i] + d * (static_cast<float>(k) / static_cast<float>(steps + 1)))] = 1;
            }
        }
        // Grown by grow_ cells (a box, one axis at a time): the gaps between strands are hair.
        std::vector<std::uint8_t> solid = hit;
        for (int a = 0; a < 3; ++a) {
            std::vector<std::uint8_t> from = solid;
            const std::size_t step = a == 0 ? 1 : a == 1 ? static_cast<std::size_t>(n_[0])
                                                         : static_cast<std::size_t>(n_[0]) * static_cast<std::size_t>(n_[1]);
            for (std::size_t i = 0; i < total; ++i) {
                if (!from[i]) continue;
                const int c = coordinate(i, a);
                for (int d = -grow_; d <= grow_; ++d) {
                    if (c + d < 0 || c + d >= n_[a]) continue;
                    solid[static_cast<std::size_t>(static_cast<std::ptrdiff_t>(i) + static_cast<std::ptrdiff_t>(d) *
                                                                                      static_cast<std::ptrdiff_t>(step))] = 1;
                }
            }
        }
        // Outside: the empty cells reaching the border.
        outside_.assign(total, 0);
        std::vector<std::size_t> stack;
        auto reach = [&](int x, int y, int z) {
            const std::size_t i = at(x, y, z);
            if (!solid[i] && !outside_[i]) {
                outside_[i] = 1;
                stack.push_back(i);
            }
        };
        for (int z = 0; z < n_[2]; ++z) {
            for (int y = 0; y < n_[1]; ++y) {
                for (int x = 0; x < n_[0]; ++x) {
                    if (x == 0 || y == 0 || z == 0 || x + 1 == n_[0] || y + 1 == n_[1] || z + 1 == n_[2]) reach(x, y, z);
                }
            }
        }
        while (!stack.empty()) {
            const std::size_t i = stack.back();
            stack.pop_back();
            const int x = coordinate(i, 0), y = coordinate(i, 1), z = coordinate(i, 2);
            if (x > 0) reach(x - 1, y, z);
            if (x + 1 < n_[0]) reach(x + 1, y, z);
            if (y > 0) reach(x, y - 1, z);
            if (y + 1 < n_[1]) reach(x, y + 1, z);
            if (z > 0) reach(x, y, z - 1);
            if (z + 1 < n_[2]) reach(x, y, z + 1);
        }
        if (depthIn) chamfer();
        if (axis >= 0) along(axis, sign);
    }

    bool empty() const { return outside_.empty(); }
    // How deep `p` lies under the hair's outer surface, scene units (0 on it and outside). (The
    // outermost strands' cells lie grow_ cells in from the outside: they are 0 deep.)
    float depth(const Vec3f& p) const {
        if (dist_.empty()) return 0.0f;
        return std::max(0.0f, static_cast<float>(dist_[indexOf(p)]) / 3.0f - static_cast<float>(grow_ + 1)) * cell_;
    }
    // How deep under the hair's surface on the axis' side, along the axis.
    float depthAlong(const Vec3f& p) const {
        if (run_.empty()) return 0.0f;
        return std::max(0.0f, static_cast<float>(run_[indexOf(p)]) - static_cast<float>(grow_ + 1)) * cell_;
    }

private:
    // The mean distance from a root to the nearest other root, over a sample of up to 512 strands
    // (0 for fewer than two).
    static float meanRootSpacing(const StrandSet& hair) {
        std::vector<Vec3f> roots;
        roots.reserve(hair.strandCount());
        for (std::size_t s = 0; s < hair.strandCount(); ++s) {
            if (hair.strandPointCount(s)) roots.push_back(hair.strandPoints(s)[0]);
        }
        if (roots.size() < 2) return 0.0f;
        const std::size_t count = roots.size(), samples = std::min<std::size_t>(count, 512);
        const PointIndex index(roots);
        double sum = 0.0;
        std::size_t found = 0;
        for (std::size_t k = 0; k < samples; ++k) {
            std::size_t near[2];
            float d2[2];
            // (itself first; a root another shares is skipped as itself)
            if (index.kNearest(roots[k * count / samples], 2, near, d2) == 2 && d2[1] > 0.0f) {
                sum += std::sqrt(d2[1]);
                ++found;
            }
        }
        return found ? static_cast<float>(sum / static_cast<double>(found)) : 0.0f;
    }
    int coordinate(std::size_t i, int a) const {
        const auto nx = static_cast<std::size_t>(n_[0]), ny = static_cast<std::size_t>(n_[1]);
        return static_cast<int>(a == 0 ? i % nx : a == 1 ? i / nx % ny : i / (nx * ny));
    }
    std::size_t at(int x, int y, int z) const {
        return (static_cast<std::size_t>(z) * static_cast<std::size_t>(n_[1]) + static_cast<std::size_t>(y)) *
                   static_cast<std::size_t>(n_[0]) +
               static_cast<std::size_t>(x);
    }
    std::size_t indexOf(const Vec3f& p) const {
        const Vec3f g = (p - origin_) / cell_;
        auto clampTo = [](float v, int n) { return std::clamp(static_cast<int>(v), 0, n - 1); };
        return at(clampTo(g.x, n_[0]), clampTo(g.y, n_[1]), clampTo(g.z, n_[2]));
    }
    // A 3-4-5 chamfer distance (in thirds of a cell) to the nearest outside cell.
    void chamfer() {
        constexpr std::int32_t kFar = 1 << 28;
        dist_.assign(outside_.size(), kFar);
        for (std::size_t i = 0; i < outside_.size(); ++i) {
            if (outside_[i]) dist_[i] = 0;
        }
        auto pass = [&](int dir) {
            const int x0 = dir > 0 ? 0 : n_[0] - 1, y0 = dir > 0 ? 0 : n_[1] - 1, z0 = dir > 0 ? 0 : n_[2] - 1;
            for (int z = z0; z >= 0 && z < n_[2]; z += dir) {
                for (int y = y0; y >= 0 && y < n_[1]; y += dir) {
                    for (int x = x0; x >= 0 && x < n_[0]; x += dir) {
                        std::int32_t& d = dist_[at(x, y, z)];
                        if (d == 0) continue;
                        // The 13 neighbours this pass has already been through.
                        for (int dz = -1; dz <= 0; ++dz) {
                            for (int dy = -1; dy <= 1; ++dy) {
                                for (int dx = -1; dx <= 1; ++dx) {
                                    if (dz == 0 && (dy > 0 || (dy == 0 && dx >= 0))) continue;
                                    const int nx = x + dx * dir, ny = y + dy * dir, nz = z + dz * dir;
                                    if (nx < 0 || ny < 0 || nz < 0 || nx >= n_[0] || ny >= n_[1] || nz >= n_[2]) continue;
                                    const int steps = (dx != 0) + (dy != 0) + (dz != 0);
                                    const std::int32_t w = steps == 1 ? 3 : steps == 2 ? 4 : 5;
                                    d = std::min(d, dist_[at(nx, ny, nz)] + w);
                                }
                            }
                        }
                    }
                }
            }
        };
        pass(1);
        pass(-1);
    }
    // Per cell, how many cells in from the outside it is, coming along the axis from its `sign` end.
    void along(int axis, int sign) {
        run_.assign(outside_.size(), 0);
        const int a = axis, b = (axis + 1) % 3, c = (axis + 2) % 3;
        int idx[3] = {0, 0, 0};
        for (int j = 0; j < n_[c]; ++j) {
            for (int i = 0; i < n_[b]; ++i) {
                std::int32_t run = 0;
                for (int k = 0; k < n_[a]; ++k) {
                    idx[a] = sign > 0 ? n_[a] - 1 - k : k;
                    idx[b] = i;
                    idx[c] = j;
                    const std::size_t cellAt = at(idx[0], idx[1], idx[2]);
                    run = outside_[cellAt] ? 0 : run + 1;
                    run_[cellAt] = run;
                }
            }
        }
    }

    float cell_ = 1.0f;
    int grow_ = 1;  // cells the strands grow by
    Vec3f origin_;
    int n_[3] = {0, 0, 0};
    std::vector<std::uint8_t> outside_;
    std::vector<std::int32_t> dist_, run_;
};

// 1 at 0 deep, 0 from `falloff` on, smoothly between (a hard edge with no falloff).
float within(float depth, float falloff) {
    if (falloff <= 0.0f) return depth <= 0.0f ? 1.0f : 0.0f;
    const float x = std::clamp(depth / falloff, 0.0f, 1.0f);
    return 1.0f - x * x * (3.0f - 2.0f * x);
}

// The point a fraction `t` of a strand's length along it.
Vec3f pointAlong(const StrandSet& strands, std::size_t s, float t) {
    const Vec3f* p = strands.strandPoints(s);
    const std::size_t count = strands.strandPointCount(s);
    if (count == 0) return {};
    if (count == 1) return p[0];
    float length = 0.0f;
    for (std::size_t i = 0; i + 1 < count; ++i) length += (p[i + 1] - p[i]).length();
    float left = std::clamp(t, 0.0f, 1.0f) * length;
    for (std::size_t i = 0; i + 1 < count; ++i) {
        const float seg = (p[i + 1] - p[i]).length();
        if (left <= seg || i + 2 == count) return seg > 0.0f ? p[i] + (p[i + 1] - p[i]) * std::min(left / seg, 1.0f) : p[i];
        left -= seg;
    }
    return p[count - 1];
}

InstanceOrient orientFrom(const std::string& name) {
    if (name == "strand") return InstanceOrient::Strand;
    if (name == "up") return InstanceOrient::Up;
    if (name == "root") return InstanceOrient::Root;
    if (name == "random") return InstanceOrient::Random;
    throw GraphError("Instance: orient must be strand, up, root or random");
}


// Instance and Scales: one class, `scales_` telling them apart (each reads only its own params).
class InstanceOp : public Operator {
public:
    explicit InstanceOp(bool scales) : scales_(scales) {}
    std::string typeName() const override { return scales_ ? op::kScales : op::kInstance; }
    std::string category() const override { return "look"; }
    // Instance 2: pinToRest on by default; 3: exposure external by default.
    int version() const override { return scales_ ? 1 : 3; }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }
    std::vector<PortSpec> inputs() const override {
        if (scales_) return {{"strands", PortType::Strands}};
        return {{"strands", PortType::Strands}, {"along", PortType::Strands, true}};
    }
    std::vector<ParamSpec> paramSpecs() const override {
        std::vector<ParamSpec> specs;
        if (scales_) {
            specs = {
                {"prototype", std::string("groomlab:scale"),
                 "The plate (a mesh's name in the scene); several separated by spaces are variants, one per scale "
                 "(variantBy). Built in: groomlab:scale, groomlab:scaleRound, groomlab:scaleShield"},
                ParamSpec{"variantBy", std::string("random"),
                          "With several prototypes: random (by each scale's ID and the seed) or map (the variant param, "
                          "0-1 over the list)", "", {"random", "map"}}
                    .in("Variants"),
                ParamSpec{"variant", 0.0, "variantBy map: which prototype, 0 the first to 1 the last (mappable)"}
                    .range(0, 1)
                    .in("Variants")
                    .when("variantBy", {"map"}),
                ParamSpec{"size", 1.0, "Each scale's size: times its strand's length along and its root width across "
                                       "(mappable)"}
                    .soft(0, 2),
                ParamSpec{"aspect", 0.1, "Width over length for strands without widths (mappable)"}.atLeast(0).soft(0, 1),
                ParamSpec{"lift", 0.0, "Degrees each tip turns up off the skin, along the flow (on top of tile's rise)",
                          "angle"}
                    .soft(-90, 90),
                ParamSpec{"tile", 1.0,
                          "How far each tip rises over the next scale's base, in plate heights (1: by its own height, as "
                          "roof tiles; 0: flat; lift turns it further). Overlap comes from the density and the strands' "
                          "length"}
                    .atLeast(0)
                    .soft(0, 2),
                ParamSpec{"stack", 0.02, "A random height of their own, a fraction of the plate's: overlapping scales "
                                         "never share a depth (no z-fighting)", "fraction"}
                    .range(0, 1)
                    .soft(0, 0.2),
                ParamSpec{"conform", true, "Bend each onto the skin, every vertex at its height above the surface under it"},
                ParamSpec{"follow", std::string("skin"),
                          "skin (flat on the skin, tilted by lift and tile) or strand (the base on the skin, the rest "
                          "bent along the strand's curve over the plate's length: swept scales, spikes and quills that "
                          "follow the groom)",
                          "", {"skin", "strand"}},
                ParamSpec{"aim", std::string("tip"),
                          "Flat on the skin: which way each points, toward its strand's tip (root to tip) or root (the "
                          "strand's direction near its root: strands that curl back). follow strand always aims by the "
                          "root",
                          "", {"tip", "root"}}
                    .when("follow", {"skin"}),
                ParamSpec{"offset", 0.0, "Their height above the skin, scene units", "length"}.soft(0, 0.5),
                ParamSpec{"showStrands", false, "The strands draw and render too (off: they only lay the scales out)"},
                ParamSpec{"material", std::string(),
                          "The shading group or shader they render with (empty: the prototype mesh's own; the built-in "
                          "plates a surface reading the scaleColor user data)"},
                ParamSpec{"preview", std::string("auto"),
                          "In the viewport: auto (their meshes, flat quads past the viewport's budget), meshes or quads "
                          "(fast while grooming); renders always take the meshes",
                          "", {"auto", "meshes", "quads"}}
                    .adv(),
                ParamSpec{"scaleRandom", 0.1, "Each scale's size up to this share more or less, at random", "fraction"}
                    .range(0, 1)
                    .in("Random"),
                ParamSpec{"spinRandom", 0.0, "Random turn about the skin's normal, either way, 0-1 (1: half a turn)",
                          "fraction"}
                    .range(0, 1)
                    .in("Random"),
            };
        } else {
            specs = {
                ParamSpec{"prototype", std::string("instance"),
                          "The mesh to instance (its name in the scene); several separated by commas are variants, one "
                          "per instance (variantBy)"}
                    .named("Object(s)"),
                ParamSpec{"variantBy", std::string("random"),
                          "With several prototypes: random (by each instance's ID and the seed) or map (the variant "
                          "param, 0-1 over the list)", "", {"random", "map"}}
                    .in("Variants"),
                ParamSpec{"variant", 0.0, "variantBy map: which prototype, 0 the first to 1 the last (mappable)"}
                    .range(0, 1)
                    .in("Variants")
                    .when("variantBy", {"map"}),
                notAnimatable({"count", std::int64_t{1}, "Instances tried per strand"}).range(0, 64).soft(0, 8),
                notAnimatable({"probability", 0.1, "Chance each one is kept, per strand (mappable)", "fraction"}).soft(0, 1),
                ParamSpec{"tMin", 0.6, "Nearest the root they sit, 0 root to 1 tip"}.range(0, 1),
                ParamSpec{"tMax", 1.0, "Nearest the tip they sit"}.range(0, 1),
                ParamSpec{"sizeMode", std::string("fixed"),
                          "fixed (scale: a size in scene units) or fitStrand (each spans its strand: fitScale of the "
                          "strand's rest length along its +Y, aspect of that across; stable while strands stretch)",
                          "", {"fixed", "fitStrand"}},
                ParamSpec{"scale", 0.02, "Size, scene units (mappable)", "length"}.soft(0, 0.2).when("sizeMode", {"fixed"}),
                ParamSpec{"fitScale", 1.0, "Length along +Y, a fraction of the strand's rest length (1: the whole strand) "
                                           "(mappable)"}
                    .soft(0, 2)
                    .when("sizeMode", {"fitStrand"}),
                ParamSpec{"aspect", 0.1, "Width across (X and Z) over the length along +Y (mappable); with "
                                         "fitWidth, over the strand's width"}
                    .atLeast(0)
                    .soft(0, 1)
                    .when("sizeMode", {"fitStrand"}),
                ParamSpec{"fitWidth", false, "Across: the strand's width where it sits (times aspect) instead of its "
                                             "length: beads and cuffs round a Braid node's ties, as thick as the braid"}
                    .when("sizeMode", {"fitStrand"}),
                ParamSpec{"scaleByWidth", true,
                          "Scale each one by the strand's width where it sits over its root width (the Width node's "
                          "profile): smaller toward thin tips"}
                    .in("Placement"),
                ParamSpec{"deform", std::string("none"),
                          "none: each one rigid, placed by its frame; alongStrand: bent along its strand (Yeti's "
                          "instance deform), the prototype's long axis (deformAxis) laid onto the strand from where it "
                          "sits, facing the vane (a feather's) or the skin: leaves on vines, feather meshes following a "
                          "shaft, ribbons",
                          "", {"none", "alongStrand"}}
                    .in("Placement"),
                notAnimatable({"deformAxis", std::string("y"), "deform alongStrand: the prototype's axis laid along the "
                                                               "strand (its length); the next two (cyclically) face up "
                                                               "and across",
                               "", {"x", "y", "z"}})
                    .in("Placement")
                    .when("deform", {"alongStrand"}),
                ParamSpec{"orient", std::string("strand"), "strand (along it), up (world up), root (the root direction) or "
                                                           "random", "", {"strand", "up", "root", "random"}}
                    .in("Placement"),
                ParamSpec{"offset", 0.0, "Distance off the strand, scene units", "length"}.soft(0, 0.5).in("Placement"),
                ParamSpec{"pinToRest", true,
                          "Keep each one at a fixed distance from the root, measured on the strand's rest pose, instead "
                          "of a fraction of its length now: strands that stretch (simulated guides) don't slide them"}
                    .formerDefault(2, false)
                    .in("Placement"),
                ParamSpec{"exposure", std::string("external"),
                          "Where in the hair they go: externally (near the hair's outer surface, where they are seen: "
                          "none hidden inside to render), internally (inside it, away from the surface) or both",
                          "", {"external", "internal", "both"}}
                    .choicesNamed({"Instance externally", "Instance internally", "Instance externally and internally"})
                    .formerDefault(3, std::string("both"))
                    .named("Instance")
                    .in("Exposure"),
                ParamSpec{"exposureFalloff", 1.0,
                          "How deep under the hair's outer surface externally reaches (and internally starts), scene "
                          "units, fading over it", "length"}
                    .atLeast(0)
                    .soft(0, 5)
                    .named("Falloff")
                    .onRowOf("exposure")
                    .when("exposure", {"external", "internal"})
                    .in("Exposure"),
                ParamSpec{"axisFacing", false,
                          "Only on the side of the hair facing an axis (the top of the hair for Y+: snow, dust), "
                          "fading over the falloff's depth under it"}
                    .in("Exposure"),
                ParamSpec{"axis", std::string("+y"), "The side they go on: the hair as seen from this end of the axis",
                          "", {"+x", "-x", "+y", "-y", "+z", "-z"}}
                    .choicesNamed({"X+", "X-", "Y+", "Y-", "Z+", "Z-"})
                    .when("axisFacing", {"true"})
                    .in("Exposure"),
                ParamSpec{"axisFalloff", 1.0,
                          "How deep under the hair's surface facing the axis they go, measured along the axis, scene "
                          "units, fading over it", "length"}
                    .atLeast(0)
                    .soft(0, 5)
                    .named("Falloff")
                    .onRowOf("axis")
                    .when("axisFacing", {"true"})
                    .in("Exposure"),
                ParamSpec{"scaleRandom", 0.3, "Each one's size up to this share more or less, at random", "fraction"}
                    .range(0, 1)
                    .in("Random"),
                ParamSpec{"spinRandom", 1.0, "Random turn about each of their three axes, either way, 0-1 (1: up to half a "
                                             "turn either way: any way round). Fitted round their strand (fitWidth) or "
                                             "bent along it (deform): about their long axis only", "fraction"}
                    .range(0, 1)
                    .in("Random"),
            };
        }
        specs.push_back(notAnimatable({"seed", std::int64_t{0}, "Random seed"}).in("Random").adv());
        return specs;
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        Value out;
        run(ctx, inputs, nullptr, &out, nullptr);
        return out;
    }
    // The strands pass through: with constant maps a strand's instances come from its ID alone
    // (the same IDs, the input's instances the same), so they are the last result's, and the
    // input's changes are the result's. A map may read what a dab changed (a channel, the shape):
    // in full.
    bool evaluateIncremental(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        return run(ctx, inputs, &incremental, out, changed);
    }

    // Masked: the mixed result the graph keeps is what the next run splices into, when it holds
    // the instances made.
    void resultMixed(const StrandsPtr& made, const StrandsPtr& mixed) const override { carried(made, mixed); }

private:
    bool scales_;  // a Scales node (else an Instance)
    // The last instances made: a full evaluation making the same again hands those on (the same
    // set, so the graph sees the strands' instances stayed and which strands changed:
    // strandChangesBetween).
    mutable std::mutex keptMutex_;
    mutable std::shared_ptr<const InstanceSet> kept_;
    mutable std::uint64_t keptHash_ = 0;
    mutable bool keptHashKnown_ = true;  // (a splice making another set leaves it to the next full run)
    // The arrays before last (their pages kept: a splice writes them again rather than faulting new
    // ones in), taken when nothing else holds them.
    mutable std::shared_ptr<const InstanceSet> spareSet_;
    mutable std::shared_ptr<const std::vector<std::size_t>> spareFirst_;
    // The last result made with no instances coming in, and per strand where its own begin in its
    // instances (one more: the end): a dab draws again only the strands it changed. (keptMutex_.)
    mutable std::weak_ptr<const StrandSet> keptOutput_;
    mutable std::shared_ptr<const std::vector<std::size_t>> keptFirst_;
    LongestKept longest_;  // the longest strand its maps read for the last result
    // A strand's maps and how many instances it gets.
    struct StrandDraw {
        float chance = 0.0f, size = 0.0f, ratio = 1.0f, variant = 0.0f;
        std::size_t count = 0;
        std::uint64_t kept = 0;  // which of the node's `count` tries are kept (bit k)
    };

    // `made`'s place taken by `next`, a result holding its very instances (the same strands: its
    // longest the same), as what the next run splices into.
    void carried(const StrandsPtr& made, const StrandsPtr& next) const {
        longest_.mixed(made, next);
        const std::lock_guard<std::mutex> lock(keptMutex_);
        if (keptOutput_.lock() == made) keptOutput_ = next && next->instances() == made->instances() ? next : StrandsPtr();
    }

    // In full, or (with `incremental`) the input with the last result's instances; false when
    // that cannot be done.
    bool run(const EvalContext& ctx, const std::vector<Value>& inputs, const IncrementalInputs* incremental, Value* result,
             StrandChanges* changed) const {
        const char* typeLabel = scales_ ? op::kScales : op::kInstance;
        const StrandsPtr hair = requireStrands(inputs, 0, typeLabel);
        // Placed along other strands (a Braid's ties): those are what the instances sit on and are
        // carried as anchors; the hair passes through with them. (In full every time.)
        const StrandsPtr along = !scales_ && inputs.size() > 1 ? valueAs<StrandsPtr>(inputs[1]) : nullptr;
        if (along && incremental) return false;
        const StrandsPtr in = along ? along : hair;
        const ScalpMesh* scalp = valueAs<MeshPtr>(ctx.external("scalp")).get();
        const bool scales = scales_;
        const std::string sizeMode = scales ? std::string("fixed") : params().getString("sizeMode");
        if (sizeMode != "fixed" && sizeMode != "fitStrand") throw GraphError("Instance: sizeMode must be fixed or fitStrand");
        const bool fitStrand = !scales && sizeMode == "fitStrand";
        // Fitted and scales: a fraction of the rest length (fitScale, size) and a ratio (aspect); fixed: a size.
        const bool fitted = fitStrand || scales;
        // Scales: one on every strand (the field is not read).
        const ScalarField probability(params(), scales ? "size" : "probability", scalp);
        const ScalarField scale(params(), scales ? "size" : fitted ? "fitScale" : "scale", scalp);
        const ScalarField aspect(params(), "aspect", scalp);
        const std::string variantBy = params().getString("variantBy");
        if (variantBy != "random" && variantBy != "map") throw GraphError(std::string(typeLabel) + ": variantBy must be random or map");
        const std::vector<std::string> names = instancePrototypeNames(params().getString("prototype"));
        const bool byMap = variantBy == "map" && names.size() > 1;
        const ScalarField variant(params(), "variant", scalp);
        if (!scales) probability.validate(*in);
        scale.validate(*in);
        if (fitted) aspect.validate(*in);
        if (byMap) variant.validate(*in);
        if (incremental && !incremental->inputs.empty() && incremental->inputs[0] && incremental->inputs[0]->pointsKept) {
            // Only widths or channels changed (a Width or HairColor edit before it): where its maps
            // read neither (constants, textures, the strands' shape), each instance is where it was.
            const StrandsPtr last = previousStrandsFor(*incremental, *in);
            if (last && !(!scales && probability.readsAttributes()) && !scale.readsAttributes() &&
                !(fitted && aspect.readsAttributes()) && !(byMap && variant.readsAttributes())) {
                auto out = std::make_shared<StrandSet>(*in);
                out->setInstances(last->instances());
                carried(last, out);
                *result = StrandsPtr(out);
                *changed = *incremental->inputs[0];
                return true;
            }
        }
        // Where in the hair (Instance's): by the whole hair's volume, so never in part.
        const std::string exposure = scales ? std::string("both") : params().getString("exposure");
        if (exposure != "both" && exposure != "external" && exposure != "internal") {
            throw GraphError("Instance: exposure must be both, external or internal");
        }
        const bool axisFacing = !scales && params().getBool("axisFacing");
        int axis = -1, axisSign = 1;
        if (axisFacing) {
            const std::string name = params().getString("axis");
            if (name.size() != 2 || (name[0] != '+' && name[0] != '-') || name[1] < 'x' || name[1] > 'z') {
                throw GraphError("Instance: axis must be +x, -x, +y, -y, +z or -z");
            }
            axisSign = name[0] == '+' ? 1 : -1;
            axis = name[1] - 'x';
        }
        const bool exposed = exposure != "both" || axisFacing;
        if (exposed && incremental) return false;
        // The fields read (one read twice for one that is not).
        const ScalarField& probabilityRead = scales ? scale : probability;
        const ScalarField& aspectRead = fitted ? aspect : scale;
        const ScalarField& variantRead = byMap ? variant : scale;
        // Incrementally with maps that read each strand alone (or with the longest strand, the
        // same as for the last result): the strands the input changed in drawn again (spliced).
        const StrandChanges* spliceChanges = nullptr;
        StrandsPtr spliceLast;
        std::shared_ptr<const std::vector<std::size_t>> spliceFirst;
        bool spliced = false;
        if (incremental) {
            const StrandChanges* changes = someStrandsChanged(*incremental, 0);
            const StrandsPtr last = changes ? previousStrandsFor(*incremental, *in) : nullptr;
            auto constant = [](const ScalarField& f) { return f.isConstant() && f.strandLocal(); };
            if (!last) return false;
            if ((!scales && !constant(probability)) || !constant(scale) || (fitted && !constant(aspect)) ||
                (byMap && !constant(variant))) {
                {
                    const std::lock_guard<std::mutex> lock(keptMutex_);
                    if (keptOutput_.lock() == last) spliceFirst = keptFirst_;
                }
                if (!spliceFirst || spliceFirst->size() != in->strandCount() + 1 || in->instances() || !last->instances() ||
                    !longest_.local(last, {&probabilityRead, &scale, &aspectRead, &variantRead})) {
                    return false;
                }
                spliceChanges = changes;
                spliceLast = last;
                spliced = true;
            }
        }
        if (incremental && !spliced) {
            const StrandChanges* changes = someStrandsChanged(*incremental, 0);
            const StrandsPtr last = previousStrandsFor(*incremental, *in);
            if (changes->none()) {
                *result = last;
            } else {
                auto out = std::make_shared<StrandSet>(*in);
                out->setInstances(last->instances());
                *result = StrandsPtr(out);
            }
            *changed = *changes;
            return true;
        }
        const auto count = scales ? std::size_t{1} : static_cast<std::size_t>(params().getInt("count"));
        const float tMin = scales ? 0.0f : static_cast<float>(params().getDouble("tMin"));
        const float tMax = scales ? 0.0f : std::max(static_cast<float>(params().getDouble("tMax")), tMin);  // never before tMin
        const float scaleRandom = static_cast<float>(params().getDouble("scaleRandom"));
        const float spinRandom = static_cast<float>(params().getDouble("spinRandom"));
        const float offset = static_cast<float>(params().getDouble("offset"));
        const auto seed = static_cast<std::uint64_t>(params().getInt("seed"));
        std::unique_ptr<HairVolume> volume;
        float exposureFalloff = 0.0f, axisFalloff = 0.0f;
        if (exposed) {
            volume = std::make_unique<HairVolume>(*hair, exposure != "both", axis, axisSign);
            exposureFalloff = static_cast<float>(params().getDouble("exposureFalloff"));
            axisFalloff = static_cast<float>(params().getDouble("axisFalloff"));
        }
        // How much of its chance an instance at `t` along strand `s` keeps (all of it unless exposed).
        auto exposureAt = [&](std::size_t s, float t) {
            if (!volume || volume->empty()) return 1.0f;
            const Vec3f p = pointAlong(*in, s, t);
            float w = 1.0f;
            if (exposure == "external") w *= within(volume->depth(p), exposureFalloff);
            if (exposure == "internal") w *= 1.0f - within(volume->depth(p), exposureFalloff);
            if (axisFacing) w *= within(volume->depthAlong(p), axisFalloff);
            return w;
        };
        // Instance's own.
        InstanceOrient orient = InstanceOrient::Strand;
        bool scaleByWidth = false, pinToRest = false, deformed = false, fitWidth = false;
        std::uint8_t axisCode = 1;
        // Scales' own.
        std::uint8_t previewCode = 0;
        float lift = 0.0f, tile = 0.0f, stack = 0.0f;
        bool conform = false, followStrand = false, aimRoot = false, showStrands = false;
        std::string material;
        if (scales) {
            const std::string preview = params().getString("preview");
            if (preview != "auto" && preview != "meshes" && preview != "quads") {
                throw GraphError("Scales: preview must be auto, meshes or quads");
            }
            previewCode = static_cast<std::uint8_t>(preview == "auto" ? 0 : preview == "meshes" ? 1 : 2);
            lift = static_cast<float>(params().getDouble("lift")) * kDegrees;
            tile = static_cast<float>(params().getDouble("tile"));
            stack = static_cast<float>(params().getDouble("stack"));
            conform = params().getBool("conform");
            const std::string follow = params().getString("follow");
            if (follow != "skin" && follow != "strand") throw GraphError("Scales: follow must be skin or strand");
            followStrand = follow == "strand";
            const std::string aim = params().getString("aim");
            if (aim != "tip" && aim != "root") throw GraphError("Scales: aim must be tip or root");
            aimRoot = aim == "root";
            material = params().getString("material");
            showStrands = params().getBool("showStrands");
        } else {
            orient = orientFrom(params().getString("orient"));
            scaleByWidth = params().getBool("scaleByWidth");
            pinToRest = params().getBool("pinToRest");
            const std::string deform = params().getString("deform");
            if (deform != "none" && deform != "alongStrand") throw GraphError("Instance: deform must be none or alongStrand");
            const std::string deformAxis = params().getString("deformAxis");
            if (deformAxis != "x" && deformAxis != "y" && deformAxis != "z") throw GraphError("Instance: deformAxis must be x, y or z");
            deformed = deform == "alongStrand";
            fitWidth = fitStrand && params().getBool("fitWidth");
            axisCode = static_cast<std::uint8_t>(deformAxis == "x" ? 0 : deformAxis == "y" ? 1 : 2);
        }

        auto instances = std::make_shared<InstanceSet>();
        if (const auto& existing = hair->instances()) *instances = *existing;
        if (along && !along->empty()) {
            InstanceSet carriers;
            carriers.anchors = std::make_shared<StrandSet>(*along);
            instances->append(carriers);
        }
        if (scales && !showStrands) instances->hidesStrands = true;
        std::vector<std::uint32_t> prototypes;
        for (const std::string& name : names) prototypes.push_back(instances->prototypeIndex(name));
        const std::int32_t materialIndex = scales && !material.empty() ? static_cast<std::int32_t>(instances->materialIndex(material)) : -1;
        const std::size_t strands = in->strandCount();
        auto instanceId = [&](std::size_t s, std::size_t k) {
            return hashCombine(hashCombine(in->ids()[s], seed ^ 0x1257A9CEull), k);
        };
        // A strand's maps and how many instances it gets, then its instances written from `at` on.
        auto drawOf = [&](std::size_t s) {
            StrandDraw d;
            d.chance = scales ? 1.0f : probability.at(*in, s);
            if (d.chance <= 0.0f) return d;
            d.size = scale.at(*in, s);
            d.ratio = fitted ? std::max(aspect.at(*in, s), 0.0f) : 1.0f;
            d.variant = byMap ? variant.at(*in, s) : 0.0f;
            for (std::size_t k = 0; k < count; ++k) {
                const std::uint64_t id = instanceId(s, k);
                bool keep = scales || unitFloat(hash64(id)) < d.chance;
                if (keep && volume) {
                    Rng rng(id);  // its t: the first draw (write)
                    keep = unitFloat(hash64(id)) < d.chance * exposureAt(s, tMin + (tMax - tMin) * rng.nextFloat());
                }
                if (keep) {
                    ++d.count;
                    d.kept |= std::uint64_t{1} << k;
                }
            }
            return d;
        };
        const std::size_t variantCount = prototypes.size();
        auto write = [&](std::size_t s, const StrandDraw& d, InstanceSet::Instance* at) {
            for (std::size_t k = 0; k < count; ++k) {
                const std::uint64_t id = instanceId(s, k);
                if (!(d.kept >> k & 1u)) continue;
                Rng rng(id);
                InstanceSet::Instance& inst = *at++;  // default-made by the resize
                // The variant: by the map over the list, else by the ID (its own hash: the draws
                // below stay as they were for one prototype).
                std::size_t which = 0;
                if (variantCount > 1) {
                    const float pick = byMap ? std::clamp(d.variant, 0.0f, 1.0f) : unitFloat(hash64(id ^ 0x7A21A47ull));
                    which = std::min(static_cast<std::size_t>(pick * static_cast<float>(variantCount)), variantCount - 1);
                }
                inst.prototype = prototypes[which];
                inst.variant = static_cast<std::uint16_t>(which);
                inst.strand = in->ids()[s];
                inst.id = id;
                if (scales) {
                    const StrandRoot& root = in->roots()[s];
                    inst.onSkin = true;
                    inst.t = 0.0f;
                    inst.spin = 0.5f * kTwoPi * spinRandom * (2.0f * rng.nextFloat() - 1.0f);
                    inst.scale = d.size * (1.0f + scaleRandom * (2.0f * rng.nextFloat() - 1.0f));
                    inst.fitStrand = false;
                    inst.aspect = d.ratio;
                    inst.offset = offset;
                    inst.scaleByWidth = false;
                    inst.conform = conform;
                    inst.preview = previewCode;
                    inst.material = materialIndex;
                    inst.lift = lift;
                    inst.tile = tile;
                    inst.stack = stack;
                    inst.followStrand = followStrand;
                    inst.aimRoot = aimRoot;
                    // The normal the node sees, for a host placing it without the scalp.
                    inst.direction = scalp && scalp->contains(root) ? scalp->normal(root) : Vec3f{0, 1, 0};
                    continue;
                }
                inst.t = tMin + (tMax - tMin) * rng.nextFloat();
                inst.orient = orient;
                inst.spin = 0.5f * kTwoPi * spinRandom * (2.0f * rng.nextFloat() - 1.0f);  // either way, as Scales
                inst.scale = d.size * (1.0f + scaleRandom * (2.0f * rng.nextFloat() - 1.0f));
                inst.offset = offset;
                inst.scaleByWidth = scaleByWidth;
                inst.pinToRest = pinToRest;
                inst.fitStrand = fitStrand;
                inst.fitWidth = fitWidth;
                inst.aspect = d.ratio;
                if (deformed) {
                    inst.deform = true;
                    inst.deformAxis = axisCode;
                }
                if (orient == InstanceOrient::Random) {
                    // Uniform on the sphere.
                    const float z = 2.0f * rng.nextFloat() - 1.0f;
                    const float a = kTwoPi * rng.nextFloat();
                    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
                    inst.direction = {r * pm::cos(a), r * pm::sin(a), z};
                }
                if (!deformed && !fitWidth && spinRandom > 0.0f) {
                    // About its other two axes as far as about its own (drawn last: the draws above
                    // stay as they were). Not round its strand (fitWidth) or bent along it (deform).
                    inst.tiltX = 0.5f * kTwoPi * spinRandom * (2.0f * rng.nextFloat() - 1.0f);
                    inst.tiltZ = 0.5f * kTwoPi * spinRandom * (2.0f * rng.nextFloat() - 1.0f);
                }
            }
        };
        // Per strand in parallel: its draw; then where its instances go (after the existing ones, in
        // strand order) and each written in place. Incrementally (the input's instances none, each
        // strand's maps its own, the last result's made here): the strands the input changed in drawn
        // again, the others' instances the last result's.
        std::shared_ptr<const InstanceSet> made;
        std::shared_ptr<const std::vector<std::size_t>> firstMade;
        const std::vector<std::uint32_t>* redrawn = nullptr;
        if (spliced) {
            const std::vector<std::uint32_t>& again = spliceChanges->strands;
            redrawn = &again;
            std::vector<StrandDraw> draws(again.size());
            parallelFor(0, again.size(), [&](std::size_t k) { draws[k] = drawOf(again[k]); }, 256);
            const std::vector<std::size_t>& before = *spliceFirst;
            const std::vector<InstanceSet::Instance>& was = spliceLast->instances()->instances;
            // Theirs on their own first: the same as the last result's (a dab seldom tips a strand's
            // chance past one of its draws), the last instances stay.
            std::vector<std::size_t> at(again.size() + 1, 0);
            for (std::size_t k = 0; k < again.size(); ++k) at[k + 1] = at[k] + draws[k].count;
            std::vector<InstanceSet::Instance> fresh(at.back());
            std::atomic<bool> differ{false};
            parallelFor(0, again.size(), [&](std::size_t k) {
                const std::size_t s = again[k];
                if (!draws[k].count) {
                    if (before[s + 1] != before[s]) differ = true;
                    return;
                }
                write(s, draws[k], fresh.data() + at[k]);
                if (before[s + 1] - before[s] != draws[k].count) {
                    differ = true;
                    return;
                }
                for (std::size_t i = 0; i < draws[k].count; ++i) {
                    if (InstanceSet::hashInstance(0, fresh[at[k] + i]) != InstanceSet::hashInstance(0, was[before[s] + i])) {
                        differ = true;
                        return;
                    }
                }
            }, 256);
            if (!differ) {
                made = spliceLast->instances();
                firstMade = spliceFirst;
            } else {
                std::shared_ptr<std::vector<std::size_t>> first;
                {
                    const std::lock_guard<std::mutex> lock(keptMutex_);
                    if (spareSet_ && spareSet_.use_count() == 1) {
                        instances->instances = std::move(const_cast<InstanceSet&>(*spareSet_).instances);
                    }
                    if (spareFirst_ && spareFirst_.use_count() == 1) {
                        first = std::const_pointer_cast<std::vector<std::size_t>>(std::move(spareFirst_));
                    }
                    spareSet_.reset();
                    spareFirst_.reset();
                }
                if (!first) first = std::make_shared<std::vector<std::size_t>>();
                first->resize(strands + 1);
                (*first)[0] = 0;
                for (std::size_t s = 0; s < strands; ++s) (*first)[s + 1] = before[s + 1] - before[s];
                for (std::size_t k = 0; k < again.size(); ++k) (*first)[again[k] + 1] = draws[k].count;
                for (std::size_t s = 0; s < strands; ++s) (*first)[s + 1] += (*first)[s];
                instances->instances.resize((*first)[strands]);  // (every one written below)
                // The runs of strands left alone, copied side by side; then the strands drawn again.
                std::vector<std::pair<std::size_t, std::size_t>> runs;  // [from, to) strands
                std::size_t from = 0;
                for (const std::uint32_t c : again) {
                    if (c > from) runs.emplace_back(from, c);
                    from = c + 1;
                }
                if (from < strands) runs.emplace_back(from, strands);
                parallelFor(0, runs.size(), [&](std::size_t r) {
                    const auto [lo, hi] = runs[r];
                    std::copy(was.begin() + static_cast<std::ptrdiff_t>(before[lo]), was.begin() + static_cast<std::ptrdiff_t>(before[hi]),
                              instances->instances.begin() + static_cast<std::ptrdiff_t>((*first)[lo]));
                }, 16);
                parallelFor(0, again.size(), [&](std::size_t k) {
                    std::copy(fresh.begin() + static_cast<std::ptrdiff_t>(at[k]), fresh.begin() + static_cast<std::ptrdiff_t>(at[k + 1]),
                              instances->instances.begin() + static_cast<std::ptrdiff_t>((*first)[again[k]]));
                }, 256);
                firstMade = std::move(first);
                // Another set than the last (its hash left to the next full run that asks).
                made = std::move(instances);
                const std::lock_guard<std::mutex> lock(keptMutex_);
                kept_ = made;
                keptHashKnown_ = false;
            }
        } else {
            auto first = std::make_shared<std::vector<std::size_t>>(strands + 1, 0);
            std::vector<StrandDraw> draws(strands);
            parallelFor(0, strands, [&](std::size_t s) {
                draws[s] = drawOf(s);
                (*first)[s + 1] = draws[s].count;
            });
            (*first)[0] = instances->instances.size();
            for (std::size_t s = 0; s < strands; ++s) (*first)[s + 1] += (*first)[s];
            instances->instances.resize((*first)[strands]);
            parallelFor(0, strands, [&](std::size_t s) {
                if (draws[s].count) write(s, draws[s], instances->instances.data() + (*first)[s]);
            });
            firstMade = std::move(first);
        }
        if (!made) {
            made = std::move(instances);
            const std::uint64_t hash = made->contentHash();
            const std::lock_guard<std::mutex> lock(keptMutex_);
            if (kept_ && !keptHashKnown_) {
                keptHash_ = kept_->contentHash();
                keptHashKnown_ = true;
            }
            if (kept_ && keptHash_ == hash && kept_->size() == made->size()) {
                made = kept_;
            } else {
                kept_ = made;
                keptHash_ = hash;
            }
        }
        auto out = std::make_shared<StrandSet>(*hair);
        if (redrawn) {
            *changed = StrandChanges::listed(*redrawn);
            changed->instancesChanged = made != spliceLast->instances();
        }
        out->setInstances(std::move(made));
        const StrandsPtr output = out;
        {
            const std::lock_guard<std::mutex> lock(keptMutex_);
            keptOutput_ = in->instances() || along ? StrandsPtr() : output;
            if (keptFirst_ != firstMade) spareFirst_ = std::move(keptFirst_);
            keptFirst_ = std::move(firstMade);
            if (spliceLast && spliceLast->instances() != output->instances()) spareSet_ = spliceLast->instances();
        }
        longest_.made(output, {&probabilityRead, &scale, &aspectRead, &variantRead});
        *result = output;
        return true;
    }
};

}  // namespace

void registerInstanceOperator(OperatorRegistry& registry) {
    registry.registerType(op::kInstance,
                          {"look", "Copies a mesh (leaves, feathers, beads) along the strands.", "copy mesh leaves beads"},
                          [] { return std::make_unique<InstanceOp>(false); });
}

void registerScalesOperator(OperatorRegistry& registry) {
    registry.registerType(op::kScales,
                          {"look",
                           "A scale at each strand's root, lying on the skin along the strand: reptile skin, armor plates.",
                           "reptile armor plates skin"},
                          [] { return std::make_unique<InstanceOp>(true); });
}

}  // namespace groomlab
