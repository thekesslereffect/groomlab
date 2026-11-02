// SPDX-License-Identifier: Apache-2.0
#include "groomlab/hair_color.h"
#include "groomlab/portable_math.h"

#include <algorithm>
#include <cmath>

#include "groomlab/groom_session.h"

namespace groomlab {

std::array<float, 3> melaninColor(float melanin, float redness, const std::array<float, 3>& dye) {
    std::array<float, 3> sigma = hairMelaninAbsorption(melanin, redness);
    const std::array<float, 3> tint = hairColorAbsorption(dye);
    for (int k = 0; k < 3; ++k) sigma[k] += tint[k];
    std::array<float, 3> c = hairAbsorptionColor(sigma);
    for (int k = 0; k < 3; ++k) c[k] = dye[k] > 0.0f ? c[k] : 0.0f;  // a black dye: black
    return c;
}

std::array<float, 3> dyedHairColor(const std::array<float, 3>& color, const std::array<float, 3>& dye) {
    std::array<float, 3> sigma = hairColorAbsorption(color);
    const std::array<float, 3> tint = hairColorAbsorption(dye);
    for (int k = 0; k < 3; ++k) sigma[k] += tint[k];
    std::array<float, 3> c = hairAbsorptionColor(sigma);
    for (int k = 0; k < 3; ++k) c[k] = color[k] > 0.0f && dye[k] > 0.0f ? c[k] : 0.0f;  // black: black
    return c;
}

std::array<float, 3> varyHairColor(const std::array<float, 3>& rgb, float hueTurn, float saturationScale, float valueScale) {
    std::array<float, 3> c{};
    for (int k = 0; k < 3; ++k) c[k] = std::clamp(rgb[k], 0.0f, 1.0f);
    if (hueTurn != 0.0f || saturationScale != 1.0f) {
        // RGB -> HSL (hue in turns).
        const float hi = std::max({c[0], c[1], c[2]}), lo = std::min({c[0], c[1], c[2]});
        const float l = 0.5f * (hi + lo), d = hi - lo;
        if (d > 1e-7f) {
            float h;
            if (hi == c[0]) h = (c[1] - c[2]) / d;
            else if (hi == c[1]) h = 2.0f + (c[2] - c[0]) / d;
            else h = 4.0f + (c[0] - c[1]) / d;
            h = h / 6.0f + hueTurn;
            h -= std::floor(h);
            const float s = std::clamp(d / std::max(1.0f - std::abs(2.0f * l - 1.0f), 1e-7f) * saturationScale, 0.0f, 1.0f);
            // HSL -> RGB.
            const float chroma = (1.0f - std::abs(2.0f * l - 1.0f)) * s;
            const float sector = h * 6.0f;
            const float x = chroma * (1.0f - std::abs(std::fmod(sector, 2.0f) - 1.0f));
            const float m = l - 0.5f * chroma;
            const int i = static_cast<int>(std::floor(sector)) % 6;
            const float r[6] = {chroma, x, 0.0f, 0.0f, x, chroma}, g[6] = {x, chroma, chroma, x, 0.0f, 0.0f},
                        b[6] = {0.0f, 0.0f, x, chroma, chroma, x};
            c = {r[i] + m, g[i] + m, b[i] + m};
        }
    }
    for (float& v : c) v = std::clamp(v * valueScale, 0.0f, 1.0f);
    return c;
}

std::array<float, 3> defaultHairColor() { return melaninColor(kDefaultMelanin, kDefaultMelaninRedness); }

HairLobes hairLobes(float specular, float roughness, float tilt) {
    HairLobes lobes;
    lobes.specularTint = std::min(1.0f, specular / kHairFullSpecular);
    lobes.roughness = kHairRoughnessMin + kHairRoughnessRange * roughness;
    lobes.shiftDegrees = tilt;
    return lobes;
}

HairLobes hairLobes(const HairLook& look) { return hairLobes(look.specular, look.roughness, look.tilt); }

float hairLongitudinalWidth(float roughness) {
    // d'Eon's variance v = (0.726 b + 0.812 b^2 + 3.7 b^20)^2 of a lobe that, around the specular
    // cone, is close to a Gaussian in theta_i + theta_o of variance v: in theta_h, of variance v / 4
    // (Arnold's aiStandardHair renders the R lobe that wide: arnold_compare.py --single-scattering).
    const float r = std::clamp(roughness, 0.02f, 1.0f);
    const float s = 0.726f * r + 0.812f * r * r + 3.7f * pm::pow(r, 20.0f);
    return std::clamp(0.5f * s, 0.02f, 1.5f);
}

float hairAzimuthalSpread(float azimuthalRoughness) {
    const float r = std::clamp(azimuthalRoughness, 0.0f, 1.0f);
    return 0.265f * r + 1.194f * r * r + 5.372f * pm::pow(r, 22.0f);
}

std::array<float, 3> hairMelaninAbsorption(float melanin, float redness) {
    const float eu[3] = {0.419f, 0.697f, 1.37f}, pheo[3] = {0.187f, 0.4f, 1.05f};
    const float m = std::clamp(melanin, 0.0f, 1.0f);
    const float amount = 6.6f * m * m;
    const float r = std::clamp(redness, 0.0f, 1.0f);
    std::array<float, 3> sigma{};
    for (int k = 0; k < 3; ++k) sigma[k] = amount * ((1.0f - r) * eu[k] + r * pheo[k]);
    return sigma;
}

namespace {

// Chiang et al. 2016's divisor for the azimuthal roughness b.
float chiangDivisor(float azimuthalRoughness) {
    const float b = std::clamp(azimuthalRoughness, 0.0f, 1.0f), b2 = b * b;
    return 5.969f - 0.215f * b + 2.532f * b2 - 10.73f * b2 * b + 5.574f * b2 * b2 + 0.245f * b2 * b2 * b;
}

}  // namespace

std::array<float, 3> hairColorAbsorption(const std::array<float, 3>& rgb, float azimuthalRoughness) {
    const float d = chiangDivisor(azimuthalRoughness);
    std::array<float, 3> sigma{};
    for (int k = 0; k < 3; ++k) {
        const float l = pm::log(std::clamp(rgb[static_cast<std::size_t>(k)], 1e-8f, 1.0f)) / d;
        sigma[static_cast<std::size_t>(k)] = l * l;
    }
    return sigma;
}

std::array<float, 3> hairAbsorptionColor(const std::array<float, 3>& absorption, float azimuthalRoughness) {
    const float d = chiangDivisor(azimuthalRoughness);
    std::array<float, 3> c{};
    for (int k = 0; k < 3; ++k) c[k] = std::clamp(pm::exp(-d * std::sqrt(std::max(absorption[k], 0.0f))), 1e-8f, 1.0f);
    return c;
}

std::array<float, 3> hairAppearanceColor(const std::array<float, 3>& baseColor) {
    const std::array<float, 3> sigma = hairColorAbsorption(baseColor);
    std::array<float, 3> out{};
    for (int k = 0; k < 3; ++k) {
        out[k] = kAppearanceSheen +
                 (1.0f - kAppearanceSheen) * pm::exp(-kAppearanceScale * pm::pow(std::max(sigma[k], 0.0f), kAppearancePower));
    }
    return out;
}

std::array<float, 3> hairBaseOfAppearance(const std::array<float, 3>& appearance) {
    std::array<float, 3> sigma{};
    for (int k = 0; k < 3; ++k) {
        const float share = std::clamp((appearance[k] - kAppearanceSheen) / (1.0f - kAppearanceSheen), 1e-6f, 1.0f);
        sigma[k] = pm::pow(-pm::log(share) / kAppearanceScale, 1.0f / kAppearancePower);
    }
    return hairAbsorptionColor(sigma);
}

WetShading wetShading(float wetness) {
    const float w = std::clamp(wetness, 0.0f, 1.0f);
    WetShading wet;
    wet.absorption = 1.0f + kWetAbsorption * w;
    wet.specular = 1.0f + kWetSpecular * w;
    wet.roughness = 1.0f - kWetRoughness * w;
    return wet;
}

std::array<float, 3> wetFiberColor(const std::array<float, 3>& color, float wetness) {
    const WetShading wet = wetShading(wetness);
    std::array<float, 3> out{};
    for (int k = 0; k < 3; ++k) out[k] = pm::pow(std::clamp(color[k], 0.0f, 1.0f), wet.absorption);
    return out;
}

HairLobes wetLobes(const HairLobes& lobes, float wetness) {
    const WetShading wet = wetShading(wetness);
    HairLobes out = lobes;
    out.specularTint *= wet.specular;
    out.roughness *= wet.roughness;
    return out;
}

std::array<float, 4> wetShadingUniform(float roughness) {
    const float dry = hairLongitudinalWidth(roughness);
    const float soaked = hairLongitudinalWidth(roughness * wetShading(1.0f).roughness);
    return {kWetAbsorption, kWetSpecular, dry > 0.0f ? soaked / dry : 1.0f, 0.0f};
}

float packWetLook(float iridescence, float wetness) {
    return kWetPacked + std::round(std::clamp(wetness, 0.0f, 1.0f) * 1000.0f) + 0.5f * std::clamp(iridescence, 0.0f, 1.0f);
}

}  // namespace groomlab
