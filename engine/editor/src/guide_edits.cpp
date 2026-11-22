// SPDX-License-Identifier: Apache-2.0
#include "groomlab/editor/guide_edits.h"

#include <algorithm>
#include <set>

#include "groomlab/guide_selection.h"
#include "groomlab/scalp_maps.h"

namespace groomlab {

namespace {

[[noreturn]] void refuse(const char* code, const std::string& what) { throw GuideEditError(code, what); }

GroupId groupNamed(const GuideHierarchy& h, const std::string& name) {
    if (name.empty()) refuse("bad_request", "give a guide group");
    const GroupId id = h.findGroup(name);
    if (id == kNoGroup) refuse("not_found", "no guide group named " + name);
    return id;
}

const std::vector<GuideId>& givenGuides(const GuideHierarchy& h, const GuideOp& op) {
    if (op.guides.empty()) refuse("state", op.op + ": no guides given and none selected");
    for (const GuideId id : op.guides) {
        if (!h.guide(id)) refuse("not_found", "no guide " + std::to_string(id));
    }
    return op.guides;
}

void needGuides(const GuideHierarchy& h) {
    if (h.guides().empty()) refuse("state", "the groom has no guides");
}

// The core's own refusals (std::invalid_argument) as the op's.
template <typename F>
auto core(F&& f) -> decltype(f()) {
    try {
        return f();
    } catch (const GuideEditError&) {
        throw;
    } catch (const std::invalid_argument& e) {
        refuse("bad_request", e.what());
    }
}

}  // namespace

ChildrenMade generateChildren(GuideHierarchy& h, const ChildrenRequest& request) {
    if (request.params.count < 1 || request.params.count > kMaxChildrenPerGuide) {
        refuse("bad_request", "a guide gets 1 to " + std::to_string(kMaxChildrenPerGuide) + " children");
    }
    GroupId parent = kNoGroup;
    if (!request.parents.empty()) {
        // Selection first: the parents' one group.
        std::set<GroupId> groups;
        for (const GuideId id : request.parents) {
            const HierarchyGuide* g = h.guide(id);
            if (!g) refuse("not_found", "no guide " + std::to_string(id));
            groups.insert(g->group);
        }
        if (groups.size() > 1) {
            std::string names;
            for (const GroupId g : groups) names += (names.empty() ? "" : ", ") + h.group(g)->name;
            refuse("state", "the selected guides are in several groups (" + names + "): select the guides of one group to give children");
        }
        parent = *groups.begin();
        if (!request.group.empty() && h.findGroup(request.group) != parent) {
            refuse("state", "the chosen parents must all be in one group (" + request.group + ")");
        }
    } else {
        if (!request.group.empty()) {
            parent = groupNamed(h, request.group);
        } else if (!h.groups().empty()) {
            parent = h.groups().back().id;  // the last group, as every host's Guides tab
        }
        if (parent == kNoGroup) refuse("state", "the groom has no guides yet");
        if (h.guidesInGroup(parent).empty()) refuse("state", "the group " + h.group(parent)->name + " has no guides to be parents");
    }
    ChildrenMade out;
    out.parentGroup = h.group(parent)->name;
    out.childGroup = request.childGroup.empty() ? h.childGroupFor(parent) : request.childGroup;  // every host's
    const std::size_t before = h.guides().size();
    const GroupId child = core([&] {
        return h.generateChildren(parent, out.childGroup, request.params, request.parents.empty() ? nullptr : &request.parents);
    });
    out.added = h.guides().size() >= before ? h.guides().size() - before : 0;
    out.childGroup = h.group(child)->name;
    out.childCount = h.guidesInGroup(child).size();
    return out;
}

GuideOpResult applyGuideOp(GuideHierarchy& h, const GuideOp& op) {
    GuideOpResult out;
    const std::string& o = op.op;
    if (o == "add_group") {
        GroupId parent = kNoGroup;
        if (!op.parent.empty()) parent = groupNamed(h, op.parent);
        out.group = op.name.empty() ? h.freeGroupName(op.parent.empty() ? "group" : op.parent + "_children") : guideGroupName(op.name);
        if (out.group.empty()) refuse("bad_request", "a guide group needs a name");
        if (h.findGroup(out.group) != kNoGroup) refuse("bad_request", "a guide group named " + out.group + " is there already");
        core([&] { return h.addGroup(out.group, parent); });
    } else if (o == "remove_group") {
        h.removeGroup(groupNamed(h, op.group));
    } else if (o == "delete_guides") {
        if (op.guides.empty()) refuse("state", "delete guides: no guides given and none selected");
        std::vector<GuideId> doomed;
        for (const GuideId id : op.guides) {
            if (h.guide(id)) doomed.push_back(id);  // (one gone already: nothing to do)
        }
        if (!doomed.empty()) h.removeGuides(doomed, true);  // (a child group left empty goes too)
        out.count = doomed.size();
    } else if (o == "lock" || o == "unlock") {
        for (const GuideId id : givenGuides(h, op)) {
            if (!h.guide(id)->isChild()) continue;  // (only children lock to their parents)
            if (o == "lock") h.lock(id);
            else h.unlock(id);
            ++out.count;
        }
        if (out.count == 0) refuse("state", "only child guides " + o + ": the guides given are primary guides");
    } else if (o == "set_attribute" || o == "freeze" || o == "unfreeze") {
        const std::string name = o == "set_attribute" ? op.name : std::string(kFreezeLayer);
        if (!validMapName(name)) refuse("bad_request", mapNameError(name));
        const float value = o == "set_attribute" ? op.value : o == "freeze" ? 1.0f : 0.0f;
        for (const GuideId id : givenGuides(h, op)) h.setAttribute(id, name, value);
        out.count = op.guides.size();
    } else if (o == "unfreeze_all") {
        h.removeAttribute(kFreezeLayer);
    } else if (o == "invert_layer") {
        const std::string layer = op.name.empty() ? std::string(kFreezeLayer) : op.name;
        if (!(layer == kHiddenLayer ? invertHidden(h) : invertFrozen(h, layer))) refuse("state", "the groom has no guides");
    } else if (o == "remove_layer") {
        const std::vector<std::string> names = h.attributeNames();
        if (std::find(names.begin(), names.end(), op.name) == names.end()) refuse("not_found", "no guide layer named " + op.name);
        h.removeAttribute(op.name);
    } else if (o == "mirror_all" || o == "mirror_group") {
        if (op.axis < 0 || op.axis > 2) refuse("bad_request", "the mirror's axis is x, y or z");
        needGuides(h);
        if (o == "mirror_all") {
            out.made = core([&] { return h.mirrorAll(op.axis, op.center, op.fromPositive); });
        } else {
            const GroupId id = groupNamed(h, op.group);
            if (h.groupDepth(id) != 0) refuse("bad_request", op.group + " is not a root group (mirror its root group: the children follow)");
            out.made = core([&] { return h.mirrorGroup(id, op.axis, op.center, op.fromPositive); });
        }
    } else if (o == "mirror_shapes") {
        if (!op.symmetry.on()) refuse("bad_request", "Mirror to Other Side needs a symmetry: x, y, z or topology");
        const std::vector<GuideId> sources = op.side == 0 ? givenGuides(h, op) : std::vector<GuideId>{};
        const GroupId only = op.side != 0 && !op.group.empty() ? groupNamed(h, op.group) : kNoGroup;
        out.mirrored = core([&] { return mirrorGuideShapes(h, op.symmetry, sources, op.side, only); });
        if (out.mirrored.reshaped.empty() && out.mirrored.created.empty() && out.mirrored.removed.empty()) {
            refuse("state", op.side == 0 ? "the selected guides are on the symmetry's seam: nothing to mirror" : "no guides on that side");
        }
    } else if (o == "move_to_group" || o == "duplicate_to_group") {
        const GroupId target = groupNamed(h, op.group);
        const std::vector<GuideId>& ids = givenGuides(h, op);
        if (o == "duplicate_to_group") {
            out.made = core([&] { return h.duplicateGuidesToGroup(ids, target); });
        } else {
            core([&] { h.moveGuidesToGroup(ids, target); return 0; });
        }
        out.count = ids.size();
    } else if (o == "new_group_from") {
        const std::vector<GuideId>& ids = givenGuides(h, op);
        out.group = h.group(core([&] { return h.newGroupFromGuides(ids, op.name.empty() ? "group" : op.name); }))->name;
        out.count = ids.size();
    } else if (o == "paste_guides") {
        const GroupId target = groupNamed(h, op.group);
        out.made = core([&] { return h.pasteGuidesJson(op.text, target); });
    } else if (o == "promote_group") {
        const GroupId id = groupNamed(h, op.group);
        if (h.groupDepth(id) == 0) refuse("bad_request", op.group + " is a root group already");
        core([&] { h.promoteGroup(id); return 0; });
    } else if (o == "demote_group") {
        const GroupId id = groupNamed(h, op.group), parent = groupNamed(h, op.parent);
        core([&] { h.demoteGroup(id, parent); return 0; });
    } else if (o == "reparent") {
        const std::vector<GuideId>& ids = givenGuides(h, op);
        if (!h.guide(op.parentGuide)) refuse("not_found", "no guide " + std::to_string(op.parentGuide) + " to be the parent");
        core([&] { h.reparent(ids, op.parentGuide); return 0; });
        out.count = ids.size();
    } else if (o == "resample") {
        if (op.segments < 1 || op.segments > kMaxGuideSegments) refuse("bad_request", "a guide has 1 to 256 segments");
        if (h.guides().empty()) refuse("state", "the groom has no guides to resample");
        const GroupId only = op.group.empty() ? kNoGroup : groupNamed(h, op.group);
        // The guides given; none: the group's, the level's, else every guide (whole groups).
        std::vector<GuideId> ids = op.guides;
        bool groupWide = op.groupWide;
        if (ids.empty()) {
            groupWide = true;
            if (only != kNoGroup) {
                ids = guidesAt(h, -1, op.group);
            } else if (op.level >= 0) {
                ids = guidesAt(h, op.level);
            } else {
                for (const HierarchyGuide& g : h.guides()) ids.push_back(g.id);
            }
        }
        // Every host's (guide_hierarchy.h resampleGuides), frozen guides kept.
        out.count = core([&] { return h.resampleGuides(ids, op.segments, groupWide, only, op.level, op.byLength); });
    } else if (o == "set_group_segments") {
        if (op.segments < 0 || op.segments > kMaxGuideSegments) refuse("bad_request", "a group's segments: 0 to 256 (0: the tool's own)");
        h.setGroupSegments(groupNamed(h, op.group), op.segments);
    } else if (o == "reset_group_segments") {
        out.segments = h.resetGroupSegments(groupNamed(h, op.group));
        if (out.segments < 1) refuse("state", "the group has no guides");
    } else {
        refuse("bad_request",
               "op is add_group, remove_group, delete_guides, lock, unlock, set_attribute, freeze, unfreeze, unfreeze_all, "
               "invert_layer, remove_layer, mirror_all, mirror_group, mirror_shapes, move_to_group, duplicate_to_group, "
               "new_group_from, paste_guides, promote_group, demote_group, reparent, resample, set_group_segments or "
               "reset_group_segments, not '" + o + "'");
    }
    return out;
}

std::string guideOpLabel(const GuideOp& op) {
    const std::string& o = op.op;
    if (o == "lock" || o == "unlock" || o == "freeze" || o == "unfreeze") return o + " guides";
    if (o == "delete_guides") return "delete guides";
    if (o == "set_attribute") return "set " + op.name;
    if (o == "invert_layer") return op.name == kHiddenLayer ? "invert hidden" : "invert frozen";
    if (o == "remove_layer") return "remove layer " + op.name;
    if (o == "mirror_shapes") return "mirror to other side";
    if (o == "move_to_group") return "move guides";
    if (o == "duplicate_to_group") return "duplicate guides";
    if (o == "new_group_from") return "new group from selection";
    if (o == "reparent") return "reparent guides";
    if (o == "resample") return "resample guides";
    if (o == "set_group_segments") return "group segments";
    if (o == "reset_group_segments") return "reset segments";
    std::string label = o;  // add_group -> add group, mirror_all -> mirror all...
    std::replace(label.begin(), label.end(), '_', ' ');
    return label;
}

}  // namespace groomlab
