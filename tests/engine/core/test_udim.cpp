// SPDX-License-Identifier: Apache-2.0
// UDIM tile sets for texture maps (issue #87, udim.h): a scalp over several tiles paints, saves,
// converts, projects and is read by nodes as one image over UVs 0-1 is.

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>

#include "groomlab/asset_files.h"
#include "groomlab/graph.h"
#include "groomlab/map_ref.h"
#include "groomlab/operators.h"
#include "groomlab/paths.h"
#include "groomlab/scalp_paint.h"
#include "groomlab/texture.h"
#include "groomlab/udim.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

namespace fs = std::filesystem;

// A plane `across` x `down` units in x and z, `cells` quads a unit, its UVs given by `uv` of
// each point (x, z): each quad's two triangles lie within one unit square.
template <class F>
std::shared_ptr<ScalpMesh> tiledPlane(int across, int down, int cells, F uv) {
    std::vector<Vec3f> points;
    std::vector<ScalpMesh::Triangle> triangles;
    const int nx = across * cells, nz = down * cells;
    for (int z = 0; z <= nz; ++z) {
        for (int x = 0; x <= nx; ++x) points.push_back({static_cast<float>(x) / cells, 0.0f, static_cast<float>(z) / cells});
    }
    auto at = [&](int x, int z) { return z * (nx + 1) + x; };
    for (int z = 0; z < nz; ++z) {
        for (int x = 0; x < nx; ++x) {
            triangles.push_back({at(x, z), at(x + 1, z), at(x, z + 1)});
            triangles.push_back({at(x + 1, z), at(x + 1, z + 1), at(x, z + 1)});
        }
    }
    auto mesh = std::make_shared<ScalpMesh>(points, triangles);
    std::vector<ScalpMesh::UV> uvs;
    for (std::size_t t = 0; t < triangles.size(); ++t) {
        const Vec3f c = (points[triangles[t][0]] + points[triangles[t][1]] + points[triangles[t][2]]) * (1.0f / 3.0f);
        for (const std::int32_t v : triangles[t]) uvs.push_back(uv(points[static_cast<std::size_t>(v)], c));
    }
    mesh->setCornerUVs(std::move(uvs));
    return mesh;
}

// The 2 x 1 plane as one image (u = x / 2), over tiles 1001 and 1002 (u = x), and over 1001
// and 1011 (the right half moved up a tile row): the same surface, three UV layouts.
std::shared_ptr<ScalpMesh> oneImagePlane() {
    return tiledPlane(2, 1, 6, [](const Vec3f& p, const Vec3f&) { return ScalpMesh::UV{p.x / 2.0f, p.z}; });
}
std::shared_ptr<ScalpMesh> twoTilePlane() {
    return tiledPlane(2, 1, 6, [](const Vec3f& p, const Vec3f&) { return ScalpMesh::UV{p.x, p.z}; });
}
std::shared_ptr<ScalpMesh> stackedTilePlane() {
    return tiledPlane(2, 1, 6, [](const Vec3f& p, const Vec3f& c) {
        return c.x < 1.0f ? ScalpMesh::UV{p.x, p.z} : ScalpMesh::UV{p.x - 1.0f, p.z + 1.0f};
    });
}

