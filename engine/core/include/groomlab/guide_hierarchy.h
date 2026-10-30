// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "groomlab/frames.h"
#include "groomlab/guide_set.h"
#include "groomlab/length_paths.h"
#include "groomlab/math.h"
#include "groomlab/strand_set.h"
#include "groomlab/transfer.h"
#include "groomlab/tube.h"

namespace groomlab {

class ScalpMesh;

using GuideId = std::uint64_t;
using GroupId = std::uint32_t;
constexpr GuideId kNoGuide = 0;
constexpr GroupId kNoGroup = 0;


// A named set of guides. Groups form a tree (GH-1): each child group's guides
// have their parents in the parent group. Depth 0 groups hold primary guides.
struct GuideGroup {
    GroupId id = kNoGroup;
    std::string name;
    GroupId parent = kNoGroup;
    // The color the group's guides draw in (a Sculpt node's editable guides take their ToGuides
    // node's): display only, not saved; a host's per-group color (guideGroupColors) wins.
    bool hasColor = false;
    std::array<float, 3> color{0, 0, 0};
    // The segments the group's new guides get (drawn, placed, from curves, baked, generated
    // children), and what Resample on the whole group sets: 0, the tool's own. Saved.
    int segments = 0;
};

struct HierarchyGuide {
    GuideId id = kNoGuide;
    GroupId group = kNoGroup;
    GuideId parent = kNoGuide;  // kNoGuide for primary guides
    bool locked = false;        // locked children keep their own world shape (GH-6)
    // Resampled on its own (picked guides, not its whole group): its segments are its own, kept
    // when its group's are set (resample with groupWide skips it). Saved.
    bool ownSegments = false;
    StrandRoot root;
    TubeProfile tube;  // tube.h: its shape and sections, the volume its children (and fillTubes' strands) fill
    // Reference direction for this guide's first frame normal (projected
    // perpendicular to the root tangent). Fixed at creation so the frame, and
    // with it every child, turns smoothly when the guide is reshaped.
    Vec3f frameUp{1, 0, 0};
    std::map<std::string, float> attributes;  // per-guide data for downstream operators (GH-7)
    // Per-point layers: one value per point of `points`, painted along the
    // guide (the pointwise Attribute brush). Children without their own take
    // the nearest ancestor's, resampled along it; Interpolate carries them
    // onto the strands as point channels.
    std::map<std::string, std::vector<float>> pointAttributes;

    // World-space shape. Authoritative for primary and locked guides until
    // they are bound to the scalp; for following children it is recomputed by
    // GuideHierarchy::evaluate.
    std::vector<Vec3f> points;

    // Primary and locked guides, once rooted (GH-2): the shape and frameUp in
    // the root's surface frame (x along the root face's first edge, y along
    // the surface normal). evaluate() rebuilds world points from these, so
    // guides move and turn with a deforming scalp. Empty until first rooted;
    // cleared by setPoints so the next evaluate re-binds.
    std::vector<Vec3f> rootLocal;
    Vec3f frameUpLocal{1, 0, 0};
    // Its frame is laid across the scalp at the next evaluate (setTube gave its tube a shape; a
    // new primary with a shaped tube is laid so when first rooted). Not stored, not hashed.
    bool layFrame = false;
    // The scalp's normal at its root, from the last evaluate on a scalp (zero before): where its
    // children root on a shaped tube's footprint (generateChildren). Not stored, not hashed.
    Vec3f rootNormal;

    // Following children only (GH-5): shape relative to the parent, one entry
    // per parent sample t = i / (n - 1). x and y are the parent tube's section
    // coordinates (tube.h: in its rotation-minimizing frame, turned by the
    // section's twist, in units of its width across and width times aspect up),
    // so children follow the tube as it flattens, twists and tapers; z is along
    // the parent's tangent, in scene units.
    std::vector<Vec3f> offsets;
    // A following child's own point count (it was resampled, drawn or its group has segments of
    // its own): its shape, rebuilt from its parent's, respaced to that many points. 0: its
    // parent's count.
    std::uint32_t pointCount = 0;

