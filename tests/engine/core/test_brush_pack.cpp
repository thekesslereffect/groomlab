// SPDX-License-Identifier: Apache-2.0
// The brush pack: the shaping brushes' strength from root to tip, the falloff as a curve, and
// what each leaves exactly as it was when left at its default.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <stdexcept>

#include "groomlab/brush.h"
#include "groomlab/brush_catalog.h"
#include "groomlab/curve.h"
#include "groomlab/frames.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom_session.h"
#include "groomlab/random.h"
#include "groomlab/scalp_maps.h"
#include "groomlab/scalp_mirror.h"
#include "groomlab/stroke.h"
#include "test_helpers.h"

#include <nlohmann/json.hpp>

namespace groomlab {
namespace {

float lengthOf(const std::vector<Vec3f>& pts) { return arcLength(pts.data(), pts.size()); }

// Straight primaries standing on the unit plane, 1 unit tall, `segments` segments each.
struct Field {
    std::shared_ptr<const ScalpMesh> scalp = test::makePlane(8);
    GuideHierarchy h;
    GroupId primary = kNoGroup;
    GuideId add(float x, float z, int segments = 10) {
        if (primary == kNoGroup) primary = h.addGroup("primary");
        const GuideId id = h.addGuide(primary, test::straightCurve({x, 0, z}, {0, 1, 0}, segments));
        h.evaluate(scalp.get());
        return id;
    }
    const std::vector<Vec3f>& pts(GuideId id) const { return h.guide(id)->points; }
};

std::shared_ptr<const Ramp> ramp(std::initializer_list<std::pair<float, float>> keys) { return std::make_shared<const Ramp>(keys); }

// A groom on a sphere with the default graph (Scatter, Width...), 30 primaries, as a session and
// its host.
struct Session {
    GroomData data;
    std::unique_ptr<GroomSession> session;
    std::unique_ptr<SessionStrokeHost> host;
    explicit Session(const std::string& graphJson = {}) {
        data.scalp = test::makeSphere(24, 48);
        data.graphJson = graphJson;
        GuideHierarchy h;
        const GroupId g = h.addGroup("primary");
        Rng rng(3);
        for (int i = 0; i < 30; ++i) {
            const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(0.3f, 1), rng.uniform(0.2f, 1)).normalized();
            h.addGuide(g, test::straightCurve(n, n * 0.6f, 10));
        }
        h.evaluate(data.scalp.get());
        data.hierarchyJson = h.toJson();
        session = std::make_unique<GroomSession>(data);
        host = std::make_unique<SessionStrokeHost>(*session);
    }
};

ToolView frontView() { return ToolView::fromMatrix(test::lookAtProjection({0, 1.2f, 4}, {0, 0.6f, 0}, 40.0, 1.0), 800, 800); }

// Pixels across the front of the sphere, over its upper half.
std::vector<std::array<float, 2>> acrossTheFront() {
    std::vector<std::array<float, 2>> line;
    for (int k = 0; k < 12; ++k) line.push_back({300.0f + 20.0f * k, 430.0f});
    return line;
}

// ---- Root to tip ----------------------------------------------------------------------------

TEST(BrushPack, StrengthProfileOfOnesIsTheBrushWithout) {
    BrushToolSettings s;
    s.setStrengthProfile("[[0, 1], [1, 1]]");
    EXPECT_EQ(s.brush.strengthProfile, nullptr) << "1 all along is no profile";
    EXPECT_EQ(s.strengthProfileJson(), "");
    s.setStrengthProfile("[[0, 0], [1, 1]]");
    ASSERT_NE(s.brush.strengthProfile, nullptr);
    EXPECT_FLOAT_EQ((*s.brush.strengthProfile)(0.5f), 0.5f);
    EXPECT_EQ(rampFromJson(s.strengthProfileJson()), *s.brush.strengthProfile);
    s.setStrengthProfile("{\"keys\": [[0, 1], [1, 0.25]], \"linear\": true}");
    EXPECT_TRUE(s.brush.strengthProfile->straight);
    EXPECT_NE(s.strengthProfileJson().find("linear"), std::string::npos);
    s.setStrengthProfile("");
    EXPECT_EQ(s.brush.strengthProfile, nullptr);
    EXPECT_THROW(s.setStrengthProfile("not a ramp"), std::invalid_argument);
    EXPECT_THROW(s.setFalloffProfile("{\"keys\": 3}"), std::invalid_argument);
}

TEST(BrushPack, ShapingBrushesUnchangedWithoutAProfile) {
    // A default (null) profile leaves every shaping brush exactly as it was: the same points
    // to the bit as a hierarchy brushed with settings that never had a profile.
    for (const BrushMode mode : {BrushMode::Comb, BrushMode::Smooth, BrushMode::Straighten, BrushMode::Lift, BrushMode::Twist,
                                 BrushMode::Curl, BrushMode::Attract}) {
        Field a, b;
        for (Field* f : {&a, &b}) {
            f->add(0.5f, 0.5f);
            f->add(0.55f, 0.45f);
            // A bent guide, so Smooth and Straighten have work.
            GuideHierarchy& h = f->h;
            std::vector<Vec3f> pts = h.guide(h.guides()[0].id)->points;
            for (std::size_t i = 1; i < pts.size(); ++i) pts[i].x += 0.05f * std::sin(static_cast<float>(i));
            h.setPoints(h.guides()[0].id, pts);
            h.evaluate(f->scalp.get());
        }
        BrushSettings s;
        s.mode = mode;
        s.radius = 0.6f;
        s.strength = 0.7f;
        BrushToolSettings flat;
        flat.brush = s;
        flat.setStrengthProfile("[[0, 1], [0.5, 1], [1, 1]]");
        const std::vector<Vec3f> path{{0.4f, 0.7f, 0.5f}, {0.7f, 0.7f, 0.55f}};
        applyBrushStroke(a.h, a.scalp.get(), s, path);
        applyBrushStroke(b.h, b.scalp.get(), flat.brush, path);
        EXPECT_EQ(a.h.toJson(), b.h.toJson()) << brushModeName(mode);
    }
}

TEST(BrushPack, CombAndTwistReachTipsMoreThanRoots) {
    // Tips only (0 at the root, 1 at the tip): the lower half barely moves; roots only: the
    // reverse. Lengths are kept either way.
    auto brushed = [](BrushMode mode, std::shared_ptr<const Ramp> profile) {
        Field f;
        const GuideId id = f.add(0.5f, 0.5f);
        BrushSettings s;
        s.mode = mode;
        s.radius = 3.0f;
        s.hardness = 1.0f;
        s.strength = mode == BrushMode::Twist ? 1.0f : 0.5f;
        s.strengthProfile = std::move(profile);
        for (int k = 0; k < (mode == BrushMode::Twist ? 1 : 3); ++k) {
            applyBrushStamp(f.h, f.scalp.get(), s, {0.5f, 0.5f, 0.5f}, {0.3f, 0.0f, 0.0f});
        }
        return f.pts(id);
    };
    const std::vector<Vec3f> plain = brushed(BrushMode::Comb, nullptr);
    const std::vector<Vec3f> tips = brushed(BrushMode::Comb, ramp({{0.0f, 0.0f}, {1.0f, 1.0f}}));
    const std::vector<Vec3f> roots = brushed(BrushMode::Comb, ramp({{0.0f, 1.0f}, {1.0f, 0.0f}}));
    const std::size_t mid = plain.size() / 2;
    EXPECT_LT(tips[mid].x - 0.5f, 0.6f * (plain[mid].x - 0.5f)) << "tips: the middle lags";
    EXPECT_GT(roots[1].x - 0.5f, 0.0f);
    EXPECT_LT(roots.back().x - roots[mid].x, plain.back().x - plain[mid].x) << "roots: the tip follows less";
    for (const auto* pts : {&plain, &tips, &roots}) EXPECT_NEAR(lengthOf(*pts), 1.0f, 1e-3f);

    // Twist: a guide leaning over swung about its root normal, more at the tip than at the root.
    auto leaning = [](std::shared_ptr<const Ramp> profile) {
        Field f;
        const GuideId id = f.h.addGuide(f.h.addGroup("primary"), test::straightCurve({0.5f, 0, 0.5f}, Vec3f(0.6f, 0.8f, 0.0f), 10));
        f.h.evaluate(f.scalp.get());
        BrushSettings s;
        s.mode = BrushMode::Twist;
        s.radius = 3.0f;
        s.hardness = 1.0f;
        s.strengthProfile = std::move(profile);
        applyBrushStamp(f.h, f.scalp.get(), s, {0.5f, 0.5f, 0.5f}, {});
        return f.pts(id);
    };
    const std::vector<Vec3f> swung = leaning(ramp({{0.0f, 0.0f}, {1.0f, 1.0f}}));
    const std::vector<Vec3f> rigid = leaning(nullptr);
    EXPECT_NEAR(lengthOf(swung), 1.0f, 1e-3f);
    // The first segment hardly turns; the tip turns about as far as the rigid swing's.
    const float firstTurn = std::atan2(swung[1].z - 0.5f, swung[1].x - 0.5f);
    const float rigidTurn = std::atan2(rigid[1].z - 0.5f, rigid[1].x - 0.5f);
    EXPECT_LT(std::fabs(firstTurn), 0.3f * std::fabs(rigidTurn));
    EXPECT_GT(std::fabs(swung.back().z), 0.5f * std::fabs(rigid.back().z));
}

// ---- The falloff as a curve -----------------------------------------------------------------

TEST(BrushPack, FalloffCurveReplacesTheSmoothFalloff) {
    // No curve: the smooth falloff past the hardness, as always.
    for (const float h : {0.0f, 0.3f, 1.0f}) {
        for (const float d : {0.0f, 0.2f, 0.5f, 0.9f, 1.0f, 1.5f}) {
            const float x = d / 1.0f;
            float expected = 0.0f;
            if (x < 1.0f) {
                if (h >= 1.0f || x <= h) {
                    expected = 1.0f;
                } else {
                    const float t = (x - h) / (1.0f - h), s = 1.0f - t * t;
                    expected = s * s;
                }
            }
            EXPECT_EQ(brushFalloff(d, 1.0f, h), expected);
            EXPECT_EQ(brushFalloff(d, 1.0f, BrushFalloff(h, nullptr)), expected);
        }
    }
    // A straight line from 1 at the center to 0 at the edge; clamped to 0..1; 0 past the radius.
    const BrushFalloff linear(0.0f, std::make_shared<const Ramp>(Ramp{{0.0f, 1.0f}, {1.0f, 0.0f}}));
    EXPECT_NEAR(brushFalloff(0.5f, 2.0f, linear), 0.75f, 1e-6f);
    EXPECT_EQ(brushFalloff(2.0f, 2.0f, linear), 0.0f);
    const BrushFalloff over(0.0f, ramp({{0.0f, 2.0f}, {1.0f, -1.0f}}));
    EXPECT_EQ(over(0.0f, 1.0f), 1.0f);
    EXPECT_EQ(over(0.99f, 1.0f), 0.0f);
    EXPECT_NEAR(sphereWeight({0, 0, 0}, 2.0f, linear)({1.0f, 0.0f, 0.0f}), 0.5f, 1e-6f);
}

TEST(BrushPack, AFlatFalloffCurveIsAHardBrushOnScreenToo) {
    // A curve of 1 all the way to the edge weighs every point as hardness 1 does: the same
    // stroke through the screen brush gives the same guides.
    const ToolView view = frontView();
    std::string results[2];
    for (int k = 0; k < 2; ++k) {
        Session g;
        BrushTool tool;
        tool.settings.setMode("comb");
        tool.settings.screenRadius = 120.0f;
        if (k == 0) {
            tool.settings.setHardness(1.0f);
        } else {
            tool.settings.setFalloffProfile("[[0, 1], [1, 1]]");
        }
        ASSERT_TRUE(tool.stroke(*g.host, view, acrossTheFront()));
        results[k] = g.session->data().hierarchyJson;
    }
    EXPECT_EQ(results[0], results[1]);
}


// ---- Direction and Noise ----------------------------------------------------------------------

TEST(BrushPack, DirectionTurnsGuidesTowardTheStroke) {
    Field f;
    const GroupId g = f.h.addGroup("primary");
    const GuideId leaning = f.h.addGuide(g, test::straightCurve({0.5f, 0, 0.5f}, Vec3f(0.6f, 0.8f, 0.0f), 10));
    const GuideId standing = f.h.addGuide(g, test::straightCurve({0.6f, 0, 0.5f}, Vec3f(0.0f, 1.0f, 0.0f), 10));
    f.h.evaluate(f.scalp.get());
    const std::vector<Vec3f> upright = f.pts(standing);
    BrushSettings s;
    s.mode = BrushMode::Direction;
    s.radius = 3.0f;
    s.hardness = 1.0f;
    s.strength = 1.0f;
    s.collide = false;
    // Stroking along +z: each stamp turns the guide half the way about its root normal (+y).
    applyBrushStamp(f.h, f.scalp.get(), s, {0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 0.2f});
    const std::vector<Vec3f>& once = f.pts(leaning);
    const float angle = std::atan2(once.back().z - 0.5f, once.back().x - 0.5f);
    EXPECT_NEAR(angle, 0.25f * 3.14159265f, 1e-3f) << "half of the quarter turn";
    EXPECT_NEAR(once.back().y, 0.8f, 1e-4f) << "the lean is kept";
    EXPECT_NEAR(lengthOf(once), 1.0f, 1e-4f);
    EXPECT_NEAR((once.front() - Vec3f(0.5f, 0, 0.5f)).length(), 0.0f, 1e-6f);
    for (int k = 0; k < 12; ++k) applyBrushStamp(f.h, f.scalp.get(), s, {0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 0.2f});
    EXPECT_NEAR(f.pts(leaning).back().x, 0.5f, 1e-3f) << "it ends up along the stroke";
    EXPECT_GT(f.pts(leaning).back().z, 1.0f);
    EXPECT_EQ(f.pts(standing), upright) << "a guide standing up has no direction to turn";
    // Negative strength (Ctrl): away from the stroke. A still stamp does nothing.
    s.strength = -1.0f;
    for (int k = 0; k < 12; ++k) applyBrushStamp(f.h, f.scalp.get(), s, {0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 0.2f});
    EXPECT_LT(f.pts(leaning).back().z, 0.0f);
    const std::vector<Vec3f> before = f.pts(leaning);
    applyBrushStamp(f.h, f.scalp.get(), s, {0.5f, 0.5f, 0.5f}, {});
    EXPECT_EQ(f.pts(leaning), before);
}

TEST(BrushPack, NoiseJittersSeededPerGuideAndCtrlSmoothsItOut) {
    auto jittered = [](int stamps, float strength) {
        Field f;
        f.add(0.5f, 0.5f, 16);
        f.add(0.55f, 0.5f, 16);
        BrushSettings s;
        s.mode = BrushMode::Noise;
        s.radius = 3.0f;
        s.hardness = 1.0f;
        s.strength = strength;
        s.collide = false;
        for (int k = 0; k < stamps; ++k) applyBrushStamp(f.h, f.scalp.get(), s, {0.5f, 0.5f, 0.5f}, {});
        return f;
    };
    // The straight line from the root: how far the guide strays from it.
    auto stray = [](const std::vector<Vec3f>& pts) {
        float most = 0.0f;
        const Vec3f dir = Vec3f(0, 1, 0);
        for (const Vec3f& p : pts) {
            const Vec3f d = p - pts.front();
            most = std::max(most, (d - dir * d.dot(dir)).length());
        }
        return most;
    };
    Field a = jittered(10, 1.0f), b = jittered(10, 1.0f);
    const std::vector<HierarchyGuide>& guides = a.h.guides();
    ASSERT_EQ(guides.size(), 2u);
    const std::vector<Vec3f>& p0 = guides[0].points;
    const std::vector<Vec3f>& p1 = guides[1].points;
    EXPECT_EQ(a.h.toJson(), b.h.toJson()) << "the same jitter every time";
    EXPECT_GT(stray(p0), 0.02f);
    EXPECT_NEAR(lengthOf(p0), 1.0f, 1e-3f);
    EXPECT_NEAR((p0.front() - Vec3f(0.5f, 0, 0.5f)).length(), 0.0f, 1e-6f);
    // Calm at the root, and each guide its own pattern.
    EXPECT_LT((p0[1] - Vec3f(0.5f, 1.0f / 16.0f, 0.5f)).length(), 0.25f * stray(p0));
    bool differs = false;
    for (std::size_t i = 1; i < p0.size(); ++i) differs = differs || ((p0[i] - Vec3f(0.5f, 0, 0.5f)) - (p1[i] - Vec3f(0.55f, 0, 0.5f))).length() > 1e-3f;
    EXPECT_TRUE(differs);
    // Negative strength (Ctrl) smooths it out: it strays less after.
    const float noisy = stray(p0);
    BrushSettings s;
    s.mode = BrushMode::Noise;
    s.radius = 3.0f;
    s.hardness = 1.0f;
    s.strength = -1.0f;
    s.collide = false;
    for (int k = 0; k < 20; ++k) applyBrushStamp(a.h, a.scalp.get(), s, {0.5f, 0.5f, 0.5f}, {});
    EXPECT_LT(stray(a.h.guides()[0].points), 0.7f * noisy);
}

TEST(BrushPack, NewBrushesAreInTheCatalog) {
    for (const char* mode : {"direction", "noise"}) {
        const BrushEntry* e = brushEntry(mode);
        ASSERT_TRUE(e) << mode;
        EXPECT_EQ(e->category, BrushCategory::Shape);
        EXPECT_TRUE(e->hasSetting("strengthProfile")) << mode;
        BrushToolSettings s;
        s.setMode(mode);
        EXPECT_EQ(s.modeName(), mode);
    }
    EXPECT_TRUE(brushEntry("noise")->signedStrength);
    EXPECT_FALSE(brushEntry("direction")->signedStrength);
    for (const char* shaping : {"comb", "smooth", "straighten", "lift", "twist", "curl", "attract"}) {
        EXPECT_TRUE(brushEntry(shaping)->hasSetting("strengthProfile")) << shaping;
        EXPECT_TRUE(brushEntry(shaping)->hasSetting("falloffProfile")) << shaping;
    }
    EXPECT_FALSE(brushEntry("slide")->hasSetting("strengthProfile"));
    EXPECT_TRUE(brushEntry("paint")->hasSetting("falloffProfile"));
}

// ---- A setting's map (+ New Map > for a setting) ------------------------------------------------

TEST(BrushPack, WidthAndDensityBrushesAreGoneAndSayWhereTheyWent) {
    for (const char* brush : {"width", "density"}) {
        BrushToolSettings s;
        try {
            s.setMode(brush);
            ADD_FAILURE() << brush << " is no brush any more";
        } catch (const std::invalid_argument& e) {
            EXPECT_NE(std::string(e.what()).find("+ New Map > for a setting"), std::string::npos) << e.what();
        }
        EXPECT_EQ(brushEntry(brush), nullptr);
    }
    EXPECT_EQ(brushSettingSpec("paramMap"), nullptr);
    EXPECT_NE(std::string(brushEntry("erase")->hint).find("density map"), std::string::npos);
}

TEST(BrushPack, PaintableSettingsAreMappableParams) {
    const nlohmann::json specs = nlohmann::json::parse(operatorSpecsJson());
    std::set<std::string> ids;
    for (const PaintableSetting& s : paintableSettings()) {
        EXPECT_TRUE(ids.insert(s.id).second) << s.id;
        EXPECT_EQ(paintableSetting(s.id), &s);
        bool mappable = false;
        for (const nlohmann::json& op : specs) {
            if (op["type"] != s.type) continue;
            for (const nlohmann::json& p : op["params"]) mappable = mappable || (p["name"] == s.param && p.value("mappable", false));
        }
        EXPECT_TRUE(mappable) << s.type << "." << s.param;
    }
    EXPECT_STREQ(paintableSettings().front().type, "Scatter");
    EXPECT_STREQ(paintableSetting("density")->param, "density");
    EXPECT_STREQ(paintableSetting("width")->type, "Width");
    EXPECT_EQ(paintableSetting("comb"), nullptr);
    EXPECT_EQ(firstNodeOfType("", "Scatter"), "scatter") << "the default graph";
    EXPECT_EQ(firstNodeOfType("", "Curl"), "");
}

TEST(BrushPack, GrayCtrlPaintsTowardZeroOrBackUp) {
    EXPECT_EQ(grayCtrlTarget(1.0f), 0.0f);
    EXPECT_EQ(grayCtrlTarget(0.3f), 0.0f) << "not 1 - value: Ctrl lowers whatever the value";
    EXPECT_EQ(grayCtrlTarget(0.0f), 1.0f) << "painting black, Ctrl paints back up";
}

TEST(BrushPack, BindParamMapFindsOrWiresTheMap) {
    // The default graph: a Width node whose width is a number. A white vertex map wired to it.
    ScalpMaps none;
    ParamMapBinding b = bindParamMap("", none, nullptr, 7, "width", "width");
    EXPECT_EQ(b.node, "width");
    EXPECT_EQ(b.map, "width_width");
    EXPECT_EQ(b.kind, "vertices");
    ASSERT_FALSE(b.graphJson.empty());
    const nlohmann::json g = nlohmann::json::parse(b.graphJson);
    nlohmann::json value;
    for (const nlohmann::json& n : g["nodes"]) {
        if (n["name"] == "width") value = n["params"]["width"];
    }
    EXPECT_EQ(value["colorSet"], "width_width");
    EXPECT_EQ(value["min"], 0.0);
    EXPECT_NEAR(value["max"].get<double>(), 0.007, 1e-9) << "white is the width as it was";
    ASSERT_TRUE(b.maps.maps.count("width_width"));
    EXPECT_EQ(b.maps.maps.at("width_width").values, std::vector<float>(7, 1.0f));
    // Wired: found again, nothing to rewire.
    ParamMapBinding again = bindParamMap(b.graphJson, b.maps, nullptr, 7, "width", "width");
    EXPECT_EQ(again.map, "width_width");
    EXPECT_TRUE(again.graphJson.empty());
    EXPECT_EQ(paramMapOf(b.graphJson, b.maps, nullptr, "width", "width").map, "width_width");
    EXPECT_EQ(paramMapOf(b.graphJson, b.maps, nullptr, "width", "width").kind, "vertices");
    EXPECT_EQ(paramMapOf(b.graphJson, b.maps, nullptr, "noise", "amplitude").map, "") << "a number: no map";
    // A name taken already: the next free one.
    ParamMapBinding taken = bindParamMap("", b.maps, nullptr, 7, "width", "width");
    EXPECT_EQ(taken.map, "width_width1");

    // A Map node wired to the param (Maya's Graph Editor's Paint), a guide layer, an expression.
    const std::string mapNode = R"({"nodes": [{"name": "w", "type": "Width"}, {"name": "thick", "type": "Map", "params": {"name": "thick"}}],
        "connections": [{"from": "thick", "output": "luminance", "to": "w", "input": "width"}], "output": "w"})";
    ScalpMaps thick;
    thick.maps["thick"] = ScalpMapEntry{"vertices", std::vector<float>(7, 0.5f), ""};
    b = bindParamMap(mapNode, thick, nullptr, 7, "w", "width");
    EXPECT_EQ(b.map, "thick");
    EXPECT_EQ(b.kind, "vertices");
    EXPECT_TRUE(b.graphJson.empty());
    b = bindParamMap(R"({"nodes": [{"name": "s", "type": "Scatter", "params": {"density": {"guide": "dens"}}}]})", none, nullptr, 7,
                     "s", "density");
    EXPECT_EQ(b.map, "dens");
    EXPECT_EQ(b.kind, "guides");
    EXPECT_THROW(bindParamMap(R"({"nodes": [{"name": "s", "type": "Scatter", "params": {"density": {"expression": "u"}}}]})", none,
                              nullptr, 7, "s", "density"),
                 std::runtime_error);
    EXPECT_THROW(bindParamMap(R"({"nodes": [{"name": "c", "type": "Clump"}]})", none, nullptr, 7, "width", "width"), std::runtime_error);
    EXPECT_THROW(paramMapOf(R"({"nodes": [{"name": "c", "type": "Clump"}]})", none, nullptr, "width", "width"), std::runtime_error);

    // A texture map wired by its file, a guide layer by name.
    ScalpMaps tex;
    tex.maps["fine"] = ScalpMapEntry{"texture", {}, "masks/fine.png"};
    const nlohmann::json wired = nlohmann::json::parse(wireParamMap("", tex, "noise", "amplitude", "fine", "texture"));
    for (const nlohmann::json& n : wired["nodes"]) {
        if (n["name"] == "noise") EXPECT_EQ(n["params"]["amplitude"]["texture"], "masks/fine.png");
    }
    EXPECT_EQ(paramMapOf(wired.dump(), tex, nullptr, "noise", "amplitude").map, "fine");
    EXPECT_THROW(wireParamMap("", none, "noise", "amplitude", "fine", "texture"), std::runtime_error);
    const nlohmann::json layer = nlohmann::json::parse(wireParamMap("", none, "noise", "amplitude", "wild", "guides"));
    for (const nlohmann::json& n : layer["nodes"]) {
        if (n["name"] == "noise") EXPECT_EQ(n["params"]["amplitude"]["guide"], "wild");
    }
}

