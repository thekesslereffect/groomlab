// SPDX-License-Identifier: Apache-2.0
// Cinematic look nodes: HairColor (melanin and RGB), Flyaway and Tangle.

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <thread>

#include "groomlab/cards.h"
#include "groomlab/curve.h"
#include "groomlab/export_view.h"
#include "groomlab/graph_io.h"
#include "groomlab/hair_color.h"
#include "groomlab/groom.h"
#include "groomlab/map_ref.h"
#include "groomlab/operators.h"
#include "groomlab/presets.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

// Scalp -> Scatter (-> Clump, picking its own centers) -> `op` -> Output on a plane; fur straight up.
StrandsPtr run(const std::string& opJson, bool clumped = false) {
    std::string nodes = R"j({"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 600.0}},
        {"name": "length", "type": "Length", "params": {"keep": 1.0}},
        {"name": "clump", "type": "Clump", "params": {"count": 20, "strength": 0.0}},
        {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 8}})j";
    if (!opJson.empty()) nodes += ", " + opJson;
    std::string links = R"j({"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "length", "input": "strands"},
        {"from": "length", "to": "clump", "input": "strands"})j";
    const std::string upstream = clumped ? "clump" : "length";
    if (opJson.empty()) {
        links += R"j(, {"from": ")j" + upstream + R"j(", "to": "out", "input": "strands"})j";
    } else {
        links += R"j(, {"from": ")j" + upstream + R"j(", "to": "op", "input": "strands"},
                   {"from": "op", "to": "out", "input": "strands"})j";
    }
    GroomData d;
    d.scalp = test::makePlane(8);
    d.graphJson = R"j({"version": 1, "output": "out", "nodes": [)j" + nodes + R"j(], "connections": [)j" + links + "]}";
    return Groom(d).evaluate(1.0f, 0.0);
}

TEST(HairColor, MelaninVariesPerStrandAndSharesPerClump) {
    const StrandsPtr s = run(R"j({"name": "op", "type": "HairColor",
        "params": {"melanin": 0.5, "melaninRandom": 0.2, "clumpShare": 1.0, "greys": 0.0}})j", true);
    const std::vector<float>* melanin = s->strandChannel("melanin");
    const std::vector<float>* clumps = s->strandChannel("clumpId");
    ASSERT_TRUE(melanin && clumps && s->strandChannel("melaninRedness"));
    // All the variation shared: one value per clump, different between clumps.
    std::map<int, float> perClump;
    for (std::size_t i = 0; i < s->strandCount(); ++i) {
        EXPECT_GE((*melanin)[i], 0.3f - 1e-5f);
        EXPECT_LE((*melanin)[i], 0.7f + 1e-5f);
        const int c = static_cast<int>((*clumps)[i]);
        if (perClump.count(c)) EXPECT_FLOAT_EQ(perClump[c], (*melanin)[i]);
        perClump[c] = (*melanin)[i];
    }
    ASSERT_GT(perClump.size(), 5u);
    float lo = 1.0f, hi = 0.0f;
    for (const auto& [c, m] : perClump) lo = std::min(lo, m), hi = std::max(hi, m);
    EXPECT_GT(hi - lo, 0.1f);

    // Greys: about the fraction asked for, nearly unpigmented.
    const StrandsPtr g = run(R"j({"name": "op", "type": "HairColor", "params": {"melanin": 0.6, "melaninRandom": 0.0, "greys": 0.25}})j");
    std::size_t greys = 0;
    for (const float m : *g->strandChannel("melanin")) greys += m < 0.1f ? 1 : 0;
    const float share = static_cast<float>(greys) / static_cast<float>(g->strandCount());
    EXPECT_NEAR(share, 0.25f, 0.06f);

    // The same strands as RGB: each one's melanin color, dyed (engines and exports read it).
    const StrandsPtr c = run(R"j({"name": "op", "type": "HairColor", "params": {"melanin": 0.5, "dye": [1.0, 0.5, 0.5]}})j");
    const std::vector<float>* m = c->strandChannel("melanin");
    const std::vector<float>* r = c->strandChannel("melaninRedness");
    ASSERT_TRUE(m && r && c->strandChannel("colorR") && c->strandChannel("colorG") && c->strandChannel("colorB"));
    for (std::size_t i = 0; i < c->strandCount(); i += 7) {
        const std::array<float, 3> dyed = melaninColor((*m)[i], (*r)[i], {1.0f, 0.5f, 0.5f});
        EXPECT_FLOAT_EQ((*c->strandChannel("colorR"))[i], dyed[0]);
        EXPECT_FLOAT_EQ((*c->strandChannel("colorG"))[i], dyed[1]);
        EXPECT_FLOAT_EQ((*c->strandChannel("colorB"))[i], dyed[2]);
        EXPECT_LT(dyed[1], melaninColor((*m)[i], (*r)[i])[1]);
    }
}

