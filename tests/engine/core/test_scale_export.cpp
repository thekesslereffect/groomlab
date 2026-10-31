// SPDX-License-Identifier: Apache-2.0
// Scales out of the groom (scales phase 2): their bent mesh and their instancer in USD, and the
// skin maps baked from them (scale_maps.h).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "groomlab/graph.h"
#include "groomlab/groom.h"
#include "groomlab/groom_file.h"
#include "groomlab/operators.h"
#include "groomlab/scale_maps.h"
#include "groomlab/scales.h"
#include "groomlab/strand_geometry.h"
#include "groomlab/texture.h"
#include "groomlab/usd_writer.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

namespace fs = std::filesystem;

// A sphere with uvs (longitude, latitude) in the 0-1 square, as a body's skin; `tileU` moves
// them to the tiles 1001 + 0..tileU-1 by longitude (a UDIM layout).
std::shared_ptr<const ScalpMesh> uvSkin(float radius, int tilesU = 1) {
    auto mesh = std::make_shared<ScalpMesh>(*test::makeSphere(48, 96, radius));
    std::vector<ScalpMesh::UV> uvs;
    for (const auto& tri : mesh->triangles()) {
        // The triangle's longitude decides its tile (one tile for all its corners).
        Vec3f mid{};
        for (const std::int32_t v : tri) mid += mesh->points()[static_cast<std::size_t>(v)] / 3.0f;
        const float midU = 0.5f + std::atan2(mid.z, mid.x) / 6.2831853f;
        const float tile = std::min(std::floor(midU * static_cast<float>(tilesU)), static_cast<float>(tilesU - 1));
        for (const std::int32_t v : tri) {
            const Vec3f& p = mesh->points()[static_cast<std::size_t>(v)];
            float u = 0.5f + std::atan2(p.z, p.x) / 6.2831853f;
            if (u - midU > 0.5f) u -= 1.0f;  // the seam: on the triangle's side
            if (midU - u > 0.5f) u += 1.0f;
            u = u * static_cast<float>(tilesU) - tile;  // within the tile, then onto it
            uvs.push_back({std::clamp(u, 0.0f, 1.0f) + tile, std::acos(std::clamp(p.y / radius, -1.0f, 1.0f)) / 3.14159265f});
        }
    }
    mesh->setCornerUVs(std::move(uvs));
    return mesh;
}

// Scatter, Bend (combed down), Width, HairColor, Instance in mode scales: the scales of a body.
struct Scaled {
    Graph g;
    ExternalInputs ext;
    MeshPtr skin;
    NodeId scales = 0;
    explicit Scaled(MeshPtr mesh, double density, double width = 0.6) : skin(std::move(mesh)) {
        ext.set("scalp", skin, skin->surfaceHash());
        const NodeId scalp = g.addNode(op::kScalp), scatter = g.addNode(op::kScatter), comb = g.addNode(op::kBend);
        const NodeId w = g.addNode(op::kWidth), color = g.addNode(op::kHairColor);
        scales = g.addNode(op::kScales);
        g.node(scatter).params().set("density", density);
        g.node(comb).params().set("angle", 180.0);  // laid down the body (-y on the skin)
        g.node(comb).params().set("end", 0.0);
        g.node(comb).params().set("along", std::string("direction"));
        g.node(comb).params().set("direction", Vec3f{0, -1, 0});
        g.node(w).params().set("width", width);
        g.node(color).params().set("melaninRandom", 0.3);
        ParamSet& p = g.node(scales).params();
        p.set("prototype", std::string("groomlab:scale groomlab:scaleRound"));
        g.connect(scalp, scatter, 0);
        g.connect(scatter, comb, 0);
        g.connect(comb, w, 0);
        g.connect(w, color, 0);
        g.connect(color, scales, 0);
    }
    ParamSet& params() { return g.node(scales).params(); }
    StrandsPtr eval() {
        EvalContext ctx;
        ctx.externals = &ext;
        return g.evaluateStrands(scales, ctx);
    }
};

