// SPDX-License-Identifier: Apache-2.0
// Groom to cards: clusters become ribbon cards with atlas UVs; baking renders
// the strands into the atlas.

#include <algorithm>
#include <array>
#include <map>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <tuple>

#include <gtest/gtest.h>

#include "groomlab/cards.h"
#include "groomlab/operators.h"
#include "groomlab/curve.h"
#include "groomlab/display.h"
#include "groomlab/export_view.h"
#include "groomlab/graph_io.h"
#include "groomlab/hair_color.h"
#include "groomlab/groom.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

StrandsPtr clumpedFur() {
    GroomData d;
    d.scalp = test::makePlane(16);
    d.graphJson = R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 800.0}},
        {"name": "clump", "type": "Clump", "params": {"count": 12, "strength": 0.8}},
        {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 10}}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "clump", "input": "strands"},
        {"from": "clump", "to": "out", "input": "strands"}]})j";
    return Groom(d).evaluate(1.0f, 0.0);
}

TEST(Cards, OneCardPerClumpWithAtlasUVs) {
    const StrandsPtr s = clumpedFur();
    const auto plane = test::makePlane(16);
    CardSettings settings;
    settings.segments = 6;
    settings.crossStyle = CardSettings::CrossStyle::Single;  // a single ribbon each (standing tufts would get an X)
    const CardMesh mesh = buildCards(*s, plane.get(), settings);
    ASSERT_GE(mesh.cards.size(), 8u);  // about the 12 clumps (a few more on a small plane: its edges)
    ASSERT_LE(mesh.cards.size(), 17u);
    EXPECT_EQ(mesh.vertexCount(), mesh.cards.size() * 2 * 7);
    EXPECT_EQ(mesh.quads.size(), mesh.cards.size() * 6 * 4);
    for (std::size_t i = 0; i < mesh.vertexCount(); ++i) {
        EXPECT_GE(mesh.u[i], 0.0f);
        EXPECT_LE(mesh.u[i], 1.0f);
        EXPECT_GE(mesh.v[i], 0.0f);
        EXPECT_LE(mesh.v[i], 1.0f);
        EXPECT_NEAR(mesh.normals[i].length(), 1.0f, 1e-3f);
    }
    // Normals are across the card (the fur stands along the scalp normal, so
    // cards face sideways here; hair lying on a head gets cards facing out).
    for (const CardMesh::Card& card : mesh.cards) {
        const Vec3f along = (card.center.back() - card.center.front()).normalized();
        EXPECT_LT(std::abs(card.normal.front().dot(along)), 0.3f);
    }
    // Cells don't overlap.
    for (std::size_t a = 0; a < mesh.cards.size(); ++a) {
        for (std::size_t b = a + 1; b < mesh.cards.size(); ++b) {
            const float* p = mesh.cards[a].cell;
            const float* q = mesh.cards[b].cell;
            EXPECT_TRUE(p[2] <= q[0] || q[2] <= p[0] || p[3] <= q[1] || q[3] <= p[1]);
        }
    }
    // A card runs from the scalp up the clump.
    const CardMesh::Card& c = mesh.cards.front();
    EXPECT_LT(c.center.front().y, 0.05f);
    EXPECT_GT(c.center.back().y, 0.5f);
    EXPECT_GT(c.width.front(), 0.0f);

    // No clump channel: spread-out centers instead, as many as the budget allows.
    settings.channel = "noSuchChannel";
    settings.maxCards = 20;
    const CardMesh fallback = buildCards(*s, plane.get(), settings);
    EXPECT_GE(fallback.cards.size(), 10u);
    EXPECT_LE(fallback.cards.size(), 20u);
}

TEST(Cards, ATipNeverFlaresBackOutToItsRootsWidth) {
    // One clump gathering from a wide spread on the scalp into a narrow lock whose ends splay out
    // over the last stretch (frizzed ends, a few strands running on): the card keeps to the lock,
    // its last quarter no wider than its body, not flared back out toward the root's width (the
    // splayed ends bake along its edge).
    const auto scalp = test::makePlane(16);
    auto s = std::make_shared<StrandSet>();
    for (int i = 0; i < 30; ++i) {
        const float a = 0.2094f * static_cast<float>(i), r = 0.05f * static_cast<float>(1 + i % 3);
        const Vec3f root{r * std::cos(a), 0.0f, r * std::sin(a)};
        std::vector<Vec3f> pts;
        for (int k = 0; k <= 16; ++k) {
            const float h = static_cast<float>(k) / 16.0f;
            const float gather = std::max(0.2f, 1.0f - h / 0.4f);  // to a fifth of the spread by 0.4
            Vec3f p{root.x * gather, h, root.z * gather};
            if (h > 0.85f) p += Vec3f{std::cos(a), 0.0f, std::sin(a)} * (1.5f * (h - 0.85f));  // splaying out
            pts.push_back(p);
        }
        s->addStrand(static_cast<StrandId>(i + 1), pts, std::vector<float>(pts.size(), 0.004f), scalp->closestPoint(root).root);
    }
    s->addStrandChannel("clumpId", 0.0f);
    CardSettings settings;
    settings.crossStyle = CardSettings::CrossStyle::Single;  // (a round tuft standing up: crossed by default)
    const CardMesh mesh = buildCards(*s, scalp.get(), settings);
    ASSERT_EQ(mesh.cards.size(), 1u);
    const CardMesh::Card& card = mesh.cards.front();
    const std::size_t n = card.refWidth.size();
    const float body = card.refWidth[3 * n / 4];
    for (std::size_t k = 3 * n / 4 + 1; k < n; ++k) EXPECT_LE(card.refWidth[k], body + 1e-6f) << "station " << k;
    EXPECT_LT(card.refWidth.back(), 0.5f * card.refWidth.front()) << "the tip as wide as the root";
}

TEST(Cards, AFlyawayNodesStraysGetCardsOfTheirOwn) {
    // Two clumps standing off the plane, three of their strands flagged by a Flyaway node: each
    // of those gets a thin card of its own (one strand), within the flyaway cards' budget, and
    // none is part of a clump's card; with no budget they make no card.
    const auto scalp = test::makePlane(16);
    auto s = std::make_shared<StrandSet>();
    std::vector<float> clump, flag;
    for (int c = 0; c < 2; ++c) {
        for (int i = 0; i < 20; ++i) {
            const float a = 0.314f * static_cast<float>(i);
            const Vec3f root{1.0f * c + 0.04f * std::cos(a), 0.0f, 0.04f * std::sin(a)};
            const bool stray = c == 0 ? (i == 3 || i == 11) : i == 7;
            const Vec3f tip = stray ? Vec3f{0.4f * std::cos(a), 0.8f, 0.4f * std::sin(a)} : Vec3f{0.0f, 1.0f, 0.0f};
            s->addStrand(static_cast<StrandId>(c * 100 + i + 1), test::straightCurve(root, tip, 8), {}, scalp->closestPoint(root).root);
            clump.push_back(static_cast<float>(c));
            flag.push_back(stray ? 1.0f : 0.0f);
        }
    }
    s->addStrandChannel("clumpId", 0.0f) = clump;
    s->addStrandChannel("flyaway", 0.0f) = flag;
    CardSettings settings;
    settings.crossStyle = CardSettings::CrossStyle::Single;
    settings.flyawayCards = 5;
    const CardMesh mesh = buildCards(*s, scalp.get(), settings);
    std::size_t flyaways = 0;
    for (const CardMesh::Card& card : mesh.cards) {
        if (card.flyaway) {
            ++flyaways;
            ASSERT_EQ(card.strands.size(), 1u);
            EXPECT_EQ(flag[card.strands.front()], 1.0f);
        } else {
            for (const std::size_t i : card.strands) EXPECT_EQ(flag[i], 0.0f) << "a flagged stray in a clump's card";
        }
    }
    EXPECT_EQ(flyaways, 3u);
    settings.flyawayCards = 0;
    for (const CardMesh::Card& card : buildCards(*s, scalp.get(), settings).cards) {
        EXPECT_FALSE(card.flyaway);
        for (const std::size_t i : card.strands) EXPECT_EQ(flag[i], 0.0f);
    }
}

TEST(Cards, ShapeControls) {
    const StrandsPtr s = clumpedFur();
    const auto plane = test::makePlane(16);
    CardSettings flat;
    flat.segments = 6;
    const CardMesh base = buildCards(*s, plane.get(), flat);

    CardSettings shaped = flat;
    shaped.widthSegments = 4;
    shaped.curvature = 0.8f;
    shaped.twist = 90.0f;
    shaped.widthProfile = Ramp::linear(1.0f, 0.2f);  // taper
    const CardMesh mesh = buildCards(*s, plane.get(), shaped);
    ASSERT_EQ(mesh.cards.size(), base.cards.size());
    EXPECT_EQ(mesh.vertexCount(), mesh.cards.size() * 5 * 7);   // 5 across, 7 along
    EXPECT_EQ(mesh.quads.size(), mesh.cards.size() * 4 * 6 * 4);
    const CardMesh::Card& a = base.cards.front();
    const CardMesh::Card& b = mesh.cards.front();
    // Taper: the tip is narrower relative to the root than on the plain card.
    EXPECT_LT(b.width.back() / b.width.front(), 0.5f * a.width.back() / a.width.front() + 1e-3f);
    // Twist: the tip's side vector has turned a quarter turn from the plain card's.
    EXPECT_LT(std::abs(b.side.back().dot(a.side.back())), 0.1f);
    EXPECT_GT(std::abs(b.side.front().dot(a.side.front())), 0.99f);
    // Curvature: the middle vertex of a row sits out along the normal from the edges.
    const std::size_t row = b.firstVertex + 5 * 3;  // station 3
    const Vec3f edgeMid = (mesh.positions[row] + mesh.positions[row + 4]) * 0.5f;
    EXPECT_GT((mesh.positions[row + 2] - edgeMid).dot(b.normal[3]), 0.1f * b.width[3] * 0.5f);
}

TEST(Cards, TheCardsNodeBuildsCardsFromTheStrandsBeforeIt) {
    GroomData d;
    d.scalp = test::makePlane(16);
    d.graphJson = R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 800.0}},
        {"name": "clump", "type": "Clump", "params": {"count": 12, "strength": 0.8}},
        {"name": "cards", "type": "Cards", "params": {"segments": 4, "widthSegments": 2, "curvature": 0.5}},
        {"name": "frizz", "type": "Noise", "params": {"amplitude": 0.2, "frequency": 8.0, "octaves": 1}},
        {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 10}}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "clump", "input": "strands"},
        {"from": "clump", "to": "cards", "input": "strands"},
        {"from": "cards", "to": "frizz", "input": "strands"}, {"from": "frizz", "to": "out", "input": "strands"}]})j";
    GraphDocument doc = loadGraphJson(d.graphJson);
    ExternalInputs externals;
    setGroomExternals(externals, d.scalp, nullptr, nullptr);
    EvalContext ctx;
    ctx.externals = &externals;
    // Normal evaluation: the node passes strands through and builds nothing.
    const StrandsPtr plain = doc.graph.evaluateStrands(doc.output, ctx);
    ASSERT_TRUE(plain);
    EXPECT_TRUE(plain->cardLayers().empty());
    // Card evaluation: the cards ride along to the output, made from the unfrizzed strands.
    ctx.buildCards = true;
    const StrandsPtr withCards = doc.graph.evaluateStrands(doc.output, ctx);
    ASSERT_EQ(withCards->cardLayers().size(), 1u);
    const CardMesh& layer = *withCards->cardLayers().front();
    ASSERT_GE(layer.cards.size(), 8u);
    ASSERT_TRUE(layer.cards.front().source);
    EXPECT_NE(layer.cards.front().source.get(), withCards.get());
    EXPECT_EQ(layer.cards.front().across, 2);
    CardMesh laid = layer;
    layoutCards(laid, 0.02f);
    EXPECT_EQ(laid.vertexCount(), laid.cards.size() * 3 * 5);
    // Baking reads each card's own strands (before the frizz), not the output.
    CardBakeSettings bake;
    bake.size = 128;
    const CardTextures t = bakeCardTextures(*withCards, laid, bake);
    std::size_t covered = 0;
    for (const auto a : t.coverage) covered += a > 0 ? 1 : 0;
    EXPECT_GT(covered, 128u * 128u / 20u);
}

TEST(Cards, BakeCoversTheCardsAndWritesTga) {
    const StrandsPtr s = clumpedFur();
    const auto plane = test::makePlane(16);
    const CardMesh mesh = buildCards(*s, plane.get(), {});
    CardBakeSettings bake;
    bake.size = 256;
    bake.scalp = plane.get();
    const CardTextures t = bakeCardTextures(*s, mesh, bake);
    ASSERT_EQ(t.coverage.size(), 256u * 256u);
    std::size_t covered = 0;
    for (const auto a : t.coverage) covered += a > 0 ? 1 : 0;
    EXPECT_GT(covered, 256u * 256u / 50);           // strands land in the cells
    EXPECT_LT(covered, 256u * 256u * 9 / 10);       // with gaps between strands and cells
    std::set<int> coordU;
    for (std::size_t p = 0; p < 256u * 256u; ++p) coordU.insert(t.attribute[p * 4 + 3]);
    EXPECT_GT(coordU.size(), 50u);                  // a gradient root to tip
    EXPECT_EQ(t.tangent.size(), 3u * 256u * 256u);
    EXPECT_EQ(t.attribute.size(), 4u * 256u * 256u);
    EXPECT_EQ(t.color.size(), 3u * 256u * 256u);
    EXPECT_EQ(t.occlusion.size(), 256u * 256u);
    // Strands run root to tip: tangents point up the texture (y, the green channel, high).
    double up = 0.0;
    std::size_t n = 0;
    for (std::size_t p = 0; p < 256u * 256u; ++p) {
        if (t.coverage[p] > 128) up += t.tangent[p * 3 + 1], ++n;
    }
    ASSERT_GT(n, 0u);
    EXPECT_GT(up / static_cast<double>(n), 200.0);
    // The normal: out of the card (z, blue, high) where the strands are; DirectX's green, OpenGL's
    // the other way up.
    ASSERT_EQ(t.normal.size(), 3u * 256u * 256u);
    double out = 0.0;
    for (std::size_t p = 0; p < 256u * 256u; ++p) {
        if (t.coverage[p] > 128) out += t.normal[p * 3 + 2];
    }
    EXPECT_GT(out / static_cast<double>(n), 200.0);
    const std::vector<std::uint8_t> gl = cardNormalOpenGl(t);
    for (std::size_t p = 0; p < 256u * 256u; p += 97) {
        EXPECT_EQ(gl[p * 3], t.normal[p * 3]);
        EXPECT_EQ(gl[p * 3 + 1], 255 - t.normal[p * 3 + 1]);
    }

    const std::string path = (std::filesystem::temp_directory_path() / "groomlab_cards_alpha.tga").string();
    writeTga(path, t.size, t.size, 1, t.coverage);
    EXPECT_EQ(std::filesystem::file_size(path), 18u + 256u * 256u);
    std::ifstream in(path, std::ios::binary);
    char header[18];
    in.read(header, 18);
    EXPECT_EQ(header[2], 3);   // grayscale
    EXPECT_EQ(header[16], 8);  // bits
    in.close();
    writeTga(path, t.size, t.size, 4, t.attribute);
    EXPECT_EQ(std::filesystem::file_size(path), 18u + 4u * 256u * 256u);
    std::ifstream rgba(path, std::ios::binary);
    rgba.read(header, 18);
    EXPECT_EQ(header[16], 32);
    EXPECT_EQ(header[17] & 0x0F, 8);  // alpha bits
    rgba.close();
    std::filesystem::remove(path);
    EXPECT_THROW(writeTga(path, 2, 2, 1, {1, 2, 3}), std::runtime_error);
}

