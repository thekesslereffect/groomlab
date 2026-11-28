// SPDX-License-Identifier: Apache-2.0
// Root-first tubes and the tube hierarchy (root_tubes.h): a root's outline drawn on the scalp
// becomes a tube's root, kept where it was drawn; tubes split level by level, copied and removed
// with their hierarchy. (Maya's test_root_tubes.py pins the same behaviour through its Tube tool,
// and compares these against the first version, written in Maya's Python.)

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <set>

#include "groomlab/brush.h"
#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/root_tubes.h"
#include "groomlab/tube.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

constexpr float kPi = 3.14159265f;

std::shared_ptr<const ScalpMesh> head() {
    static const std::shared_ptr<const ScalpMesh> sphere = test::makeSphere(48, 96, 1.0f);
    return sphere;
}

Vec3f onSphere(const Vec3f& p) { return p.normalized(); }

// `count` CVs round `center` (on the unit sphere), each `radius` (times its `scale`) away.
std::vector<Vec3f> ring(const Vec3f& center, float radius, int count = 5, const std::vector<float>& scale = {}) {
    const Vec3f up = onSphere(center);
    const Vec3f a = up.cross({0, 0, 1}).normalized();
    const Vec3f b = up.cross(a);
    std::vector<Vec3f> out;
    for (int k = 0; k < count; ++k) {
        const float s = scale.empty() ? 1.0f : scale[static_cast<std::size_t>(k) % scale.size()];
        const float t = 2.0f * kPi * static_cast<float>(k) / static_cast<float>(count) + 0.3f;
        out.push_back(onSphere(up + a * (radius * s * std::cos(t)) + b * (radius * s * std::sin(t))));
    }
    return out;
}

float nearest(const Vec3f& p, const std::vector<Vec3f>& pts) {
    float best = 1e30f;
    for (const Vec3f& q : pts) best = std::min(best, (p - q).length());
    return best;
}

float arc(const std::vector<Vec3f>& pts) {
    float total = 0.0f;
    for (std::size_t i = 1; i < pts.size(); ++i) total += (pts[i] - pts[i - 1]).length();
    return total;
}

std::size_t guideCount(const GuideHierarchy& h) { return h.guides().size(); }

TEST(RootTubes, TheRootSectionIsTheOutlineDrawn) {
    GuideHierarchy h;
    const auto scalp = head();
    const std::vector<Vec3f> outline = ring({0, 1, 0}, 0.15f);
    RootTubeOptions o;
    o.length = 0.8f;
    o.taper = 0.5f;
    const GuideId id = makeRootTube(h, *scalp, outline, {}, o);
    const HierarchyGuide& g = *h.guide(id);
    ASSERT_TRUE(g.tube.custom);
    EXPECT_EQ(g.tube.custom->name.rfind(kOwnedTubeShapePrefix, 0), 0u) << "a root shape of its own";
    EXPECT_FALSE(g.tube.rollOnScalp);
    EXPECT_EQ(h.group(g.group)->name, kRootTubeGroup);
    EXPECT_NEAR(g.tube.width.back() / g.tube.width.front(), 0.5f, 1e-4f) << "tapered";
    EXPECT_LT((g.points.front() - onSphere(outlineCentroid(outline))).length(), 0.02f) << "rooted at the middle";
    EXPECT_NEAR(arc(g.points), 0.8f, 0.05f);
    const std::vector<Vec3f> kept = rootTubeOutline(h, *scalp, id);
    ASSERT_EQ(kept.size(), outline.size());
    for (std::size_t i = 0; i < kept.size(); ++i) EXPECT_LT((kept[i] - outline[i]).length(), 0.003f) << "CV " << i;
    for (const Vec3f& q : rootTubeSectionOutline(h, *scalp, id)) EXPECT_LT(nearest(q, kept), 0.01f) << "the section on the CVs";
    EXPECT_LT(rootFootprintError(h, *scalp, id), 0.004f);
    EXPECT_EQ(rootTubesOf(h), std::vector<GuideId>{id});
    // Too few or too many CVs, or no length: said, nothing made.
    EXPECT_THROW(makeRootTube(h, *scalp, {outline[0], outline[1]}, {}), std::invalid_argument);
    EXPECT_THROW(makeRootTube(h, *scalp, std::vector<Vec3f>(65, outline[0]), {}), std::invalid_argument);
    EXPECT_EQ(guideCount(h), 1u);
}

