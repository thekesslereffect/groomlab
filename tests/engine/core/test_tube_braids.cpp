// SPDX-License-Identifier: Apache-2.0
// Root tubes made braids (issue 129, tube_braids.h): some tubes plain hair, some braided by a
// Braid joining by tube, at any level of the tube hierarchy, the outermost mark of a lineage
// winning, each braid numbered from its tube's ID; the mark saved with the hierarchy; Make Braid's
// graph edit (graph_inserts.h insertTubeBraid).

#include <algorithm>
#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <set>

#include <nlohmann/json.hpp>

#include "groomlab/graph.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_inserts.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/operators.h"
#include "groomlab/sculpt.h"
#include "groomlab/tube.h"
#include "groomlab/tube_braids.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

std::vector<Vec3f> standing(const Vec3f& root, float length = 1.0f, int segments = 16) {
    std::vector<Vec3f> pts;
    for (int i = 0; i <= segments; ++i) pts.push_back(root + Vec3f{0, length * static_cast<float>(i) / segments, 0});
    return pts;
}

TubeProfile roundTube(float width) {
    TubeProfile t;
    t.width = {width};
    return t;
}

// A tube guide standing up out of the plane at (x, z).
Guide& addTube(GuideSet& set, std::uint64_t id, float x, float z, float width) {
    Guide& g = set.add(id, standing({x, 0, z}));
    g.hasTube = true;
    g.tube = roundTube(width);
    g.frameUp = {1, 0, 0};
    return g;
}

// A tube of the level below, inside its parent's.
Guide levelTube(std::uint64_t id, std::uint64_t parent, float x, float z, float width, bool braid = false) {
    Guide g;
    g.id = id;
    g.points = standing({x, 0, z});
    g.hasTube = true;
    g.tube = roundTube(width);
    g.frameUp = {1, 0, 0};
    g.depth = 1;
    g.baseId = parent;
    g.parentId = parent;
    g.braid = braid;
    return g;
}

// Strands on the unit plane filling the guides' tubes (Interpolate fillTubes), then a Braid
// joining by tube reading the same guides.
struct BraidGraph {
    Graph g;
    ExternalInputs ext;
    NodeId interp = 0, braid = 0;
    std::uint64_t version = 1;

    BraidGraph() {
        ext.set("scalp", test::makePlane(8), 1);
        const NodeId scalp = g.addNode(op::kScalp);
        const NodeId scatter = g.addNode(op::kScatter);
        g.node(scatter).params().set("density", 20000.0);
        g.connect(scalp, scatter, 0);
        const NodeId guides = g.addNode(op::kGuides);
        interp = g.addNode(op::kInterpolate);
        g.connect(scatter, interp, 0);
        g.connect(guides, interp, 1);
        g.node(interp).params().set("fillTubes", true);
        braid = g.addNode(op::kBraid);
        g.connect(interp, braid, 0);
        g.connect(guides, braid, 1);
        g.node(braid).params().set("join", std::string("tube"));
        g.node(braid).params().set("gather", 0.05);
    }
    void setGuides(GuidesPtr set) { ext.set("guides", std::move(set), ++version); }
    StrandsPtr eval(NodeId id) {
        EvalContext ctx;
        ctx.externals = &ext;
        return g.evaluateStrands(id, ctx);
    }
};

// Each strand's channel `name` (null: none).
const std::vector<float>& channel(const StrandsPtr& s, const char* name) {
    const std::vector<float>* c = s->strandChannel(name);
    if (!c) throw std::runtime_error(std::string("no channel ") + name);
    return *c;
}

float tubeNumber(const StrandsPtr& filled, float x, float z) {
    // The tubeId of the strand rooted nearest (x, z).
    float best = 1e30f, number = -1.0f;
    const std::vector<float>& ids = channel(filled, "tubeId");
    for (std::size_t s = 0; s < filled->strandCount(); ++s) {
        const Vec3f r = filled->strandPoints(s)[0];
        const float d = std::hypot(r.x - x, r.z - z);
        if (d < best) best = d, number = ids[s];
    }
    return number;
}

