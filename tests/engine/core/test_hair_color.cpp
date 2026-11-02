// SPDX-License-Identifier: Apache-2.0
// HairColor, param by param: every param changes what the node writes (the RGB, per strand and per
// point, the melanin, the highlight channels) and what the hosts read of the look
// (GroomSession::hairLook); hue, saturation, melanin and value randomness vary each strand in both
// modes, shared per clump by clumpShare; the hair colors' model (melaninColor is the color whose
// absorption is the pigment's, varyHairColor's HSL turns).

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/display.h"
#include "groomlab/export_view.h"
#include "groomlab/groom.h"
#include "groomlab/groom_session.h"
#include "groomlab/hair_color.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

using Rgb = std::array<float, 3>;
using nlohmann::json;

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

// Scalp -> Scatter -> Length -> Clump (12 clumps, not pulling) -> HairColor(params) -> Output.
GroomData colorGroom(const json& params, bool clumped = true) {
    const std::string upstream = clumped ? "clump" : "length";
    const json graph = {
        {"version", 1},
        {"output", "out"},
        {"nodes",
         {{{"name", "scalp"}, {"type", "Scalp"}},
          {{"name", "scatter"}, {"type", "Scatter"}, {"params", {{"density", 500.0}}}},
          {{"name", "length"}, {"type", "Length"}, {"params", {{"keep", 1.0}}}},
          {{"name", "clump"}, {"type", "Clump"}, {"params", {{"count", 12}, {"strength", 0.0}}}},
          {{"name", "color"}, {"type", "HairColor"}, {"params", params}},
          {{"name", "resolution"}, {"type", "Resample"}, {"params", {{"segments", 6}}}},
          {{"name", "out"}, {"type", "Output"}}}},
        {"connections",
         {{{"from", "scalp"}, {"to", "scatter"}, {"input", "scalp"}},
          {{"from", "scatter"}, {"to", "length"}, {"input", "strands"}},
          {{"from", "length"}, {"to", "clump"}, {"input", "strands"}},
          {{"from", upstream}, {"to", "color"}, {"input", "strands"}},
          {{"from", "color"}, {"to", "out"}, {"input", "strands"}}}}};
    GroomData d;
    d.scalp = tintedPlane();
    d.graphJson = graph.dump();
    return d;
}

StrandsPtr colored(const json& params, bool clumped = true) { return Groom(colorGroom(params, clumped)).evaluate(1.0f, 0.0); }

Rgb strandColor(const StrandSet& s, std::size_t i) {
    return {(*s.strandChannel("colorR"))[i], (*s.strandChannel("colorG"))[i], (*s.strandChannel("colorB"))[i]};
}

// Everything HairColor writes, flattened: the RGB per strand and per point, the melanin and
// redness, the highlight channels (a channel's absence counts as a difference against its
// presence).
std::map<std::string, std::vector<float>> written(const StrandSet& s) {
    std::map<std::string, std::vector<float>> out;
    for (const char* name : {"colorR", "colorG", "colorB", kMelaninChannel, kMelaninRednessChannel, kSpecularChannel,
                             kRoughnessChannel, kTiltChannel}) {
        if (const std::vector<float>* v = s.strandChannel(name)) out[std::string("strand.") + name] = *v;
    }
    for (const char* name : {"colorR", "colorG", "colorB"}) {
        if (const std::vector<float>* v = s.pointChannel(name)) out[std::string("point.") + name] = *v;
    }
    return out;
}

// The largest change between two results, relative to the values (dark hair colors are small
// numbers); 1 for a channel only one has.
float change(const StrandSet& a, const StrandSet& b) {
    const auto x = written(a), y = written(b);
    float worst = 0.0f;
    for (const auto& [name, values] : x) {
        const auto it = y.find(name);
        if (it == y.end() || it->second.size() != values.size()) return 1.0f;
        for (std::size_t i = 0; i < values.size(); ++i) {
            const float scale = std::max({std::abs(values[i]), std::abs(it->second[i]), 1e-3f});
            worst = std::max(worst, std::abs(values[i] - it->second[i]) / scale);
        }
    }
    for (const auto& [name, values] : y) {
        if (!x.count(name)) return 1.0f;
    }
    return worst;
}

