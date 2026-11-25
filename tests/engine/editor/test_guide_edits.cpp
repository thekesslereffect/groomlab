// SPDX-License-Identifier: Apache-2.0
// The groom editor's guide edits (engine/editor guide_edits.h), every host's: Generate Children
// takes the selection first, all of one group, else every guide of a group.

#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "groomlab/editor/guide_edits.h"
#include "groomlab/root_tubes.h"
#include "../core/test_helpers.h"

using namespace groomlab;

namespace {

struct TwoGroups {
    std::shared_ptr<const ScalpMesh> scalp = test::makePlane(4);
    GuideHierarchy h;
    GuideId a1 = 0, a2 = 0, b1 = 0;
    TwoGroups() {
        const GroupId a = h.addGroup("front");
        const GroupId b = h.addGroup("back");
        const auto guide = [&](GroupId g, int point) {
            const Vec3f root = scalp->points()[point];
            return h.addGuide(g, {root, root + Vec3f{0.0f, 0.5f, 0.0f}, root + Vec3f{0.0f, 1.0f, 0.0f}});
        };
        a1 = guide(a, 0);
        a2 = guide(a, 6);
        b1 = guide(b, 12);
        h.evaluate(scalp.get());
    }
};

}  // namespace

TEST(GuideEdits, GenerateChildrenTakesTheSelectionFirstAllOfOneGroup) {
    TwoGroups t;
    ChildrenRequest request;
    request.params.count = 3;
    request.parents = {t.a1};
    const ChildrenMade made = generateChildren(t.h, request);
    EXPECT_EQ(made.parentGroup, "front");
    EXPECT_EQ(made.childGroup, "front_children");
    EXPECT_EQ(made.added, 3u);
    EXPECT_EQ(t.h.childrenOf(t.a1).size(), 3u);
    EXPECT_TRUE(t.h.childrenOf(t.a2).empty()) << "only the selected guide";
    // A selection across groups is refused, as is one outside the group asked for.
    request.parents = {t.a2, t.b1};
    EXPECT_THROW(generateChildren(t.h, request), std::invalid_argument);
    request.parents = {t.b1};
    request.group = "front";
    EXPECT_THROW(generateChildren(t.h, request), std::invalid_argument);
}

TEST(GuideEdits, GenerateChildrenWithoutASelectionTakesAGroupTheLastByDefault) {
    TwoGroups t;
    ChildrenRequest request;
    request.params.count = 2;
    const ChildrenMade last = generateChildren(t.h, request);
    EXPECT_EQ(last.parentGroup, "back");
    EXPECT_EQ(t.h.childrenOf(t.b1).size(), 2u);
    request.group = "front";
    const ChildrenMade front = generateChildren(t.h, request);
    EXPECT_EQ(front.childCount, 4u);
    EXPECT_EQ(t.h.childrenOf(t.a1).size(), 2u);
    // Refused: no such group, a count out of range, a groom without guides.
    request.group = "nope";
    EXPECT_THROW(generateChildren(t.h, request), std::invalid_argument);
    request.group.clear();
    request.params.count = 0;
    EXPECT_THROW(generateChildren(t.h, request), std::invalid_argument);
    GuideHierarchy empty;
    request.params.count = 1;
    EXPECT_THROW(generateChildren(empty, request), std::invalid_argument);
}

