// SPDX-License-Identifier: Apache-2.0
// Card map exports: the maps a bake gives (normal, ID, root, seed, specular, translucency...),
// presets packing them into files, the image formats, and the old bake kept byte for byte.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "groomlab/card_files.h"
#include "groomlab/card_maps.h"
#include "groomlab/cards.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "test_helpers.h"

#include "stb_image.h"  // the core holds its code

namespace groomlab {
namespace {

namespace fs = std::filesystem;

const char* kFurGraph = R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 800.0}},
        {"name": "clump", "type": "Clump", "params": {"count": 12, "strength": 0.8}},
        {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 10}}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "resolution", "input": "strands"}, {"from": "resolution", "to": "clump", "input": "strands"},
        {"from": "clump", "to": "out", "input": "strands"}]})j";

StrandsPtr clumpedFur() {
    GroomData d;
    d.scalp = test::makePlane(16);
    d.graphJson = kFurGraph;
    return Groom(d).evaluate(1.0f, 0.0);
}

// The fur through a Cards node, evaluated for cards (the strands carry the card layers); `thinnest`,
// the Cards node's Thinnest Strand (texels).
StrandsPtr furWithCards(const MeshPtr& scalp, double thinnest = 1.0) {
    GraphDocument doc = loadGraphJson(std::string(R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 800.0}},
        {"name": "clump", "type": "Clump", "params": {"count": 12, "strength": 0.8}},
        {"name": "cards", "type": "Cards", "params": {"segments": 4, "thinnestStrand": )j") + std::to_string(thinnest) + R"j(}},
        {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 10}}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "resolution", "input": "strands"}, {"from": "resolution", "to": "clump", "input": "strands"},
        {"from": "clump", "to": "cards", "input": "strands"}, {"from": "cards", "to": "out", "input": "strands"}]})j");
    ExternalInputs externals;
    setGroomExternals(externals, scalp, nullptr, nullptr);
    EvalContext ctx;
    ctx.externals = &externals;
    ctx.buildCards = true;
    return doc.graph.evaluateStrands(doc.output, ctx);
}

std::uint64_t fnv(std::uint64_t h, const std::vector<std::uint8_t>& bytes) {
    for (const std::uint8_t b : bytes) h = (h ^ b) * 1099511628211ull;
    return h;
}

std::uint64_t texturesHash(const CardTextures& t) {
    std::uint64_t h = 1469598103934665603ull ^ static_cast<std::uint64_t>(t.size);
    for (const auto* v : {&t.depth, &t.coverage, &t.tangent, &t.attribute, &t.color, &t.occlusion, &t.iridescence}) h = fnv(h, *v);
    return h;
}

