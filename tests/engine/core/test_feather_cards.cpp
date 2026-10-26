// SPDX-License-Identifier: Apache-2.0
// Feather cards: a Propagate node's feathers carded one strip per feather along its shaft, in
// the vane plane, sharing baked feathers mirrored left to right; down feathers crossed.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>

#include <nlohmann/json.hpp>

#include "groomlab/card_files.h"
#include "groomlab/card_presets.h"
#include "groomlab/cards.h"
#include "groomlab/curve.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/groom_session.h"
#include "groomlab/operators.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

// A 4 x 4 plane (normal +Y) with shafts lying just above it along +X: their vanes lie in the
// plane y = 0.05, the right vane (side +1) toward +Z.
struct FeatherScene {
    MeshPtr scalp = test::makePlane(8, 4.0f);
    ExternalInputs ext;
    FeatherScene() { ext.set("scalp", scalp, 1); }

    // One shaft per (z, length), from x = 0.5 along +X.
    StrandsPtr shafts(const std::vector<std::pair<float, float>>& zLength, StrandId firstId = 100) const {
        auto s = std::make_shared<StrandSet>();
        StrandId id = firstId;
        for (const auto& [z, length] : zLength) {
            std::vector<Vec3f> pts;
            for (int i = 0; i <= 10; ++i) pts.push_back({0.5f + length * static_cast<float>(i) / 10.0f, 0.05f, z});
            s->addStrand(id++, pts, {}, scalp->closestPoint(pts[0]).root);
        }
        return s;
    }
    // Propagate's barbs off `shafts`: flat vanes (no curl, droop or jitter), angle 45; `lift` and
    // `droop` (degrees) raise or bend them out of the vane.
    StrandsPtr feathers(const StrandsPtr& shafts, float asymmetry = 0.0f, const std::string& sides = "both",
                        std::int64_t count = 20, double lift = 0.0, double droop = 0.0) const {
        auto op = OperatorRegistry::instance().create(op::kPropagate);
        op->params().set("lift", lift);
        op->params().set("sides", sides);
        op->params().set("count", count);
        op->params().set("angle", 45.0);
        op->params().set("length", 0.3);
        op->params().set("lengthProfile", Ramp::constant(1.0f));
        op->params().set("start", 0.1);
        op->params().set("end", 0.9);
        op->params().set("curl", 0.0);
        op->params().set("droop", droop);
        op->params().set("lengthRandom", 0.0);
        op->params().set("angleRandom", 0.0);
        op->params().set("asymmetry", static_cast<double>(asymmetry));
        EvalContext ctx;
        ctx.externals = &ext;
        return valueAs<StrandsPtr>(op->evaluate(ctx, {Value(shafts)}));
    }
    static StrandsPtr merge(const StrandsPtr& a, const StrandsPtr& b) {
        auto op = OperatorRegistry::instance().create(op::kMerge);
        EvalContext ctx;
        return valueAs<StrandsPtr>(op->evaluate(ctx, {Value(a), Value(b)}));
    }
};

// Clumps of hair standing up off the plane at x = 3 (clumpId per clump, no feather channels).
StrandsPtr hairTufts(const ScalpMesh& scalp, int clumps = 4, bool clumpIds = true) {
    auto s = std::make_shared<StrandSet>();
    std::vector<float> ids;
    StrandId id = 9000;
    for (int c = 0; c < clumps; ++c) {
        for (int k = 0; k < 12; ++k) {
            const float a = 0.5236f * static_cast<float>(k);
            const Vec3f root{3.0f + 0.03f * std::cos(a), 0.0f, 0.5f + 0.8f * static_cast<float>(c) + 0.03f * std::sin(a)};
            s->addStrand(id++, test::straightCurve(root, {0.02f * std::cos(a), 0.6f, 0.0f}, 6), {},
                         scalp.closestPoint(root).root);
            ids.push_back(static_cast<float>(c));
        }
    }
    if (clumpIds) s->addStrandChannel("clumpId", -1.0f) = ids;
    return s;
}

float channel(const StrandSet& s, const char* name, std::size_t i) { return (*s.strandChannel(name))[i]; }

// Flat strips unless `spine` (the tests before featherSpine keep checking the flat cards, which
// spine off leaves exactly as they were).
CardSettings featherSettings(bool spine = false) {
    CardSettings settings;
    settings.variations = 0;
    settings.featherSegments = 6;
    settings.featherSpine = spine;
    return settings;
}

TEST(FeatherCards, OneCardPerFeatherLyingInTheVanePlaneAroundItsBarbs) {
    FeatherScene scene;
    const StrandsPtr s = scene.feathers(scene.shafts({{0.5f, 1.0f}, {1.0f, 0.9f}, {1.5f, 1.1f}, {2.0f, 1.0f}, {2.5f, 0.8f}}), 0.3f);
    const CardMesh mesh = buildCards(*s, scene.scalp.get(), featherSettings());
    ASSERT_EQ(mesh.cards.size(), 5u);  // one per shaft, both vanes on it
    std::set<int> feathers;
    for (std::size_t c = 0; c < mesh.cards.size(); ++c) {
        const CardMesh::Card& card = mesh.cards[c];
        SCOPED_TRACE("card " + std::to_string(c));
        feathers.insert(card.feather);
        EXPECT_EQ(card.strands.size(), 41u);  // 20 barbs a side and the rachis
        EXPECT_EQ(card.stations, 7);
        EXPECT_EQ(card.across, 1);
        // In the vane plane: facing +Y (out of the vane's top), every vertex at the vane's height.
        for (const Vec3f& n : card.normal) EXPECT_GT(n.y, 0.99f);
        for (std::size_t v = card.firstVertex; v < card.firstVertex + 2u * 7u; ++v) EXPECT_NEAR(mesh.positions[v].y, 0.05f, 1e-3f);
        // The right vane (+Z) is the longer: the card's middle sits off the shaft toward it.
        const float shaftZ = s->strandPoints(card.strands.front())[0].z;
        EXPECT_GT(card.center[3].z - shaftZ, 0.02f);
        // The card runs from the quill past the farthest barb tip.
        float farthest = 0.0f;
        for (const std::size_t si : card.strands) farthest = std::max(farthest, s->strandPoints(si)[s->strandPointCount(si) - 1].x);
        EXPECT_GE(card.center.back().x, farthest);
        EXPECT_LE(card.center.front().x, 0.5f + 1e-4f);
    }
    EXPECT_EQ(feathers, (std::set<int>{0, 1, 2, 3, 4}));
    // It bounds its barbs and rachis: every sample lies on it.
    const std::vector<float> distances = cardFitDistances(*s, mesh, 12);
    ASSERT_FALSE(distances.empty());
    EXPECT_LT(distances.back(), 2e-3f);
    // The cards bend with the shaft: a shaft curving up bends its card along.
    auto bent = std::make_shared<StrandSet>();
    std::vector<Vec3f> arc;
    for (int i = 0; i <= 10; ++i) {
        const float t = static_cast<float>(i) / 10.0f;
        arc.push_back({0.5f + t, 0.05f + 0.3f * t * t, 1.0f});
    }
    bent->addStrand(5, arc, {}, scene.scalp->closestPoint(arc[0]).root);
    const StrandsPtr curved = scene.feathers(bent);
    const CardMesh one = buildCards(*curved, scene.scalp.get(), featherSettings());
    ASSERT_EQ(one.cards.size(), 1u);
    const CardMesh::Card& c = one.cards.front();
    EXPECT_GT(c.center.back().y - c.center.front().y, 0.2f);
    EXPECT_LT(cardFitDistances(*curved, one, 12).back(), 0.02f);
}

