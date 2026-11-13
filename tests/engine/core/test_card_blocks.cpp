// SPDX-License-Identifier: Apache-2.0
// The Card Atlas (card_blocks.h): blocks of hair, each a groom of its own on a root band, baked
// into one texture set.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>

#include "groomlab/card_blocks.h"
#include "groomlab/groom_file.h"
#include "groomlab/groom_session.h"
#include "groomlab/guide_hierarchy.h"

using namespace groomlab;

namespace {

CardBlockAtlas threeBlocks() {
    CardBlockAtlas atlas;
    atlas.settings.size = 512;
    atlas.settings.padding = 4;
    for (const auto& [name, width, length] : {std::tuple{"wide", 3.0f, 12.0f}, {"thin", 1.0f, 15.0f}, {"short", 2.0f, 6.0f}}) {
        CardBlock b;
        b.name = name;
        b.width = width;
        b.length = length;
        b.origin = nextCardBlockOrigin(atlas, width);
        atlas.blocks.push_back(b);
    }
    return atlas;
}

float density(const CardBlock& b) { return (b.cell[2] - b.cell[0]) / b.width; }

}  // namespace

TEST(CardBlocks, JsonRoundTripsWithTheAtlasSettings) {
    CardBlock b;
    b.width = 2.5f;
    b.length = 11.0f;
    b.thickness = 0.4f;
    b.origin = {3.0f, 0.0f, -1.0f};
    b.rootRotation = {35.0f, 0.0f, -10.0f};
    b.cell = {0.1f, 0.2f, 0.3f, 0.9f};
    b.placed = true;
    CardAtlasSettings s;
    s.size = 4096;
    s.padding = 12;
    s.preset = "Unity HDRP";
    s.name = "bangs";
    CardBlock back;
    back.name = "keepMe";
    CardAtlasSettings sBack;
    ASSERT_TRUE(cardBlockFromJson(cardBlockToJson(b, s), &back, &sBack));
    EXPECT_EQ(back.name, "keepMe");  // the groom's name is the host's
    EXPECT_FLOAT_EQ(back.width, 2.5f);
    EXPECT_FLOAT_EQ(back.length, 11.0f);
    EXPECT_FLOAT_EQ(back.thickness, 0.4f);
    EXPECT_FLOAT_EQ(back.origin.x, 3.0f);
    EXPECT_FLOAT_EQ(back.rootRotation.x, 35.0f);
    EXPECT_FLOAT_EQ(back.rootRotation.z, -10.0f);
    EXPECT_EQ(back.cell, b.cell);
    EXPECT_TRUE(back.placed);
    EXPECT_TRUE(sBack == s);
    EXPECT_FALSE(cardBlockFromJson("", &back, &sBack));  // not a block
    EXPECT_THROW(cardBlockFromJson("[1]", &back, &sBack), std::runtime_error);
    // Out of range: clamped, a bad cell reset.
    ASSERT_TRUE(cardBlockFromJson(R"({"width": -2, "cell": [0.5, 0.5, 0.2, 0.9], "atlas": {"size": 99999}})", &back, &sBack));
    EXPECT_GT(back.width, 0.0f);
    EXPECT_EQ(back.cell, (std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f}));
    EXPECT_EQ(sBack.size, 8192);
}

TEST(CardBlocks, PackedAtOneDensityApartAndInside) {
    CardBlockAtlas atlas = threeBlocks();
    packCardBlocks(atlas);
    const float pad = 4.0f / 512.0f;
    for (std::size_t i = 0; i < atlas.blocks.size(); ++i) {
        const CardBlock& b = atlas.blocks[i];
        EXPECT_FALSE(b.placed);
        EXPECT_GE(b.cell[0], pad - 1e-5f);
        EXPECT_LE(b.cell[3], 1.0f - pad + 1e-5f);
        // As wide as its card and as tall as its length, at one density for all.
        EXPECT_NEAR((b.cell[3] - b.cell[1]) / b.length, density(b), 1e-4f);
        EXPECT_NEAR(density(b), density(atlas.blocks[0]), 1e-4f);
        for (std::size_t j = i + 1; j < atlas.blocks.size(); ++j) EXPECT_FALSE(cardCellsOverlap(b.cell, atlas.blocks[j].cell));
    }
    EXPECT_GT(atlas.blocks[1].cell[3] - atlas.blocks[1].cell[1], 0.5f);  // as large as they fit
}

