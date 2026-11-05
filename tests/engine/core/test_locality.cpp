// SPDX-License-Identifier: Apache-2.0
// Locality: an edit of the scalp in one place changes the groom only near it. Every shipped
// preset and the tests' own (tests/data/presets) is grown on a sphere (radius 10, the preset fitted
// to its size; a simulated one at frame 8, stepped from its start), then on the sphere with
// one side bulged (its points beyond x = 7.5 pushed out 6%): as a new rest shape (a modeling edit:
// the scalp re-made) and as a deformation from the rest (a pose). The strands rooted far from the
// bulge (x below -2 on the unbulged sphere: 9.5 away, over three times the widest reach a preset
// has on this scalp, its biggest clump spacing of about 2.6) come out bit for bit the same: their
// points and widths, and their strand channels but for a rare few (a group number, clumpId, that
// clashed with one the edit made or took: stableGroupNumbers). Presets whose look is global by
// nature are listed with why.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/groom_session.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/paths.h"
#include "groomlab/presets.h"
#include "groomlab/simulation.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

using json = nlohmann::json;

const std::string kShipped = GROOMLAB_SHIPPED_PRESETS;
const std::vector<std::string> kPresetDirs = {kShipped, std::string(GROOMLAB_TEST_DATA) + "/presets"};
constexpr float kRadius = 10.0f;
constexpr float kBulgeFrom = 7.5f;  // the points beyond this x move
constexpr float kBulge = 1.06f;
constexpr float kFar = -2.0f;  // strands rooted below this x must not change
constexpr float kDensity = 0.015f;

// Presets whose far strands may change with the edit, and why.
const std::map<std::string, std::string> kGlobal = {
    // Global by nature.
    {"expression_driven", "its Width reads $length / $longest: the longest strand anywhere, which keepClearance's "
                          "lift near the edit changes"},
    {"wet_hair_tangle", "its Clumps pick about `count` clumps over the scalp: the spacing re-fits to the "
                        "scalp's area, here across one of its 1% steps"},
    {"underwater_hair", "its own guides are scattered over the whole scalp, a dozen of them: the far strands blend "
                        "guides rooted in the bulge (the same at frame 0, before its Simulate steps); and its Clumps "
                        "pick about `count` clumps"},
};
// Presets this scalp cannot grow, and why.
const std::map<std::string, std::string> kCannot = {
    {"painted_maps", "needs a painted map the sphere has none of"},
    {"flow_with_guides", "needs a painted map (guideAreas) the sphere has none of"},
    {"parity_nodes", "its uvGrid Scatter needs UVs the sphere has none of"},
    {"hand_placed_feathers", "grows on the guides drawn into its feathers group, which the sphere has none of"},
    {"braids_cornrows", "braids along the paths drawn into its braids group, which the sphere has none of"},
    {"braids_french", "braids along the path drawn into its braids group, which the sphere has none of"},
    {"braids_fishtail_ponytail", "braids along the path drawn into its braids group, which the sphere has none of"},
    {"braids_loose", "braids along the path drawn into its braids group, which the sphere has none of"},
};

// The sphere; bulged: its points beyond kBulgeFrom pushed out; as a pose: on the unbulged rest
// (unbulged too, so both sides of a comparison carry a rest pose).
MeshPtr sphere(bool bulged, bool pose) {
    const auto round = test::makeSphere(24, 48, kRadius);
    if (!bulged && !pose) return round;
    std::vector<Vec3f> points = round->points();
    for (Vec3f& p : points) {
        if (bulged && p.x > kBulgeFrom) p = p * kBulge;
    }
    if (!pose) return std::make_shared<const ScalpMesh>(std::move(points), round->triangles());
    auto posed = std::make_shared<ScalpMesh>(round->withPoints(std::move(points)));
    posed->setRestPoints(round->points());
    return posed;
}

// Guides over the sphere (rings of 16, combed down), only on the side away from the bulge: a
// guide the edit moves would change, rightly, every strand that blends it, however far.
std::string guides() {
    GuideHierarchy h;
    const GroupId group = h.addGroup("primary");
    for (int i = 0; i < 16; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / 16.0f;
        if (std::cos(a) > 0.0f) continue;
        for (const float lat : {0.85f, 0.45f, 0.0f, -0.45f}) {
            const float across = std::sqrt(1.0f - lat * lat);
            const Vec3f n = Vec3f(std::cos(a) * across, lat, std::sin(a) * across).normalized();
            const Vec3f d = (n + Vec3f(0.0f, -1.2f, 0.0f)).normalized();
            std::vector<Vec3f> points;
            for (int k = 0; k <= 8; ++k) points.push_back(n * kRadius + d * (15.0f * static_cast<float>(k) / 8.0f));
            h.addGuide(group, std::move(points), TubeProfile{TubeShape::Round, {1.5f, 1.0f}});
        }
    }
    return h.toJson();
}

