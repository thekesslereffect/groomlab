// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include "groomlab/curve.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

#include <array>

#include <nlohmann/json.hpp>

#include "groomlab/export_view.h"
#include "groomlab/field.h"
#include "groomlab/scalp_paint.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/groom_file.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/operators.h"
#include "groomlab/scalp_maps.h"
#include "groomlab/subdivision.h"
#include "groomlab/texture.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

std::string tempPath(const char* name) { return (std::filesystem::temp_directory_path() / name).string(); }

// Binary PGM (grayscale), rows top to bottom; value(x, y) in 0-255.
template <class F>
void writePgm(const std::string& path, int w, int h, F value) {
    std::ofstream f(path, std::ios::binary);
    f << "P5\n" << w << " " << h << "\n255\n";
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) f.put(static_cast<char>(value(x, y)));
    }
}

// Unit plane with UVs (u = x, v = z) and a color set "mask": red 1 where x > 0.5.
std::shared_ptr<const ScalpMesh> mappedPlane(int cells = 16) {
    const auto plane = test::makePlane(cells);
    auto mesh = std::make_shared<ScalpMesh>(plane->points(), plane->triangles());
    std::vector<ScalpMesh::UV> uvs;
    std::vector<ScalpMesh::Color> mask;
    for (const auto& tri : mesh->triangles()) {
        for (const std::int32_t v : tri) {
            const Vec3f& p = mesh->points()[v];
            uvs.push_back({p.x, p.z});
            mask.push_back({p.x > 0.5f ? 1.0f : 0.0f, 0.0f, 0.0f, 1.0f});
        }
    }
    mesh->setCornerUVs(uvs);
    mesh->setCornerColors("mask", mask);
    return mesh;
}

struct MapGraph {
    Graph g;
    ExternalInputs ext;
    NodeId scalp, scatter;
    explicit MapGraph(std::shared_ptr<const ScalpMesh> mesh) {
        ext.set("scalp", MeshPtr(mesh), mesh->contentHash());
        scalp = g.addNode(op::kScalp);
        scatter = g.addNode(op::kScatter);
        g.connect(scalp, scatter, 0);
    }
    StrandsPtr eval(NodeId id) {
        EvalContext ctx;
        ctx.externals = &ext;
        return g.evaluateStrands(id, ctx);
    }
};

MapRef mapOf(MapRef::Source source, const std::string& name, float lo = 0.0f, float hi = 1.0f) {
    MapRef m;
    m.source = source;
    m.name = name;
    m.min = lo;
    m.max = hi;
    return m;
}

// ---------------------------------------------------------------------------

TEST(Texture, SamplesWithMayaUvOrientation) {
    const std::string path = tempPath("groomlab_tex_orient.pgm");
    writePgm(path, 2, 2, [](int x, int y) { return (y == 0 ? 200 : 0) + (x == 1 ? 50 : 0); });  // top row bright
    const TexturePtr t = loadTexture(path);
    EXPECT_EQ(t->width(), 2);
    EXPECT_NEAR(t->sample(0.25f, 0.75f)[0], 200.0f / 255.0f, 1e-5f);  // v near 1 = top row
    EXPECT_NEAR(t->sample(0.75f, 0.75f)[0], 250.0f / 255.0f, 1e-5f);
    EXPECT_NEAR(t->sample(0.25f, 0.25f)[0], 0.0f, 1e-5f);
    EXPECT_NEAR(t->sample(0.5f, 0.75f)[0], 225.0f / 255.0f, 1e-5f);  // bilinear
    std::remove(path.c_str());
}

TEST(Texture, CacheReloadsRewrittenFiles) {
    const std::string path = tempPath("groomlab_tex_cache.pgm");
    writePgm(path, 4, 4, [](int, int) { return 10; });
    const TexturePtr a = loadTexture(path);
    EXPECT_EQ(loadTexture(path), a);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    writePgm(path, 4, 4, [](int, int) { return 200; });
    const TexturePtr b = loadTexture(path);
    EXPECT_NE(b, a);
    EXPECT_NEAR(b->sample(0.5f, 0.5f)[0], 200.0f / 255.0f, 1e-5f);
    std::remove(path.c_str());
    EXPECT_THROW(loadTexture(path), TextureError);
}

TEST(Maps, ColorSetDensityScattersOnlyWherePainted) {
    MapGraph mg(mappedPlane());
    mg.g.node(mg.scatter).params().set("density", 2000.0);
    const std::size_t full = mg.eval(mg.scatter)->strandCount();

    mg.g.node(mg.scatter).params().set("density", mapOf(MapRef::Source::ColorSet, "mask", 0.0f, 2000.0f));
    const StrandsPtr painted = mg.eval(mg.scatter);
    EXPECT_NEAR(static_cast<double>(painted->strandCount()) / full, 0.5, 0.06);
    for (std::size_t s = 0; s < painted->strandCount(); ++s) EXPECT_GT(painted->strandPoints(s)[0].x, 0.44f);
}

TEST(Maps, APaintedMapOnTheScalpsVerticesReadsThere) {
    // The Graph Editor's "painted map" names any map from Groom Tools > Paint; one painted on the
    // scalp's vertices is read there, as the color set reference reads it (it once failed: "no
    // guide carries the layer").
    MapGraph mg(mappedPlane());
    mg.g.node(mg.scatter).params().set("density", mapOf(MapRef::Source::ColorSet, "mask", 0.0f, 2000.0f));
    const std::size_t asColorSet = mg.eval(mg.scatter)->strandCount();
    mg.g.node(mg.scatter).params().set("density", mapOf(MapRef::Source::Guide, "mask", 0.0f, 2000.0f));
    EXPECT_EQ(mg.eval(mg.scatter)->strandCount(), asColorSet);
}