TEST(RootTubes, ADrawnPathIsSmoothedEvenlySpacedAndClearOfTheScalp) {
    // A shaky stroke (a smooth arc plus the hand's jitter, bunched up at its slow start): the guide
    // follows the arc, its jitter gone, its spans one length, each turning under 15 degrees.
    const Vec3f root = onSphere({0, 1, 0});
    std::vector<Vec3f> clean, shaky;
    for (int i = 0; i < 90; ++i) {
        const float t = std::pow(i / 89.0f, 1.4f);
        const float a = 1.2f * t;
        const Vec3f c(0.0f, 1.0f + 0.6f * std::sin(a), 0.6f * (1.0f - std::cos(a)) + 0.4f * t);
        clean.push_back(c);
        const float jitter = 0.012f * std::sin(i * 2.7f) + 0.008f * std::cos(i * 4.1f);
        shaky.push_back(c + Vec3f(jitter, 0.6f * jitter, -0.5f * jitter));
    }
    shaky[0] = clean[0] = root;
    const std::vector<Vec3f> pts = rootTubeGuidePath(root, {0, 1, 0}, shaky, 1.0f, 12, 0.12f);
    EXPECT_GE(pts.size() - 1, 8u);
    EXPECT_LE(pts.size() - 1, 16u);
    double mean = 0.0;
    for (std::size_t i = 1; i < pts.size(); ++i) mean += (pts[i] - pts[i - 1]).length();
    mean /= static_cast<double>(pts.size() - 1);
    for (std::size_t i = 1; i < pts.size(); ++i) {
        EXPECT_LT(std::abs((pts[i] - pts[i - 1]).length() - mean) / mean, 0.05) << "evenly spaced at " << i;
    }
    for (std::size_t i = 2; i + 1 < pts.size(); ++i) {
        const Vec3f u = (pts[i] - pts[i - 1]).normalized(), v = (pts[i + 1] - pts[i]).normalized();
        EXPECT_LT(std::acos(std::clamp(u.dot(v), -1.0f, 1.0f)), 15.0f * kPi / 180.0f) << "smooth at " << i;
    }
    const std::vector<Vec3f> dense = resamplePolyline(clean, 400);
    for (std::size_t i = 1; i < pts.size(); ++i) EXPECT_LT(nearest(pts[i], dense), 0.03f) << "on the arc drawn";
    // A stroke laid flat along the scalp: the guide leaves the root at most 50 degrees off its normal.
    std::vector<Vec3f> flat{root};
    for (int k = 1; k <= 20; ++k) flat.push_back({0.6f * k / 20.0f, 1.0f + 0.02f * k / 20.0f, 0.0f});
    const std::vector<Vec3f> laid = rootTubeGuidePath(root, {0, 1, 0}, flat, 1.0f, 12, 0.12f);
    EXPECT_LE(leanOff(laid[1] - laid[0], {0, 1, 0}), 50.0f * kPi / 180.0f + 1e-4f);
    const std::vector<Vec3f>& path = clean;  // (spans read off a smooth curve: the raw shake turns everywhere)
    // A longer stroke more spans, a denser one more again.
    EXPECT_GT(rootPathSpans(path, 0.02f), rootPathSpans(path, 0.08f));
    EXPECT_GT(rootPathSpans(path, 0.08f, 2.0f), rootPathSpans(path, 0.08f));
    EXPECT_THROW(rootTubeGuidePath(root, {0, 1, 0}, {root, root}, 1.0f, 12, 0.08f), std::invalid_argument);
    // Drawn dipping into the head: kept clear of it away from the root.
    std::vector<Vec3f> dipping{root};
    for (int k = 1; k < 60; ++k) {
        const float a = 1.2f * k * kPi / 180.0f;
        dipping.push_back(Vec3f(0.0f, std::cos(a), std::sin(a)) * 0.97f);
    }
    const std::vector<Vec3f> cleared = rootTubeGuidePath(root, {0, 1, 0}, dipping, 1.0f, 12, 0.08f, 1.0f, head().get());
    float along = 0.0f;
    for (std::size_t i = 1; i < cleared.size(); ++i) {
        along += (cleared[i] - cleared[i - 1]).length();
        if (along > 4.0f * 0.08f) EXPECT_GT(cleared[i].length() - 1.0f, 0.6f * 0.35f * 0.08f) << "off the scalp at " << i;
    }
}

