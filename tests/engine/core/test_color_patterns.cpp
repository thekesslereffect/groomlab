// SPDX-License-Identifier: Apache-2.0
// The color patterns (color_patterns.h) and the nodes drawing them over HairColor's colors: Bands
// (bands along the strand with their per-strand randomness, guard hairs and undercoat) and
// FeatherColor (vane space from Propagate, every feather pattern, the painted feather image, the
// down zone and its color, the iridescence channel and tint); the two with nothing to do leaving
// the colors as they came. (HairColor itself: test_hair_color.cpp.)

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <cmath>
#include <filesystem>
#include <functional>
#include <set>

#include "groomlab/card_files.h"
#include "groomlab/cards.h"
#include "groomlab/color_patterns.h"
#include "groomlab/curve.h"
#include "groomlab/export_view.h"
#include "groomlab/groom_session.h"
#include "groomlab/hair_color.h"
#include "groomlab/operators.h"
#include "groomlab/scalp_paint.h"
#include "test_helpers.h"

#include <nlohmann/json.hpp>

namespace groomlab {
namespace {

using Rgb = std::array<float, 3>;

Value run(const char* type, const std::vector<Value>& inputs, const std::function<void(ParamSet&)>& set = {},
          std::shared_ptr<const ScalpMesh> scalp = nullptr) {
    auto node = OperatorRegistry::instance().create(type);
    if (set) set(node->params());
    ExternalInputs ext;
    if (scalp) ext.set("scalp", scalp, 1);
    EvalContext ctx;
    ctx.externals = &ext;
    return node->evaluate(ctx, inputs);
}

// Straight strands up from (x, 0, 0.5), `points` points each, `length` long.
std::shared_ptr<StrandSet> uprights(std::size_t count, int points = 21, const std::vector<float>& lengths = {}) {
    auto s = std::make_shared<StrandSet>();
    for (std::size_t i = 0; i < count; ++i) {
        const float length = i < lengths.size() ? lengths[i] : 1.0f;
        s->addStrand(500 + i, test::straightCurve({0.01f * static_cast<float>(i), 0.0f, 0.5f}, {0, length, 0}, points - 1));
    }
    return s;
}

Rgb pointColor(const StrandSet& s, std::size_t strand, std::size_t i) {
    const std::size_t p = s.strandBegin(strand) + i;
    return {(*s.pointChannel("colorR"))[p], (*s.pointChannel("colorG"))[p], (*s.pointChannel("colorB"))[p]};
}

Rgb strandColor(const StrandSet& s, std::size_t strand) {
    return {(*s.strandChannel("colorR"))[strand], (*s.strandChannel("colorG"))[strand], (*s.strandChannel("colorB"))[strand]};
}

// exp(-absorption): a fiber's color over its radius.
Rgb exp3(const Rgb& absorption) { return {std::exp(-absorption[0]), std::exp(-absorption[1]), std::exp(-absorption[2])}; }

float distance(const Rgb& a, const Rgb& b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// HairColor with no randomness, so every strand's own color is the pigment's.
void plain(ParamSet& p) {
    p.set("melaninRandom", 0.0);
    p.set("melanin", 0.7);
    p.set("redness", 0.1);
}

// HairColor (`hair`, plain by default) on `in`, then a node of `type` set by `set` over its colors.
StrandsPtr overHairColor(const char* type, const StrandsPtr& in, const std::function<void(ParamSet&)>& set,
                         std::shared_ptr<const ScalpMesh> scalp = nullptr,
                         const std::function<void(ParamSet&)>& hair = plain) {
    const Value colored = run(op::kHairColor, {in}, hair, scalp);
    return valueAs<StrandsPtr>(run(type, {colored}, set, scalp));
}

// Bands without their pigment's randomness, so every band is the band pigment's color.
void exactBands(ParamSet& p) { p.set("melaninRandom", 0.0); }

// ---- Bands along the strand ---------------------------------------------------------------------

// The look's lobes every host shades and renders with (create_hair_shader's aiStandardHair, Maya's
// viewport hair, the app's lookdev, Blender's fit): HairColor's specular against 0.3 the R lobe's
// tint, up to 1; its roughness Arnold's 0.1 to 0.5; its tilt Arnold's shift; aiStandardHair's
// defaults otherwise.
TEST(HairLook, LobesAreArnoldsForTheLook) {
    const HairLobes half = hairLobes(0.15f, 0.5f);
    EXPECT_FLOAT_EQ(half.specularTint, 0.5f);
    EXPECT_FLOAT_EQ(half.roughness, 0.3f);
    EXPECT_FLOAT_EQ(half.ior, 1.55f);
    EXPECT_FLOAT_EQ(half.shiftDegrees, 3.0f);
    EXPECT_FLOAT_EQ(half.azimuthalRoughness, 0.2f);
    EXPECT_FLOAT_EQ(hairLobes(0.9f, 0.0f).specularTint, 1.0f);
    EXPECT_FLOAT_EQ(hairLobes(0.9f, 0.0f).roughness, 0.1f);
    EXPECT_FLOAT_EQ(hairLobes(0.3f, 1.0f).roughness, 0.5f);
    EXPECT_FLOAT_EQ(hairLobes(0.3f, 0.5f, 7.0f).shiftDegrees, 7.0f);
    HairLook look;  // no HairColor: its defaults, 0.3, 0.5 and 3 degrees
    EXPECT_FLOAT_EQ(hairLobes(look).specularTint, 1.0f);
    EXPECT_FLOAT_EQ(hairLobes(look).roughness, 0.3f);
    EXPECT_FLOAT_EQ(hairLobes(look).shiftDegrees, kHairShiftDegrees);
    EXPECT_FLOAT_EQ(look.specular, kDefaultHairSpecular);
    EXPECT_FLOAT_EQ(look.roughness, kDefaultHairRoughness);
    EXPECT_FLOAT_EQ(look.tilt, kDefaultHairTilt);
    look.tilt = -2.0f;
    EXPECT_FLOAT_EQ(hairLobes(look).shiftDegrees, -2.0f) << "the tilt reaches the lobes (the viewport's uniforms)";
    // Wet hair scales the look's specular and roughness, keeps its tilt.
    EXPECT_FLOAT_EQ(wetLobes(hairLobes(look), 1.0f).shiftDegrees, -2.0f);
    // Rougher: wider lobes; more melanin: more absorbed, the blue most; none: nothing.
    EXPECT_LT(hairLongitudinalWidth(0.1f), hairLongitudinalWidth(0.5f));
    EXPECT_LT(hairAzimuthalSpread(0.1f), hairAzimuthalSpread(0.6f));
    const auto dark = hairMelaninAbsorption(1.0f, 0.0f);
    EXPECT_GT(dark[2], dark[1]);
    EXPECT_GT(dark[1], dark[0]);
    EXPECT_FLOAT_EQ(dark[0], 6.6f * 0.419f);
    for (float v : hairMelaninAbsorption(0.0f, 0.5f)) EXPECT_FLOAT_EQ(v, 0.0f);
    // A picked RGB as aiStandardHair absorbs its base color (Chiang et al. 2016): white absorbs
    // nothing; darker absorbs more, channel by channel, (ln c / D(b))^2 with D(0.2) = 5.950; a
    // rougher fiber (azimuthally) a little more. Far less than the color's own transmittance's
    // -ln c: a fiber's color is what many scatterings through the groom settle on, not one pass.
    for (float v : hairColorAbsorption({1.0f, 1.0f, 1.0f})) EXPECT_FLOAT_EQ(v, 0.0f);
    const auto skin = hairColorAbsorption({0.8f, 0.56f, 0.45f}, 0.2f);
    EXPECT_LT(skin[0], skin[1]);
    EXPECT_LT(skin[1], skin[2]);
    const float d = 5.969f - 0.215f * 0.2f + 2.532f * 0.04f - 10.73f * 0.008f + 5.574f * 0.0016f + 0.245f * 0.00032f;
    EXPECT_NEAR(d, 5.950f, 1e-3f);
    EXPECT_NEAR(skin[2], std::pow(std::log(0.45f) / d, 2.0f), 1e-6f);
    EXPECT_LT(skin[2], 0.05f * -std::log(0.45f));
    EXPECT_GT(hairColorAbsorption({0.5f, 0.5f, 0.5f}, 0.6f)[0], hairColorAbsorption({0.5f, 0.5f, 0.5f}, 0.2f)[0]);
}

// Wet hair (Wet's wetness channel, hair_color.h wetShading), as every host shades it: dry is
// exactly dry; soaked, the fiber's color darker and more saturated (its absorption doubled: a
// brown fiber's by up to a half), the highlight a quarter stronger and Arnold's roughness halved.
TEST(HairLook, WetHairIsDarkerMoreSaturatedAndGlossier) {
    const WetShading dry = wetShading(0.0f);
    EXPECT_EQ(dry.absorption, 1.0f);
    EXPECT_EQ(dry.specular, 1.0f);
    EXPECT_EQ(dry.roughness, 1.0f);
    const WetShading soaked = wetShading(1.0f);
    EXPECT_FLOAT_EQ(soaked.absorption, 2.0f);
    EXPECT_FLOAT_EQ(soaked.specular, 1.25f);
    EXPECT_FLOAT_EQ(soaked.roughness, 0.5f);
    EXPECT_FLOAT_EQ(wetShading(0.5f).roughness, 0.75f);  // linear between
    EXPECT_EQ(wetShading(2.0f).roughness, soaked.roughness);  // clamped
    // A blonde and a brown fiber: darker, the blue lost the most (more saturated); the brown's
    // green and blue by 40-65%. A mid gray 0.5 to 0.25; white stays white.
    const std::array<float, 3> blonde = exp3(hairMelaninAbsorption(0.12f, 0.45f));
    const std::array<float, 3> brown = exp3(hairMelaninAbsorption(0.35f, 0.2f));
    EXPECT_EQ(wetFiberColor(blonde, 0.0f), blonde);
    for (const auto& fiber : {blonde, brown}) {
        const std::array<float, 3> wet = wetFiberColor(fiber, 1.0f);
        for (int k = 0; k < 3; ++k) EXPECT_LT(wet[k], fiber[k]) << k;
        EXPECT_LT(wet[2] / wet[0], fiber[2] / fiber[0]);  // more saturated
    }
    const std::array<float, 3> wetBrown = wetFiberColor(brown, 1.0f);
    EXPECT_LT(wetBrown[1], 0.62f * brown[1]);
    EXPECT_GT(wetBrown[1], 0.35f * brown[1]);
    EXPECT_FLOAT_EQ(wetFiberColor({0.5f, 0.5f, 0.5f}, 1.0f)[0], 0.25f);
    EXPECT_FLOAT_EQ(wetFiberColor({1.0f, 1.0f, 1.0f}, 1.0f)[0], 1.0f);
    const HairLobes lobes = hairLobes(0.3f, 0.5f), wet = wetLobes(lobes, 1.0f);
    EXPECT_FLOAT_EQ(wet.specularTint, 1.25f * lobes.specularTint);
    EXPECT_FLOAT_EQ(wet.roughness, 0.5f * lobes.roughness);
    EXPECT_EQ(wetLobes(lobes, 0.0f).roughness, lobes.roughness);
    // The shader's numbers: the soaked shares, and the highlight's width at half the roughness.
    const std::array<float, 4> uniform = wetShadingUniform(lobes.roughness);
    EXPECT_FLOAT_EQ(uniform[0], kWetAbsorption);
    EXPECT_FLOAT_EQ(uniform[1], kWetSpecular);
    EXPECT_FLOAT_EQ(uniform[2], hairLongitudinalWidth(0.5f * lobes.roughness) / hairLongitudinalWidth(lobes.roughness));
    EXPECT_LT(uniform[2], 0.55f);
    EXPECT_GT(uniform[2], 0.35f);
    // Packed with the iridescence for the lit strands' colors, unpacked as hair_bsdf.glsl's
    // hairLookAlpha does: 4 and over, never what a dry color holds (under 2).
    auto unpack = [](float a) {
        const float x = a - 4.0f, q = std::floor(x);
        return std::array<float, 2>{2.0f * (x - q), q * 0.001f};
    };
    for (const float iridescence : {0.0f, 0.3f, 1.0f}) {
        for (const float wetness : {0.0f, 0.25f, 0.7f, 1.0f}) {
            const float a = packWetLook(iridescence, wetness);
            EXPECT_GE(a, kWetPacked);
            const std::array<float, 2> back = unpack(a);
            EXPECT_NEAR(back[0], iridescence, 1e-3f) << iridescence << " " << wetness;
            EXPECT_NEAR(back[1], wetness, 1e-3f) << iridescence << " " << wetness;
        }
    }
}

TEST(ColorPatterns, BandsRepeatTheProfileAndHoldItsEnds) {
    const Ramp profile{{0.0f, 0.0f}, {0.5f, 1.0f}, {1.0f, 0.0f}};

    for (const float t : {0.0f, 0.25f, 0.5f, 0.8f, 1.0f}) EXPECT_NEAR(bandAt(profile, t, 1), profile(t), 1e-6f) << t;
    // Three periods: the profile three times over, each a third of the strand.
    EXPECT_NEAR(bandAt(profile, 1.0f / 6.0f, 3), profile(0.5f), 1e-5f);
    EXPECT_NEAR(bandAt(profile, 0.5f, 3), profile(0.5f), 1e-5f);
    EXPECT_NEAR(bandAt(profile, 5.0f / 6.0f, 3), profile(0.5f), 1e-5f);
    // A shifted single band moves along the strand; past the pattern the ends hold.
    EXPECT_NEAR(bandAt(profile, 0.6f, 1, 1.0f, -0.1f), profile(0.5f), 1e-5f);
    EXPECT_NEAR(bandAt(profile, 0.05f, 1, 1.0f, -0.1f), profile(0.0f), 1e-6f);
    EXPECT_NEAR(bandAt(profile, 0.95f, 1, 1.0f, 0.2f), profile(1.0f), 1e-6f);
    // Stretched: the band's middle moves out with the scale.
    EXPECT_NEAR(bandAt(profile, 0.25f, 1, 2.0f), profile(0.5f), 1e-5f);
}

TEST(Bands, BandsTakeTheBandPigmentWhereTheProfileSays) {
    const auto in = uprights(4);
    const StrandsPtr out = overHairColor(op::kBands, in, [](ParamSet& p) {
        exactBands(p);
        p.set("offsetRandom", 0.0);
        p.set("widthRandom", 0.0);
        p.set("profile", Ramp{{0.0f, 0.0f}, {0.4f, 0.0f}, {0.45f, 1.0f}, {0.65f, 1.0f}, {0.7f, 0.0f}, {1.0f, 0.0f}});
    });
    ASSERT_TRUE(out->pointChannel("colorR"));
    const Rgb base = melaninColor(0.7f, 0.1f), band = melaninColor(0.08f, 0.6f);
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        EXPECT_LT(distance(strandColor(*out, s), base), 1e-5f);  // the strand's own color is the hair's
        EXPECT_LT(distance(pointColor(*out, s, 0), base), 1e-5f);    // t 0
        EXPECT_LT(distance(pointColor(*out, s, 11), band), 1e-5f);   // t 0.55: in the band
        EXPECT_LT(distance(pointColor(*out, s, 20), base), 1e-5f);   // t 1
    }
    // The pigment stays per strand for the melanin readers.
    EXPECT_TRUE(out->strandChannel(kMelaninChannel));
    // The band pigment's randomness: each strand's band a little other, stable by ID.
    const StrandsPtr varied = overHairColor(op::kBands, in, [](ParamSet& p) {
        p.set("randomness", 0.2);
        p.set("offsetRandom", 0.0);
        p.set("widthRandom", 0.0);
        p.set("profile", Ramp{{0.0f, 0.0f}, {0.4f, 0.0f}, {0.45f, 1.0f}, {0.65f, 1.0f}, {0.7f, 0.0f}, {1.0f, 0.0f}});
    });
    std::set<float> bandGreens;
    for (std::size_t s = 0; s < varied->strandCount(); ++s) {
        bandGreens.insert(pointColor(*varied, s, 11)[1]);
        EXPECT_LT(distance(pointColor(*varied, s, 0), base), 1e-5f);  // the hair's own color stays
    }
    EXPECT_EQ(bandGreens.size(), varied->strandCount());
}

TEST(Bands, OffsetRandomShiftsEachStrandsBandsStably) {
    const auto in = uprights(40, 41);
    auto banded = [&](double jitter, double widthJitter) {
        return overHairColor(op::kBands, in, [&](ParamSet& p) {
            exactBands(p);
            p.set("count", std::int64_t{3});
            p.set("offsetRandom", jitter);
            p.set("widthRandom", widthJitter);
        });
    };
    // Where each strand's first band is strongest (its point index).
    const Rgb band = melaninColor(0.08f, 0.6f);
    auto firstBand = [&](const StrandSet& s, std::size_t strand) {
        std::size_t best = 0;
        float closest = 1e9f;
        for (std::size_t i = 0; i < 14; ++i) {
            const float d = distance(pointColor(s, strand, i), band);
            if (d < closest) closest = d, best = i;
        }
        return best;
    };
    const StrandsPtr still = banded(0.0, 0.0);
    std::set<std::size_t> stillPlaces, jitteredPlaces;
    for (std::size_t s = 0; s < 40; ++s) stillPlaces.insert(firstBand(*still, s));
    EXPECT_EQ(stillPlaces.size(), 1u);  // every band lines up across the coat
    const StrandsPtr jittered = banded(0.3, 0.2);
    for (std::size_t s = 0; s < 40; ++s) jitteredPlaces.insert(firstBand(*jittered, s));
    EXPECT_GE(jitteredPlaces.size(), 4u);  // they no longer do
    // The same strands, the same bands: by ID, not order.
    const StrandsPtr again = banded(0.3, 0.2);
    EXPECT_EQ(*again->pointChannel("colorG"), *jittered->pointChannel("colorG"));
    auto reversed = std::make_shared<StrandSet>();
    for (std::size_t s = 40; s-- > 0;) reversed->addStrand(in->ids()[s], std::vector<Vec3f>(in->strandPoints(s), in->strandPoints(s) + 41));
    const StrandsPtr flipped = overHairColor(op::kBands, reversed, [&](ParamSet& p) {
        exactBands(p);
        p.set("count", std::int64_t{3});
        p.set("offsetRandom", 0.3);
        p.set("widthRandom", 0.2);
    });
    for (std::size_t s = 0; s < 40; ++s) EXPECT_EQ(firstBand(*flipped, 39 - s), firstBand(*jittered, s));
}

TEST(Bands, GuardHairsTakeTheBandsAndTheUndercoatStaysPlain) {
    // Lengths 0.5 .. 1.5: by length, the longest half are the guard hairs.
    std::vector<float> lengths;
    for (int i = 0; i < 20; ++i) lengths.push_back(0.5f + 0.05f * static_cast<float>((i * 7) % 20));
    const auto in = uprights(20, 21, lengths);
    auto colored = [&](const char* by) {
        // HairColor's tip on every strand: the undercoat loses it.
        return overHairColor(op::kBands, in, [&](ParamSet& p) {
            exactBands(p);
            p.set("offsetRandom", 0.0);
            p.set("widthRandom", 0.0);
            p.set("guards", 0.5);
            p.set("guardsBy", std::string(by));
            p.set("undercoatDye", Vec3f(0.5f, 0.5f, 0.5f));
        }, nullptr, [](ParamSet& p) {
            plain(p);
            p.set("tipStrength", 1.0);
            p.set("tipColor", Vec3f(0.0f, 0.0f, 1.0f));
        });
    };
    const Rgb base = melaninColor(0.7f, 0.1f);
    const Rgb under{base[0] * 0.5f, base[1] * 0.5f, base[2] * 0.5f};
    const StrandsPtr longest = colored("longest");
    int guards = 0;
    for (std::size_t s = 0; s < 20; ++s) {
        const bool guard = lengths[s] >= 1.0f - 1e-4f;
        guards += guard ? 1 : 0;
        if (guard) {
            EXPECT_GT(distance(pointColor(*longest, s, 14), base), 0.05f) << s;  // banded
            EXPECT_NEAR(pointColor(*longest, s, 20)[2], 1.0f, 1e-5f);           // tipped
        } else {
            for (const std::size_t i : {0u, 14u, 20u}) EXPECT_LT(distance(pointColor(*longest, s, i), under), 1e-5f) << s;
            EXPECT_LT(distance(strandColor(*longest, s), under), 1e-5f);
        }
    }
    EXPECT_EQ(guards, 10);
    // At random: about half, picked by ID.
    const StrandsPtr random = colored("random");
    int banded = 0;
    for (std::size_t s = 0; s < 20; ++s) banded += distance(pointColor(*random, s, 20), under) > 1e-3f ? 1 : 0;
    EXPECT_GT(banded, 4);
    EXPECT_LT(banded, 16);
}

// ---- Vane space ---------------------------------------------------------------------------------

// A 2 x 2 plane scalp (normal +Y) and shafts lying on it along +X.
struct Shafts {
    MeshPtr scalp = std::make_shared<ScalpMesh>(*test::makePlane(8, 2.0f));
    StrandsPtr shafts;