TEST(Cards, BakesTouchingCellsDeterministically) {
    // No padding at a small atlas: cells touch, and neighbouring cells could
    // round onto the same pixel column, which the parallel bake would then
    // write from two cards at once. The rectangles keep a pixel apart, so the
    // bake gives the same bytes every time.
    const StrandsPtr s = clumpedFur();
    const auto plane = test::makePlane(16);
    CardSettings settings;
    settings.padding = 0.0f;
    const CardMesh mesh = buildCards(*s, plane.get(), settings);
    ASSERT_GT(mesh.textureCount(), 1u);
    CardBakeSettings bake;
    bake.size = 64;  // (the cells a few pixels wide: one narrower than a pixel bakes nothing)
    bake.occlusion = 0.0f;
    bake.dilate = 0;
    const CardTextures first = bakeCardTextures(*s, mesh, bake);
    std::size_t covered = 0;
    for (const auto a : first.coverage) covered += a > 0 ? 1 : 0;
    EXPECT_GT(covered, 0u);
    for (int i = 0; i < 4; ++i) {
        const CardTextures again = bakeCardTextures(*s, mesh, bake);
        EXPECT_EQ(again.coverage, first.coverage);
        EXPECT_EQ(again.color, first.color);
        EXPECT_EQ(again.attribute, first.attribute);
        EXPECT_EQ(again.tangent, first.tangent);
        EXPECT_EQ(again.depth, first.depth);
    }
    // Every cell keeps its last pixel column and row clear: no card writes there.
    for (const CardMesh::Card& card : mesh.cards) {
        if (card.variationOf >= 0) continue;
        const int x1 = static_cast<int>(std::lround(card.cell[2] * bake.size)) - 1;
        const int y0 = static_cast<int>(std::lround((1.0f - card.cell[3]) * bake.size));
        const int y1 = static_cast<int>(std::lround((1.0f - card.cell[1]) * bake.size)) - 1;
        if (x1 < 0 || x1 >= bake.size) continue;
        for (int y = std::max(y0, 0); y < std::min(y1, bake.size); ++y) {
            EXPECT_EQ(first.coverage[static_cast<std::size_t>(y) * bake.size + static_cast<std::size_t>(x1)], 0) << x1 << "," << y;
        }
    }
}

TEST(Cards, AlikeCardsShareTexturesMirrored) {
    const StrandsPtr s = clumpedFur();
    const auto plane = test::makePlane(16);
    CardSettings settings;
    settings.variations = 4;
    const CardMesh mesh = buildCards(*s, plane.get(), settings);
    ASSERT_GE(mesh.cards.size(), 8u);
    EXPECT_EQ(mesh.textureCount(), 4u);
    std::size_t mirrored = 0;
    for (const CardMesh::Card& card : mesh.cards) {
        if (card.variationOf < 0) continue;
        const CardMesh::Card& owner = mesh.cards[static_cast<std::size_t>(card.variationOf)];
        EXPECT_LT(owner.variationOf, 0);
        EXPECT_FLOAT_EQ(card.cell[1], owner.cell[1]);  // the owner's texture
        EXPECT_FLOAT_EQ(card.cell[3], owner.cell[3]);
        if (card.mirrored) {
            ++mirrored;
            const float u0 = mesh.u[card.firstVertex], u1 = mesh.u[card.firstVertex + 1];
            EXPECT_GT(u0, u1);  // u runs the other way
        }
    }
    EXPECT_GT(mirrored, 0u);
    // Owners' textures don't overlap, and get more room than unique ones would.
    const CardMesh unique = buildCards(*s, plane.get(), CardSettings{});
    float shared = 0.0f, own = 0.0f;
    for (const CardMesh::Card& c : mesh.cards) {
        if (c.variationOf < 0) shared = std::max(shared, c.cell[3] - c.cell[1]);
    }
    for (const CardMesh::Card& c : unique.cards) own = std::max(own, c.cell[3] - c.cell[1]);
    EXPECT_GT(shared, 1.3f * own);
    for (std::size_t a = 0; a < mesh.cards.size(); ++a) {
        for (std::size_t b = a + 1; b < mesh.cards.size(); ++b) {
            if (mesh.cards[a].variationOf >= 0 || mesh.cards[b].variationOf >= 0) continue;
            const float* p = mesh.cards[a].cell;
            const float* q = mesh.cards[b].cell;
            EXPECT_TRUE(p[2] <= q[0] || q[2] <= p[0] || p[3] <= q[1] || q[3] <= p[1]);
        }
    }
    CardBakeSettings bake;
    bake.size = 128;
    const CardTextures t = bakeCardTextures(*s, mesh, bake);  // only the owners bake
    std::size_t covered = 0;
    for (const auto a : t.coverage) covered += a > 0 ? 1 : 0;
    EXPECT_GT(covered, 128u * 128u / 20u);
}

TEST(Cards, StandingTuftsGetCrossedCardsWithinTheBudget) {
    const StrandsPtr s = clumpedFur();  // round clumps standing straight up
    const auto plane = test::makePlane(16);
    CardSettings one;
    one.crossStyle = CardSettings::CrossStyle::Single;
    const std::size_t clumps = buildCards(*s, plane.get(), one).cards.size();
    const CardMesh mesh = buildCards(*s, plane.get(), CardSettings{});  // auto: an X for these tufts
    EXPECT_GE(mesh.cards.size(), clumps * 17 / 10);  // most clumps are round tufts: an X each
    EXPECT_LE(mesh.cards.size(), 2 * clumps);
    std::size_t pairs = 0;
    for (std::size_t c = 0; c + 1 < mesh.cards.size(); ++c) {
        const auto& a = mesh.cards[c];
        const auto& b = mesh.cards[c + 1];
        if (a.strands != b.strands) continue;  // the same clump's two cards
        ++pairs;
        EXPECT_LT(std::abs(a.side[3].dot(b.side[3])), 0.05f);  // crossed at right angles
        // Shaded as the tuft's strands are drawn: out of the scalp, whichever way the card faces.
        for (const Vec3f& n : a.shading) EXPECT_GT(n.y, 0.95f);
        for (const Vec3f& n : b.shading) EXPECT_GT(n.y, 0.95f);
    }
    EXPECT_GE(pairs, clumps * 7 / 10);
    CardSettings star;
    star.crossStyle = CardSettings::CrossStyle::Cross;
    star.crossCount = 3;
    EXPECT_EQ(buildCards(*s, plane.get(), star).cards.size(), 3 * clumps);
    // A budget below that: extra crossed cards go first.
    CardSettings budget;
    budget.maxCards = static_cast<int>(clumps + 2);
    const CardMesh within = buildCards(*s, plane.get(), budget);
    EXPECT_EQ(within.cards.size(), clumps + 2);
}

TEST(Cards, TexturesHaveEvenTexelDensity) {
    const StrandsPtr s = clumpedFur();
    const auto plane = test::makePlane(16);
    const CardMesh mesh = buildCards(*s, plane.get(), {});
    std::vector<float> density;
    for (const CardMesh::Card& c : mesh.cards) {
        float length = 0.0f;
        for (std::size_t k = 1; k < c.center.size(); ++k) length += (c.center[k] - c.center[k - 1]).length();
        density.push_back((c.cell[3] - c.cell[1]) / length);
    }
    const auto [lo, hi] = std::minmax_element(density.begin(), density.end());
    EXPECT_LT(*hi / *lo, 1.001f);
    // Packed, not a sparse grid: much of the atlas is used.
    float area = 0.0f;
    for (const CardMesh::Card& c : mesh.cards) area += (c.cell[2] - c.cell[0]) * (c.cell[3] - c.cell[1]);
    EXPECT_GT(area, 0.3f);
}

TEST(Cards, MaxCardsMergesNeighbouringClumps) {
    const StrandsPtr s = clumpedFur();
    const auto plane = test::makePlane(16);
    CardSettings one;
    one.crossStyle = CardSettings::CrossStyle::Single;
    const CardMesh all = buildCards(*s, plane.get(), one);
    CardSettings low = one;
    low.maxCards = 4;
    const CardMesh fewer = buildCards(*s, plane.get(), low);
    ASSERT_GE(all.cards.size(), 8u);
    EXPECT_LE(fewer.cards.size(), 4u);
    EXPECT_GE(fewer.cards.size(), 2u);
    std::size_t before = 0, after = 0;
    for (const auto& c : all.cards) before += c.strands.size();
    for (const auto& c : fewer.cards) after += c.strands.size();
    EXPECT_GE(after, before);  // every carded strand still on a card
    float wideAll = 0.0f, wideFew = 0.0f;
    for (const auto& c : all.cards) wideAll += c.width.front() / static_cast<float>(all.cards.size());
    for (const auto& c : fewer.cards) wideFew += c.width.front() / static_cast<float>(fewer.cards.size());
    EXPECT_GT(wideFew, wideAll);  // fewer, wider cards
}

// One clump along x = 0..0.2 standing up +y: strands alternately 1 and 0.5
// long, with melanin 0.5.
StrandsPtr shortAndLong() {
    auto s = std::make_shared<StrandSet>();
    for (int i = 0; i < 40; ++i) {
        const float x = 0.2f * static_cast<float>(i) / 39.0f, top = i % 2 ? 0.5f : 1.0f;
        std::vector<Vec3f> pts;
        for (int k = 0; k <= 10; ++k) pts.push_back({x, top * static_cast<float>(k) / 10.0f, 0.0f});
        s->addStrand(static_cast<StrandId>(i + 1), pts, std::vector<float>(pts.size(), 0.004f));
    }
    s->addStrandChannel("clumpId", 0.0f);
    s->addStrandChannel("melanin", 0.5f);
    return s;
}

TEST(Cards, StrandsKeepTheirLengthOnTheCardAndTheirColor) {
    const StrandsPtr s = shortAndLong();
    CardSettings settings;
    settings.minStrands = 1;
    const CardMesh mesh = buildCards(*s, nullptr, settings);
    ASSERT_EQ(mesh.cards.size(), 1u);
    CardBakeSettings bake;
    bake.size = 128;
    bake.occlusion = 0.0f;
    bake.minStrandTexels = 0.0f;  // to scale: coverage counts the strands (a texel wide, they would fill it)
    const CardTextures t = bakeCardTextures(*s, mesh, bake);
    const CardMesh::Card& card = mesh.cards.front();
    // Mean coverage between v0 and v1 of the card's texture (0 root, 1 tip).
    auto band = [&](float v0, float v1) {
        double sum = 0.0;
        std::size_t n = 0;
        const int x0 = static_cast<int>(card.cell[0] * 128), x1 = static_cast<int>(card.cell[2] * 128);
        const int ya = static_cast<int>((1.0f - (card.cell[1] + (card.cell[3] - card.cell[1]) * v1)) * 128);
        const int yb = static_cast<int>((1.0f - (card.cell[1] + (card.cell[3] - card.cell[1]) * v0)) * 128);
        for (int y = ya; y < yb; ++y) {
            for (int x = x0; x < x1; ++x) sum += t.coverage[static_cast<std::size_t>(y) * 128 + static_cast<std::size_t>(x)], ++n;
        }
        return sum / static_cast<double>(std::max<std::size_t>(n, 1));
    };
    EXPECT_LT(band(0.8f, 0.95f), 0.75 * band(0.05f, 0.2f));  // the short ones stop half way
    // Color: the strands' melanin as the hair looks (its appearance: a card is matte), sRGB.
    const std::array<float, 3> linear = hairAppearanceColor(melaninColor(0.5f, 0.2f));
    const std::size_t p = static_cast<std::size_t>(std::max_element(t.coverage.begin(), t.coverage.end()) - t.coverage.begin());
    ASSERT_GT(t.coverage[p], 60);  // thinner than a pixel here: partly covered, as a real strand would be
    for (std::size_t k = 0; k < 3; ++k) {
        const float c = linear[k];
        const float srgb = c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        EXPECT_NEAR(t.color[p * 3 + k], srgb * 255.0f, 2.0f);
    }
}

TEST(Cards, TheGroomBakesOntoItsScalp) {
    // A plane with UVs = (x, z), and hair lying flat along +x just above it on its lower half (z < 0.5).
    const auto bare = test::makePlane(8);
    auto plane = std::make_shared<ScalpMesh>(*bare);
    std::vector<ScalpMesh::UV> uvs;
    for (const auto& tri : plane->triangles()) {
        for (const int v : tri) uvs.push_back({plane->points()[static_cast<std::size_t>(v)].x, plane->points()[static_cast<std::size_t>(v)].z});
    }
    auto s = std::make_shared<StrandSet>();
    for (int i = 0; i < 60; ++i) {
        const float z = 0.5f * static_cast<float>(i) / 59.0f;
        std::vector<Vec3f> pts;
        for (int k = 0; k <= 10; ++k) pts.push_back({0.05f + 0.9f * static_cast<float>(k) / 10.0f, 0.004f, z});
        s->addStrand(static_cast<StrandId>(i + 1), pts, std::vector<float>(pts.size(), 0.01f));
    }
    s->addStrandChannel("melanin", 0.5f);
    ScalpBakeSettings bake;
    bake.size = 64;
    EXPECT_THROW(bakeScalpTextures(*s, *plane, bake), std::invalid_argument);  // no UVs yet
    plane->setCornerUVs(uvs);
    const ScalpTextures t = bakeScalpTextures(*s, *plane, bake);
    ASSERT_EQ(t.coverage.size(), 64u * 64u);
    ASSERT_EQ(t.color.size(), 3u * 64u * 64u);
    // Covered where the hair lies (v < 0.5: the image's bottom half), bare above.
    auto coverage = [&](int y0, int y1) {
        double sum = 0.0;
        for (int y = y0; y < y1; ++y) {
            for (int x = 8; x < 56; ++x) sum += t.coverage[static_cast<std::size_t>(y) * 64 + static_cast<std::size_t>(x)];
        }
        return sum / ((y1 - y0) * 48.0);
    };
    EXPECT_GT(coverage(40, 60), 100.0);
    EXPECT_LT(coverage(4, 24), 5.0);
    // The color is the hair's (its appearance), and fills the bare skin too.
    const std::array<float, 3> linear = hairAppearanceColor(melaninColor(0.5f, 0.2f));
    for (const std::size_t p : {std::size_t{50 * 64 + 30}, std::size_t{10 * 64 + 30}}) {
        for (std::size_t k = 0; k < 3; ++k) {
            const float c = linear[k];
            const float srgb = c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
            EXPECT_NEAR(t.color[p * 3 + k], srgb * 255.0f, 3.0f) << p;
        }
    }
    // Standing hair only marks the skin at its roots.
    const StrandsPtr fur = clumpedFur();
    ScalpBakeSettings wide;
    wide.size = 64;
    const ScalpTextures roots = bakeScalpTextures(*fur, *plane, wide);
    std::size_t covered = 0;
    for (const auto c : roots.coverage) covered += c > 0 ? 1 : 0;
    EXPECT_GT(covered, 64u * 64u / 4u);
}

