// SPDX-License-Identifier: Apache-2.0
#pragma once

// The viewport's look: the hair material as Arnold's aiStandardHair has it (melanin, redness,
// a dye as the base color, longitudinal and azimuthal roughness, the IOR, the lobes' weights
// and tints), the lights (key, fill, rim as directional lights, one with hair-on-hair shadows),
// the environment (a generated sky or studio, prefiltered for hair as spherical harmonics and a
// handful of directional lights), exposure, tone mapping, the background, and the quality
// mode. No GPU here: the renderer packs it into uniforms (lookdev_renderer.cpp), the tests
// check the math, the panels edit it.
//
// The melanin mapping follows the one the core bakes card colors with (hair_color.h: the
// eumelanin and pheomelanin absorption spectra of d'Eon et al., as aiStandardHair takes them),
// so the viewport, the cards and the render agree on what "melanin 0.35" is.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "groomlab/hair_color.h"

namespace groomlab::app {

using RGB = std::array<float, 3>;

// The display transfer between what a color picker shows (sRGB, 0..1) and what the material
// and the graph store (linear): the IEC 61966-2-1 curve, exact inverses of each other.
float srgbToLinear(float value);
float linearToSrgb(float value);
RGB srgbToLinear(const RGB& color);
RGB linearToSrgb(const RGB& color);

// aiStandardHair's parameters the viewport mirrors (its names, its defaults).
struct HairMaterial {
    // Where the pigment comes from: the groom's own strand channels (melanin, melaninRedness,
    // as the Arnold translator reads them through user data), the groom's RGB channels
    // (colorR/G/B: a HairColor coloring by a map or texture; Arnold's base_color over no
    // melanin) or this material alone (an override, to try a color on any groom).
    enum class Pigment { GroomMelanin, GroomColor, Material };
    Pigment pigment = Pigment::GroomMelanin;
    float melanin = 0.35f;            // 0 white .. 1 black
    float melaninRedness = 0.2f;      // 0 brown .. 1 red
    float melaninRandomize = 0.0f;    // per-strand random offset of the melanin, +/- this
    RGB baseColor{1.0f, 1.0f, 1.0f};  // the dye: absorbed as Chiang's color-to-absorption fit
    float roughness = 0.2f;           // longitudinal (along the strand): the highlights' spread
    float azimuthalRoughness = kHairAzimuthalRoughness;  // across the strand: the lobes' spread around it
    float ior = kHairIor;
    float shift = kHairShiftDegrees;  // degrees: the cuticle tilt, moving the lobes apart
    float specular = 1.0f;            // the R lobe's weight ...
    RGB specularTint{1.0f, 1.0f, 1.0f};
    float specular2 = 1.0f;           // ... the TRT lobe's
    RGB specular2Tint{1.0f, 1.0f, 1.0f};
    float transmission = 1.0f;        // ... the TT lobe's
    RGB transmissionTint{1.0f, 1.0f, 1.0f};
    float diffuse = 0.0f;             // Arnold's cheap diffuse lobe (none by default: the dual scattering stands in)
    float multipleScattering = 1.0f;  // the dual scattering's strength (0: single scattering alone)
    // The viewport's own.
    // The groom's structural color (FeatherColor's iridescence: how much is each point's own, the
    // strands' iridescence channel), its hue turned by iridescenceShift turns as the view runs
    // along a fiber (color_patterns.h's iridescentTint; Arnold's network the same, as a ramp).
    RGB iridescenceColor{0.05f, 0.55f, 0.2f};
    float iridescenceShift = 0.3f;
    float widthScale = 1.0f;          // multiplies the strands' widths
    float shadowDensity = 1.0f;       // how opaque a hair is to light (1: as opaque as rendered)

