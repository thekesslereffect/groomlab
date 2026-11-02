// SPDX-License-Identifier: Apache-2.0
// GroomLab's hair shading, one source for every host that draws it: the standalone app's lookdev
// (hosts/app/shaders/hair_shading.glsl includes it) and Maya's Viewport 2.0 hair effect
// (hosts/maya/src/hair_shading.cpp embeds it at build time, renamed into HLSL for DirectX 11 and kept
// as GLSL for the OpenGL Core Profile). Pure functions, no uniforms: each host hands in its material,
// its lights and the number of hairs in each light's way (the app's voxel volumes or deep opacity
// maps, Maya's density grid: the hairs a ray crosses, geometrically, which hairShadowCount turns into
// the shading's).
//
// Calibrated against Arnold's aiStandardHair rendering the same groom in scene-linear Rec.709-sRGB
// (tests/data/app/arnold_compare.py --linear-srgb; --single-scattering for the lobes and the shadows
// alone): Marschner's lobes R, TT and TRT in Karis 2016's closed forms, their widths from the
// roughness by d'Eon's fits, the fiber's absorption from melanin and redness as aiStandardHair maps
// them, Zinke et al. 2008's dual scattering for the light that reaches a hair through the others
// (the fits and their numbers: docs/DEVELOPING.md, "One hair shading").
//
// The words both languages share: GLSL's vector types (vec2-4: Maya renames them float2-4 for HLSL)
// and functions, never a vector built from one scalar (vec3(x)), no fract, mod or inversesqrt (HLSL
// has other names), no global constants (an HLSL effect would take them for uniforms); mix comes
// from the host on HLSL.

// The host's calibration overrides (0: this file's constants; Maya's are all 0, the app's from
// GROOMLAB_APP_TUNE): defined by the host, anywhere.
vec4 hairCalibration();
vec4 hairCalibration2();
vec4 hairCalibration3();

float hairPi() { return 3.14159265; }
float hairSq(float x) { return x * x; }
float hairGauss(float b, float x) { return exp(-0.5 * x * x / (b * b)) / (2.50662827 * b); }
float hairFresnel(float ior, float c) {
    float f0 = hairSq((ior - 1.0) / (ior + 1.0));
    return f0 + (1.0 - f0) * pow(1.0 - clamp(c, 0.0, 1.0), 5.0);
}
// (x - floor(x): Viewport 2.0's HLSL effects compiler gave 0 for frac of some sums.)
float hairFract(float x) { return x - floor(x); }
float hairMod6(float x) { return x - 6.0 * floor(x / 6.0); }
// A calibration constant: the host's override where it gives one (hairCalibration, over 0), else
// this file's.
float hairCal(float given, float own) { return given > 0.0 ? given : own; }

// ---- The fiber's color -------------------------------------------------------------------------

// The absorption inside the fiber (per unit of its radius: the lobes' paths are in radii) of
// aiStandardHair's melanin (0..1) and melanin_redness (the share of it that is pheomelanin): the
// eumelanin and pheomelanin spectra (d'Eon's) at 6.6 x melanin^2, as Arnold renders the five
// pigments of the calibration (single scattering; a fitted exponent came out 2.00).
vec3 hairMelaninSigma(float melanin, float redness) {
    float m = clamp(melanin, 0.0, 1.0);
    float r = clamp(redness, 0.0, 1.0);
    vec3 eu = vec3(0.419, 0.697, 1.37);
    vec3 pheo = vec3(0.187, 0.4, 1.05);
    vec4 cal = hairCalibration();
    float amount = cal.w > 0.5 ? pow(m, cal.w) : m * m;
    return (hairCal(cal.x, 6.6) * amount) * mix(eu, pheo, r);
}

// The fiber's transmittance over its radius (what the lobes take as `color`) from an absorption
// and the dye (aiStandardHair's base color over the melanin).
vec3 hairFiberColor(vec3 sigma, vec3 dye) {
    return clamp(exp(-sigma) * dye, 0.0001, 1.0);
}