TEST(RootTubes, ARootsGuideLeavesTheMiddleOfTheScalpItDraws) {
    // A big root on the head reaching far down one side, its CVs bunched there: its guide starts at
    // the middle of the scalp's area inside it (each part by its own area: its steep side counts as
    // much as its top), the Tube tool's middle point, not the middle of its outline's box nor of
    // its CVs. Its shape keeps that point as its origin.
    GuideHierarchy h;
    const auto scalp = head();
    std::vector<Vec3f> outline;
    for (int k = 0; k < 16; ++k) {
        const float t = 2.0f * kPi * static_cast<float>(k) / 16.0f;
        // (from the top, out to about 80 degrees down on +x, 35 degrees elsewhere)
        const float reach = (std::cos(t) > 0.0f ? 0.6f + 0.8f * std::cos(t) * std::cos(t) : 0.6f);
        outline.push_back(Vec3f{std::sin(reach) * std::cos(t), std::cos(reach), std::sin(reach) * std::sin(t)}.normalized());
    }
    const GuideId id = makeRootTube(h, *scalp, outline, {});
    const HierarchyGuide* g = h.guide(id);
    ASSERT_TRUE(g && g->tube.custom);
    EXPECT_TRUE(g->tube.custom->origin);
    // The middle by area, measured finely on the sphere: points spread evenly over the sphere,
    // those inside the drawn loop averaged.
    Vec3f sum;
    int inside = 0;
    const Vec3f c = rootOutlineCenter(*scalp, outline).point;
    for (int i = 0; i < 20000; ++i) {  // (a Fibonacci sphere)
        const float y = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / 20000.0f;
        const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const float a = 2.39996323f * static_cast<float>(i);
        const Vec3f p{r * std::cos(a), y, r * std::sin(a)};
        const float t = std::atan2(p.z, p.x);
        const float reach = (std::cos(t) > 0.0f ? 0.6f + 0.8f * std::cos(t) * std::cos(t) : 0.6f);
        if (std::acos(std::clamp(p.y, -1.0f, 1.0f)) < reach) sum += p, ++inside;
    }
    ASSERT_GT(inside, 100);
    const Vec3f middle = (sum / static_cast<float>(inside)).normalized();
    EXPECT_LT((c - middle).length(), 0.05f) << "the Tube tool's middle point is the area's middle";
    EXPECT_LT((g->points.front() - c).length(), 1e-3f) << "the guide leaves it";
    // As it was: the loop's flat middle sat well off it (toward the top).
    EXPECT_GT((Vec3f{0, 1, 0} - middle).length(), 0.2f);
}

TEST(RootTubes, TheRootStaysWhereItWasDrawnWhateverIsDoneToTheGuide) {
    GuideHierarchy h;
    const auto scalp = head();
    const GuideId id = makeRootTube(h, *scalp, ring({0, 1, 0.1f}, 0.12f), {});
    const std::vector<Vec3f> before = rootTubeOutline(h, *scalp, id);
    BrushSettings comb;
    comb.mode = BrushMode::Comb;
    comb.radius = 0.6f;
    comb.strength = 1.0f;
    const Vec3f top = h.guide(id)->points.back();
    applyBrushStroke(h, scalp.get(), comb, {top, top + Vec3f(0.0f, -0.2f, 0.6f)});
    h.evaluate(scalp.get());
    const std::vector<Vec3f> after = rootTubeOutline(h, *scalp, id);
    for (std::size_t i = 0; i < before.size(); ++i) EXPECT_LT((after[i] - before[i]).length(), 1e-5f) << "CV " << i;
    EXPECT_LT(rootFootprintError(h, *scalp, id), 0.004f) << "the footprint fixed on the scalp";
}

TEST(RootTubes, AnEditMovesTheTubeOntoTheNewOutline) {
    GuideHierarchy h;
    const auto scalp = head();
    const GuideId id = makeRootTube(h, *scalp, ring({0, 1, 0}, 0.12f, 6), {});
    const float tipWidth = h.guide(id)->tube.width.back() / h.guide(id)->tube.width.front();
    std::vector<Vec3f> cvs = rootTubeOutline(h, *scalp, id);
    for (Vec3f& p : cvs) p = onSphere(p + Vec3f(0.15f, 0.0f, 0.0f));  // the whole root moved over
    cvs[2] = onSphere(outlineCentroid(cvs) + (cvs[2] - outlineCentroid(cvs)) * 1.4f);  // one CV pulled out
    editRootTube(h, *scalp, id, cvs);
    const std::vector<Vec3f> kept = rootTubeOutline(h, *scalp, id);
    for (std::size_t i = 0; i < cvs.size(); ++i) EXPECT_LT((kept[i] - cvs[i]).length(), 0.003f) << "CV " << i;
    EXPECT_LT((h.guide(id)->points.front() - onSphere(outlineCentroid(cvs))).length(), 0.03f) << "rooted at its middle";
    for (const Vec3f& q : rootTubeSectionOutline(h, *scalp, id)) EXPECT_LT(nearest(q, kept), 0.01f);
    EXPECT_NEAR(h.guide(id)->tube.width.back() / h.guide(id)->tube.width.front(), tipWidth, 1e-3f) << "its taper kept";
    EXPECT_THROW(editRootTube(h, *scalp, id, {cvs[0], cvs[1]}), std::invalid_argument);
}

