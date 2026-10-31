// SPDX-License-Identifier: Apache-2.0
// Sculpting tools: Twist, Curl, Straighten, Attract, Erase, keeping off
// the scalp, Place (matching neighbors), Place Many and Draw.

#include <cmath>
#include <map>
#include <stdexcept>

#include <gtest/gtest.h>

#include "groomlab/brush.h"
#include "groomlab/curve.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

float lengthOf(const std::vector<Vec3f>& pts) { return arcLength(pts.data(), pts.size()); }

struct Field {
    std::shared_ptr<const ScalpMesh> plane = test::makePlane(8, 2.0f);
    GuideHierarchy h;
    GroupId g = kNoGroup;
    std::vector<GuideId> ids;

    // A row of standing guides along x at z = 1 (the plane spans 0..2).
    explicit Field(int count = 5, Vec3f dir = {0, 0.6f, 0}) {
        g = h.addGroup("primary");
        for (int i = 0; i < count; ++i) {
            ids.push_back(h.addGuide(g, test::straightCurve({0.2f + 0.4f * i, 0, 1.0f}, dir, 6)));
        }
        h.evaluate(plane.get());
    }
    const std::vector<Vec3f>& pts(GuideId id) const { return h.guide(id)->points; }
};

BrushSettings settings(BrushMode mode, float radius = 0.3f, float strength = 1.0f) {
    BrushSettings s;
    s.mode = mode;
    s.radius = radius;
    s.strength = strength;
    return s;
}

TEST(Sculpt, CombWithSymmetryMovesTheMirrorImage) {
    Field f;  // x from 0.2 to 1.8: guides 0 and 4 mirror across x = 1
    BrushSettings s = settings(BrushMode::Comb, 0.35f);
    s.mirrorAxis = 0;
    s.mirrorCenter = 1.0f;
    applyBrushStroke(f.h, f.plane.get(), s, {{0.2f, 0.6f, 1.0f}, {0.0f, 0.6f, 1.0f}});
    EXPECT_LT(f.pts(f.ids[0]).back().x, 0.15f);
    EXPECT_NEAR(f.pts(f.ids[0]).back().x - 0.2f, -(f.pts(f.ids[4]).back().x - 1.8f), 1e-4f);
    EXPECT_EQ(f.pts(f.ids[2]), Field().pts(Field().ids[2]));  // the middle: out of reach
}

TEST(Sculpt, CutTrimsWhereTheBrushTouches) {
    Field f(2, {0, 1.0f, 0});
    applyBrushStroke(f.h, f.plane.get(), settings(BrushMode::Cut, 0.2f), {{0.2f, 0.5f, 1.0f}});
    const float cut = lengthOf(f.pts(f.ids[0]));
    EXPECT_GT(cut, 0.3f);
    EXPECT_LT(cut, 0.5f);
    EXPECT_EQ(f.pts(f.ids[0]).size(), 7u);  // same point count, respaced
    EXPECT_NEAR(lengthOf(f.pts(f.ids[1])), 1.0f, 1e-4f);  // out of reach
    // Cutting above the tip does nothing.
    applyBrushStroke(f.h, f.plane.get(), settings(BrushMode::Cut, 0.1f), {{0.6f, 1.3f, 1.0f}});
    EXPECT_NEAR(lengthOf(f.pts(f.ids[1])), 1.0f, 1e-4f);
}

TEST(Sculpt, BrushesKeepGuidesOffTheScalp) {
    Field f;
    // Comb hard straight down, into the plane.
    for (int i = 0; i < 5; ++i) {
        applyBrushStroke(f.h, f.plane.get(), settings(BrushMode::Comb, 3.0f), {{1, 0.6f, 1}, {1, -0.4f, 1}});
    }
    for (const GuideId id : f.ids) {
        for (const Vec3f& p : f.pts(id)) EXPECT_GE(p.y, -1e-3f);
        EXPECT_NEAR(lengthOf(f.pts(id)), 0.6f, 1e-3f);
    }

    Field off;
    BrushSettings s = settings(BrushMode::Comb, 3.0f);
    s.collide = false;
    applyBrushStroke(off.h, off.plane.get(), s, {{1, 0.6f, 1}, {1, -0.4f, 1}});
    float lowest = 0.0f;
    for (const GuideId id : off.ids) lowest = std::min(lowest, off.pts(id).back().y);
    EXPECT_LT(lowest, -0.05f);  // without it, they go through
}