    bool isChild() const { return parent != kNoGuide; }
    bool followsParent() const { return isChild() && !locked; }
    // The guide's rotation-minimizing frames along `points`, starting from frameUp.
    std::vector<Frame> frames() const;
};

// Settings for GuideHierarchy::generateChildren (GH-4).
// Children are placed inside the parent tube's shape (a round tube: a disc; other shapes: their
// outline, uniformly over its area), spread over rootSpread of it at the root and tipSpread at the
// tip. A child of a shaped tube (TubeProfile::shaped) starts its frames from its parent's root
// frame, so its own tube (the parent's, narrower) lies the same way. On a scalp, a child of a
// guide leaving the scalp at a slant has its root on the parent's root footprint (the root section
// laid flat on the scalp, as TubeSpine::setRootSurface lays it for strands): spread over the
// section's area rather than squeezed onto a line, its point joining the section over a few
// section widths along the parent. Leaning up to 30 degrees from the scalp's normal: the root
// section snapped onto the scalp as it always was; the footprint in full from 60 degrees.
struct ChildGenerationParams {
    std::size_t count = 5;         // children per parent guide
    float rootSpread = 0.8f;       // share of the parent's section the roots spread over
    float tipSpread = 1.0f;        // share of the parent's section the tips spread over
    float twist = 0.0f;            // radians, extra rotation of each child around the parent, root to tip
    float jitter = 0.1f;           // random tip offset, in section units of the parent's tube
    float childTubeScale = 0.35f;  // child tube width as a fraction of the parent's (its shape, aspect and twist the parent's)
    std::uint64_t seed = 0;
};

// Tonic-style guide hierarchy: primary guides, children generated inside
// their parents' tubes, grandchildren inside the children's tubes, and so on.
// Children store offsets in their parent's frame, so editing a parent carries
// every descendant with it.
class GuideHierarchy {
public:
    // ---- Groups ---------------------------------------------------------
    GroupId addGroup(const std::string& name, GroupId parent = kNoGroup);
    // A group's own draw color (GuideGroup::color; display only).
    void setGroupColor(GroupId id, const std::array<float, 3>& linearRgb);
    const std::vector<GuideGroup>& groups() const { return groups_; }
    const GuideGroup* group(GroupId id) const;
    GroupId findGroup(const std::string& name) const;  // kNoGroup if absent
    int groupDepth(GroupId id) const;                   // 0 for root groups, -1 if unknown
    int maxDepth() const;                               // -1 when there are no groups
    // The deepest groups of each family: those holding guides (2 points or more) with no group
    // below them that does. What level -1 takes (flatten): a group without children is its own
    // family's deepest, and a group whose child groups are empty keeps its guides.
    std::set<GroupId> deepestGroups() const;

    // A group's segments for new guides (GuideGroup::segments, 0 to 256; 0: none of its own).
    void setGroupSegments(GroupId id, int segments);
    // `points` (a new guide's for group `id`) along the smooth curve through them at the group's
    // segments; as they are when it has none.
    std::vector<Vec3f> withGroupSegments(GroupId id, std::vector<Vec3f> points) const;

    // Removes a group, its descendant groups and all their guides.
    void removeGroup(GroupId id);
    // Throws std::invalid_argument when the name is empty or taken.
    void renameGroup(GroupId id, const std::string& name);

    // ---- Guides ---------------------------------------------------------
    // Adds a primary guide to a root group. IDs are assigned when id is kNoGuide.
    GuideId addGuide(GroupId group, std::vector<Vec3f> points, TubeProfile tube = {}, GuideId id = kNoGuide);
    // Adds every guide of a flat set (a node's Guides output, ToGuides say) as
    // primaries of a root group, with their attributes as painted layers.
    // Guides keep their IDs when free, else get new ones; guides with fewer
    // than 2 points are skipped. Returns the IDs added. Call evaluate after,
    // to root them on the scalp.
    std::vector<GuideId> addGuides(GroupId group, const GuideSet& guides, TubeProfile tube = {});
    const std::vector<HierarchyGuide>& guides() const { return guides_; }
    const HierarchyGuide* guide(GuideId id) const;
    std::vector<GuideId> guidesInGroup(GroupId group) const;
    std::vector<GuideId> childrenOf(GuideId parent) const;

    // Generates `params.count` children for each of `parents` (default: every
    // guide of `parentGroup`) into the group `childGroupName` under
    // parentGroup, which is created if missing and added to otherwise. A parent
    // that already has children there gets new ones in their place (their own
    // descendants go too), so artists can pick, parent by parent, which guides
    // have children. Returns the child group. Parent shapes must be current
    // (call evaluate first after editing).
    GroupId generateChildren(GroupId parentGroup, const std::string& childGroupName,
                             const ChildGenerationParams& params, const std::vector<GuideId>* parents = nullptr);

    // Adds a child of `parent` to `group` (a child group of the parent's group)
    // with the given world shape, e.g. placed by hand. It follows its parent
    // from then on, keeping this shape relative to it. Without a tube (or with
    // one of no widths) it takes the parent's, its width scaled by 0.35. Returns its ID.
    GuideId addChildGuide(GroupId group, GuideId parent, std::vector<Vec3f> points,
                          std::optional<TubeProfile> tube = std::nullopt);