// HSL hue (turns) and saturation of a color.
std::array<float, 2> hueSaturation(const Rgb& c) {
    const float hi = std::max({c[0], c[1], c[2]}), lo = std::min({c[0], c[1], c[2]}), d = hi - lo;
    if (d < 1e-7f) return {0.0f, 0.0f};
    float h;
    if (hi == c[0]) h = (c[1] - c[2]) / d;
    else if (hi == c[1]) h = 2.0f + (c[2] - c[0]) / d;
    else h = 4.0f + (c[0] - c[1]) / d;
    h /= 6.0f;
    if (h < 0.0f) h += 1.0f;
    const float l = 0.5f * (hi + lo);
    return {h, d / std::max(1.0f - std::abs(2.0f * l - 1.0f), 1e-7f)};
}

// ---- The color model ----------------------------------------------------------------------------

// A melanin's color is the hair color whose absorption (Chiang's mapping: Arnold's base color, the
// viewport's hairColorSigma) is the pigment's: so the RGB renders as aiStandardHair's melanin.
TEST(HairColorModel, AMelaninsColorAbsorbsAsThePigment) {
    for (const float m : {0.0f, 0.1f, 0.35f, 0.6f, 0.9f, 1.0f}) {
        for (const float r : {0.0f, 0.3f, 1.0f}) {
            const Rgb sigma = hairMelaninAbsorption(m, r), back = hairColorAbsorption(melaninColor(m, r));
            for (int k = 0; k < 3; ++k) EXPECT_NEAR(back[k], sigma[k], 1e-3f * std::max(1.0f, sigma[k])) << m << " " << r << " " << k;
        }
    }
    EXPECT_EQ(melaninColor(0.0f, 0.5f), (Rgb{1.0f, 1.0f, 1.0f}));  // no pigment: white
    const Rgb brown = melaninColor(0.35f, 0.2f), dyed = melaninColor(0.35f, 0.2f, {1.0f, 0.5f, 0.25f});
    // The dye's absorption adds to the pigment's (aiStandardHair's melanin and base color).
    const Rgb sum = hairColorAbsorption(dyed), pigment = hairMelaninAbsorption(0.35f, 0.2f),
              tint = hairColorAbsorption({1.0f, 0.5f, 0.25f});
    for (int k = 0; k < 3; ++k) EXPECT_NEAR(sum[k], pigment[k] + tint[k], 1e-3f * sum[k]);
    EXPECT_FLOAT_EQ(dyed[0], brown[0]);
    EXPECT_LT(dyed[1], brown[1]);
    EXPECT_EQ(melaninColor(0.0f, 0.3f, {0.6f, 0.3f, 0.15f})[1], dyedHairColor({1.0f, 1.0f, 1.0f}, {0.6f, 0.3f, 0.15f})[1]);
    EXPECT_GT(brown[0], brown[1]);
    EXPECT_GT(brown[1], brown[2]);
    EXPECT_LT(melaninColor(0.9f, 0.2f)[0], melaninColor(0.3f, 0.2f)[0]);  // more melanin: darker
    EXPECT_GT(melaninColor(0.4f, 1.0f)[0] / melaninColor(0.4f, 1.0f)[1], brown[0] / brown[1]);  // redness: redder
}