// The absorption (per fiber radius) that makes hair look `rgb`: aiStandardHair's base color over no
// melanin, as Arnold maps it (Chiang et al. 2016's fit for the azimuthal roughness b, the color a
// groom's many scatterings settle on; core's hairColorAbsorption). Every hair color shades through
// it (HairColor's, tips, Bands, FeatherColor, a material's base color; a melanin's color is
// core's melaninColor, whose absorption this gives back as hairMelaninSigma's): handed to the
// lobes as it is, an RGB is the transmittance of one pass through the fiber, and TT, TRT and the
// multiple scattering take it to the second, fourth and higher powers: a skin tone drew dark red.
vec3 hairColorSigma(vec3 rgb, float azimuthalRoughness) {
    float b = clamp(azimuthalRoughness, 0.0, 1.0);
    float b2 = b * b;
    float d = 5.969 - 0.215 * b + 2.532 * b2 - 10.73 * b2 * b + 5.574 * b2 * b2 + 0.245 * b2 * b2 * b;
    vec3 l = log(clamp(rgb, 1e-8, 1.0)) / d;
    return l * l;
}

// The structural color: the color's hue turned by `turns`, its saturation and value kept
// (color_patterns.h iridescentTint, the Arnold network's ramp).
vec3 hairHue(vec3 c, float turns) {
    float hi = max(c.r, max(c.g, c.b)), lo = min(c.r, min(c.g, c.b)), d = hi - lo;
    float h = 0.0;
    if (d > 1e-8) {
        if (hi == c.r) h = (c.g - c.b) / d;
        else if (hi == c.g) h = 2.0 + (c.b - c.r) / d;
        else h = 4.0 + (c.r - c.g) / d;
        h /= 6.0;
    }
    float s = hi > 1e-8 ? d / hi : 0.0;
    h = hairFract(h + turns);
    vec3 k = clamp(abs(vec3(hairMod6(h * 6.0), hairMod6(h * 6.0 + 4.0), hairMod6(h * 6.0 + 2.0)) - 3.0) - 1.0, 0.0, 1.0);
    return hi * (1.0 - s + s * k);
}

// The iridescence (x) and Wet's wetness (y) a look color's fourth float carries (core's packWetLook,
// hair_color.h), unpacked per vertex: kWetPacked (4) and over, the wetness in thousandths plus half
// the iridescence; under it, the iridescence plus `dry` (Maya's colors hold 1 + it, the app's it).
vec2 hairLookAlpha(float a, float dry) {
    if (a < 3.5) return vec2(clamp(a - dry, 0.0, 1.0), 0.0);
    float x = a - 4.0;
    float q = floor(x);
    return vec2(clamp(2.0 * (x - q), 0.0, 1.0), clamp(q * 0.001, 0.0, 1.0));
}

// ---- The lobes ---------------------------------------------------------------------------------

// The material the lobes take: x the longitudinal width B (d'Eon's, from the roughness), y the
// azimuthal spread s, z the cuticle tilt (radians), w the index of refraction; the lobes' weights
// (aiStandardHair's specular, transmission, specular2 times their tints; the R lobe's times the
// structural color's tint).
struct HairLobes {
    vec4 shape;
    vec3 weightR;
    vec3 weightTT;
    vec3 weightTRT;
    float multiple;  // aiStandardHair's multiple scattering weight (dual scattering's terms)
};

// Wet hair (the wetness channel, 0 dry .. 1 soaked; core's wetShading, hair_color.h, whose numbers
// soaked `wet` holds: wetShadingUniform): the fiber's color to the power 1 + wet.x w (more absorbed
// inside: darker, more saturated), the R lobe's weight times 1 + wet.y w and the lobes' longitudinal
// width toward wet.z of it (the water-filled cuticle: a narrower, stronger highlight). Nothing at
// all when dry.
void hairWet(float wetness, vec4 wet, inout vec3 color, inout HairLobes m) {
    float w = clamp(wetness, 0.0, 1.0);
    if (w <= 0.0) return;
    float k = 1.0 + wet.x * w;
    color = clamp(pow(color, vec3(k, k, k)), 0.0001, 1.0);
    m.weightR = m.weightR * (1.0 + wet.y * w);
    m.shape.x = m.shape.x * (1.0 + (wet.z - 1.0) * w);
}