TEST(Maps, APaintedMapThatIsATextureReadsItsImage) {
    // The same for a texture map: a painted map named reads the catalog's image, in a param and
    // in a Layers stack feeding Scatter (it failed: "a guide map here needs guides"). Repainting
    // the image re-evaluates.
    const std::string right = tempPath("groomlab_painted_right.pgm"), top = tempPath("groomlab_painted_top.pgm");
    writePgm(right, 32, 32, [](int x, int) { return x < 16 ? 0 : 255; });  // white where x > 0.5
    writePgm(top, 32, 32, [](int, int y) { return y < 16 ? 255 : 0; });    // white where z > 0.5 (rows top down)
    auto groomWith = [&](const std::string& densityParams, const std::string& extraNodes, const std::string& extraLinks) {
        GroomData d;
        d.scalp = mappedPlane();
        d.graphJson = R"j({"version": 1, "output": "out", "nodes": [
            {"name": "scalp", "type": "Scalp"},
            {"name": "scatter", "type": "Scatter", "params": )j" + densityParams + R"j(},)j" + extraNodes + R"j(
            {"name": "out", "type": "Output"}],
          "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},)j" + extraLinks + R"j(
            {"from": "scatter", "to": "out", "input": "strands"}]})j";
        d.maps["right"] = {"texture", right};
        d.maps["top"] = {"texture", top};
        return std::make_unique<Groom>(d);
    };

    const auto direct = groomWith(R"({"density": {"guide": "right", "min": 0.0, "max": 800.0}})", "", "");
    const StrandsPtr painted = direct->evaluate(1.0f, 0.0);
    ASSERT_TRUE(painted);
    std::size_t left = 0, rightSide = 0;
    for (std::size_t s = 0; s < painted->strandCount(); ++s) (painted->strandPoints(s)[0].x < 0.5f ? left : rightSide)++;
    EXPECT_NEAR(static_cast<double>(rightSide), 400.0, 80.0);
    EXPECT_LT(left, rightSide / 10);

    // Layers: the right half times the top half (multiply), the user's stack.
    const auto stacked = groomWith(R"({"density": 800.0}, "paramInputs": ["density"])",
                                   R"j({"name": "layers", "type": "Layers", "params": {"layers": 2,
                                        "layer1": {"guide": "right", "component": "r", "min": 0.0, "max": 1.0},
                                        "layer2": {"guide": "top", "component": "r", "min": 0.0, "max": 1.0},
                                        "layer2Mode": "multiply"}},)j",
                                   R"j({"from": "layers", "to": "scatter", "input": "density"},)j");
    const StrandsPtr quarter = stacked->evaluate(1.0f, 0.0);
    ASSERT_TRUE(quarter);
    EXPECT_NEAR(static_cast<double>(quarter->strandCount()), 200.0, 60.0);
    for (std::size_t s = 0; s < quarter->strandCount(); ++s) {
        const Vec3f root = quarter->strandPoints(s)[0];
        if (root.x < 0.1f || root.x > 0.9f || root.z < 0.1f || root.z > 0.9f) continue;  // the edges (filtering wraps)
        EXPECT_TRUE(root.x > 0.4f && root.z > 0.4f) << root.x << ", " << root.z;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    writePgm(top, 32, 32, [](int, int) { return 0; });  // the top layer painted black
    EXPECT_EQ(stacked->evaluate(1.0f, 0.0)->strandCount(), 0u);
    std::remove(right.c_str());
    std::remove(top.c_str());
}

TEST(Maps, TextureDensityFollowsTheImageAndReloads) {
    const std::string path = tempPath("groomlab_density.pgm");
    writePgm(path, 64, 64, [](int x, int) { return x * 255 / 63; });  // black left, white right
    MapGraph mg(mappedPlane());
    mg.g.node(mg.scatter).params().set("density", mapOf(MapRef::Source::Texture, path, 0.0f, 2000.0f));
    const StrandsPtr out = mg.eval(mg.scatter);
    std::size_t left = 0, right = 0;
    for (std::size_t s = 0; s < out->strandCount(); ++s) (out->strandPoints(s)[0].x < 0.5f ? left : right)++;
    EXPECT_NEAR(static_cast<double>(out->strandCount()), 1000.0, 120.0);  // mean density 1000
    EXPECT_GT(right, 2 * left);

    // Rewriting the texture invalidates the cached result.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    writePgm(path, 64, 64, [](int, int) { return 0; });
    EXPECT_EQ(mg.eval(mg.scatter)->strandCount(), 0u);
    std::remove(path.c_str());
}

TEST(Maps, MapTextureRewrittenReloads) {
    // A Map node reading an image, feeding Scatter's density: the same graph evaluated again
    // after the image is rewritten (a paint stroke) gives the new mask, not the cached strands.
    const std::string path = tempPath("groomlab_scalpmask.pgm");
    writePgm(path, 32, 32, [](int, int) { return 255; });
    GroomData d;
    d.scalp = mappedPlane();
    d.graphJson = R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 800.0}, "paramInputs": ["density"]},
        {"name": "mask", "type": "Map", "params": {"name": "mask"}},
        {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "mask", "to": "scatter", "input": "density"}, {"from": "scatter", "to": "out", "input": "strands"}]})j";
    d.maps["mask"] = {"texture", path};  // the host's catalog says where the map lives
    Groom groom(d);
    const std::size_t white = groom.evaluate(1.0f, 0.0)->strandCount();
    EXPECT_NEAR(static_cast<double>(white), 800.0, 120.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    writePgm(path, 32, 32, [](int x, int) { return x < 16 ? 0 : 255; });  // the left half painted black
    const StrandsPtr half = groom.evaluate(1.0f, 0.0);
    EXPECT_NEAR(static_cast<double>(half->strandCount()), white * 0.5, 100.0);
    std::size_t stray = 0;  // the image is sampled filtered: a few strands land near the black side
    for (std::size_t s = 0; s < half->strandCount(); ++s) stray += half->strandPoints(s)[0].x < 0.44f;
    EXPECT_LT(stray, half->strandCount() / 20);
    std::remove(path.c_str());
}

TEST(Maps, PaintingMoreDensityOnlyAddsStrands) {
    MapGraph mg(mappedPlane());
    mg.g.node(mg.scatter).params().set("density", mapOf(MapRef::Source::ColorSet, "mask", 200.0f, 1000.0f));
    const StrandsPtr sparse = mg.eval(mg.scatter);
    mg.g.node(mg.scatter).params().set("density", mapOf(MapRef::Source::ColorSet, "mask", 500.0f, 1000.0f));
    const StrandsPtr dense = mg.eval(mg.scatter);
    std::set<StrandId> denseIds(dense->ids().begin(), dense->ids().end());
    for (StrandId id : sparse->ids()) EXPECT_TRUE(denseIds.count(id));
    EXPECT_GT(dense->strandCount(), sparse->strandCount());
}

TEST(Maps, AttributeDrivesClumpStrength) {
    MapGraph mg(mappedPlane(8));
    mg.g.node(mg.scatter).params().set("density", 400.0);
    auto centers = std::make_shared<GuideSet>();
    centers->add(1, test::straightCurve({0.5f, 0, 0.5f}, {0.4f, 1, 0}, 8));
    mg.ext.set("centers", GuidesPtr(centers), 3);
    const NodeId src = mg.g.addNode(op::kGuides);
    mg.g.node(src).params().set("input", std::string("centers"));
    const NodeId attr = mg.g.addNode(op::kAttribute);
    mg.g.node(attr).params().set("name", std::string("pull"));
    mg.g.connect(mg.scatter, attr, 0);
    const NodeId clump = mg.g.addNode(op::kClump);
    mg.g.connect(attr, clump, 0);
    mg.g.connect(src, clump, 1);
    mg.g.node(clump).params().set("strength", mapOf(MapRef::Source::Attribute, "pull"));
    mg.g.node(clump).params().set("centersFrom", std::string("inputCurves"));  // onto the center curve itself

    const StrandsPtr before = mg.eval(attr);
    const StrandsPtr out = mg.eval(clump);
    const auto& pull = *out->strandChannel("pull");
    const std::vector<Vec3f>& center = centers->guides()[0].points;  // straight, longer than the strands
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const std::size_t n = out->strandPointCount(s);
        const Vec3f tip0 = before->strandPoints(s)[n - 1];
        // The tip is pulled toward the center's point as far along it as the strand is long.
        const float along = std::min(arcLength(before->strandPoints(s), n), arcLength(center));
        const Vec3f centerAt = center.front() + (center.back() - center.front()).normalized() * along;
        const Vec3f expected = tip0 + (centerAt - tip0) * pull[s];  // tip pull = strength * profile(1)
        EXPECT_NEAR((out->strandPoints(s)[n - 1] - expected).length(), 0.0f, 1e-4f);
    }
}