std::unique_ptr<GroomSession> session(const std::string& graphJson, const MeshPtr& scalp, const std::string& hierarchy) {
    GroomData data;
    data.graphJson = graphJson;
    data.hierarchyJson = hierarchy;
    data.scalp = scalp;
    auto groom = std::make_unique<GroomSession>(data);
    groom->setPathResolver(searchPathResolver(kPresetDirs));
    return groom;
}

// What changed among the strands of `before` rooted (on `scalp`, the unbulged sphere; a barb at its
// feather's root) below kFar.
struct Changes {
    std::size_t far = 0, lost = 0, moved = 0, widths = 0, channels = 0;
    float most = 0.0f;
    std::set<std::string> names;  // the channels that changed
    bool none() const { return lost + moved + widths + channels == 0; }
    // Only a rare few strands' channels (a clashed group number).
    bool fewChannels() const { return lost + moved + widths == 0 && channels <= std::max<std::size_t>(2, far / 200); }
    std::string text() const {
        std::ostringstream out;
        out << "of " << far << " far strands " << lost << " lost, " << moved << " moved (most " << most << "), " << widths
            << " widths, " << channels << " channels (";
        for (const std::string& c : names) out << c << " ";
        out << ")";
        return out.str();
    }
};
Changes farChanges(const ScalpMesh& scalp, const StrandSet& before, const StrandSet& after) {
    std::map<StrandId, std::size_t> index;
    for (std::size_t i = 0; i < after.strandCount(); ++i) index.emplace(after.ids()[i], i);
    Changes c;
    for (std::size_t i = 0; i < before.strandCount(); ++i) {
        const std::size_t n = before.strandPointCount(i);
        if (n == 0) continue;
        const StrandRoot& root = before.roots()[i];
        if (!((scalp.contains(root) ? scalp.position(root) : before.strandPoints(i)[0]).x < kFar)) continue;
        ++c.far;
        const auto it = index.find(before.ids()[i]);
        if (it == index.end()) {
            ++c.lost;
            continue;
        }
        const std::size_t j = it->second;
        if (after.strandPointCount(j) != n) {
            ++c.moved;
            continue;
        }
        if (std::memcmp(before.strandPoints(i), after.strandPoints(j), n * sizeof(Vec3f)) != 0) {
            ++c.moved;
            for (std::size_t k = 0; k < n; ++k) c.most = std::max(c.most, (before.strandPoints(i)[k] - after.strandPoints(j)[k]).length());
        }
        if (std::memcmp(before.widths().data() + before.strandBegin(i), after.widths().data() + after.strandBegin(j),
                        n * sizeof(float)) != 0) {
            ++c.widths;
        }
        bool same = true;
        for (const std::string& name : before.strandChannelNames()) {
            const std::vector<float>* theirs = after.strandChannel(name);
            const float a = (*before.strandChannel(name))[i];
            if (!theirs || std::memcmp(&a, &(*theirs)[j], sizeof a) != 0) {
                same = false;
                c.names.insert(name);
            }
        }
        c.channels += same ? 0 : 1;
    }
    return c;
}

// The preset fitted to the sphere.
std::string presetGraph(const std::string& name, const MeshPtr& scalp) {
    std::ifstream in(std::filesystem::u8path(presetPath(name, kPresetDirs)));
    std::stringstream text;
    text << in.rdbuf();
    json graph = json::parse(presetGraphJson(text.str(), presetScalpSize(*scalp)));
    return graph.dump();
}

// When a preset is looked at: a simulated one at frame 8 (stepped from its start frame), the rest
// at 0.
double secondsFor(const std::string& graph) {
    const json parsed = json::parse(graph);  // (its nodes live as long as it: not a temporary's)
    for (const json& node : parsed["nodes"]) {
        if (node["type"] == "Simulate") return 8.0 / 24.0;
    }
    return 0.0;
}

}  // namespace