// ---------------------------------------------------------------------------
// HairColor as RGB: dye, a map (an image, a color set), randomness, tip color

std::string tempPath(const char* name) { return (std::filesystem::temp_directory_path() / name).generic_string(); }

// Binary PPM (RGB), rows top to bottom; rgb(x, y) in 0-255.
template <class F>
void writePpm(const std::string& path, int w, int h, F rgb) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::array<int, 3> c = rgb(x, y);
            for (const int v : c) f.put(static_cast<char>(v));
        }
    }
}

// Unit plane with UVs (u = x, v = z) and a color set "tint": green where x > 0.5, else blue.
std::shared_ptr<const ScalpMesh> tintedPlane(int cells = 8) {
    const auto plane = test::makePlane(cells);
    auto mesh = std::make_shared<ScalpMesh>(plane->points(), plane->triangles());
    std::vector<ScalpMesh::UV> uvs;
    std::vector<ScalpMesh::Color> tint;
    for (const auto& tri : mesh->triangles()) {
        for (const std::int32_t v : tri) {
            const Vec3f& p = mesh->points()[v];
            uvs.push_back({p.x, p.z});
            tint.push_back(p.x > 0.5f ? ScalpMesh::Color{0.0f, 1.0f, 0.0f, 1.0f} : ScalpMesh::Color{0.0f, 0.0f, 1.0f, 1.0f});
        }
    }
    mesh->setCornerUVs(uvs);
    mesh->setCornerColors("tint", tint);
    return mesh;
}

// Scalp -> Scatter -> Length (-> Clump) -> HairColor(params) -> Output, as a Groom (a live cache).
struct ColorGroom {
    GroomData d;
    std::unique_ptr<Groom> groom;
    explicit ColorGroom(std::shared_ptr<const ScalpMesh> scalp, const std::string& params, bool clumped = false) {
        const std::string nodes = R"j({"name": "scalp", "type": "Scalp"},
            {"name": "scatter", "type": "Scatter", "params": {"density": 400.0}},
            {"name": "length", "type": "Length", "params": {"keep": 1.0}},
            {"name": "clump", "type": "Clump", "params": {"count": 12, "strength": 0.0}},
            {"name": "op", "type": "HairColor", "params": )j" + params + R"j(},
            {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 4}})j";
        const std::string upstream = clumped ? "clump" : "length";
        const std::string links = R"j({"from": "scalp", "to": "scatter", "input": "scalp"},
            {"from": "scatter", "to": "length", "input": "strands"},
            {"from": "length", "to": "clump", "input": "strands"},
            {"from": ")j" + upstream + R"j(", "to": "op", "input": "strands"},
            {"from": "op", "to": "out", "input": "strands"})j";
        d.scalp = std::move(scalp);
        d.graphJson = R"j({"version": 1, "output": "out", "nodes": [)j" + nodes + R"j(], "connections": [)j" + links + "]}";
        groom = std::make_unique<Groom>(d);
    }
    StrandsPtr eval() { return groom->evaluate(1.0f, 0.0); }
};

std::array<float, 3> colorOf(const StrandSet& s, std::size_t i) {
    return {(*s.strandChannel("colorR"))[i], (*s.strandChannel("colorG"))[i], (*s.strandChannel("colorB"))[i]};
}