// The base color a hair BSDF takes against the appearance: what the hair looks like (measured with
// kick), for what stores or draws a color without a hair BSDF. Medium brown looks a medium brown,
// blond light, black near black with its sheen, white white; the inverse gives the base color back.
TEST(HairColorModel, TheAppearanceIsWhatTheHairLooksLike) {
    auto srgb = [](float c) { return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; };
    const Rgb brown = melaninColor(0.35f, 0.2f), shown = hairAppearanceColor(brown);
    EXPECT_LT(brown[0], 0.06f);  // the base color is dark
    // A medium brown on screen (sRGB about 145, 108, 66).
    EXPECT_NEAR(255.0f * srgb(shown[0]), 145.0f, 15.0f);
    EXPECT_NEAR(255.0f * srgb(shown[1]), 108.0f, 15.0f);
    EXPECT_NEAR(255.0f * srgb(shown[2]), 66.0f, 15.0f);
    EXPECT_GT(shown[0], shown[1]);
    EXPECT_GT(shown[1], shown[2]);
    const Rgb blond = hairAppearanceColor(melaninColor(0.15f, 0.2f)), black = hairAppearanceColor(melaninColor(1.0f, 0.1f));
    EXPECT_GT(blond[2], 0.3f);
    EXPECT_LT(black[0], 0.05f);
    EXPECT_GE(black[0], kAppearanceSheen - 1e-6f);
    EXPECT_NEAR(hairAppearanceColor({1.0f, 1.0f, 1.0f})[1], 1.0f, 1e-5f);
    for (const Rgb& c : {brown, melaninColor(0.15f, 0.2f), Rgb{0.6f, 0.35f, 0.2f}, Rgb{0.9f, 0.9f, 0.9f}}) {
        const Rgb back = hairBaseOfAppearance(hairAppearanceColor(c));
        for (int k = 0; k < 3; ++k) EXPECT_NEAR(back[k], c[k], 2e-3f * std::max(c[k], 0.05f)) << k;
    }
    // Lighter than the base color, the order of colors kept.
    for (const float c : {0.001f, 0.01f, 0.1f, 0.5f}) {
        EXPECT_GT(hairAppearanceColor({c, c, c})[0], c);
        EXPECT_LT(hairAppearanceColor({c, c, c})[0], hairAppearanceColor({2.0f * c, 0.0f, 0.0f})[0]);
    }
}

// What each consumer gets: renderers the base color, writers of a display or albedo color the
// appearance (USD displayColor, Alembic Cd, Unreal's groom_color: describeExport's
// kExportAppearanceColors), and the display's lit strands either, as the host draws them.
TEST(HairColorModel, RenderersGetTheBaseColorAndDisplaysTheAppearance) {
    const StrandsPtr s = colored({{"melanin", 0.35}, {"melaninRandom", 0.0}, {"tipStrength", 0.5}});
    const ExportView base = describeExport(*s, nullptr, kExportPointColors);
    const ExportView shown = describeExport(*s, nullptr, kExportPointColors | kExportAppearanceColors);
    ASSERT_FALSE(base.colors.empty());
    ASSERT_EQ(base.pointColors.size(), s->pointCount());
    EXPECT_EQ(base.colors[0], strandColor(*s, 0));
    EXPECT_EQ(shown.colors[0], hairAppearanceColor(strandColor(*s, 0)));
    EXPECT_EQ(shown.pointColors[3], hairAppearanceColor(base.pointColors[3]));
    EXPECT_EQ(strandAppearanceColors(*s)[0], shown.colors[0]);
    const auto bsdf = strandLookColors(s, false), plain = strandLookColors(s, true);
    ASSERT_TRUE(bsdf && plain);
    EXPECT_NE(bsdf->key, plain->key);
    EXPECT_EQ(plain->at(0, s->strandBegin(0) + 2), hairAppearanceColor(bsdf->at(0, s->strandBegin(0) + 2)));
}