TEST(Maps, ColorSetDrivesWidth) {
    MapGraph mg(mappedPlane(8));
    mg.g.node(mg.scatter).params().set("density", 300.0);
    const NodeId width = mg.g.addNode(op::kWidth);
    mg.g.connect(mg.scatter, width, 0);
    mg.g.node(width).params().set("width", mapOf(MapRef::Source::ColorSet, "mask", 0.01f, 0.05f));
    const StrandsPtr out = mg.eval(width);
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const float x = out->strandPoints(s)[0].x;
        const float w = out->widths()[out->strandBegin(s)];
        // Painted per vertex: the mask blends across the cells between x = 0.5 and 0.625.
        if (x < 0.49f) EXPECT_NEAR(w, 0.01f, 1e-5f);
        if (x > 0.63f) EXPECT_NEAR(w, 0.05f, 1e-5f);
    }
}

TEST(Maps, AnAttributeMapReadsAPointChannelPerPoint) {
    // A point channel of the name is read per point (before any strand channel of it).
    StrandSet s;
    s.addStrand(1, {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}});
    s.addStrand(2, {{1, 0, 0}, {1, 1, 0}, {1, 3, 0}});
    s.addStrandChannel("band", 0.5f);
    ParamSet strandOnly;
    strandOnly.set("x", mapOf(MapRef::Source::Attribute, "band", 0.0f, 2.0f));
    const ScalarField perStrand(strandOnly, "x", nullptr);
    perStrand.validate(s);
    EXPECT_FALSE(perStrand.perPoint());
    EXPECT_FLOAT_EQ(perStrand.atPoint(s, 1, 2), 1.0f);  // the strand's value at every point

    std::vector<float>& band = s.addPointChannel("band", 0.0f);
    for (std::size_t i = 0; i < band.size(); ++i) band[i] = static_cast<float>(i) * 0.1f;
    const ScalarField perPoint(strandOnly, "x", nullptr);
    perPoint.validate(s);
    EXPECT_TRUE(perPoint.perPoint());
    EXPECT_FLOAT_EQ(perPoint.atPoint(s, 1, 2), 2.0f * 0.5f);  // point 5 of the set
    EXPECT_FLOAT_EQ(perPoint.atPoint(s, 0, 1), 2.0f * 0.1f);
    EXPECT_FLOAT_EQ(perPoint.at(s, 1), 2.0f * 0.3f);  // per strand: the root's value

    // Through a graph: HairColor writes colorR per point (root 0 to tip 1 here),
    // and Width reads it per point through an attribute map.
    MapGraph mg(mappedPlane(4));
    mg.g.node(mg.scatter).params().set("density", 100.0);
    const NodeId color = mg.g.addNode(op::kHairColor);
    mg.g.connect(mg.scatter, color, 0);
    mg.g.node(color).params().set("melanin", 0.0);
    mg.g.node(color).params().set("melaninRandom", 0.0);
    mg.g.node(color).params().set("dye", Vec3f(0.0f, 0.0f, 0.0f));
    mg.g.node(color).params().set("tipColor", Vec3f(1.0f, 1.0f, 1.0f));
    mg.g.node(color).params().set("tipStrength", 1.0);
    mg.g.node(color).params().set("valueRandom", 0.0);
    const NodeId width = mg.g.addNode(op::kWidth);
    mg.g.connect(color, width, 0);
    mg.g.node(width).params().set("width", mapOf(MapRef::Source::Attribute, "colorR", 0.0f, 0.1f));
    mg.g.node(width).params().set("profile", Ramp::constant(1.0f));
    const StrandsPtr out = mg.eval(width);
    ASSERT_GT(out->strandCount(), 0u);
    for (std::size_t i = 0; i < out->strandCount(); ++i) {
        const std::size_t n = out->strandPointCount(i), begin = out->strandBegin(i);
        EXPECT_NEAR(out->widths()[begin], 0.0f, 1e-6f);
        EXPECT_NEAR(out->widths()[begin + n - 1], 0.1f, 1e-6f);
        EXPECT_NEAR(out->widths()[begin + n / 2], 0.1f * static_cast<float>(n / 2) / static_cast<float>(n - 1), 1e-5f);
    }
}

TEST(Maps, ErrorsNameTheProblem) {
    auto noUvs = test::makePlane(4);
    MapGraph plain(noUvs);
    plain.g.node(plain.scatter).params().set("density", mapOf(MapRef::Source::Texture, tempPath("x.pgm")));
    EXPECT_THROW(plain.eval(plain.scatter), GraphError);  // no UVs

    MapGraph mg(mappedPlane(4));
    mg.g.node(mg.scatter).params().set("density", mapOf(MapRef::Source::Texture, tempPath("groomlab_missing.pgm")));
    EXPECT_THROW(mg.eval(mg.scatter), GraphError);  // missing file
    mg.g.node(mg.scatter).params().set("density", mapOf(MapRef::Source::ColorSet, "nope"));
    EXPECT_THROW(mg.eval(mg.scatter), GraphError);  // missing color set
    mg.g.node(mg.scatter).params().set("density", mapOf(MapRef::Source::Attribute, "a"));
    EXPECT_THROW(mg.eval(mg.scatter), GraphError);  // no strands yet

    mg.g.node(mg.scatter).params().set("density", 100.0);
    const NodeId noise = mg.g.addNode(op::kNoise);
    mg.g.connect(mg.scatter, noise, 0);
    mg.g.node(noise).params().set("amplitude", mapOf(MapRef::Source::Attribute, "missing"));
    EXPECT_THROW(mg.eval(noise), GraphError);  // channel not written upstream
}

TEST(Maps, JsonRoundTrip) {
    GraphDocument doc = loadGraphJson(R"({
        "nodes": [{"name": "s", "type": "Scatter",
                   "params": {"density": {"colorSet": "dens", "component": "luminance", "min": 10, "max": 900}}},
                  {"name": "o", "type": "Output"}],
        "connections": [{"from": "s", "to": "o", "input": "strands"}], "output": "o"})");
    const MapRef& m = std::get<MapRef>(doc.graph.node(doc.node("s")).params().get("density"));
    EXPECT_EQ(m.source, MapRef::Source::ColorSet);
    EXPECT_EQ(m.name, "dens");
    EXPECT_EQ(m.component, MapRef::Component::Luminance);
    EXPECT_FLOAT_EQ(m.max, 900.0f);
    const GraphDocument again = loadGraphJson(saveGraphJson(doc));
    EXPECT_EQ(std::get<MapRef>(again.graph.node(again.node("s")).params().get("density")), m);

    EXPECT_THROW(loadGraphJson(R"({"nodes": [{"name": "s", "type": "Scatter",
        "params": {"density": {"texture": "a.png", "colorSet": "b"}}}], "output": "s"})"), GraphError);
    EXPECT_THROW(loadGraphJson(R"({"nodes": [{"name": "s", "type": "Scatter",
        "params": {"density": {"texture": "a.png", "component": "q"}}}], "output": "s"})"), GraphError);
}