TEST(Sculpt, TwistSwingsAroundTheRootNormal) {
    Field f(1, {0.4f, 0.4f, 0});  // leaning +x
    const auto before = f.pts(f.ids[0]);
    for (int i = 0; i < 5; ++i) applyBrushStroke(f.h, f.plane.get(), settings(BrushMode::Twist, 1.0f), {{0.2f, 0.3f, 1}});
    const auto after = f.pts(f.ids[0]);
    EXPECT_EQ(after.front(), before.front());
    EXPECT_NEAR(after.back().y, before.back().y, 1e-4f);  // same height: turned about the normal
    EXPECT_GT(std::abs(after.back().z - 1.0f), 0.1f);     // swung out of the xz line
    EXPECT_NEAR(lengthOf(after), lengthOf(before), 1e-4f);
}

TEST(Sculpt, StraightenUndoesABend) {
    Field f(1, {0, 1.0f, 0});
    for (int i = 0; i < 10; ++i) applyBrushStroke(f.h, f.plane.get(), settings(BrushMode::Comb, 0.4f), {{0.2f, 0.8f, 1}, {0.8f, 0.8f, 1}});
    const float bent = (f.pts(f.ids[0]).back() - f.pts(f.ids[0]).front()).length();
    for (int i = 0; i < 20; ++i) applyBrushStroke(f.h, f.plane.get(), settings(BrushMode::Straighten, 2.0f), {{0.2f, 0.5f, 1}});
    const float chord = (f.pts(f.ids[0]).back() - f.pts(f.ids[0]).front()).length();
    EXPECT_GT(chord, bent + 0.05f);
    EXPECT_NEAR(lengthOf(f.pts(f.ids[0])), 1.0f, 1e-3f);
}

TEST(Sculpt, AttractPullsTipsTogether) {
    Field f(2, {0, 1.0f, 0});  // x = 0.2 and 0.6
    const float apart = f.pts(f.ids[1]).back().x - f.pts(f.ids[0]).back().x;
    for (int i = 0; i < 10; ++i) applyBrushStroke(f.h, f.plane.get(), settings(BrushMode::Attract, 1.0f), {{0.4f, 1.0f, 1}});
    const float after = f.pts(f.ids[1]).back().x - f.pts(f.ids[0]).back().x;
    EXPECT_LT(after, apart * 0.7f);
    EXPECT_EQ(f.pts(f.ids[0]).front().x, 0.2f);
}

TEST(Sculpt, EraseRemovesGuidesAndTheirChildren) {
    Field f;
    ChildGenerationParams params;
    params.count = 2;
    f.h.generateChildren(f.g, "children", params);
    f.h.evaluate(f.plane.get());
    const std::size_t before = f.h.guides().size();
    EXPECT_EQ(applyBrushStroke(f.h, f.plane.get(), settings(BrushMode::Erase, 0.3f), {{0.2f, 0.3f, 1.0f}}), 1u);
    EXPECT_EQ(f.h.guide(f.ids[0]), nullptr);
    EXPECT_EQ(f.h.guides().size(), before - 3);  // the guide and its two children
    EXPECT_NE(f.h.guide(f.ids[1]), nullptr);
}

TEST(Sculpt, PlaceMatchesTheNeighborsShape) {
    Field f(3, {0.4f, 0.3f, 0});  // leaning +x, length 0.5
    PlaceSettings s;
    const GuideId id = placeGuide(f.h, *f.plane, s, f.plane->closestPoint({0.6f, 0, 1.4f}));
    const auto& pts = f.pts(id);
    EXPECT_EQ(pts.size(), f.pts(f.ids[0]).size());  // the neighbors' point count, not `segments`
    EXPECT_NEAR(lengthOf(pts), 0.5f, 0.02f);
    const Vec3f dir = (pts.back() - pts.front()).normalized();
    EXPECT_GT(dir.dot(Vec3f{0.8f, 0.6f, 0}), 0.98f);

    s.matchNeighbors = false;
    const GuideId standing = placeGuide(f.h, *f.plane, s, f.plane->closestPoint({1.0f, 0, 0.4f}));
    EXPECT_GT((f.pts(standing).back() - f.pts(standing).front()).normalized().y, 0.999f);
}

