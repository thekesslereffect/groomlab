// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>

// Hair colors. Every color GroomLab writes for hair (HairColor's RGB, Bands, FeatherColor, the
// tips) is a hair color in aiStandardHair's base-color sense: the color a lock of that hair shows
// once light has scattered through many strands (Chiang et al. 2016). Arnold renders it as the
// base color over no melanin, Maya's viewport and the app turn it into the fiber's absorption with
// the same mapping (hair_bsdf.glsl hairColorSigma, hairColorAbsorption here), Unreal's hair shading
// takes it as its base color. Melanin and redness name such a color too: the one whose absorption
// is the pigment's (hairMelaninAbsorption), so melanin hair renders as aiStandardHair's melanin.

namespace groomlab {

struct HairLook;  // groom_session.h

// The hair color (linear RGB) of `melanin` 0 (white, blond) to 1 (black) and `redness` 0 (brown)
// to 1 (red), dyed by `dye` (linear RGB, white: none): hairAbsorptionColor of the pigment's
// absorption (hairMelaninAbsorption) plus the dye's (hairColorAbsorption), as aiStandardHair
// combines its melanin and its base color. Melanin 0: the dye itself.
std::array<float, 3> melaninColor(float melanin, float redness, const std::array<float, 3>& dye = {1.0f, 1.0f, 1.0f});
// `color` dyed by `dye`: their absorptions added (white: none; black: black).
std::array<float, 3> dyedHairColor(const std::array<float, 3>& color, const std::array<float, 3>& dye);

// A color varied as HairColor varies each strand's: its hue turned by `hueTurn` (turns) and its
// saturation scaled by `saturationScale` (in HSL, as PxrHairColor varies its dye: the lightness
// kept; grey and white stay neutral), then all of it scaled by `valueScale`. Clamped to 0-1.
std::array<float, 3> varyHairColor(const std::array<float, 3>& rgb, float hueTurn, float saturationScale, float valueScale);

// HairColor's defaults: the pigment of a groom no HairColor node colored. The
// melanin channels and the RGB channels HairColor adds take these as their
// defaults, so strands merged in without color read as this brown, not black.
constexpr float kDefaultMelanin = 0.35f;
constexpr float kDefaultMelaninRedness = 0.2f;
// melaninColor(kDefaultMelanin, kDefaultMelaninRedness).
std::array<float, 3> defaultHairColor();

// ---- The look's lobes (aiStandardHair's) --------------------------------------------------------
//
// The groom's look (HairColor's highlight: GroomSession::hairLook) as every host shades and renders
// it: Maya's create_hair_shader gives it to aiStandardHair, Maya's viewport hair and the app's
// lookdev shade it (engine/gpu/shaders/hair_bsdf.glsl), Blender's material fits Cycles to it. The
// specular against kHairFullSpecular is the R lobe's tint (up to 1: HairColor's default is Arnold's
// full tint), the roughness 0..1 is Arnold's kHairRoughnessMin + kHairRoughnessRange x it, the tilt
// (degrees) Arnold's shift as it is; the rest aiStandardHair's defaults.
constexpr float kHairFullSpecular = 0.3f;
constexpr float kHairRoughnessMin = 0.1f, kHairRoughnessRange = 0.4f;
constexpr float kHairIor = 1.55f;
constexpr float kHairShiftDegrees = 3.0f;  // the cuticle tilt, moving the lobes apart
constexpr float kHairAzimuthalRoughness = 0.2f;
// HairColor's highlight defaults: its specular, roughness and tilt params', and the values of
// strands without the channels (export_view.h kSpecularChannel, kRoughnessChannel, kTiltChannel).
constexpr float kDefaultHairSpecular = 0.3f;
constexpr float kDefaultHairRoughness = 0.5f;
constexpr float kDefaultHairTilt = kHairShiftDegrees;
struct HairLobes {
    float specularTint = 1.0f;  // the R lobe's tint (gray)
    float roughness = kHairRoughnessMin + 0.5f * kHairRoughnessRange;  // longitudinal
    float azimuthalRoughness = kHairAzimuthalRoughness;
    float ior = kHairIor;
    float shiftDegrees = kHairShiftDegrees;  // the cuticle tilt: HairColor's tilt
};
HairLobes hairLobes(float specular, float roughness, float tilt = kDefaultHairTilt);
HairLobes hairLobes(const HairLook& look);
// The longitudinal lobe's width in sin-theta space for an Arnold roughness (d'Eon et al. 2011's
// mapping of beta_m to the lobe's variance): roughness 0.2 is a tight highlight, 1 a smear.
float hairLongitudinalWidth(float roughness);
// The azimuthal spread (radians) for an Arnold azimuthal roughness (d'Eon's beta_n fit).
float hairAzimuthalSpread(float azimuthalRoughness);
// Absorption per fiber radius (linear RGB) for melanin and redness as aiStandardHair renders them:
// eumelanin and pheomelanin spectra at concentration 6.6 x melanin^2 (hair_bsdf.glsl's
// hairMelaninSigma, uncalibrated). exp(-absorption) is the fiber's color the lobes take.
std::array<float, 3> hairMelaninAbsorption(float melanin, float redness);
// Absorption per fiber radius (linear RGB) that makes hair look `rgb`, as aiStandardHair absorbs its
// base color over no melanin: Chiang et al. 2016's mapping for the azimuthal roughness,
// (ln c / (5.969 - 0.215 b + 2.532 b^2 - 10.73 b^3 + 5.574 b^4 + 0.245 b^5))^2 (hair_bsdf.glsl's
// hairColorSigma). Every hair color shades through it: exp(-absorption) is the fiber's color the
// lobes take.
std::array<float, 3> hairColorAbsorption(const std::array<float, 3>& rgb, float azimuthalRoughness = kHairAzimuthalRoughness);
// The hair color whose absorption (hairColorAbsorption) is `absorption`: its inverse.
std::array<float, 3> hairAbsorptionColor(const std::array<float, 3>& absorption, float azimuthalRoughness = kHairAzimuthalRoughness);
// aiStandardHair absorbs a base color c about 1.175 times Chiang's absorption of it, plus 0.0325
// (Arnold 7.0 fitted with kick: docs/BLENDER.md "Rendering"). The hosts hand Arnold c to the power
// 1 / kArnoldBaseColorGamma (color_correct's gamma), which it absorbs as hairColorAbsorption(c) plus
// that constant: as the viewport shades c. Checked with kick: a melanin's color so corrected renders
// within 5% of aiStandardHair's own melanin 0.1-0.7 (build/probe/haircolor/kick_melanin_vs_base.py).
constexpr float kArnoldBaseColorGamma = 1.0840f;  // sqrt(1.175)

// ---- Base color and appearance --------------------------------------------------------------------
//
// A hair color as above is a BASE color: what a hair BSDF takes (aiStandardHair's base color,
// RenderMan's rman_color, the viewport's and the app's hair shading, Cycles' hair). It is not what
// the hair looks like: light hair reflects its highlight and scatters light through a few strands,
// so a lock of medium brown hair (base color about 0.04, 0.015, 0.003) looks a medium brown. The
// APPEARANCE color is that look: the matte color the hair matches under even white light, for
// what draws or stores a color without a hair BSDF (USD displayColor, Alembic Cd, Unreal's
// groom_color, the cards' baked color maps, Maya's hair tube shading, the app's plain modes).
// Measured with kick (build/probe/haircolor/kick_appearance_sparse.py: a lock a few strands deep
// under a uniform white dome, the base color through kArnoldBaseColorGamma, normalized so white
// hair is white): appearance = s + (1 - s) exp(-k a^q), a the base color's absorption
// (hairColorAbsorption), s the highlight's share. Per channel; white stays white.
constexpr float kAppearanceSheen = 0.03f;
constexpr float kAppearanceScale = 3.52f;
constexpr float kAppearancePower = 0.79f;
std::array<float, 3> hairAppearanceColor(const std::array<float, 3>& baseColor);
// Its inverse: the base color that looks like `appearance`.
std::array<float, 3> hairBaseOfAppearance(const std::array<float, 3>& appearance);

// ---- Wet hair -----------------------------------------------------------------------------------
//
// The Wet node's `wetness` channel (0 dry .. 1 soaked; a Simulate's with water or rain, or an
// Attribute node's of the name, alike) as every host shades and renders it, with no param of its
// own: where the channel is, it drives the look. Wet hair looks darker, more saturated and
// glossier. Water fills the gaps between the cuticle scales, so the surface is smoother: the
// highlight narrower and stronger (Arnold's roughness halved, the R lobe's tint up a quarter). And
// with water around it the step in refractive index at the fiber's surface is smaller (keratin
// 1.55 against water 1.33, not air): less light is turned back at the surface and more goes through
// the fiber, the longer way, absorbed as it goes. So the fiber's absorption doubles (its color, the
// transmittance over a radius, squared: darker and more saturated, as Beer-Lambert makes a longer
// path; white stays white). At full wetness a fiber's 0.8 goes to 0.64, 0.6 to 0.36, 0.5 to 0.25
// (20%, 40% and 50% darker), and the light it scatters on through the other hairs falls further:
// soaked blonde looks dark golden, brown a deep brown. Linear in the wetness between dry and
// soaked. Maya's viewport and the app shade it (hair_bsdf.glsl's hairWet, by wetShadingUniform),
// create_hair_shader builds it for Arnold (melanin x sqrt(absorption), the base color to the
// absorption's power, the specular tint and roughness scaled), all from these numbers.
constexpr float kWetAbsorption = 1.0f;  // the fiber's absorption's extra share, soaked
constexpr float kWetSpecular = 0.25f;   // the R lobe's tint's extra share, soaked
constexpr float kWetRoughness = 0.5f;   // Arnold's roughness's share lost, soaked
struct WetShading {
    float absorption = 1.0f;  // the fiber's absorption times this: its color to this power
    float specular = 1.0f;    // the R lobe's tint times this
    float roughness = 1.0f;   // Arnold's longitudinal roughness times this
};
WetShading wetShading(float wetness);
// A fiber color (its transmittance over a radius, linear RGB) wet.
std::array<float, 3> wetFiberColor(const std::array<float, 3>& color, float wetness);
// The lobes wet: the R lobe's tint and the roughness scaled.
HairLobes wetLobes(const HairLobes& lobes, float wetness);
// What hair_bsdf.glsl's hairWet takes (a uniform of the hosts'), soaked: x the absorption's extra
// share, y the R lobe's extra share, z the longitudinal width's ratio (hairLongitudinalWidth) for
// Arnold's `roughness` wet to dry, which the shader blends toward linearly in the wetness; w 0.
std::array<float, 4> wetShadingUniform(float roughness);
// The lit strands' per-point colors carry the iridescence and the wetness in one float (Maya's
// colors' alpha, the fourth float of the app's point colors), unpacked per vertex by
// hair_bsdf.glsl's hairLookAlpha (never interpolated packed): strands without a wetness channel keep
// what they had (Maya 1 + the iridescence, the app the iridescence: under 2), strands with one
// kWetPacked + the wetness in thousandths + half the iridescence.
constexpr float kWetPacked = 4.0f;
float packWetLook(float iridescence, float wetness);

}  // namespace groomlab
