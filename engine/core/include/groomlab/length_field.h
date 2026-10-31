// SPDX-License-Identifier: Apache-2.0
#pragma once

// Length field: the Length Keys tool's keys on the scalp, each a scale for the guides' lengths
// there (x1.5: half as long again; x1: kept as they are). Between keys the scale blends smoothly
// over the whole scalp, by distance along the skin (surface_paths.h: a key on a leg does not reach
// through to the belly), so no key has a reach to set: each one's ends where the others' begin.
// Away from every key the scale eases back to x1 (past the keys' spacing: the largest distance from
// a key to its nearest other), so guides far from the keys stay as they are; a single key scales
// every guide. A key at x1 holds the guides round it. The keys are saved with the guides
// (GuideHierarchy::lengthKeys) to place again; Apply bakes their scales into the guides and sets
// them back to x1, so applying twice does not scale twice.
//
// Scales blend as logarithms (x2 and x0.5 meet at x1), each key weighing 1 / distance^2 (so at a
// key the scale is the key's, level round it).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/math.h"
#include "groomlab/strand_set.h"

namespace groomlab {

class GuideHierarchy;
class ScalpMesh;
class SurfaceFronts;
class SurfaceGraph;
using GuideId = std::uint64_t;  // (guide_hierarchy.h's)

// One key: where it is on the scalp (a root, so it follows the scalp when it moves; face -1: only
// the point is known) and the point as placed, and the scale it gives the guides' lengths there.
struct LengthKey {
    StrandRoot root;
    Vec3f point;
    float scale = 1.0f;
};

struct LengthKeySet {
    std::vector<LengthKey> keys;

    bool empty() const { return keys.empty(); }
    bool scaled() const;  // a key at a scale other than x1 (applying would change a guide)
    // JSON, saved with the guides ("lengthKeys"):
    //   [{"face": 3, "u": 0.2, "v": 0.3, "point": [x, y, z], "scale": 1.5}, ...]
    // Throws std::invalid_argument for text that is not such a list.
    std::string toJson() const;
    static LengthKeySet fromJson(const std::string& text);
};

// How a guide takes its new length: Scale (the whole guide scaled about its root, so a curl or bend
// keeps its shape, larger or smaller) or Trim (cut at the tip, or extended along its last
// direction: curls keep their size).
enum class LengthChange { Scale, Trim };
const char* lengthChangeName(LengthChange change);           // "scale", "trim"
LengthChange lengthChangeFromName(const std::string& name);  // throws std::invalid_argument

// The keys' points on `scalp` (their roots where they are on it, else their points).
Vec3f lengthKeyPosition(const LengthKey& key, const ScalpMesh* scalp);

// The scale the keys give every point of the scalp. Distances run along `scalp`'s skin (its rest
// pose) between points on it; a point on another piece, or without a scalp, measures the straight
// line. The fronts from the keys are kept on the scalp's shared graph, so a field made again with
// one key moved walks the skin from that key only.
class LengthField {
public:
    LengthField(const LengthKeySet& keys, const ScalpMesh* scalp);
    ~LengthField();

    // The scale at a point on (or near) the scalp as it is now: 1 with no keys.
    float scaleAt(const Vec3f& point) const;
    float spacing() const { return spacing_; }  // where the scale starts easing back (infinite: one key)

private:
    struct End;
    End endAt(const Vec3f& point) const;
    float distance(std::size_t key, const End& end) const;

    std::vector<float> logScales_;
    std::vector<Vec3f> points_;  // the keys' points now (straight lines)
    std::vector<Vec3f> rest_;    // and on the rest pose (paths)
    const ScalpMesh* scalp_ = nullptr;
    std::shared_ptr<const SurfaceGraph> graph_;
    std::unique_ptr<SurfaceFronts> fronts_;
    float spacing_ = 0.0f;
};

// The keys mirrored (symmetry): each key's point taken to `mirror(point)` (a world plane's image,
// or topology's partner spot: StrokeSymmetry::point), its root found again on `scalp` (the closest
// point; none: point only).
LengthKeySet mirroredLengthKeys(const LengthKeySet& keys, const ScalpMesh* scalp,
                                const std::function<Vec3f(const Vec3f&)>& mirror);

// The length of the guide of `guides` (primary guides, by root) nearest `point`: what a key there
// at x1 keeps (0: no guides).
float nearestGuideLength(const GuideHierarchy& hierarchy, const std::vector<GuideId>& guides, const Vec3f& point);

// A guide's points at a new length (`change`), its root kept. Fewer than 2 points, or a length at
// or below 0: as they were.
std::vector<Vec3f> guideAtLength(const std::vector<Vec3f>& points, float length, LengthChange change);

// The field applied to `guides` of `hierarchy` (each guide's length times the field's scale at its
// root): the guides that changed and their new points. A guide whose length stays within a part in
// a million is left out.
struct LengthResult {
    GuideId id = 0;
    std::vector<Vec3f> points;
};
std::vector<LengthResult> applyLengthField(const GuideHierarchy& hierarchy, const std::vector<GuideId>& guides,
                                           const LengthField& field, LengthChange change);

}  // namespace groomlab