TEST(FeatherCards, AFeatherGrowingFromTheSkinHasItsCardsBaseOnIt) {
    // Shafts rooted on the plane, rising off it: the card reaches back past the quill as far as
    // the barbs do, and its root row is seated on the skin (a hair above it), flat and spined,
    // vaned and down; the feathers above lying just over the skin (rooted 0.05 off it) keep theirs.
    FeatherScene scene;
    auto rising = std::make_shared<StrandSet>();
    StrandId id = 300;
    for (const float z : {0.5f, 1.5f, 2.5f}) {
        std::vector<Vec3f> pts;
        for (int i = 0; i <= 10; ++i) pts.push_back({0.5f + 0.09f * static_cast<float>(i), 0.04f * static_cast<float>(i), z});
        rising->addStrand(id++, pts, {}, scene.scalp->closestPoint(pts[0]).root);
    }
    for (const std::string sides : {"both", "around"}) {
        const StrandsPtr s = scene.feathers(rising, 0.3f, sides);
        for (const bool spine : {false, true}) {
            SCOPED_TRACE(sides + (spine ? ", spine" : ", flat"));
            const CardMesh mesh = buildCards(*s, scene.scalp.get(), featherSettings(spine));
            ASSERT_GE(mesh.cards.size(), 3u);
            for (const CardMesh::Card& card : mesh.cards) {
                ASSERT_FALSE(card.base.empty());
                const std::size_t row = card.spine ? 3 : static_cast<std::size_t>(card.across) + 1;
                for (std::size_t e = 0; e < row; ++e) {
                    const float y = mesh.positions[card.firstVertex + e].y;
                    EXPECT_GT(y, 0.0f);
                    EXPECT_LT(y, 0.01f);
                }
            }
        }
    }
    const StrandsPtr lying = scene.feathers(scene.shafts({{0.5f, 1.0f}}));
    for (const CardMesh::Card& card : buildCards(*lying, scene.scalp.get(), featherSettings()).cards) EXPECT_TRUE(card.base.empty());
}

TEST(FeatherCards, BarbsCurlingBackNearTheTipDoNotFlipTheCard) {
    // The barbs near a feather's tip curled back on themselves (a tightly curled Propagate): each
    // ends a little across the shaft the other way. They neither flip the vane's side there nor
    // shear the card: it lies in the vane plane from root to tip.
    FeatherScene scene;
    const StrandsPtr flat = scene.feathers(scene.shafts({{1.0f, 1.0f}}), 0.0f, "both", 30);
    auto curled = std::make_shared<StrandSet>(*flat);
    const std::vector<float>& side = *curled->strandChannel("side");
    const std::vector<float>& shaftU = *curled->strandChannel("shaftU");
    int bent = 0;
    for (std::size_t i = 0; i < curled->strandCount(); ++i) {
        if (std::abs(side[i]) < 0.5f || shaftU[i] < 0.75f) continue;
        Vec3f* p = curled->strandPoints(i);
        const std::size_t n = curled->strandPointCount(i);
        for (std::size_t k = 1; k < n; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(n - 1);
            p[k] = p[0] + Vec3f{0.12f * t, 0.0f, -side[i] * 0.05f * t};
        }
        ++bent;
    }
    ASSERT_GT(bent, 4);
    const CardMesh mesh = buildCards(*curled, scene.scalp.get(), featherSettings(true));
    ASSERT_EQ(mesh.cards.size(), 1u);
    const CardMesh::Card& card = mesh.cards.front();
    for (std::size_t k = 0; k < card.side.size(); ++k) {
        SCOPED_TRACE("station " + std::to_string(k));
        EXPECT_GT(card.side[k].dot(card.side.front()), 0.9f) << "the vane's side the same way all along";
        EXPECT_GT(card.normal[k].y, 0.9f) << "facing out of the vane's top";
    }
}

TEST(FeatherCards, TheTangentMapFollowsTheBarbsNotTheCard) {
    // Barbs at 45 degrees: the bake's tangent is each barb's own direction in card space, so
    // the vanes show (+-0.71, 0.71, 0), and coordU runs along each barb out from the shaft.
    FeatherScene scene;
    const StrandsPtr s = scene.feathers(scene.shafts({{1.0f, 1.0f}}), 0.0f, "both", 40);
    CardSettings settings = featherSettings();
    settings.margin = 0.05f;
    const CardMesh mesh = buildCards(*s, scene.scalp.get(), settings);
    ASSERT_EQ(mesh.cards.size(), 1u);
    const CardMesh::Card& card = mesh.cards.front();
    CardBakeSettings bake;
    bake.size = 256;
    bake.occlusion = 0.0f;
    bake.dilate = 0;
    bake.rootFade = 0.0f;
    const CardTextures t = bakeCardTextures(*s, mesh, bake);
    const float middle = 0.5f * (card.cell[0] + card.cell[2]) * 256.0f;  // the shaft's column (symmetric vanes)
    const float half = 0.5f * (card.cell[2] - card.cell[0]) * 256.0f;
    std::size_t covered = 0, diagonal = 0, sided = 0, rachis = 0;
    float nearU = 0.0f, farU = 0.0f, nearN = 0.0f, farN = 0.0f;
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) {
            const std::size_t p = static_cast<std::size_t>(y) * 256 + static_cast<std::size_t>(x);
            if (t.coverage[p] < 200) continue;
            ++covered;
            const Vec3f tan{t.tangent[p * 3] / 127.5f - 1.0f, t.tangent[p * 3 + 1] / 127.5f - 1.0f, t.tangent[p * 3 + 2] / 127.5f - 1.0f};
            const float dx = static_cast<float>(x) + 0.5f - middle;
            if (std::abs(tan.x) > 0.55f && std::abs(tan.x) < 0.85f && tan.y > 0.55f && tan.y < 0.85f && std::abs(tan.z) < 0.2f) ++diagonal;
            if (std::abs(dx) > 3.0f && tan.x * dx > 0.0f) ++sided;  // the right vane's barbs head right
            if (std::abs(dx) < 2.0f && tan.y > 0.97f) ++rachis;           // the shaft runs up the card
            const float coordU = t.attribute[p * 4 + 3] / 255.0f;
            if (std::abs(dx) > 3.0f && std::abs(dx) < 0.3f * half) nearU += coordU, nearN += 1.0f;
            if (std::abs(dx) > 0.6f * half) farU += coordU, farN += 1.0f;
        }
    }
    ASSERT_GT(covered, 1000u);
    EXPECT_GT(diagonal, covered * 7 / 10) << "the barbs' 45 degrees, not the card's V";
    EXPECT_GT(sided, covered * 7 / 10);
    const float cellRows = (card.cell[3] - card.cell[1]) * 256.0f;
    EXPECT_GT(static_cast<float>(rachis), 1.5f * cellRows) << "the rachis is baked, thicker than a barb";
    ASSERT_GT(nearN, 0.0f);
    ASSERT_GT(farN, 0.0f);
    EXPECT_LT(nearU / nearN + 0.3f, farU / farN) << "coordU runs out along each barb";
}