TEST(RootTubes, CvsSnapSealAndCarryOverTheScalp) {
    GuideHierarchy h;
    const auto scalp = head();
    const GuideId a = makeRootTube(h, *scalp, ring({0, 1, 0}, 0.1f, 4), {});
    const std::vector<Vec3f> cvs = rootTubeOutline(h, *scalp, a);
    // A top view, 1000 pixels a unit: a point 6 px from a CV snaps onto it, one far off stays.
    auto toScreen = [](const Vec3f& p) { return std::array<float, 2>{p.x * 1000.0f, p.z * 1000.0f}; };
    Vec3f near = cvs[1] + Vec3f(0.004f, 0.0f, 0.004f);
    EXPECT_EQ(snapRootCv(near, {cvs}, toScreen), RootSnap::Cv);
    EXPECT_LT((near - cvs[1]).length(), 1e-6f);
    Vec3f mid = (cvs[0] + cvs[1]) * 0.5f + Vec3f(0.003f, 0.0f, 0.0f);
    EXPECT_EQ(snapRootCv(mid, {cvs}, toScreen), RootSnap::Edge) << "onto an edge between CVs";
    Vec3f far = cvs[0] + Vec3f(0.2f, 0.0f, 0.2f);
    EXPECT_EQ(snapRootCv(far, {cvs}, toScreen), RootSnap::None);
    // A neighbour drawn on two of its CVs: those two are sealed.
    std::vector<Vec3f> next{cvs[0], cvs[1], onSphere(cvs[1] + (cvs[1] - cvs[2])), onSphere(cvs[0] + (cvs[0] - cvs[3]))};
    EXPECT_EQ(sealedCvs(next, {cvs}), (std::vector<int>{0, 1}));
    // Carried over the head, the outline keeps its size; held CVs stay.
    const Vec3f start = rootTubeCenter(h, *scalp, a).point;
    const std::vector<Vec3f> moved = carryOutline(*scalp, cvs, start, onSphere({0.5f, 0.85f, 0.1f}), {0});
    EXPECT_LT((moved[0] - cvs[0]).length(), 1e-6f) << "held";
    for (std::size_t i = 1; i + 1 < cvs.size(); ++i) {
        EXPECT_NEAR((moved[i + 1] - moved[i]).length(), (cvs[i + 1] - cvs[i]).length(), 0.003f) << "side " << i;
    }
}