// HSL turns, as PxrHairColor varies its dye: the hue turns with the lightness kept, the
// saturation scales, grey and white stay neutral, the value scales it all.
TEST(HairColorModel, VaryHairColorTurnsTheHueAndScalesTheSaturationInHsl) {
    const Rgb c{0.6f, 0.3f, 0.1f};
    const Rgb turned = varyHairColor(c, 0.25f, 1.0f, 1.0f);
    EXPECT_NEAR(hueSaturation(turned)[0], hueSaturation(c)[0] + 0.25f, 1e-4f);
    EXPECT_NEAR(std::max({turned[0], turned[1], turned[2]}) + std::min({turned[0], turned[1], turned[2]}), 0.7f, 1e-5f);
    const Rgb flat = varyHairColor(c, 0.0f, 0.5f, 1.0f);
    EXPECT_NEAR(hueSaturation(flat)[1], 0.5f * hueSaturation(c)[1], 1e-4f);
    EXPECT_NEAR(hueSaturation(flat)[0], hueSaturation(c)[0], 1e-4f);
    for (const Rgb& neutral : {Rgb{1.0f, 1.0f, 1.0f}, Rgb{0.4f, 0.4f, 0.4f}}) {
        EXPECT_EQ(varyHairColor(neutral, 0.3f, 1.8f, 1.0f), neutral);
    }
    const Rgb darker = varyHairColor(c, 0.0f, 1.0f, 0.5f);
    for (int k = 0; k < 3; ++k) EXPECT_FLOAT_EQ(darker[k], c[k] * 0.5f);
    EXPECT_EQ(varyHairColor(c, 0.0f, 1.0f, 1.0f), c);
    EXPECT_EQ(varyHairColor(c, 1.0f, 1.0f, 1.0f)[0], varyHairColor(c, 0.0f, 1.0f, 1.0f)[0]);  // a whole turn
}

// ---- Every param --------------------------------------------------------------------------------

// Each param set off its default (on a base that lets it show) changes what HairColor writes.
TEST(HairColor, EveryParamChangesWhatItWrites) {
    struct Case {
        std::string param;
        json base;  // the other params (the param itself at its default unless set here)
        json value;
    };
    const json melanin = {{"melaninRandom", 0.0}};
    const json color = {{"mode", "color"}};
    const json tipped = {{"melaninRandom", 0.0}, {"tipStrength", 1.0}};
    const json random = {{"melaninRandom", 0.3}};
    const std::vector<Case> cases = {
        {"mode", melanin, "color"},
        {"melanin", melanin, 0.7},
        {"redness", melanin, 0.8},
        {"dye", melanin, {1.0, 0.5, 0.4}},
        {"color", color, {0.2, 0.5, 0.9}},
        {"map", melanin, "tint"},
        {"map", color, "tint"},
        {"greys", melanin, 0.5},
        {"melaninRandom", melanin, 0.3},
        {"valueRandom", color, 0.5},
        {"hueRandom", melanin, 0.5},
        {"hueRandom", {{"mode", "color"}, {"color", {0.6, 0.4, 0.3}}}, 0.5},
        {"saturationRandom", melanin, 0.5},
        {"saturationRandom", {{"mode", "color"}, {"color", {0.6, 0.4, 0.3}}}, 0.5},
        {"clumpShare", random, 1.0},
        {"clumpChannel", {{"melaninRandom", 0.3}, {"clumpShare", 1.0}}, "nothing"},
        {"seed", random, 7},
        {"tipColor", tipped, {0.9, 0.1, 0.1}},
        {"tipStrength", melanin, 0.6},
        {"tipProfile", {{"melaninRandom", 0.0}, {"tipStrength", 0.5}}, {{0, 1}, {1, 1}}},
        {"tipOffsetRandom", tipped, 0.4},
        {"specular", melanin, 0.8},
        {"specularRandom", melanin, 0.5},
        {"roughness", melanin, 0.2},
        {"roughnessRandom", melanin, 0.5},
        {"tilt", melanin, 8.0},
    };
    std::set<std::string> covered;
    for (const Case& c : cases) {
        json set = c.base;
        set[c.param] = c.value;
        const StrandsPtr before = colored(c.base), after = colored(set);
        ASSERT_TRUE(before && after);
        EXPECT_GT(change(*before, *after), 0.01f) << c.param << " " << c.value.dump() << " on " << c.base.dump();
        covered.insert(c.param);
    }
    // No param left out (Groom Tools' panels and the Attribute Editor show every one).
    for (const ParamSpec& spec : OperatorRegistry::instance().create("HairColor")->paramSpecs()) {
        EXPECT_TRUE(covered.count(spec.name)) << spec.name;
    }
}