TEST(FeatherCards, AlikeFeathersShareATextureMirroredLeftToRight) {
    // Four feathers with the longer vane right and four with it left, of two lengths: one
    // variation shows them all, mirrored for the other side, scaled to each one's length, the
    // shaft where the owner's is.
    FeatherScene scene;
    const StrandsPtr right = scene.feathers(scene.shafts({{0.4f, 1.0f}, {0.8f, 0.8f}, {1.2f, 1.0f}, {1.6f, 0.8f}}, 100), 0.4f);
    const StrandsPtr left = scene.feathers(scene.shafts({{2.2f, 1.0f}, {2.6f, 0.8f}, {3.0f, 1.0f}, {3.4f, 0.8f}}, 200), -0.4f);
    const StrandsPtr s = FeatherScene::merge(right, left);
    CardSettings settings = featherSettings();
    settings.variations = 1;
    const CardMesh mesh = buildCards(*s, scene.scalp.get(), settings);
    ASSERT_EQ(mesh.cards.size(), 8u);
    EXPECT_EQ(mesh.textureCount(), 1u);
    const auto owner = static_cast<std::size_t>(
        std::find_if(mesh.cards.begin(), mesh.cards.end(), [](const CardMesh::Card& c) { return c.variationOf < 0; }) - mesh.cards.begin());
    const CardMesh::Card& o = mesh.cards[owner];
    auto shaftZ = [&](const CardMesh::Card& c) { return s->strandPoints(c.strands.front())[0].z; };
    auto isRight = [&](const CardMesh::Card& c) { return shaftZ(c) < 2.0f; };
    // Where the shaft shows in the texture (0..1 across the cell).
    auto shaftInTexture = [&](const CardMesh::Card& c) {
        const float offset = c.center[3].z - shaftZ(c);  // side is +Z
        const float across = 0.5f - offset / c.textureWidth;
        return c.mirrored ? 1.0f - across : across;
    };
    std::size_t mirrored = 0;
    for (std::size_t i = 0; i < mesh.cards.size(); ++i) {
        const CardMesh::Card& c = mesh.cards[i];
        if (i == owner) continue;
        SCOPED_TRACE("card " + std::to_string(i));
        EXPECT_EQ(c.variationOf, static_cast<int>(owner));
        for (int k = 0; k < 4; ++k) EXPECT_FLOAT_EQ(c.cell[k], o.cell[k]);
        EXPECT_EQ(c.mirrored, isRight(c) != isRight(o));  // its longer vane on the other side
        mirrored += c.mirrored ? 1 : 0;
        if (c.mirrored) EXPECT_GT(mesh.u[c.firstVertex], mesh.u[c.firstVertex + 1]);  // u runs the other way
        const float scale = arcLength(c.center) / arcLength(o.center);
        EXPECT_NEAR(c.textureWidth / o.textureWidth, scale, 0.02f * scale);
        EXPECT_NEAR(shaftInTexture(c), shaftInTexture(o), 0.01f);
    }
    EXPECT_EQ(mirrored, 4u);
    // Owners' textures: only the owner bakes.
    CardBakeSettings bake;
    bake.size = 128;
    const CardTextures t = bakeCardTextures(*s, mesh, bake);
    std::size_t covered = 0;
    for (const auto a : t.coverage) covered += a > 0 ? 1 : 0;
    EXPECT_GT(covered, 200u);

    // Without mirroring: each side its own texture, none mirrored.
    settings.mirror = false;
    settings.variations = 2;
    const CardMesh apart = buildCards(*s, scene.scalp.get(), settings);
    EXPECT_EQ(apart.textureCount(), 2u);
    for (const CardMesh::Card& c : apart.cards) {
        EXPECT_FALSE(c.mirrored);
        const CardMesh::Card& own = c.variationOf >= 0 ? apart.cards[static_cast<std::size_t>(c.variationOf)] : c;
        EXPECT_EQ(isRight(c), isRight(own));
    }
}

TEST(FeatherCards, DownFeathersGetCrossedCardsAndSmallOnesCanGo) {
    FeatherScene scene;
    const StrandsPtr s = scene.feathers(scene.shafts({{0.5f, 1.0f}, {1.0f, 0.9f}, {1.5f, 1.0f}, {2.0f, 0.9f}, {2.5f, 0.3f}, {3.0f, 0.25f}}),
                                        0.0f, "around", 12);
    CardSettings settings = featherSettings();
    const CardMesh mesh = buildCards(*s, scene.scalp.get(), settings);
    ASSERT_EQ(mesh.cards.size(), 12u);  // a cross of two per feather
    for (std::size_t c = 0; c + 1 < mesh.cards.size(); c += 2) {
        const CardMesh::Card& a = mesh.cards[c];
        const CardMesh::Card& b = mesh.cards[c + 1];
        EXPECT_EQ(a.feather, b.feather);
        EXPECT_EQ(a.strands, b.strands);
        EXPECT_LT(std::abs(a.side[3].dot(b.side[3])), 0.05f);  // crossed at right angles
        // As wide as the puff: every barb within half the width of the shaft.
        const float radius = 0.3f;
        EXPECT_GE(a.width[3], 2.0f * radius * std::sin(0.785f) * 0.9f);
    }
    EXPECT_EQ(mesh.textureCount(), 12u);  // no variations: every card its own
    // featherSpine leaves the crossed cards flat strips: a puff has no vanes to fold to.
    CardSettings spined = featherSettings(true);
    const CardMesh crossed = buildCards(*s, scene.scalp.get(), spined);
    ASSERT_EQ(crossed.cards.size(), 12u);
    for (const CardMesh::Card& c : crossed.cards) EXPECT_EQ(std::make_pair(c.across, c.spine), std::make_pair(1, false));
    EXPECT_EQ(crossed.positions, mesh.positions);
    // Shared: each cross shows its owner's first card, alternate cards mirrored.
    settings.variations = 2;
    const CardMesh shared = buildCards(*s, scene.scalp.get(), settings);
    EXPECT_EQ(shared.textureCount(), 2u);
    std::size_t mirrored = 0;
    for (const CardMesh::Card& c : shared.cards) mirrored += c.mirrored ? 1 : 0;
    EXPECT_EQ(mirrored, 6u);
    // Small down feathers left out (a low LOD).
    settings.featherMinLength = 0.5f;
    const CardMesh fewer = buildCards(*s, scene.scalp.get(), settings);
    EXPECT_EQ(fewer.cards.size(), 8u);
}