    // ---- Evaluation and editing -----------------------------------------
    // Binds unbound primary and locked guides to the scalp (flipping tip-first
    // primaries, then rooting at the closest point) and rebuilds bound ones
    // from their root frames, so they follow the scalp. Then recomputes every
    // following child top-down from its parent's frames and snaps its root
    // onto the scalp. Without a scalp, guides keep their current shapes.
    // A scalp with other triangles than the one the guides were bound on (a
    // re-meshed or subdivided head) unbinds them first: each keeps its world
    // shape and re-roots at the closest point, instead of being rebuilt on
    // the wrong face.
    void evaluate(const ScalpMesh* scalp);
    // The size the guides are shown at, as a groom scaled with its scalp is (GroomSession::
    // groomScale): evaluate rebuilds each bound guide's shape (rootLocal) times it about its root,
    // following children sit at their offsets times it (tube widths and the distance along the
    // parent: tubes are that much wider), and binding (a guide edited or added in world space)
    // divides it out, so what is stored (rootLocal, offsets, tube widths) stays at the groom's own
    // size whatever size it is shown at. 1 by default; copies keep it; not stored (toJson). Call
    // evaluate after.
    void setScale(float scale);
    float scale() const { return scale_; }
    // The topology (ScalpMesh::topologyHash) the bound guides' root frames
    // belong to; 0 until bound.
    std::uint64_t boundTopology() const { return boundTopology_; }

    // Same as evaluate, restricted to the given guides and their descendants:
    // the cheap path after editing a few guides (brush strokes). The scalp
    // must be the one the rest of the hierarchy was last evaluated on.
    void evaluateSubtrees(const ScalpMesh* scalp, const std::vector<GuideId>& changed);

    // Sets a guide's world points for this evaluation only, e.g. a simulated
    // pose. Its binding and offsets are kept, so the next evaluate() restores
    // the authored shape. Follow with evaluateDescendants.
    void pose(GuideId id, std::vector<Vec3f> points);
    // The same with the frame reference given (a guide moved rigidly, follow_scalp.h) rather than
    // carried along with the root direction.
    void pose(GuideId id, std::vector<Vec3f> points, const Vec3f& frameUp);
    // Re-evaluates the descendants of `guides`, not the guides themselves.
    void evaluateDescendants(const ScalpMesh* scalp, const std::vector<GuideId>& guides);

    // Hierarchy tools. Every guide keeps its current world shape; what changes
    // is what it follows. Throw std::invalid_argument on a bad request.
    //
    // Moves guides under another parent guide, which must be in their
    // group's parent group.
    void reparent(const std::vector<GuideId>& guides, GuideId newParent);
    // Moves a group one level up: its guides follow their grandparents (a
    // group reaching the root: its guides become primaries rooted on the
    // scalp at the next evaluate). Sub-groups move with it.
    void promoteGroup(GroupId group);
    // Moves a group one level down, under `newParent`, a group with the same
    // parent; each guide follows the guide of newParent whose root is nearest.
    void demoteGroup(GroupId group, GroupId newParent);
    // Mirrors a root group across the plane where coordinate `axis` (0 x,
    // 1 y, 2 z) equals `center`: guides whose roots are on the source side
    // (positive: above center) are copied to the other side, replacing the
    // guides there (and their descendants). A copy's frames (frameUp) are the
    // mirror image of its source's, as in mirrorAll. Returns the new guide IDs.
    std::vector<GuideId> mirrorGroup(GroupId group, int axis, float center, bool fromPositive);
    // Symmetry sync: makes the whole groom symmetric across the plane. Every
    // root group is mirrored as mirrorGroup does, and each copied guide takes
    // mirror images of its descendants (children, grandchildren... in their
    // own groups, attributes and locks kept), so hand-edited children survive.
    // Guides on the plane are left alone. Needs an evaluated hierarchy (the
    // children's world shapes). Returns the new guide IDs.
    std::vector<GuideId> mirrorAll(int axis, float center, bool fromPositive);
    // Moves every guide onto another scalp (GH-8). Evaluates on `from`, then
    // each primary and locked guide keeps its shape, turned with the surface
    // normal, and is re-rooted at the closest point of `to` to fit.apply(its
    // root). Children follow their parents. Both meshes at their current
    // positions; the hierarchy is left evaluated on `to`.
    void transfer(const ScalpMesh& from, const ScalpMesh& to, const ScalpFit& fit = {});
    // The same by any mapping (transfer.h) between the two meshes it was built on: each primary
    // and locked guide re-rooted where the mapping takes its root, its shape carried by the
    // mapping's local rotation and scale there (closest and bounds: as above); tube widths and
    // children's offsets along scale with the guide they hang from (keepLengths: they keep theirs).
    void transfer(const ScalpMapping& mapping);
    // Removes guides and their descendants.
    void removeGuides(const std::vector<GuideId>& guides);