// The new flow end to end on a session: the map made, linked and picked in one undo step; strokes
// paint it; Ctrl lowers it (no hair where it reaches 0).
TEST(BrushPack, PaintParamMapMakesLinksPicksThenCtrlLowers) {
    Session g;
    const std::string graphBefore = g.session->data().graphJson;
    const ToolView view = frontView();
    BrushTool tool;
    tool.settings.setMode("comb");
    tool.settings.brush.value = 0.25f;
    const ParamMapBinding b = paintParamMap(*g.host, tool.settings, "scatter", "density");
    EXPECT_EQ(b.map, "scatter_density");
    EXPECT_FALSE(b.graphJson.empty()) << "made";
    ASSERT_EQ(g.host->edits().size(), 1u) << "the map and the link: one step";
    EXPECT_TRUE(g.host->edits()[0].text(StrokeField::Graph).set);
    EXPECT_TRUE(g.host->edits()[0].text(StrokeField::ScalpMaps).set);
    EXPECT_EQ(tool.settings.modeName(), "paint");
    EXPECT_EQ(tool.settings.brush.attribute, "scatter_density");
    EXPECT_EQ(tool.settings.paintKind, "vertices");
    EXPECT_EQ(tool.settings.brush.value, 1.0f);
    EXPECT_EQ(paramMapOf(g.session->data().graphJson, g.session->scalpMaps(), nullptr, "scatter", "density").map, "scatter_density");
    // Picked again: the same map, no new step.
    BrushToolSettings other;
    EXPECT_EQ(paintParamMap(*g.host, other, "scatter", "density").map, "scatter_density");
    EXPECT_EQ(g.host->edits().size(), 1u);

    // Ctrl: toward 0, hair removed where the brush passes.
    tool.settings.screenRadius = 80.0f;
    tool.settings.brush.strength = 1.0f;
    ASSERT_TRUE(tool.stroke(*g.host, view, acrossTheFront(), 1.0f, kStrokeCtrl));
    ASSERT_EQ(g.host->edits().size(), 2u);
    EXPECT_FALSE(g.host->edits()[1].text(StrokeField::Graph).set) << "a stroke paints, it wires nothing";
    {
        const std::vector<float>& values = g.session->scalpMaps().maps.at("scatter_density").values;
        ASSERT_EQ(values.size(), g.data.scalp->pointCount());
        EXPECT_LT(*std::min_element(values.begin(), values.end()), 0.5f) << "painted away under the stroke";
        EXPECT_EQ(*std::max_element(values.begin(), values.end()), 1.0f) << "white elsewhere: the density as it was";
    }
    // A value under a half still lowers with Ctrl (1 - value would paint hair back in).
    tool.settings.brush.value = 0.3f;
    ASSERT_TRUE(tool.stroke(*g.host, view, acrossTheFront(), 1.0f, kStrokeCtrl));
    {
        const std::vector<float>& values = g.session->scalpMaps().maps.at("scatter_density").values;
        EXPECT_LT(*std::min_element(values.begin(), values.end()), 0.25f);
    }
    // Undo: the strokes, then the map and its link in one step.
    ASSERT_TRUE(g.host->undo());
    ASSERT_TRUE(g.host->undo());
    EXPECT_TRUE(g.session->scalpMaps().maps.count("scatter_density"));
    ASSERT_TRUE(g.host->undo());
    EXPECT_EQ(g.session->data().graphJson, graphBefore);
    EXPECT_FALSE(g.session->scalpMaps().maps.count("scatter_density"));

    // A node the graph lacks: refused, nothing done.
    EXPECT_THROW(paintParamMap(*g.host, other, "curl", "affect"), std::runtime_error);
}