    std::string presetName;  // the preset it came from, "" when edited
};

// Presets by name: black, brown, blonde, red, white, dyed (a vivid dye over bleached hair).
std::vector<std::string> hairPresetNames();
HairMaterial hairPreset(const std::string& name);

// The groom's HairColor highlight (specular, roughness, tilt) as create_hair_shader maps it to
// Arnold (core's hairLobes: its specular tint, roughness and shift). The lobes' numbers (the
// absorption, the lobe widths) are core's too: hair_color.h.
void applyGroomLook(HairMaterial& material, float lookSpecular, float lookRoughness, float lookTilt = kDefaultHairTilt);

// ---- Lights ---------------------------------------------------------------------------------------

struct DirectionalLight {
    std::string name;
    std::array<float, 3> direction{0.0f, 1.0f, 0.0f};  // toward the light, world space, unit
    RGB color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    bool enabled = true;
    bool shadows = true;  // casts a deep opacity map (hair-on-hair shadows, the scalp in them)
};

// Toward-the-light direction from an azimuth (degrees around y, 0 behind the camera's default)
// and an elevation (degrees above the horizon).
std::array<float, 3> lightDirection(float azimuthDegrees, float elevationDegrees);

enum class Environment { None, Studio, Sky, Overcast, Sunset };
enum class ToneMap { None, ACES, AgX };
enum class Background { Solid, Gradient, Environment };
// Interactive: one frame per change, every frame complete. Refine: the same while anything
// moves, then the viewport keeps accumulating jittered frames (anti-aliasing, true strand
// transparency, every environment light, softer shadows) until the image converges.
enum class Quality { Interactive, Refine };
// What the frames of a stream of changes (a camera drag, a slider's drag, a stroke) draw at.
// Adaptive: a resolution scaled down until a frame fits in a display's refresh (the frame's
// cost measured as it goes), the full picture back once nothing moves. Full: every frame as
// the idle one.
enum class InteractiveQuality { Adaptive, Full };

struct LightingSetup {
    std::vector<DirectionalLight> lights;  // key, fill, rim (any number; the first with shadows casts)
    Environment environment = Environment::Studio;
    float environmentIntensity = 1.0f;
    float environmentRotation = 0.0f;  // degrees around y
    float exposure = 0.0f;             // EV
    ToneMap toneMap = ToneMap::ACES;
    Background background = Background::Gradient;
    // Linear, before the display transform: deep charcoal, #17181B on screen through the default
    // ACES at exposure 0 (the line view clears to the same #17181B).
    RGB backgroundColor{0.030f, 0.031f, 0.033f};
    float backgroundIntensity = 1.0f;          // the environment as background, dimmed
    Quality quality = Quality::Interactive;
    // Refine mode stops accumulating at this many frames (1 to LookdevRenderer::kMaxAccumulatedFrames).
    int refineFrames = 256;
    // The lookdev passes render at this fraction of the view's size (0.25 to 2: a faster
    // interactive picture, or supersampling), the display transform scaling the result.
    float resolutionScale = 1.0f;
    InteractiveQuality interactiveQuality = InteractiveQuality::Adaptive;
    float shadowSoftness = 1.0f;  // the deep opacity map's layer thickness, x the groom's size / 100
    bool ambientOcclusion = true;
    std::string presetName;
};

// Presets: studio (key, fill, rim over a studio environment), daylight (sun over a sky),
// overcast, sunset, rim (a dark backdrop with two rims).
std::vector<std::string> lightingPresetNames();
LightingSetup lightingPreset(const std::string& name);
// A preset picked in the UI: its lights and environment over `current`, whose background, exposure,
// tone mapping and render settings stay the user's.
LightingSetup withLightingPreset(const LightingSetup& current, const std::string& name);

// ---- The environment -----------------------------------------------------------------------------

// A latitude-longitude map (linear RGB, `width` x `height`, row 0 the zenith, column 0 toward
// -z... +x at a quarter turn), generated: nothing to license, nothing to ship.
struct EnvironmentMap {
    int width = 0, height = 0;
    std::vector<float> rgb;  // width * height * 3
    RGB at(int x, int y) const { return {rgb[(y * width + x) * 3], rgb[(y * width + x) * 3 + 1], rgb[(y * width + x) * 3 + 2]}; }
    // The direction a texel looks at (unit, y up).
    static std::array<float, 3> direction(float u, float v);
};
EnvironmentMap makeEnvironment(Environment kind, int width = 128, int height = 64);

// The map's irradiance as 9 spherical harmonics coefficients (RGB each, the order of
// Ramamoorthi and Hanrahan 2001), convolved with the clamped cosine for a diffuse-like look-up.
std::array<RGB, 9> environmentSH(const EnvironmentMap& map);
// The irradiance the SH give toward `direction` (what the shader computes).
RGB irradianceSH(const std::array<RGB, 9>& sh, const std::array<float, 3>& direction);

// A handful of directional lights standing in for the map (median cut, Debevec 2005): the
// bright regions get their own, each with the radiance of its region summed.
std::vector<DirectionalLight> environmentLights(const EnvironmentMap& map, int count);

// A rotation of `direction` around y by degrees.
std::array<float, 3> rotateY(const std::array<float, 3>& direction, float degrees);

// The map prefiltered for a GGX lobe of `roughness` (Karis 2013, "Real Shading in Unreal
// Engine 4": the split sum's first half, the view taken along the normal), at `width` x
// `height`: what a glossy surface reflects of it, looked up by the reflected direction.
// Roughness 0 is the map itself. `samples` importance-samples the lobe per texel.
EnvironmentMap prefilterEnvironment(const EnvironmentMap& map, float roughness, int width, int height, int samples = 256);
// The map's radiance toward a direction, bilinear.
RGB environmentRadiance(const EnvironmentMap& map, const std::array<float, 3>& direction);

// ---- Surfaces: the scalp, props (a head), eyes --------------------------------------------------

// What a mesh is shaded as: a general metal/roughness material (GGX with multiple-scattering
// compensation, the environment through the split sum), skin (a pre-integrated diffusion
// diffuse, two specular lobes, light through thin parts) or an eye (the cornea's refraction
// onto the iris, a procedural iris and sclera). surface_lookdev.frag is the shader; the
// parameters here are its uniforms, the texture files its samplers.
enum class SurfaceKind { Default, Skin, Eye };

struct SurfaceMaterial {
    SurfaceKind kind = SurfaceKind::Default;
    RGB baseColor{0.8f, 0.8f, 0.8f};  // linear; times the albedo map
    float roughness = 0.5f;           // times the roughness map
    float metallic = 0.0f;            // times the metallic map
    float specular = 0.5f;            // the dielectric F0 as Burley 2012 scales it: 0.5 is 4 percent
    float occlusion = 1.0f;           // how much the AO map and the baked vertex occlusion darken
    float normalStrength = 1.0f;      // the normal map's slope
    // Texture files ("": none). The albedo is sRGB; the others linear, gray (their red channel).
    std::string albedoMap, normalMap, roughnessMap, metallicMap, occlusionMap, specularMap;
    float specularMapGain = 1.0f;     // the specular (cavity) map times this before it occludes
    // Cutout: where the albedo map's alpha is below this the surface is cut away, in the picture
    // and in the shadow maps (0: opaque). The baked cards cut their coverage at 0.5, as Maya's do.
    float alphaCutoff = 0.0f;
    // A generated tileable micro-detail normal (pores) repeated `detailScale` times over the UV
    // square (0: none), leaning the normal by `detailStrength`.
    float detailScale = 0.0f;
    float detailStrength = 0.4f;
    // Skin. The diffusion profile is Burley's normalized diffusion (Christensen and Burley
    // 2015) with a diffuse mean free path per channel (scene units: cm, Jensen et al. 2001's
    // measured skin), times a scale (Arnold's subsurface_radius x subsurface_scale). It is
    // applied in screen space (Jimenez 2015's separable blur of the irradiance, the albedo
    // partly before and partly after it) or, the low-end fallback, pre-integrated per
    // curvature (Penner 2011).
    enum class SssMode { Preintegrated, ScreenSpace };
    SssMode sssMode = SssMode::ScreenSpace;
    float sssRadius = 1.0f;           // x the mean free paths: 1 is human skin in a scene in cm
    RGB sssMeanFreePath{0.367f, 0.137f, 0.068f};  // cm (3.67, 1.37, 0.68 mm: Jensen 2001's skin1)
    float sssStrength = 1.0f;         // 0: plain Lambert, 1: the profile
    float sssTexturing = 0.5f;        // how much of the albedo goes before the blur (1: pre-scatter, 0: post-scatter)
    float roughness2 = 0.6f;          // the second, wider specular lobe
    float lobe2Weight = 0.2f;         // its share of the specular
    float cavity = 1.0f;              // the specular map as specular occlusion: its strength
    float transmission = 1.0f;        // light through thin parts (ears, nostrils), by the shadow map's thickness
    // Eyes: the geometry's frame comes from the eye surface itself (surface.h makeEyeSurface);
    // these shape its look.
    RGB irisColor{0.35f, 0.22f, 0.12f};
    RGB scleraColor{0.95f, 0.9f, 0.86f};
    float irisRadius = 0.49f;         // as a fraction of the eye's radius (5.9 of 12 mm)
    float pupilRadius = 0.28f;        // as a fraction of the iris'
    float limbalWidth = 0.12f;        // the dark ring at the iris' edge, as a fraction of the iris' radius
    float corneaIor = 1.376f;
    float irisDetail = 1.0f;          // the fibers' contrast
    float veins = 0.5f;               // the sclera's veins
    float eyeOcclusion = 2.0f;        // the lids' shadow on the eye: the baked occlusion to this power (0: none)
    float tearLine = 0.5f;            // a wet band where the lids meet the eye (0: none)
    float wetness = 1.0f;             // the tear film's specular over the whole eye