std::string readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

// The numbers of the first time sample of `declaration` within the prim `prim`.
std::vector<double> firstSample(const std::string& text, const std::string& prim, const std::string& declaration) {
    const std::size_t at = text.find(prim);
    if (at == std::string::npos) return {};
    const std::size_t decl = text.find(declaration + ".timeSamples = {", at);
    if (decl == std::string::npos) return {};
    const std::size_t open = text.find(": [", decl) + 2, close = text.find(']', open);  // (past the type's [])
    std::string list = text.substr(open + 1, close - open - 1);
    for (char& c : list) {
        if (c == '(' || c == ')' || c == ',') c = ' ';
    }
    std::istringstream in(list);
    std::vector<double> out;
    for (double v; in >> v;) out.push_back(v);
    return out;
}

// v rotated by the unit quaternion (w, x, y, z).
Vec3f rotate(const double q[4], const Vec3f& v) {
    const Vec3f u{static_cast<float>(q[1]), static_cast<float>(q[2]), static_cast<float>(q[3])};
    const float w = static_cast<float>(q[0]);
    return u * (2.0f * u.dot(v)) + v * (w * w - u.dot(u)) + u.cross(v) * (2.0f * w);
}

TEST(ScaleExport, UsdWritesTheBentMeshAndATiltedInstancer) {
    Scaled sc(uvSkin(5.0f), 1.0);
    sc.params().set("lift", 12.0);  // the frames tilt off the skin: the instancer must too
    const StrandsPtr strands = sc.eval();
    const auto mesh = buildScaleMesh(*strands, sc.skin);
    ASSERT_TRUE(mesh);
    ASSERT_GT(mesh->scales.size(), 100u);
    const fs::path path = fs::temp_directory_path() / "groomlab_scales.usda";
    UsdCurvesWriter::Options options;
    options.scales = UsdCurvesWriter::Options::Scales::Both;
    {
        UsdCurvesWriter w(path.u8string(), "dragon", options);
        w.writeFrame(1.0, *strands, sc.skin.get(), mesh.get());
        w.writeFrame(2.0, *strands, sc.skin.get(), mesh.get());
    }
    const std::string text = readText(path);
    if (!std::getenv("GROOMLAB_KEEP_TEST_FILES")) fs::remove(path);
    // The strands only lay the scales out.
    EXPECT_NE(text.find("token visibility = \"invisible\""), std::string::npos);
    EXPECT_EQ(text.find("def PointInstancer \"instances\""), std::string::npos);  // (no instances along strands)

    // The mesh: every vertex and triangle, exact, with the per-scale data.
    ASSERT_NE(text.find("def Mesh \"scales\""), std::string::npos);
    const std::vector<double> points = firstSample(text, "def Mesh \"scales\"", "point3f[] points");
    ASSERT_EQ(points.size(), 3 * mesh->vertexCount());
    for (std::size_t i = 0; i < mesh->vertexCount(); i += 97) {
        EXPECT_EQ(static_cast<float>(points[3 * i]), mesh->points[i].x);
        EXPECT_EQ(static_cast<float>(points[3 * i + 2]), mesh->points[i].z);
    }
    EXPECT_EQ(firstSample(text, "def Mesh \"scales\"", "int[] faceVertexIndices").size(), mesh->triangles.size());
    const std::vector<double> ids = firstSample(text, "def Mesh \"scales\"", "float[] primvars:scaleId");
    const std::vector<double> variants = firstSample(text, "def Mesh \"scales\"", "int[] primvars:scaleVariant");
    const std::vector<double> rootUvs = firstSample(text, "def Mesh \"scales\"", "float2[] primvars:scaleUV");
    ASSERT_EQ(ids.size(), mesh->triangleCount());
    ASSERT_EQ(rootUvs.size(), 2 * mesh->triangleCount());
    for (const ScaleMesh::Scale& s : mesh->scales) {
        EXPECT_EQ(static_cast<float>(ids[s.firstTriangle]), s.random);
        EXPECT_EQ(static_cast<float>(ids[s.firstTriangle + s.triangleCount - 1]), s.random);
        EXPECT_EQ(static_cast<int>(variants[s.firstTriangle]), s.variant);
        EXPECT_EQ(static_cast<float>(rootUvs[2 * s.firstTriangle + 1]), s.rootUv[1]);
    }
    EXPECT_EQ(firstSample(text, "def Mesh \"scales\"", "color3f[] primvars:scaleColor").size(), 3 * mesh->vertexCount());
    EXPECT_EQ(firstSample(text, "def Mesh \"scales\"", "color3f[] primvars:scaleRootColor").size(), 3 * mesh->triangleCount());

    // The instancer: each scale's frame, tilt and all, as position, orientation and scale.
    const std::string instancer = "def PointInstancer \"scaleInstances\"";
    ASSERT_NE(text.find(instancer), std::string::npos);
    EXPECT_NE(text.find("rel prototypes = [</dragon/scaleInstances/prototypes/groomlab_scale>, "
                        "</dragon/scaleInstances/prototypes/groomlab_scaleRound>]"),
              std::string::npos);
    EXPECT_NE(text.find("def Mesh \"groomlab_scale\""), std::string::npos);  // the plates' own geometry
    const std::vector<double> positions = firstSample(text, instancer, "point3f[] positions");
    const std::vector<double> orientations = firstSample(text, instancer, "quath[] orientations");
    const std::vector<double> scales = firstSample(text, instancer, "float3[] scales");
    const std::vector<double> protoIndices = firstSample(text, instancer, "int[] protoIndices");
    ASSERT_EQ(positions.size(), 3 * mesh->scales.size());
    ASSERT_EQ(orientations.size(), 4 * mesh->scales.size());
    std::size_t tilted = 0;
    for (std::size_t k = 0; k < mesh->scales.size(); ++k) {
        const ScaleMesh::Scale& s = mesh->scales[k];
        EXPECT_EQ(static_cast<std::uint32_t>(protoIndices[k]), s.prototype);
        EXPECT_NEAR(positions[3 * k], s.origin.x, 1e-5);
        const double* q = &orientations[4 * k];
        EXPECT_NEAR(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3], 1.0, 1e-4);
        const Vec3f axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        const Vec3f frame[3] = {s.x, s.y, s.z};
        for (int a = 0; a < 3; ++a) {
            const Vec3f placed = rotate(q, axes[a]) * static_cast<float>(scales[3 * k + static_cast<std::size_t>(a)]);
            EXPECT_LT((placed - frame[a]).length(), 1e-4f * (1.0f + frame[a].length())) << k << " axis " << a;
        }
        // Lifted off the skin: +Y climbs away from it (the skin's normal at the root).
        const Vec3f normal = sc.skin->closestPoint(s.origin).normal;
        tilted += s.y.normalized().dot(normal) > std::sin(10.0f * 3.14159265f / 180.0f);
    }
    EXPECT_GT(tilted, mesh->scales.size() * 9 / 10);
    EXPECT_EQ(firstSample(text, instancer, "float[] primvars:scaleId").size(), mesh->scales.size());
}