// Marschner's lobes for one light: the single scattering (the lobes' own longitudinal widths) and
// the same lobes widened by `spreadSq` (the forward scattering's spread, dual scattering). The
// azimuthal, Fresnel and attenuation terms do not depend on the longitudinal width, so both come
// from one evaluation (docs/research/dual_scatter.md, the exact factorization). `color` is the
// fiber's transmittance over its radius.
void hairLobePair(vec3 T, vec3 L, vec3 V, vec3 color, HairLobes m, float spreadSq, out vec3 narrow, out vec3 wide) {
    vec4 cal3 = hairCalibration3();
    float B = m.shape.x, s = m.shape.y * hairCal(cal3.y, 1.24), shift = m.shape.z, ior = m.shape.w;
    float sinL = clamp(dot(T, L), -1.0, 1.0), sinV = clamp(dot(T, V), -1.0, 1.0);
    float cosThetaD = max(cos(0.5 * abs(asin(sinV) - asin(sinL))), 0.05);
    vec3 Lp = L - sinL * T;
    vec3 Vp = V - sinV * T;
    float cosPhi = dot(Lp, Vp) / sqrt(max(dot(Lp, Lp) * dot(Vp, Vp), 1e-8));
    float cosHalfPhi = sqrt(clamp(0.5 + 0.5 * cosPhi, 0.0, 1.0));
    float sinThetaH = 0.5 * (sinL + sinV);
    // R: the surface reflection, white, the cuticle tilt moving it toward the tip (T runs root to
    // tip: the highlight at theta_h = +shift, as Arnold renders it; TT and TRT the other way).
    float Nr = 0.25 * cosHalfPhi;
    float Fr = hairFresnel(ior, sqrt(clamp(0.5 + 0.5 * dot(V, L), 0.0, 1.0)));
    vec3 xR = (Nr * Fr * hairCal(hairCalibration2().z, 0.89)) * m.weightR;
    // TT: light through the fiber, bright when backlit, the fiber's color once through.
    float sinThetaD = sqrt(max(1.0 - cosThetaD * cosThetaD, 0.0));
    float nPrime = sqrt(max(ior * ior - sinThetaD * sinThetaD, 1.0)) / cosThetaD;
    float a = 1.0 / nPrime;
    float h = cosHalfPhi * (1.0 + a * (0.6 - 0.8 * cosPhi));
    float f = hairFresnel(ior, cosThetaD * sqrt(clamp(1.0 - h * h, 0.0, 1.0)));
    float eTT = 2.0 * sqrt(max(1.0 - h * h * a * a, 0.0)) / cosThetaD;
    vec3 Ttt = pow(color, vec3(eTT, eTT, eTT));
    float kTT = 1.0 / (1.0 / 3.65 + s * s);
    float Ntt = 0.945 * sqrt(kTT / (2.0 * hairPi())) * exp(kTT * (-cosPhi - 1.0));
    vec3 xTT = (Ntt * hairSq(1.0 - f) * hairCal(cal3.z, 0.52)) * Ttt * m.weightTT;
    // TRT: reflected off the back of the fiber, the colored highlight toward the tip.
    float f2 = hairFresnel(ior, cosThetaD * 0.5);
    float eTRT = 3.5 / cosThetaD;
    vec3 Ttrt = pow(color, vec3(eTRT, eTRT, eTRT));
    float kTRT = 1.0 / (1.0 / 17.0 + s * s);
    float Ntrt = 0.758 * sqrt(kTRT / (2.0 * hairPi())) * exp(kTRT * (cosPhi - 1.0));
    vec3 xTRT = (Ntrt * f2 * hairSq(1.0 - f2) * hairCal(cal3.w, 0.82)) * Ttrt * m.weightTRT;
    float gR = sinThetaH - shift, gTT = sinThetaH + 0.5 * shift, gTRT = sinThetaH + 1.5 * shift;
    narrow = hairGauss(B, gR) * xR + hairGauss(0.5 * B, gTT) * xTT + hairGauss(2.0 * B, gTRT) * xTRT;
    wide = hairGauss(sqrt(B * B + spreadSq), gR) * xR + hairGauss(sqrt(0.25 * B * B + spreadSq), gTT) * xTT +
           hairGauss(sqrt(4.0 * B * B + spreadSq), gTRT) * xTRT;
}

// ---- Through the other hairs -------------------------------------------------------------------

// The number of hairs in a light's way as the shading takes it (its optical depth: the light
// straight through is exp(-count)), from the hairs a ray crosses there, geometrically (each fiber
// blocking its projected width: the hosts' grids and volumes). Arnold's shadow rays through the
// same groom (arnold_compare.py --single-scattering) go dark more gently than one hair a crossing,
// with a quick first step: a strand's own clump, finer than any grid, shadows it some.
float hairShadowCount(float hairs) {
    float g = max(hairs, 0.0);
    return hairCal(hairCalibration().z, 1.21) * g + 0.33 * (1.0 - exp(-g / 0.18));
}

