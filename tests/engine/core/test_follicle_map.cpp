// SPDX-License-Identifier: Apache-2.0
// Bake Follicle Map: a linear radial gradient round each root on the skin's uvs, 1 at the root, 0
// at the radius (1 - distance / radius), the nearest root's at each texel; the
// radius from the roots' spacing when none is given; a UDIM tile set when the uvs leave 0-1.

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <vector>

#include "groomlab/follicle_map.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

// The unit square in XZ with uvs u = x, v = z (shifted `tileU` tiles along u).
std::shared_ptr<const ScalpMesh> uvPlane(float tileU = 0.0f) {
    auto mesh = std::make_shared<ScalpMesh>(*test::makePlane(8, 1.0f));
    std::vector<ScalpMesh::UV> uvs;
    for (const auto& tri : mesh->triangles()) {
        for (const std::int32_t v : tri) {
            const Vec3f& p = mesh->points()[static_cast<std::size_t>(v)];
            uvs.push_back({p.x + tileU, p.z});
        }
    }
    mesh->setCornerUVs(std::move(uvs));
    return mesh;
}

StrandSet rootedAt(const ScalpMesh& skin, const std::vector<Vec3f>& at) {
    StrandSet s;
    StrandId id = 1;
    for (const Vec3f& p : at) s.addStrand(id++, {p, p + Vec3f{0.0f, 0.2f, 0.0f}}, {}, skin.closestPoint(p).root);
    return s;
}

// The value at the texel over (x, z) on the plane, 0-1.
float at(const FollicleMap& map, float x, float z) {
    const int res = map.resolution;
    const int px = std::clamp(static_cast<int>(x * static_cast<float>(res)), 0, res - 1);
    const int py = std::clamp(static_cast<int>((1.0f - z) * static_cast<float>(res)), 0, res - 1);
    return static_cast<float>(map.tiles.front().value[static_cast<std::size_t>(py * res + px)]) / 65535.0f;
}

TEST(FollicleMap, AGradientFromEachRootToTheRadius) {
    const auto skin = uvPlane();
    const StrandSet strands = rootedAt(*skin, {{0.5f, 0.0f, 0.5f}});
    FollicleMapOptions options;
    options.resolution = 256;
    options.radius = 0.1f;
    const FollicleMap map = bakeFollicleMap(*skin, strands, options);
    ASSERT_EQ(map.tiles.size(), 1u);
    EXPECT_FALSE(map.udim);
    EXPECT_EQ(map.follicles, 1u);
    EXPECT_FLOAT_EQ(map.radius, 0.1f);
    EXPECT_GT(at(map, 0.5f, 0.5f), 0.95f);  // (the texel's middle a little off the root)
    EXPECT_NEAR(at(map, 0.55f, 0.5f), 0.5f, 0.03f);  // halfway: half
    EXPECT_NEAR(at(map, 0.5f, 0.45f), 0.5f, 0.03f);
    EXPECT_NEAR(at(map, 0.575f, 0.5f), 0.25f, 0.03f);  // linear: a quarter of the way from the radius
    EXPECT_EQ(at(map, 0.62f, 0.5f), 0.0f);
    EXPECT_EQ(at(map, 0.1f, 0.1f), 0.0f);
    // Falling all the way out.
    float last = 2.0f;
    for (float x = 0.5f; x < 0.6f; x += 0.01f) {
        const float v = at(map, x, 0.5f);
        EXPECT_LE(v, last + 1e-4f) << x;
        last = v;
    }
    // A wider radius reaches further: past the strands' width.
    options.radius = 0.3f;
    const FollicleMap wide = bakeFollicleMap(*skin, strands, options);
    EXPECT_NEAR(at(wide, 0.65f, 0.5f), 0.5f, 0.03f);
}

TEST(FollicleMap, TheRadiusComesFromTheRootsSpacing) {
    const auto skin = uvPlane();
    std::vector<Vec3f> grid;
    for (int i = 1; i < 10; ++i) {
        for (int j = 1; j < 10; ++j) grid.push_back({0.1f * static_cast<float>(i), 0.0f, 0.1f * static_cast<float>(j)});
    }
    const StrandSet strands = rootedAt(*skin, grid);
    EXPECT_NEAR(follicleMapAutoRadius(*skin, strands), 0.04f, 1e-4f);
    FollicleMapOptions options;
    options.resolution = 128;
    const FollicleMap map = bakeFollicleMap(*skin, strands, options);
    EXPECT_NEAR(map.radius, 0.04f, 1e-4f);
    EXPECT_EQ(map.follicles, grid.size());
    // Every root a dot; between two roots bare skin.
    for (const Vec3f& p : grid) EXPECT_GT(at(map, p.x, p.z), 0.8f);  // (a texel is 0.2 of the radius here)
    EXPECT_EQ(at(map, 0.15f, 0.15f), 0.0f);
    EXPECT_GT(map.touched, 0u);
    EXPECT_LT(map.touched, map.texels);
}

TEST(FollicleMap, AUdimTileSetAndItsFiles) {
    const auto skin = uvPlane(1.0f);  // u in 1-2: tile 1002
    const StrandSet strands = rootedAt(*skin, {{0.5f, 0.0f, 0.5f}});
    FollicleMapOptions options;
    options.resolution = 64;
    options.radius = 0.2f;
    const FollicleMap map = bakeFollicleMap(*skin, strands, options);
    ASSERT_EQ(map.tiles.size(), 1u);
    EXPECT_TRUE(map.udim);
    EXPECT_EQ(map.tiles.front().udim, 1002);
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "groomlab_follicle_map";
    std::filesystem::create_directories(dir);
    const std::string prefix = (dir / "head_").string();
    const std::vector<std::string> files = writeFollicleMap(map, prefix);
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files.front(), prefix + "FollicleMap.1002.png");
    EXPECT_TRUE(std::filesystem::exists(files.front()));
    EXPECT_EQ(follicleMapPath(prefix, true), prefix + "FollicleMap.<UDIM>.png");
    std::filesystem::remove_all(dir);
}

TEST(FollicleMap, NeedsUvsAndRoots) {
    const auto skin = uvPlane();
    EXPECT_THROW(bakeFollicleMap(*test::makePlane(4, 1.0f), rootedAt(*skin, {{0.5f, 0.0f, 0.5f}})), std::invalid_argument);
    EXPECT_THROW(bakeFollicleMap(*skin, StrandSet{}), std::invalid_argument);
    FollicleMapOptions options;
    options.resolution = 8;
    EXPECT_THROW(bakeFollicleMap(*skin, rootedAt(*skin, {{0.5f, 0.0f, 0.5f}}), options), std::invalid_argument);
}

}  // namespace
}  // namespace groomlab