// ---- Topological symmetry -----------------------------------------------------------------------

// The sphere turned 35 degrees about y and moved, its rest pose the sphere as made: a head turned.
struct TurnedHead {
    std::shared_ptr<const ScalpMesh> rest = test::makeSphere(24, 48);
    float angle = 0.61f;
    Vec3f offset{0.4f, 0.1f, -0.2f};
    std::shared_ptr<ScalpMesh> posed;
    TurnedHead() {
        std::vector<Vec3f> pts;
        for (const Vec3f& p : rest->points()) pts.push_back(pose(p));
        posed = std::make_shared<ScalpMesh>(pts, rest->triangles());
        posed->setRestPoints(rest->points());
    }
    Vec3f pose(const Vec3f& p) const { return rotateAxisAngle(p, {0, 1, 0}, angle) + offset; }
    Vec3f unpose(const Vec3f& p) const { return rotateAxisAngle(p - offset, {0, 1, 0}, -angle); }
    // The mirror image a head-relative X mirror gives.
    Vec3f mirrored(const Vec3f& p) const { return pose(mirrorPoint(unpose(p), 0, 0.0f)); }
};

TEST(BrushPack, ScalpMirrorPairsASymmetricMeshVertexByVertex) {
    const auto sphere = test::makeSphere(24, 48);
    const ScalpMirror mirror(*sphere);
    EXPECT_EQ(mirror.pairedCount(), sphere->pointCount()) << "every vertex has its twin";
    for (std::size_t i = 0; i < sphere->pointCount(); i += 37) {
        const int j = mirror.pairs()[i];
        ASSERT_GE(j, 0);
        const Vec3f p = sphere->points()[i], q = sphere->points()[static_cast<std::size_t>(j)];
        EXPECT_NEAR((q - mirrorPoint(p, 0, 0.0f)).length(), 0.0f, 1e-4f);
    }
    // A point off the surface keeps its height on the other side.
    const Vec3f above = Vec3f(0.6f, 0.5f, 0.62f).normalized() * 1.3f;
    EXPECT_NEAR((mirror.point(*sphere, above) - mirrorPoint(above, 0, 0.0f)).length(), 0.0f, 2e-3f);
    EXPECT_NEAR((mirror.vector(*sphere, above, {0.2f, 0.1f, 0.0f}) - Vec3f(-0.2f, 0.1f, 0.0f)).length(), 0.0f, 2e-3f);
    EXPECT_EQ(ScalpMirror::of(*sphere), ScalpMirror::of(*sphere)) << "made once per scalp";
    EXPECT_FALSE(mirror.fits(*test::makeSphere(12, 24)));
}