TEST(FeatherCards, TheBudgetKeepsTheLongestAndSegmentsSetTheTriangles) {
    FeatherScene scene;
    const StrandsPtr s = scene.feathers(scene.shafts({{0.5f, 1.0f}, {1.0f, 0.6f}, {1.5f, 0.9f}, {2.0f, 0.7f}, {2.5f, 0.8f}}));
    CardSettings settings = featherSettings();
    settings.featherSegments = 1;  // a single quad each
    EXPECT_EQ(buildCards(*s, scene.scalp.get(), settings).triangleCount(), 5u * 2u);
    settings.featherSegments = 8;
    EXPECT_EQ(buildCards(*s, scene.scalp.get(), settings).triangleCount(), 5u * 16u);
    // With the spine (the default): two quads a segment, 4 x featherSegments triangles a card.
    EXPECT_TRUE(CardSettings{}.featherSpine);
    settings.featherSpine = true;
    EXPECT_EQ(buildCards(*s, scene.scalp.get(), settings).triangleCount(), 5u * 32u);
    settings.featherSegments = 1;  // a V: two quads each
    const CardMesh vees = buildCards(*s, scene.scalp.get(), settings);
    EXPECT_EQ(vees.triangleCount(), 5u * 4u);
    EXPECT_EQ(vees.vertexCount(), 5u * 6u);
    std::size_t triangles = 0;
    for (const CardNodeSummary& n : summarizeCardNodes(vees, s.get())) triangles += n.triangles;
    EXPECT_EQ(triangles, 5u * 4u);  // the stats count them as the mesh has them
    settings.featherSegments = 8;
    settings.featherSpine = false;
    settings.maxCards = 3;
    const CardMesh within = buildCards(*s, scene.scalp.get(), settings);
    ASSERT_EQ(within.cards.size(), 3u);
    for (const CardMesh::Card& c : within.cards) EXPECT_GT(s->strandPoints(c.strands.front())[10].x, 0.5f + 0.8f - 1e-3f);
}

TEST(FeatherCards, ModesCardFeathersHairOrBoth) {
    FeatherScene scene;
    const StrandsPtr feathers = scene.feathers(scene.shafts({{0.5f, 1.0f}, {1.0f, 0.9f}, {1.5f, 1.0f}, {2.0f, 0.9f}, {2.5f, 1.0f}}));
    const StrandsPtr s = FeatherScene::merge(hairTufts(*scene.scalp), feathers);
    ASSERT_TRUE(s->strandChannel("featherId"));
    auto isFeatherStrand = [&](std::size_t si) { return channel(*s, "featherId", si) >= 0.0f; };
    auto count = [&](const CardMesh& m, bool feather) {
        std::size_t n = 0;
        for (const CardMesh::Card& c : m.cards) {
            n += (c.feather >= 0) == feather ? 1 : 0;
            for (const std::size_t si : c.strands) EXPECT_EQ(isFeatherStrand(si), c.feather >= 0) << "a card holds the other kind";
        }
        return n;
    };
    CardSettings settings = featherSettings();
    settings.crossStyle = CardSettings::CrossStyle::Single;
    const CardMesh both = buildCards(*s, scene.scalp.get(), settings);
    EXPECT_EQ(count(both, true), 5u);
    EXPECT_EQ(count(both, false), 4u);
    settings.featherMode = CardSettings::FeatherMode::PerFeather;
    const CardMesh feathersOnly = buildCards(*s, scene.scalp.get(), settings);
    EXPECT_EQ(count(feathersOnly, true), 5u);
    EXPECT_EQ(count(feathersOnly, false), 0u);
    settings.featherMode = CardSettings::FeatherMode::Off;
    const CardMesh hairOnly = buildCards(*s, scene.scalp.get(), settings);
    EXPECT_EQ(count(hairOnly, true), 0u);
    EXPECT_EQ(count(hairOnly, false), 4u);
    // Per vane: everything as hair, a card per clumpId (every vane of every feather, and the tufts).
    settings.featherMode = CardSettings::FeatherMode::PerVane;
    const CardMesh perVane = buildCards(*s, scene.scalp.get(), settings);
    for (const CardMesh::Card& c : perVane.cards) EXPECT_EQ(c.feather, -1);
    EXPECT_EQ(perVane.cards.size(), 4u + 10u);
    // Feathers take their cards from the budget first.
    settings.featherMode = CardSettings::FeatherMode::Auto;
    settings.maxCards = 7;
    const CardMesh budget = buildCards(*s, scene.scalp.get(), settings);
    EXPECT_EQ(count(budget, true), 5u);
    EXPECT_EQ(count(budget, false), 2u);
    // Hair without clumps merged with feathers (whose barbs bring clumpId along): the hair is
    // grouped as unclumped hair is, not dropped for want of a clump.
    const StrandsPtr loose = FeatherScene::merge(hairTufts(*scene.scalp, 4, false), feathers);
    settings = featherSettings();
    settings.crossStyle = CardSettings::CrossStyle::Single;
    settings.maxCards = 12;
    EXPECT_GE(count(buildCards(*loose, scene.scalp.get(), settings), false), 2u);
    settings.featherMode = CardSettings::FeatherMode::Off;
    EXPECT_GE(count(buildCards(*loose, scene.scalp.get(), settings), false), 2u);
}