TEST(RootTubes, ARootCopiedWithItsHierarchyAndDeleted) {
    GuideHierarchy h;
    const auto scalp = head();
    const Vec3f root = onSphere({0, 1, 0});
    std::vector<Vec3f> path{root};
    for (int k = 1; k <= 13; ++k) path.push_back(onSphere(root + Vec3f(0.0f, 0.0f, 0.06f * k)) * (1.05f + 0.01f * k));
    const GuideId a = makeRootTube(h, *scalp, ring({0, 1, 0}, 0.15f, 6, {1.0f, 0.75f, 1.2f, 0.9f, 1.1f, 0.7f}), path);
    EXPECT_EQ(addTubeLevel(h, scalp.get()), 1u);
    EXPECT_EQ(addTubeLevel(h, scalp.get()), 2u);
    EXPECT_EQ(guideCount(h), 7u);
    const GuideId b = duplicateRootTube(h, *scalp, a, onSphere({0.5f, 0.85f, 0.1f}));
    EXPECT_EQ(guideCount(h), 14u) << "the copy and its hierarchy";
    // (The drawn roots; + cut their roots too: each half a root of its own, at its level.)
    auto drawn = [&h] {
        std::vector<GuideId> out;
        for (const GuideId g : rootTubesOf(h)) {
            if (!h.guide(g)->isChild()) out.push_back(g);
        }
        return out;
    };
    EXPECT_EQ(drawn(), (std::vector<GuideId>{a, b}));
    for (const GuideId k : h.childrenOf(a)) {
        const std::vector<GuideId> all = rootTubesOf(h);
        EXPECT_NE(std::find(all.begin(), all.end(), k), all.end()) << "a half of a + is a root of its own";
        EXPECT_GE(h.guide(k)->rootOutline.size(), 3u) << "its part of its parent's root as its CVs";
    }
    const std::vector<Vec3f> o1 = rootTubeOutline(h, *scalp, a), o2 = rootTubeOutline(h, *scalp, b);
    ASSERT_EQ(o1.size(), o2.size());
    for (std::size_t i = 0; i < o1.size(); ++i) {
        const std::size_t j = (i + 1) % o1.size();
        EXPECT_NEAR((o1[j] - o1[i]).length(), (o2[j] - o2[i]).length(), 0.003f) << "the same shape";
    }
    EXPECT_LT((onSphere(outlineCentroid(o2)) - onSphere({0.5f, 0.85f, 0.1f})).length(), 0.005f) << "set down there";
    for (const Vec3f& q : rootTubeSectionOutline(h, *scalp, b)) EXPECT_LT(nearest(q, o2), 0.005f) << "its tube on its root";
    EXPECT_EQ(h.childrenOf(b).size(), 2u);
    for (const GuideId k : h.childrenOf(b)) EXPECT_EQ(h.childrenOf(k).size(), 2u);
    const std::string shape = h.guide(b)->tube.custom->name;
    EXPECT_EQ(deleteRootTubes(h, *scalp, {b}), 7u);
    EXPECT_EQ(guideCount(h), 7u);
    EXPECT_FALSE(h.tubeShape(shape)) << "its root shape went with it";
    EXPECT_EQ(drawn(), std::vector<GuideId>{a});
    EXPECT_THROW(duplicateRootTube(h, *scalp, h.childrenOf(a).front(), onSphere({0, 1, 0})), std::invalid_argument)
        << "not a root tube";
}

TEST(TubeLevels, ThinSplitTubesGatherTheirShareOfTheRoot) {
    // A root split twice, its deepest tubes then made far thinner than their share of it: every
    // strand rooted in the root still rides one of them (none falls back to a level above), its
    // place in the section inside the tube's outline (the share gathered in).
    auto h = std::make_shared<GuideHierarchy>();
    const auto scalp = head();
    makeRootTube(*h, *scalp, ring({0, 1, 0}, 0.3f, 6), {});
    EXPECT_EQ(addTubeLevel(*h, scalp.get()), 1u);
    EXPECT_EQ(addTubeLevel(*h, scalp.get()), 2u);
    const std::vector<GuideId> deepest = h->guidesInGroup(h->findGroup("rootTubes_L3"));
    ASSERT_EQ(deepest.size(), 4u);
    for (const GuideId id : deepest) {
        TubeProfile thin = h->guide(id)->tube;
        for (float& w : thin.width) w *= 0.3f;
        h->setTube(id, thin);
    }
    h->evaluate(scalp.get());
    Graph g;
    ExternalInputs ext;
    ext.set("scalp", scalp, 1);
    ext.set("hierarchy", HierarchyPtr(h), h->contentHash());
    const NodeId scalpNode = g.addNode(op::kScalp);
    const NodeId scatter = g.addNode(op::kScatter);
    g.node(scatter).params().set("density", 3000.0);
    g.connect(scalpNode, scatter, 0);
    const NodeId guides = g.addNode(op::kGuides);
    const NodeId interp = g.addNode(op::kInterpolate);
    g.connect(scatter, interp, 0);
    g.connect(guides, interp, 1);
    g.node(interp).params().set("fillTubes", true);
    EvalContext ctx;
    ctx.externals = &ext;
    const StrandsPtr out = g.evaluateStrands(interp, ctx);
    ASSERT_TRUE(out);
    const std::vector<float>* ids = out->strandChannel("tubeId");
    const std::vector<float>* u = out->strandChannel("tubeU");
    const std::vector<float>* v = out->strandChannel("tubeV");
    ASSERT_TRUE(ids && u && v);
    std::set<float> tubes;
    std::size_t riding = 0;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        if ((*ids)[s] < 0.0f) continue;
        ++riding;
        tubes.insert((*ids)[s]);
        // (Inside its slice's outline: a slice's shape reaches its box's corners, sqrt 2 out.)
        EXPECT_LT(std::hypot((*u)[s], (*v)[s]), 1.42f) << s;
    }
    EXPECT_GT(riding, 100u);
    EXPECT_EQ(tubes.size(), 4u) << "the deepest tubes take every strand of the root";
}