TEST(BrushPack, ScalpMirrorFollowsTheHeadAndToleratesAsymmetry) {
    // Turned and moved: the mirror is the head's own, not world X's.
    const TurnedHead head;
    const ScalpMirror& mirror = *ScalpMirror::of(*head.posed);
    for (const Vec3f& local : {Vec3f(0.5f, 0.6f, 0.62f), Vec3f(-0.8f, 0.2f, 0.3f), Vec3f(0.1f, 0.9f, -0.4f)}) {
        const Vec3f p = head.pose(local.normalized() * 1.2f);
        EXPECT_NEAR((mirror.point(*head.posed, p) - head.mirrored(p)).length(), 0.0f, 3e-3f);
        EXPECT_GT((mirrorPoint(p, 0, 0.0f) - head.mirrored(p)).length(), 0.2f) << "world X would miss";
    }
    // A mesh not quite symmetric: vertices nudged, so some find no twin and take the surface.
    const auto base = test::makeSphere(24, 48);
    std::vector<Vec3f> pts = base->points();
    Rng rng(9);
    for (std::size_t i = 0; i < pts.size(); i += 3) pts[i] += Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)) * 0.01f;
    const ScalpMesh lumpy(pts, base->triangles());
    const ScalpMirror rough(lumpy);
    EXPECT_LT(rough.pairedCount(), lumpy.pointCount());
    EXPECT_GT(rough.pairedCount(), 0u);
    const Vec3f p = Vec3f(0.55f, 0.5f, 0.6f).normalized();
    EXPECT_NEAR((rough.point(lumpy, p) - mirrorPoint(p, 0, 0.0f)).length(), 0.0f, 0.03f);
}