TEST(FeatherCards, TheBakedAtlasKeepsFeatherCellsAndNewFeathersTakeAFeathers) {
    FeatherScene scene;
    const StrandsPtr feathers = scene.feathers(scene.shafts({{0.5f, 1.0f}, {1.0f, 0.9f}, {1.5f, 1.0f}}));
    const StrandsPtr s = FeatherScene::merge(hairTufts(*scene.scalp), feathers);
    CardSettings settings = featherSettings();
    settings.crossStyle = CardSettings::CrossStyle::Single;
    const CardMesh baked = buildCards(*s, scene.scalp.get(), settings);
    const CardAtlas atlas = CardAtlas::fromJson(captureCardAtlas(baked, s.get()).toJson());  // round trip
    std::size_t featherEntries = 0;
    for (std::size_t c = 0; c < baked.cards.size(); ++c) {
        EXPECT_EQ(atlas.entries[c].feather, baked.cards[c].feather >= 0);
        featherEntries += atlas.entries[c].feather ? 1 : 0;
    }
    EXPECT_EQ(featherEntries, 3u);
    CardMesh rebuilt = buildCards(*s, scene.scalp.get(), settings);
    EXPECT_EQ(applyCardAtlas(rebuilt, atlas, s.get()), rebuilt.cards.size());
    for (std::size_t c = 0; c < baked.cards.size(); ++c) {
        for (int k = 0; k < 4; ++k) EXPECT_FLOAT_EQ(rebuilt.cards[c].cell[k], baked.cards[c].cell[k]);
    }
    // A new feather (strands the bake never saw) shows a baked feather, never a hair texture.
    const StrandsPtr more = FeatherScene::merge(s, scene.feathers(scene.shafts({{3.0f, 1.0f}}, 700)));
    CardMesh grown = buildCards(*more, scene.scalp.get(), settings);
    applyCardAtlas(grown, atlas, more.get());
    std::set<std::array<float, 4>> featherCells;
    for (const CardMesh::Card& c : baked.cards) {
        if (c.feather >= 0) featherCells.insert({c.cell[0], c.cell[1], c.cell[2], c.cell[3]});
    }
    for (const CardMesh::Card& c : grown.cards) {
        if (c.feather >= 0) EXPECT_TRUE(featherCells.count({c.cell[0], c.cell[1], c.cell[2], c.cell[3]}));
    }
}

// The share of a card's cell across (0 at cell[0], 1 at cell[2]) where vertex v sits.
float acrossCell(const CardMesh& mesh, const CardMesh::Card& card, std::size_t v) {
    return (mesh.u[v] - card.cell[0]) / (card.cell[2] - card.cell[0]);
}

// Card space colors of a bake at pixel p: the tangent (-1..1 each).
Vec3f bakedTangent(const CardTextures& t, std::size_t p) {
    return {t.tangent[p * 3] / 127.5f - 1.0f, t.tangent[p * 3 + 1] / 127.5f - 1.0f, t.tangent[p * 3 + 2] / 127.5f - 1.0f};
}

CardBakeSettings plainBake(int size) {
    CardBakeSettings bake;
    bake.size = size;
    bake.occlusion = 0.0f;
    bake.dilate = 0;
    bake.rootFade = 0.0f;
    return bake;
}

TEST(FeatherCards, TheSpineRunsDownTheShaftTwoQuadsASegment) {
    // Flat vanes, the right one longer: each card gets a column on the shaft (left edge, spine,
    // right edge per station), two quads a segment, the spine's U where the rachis bakes.
    FeatherScene scene;
    const StrandsPtr s = scene.feathers(scene.shafts({{0.5f, 1.0f}, {1.5f, 0.9f}, {2.5f, 1.1f}}), 0.3f, "both", 40);
    const CardMesh mesh = buildCards(*s, scene.scalp.get(), featherSettings(true));
    ASSERT_EQ(mesh.cards.size(), 3u);
    EXPECT_EQ(mesh.triangleCount(), 3u * 4u * 6u);
    EXPECT_EQ(mesh.vertexCount(), 3u * 3u * 7u);
    for (std::size_t c = 0; c < mesh.cards.size(); ++c) {
        const CardMesh::Card& card = mesh.cards[c];
        SCOPED_TRACE("card " + std::to_string(c));
        EXPECT_TRUE(card.spine);
        EXPECT_EQ(card.across, 2);
        ASSERT_EQ(card.stations, 7);
        const float shaftZ = s->strandPoints(card.strands.front())[0].z;
        for (std::size_t k = 0; k < 7; ++k) {
            const std::size_t row = card.firstVertex + 3 * k;
            const Vec3f& left = mesh.positions[row];
            const Vec3f& spine = mesh.positions[row + 1];
            const Vec3f& right = mesh.positions[row + 2];
            // On the shaft (a straight line at y 0.05 along +X, carried on past its ends).
            EXPECT_NEAR(spine.y, 0.05f, 1e-3f);
            EXPECT_NEAR(spine.z, shaftZ, 1e-3f);
            // The left vane toward -Z, the right toward +Z (the bare quill at the root is narrow).
            EXPECT_LT(left.z, spine.z - (k >= 2 && k <= 4 ? 0.05f : 0.0f));
            EXPECT_GT(right.z, spine.z + (k >= 2 && k <= 4 ? 0.05f : 0.0f));
            // Flat vanes fold nowhere: every vertex in the vane plane, facing out of its top.
            for (std::size_t e = 0; e < 3; ++e) {
                EXPECT_NEAR(mesh.positions[row + e].y, 0.05f, 2e-3f);
                EXPECT_GT(mesh.normals[row + e].y, 0.999f);
            }
            // The spine's U is the texture's shaft line (off the middle toward the shorter vane);
            // the edges keep the flat card's.
            const float shaftLine = 0.5f - (card.center[k].z - shaftZ) / card.textureWidth;
            EXPECT_NEAR(acrossCell(mesh, card, row + 1), shaftLine, 1e-4f);
            EXPECT_LT(shaftLine, 0.47f);
            EXPECT_NEAR(acrossCell(mesh, card, row), 0.5f - 0.5f * card.width[k] / card.textureWidth, 1e-4f);
            EXPECT_NEAR(acrossCell(mesh, card, row + 2), 0.5f + 0.5f * card.width[k] / card.textureWidth, 1e-4f);
        }
    }
    // The rachis bakes under the spine: its pixels (running up the card) sit at the spine's U.
    const CardTextures t = bakeCardTextures(*s, mesh, plainBake(512));
    const CardMesh::Card& card = mesh.cards.front();
    const float spineX = mesh.u[card.firstVertex + 3 * 3 + 1] * 512.0f;
    const int y0 = static_cast<int>((1.0f - card.cell[3]) * 512.0f), y1 = static_cast<int>((1.0f - card.cell[1]) * 512.0f);
    const int x0 = static_cast<int>(card.cell[0] * 512.0f), x1 = static_cast<int>(card.cell[2] * 512.0f);
    float sum = 0.0f, count = 0.0f;
    for (int y = y0 + (y1 - y0) / 4; y < y1 - (y1 - y0) / 4; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t p = static_cast<std::size_t>(y) * 512 + static_cast<std::size_t>(x);
            if (t.coverage[p] > 200 && bakedTangent(t, p).y > 0.97f) sum += static_cast<float>(x) + 0.5f, count += 1.0f;
        }
    }
    ASSERT_GT(count, 20.0f);
    EXPECT_NEAR(sum / count, spineX, 1.5f);
}