TEST(TubeLevels, SplitTubesOfALeaningRootStayRootedOnTheScalp) {
    // A big root split a few levels, then its guide combed over to lie almost along the scalp: its
    // slices' roots stay on the scalp at their places in the root (before, the side of the root's
    // section rising off the head left them hanging in the air, from level 2 down).
    GuideHierarchy h;
    const auto scalp = head();
    const GuideId a = makeRootTube(h, *scalp, ring({0, 1, 0}, 0.35f, 6), {});
    EXPECT_EQ(addTubeLevel(h, scalp.get()), 1u);
    EXPECT_EQ(addTubeLevel(h, scalp.get()), 2u);
    EXPECT_EQ(addTubeLevel(h, scalp.get()), 4u);
    std::vector<Vec3f> leaning;
    const Vec3f root = h.guide(a)->points.front();
    for (int i = 0; i < 8; ++i) leaning.push_back(root + Vec3f(0.25f * i, 0.04f * i, 0.0f));  // ~10 degrees off the scalp
    h.setPoints(a, leaning);
    h.evaluate(scalp.get());
    std::size_t children = 0;
    for (const HierarchyGuide& g : h.guides()) {
        if (!g.isChild()) continue;
        ++children;
        const Vec3f first = g.points.front();
        EXPECT_LT((scalp->closestPoint(first).position - first).length(), 1e-3f) << g.id;
    }
    EXPECT_EQ(children, 14u);
}

TEST(TubeLevels, AGuidesNodeHandsTheOtherLevelsOnWithTheirRootLoops) {
    // The other levels' tubes a Guides node hands on (GuideSet::levelTubes) keep their root loop
    // and footprint: without them a Scatter's guideRadius took a big root's footprint through the
    // head, growing hair on the far side (the face, under a drawn root split into levels).
    auto h = std::make_shared<GuideHierarchy>();
    const auto scalp = head();
    const GuideId a = makeRootTube(*h, *scalp, ring({0, 1, 0}, 0.3f), {});
    EXPECT_EQ(addTubeLevel(*h, scalp.get()), 1u);
    h->evaluate(scalp.get());
    ASSERT_GE(h->guide(a)->rootRing.size(), 3u);
    Graph g;
    ExternalInputs ext;
    ext.set("hierarchy", HierarchyPtr(h), h->contentHash());
    const NodeId guides = g.addNode(op::kGuides);  // level -1: the split tubes, the root a level above
    EvalContext ctx;
    ctx.externals = &ext;
    const GuidesPtr out = valueAs<GuidesPtr>(g.evaluate(guides, ctx));
    ASSERT_TRUE(out && out->levelTubes());
    const auto found = std::find_if(out->levelTubes()->begin(), out->levelTubes()->end(),
                                    [&](const Guide& t) { return t.id == a; });
    ASSERT_NE(found, out->levelTubes()->end());
    EXPECT_EQ(found->rootRing, h->guide(a)->rootRing);
    EXPECT_EQ(found->rootAcross.length() > 0.5f, h->guide(a)->rootAcross.length() > 0.5f);
}