TEST(GuideEdits, OpsCheckWhatTheyAreGivenThenEditTheHierarchy) {
    TwoGroups t;
    auto run = [&](GuideOp op) { return applyGuideOp(t.h, op); };
    GuideOp add;
    add.op = "add_group";
    add.parent = "front";
    EXPECT_EQ(run(add).group, "front_children");
    add.name = "front children";
    EXPECT_THROW(run(add), GuideEditError) << "front_children is taken";
    GuideOp mirror;
    mirror.op = "mirror_group";
    mirror.group = "front_children";
    try {
        run(mirror);
        ADD_FAILURE() << "a child group is not mirrored";
    } catch (const GuideEditError& e) {
        EXPECT_EQ(e.code, "bad_request");
    }
    GuideOp promote;
    promote.op = "promote_group";
    promote.group = "front";
    EXPECT_THROW(run(promote), GuideEditError) << "a root group already";
    promote.group = "nope";
    try {
        run(promote);
    } catch (const GuideEditError& e) {
        EXPECT_EQ(e.code, "not_found");
    }
    // Layers: set on the guides given, a missing one refused, removed.
    GuideOp set;
    set.op = "set_attribute";
    set.name = "frizz";
    set.value = 0.5f;
    EXPECT_THROW(run(set), GuideEditError) << "no guides given";
    set.guides = {t.a1, t.b1};
    EXPECT_EQ(run(set).count, 2u);
    EXPECT_FLOAT_EQ(t.h.guide(t.b1)->attributes.at("frizz"), 0.5f);
    GuideOp remove;
    remove.op = "remove_layer";
    remove.name = "curl";
    EXPECT_THROW(run(remove), GuideEditError);
    remove.name = "frizz";
    run(remove);
    EXPECT_TRUE(t.h.attributeNames().empty());
    // Moves and copies, a new group, deletes.
    GuideOp duplicate;
    duplicate.op = "duplicate_to_group";
    duplicate.group = "back";
    duplicate.guides = {t.a1};
    EXPECT_EQ(run(duplicate).made.size(), 1u);
    EXPECT_EQ(t.h.guidesInGroup(t.h.findGroup("back")).size(), 2u);
    GuideOp from;
    from.op = "new_group_from";
    from.guides = {t.a2};
    EXPECT_FALSE(run(from).group.empty());
    GuideOp del;
    del.op = "delete_guides";
    del.guides = {t.b1, 99999};
    EXPECT_EQ(run(del).count, 1u) << "a guide gone already is nothing to do";
    // Resample: whole groups without guides given; segments in range.
    GuideOp resample;
    resample.op = "resample";
    resample.segments = 4;
    EXPECT_GT(run(resample).count, 0u);
    EXPECT_EQ(t.h.guide(t.a1)->points.size(), 5u);
    resample.segments = 0;
    EXPECT_THROW(run(resample), GuideEditError);
    GuideOp odd;
    odd.op = "fold";
    EXPECT_THROW(run(odd), GuideEditError);
    EXPECT_EQ(guideOpLabel(mirror), "mirror group");
    EXPECT_EQ(guideOpLabel(set), "set frizz");
}

TEST(GuideEdits, TubesAndTheTubeShapeLibrary) {
    TwoGroups t;
    EXPECT_EQ(editTubes(t.h, {t.a1, t.b1}, R"({"aspect": 0.5})"), 2u);
    ASSERT_FALSE(t.h.guide(t.a1)->tube.aspect.empty());
    EXPECT_FLOAT_EQ(t.h.guide(t.a1)->tube.aspect.front(), 0.5f);
    EXPECT_THROW(editTubes(t.h, {}, R"({"aspect": 0.5})"), GuideEditError);
    EXPECT_THROW(editTubes(t.h, {99999}, R"({"aspect": 0.5})"), GuideEditError);
    TubeShapeOp add;
    add.op = "add";
    add.name = "leaf";
    EXPECT_THROW(editTubeShape(t.h, add), GuideEditError) << "a new shape needs its points";
    add.points = std::vector<SectionPoint>{{-1.0f, 0.0f}, {0.0f, 0.5f}, {1.0f, 0.0f}, {0.0f, -0.5f}};
    add.give = {t.a2};
    EXPECT_EQ(editTubeShape(t.h, add), 1u);
    EXPECT_EQ(tubeShapeLabel(add), "tube shape");
    EXPECT_THROW(editTubeShape(t.h, add), GuideEditError) << "taken";
    TubeShapeOp rename;
    rename.op = "rename";
    rename.name = "leaf";
    rename.to = "petal";
    EXPECT_EQ(editTubeShape(t.h, rename), 1u);
    TubeShapeOp remove;
    remove.op = "remove";
    remove.name = "leaf";
    EXPECT_THROW(editTubeShape(t.h, remove), GuideEditError) << "renamed: no leaf";
    remove.name = "petal";
    EXPECT_EQ(editTubeShape(t.h, remove), 1u);
    EXPECT_EQ(t.h.tubeShape("petal"), nullptr);
}