TEST(FeatherCards, EachHalfFoldsToFollowItsVane) {
    FeatherScene scene;
    CardSettings settings = featherSettings(true);
    settings.margin = 0.0f;  // edges right at the barbs' reach
    constexpr float kDeg = 0.017453293f;
    // Barbs 45 degrees off the shaft rising 30 out of the vane: across the shaft they head
    // atan(tan 30 / sin 45) = 39.2 degrees up and reach 0.3 x sqrt(sin^2 45 cos^2 30 + sin^2 30).
    const float up = std::atan(std::tan(30.0f * kDeg) / std::sin(45.0f * kDeg));
    const float reach = 0.3f * std::sqrt(0.5f * 0.75f + 0.25f);
    const StrandsPtr lifted = scene.feathers(scene.shafts({{1.0f, 1.0f}}), 0.0f, "both", 40, 30.0);
    const CardMesh mesh = buildCards(*lifted, scene.scalp.get(), settings);
    ASSERT_EQ(mesh.cards.size(), 1u);
    const CardMesh::Card& card = mesh.cards.front();
    ASSERT_EQ(card.stations, 7);
    for (std::size_t k = 2; k <= 4; ++k) {
        SCOPED_TRACE("station " + std::to_string(k));
        const std::size_t row = card.firstVertex + 3 * k;
        const Vec3f spine = mesh.positions[row + 1];
        const Vec3f l = mesh.positions[row] - spine, r = mesh.positions[row + 2] - spine;
        // A V opening up: each half at the barbs' angle, its edge where they reach.
        EXPECT_NEAR(std::atan2(r.y, r.z), up, 0.06f);
        EXPECT_NEAR(std::atan2(l.y, -l.z), up, 0.06f);
        EXPECT_NEAR(r.length(), reach, 0.1f * reach);
        EXPECT_NEAR(l.length(), reach, 0.1f * reach);
        // Normals per half: each edge's is its half's (square to it, leaning in over the V),
        // the spine's between them.
        const Vec3f nl = mesh.normals[row], ns = mesh.normals[row + 1], nr = mesh.normals[row + 2];
        EXPECT_NEAR(nr.dot(r.normalized()), 0.0f, 0.02f);
        EXPECT_NEAR(nl.dot(l.normalized()), 0.0f, 0.02f);
        EXPECT_NEAR(nr.z, -std::sin(up), 0.06f);
        EXPECT_NEAR(nl.z, std::sin(up), 0.06f);
        EXPECT_GT(ns.y, 0.99f);
    }
    // The folded card lies along the barbs; the flat strip leaves them hanging above it.
    auto percentile90 = [](const std::vector<float>& d) { return d[d.size() * 9 / 10]; };
    CardSettings flat = settings;
    flat.featherSpine = false;
    const float folded = percentile90(cardFitDistances(*lifted, mesh, 12));
    const float plane = percentile90(cardFitDistances(*lifted, buildCards(*lifted, scene.scalp.get(), flat), 12));
    EXPECT_LT(folded, 0.01f);
    EXPECT_GT(plane, 5.0f * folded);
    // The bake's tangents are in each half's frame: barbs lying in their half point along it
    // (no z), where the flat strip's frame has them rising out of it.
    auto meanZ = [&](const CardMesh& m) {
        const CardTextures t = bakeCardTextures(*lifted, m, plainBake(256));
        float z = 0.0f, n = 0.0f;
        for (std::size_t p = 0; p < t.coverage.size(); ++p) {
            if (t.coverage[p] < 200 || bakedTangent(t, p).y > 0.9f) continue;  // barbs, not the rachis
            z += std::abs(bakedTangent(t, p).z), n += 1.0f;
        }
        return n > 0.0f ? z / n : 1.0f;
    };
    EXPECT_LT(meanZ(mesh), 0.12f);
    EXPECT_GT(meanZ(buildCards(*lifted, scene.scalp.get(), flat)), 0.35f);

    // Drooping vanes (barbs bending down over their length): a V opening down.
    const StrandsPtr drooped = scene.feathers(scene.shafts({{1.0f, 1.0f}}), 0.0f, "both", 40, 0.0, 40.0);
    const CardMesh down = buildCards(*drooped, scene.scalp.get(), settings);
    for (std::size_t k = 2; k <= 4; ++k) {
        const std::size_t row = down.cards.front().firstVertex + 3 * k;
        const Vec3f spine = down.positions[row + 1];
        EXPECT_LT(down.positions[row].y - spine.y, -0.02f);
        EXPECT_LT(down.positions[row + 2].y - spine.y, -0.02f);
        EXPECT_GT(down.normals[row + 2].z, 0.1f);  // leaning out, over the drop
    }

    // One vane up, the other down (the lifted feather's left vane turned down to half its
    // height): each half follows its own.
    auto mixed = std::make_shared<StrandSet>(*lifted);
    for (std::size_t i = 0; i < mixed->strandCount(); ++i) {
        if (channel(*mixed, "side", i) > -0.5f) continue;
        Vec3f* p = mixed->strandPoints(i);
        for (std::size_t j = 0; j < mixed->strandPointCount(i); ++j) p[j].y = 0.05f - 0.5f * (p[j].y - 0.05f);
    }
    const CardMesh both = buildCards(*mixed, scene.scalp.get(), settings);
    ASSERT_EQ(both.cards.size(), 1u);
    const float downLeft = std::atan(-0.5f * std::tan(up));
    for (std::size_t k = 2; k <= 4; ++k) {
        const std::size_t row = both.cards.front().firstVertex + 3 * k;
        const Vec3f spine = both.positions[row + 1];
        const Vec3f l = both.positions[row] - spine, r = both.positions[row + 2] - spine;
        EXPECT_NEAR(std::atan2(r.y, r.z), up, 0.07f);
        EXPECT_NEAR(std::atan2(l.y, -l.z), downLeft, 0.07f);
    }
}

TEST(FeatherCards, SharedAndMirroredFeathersKeepTheSpineOnTheBakedShaft) {
    // As AlikeFeathersShareATextureMirroredLeftToRight, with the spine: every card's spine sits
    // on its own shaft and at the one place across the shared texture where the owner's rachis
    // baked, mirrored or not, whatever its length.
    FeatherScene scene;
    const StrandsPtr right = scene.feathers(scene.shafts({{0.4f, 1.0f}, {0.8f, 0.8f}, {1.2f, 1.0f}, {1.6f, 0.8f}}, 100), 0.4f);
    const StrandsPtr left = scene.feathers(scene.shafts({{2.2f, 1.0f}, {2.6f, 0.8f}, {3.0f, 1.0f}, {3.4f, 0.8f}}, 200), -0.4f);
    const StrandsPtr s = FeatherScene::merge(right, left);
    CardSettings settings = featherSettings(true);
    settings.variations = 1;
    const CardMesh mesh = buildCards(*s, scene.scalp.get(), settings);
    ASSERT_EQ(mesh.cards.size(), 8u);
    EXPECT_EQ(mesh.textureCount(), 1u);
    const auto owner = static_cast<std::size_t>(
        std::find_if(mesh.cards.begin(), mesh.cards.end(), [](const CardMesh::Card& c) { return c.variationOf < 0; }) - mesh.cards.begin());
    const CardMesh::Card& o = mesh.cards[owner];
    const float shaftLine = acrossCell(mesh, o, o.firstVertex + 1);
    EXPECT_GT(std::abs(shaftLine - 0.5f), 0.03f);  // asymmetric vanes: off the middle
    std::size_t mirrored = 0;
    for (std::size_t i = 0; i < mesh.cards.size(); ++i) {
        const CardMesh::Card& c = mesh.cards[i];
        SCOPED_TRACE("card " + std::to_string(i));
        ASSERT_TRUE(c.spine);
        mirrored += c.mirrored ? 1 : 0;
        const float shaftZ = s->strandPoints(c.strands.front())[0].z;
        for (int k = 0; k < c.stations; ++k) {
            const std::size_t row = c.firstVertex + 3 * static_cast<std::size_t>(k);
            EXPECT_NEAR(mesh.positions[row + 1].z, shaftZ, 1e-3f);
            EXPECT_NEAR(acrossCell(mesh, c, row + 1), shaftLine, 1e-3f);
            // Left edge to right edge in the world; mirrored, U runs the other way.
            EXPECT_EQ(mesh.u[row] > mesh.u[row + 2], c.mirrored);
        }
    }
    EXPECT_EQ(mirrored, 4u);
}