// The look the hosts shade and render by follows the highlight params, and every HairColor colors
// by its RGB (the viewport's hairColorSigma, Arnold's base color, RenderMan's rman_color).
TEST(HairColor, TheHostsLookFollowsItsParams) {
    auto look = [](const json& params) {
        GroomSession session(colorGroom(params));
        session.evaluate(ViewRequest{});
        return nlohmann::json::parse(session.hairLookJson());
    };
    const json plain = look(json::object());
    EXPECT_TRUE(plain["colorsByRgb"].get<bool>());
    EXPECT_EQ(plain["node"], "color");
    for (const auto& [param, value] : std::map<std::string, double>{
             {"specular", 0.8}, {"specularRandom", 0.4}, {"roughness", 0.2}, {"roughnessRandom", 0.4}, {"tilt", 7.0}}) {
        const json changed = look({{param, value}});
        EXPECT_NEAR(changed[param].get<double>(), value, 1e-6) << param;
        EXPECT_NE(changed[param], plain[param]) << param;
        EXPECT_NEAR(changed["hairColorParams"][param].get<double>(), value, 1e-6) << param;
    }
    EXPECT_NE(look({{"specular", 0.15}})["lobes"]["specularTint"], plain["lobes"]["specularTint"]);
    EXPECT_NE(look({{"roughness", 0.9}})["lobes"]["roughness"], plain["lobes"]["roughness"]);
    // Color params reach the hosts through the RGB, and the params as they are set.
    EXPECT_TRUE(look({{"mode", "color"}, {"hueRandom", 0.3}})["colorsByRgb"].get<bool>());
    EXPECT_NEAR(look({{"saturationRandom", 0.25}})["hairColorParams"]["saturationRandom"].get<double>(), 0.25, 1e-6);
}

// ---- Randomness ---------------------------------------------------------------------------------