    // Rebuilds guides with `segments` segments (1 to 256) evenly along the smooth curve through
    // their points, as the viewport draws it, keeping their shape (the Segments slider). Frozen
    // guides (the freeze layer, their own or inherited) and guides of fewer than 2 points are
    // left as they are. Picked guides (not `groupWide`) get segments of their own
    // (HierarchyGuide::ownSegments), unless it is their group's count (they follow the group
    // again); `groupWide` (a whole group, a level, every guide) leaves guides with their own as
    // they are. Returns how many were rebuilt.
    std::size_t resample(const std::vector<GuideId>& ids, int segments, bool groupWide = false);
    // Every guide of a group back to the group's segments, those with their own too (they follow
    // the group again); frozen guides are left as they are. A group without segments of its own
    // takes its guides' most common count first. Returns the segments (0: the group has no
    // guides).
    int resetGroupSegments(GroupId id);

    // Detaches a child: it keeps its current world shape from now on.
    void lock(GuideId id);
    // Re-attaches a child: its current world shape becomes offsets.
    void unlock(GuideId id);
    // Replaces a guide's world shape (brush edits). Following children convert
    // it to offsets; their children follow on the next evaluate. Per-point
    // layers are resampled to the new point count.
    void setPoints(GuideId id, std::vector<Vec3f> points);
    // A guide's tube. A primary without children given a shape (another shape, or round to
    // shaped) has its frame laid across the scalp at its next evaluate: the new shape lies flat on
    // the head at twist 0, across the way the guide lies over the scalp. A library shape (tube.h:
    // TubeProfile::custom) is the library's entry of that name; one the library lacks is added.
    void setTube(GuideId id, TubeProfile tube);

    // ---- Tube shape library ---------------------------------------------
    // Custom tube shapes (tube.h: CustomTubeShape), saved with the hierarchy (toJson). Guides take
    // one by name (TubeProfile::custom) as they take a built-in shape: children, Interpolate's
    // fillTubes, clumps and the display fill and draw its outline.
    const TubeShapeLibrary& tubeShapes() const { return tubeShapes_; }
    CustomTubeShapePtr tubeShape(const std::string& name) const;  // null when absent
    // Adds a shape, or gives the one of that name new points: every guide taking it takes the new
    // outline (call evaluate after). Throws std::invalid_argument as makeCustomTubeShape does.
    CustomTubeShapePtr setTubeShape(const std::string& name, const std::vector<SectionPoint>& points, bool smooth = true);
    // Renames a shape; the guides taking it keep it. Throws std::invalid_argument on an unknown
    // name, or a new one taken or not allowed (checkTubeShapeName).
    void renameTubeShape(const std::string& from, const std::string& to);
    // Removes a shape (throws std::invalid_argument on an unknown name): the guides taking it
    // become round. Returns how many did.
    std::size_t removeTubeShape(const std::string& name);
    // How many guides take the shape.
    std::size_t tubeShapeUsers(const std::string& name) const;
    // ---- Length paths ---------------------------------------------------
    // The Length Paths tool's paths (length_paths.h), saved with the hierarchy (toJson) so they
    // can be edited and applied again. They change no guide by themselves.
    const LengthPathSet& lengthPaths() const { return lengthPaths_; }
    void setLengthPaths(LengthPathSet paths);

    // What fromJson could not read as saved, said for the user: guides whose library shape was
    // missing (each loads round).
    const std::vector<std::string>& loadWarnings() const { return loadWarnings_; }
    void setAttribute(GuideId id, const std::string& name, float value);
    // Sets a per-point layer: `values` are resampled to the guide's point count
    // when they differ. Empty values remove the layer from the guide.
    void setPointAttribute(GuideId id, const std::string& name, std::vector<float> values);