// hair (scatter -> clump) and feathers (shafts -> comb -> Propagate -> featherCards) merged, then
// hairCards: the feathers carded on their own branch, the hair after the merge with feathers off.
GraphDocument hairAndFeathers() {
    return loadGraphJson(R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "segments", "type": "Resample", "params": {"segments": 8}},
        {"name": "hair", "type": "Scatter", "params": {"density": 300.0, "length": 0.3}},
        {"name": "clump", "type": "Clump", "params": {"count": 10, "strength": 0.8}},
        {"name": "shafts", "type": "Scatter", "params": {"density": 6.0, "length": 0.8, "seed": 5}},
        {"name": "comb", "type": "Bend", "params": {"angle": 180.0, "end": 0.0, "along": "direction", "direction": [1, 0, 0]}},
        {"name": "feathers", "type": "Propagate", "params": {"count": 12, "length": 0.25}},
        {"name": "featherCards", "type": "Cards", "params": {"featherMode": "perFeather", "variations": 4}},
        {"name": "merge", "type": "Merge"},
        {"name": "hairCards", "type": "Cards", "params": {"featherMode": "off", "segments": 4}},
        {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "hair", "input": "scalp"},
        {"from": "hair", "to": "clump", "input": "strands"},
        {"from": "scalp", "to": "shafts", "input": "scalp"}, {"from": "shafts", "to": "comb", "input": "strands"},
        {"from": "comb", "to": "feathers", "input": "shafts"}, {"from": "feathers", "to": "featherCards", "input": "strands"},
        {"from": "clump", "to": "merge", "input": "a"}, {"from": "featherCards", "to": "merge", "input": "b"},
        {"from": "merge", "to": "hairCards", "input": "strands"}, {"from": "hairCards", "to": "out", "input": "strands"}]})j");
}

TEST(FeatherCards, HairAndFeatherCardsNodesShareOneAtlasEachWithItsOwnStamp) {
    const auto plane = test::makePlane(8, 2.0f);
    GraphDocument doc = hairAndFeathers();
    auto evaluate = [&](StrandsPtr* strands) {
        ExternalInputs externals;
        setGroomExternals(externals, plane, nullptr, nullptr);
        EvalContext ctx;
        ctx.externals = &externals;
        ctx.buildCards = true;
        const StrandsPtr out = doc.graph.evaluateStrands(doc.output, ctx);
        CardMesh cards = combineCardLayers(out->cardLayers());
        layoutCards(cards, cards.padding);
        if (strands) *strands = out;
        return cards;
    };
    auto stamps = [&]() {
        StrandsPtr strands;
        const CardMesh cards = evaluate(&strands);
        std::map<std::string, CardNodeSummary> out;
        for (const CardNodeSummary& n : summarizeCardNodes(cards, strands.get())) out[n.node] = n;
        return out;
    };
    StrandsPtr strands;
    const CardMesh cards = evaluate(&strands);
    ASSERT_EQ(cards.nodes.size(), 2u);
    // One atlas: no two textures overlap, whichever node they came from.
    for (std::size_t a = 0; a < cards.cards.size(); ++a) {
        for (std::size_t b = a + 1; b < cards.cards.size(); ++b) {
            if (cards.cards[a].variationOf >= 0 || cards.cards[b].variationOf >= 0) continue;
            const float* p = cards.cards[a].cell;
            const float* q = cards.cards[b].cell;
            EXPECT_TRUE(p[2] <= q[0] || q[2] <= p[0] || p[3] <= q[1] || q[3] <= p[1]);
        }
    }
    for (const CardMesh::Card& c : cards.cards) {
        const bool feathers = cardNodeOf(cards, c) == "featherCards";
        EXPECT_EQ(c.feather >= 0, feathers);
        const StrandSet& source = *c.source;
        for (const std::size_t si : c.strands) EXPECT_EQ((*source.strandChannel("featherId"))[si] >= 0.0f, feathers);
    }
    const auto baked = stamps();
    ASSERT_EQ(baked.size(), 2u);
    EXPECT_GT(baked.at("featherCards").feathers, 5u);
    EXPECT_EQ(baked.at("featherCards").cards, baked.at("featherCards").feathers);
    EXPECT_LT(baked.at("featherCards").textures, baked.at("featherCards").cards);  // shared
    EXPECT_EQ(baked.at("hairCards").feathers, 0u);
    EXPECT_GE(baked.at("hairCards").cards, 5u);

    // A feather change restales only the feathers' node; a hair change only the hair's.
    doc.graph.node(doc.node("feathers")).params().set("angle", 60.0);
    auto now = stamps();
    EXPECT_NE(now.at("featherCards").bakeHash, baked.at("featherCards").bakeHash);
    EXPECT_EQ(now.at("hairCards").bakeHash, baked.at("hairCards").bakeHash);
    doc.graph.node(doc.node("feathers")).params().set("angle", 50.0);
    doc.graph.node(doc.node("clump")).params().set("strength", 0.5);
    now = stamps();
    EXPECT_EQ(now.at("featherCards").bakeHash, baked.at("featherCards").bakeHash);
    EXPECT_NE(now.at("hairCards").bakeHash, baked.at("hairCards").bakeHash);
}