// The plate's top over (0, t) in its own space: a ray down onto its triangles.
float plateTop(const InstancePrototype& plate, float x, float y) {
    float best = -1.0f;
    for (std::size_t t = 0; t + 2 < plate.triangles.size(); t += 3) {
        const Vec3f& a = plate.points[plate.triangles[t]];
        const Vec3f& b = plate.points[plate.triangles[t + 1]];
        const Vec3f& c = plate.points[plate.triangles[t + 2]];
        const float d = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
        if (std::fabs(d) < 1e-12f) continue;
        const float w1 = ((x - a.x) * (c.y - a.y) - (c.x - a.x) * (y - a.y)) / d;
        const float w2 = ((b.x - a.x) * (y - a.y) - (x - a.x) * (b.y - a.y)) / d;
        if (w1 < 0.0f || w2 < 0.0f || w1 + w2 > 1.0f) continue;
        best = std::max(best, a.z * (1.0f - w1 - w2) + b.z * w1 + c.z * w2);
    }
    return best;
}

TEST(ScaleExport, BakedMapsHoldTheScalesHeightsAndNormals) {
    // Scales well apart (small, no tilt, no stack): each one's middle stands its plate's dome
    // height off the skin, which the height map must give there.
    Scaled sc(uvSkin(5.0f), 0.6);
    sc.params().set("prototype", std::string("groomlab:scale"));
    sc.params().set("size", 0.5);
    sc.params().set("lift", 0.0);
    sc.params().set("tile", 0.0);
    sc.params().set("stack", 0.0);
    sc.params().set("offset", 0.0);
    const StrandsPtr strands = sc.eval();
    const auto mesh = buildScaleMesh(*strands, sc.skin);
    ASSERT_TRUE(mesh);
    ASSERT_GT(mesh->scales.size(), 50u);
    ScaleMapOptions options;
    options.resolution = 1024;
    options.padding = 2;
    const ScaleMaps maps = bakeScaleMaps(*sc.skin, *mesh, options);
    EXPECT_FALSE(maps.udim);
    ASSERT_EQ(maps.tiles.size(), 1u);
    EXPECT_NEAR(maps.maxHeight, scalesMaxHeight(*sc.skin, *mesh), 1e-6f);
    EXPECT_GT(maps.covered, maps.texels / 50);  // scales here and there
    EXPECT_LT(maps.covered, maps.texels / 2);   // the skin between them
    const ScaleMaps::Tile& tile = maps.tiles[0];
    // Non-flat where scales are: the normal tips and the height rises; flat between them.
    std::size_t bumpy = 0, raised = 0;
    for (std::size_t i = 0; i < tile.height.size(); ++i) {
        raised += tile.height[i] > 0;
        bumpy += std::abs(tile.normal[3 * i] - 128) > 12 || std::abs(tile.normal[3 * i + 1] - 128) > 12;
        if (tile.color[4 * i + 3] == 0) EXPECT_EQ(tile.height[i], 0);  // the bare skin
    }
    EXPECT_GT(raised, maps.covered * 9 / 10);
    EXPECT_GT(bumpy, maps.covered / 4);

    // Each scale's middle: the height there is the plate's dome over its middle, scaled.
    const InstancePrototype& plate = *builtinPrototype("groomlab:scale");
    const int res = maps.resolution;
    std::size_t checked = 0;
    for (const ScaleMesh::Scale& s : mesh->scales) {
        const Vec3f middle = s.origin + s.y * 0.5f;
        const SurfacePoint under = sc.skin->closestPoint(middle);
        const ScalpMesh::UV uv = sc.skin->uvAt(under.root);
        if (uv[1] < 0.15f || uv[1] > 0.85f) continue;  // (the poles squeeze the uvs)
        const int x = static_cast<int>(uv[0] * static_cast<float>(res)), y = static_cast<int>((1.0f - uv[1]) * static_cast<float>(res));
        const float baked = tile.height[static_cast<std::size_t>(std::clamp(y, 0, res - 1) * res + std::clamp(x, 0, res - 1))] / 65535.0f * maps.maxHeight;
        const float expected = plateTop(plate, 0.0f, 0.5f) * s.z.length();
        EXPECT_NEAR(baked, expected, 0.12f * expected + 2e-3f) << "scale " << s.id;
        ++checked;
    }
    EXPECT_GT(checked, mesh->scales.size() / 2);

    // Written: an 8-bit normal and color map and a 16-bit height map, read back as written.
    const fs::path dir = fs::temp_directory_path() / "groomlab_scale_maps";
    fs::create_directories(dir);
    const std::vector<std::string> files = writeScaleMaps(maps, (dir / "dragon_").u8string());
    ASSERT_EQ(files.size(), 3u);
    EXPECT_EQ(files[1], scaleMapPath((dir / "dragon_").u8string(), "ScaleHeight", false));
    const TexturePtr height = loadTexture(files[1]);
    ASSERT_TRUE(height->image());
    EXPECT_EQ(height->image()->bits, 16);
    std::size_t mismatched = 0;
    for (int i = 0; i < res * res; i += 101) {
        const float read = height->image()->rgba[4 * static_cast<std::size_t>(i)];
        mismatched += std::fabs(read - tile.height[static_cast<std::size_t>(i)] / 65535.0f) > 1e-5f;
    }
    EXPECT_EQ(mismatched, 0u);
    fs::remove_all(dir);
}