TEST(HairColorRgb, WritesTheBaseColorAndVariesItDeterministically) {
    ColorGroom flat(test::makePlane(8), R"j({"melanin": 0.0, "melaninRandom": 0.0, "dye": [0.5, 0.25, 0.1]})j");
    const StrandsPtr s = flat.eval();
    ASSERT_GT(s->strandCount(), 50u);
    ASSERT_TRUE(s->strandChannel("colorR") && s->strandChannel("colorG") && s->strandChannel("colorB"));
    EXPECT_TRUE(s->pointChannel("colorR") == nullptr);  // no tip blend: per strand only
    for (std::size_t i = 0; i < s->strandCount(); ++i) {
        const std::array<float, 3> c = colorOf(*s, i);
        EXPECT_FLOAT_EQ(c[0], 0.5f);
        EXPECT_FLOAT_EQ(c[1], 0.25f);
        EXPECT_FLOAT_EQ(c[2], 0.1f);
    }
    // Exporters read the channels as the strand color.
    const std::vector<std::array<float, 3>> exported = strandColors(*s);
    ASSERT_EQ(exported.size(), s->strandCount());
    EXPECT_FLOAT_EQ(exported[0][1], 0.25f);

    // Variation: different per strand, in range, the same for the same seed and different for another.
    const char* varied = R"j({"mode": "color", "color": [0.5, 0.25, 0.1], "hueRandom": 0.1, "saturationRandom": 0.3, "valueRandom": 0.5, "seed": 4})j";
    const StrandsPtr a = ColorGroom(test::makePlane(8), varied).eval();
    const StrandsPtr b = ColorGroom(test::makePlane(8), varied).eval();
    const StrandsPtr c = ColorGroom(test::makePlane(8), R"j({"mode": "color", "color": [0.5, 0.25, 0.1], "valueRandom": 0.5, "seed": 5})j").eval();
    ASSERT_EQ(a->strandCount(), b->strandCount());
    ASSERT_EQ(a->strandCount(), c->strandCount());
    float lo = 1.0f, hi = 0.0f;
    std::size_t differ = 0;
    for (std::size_t i = 0; i < a->strandCount(); ++i) {
        const std::array<float, 3> ca = colorOf(*a, i), cb = colorOf(*b, i), cc = colorOf(*c, i);
        EXPECT_EQ(ca, cb);
        if (ca != cc) ++differ;
        lo = std::min(lo, ca[0]);
        hi = std::max(hi, ca[0]);
        for (const float v : ca) {
            EXPECT_GE(v, 0.0f);
            EXPECT_LE(v, 1.0f);
        }
    }
    EXPECT_GT(hi - lo, 0.1f);
    EXPECT_GT(differ, a->strandCount() / 2);
}

TEST(HairColorRgb, ClumpVariationIsSharedPerClump) {
    ColorGroom g(test::makePlane(8), R"j({"mode": "color", "color": [0.5, 0.25, 0.1], "valueRandom": 0.5, "clumpShare": 1.0})j", true);
    const StrandsPtr s = g.eval();
    const std::vector<float>* clumps = s->strandChannel("clumpId");
    ASSERT_TRUE(clumps);
    std::map<int, std::array<float, 3>> perClump;
    for (std::size_t i = 0; i < s->strandCount(); ++i) {
        const int c = static_cast<int>((*clumps)[i]);
        const std::array<float, 3> color = colorOf(*s, i);
        if (perClump.count(c)) EXPECT_EQ(perClump[c], color);
        perClump[c] = color;
    }
    ASSERT_GT(perClump.size(), 5u);
    float lo = 1.0f, hi = 0.0f;
    for (const auto& [c, color] : perClump) lo = std::min(lo, color[0]), hi = std::max(hi, color[0]);
    EXPECT_GT(hi - lo, 0.05f);
}

TEST(HairColorRgb, AnImageAndAColorSetSampleTheScalpAtTheRoot) {
    const std::string path = tempPath("groomlab_strand_color.ppm");
    // Red on the left half (u < 0.5), white on the right.
    writePpm(path, 32, 32, [](int x, int) { return x < 16 ? std::array<int, 3>{255, 0, 0} : std::array<int, 3>{255, 255, 255}; });
    const std::string quoted = "\"" + path + "\"";  // generic_string: forward slashes, so no JSON escaping
    {
        ColorGroom g(tintedPlane(), R"j({"mode": "color", "map": )j" + quoted + R"j(})j");
        const StrandsPtr s = g.eval();
        ASSERT_GT(s->strandCount(), 50u);
        std::size_t left = 0, right = 0;
        for (std::size_t i = 0; i < s->strandCount(); ++i) {
            const float x = s->strandPoints(i)[0].x;
            if (std::abs(x - 0.5f) < 0.06f || x < 0.05f || x > 0.95f) continue;  // bilinear seams (the texture repeats)
            const std::array<float, 3> c = colorOf(*s, i);
            EXPECT_NEAR(c[0], 1.0f, 1e-5f);
            EXPECT_NEAR(c[1], x < 0.5f ? 0.0f : 1.0f, 1e-5f) << "x " << x;
            (x < 0.5f ? left : right)++;
        }
        EXPECT_GT(left, 10u);
        EXPECT_GT(right, 10u);

        // Rewriting the texture re-evaluates: all blue now.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        writePpm(path, 32, 32, [](int, int) { return std::array<int, 3>{0, 0, 255}; });
        const StrandsPtr again = g.eval();
        EXPECT_NEAR(colorOf(*again, 0)[2], 1.0f, 1e-5f);
        EXPECT_NEAR(colorOf(*again, 0)[0], 0.0f, 1e-5f);
    }
    // Mode melanin: the texture (blue) dyes the pigment's color.
    {
        ColorGroom g(tintedPlane(), R"j({"map": )j" + quoted + R"j(, "melanin": 0.0, "melaninRandom": 0.0, "dye": [1.0, 1.0, 0.5]})j");
        const StrandsPtr s = g.eval();
        EXPECT_NEAR(colorOf(*s, 0)[2], 0.5f, 1e-5f);
        EXPECT_NEAR(colorOf(*s, 0)[0], 0.0f, 1e-5f);
    }
    std::remove(path.c_str());

    // A color set, replacing the color: green right of x = 0.5, blue left.
    {
        ColorGroom g(tintedPlane(), R"j({"mode": "color", "map": "tint"})j");
        const StrandsPtr s = g.eval();
        std::size_t checked = 0;
        for (std::size_t i = 0; i < s->strandCount(); ++i) {
            const float x = s->strandPoints(i)[0].x;
            if (std::abs(x - 0.5f) < 0.13f) continue;  // interpolated across the middle cells
            const std::array<float, 3> c = colorOf(*s, i);
            EXPECT_NEAR(c[1], x > 0.5f ? 1.0f : 0.0f, 1e-4f) << "x " << x;
            EXPECT_NEAR(c[2], x > 0.5f ? 0.0f : 1.0f, 1e-4f) << "x " << x;
            ++checked;
        }
        EXPECT_GT(checked, 20u);
    }

    // Errors are GraphErrors on the node: no UVs, a missing file, a name nothing has.
    EXPECT_THROW(ColorGroom(test::makePlane(4), R"j({"map": )j" + quoted + "}").eval(), GraphError);
    EXPECT_THROW(ColorGroom(tintedPlane(), R"j({"map": "groomlab_no_such_texture.ppm"})j").eval(), GraphError);
    EXPECT_THROW(ColorGroom(tintedPlane(), R"j({"map": "nope"})j").eval(), GraphError);
}