TEST(Cards, SegmentsOnlyAddEdgeLoopsTheUvsStay) {
    const StrandsPtr s = clumpedFur();
    const auto plane = test::makePlane(16);
    CardSettings coarse;
    coarse.segments = 3;
    coarse.variations = 4;
    CardSettings fine = coarse;
    fine.segments = 12;
    const CardMesh a = buildCards(*s, plane.get(), coarse);
    const CardMesh b = buildCards(*s, plane.get(), fine);
    ASSERT_EQ(a.cards.size(), b.cards.size());
    for (std::size_t c = 0; c < a.cards.size(); ++c) {
        for (int k = 0; k < 4; ++k) EXPECT_FLOAT_EQ(a.cards[c].cell[k], b.cards[c].cell[k]);  // the same texture place
        EXPECT_EQ(a.cards[c].variationOf, b.cards[c].variationOf);
        EXPECT_EQ(a.cards[c].mirrored, b.cards[c].mirrored);
        // The same outline: root and tip rows sit where they did, with the same UVs.
        const auto& ca = a.cards[c];
        const auto& cb = b.cards[c];
        const std::uint32_t acrossA = static_cast<std::uint32_t>(ca.across + 1);
        const std::uint32_t tipA = ca.firstVertex + static_cast<std::uint32_t>(ca.stations - 1) * acrossA;
        const std::uint32_t tipB = cb.firstVertex + static_cast<std::uint32_t>(cb.stations - 1) * acrossA;
        EXPECT_LT((a.positions[tipA] - b.positions[tipB]).length(), 1e-4f);
        EXPECT_FLOAT_EQ(a.v[tipA], b.v[tipB]);
        EXPECT_FLOAT_EQ(a.u[ca.firstVertex], b.u[cb.firstVertex]);
    }
    EXPECT_GT(b.vertexCount(), a.vertexCount());  // more edge loops
}

TEST(Cards, TheBudgetMergesNeighbouringClumps) {
    const StrandsPtr s = clumpedFur();  // clumped: one card per clump by default
    const auto plane = test::makePlane(16);
    CardSettings one;
    one.crossStyle = CardSettings::CrossStyle::Single;
    const std::size_t perClump = buildCards(*s, plane.get(), one).cards.size();
    CardSettings fewer = one;
    fewer.maxCards = 4;
    const std::size_t merged = buildCards(*s, plane.get(), fewer).cards.size();
    EXPECT_GE(perClump, 8u);
    EXPECT_LE(merged, 4u);
    EXPECT_GE(merged, 2u);
}

TEST(Cards, TheHairFitsOnItsCard) {
    const StrandsPtr s = clumpedFur();
    const auto plane = test::makePlane(16);
    CardSettings settings;
    settings.crossStyle = CardSettings::CrossStyle::Single;
    const CardMesh mesh = buildCards(*s, plane.get(), settings);
    // Each card's own strands, sampled along their length: nearly all of them land on the card.
    std::size_t on = 0, all = 0;
    for (const CardMesh::Card& card : mesh.cards) {
        for (const std::size_t si : card.strands) {
            for (const Vec3f& p : resampleByArcLength(s->strandPoints(si), s->strandPointCount(si), 12)) {
                // Nearest reference station, and the offset across the card there.
                std::size_t k = 0;
                for (std::size_t j = 1; j < card.refCenter.size(); ++j) {
                    if ((card.refCenter[j] - p).length() < (card.refCenter[k] - p).length()) k = j;
                }
                const float x = std::abs((p - card.refCenter[k]).dot(card.refSide[k]));
                on += x <= 0.5f * card.refWidth[k] * 1.001f ? 1 : 0;
                ++all;
            }
        }
    }
    EXPECT_GT(static_cast<double>(on) / static_cast<double>(all), 0.9);
}

TEST(Cards, CardsStayClearOfTheScalp) {
    // Hair combed flat along the plane: its card lies on the skin, just above it.
    auto s = std::make_shared<StrandSet>();
    for (int i = 0; i < 30; ++i) {
        std::vector<Vec3f> pts;
        for (int k = 0; k <= 10; ++k) pts.push_back({0.2f + 0.06f * static_cast<float>(k), 0.0f, 0.4f + 0.2f * static_cast<float>(i) / 29.0f});
        s->addStrand(static_cast<StrandId>(i + 1), pts, std::vector<float>(pts.size(), 0.004f));
    }
    s->addStrandChannel("clumpId", 0.0f);
    const auto plane = test::makePlane(16);
    CardSettings settings;
    settings.minStrands = 1;
    settings.widthSegments = 2;
    const CardMesh mesh = buildCards(*s, plane.get(), settings);
    ASSERT_EQ(mesh.cards.size(), 1u);
    float lowest = 1e9f;
    for (const Vec3f& p : mesh.positions) lowest = std::min(lowest, p.y);
    EXPECT_GT(lowest, 0.001f);  // never in the skin
    EXPECT_LT(lowest, 0.05f);   // but on it
}

TEST(Cards, CardBasesSitOnACurvedScalp) {
    // Hair hanging off a sphere in loose clumps, carded as a hero LOD is (sub-cards bowed by
    // their curvature, ruffled, turned the way their strands lie, flyaways): every card's root
    // row sits on the scalp under it, a strand's width above the skin. Before, the row was the
    // stations' straight one across, at the members' mean root lifted clear by a share of the
    // card's length, bowed out by the curvature and tilted by the fit: on the user's head 0.45
    // (median) over the skin, 50 strand widths.
    GroomData d;
    d.scalp = test::makeSphere(16, 24, 1.0f);
    d.graphJson = R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 60.0, "length": 1.5}},
        {"name": "gravity", "type": "Gravity", "params": {"strength": 0.6}},
        {"name": "clump", "type": "Clump", "params": {"count": 30, "strength": {"expression": "rand(3) < 0.05 ? 0.15 : 0.75"}}},
        {"name": "width", "type": "Width", "params": {"width": 0.006}},
        {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 12}}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "gravity", "input": "strands"},
        {"from": "gravity", "to": "clump", "input": "strands"}, {"from": "clump", "to": "width", "input": "strands"},
        {"from": "width", "to": "out", "input": "strands"}]})j";
    const StrandsPtr s = Groom(d).evaluate(1.0f, 0.0);
    ASSERT_GT(s->strandCount(), 400u);
    CardSettings settings;
    settings.cardsPerClump = 6;
    settings.ruffle = 0.3f;
    settings.flyawayCards = 20;
    settings.segments = 10;
    settings.widthSegments = 3;
    settings.curvature = 0.35f;
    settings.variations = 8;
    settings.mirror = false;
    const float strandWidth = 0.006f;
    for (const int layers : {1, 2}) {
        SCOPED_TRACE("layers " + std::to_string(layers));
        settings.layers = layers;
        const CardMesh mesh = buildCards(*s, d.scalp.get(), settings);
        ASSERT_GT(mesh.cards.size(), 60u);
        std::size_t flyaways = 0;
        float worst = 0.0f;
        for (const CardMesh::Card& card : mesh.cards) {
            flyaways += card.flyaway ? 1 : 0;
            for (int e = 0; e <= card.across; ++e) {
                const Vec3f& p = mesh.positions[card.firstVertex + static_cast<std::uint32_t>(e)];
                const SurfacePoint near = d.scalp->closestPoint(p);
                const float height = (p - near.position).dot(near.normal.normalized());
                worst = std::max(worst, std::abs(height));
                EXPECT_GT(height, 0.0f);  // never in the skin
            }
        }
        EXPECT_GT(flyaways, 0u);
        EXPECT_LT(worst, 1.5f * strandWidth) << "root rows off the scalp by up to " << worst / strandWidth << " strand widths";
    }
    // Only the base moved: rows from a tenth of the card on, and every UV, are the stations'.
    settings.layers = 1;
    const CardMesh seated = buildCards(*s, d.scalp.get(), settings);
    CardMesh loose = seated;
    for (CardMesh::Card& card : loose.cards) card.base.clear();
    layoutCards(loose, loose.padding);
    ASSERT_EQ(loose.positions.size(), seated.positions.size());
    EXPECT_EQ(loose.u, seated.u);
    EXPECT_EQ(loose.v, seated.v);
    std::size_t moved = 0;
    for (const CardMesh::Card& card : seated.cards) {
        const std::size_t row = static_cast<std::size_t>(card.across) + 1;
        for (std::size_t v = 0; v < row * static_cast<std::size_t>(card.stations); ++v) {
            const std::size_t at = card.firstVertex + v;
            if (v < row) {
                moved += (seated.positions[at] - loose.positions[at]).length() > 1e-6f ? 1 : 0;
            } else {
                EXPECT_EQ(seated.positions[at], loose.positions[at]);
            }
        }
    }
    EXPECT_GT(moved, seated.cards.size());
}

TEST(Cards, SegmentsByLengthAndCrossStyles) {
    const StrandsPtr s = shortAndLong();  // one clump; split it into short and long for two cards
    auto two = std::make_shared<StrandSet>(*s);
    std::vector<float>& clump = two->addStrandChannel("clumpId");
    for (std::size_t i = 0; i < two->strandCount(); ++i) clump[i] = i % 2 ? 1.0f : 0.0f;  // odd ones are the short
    CardSettings settings;
    settings.minStrands = 1;
    settings.segments = 10;
    settings.crossStyle = CardSettings::CrossStyle::Single;
    const CardMesh fixed = buildCards(*two, nullptr, settings);
    settings.segmentsByLength = true;
    settings.minSegments = 2;
    const CardMesh byLength = buildCards(*two, nullptr, settings);
    ASSERT_EQ(byLength.cards.size(), 2u);
    int shortest = 100, longest = 0;
    for (const auto& c : byLength.cards) shortest = std::min(shortest, c.stations - 1), longest = std::max(longest, c.stations - 1);
    EXPECT_EQ(longest, 10);
    EXPECT_LE(shortest, 6);  // half as long: about half the segments
    EXPECT_GE(shortest, 2);
    for (std::size_t c = 0; c < 2; ++c) {
        for (int k = 0; k < 4; ++k) EXPECT_FLOAT_EQ(fixed.cards[c].cell[k], byLength.cards[c].cell[k]);  // same UVs
    }
    // Cross styles: single, a cross of four, auto.
    const StrandsPtr fur = clumpedFur();
    const auto plane = test::makePlane(16);
    CardSettings single;
    single.crossStyle = CardSettings::CrossStyle::Single;
    const std::size_t clumps = buildCards(*fur, plane.get(), single).cards.size();
    CardSettings four;
    four.crossStyle = CardSettings::CrossStyle::Cross;
    four.crossCount = 4;
    EXPECT_EQ(buildCards(*fur, plane.get(), four).cards.size(), 4 * clumps);
}

// A head of hair: a sphere scalp, a few hundred strands in two dozen loose clumps.
StrandsPtr clumpedHead() {
    GroomData d;
    d.scalp = test::makeSphere(16, 24, 1.0f);
    d.graphJson = R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 40.0}},
        {"name": "clump", "type": "Clump", "params": {"count": 24, "strength": 0.7}},
        {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 12}}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "clump", "input": "strands"},
        {"from": "clump", "to": "out", "input": "strands"}]})j";
    return Groom(d).evaluate(1.0f, 0.0);
}

// The old look: one flat card per clump, facing out of the scalp, no fit, no ruffle.
CardSettings singleCardSettings() {
    CardSettings s;
    s.crossStyle = CardSettings::CrossStyle::Single;
    s.cardsPerClump = 1;
    s.layers = 1;
    s.fitTwist = 0.0f;
    s.ruffle = 0.0f;
    s.margin = 0.0f;
    return s;
}

float quantile(const std::vector<float>& sorted, float q) {
    return sorted[std::min(sorted.size() - 1, static_cast<std::size_t>(q * static_cast<float>(sorted.size())))];
}

TEST(Cards, SubCardsLieMuchCloserToTheHairThanOneCardPerClump) {
    // How far the strands lie from the cards that draw them (95th percentile of samples
    // along every strand), against the clumps' radius. One card per clump, the old path:
    // about 0.5 of the radius here. Sub-cards along their own strands, layered and turned
    // the way the strands lie: well under half of that.
    const StrandsPtr s = clumpedHead();
    const auto sphere = test::makeSphere(16, 24, 1.0f);
    ASSERT_GT(s->strandCount(), 300u);
    const CardMesh single = buildCards(*s, sphere.get(), singleCardSettings());
    ASSERT_GE(single.cards.size(), 12u);  // about the 24 clumps (the smallest make none)
    float radius = 0.0f;
    for (const CardMesh::Card& c : single.cards) radius += 0.5f * c.refWidth[c.refWidth.size() / 2] / static_cast<float>(single.cards.size());
    const std::vector<float> before = cardFitDistances(*s, single);
    CardSettings hero = singleCardSettings();
    hero.cardsPerClump = 4;
    hero.layers = 2;
    hero.fitTwist = 1.0f;
    hero.ruffle = 0.3f;
    hero.margin = 0.1f;
    const CardMesh many = buildCards(*s, sphere.get(), hero);
    const std::vector<float> after = cardFitDistances(*s, many);
    ASSERT_FALSE(before.empty());
    ASSERT_FALSE(after.empty());
    const float p95Before = quantile(before, 0.95f), p95After = quantile(after, 0.95f);
    // Recorded on this groom (radius: the clumps' half-width, 0.36): p95 before 1.17 x radius
    // (one flat card can't hold a round, loose clump), after 0.37 x radius; the fit alone
    // takes 1.17 to 0.98, sub-cards without it reach 0.52. Cards fitted to most of their strands
    // (kFitShare: the farthest tenth bake at the edge rather than widen them) and lying on their
    // strands' outer side (kOuterShell: the groom's silhouette kept) take after to 0.52.
    EXPECT_LT(p95After, 0.5f * p95Before) << p95Before / radius << " -> " << p95After / radius;
    EXPECT_LT(p95After, 0.6f * radius) << p95After / radius;
    EXPECT_GE(many.cards.size(), 4 * single.cards.size());
    EXPECT_LE(many.cards.size(), 8 * single.cards.size());
    // Every strand of every clump is still on a card, on a card of its clump.
    std::size_t carded = 0;
    for (const CardMesh::Card& c : many.cards) {
        EXPECT_GE(c.clump, 0);
        EXPECT_FALSE(c.flyaway);
        carded += c.strands.size();
    }
    std::size_t clumpedStrands = 0;
    for (const CardMesh::Card& c : single.cards) clumpedStrands += c.strands.size();
    EXPECT_GE(carded, clumpedStrands * 9 / 10);
    // Layers: an inner and an outer shell per clump, the outer holding the strands
    // further out of the clump along its normal. Measured against the one-card
    // build's centre line and normals (the clump's mean curve and frame): each
    // strand's mean offset along the normal, compared within its clump.
    std::map<int, const CardMesh::Card*> clumpCard;
    for (const CardMesh::Card& c : single.cards) clumpCard[c.clump] = &c;
    auto depthOf = [&](const CardMesh::Card& clump, std::size_t strand) {
        std::vector<Vec3f> pts(clump.refCenter.size());
        resampleByArcLengthInto(s->strandPoints(strand), s->strandPointCount(strand), pts.data(), pts.size());
        float depth = 0.0f;
        for (std::size_t k = 0; k < pts.size(); ++k) depth += (pts[k] - clump.refCenter[k]).dot(clump.refNormal[k]);
        return depth / static_cast<float>(pts.size());
    };
    struct Shell {
        float depth = 0.0f;
        std::size_t strands = 0;
    };
    std::map<int, std::array<Shell, 2>> clumps;
    for (const CardMesh::Card& c : many.cards) {
        auto it = clumpCard.find(c.clump);
        if (it == clumpCard.end()) continue;
        Shell& shell = clumps[c.clump][c.layer == 0 ? 0 : 1];
        for (const std::size_t strand : c.strands) {
            shell.depth += depthOf(*it->second, strand);
            ++shell.strands;
        }
    }
    std::size_t layered = 0, ordered = 0;
    for (const auto& [clump, shells] : clumps) {
        if (shells[0].strands == 0 || shells[1].strands == 0) continue;
        ++layered;
        if (shells[1].depth / static_cast<float>(shells[1].strands) >= shells[0].depth / static_cast<float>(shells[0].strands)) {
            ++ordered;
        }
    }
    ASSERT_GT(layered, 10u);
    EXPECT_GE(ordered * 10, layered * 9) << ordered << " of " << layered << " clumps have their outer layer further out";
}