TEST(BrushPack, TopologySymmetryMirrorsStrokesOnATurnedHead) {
    BrushToolSettings s;
    s.setSymmetry("topology");
    EXPECT_TRUE(s.brush.mirrorTopology);
    EXPECT_EQ(s.brush.mirrorAxis, -1);
    EXPECT_EQ(s.symmetryName(), "topology");
    s.setSymmetry("x");
    EXPECT_FALSE(s.brush.mirrorTopology);
    EXPECT_EQ(s.symmetryName(), "x");
    EXPECT_THROW(s.setSymmetry("sideways"), std::invalid_argument);

    // A guide and its mirror twin on the turned head; a comb on one moves the other as its mirror image.
    const TurnedHead head;
    GuideHierarchy h;
    const GroupId g = h.addGroup("primary");
    const Vec3f local = Vec3f(0.5f, 0.6f, 0.62f).normalized();
    const Vec3f twin = mirrorPoint(local, 0, 0.0f);
    const GuideId a = h.addGuide(g, test::straightCurve(head.pose(local), head.pose(local * 1.5f) - head.pose(local), 10));
    const GuideId b = h.addGuide(g, test::straightCurve(head.pose(twin), head.pose(twin * 1.5f) - head.pose(twin), 10));
    h.evaluate(head.posed.get());
    BrushSettings brush;
    brush.mode = BrushMode::Comb;
    brush.radius = 0.35f;
    brush.mirrorTopology = true;
    const Vec3f from = head.pose(local * 1.3f), to = head.pose(local * 1.3f + Vec3f(0.0f, 0.25f, 0.0f));
    applyBrushStroke(h, head.posed.get(), brush, {from, to});
    const std::vector<Vec3f>& pa = h.guide(a)->points;
    const std::vector<Vec3f>& pb = h.guide(b)->points;
    EXPECT_GT((head.unpose(pa.back()) - local * 1.5f).length(), 0.05f) << "combed";
    for (std::size_t i = 0; i < pa.size(); ++i) {
        EXPECT_NEAR((head.unpose(pb[i]) - mirrorPoint(head.unpose(pa[i]), 0, 0.0f)).length(), 0.0f, 0.01f) << i;
    }
    // Place: a guide and its twin, rooted at the head's mirror spot.
    BrushToolSettings place;
    place.setMode("place");
    place.setSymmetry("topology");
    place.place.matchNeighbors = false;
    GuideHierarchy placed;
    EXPECT_EQ(placeWithSymmetry(placed, *head.posed, place, head.posed->closestPoint(head.pose(Vec3f(0.7f, 0.3f, 0.65f).normalized())),
                                false),
              2u);
    ASSERT_EQ(placed.guides().size(), 2u);
    const Vec3f r0 = placed.guides()[0].points.front(), r1 = placed.guides()[1].points.front();
    EXPECT_NEAR((r1 - head.mirrored(r0)).length(), 0.0f, 0.01f);
}