TEST(CardBlocks, ABlockPlacedByHandStaysAndANewOneFindsRoomBesideIt) {
    CardBlockAtlas atlas = threeBlocks();
    packCardBlocks(atlas);
    // The first block dragged to the bottom left and made smaller by hand.
    atlas.blocks[0].cell = {0.02f, 0.02f, 0.02f + 3.0f * 0.02f, 0.02f + 12.0f * 0.02f};
    atlas.blocks[0].placed = true;
    const auto placed = atlas.blocks[0].cell;
    CardBlock added;
    added.name = "added";
    added.width = 2.0f;
    added.length = 8.0f;
    added.cell = placed;  // over the placed one: moved
    atlas.blocks.push_back(added);
    updateCardBlockLayout(atlas);
    EXPECT_EQ(atlas.blocks[0].cell, placed);
    for (std::size_t i = 0; i < atlas.blocks.size(); ++i) {
        for (std::size_t j = i + 1; j < atlas.blocks.size(); ++j) {
            EXPECT_FALSE(cardCellsOverlap(atlas.blocks[i].cell, atlas.blocks[j].cell)) << i << " " << j;
        }
        const auto& c = atlas.blocks[i].cell;
        EXPECT_GE(c[0], 0.0f);
        EXPECT_LE(c[2], 1.0f);
        // Its card's shape.
        EXPECT_NEAR((c[2] - c[0]) * atlas.blocks[i].length, (c[3] - c[1]) * atlas.blocks[i].width,
                    1e-3f * atlas.blocks[i].length);
    }
    // With nothing placed by hand, a change packs afresh.
    atlas.blocks[0].placed = false;
    updateCardBlockLayout(atlas);
    EXPECT_NEAR(density(atlas.blocks[0]), density(atlas.blocks[3]), 1e-4f);
}

TEST(CardBlocks, BlocksSitSideBySideInTheScene) {
    const CardBlockAtlas atlas = threeBlocks();
    for (std::size_t i = 1; i < atlas.blocks.size(); ++i) {
        const CardBlock& a = atlas.blocks[i - 1];
        const CardBlock& b = atlas.blocks[i];
        EXPECT_GT(b.origin.x - 0.5f * b.width, a.origin.x + 0.5f * a.width);  // a gap between them
    }
}

TEST(CardBlocks, TheRootBandFacesTheViewerAndTiltsBack) {
    CardBlock b;
    b.origin = {5.0f, 1.0f, 0.0f};
    const auto scalp = cardBlockScalp(b);
    ASSERT_GT(scalp->triangleCount(), 0u);
    EXPECT_NEAR(scalp->totalArea(), b.width * b.thickness, 1e-4f);
    for (std::size_t t = 0; t < scalp->triangleCount(); ++t) {
        EXPECT_GT(scalp->normal(StrandRoot{static_cast<std::int32_t>(t), 0.3f, 0.3f}).z, 0.99f);  // facing +z: drawn on from the front
    }
    // Its top edge at the origin, down the card.
    const CardBlockMesh flat = cardBlockMesh(b);
    float top = -1e9f, bottom = 1e9f;
    for (const Vec3f& p : flat.points) top = std::max(top, p.y), bottom = std::min(bottom, p.y);
    EXPECT_NEAR(top, 1.0f, 1e-5f);
    EXPECT_NEAR(top - bottom, b.thickness, 1e-5f);
    // Tilted back 90 degrees: lying flat, facing down the card; its roots spread in depth.
    b.rootRotation = {90.0f, 0.0f, 0.0f};
    EXPECT_LT(cardBlockScalp(b)->normal(StrandRoot{0, 0.3f, 0.3f}).y, -0.99f);
    // Tilted 40: its lower edge behind the top (the depth a scalp seen at an angle shows).
    b.rootRotation = {40.0f, 0.0f, 0.0f};
    const CardBlockMesh m = cardBlockMesh(b);
    float lo = 1e9f, hi = -1e9f;
    for (const Vec3f& p : m.points) lo = std::min(lo, p.z), hi = std::max(hi, p.z);
    EXPECT_NEAR(hi - lo, b.thickness * std::sin(40.0f * 3.14159265f / 180.0f), 1e-4f);
    EXPECT_NEAR(hi, 0.0f, 1e-5f);
    EXPECT_EQ(m.uvs.size(), 2 * m.points.size());
}

