// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cmath>

#include "groomlab/math.h"

// Color conversions, a color as a Vec3f (r, g, b). (Hair's own color model, melanin and the
// rest, is hair_color.h.)

namespace groomlab {

// A linear color's luminance (Rec. 709's weights).
inline float rec709Luminance(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

// sRGB's transfer curve, a channel: encoded to linear and back (past 0..1 the curve carries on),
// and the same of a channel held to 0..1 first (what an 8-bit texel holds).
inline float srgbToLinear(float v) { return v <= 0.04045f ? v / 12.92f : pm::pow((v + 0.055f) / 1.055f, 2.4f); }
inline float linearToSrgb(float v) { return v <= 0.0031308f ? 12.92f * v : 1.055f * pm::pow(v, 1.0f / 2.4f) - 0.055f; }
inline float srgbToLinear01(float v) { return srgbToLinear(std::clamp(v, 0.0f, 1.0f)); }
inline float linearToSrgb01(float v) { return linearToSrgb(std::clamp(v, 0.0f, 1.0f)); }

// Hue, saturation, lightness as SeExpr makes them (hue 0 to 1). Out-of-range colors keep
// SeExpr's answers (a negative channel or one over 1 gives a saturation past 1).
inline Vec3f rgbToHsl(const Vec3f& c) {
    const float mn = std::min({c.x, c.y, c.z}), mx = std::max({c.x, c.y, c.z});
    const float sum = mn + mx, diff = mx - mn, l = sum * 0.5f;
    if (diff < 1e-6f) return {0.0f, 0.0f, l};
    float s = 0.0f;
    if (l <= 0.5f) s = mn < 0.0f ? 1.0f - mn : diff / sum;
    else s = mx > 1.0f ? mx : diff / (2.0f - sum);
    float h = 0.0f;
    if (c.x == mx) h = (c.y - c.z) / diff;
    else if (c.y == mx) h = (c.z - c.x) / diff + 2.0f;
    else h = (c.x - c.y) / diff + 4.0f;
    h /= 6.0f;
    h -= std::floor(h);
    return {h, s, l};
}

// One channel of hslToRgb: x to y and back around the hue circle.
inline float hslChannel(float x, float y, float h) {
    h -= std::floor(h);
    if (h < 1.0f / 6.0f) return x + (y - x) * h * 6.0f;
    if (h < 3.0f / 6.0f) return y;
    if (h < 4.0f / 6.0f) return x + (y - x) * (4.0f / 6.0f - h) * 6.0f;
    return x;
}

// rgbToHsl's inverse.
inline Vec3f hslToRgb(const Vec3f& hsl) {
    const float h = hsl.x, s = hsl.y, l = hsl.z;
    if (s <= 0.0f) return {l, l, l};
    float y = 0.0f;
    if (l < 0.5f) y = s > 1.0f ? 2.0f * l + s - 1.0f : l + l * s;
    else y = s > 1.0f ? s : l + s - l * s;
    const float x = 2.0f * l - y;
    return {hslChannel(x, y, h + 1.0f / 3.0f), hslChannel(x, y, h), hslChannel(x, y, h - 1.0f / 3.0f)};
}

}  // namespace groomlab