TEST(ScaleExport, AUdimSkinBakesATileEach) {
    Scaled sc(uvSkin(5.0f, 2), 0.6);
    const auto mesh = buildScaleMesh(*sc.eval(), sc.skin);
    ASSERT_TRUE(mesh);
    ScaleMapOptions options;
    options.resolution = 128;
    const ScaleMaps maps = bakeScaleMaps(*sc.skin, *mesh, options);
    EXPECT_TRUE(maps.udim);
    ASSERT_EQ(maps.tiles.size(), 2u);
    EXPECT_EQ(maps.tiles[0].udim, 1001);
    EXPECT_EQ(maps.tiles[1].udim, 1002);
    for (const ScaleMaps::Tile& t : maps.tiles) {
        EXPECT_GT(std::count_if(t.height.begin(), t.height.end(), [](std::uint16_t h) { return h > 0; }), 100);
    }
    const fs::path dir = fs::temp_directory_path() / "groomlab_scale_udims";
    fs::create_directories(dir);
    const auto files = writeScaleMaps(maps, (dir / "body_").u8string());
    ASSERT_EQ(files.size(), 6u);
    EXPECT_EQ(fs::path(fs::u8path(files[3])).filename().u8string(), "body_ScaleNormal.1002.png");
    EXPECT_EQ(scaleMapPath("body_", "ScaleColor", true), "body_ScaleColor.<UDIM>.png");
    fs::remove_all(dir);
    // A skin without uvs has nothing to bake onto.
    EXPECT_THROW(bakeScaleMaps(*test::makeSphere(8, 16, 5.0f), *mesh, options), std::invalid_argument);
}