TEST(CardBlocks, NewGuidesHangDownTheCardFromTheBand) {
    CardBlock b;
    b.width = 3.0f;
    b.length = 10.0f;
    const GuideHierarchy h = GuideHierarchy::fromJson(cardBlockGuidesJson(b));
    ASSERT_EQ(h.guides().size(), 5u);  // one per 0.6 of the width
    for (const HierarchyGuide& g : h.guides()) {
        EXPECT_NEAR(g.points.front().y, -0.5f * b.thickness, 1e-5f);  // from the band's middle
        EXPECT_NEAR(g.points.back().y, -10.0f, 1e-3f);                 // to the card's length
        EXPECT_LE(std::abs(g.points.front().x), 1.5f);
    }
    EXPECT_EQ(GuideHierarchy::fromJson(cardBlockGuidesJson(b, 2)).guides().size(), 2u);
}

// The whole way: each block a groom of the default graph on its band, grown, and every block
// baked into its own cell of one texture set; the cards' shapes exported.
TEST(CardBlocks, EveryBlockBakesIntoItsOwnCell) {
    CardBlockAtlas atlas = threeBlocks();
    atlas.blocks[2].rootRotation = {40.0f, 0.0f, 0.0f};
    packCardBlocks(atlas);
    std::vector<StrandsPtr> strands;
    for (const CardBlock& b : atlas.blocks) {
        GroomData d;
        d.graphJson = cardBlockGraphJson();
        d.hierarchyJson = cardBlockGuidesJson(b);
        d.scalp = cardBlockScalp(b);
        GroomSession session(d);
        const StrandsPtr s = session.evaluate(1.0f, 0.0);
        ASSERT_TRUE(s);
        EXPECT_GT(s->strandCount(), 200u) << b.name;
        // Hanging down the card from the band.
        float lowest = 0.0f;
        for (const Vec3f& p : s->positions()) lowest = std::min(lowest, p.y - b.origin.y);
        EXPECT_LT(lowest, -0.6f * b.length) << b.name;
        strands.push_back(s);
    }
    CardMapExport settings;
    settings.resolution = 256;
    const CardMaps maps = bakeCardBlockMaps(atlas, strands, settings, 1.0f);
    ASSERT_EQ(maps.size, 256);
    // Coverage only inside the cells, some in each.
    const auto at = [&](int x, int y) { return maps.coverage[static_cast<std::size_t>(y) * 256 + static_cast<std::size_t>(x)]; };
    std::vector<double> inCell(atlas.blocks.size(), 0.0);
    double outside = 0.0;
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / 256.0f, v = 1.0f - (static_cast<float>(y) + 0.5f) / 256.0f;
            bool in = false;
            for (std::size_t i = 0; i < atlas.blocks.size(); ++i) {
                const auto& c = atlas.blocks[i].cell;
                if (u >= c[0] && u <= c[2] && v >= c[1] && v <= c[3]) {
                    inCell[i] += at(x, y);
                    in = true;
                }
            }
            if (!in) outside += at(x, y);
        }
    }
    for (std::size_t i = 0; i < inCell.size(); ++i) EXPECT_GT(inCell[i], 10.0) << atlas.blocks[i].name;
    EXPECT_EQ(outside, 0.0);
    // GROOMLAB_CARD_BLOCKS_PREVIEW=<folder>: the maps written there to look at.
    if (const char* folder = std::getenv("GROOMLAB_CARD_BLOCKS_PREVIEW")) {
        CardMapExport big;
        big.resolution = 1024;
        const CardMaps preview = bakeCardBlockMaps(atlas, strands, big, 1.0f);
        writeCardMaps(preview, *findCardMapPreset("Separate maps (all)"), big, folder, "atlas");
    }
    EXPECT_THROW(bakeCardBlockMaps(atlas, {}, settings), std::invalid_argument);
    EXPECT_THROW(bakeCardBlockMaps(atlas, {nullptr, nullptr, nullptr}, settings), std::runtime_error);

    CardShapeOptions shape;
    shape.segments = 6;
    shape.bend = 30.0f;
    const std::vector<FbxMesh> meshes = cardBlockShapes(atlas, shape);
    ASSERT_EQ(meshes.size(), 3u);
    EXPECT_EQ(meshes[1].name, "thin_card");  // apart from the block's groom ("thin")
    EXPECT_EQ(meshes[1].material, atlas.settings.name + "_mat");
    EXPECT_EQ(meshes[0].faceCounts.size(), 6u);
    for (std::size_t c = 0; c < meshes[0].uvs.size(); c += 2) {  // UVs in its cell
        EXPECT_GE(meshes[0].uvs[c], atlas.blocks[0].cell[0] - 1e-5f);
        EXPECT_LE(meshes[0].uvs[c + 1], atlas.blocks[0].cell[3] + 1e-5f);
    }
    const auto path = std::filesystem::temp_directory_path() / "groomlab_card_blocks.fbx";
    writeCardBlockShapes(path.u8string(), atlas, shape);
    EXPECT_GT(std::filesystem::file_size(path), 1000u);
    std::filesystem::remove(path);
}