TEST(TubeLevels, AddSplitsTheLeavesRemoveFollowsTheSelection) {
    GuideHierarchy h;
    const auto scalp = head();
    const GuideId a = makeRootTube(h, *scalp, ring({0, 1, 0}, 0.1f), {});
    makeRootTube(h, *scalp, ring({0.4f, 0.9f, 0.0f}, 0.1f), {});
    EXPECT_EQ(addTubeLevel(h, scalp.get()), 2u);
    EXPECT_EQ(addTubeLevel(h, scalp.get()), 4u);
    TubeLevels levels = tubeLevels(h);
    EXPECT_EQ(levels.base, kRootTubeGroup);
    EXPECT_EQ(levels.levels, (std::vector<std::string>{"rootTubes", "rootTubes_L2", "rootTubes_L3"}));
    EXPECT_EQ(h.guidesInGroup(h.findGroup("rootTubes_L3")).size(), 8u);
    // Add on one tube: its four leaves split.
    EXPECT_EQ(addTubeLevel(h, scalp.get(), {a}), 4u);
    EXPECT_EQ(h.guidesInGroup(h.findGroup("rootTubes_L4")).size(), 8u);
    // One level shown at a time.
    const auto shown = tubeLevelVisibility(h, kRootTubeGroup, 2);
    EXPECT_TRUE(shown.at("rootTubes_L2"));
    EXPECT_FALSE(shown.at("rootTubes"));
    EXPECT_FALSE(shown.at("rootTubes_L3"));
    // Remove on a split tube: its deepest leaves go; a level left empty goes with its group.
    EXPECT_EQ(removeTubeLevel(h, scalp.get(), kRootTubeGroup, {a}), 8u);
    EXPECT_EQ(h.findGroup("rootTubes_L4"), kNoGroup);
    // Remove with nothing selected: the deepest level everywhere.
    EXPECT_EQ(removeTubeLevel(h, scalp.get(), kRootTubeGroup), 0u);
    EXPECT_EQ(tubeLevels(h).levels.size(), 2u);
    // A plain round tube splits too, side by side.
    GuideHierarchy round;
    const GroupId g = round.addGroup("primary");
    const GuideId only = round.addGuide(g, test::straightCurve({0, 1, 0}, {0, 1, 0}, 4));
    TubeProfile tube;
    tube.width = {0.1f, 0.1f};
    round.setTube(only, tube);
    round.evaluate(scalp.get());
    EXPECT_EQ(addTubeLevel(round, scalp.get(), {}, "primary"), 1u);
    const std::vector<GuideId> kids = round.guidesInGroup(round.findGroup("primary_L2"));
    ASSERT_EQ(kids.size(), 2u);
    EXPECT_NEAR((round.guide(kids[0])->points.front() - round.guide(kids[1])->points.front()).length(), 0.1f, 0.02f);
    GuideHierarchy flatOnly;
    flatOnly.addGuide(flatOnly.addGroup("primary"), test::straightCurve({0, 1, 0}, {0, 1, 0}, 4));
    EXPECT_THROW(removeTubeLevel(flatOnly, nullptr, "primary"), std::invalid_argument) << "no level under it";
}

TEST(RootTubes, TheOutlineIsSavedAndMovesToAnotherScalp) {
    GuideHierarchy h;
    const auto scalp = head();
    const GuideId id = makeRootTube(h, *scalp, ring({0, 1, 0}, 0.12f), {});
    const std::vector<Vec3f> kept = rootTubeOutline(h, *scalp, id);
    // Saved with the guides and read back.
    GuideHierarchy loaded = GuideHierarchy::fromJson(h.toJson());
    loaded.evaluate(scalp.get());
    EXPECT_EQ(loaded.guide(id)->rootOutline.size(), kept.size());
    const std::vector<Vec3f> back = rootTubeOutline(loaded, *scalp, id);
    for (std::size_t i = 0; i < kept.size(); ++i) EXPECT_LT((back[i] - kept[i]).length(), 1e-5f);
    // Moved to a bigger head (no fit: the guide keeps its size): its CVs go with it, the root's
    // shape and size kept round its new root.
    const auto bigger = test::makeSphere(40, 80, 2.0f);
    h.transfer(*scalp, *bigger);
    h.evaluate(bigger.get());
    const std::vector<Vec3f> moved = rootTubeOutline(h, *bigger, id);
    ASSERT_EQ(moved.size(), kept.size());
    const Vec3f oldRoot = GuideHierarchy::fromJson(loaded.toJson()).guide(id)->points.front();
    EXPECT_NEAR((outlineCentroid(moved) - h.guide(id)->points.front()).length(), (outlineCentroid(kept) - oldRoot).length(), 0.01f)
        << "round its new root as round its old one";
    for (std::size_t i = 0; i < kept.size(); ++i) {
        const std::size_t j = (i + 1) % kept.size();
        EXPECT_NEAR((moved[j] - moved[i]).length(), (kept[j] - kept[i]).length(), 0.01f) << "side " << i;
        EXPECT_NEAR(moved[i].length(), 2.0f, 0.01f) << "on the new scalp";
    }
}