TEST(ScaleExport, GroomFilesCarryTheirOwnPrototypes) {
    // The graph names a scene mesh beside a built-in plate: only the mesh is stored.
    const std::string graph = R"({"nodes": [{"name": "scales", "type": "Scales",
        "params": {"prototype": "myPlate, groomlab:scale"}},
        {"name": "inner", "type": "Compound", "graph": {"nodes": [{"name": "i", "type": "Instance", "params": {"prototype": "leaf"}}]}}]})";
    EXPECT_EQ(graphMeshPrototypes(graph), (std::vector<std::string>{"leaf", "myPlate"}));
    GroomFile file;
    file.data.graphJson = graph;
    file.data.scalp = uvSkin(5.0f);
    InstancePrototype plate;
    plate.points = {{-0.5f, 0, 0}, {0.5f, 0, 0}, {0, 1, 0.1f}, {0, 0.5f, 0.2f}};
    plate.triangles = {0, 1, 3, 1, 2, 3, 2, 0, 3};
    plate.uvs = {{0, 0}, {1, 0}, {0.5f, 1}, {0.5f, 0.5f}};
    plate.found = true;
    file.prototypes["myPlate"] = plate;
    const GroomFile back = groomFileFromJson(groomFileToJson(file));
    ASSERT_EQ(back.prototypes.size(), 1u);
    const InstancePrototype& read = back.prototypes.at("myPlate");
    EXPECT_TRUE(read.found);
    EXPECT_EQ(read.points.size(), 4u);
    EXPECT_EQ(read.points[2].z, 0.1f);  // exact
    EXPECT_EQ(read.triangles, plate.triangles);
    EXPECT_EQ(read.uvs[2][0], 0.5f);
    ASSERT_EQ(read.normals.size(), 4u);  // made: none were stored
    EXPECT_GT(read.normals[3].z, 0.5f);
    EXPECT_NE(read.signature, 0u);
    // Scales of that mesh built from the file: its four points each, not a quad's.
    Scaled sc(file.data.scalp, 0.3);
    sc.params().set("prototype", std::string("myPlate"));
    const auto mesh = buildScaleMesh(*sc.eval(), sc.skin, [&](const std::string& name) -> const InstancePrototype* {
        const auto it = back.prototypes.find(name);
        return it == back.prototypes.end() ? nullptr : &it->second;
    });
    ASSERT_TRUE(mesh);
    EXPECT_EQ(mesh->vertexCount(), 4 * mesh->scales.size());
    EXPECT_FALSE(mesh->scales[0].quad);
    // A triangle naming a point the mesh lacks is refused.
    std::string broken = groomFileToJson(file);
    broken.replace(broken.find("\"triangles\":[0,1,3"), 18, "\"triangles\":[9,1,3");
    EXPECT_THROW(groomFileFromJson(broken), GroomFileError);
}
TEST(ScaleExport, ToMeshMakesTheScalesPlates) {
    // A ToMesh node on scaled strands: auto picks the scales; the mesh is their plates, bent as
    // buildScaleMesh bends them, in quads (the plates' triangle pairs), each face naming its scale.
    Scaled sc(uvSkin(5.0f), 0.6);
    const StrandsPtr strands = sc.eval();
    StrandMeshOptions o;
    EXPECT_EQ(resolveMeshMake(*strands, o), StrandMeshOptions::Make::Scales);
    const auto plates = buildScaleMesh(*strands, sc.skin);
    ASSERT_TRUE(plates);
    const StrandMesh mesh = buildStrandMesh(*strands, sc.skin.get(), o);
    ASSERT_EQ(mesh.mesh.points.size(), plates->points.size());
    for (std::size_t i = 0; i < plates->points.size(); ++i) EXPECT_LT((mesh.mesh.points[i] - plates->points[i]).length(), 1e-6f);
    // Every triangle in one face: two in each quad.
    std::size_t triangles = 0, quads = 0;
    for (int n : mesh.mesh.faceCounts) {
        triangles += static_cast<std::size_t>(n - 2);
        quads += n == 4 ? 1 : 0;
    }
    EXPECT_EQ(triangles, plates->triangleCount());
    EXPECT_GT(quads * 10, mesh.mesh.faceCount() * 9) << "the plates are quads, nearly all";
    EXPECT_TRUE(std::all_of(mesh.mesh.faceCounts.begin(), mesh.mesh.faceCounts.end(), [](int n) { return n == 3 || n == 4; }));
    EXPECT_EQ(mesh.mesh.featherCount, plates->scales.size());
    EXPECT_EQ(mesh.faceParts, std::vector<int>(mesh.mesh.faceCount(), static_cast<int>(MeshPart::Scales)));
    EXPECT_EQ(mesh.mesh.faceFeather.back(), static_cast<int>(plates->scales.size()) - 1);
    // The points alone (a host's per-frame update) are the same ones.
    ASSERT_EQ(strandMeshPointCount(*strands, o), plates->points.size());
    std::vector<Vec3f> points(plates->points.size());
    strandMeshPoints(*strands, sc.skin.get(), o, points.data());
    EXPECT_LT((points.back() - plates->points.back()).length(), 1e-6f);
    // Strands asked for: tubes round the strands instead.
    o.make = StrandMeshOptions::Make::Strands;
    const StrandMesh tubes = buildStrandMesh(*strands, sc.skin.get(), o);
    EXPECT_EQ(tubes.faceParts.front(), static_cast<int>(MeshPart::Strands));
    EXPECT_NE(tubes.mesh.points.size(), plates->points.size());
}

}  // namespace
}  // namespace groomlab