    explicit Shafts(int count = 1) {
        auto s = std::make_shared<StrandSet>();
        for (int k = 0; k < count; ++k) {
            std::vector<Vec3f> pts;
            const float z = 0.3f + 0.5f * static_cast<float>(k);
            for (int i = 0; i <= 10; ++i) pts.push_back({-0.5f + 0.1f * static_cast<float>(i), 0.0f, z});
            s->addStrand(70 + k, pts, std::vector<float>(pts.size(), 0.02f), scalp->closestPoint(pts[0]).root);
        }
        shafts = s;
    }
    StrandsPtr propagate(const std::function<void(ParamSet&)>& set = {}) const {
        return valueAs<StrandsPtr>(run(op::kPropagate, {shafts}, [&](ParamSet& p) {
            p.set("count", std::int64_t{20});
            p.set("lengthRandom", 0.0);
            p.set("angleRandom", 0.0);
            p.set("curl", 0.0);
            p.set("droop", 0.0);
            if (set) set(p);
        }, scalp));
    }
};

float ch(const StrandSet& s, const char* name, std::size_t i) { return (*s.strandChannel(name))[i]; }

TEST(Propagate, VaneSpaceLaysEveryBarbOutFlat) {
    const Shafts f;
    const Ramp outline{{0.0f, 0.5f}, {0.5f, 1.0f}, {1.0f, 0.25f}};
    const StrandsPtr out = f.propagate([&](ParamSet& p) {
        p.set("angle", 90.0);  // straight out: a barb's tip is as far along as its root
        p.set("lengthProfile", outline);
        p.set("length", 0.3);
    });
    const VaneSpace vane(*out);
    ASSERT_TRUE(vane.available());
    // The shaft runs up the middle, root to tip (nothing reaches past it).
    EXPECT_EQ(ch(*out, "isShaft", 0), 1.0f);
    EXPECT_FLOAT_EQ(ch(*out, kVaneUChannel, 0), 0.0f);
    EXPECT_NEAR(ch(*out, kVaneTipUChannel, 0), 1.0f, 1e-5f);
    EXPECT_FLOAT_EQ(ch(*out, kVaneTipVChannel, 0), 0.0f);
    float widest = 0.0f;
    for (std::size_t s = 1; s < out->strandCount(); ++s) {
        const float u = ch(*out, "shaftU", s);
        EXPECT_NEAR(ch(*out, kVaneUChannel, s), u, 1e-5f);     // where it leaves the shaft
        EXPECT_NEAR(ch(*out, kVaneTipUChannel, s), u, 1e-4f);  // straight out
        EXPECT_NEAR(ch(*out, kVaneTipVChannel, s), outline(u) / 1.0f, 0.03f);  // its length over the widest's
        widest = std::max(widest, ch(*out, kVaneTipVChannel, s));
        // A point halfway along the barb, halfway out.
        const std::size_t n = out->strandPointCount(s);
        const VanePoint mid = vane.at(s, (n - 1) / 2, n);
        EXPECT_NEAR(mid.v, 0.5f * ch(*out, kVaneTipVChannel, s), 1e-5f);
        EXPECT_EQ(mid.side, static_cast<int>(ch(*out, "side", s)));
        EXPECT_TRUE(mid.barb);
        EXPECT_NEAR(mid.along, 0.5f, 1e-5f);
    }
    EXPECT_NEAR(widest, 1.0f, 1e-5f);
    // Leaning toward the tip, the tips reach past the shaft's end: u is the feather's reach.
    const StrandsPtr leaning = f.propagate([&](ParamSet& p) {
        p.set("angle", 30.0);
        p.set("length", 0.5);
        p.set("end", 0.95);
    });
    float farthest = 0.0f;
    for (std::size_t s = 1; s < leaning->strandCount(); ++s) {
        farthest = std::max(farthest, ch(*leaning, kVaneTipUChannel, s));
        EXPECT_GT(ch(*leaning, kVaneTipUChannel, s), ch(*leaning, kVaneUChannel, s));
    }
    EXPECT_NEAR(farthest, 1.0f, 1e-5f);
    EXPECT_LT(ch(*leaning, kVaneTipUChannel, 0), 1.0f);  // the shaft stops short of it
}

TEST(Propagate, VaneSpaceIsTheBarbsGeometryLaidFlat) {
    // Straight barbs leaning toward the tip in the scalp's plane: vane space is their positions
    // scaled, the same scale for every barb along and across.
    const Shafts f;
    const StrandsPtr out = f.propagate([](ParamSet& p) {
        p.set("angle", 40.0);
        p.set("angleProfile", Ramp{{0.0f, 1.3f}, {1.0f, 0.6f}});
        p.set("length", 0.3);
    });
    float along = -1.0f, across = -1.0f;
    for (std::size_t s = 1; s < out->strandCount(); ++s) {
        const Vec3f* pts = out->strandPoints(s);
        const Vec3f d = pts[out->strandPointCount(s) - 1] - pts[0];
        const float du = ch(*out, kVaneTipUChannel, s) - ch(*out, kVaneUChannel, s), dv = ch(*out, kVaneTipVChannel, s);
        ASSERT_GT(du, 0.0f);
        const float a = d.x / du, c = std::abs(d.z) / dv;  // the shaft runs along +x, the vane across z
        if (along < 0.0f) along = a, across = c;
        EXPECT_NEAR(a, along, 1e-3f * along) << s;
        EXPECT_NEAR(c, across, 1e-3f * across) << s;
    }
    // Every strand's normal is the vane's (here the scalp's, the feather lying on it); hair has none.
    const VaneNormals normals(*out);
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        Vec3f n;
        ASSERT_TRUE(normals.at(s, &n)) << s;
        EXPECT_NEAR(n.y, 1.0f, 1e-4f) << s;
    }
    Vec3f none;
    EXPECT_FALSE(VaneNormals(*uprights(2)).at(0, &none));
}

TEST(Propagate, AFeatherStandingOffTheSkinFacesItsOwnWay) {
    // A shaft along the scalp's normal: its vane faces world +Z (Propagate's rule), and so do its
    // strands' normals, whatever the scalp's.
    auto scalp = std::make_shared<ScalpMesh>(*test::makePlane(8, 2.0f));
    auto shaft = std::make_shared<StrandSet>();
    std::vector<Vec3f> pts;
    for (int i = 0; i <= 10; ++i) pts.push_back({0.1f, 0.1f * static_cast<float>(i), 0.1f});
    shaft->addStrand(5, pts, std::vector<float>(pts.size(), 0.02f), scalp->closestPoint(pts[0]).root);
    const StrandsPtr out = valueAs<StrandsPtr>(run(op::kPropagate, {StrandsPtr(shaft)}, [](ParamSet& p) {
        p.set("count", std::int64_t{6});
    }, scalp));
    const VaneNormals normals(*out);
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        Vec3f n;
        ASSERT_TRUE(normals.at(s, &n));
        EXPECT_NEAR(std::abs(n.z), 1.0f, 1e-3f) << s;
    }
}