TEST(Locality, AnEditOfTheScalpChangesEveryPresetOnlyNearIt) {
    const MeshPtr round = sphere(false, false);
    const std::string hierarchy = guides();
    std::size_t checked = 0;
    for (const PresetInfo& p : listPresets(kPresetDirs)) {
        if (kCannot.count(p.name)) continue;
        const std::string graph = presetGraph(p.name, round);
        const double seconds = secondsFor(graph);
        for (const bool pose : {false, true}) {
            const StrandsPtr before = session(graph, sphere(false, pose), hierarchy)->evaluate(kDensity, seconds);
            const StrandsPtr after = session(graph, sphere(true, pose), hierarchy)->evaluate(kDensity, seconds);
            ASSERT_TRUE(before && after) << p.name;
            const Changes changes = farChanges(*round, *before, *after);
            ASSERT_GT(changes.far, 100u) << p.name;
            const std::string what = p.name + (pose ? " posed: " : " re-made: ") + changes.text();
            const auto global = kGlobal.find(p.name);
            if (global != kGlobal.end()) {
                std::printf("[ global   ] %s (%s)\n", what.c_str(), global->second.c_str());
            } else if (!changes.none()) {
                EXPECT_TRUE(changes.fewChannels()) << what;
                std::printf("[ few      ] %s\n", what.c_str());
            }
        }
        ++checked;
    }
    EXPECT_GT(checked, 28u);
}

// Scatter's even roots (randomness below 1: relaxed apart, each root until those around it settle;
// an edit reaches at most kRelaxIterations x kRelaxReach spacings, 8.3 at this density, and its
// ripple dies out sooner) and a preview's evenly spread share of them, and Clumps picking their own centers evenly (by
// size) and at random: local too, re-made and posed.
TEST(Locality, EvenRootsTheirPreviewAndOwnClumpsChangeOnlyNearTheEdit) {
    const MeshPtr round = sphere(false, false);
    const std::string graph = R"({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 60.0, "randomness": 0.3, "length": 3.0}},
        {"name": "even", "type": "Clump", "params": {"clumpBy": "size", "size": 2.0, "randomness": 0.0}},
        {"name": "random", "type": "Clump", "params": {"clumpBy": "size", "size": 1.0, "randomness": 1.0, "clumpChannel": "small"}},
        {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "even", "input": "strands"}, {"from": "even", "to": "random", "input": "strands"},
        {"from": "random", "to": "out", "input": "strands"}]})";
    for (const float density : {0.2f}) {  // the preview relaxes every root (up to 1M), then thins them
        for (const bool pose : {false, true}) {
            const StrandsPtr before = session(graph, sphere(false, pose), "")->evaluate(density, 0.0);
            const StrandsPtr after = session(graph, sphere(true, pose), "")->evaluate(density, 0.0);
            ASSERT_TRUE(before && after);
            const Changes changes = farChanges(*round, *before, *after);
            ASSERT_GT(changes.far, 1000u);
            const std::string what = std::string(pose ? "posed" : "re-made") + " at density " + std::to_string(density) + ": " + changes.text();
            EXPECT_TRUE(changes.none() || changes.fewChannels()) << what;
            if (!changes.none()) std::printf("[ few      ] %s\n", what.c_str());
        }
    }
}

// The guide simulation: guides on two sides of the sphere (rooted beyond x = 5, and below
// x = -2: farther apart than anything reaches between them, 2 volume cells or the repulsion
// radius a substep, over the frames simulated), simulated with every guide-to-guide force on,
// wind, collisions and water. The edit bulges the scalp (re-made and posed) and makes the guides
// rooted in the bulge 40% longer: the far side's guides move the same, to the bit, frame by frame.
GuidesPtr simGuides(const ScalpMesh& scalp, bool edited) {
    const MeshPtr round = sphere(false, false);
    auto guides = std::make_shared<GuideSet>();
    std::uint64_t id = 1;
    for (int i = 0; i < 32; ++i) {
        for (int j = 0; j < 12; ++j) {
            const float a = 6.2831853f * static_cast<float>(i) / 32.0f, lat = -0.8f + 1.6f * static_cast<float>(j) / 11.0f;
            const float across = std::sqrt(1.0f - lat * lat);
            const SurfacePoint sp = round->closestPoint(Vec3f(std::cos(a) * across, lat, std::sin(a) * across) * kRadius);
            if (!(sp.position.x > 5.0f || sp.position.x < kFar)) continue;
            const Vec3f at = scalp.position(sp.root), n = scalp.normal(sp.root);
            const Vec3f d = (n + Vec3f(0.0f, -1.2f, 0.0f)).normalized();
            const float length = edited && sp.position.x > kBulgeFrom ? 8.4f : 6.0f;
            std::vector<Vec3f> points;
            for (int k = 0; k < 12; ++k) points.push_back(at + d * (length * static_cast<float>(k) / 11.0f));
            guides->add(id++, std::move(points)).root = sp.root;
        }
    }
    return guides;
}