TEST(TubeBraids, TheOutermostMarkOfALineageWins) {
    GuideSet set;
    addTube(set, 1, 0.3f, 0.5f, 0.2f).braid = false;
    addTube(set, 2, 0.7f, 0.5f, 0.2f).braid = true;
    std::vector<Guide> levels{levelTube(3, 1, 0.3f, 0.5f, 0.08f, true), levelTube(4, 2, 0.7f, 0.5f, 0.08f, true),
                              levelTube(5, 3, 0.3f, 0.5f, 0.04f, true)};
    set.setLevelTubes(std::make_shared<const std::vector<Guide>>(levels));
    const TubeBraids b = tubeBraids(set);
    ASSERT_EQ(b.braids.size(), 2u) << "tube 2 (covering 4) and tube 3 (covering 5)";
    EXPECT_EQ(b.braids[0]->id, 2u);
    EXPECT_EQ(b.braids[1]->id, 3u);
    EXPECT_EQ(b.of(1), -1) << "an unmarked tube above a braid is in none";
    EXPECT_EQ(b.of(2), 0);
    EXPECT_EQ(b.of(4), 0) << "a mark under a marked tube is covered";
    EXPECT_EQ(b.of(3), 1);
    EXPECT_EQ(b.of(5), 1);
    ASSERT_EQ(b.numbers.size(), 2u);
    EXPECT_NE(b.numbers[0], b.numbers[1]);

    // Nothing marked: no braids.
    GuideSet plain;
    addTube(plain, 1, 0.3f, 0.5f, 0.2f);
    EXPECT_TRUE(tubeBraids(plain).empty());
}

TEST(TubeBraids, EachBraidKeepsItsNumberWhenOtherTubesChange) {
    GuideSet one;
    addTube(one, 11, 0.3f, 0.5f, 0.2f).braid = true;
    const float number = tubeBraids(one).numbers.at(0);
    GuideSet more;
    addTube(more, 7, 0.1f, 0.1f, 0.1f).braid = true;  // another braid, before it
    addTube(more, 9, 0.9f, 0.9f, 0.1f);               // and a plain tube
    addTube(more, 11, 0.3f, 0.5f, 0.2f).braid = true;
    const TubeBraids b = tubeBraids(more);
    ASSERT_EQ(b.braids.size(), 2u);
    EXPECT_EQ(b.numbers[b.of(11)], number) << "numbered from its tube's ID, not its place";
}

TEST(TubeBraids, OnlyTheMarkedTubesHairIsBraided) {
    BraidGraph f;
    auto set = std::make_shared<GuideSet>();
    addTube(*set, 1, 0.3f, 0.5f, 0.16f).braid = true;
    addTube(*set, 2, 0.7f, 0.5f, 0.16f);
    f.setGuides(set);
    const StrandsPtr filled = f.eval(f.interp);
    const StrandsPtr out = f.eval(f.braid);
    ASSERT_EQ(out->strandCount(), filled->strandCount());
    const float a = tubeNumber(filled, 0.3f, 0.5f), b = tubeNumber(filled, 0.7f, 0.5f);
    ASSERT_GE(a, 0.0f);
    ASSERT_GE(b, 0.0f);
    const std::vector<float>& tubeIds = channel(filled, "tubeId");
    const std::vector<float>& tubeBraid = channel(filled, "tubeBraid");
    const std::vector<float>& braidId = channel(out, "braidId");
    const std::vector<float>& section = channel(out, "braidSection");
    const float expected = tubeBraids(*set).numbers.at(0);
    std::size_t braided = 0, plain = 0;
    std::set<int> sections;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        if (tubeIds[s] == a) {
            EXPECT_EQ(tubeBraid[s], expected);
            EXPECT_EQ(braidId[s], expected) << "braided, numbered from its tube";
            sections.insert(static_cast<int>(section[s]));
            ++braided;
        } else {
            EXPECT_EQ(tubeBraid[s], -1.0f);
            EXPECT_EQ(braidId[s], -1.0f) << "every other strand passes through";
            if (tubeIds[s] == b) {
                ++plain;
                // Its points as the tube laid them.
                ASSERT_EQ(out->strandPointCount(s), filled->strandPointCount(s));
                for (std::size_t j = 0; j < out->strandPointCount(s); ++j) {
                    EXPECT_LT((out->strandPoints(s)[j] - filled->strandPoints(s)[j]).length(), 1e-4f);
                }
            }
        }
    }
    EXPECT_GT(braided, 50u);
    EXPECT_GT(plain, 50u);
    EXPECT_EQ(sections.size(), 3u) << "a three-strand braid's sections";

    // As wide as its tube (tubeWidth 1): the braided hair stays within about the tube's width of
    // the path, half way up.
    float reach = 0.0f;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        if (braidId[s] < 0.0f) continue;
        const Vec3f* p = out->strandPoints(s);
        const Vec3f mid = p[out->strandPointCount(s) / 2];
        reach = std::max(reach, std::hypot(mid.x - 0.3f, mid.z - 0.5f));
    }
    EXPECT_LT(reach, 0.16f) << "within the tube's width";
    EXPECT_GT(reach, 0.02f);
}