TEST(Propagate, VaneSpaceComesFromGuidesTooAndTheSameFeatherSharesItsKey) {
    const Shafts f(2);
    const StrandsPtr out = f.propagate();
    const VaneSpace vane(*out);
    std::set<std::uint64_t> keys;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        ASSERT_TRUE(vane.inFeather(s));
        keys.insert(vane.featherKey(s));
    }
    EXPECT_EQ(keys.size(), 2u);  // one per feather
    // Hair beside the feathers (a Merge) has no feather.
    auto hair = uprights(2);
    hair->addStrandChannel("featherId", -1.0f);
    hair->addStrandChannel(kVaneUChannel, 0.0f);
    hair->addStrandChannel(kVaneTipUChannel, 0.0f);
    hair->addStrandChannel(kVaneTipVChannel, 0.0f);
    EXPECT_FALSE(VaneSpace(*hair).inFeather(0));
}

// ---- Patterns -----------------------------------------------------------------------------------

VanePoint at(float u, float v, int side, float along = 0.5f) {
    VanePoint p;
    p.u = u;
    p.v = v;
    p.side = side;
    p.along = along;
    p.barb = true;
    return p;
}

TEST(ColorPatterns, BarsCrossTheVaneEveryPeriod) {
    VanePatternParams p;
    p.kind = VanePattern::Bars;
    p.count = 4.0f;  // a bar every quarter of the feather
    p.width = 0.5f;
    p.offset = 0.0f;
    p.sharpness = 1.0f;
    const FeatherJitter none;
    for (const float u : {0.0f, 0.25f, 0.5f, 0.75f, 0.06f}) EXPECT_EQ(vanePatternMask(p, at(u, 0.4f, -1), none, 1), 1.0f) << u;
    for (const float u : {0.125f, 0.375f, 0.1f}) EXPECT_EQ(vanePatternMask(p, at(u, 0.4f, 1), none, 1), 0.0f) << u;
    // Straight across: the same on both vanes and all the way out.
    EXPECT_EQ(vanePatternMask(p, at(0.05f, 0.0f, 1), none, 1), vanePatternMask(p, at(0.05f, 1.0f, -1), none, 1));
    // Tilted, a bar crosses each barb at another place along it.
    p.angle = 0.6f;
    EXPECT_NE(vanePatternMask(p, at(0.1f, 0.0f, 1), none, 1), vanePatternMask(p, at(0.1f, 1.0f, 1), none, 1));
    // Soft edges fade.
    p.angle = 0.0f;
    p.sharpness = 0.0f;
    const float edge = vanePatternMask(p, at(0.05f, 0.5f, 1), none, 1);
    EXPECT_GT(edge, 0.0f);
    EXPECT_LT(edge, 1.0f);
}