// ---- Color painted maps -------------------------------------------------------

// An RGBA PNG of `w` x `h` (rows top to bottom) from color(x, y) in 0-255.
template <class F>
void writeRgba(const std::string& path, int w, int h, F color) {
    std::vector<std::uint8_t> px;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::array<int, 4> c = color(x, y);
            for (const int v : c) px.push_back(static_cast<std::uint8_t>(v));
        }
    }
    writePng(path, w, h, 4, px);
}

// A graph whose Scatter's density a Map node (with `mapParams`) drives, from its output `output`
// (empty: the link names none, so the first, luminance).
std::string densityFromMap(const std::string& mapParams, const std::string& output = {}) {
    const std::string port = output.empty() ? std::string() : R"(, "output": ")" + output + "\"";
    return R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 800.0}, "paramInputs": ["density"]},
        {"name": "mask", "type": "Map", "params": )j" + mapParams + R"j(},
        {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "mask", "to": "scatter", "input": "density")j" + port + R"j(},
        {"from": "scatter", "to": "out", "input": "strands"}]})j";
}

// Strands on the left (x < 0.5) and right halves of the plane.
std::pair<std::size_t, std::size_t> halves(const StrandSet& s) {
    std::size_t left = 0, right = 0;
    for (std::size_t i = 0; i < s.strandCount(); ++i) (s.strandPoints(i)[0].x < 0.5f ? left : right)++;
    return {left, right};
}

TEST(Maps, MapNodeOutputsEachChannelOfAColorTexture) {
    // Masks packed in one color texture: r on the right half, g on the left, b nowhere, a all
    // over. Each output of the Map node gives its channel; luminance (the first output, what a
    // link naming none reads) the mix as before.
    const std::string path = tempPath("groomlab_packed.png");
    writeRgba(path, 32, 32, [](int x, int) { return std::array<int, 4>{x >= 16 ? 255 : 0, x < 16 ? 255 : 0, 0, 255}; });
    auto run = [&](const std::string& channel) {
        GroomData d;
        d.scalp = mappedPlane();
        d.graphJson = densityFromMap(R"({"name": "packed"})", channel);
        d.maps["packed"] = {"texture", path};
        Groom groom(d);
        return halves(*groom.evaluate(1.0f, 0.0));
    };
    const auto red = run("r"), green = run("g"), blue = run("b"), alpha = run("a"), gray = run("");
    EXPECT_LT(red.first, red.second / 10);
    EXPECT_LT(green.second, green.first / 10);
    EXPECT_EQ(blue.first + blue.second, 0u);
    EXPECT_NEAR(static_cast<double>(alpha.first + alpha.second), 800.0, 120.0);
    // Luminance: 0.7152 on the left (green), 0.2126 on the right (red).
    EXPECT_GT(gray.first, gray.second * 2);
    EXPECT_EQ(run("luminance"), gray);
    std::remove(path.c_str());
}

TEST(Maps, AMapNodeDrivesTheDensityOnARefinedScalpWhereItIsPainted) {
    // A checker painted as a texture, through a Map node into Scatter's density, on a scalp
    // refined twice: Scatter places its roots on the cage, and the Map node reads them on the
    // refined scalp, so each root goes there first (read as the refined scalp's own face, a
    // cage root sampled the image somewhere else: strands in the black squares).
    const std::string path = tempPath("groomlab_checker.png");
    auto white = [](float u, float v) { return (static_cast<int>(u * 4.0f) + static_cast<int>(v * 4.0f)) % 2 == 0; };
    writeRgba(path, 64, 64, [&](int x, int y) {
        const int c = white((x + 0.5f) / 64.0f, 1.0f - (y + 0.5f) / 64.0f) ? 255 : 0;  // (rows from the top: v up)
        return std::array<int, 4>{c, c, c, 255};
    });
    const MeshPtr refined = subdivideScalp(mappedPlane(8), 2);
    ASSERT_EQ(refined->subdivisionLevel(), 2);
    GroomData d;
    d.scalp = refined;
    d.graphJson = densityFromMap(R"({"name": "checker"})");
    d.maps["checker"] = {"texture", path};
    Groom groom(d);
    const StrandsPtr out = groom.evaluate(1.0f, 0.0);
    ASSERT_GT(out->strandCount(), 200u);
    std::size_t misplaced = 0;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const Vec3f root = out->strandPoints(s)[0];
        // (Within two texels of a square's edge, the filtered image, either side will do.)
        const float u = root.x * 4.0f, v = root.z * 4.0f;
        const bool edge = std::abs(u - std::round(u)) < 0.13f || std::abs(v - std::round(v)) < 0.13f;
        if (!edge && !white(root.x, root.z)) ++misplaced;
    }
    EXPECT_EQ(misplaced, 0u) << "of " << out->strandCount();
    std::remove(path.c_str());
}

TEST(Maps, MapNodeOutputsEachChannelOfAColorVertexMap) {
    // A color vertex map as the host lays it on the scalp: a color set of the map's name with
    // the real r g b a (cornerColorsFromVertexMap). b is 1 on the right half only.
    const auto base = mappedPlane();
    auto mesh = std::make_shared<ScalpMesh>(*base);
    std::vector<float> values;
    for (const Vec3f& p : mesh->points()) values.insert(values.end(), {0.2f, 0.0f, p.x > 0.5f ? 1.0f : 0.0f, 1.0f});
    mesh->setCornerColors("tint", cornerColorsFromVertexMap(values, *mesh, kColorMap));
    auto run = [&](const std::string& channel) {
        GroomData d;
        d.scalp = mesh;
        d.graphJson = densityFromMap(R"({"name": "tint"})", channel);
        d.maps["tint"] = {"vertices", ""};
        Groom groom(d);
        return halves(*groom.evaluate(1.0f, 0.0));
    };
    const auto blue = run("b"), red = run("r");
    EXPECT_LT(blue.first, blue.second / 8);
    EXPECT_NEAR(static_cast<double>(red.first + red.second), 800.0 * 0.2, 60.0);  // r = 0.2 everywhere
    EXPECT_NEAR(static_cast<double>(red.first) / std::max<std::size_t>(red.second, 1), 1.0, 0.5);
}