TEST(TubeBraids, AMarkedSmallerTubeBraidsOnlyItsHair) {
    BraidGraph f;
    auto set = std::make_shared<GuideSet>();
    addTube(*set, 1, 0.5f, 0.5f, 0.3f);  // a big lock, plain
    std::vector<Guide> levels{levelTube(3, 1, 0.42f, 0.5f, 0.12f, true), levelTube(4, 1, 0.58f, 0.5f, 0.12f)};
    set->setLevelTubes(std::make_shared<const std::vector<Guide>>(levels));
    f.setGuides(set);
    const StrandsPtr filled = f.eval(f.interp);
    const StrandsPtr out = f.eval(f.braid);
    const float child = tubeNumber(filled, 0.42f, 0.5f), sibling = tubeNumber(filled, 0.58f, 0.5f);
    ASSERT_NE(child, sibling);
    const std::vector<float>& tubeIds = channel(filled, "tubeId");
    const std::vector<float>& braidId = channel(out, "braidId");
    std::size_t braided = 0, loose = 0;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        if (tubeIds[s] == child) {
            EXPECT_GE(braidId[s], 0.0f);
            ++braided;
        } else {
            EXPECT_EQ(braidId[s], -1.0f) << "the lock's other hair stays as it is";
            loose += tubeIds[s] == sibling ? 1 : 0;
        }
    }
    EXPECT_GT(braided, 20u);
    EXPECT_GT(loose, 20u);

    // The lock marked too: its braid takes the child's hair and the rest of the lock's, as one.
    auto both = std::make_shared<GuideSet>(*set);
    both->guides()[0].braid = true;
    f.setGuides(both);
    const StrandsPtr whole = f.eval(f.braid);
    const std::vector<float>& wholeIds = channel(whole, "braidId");
    std::set<float> numbers;
    for (std::size_t s = 0; s < whole->strandCount(); ++s) {
        if (tubeIds[s] >= 0.0f) numbers.insert(wholeIds[s]);
    }
    ASSERT_EQ(numbers.size(), 1u) << "one braid: the outermost mark";
    EXPECT_EQ(*numbers.begin(), tubeBraids(*both).numbers.at(0));
}

TEST(TubeBraids, NothingMarkedPassesTheHairThrough) {
    BraidGraph f;
    auto set = std::make_shared<GuideSet>();
    addTube(*set, 1, 0.3f, 0.5f, 0.16f);
    f.setGuides(set);
    const StrandsPtr filled = f.eval(f.interp);
    EXPECT_FALSE(filled->strandChannel("tubeBraid")) << "no braid: no channel";
    const StrandsPtr out = f.eval(f.braid);
    // The hair as it was, with the Braid's channels all the same (none braided), so what reads
    // them after it ($braidWoven: the Braids and tubes preset's Clumps) works before any mark.
    EXPECT_EQ(out->positions(), filled->positions());
    EXPECT_EQ(out->widths(), filled->widths());
    const std::vector<float>* woven = out->pointChannel("braidWoven");
    ASSERT_TRUE(woven);
    EXPECT_TRUE(std::all_of(woven->begin(), woven->end(), [](float w) { return w == 0.0f; }));
    const std::vector<float>* ids = out->strandChannel("braidId");
    ASSERT_TRUE(ids);
    EXPECT_TRUE(std::all_of(ids->begin(), ids->end(), [](float b) { return b < 0.0f; }));
}

TEST(TubeBraids, TheMarkIsSavedWithTheHierarchy) {
    const auto scalp = test::makePlane(8);
    GuideHierarchy h;
    const GroupId tubes = h.addGroup("tubes");
    const GuideId a = h.addGuide(tubes, standing({0.3f, 0, 0.5f}), roundTube(0.2f));
    const GuideId b = h.addGuide(tubes, standing({0.7f, 0, 0.5f}), roundTube(0.2f));
    h.evaluate(scalp.get());
    const std::uint64_t before = h.contentHash();
    EXPECT_EQ(h.setBraid({a}, true), 1u);
    EXPECT_EQ(h.setBraid({a}, true), 0u) << "already a braid";
    EXPECT_NE(h.contentHash(), before);
    EXPECT_EQ(braidTubeOf(h, a), a);
    EXPECT_EQ(braidTubeOf(h, b), kNoGuide);

    const GuideHierarchy loaded = GuideHierarchy::fromJson(h.toJson());
    ASSERT_TRUE(loaded.guide(a));
    EXPECT_TRUE(loaded.guide(a)->braid);
    EXPECT_FALSE(loaded.guide(b)->braid);
    const GuideSet flat = loaded.flatten(0);
    for (const Guide& g : flat.guides()) EXPECT_EQ(g.braid, g.id == a);

    EXPECT_EQ(h.setBraid({a}, false), 1u);
    EXPECT_EQ(braidTubeOf(h, a), kNoGuide);
}