TEST(Sculpt, NewGuidesTakeTheNeighborsPaintedLayers) {
    Field f(3, {0.4f, 0.3f, 0});  // leaning +x, length 0.5, in "primary"
    for (const GuideId id : f.ids) f.h.setAttribute(id, "density", 0.25f);
    f.h.setAttribute(f.ids[0], "clump", 1.0f);  // only one neighbor has it: its own value, not diluted
    PlaceSettings s;
    const GuideId placed = placeGuide(f.h, *f.plane, s, f.plane->closestPoint({0.6f, 0, 1.4f}));
    const auto layers = f.h.effectiveAttributes(*f.h.guide(placed));
    EXPECT_NEAR(layers.at("density"), 0.25f, 1e-5f);
    EXPECT_NEAR(layers.at("clump"), 1.0f, 1e-5f);

    // The first guide of a new group learns from the groups around it: shape and layers.
    s.group = "bangs";
    const GuideId first = placeGuide(f.h, *f.plane, s, f.plane->closestPoint({0.7f, 0, 1.3f}));
    EXPECT_EQ(f.h.group(f.h.guide(first)->group)->name, "bangs");
    const auto& pts = f.pts(first);
    EXPECT_GT((pts.back() - pts.front()).normalized().dot(Vec3f{0.8f, 0.6f, 0}), 0.98f);
    EXPECT_NEAR(f.h.effectiveAttributes(*f.h.guide(first)).at("density"), 0.25f, 1e-5f);

    // Drawn guides keep their drawn shape and take the layers.
    const GuideId drawn = drawGuide(f.h, *f.plane, s, {{0.9f, 0, 1.2f}, {0.9f, 0.3f, 1.2f}, {0.9f, 0.6f, 1.2f}});
    EXPECT_NEAR(f.h.effectiveAttributes(*f.h.guide(drawn)).at("density"), 0.25f, 1e-5f);

    // Match neighbors off: a plain standing guide, no layers.
    s.matchNeighbors = false;
    s.group = "loose";
    const GuideId plain = placeGuide(f.h, *f.plane, s, f.plane->closestPoint({1.0f, 0, 1.2f}));
    EXPECT_GT((f.pts(plain).back() - f.pts(plain).front()).normalized().y, 0.999f);
    EXPECT_TRUE(f.h.effectiveAttributes(*f.h.guide(plain)).empty());
}

TEST(Sculpt, PlaceManyKeepsItsSpacing) {
    const auto plane = test::makePlane(8, 2.0f);
    GuideHierarchy h;
    PlaceSettings s;
    s.spacing = 0.2f;
    std::vector<Vec3f> path;
    for (int i = 0; i <= 100; ++i) path.push_back({0.1f + 0.018f * i, 0.0f, 1.0f});  // 1.8 units
    const std::vector<GuideId> placed = placeGuides(h, *plane, s, path);
    EXPECT_GE(placed.size(), 8u);
    EXPECT_LE(placed.size(), 10u);
    for (std::size_t i = 0; i < placed.size(); ++i) {
        for (std::size_t j = i + 1; j < placed.size(); ++j) {
            EXPECT_GE((h.guide(placed[i])->points.front() - h.guide(placed[j])->points.front()).length(), 0.2f - 1e-4f);
        }
    }
    // A second pass over the same path adds nothing.
    EXPECT_TRUE(placeGuides(h, *plane, s, path).empty());
}