TEST(Maps, MapNodeOutputsAreTheChannelsAtEachRoot) {
    // One Map node on a packed color texture (r = 1 on the right half, g on the left, b 0.4, a
    // 0.8 everywhere) and on a color vertex map with the same colors: each output read at the
    // roots is that channel there, luminance the mix; one evaluation makes all five.
    const std::string path = tempPath("groomlab_map_outputs.png");
    writeRgba(path, 32, 32, [](int x, int) { return std::array<int, 4>{x >= 16 ? 255 : 0, x < 16 ? 255 : 0, 102, 204}; });
    const auto base = mappedPlane();
    auto mesh = std::make_shared<ScalpMesh>(*base);
    std::vector<float> values;
    for (const Vec3f& p : mesh->points()) {
        values.insert(values.end(), {p.x > 0.5f ? 1.0f : 0.0f, p.x > 0.5f ? 0.0f : 1.0f, 0.4f, 0.8f});
    }
    mesh->setCornerColors("packed_v", cornerColorsFromVertexMap(values, *mesh, kColorMap));
    for (const auto& [name, kind] : {std::pair<std::string, std::string>{"packed", "texture"}, {"packed_v", "vertices"}}) {
        SCOPED_TRACE(kind);
        MapGraph mg(mesh);
        mg.g.node(mg.scatter).params().set("density", 400.0);
        setMapExternals(mg.ext, {{"packed", {"texture", path}}, {"packed_v", {"vertices", ""}}});
        const NodeId map = mg.g.addNode(op::kMap);
        mg.g.node(map).params().set("name", name);
        const auto outs = mg.g.outputSpecs(map);
        ASSERT_EQ(outs.size(), 6u);
        EXPECT_EQ(outs[0].name, "luminance");
        EXPECT_EQ(outs[4].name, "a");
        EXPECT_EQ(outs[5].name, "rgb");
        EvalContext ctx;
        ctx.externals = &mg.ext;
        const StrandsPtr strands = mg.g.evaluateStrands(mg.scatter, ctx);
        ASSERT_TRUE(strands && strands->strandCount() > 50);
        const std::size_t before = mg.g.evaluationCount();
        const auto fields = mg.g.evaluateOutputs(map, ctx);
        EXPECT_EQ(mg.g.evaluationCount(), before + 1);  // all six from one run
        EXPECT_EQ(valueAs<FieldPtr>(mg.g.evaluateOutput(map, 2, ctx)), valueAs<FieldPtr>(fields[2]));  // cached
        EXPECT_EQ(mg.g.evaluationCount(), before + 1);
        EXPECT_EQ(valueAs<FieldPtr>(mg.g.evaluate(map, ctx)), valueAs<FieldPtr>(fields[0]));  // the primary
        EXPECT_THROW(mg.g.evaluateOutput(map, 6, ctx), GraphError);
        const FieldPtr rgb = valueAs<FieldPtr>(fields[5]);
        ASSERT_TRUE(rgb && rgb->isColor());
        EXPECT_FALSE(valueAs<FieldPtr>(fields[1])->isColor());
        std::size_t checked = 0;
        for (std::size_t s = 0; s < strands->strandCount(); ++s) {
            const float x = strands->strandPoints(s)[0].x;
            if (std::abs(x - 0.5f) < 0.15f || x < 0.1f || x > 0.9f) continue;  // off the edges (filtering, wrapping)
            const float r = x > 0.5f ? 1.0f : 0.0f, g = 1.0f - r;
            const float at[5] = {0.2126f * r + 0.7152f * g + 0.0722f * 0.4f, r, g, 0.4f, 0.8f};
            for (std::size_t o = 0; o < 5; ++o) {
                EXPECT_NEAR(valueAs<FieldPtr>(fields[o])->at(*strands, s), at[o], 0.02f) << outs[o].name << " at x " << x;
            }
            // rgb: the color for a color param, the luminance for a number.
            const std::array<float, 3> color = rgb->colorAt(*strands, s);
            for (int k = 0; k < 3; ++k) EXPECT_NEAR(color[k], at[k + 1], 0.02f) << "rgb at x " << x;
            EXPECT_NEAR(rgb->at(*strands, s), at[0], 0.02f);
            ++checked;
        }
        EXPECT_GT(checked, 20u);
    }
    std::remove(path.c_str());
}

TEST(Maps, OneMapNodeFeedsTwoParamsFromTwoChannels) {
    // The point of the outputs: one Map node feeds Scatter's density from r and Length's length
    // from g (a packed map), no duplicate nodes; it runs once. Saved, the links keep their
    // outputs ("output" written only when not the first); the old `channel` param is ignored
    // (luminance until relinked).
    const std::string path = tempPath("groomlab_map_two.png");
    writeRgba(path, 32, 32, [](int x, int) { return std::array<int, 4>{x >= 16 ? 255 : 0, 64, 0, 255}; });
    const std::string graph = R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 800.0}, "paramInputs": ["density"]},
        {"name": "length", "type": "Length", "paramInputs": ["keep"]},
        {"name": "mask", "type": "Map", "params": {"name": "packed"}},
        {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "mask", "to": "scatter", "input": "density", "output": "r"},
        {"from": "scatter", "to": "length", "input": "strands"},
        {"from": "mask", "to": "length", "input": "keep", "output": "g"},
        {"from": "length", "to": "out", "input": "strands"}]})j";
    GroomData d;
    d.scalp = mappedPlane();
    d.graphJson = graph;
    d.maps["packed"] = {"texture", path};
    Groom groom(d);
    const StrandsPtr strands = groom.evaluate(1.0f, 0.0);
    ASSERT_TRUE(strands);
    const auto [left, right] = halves(*strands);
    EXPECT_LT(left, right / 10);  // density from r
    // Length from g (0.25 everywhere: the fraction kept), against luminance (0.39 on the right).
    const Vec3f* pts = strands->strandPoints(0);
    const std::size_t n = strands->strandPointCount(0);
    float length = 0.0f;
    for (std::size_t i = 1; i < n; ++i) length += (pts[i] - pts[i - 1]).length();
    GroomData plain = d;
    plain.graphJson = graph;
    const std::string noG = R"(, "output": "g")";
    plain.graphJson.erase(plain.graphJson.find(noG), noG.size());  // length from luminance instead
    const StrandsPtr lum = Groom(plain).evaluate(1.0f, 0.0);
    ASSERT_TRUE(lum && lum->strandCount() == strands->strandCount());
    const Vec3f* lp = lum->strandPoints(0);
    float lumLength = 0.0f;
    for (std::size_t i = 1; i < lum->strandPointCount(0); ++i) lumLength += (lp[i] - lp[i - 1]).length();
    EXPECT_NEAR(length / lumLength, (64.0f / 255.0f) / (0.2126f + 0.7152f * 64.0f / 255.0f), 0.03f);

    GraphDocument doc = loadGraphJson(graph);
    EvalContext ctx;
    ExternalInputs ext;
    ext.set("scalp", MeshPtr(d.scalp), d.scalp->contentHash());
    setMapExternals(ext, d.maps);
    ctx.externals = &ext;
    doc.graph.evaluate(doc.output, ctx);
    EXPECT_EQ(doc.graph.evaluationCount(), 5u);  // scalp, the Map node once, scatter, length, out
    const std::string saved = saveGraphJson(doc);
    EXPECT_NE(saved.find(R"("output": "r")"), std::string::npos);
    EXPECT_NE(saved.find(R"("output": "g")"), std::string::npos);
    EXPECT_EQ(saveGraphJson(loadGraphJson(saved)), saved);

    // A graph from before the outputs: `channel` is an unknown param now, and the link reads luminance.
    std::string old = graph;
    old.replace(old.find(R"({"name": "packed"})"), 18, R"({"name": "packed", "channel": "r"})");
    for (const char* port : {R"(, "output": "r")", R"(, "output": "g")"}) old.erase(old.find(port), std::string(port).size());
    GraphDocument oldDoc = loadGraphJson(old);
    std::size_t output = 9;
    NodeId src = 0;
    const std::size_t density = oldDoc.graph.inputSpecs(oldDoc.node("scatter")).size() - 1;  // the exposed param's slot
    ASSERT_TRUE(oldDoc.graph.inputSource(oldDoc.node("scatter"), density, &src, &output));
    EXPECT_EQ(output, 0u);
    GroomData before = d;
    before.graphJson = old;
    const auto oldHalves = halves(*Groom(before).evaluate(1.0f, 0.0));
    EXPECT_GT(oldHalves.first, oldHalves.second / 4);  // luminance: strands on the left too
    std::remove(path.c_str());
}