TEST(HairColorRgb, TipBlendWritesPointChannels) {
    ColorGroom g(test::makePlane(6), R"j({"melanin": 0.0, "melaninRandom": 0.0, "dye": [0.2, 0.2, 0.2], "tipColor": [1.0, 1.0, 1.0], "tipStrength": 0.5})j");
    const StrandsPtr s = g.eval();
    const std::vector<float>* r = s->pointChannel("colorR");
    ASSERT_TRUE(r && s->pointChannel("colorG") && s->pointChannel("colorB"));
    ASSERT_EQ(r->size(), s->pointCount());
    for (std::size_t i = 0; i < s->strandCount(); ++i) {
        const std::size_t n = s->strandPointCount(i), begin = s->strandBegin(i);
        EXPECT_FLOAT_EQ((*r)[begin], 0.2f);           // root: the strand color
        EXPECT_FLOAT_EQ((*r)[begin + n - 1], 0.6f);   // tip: halfway to white
        EXPECT_FLOAT_EQ(colorOf(*s, i)[0], 0.2f);     // the per-strand value stays the root color
    }
}

// mode color: the picked color as it is, whatever the pigment params say (white hair is white, not
// a dyed brown).
TEST(HairColorRgb, ColorModeIsThePickedColorWhateverThePigment) {
    ColorGroom g(test::makePlane(6), R"j({"mode": "color", "color": [1.0, 1.0, 1.0], "melanin": 0.9, "redness": 0.8,
        "dye": [0.3, 0.2, 0.1], "melaninRandom": 0.3, "greys": 0.5})j");
    const StrandsPtr s = g.eval();
    ASSERT_GT(s->strandCount(), 20u);
    for (std::size_t i = 0; i < s->strandCount(); ++i) {
        EXPECT_EQ(colorOf(*s, i), (std::array<float, 3>{1.0f, 1.0f, 1.0f}));
        EXPECT_FLOAT_EQ((*s->strandChannel(kMelaninChannel))[i], 0.0f);
        EXPECT_FLOAT_EQ((*s->strandChannel(kMelaninRednessChannel))[i], 0.0f);
    }
    // The same params in melanin mode: the pigment's brown, dyed.
    ColorGroom natural(test::makePlane(6), R"j({"mode": "melanin", "color": [1.0, 1.0, 1.0], "melanin": 0.9, "redness": 0.8,
        "dye": [0.3, 0.2, 0.1]})j");
    EXPECT_LT(colorOf(*natural.eval(), 0)[0], 0.3f);
    EXPECT_THROW(ColorGroom(test::makePlane(4), R"j({"mode": "rgb"})j").eval(), GraphError);
}

