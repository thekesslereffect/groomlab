// SPDX-License-Identifier: Apache-2.0
// The look in RenderMan's terms (renderman_look.h): PxrHairColor's pigment, PxrMarschnerHair's
// lobes and the primvars the RenderMan procedural writes, without the RenderMan SDK.

#include <cmath>
#include <map>
#include <string>

#include <gtest/gtest.h>

#include <algorithm>

#include "groomlab/export_view.h"
#include "groomlab/groom_session.h"
#include "groomlab/renderman_look.h"

namespace groomlab {
namespace {

using renderman::Detail;
using renderman::Primvar;
using renderman::Type;

std::map<std::string, const Primvar*> byName(const std::vector<Primvar>& primvars) {
    std::map<std::string, const Primvar*> out;
    for (const Primvar& p : primvars) out[p.name] = &p;
    return out;
}

TEST(RenderManLook, PigmentScalesTheMelaninAndFitsTheRedness) {
    const renderman::HairColorPigment none = renderman::pxrHairColorPigment(0.0f, 0.3f);
    EXPECT_FLOAT_EQ(none.melanin, 0.0f);
    EXPECT_FLOAT_EQ(none.redness, 0.3f);  // nothing to fit: kept

    const renderman::HairColorPigment brown = renderman::pxrHairColorPigment(0.5f, 0.0f);
    EXPECT_NEAR(brown.melanin, 0.5f * renderman::kPxrMelaninScale, 1e-6f);
    EXPECT_NEAR(brown.redness, 0.0f, 1e-6f);
    const renderman::HairColorPigment red = renderman::pxrHairColorPigment(0.5f, 1.0f);
    EXPECT_NEAR(red.melanin, 0.5f * (renderman::kPxrMelaninScale + renderman::kPxrMelaninScaleRedness), 1e-6f);
    EXPECT_NEAR(red.redness, 1.0f, 1e-6f);

    // In between, PxrHairColor's mixed colors come close to GroomLab's mixed absorption.
    for (const float r : {0.2f, 0.45f, 0.8f}) {
        const renderman::HairColorPigment p = renderman::pxrHairColorPigment(0.35f, r);
        EXPECT_GT(p.redness, 0.0f);
        EXPECT_LT(p.redness, 1.0f);
        const auto fiber = renderman::pxrHairColorFiber(p.melanin, p.redness);
        const float w = renderman::kPxrHairColorConcentration * p.melanin * p.melanin;
        const float eu[3] = {0.419f, 0.697f, 1.37f}, pheo[3] = {0.187f, 0.4f, 1.05f};
        for (int k = 0; k < 3; ++k) {
            EXPECT_NEAR(fiber[k], std::exp(-w * ((1.0f - r) * eu[k] + r * pheo[k])), 0.02f) << r << " channel " << k;
        }
    }
    // Redder hair is redder in PxrHairColor too.
    EXPECT_LT(renderman::pxrHairColorPigment(0.35f, 0.2f).redness, renderman::pxrHairColorPigment(0.35f, 0.6f).redness);

    // Soaked: twice the absorption, the melanin times sqrt(2) (hair_color.h wetShading).
    EXPECT_NEAR(renderman::pxrHairColorPigment(0.3f, 0.0f, 1.0f).melanin,
                renderman::pxrHairColorPigment(0.3f, 0.0f).melanin * std::sqrt(1.0f + kWetAbsorption), 1e-5f);
}

TEST(RenderManLook, FiberIsPxrHairColorsPhysicalModel) {
    // PxrHairColor at melanin 0.3, redness 0.5 renders (0.431, 0.2155, 0.0307) (checked in prman).
    const auto c = renderman::pxrHairColorFiber(0.3f, 0.5f);
    EXPECT_NEAR(c[0], 0.43098f, 1e-4f);
    EXPECT_NEAR(c[1], 0.21550f, 1e-4f);
    EXPECT_NEAR(c[2], 0.03066f, 1e-4f);
}

TEST(RenderManLook, MarschnerLobesFollowArnoldsRoughness) {
    const renderman::MarschnerLobes defaults = renderman::pxrMarschnerLobes(hairLobes(0.3f, 0.5f));
    EXPECT_NEAR(defaults.coneAngleR, 8.34f, 0.05f);  // PxrMarschnerHair's own default is 8
    EXPECT_FLOAT_EQ(defaults.coneAngleTT, 0.5f * defaults.coneAngleR);
    EXPECT_FLOAT_EQ(defaults.coneAngleTRT, 2.0f * defaults.coneAngleR);
    EXPECT_FLOAT_EQ(defaults.specularGainR, renderman::kPxrSpecularGainR);  // HairColor's default: full tint
    EXPECT_FLOAT_EQ(defaults.offset, -kDefaultHairTilt);
    EXPECT_FLOAT_EQ(renderman::pxrMarschnerLobes(hairLobes(0.3f, 0.5f, 6.0f)).offset, -6.0f);  // HairColor's tilt
    EXPECT_FLOAT_EQ(defaults.ior, kHairIor);
    const renderman::MarschnerLobes smooth = renderman::pxrMarschnerLobes(hairLobes(0.15f, 0.1f));
    EXPECT_LT(smooth.coneAngleR, defaults.coneAngleR);
    EXPECT_FLOAT_EQ(smooth.specularGainR, 0.5f * renderman::kPxrSpecularGainR);
}

TEST(RenderManLook, PrimvarsCoverTheStrandsAndTheLook) {
    StrandSet s;
    s.addStrand(7, {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}}, {0.02f, 0.02f, 0.01f});
    s.addStrand((std::uint64_t{1} << 40) + 9, {{1, 0, 0}, {1, 1, 0}}, {0.02f, 0.01f});
    s.addStrandChannel(kMelaninChannel, 0.0f) = {0.35f, 0.6f};
    s.addStrandChannel(kMelaninRednessChannel, 0.0f) = {0.2f, 0.9f};
    s.addStrandChannel(kClumpIdChannel, 0.0f) = {3.0f, 4.0f};
    s.addStrandChannel("width", 0.0f) = {5.0f, 5.0f};  // a renderer's name: never a primvar of a channel
    HairLook look;
    look.dye = {0.9f, 0.8f, 0.7f};
    {
        const ExportView view = describeExport(s, nullptr, kExportPointColors);
        const std::vector<Primvar> primvars = renderman::curvePrimvars(view, look);
        const auto p = byName(primvars);
        for (const Primvar& pv : primvars) {
            const std::size_t want = pv.detail == Detail::Constant ? 1 : pv.detail == Detail::Uniform ? 2 : 5;
            EXPECT_EQ(pv.valueCount(), want) << pv.declaration();
        }
        ASSERT_TRUE(p.count("strand_id") && p.count("hair_index") && p.count("melanin") && p.count("clumpId"));
        EXPECT_EQ(p.at("strand_id")->declaration(), "uniform int strand_id");
        EXPECT_EQ(p.at("strand_id")->ints[1], 9);  // low 32 bits
        EXPECT_FLOAT_EQ(p.at("hair_index")->floats[0], 8.0f);  // never 0: PxrHairColor's randomness needs it
        EXPECT_EQ(p.at("clumpId")->declaration(), "uniform float clumpId");
        EXPECT_FALSE(p.count("width"));
        EXPECT_FALSE(p.count("st"));  // no scalp, no UVs
        ASSERT_TRUE(p.count("hair_color"));  // from the melanin (export_view.h strandColors)
        EXPECT_EQ(p.at("hair_color")->declaration(), "uniform color hair_color");
        EXPECT_EQ(p.at("rman_melanin")->detail, Detail::Uniform);
        EXPECT_FLOAT_EQ(p.at("rman_melanin")->floats[1], renderman::pxrHairColorPigment(0.6f, 0.9f).melanin);
        EXPECT_FLOAT_EQ(p.at("rman_redness")->floats[0], renderman::pxrHairColorPigment(0.35f, 0.2f).redness);
        EXPECT_EQ(p.at("rman_dye")->declaration(), "constant color rman_dye");
        EXPECT_FLOAT_EQ(p.at("rman_dye")->floats[2], 0.7f);
        EXPECT_FLOAT_EQ(p.at("rman_rgbShare")->floats[0], 0.0f);  // the pigment's look
        EXPECT_NEAR(p.at("rman_coneAngleR")->floats[0], 8.34f, 0.05f);
        EXPECT_FALSE(p.count("wetness"));
        EXPECT_EQ(p.at("rman_specularOffset")->declaration(), "constant float rman_specularOffset");
        EXPECT_FLOAT_EQ(p.at("rman_specularOffset")->floats[0], -look.tilt);
    }