// A root tube keeps its drawn root (issue: a Root profile turned it into a plain tube, its root
// ring cleared): a Root profile given goes to the other guides, the rest of the edit to it too, and
// the note says how many kept theirs; given to root tubes alone, it is refused.
TEST(GuideEdits, ARootTubeKeepsItsDrawnRoot) {
    const std::shared_ptr<const ScalpMesh> scalp = test::makeSphere(48, 96, 1.0f);
    GuideHierarchy h;
    std::vector<Vec3f> outline;
    for (int k = 0; k < 5; ++k) {
        const float t = 6.2831853f * static_cast<float>(k) / 5.0f;
        outline.push_back(Vec3f(0.12f * std::cos(t), 1.0f, 0.12f * std::sin(t)).normalized());
    }
    const GuideId root = makeRootTube(h, *scalp, outline, {});
    const GroupId primary = h.addGroup("primary");
    const Vec3f at = Vec3f(0.6f, 0.8f, 0.0f).normalized();
    const GuideId plain = h.addGuide(primary, {at, at * 1.2f, at * 1.4f});
    h.evaluate(scalp.get());
    const std::string drawn = h.guide(root)->tube.custom->name;
    ASSERT_GE(h.guide(root)->rootRing.size(), 3u);
    std::string note;
    EXPECT_EQ(editTubes(h, {root, plain}, R"({"shape": "ribbon"})", &note), 1u) << "the plain guide only";
    EXPECT_NE(note.find("1 root tube keeps its drawn root"), std::string::npos) << note;
    EXPECT_EQ(h.guide(plain)->tube.shapeName(), "ribbon");
    EXPECT_EQ(h.guide(root)->tube.custom->name, drawn);
    h.evaluate(scalp.get());
    EXPECT_TRUE(isRootTube(h.guide(root)->tube, 0));
    EXPECT_GE(h.guide(root)->rootRing.size(), 3u) << "its root ring kept";
    // The rest of an edit still applies to it.
    EXPECT_EQ(editTubes(h, {root}, R"({"shape": "blade", "tipShape": "teardrop"})", &note), 1u);
    EXPECT_EQ(h.guide(root)->tube.custom->name, drawn);
    ASSERT_TRUE(h.guide(root)->tube.tipShape);
    EXPECT_EQ(h.guide(root)->tube.tipShape->name(), "teardrop");
    // A Root profile alone, to root tubes alone: refused, nothing changed.
    EXPECT_THROW(editTubes(h, {root}, R"({"shape": "blade"})"), GuideEditError);
    EXPECT_EQ(h.guide(root)->tube.custom->name, drawn);
    note = "?";
    EXPECT_EQ(editTubes(h, {root}, nlohmann::json{{"shape", drawn}}.dump(), &note), 1u) << "its own root again";
    EXPECT_TRUE(note.empty());
    // A new library shape given at once (Duplicate): the root tube keeps its root.
    TubeShapeOp add;
    add.op = "add";
    add.name = "leaf";
    add.points = std::vector<SectionPoint>{{-1.0f, 0.0f}, {0.0f, 0.5f}, {1.0f, 0.0f}, {0.0f, -0.5f}};
    add.give = {root, plain};
    EXPECT_EQ(editTubeShape(h, add), 1u);
    EXPECT_EQ(h.guide(root)->tube.custom->name, drawn);
    EXPECT_EQ(h.guide(plain)->tube.shapeName(), "leaf");
    // Its drawn root is no library shape: the library's edits refuse it, the root tools' scripts
    // (owned) make and fit theirs.
    TubeShapeOp set;
    set.op = "set";
    set.name = drawn;
    set.points = add.points;
    EXPECT_THROW(editTubeShape(h, set), GuideEditError);
    TubeShapeOp rename;
    rename.op = "rename";
    rename.name = drawn;
    rename.to = "mine";
    EXPECT_THROW(editTubeShape(h, rename), GuideEditError);
    TubeShapeOp remove;
    remove.op = "remove";
    remove.name = drawn;
    EXPECT_THROW(editTubeShape(h, remove), GuideEditError);
    TubeShapeOp own;
    own.op = "add";
    own.name = std::string(kOwnedTubeShapePrefix) + "77";
    own.points = add.points;
    EXPECT_THROW(editTubeShape(h, own), GuideEditError) << "the name is kept for root tubes";
    own.owned = true;
    EXPECT_NO_THROW(editTubeShape(h, own));
    EXPECT_TRUE(h.tubeShape(own.name));
}