TEST(HairColor, APaintedMapColorsTheHairAndFollowsItsFile) {
    // HairColor.map names a painted map: a color texture map's color at each root replaces the
    // pigment's (or multiplies it); rewriting the file (a stroke) re-evaluates the node.
    const std::string path = tempPath("groomlab_haircolor_map.png");
    writeRgba(path, 16, 16, [](int x, int) { return x < 8 ? std::array<int, 4>{255, 0, 0, 255} : std::array<int, 4>{0, 0, 255, 255}; });
    auto graphWith = [](const std::string& colorParams) {
        return R"j({"version": 1, "output": "out", "nodes": [
            {"name": "scalp", "type": "Scalp"},
            {"name": "scatter", "type": "Scatter", "params": {"density": 400.0}},
            {"name": "color", "type": "HairColor", "params": )j" + colorParams + R"j(},
            {"name": "out", "type": "Output"}],
          "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
            {"from": "scatter", "to": "color", "input": "strands"}, {"from": "color", "to": "out", "input": "strands"}]})j";
    };
    GroomData d;
    d.scalp = mappedPlane();
    d.graphJson = graphWith(R"({"mode": "color", "map": "dye"})");
    d.maps["dye"] = {"texture", path};
    Groom groom(d);
    auto colorsBySide = [](const StrandSet& s) {
        std::array<float, 3> left{}, right{};
        std::size_t nl = 0, nr = 0;
        const auto colors = strandColors(s);
        for (std::size_t i = 0; i < s.strandCount(); ++i) {
            const float x = s.strandPoints(i)[0].x;
            if (x > 0.1f && x < 0.3f) {  // away from the edges (textures repeat) and the middle
                for (int k = 0; k < 3; ++k) left[k] += colors[i][k];
                ++nl;
            } else if (x > 0.7f && x < 0.9f) {
                for (int k = 0; k < 3; ++k) right[k] += colors[i][k];
                ++nr;
            }
        }
        for (int k = 0; k < 3; ++k) {
            left[k] /= static_cast<float>(std::max<std::size_t>(nl, 1));
            right[k] /= static_cast<float>(std::max<std::size_t>(nr, 1));
        }
        return std::make_pair(left, right);
    };
    const auto [left, right] = colorsBySide(*groom.evaluate(1.0f, 0.0));
    EXPECT_NEAR(left[0], 1.0f, 0.02f);
    EXPECT_NEAR(left[2], 0.0f, 0.02f);
    EXPECT_NEAR(right[2], 1.0f, 0.02f);
    EXPECT_NEAR(right[0], 0.0f, 0.02f);

    // Rewritten green: the cached node reads the new file.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    writeRgba(path, 16, 16, [](int, int) { return std::array<int, 4>{0, 255, 0, 255}; });
    const auto [greenLeft, greenRight] = colorsBySide(*groom.evaluate(1.0f, 0.0));
    EXPECT_NEAR(greenLeft[1], 1.0f, 0.02f);
    EXPECT_NEAR(greenRight[1], 1.0f, 0.02f);
    EXPECT_NEAR(greenLeft[0], 0.0f, 0.02f);

    // Mode melanin: the map dyes the pigment (green keeps only the pigment's green).
    GroomData m = d;
    m.graphJson = graphWith(R"({"map": "dye", "melaninRandom": 0})");
    GroomData plain = d;
    plain.graphJson = graphWith(R"({"melaninRandom": 0})");
    const auto [mulLeft, mulRight] = colorsBySide(*Groom(m).evaluate(1.0f, 0.0));
    const auto [pigLeft, pigRight] = colorsBySide(*Groom(plain).evaluate(1.0f, 0.0));
    EXPECT_NEAR(mulLeft[0], 0.0f, 1e-4f);
    EXPECT_NEAR(mulLeft[1], pigLeft[1], 0.01f);
    EXPECT_GT(pigLeft[0], 0.01f);

    // A color vertex map (its color set on the scalp) colors it the same way.
    auto mesh = std::make_shared<ScalpMesh>(*mappedPlane());
    std::vector<float> values;
    for (const Vec3f& p : mesh->points()) values.insert(values.end(), {p.x > 0.5f ? 1.0f : 0.0f, 0.5f, 0.0f, 1.0f});
    mesh->setCornerColors("dye", cornerColorsFromVertexMap(values, *mesh, kColorMap));
    GroomData v = d;
    v.scalp = mesh;
    v.maps["dye"] = {"vertices", ""};
    const auto [vLeft, vRight] = colorsBySide(*Groom(v).evaluate(1.0f, 0.0));
    EXPECT_NEAR(vLeft[1], 0.5f, 0.02f);
    EXPECT_LT(vLeft[0], 0.1f);
    EXPECT_GT(vRight[0], 0.9f);

    // A missing map is an error on the node, not a silent color.
    GroomData missing = d;
    missing.maps.clear();
    missing.graphJson = graphWith(R"({"map": "nowhere"})");
    Groom broken(missing);
    EXPECT_THROW(broken.evaluate(1.0f, 0.0), GraphError);
    std::remove(path.c_str());
}

// The scalp with color set `name` set to `value` (r g b) on every corner.
std::shared_ptr<const ScalpMesh> recolored(const ScalpMesh& scalp, const std::string& name, float value) {
    auto out = std::make_shared<ScalpMesh>(scalp);
    out->setCornerColors(name, std::vector<ScalpMesh::Color>(scalp.triangleCount() * 3, {value, value, value, 1.0f}));
    return out;
}