// Skin-toned roots, white tips and a little randomness: valueRandom lightens and darkens the
// picked color itself (its hue kept: no melanin browns it), the tips vary with their strand, and
// tipOffsetRandom moves where each strand's tip color starts.
TEST(HairColorRgb, ColorModeRootsToWhiteTipsWithRandomness) {
    const std::array<float, 3> skin{0.8f, 0.55f, 0.45f};
    ColorGroom g(test::makePlane(6), R"j({"mode": "color", "color": [0.8, 0.55, 0.45], "tipColor": [1.0, 1.0, 1.0],
        "tipStrength": 1.0, "tipProfile": [[0, 0], [0.5, 0], [1, 1]], "valueRandom": 0.2, "tipOffsetRandom": 0.3})j");
    const StrandsPtr s = g.eval();
    const std::vector<float>* r = s->pointChannel("colorR");
    const std::vector<float>* gch = s->pointChannel("colorG");
    ASSERT_TRUE(r && gch);
    float rootLo = 1.0f, rootHi = 0.0f, middleLo = 1.0f, middleHi = 0.0f;
    for (std::size_t i = 0; i < s->strandCount(); ++i) {
        const std::size_t n = s->strandPointCount(i), begin = s->strandBegin(i);
        const std::array<float, 3> root = colorOf(*s, i);
        EXPECT_NEAR(root[1] / root[0], skin[1] / skin[0], 1e-4f);  // the hue kept
        rootLo = std::min(rootLo, root[0]);
        rootHi = std::max(rootHi, root[0]);
        EXPECT_FLOAT_EQ((*r)[begin], root[0]);  // the root: the strand's color
        // The tip: white, as light or dark as its strand (valueRandom scales the root too).
        EXPECT_NEAR((*r)[begin + n - 1], (*gch)[begin + n - 1], 1e-5f);
        EXPECT_NEAR((*r)[begin + n - 1], std::min(1.0f, root[0] / skin[0]), 1e-4f);
        const float middle = (*gch)[begin + 3 * (n - 1) / 4];
        middleLo = std::min(middleLo, middle - root[1]);
        middleHi = std::max(middleHi, middle - root[1]);
    }
    EXPECT_GT(rootHi - rootLo, 0.05f);   // the roots vary
    EXPECT_GT(middleHi - middleLo, 0.05f);  // the tip color starts at different places
}

TEST(HairColorRgb, ExportPrefersExplicitColorOverMelanin) {
    StrandSet s;
    s.addStrand(1, {{0, 0, 0}, {0, 1, 0}}, {0.01f, 0.01f});
    s.addStrand(2, {{1, 0, 0}, {1, 1, 0}}, {0.01f, 0.01f});
    s.addStrandChannel(kMelaninChannel, 1.0f);  // black hair
    EXPECT_LT(strandColors(s)[0][0], 0.2f);
    s.addStrandChannel(kColorChannels[0], 0.9f);
    s.addStrandChannel(kColorChannels[1], 0.8f);
    EXPECT_LT(strandColors(s)[0][0], 0.2f);  // two of three channels: not a color yet
    s.addStrandChannel(kColorChannels[2], 0.7f);
    const std::vector<std::array<float, 3>> colors = strandColors(s);
    ASSERT_EQ(colors.size(), 2u);
    EXPECT_FLOAT_EQ(colors[1][0], 0.9f);
    EXPECT_FLOAT_EQ(colors[1][1], 0.8f);
    EXPECT_FLOAT_EQ(colors[1][2], 0.7f);
    const ExportView view = describeExport(s, nullptr);
    EXPECT_TRUE(view.hasColors());
    EXPECT_FLOAT_EQ(view.colors[0][2], 0.7f);
}

TEST(HairColorRgb, ExportViewHasPointColorsOnlyWithAllThreePointChannels) {
    StrandSet s;
    s.addStrand(1, {{0, 0, 0}, {0, 1, 0}}, {0.01f, 0.01f});
    s.addStrand(2, {{1, 0, 0}, {1, 1, 0}, {1, 2, 0}}, {0.01f, 0.01f, 0.01f});
    for (const char* name : kColorChannels) s.addStrandChannel(name, 0.4f);
    EXPECT_FALSE(describeExport(s, nullptr).hasPointColors());
    s.addPointChannel(kColorChannels[0], 0.2f);
    s.addPointChannel(kColorChannels[1], 0.3f);
    EXPECT_FALSE(describeExport(s, nullptr).hasPointColors());  // two of three: per strand still
    s.addPointChannel(kColorChannels[2], 0.5f);
    (*s.mutablePointChannel(kColorChannels[0]))[4] = 0.9f;
    const ExportView view = describeExport(s, nullptr);
    ASSERT_TRUE(view.hasPointColors());
    ASSERT_EQ(view.pointColors.size(), s.pointCount());
    EXPECT_FLOAT_EQ(view.pointColors[0][1], 0.3f);
    EXPECT_FLOAT_EQ(view.pointColors[4][0], 0.9f);
    EXPECT_FLOAT_EQ(view.pointColors[4][2], 0.5f);
    EXPECT_FLOAT_EQ(view.colors[1][0], 0.4f);  // the per-strand color stays
}