TEST(ColorPatterns, ChevronsMirrorAcrossTheShaft) {
    VanePatternParams p;
    p.kind = VanePattern::Chevrons;
    p.count = 5.0f;
    p.angle = 0.5f;
    p.offset = 0.0f;
    for (const float u : {0.1f, 0.33f, 0.71f}) {
        for (const float v : {0.2f, 0.7f}) {
            EXPECT_EQ(vanePatternMask(p, at(u, v, -1), {}, 3), vanePatternMask(p, at(u, v, 1), {}, 3));
        }
    }
    // A V: a point on the shaft and one out on the vane at the same u sit at different places in it.
    int differ = 0;
    for (int i = 0; i < 20; ++i) {
        const float u = 0.05f * static_cast<float>(i);
        differ += vanePatternMask(p, at(u, 0.0f, 1), {}, 3) != vanePatternMask(p, at(u, 0.9f, 1), {}, 3) ? 1 : 0;
    }
    EXPECT_GT(differ, 4);
}

TEST(ColorPatterns, SpotsCoverAboutTheirShareAndDifferPerFeather) {
    VanePatternParams p;
    p.kind = VanePattern::Spots;
    p.count = 8.0f;
    p.width = 0.5f;
    p.sharpness = 1.0f;
    auto coverage = [&](std::uint64_t key) {
        int in = 0, all = 0;
        for (int i = 0; i < 100; ++i) {
            for (int j = 0; j < 40; ++j) {
                in += vanePatternMask(p, at(0.01f * static_cast<float>(i), 0.025f * static_cast<float>(j), 1), {}, key) > 0.5f ? 1 : 0;
                ++all;
            }
        }
        return static_cast<float>(in) / static_cast<float>(all);
    };
    // A spot of diameter half a cell in each cell: pi / 16 of the area (less what overlaps).
    const float share = coverage(11);
    EXPECT_GT(share, 0.12f);
    EXPECT_LT(share, 0.26f);
    int differ = 0;
    for (int i = 0; i < 50; ++i) {
        const VanePoint q = at(0.02f * static_cast<float>(i), 0.3f, 1);
        differ += vanePatternMask(p, q, {}, 11) != vanePatternMask(p, q, {}, 12) ? 1 : 0;
    }
    EXPECT_GT(differ, 3);  // another feather's spots sit elsewhere
}

