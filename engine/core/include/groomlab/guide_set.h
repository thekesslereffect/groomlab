// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/math.h"
#include "groomlab/strand_set.h"
#include "groomlab/tube.h"

namespace groomlab {

class ScalpMesh;

// A guide curve. points[0] is the root end.
struct Guide {
    std::uint64_t id = 0;
    StrandRoot root;  // set by GuideSet::rootOnMesh
    std::vector<Vec3f> points;
    // Per-guide values (GH-7), e.g. painted "clump"; Interpolate blends them
    // into strand channels of the same name.
    std::map<std::string, float> attributes;
    // Per-point layers: one value per point of `points`, painted along the
    // guide; Interpolate blends them into point channels of the same name.
    std::map<std::string, std::vector<float>> pointAttributes;
    // Its tube (tube.h), when it comes from a guide hierarchy (hasTube; widths at the size the
    // hierarchy shows), and the reference its first frame normal starts from: what
    // Interpolate's fillTubes fills. Guides from curves, ToGuides and the like have none.
    bool hasTube = false;
    TubeProfile tube;
    Vec3f frameUp{1, 0, 0};
    // A drawn root's tube or a slice of one: the direction across the scalp its footprint is laid
    // along (GuideHierarchy: its root's, a slice its parent's, so the slices of a root lie as one).
    // Zero: the scalp's own at its root (ScalpMesh::acrossAt). Not saved.
    Vec3f rootAcross;
    // A slice of a drawn root's tube: its footprint's center, its place in its level 1 root's
    // footprint (GuideHierarchy: HierarchyGuide::rootFoot). Unset: its root. Not saved.
    Vec3f rootFoot;
    bool rootFootSet = false;
    // A slice of a drawn root's tube: the path its tube takes (HierarchyGuide::tubePoints), its
    // guide's points with the root joined to the scalp. Empty: its tube along its points. Not saved.
    std::vector<Vec3f> tubePoints;
    // The level of the hierarchy it comes from (0: a root group's): where tubes of several levels
    // hold a strand's root, fillTubes takes the deepest.
    int depth = 0;
    // The root of the guide its hierarchy starts from (its level-0 ancestor's: its own at level 0;
    // face -1 when it has none): where an Interpolate reads a guide's region on a region map, so a
    // tube split into tubes of tubes keeps the region of the tube it was split from.
    StrandRoot baseRoot{-1, 0.0f, 0.0f};
    // That guide's ID (its own at level 0; 0 when it has none): fillTubes fills a strand rooted in a
    // level-0 tube only with that tube's own deeper tubes, never a neighbour's.
    std::uint64_t baseId = 0;
};

// Whether a curve was drawn tip-first: its last point nearer the mesh than its
// first by more than 2% of its length (a curve with both ends on the mesh, a
// path drawn on it, starts at its root).
bool drawnTipFirst(const std::vector<Vec3f>& points, const ScalpMesh& mesh);

// Moves a curve rigidly so its first point sits on the closest point of the
// mesh; returns that root. Points must not be empty.
StrandRoot rootRigidly(std::vector<Vec3f>& points, const ScalpMesh& mesh);

// Flat set of guides: curves for Interpolate and Clump. The editable parent/child
// hierarchy is GuideHierarchy (guide_hierarchy.h), which flattens into these.
class GuideSet {
public:
    std::size_t size() const { return guides_.size(); }
    bool empty() const { return guides_.empty(); }

    Guide& add(std::uint64_t id, std::vector<Vec3f> points);
    const std::vector<Guide>& guides() const { return guides_; }
    std::vector<Guide>& guides() { return guides_; }

    // Roots every guide on the closest scalp point and moves the curve rigidly
    // so its first point sits exactly on the surface (GH-2).
    void rootOnMesh(const ScalpMesh& mesh);

    // Reverses guides whose last point is nearer the scalp than their first,
    // so curves drawn tip-first still root correctly, then roots them.
    void orientAndRoot(const ScalpMesh& mesh);

    std::uint64_t contentHash() const;

    // Interpolate's fillTubes fills the tubes of every level of the hierarchy these guides come
    // from, not only theirs: the guides of the other levels of their groups' family (the groups
    // above and below theirs; a Guides node's exclude still holds), each with its tube, frameUp
    // and depth (no attributes). Null: none (a single level, or guides not from a hierarchy).
    // Copies of the set share it; part of the hash.
    const std::shared_ptr<const std::vector<Guide>>& levelTubes() const { return levelTubes_; }
    void setLevelTubes(std::shared_ptr<const std::vector<Guide>> tubes) { levelTubes_ = std::move(tubes); }

    // The color these guides draw in when they are shown as guides (a ToGuides node's `color`,
    // carried by copies: a Sculpt node's editable guides). Display only: not part of the hash.
    bool hasDisplayColor() const { return hasDisplayColor_; }
    const Vec3f& displayColor() const { return displayColor_; }
    void setDisplayColor(const Vec3f& linearRgb) {
        displayColor_ = linearRgb;
        hasDisplayColor_ = true;
    }

private:
    std::vector<Guide> guides_;
    std::shared_ptr<const std::vector<Guide>> levelTubes_;
    Vec3f displayColor_{0, 0, 0};
    bool hasDisplayColor_ = false;
};

}  // namespace groomlab