TEST(Flyaway, AFractionStraysFromMidLengthAndGrows) {
    const StrandsPtr base = run("");
    const StrandsPtr s = run(R"j({"name": "op", "type": "Flyaway",
        "params": {"fraction": 0.2, "stray": 0.3, "start": 0.4, "lengthen": 0.2, "seed": 1}})j");
    ASSERT_EQ(s->strandCount(), base->strandCount());
    const std::vector<float>& flag = *s->strandChannel("flyaway");
    std::size_t strays = 0;
    for (std::size_t i = 0; i < s->strandCount(); ++i) {
        const std::size_t n = s->strandPointCount(i);
        const Vec3f* a = base->strandPoints(i);
        const Vec3f* b = s->strandPoints(i);
        if (flag[i] < 0.5f) {
            for (std::size_t k = 0; k < n; ++k) EXPECT_LT((a[k] - b[k]).length(), 1e-6f);
            continue;
        }
        ++strays;
        EXPECT_LT((a[0] - b[0]).length(), 1e-6f);       // rooted
        EXPECT_LT((a[2] - b[2]).length(), 0.3f * 0.3f);  // barely moved before start
        EXPECT_GT(arcLength(b, n), arcLength(a, n) + 0.05f);  // longer
    }
    const float share = static_cast<float>(strays) / static_cast<float>(s->strandCount());
    EXPECT_NEAR(share, 0.2f, 0.06f);
}

TEST(Tangle, StuckStrandsComeTogetherOverASpan) {
    // Straight strands, tangled with their neighbors.
    const StrandsPtr base = run("");
    const StrandsPtr s = run(R"j({"name": "op", "type": "Tangle",
        "params": {"fraction": 1.0, "radius": 0.2, "strength": 1.0, "start": 0.2, "span": 0.5, "seed": 2}})j");
    ASSERT_EQ(s->strandCount(), base->strandCount());
    std::size_t moved = 0;
    for (std::size_t i = 0; i < s->strandCount(); ++i) {
        const Vec3f* a = base->strandPoints(i);
        const Vec3f* b = s->strandPoints(i);
        EXPECT_LT((a[0] - b[0]).length(), 1e-6f);  // roots stay
        float most = 0.0f;
        for (std::size_t k = 0; k < s->strandPointCount(i); ++k) most = std::max(most, (a[k] - b[k]).length());
        if (most > 1e-3f) ++moved;
        EXPECT_LE(most, 0.2f + 1e-4f);  // at most onto a neighbor within 0.2
    }
    EXPECT_GT(moved, s->strandCount() * 9 / 10);
}

// Two straight strands 0.1 apart (100 points each, so a point sits near the span's
// middle), tangled at strength 1; `stick` says which of them stick (the fraction map),
// `clumps` their clumpId when given; `set` edits Tangle's params further.
std::array<StrandsPtr, 2> tangledPair(std::vector<float> stick, std::vector<float> clumps = {},
                                      const std::function<void(ParamSet&)>& set = {}) {
    auto in = std::make_shared<StrandSet>();
    for (int i = 0; i < 2; ++i) {
        in->addStrand(7 + i, test::straightCurve({0.4f + 0.1f * static_cast<float>(i), 0.0f, 0.5f}, {0, 1, 0}, 99));
    }
    in->addStrandChannel("stick") = std::move(stick);
    if (!clumps.empty()) in->addStrandChannel("clumpId") = std::move(clumps);
    auto tangle = OperatorRegistry::instance().create(op::kTangle);
    MapRef fraction;
    fraction.source = MapRef::Source::Attribute;
    fraction.name = "stick";
    tangle->params().set("fraction", fraction);
    tangle->params().set("strength", 1.0);
    tangle->params().set("radius", 0.2);
    if (set) set(tangle->params());
    EvalContext ctx;
    return {StrandsPtr(in), valueAs<StrandsPtr>(tangle->evaluate(ctx, {StrandsPtr(in)}))};
}

// The closest the first strand of `s` comes to the second's curve (a stuck strand keeps its
// length, so its points lag a little behind its partner's along the span).
float closest(const StrandSet& s) {
    float d = 1e9f;
    const Vec3f* other = s.strandPoints(1);
    for (std::size_t k = 0; k < s.strandPointCount(0); ++k) {
        const Vec3f p = s.strandPoints(0)[k];
        for (std::size_t j = 0; j + 1 < s.strandPointCount(1); ++j) {
            const Vec3f seg = other[j + 1] - other[j];
            const float t = std::clamp((p - other[j]).dot(seg) / std::max(1e-12f, seg.dot(seg)), 0.0f, 1.0f);
            d = std::min(d, (other[j] + seg * t - p).length());
        }
    }
    return d;
}