TEST(Locality, AnEditOfTheGuidesAndScalpInOnePlaceSimulatesTheFarGuidesAsBefore) {
    const MeshPtr round = sphere(false, false);
    SimSettings s;
    s.collisionOffset = 0.1f;
    s.windSpeed = 40.0f;
    s.repulsion = 0.3f;
    s.repulsionRadius = 0.4f;
    s.volume = 0.5f;
    s.volumeCellSize = 0.5f;
    s.water = true;
    s.waterLevel = -6.0f;
    s.rain = 0.2f;
    constexpr int kFrames = 8;
    for (const bool pose : {false, true}) {
        std::vector<GuidesPtr> frames[2];
        for (const bool edited : {false, true}) {
            const MeshPtr scalp = sphere(edited, pose);
            const GuidesPtr guides = simGuides(scalp->hasRest() ? ScalpMesh(scalp->restPose()) : *scalp, edited);
            SimCache cache;
            cache.configure(s, 24.0);
            cache.start({guides, scalp, {}}, 1);
            for (int f = 0; f < kFrames; ++f) {
                ASSERT_TRUE(cache.step({guides, scalp, {}}, 1));
                frames[edited].push_back(cache.frame(cache.lastFrame()));
            }
        }
        std::size_t far = 0, moved = 0, changed = 0;
        float most = 0.0f;
        for (int f = 0; f < kFrames; ++f) {
            const GuideSet& a = *frames[0][static_cast<std::size_t>(f)];
            const GuideSet& b = *frames[1][static_cast<std::size_t>(f)];
            ASSERT_EQ(a.size(), b.size());
            for (std::size_t g = 0; g < a.size(); ++g) {
                if (!(round->position(a.guides()[g].root).x < kFar)) continue;
                const std::vector<Vec3f>& p = a.guides()[g].points;
                const std::vector<Vec3f>& q = b.guides()[g].points;
                if (f == kFrames - 1) {
                    ++far;
                    moved += p != frames[0][0]->guides()[g].points ? 1 : 0;
                }
                if (std::memcmp(p.data(), q.data(), p.size() * sizeof(Vec3f)) == 0) continue;
                ++changed;
                for (std::size_t k = 0; k < p.size(); ++k) most = std::max(most, (p[k] - q[k]).length());
            }
        }
        std::printf("[ sim      ] %s: of %zu far guides (%zu moving) %zu changed over %d frames (most %g)\n",
                    pose ? "posed" : "re-made", far, moved, changed, kFrames, most);
        EXPECT_GT(far, 50u);
        EXPECT_GT(moved, far / 2);
        EXPECT_EQ(changed, 0u);
    }
}

// Which node of a preset (LOCALITY_PRESET; LOCALITY_POSE set: posed) changes far strands: a
// diagnosis, skipped without it.
TEST(Locality, NodeByNode) {
    const char* name = std::getenv("LOCALITY_PRESET");
    if (!name) GTEST_SKIP() << "set LOCALITY_PRESET to a preset's name";
    const bool pose = std::getenv("LOCALITY_POSE") != nullptr;
    const MeshPtr round = sphere(false, false);
    const std::string hierarchy = guides();
    const std::string graph = presetGraph(name, round);
    auto a = session(graph, sphere(false, pose), hierarchy);
    auto b = session(graph, sphere(true, pose), hierarchy);
    const json nodes = json::parse(graph)["nodes"];
    for (const json& node : nodes) {
        ViewRequest r;
        r.density = kDensity;
        r.time = secondsFor(graph);
        r.viewNode = node["name"].get<std::string>();
        const StrandsPtr sa = a->evaluate(r).strands;
        const StrandsPtr sb = b->evaluate(r).strands;
        if (sa && sb) std::printf("%-20s %s\n", r.viewNode.c_str(), farChanges(*round, *sa, *sb).text().c_str());
    }
}

}  // namespace groomlab