// A groom file of blocks is the atlas: each groom keeps its block ("cardBlock"), the settings with it.
TEST(CardBlocks, AGroomFileOfBlocksIsTheAtlas) {
    CardBlockAtlas atlas = threeBlocks();
    atlas.settings.name = "bangs";
    packCardBlocks(atlas);
    std::vector<GroomFile> files;
    for (const CardBlock& b : atlas.blocks) {
        GroomFile f;
        f.name = b.name;
        f.data.scalp = cardBlockScalp(b);
        f.data.graphJson = cardBlockGraphJson();
        f.cardBlock = cardBlockToJson(b, atlas.settings);
        files.push_back(f);
    }
    GroomFile other;  // a groom that is no block
    other.name = "head";
    other.data.scalp = cardBlockScalp(CardBlock{});
    files.insert(files.begin() + 1, other);
    const std::vector<GroomFile> back = groomFilesFromJson(groomFilesToJson(files));
    ASSERT_EQ(back.size(), 4u);
    EXPECT_TRUE(back[1].cardBlock.empty());
    std::vector<std::pair<std::string, std::string>> named;
    for (const GroomFile& f : back) named.emplace_back(f.name, f.cardBlock);
    const CardBlockAtlas read = cardBlockAtlasOf(named);
    ASSERT_EQ(read.blocks.size(), 3u);
    EXPECT_EQ(read.settings.name, "bangs");
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(read.blocks[i].name, atlas.blocks[i].name);
        EXPECT_EQ(read.blocks[i].cell, atlas.blocks[i].cell);
        EXPECT_FLOAT_EQ(read.blocks[i].length, atlas.blocks[i].length);
    }
    EXPECT_THROW(cardBlockAtlasOf({{"bad", "[1]"}}), std::runtime_error);
}

// Roots at the top of their rectangles (as hand-made card atlases lay hair out; the default), else
// at the bottom: the maps (the root-to-tip map, the along direction in the tangent map) and the
// card shapes' UVs alike.
TEST(CardBlocks, RootsAtTheTopOfTheirRectanglesOrTheBottom) {
    CardBlockAtlas atlas;
    atlas.settings.size = 128;
    CardBlock b;
    b.name = "one";
    atlas.blocks.push_back(b);
    packCardBlocks(atlas);
    GroomData d;
    d.graphJson = cardBlockGraphJson();
    d.hierarchyJson = cardBlockGuidesJson(b);
    d.scalp = cardBlockScalp(b);
    GroomSession session(d);
    const std::vector<StrandsPtr> strands{session.evaluate(1.0f, 0.0)};
    const auto& c = atlas.blocks[0].cell;
    for (const bool top : {true, false}) {
        atlas.settings.rootAtTop = top;
        CardMapExport settings;
        settings.resolution = 128;
        const CardMaps maps = bakeCardBlockMaps(atlas, strands, settings, 0.0f);
        // Over the hair in the cell's top and bottom quarters: the root-to-tip value and the
        // tangent's along-the-texture component (+v up).
        double rootTop = 0, rootBottom = 0, alongV = 0, nTop = 0, nBottom = 0, n = 0;
        const int x0 = static_cast<int>(c[0] * 128), x1 = static_cast<int>(c[2] * 128);
        const int yTop = static_cast<int>((1.0f - c[3]) * 128), yBottom = static_cast<int>((1.0f - c[1]) * 128);
        const int quarter = (yBottom - yTop) / 4;
        for (int y = yTop; y < yBottom; ++y) {
            for (int x = x0; x < x1; ++x) {
                const std::size_t p = static_cast<std::size_t>(y) * 128 + static_cast<std::size_t>(x);
                if (maps.coverage[p] < 0.2f) continue;
                alongV += maps.tangent[3 * p + 1], n += 1;
                if (y < yTop + quarter) rootTop += maps.root[p], nTop += 1;
                if (y >= yBottom - quarter) rootBottom += maps.root[p], nBottom += 1;
            }
        }
        ASSERT_GT(nTop, 0);
        ASSERT_GT(nBottom, 0);
        if (top) {
            EXPECT_LT(rootTop / nTop, rootBottom / nBottom);  // the roots up top, the tips below
            EXPECT_LT(alongV / n, -0.5);                      // root to tip runs down the texture
        } else {
            EXPECT_GT(rootTop / nTop, rootBottom / nBottom);
            EXPECT_GT(alongV / n, 0.5);
        }
        // The card shape's root row at the same end of its cell.
        const FbxMesh shape = cardBlockShapes(atlas, CardShapeOptions{}).front();
        const float rootV = shape.uvs[1];  // the first corner: on the root row
        EXPECT_NEAR(rootV, top ? c[3] : c[1], 1e-5f) << top;
    }
}

