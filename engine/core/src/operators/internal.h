// SPDX-License-Identifier: Apache-2.0
#pragma once

// Registration hooks for built-in operators, one per type (builtin_operators.inc), and the
// helpers they share.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/parallel.h"
#include "groomlab/portable_math.h"
#include "groomlab/preview_core.h"
#include "groomlab/random.h"
#include "groomlab/ramp.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab {

// Position along a strand of n points: 0 at the root, 1 at the tip.
inline float paramAt(std::size_t i, std::size_t n) {
    return n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f;
}

// The "scalp" external (maps sample textures and color sets on it); null without one.
inline const ScalpMesh* scalpOf(const EvalContext& ctx) { return valueAs<MeshPtr>(ctx.external("scalp")).get(); }

// What reading color set `name` of the "scalp" external adds to a node's cache key: the set's
// hash (whether the scalp has it, and its values). The scalp's own key is its surface alone
// (ScalpMesh::surfaceHash), so every reader of a color set salts its key with this. 0 for no name.
inline std::uint64_t colorSetSalt(const EvalContext& ctx, const std::string& name) {
    if (name.empty()) return 0;
    const ScalpMesh* scalp = scalpOf(ctx);
    return scalp ? scalp->colorSetHash(name) : 0;
}

// The painted map `name` in the host's catalog (the "maps" external; null without one, or
// without the name).
inline const MapInfo* catalogMap(const EvalContext& ctx, const std::string& name) {
    if (name.empty()) return nullptr;
    const MapCatalogPtr catalog = valueAs<MapCatalogPtr>(ctx.external("maps"));
    if (!catalog) return nullptr;
    const auto it = catalog->maps.find(name);
    return it == catalog->maps.end() ? nullptr : &it->second;
}

// The strand channel `channel` numbering each strand's region (Interpolate's regionMap writes it:
// its regionChannel), for the nodes that keep within regions (Clump, BlendNeighbors, Tangle):
// null for no name, or when the strands lack it.
inline const std::vector<float>* regionsOf(const StrandSet& strands, const std::string& channel) {
    if (channel.empty()) return nullptr;
    const std::vector<float>* regions = strands.strandChannel(channel);
    return regions && regions->size() == strands.strandCount() ? regions : nullptr;
}

// The strands on input `slot`; throws GraphError when it is not connected.
inline StrandsPtr requireStrands(const std::vector<Value>& inputs, std::size_t slot, const char* opName) {
    StrandsPtr s = slot < inputs.size() ? valueAs<StrandsPtr>(inputs[slot]) : nullptr;
    if (!s) {
        throw GraphError(std::string(opName) + (slot == 0 ? ": strands input not connected"
                                                          : ": input " + std::to_string(slot) + " is not connected"));
    }
    return s;
}
inline StrandsPtr requireStrands(const std::vector<Value>& inputs, const char* opName) {
    return requireStrands(inputs, 0, opName);
}

// A rotation as a unit quaternion (w, v): Gravity and Static carry the turns of the segments above
// down the strand with it.
struct Turn {
    float w = 1.0f;
    Vec3f v;
    static Turn about(const Vec3f& unitAxis, float angle) {
        Turn t;
        t.w = pm::cos(0.5f * angle);
        t.v = unitAxis * pm::sin(0.5f * angle);
        return t;
    }
    Vec3f operator()(const Vec3f& p) const {  // p turned: p + 2w (v x p) + 2 v x (v x p)
        const Vec3f c = v.cross(p);
        return p + c * (2.0f * w) + v.cross(c) * 2.0f;
    }
    Turn then(const Turn& next) const {  // this turn, then `next`
        Turn t;
        t.w = next.w * w - next.v.dot(v);
        t.v = v * next.w + next.v * w + next.v.cross(v);
        const float len = std::sqrt(t.w * t.w + t.v.dot(t.v));  // kept a unit quaternion
        t.w /= len;
        t.v = t.v / len;
        return t;
    }
};