TEST(BrushPack, TopologySymmetryPaintsTheTwinVertices) {
    const TurnedHead head;
    BrushToolSettings s;
    s.setMode("paint");
    s.paintKind = "vertices";
    s.brush.attribute = "mask";
    s.brush.value = 1.0f;
    s.brush.radius = 0.3f;
    s.setSymmetry("topology");
    std::vector<float> values(head.posed->pointCount(), 0.0f);
    MapCanvas canvas;
    canvas.vertices = &values;
    const Vec3f at = head.pose(Vec3f(0.6f, 0.5f, 0.62f).normalized());
    EXPECT_GT(paintMapStroke(canvas, *head.posed, MapDab::of(s, s.brush, kGrayMap, false, 1.0f), {at}, 0.3f), 0u);
    const ScalpMirror& mirror = *ScalpMirror::of(*head.posed);
    float most = 0.0f;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const int j = mirror.pairs()[i];
        ASSERT_GE(j, 0);
        most = std::max(most, values[i]);
        EXPECT_NEAR(values[i], values[static_cast<std::size_t>(j)], 0.05f) << i;
    }
    EXPECT_GT(most, 0.3f);
}

// ---- Solo: new guides, Shift+click, Unfreeze All ---------------------------------------------------

// A guide's freeze value as the brushes read it (inherited too); 0 without one.
float frozen(const GuideHierarchy& h, GuideId id) {
    float v = 0.0f;
    h.effectiveAttribute(*h.guide(id), "freeze", &v);
    return v;
}