TEST(Maps, PaintingAColorSetReevaluatesOnlyWhatReadsIt) {
    // The scalp's key is its surface: a color set's values key only the nodes reading that set
    // (the Scatter's density, the Length's length here), and each reads the scalp as painted now.
    const std::string graph = R"({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": {"colorSet": "mask", "min": 0, "max": 1500}}},
        {"name": "length", "type": "Transform", "params": {"scale": {"colorSet": "tip", "min": 0.2, "max": 1}}},
        {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "length", "input": "strands"}, {"from": "length", "to": "out", "input": "strands"}]})";
    GraphDocument doc = loadGraphJson(graph);
    ExternalInputs ext;
    EvalContext ctx;
    ctx.externals = &ext;
    auto run = [&](const MeshPtr& scalp) {
        setGroomExternals(ext, scalp, nullptr, nullptr);
        const std::size_t before = doc.graph.evaluationCount();
        const StrandsPtr strands = doc.graph.evaluateStrands(doc.output, ctx);
        // What a graph with nothing cached makes of the same scalp.
        GraphDocument fresh = loadGraphJson(graph);
        const StrandsPtr expected = fresh.graph.evaluateStrands(fresh.output, ctx);
        EXPECT_TRUE(strands && expected && strands->contentHash() == expected->contentHash());
        return doc.graph.evaluationCount() - before;
    };
    MeshPtr scalp = recolored(*recolored(*mappedPlane(), "mask", 0.5f), "tip", 0.5f);
    EXPECT_EQ(run(scalp), 4u);
    EXPECT_EQ(scalp->surfaceHash(), mappedPlane()->surfaceHash());  // color sets are not the surface
    scalp = recolored(*scalp, "tip", 0.9f);
    EXPECT_EQ(run(scalp), 2u);  // the Length and the output; the Scatter stays cached
    scalp = recolored(*scalp, "mask", 0.8f);
    EXPECT_EQ(run(scalp), 3u);  // the Scatter reads the new mask through the Scalp node
    scalp = recolored(*scalp, "other", 0.3f);
    EXPECT_EQ(run(scalp), 0u);  // a set nobody reads
    const std::uint64_t moved = [&] {
        std::vector<Vec3f> points = scalp->points();
        points[0].y += 0.01f;
        return scalp->withPoints(points).surfaceHash();
    }();
    EXPECT_NE(moved, scalp->surfaceHash());
}

TEST(Maps, MapNodeKeysOnTheGuidesOnlyWhenReadingThem) {
    // A Map node reading a vertex map stays cached through a guide stroke (a new hierarchy).
    const std::string graph = R"({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "scatter", "type": "Scatter", "params": {"density": 300}},
        {"name": "length", "type": "Transform", "params": {}, "paramInputs": ["scale"]},
        {"name": "mask", "type": "Map", "params": {"name": "tip"}}, {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "length", "input": "strands"}, {"from": "mask", "to": "length", "input": "scale"},
        {"from": "length", "to": "out", "input": "strands"}]})";
    GraphDocument doc = loadGraphJson(graph);
    const MeshPtr scalp = recolored(*mappedPlane(), "tip", 0.5f);
    ExternalInputs ext;
    setGroomExternals(ext, scalp, nullptr, nullptr);
    ext.set("hierarchy", HierarchyPtr(std::make_shared<GuideHierarchy>()), 1);
    EvalContext ctx;
    ctx.externals = &ext;
    ASSERT_TRUE(doc.graph.evaluateStrands(doc.output, ctx));
    const std::size_t runs = doc.graph.evaluationCount();
    ext.set("hierarchy", HierarchyPtr(std::make_shared<GuideHierarchy>()), 2);
    doc.graph.evaluateStrands(doc.output, ctx);
    EXPECT_EQ(doc.graph.evaluationCount(), runs);  // nothing read the guides
    setGroomExternals(ext, recolored(*scalp, "tip", 0.7f), nullptr, nullptr);
    doc.graph.evaluateStrands(doc.output, ctx);
    EXPECT_EQ(doc.graph.evaluationCount(), runs + 3);  // the Map node, the Length and the output
}

TEST(Maps, GroomKeepsItsScalpAndPosesWhenTheyDoNotChange) {
    const auto plane = mappedPlane();
    GuideHierarchy h;
    h.addGuide(h.addGroup("primary"), test::straightCurve({0.5f, 0.0f, 0.5f}, {0.0f, 0.3f, 0.0f}));
    h.evaluate(plane.get());
    GroomData d;
    d.scalp = plane;
    d.hierarchyJson = h.toJson();
    Groom groom(d);
    const HierarchyPtr first = groom.hierarchy();
    ASSERT_TRUE(first);
    groom.setScalp(std::make_shared<ScalpMesh>(*plane));  // the same content: the old mesh stays
    EXPECT_EQ(groom.data().scalp, plane);
    groom.setGuidePoses(nullptr);  // no poses before or after
    EXPECT_EQ(groom.hierarchy(), first);
    groom.setScalp(recolored(*plane, "tip", 0.2f));  // repainted, the same surface: the hierarchy stays
    EXPECT_NE(groom.data().scalp, plane);
    EXPECT_EQ(groom.hierarchy(), first);
    auto poses = std::make_shared<GuideSet>();
    poses->add(first->guides().front().id, test::straightCurve({0.5f, 0.0f, 0.5f}, {0.2f, 0.3f, 0.0f}));
    groom.setGuidePoses(poses);
    const HierarchyPtr posed = groom.hierarchy();
    EXPECT_NE(posed, first);
    groom.setGuidePoses(std::make_shared<GuideSet>(*poses));  // the same poses again
    EXPECT_EQ(groom.hierarchy(), posed);
}

TEST(Texture, UdimStampsFollowTheTilesOnDisk) {
    // One listing of the folder finds the tiles (any case of the name on Windows); every change
    // of a tile changes the stamp, and within one ScopedFileStamps the first answer holds.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "groomlab_udim_stamps";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string pattern = (dir / "map.<UDIM>.pgm").string();
    writePgm((dir / "map.1001.pgm").string(), 2, 2, [](int, int) { return 10; });
    const std::uint64_t one = fileStamp(pattern);
    EXPECT_NE(one, 0u);
    writePgm((dir / "map.1012.pgm").string(), 2, 2, [](int, int) { return 20; });
    writePgm((dir / "other.1003.pgm").string(), 2, 2, [](int, int) { return 20; });  // another set
    writePgm((dir / "map.1101.pgm").string(), 2, 2, [](int, int) { return 20; });   // past the grid
    const std::uint64_t two = fileStamp(pattern);
    EXPECT_NE(two, one);
    EXPECT_EQ(fileStamp((dir / "map.1001.pgm").string()), two);  // a tile names its set
    {
        const ScopedFileStamps scope;
        EXPECT_EQ(fileStamp(pattern), two);
        std::filesystem::remove(dir / "map.1012.pgm");
        EXPECT_EQ(fileStamp(pattern), two);  // remembered for the scope
    }
    EXPECT_EQ(fileStamp(pattern), one);
#ifdef _WIN32
    std::filesystem::rename(dir / "map.1001.pgm", dir / "MAP.1001.PGM");
    EXPECT_NE(fileStamp(pattern), 0u);  // Windows names ignore case
#endif
    std::filesystem::remove_all(dir);
    EXPECT_EQ(fileStamp(pattern), 0u);
}