TEST(TubeBraids, AChildOfAMarkedTubeIsInItsBraid) {
    const auto scalp = test::makePlane(8);
    GuideHierarchy h;
    const GroupId tubes = h.addGroup("tubes");
    const GuideId a = h.addGuide(tubes, standing({0.5f, 0, 0.5f}), roundTube(0.3f));
    h.evaluate(scalp.get());
    const GroupId level2 = h.addGroup("tubes_L2", tubes);
    const GuideId c = h.addChildGuide(level2, a, standing({0.45f, 0, 0.5f}), roundTube(0.1f));
    h.evaluate(scalp.get());
    h.setBraid({c}, true);
    EXPECT_EQ(braidTubeOf(h, c), c);
    h.setBraid({a}, true);
    EXPECT_EQ(braidTubeOf(h, c), a) << "covered by its marked parent";
    const GuideSet children = h.flattenGroup(level2);
    ASSERT_EQ(children.size(), 1u);
    EXPECT_EQ(children.guides()[0].parentId, a);
    EXPECT_TRUE(children.guides()[0].braid);
}

TEST(TubeBraids, MakeBraidAddsOneBraidAfterTheTubes) {
    const nlohmann::json graph = {
        {"nodes",
         {{{"name", "scalp"}, {"type", "Scalp"}},
          {{"name", "guides"}, {"type", "Guides"}},
          {{"name", "scatter"}, {"type", "Scatter"}},
          {{"name", "interpolate"}, {"type", "Interpolate"}},
          {{"name", "output"}, {"type", "Output"}}}},
        {"connections",
         {{{"from", "scalp"}, {"to", "scatter"}, {"input", "scalp"}},
          {{"from", "scatter"}, {"to", "interpolate"}, {"input", "strands"}},
          {{"from", "guides"}, {"to", "interpolate"}, {"input", "guides"}},
          {{"from", "interpolate"}, {"to", "output"}, {"input", "strands"}}}},
        {"output", "output"}};
    GraphEdit edit(graph.dump());
    EXPECT_EQ(tubeBraidNode(edit), "");
    const TubeBraidNode made = insertTubeBraid(edit, R"({"style": "fishtail"})");
    EXPECT_TRUE(made.made);
    ASSERT_EQ(edit.nodeType(made.braid), "Braid");
    EXPECT_EQ(tubeBraidNode(edit), made.braid);
    const nlohmann::json after = nlohmann::json::parse(edit.toJson());
    std::map<std::string, nlohmann::json> nodes;
    for (const auto& n : after["nodes"]) nodes[n["name"].get<std::string>()] = n;
    EXPECT_EQ(nodes[made.braid]["params"]["join"], "tube");
    EXPECT_EQ(nodes[made.braid]["params"]["style"], "fishtail");
    EXPECT_EQ(nodes["interpolate"]["params"]["fillTubes"], true) << "the tubes filled";
    std::map<std::string, std::string> into;  // "<to>.<input>": from
    for (const auto& c : after["connections"]) {
        into[c["to"].get<std::string>() + "." + c["input"].get<std::string>()] = c["from"].get<std::string>();
    }
    EXPECT_EQ(into[made.braid + ".strands"], "interpolate");
    EXPECT_EQ(into[made.braid + ".braids"], "guides") << "the tubes Interpolate fills";
    EXPECT_EQ(into["output.strands"], made.braid);

    const TubeBraidNode again = insertTubeBraid(edit);
    EXPECT_FALSE(again.made);
    EXPECT_EQ(again.braid, made.braid);
}