TEST(BrushPack, SoloFreezesEveryOtherGuideAndUnfreezeAllFreesThem) {
    Field f;
    const GuideId a = f.add(0.3f, 0.3f), b = f.add(0.7f, 0.7f);
    const GroupId kids = f.h.generateChildren(f.primary, "children", {});
    f.h.evaluate(f.scalp.get());
    soloGuides(f.h, {a});
    EXPECT_EQ(frozen(f.h, a), 0.0f);
    EXPECT_EQ(frozen(f.h, b), 1.0f);
    for (const GuideId c : f.h.guidesInGroup(kids)) {
        EXPECT_EQ(frozen(f.h, c), f.h.guide(c)->parent == a ? 0.0f : 1.0f) << "children go with their parent";
    }
    // A comb over both moves the soloed guide alone.
    const std::vector<Vec3f> before = f.pts(b);
    BrushSettings s;
    s.radius = 2.0f;
    applyBrushStroke(f.h, f.scalp.get(), s, {{0.5f, 0.8f, 0.5f}, {0.9f, 0.8f, 0.5f}});
    EXPECT_EQ(f.pts(b), before);
    EXPECT_GT(f.pts(a).back().x, 0.4f);
    EXPECT_TRUE(unfreezeAll(f.h));
    EXPECT_EQ(frozen(f.h, b), 0.0f);
    EXPECT_FALSE(unfreezeAll(f.h)) << "nothing frozen any more";
}

TEST(BrushPack, PlaceSolosItsNewGuidesInTheSameStep) {
    Session g;
    const ToolView view = frontView();
    BrushTool tool;
    tool.settings.setMode("place");
    tool.settings.place.soloNewGuides = true;
    tool.settings.place.matchNeighbors = false;
    ASSERT_TRUE(tool.stroke(*g.host, view, {{400.0f, 430.0f}}));
    ASSERT_EQ(g.host->edits().size(), 1u) << "one undo step for the guide and the freeze";
    const GuideHierarchy h = GuideHierarchy::fromJson(g.session->data().hierarchyJson);
    ASSERT_EQ(h.guides().size(), 31u);
    const GuideId made = h.guides().back().id;
    for (const HierarchyGuide& guide : h.guides()) EXPECT_EQ(frozen(h, guide.id), guide.id == made ? 0.0f : 1.0f);
    ASSERT_TRUE(g.host->undo());
    EXPECT_EQ(GuideHierarchy::fromJson(g.session->data().hierarchyJson).attributeNames().size(), 0u);
    // Off (the core's default, and scripts'): nothing is frozen.
    BrushToolSettings s;
    s.setMode("place");
    scriptedStroke(*g.host, s, {Vec3f(0.2f, 0.9f, 0.4f).normalized()});
    EXPECT_EQ(GuideHierarchy::fromJson(g.session->data().hierarchyJson).attributeNames().size(), 0u);
    s.place.soloNewGuides = true;
    scriptedStroke(*g.host, s, {Vec3f(-0.2f, 0.9f, 0.4f).normalized()});
    const GuideHierarchy after = GuideHierarchy::fromJson(g.session->data().hierarchyJson);
    EXPECT_EQ(frozen(after, after.guides().back().id), 0.0f);
    EXPECT_EQ(frozen(after, after.guides().front().id), 1.0f);
}