fs::path freshFolder(const char* name) {
    const fs::path dir = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

TEST(Udim, LayoutsSlotsAndTilePaths) {
    const UdimLayout one = udimLayoutOf(*oneImagePlane());
    EXPECT_FALSE(one.udim());
    EXPECT_EQ(one.tiles, std::vector<int>{1001});
    EXPECT_EQ(one.mosaicSize(64, 32), (std::array<int, 2>{64, 32}));

    const UdimLayout two = udimLayoutOf(*twoTilePlane());
    EXPECT_TRUE(two.udim());
    EXPECT_EQ(two.tiles, (std::vector<int>{1001, 1002}));
    EXPECT_EQ(two.columns, 2);
    EXPECT_EQ(two.rows, 1);
    const int g = UdimLayout::kGutter;
    EXPECT_EQ(two.mosaicSize(64, 32), (std::array<int, 2>{2 * (64 + 2 * g), 32 + 2 * g}));
    EXPECT_EQ(two.tileOrigin(1002, 64, 32), (std::array<int, 2>{64 + 3 * g, g}));
    // A UV on its tile into the mosaic and back.
    const ScalpMesh::UV m = two.toMosaic({1.25f, 0.5f}, 1002, 64, 32);
    const std::array<int, 2> size = two.mosaicSize(64, 32);
    ScalpMesh::UV back{};
    EXPECT_EQ(two.fromMosaic(m[0] * size[0], (1.0f - m[1]) * size[1], 64, 32, &back), 1002);
    EXPECT_NEAR(back[0], 1.25f, 1e-4f);
    EXPECT_NEAR(back[1], 0.5f, 1e-4f);
    EXPECT_EQ(two.fromMosaic(1.0f, 1.0f, 64, 32, nullptr), -1);  // a gutter

    const UdimLayout stacked = udimLayoutOf(*stackedTilePlane());
    EXPECT_EQ(stacked.tiles, (std::vector<int>{1001, 1011}));
    EXPECT_EQ(udimTileAt(0.5f, 0.5f), 1001);
    EXPECT_EQ(udimTileAt(3.5f, 2.5f), 1024);
    EXPECT_EQ(udimTileAt(10.5f, 0.5f), -1);

    // Paths: a plain name gets ".<UDIM>"; a tile's name keeps its separator; tokens are tiles.
    EXPECT_EQ(udimPathFor("masks/head/density.png"), "masks/head/density.<UDIM>.png");
    EXPECT_EQ(udimPathFor("masks/head_1001.png"), "masks/head_<UDIM>.png");
    EXPECT_EQ(udimPathFor("x.1002.png"), "x.<UDIM>.png");
    EXPECT_EQ(udimPathFor("y.<UDIM>.png"), "y.<UDIM>.png");
    EXPECT_EQ(udimTilePath("h_<UDIM>.png", 1003), "h_1003.png");
    EXPECT_EQ(udimTilePath("h.%(UDIM)d.png", 1012), "h.1012.png");
    EXPECT_EQ(udimTilePath("h.<udim>.png", 1001), "h.1001.png");
}

TEST(Udim, PaintsAcrossTilesAndSavesATileSetInEitherNaming) {
    const auto scalp = twoTilePlane();
    TexturePaint paint(*scalp, 32, 32, 0.0f);
    ASSERT_TRUE(paint.udim());
    EXPECT_EQ(paint.tileWidth(), 32);
    EXPECT_EQ(paint.width(), 2 * (32 + 2 * UdimLayout::kGutter));
    // A dab on the tiles' shared border paints both sides of it.
    EXPECT_GT(paint.stamp({1.0f, 0.0f, 0.5f}, 0.3f, 1.0f, 1.0f), 0u);

    const fs::path dir = freshFolder("groomlab_udim_paint");
    const std::string plain = (dir / "density.png").u8string();
    const std::string written = paint.save(plain);
    EXPECT_EQ(written, (dir / "density.<UDIM>.png").u8string());
    EXPECT_TRUE(fs::exists(dir / "density.1001.png"));
    EXPECT_TRUE(fs::exists(dir / "density.1002.png"));
    EXPECT_FALSE(fs::exists(dir / "density.png"));
    // Read back through the map's plain path (as a node reads it): the set, both sides painted.
    const TexturePtr read = loadTexture(plain);
    ASSERT_TRUE(read->isUdim());
    EXPECT_GT(read->sample(0.95f, 0.5f)[0], 0.5f);
    EXPECT_GT(read->sample(1.05f, 0.5f)[0], 0.5f);
    EXPECT_LT(read->sample(0.2f, 0.5f)[0], 0.05f);
    EXPECT_LT(read->sample(1.8f, 0.5f)[0], 0.05f);
    // Opened again from the plain path: the same texels (8-bit), on the same mosaic.
    const std::shared_ptr<TexturePaint> again = openTexturePaint(*scalp, plain, kGrayMap, 64);
    ASSERT_TRUE(again->udim());
    EXPECT_EQ(again->tileWidth(), 32);
    EXPECT_EQ(again->toBytes(0), paint.toBytes(0));

    // "<name>_1001.png" (a set named by its first tile): its "_" kept.
    const std::string underscored = paint.save((dir / "head_1001.png").u8string());
    EXPECT_EQ(underscored, (dir / "head_<UDIM>.png").u8string());
    EXPECT_TRUE(fs::exists(dir / "head_1002.png"));
    const TexturePtr byTile = loadTexture((dir / "head_1001.png").u8string());  // (siblings: a set)
    EXPECT_TRUE(byTile->isUdim());
    EXPECT_GT(byTile->sample(1.05f, 0.5f)[0], 0.5f);
    // A color map too: its tiles RGBA.
    TexturePaint color(*scalp, 16, 16, PaintColor{0.0f, 0.0f, 0.0f, 1.0f});
    color.stamp({1.5f, 0.0f, 0.5f}, 0.2f, 1.0f, PaintColor{1.0f, 0.5f, 0.0f, 1.0f}, kAllChannels);
    color.save((dir / "tint.png").u8string());
    const TexturePtr tint = loadTexture((dir / "tint.png").u8string());
    const std::array<float, 4> c = tint->sample(1.5f, 0.5f);  // (the dab's middle, its falloff near 1)
    EXPECT_GT(c[0], 0.8f);
    EXPECT_NEAR(c[1] / c[0], 0.5f, 0.03f);
    EXPECT_LT(c[2], 0.02f);
    EXPECT_LT(tint->sample(0.5f, 0.5f)[0], 0.02f);  // (tile 1001 untouched)
    EXPECT_THROW(paint.pngBytes(), std::logic_error);  // (a set is written tile by tile)
    fs::remove_all(dir);
}

TEST(Udim, MapsProjectBetweenOneImageAndTileSetsEitherWay) {
    // A gradient (x / 2 over the plane) painted on each layout, projected onto each other one:
    // every texel of the new map reads what lies at its point, whichever tile either is on.
    const fs::path dir = freshFolder("groomlab_udim_project");
    const std::vector<std::pair<const char*, std::shared_ptr<ScalpMesh>>> layouts = {
        {"one", oneImagePlane()}, {"two", twoTilePlane()}, {"stacked", stackedTilePlane()}};
    for (const auto& [fromName, from] : layouts) {
        TexturePaint source(*from, 64, 64, 0.0f);
        source.assign([](const StrandRoot&, const Vec3f& p) { return p.x / 2.0f; });
        const std::string path = source.save((dir / (std::string(fromName) + ".png")).u8string());
        const TexturePtr image = loadTexture(path);
        for (const auto& [toName, to] : layouts) {
            SCOPED_TRACE(std::string(fromName) + " -> " + toName);
            TexturePaint out = projectTexture(*image, kGrayMap, *from, *to);
            EXPECT_EQ(out.udim(), udimLayoutOf(*to).udim());
            EXPECT_EQ(out.tileWidth(), 64);
            float worst = 0.0f;
            std::size_t covered = 0;
            for (int y = 0; y < out.height(); ++y) {
                for (int x = 0; x < out.width(); ++x) {
                    if (!out.covered(x, y)) continue;
                    ++covered;
                    const float want = out.position(x, y).x / 2.0f;
                    worst = std::max(worst, std::abs(out.values()[static_cast<std::size_t>(y) * out.width() + x] - want));
                }
            }
            EXPECT_GT(covered, 1000u);
            EXPECT_LT(worst, 0.03f);
        }
    }
    fs::remove_all(dir);
}

TEST(Udim, ConversionsAndNodesReadTileSets) {
    const fs::path dir = freshFolder("groomlab_udim_read");
    const auto scalp = twoTilePlane();
    // Tile 1001 black, 1002 white.
    TexturePaint paint(*scalp, 32, 32, 0.0f);
    paint.assign([](const StrandRoot&, const Vec3f& p) { return p.x > 1.0f ? 1.0f : 0.0f; });
    const std::string plain = (dir / "mask.png").u8string();
    paint.save(plain);
    const TexturePtr image = loadTexture(plain);
    // To a vertex map: each vertex reads its triangles' own tiles (a border vertex between them).
    const std::vector<float> perVertex = vertexMapFromTexture(*image, *scalp);
    for (std::size_t v = 0; v < scalp->pointCount(); ++v) {
        const float x = scalp->points()[v].x;
        if (x < 0.9f) EXPECT_LT(perVertex[v], 0.05f) << x;
        if (x > 1.1f) EXPECT_GT(perVertex[v], 0.95f) << x;
    }
    // Roots: the tile of the triangle each lies on.
    std::vector<StrandRoot> roots;
    for (std::size_t f = 0; f < scalp->triangleCount(); f += 7) roots.push_back(StrandRoot{static_cast<std::int32_t>(f), 0.3f, 0.3f});
    const std::vector<float> atRoots = rootValuesFromTexture(*image, *scalp, roots);
    for (std::size_t i = 0; i < roots.size(); ++i) {
        const float x = scalp->position(roots[i]).x;
        if (x < 0.95f) EXPECT_LT(atRoots[i], 0.05f);
        if (x > 1.05f) EXPECT_GT(atRoots[i], 0.95f);
    }
    // A vertex map back to a texture: a tile set again.
    const TexturePaint back = textureFromVertexMap(perVertex, *scalp, 32, 32);
    EXPECT_TRUE(back.udim());

    // A node: Scatter's density read from the map's plain path grows hair on tile 1002 alone.
    Graph g;
    ExternalInputs ext;
    ext.set("scalp", MeshPtr(scalp), scalp->contentHash());
    const NodeId s = g.addNode(op::kScalp), scatter = g.addNode(op::kScatter);
    g.connect(s, scatter, 0);
    MapRef density;
    density.source = MapRef::Source::Texture;
    density.name = plain;
    density.min = 0.0f;
    density.max = 600.0f;
    g.node(scatter).params().set("density", density);
    EvalContext ctx;
    ctx.externals = &ext;
    const StrandsPtr strands = g.evaluateStrands(scatter, ctx);
    ASSERT_TRUE(strands);
    ASSERT_GT(strands->strandCount(), 50u);
    std::size_t onFirst = 0;
    for (std::size_t i = 0; i < strands->strandCount(); ++i) {
        if (strands->strandPoints(i)[0].x < 0.95f) ++onFirst;
    }
    EXPECT_EQ(onFirst, 0u);
    fs::remove_all(dir);
}

TEST(Udim, SaveAsTakesATileSetAlong) {
    // A map painted while the scene was untitled (its plain path standing for a tile set): a save
    // copies every tile into the new scene's folder and the map names the copy; other content
    // there takes a free name; the same tiles there are used as they are.
    const fs::path dir = freshFolder("groomlab_udim_follow");
    const auto scalp = twoTilePlane();
    TexturePaint paint(*scalp, 16, 16, 0.25f);
    const std::string oldBase = (dir / "project").generic_u8string(), newBase = (dir / "shot").generic_u8string();
    const std::string stored = "groomlab/untitled/masks/g/mask.png";
    fs::create_directories(fs::u8path(oldBase + "/groomlab/untitled/masks/g"));
    paint.save(oldBase + "/" + stored);
    ASSERT_TRUE(fs::exists(fs::u8path(oldBase + "/groomlab/untitled/masks/g/mask.1002.png")));
    FileFollower first(oldBase, oldBase + "/groomlab/untitled", newBase, newBase + "/groomlab/hero");
    EXPECT_EQ(first.follow(stored), "groomlab/hero/masks/g/mask.png");
    EXPECT_EQ(first.copied().size(), 2u);
    for (const char* tile : {"1001", "1002"}) {
        EXPECT_TRUE(fs::exists(fs::u8path(newBase + "/groomlab/hero/masks/g/mask." + tile + ".png"))) << tile;
    }
    EXPECT_TRUE(sameMapImage(oldBase + "/" + stored, newBase + "/groomlab/hero/masks/g/mask.png"));
    EXPECT_GT(loadTexture(newBase + "/groomlab/hero/masks/g/mask.png")->udimTiles().size(), 1u);
    // Again: the same tiles there, nothing copied.
    FileFollower again(oldBase, oldBase + "/groomlab/untitled", newBase, newBase + "/groomlab/hero");
    EXPECT_EQ(again.follow(stored), "groomlab/hero/masks/g/mask.png");
    EXPECT_TRUE(again.copied().empty());
    // Other content there: a free name, every tile.
    TexturePaint other(*scalp, 16, 16, 0.75f);
    fs::remove_all(fs::u8path(newBase + "/groomlab/hero"));
    fs::create_directories(fs::u8path(newBase + "/groomlab/hero/masks/g"));
    other.save(newBase + "/groomlab/hero/masks/g/mask.png");
    FileFollower clash(oldBase, oldBase + "/groomlab/untitled", newBase, newBase + "/groomlab/hero");
    EXPECT_EQ(clash.follow(stored), "groomlab/hero/masks/g/mask_2.png");
    EXPECT_EQ(clash.warnings().size(), 1u);
    EXPECT_TRUE(sameMapImage(oldBase + "/" + stored, newBase + "/groomlab/hero/masks/g/mask_2.png"));
    fs::remove_all(dir);
}

TEST(Udim, AMapWhoseScalpReachesAnotherTileBecomesATileSet) {
    // A map painted as one image; the scalp's UVs then reach tile 1002 (edited after): the paint
    // reads the image into each tile, and its save writes the tiles and removes the image, which
    // readers would otherwise keep reading before them (the new tiles never seen).
    const fs::path dir = freshFolder("groomlab_udim_grow");
    const std::string plain = (dir / "dens.png").generic_u8string();
    TexturePaint one(*oneImagePlane(), 16, 16, 0.25f);
    one.save(plain);
    EXPECT_TRUE(isPlainImagePath(plain));
    EXPECT_FALSE(isPlainImagePath((dir / "dens.1001.png").generic_u8string()));
    EXPECT_FALSE(isPlainImagePath((dir / "head_1002.png").generic_u8string()));
    EXPECT_FALSE(isPlainImagePath((dir / "dens.<UDIM>.png").generic_u8string()));
    const auto scalp = twoTilePlane();
    TexturePaint grown(*scalp, *loadTexture(plain), 1);
    ASSERT_TRUE(grown.udim());
    grown.fill(0.75f);
    const std::vector<std::string> files = mapImageFiles(plain, grown.udimLayout()->tiles);
    ASSERT_EQ(files.size(), 3u);  // the image it replaces, then its tiles
    EXPECT_EQ(files.front(), plain);
    grown.save(plain);
    EXPECT_FALSE(fs::exists(fs::u8path(plain)));
    const TexturePtr read = loadTexture(plain);
    ASSERT_TRUE(read->isUdim());
    EXPECT_EQ(read->udimTiles(), (std::vector<int>{1001, 1002}));
    EXPECT_NEAR(read->udimTile(1002)->sample(0.5f, 0.5f)[0], 0.75f, 0.01f);
    fs::remove_all(dir);
}

TEST(Udim, HairColorReadsAColorTileSet) {
    // A color texture map over two tiles (1001 red, 1002 blue), read by HairColor's map by its
    // plain path: each strand takes its root's tile's color.
    const fs::path dir = freshFolder("groomlab_udim_color");
    const auto scalp = twoTilePlane();
    TexturePaint paint(*scalp, 16, 16, PaintColor{0.0f, 0.0f, 0.0f, 1.0f});
    paint.assignColor([](const StrandRoot&, const Vec3f& p) {
        return p.x > 1.0f ? PaintColor{0.0f, 0.0f, 1.0f, 1.0f} : PaintColor{1.0f, 0.0f, 0.0f, 1.0f};
    });
    const std::string plain = (dir / "tint.png").u8string();
    paint.save(plain);
    ASSERT_TRUE(loadTexture(plain)->isUdim());
    Graph g;
    ExternalInputs ext;
    ext.set("scalp", MeshPtr(scalp), scalp->contentHash());
    const NodeId s = g.addNode(op::kScalp), scatter = g.addNode(op::kScatter), color = g.addNode(op::kHairColor);
    g.connect(s, scatter, 0);
    g.connect(scatter, color, 0);
    g.node(scatter).params().set("density", 200.0);
    g.node(color).params().set("mode", std::string("color"));
    g.node(color).params().set("map", plain);
    g.node(color).params().set("valueRandom", 0.0);
    g.node(color).params().set("hueRandom", 0.0);
    g.node(color).params().set("saturationRandom", 0.0);
    EvalContext ctx;
    ctx.externals = &ext;
    const StrandsPtr strands = g.evaluateStrands(color, ctx);
    ASSERT_TRUE(strands);
    const std::vector<float>* r = strands->strandChannel("colorR");
    const std::vector<float>* b = strands->strandChannel("colorB");
    ASSERT_TRUE(r && b);
    std::size_t red = 0, blue = 0;
    for (std::size_t i = 0; i < strands->strandCount(); ++i) {
        const float x = strands->strandPoints(i)[0].x;
        if (x < 0.9f) {
            EXPECT_GT((*r)[i], (*b)[i]) << x;
            ++red;
        } else if (x > 1.1f) {
            EXPECT_GT((*b)[i], (*r)[i]) << x;
            ++blue;
        }
    }
    EXPECT_GT(red, 20u);
    EXPECT_GT(blue, 20u);
    fs::remove_all(dir);
}

TEST(Udim, ResolveAndCollectFindATileSetByItsPlainPath) {
    // A map's plain path standing for a tile set is there for the resolver (the folder holding
    // the tiles, not the first one searched) and Collect Files (every tile, under one name).
    const fs::path dir = freshFolder("groomlab_udim_collect");
    const auto scalp = twoTilePlane();
    TexturePaint paint(*scalp, 16, 16, 0.5f);
    const std::string project = (dir / "project").generic_u8string(), scene = (dir / "scene").generic_u8string();
    fs::create_directories(fs::u8path(project + "/masks"));
    fs::create_directories(fs::u8path(scene));
    paint.save(project + "/masks/dens.png");
    EXPECT_EQ(fs::u8path(resolvePathIn("masks/dens.png", {scene, project})).generic_u8string(), project + "/masks/dens.png");
    std::vector<std::string> copied;
    const std::string copy = collectFile(project + "/masks/dens.png", scene + "/masks", &copied);
    EXPECT_EQ(copy, scene + "/masks/dens.png");
    EXPECT_EQ(copied.size(), 2u);
    EXPECT_TRUE(sameMapImage(project + "/masks/dens.png", copy));
    // Again: the same tiles there, used as they are; other content there: a free name.
    copied.clear();
    EXPECT_EQ(collectFile(project + "/masks/dens.png", scene + "/masks", &copied), copy);
    EXPECT_TRUE(copied.empty());
    TexturePaint other(*scalp, 16, 16, 0.9f);
    fs::create_directories(fs::u8path(scene + "/other"));
    other.save(scene + "/other/dens.png");
    EXPECT_EQ(collectFile(project + "/masks/dens.png", scene + "/other"), scene + "/other/dens_2.png");
    fs::remove_all(dir);
}

}  // namespace
}  // namespace groomlab