std::vector<std::uint8_t> fileBytes(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

fs::path freshFolder(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / ("groomlab_card_maps_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void setEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) unsetenv(name);
    else setenv(name, value.c_str(), 1);
#endif
}

struct FurMaps {
    StrandsPtr strands;
    MeshPtr plane;
    CardMesh mesh;
    CardMaps maps;
};

FurMaps furMaps(int size, int dilate = 8) {
    FurMaps f;
    f.strands = clumpedFur();
    f.plane = test::makePlane(16);
    f.mesh = buildCards(*f.strands, f.plane.get(), {});
    CardBakeSettings bake;
    bake.size = size;
    bake.scalp = f.plane.get();
    bake.dilate = dilate;
    f.maps = bakeCardMaps(*f.strands, f.mesh, bake);
    return f;
}

// The 8-bit maps of the bake as they were before the full-precision maps came (recorded from
// that code): the old maps, and so the "Unreal Hair Cards" preset, are the same bytes.
TEST(CardMaps, TheOldBakeIsTheSameBytes) {
    const StrandsPtr s = clumpedFur();
    const auto plane = test::makePlane(16);
    const CardMesh mesh = buildCards(*s, plane.get(), {});
    CardBakeSettings bake;
    bake.size = 256;
    bake.scalp = plane.get();
    EXPECT_EQ(texturesHash(bakeCardTextures(*s, mesh, bake)), 0x125d253404b32ef7ull);
    bake.size = 128;
    bake.dilate = 0;
    bake.occlusion = 0.0f;
    EXPECT_EQ(texturesHash(bakeCardTextures(*s, mesh, bake)), 0x9e886fefa24a88d7ull);
}

TEST(CardMaps, TheUnrealPresetWritesTheBakesOwnFiles) {
    const MeshPtr plane = test::makePlane(16);
    const StrandsPtr strands = furWithCards(plane);
    ASSERT_TRUE(strands && !strands->cardLayers().empty());
    const fs::path dir = freshFolder("unreal");
    // The bake as Bake Maps writes it...
    const CardBake old = bakeCardsInMemory(strands, plane, 128);
    writeCardBake(old, CardBakePaths::in((dir / "old").generic_u8string(), "fur"), false);
    // ... and the export with the default preset at the same size.
    CardMapExport settings;
    settings.resolution = 128;
    const CardBake bake = bakeCardMapsInMemory(strands, plane, settings);
    ASSERT_TRUE(bake.maps);
    EXPECT_TRUE(bake.textures.coverage.empty());
    const auto preset = findCardMapPreset("Unreal Hair Cards");
    ASSERT_TRUE(preset);
    const std::vector<std::string> paths = writeCardMaps(*bake.maps, *preset, settings, (dir / "new").generic_u8string(), "fur");
    ASSERT_EQ(paths.size(), 7u);
    for (std::size_t i = 0; i < paths.size(); ++i) {
        const std::string map = cardMapNames()[i];
        EXPECT_EQ(fs::path(paths[i]).filename().u8string(), "fur_" + map + ".tga");
        const std::vector<std::uint8_t> a = fileBytes((dir / "old" / ("fur_" + map + ".tga")).u8string());
        ASSERT_FALSE(a.empty()) << map;
        EXPECT_TRUE(a == fileBytes(paths[i])) << map;
    }
}

// The Cards node's Thinnest Strand reaches both bakes: at 1 (the default) every strand bakes at least
// a texel wide and solid; at 0 to scale, the fur's strands (a fraction of a texel at 128) fainter, so
// the cards' coverage sums lower; the export's maps alike.
TEST(CardMaps, ThinnestStrandBakesStrandsToScaleAtZero) {
    const MeshPtr plane = test::makePlane(16);
    const StrandsPtr solid = furWithCards(plane, 1.0), scaled = furWithCards(plane, 0.0);
    ASSERT_EQ(combineCardLayers(solid->cardLayers()).thinnestStrand, 1.0f);
    ASSERT_EQ(combineCardLayers(scaled->cardLayers()).thinnestStrand, 0.0f);
    auto sum = [](const std::vector<std::uint8_t>& v) {
        double s = 0;
        for (const std::uint8_t b : v) s += b;
        return s;
    };
    const double a = sum(bakeCardsInMemory(solid, plane, 128).textures.coverage);
    const double b = sum(bakeCardsInMemory(scaled, plane, 128).textures.coverage);
    EXPECT_GT(a, 0.0);
    EXPECT_GT(b, 0.0);
    EXPECT_LT(b, 0.9 * a) << "to scale, the thin strands bake fainter";
    CardMapExport settings;
    settings.resolution = 128;
    const CardBake solidMaps = bakeCardMapsInMemory(solid, plane, settings), scaledMaps = bakeCardMapsInMemory(scaled, plane, settings);
    ASSERT_TRUE(solidMaps.maps && scaledMaps.maps);
    auto coverageOf = [](const CardMaps& m) {
        double s = 0;
        for (const float v : m.coverage) s += v;
        return s;
    };
    EXPECT_LT(coverageOf(*scaledMaps.maps), 0.9 * coverageOf(*solidMaps.maps));
}

TEST(CardMaps, NormalsAreRoundFibersOutOfTheCard) {
    const FurMaps f = furMaps(256);
    const CardMaps& m = f.maps;
    const auto n = static_cast<std::size_t>(m.size);
    // Across every run of covered texels in a row (a strand's cross-section: strands run up the
    // texture), the middle faces out of the card and the edges lean to their side.
    double middleZ = 0.0, edgeZ = 0.0, leftX = 0.0, rightX = 0.0;
    std::size_t runs = 0;
    for (std::size_t y = 0; y < n; ++y) {
        std::size_t x = 0;
        while (x < n) {
            if (m.coverage[y * n + x] < 0.5f) {
                ++x;
                continue;
            }
            const std::size_t start = x;
            while (x < n && m.coverage[y * n + x] >= 0.5f) ++x;
            const std::size_t end = x - 1;
            if (end - start < 2 || end - start > 8) continue;  // a strand two to eight texels wide
            const std::size_t mid = (start + end) / 2;
            middleZ += m.normal[(y * n + mid) * 3 + 2];
            edgeZ += 0.5 * (m.normal[(y * n + start) * 3 + 2] + m.normal[(y * n + end) * 3 + 2]);
            leftX += m.normal[(y * n + start) * 3];
            rightX += m.normal[(y * n + end) * 3];
            ++runs;
        }
    }
    ASSERT_GT(runs, 50u);
    const double r = static_cast<double>(runs);
    EXPECT_GT(middleZ / r, 0.7);            // roughly up at strand centers
    EXPECT_GT(middleZ / r, edgeZ / r + 0.05);
    EXPECT_LT(leftX / r, -0.1);             // the left edge leans left, the right edge right
    EXPECT_GT(rightX / r, 0.1);
    // Unit length everywhere; out of the card where there is no hair.
    for (std::size_t p = 0; p < n * n; p += 97) {
        const float len = std::sqrt(m.normal[p * 3] * m.normal[p * 3] + m.normal[p * 3 + 1] * m.normal[p * 3 + 1] +
                                    m.normal[p * 3 + 2] * m.normal[p * 3 + 2]);
        EXPECT_NEAR(len, 1.0f, 1e-4f);
    }
    // DirectX's green is OpenGL's flipped.
    CardMapFile file;
    file.channels = {"normal.x", "normal.y", "normal.z"};
    int channels = 0;
    const std::vector<float> gl = packCardMapFile(m, file, "opengl", "png", &channels);
    const std::vector<float> dx = packCardMapFile(m, file, "directx", "png", &channels);
    ASSERT_EQ(channels, 3);
    for (std::size_t p = 0; p < n * n; p += 101) {
        EXPECT_FLOAT_EQ(gl[p * 3], dx[p * 3]);
        EXPECT_NEAR(gl[p * 3 + 1], 1.0f - dx[p * 3 + 1], 1e-6f);
    }
}

// The normal is in the card's tangent space (x across, y along, z out), not the groom's: the
// whole groom and its cards turned (x, y, z to z, x, y), the map stays as it was.
TEST(CardMaps, NormalsStayInTangentSpaceWhenTheGroomTurns) {
    FurMaps f = furMaps(128, 0);
    CardBakeSettings bake;
    bake.size = 128;
    bake.dilate = 0;
    bake.occlusion = 0.0f;
    const CardMaps before = bakeCardMaps(*f.strands, f.mesh, bake);
    auto turn = [](const Vec3f& p) { return Vec3f{p.z, p.x, p.y}; };
    StrandSet turned = *f.strands;
    for (Vec3f& p : turned.mutablePositions()) p = turn(p);
    for (CardMesh::Card& card : f.mesh.cards) {
        ASSERT_FALSE(card.source);
        for (std::vector<Vec3f>* values : {&card.center, &card.side, &card.normal, &card.shading, &card.refCenter,
                                           &card.refSide, &card.refNormal, &card.refShading, &card.base}) {
            for (Vec3f& value : *values) value = turn(value);
        }
    }
    const CardMaps after = bakeCardMaps(turned, f.mesh, bake);
    ASSERT_EQ(after.normal.size(), before.normal.size());
    for (std::size_t i = 0; i < before.normal.size(); ++i) EXPECT_NEAR(after.normal[i], before.normal[i], 5e-5f) << i;
}

TEST(CardMaps, RootRunsFromRootToTipAndIdsDifferPerClump) {
    const FurMaps f = furMaps(256);
    const CardMaps& m = f.maps;
    const auto n = static_cast<std::size_t>(m.size);
    double low = 0.0, high = 0.0;
    std::size_t lowCount = 0, highCount = 0;
    std::map<int, std::multiset<float>> idsByCard;
    for (std::size_t c = 0; c < f.mesh.cards.size(); ++c) {
        const CardMesh::Card& card = f.mesh.cards[c];
        if (card.variationOf >= 0) continue;
        const auto x0 = static_cast<std::size_t>(card.cell[0] * m.size), x1 = static_cast<std::size_t>(card.cell[2] * m.size);
        const auto yTop = static_cast<std::size_t>((1.0f - card.cell[3]) * m.size), yBottom = static_cast<std::size_t>((1.0f - card.cell[1]) * m.size);
        const std::size_t quarter = (yBottom - yTop) / 4;
        for (std::size_t y = yTop; y < yBottom && y < n; ++y) {
            for (std::size_t x = x0; x < x1 && x < n; ++x) {
                const std::size_t p = y * n + x;
                if (m.coverage[p] < 0.5f) continue;
                if (y >= yBottom - quarter) low += m.root[p], ++lowCount;  // near the roots (the cell's bottom)
                if (y < yTop + quarter) high += m.root[p], ++highCount;     // near the tips
                idsByCard[static_cast<int>(c)].insert(m.id[p]);
            }
        }
    }
    ASSERT_GT(lowCount, 0u);
    ASSERT_GT(highCount, 0u);
    EXPECT_LT(low / static_cast<double>(lowCount), 0.3);
    EXPECT_GT(high / static_cast<double>(highCount), 0.7);
    // Each card holds one clump: its texels share one ID (the median), and clumps differ.
    std::set<int> medians;
    for (const auto& [card, ids] : idsByCard) medians.insert(static_cast<int>(std::lround(*std::next(ids.begin(), static_cast<long>(ids.size() / 2)) * 1000.0f)));
    EXPECT_GE(medians.size(), 8u);  // the fur has 12 clumps
    ASSERT_GE(idsByCard.size(), 4u);
    // The root map is the attribute map's coordU, and the seed its third channel.
    const CardTextures t = cardTexturesFromMaps(m);
    for (std::size_t p = 0; p < n * n; p += 13) {
        EXPECT_EQ(t.attribute[p * 4 + 3], static_cast<std::uint8_t>(std::lround(std::clamp(m.root[p], 0.0f, 1.0f) * 255.0f)));
        EXPECT_EQ(t.attribute[p * 4 + 2], static_cast<std::uint8_t>(std::lround(std::clamp(m.seed[p], 0.0f, 1.0f) * 255.0f)));
    }
}

TEST(CardMaps, TranslucencyIsHigherWhereTheHairIsDenser) {
    const FurMaps f = furMaps(256);
    const CardMaps& m = f.maps;
    CardMapFile file;
    file.channels = {"translucency"};
    int channels = 0;
    const std::vector<float> tl = packCardMapFile(m, file, "opengl", "tga", &channels);
    ASSERT_EQ(channels, 1);
    double dense = 0.0, sparse = 0.0, empty = 0.0;
    std::size_t denseCount = 0, sparseCount = 0, emptyCount = 0;
    const auto n = static_cast<std::size_t>(m.size);
    for (std::size_t p = 0; p < n * n; ++p) {
        if (m.layers[p] >= 3.0f) dense += tl[p], ++denseCount;
        else if (m.layers[p] > 0.2f && m.layers[p] <= 1.0f) sparse += tl[p], ++sparseCount;
        else if (m.layers[p] == 0.0f) empty += tl[p], ++emptyCount;
    }
    ASSERT_GT(denseCount, 0u);
    ASSERT_GT(sparseCount, 0u);
    EXPECT_GT(dense / static_cast<double>(denseCount), sparse / static_cast<double>(sparseCount) + 0.05);
    EXPECT_LT(empty / static_cast<double>(emptyCount), sparse / static_cast<double>(sparseCount));
    for (const float v : tl) {
        ASSERT_GE(v, 0.0f);
        ASSERT_LE(v, 1.0f);
    }
}

TEST(CardMaps, PackingPutsEachChannelWhereThePresetSays) {
    const FurMaps f = furMaps(64);
    const CardMaps& m = f.maps;
    const std::size_t pixels = static_cast<std::size_t>(m.size) * static_cast<std::size_t>(m.size);
    CardMapFile file;
    file.channels = {"depth", "1-ao", "0", "1"};
    int channels = 0;
    const std::vector<float> packed = packCardMapFile(m, file, "opengl", "tga", &channels);
    ASSERT_EQ(channels, 4);
    for (std::size_t p = 0; p < pixels; ++p) {
        ASSERT_FLOAT_EQ(packed[p * 4], m.depth[p]);
        ASSERT_FLOAT_EQ(packed[p * 4 + 1], 1.0f - m.ao[p]);
        ASSERT_EQ(packed[p * 4 + 2], 0.0f);
        ASSERT_EQ(packed[p * 4 + 3], 1.0f);
    }
    // Two channels: a third, 0. Color: sRGB in an sRGB file, linear in a linear one (and EXR's auto).
    file.channels = {"tangent.y", "color.r"};
    file.colorSpace = "srgb";
    const std::vector<float> srgb = packCardMapFile(m, file, "opengl", "png", &channels);
    ASSERT_EQ(channels, 3);
    file.colorSpace = "auto";
    const std::vector<float> linear = packCardMapFile(m, file, "opengl", "exr", &channels);
    bool brighter = false;
    for (std::size_t p = 0; p < pixels; ++p) {
        ASSERT_FLOAT_EQ(srgb[p * 3], 0.5f + 0.5f * m.tangent[p * 3 + 1]);
        ASSERT_FLOAT_EQ(linear[p * 3 + 1], m.color[p * 3]);
        ASSERT_EQ(srgb[p * 3 + 2], 0.0f);
        brighter = brighter || srgb[p * 3 + 1] > linear[p * 3 + 1] + 0.01f;
    }
    EXPECT_TRUE(brighter);
    // An optional channel the groom doesn't have is left out (and a file of only such, too).
    EXPECT_FALSE(m.iridescent);
    file.channels = {"color.r", "color.g", "color.b", "iridescence?"};
    packCardMapFile(m, file, "opengl", "tga", &channels);
    EXPECT_EQ(channels, 3);
    file.channels = {"iridescence?"};
    EXPECT_TRUE(packCardMapFile(m, file, "opengl", "tga", &channels).empty());
    EXPECT_EQ(channels, 0);
}

TEST(CardMaps, ChannelsParse) {
    EXPECT_EQ(parseCardMapChannel("depth").source, 0);
    const CardMapChannel t = parseCardMapChannel("tangent.y");
    EXPECT_EQ(cardMapSources()[static_cast<std::size_t>(t.source)].name, "tangent");
    EXPECT_EQ(t.component, 1);
    EXPECT_EQ(parseCardMapChannel("color.b").component, 2);
    EXPECT_EQ(parseCardMapChannel("rootuv.v").component, 1);
    EXPECT_TRUE(parseCardMapChannel("1-ao").invert);
    EXPECT_TRUE(parseCardMapChannel("iridescence?").optional);
    const CardMapChannel half = parseCardMapChannel("0.5");
    EXPECT_EQ(half.source, -1);
    EXPECT_FLOAT_EQ(half.constant, 0.5f);
    EXPECT_EQ(cardMapSources()[static_cast<std::size_t>(parseCardMapChannel("alpha").source)].name, "coverage");
    EXPECT_THROW(parseCardMapChannel("tangent"), std::invalid_argument);    // which of its three?
    EXPECT_THROW(parseCardMapChannel("depth.y"), std::invalid_argument);
    EXPECT_THROW(parseCardMapChannel("shine"), std::invalid_argument);
    EXPECT_THROW(parseCardMapChannel("2"), std::invalid_argument);
    EXPECT_THROW(parseCardMapChannel(""), std::invalid_argument);
    const std::vector<std::string> names = cardMapChannelNames();
    EXPECT_NE(std::find(names.begin(), names.end(), "normal.z"), names.end());
    EXPECT_EQ(names.back(), "1");
    for (const std::string& name : names) EXPECT_NO_THROW(parseCardMapChannel(name)) << name;
}

TEST(CardMaps, PaddingSpreadsValuesPastTheStrands) {
    const FurMaps bare = furMaps(128, 0);
    const FurMaps padded = furMaps(128, 8);
    auto spread = [&](const CardMaps& m) {
        std::size_t count = 0;
        for (std::size_t p = 0; p < m.coverage.size(); ++p) {
            if (m.coverage[p] > 0.0f) continue;
            if (m.seed[p] > 0.0f) ++count;  // a strand's value spread there, not the empty card's 0
        }
        return count;
    };
    EXPECT_EQ(spread(bare.maps), 0u);
    EXPECT_GT(spread(padded.maps), 500u);
    // The auto padding: 8 texels at 2K and below, scaled above.
    EXPECT_EQ(cardMapAutoPadding(1024), 8);
    EXPECT_EQ(cardMapAutoPadding(2048), 8);
    EXPECT_EQ(cardMapAutoPadding(4096), 16);
    EXPECT_EQ(cardMapAutoPadding(8192), 32);
    EXPECT_EQ(cardMapSupersample(2048, 4), 4);
    EXPECT_EQ(cardMapSupersample(4096, 4), 2);
    EXPECT_EQ(cardMapSupersample(8192, 2), 1);
}

TEST(CardMaps, SupersamplingBakesBiggerAndBringsItDown) {
    const FurMaps f = furMaps(256);
    CardBakeSettings base;
    base.scalp = f.plane.get();
    CardMapExport settings;
    settings.supersample = 2;
    const CardBakeSettings bake = cardMapBakeSettings(base, settings, 256);
    EXPECT_EQ(bake.size, 512);
    EXPECT_EQ(bake.dilate, 16);
    EXPECT_FLOAT_EQ(bake.minStrandTexels, 2.0f);
    const CardMaps big = bakeCardMaps(*f.strands, f.mesh, bake);
    const CardMaps small = downsampleCardMaps(big, 2);
    ASSERT_EQ(small.size, 256);
    ASSERT_EQ(small.coverage.size(), 256u * 256u);
    double a = 0.0, b = 0.0;
    for (std::size_t p = 0; p < small.coverage.size(); ++p) a += small.coverage[p], b += f.maps.coverage[p];
    EXPECT_NEAR(a / b, 1.0, 0.35);  // about as much hair either way
    // Anti-aliased: more in-between coverage values than the plain bake.
    auto between = [](const CardMaps& m) {
        std::size_t n = 0;
        for (const float c : m.coverage) n += (c > 0.05f && c < 0.95f) ? 1 : 0;
        return n;
    };
    EXPECT_GT(between(small), between(f.maps) / 2);
}

// ---- Image files read back

std::uint16_t le16(const std::vector<std::uint8_t>& b, std::size_t at) { return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8)); }
std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) | (static_cast<std::uint32_t>(b[at + 2]) << 16) |
           (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

float fromHalf(std::uint16_t h) {
    const std::uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1Fu, mant = h & 0x3FFu;
    if (exp == 0) {
        const float v = std::ldexp(static_cast<float>(mant), -24);
        return sign ? -v : v;
    }
    std::uint32_t bits = sign | ((exp == 31 ? 255u : exp - 15u + 127u) << 23) | (mant << 13);
    float out;
    std::memcpy(&out, &bits, 4);
    return out;
}

// An EXR this writer made: its channels by name and their values (row-major), decoded by hand.
std::map<std::string, std::vector<float>> readExr(const std::string& path, int* width, int* height) {
    const std::vector<std::uint8_t> b = fileBytes(path);
    EXPECT_EQ(le32(b, 0), 20000630u);
    std::size_t at = 8;
    auto str = [&]() {
        std::string s;
        while (b[at]) s += static_cast<char>(b[at++]);
        ++at;
        return s;
    };
    std::vector<std::pair<std::string, int>> channels;
    int compression = -1;
    while (b[at]) {
        const std::string name = str(), type = str();
        const auto size = le32(b, at);
        at += 4;
        const std::size_t value = at;
        if (name == "channels") {
            std::size_t c = value;
            while (b[c]) {
                std::string ch;
                while (b[c]) ch += static_cast<char>(b[c++]);
                ++c;
                channels.emplace_back(ch, static_cast<int>(le32(b, c)));
                c += 16;
            }
        } else if (name == "compression") {
            compression = b[value];
        } else if (name == "dataWindow") {
            *width = static_cast<int>(le32(b, value + 8)) + 1;
            *height = static_cast<int>(le32(b, value + 12)) + 1;
        }
        at = value + size;
    }
    ++at;
    EXPECT_EQ(compression, 3);
    const int chunks = (*height + 15) / 16;
    std::map<std::string, std::vector<float>> out;
    for (const auto& [name, type] : channels) out[name].resize(static_cast<std::size_t>(*width) * static_cast<std::size_t>(*height));
    for (int chunk = 0; chunk < chunks; ++chunk) {
        std::uint64_t offset;
        std::memcpy(&offset, b.data() + at + static_cast<std::size_t>(chunk) * 8, 8);
        const int y0 = static_cast<int>(le32(b, offset));
        const auto size = le32(b, offset + 4);
        const int rows = std::min(16, *height - y0);
        std::size_t rowBytes = 0;
        for (const auto& [name, type] : channels) rowBytes += static_cast<std::size_t>(*width) * (type == 1 ? 2 : 4);
        const std::size_t rawSize = rowBytes * static_cast<std::size_t>(rows);
        std::vector<std::uint8_t> raw(b.begin() + static_cast<long>(offset + 8), b.begin() + static_cast<long>(offset + 8 + size));
        if (size < rawSize) {
            int length = 0;
            char* inflated = stbi_zlib_decode_malloc(reinterpret_cast<const char*>(raw.data()), static_cast<int>(raw.size()), &length);
            EXPECT_TRUE(inflated);
            EXPECT_EQ(static_cast<std::size_t>(length), rawSize);
            std::vector<std::uint8_t> t(inflated, inflated + length);
            std::free(inflated);
            for (std::size_t i = 1; i < t.size(); ++i) t[i] = static_cast<std::uint8_t>(t[i - 1] + t[i] - 128);
            raw.assign(t.size(), 0);
            const std::size_t half = (t.size() + 1) / 2;
            for (std::size_t i = 0; i < t.size(); ++i) raw[i] = (i & 1u) ? t[half + i / 2] : t[i / 2];
        }
        std::size_t r = 0;
        for (int y = y0; y < y0 + rows; ++y) {
            for (const auto& [name, type] : channels) {
                for (int x = 0; x < *width; ++x) {
                    float v;
                    if (type == 1) {
                        v = fromHalf(le16(raw, r));
                        r += 2;
                    } else {
                        std::memcpy(&v, raw.data() + r, 4);
                        r += 4;
                    }
                    out[name][static_cast<std::size_t>(y) * static_cast<std::size_t>(*width) + static_cast<std::size_t>(x)] = v;
                }
            }
        }
    }
    return out;
}

TEST(CardMaps, EveryFormatAndBitDepthWritesAndReadsBack) {
    // A gradient image with some structure (EXR's ZIP compresses it).
    const int w = 37, h = 41;
    for (const int channels : {1, 3, 4}) {
        std::vector<float> pixels(static_cast<std::size_t>(w * h * channels));
        for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<float>((i * 7) % 256) / 255.0f;
        const fs::path dir = freshFolder("formats");
        // TGA / PNG 8 and 16: stb reads them.
        for (const auto& [format, depth] : std::vector<std::pair<std::string, int>>{{"tga", 8}, {"png", 8}, {"png", 16}}) {
            const std::string path = (dir / ("img." + format)).u8string();
            writeImageFile(path, format, depth, w, h, channels, pixels);
            const std::vector<std::uint8_t> bytes = fileBytes(path);
            int rw = 0, rh = 0, rc = 0;
            if (depth == 16) {
                stbi_us* data = stbi_load_16_from_memory(bytes.data(), static_cast<int>(bytes.size()), &rw, &rh, &rc, 0);
                ASSERT_TRUE(data) << format;
                ASSERT_EQ(rc, channels);
                for (std::size_t i = 0; i < pixels.size(); ++i) ASSERT_EQ(data[i], std::lround(pixels[i] * 65535.0f)) << i;
                stbi_image_free(data);
            } else {
                stbi_uc* data = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &rw, &rh, &rc, 0);
                ASSERT_TRUE(data) << format;
                ASSERT_EQ(rc, channels);
                for (std::size_t i = 0; i < pixels.size(); ++i) ASSERT_EQ(data[i], std::lround(pixels[i] * 255.0f)) << i;
                stbi_image_free(data);
            }
            EXPECT_EQ(rw, w);
            EXPECT_EQ(rh, h);
        }
        // TIFF 8, 16, 32: the strip read by hand.
        for (const int depth : {8, 16, 32}) {
            const std::string path = (dir / "img.tif").u8string();
            writeImageFile(path, "tiff", depth, w, h, channels, pixels);
            const std::vector<std::uint8_t> b = fileBytes(path);
            ASSERT_EQ(b[0], 'I');
            ASSERT_EQ(le16(b, 2), 42);
            const std::uint32_t ifd = le32(b, 4);
            std::map<int, std::uint32_t> tags;
            for (std::uint16_t e = 0; e < le16(b, ifd); ++e) {
                const std::size_t at = ifd + 2 + e * 12u;
                tags[le16(b, at)] = le16(b, at + 2) == 3 && le32(b, at + 4) == 1 ? le16(b, at + 8) : le32(b, at + 8);
            }
            EXPECT_EQ(tags[256], static_cast<std::uint32_t>(w));
            EXPECT_EQ(tags[257], static_cast<std::uint32_t>(h));
            EXPECT_EQ(tags[277], static_cast<std::uint32_t>(channels));
            EXPECT_EQ(tags[262], channels == 1 ? 1u : 2u);
            const std::uint32_t data = tags[273];
            EXPECT_EQ(tags[279], pixels.size() * static_cast<std::size_t>(depth / 8));
            for (std::size_t i = 0; i < pixels.size(); ++i) {
                if (depth == 8) ASSERT_EQ(b[data + i], std::lround(pixels[i] * 255.0f));
                if (depth == 16) ASSERT_EQ(le16(b, data + i * 2), std::lround(pixels[i] * 65535.0f));
                if (depth == 32) {
                    float v;
                    std::memcpy(&v, b.data() + data + i * 4, 4);
                    ASSERT_EQ(v, pixels[i]);
                }
            }
        }
        // EXR half and float.
        for (const int depth : {16, 32}) {
            const std::string path = (dir / "img.exr").u8string();
            writeImageFile(path, "exr", depth, w, h, channels, pixels);
            int rw = 0, rh = 0;
            const auto read = readExr(path, &rw, &rh);
            ASSERT_EQ(rw, w);
            ASSERT_EQ(rh, h);
            const std::vector<std::string> names = channels == 1 ? std::vector<std::string>{"Y"}
                                                   : channels == 3 ? std::vector<std::string>{"R", "G", "B"}
                                                                   : std::vector<std::string>{"R", "G", "B", "A"};
            ASSERT_EQ(read.size(), names.size());
            for (std::size_t c = 0; c < names.size(); ++c) {
                const std::vector<float>& got = read.at(names[c]);
                for (std::size_t p = 0; p < got.size(); ++p) {
                    ASSERT_NEAR(got[p], pixels[p * static_cast<std::size_t>(channels) + c], depth == 16 ? 1e-3f : 0.0f) << names[c] << p;
                }
            }
        }
    }
    const fs::path dir = freshFolder("bad");
    EXPECT_THROW(writeImageFile((dir / "x.tga").u8string(), "tga", 16, 2, 2, 1, std::vector<float>(4)), std::runtime_error);
    EXPECT_THROW(writeImageFile((dir / "x.png").u8string(), "png", 8, 2, 2, 2, std::vector<float>(8)), std::runtime_error);
    EXPECT_THROW(writeImageFile((dir / "x.exr").u8string(), "exr", 8, 2, 2, 1, std::vector<float>(4)), std::runtime_error);
}