TEST(BrushPack, AShiftClickDoesNothingAShiftDragSmooths) {
    // Shift is smoothing alone: a Shift+click (pressed and released in place) starts no stroke,
    // commits nothing and leaves the brush's group as it was (selecting guides is the hosts'
    // Ctrl+Shift).
    Session g;
    const ToolView view = frontView();
    BrushTool tool;
    tool.settings.setMode("comb");
    tool.settings.screenRadius = 40.0f;
    tool.settings.brush.group = "nothing";
    tool.press(*g.host, view, 400.0f, 450.0f, 1.0f, kStrokeShift);
    EXPECT_FALSE(tool.stroking()) << "no stroke starts on the click";
    EXPECT_EQ(tool.release(*g.host, view), StrokeRelease::None);
    EXPECT_TRUE(g.host->edits().empty());
    EXPECT_EQ(tool.settings.brush.group, "nothing");
}

TEST(BrushPack, FreezeToSelectionFollowsTheSelection) {
    Field f;
    const GuideId a = f.add(0.2f, 0.2f), b = f.add(0.5f, 0.5f), c = f.add(0.8f, 0.8f);
    EXPECT_TRUE(freezeToSelection(f.h, {a, b}));
    EXPECT_EQ(frozen(f.h, a), 0.0f);
    EXPECT_EQ(frozen(f.h, b), 0.0f);
    EXPECT_EQ(frozen(f.h, c), 1.0f);
    EXPECT_FALSE(freezeToSelection(f.h, {b, a})) << "the same set: nothing changes";
    EXPECT_TRUE(freezeToSelection(f.h, {c}));  // a selection made with a modifier: the set it gives
    EXPECT_EQ(frozen(f.h, a), 1.0f);
    EXPECT_EQ(frozen(f.h, c), 0.0f);
    EXPECT_TRUE(freezeToSelection(f.h, {})) << "nothing selected: every guide unfrozen";
    EXPECT_TRUE(f.h.attributeNames().empty());
    EXPECT_FALSE(freezeToSelection(f.h, {}));
}

TEST(BrushPack, SoloHonoursSymmetry) {
    // Mirror guides: across world X (here x = 0.5 on the plane), and the head's own topology.
    Field f;
    const GuideId a = f.add(0.3f, 0.4f), b = f.add(0.7f, 0.4f), c = f.add(0.5f, 0.9f);
    BrushSettings x;
    x.mirrorAxis = 0;
    x.mirrorCenter = 0.5f;
    const StrokeSymmetry sym = StrokeSymmetry::of(x, f.scalp.get());
    EXPECT_EQ(mirrorGuide(f.h, a, sym), b);
    EXPECT_EQ(mirrorGuide(f.h, c, sym), c) << "on the seam: its own mirror";
    EXPECT_EQ(withMirrorGuides(f.h, {a, c}, sym), (std::vector<GuideId>{a, b, c}));
    EXPECT_EQ(withMirrorGuides(f.h, {a}, StrokeSymmetry::of(BrushSettings{}, f.scalp.get())), std::vector<GuideId>{a});
    EXPECT_EQ(mirrorGuide(f.h, a, StrokeSymmetry::of(BrushSettings{}, f.scalp.get())), kNoGuide);

    // The turned head, topology: a guide selected (as the hosts' Ctrl+Shift picks, its mirror
    // guide with it) and the rest frozen: it and its twin stay live.
    const TurnedHead head;
    GuideHierarchy h;
    const GroupId g = h.addGroup("primary");
    const Vec3f local = Vec3f(0.3f, 0.75f, 0.6f).normalized(), other = Vec3f(-0.1f, 0.3f, 0.95f).normalized();
    for (const Vec3f& p : {local, mirrorPoint(local, 0, 0.0f), other}) {
        h.addGuide(g, test::straightCurve(head.pose(p), head.pose(p * 1.4f) - head.pose(p), 10));
    }
    h.evaluate(head.posed.get());
    BrushToolSettings topology;
    topology.setSymmetry("topology");
    const std::vector<GuideId> picked =
        withMirrorGuides(h, {h.guides()[0].id}, StrokeSymmetry::of(topology.brush, head.posed.get()));
    EXPECT_TRUE(freezeToSelection(h, picked));
    ASSERT_EQ(h.guides().size(), 3u);
    EXPECT_EQ(frozen(h, h.guides()[0].id), 0.0f);
    EXPECT_EQ(frozen(h, h.guides()[1].id), 0.0f) << "its mirror twin stays live too";
    EXPECT_EQ(frozen(h, h.guides()[2].id), 1.0f);
}

TEST(BrushPack, PlaceWithSymmetrySolosBothNewGuides) {
    Session g;
    BrushToolSettings s;
    s.setMode("place");
    s.setSymmetry("x");
    s.place.soloNewGuides = true;
    scriptedStroke(*g.host, s, {Vec3f(0.4f, 0.8f, 0.45f).normalized()});
    const GuideHierarchy h = GuideHierarchy::fromJson(g.session->data().hierarchyJson);
    ASSERT_EQ(h.guides().size(), 32u) << "the guide and its mirror image";
    std::size_t live = 0;
    for (const HierarchyGuide& guide : h.guides()) live += frozen(h, guide.id) == 0.0f;
    EXPECT_EQ(live, 2u);
    EXPECT_EQ(frozen(h, h.guides()[30].id), 0.0f);
    EXPECT_EQ(frozen(h, h.guides()[31].id), 0.0f);
}

}  // namespace
}  // namespace groomlab
