// SPDX-License-Identifier: Apache-2.0
#pragma once

// The groom editor's guide edits (engine/editor), every host's: what an edit of the guide
// hierarchy does to it, worked out once; the host runs it on the hierarchy it edits (its own copy
// of the authored one), evaluates and stores it in its undo step.

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "groomlab/brush.h"
#include "groomlab/guide_hierarchy.h"

namespace groomlab {

// The guide layer Freeze paints (1: a guide no brush moves), every host's.
inline constexpr const char* kFreezeLayer = "freeze";

// Generate Children, as asked.
struct ChildrenRequest {
    // The parents: these guides (the selection, or ids a script names), all of one group. None:
    // every guide of `group`.
    std::vector<GuideId> parents;
    // The parents' group: with `parents`, the group they must be in ("": theirs); without, the
    // group whose guides get children ("": the last group).
    std::string group;
    std::string childGroup;        // "": the group's own child group (childGroupFor), else that one (made if missing)
    ChildGenerationParams params;  // its count 1 to kMaxChildrenPerGuide
};

// Children generated.
struct ChildrenMade {
    std::string parentGroup, childGroup;
    std::size_t added = 0;       // guides more than before (a parent's old children replaced: the new ones less those)
    std::size_t childCount = 0;  // guides in the child group after
};

// Generate Children, every host's: the selected guides when there are some (selection first),
// all of one group (an error when they span several), else every guide of the group asked for (the
// last group by default); `count` children each in the group's child group. On `h`, which must be
// evaluated (the parents' shapes). Throws std::invalid_argument for a count out of range, no such
// guide or group, parents in several groups or not in the group asked for, a group without guides.
ChildrenMade generateChildren(GuideHierarchy& h, const ChildrenRequest& request);

// A guide edit refused, with what kind of refusal it is: "not_found" (no such guide, group or
// layer), "state" (nothing to do it on: no guides, none given) or "bad_request" (an ask it cannot
// take). The Blender helper passes the code on; the others show the text.
struct GuideEditError : std::invalid_argument {
    std::string code;
    GuideEditError(std::string kind, const std::string& what) : std::invalid_argument(what), code(std::move(kind)) {}
};

// An Edit Guides op, as asked (every host's Guides tab, Edit Guides menu and scripting).
//   add_group (name: "" a free one; parent), remove_group (group), delete_guides (guides),
//   lock / unlock (guides: their children), set_attribute (guides, name, value), freeze /
//   unfreeze (guides), unfreeze_all, invert_layer (name: "freeze" or "hidden"), remove_layer
//   (name), mirror_all / mirror_group (group; axis, center, fromPositive), mirror_shapes
//   (guides for side 0, else side 1 or -1 and group; symmetry), move_to_group /
//   duplicate_to_group (guides, group), new_group_from (guides, name), paste_guides (text,
//   group), promote_group (group), demote_group (group under parent), reparent (guides under
//   parentGuide), resample (guides, segments; groupWide, group, level, byLength),
//   set_group_segments (group, segments: 0 the tool's own), reset_group_segments (group).
struct GuideOp {
    std::string op;
    std::vector<GuideId> guides;       // the guides it works on (the selection, or ids a script gives)
    std::string group;                 // the group it works on (a move's or paste's target)
    std::string parent;                // add_group's parent group, demote_group's new parent
    std::string name;                  // a new group's base or name, a layer's name
    std::string text;                  // paste_guides' copied guides (copyGuidesJson)
    GuideId parentGuide = kNoGuide;    // reparent's new parent
    float value = 1.0f;                // set_attribute's value
    int axis = 0;                      // mirror_all / mirror_group: the plane where coordinate axis (0 x, 1 y, 2 z)...
    float center = 0.0f;               // ... equals center
    bool fromPositive = true;          // the side mirrored over the other
    StrokeSymmetry symmetry;           // mirror_shapes' (StrokeSymmetry::of the brushes' settings)
    int side = 0;                      // mirror_shapes: 0 the guides' mirrors take their images; 1 / -1 that whole side over the other
    int segments = 0;                  // resample (1 to kMaxGuideSegments), set_group_segments (0 to it)
    bool groupWide = false;            // resample: whole groups (their new guides get the segments too)
    int level = -1;                    // resample: the level asked for (-1: none)
    bool byLength = false;             // resample: segments by length
};

// What an op did.
struct GuideOpResult {
    std::string group;                 // the group made (add_group, new_group_from)
    std::vector<GuideId> made;         // guides made (mirror_all / mirror_group, duplicate_to_group, paste_guides)
    std::size_t count = 0;             // guides it changed (delete_guides, lock / unlock, resample, move_to_group...)
    int segments = 0;                  // reset_group_segments' segments
    GuideHierarchy::MirrorShapesResult mirrored;  // mirror_shapes'
};

// An Edit Guides op on `h` (evaluated: shapes current), every host's: its checks, then the core's
// edit (guide_hierarchy.h). The host evaluates `h` after and stores it as one undo step. Throws
// GuideEditError.
GuideOpResult applyGuideOp(GuideHierarchy& h, const GuideOp& op);
// The op's undo step's name, every host's ("add group", "mirror group", "set <layer>"...).
std::string guideOpLabel(const GuideOp& op);

// Tubes edited, every host's Tubes section (Maya's -setTube, the helper's tube_set / tube_scale /
// tube_smooth): each of `guides`' tube by `edit` (tube.h editedTube's JSON: the keys given replace
// theirs, widths as the hierarchy stores them). Returns the guides changed. Throws GuideEditError:
// none given ("state"), one not there ("not_found"), an edit it cannot take ("bad_request").
std::size_t editTubes(GuideHierarchy& h, const std::vector<GuideId>& guides, const std::string& edit);

// An edit of the groom's tube shape library, as asked (every host's: add, set, rename, remove).
struct TubeShapeOp {
    std::string op;                                  // add, set, rename or remove
    std::string name;                                // the shape
    std::string to;                                  // rename's new name
    std::optional<std::vector<SectionPoint>> points; // add (needed), set: its outline
    std::optional<bool> smooth;                      // add (default smooth), set: kept when not given
    std::vector<GuideId> give;                       // add: these guides take it at once (as `profile`)
    std::string profile = "shape";                   // shape, midShape or tipShape
};
// The library edited, every guide taking the shape following it (remove: round). Returns the
// guides taking the shape after (remove: the ones that took it). Throws GuideEditError.
std::size_t editTubeShape(GuideHierarchy& h, const TubeShapeOp& op);
// The edit's undo step name, every host's ("add tube shape", "edit tube shape"...).
std::string tubeShapeLabel(const TubeShapeOp& op);

}  // namespace groomlab