// A ramp along the length, sampled once per point index when every strand has
// the same point count (the usual case), so per-point loops skip the curve
// evaluation; strands of other counts evaluate it directly.
class RampAlong {
public:
    RampAlong(const Ramp& ramp, const StrandSet& strands) : ramp_(ramp), n_(strands.uniformPointCount()) {
        table_.resize(n_);
        for (std::size_t i = 0; i < n_; ++i) table_[i] = ramp_(paramAt(i, n_));
    }
    float operator()(std::size_t i, std::size_t n) const { return n == n_ ? table_[i] : ramp_(paramAt(i, n)); }

private:
    Ramp ramp_;
    std::size_t n_;
    std::vector<float> table_;
};

// Guides as strands (graph.cpp, as a Curves node gets them): IDs, roots, points, painted maps
// as strand channels and per-point layers as point channels. For nodes taking either kind on
// an input whose output is not Curves (Propagate's shafts).
StrandsPtr guidesAsStrands(const GuideSet& guides);
// Strands as guides (graph.cpp, as a Curves node hands guides back): IDs, roots, points, strand
// channels as attributes and point channels as per-point layers. For an input taking either kind
// whose operator reads guides (Clump's centers). `coreOnly`: only the preview core's strands
// (preview_core.h), the same at every preview density. The roles' channel never comes along.
// `channels`: only those strand channels (those the strands have) and no point layers.
GuidesPtr strandsAsGuides(const StrandSet& strands, bool coreOnly = false, const std::vector<std::string>* channels = nullptr);

// Numbers b's groups after a's highest (look_ops.cpp): its clumpId, braidId and featherId (each
// channel both have) offset by groupNumberBase of the largest of a's core strands (strand_set.h,
// preview_core.h: the same at every preview density), so the two sets'
// groups stay apart when b joins a (Merge, Mirror's mirrored copy); -1 (no group) stays.
void offsetGroupIds(const StrandSet& a, StrandSet& b);

// Numbers for groups named by keys (look_ops.cpp; Clump's clumpId from its centers' IDs,
// Propagate's featherId from its shafts', Braid's braidId from its own paths'): each from its
// key, hashed into 0 .. kGroupNumbers - 1 (strand_set.h), the next free one on a clash (keys
// taken by that hash, then key, then place). A group keeps its number whatever the others are
// (unless a rare clash ties it to one), so an edit of the scalp in one place renumbers no group
// elsewhere, as a count would.
std::vector<float> stableGroupNumbers(const std::vector<std::uint64_t>& keys);

class OperatorRegistry;

// Each built-in type's registration (builtin_operators.inc): register<Type>Operator, defined in
// the type's source file next to its operator, called by registerBuiltinOperators (registry.cpp).
#define GROOMLAB_OPERATOR(Type) void register##Type##Operator(OperatorRegistry& registry);
#include "groomlab/builtin_operators.inc"
#undef GROOMLAB_OPERATOR

// Compound definitions (graph_io.cpp, which owns the JSON): the inner graph
// as graph JSON, the declared inputs, and exposed name -> "node.param".
// Throw GraphError on a malformed definition.
std::string compoundGraphJson(const std::string& definition);
std::vector<PortSpec> compoundInputs(const std::string& definition);
std::map<std::string, std::string> compoundExposed(const std::string& definition);

// A node of type "node:<id>" in a graph's JSON as the Compound it stands for (node_library.cpp):
// its params (JSON text) with the library's copy as "definition" when it has none, else a copy
// that only names the id (the node then hands its strands on, saying why).
// `type` (when given): the type the node is, "Script" for a script node's copy, else "Compound".
std::string libraryNodeStubParams(const std::string& id, const std::string& paramsJson, std::string* type = nullptr);

// What a node's map params (PG-2) add to its cache key (graph.cpp): each
// texture's file stamp, each color set's hash (colorSetSalt), and the scalp
// they are sampled on. Compounds add their inner nodes' the same way.
std::uint64_t mapParamsKey(const ParamSet& params, const EvalContext& ctx);

class ScalarField;
// The node's result `out` mixed back toward its input `in` by the affect field, strand by strand
// (point by point for a per-point field), clamped to 0-1: points, widths and channels mixed, and
// ID channels (names ending in "Id": clumpId, braidId...) the result's where it is 0.5 or more,
// the input's below. 1: the result's values exactly; 0: the input's. Arrays the two share are left
// shared. Null when `out` does not hold the same strands as `in` (count, IDs, points per strand).
StrandsPtr applyAffect(const StrandSet& in, const StrandsPtr& out, const ScalarField& affect);