// A root tube's drawn root is a library shape of its own (isOwnedTubeShape), not one of the
// library's: the pickers never list it (libraryTubeShapes, tubeTargetsOf's inUse), and the library's
// edits refuse it (only the root tools change it, setOwnedTubeShape).
TEST(RootTubes, ItsDrawnRootIsNotALibraryShape) {
    GuideHierarchy h;
    const auto scalp = head();
    const GuideId id = makeRootTube(h, *scalp, ring({0, 1, 0}, 0.12f), {});
    const std::string drawn = h.guide(id)->tube.custom->name;
    ASSERT_TRUE(isOwnedTubeShape(drawn));
    EXPECT_FALSE(isOwnedTubeShape("leaf"));
    const std::vector<SectionPoint> square{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    h.setTubeShape("leaf", square, false);
    std::vector<std::string> listed;
    for (const CustomTubeShapePtr& s : h.libraryTubeShapes()) listed.push_back(s->name);
    EXPECT_EQ(listed, std::vector<std::string>{"leaf"}) << "the library as artists see it";
    EXPECT_EQ(h.tubeShapes().size(), 2u) << "the hierarchy keeps both";
    // Refused as a library shape: edited, renamed (either way), removed.
    EXPECT_THROW(h.setTubeShape(drawn, square, false), std::invalid_argument);
    EXPECT_THROW(h.renameTubeShape(drawn, "mine"), std::invalid_argument);
    EXPECT_THROW(h.renameTubeShape("leaf", std::string(kOwnedTubeShapePrefix) + "99"), std::invalid_argument);
    EXPECT_THROW(h.removeTubeShape(drawn), std::invalid_argument);
    EXPECT_THROW(h.setOwnedTubeShape("leaf", square, false), std::invalid_argument) << "only an owned name";
    EXPECT_EQ(h.guide(id)->tube.custom->name, drawn);
    EXPECT_TRUE(isRootTube(h.guide(id)->tube, 0)) << "still a root tube";
    // The Tubes section's targets: the root tube counted, its drawn root among no profiles in use.
    const TubeTargets t = tubeTargetsOf(h, {id});
    EXPECT_EQ(t.rootTubes, 1u);
    EXPECT_EQ(t.splitTubes, 0u);
    for (const auto& [name, count] : t.inUse) EXPECT_FALSE(isOwnedTubeShape(name)) << name;
}

// A Mid or Tip profile does not carry into a tube hierarchy's split levels: each split tube's root
// shape is its part of the parent's root section, and a blended tube (TubeProfile::blended) would
// leave the root ring and the overlap check (tubeOverlaps) that keep the levels sealed. So sliceTube
// starts each split tube with no Mid or Tip, an edit of the parent's leaves them so
// (carryToSlices carries width, flatness and twist only), and tubeTargetsOf counts the targets in
// split levels, for the hosts to say so.
TEST(TubeLevels, MidAndTipProfilesDoNotCarryIntoSplitLevels) {
    GuideHierarchy h;
    const auto scalp = head();
    const Vec3f root = onSphere({0, 1, 0});
    std::vector<Vec3f> path{root};
    for (int k = 1; k <= 10; ++k) path.push_back(onSphere(root + Vec3f(0.0f, 0.0f, 0.06f * k)) * (1.05f + 0.01f * k));
    const GuideId a = makeRootTube(h, *scalp, ring({0, 1, 0}, 0.15f, 6), path);
    const GuideId b = makeRootTube(h, *scalp, ring({0.5f, 0.85f, 0.1f}, 0.1f), {});
    // b with a Tip profile before it is split, a after.
    h.setTube(b, editedTube(h.guide(b)->tube, R"({"tipShape": "blade"})", h.scale(), h.guide(b)->points.size(), &h.tubeShapes()));
    ASSERT_TRUE(h.guide(b)->tube.tipShape);
    EXPECT_EQ(addTubeLevel(h, scalp.get()), 2u);
    h.setTube(a, editedTube(h.guide(a)->tube, R"({"midShape": "ribbon"})", h.scale(), h.guide(a)->points.size(), &h.tubeShapes()));
    ASSERT_TRUE(h.guide(a)->tube.midShape);
    h.evaluate(scalp.get());
    std::vector<GuideId> slices;
    for (const GuideId parent : {a, b}) {
        for (const GuideId k : h.childrenOf(parent)) {
            const HierarchyGuide& s = *h.guide(k);
            EXPECT_TRUE(isRootTube(s.tube, 0)) << "a split tube: a root of its own";
            EXPECT_FALSE(s.tube.midShape) << "the parent's Mid stays its own";
            EXPECT_FALSE(s.tube.tipShape) << "the parent's Tip stays its own";
            EXPECT_FALSE(s.tube.blended());
            slices.push_back(k);
        }
    }
    ASSERT_EQ(slices.size(), 4u);
    // The targets say which are in split levels: the split tubes and the tubes split into them.
    TubeTargets t = tubeTargetsOf(h, slices);
    EXPECT_EQ(t.splitTubes, 4u);
    EXPECT_EQ(t.rootTubes, 4u);
    t = tubeTargetsOf(h, {a});
    EXPECT_EQ(t.splitTubes, 1u) << "split into the level below";
    GuideHierarchy alone;
    const GuideId c = makeRootTube(alone, *scalp, ring({0, 1, 0}, 0.12f), {});
    EXPECT_EQ(tubeTargetsOf(alone, {c}).splitTubes, 0u);
}

}  // namespace
}  // namespace groomlab