TEST(CardMaps, ExportWritesThePresetsFilesInAnyFormat) {
    const FurMaps f = furMaps(64);
    const fs::path dir = freshFolder("export");
    CardMapExport settings;
    settings.format = "exr";
    settings.nameTemplate = "{name}_{map}_{res}";
    const auto separate = findCardMapPreset("Separate maps (all)");
    ASSERT_TRUE(separate);
    const std::vector<std::string> paths = writeCardMaps(f.maps, *separate, settings, dir.u8string(), "fur");
    // Every source its own file; no iridescence in this groom, so not that one.
    EXPECT_EQ(paths.size(), separate->files.size() - 1);
    EXPECT_EQ(fs::path(paths.front()).filename().u8string(), "fur_Albedo_64.exr");
    int w = 0, h = 0;
    const auto albedo = readExr(paths.front(), &w, &h);
    EXPECT_EQ(w, 64);
    EXPECT_NEAR(albedo.at("R")[100], f.maps.color[300], 1e-3f);  // half float, linear (EXR's auto)
    settings.format = "png";
    settings.bitDepth = 16;
    const auto unity = findCardMapPreset("Unity HDRP Hair");
    ASSERT_TRUE(unity);
    const std::vector<std::string> pngs = writeCardMaps(f.maps, *unity, settings, dir.u8string(), "fur");
    ASSERT_EQ(pngs.size(), 5u);
    const std::vector<std::uint8_t> bytes = fileBytes(pngs.front());
    int rw = 0, rh = 0, rc = 0;
    stbi_us* data = stbi_load_16_from_memory(bytes.data(), static_cast<int>(bytes.size()), &rw, &rh, &rc, 0);
    ASSERT_TRUE(data);
    EXPECT_EQ(rc, 4);
    for (std::size_t p = 0; p < 64u * 64u; p += 37) EXPECT_EQ(data[p * 4 + 3], std::lround(f.maps.coverage[p] * 65535.0f));
    stbi_image_free(data);
    settings.bitDepth = 32;
    EXPECT_THROW(writeCardMaps(f.maps, *unity, settings, dir.u8string(), "fur"), std::runtime_error);  // no 32-bit PNG
    settings.format = "jpg";
    EXPECT_THROW(writeCardMaps(f.maps, *unity, settings, dir.u8string(), "fur"), std::runtime_error);
}