TEST(Cards, TheBakeStaysOffTheCellEdges) {
    // Every texture keeps its edge rows and columns clear, even for cards narrower than
    // their hair: strands past the edges are drawn along them, never into the neighbour.
    const StrandsPtr s = clumpedFur();
    const auto plane = test::makePlane(16);
    for (const float scale : {1.0f, 0.4f}) {
        CardSettings settings;
        settings.widthScale = scale;
        settings.cardsPerClump = 2;
        const CardMesh mesh = buildCards(*s, plane.get(), settings);
        CardBakeSettings bake;
        bake.size = 256;
        bake.dilate = 0;
        bake.occlusion = 0.0f;
        const CardTextures t = bakeCardTextures(*s, mesh, bake);
        std::size_t covered = 0, edges = 0;
        for (const CardMesh::Card& card : mesh.cards) {
            if (card.variationOf >= 0) continue;
            const int x0 = static_cast<int>(std::lround(card.cell[0] * bake.size)), x1 = static_cast<int>(std::lround(card.cell[2] * bake.size)) - 1;
            const int y0 = static_cast<int>(std::lround((1.0f - card.cell[3]) * bake.size)), y1 = static_cast<int>(std::lround((1.0f - card.cell[1]) * bake.size)) - 1;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const std::uint8_t a = t.coverage[static_cast<std::size_t>(y) * bake.size + static_cast<std::size_t>(x)];
                    covered += a > 0 ? 1 : 0;
                    if (x == x0 || x == x1 - 1 || y == y0 || y == y1 - 1) edges += a > 0 ? 1 : 0;
                }
            }
        }
        EXPECT_GT(covered, 256u * 256u / 40u) << scale;
        EXPECT_EQ(edges, 0u) << scale;
    }
    // At full width the margin keeps the outer tenth of every texture clear too.
    CardSettings settings;
    settings.cardsPerClump = 2;
    const CardMesh mesh = buildCards(*s, plane.get(), settings);
    CardBakeSettings bake;
    bake.size = 512;
    bake.dilate = 0;
    const CardTextures t = bakeCardTextures(*s, mesh, bake);
    std::size_t inMargin = 0, all = 0;
    for (const CardMesh::Card& card : mesh.cards) {
        if (card.variationOf >= 0) continue;
        const int x0 = static_cast<int>(std::lround(card.cell[0] * bake.size)), x1 = static_cast<int>(std::lround(card.cell[2] * bake.size)) - 1;
        const int y0 = static_cast<int>(std::lround((1.0f - card.cell[3]) * bake.size)), y1 = static_cast<int>(std::lround((1.0f - card.cell[1]) * bake.size)) - 1;
        const int margin = static_cast<int>(0.08f * static_cast<float>(x1 - x0));
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const bool hit = t.coverage[static_cast<std::size_t>(y) * bake.size + static_cast<std::size_t>(x)] > 0;
                all += hit ? 1 : 0;
                if (x < x0 + margin || x >= x1 - margin) inMargin += hit ? 1 : 0;
            }
        }
    }
    EXPECT_GT(all, 0u);
    // Nearly: a card fits most of its strands (kFitShare), the few beyond bake along its edges.
    EXPECT_LT(inMargin, all / 20);
}

TEST(Cards, TheCardReachesTheLongestStrandAndTipsAreDrawn) {
    const StrandsPtr s = shortAndLong();  // strands 1.0 and 0.5 long, the mean curve 0.75
    CardSettings settings;
    settings.minStrands = 1;
    const CardMesh mesh = buildCards(*s, nullptr, settings);
    ASSERT_EQ(mesh.cards.size(), 1u);
    const CardMesh::Card& card = mesh.cards.front();
    EXPECT_GT(card.refCenter.back().y, 1.0f);   // to the long strands' tips
    EXPECT_LT(card.refCenter.back().y, 1.05f);  // and hardly further
    const std::vector<float> fit = cardFitDistances(*s, mesh);
    EXPECT_LT(fit.back(), 1e-3f);  // every sample of every strand lies on the card
    // The long strands' last stretch is baked, not cut off.
    CardBakeSettings bake;
    bake.size = 128;
    bake.occlusion = 0.0f;
    const CardTextures t = bakeCardTextures(*s, mesh, bake);
    const int x0 = static_cast<int>(card.cell[0] * 128), x1 = static_cast<int>(card.cell[2] * 128);
    const int yTip = static_cast<int>((1.0f - (card.cell[1] + (card.cell[3] - card.cell[1]) * 0.97f)) * 128);
    std::size_t tip = 0;
    for (int y = yTip; y < yTip + 3; ++y) {
        for (int x = x0; x < x1; ++x) tip += t.coverage[static_cast<std::size_t>(y) * 128 + static_cast<std::size_t>(x)] > 0 ? 1 : 0;
    }
    EXPECT_GT(tip, 10u);
}

TEST(Cards, ThinStrandsBakeAtLeastATexelWideAtFullCoverage) {
    // A strand a fraction of a texel wide bakes a texel wide and opaque (the viewport draws every
    // strand a pixel wide and opaque), so the coverage holds it where the strands' drawing does;
    // to scale (minStrandTexels 0) it bakes faint, under the cards' cutoff.
    const StrandsPtr s = shortAndLong();  // 0.004 wide on a card about 0.25 across: a third of a texel
    CardSettings settings;
    settings.minStrands = 1;
    const CardMesh mesh = buildCards(*s, nullptr, settings);
    ASSERT_EQ(mesh.cards.size(), 1u);
    CardBakeSettings bake;
    bake.size = 128;
    bake.occlusion = 0.0f;
    bake.dilate = 0;
    const CardTextures solid = bakeCardTextures(*s, mesh, bake);
    bake.minStrandTexels = 0.0f;
    const CardTextures faint = bakeCardTextures(*s, mesh, bake);
    auto opaque = [](const CardTextures& t) {
        return std::count_if(t.coverage.begin(), t.coverage.end(), [](std::uint8_t a) { return a >= 128; });
    };
    EXPECT_GT(opaque(solid), 3 * opaque(faint) / 2);  // recorded: 3085 against 1515
    EXPECT_GE(*std::max_element(solid.coverage.begin(), solid.coverage.end()), 250);
}

// A clump lying along +x on the plane y = 0, its strands at heights 0.01 to 0.09 and across z
// 0.4 to 0.6, plus `strays` strands across farther out at z 0.64 (not so far that they leave it as strays).
StrandsPtr layeredClump(const ScalpMesh& plane, int strays) {
    auto s = std::make_shared<StrandSet>();
    StrandId id = 1;
    auto add = [&](float z, float height) {
        std::vector<Vec3f> pts;
        for (int k = 0; k <= 10; ++k) {
            const float x = 0.1f + 0.06f * static_cast<float>(k);
            pts.push_back({x, k == 0 ? 0.0f : height, z});
        }
        s->addStrand(id++, pts, std::vector<float>(pts.size(), 0.004f), plane.closestPoint(pts.front()).root);
    };
    for (int i = 0; i < 9; ++i) {
        for (int j = 0; j < 5; ++j) add(0.4f + 0.05f * static_cast<float>(j), 0.01f + 0.01f * static_cast<float>(i));
    }
    for (int k = 0; k < strays; ++k) add(0.64f, 0.05f);
    s->addStrandChannel("clumpId", 0.0f);
    return s;
}

TEST(Cards, AFewFarStrandsDontWidenTheCardAndItLiesOnItsStrandsOuterSide) {
    // The groom's envelope at any level of detail: a card fits most of its strands (the farthest
    // few bake along its edge), so it is no wider than they are, and lies where a quarter of them
    // are farther out (the silhouette is the outermost strands), never past the outermost.
    const auto plane = test::makePlane(8);
    CardSettings settings;
    settings.fitTwist = 0.0f;  // facing out of the plane
    settings.ruffle = 0.0f;
    settings.margin = 0.0f;
    const CardMesh clean = buildCards(*layeredClump(*plane, 0), plane.get(), settings);
    const CardMesh strayed = buildCards(*layeredClump(*plane, 3), plane.get(), settings);
    ASSERT_EQ(clean.cards.size(), 1u);
    ASSERT_EQ(strayed.cards.size(), 1u);
    const CardMesh::Card& a = clean.cards.front();
    const CardMesh::Card& b = strayed.cards.front();
    EXPECT_NEAR(a.textureWidth, 0.2f, 0.03f);
    EXPECT_LT(b.textureWidth, 1.2f * a.textureWidth);  // as wide as the strays: 0.3
    const std::size_t mid = a.refCenter.size() / 2;
    EXPECT_GT(a.refCenter[mid].y, 0.055f);  // past the middle of the strands' heights (0.05)
    EXPECT_LT(a.refCenter[mid].y, 0.09f);   // and inside them
    EXPECT_EQ(b.strands.size(), 48u);       // the strays are still on it, drawn at its edge
}

TEST(Cards, CardsShadeLikeTheirStrandsLitDrawing) {
    // The strands' lit drawing leans each strand's normal out of the scalp at its root, half its
    // part along the strand taken out; a card of strands rising at 45 degrees shades the same.
    const auto plane = test::makePlane(8);
    auto s = std::make_shared<StrandSet>();
    for (int i = 0; i < 12; ++i) {
        const float z = 0.4f + 0.02f * static_cast<float>(i);
        std::vector<Vec3f> pts;
        for (int k = 0; k <= 8; ++k) pts.push_back({0.2f + 0.05f * static_cast<float>(k), 0.05f * static_cast<float>(k), z});
        s->addStrand(static_cast<StrandId>(i + 1), pts, std::vector<float>(pts.size(), 0.004f), plane->closestPoint(pts.front()).root);
    }
    s->addStrandChannel("clumpId", 0.0f);
    const CardMesh mesh = buildCards(*s, plane.get(), CardSettings{});
    ASSERT_EQ(mesh.cards.size(), 1u);
    const Vec3f t = Vec3f{1.0f, 1.0f, 0.0f}.normalized(), up{0.0f, 1.0f, 0.0f};
    const Vec3f expected = (up - t * (0.5f * up.dot(t))).normalized();
    const CardMesh::Card& card = mesh.cards.front();
    for (std::size_t k = 1; k + 1 < card.shading.size(); ++k) EXPECT_GT(card.shading[k].dot(expected), 0.99f) << k;
    // The viewport carries that normal in the bitangent stream too, which two-sided lighting
    // leaves alone (the card effect reads it there).
    const DisplayGeometry g = cardGeometry(mesh, true);
    ASSERT_EQ(g.bitangents.size(), g.normals.size());
    for (std::size_t i = 0; i < g.normals.size(); ++i) EXPECT_FLOAT_EQ(g.bitangents[i], g.normals[i]);
}

// The cards' wireframe: every quad's four edges, each once (neighbors share theirs), no diagonals.
TEST(Cards, WireframeIsTheQuadsEdgesOnce) {
    const auto plane = test::makePlane(16);
    const CardMesh mesh = buildCards(*clumpedFur(), plane.get(), CardSettings{});
    ASSERT_FALSE(mesh.cards.empty());
    const DisplayGeometry g = cardGeometry(mesh, false);
    const DisplayGeometry wire = cardWireGeometry(g);
    EXPECT_FALSE(wire.triangles);
    EXPECT_EQ(wire.positions, g.positions);
    // A card of r rows and c columns of quads has r (c + 1) + c (r + 1) edges.
    std::size_t expected = 0;
    for (const CardMesh::Card& card : mesh.cards) {
        const std::size_t rows = static_cast<std::size_t>(card.stations - 1), columns = static_cast<std::size_t>(std::max(card.across, 1));
        expected += rows * (columns + 1) + columns * (rows + 1);
    }
    EXPECT_EQ(wire.indices.size(), 2 * expected);
    std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
    for (std::size_t i = 0; i < wire.indices.size(); i += 2) {
        EXPECT_TRUE(seen.insert({wire.indices[i], wire.indices[i + 1]}).second) << "an edge twice";
    }
    EXPECT_TRUE(cardWireGeometry(DisplayGeometry{}).empty());
}

// Two clumps alike in shape side by side, standing up: clump 0 dark, clump 1 light, both
// blending toward a light tip per point (HairColor's colorR/G/B, per strand and per point).
StrandsPtr twoColoredClumps() {
    auto s = std::make_shared<StrandSet>();
    std::vector<float> rgb[3];
    for (int clump = 0; clump < 2; ++clump) {
        for (int i = 0; i < 20; ++i) {
            const float x = 0.5f * static_cast<float>(clump) + 0.1f * static_cast<float>(i) / 19.0f;
            std::vector<Vec3f> pts;
            for (int k = 0; k <= 10; ++k) pts.push_back({x, 0.1f * static_cast<float>(k), 0.0f});
            s->addStrand(static_cast<StrandId>(clump * 100 + i + 1), pts, std::vector<float>(pts.size(), 0.004f));
        }
    }
    std::vector<float>& clumpId = s->addStrandChannel("clumpId", 0.0f);
    for (std::size_t i = 20; i < 40; ++i) clumpId[i] = 1.0f;
    const std::array<float, 3> base[2] = {{0.1f, 0.05f, 0.02f}, {0.4f, 0.3f, 0.2f}}, tip = {0.6f, 0.5f, 0.4f};
    for (int k = 0; k < 3; ++k) {
        std::vector<float>& strand = s->addStrandChannel(kColorChannels[k], 0.0f);
        std::vector<float>& point = s->addPointChannel(kColorChannels[k], 0.0f);
        for (std::size_t i = 0; i < s->strandCount(); ++i) {
            const std::array<float, 3>& b = base[i < 20 ? 0 : 1];
            strand[i] = b[static_cast<std::size_t>(k)];
            for (std::size_t p = 0; p < s->strandPointCount(i); ++p) {
                const float t = static_cast<float>(p) / static_cast<float>(s->strandPointCount(i) - 1);
                point[s->strandBegin(i) + p] = b[static_cast<std::size_t>(k)] + (tip[static_cast<std::size_t>(k)] - b[static_cast<std::size_t>(k)]) * t;
            }
        }
    }
    return s;
}