    std::string presetName;
};

// Presets by name: clay, scalp, plastic, metal (Default); skin, skin light, skin dark (Skin);
// eye brown, eye blue, eye green (Eye). An unknown name gives clay without a preset name.
std::vector<std::string> surfacePresetNames();
SurfaceMaterial surfacePreset(const std::string& name);
// A parameter by name (the field's name: "roughness", "sssStrength", a color's "irisColor.r",
// "kind" as default|skin|eye, a map as "albedoMap"), from text; false for an unknown name or
// a value that does not parse. The CLI's --prop-set and the tests set materials this way.
bool setSurfaceParam(SurfaceMaterial& material, const std::string& name, const std::string& value);

// ---- The surface shader's look-up tables (shared by the renderer and the tests) ------------------

// The split-sum environment BRDF (Karis 2013): for a GGX lobe of roughness (rows, 0 at the
// top... row 0 roughness 0) seen at n.v (columns, 0 grazing), the scale and bias on F0 that
// give the lobe's directional albedo: specular = prefiltered(R) * (F0 * scale + bias). RG
// per texel, `size` x `size`, importance-sampled with `samples` per texel.
std::vector<float> environmentBrdfLUT(int size, int samples = 128);
// The scale and bias at (roughness, nDotV) by bilinear lookup of such a table (tests).
std::array<float, 2> environmentBrdfAt(const std::vector<float>& lut, int size, float roughness, float nDotV);

// Burley's normalized diffusion profile (Christensen and Burley 2015, "An Approximate
// Reflectance Profile for Efficient Subsurface Scattering": a fit to the searchlight
// problem's exit distribution, (e^(-r/d) + e^(-r/(3d))) / (8 pi d r) per channel, which
// integrates to 1 over the plane): the reflectance at distance `r` from where light
// entered, per channel, for diffuse mean free paths `mfp` (the same units as `r`).
RGB burleyProfile(float r, const RGB& mfp);
// Light through a slab of skin `thickness` thick: e^(-t/d) per channel, diffusion theory's
// far-field decay with the diffuse mean free path as the transport length (Jensen et al.
// 2001; the profile's own tail, e^(-t/(3d)), is a fit to reflectance on a half-space and
// would pass far too much through centimeters of flesh); 1 at 0. Jimenez 2010 reads the
// thickness off the shadow map.
RGB burleyTransmittance(float thickness, const RGB& mfp);
// The default skin's mean free paths in mm (Jensen et al. 2001's measured skin1: red
// travels furthest), which the tables below are built for.
constexpr RGB kSkinMeanFreePathMm{3.67f, 1.37f, 0.68f};
// Skin's diffusion profile at `distanceMm` for the default skin (burleyProfile in mm).
RGB skinDiffusionProfile(float distanceMm);

// The mass of the profile in the strip |x| <= `halfWidth` of the plane (per channel, in
// 0..1), tabulated: what a one-dimensional kernel over the profile (Jimenez 2015's
// separable blur, Penner's ring) gives each cell, with the 1 / r singularity integrated
// out exactly in polar coordinates.
struct ProfileStripTable {
    float xMax = 0.0f;
    std::vector<RGB> mass;  // mass[i] at x = xMax * i / (size - 1)
    RGB at(float halfWidth) const;
};
ProfileStripTable profileStripTable(const RGB& mfp, float xMax, int size = 2048);

// Jimenez et al. 2015 ("Separable Subsurface Scattering"): the profile as a one-dimensional
// kernel of `samples` taps over [-radius, radius] (the same units as `mfp`), the taps
// crowded toward the center (offset = radius * t |t|, t uniform in -1..1), each weighing
// the mass of the profile over its cell, the weights summing to 1 per channel. Applied
// twice, along two screen axes, it approximates the two-dimensional profile.
struct SssKernelSample {
    float offset;
    RGB weight;
};
std::vector<SssKernelSample> separableSssKernel(int samples, const RGB& mfp, float radius);
// The pre-integrated diffuse of Penner 2011 ("Pre-Integrated Skin Shading", SIGGRAPH Advances
// in Real-Time Rendering): the profile integrated around a sphere of radius r (rows: the
// curvature 1 / r from 0 to 1 per mm, row 0 flat) for each n.l (columns, -1 to 1): the
// diffuse light a point with that curvature gets, which wraps past the terminator and
// reddens there. RGB per texel, `width` (n.l) x `height` (curvature); each a mean of
// `steps` samples around the sphere.
std::vector<float> preintegratedSkinLUT(int width, int height, int steps = 256);
// The value of such a table at (nDotL, curvature per mm), bilinear (tests).
RGB preintegratedSkinAt(const std::vector<float>& lut, int width, int height, float nDotL, float curvaturePerMm);
// Light through a slab of the default skin `thicknessMm` thick (burleyTransmittance in mm).
RGB skinTransmittance(float thicknessMm);

// A tileable micro-detail normal map (pores and fine creases: a few octaves of value noise as
// a height field, its slopes as a tangent-space normal), `size` x `size`, RGB in 0..1.
std::vector<float> microNormalTexture(int size, std::uint32_t seed = 7);

}  // namespace groomlab::app