    // The RGB look, and per-point wetness: the look goes per point, darker and sharper where wet.
    look.colorsByRgb = true;
    s.addStrandChannel(kSpecularChannel, 0.0f) = {0.3f, 0.15f};
    s.addPointChannel(kWetnessChannel, 0.0f) = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    s.addPointChannel(kOpacityChannel, 1.0f) = {1.0f, 0.5f, 0.2f, 1.0f, 1.0f};
    const ExportView view = describeExport(s, nullptr, kExportPointColors);
    const std::vector<Primvar> primvars = renderman::curvePrimvars(view, look);
    const auto p = byName(primvars);
    EXPECT_EQ(p.at("wetness")->declaration(), "vertex float wetness");
    EXPECT_EQ(p.at("opacity")->declaration(), "vertex float opacity");
    EXPECT_FLOAT_EQ(p.at("opacity")->floats[2], 0.2f);
    const Primvar& melanin = *p.at("rman_melanin");
    EXPECT_EQ(melanin.detail, Detail::Vertex);
    ASSERT_EQ(melanin.valueCount(), 5u);
    EXPECT_FLOAT_EQ(melanin.floats[0], melanin.floats[1]);
    EXPECT_GT(melanin.floats[2], melanin.floats[1]);  // the wet tip
    EXPECT_FLOAT_EQ(melanin.floats[3], renderman::pxrHairColorPigment(0.6f, 0.9f).melanin);
    const Primvar& cone = *p.at("rman_coneAngleR");
    EXPECT_LT(cone.floats[2], cone.floats[1]);  // wet: a narrower highlight
    const Primvar& gain = *p.at("rman_specularGainR");
    EXPECT_FLOAT_EQ(gain.floats[0], renderman::kPxrSpecularGainR);
    EXPECT_FLOAT_EQ(gain.floats[3], 0.5f * renderman::kPxrSpecularGainR);  // its own specular channel
    EXPECT_EQ(p.at("rman_color")->detail, Detail::Vertex);
    EXPECT_LT(p.at("rman_color")->floats[3 * 2], p.at("rman_color")->floats[3 * 1]);  // darker where wet
    EXPECT_FLOAT_EQ(p.at("rman_rgbShare")->floats[1], 1.0f);
}

TEST(RendermanLook, TheHairNetworkAsEveryHostWritesIt) {
    // renderman_look.h hairNetworkRib: the app's render and Maya's prman_scene write it; handles
    // prefixed, the highlight's offset minus the tilt.
    const std::string rib = renderman::hairNetworkRib("hair_", 3.0f, "  ");
    EXPECT_NE(rib.find("  Pattern \"PxrPrimvar\" \"hair_pv_melanin\""), std::string::npos);
    EXPECT_NE(rib.find("\"float defaultFloat\" [1.75]"), std::string::npos);
    EXPECT_NE(rib.find("Bxdf \"PxrMarschnerHair\" \"hair_hair\""), std::string::npos);
    EXPECT_NE(rib.find("\"float specularOffset\" [-3]"), std::string::npos);
    EXPECT_EQ(std::count(rib.begin(), rib.end(), '\n'), 16);  // 10 readers, 2 colors, 3 mixes, the Bxdf
}

}  // namespace
}  // namespace groomlab