TEST(ColorPatterns, FringeIsTheEdgeAndAnEyeIsRound) {
    VanePatternParams fringe;
    fringe.kind = VanePattern::Fringe;
    fringe.width = 0.2f;
    fringe.sharpness = 1.0f;
    EXPECT_EQ(vanePatternMask(fringe, at(0.5f, 0.9f, 1, 0.95f), {}, 1), 1.0f);  // near a barb's tip: the vane's edge
    EXPECT_EQ(vanePatternMask(fringe, at(0.5f, 0.5f, 1, 0.5f), {}, 1), 0.0f);
    VanePoint shaft = at(0.5f, 0.0f, 0, 1.0f);
    shaft.barb = false;
    EXPECT_EQ(vanePatternMask(fringe, shaft, {}, 1), 0.0f);  // the shaft has no edge

    VanePatternParams eye;
    eye.kind = VanePattern::Eye;
    eye.width = 0.3f;    // 0.15 of the feather's length across
    eye.offset = 0.75f;  // its center
    eye.sharpness = 1.0f;
    EXPECT_EQ(vanePatternMask(eye, at(0.75f, 0.0f, 1), {}, 1), 1.0f);
    EXPECT_EQ(vanePatternMask(eye, at(0.85f, 0.0f, 1), {}, 1), 1.0f);
    EXPECT_EQ(vanePatternMask(eye, at(0.95f, 0.0f, 1), {}, 1), 0.0f);
    // Round in feather lengths: 0.1 across the shaft is 0.3 of a vane's half-width (kVaneAspect 3).
    EXPECT_EQ(vanePatternMask(eye, at(0.75f, 0.1f * kVaneAspect * 1.4f, -1), {}, 1), 1.0f);
    EXPECT_EQ(vanePatternMask(eye, at(0.75f, 0.1f * kVaneAspect * 1.6f, -1), {}, 1), 0.0f);
    // Rings from the profile: a pupil and a ring with a gap between.
    eye.profile = Ramp{{0.0f, 1.0f}, {0.35f, 1.0f}, {0.4f, 0.0f}, {0.65f, 0.0f}, {0.7f, 1.0f}, {1.0f, 1.0f}};
    eye.profile.straight = true;
    EXPECT_NEAR(vanePatternMask(eye, at(0.75f + 0.15f * 0.2f, 0.0f, 1), {}, 1), 1.0f, 1e-5f);
    EXPECT_NEAR(vanePatternMask(eye, at(0.75f + 0.15f * 0.5f, 0.0f, 1), {}, 1), 0.0f, 1e-5f);
    EXPECT_NEAR(vanePatternMask(eye, at(0.75f + 0.15f * 0.85f, 0.0f, 1), {}, 1), 1.0f, 1e-5f);
}

TEST(ColorPatterns, JitterIsPerFeatherAndStable) {
    VanePatternParams p;
    p.kind = VanePattern::Bars;
    p.count = 5.0f;
    p.jitter = 0.5f;
    p.scaleJitter = 0.2f;
    p.rotationJitter = 0.1f;
    const FeatherJitter a = featherJitter(p, 101), b = featherJitter(p, 202), again = featherJitter(p, 101);
    EXPECT_EQ(a.offset, again.offset);
    EXPECT_EQ(a.scale, again.scale);
    EXPECT_EQ(a.rotation, again.rotation);
    EXPECT_TRUE(a.offset != b.offset || a.scale != b.scale);
    EXPECT_LE(std::abs(a.offset), 0.5f / (5.0f * a.scale) + 1e-6f);  // within half a period
    EXPECT_LE(std::abs(a.scale - 1.0f), 0.2f + 1e-6f);
    EXPECT_LE(std::abs(a.rotation), 0.1f + 1e-6f);
    p.seed = 9;
    EXPECT_NE(featherJitter(p, 101).offset, a.offset);
}

TEST(ColorPatterns, PatternNamesAndVanes) {
    for (const std::string& name : vanePatternNames()) {
        VanePattern kind;
        EXPECT_TRUE(parseVanePattern(name, &kind)) << name;
    }
    VanePattern kind;
    EXPECT_FALSE(parseVanePattern("stripes", &kind));
    VaneSides sides;
    ASSERT_TRUE(parseVaneSides("left", &sides));
    EXPECT_TRUE(onVane(sides, -1));
    EXPECT_FALSE(onVane(sides, 1));
    EXPECT_FALSE(onVane(sides, 0));
    ASSERT_TRUE(parseVaneSides("both", &sides));
    EXPECT_TRUE(onVane(sides, 0));
    EXPECT_FALSE(parseVaneSides("outer", &sides));
}

// ---- Feather colors -----------------------------------------------------------------------------

TEST(FeatherColor, BarsOnOneVaneOfEachFeather) {
    const Shafts f(2);
    const StrandsPtr feathers = f.propagate([](ParamSet& p) { p.set("segments", std::int64_t{10}); });
    const StrandsPtr out = overHairColor(op::kFeatherColor, feathers, [](ParamSet& p) {
        p.set("pattern", std::string("bars"));
        p.set("vanes", std::string("left"));
        p.set("count", 5.0);
        p.set("width", 0.5);
        p.set("sharpness", 1.0);
        p.set("offsetRandom", 0.0);
        p.set("scaleRandom", 0.0);
        p.set("angleRandom", 0.0);
    }, f.scalp);
    ASSERT_TRUE(out->pointChannel("colorR"));
    const Rgb base = melaninColor(0.7f, 0.1f), white = melaninColor(0.0f, 0.0f);
    int barred = 0, plainPoints = 0;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const float side = ch(*out, "side", s);
        for (std::size_t i = 0; i < out->strandPointCount(s); ++i) {
            const Rgb c = pointColor(*out, s, i);
            if (side > 0 || ch(*out, "isShaft", s) > 0.5f) {
                EXPECT_LT(distance(c, base), 1e-5f);  // the right vane and the shaft: the hair's own color
            } else if (distance(c, white) < 1e-5f) {
                ++barred;
            } else {
                EXPECT_LT(distance(c, base), 1e-5f);
                ++plainPoints;
            }
        }
    }
    EXPECT_GT(barred, 40);  // half the left vane, give or take
    EXPECT_GT(plainPoints, 40);
}