TEST(Cards, ACardSharingATextureIsTintedToItsOwnStrandsColor) {
    // Textures are shared by shape; a card showing another card's texture (baked from strands of
    // another color) is tinted by its own strands' color over theirs, root to tip, so it shows
    // its own color; the owner shows its texture as baked (no tint).
    const StrandsPtr s = twoColoredClumps();
    CardSettings settings;
    settings.variations = 1;
    settings.mirror = false;
    const CardMesh mesh = buildCards(*s, nullptr, settings);
    ASSERT_EQ(mesh.cards.size(), 2u);
    ASSERT_EQ(mesh.tints.size(), mesh.positions.size());
    const std::size_t owner = mesh.cards[0].variationOf < 0 ? 0 : 1, sharer = 1 - owner;
    ASSERT_EQ(mesh.cards[sharer].variationOf, static_cast<int>(owner));
    const auto tintAt = [&](std::size_t c, int station) {
        const CardMesh::Card& card = mesh.cards[c];
        return mesh.tints[card.firstVertex + static_cast<std::size_t>(station) * static_cast<std::size_t>(card.across + 1)];
    };
    const int last = mesh.cards[sharer].stations - 1;
    for (const int station : {0, last}) {
        const std::array<float, 3> own = tintAt(owner, station), other = tintAt(sharer, station);
        for (std::size_t k = 0; k < 3; ++k) EXPECT_NEAR(own[k], 1.0f, 1e-4f);
        // Root: the strands' own color over the owner's (clamped to kCardTintMax); at the tip
        // both blend to the same color, so the tint fades to none.
        const std::array<float, 3> rootDark = hairAppearanceColor({0.1f, 0.05f, 0.02f}),
                                    rootLight = hairAppearanceColor({0.4f, 0.3f, 0.2f});
        const bool sharerDark = sharer == 0;
        for (std::size_t k = 0; k < 3; ++k) {
            const float want = station == 0 ? std::min((sharerDark ? rootDark : rootLight)[k] / (sharerDark ? rootLight : rootDark)[k], kCardTintMax)
                                            : 1.0f;
            EXPECT_NEAR(other[k], want, 0.03f) << "station " << station << " channel " << k;
        }
    }
    // The baked atlas keeps what each cell was baked from: a card rebuilt onto it tints the same.
    const CardAtlas atlas = CardAtlas::fromJson(captureCardAtlas(mesh, s.get()).toJson());
    ASSERT_EQ(atlas.entries[sharer].colors.size(), static_cast<std::size_t>(kCardColorStations));
    CardMesh rebuilt = buildCards(*s, nullptr, settings);
    applyCardAtlas(rebuilt, atlas, s.get());
    ASSERT_EQ(rebuilt.tints.size(), mesh.tints.size());
    for (std::size_t v = 0; v < mesh.tints.size(); ++v) {
        for (std::size_t k = 0; k < 3; ++k) EXPECT_NEAR(rebuilt.tints[v][k], mesh.tints[v][k], 1e-4f);
    }
}

TEST(Cards, TheColorMapCarriesTheStrandsRootToTipColor) {
    // HairColor's tip blend (per point colors) bakes into the color map along the card, as the
    // strands' lit drawing shows it: the root band the root color, the tip band the tip's.
    const StrandsPtr s = twoColoredClumps();
    CardSettings settings;
    settings.variations = 0;
    const CardMesh mesh = buildCards(*s, nullptr, settings);
    ASSERT_EQ(mesh.cards.size(), 2u);
    CardBakeSettings bake;
    bake.size = 256;
    bake.occlusion = 0.0f;
    const CardTextures t = bakeCardTextures(*s, mesh, bake);
    const CardMesh::Card& dark = mesh.cards[0].strands.front() < 20 ? mesh.cards[0] : mesh.cards[1];
    // The mean red (sRGB bytes) of the covered pixels in a band along the dark card's texture.
    auto red = [&](float v0, float v1) {
        double sum = 0.0, n = 0.0;
        const int x0 = static_cast<int>(dark.cell[0] * 256), x1 = static_cast<int>(dark.cell[2] * 256);
        const int ya = static_cast<int>((1.0f - (dark.cell[1] + (dark.cell[3] - dark.cell[1]) * v1)) * 256);
        const int yb = static_cast<int>((1.0f - (dark.cell[1] + (dark.cell[3] - dark.cell[1]) * v0)) * 256);
        for (int y = ya; y < yb; ++y) {
            for (int x = x0; x < x1; ++x) {
                const std::size_t p = static_cast<std::size_t>(y) * 256 + static_cast<std::size_t>(x);
                if (t.coverage[p] < 128) continue;
                sum += t.color[p * 3], n += 1.0;
            }
        }
        return n > 0.0 ? sum / n : 0.0;
    };
    const auto srgb = [](float c) { return 255.0 * (c <= 0.0031308f ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055); };
    // (As the hair looks: hairAppearanceColor, channel by channel.)
    const auto shown = [](float c) { return hairAppearanceColor({c, c, c})[0]; };
    EXPECT_NEAR(red(0.1f, 0.2f), srgb(shown(0.1f + 0.5f * 0.15f)), 25.0);   // near the root: dark
    EXPECT_NEAR(red(0.85f, 0.95f), srgb(shown(0.1f + 0.5f * 0.9f)), 25.0);  // near the tip: the tip color
}

TEST(Cards, TheFitTurnsTheCardTheWayTheStrandsLie) {
    // A sheet of strands standing up from the plane, spread along z at the root and
    // turning to lie along x by the tip (a helicoid): the card follows it around.
    auto s = std::make_shared<StrandSet>();
    for (int i = 0; i < 30; ++i) {
        const float r = 0.15f * (static_cast<float>(i) / 29.0f - 0.5f);
        std::vector<Vec3f> pts;
        for (int k = 0; k <= 16; ++k) {
            const float t = static_cast<float>(k) / 16.0f;
            const float a = 1.5707963f * t;
            pts.push_back({0.5f + r * std::sin(a), 0.6f * t, 0.5f + r * std::cos(a)});
        }
        s->addStrand(static_cast<StrandId>(i + 1), pts, std::vector<float>(pts.size(), 0.004f));
    }
    s->addStrandChannel("clumpId", 0.0f);
    const auto plane = test::makePlane(16);
    CardSettings settings;
    settings.minStrands = 1;
    settings.crossStyle = CardSettings::CrossStyle::Single;
    settings.ruffle = 0.0f;
    const CardMesh fitted = buildCards(*s, plane.get(), settings);
    ASSERT_EQ(fitted.cards.size(), 1u);
    const CardMesh::Card& card = fitted.cards.front();
    EXPECT_GT(std::abs(card.refSide.front().z), 0.95f);  // along z at the root
    EXPECT_GT(std::abs(card.refSide.back().x), 0.9f);    // along x at the tip
    for (std::size_t k = 0; k < card.refCenter.size(); ++k) {
        EXPECT_NEAR(card.refWidth[k], 0.15f / 0.8f + 0.008f / 0.8f, 0.03f) << k;  // the sheet's width, plus the margin
    }
    EXPECT_LT(cardFitDistances(*s, fitted).back(), 0.01f);
    // Without the fit the card keeps one facing, and the strands leave its plane.
    settings.fitTwist = 0.0f;
    settings.margin = 0.0f;
    const CardMesh flat = buildCards(*s, plane.get(), settings);
    EXPECT_GT(cardFitDistances(*s, flat).back(), 0.04f);
}

TEST(Cards, ABakedAtlasKeepsEveryCardsCellThroughARebuild) {
    // Cards baked, then the groom brushed: the rebuilt cards take the cells they were baked
    // with (by the strands they share with the baked cards), so the maps keep fitting; a card
    // of strands no baked card had borrows a baked card's cell rather than going blank.
    const StrandsPtr s = clumpedFur();
    const auto plane = test::makePlane(16);
    CardSettings settings;
    settings.segments = 6;
    settings.crossStyle = CardSettings::CrossStyle::Single;
    const CardMesh baked = buildCards(*s, plane.get(), settings);
    ASSERT_GE(baked.cards.size(), 8u);
    const CardAtlas atlas = CardAtlas::fromJson(captureCardAtlas(baked, s.get()).toJson());  // round trip
    ASSERT_EQ(atlas.entries.size(), baked.cards.size());
    for (std::size_t c = 0; c < baked.cards.size(); ++c) {
        EXPECT_EQ(atlas.entries[c].strands.size(), baked.cards[c].strands.size());
        EXPECT_FLOAT_EQ(atlas.entries[c].textureWidth, baked.cards[c].textureWidth);
    }

    // Brushed: every tip leans over, and the strands are listed in another order.
    auto brushed = std::make_shared<StrandSet>();
    std::vector<std::size_t> order(s->strandCount());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = order.size() - 1 - i;
    for (const std::size_t i : order) {
        const Vec3f* pts = s->strandPoints(i);
        std::vector<Vec3f> moved(pts, pts + s->strandPointCount(i));
        for (std::size_t k = 1; k < moved.size(); ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(moved.size() - 1);
            moved[k] += Vec3f{0.15f * t * t, 0.0f, 0.05f * t};
        }
        brushed->addStrand(s->ids()[i], moved, std::vector<float>(moved.size(), 0.01f));
    }
    if (const auto* ids = s->strandChannel("clumpId")) {
        std::vector<float>& channel = brushed->addStrandChannel("clumpId", 0.0f);
        for (std::size_t n = 0; n < order.size(); ++n) channel[n] = (*ids)[order[n]];
    }
    CardMesh rebuilt = buildCards(*brushed, plane.get(), settings);
    ASSERT_GE(rebuilt.cards.size(), 8u);
    const std::size_t matched = applyCardAtlas(rebuilt, atlas, brushed.get());
    EXPECT_EQ(matched, rebuilt.cards.size());
    for (const CardMesh::Card& card : rebuilt.cards) {
        // Its cell is the baked cell of the card sharing most of its strands.
        std::size_t best = SIZE_MAX, most = 0;
        for (std::size_t e = 0; e < atlas.entries.size(); ++e) {
            std::size_t shared = 0;
            for (const std::size_t si : card.strands) {
                const std::uint64_t id = brushed->ids()[si];
                shared += static_cast<std::size_t>(std::count(atlas.entries[e].strands.begin(), atlas.entries[e].strands.end(), id));
            }
            if (shared > most) most = shared, best = e;
        }
        ASSERT_NE(best, SIZE_MAX);
        for (int k = 0; k < 4; ++k) EXPECT_FLOAT_EQ(card.cell[k], atlas.entries[best].cell[k]);
        EXPECT_FLOAT_EQ(card.textureWidth, atlas.entries[best].textureWidth);
        // The UVs stay inside the cell however the card grew.
        const auto columns = static_cast<std::uint32_t>(card.across + 1);
        for (std::uint32_t v = card.firstVertex; v < card.firstVertex + columns * static_cast<std::uint32_t>(card.stations); ++v) {
            EXPECT_GE(rebuilt.u[v], std::min(card.cell[0], card.cell[2]) - 1e-5f);
            EXPECT_LE(rebuilt.u[v], std::max(card.cell[0], card.cell[2]) + 1e-5f);
        }
    }

    // A card of strands the bake never saw borrows a baked cell (the same one every time).
    auto strangers = std::make_shared<StrandSet>();
    for (int i = 0; i < 6; ++i) {
        strangers->addStrand(static_cast<StrandId>(900000 + i),
                             test::straightCurve({0.5f + 0.01f * static_cast<float>(i), 0.0f, 0.5f}, {0.0f, 1.0f, 0.0f}, 8),
                             std::vector<float>(9, 0.003f));
    }
    strangers->addStrandChannel("clumpId", 0.0f);
    CardMesh stranger = buildCards(*strangers, nullptr, settings);
    ASSERT_EQ(stranger.cards.size(), 1u);
    EXPECT_EQ(applyCardAtlas(stranger, atlas, strangers.get()), 0u);
    bool borrowed = false;
    for (const CardAtlas::Entry& entry : atlas.entries) {
        borrowed = borrowed || std::equal(std::begin(entry.cell), std::end(entry.cell), std::begin(stranger.cards[0].cell));
    }
    EXPECT_TRUE(borrowed);
    CardMesh again = buildCards(*strangers, nullptr, settings);
    applyCardAtlas(again, atlas, strangers.get());
    for (int k = 0; k < 4; ++k) EXPECT_FLOAT_EQ(again.cards[0].cell[k], stranger.cards[0].cell[k]);
}

TEST(Cards, TipsTaperAndNeverFlare) {
    // Six strands up, five ending at 0.7 and one going on to 1.0 with its tip flaring
    // sideways: the card runs on to that tip, but its last quarter is never wider than the
    // card before it, and it narrows no faster than a quarter of its width per station, so
    // no spearhead (a widest station right before one a strand width wide).
    auto s = std::make_shared<StrandSet>();
    for (int i = 0; i < 5; ++i) {
        const float x = 0.45f + 0.1f * static_cast<float>(i) / 4.0f;
        s->addStrand(static_cast<StrandId>(i + 1), test::straightCurve({x, 0.0f, 0.5f}, {0.0f, 0.7f, 0.0f}, 12),
                     std::vector<float>(13, 0.003f));
    }
    std::vector<Vec3f> flare;
    for (int k = 0; k <= 12; ++k) {
        const float t = static_cast<float>(k) / 12.0f;
        flare.push_back({0.5f + (t > 0.7f ? 0.6f * (t - 0.7f) / 0.3f : 0.0f), t, 0.5f});
    }
    s->addStrand(6, flare, std::vector<float>(13, 0.003f));
    s->addStrandChannel("clumpId", 0.0f);
    CardSettings settings;
    settings.crossStyle = CardSettings::CrossStyle::Single;
    settings.ruffle = 0.0f;
    settings.segments = 16;
    settings.minStrands = 6;  // no stray removal: the flare stays a member
    const CardMesh mesh = buildCards(*s, nullptr, settings);
    ASSERT_EQ(mesh.cards.size(), 1u);
    const CardMesh::Card& card = mesh.cards.front();
    const std::vector<float>& w = card.refWidth;
    float widest = 0.0f;
    for (std::size_t k = 0; k <= 3 * w.size() / 4; ++k) widest = std::max(widest, w[k]);
    for (std::size_t k = 3 * w.size() / 4 + 1; k < w.size(); ++k) EXPECT_LE(w[k], widest * 1.001f) << k;
    for (std::size_t k = 1; k < w.size(); ++k) EXPECT_GE(w[k], w[k - 1] - 0.26f * widest) << k;
    EXPECT_GT(w.back(), 0.3f * widest);  // the end is a taper, not a point
}