TEST(Maps, OneNameRuleAndOneRenameForEveryHost) {
    // Maya's rule (scalp_maps.h): letters, digits and _, not starting with a digit.
    for (const char* good : {"density", "Tint_2", "_1st", "a"}) EXPECT_TRUE(validMapName(good)) << good;
    for (const char* bad : {"", "hair-mask", "tint.r", "2nd", "a b"}) {
        EXPECT_FALSE(validMapName(bad)) << bad;
        EXPECT_FALSE(mapNameError(bad).empty()) << bad;
        EXPECT_TRUE(validMapName(sanitizeMapName(bad))) << bad;
    }
    EXPECT_EQ(mapNameError("density"), "");
    EXPECT_EQ(sanitizeMapName("hair-mask"), "hair_mask");
    EXPECT_EQ(sanitizeMapName("2nd.pass"), "_2nd_pass");
    EXPECT_EQ(sanitizeMapName(""), "map");

    // A rename's readers: Map nodes (the default name is "mask"), map params, inside compounds.
    const std::string graph = R"({"version": 1, "output": "out", "nodes": [
        {"name": "m", "type": "Map", "params": {"name": "density"}},
        {"name": "dflt", "type": "Map"},
        {"name": "len", "type": "Length", "params": {"keep": {"guide": "density", "min": 0.0, "max": 1.0}, "pos": 3}},
        {"name": "grp", "type": "Compound", "params": {"definition": {"graph": {"version": 1, "output": "x", "nodes": [
            {"name": "x", "type": "Map", "params": {"name": "density"}}], "connections": []}}}},
        {"name": "out", "type": "Output"}], "connections": []})";
    bool changed = false;
    const nlohmann::json moved = nlohmann::json::parse(retargetMapReaders(graph, "density", "thickness", &changed));
    EXPECT_TRUE(changed);
    EXPECT_EQ(moved["nodes"][0]["params"]["name"], "thickness");
    EXPECT_FALSE(moved["nodes"][1].contains("params") && moved["nodes"][1]["params"].contains("name"));
    EXPECT_EQ(moved["nodes"][2]["params"]["keep"]["guide"], "thickness");
    EXPECT_EQ(moved["nodes"][2]["params"]["pos"], 3);  // the rest as written
    EXPECT_EQ(moved["nodes"][3]["params"]["definition"]["graph"]["nodes"][0]["params"]["name"], "thickness");
    const nlohmann::json mask = nlohmann::json::parse(retargetMapReaders(graph, "mask", "cover", &changed));
    EXPECT_EQ(mask["nodes"][1]["params"]["name"], "cover");
    EXPECT_EQ(retargetMapReaders(graph, "nothing", "else", &changed), graph);
    EXPECT_FALSE(changed);
    // HairColor's map, by name.
    const nlohmann::json colored = nlohmann::json::parse(retargetMapReaders(
        R"({"version": 1, "output": "c", "nodes": [{"name": "c", "type": "HairColor", "params": {"map": "density"}}]})", "density",
        "thickness", &changed));
    EXPECT_TRUE(changed);
    EXPECT_EQ(colored["nodes"][0]["params"]["map"], "thickness");
    // Interpolate's regionMap, by name.
    const nlohmann::json regioned = nlohmann::json::parse(retargetMapReaders(
        R"({"version": 1, "output": "i", "nodes": [{"name": "i", "type": "Interpolate", "params": {"regionMap": "density"}}]})",
        "density", "thickness", &changed));
    EXPECT_TRUE(changed);
    EXPECT_EQ(regioned["nodes"][0]["params"]["regionMap"], "thickness");

    // The file follows the map when it is named after it.
    EXPECT_EQ(renamedMapTexture("masks/hair/density.png", "density", "thickness"), "masks/hair/thickness.png");
    EXPECT_EQ(renamedMapTexture("masks/hair/density_2.png", "density", "thickness"), "masks/hair/thickness.png");
    EXPECT_EQ(renamedMapTexture("masks\\hair_density.png", "density", "thickness"), "masks/hair_thickness.png");
    EXPECT_EQ(renamedMapTexture("masks/hair/mydensity.png", "density", "thickness"), "");
    EXPECT_EQ(renamedMapTexture("masks/hair/other.png", "density", "thickness"), "");

    // A groom file an older app wrote: its invalid names sanitized on read, with their readers.
    auto scalp = std::make_shared<ScalpMesh>(*test::makePlane(2));
    scalp->setCornerColors("hair-mask", std::vector<ScalpMesh::Color>(scalp->triangleCount() * 3, {0.5f, 0.5f, 0.5f, 1.0f}));
    scalp->setCornerColors("hair_mask", std::vector<ScalpMesh::Color>(scalp->triangleCount() * 3, {0.2f, 0.2f, 0.2f, 1.0f}));
    GuideHierarchy h;
    const GroupId group = h.addGroup("primary");
    const GuideId guide = h.addGuide(group, test::straightCurve({0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}));
    h.setAttribute(guide, "2stiff", 0.25f);
    GroomFile file;
    file.data.scalp = scalp;
    file.data.hierarchyJson = h.toJson();
    file.data.maps["hair-mask"] = {"vertices", "", 1};
    file.data.maps["hair_mask"] = {"vertices", "", 1};
    file.data.graphJson = R"({"version": 1, "output": "out", "nodes": [{"name": "a", "type": "Map", "params": {"name": "hair-mask"}},
        {"name": "b", "type": "Map", "params": {"name": "2stiff"}}, {"name": "out", "type": "Output"}], "connections": []})";
    const GroomFile read = groomFileFromJson(groomFileToJson(file));
    EXPECT_EQ(read.data.maps.count("hair-mask"), 0u);
    EXPECT_EQ(read.data.maps.count("hair_mask"), 1u);  // the valid one keeps its name
    ASSERT_EQ(read.data.maps.count("hair_mask_2"), 1u);  // the sanitized one takes a free one
    EXPECT_TRUE(read.data.scalp->hasColorSet("hair_mask_2"));
    EXPECT_FALSE(read.data.scalp->hasColorSet("hair-mask"));
    EXPECT_FLOAT_EQ((*read.data.scalp->cornerColors("hair_mask_2"))[0][0], 0.5f);
    const std::vector<std::string> layers = GuideHierarchy::fromJson(read.data.hierarchyJson).attributeNames();
    EXPECT_EQ(layers, std::vector<std::string>{"_2stiff"});
    const nlohmann::json g = nlohmann::json::parse(read.data.graphJson);
    EXPECT_EQ(g["nodes"][0]["params"]["name"], "hair_mask_2");
    EXPECT_EQ(g["nodes"][1]["params"]["name"], "_2stiff");
    GroomFile valid;
    valid.data.maps["density"] = {"vertices", "", 1};
    EXPECT_TRUE(sanitizeMapNames(valid.data).empty());
}

}  // namespace
}  // namespace groomlab