// The band tilted, widened or made taller: each guide moved, not turned. Its root keeps its place on
// the band (as shares of its width and height), its shape hangs as it did.
TEST(CardBlocks, GuidesMoveWithTheBandWithoutTurning) {
    CardBlock from;
    from.width = 2.0f;
    from.thickness = 0.5f;
    const std::string before = cardBlockGuidesJson(from, 3);
    CardBlock to = from;
    to.width = 3.0f;
    to.thickness = 0.8f;
    to.rootRotation = {50.0f, 0.0f, 10.0f};
    const GuideHierarchy a = GuideHierarchy::fromJson(before);
    const GuideHierarchy b = GuideHierarchy::fromJson(moveCardBlockGuides(before, from, to));
    ASSERT_EQ(a.guides().size(), b.guides().size());
    const auto band = cardBlockScalp(to);
    for (std::size_t i = 0; i < a.guides().size(); ++i) {
        const auto& p = a.guides()[i].points;
        const auto& q = b.guides()[i].points;
        ASSERT_EQ(p.size(), q.size());
        // The same shape, moved (every point by the root's shift).
        for (std::size_t k = 0; k < p.size(); ++k) {
            EXPECT_NEAR(((q[k] - q[0]) - (p[k] - p[0])).length(), 0.0f, 1e-4f);
        }
        // Its root on the new band.
        EXPECT_LT((band->closestPoint(q[0]).position - q[0]).length(), 1e-3f);
    }
    // Across the band in the same order, spread to its new width.
    EXPECT_LT(b.guides()[0].points[0].x, b.guides()[2].points[0].x);
    EXPECT_GT(b.guides()[2].points[0].x - b.guides()[0].points[0].x, a.guides()[2].points[0].x - a.guides()[0].points[0].x);
    EXPECT_EQ(moveCardBlockGuides("", from, to), "");
}

namespace {

// A block's hair with its guides combed `lean` across toward their tips (a straight sweep).
StrandsPtr leaningHair(const CardBlock& b, float lean) {
    GuideHierarchy h = GuideHierarchy::fromJson(cardBlockGuidesJson(b));
    for (const HierarchyGuide& g : std::vector<HierarchyGuide>(h.guides())) {
        std::vector<Vec3f> points = g.points;
        for (std::size_t k = 0; k < points.size(); ++k) points[k].x += lean * static_cast<float>(k) / static_cast<float>(points.size() - 1);
        h.setPoints(g.id, std::move(points));
    }
    GroomData d;
    d.graphJson = cardBlockGraphJson();
    d.hierarchyJson = h.toJson();
    d.scalp = cardBlockScalp(b);
    GroomSession session(d);
    return session.evaluate(1.0f, 0.0);
}

}  // namespace