// The levels of detail every host offers (card_presets.h): Maya's numbers (hero 10 segments and 32
// variations, low 5 segments), maxCards from the triangle budget, and what a Cards node cards
// decides which preset it takes.
TEST(CardPresets, OneTableForEveryHost) {
    ASSERT_EQ(cardLodPresets().size(), 3u);
    const CardLodPreset& hero = *findCardLodPreset("hero");
    EXPECT_EQ(hero.segments, 10);
    EXPECT_EQ(hero.variations, 32);
    EXPECT_EQ(hero.cardsPerClump, 6);
    EXPECT_EQ(hero.layers, 2);
    EXPECT_EQ(findCardLodPreset("mid")->segments, 8);
    EXPECT_EQ(findCardLodPreset("low")->segments, 5);
    EXPECT_EQ(findCardLodPreset("ultra"), nullptr);
    EXPECT_EQ(featherCardTriangles(6), 24);
    EXPECT_EQ(featherCardTriangles(1, false), 2);

    auto params = [](const std::vector<std::pair<std::string, ParamValue>>& list) {
        std::map<std::string, ParamValue> out(list.begin(), list.end());
        return out;
    };
    const auto hair = params(cardPresetParams("hero", "hair"));
    EXPECT_EQ(std::get<std::int64_t>(hair.at("segments")), 10);
    EXPECT_EQ(std::get<std::int64_t>(hair.at("maxCards")), 50000 / (2 * 10 * 3));
    EXPECT_EQ(hair.count("featherSegments"), 0u);
    EXPECT_EQ(std::get<std::int64_t>(params(cardPresetParams("low", "hair", 1000)).at("maxCards")), 1000 / 10);
    const auto feathers = params(cardPresetParams("mid", "feathers"));
    EXPECT_EQ(std::get<std::int64_t>(feathers.at("maxCards")), 8000 / 12);
    EXPECT_EQ(feathers.count("segments"), 0u);
    const auto both = params(cardPresetParams("mid", "both"));
    EXPECT_EQ(std::get<std::int64_t>(both.at("featherSegments")), 3);
    EXPECT_EQ(std::get<std::int64_t>(both.at("maxCards")), 15000 / 32 + 8000 / 12);
    EXPECT_THROW(cardPresetParams("ultra", "hair"), std::invalid_argument);
    EXPECT_THROW(cardPresetParams("mid", "fur"), std::invalid_argument);

    // The kind: the featherMode, else what reaches the node.
    const std::string graph = saveGraphJson(hairAndFeathers());
    EXPECT_EQ(cardNodeKind(graph, "featherCards"), "feathers");
    EXPECT_EQ(cardNodeKind(graph, "hairCards"), "hair");  // featherMode off
    nlohmann::json g = nlohmann::json::parse(graph);
    for (auto& n : g["nodes"]) {
        if (n["name"] == "hairCards") n["params"].erase("featherMode");
    }
    EXPECT_EQ(cardNodeKind(g.dump(), "hairCards"), "both");  // auto: hair and feathers merged
    EXPECT_TRUE(strandsReaching(g.dump(), "featherCards").feathers);
    EXPECT_FALSE(strandsReaching(g.dump(), "featherCards").hair);
    EXPECT_FALSE(strandsReaching(g.dump(), "clump").feathers);
    EXPECT_TRUE(strandsReaching(g.dump(), "clump").hair);
    EXPECT_THROW(cardNodeKind(graph, "nothing"), std::invalid_argument);
}

// A card bake as every host names, writes and records it (card_files.h): the seven maps of
// Unreal's layout, the atlas and the preview at <folder>/<name>_, a groom file's at
// <stem>_cards/<stem>_ beside it, and the cardMaps text formatted, parsed and moved.
TEST(CardFiles, OneLayoutAndRecordForEveryHost) {
    EXPECT_EQ(cardMapNames(), (std::vector<std::string>{"Depth", "Coverage", "Tangent", "Attribute", "Color", "AO", "Normal"}));
    const CardBakePaths at = CardBakePaths::in("C:\\bakes\\hair", "hair");
    EXPECT_EQ(at.prefix, "C:/bakes/hair/hair_");
    EXPECT_EQ(at.map("Color"), "C:/bakes/hair/hair_Color.tga");
    EXPECT_EQ(at.files().size(), 8u);
    EXPECT_EQ(at.files().back(), at.atlas());
    EXPECT_EQ(at.preview(), "C:/bakes/hair/hair_Preview.png");
    EXPECT_EQ(groomFileCardBakePrefix("bob"), "bob_cards/bob_");
    EXPECT_EQ(CardBakePaths::besideGroomFile("/shots/a/bob.groom").prefix, "/shots/a/bob_cards/bob_");

    const std::string text = formatCardMaps("groomlab/s/cards/hair/hair_", "cards:12", 3);
    EXPECT_EQ(text, "Color=groomlab/s/cards/hair/hair_Color.tga;Coverage=groomlab/s/cards/hair/hair_Coverage.tga;"
                    "AO=groomlab/s/cards/hair/hair_AO.tga;Atlas=groomlab/s/cards/hair/hair_Atlas.json;Strands=cards:12;Bake=3");
    EXPECT_EQ(formatCardMaps("x_", "", 0).find("Strands"), std::string::npos);
    EXPECT_EQ(cardMapsPrefix(text), "groomlab/s/cards/hair/hair_");
    EXPECT_EQ(cardMapsPrefix(""), "");
    EXPECT_EQ(parseCardMaps(text).at("Strands"), "cards:12");
    const std::string moved = withCardMapsPrefix(text, "other/hair_2_");
    EXPECT_EQ(cardMapsPrefix(moved), "other/hair_2_");
    EXPECT_EQ(parseCardMaps(moved).at("Atlas"), "other/hair_2_Atlas.json");
    EXPECT_EQ(parseCardMaps(moved).at("Strands"), "cards:12");
    EXPECT_EQ(parseCardMaps(moved).at("Bake"), "3");

    // The bake in memory and on disk.
    const auto plane = test::makePlane(8, 2.0f);
    GraphDocument doc = hairAndFeathers();
    ExternalInputs externals;
    setGroomExternals(externals, plane, nullptr, nullptr);
    EvalContext ctx;
    ctx.externals = &externals;
    ctx.buildCards = true;
    const StrandsPtr out = doc.graph.evaluateStrands(doc.output, ctx);
    EXPECT_THROW(bakeCardsInMemory(std::make_shared<StrandSet>(), plane, 16), std::runtime_error);
    const CardBake bake = bakeCardsInMemory(out, plane, 16);
    ASSERT_TRUE(bake.cards);
    EXPECT_GT(bake.cards->cards.size(), 0u);
    EXPECT_EQ(bake.textures.size, 16);
    const std::string folder = (std::filesystem::temp_directory_path() / "groomlab_card_files").generic_u8string();
    std::filesystem::remove_all(folder);
    const CardBakePaths files = CardBakePaths::in(folder + "/hair", "hair");
    EXPECT_FALSE(files.exists());
    const std::string stamp = writeCardBake(bake, files, true);
    EXPECT_EQ(stamp, cardsBakeStamp(summarizeCardNodes(*bake.cards, bake.strands.get())));
    EXPECT_TRUE(files.exists());
    for (const std::string& f : files.files()) EXPECT_TRUE(std::filesystem::exists(f)) << f;
    EXPECT_TRUE(std::filesystem::exists(files.preview()));
    std::filesystem::remove_all(folder);
}

}  // namespace
}  // namespace groomlab