TEST(Sculpt, DrawFollowsTheStroke) {
    const auto plane = test::makePlane(8, 2.0f);
    GuideHierarchy h;
    PlaceSettings s;
    s.segments = 8;
    std::vector<Vec3f> path;
    for (int i = 0; i <= 20; ++i) {
        const float a = 1.5708f * i / 20.0f;  // a quarter circle rising from the plane
        path.push_back({1.0f + 0.5f - 0.5f * std::cos(a), 0.5f * std::sin(a), 1.0f});
    }
    const GuideId id = drawGuide(h, *plane, s, path);
    const auto& pts = h.guide(id)->points;
    EXPECT_EQ(pts.size(), 9u);
    EXPECT_NEAR(pts.front().y, 0.0f, 1e-4f);
    EXPECT_NEAR(pts.back().x, 1.5f, 0.02f);
    EXPECT_NEAR(pts.back().y, 0.5f, 0.02f);
    for (const Vec3f& p : pts) EXPECT_GE(p.y, -1e-4f);
    EXPECT_THROW(drawGuide(h, *plane, s, {{1, 0, 1}}), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Tonic-style children

std::size_t childrenOf(const GuideHierarchy& h, GuideId parent) { return h.childrenOf(parent).size(); }

TEST(Children, ChosenParentsGetChildrenAndRegenerateInPlace) {
    Field f(3, {0, 0.6f, 0});
    ChildGenerationParams p;
    p.count = 4;
    const std::vector<GuideId> first{f.ids[0]};
    const GroupId kids = f.h.generateChildren(f.g, "kids", p, &first);
    EXPECT_EQ(childrenOf(f.h, f.ids[0]), 4u);
    EXPECT_EQ(childrenOf(f.h, f.ids[1]), 0u);

    const std::vector<GuideId> second{f.ids[1]};
    EXPECT_EQ(f.h.generateChildren(f.g, "kids", p, &second), kids);  // the same group, added to
    EXPECT_EQ(childrenOf(f.h, f.ids[1]), 4u);

    p.count = 2;
    f.h.generateChildren(f.g, "kids", p, &first);  // regenerate: replaced, not added
    EXPECT_EQ(childrenOf(f.h, f.ids[0]), 2u);
    EXPECT_EQ(f.h.guidesInGroup(kids).size(), 6u);

    // A child picked to have children of its own.
    const std::vector<GuideId> grandparent{f.h.childrenOf(f.ids[1]).front()};
    const GroupId grand = f.h.generateChildren(kids, "grandkids", p, &grandparent);
    EXPECT_EQ(f.h.groupDepth(grand), 2);
    EXPECT_EQ(f.h.guidesInGroup(grand).size(), 2u);

    const std::vector<GuideId> wrong{f.ids[2]};
    EXPECT_THROW(f.h.generateChildren(kids, "grandkids", p, &wrong), std::invalid_argument);
}

TEST(Children, AnEditedChildStillFollowsItsParent) {
    Field f(1, {0, 0.6f, 0});
    ChildGenerationParams p;
    p.count = 3;
    f.h.generateChildren(f.g, "kids", p);
    f.h.evaluate(f.plane.get());
    const GuideId child = f.h.childrenOf(f.ids[0]).front();

    // Brush the child alone (level 1): it bends, its siblings do not.
    BrushSettings s = settings(BrushMode::Comb, 0.5f);
    s.level = 1;
    const GuideId sibling = f.h.childrenOf(f.ids[0])[1];
    const auto siblingBefore = f.pts(sibling);
    const Vec3f tip = f.pts(child).back();
    const std::map<GuideId, float> only{{child, 1.0f}};
    BrushSettings childOnly = s;
    childOnly.mask = &only;
    applyBrushStroke(f.h, f.plane.get(), childOnly, {tip, tip + Vec3f{0.2f, 0, 0}});
    const Vec3f edited = f.pts(child).back();
    EXPECT_GT(edited.x, tip.x + 0.1f);
    EXPECT_EQ(f.pts(sibling), siblingBefore);

    // Now comb the parent (level 0): the child comes along, keeping its own bend.
    const float bendBefore = edited.x - f.pts(f.ids[0]).back().x;
    s.level = 0;
    applyBrushStroke(f.h, f.plane.get(), s, {{0.2f, 0.6f, 1.0f}, {0.2f, 0.6f, 1.4f}});
    const Vec3f parentTip = f.pts(f.ids[0]).back();
    EXPECT_GT(parentTip.z, 1.1f);                   // the parent was combed
    EXPECT_GT(f.pts(child).back().z, 1.05f);        // the child followed
    EXPECT_NEAR(f.pts(child).back().x - parentTip.x, bendBefore, 0.1f);  // with its own shape
}

TEST(Children, PlacedByHandIntoAChildGroup) {
    Field f(3, {0.2f, 0.5f, 0});
    const GroupId kids = f.h.addGroup("kids", f.g);
    PlaceSettings s;
    s.group = "kids";
    const GuideId id = placeGuide(f.h, *f.plane, s, f.plane->closestPoint({0.65f, 0, 1.1f}));
    const HierarchyGuide* g = f.h.guide(id);
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->group, kids);
    EXPECT_EQ(g->parent, f.ids[1]);  // the nearest primary (x = 0.6)
    EXPECT_NEAR(g->points.front().x, 0.65f, 1e-3f);
    // The first child copies its parents' lean; then it follows the parent.
    EXPECT_GT(g->points.back().x - g->points.front().x, 0.1f);
    const Vec3f before = g->points.back();
    applyBrushStroke(f.h, f.plane.get(), settings(BrushMode::Comb, 0.5f), {{0.6f, 0.4f, 1.0f}, {0.6f, 0.4f, 1.4f}});
    EXPECT_GT(f.h.guide(id)->points.back().z, before.z + 0.05f);
    // Drawing into it works the same way.
    std::vector<Vec3f> stroke;
    for (int i = 0; i <= 10; ++i) stroke.push_back({1.35f, 0.05f * i, 1.0f + 0.02f * i});
    const GuideId drawn = drawGuide(f.h, *f.plane, s, stroke);
    EXPECT_EQ(f.h.guide(drawn)->parent, f.ids[2]);
}

TEST(Sculpt, FrozenGuidesAreLeftAlone) {
    Field f(2, {0, 0.6f, 0});
    ChildGenerationParams p;
    p.count = 2;
    f.h.generateChildren(f.g, "kids", p);
    f.h.evaluate(f.plane.get());
    f.h.setAttribute(f.ids[0], "freeze", 1.0f);  // its children inherit it
    const auto frozen = f.pts(f.ids[0]);
    const GuideId frozenChild = f.h.childrenOf(f.ids[0]).front();
    const auto frozenChildBefore = f.pts(frozenChild);
    BrushSettings s = settings(BrushMode::Comb, 3.0f);
    s.level = -1;
    applyBrushStroke(f.h, f.plane.get(), s, {{0.4f, 0.6f, 1.0f}, {0.4f, 0.6f, 1.5f}});
    EXPECT_EQ(f.pts(f.ids[0]), frozen);
    EXPECT_EQ(f.pts(frozenChild), frozenChildBefore);
    EXPECT_NE(f.pts(f.ids[1]).back(), Field(2, {0, 0.6f, 0}).pts(f.ids[1]).back());  // the other one moved

    // Painting the freeze mask itself is never held back: unfreeze.
    BrushSettings paint = settings(BrushMode::Attribute, 3.0f);
    paint.attribute = "freeze";
    paint.value = 0.0f;
    applyBrushStroke(f.h, f.plane.get(), paint, {{0.4f, 0.3f, 1.0f}});
    EXPECT_LT(f.h.guide(f.ids[0])->attributes.at("freeze"), 0.01f);
    s.freeze.clear();  // off
    applyBrushStroke(f.h, f.plane.get(), s, {{0.4f, 0.6f, 1.0f}, {0.4f, 0.6f, 1.5f}});
    EXPECT_NE(f.pts(f.ids[0]), frozen);
}

TEST(Sculpt, ModeNamesRoundTrip) {
    for (const char* name : {"twist", "straighten", "attract", "erase", "cut"}) {
        EXPECT_STREQ(brushModeName(brushModeFromString(name)), name);
    }
    EXPECT_THROW(brushModeFromString("grab"), std::invalid_argument);  // gone: Comb (with Stretch) does it
}

}  // namespace
}  // namespace groomlab