// How far the points of strand `s` moved from `in` to `out`, at most.
float movedMost(const StrandSet& in, const StrandSet& out, std::size_t s) {
    float most = 0.0f;
    for (std::size_t k = 0; k < in.strandPointCount(s); ++k) {
        most = std::max(most, (out.strandPoints(s)[k] - in.strandPoints(s)[k]).length());
    }
    return most;
}

TEST(Tangle, AtFullStrengthAStrandMeetsAPartnerThatStaysPut) {
    const auto [in, out] = tangledPair({1.0f, 0.0f});  // only the first sticks: all the way onto the second
    EXPECT_LT(closest(*out), 5e-3f);  // touching, within 5% of the gap (its length kept, it lags a little)
    for (std::size_t k = 0; k < 100; ++k) {
        EXPECT_LT((out->strandPoints(1)[k] - in->strandPoints(1)[k]).length(), 1e-7f);  // the partner stays
    }
    EXPECT_NEAR(arcLength(out->strandPoints(0), 100), arcLength(in->strandPoints(0), 100), 1e-4f);  // no stretch
}

TEST(Tangle, AtFullStrengthAMutualPairMeetsInTheMiddle) {
    const auto [in, out] = tangledPair({1.0f, 1.0f});  // each picks the other: both move halfway
    EXPECT_LT(closest(*out), 1e-3f);
    EXPECT_NEAR(std::max(movedMost(*in, *out, 0), movedMost(*in, *out, 1)), 0.05f, 1e-3f);
}

TEST(Tangle, PartnersComeFromTheStrandsOwnClump) {
    // Neighbours of two clumps never stick (they would bridge the gap between the clumps)...
    const auto [in, apart] = tangledPair({1.0f, 1.0f}, {3.0f, 4.0f});
    EXPECT_EQ(movedMost(*in, *apart, 0), 0.0f);
    EXPECT_EQ(movedMost(*in, *apart, 1), 0.0f);
    // ...unless asked, or when they share the clump.
    const auto across = tangledPair({1.0f, 1.0f}, {3.0f, 4.0f}, [](ParamSet& p) { p.set("clumpChannel", std::string()); })[1];
    EXPECT_LT(closest(*across), 1e-3f);
    const auto together = tangledPair({1.0f, 1.0f}, {3.0f, 3.0f})[1];
    EXPECT_LT(closest(*together), 1e-3f);
}

TEST(Tangle, APartnerFartherThanTheReachDoesNotPull) {
    const auto set = [](double reach) { return [reach](ParamSet& p) { p.set("reach", reach); }; };
    const auto [in, held] = tangledPair({1.0f, 0.0f}, {}, set(0.05));  // 0.1 apart: out of reach
    EXPECT_EQ(movedMost(*in, *held, 0), 0.0f);
    const auto unlimited = tangledPair({1.0f, 0.0f}, {}, set(0.0))[1];  // 0: no limit
    EXPECT_LT(closest(*unlimited), 5e-3f);
}

TEST(Tangle, WetHairStaysInItsClumpsWithoutStretching) {
    // The former wet_hair preset (tests/data/presets) on a ball: its tight clumps stuck together by
    // Tangle. Strands of neighbouring clumps once pulled tips across the gap, up to 7.5 times a segment's length.
    std::ifstream file(std::filesystem::u8path(std::string(GROOMLAB_TEST_DATA) + "/presets/wet_hair_tangle.json"));
    std::stringstream text;
    text << file.rdbuf();
    GraphDocument doc = loadGraphJson(presetGraphJson(text.str(), 20.0));
    doc.graph.node(doc.node("scatter")).params().set("density", 40.0);
    ExternalInputs externals;
    setGroomExternals(externals, test::makeSphere(24, 48, 8.0f), nullptr, nullptr);
    EvalContext ctx;
    ctx.externals = &externals;
    const StrandsPtr in = doc.graph.evaluateStrands(doc.node("clump"), ctx);
    const StrandsPtr out = doc.graph.evaluateStrands(doc.node("tangle"), ctx);
    const float reach = static_cast<float>(doc.graph.node(doc.node("tangle")).params().getDouble("reach", 0.25));
    std::size_t moved = 0;
    for (std::size_t s = 0; s < in->strandCount(); ++s) {
        const std::size_t n = in->strandPointCount(s);
        const Vec3f* a = in->strandPoints(s);
        const Vec3f* b = out->strandPoints(s);
        const float most = movedMost(*in, *out, s);
        if (most == 0.0f) continue;
        ++moved;
        EXPECT_LE(most, reach * 1.5f) << s;  // the reach, and the lengths kept along it
        for (std::size_t k = 1; k < n; ++k) {
            ASSERT_NEAR((b[k] - b[k - 1]).length(), (a[k] - a[k - 1]).length(), 1e-4f) << s << " " << k;
        }
    }
    EXPECT_GT(moved, in->strandCount() / 3);  // still matted
}