// Hue and saturation randomness vary each strand's hue and saturation, in both modes (the
// pigment's color too: nothing is lost on the way to the hosts, which all shade the RGB).
TEST(HairColor, HueAndSaturationRandomnessVaryEachStrandInBothModes) {
    for (const json& base : {json{{"melaninRandom", 0.0}, {"clumpShare", 0.0}},
                             json{{"mode", "color"}, {"color", {0.5, 0.3, 0.15}}, {"clumpShare", 0.0}}}) {
        auto spread = [&](const char* param, int which) {
            json set = base;
            set[param] = 0.6;
            const StrandsPtr s = colored(set);
            float lo = 1e9f, hi = -1e9f;
            for (std::size_t i = 0; i < s->strandCount(); ++i) {
                float v = hueSaturation(strandColor(*s, i))[which];
                if (which == 0 && v > 0.75f) v -= 1.0f;  // reds either side of 0
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            return hi - lo;
        };
        const StrandsPtr flat = colored(base);
        for (std::size_t i = 1; i < flat->strandCount(); ++i) EXPECT_EQ(strandColor(*flat, i), strandColor(*flat, 0)) << base.dump();
        EXPECT_GT(spread("hueRandom", 0), 0.3f) << base.dump();   // up to +/- 0.3 turns
        EXPECT_GT(spread("saturationRandom", 1), 0.2f) << base.dump();
        // ... and the absorption the hosts shade with differs strand to strand, beyond the brightness.
        json hue = base;
        hue["hueRandom"] = 0.6;
        const StrandsPtr s = colored(hue);
        std::set<int> ratios;
        for (std::size_t i = 0; i < s->strandCount(); ++i) {
            const Rgb sigma = hairColorAbsorption(strandColor(*s, i));
            ratios.insert(static_cast<int>(std::round(10.0f * sigma[0] / std::max(sigma[2], 1e-4f))));
        }
        EXPECT_GT(ratios.size(), 4u) << base.dump();
    }
}

// Melanin randomness (mode melanin) and value randomness (mode color) lighten and darken each
// strand: its melanin within the randomness, its picked color scaled, the hue kept.
TEST(HairColor, MelaninAndValueRandomnessLightenAndDarkenEachStrand) {
    const StrandsPtr m = colored({{"melanin", 0.5}, {"melaninRandom", 0.2}, {"clumpShare", 0.0}});
    std::set<float> melanins;
    for (std::size_t i = 0; i < m->strandCount(); ++i) {
        const float v = (*m->strandChannel(kMelaninChannel))[i];
        EXPECT_GE(v, 0.3f - 1e-5f);
        EXPECT_LE(v, 0.7f + 1e-5f);
        melanins.insert(v);
        const Rgb want = melaninColor(v, 0.2f);
        for (int k = 0; k < 3; ++k) EXPECT_NEAR(strandColor(*m, i)[k], want[k], 1e-6f);
    }
    EXPECT_GT(melanins.size(), m->strandCount() / 2);
    const StrandsPtr v = colored({{"mode", "color"}, {"color", {0.6, 0.3, 0.15}}, {"valueRandom", 0.4}, {"clumpShare", 0.0}});
    float lo = 1.0f, hi = 0.0f;
    for (std::size_t i = 0; i < v->strandCount(); ++i) {
        const Rgb c = strandColor(*v, i);
        EXPECT_NEAR(c[1] / c[0], 0.5f, 1e-4f);
        lo = std::min(lo, c[0]);
        hi = std::max(hi, c[0]);
    }
    EXPECT_LT(lo, 0.6f * 0.75f);
    EXPECT_GT(hi, 0.6f * 1.25f);
}

// clumpShare: 1 every strand of a clump alike and the clumps apart, 0 each strand its own; between,
// the spread over the groom stays the same while strands of one clump keep closer together. For
// hue, saturation, melanin and value alike.
TEST(HairColor, ClumpShareGivesEachClumpItsOwnColor) {
    struct Case {
        json params;
        std::function<float(const StrandSet&, std::size_t)> measure;
    };
    const std::vector<Case> cases = {
        {{{"melaninRandom", 0.3}}, [](const StrandSet& s, std::size_t i) { return (*s.strandChannel(kMelaninChannel))[i]; }},
        {{{"melaninRandom", 0.0}, {"hueRandom", 0.5}}, [](const StrandSet& s, std::size_t i) {
             const float h = hueSaturation(strandColor(s, i))[0];
             return h > 0.75f ? h - 1.0f : h;
         }},
        {{{"mode", "color"}, {"color", {0.6, 0.4, 0.3}}, {"saturationRandom", 0.8}},
         [](const StrandSet& s, std::size_t i) { return hueSaturation(strandColor(s, i))[1]; }},
        {{{"mode", "color"}, {"valueRandom", 0.5}}, [](const StrandSet& s, std::size_t i) { return strandColor(s, i)[0]; }},
    };
    for (const Case& c : cases) {
        // The spread within clumps (the mean distance from the clump's mean) and across them.
        auto spreads = [&](double share) {
            json set = c.params;
            set["clumpShare"] = share;
            const StrandsPtr s = colored(set);
            const std::vector<float>& clumps = *s->strandChannel("clumpId");
            std::map<int, std::vector<float>> by;
            for (std::size_t i = 0; i < s->strandCount(); ++i) by[static_cast<int>(clumps[i])].push_back(c.measure(*s, i));
            float within = 0.0f, lo = 1e9f, hi = -1e9f;
            std::size_t n = 0;
            for (const auto& [id, values] : by) {
                float mean = 0.0f;
                for (const float v : values) mean += v / static_cast<float>(values.size());
                for (const float v : values) within += std::abs(v - mean), ++n;
                lo = std::min(lo, mean);
                hi = std::max(hi, mean);
            }
            EXPECT_GT(by.size(), 8u);
            return std::make_pair(within / static_cast<float>(n), hi - lo);
        };
        const auto [ownWithin, ownAcross] = spreads(0.0);
        const auto [halfWithin, halfAcross] = spreads(0.5);
        const auto [allWithin, allAcross] = spreads(1.0);
        EXPECT_LT(allWithin, 1e-5f) << c.params.dump();  // one color per clump
        EXPECT_GT(allAcross, 2.5f * ownAcross) << c.params.dump();  // and the clumps apart
        EXPECT_GT(ownWithin, 0.02f) << c.params.dump();
        EXPECT_LT(halfWithin, 0.85f * ownWithin) << c.params.dump();
        EXPECT_GT(halfAcross, 1.5f * ownAcross) << c.params.dump();
    }
}

// Without a clump channel reaching it, clumpShare says so in the node's note (set off its default:
// a fresh groom has no Clump) and each strand varies on its own.
TEST(HairColor, ClumpShareWithoutAClumpSaysSo) {
    auto noteOf = [](const json& params, bool clumped) {
        GroomSession session(colorGroom(params, clumped));
        session.evaluate(ViewRequest{});
        for (const auto& [node, note] : session.nodeNotes()) {
            if (node == "color") return note;
        }
        return std::string();
    };
    EXPECT_NE(noteOf({{"clumpShare", 1.0}}, false).find("clumpId"), std::string::npos);
    EXPECT_EQ(noteOf({{"clumpShare", 1.0}}, true).find("clumpId"), std::string::npos);
    EXPECT_EQ(noteOf(json::object(), false).find("clumpId"), std::string::npos);  // at its default: quiet
    const StrandsPtr s = colored({{"clumpShare", 1.0}, {"melaninRandom", 0.3}}, false);
    std::set<float> melanins(s->strandChannel(kMelaninChannel)->begin(), s->strandChannel(kMelaninChannel)->end());
    EXPECT_GT(melanins.size(), s->strandCount() / 2);
}

// The tip color takes its strand's hue and saturation draws, and tipOffsetRandom moves where it
// starts; the roots keep the strand's color.
TEST(HairColor, TheTipsVaryWithTheirStrand) {
    const StrandsPtr s = colored({{"mode", "color"}, {"color", {0.3, 0.3, 0.3}}, {"tipColor", {0.8, 0.4, 0.2}}, {"tipStrength", 1.0},
                                  {"hueRandom", 0.4}, {"clumpShare", 0.0}});
    std::set<int> tipHues;
    for (std::size_t i = 0; i < s->strandCount(); ++i) {
        const std::size_t tip = s->strandBegin(i) + s->strandPointCount(i) - 1;
        const Rgb c{(*s->pointChannel("colorR"))[tip], (*s->pointChannel("colorG"))[tip], (*s->pointChannel("colorB"))[tip]};
        tipHues.insert(static_cast<int>(std::round(100.0f * hueSaturation(c)[0])));
        EXPECT_NEAR((*s->pointChannel("colorR"))[s->strandBegin(i)], 0.3f, 1e-5f);  // grey roots stay grey
    }
    EXPECT_GT(tipHues.size(), 10u);
}

// A Map node's rgb output colors a color param per strand: a texture red on the left half and green
// on the right drives HairColor's color (mode color, times its own white), its dye (mode melanin)
// and its tipColor, the hosts' RGB along.
TEST(HairColor, AMapsRgbOutputColorsTheStrands) {
    const auto path = (std::filesystem::temp_directory_path() / "groomlab_rgb_split.ppm").generic_string();
    {
        std::ofstream f(path, std::ios::binary);
        f << "P6\n16 16\n255\n";
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                const char px[3] = {static_cast<char>(x < 8 ? 255 : 0), static_cast<char>(x < 8 ? 0 : 255), 0};
                f.write(px, 3);
            }
        }
    }
    auto withMap = [&](const json& params, const std::string& param) {
        json graph = json::parse(colorGroom(params, false).graphJson);
        graph["nodes"].push_back({{"name", "paint"}, {"type", "Map"}, {"params", {{"name", path}}}});
        for (json& n : graph["nodes"]) {
            if (n["name"] == "color") n["paramInputs"] = {param};
        }
        graph["connections"].push_back({{"from", "paint"}, {"output", "rgb"}, {"to", "color"}, {"input", param}});
        GroomData d;
        d.scalp = tintedPlane();
        d.graphJson = graph.dump();
        GroomSession session(d);
        const ViewResult& view = session.evaluate(ViewRequest{});
        EXPECT_TRUE(view.errors.empty()) << (view.errors.empty() ? "" : view.errors[0]);
        return view.strands;
    };
    auto sides = [](const StrandSet& s, const std::function<Rgb(std::size_t)>& colorOf) {
        std::size_t left = 0, right = 0, wrong = 0;
        for (std::size_t i = 0; i < s.strandCount(); ++i) {
            const float x = s.strandPoints(i)[0].x;
            if (std::abs(x - 0.5f) < 0.08f || x < 0.04f || x > 0.96f) continue;  // the texture's seams
            const Rgb c = colorOf(i);
            if (x < 0.5f) {
                (c[0] > 0.5f && c[1] < 0.05f) ? ++left : ++wrong;
            } else {
                (c[1] > 0.5f && c[0] < 0.05f) ? ++right : ++wrong;
            }
        }
        return std::array<std::size_t, 3>{left, right, wrong};
    };
    // Mode color: the map is the color (the param's white times it).
    const StrandsPtr picked = withMap({{"mode", "color"}, {"color", {1.0, 1.0, 1.0}}}, "color");
    ASSERT_TRUE(picked);
    const auto p = sides(*picked, [&](std::size_t i) { return strandColor(*picked, i); });
    EXPECT_GT(p[0], 20u);
    EXPECT_GT(p[1], 20u);
    EXPECT_EQ(p[2], 0u);
    // The exporters' and renderers' colors are the same.
    const std::vector<std::array<float, 3>> exported = strandColors(*picked);
    for (std::size_t i = 0; i < picked->strandCount(); ++i) EXPECT_EQ(exported[i], strandColor(*picked, i));
    // The param's own color multiplies it.
    const StrandsPtr half = withMap({{"mode", "color"}, {"color", {0.5, 0.5, 0.5}}}, "color");
    for (std::size_t i = 0; i < half->strandCount(); ++i) {
        EXPECT_NEAR(std::max(strandColor(*half, i)[0], strandColor(*half, i)[1]),
                    0.5f * std::max(strandColor(*picked, i)[0], strandColor(*picked, i)[1]), 1e-5f);
    }
    // Mode melanin: it dyes the pigment (no melanin: the dye itself).
    const StrandsPtr dyed = withMap({{"melanin", 0.0}, {"melaninRandom", 0.0}}, "dye");
    const auto d = sides(*dyed, [&](std::size_t i) { return strandColor(*dyed, i); });
    EXPECT_GT(d[0], 20u);
    EXPECT_GT(d[1], 20u);
    EXPECT_EQ(d[2], 0u);
    // tipColor: the tips take the map's color.
    const StrandsPtr tipped = withMap({{"mode", "color"}, {"color", {0.2, 0.2, 0.2}}, {"tipColor", {1.0, 1.0, 1.0}},
                                       {"tipStrength", 1.0}, {"tipProfile", {{0, 1}, {1, 1}}}},
                                      "tipColor");
    const auto t = sides(*tipped, [&](std::size_t i) {
        const std::size_t at = tipped->strandBegin(i) + tipped->strandPointCount(i) - 1;
        return Rgb{(*tipped->pointChannel("colorR"))[at], (*tipped->pointChannel("colorG"))[at], (*tipped->pointChannel("colorB"))[at]};
    });
    EXPECT_GT(t[0], 20u);
    EXPECT_GT(t[1], 20u);
    EXPECT_EQ(t[2], 0u);
    // The spec says which params take a color field (the hosts offer exposing them).
    std::set<std::string> colorParams;
    for (const ParamSpec& spec : OperatorRegistry::instance().create("HairColor")->paramSpecs()) {
        if (spec.colorField) colorParams.insert(spec.name);
    }
    EXPECT_EQ(colorParams, (std::set<std::string>{"color", "dye", "tipColor"}));
    std::filesystem::remove(path);
}

}  // namespace
}  // namespace groomlab