    // ---- Output ---------------------------------------------------------
    // Guides of every group at `level` (-1: each family's deepest, deepestGroups), as a flat set
    // for operators such as Interpolate. Each carries its tube (widths at the
    // size shown, scale()) and frameUp, for Interpolate's fillTubes.
    GuideSet flatten(int level = -1) const;
    // A guide's attributes over those it inherits from its ancestors.
    std::map<std::string, float> effectiveAttributes(const HierarchyGuide& g) const;
    // One attribute as effectiveAttributes would give it (the nearest guide up
    // the chain that sets it); false when none does. No map is built.
    bool effectiveAttribute(const HierarchyGuide& g, const std::string& name, float* value) const;
    bool hasAttributes() const;  // any guide carries an attribute
    // Per-point layers (one value per guide point). A guide's effective layers
    // are its own over those of its ancestors, each resampled along the guide
    // to its point count (a child follows its parent point for point).
    std::map<std::string, std::vector<float>> effectivePointAttributes(const HierarchyGuide& g) const;
    // One per-point layer as effectivePointAttributes gives it, resampled to
    // g's point count; false when no guide up the chain carries it.
    bool effectivePointAttribute(const HierarchyGuide& g, const std::string& name, std::vector<float>* values) const;
    bool hasPointAttributes() const;  // any guide carries a per-point layer
    std::vector<std::string> pointAttributeNames() const;
    // Attribute layers of both kinds: every name any guide carries, sorted;
    // removing one from every guide; renaming one (throws if `to` exists).
    std::vector<std::string> attributeNames() const;
    void removeAttribute(const std::string& name);
    void renameAttribute(const std::string& from, const std::string& to);
    GuideSet flattenGroup(GroupId group) const;

    // Everything the hierarchy holds, hashed. Worked out once and kept until the next change:
    // every non-const member clears it (a new one must too), so hosts may call it on every
    // evaluation. computeContentHash works it out now, without the memo (tests check the two).
    std::uint64_t contentHash() const;
    std::uint64_t computeContentHash() const;

    // JSON round trip, for storage on the Maya node.
    std::string toJson() const;
    static GuideHierarchy fromJson(const std::string& text);  // throws std::runtime_error

private:
    HierarchyGuide* mutableGuide(GuideId id);
    GuideId nextGuideId();
    // `parentFrames`: the parent's rotation-minimizing frames when already known (null: computed).
    void resolveFollower(HierarchyGuide& child, const HierarchyGuide& parent, const ScalpMesh* scalp,
                         const std::vector<Frame>* parentFrames = nullptr);
    // Evaluates guides in `order` (parents first), computing each parent's frames once for all its followers.
    void evaluateInOrder(const std::vector<std::size_t>& order, const ScalpMesh* scalp);
    void evaluateGuide(HierarchyGuide& g, const ScalpMesh* scalp, const std::vector<Frame>* parentFrames = nullptr);
    // evaluateGuide's shape and rooting, before what it reads of the scalp for its children and tube.
    void placeGuide(HierarchyGuide& g, const ScalpMesh* scalp, const std::vector<Frame>* parentFrames);
    std::vector<std::size_t> depthOrder(const std::vector<std::size_t>& indices) const;
    std::vector<std::size_t> subtrees(const std::vector<GuideId>& roots, bool includeRoots) const;
    std::vector<Vec3f> offsetsFromWorld(const HierarchyGuide& child, const HierarchyGuide& parent) const;
    // A following child bound to `parent` as it is now: its offsets, and its own point count when
    // it differs from the parent's.
    void bindOffsets(HierarchyGuide& child, const HierarchyGuide& parent) const;
    // A tube's library shape made this hierarchy's entry of that name (added when it has none).
    void adoptTubeShape(TubeProfile& tube);
    void rebuildIndex();
    void invalidateHash() { hashMemo_.valid.store(false, std::memory_order_relaxed); }

    // contentHash's memo. Copies carry it (the same content).
    struct HashMemo {
        mutable std::atomic<bool> valid{false};
        mutable std::atomic<std::uint64_t> value{0};
        HashMemo() = default;
        HashMemo(const HashMemo& o) : valid(o.valid.load(std::memory_order_acquire)), value(o.value.load()) {}
        HashMemo& operator=(const HashMemo& o) {
            const bool known = o.valid.load(std::memory_order_acquire);  // before the value (stored after it)
            value.store(o.value.load());
            valid.store(known);
            return *this;
        }
    };

    HashMemo hashMemo_;
    std::vector<GuideGroup> groups_;
    std::vector<HierarchyGuide> guides_;
    std::unordered_map<GuideId, std::size_t> index_;
    GroupId nextGroupId_ = 1;
    GuideId nextGuideId_ = 1;
    std::uint64_t boundTopology_ = 0;
    float scale_ = 1.0f;  // setScale
    TubeShapeLibrary tubeShapes_;
    LengthPathSet lengthPaths_;
    std::vector<std::string> loadWarnings_;
};

}  // namespace groomlab
