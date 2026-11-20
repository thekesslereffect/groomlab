// SPDX-License-Identifier: Apache-2.0
#pragma once

// GroomLab's hair look in RenderMan's terms, for the RenderMan procedural (hosts/renderman): what
// it writes on the curves as primvars, and how GroomLab's pigment and highlight map onto
// PxrHairColor and PxrMarschnerHair, as hair_color.h maps them onto Arnold's aiStandardHair. No
// RenderMan headers: the procedural turns these into Ri calls, and the tests check them without
// the SDK.
//
// The shading network the primvars are named for (docs/DEVELOPING.md, "RenderMan procedural";
// hosts/renderman/examples/groom.rib):
//
//   PxrPrimvar rman_melanin, rman_redness -> PxrHairColor (Physical).melanin, .redness
//   PxrPrimvar rman_dye                   -> PxrHairColor.dye
//   PxrPrimvar rman_color                 -> PxrHairColor (Artistic).Color      (the RGB look)
//   PxrMix by rman_rgbShare of the two    -> PxrMarschnerHair.diffuseColor, specularColorTT, specularColorTRT
//   PxrPrimvar rman_specularGainR, rman_coneAngleR/TT/TRT -> PxrMarschnerHair's
//   PxrMarschnerHair specularGainTT 2, specularGainTRT 1.2, specularIor 1.55, and specularOffset
//   minus HairColor's tilt (rman_specularOffset: -3 by default; the param takes no connection)
//   PxrPrimvar opacity                    -> PxrMarschnerHair.presence
//   hairIndexPrimvar "hair_index"         (PxrHairColor's own randomness, off by default)

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "groomlab/hair_color.h"