TEST(Cards, TheTextureKeepsOneScaleAlongATaperedCard) {
    // Straight parallel strands on a card tapering to a fifth of its root width: the mesh
    // narrows toward the tip, but the texture keeps one scale along the whole card (the
    // widest station's), so a point at an offset across maps to the same u at every
    // station, the tip's vertices use the middle band of the cell, and every strand bakes
    // as a straight column instead of bending with the width.
    auto s = std::make_shared<StrandSet>();
    constexpr int kStrands = 7;
    for (int i = 0; i < kStrands; ++i) {
        const float x = 0.45f + 0.1f * static_cast<float>(i) / static_cast<float>(kStrands - 1);
        s->addStrand(static_cast<StrandId>(i + 1), test::straightCurve({x, 0.0f, 0.5f}, {0.0f, 1.0f, 0.0f}, 12),
                     std::vector<float>(13, 0.003f));
    }
    s->addStrandChannel("clumpId", 0.0f);
    CardSettings settings;
    settings.crossStyle = CardSettings::CrossStyle::Single;
    settings.ruffle = 0.0f;
    settings.segments = 8;
    settings.widthProfile = Ramp::linear(1.0f, 0.2f);
    const CardMesh mesh = buildCards(*s, nullptr, settings);
    ASSERT_EQ(mesh.cards.size(), 1u);
    const CardMesh::Card& card = mesh.cards.front();
    ASSERT_EQ(card.strands.size(), static_cast<std::size_t>(kStrands));
    // The card itself tapers; its texture frame is the widest station.
    EXPECT_LT(card.refWidth.back(), 0.3f * card.refWidth.front());
    EXPECT_FLOAT_EQ(card.textureWidth, *std::max_element(card.refWidth.begin(), card.refWidth.end()));
    // Mesh UVs: every station's row spans the cell at the same scale (u per scene unit), centred
    // in the cell, so the tip's u range is as much narrower than the root's as the card is.
    const float cellW = card.cell[2] - card.cell[0];
    const auto columns = static_cast<std::uint32_t>(card.across + 1);
    const float scale = cellW / card.textureWidth;
    float rootRange = 0.0f, tipRange = 0.0f;
    const float probe = 0.01f;  // a point 0.01 across the card from its centre line
    float probeU = -1.0f;
    for (int k = 0; k < card.stations; ++k) {
        const std::uint32_t left = card.firstVertex + static_cast<std::uint32_t>(k) * columns, right = left + columns - 1;
        const float range = mesh.u[right] - mesh.u[left];
        EXPECT_NEAR(range / card.width[static_cast<std::size_t>(k)], scale, 1e-4f) << k;
        EXPECT_NEAR(0.5f * (mesh.u[left] + mesh.u[right]), card.cell[0] + 0.5f * cellW, 1e-5f) << k;
        // The same offset across lands on the same u at every station.
        const float u = 0.5f * (mesh.u[left] + mesh.u[right]) + probe * range / card.width[static_cast<std::size_t>(k)];
        if (k == 0) probeU = u;
        EXPECT_NEAR(u, probeU, 1e-5f) << k;
        if (k == 0) rootRange = range;
        if (k == card.stations - 1) tipRange = range;
    }
    EXPECT_LT(tipRange, 0.3f * rootRange);
    EXPECT_NEAR(rootRange, cellW, 1e-5f);  // the widest station fills the cell
    // Bake: each strand a straight column, at the u its offset across says.
    CardBakeSettings bake;
    bake.size = 256;
    bake.dilate = 0;
    bake.occlusion = 0.0f;
    bake.rootFade = 0.0f;
    const CardTextures t = bakeCardTextures(*s, mesh, bake);
    const int x0 = static_cast<int>(std::lround(card.cell[0] * bake.size)), x1 = static_cast<int>(std::lround(card.cell[2] * bake.size)) - 1;
    const int y0 = static_cast<int>(std::lround((1.0f - card.cell[3]) * bake.size)), y1 = static_cast<int>(std::lround((1.0f - card.cell[1]) * bake.size)) - 1;
    ASSERT_GT(x1 - x0, 20);
    const auto row = [&](int y) {
        std::vector<int> cols;
        for (int x = x0; x < x1; ++x) {
            if (t.coverage[static_cast<std::size_t>(y) * bake.size + static_cast<std::size_t>(x)] > 0) cols.push_back(x);
        }
        return cols;
    };
    const int yTop = y0 + static_cast<int>(0.15f * static_cast<float>(y1 - y0)), yBottom = y0 + static_cast<int>(0.85f * static_cast<float>(y1 - y0));
    const std::vector<int> reference = row(yBottom);
    ASSERT_GE(reference.size(), static_cast<std::size_t>(kStrands));
    for (int y = yTop; y <= yBottom; ++y) EXPECT_EQ(row(y), reference) << "row " << y;
    for (const std::size_t si : card.strands) {
        const float offset = (s->strandPoints(si)[0] - card.refCenter.front()).dot(card.refSide.front());
        const float u = card.cell[0] + cellW * (0.5f + offset / card.textureWidth);
        const int px = static_cast<int>(std::floor(u * static_cast<float>(bake.size)));
        bool hit = false;
        for (int x = px - 1; x <= px + 1; ++x) hit = hit || std::find(reference.begin(), reference.end(), x) != reference.end();
        EXPECT_TRUE(hit) << "strand " << si << " at column " << px;
    }
}

TEST(Cards, AStrayStrandLeavesItsCardForAFlyawayCard) {
    // A bundle of straight strands and one that bends far out of it (a flyaway): the
    // card stays as wide as the bundle, and the stray gets a thin card of its own.
    auto s = std::make_shared<StrandSet>();
    for (int i = 0; i < 24; ++i) {
        const float x = 0.45f + 0.1f * static_cast<float>(i) / 23.0f;
        s->addStrand(static_cast<StrandId>(i + 1), test::straightCurve({x, 0.0f, 0.5f}, {0.0f, 1.0f, 0.0f}, 12),
                     std::vector<float>(13, 0.004f));
    }
    std::vector<Vec3f> stray;
    for (int k = 0; k <= 12; ++k) {
        const float t = static_cast<float>(k) / 12.0f;
        stray.push_back({0.5f + 0.9f * t * t, t, 0.5f});
    }
    s->addStrand(99, stray, std::vector<float>(13, 0.004f));
    s->addStrandChannel("clumpId", 0.0f);
    const auto plane = test::makePlane(16);
    CardSettings settings;
    settings.crossStyle = CardSettings::CrossStyle::Single;
    settings.ruffle = 0.0f;
    settings.margin = 0.0f;
    settings.flyawayCards = 4;
    const CardMesh cards = buildCards(*s, plane.get(), settings);
    ASSERT_GE(cards.cards.size(), 2u);
    const CardMesh::Card* bundle = nullptr;
    const CardMesh::Card* flyaway = nullptr;
    for (const CardMesh::Card& c : cards.cards) {
        if (c.flyaway) flyaway = &c;
        else bundle = &c;
    }
    ASSERT_TRUE(bundle);
    ASSERT_TRUE(flyaway);
    EXPECT_EQ(bundle->strands.size(), 24u);
    EXPECT_EQ(std::count(bundle->strands.begin(), bundle->strands.end(), 24u), 0);  // the stray (index 24) left
    for (const float w : bundle->refWidth) EXPECT_LT(w, 0.2f);  // the bundle's 0.1 plus strand widths, not the stray's 0.9
    EXPECT_EQ(flyaway->strands, std::vector<std::size_t>{24});
    // Without a flyaway budget the stray is still left out rather than widening the card.
    settings.flyawayCards = 0;
    const CardMesh tight = buildCards(*s, plane.get(), settings);
    for (const CardMesh::Card& c : tight.cards) {
        for (const float w : c.refWidth) EXPECT_LT(w, 0.2f);
    }
}

TEST(Cards, RuffleAlternatesCardsInAndOut) {
    const StrandsPtr s = clumpedFur();
    const auto plane = test::makePlane(16);
    CardSettings settings;
    settings.crossStyle = CardSettings::CrossStyle::Single;
    settings.cardsPerClump = 2;
    settings.ruffle = 0.0f;
    const CardMesh plain = buildCards(*s, plane.get(), settings);
    settings.ruffle = 1.0f;
    const CardMesh ruffled = buildCards(*s, plane.get(), settings);
    ASSERT_EQ(plain.cards.size(), ruffled.cards.size());
    std::size_t out = 0, in = 0;
    for (std::size_t c = 0; c < plain.cards.size(); ++c) {
        const CardMesh::Card& a = plain.cards[c];
        const CardMesh::Card& b = ruffled.cards[c];
        ASSERT_EQ(a.strands, b.strands);
        const std::size_t mid = a.refCenter.size() / 2;
        const float push = (b.refCenter[mid] - a.refCenter[mid]).dot(a.refNormal[mid]) / a.refWidth[mid];
        if (push > 0.05f) ++out;
        if (push < -0.05f) ++in;
        EXPECT_LT(std::abs(a.refSide[mid].dot(b.refSide[mid])), 0.995f);  // turned a little
    }
    EXPECT_GT(out, 0u);
    EXPECT_GT(in, 0u);
}

TEST(Cards, LooseStrandsJoinTheNearestCardOrGetFlyawayCards) {
    // One clump standing up along x = 0..0.2, three loose strands beside it (within its
    // width) and two far off, with no clump.
    auto s = std::make_shared<StrandSet>();
    auto add = [&](int id, float x, float z, float top) {
        std::vector<Vec3f> pts;
        for (int k = 0; k <= 10; ++k) pts.push_back({x, top * static_cast<float>(k) / 10.0f, z});
        s->addStrand(static_cast<StrandId>(id), pts, std::vector<float>(pts.size(), 0.004f));
    };
    for (int i = 0; i < 20; ++i) add(i + 1, 0.2f * static_cast<float>(i) / 19.0f, 0.0f, 1.0f);
    for (int i = 0; i < 3; ++i) add(100 + i, 0.25f + 0.01f * static_cast<float>(i), 0.0f, 0.8f);  // beside it
    add(200, 2.0f, 2.0f, 0.6f);  // far
    add(201, 2.02f, 2.0f, 0.6f);
    std::vector<float>& clump = s->addStrandChannel("clumpId", -1.0f);
    for (std::size_t i = 0; i < 20; ++i) clump[i] = 0.0f;
    CardSettings settings;
    settings.crossStyle = CardSettings::CrossStyle::Single;
    settings.ruffle = 0.0f;
    const CardMesh joined = buildCards(*s, nullptr, settings);
    ASSERT_EQ(joined.cards.size(), 1u);  // no flyaway budget: the near ones join, the far are dropped
    EXPECT_EQ(joined.cards.front().strands.size(), 23u);
    EXPECT_GT(joined.cards.front().refWidth.front(), 0.27f / 0.8f);  // widened to hold them
    EXPECT_LT(cardFitDistances(*s, joined).back(), 1e-3f);
    settings.flyawayCards = 4;
    const CardMesh withFlyaways = buildCards(*s, nullptr, settings);
    ASSERT_EQ(withFlyaways.cards.size(), 2u);  // the far pair share one thin card; the near ones still join
    const CardMesh::Card& fly = withFlyaways.cards.back();
    EXPECT_TRUE(fly.flyaway);
    EXPECT_EQ(fly.clump, -1);
    EXPECT_EQ(fly.strands, (std::vector<std::size_t>{23, 24}));
    EXPECT_LT(fly.refWidth.front(), 0.1f);
    EXPECT_GT(fly.refWidth.front(), 0.016f);  // a few strand widths
    EXPECT_EQ(withFlyaways.cards.front().strands.size(), 23u);
    settings.maxCards = 1;  // flyaway cards count against the budget
    EXPECT_EQ(buildCards(*s, nullptr, settings).cards.size(), 1u);
}

TEST(Cards, TheBudgetTrimsSubCardsBeforeMergingClumps) {
    const StrandsPtr s = clumpedFur();
    const auto plane = test::makePlane(16);
    CardSettings settings;
    settings.crossStyle = CardSettings::CrossStyle::Single;
    const std::size_t clumps = buildCards(*s, plane.get(), settings).cards.size();
    settings.cardsPerClump = 4;
    const std::size_t many = buildCards(*s, plane.get(), settings).cards.size();
    EXPECT_GE(many, 3 * clumps);
    settings.maxCards = static_cast<int>(clumps + 3);
    const CardMesh trimmed = buildCards(*s, plane.get(), settings);
    EXPECT_EQ(trimmed.cards.size(), clumps + 3);
    std::set<int> seen;
    for (const CardMesh::Card& c : trimmed.cards) seen.insert(c.clump);
    EXPECT_EQ(seen.size(), clumps);  // every clump keeps at least one card
    // Sub-cards share their clump's texture variations and pack like any other.
    settings.variations = 6;
    const CardMesh shared = buildCards(*s, plane.get(), settings);
    EXPECT_EQ(shared.textureCount(), 6u);
    for (std::size_t i = 0; i < shared.vertexCount(); ++i) {
        EXPECT_GE(shared.u[i], 0.0f);
        EXPECT_LE(shared.u[i], 1.0f);
    }
}

// Strands wound around an axis (a braid's weave): `count` of them, `points` each,
// `turns` turns around `axis(t)` at `radius`, t root to tip.
template <class Axis>
void addWoundStrands(StrandSet& s, int firstId, int count, const Axis& axis, float radius, float turns, int points = 13) {
    for (int i = 0; i < count; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(count);
        std::vector<Vec3f> pts;
        for (int k = 0; k < points; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(points - 1);
            const float phase = a + 6.2831853f * turns * t;
            const auto [c, u, v] = axis(t);  // the axis point and two unit vectors across it
            pts.push_back(c + u * (radius * std::cos(phase)) + v * (radius * std::sin(phase)));
        }
        s.addStrand(static_cast<StrandId>(firstId + i), pts, std::vector<float>(pts.size(), 0.004f));
    }
}