// Zinke's average attenuations of one hair: forward (af: what passes on through it, TT's path
// through the fiber's color) and backward (ab: what it sends back, the TRT's and R's share).
void hairAttenuations(vec3 color, out vec3 af, out vec3 ab) {
    float pf = hairCal(hairCalibration2().y, 1.95);
    af = 0.04 + 0.86 * pow(color, vec3(pf, pf, pf));
    ab = 0.02 + hairCal(hairCalibration2().w, 0.34) * pow(color, vec3(0.85, 0.85, 0.85));
}

// One light reaching a hair through `count` others (hairShadowCount's): the single scattering of
// the light that comes straight through them, the light forward-scattered by them (dual scattering:
// attenuated by their forward attenuation, the lobes widened by their spread) and the back
// scattering off the hairs behind, all times the fiber's projected width toward the light
// (Marschner's cos theta_i); and the multiple scattering those leave out, from every way.
vec3 hairShadeLight(vec3 T, vec3 L, vec3 V, vec3 color, HairLobes m, float count, vec3 af, vec3 ab) {
    float sinL = clamp(dot(T, L), -1.0, 1.0), sinV = clamp(dot(T, V), -1.0, 1.0);
    float sinThetaH = 0.5 * (sinL + sinV);
    float B = m.shape.x, shift = m.shape.z, ms = m.multiple;
    float direct = exp(-count);
    float bTT = 0.5 * B;
    float spreadSq = count * bTT * bTT;
    // The back scattering lobe, Zinke's A_b = a_b a_f^2 / (1 - a_f^2) channel by channel (its
    // color is what makes light hair's depth saturated in Arnold), scaled to Arnold's brightness.
    vec4 cal2 = hairCalibration2();
    vec3 af2 = min(af * af, vec3(0.95, 0.95, 0.95));
    vec3 Ab = ab * af2 / (1.0 - af2);
    // Its longitudinal spread, Zinke et al.'s for the light sent back: sqrt(beta_b^2 + 2 beta_f^2),
    // the back lobe's width (R's) and twice the forward spread's (TT's). (It was the TRT lobe's
    // with one forward spread, about 1.7 times as wide: strands far off the specular cone, light
    // hair's tips hanging under the head's curve, took back scattering Arnold gives them only a
    // little of, and read too bright.)
    float sigmaB = sqrt(B * B + 2.0 * bTT * bTT);
    float gb = hairGauss(sigmaB, sinThetaH + 2.0 * shift);
    vec3 back = (hairCal(hairCalibration().y, 0.22) * 2.0 / hairPi() * ms) * Ab;
    vec3 narrow;
    vec3 wide;
    hairLobePair(T, L, V, color, m, count > 0.05 ? spreadSq : 0.0, narrow, wide);
    vec3 result = direct * (narrow + 0.7 * gb * back);
    if (count > 0.05) {
        vec3 Tf = (hairCal(cal2.x, 0.053) * ms) * pow(af, vec3(count, count, count));
        float gbSpread = hairGauss(sqrt(sigmaB * sigmaB + spreadSq), sinThetaH + 2.0 * shift);
        result += Tf * (wide + (hairPi() * 0.7 * gbSpread) * back);
    }
    // The light scattered many times among the hairs comes to a strand from every way: in Arnold
    // light hair keeps its brightness however a strand lies, where the terms above (the light's
    // and the view's longitudinal angles through the lobes, times cos theta_i) fade for a strand
    // along the light or the view: a furball's strands toward the camera, the gaps between its
    // clumps, its rims. What they lose, (1 - cos theta_i cos theta_o)^2, comes back as light of A_b's
    // color through the hairs in the way at half their forward attenuation (fitted on a furball
    // and the head against Arnold: dark hair, whose A_b is nothing, keeps its look).
    float cosI = sqrt(max(1.0 - sinL * sinL, 0.0)), cosO = sqrt(max(1.0 - sinV * sinV, 0.0));
    float faded = hairSq(1.0 - cosI * cosO);
    float halfCount = 0.5 * count;
    vec3 around = (hairCal(hairCalibration3().x, 0.17) * ms * faded) * Ab * pow(af, vec3(halfCount, halfCount, halfCount));
    return result * cosI + around;
}

// What light from everywhere (an environment's irradiance, an ambient light) shows of the fiber:
// its albedo, by the multiple scattering's weight.
vec3 hairAmbientAlbedo(vec3 color, float ms) {
    return pow(color, vec3(1.5, 1.5, 1.5)) * (0.5 + 0.5 * ms);
}