TEST(CardBlocks, TheCardFitsTheHairCombedPastItsWidth) {
    CardBlockAtlas atlas = threeBlocks();
    packCardBlocks(atlas);
    std::vector<StrandsPtr> strands;
    for (const CardBlock& b : atlas.blocks) strands.push_back(leaningHair(b, &b == &atlas.blocks[0] ? 2.5f : 0.0f));
    float reach = -1e9f;
    for (const Vec3f& p : strands[0]->positions()) reach = std::max(reach, p.x - atlas.blocks[0].origin.x);
    ASSERT_GT(reach, 0.5f * atlas.blocks[0].width + 1.5f);  // well past the card's edge

    // Fitted: every block's frame holds its hair; the combed one wider, to the right.
    ASSERT_TRUE(fitCardBlocks(atlas, strands));
    const CardFrame wide = cardBlockFrame(atlas.blocks[0]);
    EXPECT_GE(wide.right, reach);
    EXPECT_LT(wide.left, -0.4f * atlas.blocks[0].width);
    EXPECT_GT(wide.width(), atlas.blocks[0].width + 1.5f);
    for (std::size_t i = 0; i < strands.size(); ++i) {
        const CardFrame f = cardBlockFrame(atlas.blocks[i]);
        for (const Vec3f& p : strands[i]->positions()) {
            const Vec3f d = p - atlas.blocks[i].origin;
            EXPECT_GE(d.x, f.left);
            EXPECT_LE(d.x, f.right);
            EXPECT_LE(-d.y, f.length);
        }
    }
    // The layout follows: still apart, the combed block's cell as wide for its length as its frame.
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = i + 1; j < 3; ++j) EXPECT_FALSE(cardCellsOverlap(atlas.blocks[i].cell, atlas.blocks[j].cell));
    }
    const auto& c = atlas.blocks[0].cell;
    EXPECT_NEAR((c[2] - c[0]) / (c[3] - c[1]), wide.width() / wide.length, 0.02f);
    // Nothing more to fit the second time.
    EXPECT_FALSE(fitCardBlocks(atlas, strands));

    // Baked: the combed hair's far side lands in the cell, nothing outside the cells.
    CardMapExport settings;
    settings.resolution = 256;
    const CardMaps maps = bakeCardBlockMaps(atlas, strands, settings, 0.0f);
    double rightQuarter = 0.0, outside = 0.0;
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / 256.0f, v = 1.0f - (static_cast<float>(y) + 0.5f) / 256.0f;
            const float value = maps.coverage[static_cast<std::size_t>(y) * 256 + static_cast<std::size_t>(x)];
            bool in = false;
            for (const CardBlock& b : atlas.blocks) in = in || (u >= b.cell[0] && u <= b.cell[2] && v >= b.cell[1] && v <= b.cell[3]);
            if (!in) outside += value;
            if (u >= c[0] + 0.75f * (c[2] - c[0]) && u <= c[2] && v >= c[1] && v <= c[3]) rightQuarter += value;
        }
    }
    EXPECT_GT(rightQuarter, 5.0);
    EXPECT_EQ(outside, 0.0);

    // The card shapes take the frame's size.
    const std::vector<FbxMesh> shapes = cardBlockShapes(atlas, {});
    float lo = 1e9f, hi = -1e9f;
    for (const Vec3f& p : shapes[0].points) lo = std::min(lo, p.x), hi = std::max(hi, p.x);
    EXPECT_NEAR(hi - lo, wide.width(), 1e-3f);

    // Kept through its text.
    CardBlock back;
    ASSERT_TRUE(cardBlockFromJson(cardBlockToJson(atlas.blocks[0], atlas.settings), &back, nullptr));
    EXPECT_FLOAT_EQ(back.frameLeft, atlas.blocks[0].frameLeft);
    EXPECT_FLOAT_EQ(back.frameRight, atlas.blocks[0].frameRight);
    EXPECT_FLOAT_EQ(back.frameLength, atlas.blocks[0].frameLength);
}

TEST(CardBlocks, AFittedBlockPlacedByHandKeepsItsRootCornerAndDensity) {
    CardBlockAtlas atlas = threeBlocks();
    packCardBlocks(atlas);
    std::vector<StrandsPtr> strands;
    for (const CardBlock& b : atlas.blocks) strands.push_back(leaningHair(b, 0.0f));
    fitCardBlocks(atlas, strands);
    CardBlock& b = atlas.blocks[1];
    b.cell = {0.6f, 0.2f, 0.65f, 0.8f};
    b.placed = true;
    updateCardBlockLayout(atlas);
    const CardFrame before = cardBlockFrame(b);
    const float du = (b.cell[2] - b.cell[0]) / before.width(), dv = (b.cell[3] - b.cell[1]) / before.length;
    strands[1] = leaningHair(b, 1.5f);
    ASSERT_TRUE(fitCardBlocks(atlas, strands));
    const CardFrame after = cardBlockFrame(b);
    EXPECT_GT(after.width(), before.width() + 1.0f);
    EXPECT_TRUE(b.placed);
    EXPECT_FLOAT_EQ(b.cell[0], 0.6f);  // the root corner (top left: roots at the top) kept
    EXPECT_FLOAT_EQ(b.cell[3], 0.8f);
    EXPECT_NEAR((b.cell[2] - b.cell[0]) / after.width(), du, 1e-4f);
    EXPECT_NEAR((b.cell[3] - b.cell[1]) / after.length, dv, 1e-4f);
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = i + 1; j < 3; ++j) EXPECT_FALSE(cardCellsOverlap(atlas.blocks[i].cell, atlas.blocks[j].cell));
    }
}