TEST(CardMaps, PresetsSaveLoadAndCheck) {
    const fs::path dir = freshFolder("presets");
    setEnv("GROOMLAB_USER_CARD_PRESETS", dir.u8string());
    setEnv("GROOMLAB_CARD_PRESET_PATH", "");
    EXPECT_EQ(cardMapPresetUserDir(), dir.u8string());
    const std::vector<CardMapPreset> builtins = builtinCardMapPresets();
    ASSERT_EQ(builtins.size(), 4u);
    for (const CardMapPreset& p : builtins) {
        EXPECT_EQ(checkCardMapPreset(p), "") << p.name;
        // Round trip through JSON.
        const CardMapPreset back = cardMapPresetFromJson(cardMapPresetToJson(p));
        EXPECT_EQ(back.name, p.name);
        EXPECT_EQ(back.format, p.format);
        EXPECT_EQ(back.normal, p.normal);
        ASSERT_EQ(back.files.size(), p.files.size());
        for (std::size_t i = 0; i < p.files.size(); ++i) {
            EXPECT_EQ(back.files[i].map, p.files[i].map);
            EXPECT_EQ(back.files[i].channels, p.files[i].channels);
            EXPECT_EQ(back.files[i].colorSpace, p.files[i].colorSpace);
        }
    }
    CardMapPreset mine;
    mine.name = "My Packed";
    mine.format = "png";
    mine.files = {{"Mask", {"depth", "root", "id", "coverage"}, "auto"}, {"Color", {"color.r", "color.g", "color.b"}, "srgb"}};
    const std::string path = saveCardMapPreset(mine);
    EXPECT_TRUE(fs::exists(fs::u8path(path)));
    const auto found = findCardMapPreset("my packed");
    ASSERT_TRUE(found);
    EXPECT_FALSE(found->builtin);
    EXPECT_EQ(found->files.size(), 2u);
    EXPECT_EQ(found->files[0].channels[2], "id");
    EXPECT_EQ(cardMapPresets().size(), 5u);
    EXPECT_NE(cardMapPresetsJson().find("My Packed"), std::string::npos);
    // Saving again replaces it; a built-in's name can't be taken.
    mine.files.pop_back();
    saveCardMapPreset(mine);
    EXPECT_EQ(findCardMapPreset("My Packed")->files.size(), 1u);
    EXPECT_EQ(cardMapPresets().size(), 5u);
    CardMapPreset clash = mine;
    clash.name = "Unreal Hair Cards";
    EXPECT_THROW(saveCardMapPreset(clash), std::runtime_error);
    // A file that isn't a preset is skipped.
    std::ofstream(dir / "junk.json") << "{not json";
    EXPECT_EQ(cardMapPresets().size(), 5u);
    EXPECT_TRUE(deleteCardMapPreset("My Packed"));
    EXPECT_FALSE(findCardMapPreset("My Packed"));
    EXPECT_FALSE(deleteCardMapPreset("Unreal Hair Cards"));
    EXPECT_TRUE(findCardMapPreset("Unreal Hair Cards"));
    // What check says.
    CardMapPreset bad = mine;
    bad.files = {};
    EXPECT_NE(checkCardMapPreset(bad), "");
    bad.files = {{"A", {"depth", "depth", "depth", "depth", "depth"}, "auto"}};
    EXPECT_NE(checkCardMapPreset(bad), "");
    bad.files = {{"A/B", {"depth"}, "auto"}};
    EXPECT_NE(checkCardMapPreset(bad), "");
    bad.files = {{"A", {"depth"}, "auto"}, {"a", {"ao"}, "auto"}};
    EXPECT_NE(checkCardMapPreset(bad), "");
    bad.files = {{"A", {"nothing"}, "auto"}};
    EXPECT_NE(checkCardMapPreset(bad), "");
    bad.files = {{"A", {"iridescence?", "depth"}, "auto"}};
    EXPECT_NE(checkCardMapPreset(bad), "");
    bad.files = {{"A", {"depth"}, "auto"}};
    bad.format = "png";
    bad.bitDepth = 32;
    EXPECT_NE(checkCardMapPreset(bad), "");
    EXPECT_THROW(cardMapPresetFromJson("[]"), std::runtime_error);
    EXPECT_EQ(cardMapFileName("{name}_{map}", "hair", "Depth", 2048), "hair_Depth");
    EXPECT_EQ(cardMapFileName("{map}-{res}-{name}", "hair", "AO", 1024), "AO-1024-hair");
    setEnv("GROOMLAB_USER_CARD_PRESETS", "");
}

}  // namespace
}  // namespace groomlab