TEST(TubeSculpts, ATubeEditRecordsAndAppliesRelativeToItsTube) {
    TubeProfile fed;
    fed.width = {0.1f, 0.06f};
    fed.aspect = {0.5f};
    fed.twist = {0.0f, 0.5f};
    TubeProfile sculpted = fed;
    sculpted.width = {0.15f, 0.09f};  // 1.5 times wider
    sculpted.aspect = {0.25f};        // flatter
    sculpted.twist = {0.3f, 0.8f};    // turned
    const std::vector<TubeSculptSample> edit = recordTubeSculpt(fed, sculpted, 1.0f);
    const TubeProfile back = applyTubeSculpt(fed, edit, 1.0f);
    for (const float t : {0.0f, 0.3f, 0.7f, 1.0f}) {
        EXPECT_NEAR(back.widthAt(t), sculpted.widthAt(t), 1e-4f) << t;
        EXPECT_NEAR(back.aspectAt(t), sculpted.aspectAt(t), 1e-4f) << t;
        EXPECT_NEAR(back.twistAt(t), sculpted.twistAt(t), 1e-4f) << t;
    }
    // On another tube (the braid above changed): the same factors and turn on top of it.
    TubeProfile other = fed;
    other.width = {0.2f, 0.12f};
    const TubeProfile carried = applyTubeSculpt(other, edit, 1.0f);
    EXPECT_NEAR(carried.widthAt(0.0f), 0.3f, 1e-4f);
    EXPECT_NEAR(carried.aspectAt(0.5f), 0.25f, 1e-4f);
    // Half strength: half of it.
    EXPECT_NEAR(applyTubeSculpt(fed, edit, 0.5f).widthAt(0.0f), 0.125f, 1e-4f);
}

TEST(TubeSculpts, TubeEditsAreSavedAndOldEditsStillLoad) {
    SculptSet set;
    GuideSculpt g;
    g.samples.push_back({0.5f, 0.1f, 0.0f, 1.0f});
    g.tube.push_back({0.0f, 1.5f, 0.5f, 0.2f});
    g.tube.push_back({1.0f, 1.2f, 0.8f, 0.0f});
    set.nodes["sculpt"].guides[7] = g;
    const SculptSet back = SculptSet::fromJson(set.toJson());
    ASSERT_TRUE(back.node("sculpt"));
    EXPECT_TRUE(back.node("sculpt")->guides.at(7) == g);
    // The format before tube edits: a guide's bends alone, as an array.
    const SculptSet old = SculptSet::fromJson(R"({"sculpt": {"guides": {"7": [0.5, 0.1, 0.0, 1.0]}}})");
    ASSERT_TRUE(old.node("sculpt"));
    EXPECT_EQ(old.node("sculpt")->guides.at(7).samples.size(), 1u);
    EXPECT_TRUE(old.node("sculpt")->guides.at(7).tube.empty());
    // An edit of the bends alone saves as it always did.
    SculptSet bends;
    GuideSculpt b;
    b.samples.push_back({0.5f, 0.1f, 0.0f, 1.0f});
    bends.nodes["sculpt"].guides[7] = b;
    EXPECT_EQ(nlohmann::json::parse(bends.toJson())["sculpt"]["guides"]["7"].is_array(), true);
}

TEST(TubeSculpts, AStrokeOnATubeRecordsItsTubeEdit) {
    const auto scalp = test::makePlane(8);
    GuideSet fed;
    Guide& g = fed.add(5, standing({0.5f, 0, 0.5f}));
    g.hasTube = true;
    g.tube = roundTube(0.1f);
    g.tube.rollOnScalp = false;
    std::shared_ptr<GuideHierarchy> h = sculptHierarchy(fed, "sections", scalp.get());
    ASSERT_TRUE(h->guide(5));
    TubeProfile wider = h->guide(5)->tube;
    for (float& w : wider.width) w *= 2.0f;
    h->setTube(5, wider);
    SculptEdits edits;
    const std::vector<GuideId> recorded = recordSculptStroke(edits, fed, *h, 1.0f, scalp.get());
    ASSERT_EQ(recorded.size(), 1u);
    const GuideSculpt& e = edits.guides.at(5);
    EXPECT_TRUE(e.bendsIdentity() && e.samples.empty()) << "its points stayed";
    ASSERT_FALSE(e.tube.empty());
    EXPECT_NEAR(e.tubeAt(0.5f).width, 2.0f, 1e-3f);
    const std::shared_ptr<const GuideSet> out = applySculpts(std::make_shared<const GuideSet>(fed), edits, 1.0f, scalp.get());
    EXPECT_NEAR(out->guides()[0].tube.widthAt(0.5f), 0.2f, 1e-4f);
    EXPECT_EQ(out->guides()[0].points, fed.guides()[0].points);
}

}  // namespace
}  // namespace groomlab