TEST(Cards, BraidsGetTheirOwnCrossedCardsAlongTheBraid) {
    // Two braids standing up off the scalp (60 and 40 strands wound around vertical axes),
    // each split across several clumps by the Clump before them, and a plain clump beside.
    auto s = std::make_shared<StrandSet>();
    const auto axisAt = [](float x, float z) {
        return [x, z](float t) { return std::make_tuple(Vec3f{x, t, z}, Vec3f{1, 0, 0}, Vec3f{0, 0, 1}); };
    };
    addWoundStrands(*s, 1, 60, axisAt(0.5f, 0.5f), 0.06f, 1.0f);
    addWoundStrands(*s, 101, 40, axisAt(0.2f, 0.2f), 0.05f, 1.0f);
    for (int i = 0; i < 12; ++i) {
        const float x = 0.8f + 0.1f * static_cast<float>(i) / 11.0f;
        s->addStrand(static_cast<StrandId>(201 + i), test::straightCurve({x, 0.0f, 0.8f}, {0.0f, 1.0f, 0.0f}, 12),
                     std::vector<float>(13, 0.004f));
    }
    std::vector<float>& braidId = s->addStrandChannel("braidId", -1.0f);
    std::vector<float>& clumpId = s->addStrandChannel("clumpId", -1.0f);
    s->addStrandChannel("braidFlat", 0.0f);
    for (std::size_t i = 0; i < 60; ++i) braidId[i] = 0.0f, clumpId[i] = static_cast<float>(i % 3);
    for (std::size_t i = 60; i < 100; ++i) braidId[i] = 1.0f, clumpId[i] = static_cast<float>(3 + i % 2);
    for (std::size_t i = 100; i < 112; ++i) clumpId[i] = 5.0f;
    const auto plane = test::makePlane(16);
    CardSettings settings;
    settings.crossStyle = CardSettings::CrossStyle::Cross;
    settings.crossCount = 2;
    settings.ruffle = 0.0f;
    settings.cardsPerClump = 2;  // no sub-cards for braids

    const CardMesh mesh = buildCards(*s, plane.get(), settings);
    std::map<int, std::vector<const CardMesh::Card*>> byBraid;
    std::vector<const CardMesh::Card*> plain;
    for (const CardMesh::Card& c : mesh.cards) (c.braid >= 0 ? byBraid[c.braid] : plain).push_back(&c);
    ASSERT_EQ(byBraid.size(), 2u);
    for (const auto& [braid, cards] : byBraid) {
        EXPECT_EQ(cards.size(), 2u) << "braid " << braid;  // an X along the braid, whatever the clumps
        std::vector<std::size_t> expected;
        for (std::size_t i = braid == 0 ? 0 : 60; i < (braid == 0 ? 60u : 100u); ++i) expected.push_back(i);
        const Vec3f axis = braid == 0 ? Vec3f{0.5f, 0.5f, 0.5f} : Vec3f{0.2f, 0.5f, 0.2f};
        for (const CardMesh::Card* c : cards) {
            EXPECT_EQ(c->strands, expected);  // exactly the braid's strands
            EXPECT_EQ(c->clump, -1);
            EXPECT_FALSE(c->flyaway);
            const std::size_t mid = c->refCenter.size() / 2;
            EXPECT_LT((c->refCenter[mid] - axis).length(), 0.03f);  // runs down the braid's axis
            EXPECT_GT(c->refWidth[mid], 0.1f);   // as wide as the braid
            EXPECT_LT(c->refWidth[mid], 0.25f);
        }
        // The two cards cross: their normals at the middle are at right angles.
        EXPECT_LT(std::abs(cards[0]->refNormal[16].dot(cards[1]->refNormal[16])), 0.1f);
    }
    // The plain clump gets its usual cards (two sub-cards, each an X), with only its own strands.
    EXPECT_GE(plain.size(), 2u);
    EXPECT_LE(plain.size(), 4u);
    for (const CardMesh::Card* c : plain) {
        EXPECT_EQ(c->clump, 0);
        EXPECT_EQ(c->braid, -1);
        for (const std::size_t i : c->strands) EXPECT_GE(i, 100u);
    }
    // Braid cards count against the budget, and are taken first: 4 for the braids, the
    // plain clump keeps one of its X.
    settings.maxCards = 5;
    const CardMesh tight = buildCards(*s, plane.get(), settings);
    EXPECT_EQ(tight.cards.size(), 5u);
    std::size_t braidCards = 0;
    for (const CardMesh::Card& c : tight.cards) braidCards += c.braid >= 0 ? 1 : 0;
    EXPECT_EQ(braidCards, 4u);

    // Off: the braided strands are carded by their clumps like any others (more cards,
    // none holding a whole braid).
    settings.maxCards = 0;
    settings.braids = false;
    const CardMesh off = buildCards(*s, plane.get(), settings);
    std::size_t holdingBraid0 = 0;
    for (const CardMesh::Card& c : off.cards) {
        EXPECT_EQ(c.braid, -1);
        std::size_t inBraid0 = 0;
        for (const std::size_t i : c.strands) inBraid0 += i < 60 ? 1 : 0;
        if (inBraid0 > 0) ++holdingBraid0;
        EXPECT_LT(inBraid0, 60u);
    }
    EXPECT_GT(holdingBraid0, 2u);
    EXPECT_GT(off.cards.size(), mesh.cards.size());
}

TEST(Cards, ACornrowGetsOneCardLyingOnTheScalp) {
    // A braid wound around a path lying just above a plane scalp (braidFlat 1): one ribbon
    // along it, facing out of the skin, however the cross style is set.
    auto s = std::make_shared<StrandSet>();
    const auto axis = [](float t) {
        return std::make_tuple(Vec3f{0.2f + 0.6f * t, 0.03f, 0.5f}, Vec3f{0, 1, 0}, Vec3f{0, 0, 1});
    };
    addWoundStrands(*s, 1, 50, axis, 0.02f, 3.0f, 25);
    std::vector<float>& braidId = s->addStrandChannel("braidId", -1.0f);
    std::vector<float>& flat = s->addStrandChannel("braidFlat", 0.0f);
    std::vector<float>& clumpId = s->addStrandChannel("clumpId", -1.0f);
    for (std::size_t i = 0; i < 50; ++i) braidId[i] = 0.0f, flat[i] = 1.0f, clumpId[i] = static_cast<float>(i % 4);
    const auto plane = test::makePlane(16);
    for (const CardSettings::CrossStyle style : {CardSettings::CrossStyle::Auto, CardSettings::CrossStyle::Cross}) {
        CardSettings settings;
        settings.crossStyle = style;
        settings.cardsPerClump = 3;
        const CardMesh mesh = buildCards(*s, plane.get(), settings);
        ASSERT_EQ(mesh.cards.size(), 1u);
        const CardMesh::Card& c = mesh.cards.front();
        EXPECT_EQ(c.braid, 0);
        EXPECT_EQ(c.strands.size(), 50u);
        const std::size_t mid = c.refCenter.size() / 2;
        EXPECT_GT(c.refNormal[mid].dot(Vec3f{0, 1, 0}), 0.866f);  // faces out of the scalp, within 30 degrees
        EXPECT_GT(c.refNormal.front().dot(Vec3f{0, 1, 0}), 0.866f);
        EXPECT_LT(std::abs(c.refCenter[mid].y - 0.03f), 0.02f);   // lies along the path
        EXPECT_NEAR(c.refCenter[mid].z, 0.5f, 0.01f);
        EXPECT_GT(c.refWidth[mid], 0.04f);  // the braid's width
        EXPECT_LT(c.refWidth[mid], 0.1f);
    }
}

// clump -> outer (Cards, one card per clump) -> frizz -> inner (Cards, 3 per clump) -> look -> out:
// two Cards nodes layering cards over the same clumps.
GraphDocument layeredCards() {
    return loadGraphJson(R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 800.0}},
        {"name": "clump", "type": "Clump", "params": {"count": 12, "strength": 0.8}},
        {"name": "outer", "type": "Cards", "params": {"segments": 4, "crossStyle": "single"}},
        {"name": "frizz", "type": "Noise", "params": {"amplitude": 0.02, "frequency": 4.0, "octaves": 1}},
        {"name": "inner", "type": "Cards", "params": {"segments": 4, "crossStyle": "single", "cardsPerClump": 3}},
        {"name": "look", "type": "Noise", "params": {"amplitude": 0.05, "frequency": 8.0, "octaves": 1}},
        {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 10}}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "clump", "input": "strands"},
        {"from": "clump", "to": "outer", "input": "strands"},
        {"from": "outer", "to": "frizz", "input": "strands"}, {"from": "frizz", "to": "inner", "input": "strands"},
        {"from": "inner", "to": "look", "input": "strands"}, {"from": "look", "to": "out", "input": "strands"}]})j");
}

// The groom's cards as the host lays them out: every layer in one atlas.
CardMesh laidOutCards(GraphDocument& doc, const MeshPtr& scalp, StrandsPtr* strands = nullptr) {
    ExternalInputs externals;
    setGroomExternals(externals, scalp, nullptr, nullptr);
    EvalContext ctx;
    ctx.externals = &externals;
    ctx.buildCards = true;
    const StrandsPtr out = doc.graph.evaluateStrands(doc.output, ctx);
    CardMesh cards = combineCardLayers(out->cardLayers());
    layoutCards(cards, cards.padding);
    if (strands) *strands = out;
    return cards;
}

TEST(Cards, EachCardsNodeKeepsItsOwnBakedCells) {
    // Two Cards nodes card the same clumps: the inner node's cards hold subsets of the strands
    // the outer node's cards hold, so across the whole atlas an inner card shares as many
    // strands with its clump's outer card as with its own. The atlas records each card's node
    // and a rebuild matches within it: every card keeps its own baked cell.
    const auto plane = test::makePlane(16);
    GraphDocument doc = layeredCards();
    StrandsPtr strands;
    const CardMesh baked = laidOutCards(doc, plane, &strands);
    ASSERT_EQ(baked.nodes, (std::vector<std::string>{"outer", "inner"}));
    std::size_t inner = 0;
    for (const CardMesh::Card& card : baked.cards) inner += cardNodeOf(baked, card) == "inner" ? 1 : 0;
    ASSERT_GE(inner, 16u);
    ASSERT_GE(baked.cards.size() - inner, 8u);
    const CardAtlas atlas = CardAtlas::fromJson(captureCardAtlas(baked, strands.get()).toJson());  // round trip
    ASSERT_EQ(atlas.entries.size(), baked.cards.size());
    for (std::size_t c = 0; c < baked.cards.size(); ++c) EXPECT_EQ(atlas.entries[c].node, cardNodeOf(baked, baked.cards[c]));

    CardMesh rebuilt = laidOutCards(doc, plane);
    ASSERT_EQ(rebuilt.cards.size(), baked.cards.size());
    EXPECT_EQ(applyCardAtlas(rebuilt, atlas, nullptr), rebuilt.cards.size());
    for (std::size_t c = 0; c < baked.cards.size(); ++c) {
        SCOPED_TRACE(cardNodeOf(baked, baked.cards[c]) + " card " + std::to_string(c));
        for (int k = 0; k < 4; ++k) EXPECT_FLOAT_EQ(rebuilt.cards[c].cell[k], baked.cards[c].cell[k]);
        EXPECT_EQ(rebuilt.cards[c].mirrored, baked.cards[c].mirrored);
    }

    // A node the bake never saw (renamed since) still borrows baked cells from any node.
    CardAtlas renamed = atlas;
    for (CardAtlas::Entry& entry : renamed.entries) {
        if (entry.node == "inner") entry.node = "old";
    }
    CardMesh again = laidOutCards(doc, plane);
    EXPECT_EQ(applyCardAtlas(again, renamed, nullptr), again.cards.size());
}

TEST(Cards, CrossedCardsKeepTheirOwnBakedCells) {
    // The cards of a cross hold the same strands: each takes a baked entry no card took yet,
    // so every card of the cross keeps its own cell through a rebuild.
    const StrandsPtr s = clumpedFur();
    CardSettings settings;
    settings.segments = 4;
    settings.crossStyle = CardSettings::CrossStyle::Cross;
    settings.crossCount = 3;
    const CardMesh baked = buildCards(*s, nullptr, settings);
    ASSERT_GE(baked.cards.size(), 24u);
    const CardAtlas atlas = captureCardAtlas(baked, s.get());
    CardMesh rebuilt = buildCards(*s, nullptr, settings);
    EXPECT_EQ(applyCardAtlas(rebuilt, atlas, s.get()), rebuilt.cards.size());
    for (std::size_t c = 0; c < baked.cards.size(); ++c) {
        for (int k = 0; k < 4; ++k) EXPECT_FLOAT_EQ(rebuilt.cards[c].cell[k], baked.cards[c].cell[k]) << "card " << c;
        EXPECT_EQ(rebuilt.cards[c].mirrored, baked.cards[c].mirrored) << "card " << c;
    }
}

TEST(Cards, EachCardsNodeHasItsOwnBakeStamp) {
    // What a bake is made from, per Cards node: the strands its cards are made of and their
    // shapes. A look after the cards changes neither node's; a change between the nodes only
    // the inner one's; a change under both, both.
    const auto plane = test::makePlane(16);
    GraphDocument doc = layeredCards();
    auto stamps = [&]() {
        StrandsPtr strands;
        const CardMesh cards = laidOutCards(doc, plane, &strands);
        std::map<std::string, CardNodeSummary> out;
        for (const CardNodeSummary& n : summarizeCardNodes(cards, strands.get())) out[n.node] = n;
        return out;
    };
    auto set = [&](const std::string& node, const std::string& param, double value) {
        doc.graph.node(doc.node(node)).params().set(param, value);
    };
    const auto baked = stamps();
    ASSERT_EQ(baked.size(), 2u);
    EXPECT_GE(baked.at("outer").cards, 8u);
    EXPECT_GT(baked.at("inner").cards, baked.at("outer").cards);
    EXPECT_GT(baked.at("inner").triangles, 0u);
    EXPECT_NE(baked.at("outer").bakeHash, baked.at("inner").bakeHash);

    set("look", "amplitude", 0.2);  // a look with no Cards node after it
    auto now = stamps();
    EXPECT_EQ(now.at("outer").bakeHash, baked.at("outer").bakeHash);
    EXPECT_EQ(now.at("inner").bakeHash, baked.at("inner").bakeHash);

    set("frizz", "amplitude", 0.06);  // the inner node's strands only
    now = stamps();
    EXPECT_EQ(now.at("outer").bakeHash, baked.at("outer").bakeHash);
    EXPECT_NE(now.at("inner").bakeHash, baked.at("inner").bakeHash);

    set("frizz", "amplitude", 0.02);
    now = stamps();
    EXPECT_EQ(now.at("inner").bakeHash, baked.at("inner").bakeHash);  // back as baked

    set("clump", "strength", 0.5);  // under both
    now = stamps();
    EXPECT_NE(now.at("outer").bakeHash, baked.at("outer").bakeHash);
    EXPECT_NE(now.at("inner").bakeHash, baked.at("inner").bakeHash);

    set("clump", "strength", 0.8);
    set("inner", "widthScale", 1.4);  // the inner node's card params
    now = stamps();
    EXPECT_EQ(now.at("outer").bakeHash, baked.at("outer").bakeHash);
    EXPECT_NE(now.at("inner").bakeHash, baked.at("inner").bakeHash);
}

TEST(Cards, ACardsNodeInsideACompoundNamesItsCardsByItsPath) {
    // scatter -> wrap (Compound: in -> cards -> dye) -> pick (Switch) -> out, pick's b: plain.
    const std::string graph = R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 300.0}},
        {"name": "wrap", "type": "Compound", "params": {"definition": {
            "graph": {"version": 1, "output": "dye",
              "nodes": [{"name": "in", "type": "Input", "params": {"name": "strands", "type": "Strands"}},
                        {"name": "cards", "type": "Cards", "params": {"clumpChannel": "none", "maxCards": 8}},
                        {"name": "dye", "type": "HairColor"}],
              "connections": [{"from": "in", "to": "cards", "input": "strands"},
                              {"from": "cards", "to": "dye", "input": "strands"}]},
            "inputs": [{"name": "strands", "type": "Strands"}]}}},
        {"name": "plain", "type": "HairColor"},
        {"name": "pick", "type": "Switch", "params": {"index": 0}},
        {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "wrap", "input": "strands"}, {"from": "scatter", "to": "plain", "input": "strands"},
        {"from": "wrap", "to": "pick", "input": "a"}, {"from": "plain", "to": "pick", "input": "b"},
        {"from": "pick", "to": "out", "input": "strands"}]})j";
    GraphDocument doc = loadGraphJson(graph);
    auto walk = [&]() {
        std::vector<std::string> paths;
        doc.graph.forEachActiveUpstream(doc.output, [&](const std::string& path, const Operator&) {
            paths.push_back(path);
            return true;
        });
        return paths;
    };
    // The output's first-input chain, into the compound, the Switch's picked input only.
    EXPECT_EQ(walk(), (std::vector<std::string>{"out", "pick", "wrap", "wrap/dye", "wrap/cards", "wrap/in", "scatter"}));
    const CardMesh cards = laidOutCards(doc, test::makePlane(16));
    ASSERT_GE(cards.cards.size(), 4u);
    EXPECT_EQ(cards.nodes, std::vector<std::string>{"wrap/cards"});
    // The other input picked: the compound is out of the way.
    doc.graph.node(doc.node("pick")).params().set("index", std::int64_t{1});
    EXPECT_EQ(walk(), (std::vector<std::string>{"out", "pick", "plain", "scatter"}));
    // Bypassed nodes hand their input on and never show.
    doc.graph.setBypass(doc.node("plain"), true);
    EXPECT_EQ(walk(), (std::vector<std::string>{"out", "pick", "scatter"}));
    // A visitor can stop the walk.
    std::size_t visited = 0;
    EXPECT_FALSE(doc.graph.forEachActiveUpstream(doc.output, [&](const std::string&, const Operator&) {
        return ++visited < 2;
    }));
    EXPECT_EQ(visited, 2u);
}