TEST(FeatherColor, PaintedFeatherImagesReadTheVaneLaidFlat) {
    const auto folder = std::filesystem::temp_directory_path() / "groomlab_feather_texture";
    std::filesystem::create_directories(folder);
    // The whole feather: left half red, right half green; the right vane's own image blue.
    const std::string whole = (folder / "whole.png").string(), right = (folder / "right.png").string();
    std::vector<std::uint8_t> px;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 16; ++x) {
            const bool left = x < 8;
            for (const int v : {left ? 255 : 0, left ? 0 : 255, 0, 255}) px.push_back(static_cast<std::uint8_t>(v));
        }
    }
    writePng(whole, 16, 8, 4, px);
    writePng(right, 1, 1, 4, {0, 0, 255, 255});

    const Shafts f;
    const StrandsPtr feathers = f.propagate();
    auto colored = [&](const std::string& rightImage) {
        return overHairColor(op::kFeatherColor, feathers, [&](ParamSet& p) {
            p.set("pattern", std::string("texture"));
            p.set("texture", whole);
            p.set("textureRight", rightImage);
        }, f.scalp);
    };
    const StrandsPtr out = colored("");
    for (std::size_t s = 1; s < out->strandCount(); ++s) {
        const std::size_t n = out->strandPointCount(s);
        const Rgb tip = pointColor(*out, s, n - 1);
        if (ch(*out, "side", s) < 0) EXPECT_GT(tip[0], 0.9f) << s;
        else EXPECT_GT(tip[1], 0.9f) << s;
    }
    const StrandsPtr perSide = colored(right);
    for (std::size_t s = 1; s < perSide->strandCount(); ++s) {
        const Rgb tip = pointColor(*perSide, s, perSide->strandPointCount(s) - 1);
        const float v = ch(*perSide, kVaneTipVChannel, s);
        if (ch(*perSide, "side", s) > 0) EXPECT_GT(tip[2], 0.9f) << s;
        else if (v < 0.45f) EXPECT_GT(tip[0], 0.9f) << s;  // the left vane's image: the shaft at its left edge
        else if (v > 0.55f) EXPECT_GT(tip[1], 0.9f) << s;
    }
    EXPECT_THROW(run(op::kFeatherColor, {feathers}, [&](ParamSet& p) {
        p.set("pattern", std::string("texture"));
        p.set("texture", (folder / "missing.png").string());
    }, f.scalp), GraphError);
}

TEST(FeatherColor, PatternsLeaveHairAlone) {
    const auto in = uprights(3);
    const StrandsPtr out = overHairColor(op::kFeatherColor, in, [](ParamSet& p) { p.set("pattern", std::string("spots")); });
    EXPECT_FALSE(out->pointChannel("colorR"));  // no feathers: no pattern, one color a strand
    for (std::size_t s = 0; s < 3; ++s) EXPECT_LT(distance(strandColor(*out, s), melaninColor(0.7f, 0.1f)), 1e-5f);
}

// ---- The down zone ------------------------------------------------------------------------------

TEST(Propagate, ADownZoneGrowsSoftCurlyBarbsAtTheBase) {
    const Shafts f;
    const StrandsPtr out = f.propagate([](ParamSet& p) {
        p.set("start", 0.1);
        p.set("down", 0.4);
        p.set("downCount", std::int64_t{8});
        p.set("downSegments", std::int64_t{16});
        p.set("downCurl", 300.0);
        p.set("downFrizz", 20.0);
    });
    std::string error;
    ASSERT_TRUE(out->validate(&error)) << error;
    ASSERT_TRUE(out->strandChannel(kIsDownChannel));
    int down = 0, vane = 0;
    float straightness = 0.0f;
    for (std::size_t s = 1; s < out->strandCount(); ++s) {
        const float u = ch(*out, "shaftU", s);
        const Vec3f* pts = out->strandPoints(s);
        const std::size_t n = out->strandPointCount(s);
        if (ch(*out, kIsDownChannel, s) > 0.5f) {
            ++down;
            EXPECT_EQ(n, 17u);
            EXPECT_GE(u, 0.1f);
            EXPECT_LE(u, 0.4f);
            straightness += (pts[n - 1] - pts[0]).length() / arcLength(pts, n);
            // Out of the vane: frizz lifts it off the plane somewhere.
            float off = 0.0f;
            for (std::size_t i = 0; i < n; ++i) off = std::max(off, std::abs(pts[i].y));
            EXPECT_GT(off, 1e-4f);
        } else {
            ++vane;
            EXPECT_EQ(n, 7u);
            EXPECT_GT(u, 0.4f);
        }
    }
    EXPECT_EQ(down, 16);  // 8 a side
    EXPECT_LT(straightness / static_cast<float>(down), 0.7f);  // curly: on average their ends far closer than their length
    EXPECT_EQ(vane, 40);
    // The down barbs have IDs of their own and a clump apart from the vane's splits.
    std::set<StrandId> ids(out->ids().begin(), out->ids().end());
    EXPECT_EQ(ids.size(), out->strandCount());
    // Colored through FeatherColor: the down's color on them alone.
    const StrandsPtr colored = overHairColor(op::kFeatherColor, out, [](ParamSet& p) {
        p.set("pattern", std::string("none"));
        p.set("downStrength", 1.0);
        p.set("downColor", Vec3f(1.0f, 1.0f, 1.0f));
    }, f.scalp);
    for (std::size_t s = 1; s < colored->strandCount(); ++s) {
        const Rgb c = pointColor(*colored, s, 3);
        if (ch(*out, kIsDownChannel, s) > 0.5f) EXPECT_LT(distance(c, {1.0f, 1.0f, 1.0f}), 1e-5f);
        else EXPECT_LT(distance(c, melaninColor(0.7f, 0.1f)), 1e-5f);
    }
}

TEST(Propagate, WithoutADownZoneFeathersStayAsTheyWere) {
    const Shafts f(2);
    const StrandsPtr before = f.propagate([](ParamSet& p) { p.set("splits", 3.0); });
    const StrandsPtr after = f.propagate([](ParamSet& p) {
        p.set("splits", 3.0);
        p.set("downCount", std::int64_t{50});  // down's own settings do nothing without a down zone
        p.set("downCurl", 10.0);
        p.set("downSegments", std::int64_t{3});
        p.set("down", 0.1);                    // nor does a zone ending before the barbs start (0.15)
    });
    EXPECT_EQ(before->ids(), after->ids());
    EXPECT_EQ(before->positions(), after->positions());
    EXPECT_EQ(*before->strandChannel("clumpId"), *after->strandChannel("clumpId"));
    EXPECT_FALSE(after->strandChannel(kIsDownChannel));
}

// ---- Iridescence --------------------------------------------------------------------------------

TEST(FeatherColor, IridescenceIsAChannelPerPointWhereAsked) {
    const Shafts f;
    const StrandsPtr feathers = f.propagate();
    auto colored = [&](const char* on) {
        return overHairColor(op::kFeatherColor, feathers, [&](ParamSet& p) {
            p.set("pattern", std::string("none"));
            p.set("iridescence", 0.8);
            p.set("iridescenceOn", std::string(on));
        }, f.scalp);
    };
    const StrandsPtr all = colored("all");
    const std::vector<float>* iridescence = all->pointChannel(kIridescenceChannel);
    ASSERT_TRUE(iridescence);
    for (const float v : *iridescence) EXPECT_FLOAT_EQ(v, 0.8f);
    const StrandsPtr right = colored("rightVane");
    for (std::size_t s = 0; s < right->strandCount(); ++s) {
        const float expected = ch(*right, "side", s) > 0 ? 0.8f : 0.0f;
        EXPECT_FLOAT_EQ((*right->pointChannel(kIridescenceChannel))[right->strandBegin(s) + 2], expected) << s;
    }
    // A HairColor after it sets a look afresh: no channel (an earlier node's goes).
    const StrandsPtr none = valueAs<StrandsPtr>(run(op::kHairColor, {all}, plain, f.scalp));
    EXPECT_FALSE(none->pointChannel(kIridescenceChannel));
    // Not pigment: the colors are the same with or without it.
    EXPECT_EQ(*none->strandChannel("colorG"), *all->strandChannel("colorG"));
}