namespace groomlab {

struct ExportView;
struct HairLook;

namespace renderman {

// ---- PxrHairColor -------------------------------------------------------------------------------
//
// PxrHairColor (Physical) colors a fiber exp(-33 m^2 eumelanin) mixed toward exp(-33 m^2
// pheomelanin) by its redness (the colors mixed, not the absorptions), times the dye, and hands
// PxrMarschnerHair that as the TT lobe's color (TRT: its square). GroomLab's melanin is
// aiStandardHair's (hairMelaninAbsorption: 6.6 m^2 of the same spectra, the absorptions mixed by
// redness). pxrHairColorPigment gives PxrHairColor's melanin and redness that render a GroomLab
// pigment as Arnold renders it: the melanin times kPxrMelaninScale + kPxrMelaninScaleRedness x
// redness, the redness the mix of the two colors closest (least squares over R, G, B) to the color
// of GroomLab's mixed absorption at that melanin.
//
// Fitted (with MarschnerLobes' gains) by rendering the head groom (tests/data/app/head_graph.json,
// 100k strands) at five pigments (melanin/redness 0.35/0.2, 0.12/0.45, 0.6/0.25, 0.85/0.1,
// 0.4/0.95) in kick (arnold_compare.py --linear-srgb) and prman (renderman_compare.py), under a
// distant light from the camera's side and the app's key light alone, the hair's mean linear
// color compared. Every pigment comes within about 10% channel by channel under the camera's
// light; under the key (the light through the hair from the side) brown, dark and red do, blonde
// renders 15-30% darker (and a light RGB look, PxrHairColor's Artistic mode, 35-45%):
// PxrMarschnerHair carries less light through many hairs than aiStandardHair.
constexpr float kPxrHairColorConcentration = 33.0f;  // PxrHairColor's exp(-33 m^2 ...)
constexpr float kPxrMelaninScale = 0.68f;           // PxrHairColor's melanin per GroomLab's, brown
constexpr float kPxrMelaninScaleRedness = 0.12f;    // and more of it for red hair
struct HairColorPigment {
    float melanin = 0.0f;
    float redness = 0.0f;
};
// GroomLab's melanin and redness (0-1) as PxrHairColor's. `wetness` (0-1) darkens the pigment as
// hair_color.h wetShading does: the absorption times 1 + kWetAbsorption x wetness (the melanin
// times its square root).
HairColorPigment pxrHairColorPigment(float melanin, float redness, float wetness = 0.0f);
// PxrHairColor's fiber color (resultTT) for its own melanin and redness, dye white.
std::array<float, 3> pxrHairColorFiber(float melanin, float redness);

// ---- PxrMarschnerHair ---------------------------------------------------------------------------
//
// The look's lobes (hair_color.h hairLobes: aiStandardHair's) as PxrMarschnerHair's: the R lobe's
// gain the specular tint times kPxrSpecularGainR (PxrMarschnerHair's R lobe is weaker than
// aiStandardHair's: black hair, R alone, matches at 1.75); TT and TRT gains fixed (fitted with the
// melanin, above); the cone angles (degrees: the longitudinal lobes' widths) from
// hairLongitudinalWidth of the roughness, TT half the R's and TRT twice it (Marschner's, and
// d'Eon's variances as Arnold scales them); the offset minus the cuticle tilt; the IOR the same.
// Defaults (roughness 0.5): cones of 8.3 degrees, PxrMarschnerHair's 8.
constexpr float kPxrSpecularGainR = 1.75f;
constexpr float kPxrSpecularGainTT = 2.0f;
constexpr float kPxrSpecularGainTRT = 1.2f;
struct MarschnerLobes {
    float specularGainR = kPxrSpecularGainR;
    float specularGainTT = kPxrSpecularGainTT, specularGainTRT = kPxrSpecularGainTRT;
    float coneAngleR = 8.0f, coneAngleTT = 4.0f, coneAngleTRT = 16.0f;
    float offset = -kHairShiftDegrees;
    float ior = kHairIor;
};
MarschnerLobes pxrMarschnerLobes(const HairLobes& lobes);

// ---- Primvars -----------------------------------------------------------------------------------

enum class Detail { Constant, Uniform, Vertex };  // one value, one per curve, one per point
enum class Type { Float, Int, Color, Float2 };
struct Primvar {
    std::string name;
    Detail detail = Detail::Uniform;
    Type type = Type::Float;
    std::vector<float> floats;   // Float, Color (3 per value), Float2 (2 per value)
    std::vector<std::int32_t> ints;
    // The Ri declaration: "uniform float melanin", "vertex color hair_color", "uniform float[2] st".
    std::string declaration() const;
    std::size_t valueCount() const;  // values (not floats)
};

// The curves' primvars for `view` (after LOD and render overrides: the strands rendered), the
// groom's look `look` (GroomSession::hairLook). Besides P and width, which the procedural writes:
//   strand_id      uniform int     low 32 bits of the stable strand ID
//   hair_index     uniform float   (ID mod 2^24) + 1: PxrHairColor's hairIndexPrimvar (never 0)
//   st, scalp_st   uniform float[2] the root's UV on the scalp (when it has UVs)
//   <channel>      uniform float   every strand channel (melanin, melaninRedness, clumpId,
//                                  colorR/G/B, specular, roughness, ...), as Arnold's
//   hair_color     uniform color   the RGB (export_view.h strandColors); vertex color with the
//                                  color nodes' per-point color (a tip, bands, a pattern)
//   opacity, wetness, iridescence  vertex float, from the point channels of those names
//   rman_melanin, rman_redness     PxrHairColor's pigment (pxrHairColorPigment; vertex with a
//                                  per-point wetness, else uniform)
//   rman_dye       constant color  the HairColor's dye
//   rman_color     the RGB look for PxrHairColor (Artistic): hair_color, darkened where wet
//   rman_rgbShare  uniform float   0 the pigment's look, 1 the RGB's (1 when a color node makes the
//                                  output: GroomSession::hairLook colorsByRgb, else 0)
//   rman_specularGainR, rman_coneAngleR, rman_coneAngleTT, rman_coneAngleTRT
//                  uniform float   PxrMarschnerHair's lobes per strand (pxrMarschnerLobes of
//                                  HairColor's specular, roughness and tilt channels, else the look's:
//                                  GroomSession::hairLook), vertex where wet
//   rman_specularOffset constant float  minus the look's cuticle tilt: PxrMarschnerHair's
//                                  specularOffset takes no connection, so the network sets this value
//                                  and a tilt varying per strand (the tilt channel) renders as one
// Names a renderer reserves (P, width, N, Cs, Os, id) and the ones above are never taken by a
// channel of the same name.
std::vector<Primvar> curvePrimvars(const ExportView& view, const HairLook& look);

// One look for the whole groom: each rman_* primvar's mean over its values (curvePrimvars of the
// same view and look), for a renderer path with no per-strand primvars (RenderMan XPU through an
// XGen Interactive Groom, whose curves carry only P, width and N). Missing: the defaults below.
struct ConstantLook {
    float melanin = 0.0f;
    float redness = 0.0f;
    std::array<float, 3> dye{1.0f, 1.0f, 1.0f};
    std::array<float, 3> color{0.0f, 0.0f, 0.0f};
    float rgbShare = 0.0f;
    float specularGainR = 1.75f;
    float coneAngleR = 8.0f;
    float coneAngleTT = 4.0f;
    float coneAngleTRT = 16.0f;
    float specularOffset = -3.0f;
};
ConstantLook constantLook(const std::vector<Primvar>& primvars);

// The shading network above as RIB (hosts/renderman/examples/groom.rib), every host's: PxrPrimvar
// readers of the primvars, PxrHairColor's pigment and RGB look mixed by rman_rgbShare into
// PxrMarschnerHair, specularOffset minus HairColor's `tilt`. Each handle starts with `prefix`
// (Maya's "<groom>_"), each line with `indent`; lines end in a newline.
std::string hairNetworkRib(const std::string& prefix, float tilt, const std::string& indent = {});

}  // namespace renderman
}  // namespace groomlab