// Every strand resampled by arc length to points[s] points, widths and per-point channels
// following; IDs, roots and per-strand channels stay (shape_ops.cpp: Resample's).
StrandsPtr resampleStrands(const StrandSet& source, const std::vector<std::size_t>& points, bool byCurvature = false);
// Strand s of `source` resampled into `out`'s points for it (as many as `out` has; evenly by arc
// length, or closer together where it bends with `byCurvature`), widths (`widths`: `source` has
// them) and point channels (`inPoint` into `outPoint`) following: what resampleStrands does for each.
void resampleStrand(const StrandSet& source, StrandSet& out, std::size_t s, bool byCurvature, bool widths,
                    const std::vector<const std::vector<float>*>& inPoint, const std::vector<std::vector<float>*>& outPoint,
                    std::vector<float>& outWidths);

// Points binned in a world-aligned grid of cubes `cell` wide (at least the farthest a query
// reaches): a query visits the points of a point's cell and its 26 neighbours. What a query finds
// within its reach is the same whatever the cell, and depends only on the points near it (an
// operator using it stays local: Clump's center pick, Scatter's thinned preview).
class CellGrid {
public:
    CellGrid(const std::vector<Vec3f>& points, float cell) : cell_(std::max(cell, 1e-6f)) {
        std::vector<std::pair<std::uint64_t, std::uint32_t>> keyed(points.size());
        parallelFor(0, points.size(), [&](std::size_t i) { keyed[i] = {keyOf(points[i]), static_cast<std::uint32_t>(i)}; }, 4096);
        std::sort(keyed.begin(), keyed.end());
        members_.resize(keyed.size());
        for (std::size_t i = 0; i < keyed.size(); ++i) {
            members_[i] = keyed[i].second;
            auto& range = ranges_[keyed[i].first];
            if (i == 0 || keyed[i].first != keyed[i - 1].first) range.first = static_cast<std::uint32_t>(i);
            range.second = static_cast<std::uint32_t>(i + 1);
        }
    }
    // The key of p's cell, and of the cell (dx, dy, dz) from it.
    std::uint64_t keyOf(const Vec3f& p, std::int64_t dx = 0, std::int64_t dy = 0, std::int64_t dz = 0) const {
        return cellKey(static_cast<std::int64_t>(std::floor(p.x / cell_)) + dx, static_cast<std::int64_t>(std::floor(p.y / cell_)) + dy,
                       static_cast<std::int64_t>(std::floor(p.z / cell_)) + dz);
    }
    static std::uint64_t cellKey(std::int64_t x, std::int64_t y, std::int64_t z) {
        return hashCombine(hashCombine(hash64(static_cast<std::uint64_t>(x)), static_cast<std::uint64_t>(y)), static_cast<std::uint64_t>(z));
    }
    // Calls visit(i) for the points of p's cell and its neighbours (each cell's in index order)
    // until it returns false.
    template <typename Visit>
    void around(const Vec3f& p, Visit visit) const {
        for (std::int64_t dx = -1; dx <= 1; ++dx) {
            for (std::int64_t dy = -1; dy <= 1; ++dy) {
                for (std::int64_t dz = -1; dz <= 1; ++dz) {
                    const auto it = ranges_.find(keyOf(p, dx, dy, dz));
                    if (it == ranges_.end()) continue;
                    for (std::uint32_t k = it->second.first; k < it->second.second; ++k) {
                        if (!visit(members_[k])) return;
                    }
                }
            }
        }
    }

private:
    float cell_;
    std::vector<std::uint32_t> members_;
    std::unordered_map<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>> ranges_;
};