TEST(ColorNodes, EachTakesItsOwnParams) {
    auto specsOf = [](const char* type) {
        std::map<std::string, ParamSpec> specs;
        for (const ParamSpec& spec : OperatorRegistry::instance().create(type)->paramSpecs()) specs[spec.name] = spec;
        return specs;
    };
    std::map<std::string, ParamSpec> hair = specsOf(op::kHairColor), bands = specsOf(op::kBands),
                                      feather = specsOf(op::kFeatherColor);
    for (const char* name : {"mode", "melanin", "redness", "dye", "color", "map", "greys", "melaninRandom", "valueRandom",
                             "hueRandom", "saturationRandom", "clumpShare", "clumpChannel", "seed", "tipColor", "tipStrength",
                             "tipProfile", "tipOffsetRandom", "specular", "specularRandom", "roughness", "roughnessRandom",
                             "tilt"}) {
        EXPECT_TRUE(hair.count(name)) << name;
    }
    EXPECT_EQ(hair.size(), 23u);
    // The highlight, in its own section of the panels, at the look's defaults.
    for (const char* name : {"specular", "specularRandom", "roughness", "roughnessRandom", "tilt"}) {
        EXPECT_EQ(hair[name].group, "Highlight") << name;
    }
    EXPECT_FLOAT_EQ(static_cast<float>(std::get<double>(hair["specular"].defaultValue)), kDefaultHairSpecular);
    EXPECT_FLOAT_EQ(static_cast<float>(std::get<double>(hair["roughness"].defaultValue)), kDefaultHairRoughness);
    EXPECT_FLOAT_EQ(static_cast<float>(std::get<double>(hair["tilt"].defaultValue)), kDefaultHairTilt);
    EXPECT_EQ(hair["tilt"].unit, "angle");
    const std::map<std::string, ParamSpec> render = specsOf(op::kRenderData);
    for (const char* name : {"specular", "specularRandom", "roughness", "roughnessRandom"}) EXPECT_FALSE(render.count(name)) << name;
    for (const char* name : {"strength", "melanin", "redness", "dye", "profile", "count", "offsetRandom", "widthRandom",
                             "melaninRandom", "guards", "guardsBy", "undercoatDye", "seed"}) {
        EXPECT_TRUE(bands.count(name)) << name;
    }
    EXPECT_EQ(bands.size(), 13u);
    for (const char* name : {"pattern", "strength", "melanin", "redness", "dye", "vanes", "count", "width", "angle", "sharpness",
                             "offset", "profile", "offsetRandom", "scaleRandom", "angleRandom", "texture", "textureRight",
                             "downColor", "downStrength", "iridescence", "iridescenceColor", "iridescenceShift",
                             "iridescenceOn", "seed"}) {
        EXPECT_TRUE(feather.count(name)) << name;
    }
    EXPECT_EQ(feather.size(), 24u);
    EXPECT_EQ(std::get<double>(feather["iridescence"].defaultValue), 0.0);
    EXPECT_EQ(feather["iridescenceColor"].unit, "color");
    EXPECT_EQ(feather["iridescenceOn"].choices.size(), 5u);
    EXPECT_EQ(feather["pattern"].choices, vanePatternNames());
    EXPECT_EQ(std::get<double>(bands["strength"].defaultValue), 1.0);
    EXPECT_EQ(std::get<double>(bands["guards"].defaultValue), 1.0);
    for (const char* type : {op::kHairColor, op::kBands, op::kFeatherColor}) {
        EXPECT_EQ(OperatorRegistry::instance().create(type)->category(), "color") << type;
    }
}

TEST(ColorPatterns, TheIridescentTintTurnsItsHueWithTheAngle) {
    const Rgb green{0.1f, 0.6f, 0.2f};
    const Rgb faceOn = iridescentTint(green, 0.3f, 0.0f);
    EXPECT_LT(distance(faceOn, green), 1e-5f);  // facing the view: the color itself
    const Rgb along = iridescentTint(green, 0.3f, 1.0f);  // edge-on
    EXPECT_GT(along[2], along[1]);  // a third of a turn on: toward blue
    // Its brightness and saturation stay.
    EXPECT_NEAR(std::max({along[0], along[1], along[2]}), 0.6f, 1e-4f);
    EXPECT_NEAR(std::min({along[0], along[1], along[2]}), 0.1f, 1e-4f);
    EXPECT_LT(distance(iridescentTint(green, 1.0f, 1.0f), green), 1e-4f);  // a whole turn: back
}

// ---- Cards --------------------------------------------------------------------------------------

float toSrgb(float v) { return v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f; }

// Two clumps of 20 straight strands, colored by HairColor (plain), then by a node of `type` with
// `set` (none without a type).
StrandsPtr coloredClumps(const char* type, const std::function<void(ParamSet&)>& set) {
    auto s = std::make_shared<StrandSet>();
    for (int clump = 0; clump < 2; ++clump) {
        for (int i = 0; i < 20; ++i) {
            const float x = 0.5f * static_cast<float>(clump) + 0.1f * static_cast<float>(i) / 19.0f;
            std::vector<Vec3f> pts;
            for (int k = 0; k <= 20; ++k) pts.push_back({x, 0.05f * static_cast<float>(k), 0.0f});
            s->addStrand(static_cast<StrandId>(clump * 100 + i + 1), pts, std::vector<float>(pts.size(), 0.004f));
        }
    }
    std::vector<float>& clumpId = s->addStrandChannel("clumpId", 0.0f);
    for (std::size_t i = 20; i < 40; ++i) clumpId[i] = 1.0f;
    if (!type) return valueAs<StrandsPtr>(run(op::kHairColor, {StrandsPtr(s)}, plain));
    return overHairColor(type, s, set);
}

TEST(Cards, BandsBakeIntoTheColorMapAndTheIridescenceIntoItsAlpha) {
    const StrandsPtr banded = coloredClumps(op::kBands, [](ParamSet& p) {
        exactBands(p);
        p.set("offsetRandom", 0.0);
        p.set("widthRandom", 0.0);
        p.set("profile", Ramp{{0.0f, 0.0f}, {0.4f, 0.0f}, {0.45f, 1.0f}, {0.7f, 1.0f}, {0.75f, 0.0f}, {1.0f, 0.0f}});
    });
    const StrandsPtr s = valueAs<StrandsPtr>(run(op::kFeatherColor, {banded}, [](ParamSet& p) {
        p.set("pattern", std::string("none"));
        p.set("iridescence", 0.8);
    }));
    CardSettings settings;
    settings.variations = 0;
    CardMesh mesh = buildCards(*s, nullptr, settings);
    ASSERT_FALSE(mesh.cards.empty());
    layoutCards(mesh, 0.02f);
    // Each card keeps its strands' iridescence, every vertex its card's.
    for (const CardMesh::Card& card : mesh.cards) {
        EXPECT_NEAR(card.iridescence[0], 0.8f, 1e-4f);
        EXPECT_NEAR(card.iridescence[1], 0.8f, 1e-4f);
    }
    ASSERT_EQ(mesh.iridescence.size(), mesh.positions.size());
    CardBakeSettings bake;
    bake.size = 256;
    const CardTextures t = bakeCardTextures(*s, mesh, bake);
    // The bands bake into the Color map where they are along the strands (the Attribute map's
    // coordU says where): the band's pigment in the middle, the hair's at the root.
    // (The Color map holds the hair's appearance: hairAppearanceColor.)
    const Rgb base = hairAppearanceColor(melaninColor(0.7f, 0.1f)), band = hairAppearanceColor(melaninColor(0.08f, 0.6f));
    double bandSum = 0.0, rootSum = 0.0, bandCount = 0.0, rootCount = 0.0;
    for (std::size_t p = 0; p < t.coverage.size(); ++p) {
        if (t.coverage[p] < 200) continue;
        const float u = static_cast<float>(t.attribute[p * 4 + 3]) / 255.0f;
        const float g = static_cast<float>(t.color[p * 3 + 1]) / 255.0f;
        if (u > 0.5f && u < 0.65f) bandSum += g, bandCount += 1.0;
        if (u < 0.3f) rootSum += g, rootCount += 1.0;
    }
    ASSERT_GT(bandCount, 10.0);
    ASSERT_GT(rootCount, 10.0);
    EXPECT_NEAR(bandSum / bandCount, toSrgb(band[1]), 0.04);
    EXPECT_NEAR(rootSum / rootCount, toSrgb(base[1]), 0.04);
    // The iridescence: its own map, 0.8 where strands cover.
    ASSERT_EQ(t.iridescence.size(), t.coverage.size());
    for (std::size_t p = 0; p < t.coverage.size(); ++p) {
        if (t.coverage[p] > 200) EXPECT_NEAR(t.iridescence[p], 204, 2) << p;
    }
    // Written as the Color map's alpha: an RGBA TGA.
    CardBake written;
    written.strands = s;
    written.cards = std::make_shared<CardMesh>(mesh);
    written.textures = t;
    const auto folder = std::filesystem::temp_directory_path() / "groomlab_iridescent_bake";
    std::filesystem::create_directories(folder);
    const CardBakePaths at = CardBakePaths::in(folder.string(), "fur");
    writeCardBake(written, at, false);
    EXPECT_EQ(std::filesystem::file_size(at.map("Color")), 18u + 4u * 256u * 256u);
    // Without iridescence the map stays RGB, the cards carry none.
    const StrandsPtr plainFur = coloredClumps(nullptr, {});
    CardMesh plainMesh = buildCards(*plainFur, nullptr, settings);
    layoutCards(plainMesh, 0.02f);
    EXPECT_TRUE(plainMesh.iridescence.empty());
    written.strands = plainFur;
    written.cards = std::make_shared<CardMesh>(plainMesh);
    written.textures = bakeCardTextures(*plainFur, plainMesh, bake);
    EXPECT_TRUE(written.textures.iridescence.empty());
    writeCardBake(written, at, false);
    EXPECT_EQ(std::filesystem::file_size(at.map("Color")), 18u + 3u * 256u * 256u);
    std::filesystem::remove_all(folder);
}