TEST(Cleanup, RampLinearFlagIsPartOfTheCacheKey) {
    // Toggling only a ramp's Linear flag must re-evaluate (it changes the curve).
    GroomData d;
    d.scalp = test::makePlane(8);
    d.graphJson = R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 200.0}},
        {"name": "noise", "type": "Noise", "params": {"amplitude": 0.2, "profile": [[0, 0], [0.5, 1], [1, 0.3]]}},
        {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 8}}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                      {"from": "scatter", "to": "noise", "input": "strands"},
                      {"from": "noise", "to": "out", "input": "strands"}]})j";
    GraphDocument doc = loadGraphJson(d.graphJson);
    ExternalInputs externals;
    setGroomExternals(externals, d.scalp, nullptr, nullptr);
    EvalContext ctx;
    ctx.externals = &externals;
    const StrandsPtr smooth = doc.graph.evaluateStrands(doc.output, ctx);
    Ramp straight = doc.graph.node(doc.node("noise")).params().getRamp("profile");
    straight.straight = true;
    doc.graph.node(doc.node("noise")).params().set("profile", straight);
    const StrandsPtr linear = doc.graph.evaluateStrands(doc.output, ctx);
    EXPECT_NE(smooth->contentHash(), linear->contentHash());
}

TEST(Cleanup, AReloadedGraphReusesTheOldCache) {
    // Editing one param reloads the whole graph document (as Maya does);
    // only the edited node and what follows it may run again.
    auto graphWith = [](double amplitude) {
        return std::string(R"j({"version": 1, "output": "out", "nodes": [
            {"name": "scalp", "type": "Scalp"},
            {"name": "scatter", "type": "Scatter", "params": {"density": 300.0}},
            {"name": "length", "type": "Length", "params": {"keep": 0.8}},
            {"name": "noise", "type": "Noise", "params": {"amplitude": )j") + std::to_string(amplitude) + R"j(}},
            {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 8}}],
          "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                          {"from": "scatter", "to": "length", "input": "strands"},
                          {"from": "length", "to": "noise", "input": "strands"},
                          {"from": "noise", "to": "out", "input": "strands"}]})j";
    };
    const auto plane = test::makePlane(8);
    ExternalInputs externals;
    setGroomExternals(externals, plane, nullptr, nullptr);
    EvalContext ctx;
    ctx.externals = &externals;
    GraphDocument before = loadGraphJson(graphWith(0.05));
    before.graph.evaluateStrands(before.output, ctx);
    EXPECT_EQ(before.graph.evaluationCount(), 5u);

    GraphDocument after = loadGraphJson(graphWith(0.2));
    after.graph.adoptCache(before.graph);
    const StrandsPtr edited = after.graph.evaluateStrands(after.output, ctx);
    EXPECT_EQ(after.graph.evaluationCount(), 2u);  // noise and the output; scalp, scatter and length reused

    GraphDocument fresh = loadGraphJson(graphWith(0.2));
    EXPECT_EQ(fresh.graph.evaluateStrands(fresh.output, ctx)->contentHash(), edited->contentHash());
}

TEST(Cleanup, MergeKeepsBothSidesCards) {
    StrandSet a, b;
    a.addStrand(1, {{0, 0, 0}, {0, 1, 0}});
    b.addStrand(2, {{1, 0, 0}, {1, 1, 0}});
    b.addCardLayer(std::make_shared<CardMesh>());
    auto& channel = a.addStrandChannel("clumpId", -1.0f);
    channel[0] = 5.0f;
    StrandSet merged = a;
    merged.append(b);
    EXPECT_EQ(merged.strandCount(), 2u);
    EXPECT_EQ(merged.strandPointCount(1), 2u);
    EXPECT_EQ(merged.strandPoints(1)[1].y, 1.0f);
    EXPECT_EQ((*merged.strandChannel("clumpId"))[1], -1.0f);  // default for b's strand
    std::string error;
    EXPECT_TRUE(merged.validate(&error)) << error;
}


}  // namespace
}  // namespace groomlab
