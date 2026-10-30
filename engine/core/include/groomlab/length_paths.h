// SPDX-License-Identifier: Apache-2.0
#pragma once

// Length paths: the Length Paths tool's lines of keys on the scalp, each key a guide length and a
// reach. Guides near a path take their lengths from it: the length at the nearest point of the
// path, blended between the keys on either side, faded back to the guide's own length toward the
// edge of the reach there (blended between the keys' reaches too: thick at a leg's top, thin at
// its paw). Several paths (one per leg of an animal) blend by how near each is. The
// paths are saved with the guides (GuideHierarchy::lengthPaths), so they can be edited and applied
// again; keys hold lengths, not shares, so applying twice gives what applying once did.
//
// Distances are straight lines between the guides' roots and the path's segments (not along the
// scalp). A path of one key is a point: guides within its reach take its length.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "groomlab/math.h"
#include "groomlab/strand_set.h"

namespace groomlab {

class GuideHierarchy;
class ScalpMesh;
using GuideId = std::uint64_t;  // (guide_hierarchy.h's)

// One key: where it is on the scalp (a root, so it follows the scalp when it moves; face -1:
// only the point is known) and the point as placed, the length guides take there, and how far
// from the path guides are reached there.
struct LengthKey {
    StrandRoot root;
    Vec3f point;
    float length = 1.0f;
    float reach = 1.0f;
};

// One path: its keys in order along it, and how lengths go between keys: smooth (a monotone curve
// through the keys: no overshoot past either key) or straight. Reaches go straight between keys.
struct LengthPath {
    std::vector<LengthKey> keys;
    bool smooth = true;
};

struct LengthPathSet {
    std::vector<LengthPath> paths;

    bool empty() const;  // no path with a key
    // JSON, saved with the guides ("lengthPaths"):
    //   [{"smooth": true, "keys": [{"face": 3, "u": 0.2, "v": 0.3,
    //     "point": [x, y, z], "length": 4.5, "reach": 2.0}, ...]}, ...]
    // Throws std::invalid_argument for text that is not such a list.
    std::string toJson() const;
    static LengthPathSet fromJson(const std::string& text);
};

// How a guide takes its new length: Scale (the whole guide scaled about its root, so a curl or bend
// keeps its shape, larger or smaller) or Trim (cut at the tip, or extended along its last
// direction: curls keep their size).
enum class LengthChange { Scale, Trim };
const char* lengthChangeName(LengthChange change);           // "scale", "trim"
LengthChange lengthChangeFromName(const std::string& name);  // throws std::invalid_argument

// Where a path puts a point: the length there, and how much it counts (1 within most of the
// reach, easing to 0 at its edge; 0 beyond it, or for a path without keys). The spot of the path
// that counts is the one the point lies deepest inside the reach of.
struct LengthSample {
    float length = 0.0f;
    float weight = 0.0f;
};
// The keys' points on `scalp` (their roots where they are on it, else their points).
Vec3f lengthKeyPosition(const LengthKey& key, const ScalpMesh* scalp);
LengthSample sampleLengthPath(const LengthPath& path, const Vec3f& point, const ScalpMesh* scalp = nullptr);
// Every path at the point: the paths' lengths blended by their weights, and the largest weight
// (how much the result replaces the guide's own length). Weight 0: no path reaches it.
LengthSample sampleLengthPaths(const LengthPathSet& paths, const Vec3f& point, const ScalpMesh* scalp = nullptr);

// The paths mirrored across the plane where coordinate `axis` (0 x, 1 y, 2 z) is `center` (symmetry):
// each key's point mirrored, its root found again on `scalp` (the closest point; none: point only).
LengthPathSet mirroredLengthPaths(const LengthPathSet& paths, const ScalpMesh* scalp, int axis = 0, float center = 0.0f);

// A new key's length at `point`: the guides of `guides` (primary guides, by root) within `radius`,
// their lengths averaged, the nearer ones counting more; none within it: the nearest guide's; no
// guides: 0.
float averageGuideLength(const GuideHierarchy& hierarchy, const std::vector<GuideId>& guides, const Vec3f& point,
                         float radius);

// A guide's points at a new length (`change`), its root kept. Fewer than 2 points, or a length at
// or below 0: as they were.
std::vector<Vec3f> guideAtLength(const std::vector<Vec3f>& points, float length, LengthChange change);

// The paths applied to `guides` of `hierarchy` (each guide's length blended toward the paths'
// length at its root by their weight): the guides that changed and their new points. A guide no
// path reaches, or whose length stays within a part in a million, is left out.
struct LengthResult {
    GuideId id = 0;
    std::vector<Vec3f> points;
};
std::vector<LengthResult> applyLengthPaths(const GuideHierarchy& hierarchy, const std::vector<GuideId>& guides,
                                           const LengthPathSet& paths, LengthChange change,
                                           const ScalpMesh* scalp = nullptr);

}  // namespace groomlab