TEST(Cards, AFeathersBarsBakeOntoItsVaneLaidFlat) {
    // A feather card is its vane laid flat: bars on the left vane bake onto one half of the card's
    // texture, the other half the hair's color alone.
    const Shafts f;
    const StrandsPtr feathers = f.propagate([](ParamSet& p) {
        p.set("count", std::int64_t{60});
        p.set("segments", std::int64_t{8});
        p.set("width", 0.004);
    });
    const StrandsPtr colored = overHairColor(op::kFeatherColor, feathers, [](ParamSet& p) {
        p.set("pattern", std::string("bars"));
        p.set("vanes", std::string("left"));
        p.set("count", 4.0);
        p.set("width", 0.5);
        p.set("sharpness", 1.0);
        p.set("offsetRandom", 0.0);
    }, f.scalp);
    CardSettings settings;
    settings.variations = 0;
    CardMesh mesh = buildCards(*colored, f.scalp.get(), settings);
    ASSERT_EQ(mesh.cards.size(), 1u);
    ASSERT_GE(mesh.cards[0].feather, 0);
    layoutCards(mesh, 0.02f);
    CardBakeSettings bake;
    bake.size = 256;
    const CardTextures t = bakeCardTextures(*colored, mesh, bake);
    const CardMesh::Card& card = mesh.cards[0];
    const float middle = 0.5f * (card.cell[0] + card.cell[2]) * 256.0f;
    const float white = toSrgb(melaninColor(0.0f, 0.0f)[1]) * 255.0f;
    int whiteHalves[2] = {0, 0}, covered[2] = {0, 0};
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) {
            const std::size_t p = static_cast<std::size_t>(y) * 256u + static_cast<std::size_t>(x);
            if (t.coverage[p] < 200) continue;
            const int half = static_cast<float>(x) < middle ? 0 : 1;
            ++covered[half];
            whiteHalves[half] += std::abs(static_cast<float>(t.color[p * 3 + 1]) - white) < 12.0f ? 1 : 0;
        }
    }
    ASSERT_GT(covered[0], 100);
    ASSERT_GT(covered[1], 100);
    // One half barred (a good share white), the other not at all.
    const int barred = whiteHalves[0] > whiteHalves[1] ? 0 : 1;
    EXPECT_GT(whiteHalves[barred], covered[barred] / 6);
    EXPECT_LT(whiteHalves[1 - barred], covered[1 - barred] / 50);
}

TEST(Cards, AFeathersCardCarriesEachVanesIridescence) {
    // One vane iridescent: the feather's card has it on that vane's edge, none on the other's.
    const Shafts f;
    const StrandsPtr feathers = f.propagate([](ParamSet& p) { p.set("width", 0.004); });
    const StrandsPtr colored = overHairColor(op::kFeatherColor, feathers, [](ParamSet& p) {
        p.set("pattern", std::string("none"));
        p.set("iridescence", 0.8);
        p.set("iridescenceOn", std::string("rightVane"));
    }, f.scalp);
    CardSettings settings;
    settings.variations = 0;
    CardMesh mesh = buildCards(*colored, f.scalp.get(), settings);
    ASSERT_EQ(mesh.cards.size(), 1u);
    EXPECT_NEAR(mesh.cards[0].iridescence[0], 0.0f, 1e-5f);
    EXPECT_NEAR(mesh.cards[0].iridescence[1], 0.8f, 0.05f);  // (the shaft, on both, has none)
    layoutCards(mesh, 0.02f);
    ASSERT_EQ(mesh.iridescence.size(), mesh.positions.size());
    // The right vane is toward +z (the shaft along +x on a +y scalp): its edge carries it.
    int right = 0, left = 0;
    for (std::size_t v = 0; v < mesh.positions.size(); ++v) {
        if (mesh.positions[v].z > 0.3f + 0.05f) right += mesh.iridescence[v] > 0.65f ? 1 : -100;
        if (mesh.positions[v].z < 0.3f - 0.05f) left += mesh.iridescence[v] < 0.1f ? 1 : -100;
    }
    EXPECT_GT(right, 2);
    EXPECT_GT(left, 2);
}

// ---- Defaults -----------------------------------------------------------------------------------

TEST(ColorNodes, BandsAndFeatherColorWithNothingToDoHandTheStrandsOn) {
    const Shafts f;
    const StrandsPtr feathers = f.propagate([](ParamSet& p) { p.set("down", 0.3); });
    const Value colored = run(op::kHairColor, {feathers}, [](ParamSet& p) {
        plain(p);
        p.set("tipStrength", 0.6);
    }, f.scalp);
    const StrandsPtr in = valueAs<StrandsPtr>(colored);
    const StrandsPtr bands = valueAs<StrandsPtr>(run(op::kBands, {colored}, [](ParamSet& p) {
        p.set("strength", 0.0);
        p.set("melanin", 0.9);
        p.set("undercoatDye", Vec3f(0.1f, 0.2f, 0.3f));  // no undercoat: guards 1
    }, f.scalp));
    EXPECT_EQ(bands, in);
    const StrandsPtr feather = valueAs<StrandsPtr>(run(op::kFeatherColor, {colored}, [](ParamSet& p) {
        p.set("pattern", std::string("none"));
        p.set("downColor", Vec3f(1.0f, 0.0f, 0.0f));
        p.set("iridescenceColor", Vec3f(1.0f, 0.0f, 0.0f));
    }, f.scalp));
    EXPECT_EQ(feather, in);
    // Strands without feathers: a pattern finds no vane space, and hands them on too.
    const StrandsPtr hair = valueAs<StrandsPtr>(run(op::kHairColor, {StrandsPtr(uprights(3))}, plain));
    EXPECT_EQ(valueAs<StrandsPtr>(run(op::kFeatherColor, {hair}, [](ParamSet& p) { p.set("pattern", std::string("bars")); })),
              hair);
}

}  // namespace
}  // namespace groomlab