// Points added one at a time in a grid of cubes `cell` wide (at least the reach of a query):
// whether one added lies within `reach` of p.
class GrowingCellGrid {
public:
    explicit GrowingCellGrid(float cell) : cell_(std::max(cell, 1e-6f)) {}
    std::uint64_t keyOf(const Vec3f& p, std::int64_t dx = 0, std::int64_t dy = 0, std::int64_t dz = 0) const {
        return CellGrid::cellKey(static_cast<std::int64_t>(std::floor(p.x / cell_)) + dx,
                                 static_cast<std::int64_t>(std::floor(p.y / cell_)) + dy,
                                 static_cast<std::int64_t>(std::floor(p.z / cell_)) + dz);
    }
    void add(const Vec3f& p, std::uint32_t index) { cells_[keyOf(p)].push_back({p, index}); }
    // Calls visit(point, index) for those added in p's cell and its neighbours until it returns false.
    template <typename Visit>
    void around(const Vec3f& p, Visit visit) const {
        for (std::int64_t dx = -1; dx <= 1; ++dx) {
            for (std::int64_t dy = -1; dy <= 1; ++dy) {
                for (std::int64_t dz = -1; dz <= 1; ++dz) {
                    const auto it = cells_.find(keyOf(p, dx, dy, dz));
                    if (it == cells_.end()) continue;
                    for (const auto& [q, index] : it->second) {
                        if (!visit(q, index)) return;
                    }
                }
            }
        }
    }

private:
    float cell_;
    std::unordered_map<std::uint64_t, std::vector<std::pair<Vec3f, std::uint32_t>>> cells_;
};

// A positive value rounded to a step of about 1% (230 steps a decade: roundToDecadeStep, math.h):
// for one an operator takes from all the strands or the whole scalp (Clump's spacing by count,
// Resample's reference length, Bands' guard length), so that an edit changing a few strands, or
// the scalp's area a little, keeps it, and with it the result everywhere else; a scene in other
// units (a power of ten bigger or smaller) rounds alike. Not positive or not finite: as it is.
// `scale`: the groom's (EvalContext::scale): the lattice anchored at it, so a groom scaled with its
// scalp rounds as at 1x, scaled.
inline float roundToPercentStep(float value, double scale = 1.0) {
    if (scale == 1.0 || !(scale > 0.0) || !std::isfinite(scale)) return roundToDecadeStep(value, 230);
    return static_cast<float>(roundToDecadeStep(static_cast<float>(value / scale), 230) * scale);
}

// For an operator making again only the strands its input changed in (Operator::evaluateIncremental)
// whose fields may read `longest` (the longest strand's length: an expression of $length / $longest):
// the longest its last result was made with. With the longest the same, each strand's value is still
// the strand's own, and a dab that changes some strands needs only theirs made again.
class LongestKept {
public:
    // Whether `fields` (validated on the strands at hand) read each strand alone, or with all they
    // read of the rest `longest` and that what it was for `last` (the node's last result).
    bool local(const StrandsPtr& last, std::initializer_list<const ScalarField*> fields) const {
        const float longest = read(fields);
        if (longest == kNotLocal) return false;
        if (longest < 0.0f) return true;
        const std::lock_guard<std::mutex> lock(mutex_);
        return last && output_.lock() == last && longest_ == longest;
    }
    // `output` made with `fields` (validated on its input).
    void made(const StrandsPtr& output, std::initializer_list<const ScalarField*> fields) const {
        const float longest = read(fields);
        const std::lock_guard<std::mutex> lock(mutex_);
        output_ = longest >= 0.0f ? output : StrandsPtr();
        longest_ = longest;
    }
    // Masked: it answers for the mixed result the graph keeps (Operator::resultMixed).
    void mixed(const StrandsPtr& made, const StrandsPtr& mixed) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (made && output_.lock() == made) output_ = mixed;
    }

private:
    static constexpr float kNotLocal = -2.0f;
    // The longest the fields read (negative: none reads it), or kNotLocal.
    static float read(std::initializer_list<const ScalarField*> fields) {
        float longest = -1.0f;
        for (const ScalarField* f : fields) {
            if (f->strandLocal()) continue;
            const float l = f->longestRead();
            if (l < 0.0f) return kNotLocal;
            longest = std::max(longest, l);
        }
        return longest;
    }
    mutable std::mutex mutex_;
    mutable std::weak_ptr<const StrandSet> output_;
    mutable float longest_ = -1.0f;
};

// Strands of `in` spread over the scalp about `spacing` apart (look_ops.cpp: a Clump's own pick of
// its centers by size, at its randomness default, every core strand allowed: preview_core.h), in
// the order picked: Braid's paths without a braids input.
std::vector<std::size_t> pickSpreadStrands(const StrandSet& in, const ScalpMesh* scalp, float spacing, std::uint64_t seed);

}  // namespace groomlab