TEST(Resample, ItsSegmentsAreTheStrandsResolution) {
    // Scatter only places roots: the Resample node's segments grow every strand,
    // whichever node is evaluated, so Interpolate before it works at them too.
    GroomData d;
    d.scalp = test::makePlane(4);
    d.graphJson = R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 50.0}},
        {"name": "resample", "type": "Resample", "params": {"segments": 13, "mode": "even"}},
        {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "resample", "input": "strands"}, {"from": "resample", "to": "out", "input": "strands"}]})j";
    Groom groom(d);
    const StrandsPtr s = groom.evaluate(1.0f, 0.0);
    ASSERT_GT(s->strandCount(), 10u);
    EXPECT_EQ(s->uniformPointCount(), 14u);
    GraphDocument doc = loadGraphJson(d.graphJson);
    ExternalInputs ext;
    ext.set("scalp", test::makePlane(4), 1);
    EvalContext ctx;
    ctx.externals = &ext;
    EXPECT_EQ(doc.graph.evaluateStrands(doc.node("scatter"), ctx)->uniformPointCount(), 14u);
    doc.graph.node(doc.node("resample")).params().set("segments", std::int64_t{5});
    EXPECT_EQ(doc.graph.evaluateStrands(doc.node("scatter"), ctx)->uniformPointCount(), 6u);  // not a stale cache

    // Strands of any resolution through it (here 3 and 20 points) come out evenly resampled, shape kept.
    auto mixed = std::make_shared<StrandSet>();
    mixed->addStrand(1, {{0, 0, 0}, {0, 0.5f, 0}, {0, 1, 0}}, {0.02f, 0.015f, 0.01f});
    std::vector<Vec3f> bent;
    for (int k = 0; k < 20; ++k) bent.push_back({0.1f * static_cast<float>(k), static_cast<float>(k) / 19.0f, 1.0f});
    mixed->addStrand(2, bent, std::vector<float>(20, 0.01f));
    auto resample = OperatorRegistry::instance().create(op::kResample);
    resample->params().set("segments", std::int64_t{8});
    resample->params().set("mode", std::string("even"));
    const StrandsPtr even = valueAs<StrandsPtr>(resample->evaluate(ctx, {StrandsPtr(mixed)}));
    EXPECT_EQ(even->uniformPointCount(), 9u);
    EXPECT_TRUE(even->validate());
    EXPECT_NEAR(even->strandPoints(0)[8].y, 1.0f, 1e-5f);
    EXPECT_NEAR(even->strandPoints(1)[8].x, 1.9f, 1e-4f);
    EXPECT_NEAR(even->widths()[4], 0.015f, 1e-5f);  // widths follow
    auto keep = OperatorRegistry::instance().create(op::kResample);  // 0: each keeps its own
    keep->params().set("segments", std::int64_t{0});
    keep->params().set("mode", std::string("even"));
    EXPECT_EQ(valueAs<StrandsPtr>(keep->evaluate(ctx, {StrandsPtr(mixed)}))->strandPointCount(1), 20u);
}

TEST(Resample, ByCurvature) {
    // A straight strand, and one straight for its first half and curling in its second (both 40 points).
    auto s = std::make_shared<StrandSet>();
    std::vector<Vec3f> straight, curl;
    for (int k = 0; k < 40; ++k) {
        const float t = static_cast<float>(k) / 39.0f;
        straight.push_back({0.0f, t, 0.0f});
        if (t <= 0.5f) {
            curl.push_back({1.0f, t, 0.0f});
        } else {
            const float a = (t - 0.5f) * 6.0f * 3.14159265f;  // one and a half turns
            curl.push_back({1.0f + 0.05f * std::sin(a), 0.5f + 0.1f * (t - 0.5f), 0.05f * (1.0f - std::cos(a))});
        }
    }
    s->addStrand(1, straight, std::vector<float>(40, 0.01f));
    s->addStrand(2, curl, std::vector<float>(40, 0.01f));
    auto op = OperatorRegistry::instance().create(op::kResample);
    op->params().set("segments", std::int64_t{12});
    op->params().set("mode", std::string("curvature"));
    op->params().set("minSegments", std::int64_t{2});
    EvalContext ctx;
    const StrandsPtr out = valueAs<StrandsPtr>(op->evaluate(ctx, {StrandsPtr(s)}));
    ASSERT_TRUE(out->validate());
    EXPECT_EQ(out->strandPointCount(0), 3u);   // straight: the fewest
    EXPECT_EQ(out->strandPointCount(1), 13u);  // the curliest: all of them
    // The curl's points gather where it bends: most of them in its curling half.
    std::size_t inCurl = 0;
    for (std::size_t k = 0; k < out->strandPointCount(1); ++k) inCurl += out->strandPoints(1)[k].y > 0.5f ? 1 : 0;
    EXPECT_GE(inCurl, 9u);
    EXPECT_NEAR(out->strandPoints(1)[0].y, 0.0f, 1e-6f);  // the ends stay
    EXPECT_NEAR((out->strandPoints(1)[12] - curl.back()).length(), 0.0f, 1e-5f);
    // Evenly, the same count would put about half of them there.
    op->params().set("mode", std::string("even"));
    const StrandsPtr even = valueAs<StrandsPtr>(op->evaluate(ctx, {StrandsPtr(s)}));
    std::size_t evenCurl = 0;
    for (std::size_t k = 0; k < even->strandPointCount(1); ++k) evenCurl += even->strandPoints(1)[k].y > 0.5f ? 1 : 0;
    EXPECT_LT(evenCurl, inCurl);
    op->params().set("mode", std::string("spiral"));
    EXPECT_THROW(op->evaluate(ctx, {StrandsPtr(s)}), GraphError);
}

TEST(Subdivide, SmoothStrandsThroughTheirPointsAtTheRendersLevels) {
    // A quarter circle of 5 points (radius 1, about the origin), its widths and a point channel going
    // down to the tip, and a strand of 2 points.
    auto s = std::make_shared<StrandSet>();
    std::vector<Vec3f> arc;
    std::vector<float> widths;
    for (int k = 0; k < 5; ++k) {
        const float a = 0.5f * 3.14159265f * static_cast<float>(k) / 4.0f;
        arc.push_back({std::cos(a), std::sin(a), 0.0f});
        widths.push_back(0.02f - 0.004f * static_cast<float>(k));
    }
    s->addStrand(7, arc, widths);
    s->addStrand(9, {{0, 0, 0}, {0, 1, 0}}, {0.01f, 0.01f});
    std::vector<float>& opacity = s->addPointChannel("opacity", 1.0f);
    for (std::size_t k = 0; k < 5; ++k) opacity[k] = 1.0f - 0.25f * static_cast<float>(k);
    s->addStrandChannel("melanin", 0.0f)[0] = 0.4f;
    auto op = OperatorRegistry::instance().create(op::kSubdivide);
    EvalContext render;
    const StrandsPtr out = valueAs<StrandsPtr>(op->evaluate(render, {StrandsPtr(s)}));
    ASSERT_TRUE(out->validate());
    ASSERT_EQ(out->strandPointCount(0), 17u);  // 2 levels: 4 pieces a segment
    EXPECT_EQ(out->strandPointCount(1), 2u);   // too short to bend: as it came
    EXPECT_EQ(out->ids(), s->ids());
    EXPECT_FLOAT_EQ(out->strandChannel("melanin")->at(0), 0.4f);
    float worstNew = 0.0f, chordMid = 0.0f;
    for (std::size_t j = 0; j < 17; ++j) {
        const Vec3f p = out->strandPoints(0)[j];
        if (j % 4 == 0) {
            EXPECT_LT((p - arc[j / 4]).length(), 1e-6f) << j;  // through the strand's own points
            EXPECT_NEAR(out->widths()[j], widths[j / 4], 1e-6f);
        } else {
            worstNew = std::max(worstNew, std::abs(p.length() - 1.0f));
        }
    }
    chordMid = 1.0f - ((arc[0] + arc[1]) * 0.5f).length();
    EXPECT_LT(worstNew, 0.1f * chordMid) << "the new points lie on the circle, not on its chords";
    EXPECT_NEAR(out->widths()[2], 0.5f * (widths[0] + widths[1]), 1e-6f);  // widths follow, linearly
    EXPECT_NEAR(out->pointChannel("opacity")->at(6), 0.625f, 1e-6f);  // halfway from 0.75 to 0.5
    // The viewport's levels: 1 by default; none hands the strands on as they came.
    EvalContext viewport;
    viewport.viewport = true;
    EXPECT_EQ(valueAs<StrandsPtr>(op->evaluate(viewport, {StrandsPtr(s)}))->strandPointCount(0), 9u);
    // A host's live drag in the viewport: handed on as they came (the GPU runs the nodes before),
    // keyed apart from the refined result; a render refines all the same.
    EvalContext dragging = viewport;
    dragging.liveDrag = true;
    EXPECT_EQ(valueAs<StrandsPtr>(op->evaluate(dragging, {StrandsPtr(s)})), StrandsPtr(s));
    EXPECT_NE(op->contextSalt(dragging), op->contextSalt(viewport));
    render.liveDrag = true;
    EXPECT_EQ(valueAs<StrandsPtr>(op->evaluate(render, {StrandsPtr(s)}))->strandPointCount(0), 17u);
    op->params().set("viewportLevels", std::int64_t{0});
    EXPECT_EQ(valueAs<StrandsPtr>(op->evaluate(viewport, {StrandsPtr(s)})), StrandsPtr(s));
}

TEST(Subdivide, LeavesTheGrowthResolutionToTheResample) {
    // Resample asks the Scatter for its segments; a Subdivide after it adds points to the output only.
    GroomData d;
    d.scalp = test::makePlane(4);
    d.graphJson = R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 50.0}},
        {"name": "resample", "type": "Resample", "params": {"segments": 4}},
        {"name": "smooth", "type": "Subdivide", "params": {"levels": 2}},
        {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"}, {"from": "scatter", "to": "resample", "input": "strands"},
        {"from": "resample", "to": "smooth", "input": "strands"}, {"from": "smooth", "to": "out", "input": "strands"}]})j";
    Groom groom(d);
    EXPECT_EQ(groom.evaluate(1.0f, 0.0)->uniformPointCount(), 17u);
    GraphDocument doc = loadGraphJson(d.graphJson);
    ExternalInputs ext;
    ext.set("scalp", test::makePlane(4), 1);
    EvalContext ctx;
    ctx.externals = &ext;
    EXPECT_EQ(doc.graph.evaluateStrands(doc.node("scatter"), ctx)->uniformPointCount(), 5u);
}

TEST(Resample, OnlyEnabledOnesOnStrandsSetTheResolution) {
    // A bypassed Resample, or one fed guides, doesn't resample the strands: its segments don't grow them.
    GraphDocument doc = loadGraphJson(R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 50.0}},
        {"name": "resample", "type": "Resample", "params": {"segments": 6, "mode": "even"}},
        {"name": "off", "type": "Resample", "params": {"segments": 40}, "bypass": true},
        {"name": "guideRes", "type": "Resample", "params": {"segments": 30}},
        {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "off", "input": "strands"}, {"from": "off", "to": "resample", "input": "strands"},
        {"from": "guides", "to": "guideRes", "input": "strands"}, {"from": "resample", "to": "out", "input": "strands"}]})j");
    ExternalInputs ext;
    ext.set("scalp", test::makePlane(4), 1);
    auto guides = std::make_shared<GuideSet>();
    guides->add(1, test::straightCurve({0.5f, 0, 0.5f}, {0, 1, 0}, 4));
    ext.set("guides", GuidesPtr(guides), guides->contentHash());
    EvalContext ctx;
    ctx.externals = &ext;
    EXPECT_EQ(doc.graph.evaluateStrands(doc.node("scatter"), ctx)->uniformPointCount(), 7u);
    EXPECT_EQ(doc.graph.evaluateStrands(doc.output, ctx)->uniformPointCount(), 7u);
    doc.graph.setBypass(doc.node("off"), false);  // enabled, it counts
    EXPECT_EQ(doc.graph.evaluateStrands(doc.node("scatter"), ctx)->uniformPointCount(), 41u);
}

TEST(Resample, SegmentsByLength) {
    // A long strand (1.0) and a short one (0.25), 10 segments each, with widths and channels.
    auto s = std::make_shared<StrandSet>();
    for (int i = 0; i < 2; ++i) {
        const float top = i ? 0.25f : 1.0f;
        std::vector<Vec3f> pts;
        std::vector<float> widths;
        for (int k = 0; k <= 10; ++k) {
            pts.push_back({static_cast<float>(i), top * static_cast<float>(k) / 10.0f, 0.0f});
            widths.push_back(0.02f - 0.0015f * static_cast<float>(k));
        }
        s->addStrand(static_cast<StrandId>(i + 7), pts, widths);
    }
    s->addStrandChannel("melanin", 0.0f)[1] = 0.6f;
    s->addPointChannel("rootTip", 0.0f);
    Graph g;
    ExternalInputs ext;
    ext.set("strands", StrandsPtr(s), 1);
    auto op = OperatorRegistry::instance().create(op::kResample);
    op->params().set("segments", std::int64_t{0});  // the longest keep theirs
    op->params().set("mode", std::string("length"));
    op->params().set("minSegments", std::int64_t{2});
    EvalContext ctx;
    const StrandsPtr out = valueAs<StrandsPtr>(op->evaluate(ctx, {StrandsPtr(s)}));
    ASSERT_EQ(out->strandCount(), 2u);
    EXPECT_EQ(out->strandPointCount(0), 11u);  // the longest keeps its 10 segments
    EXPECT_LE(out->strandPointCount(1), 5u);   // a quarter as long
    EXPECT_GE(out->strandPointCount(1), 3u);
    EXPECT_TRUE(out->validate());
    EXPECT_EQ(out->ids()[1], 8u);
    EXPECT_FLOAT_EQ((*out->strandChannel("melanin"))[1], 0.6f);
    ASSERT_TRUE(out->pointChannel("rootTip"));
    EXPECT_NEAR(out->widths()[out->strandBegin(1)], 0.02f, 1e-6f);  // widths follow, root to tip
    EXPECT_NEAR(out->widths()[out->strandBegin(1) + out->strandPointCount(1) - 1], 0.005f, 1e-6f);
    EXPECT_NEAR(out->strandPoints(1)[out->strandPointCount(1) - 1].y, 0.25f, 1e-5f);  // same shape
    // Segments set: those for the longest.
    op->params().set("segments", std::int64_t{6});
    const StrandsPtr six = valueAs<StrandsPtr>(op->evaluate(ctx, {StrandsPtr(s)}));
    EXPECT_EQ(six->strandPointCount(0), 7u);
    EXPECT_EQ(six->strandPointCount(1), 3u);  // a quarter: 1.5, at least 2
    // Off: each keeps its own.
    auto off = OperatorRegistry::instance().create(op::kResample);
    off->params().set("segments", std::int64_t{0});
    off->params().set("mode", std::string("even"));
    const StrandsPtr kept = valueAs<StrandsPtr>(off->evaluate(ctx, {StrandsPtr(s)}));
    EXPECT_EQ(kept->strandPointCount(0), 11u);
    EXPECT_EQ(kept->strandPointCount(1), 11u);
}

}  // namespace
}  // namespace groomlab
