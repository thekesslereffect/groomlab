// SPDX-License-Identifier: Apache-2.0
#include "look.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include "groomlab/color.h"

namespace groomlab::app {

namespace {

constexpr float kPi = 3.14159265358979f;
float radians(float degrees) { return degrees * kPi / 180.0f; }

std::array<float, 3> normalized(std::array<float, 3> v) {
    const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > 0.0f) for (float& c : v) c /= n;
    return v;
}

DirectionalLight makeLight(const char* name, float azimuth, float elevation, RGB color, float intensity, bool shadows) {
    DirectionalLight l;
    l.name = name;
    l.direction = lightDirection(azimuth, elevation);
    l.color = color;
    l.intensity = intensity;
    l.shadows = shadows;
    return l;
}

}  // namespace

// ---- Material ---------------------------------------------------------------------------------

float srgbToLinear(float value) { return groomlab::srgbToLinear01(value); }  // (color.h: every host's)

float linearToSrgb(float value) { return groomlab::linearToSrgb01(value); }

RGB srgbToLinear(const RGB& c) { return {srgbToLinear(c[0]), srgbToLinear(c[1]), srgbToLinear(c[2])}; }

RGB linearToSrgb(const RGB& c) { return {linearToSrgb(c[0]), linearToSrgb(c[1]), linearToSrgb(c[2])}; }

std::vector<std::string> hairPresetNames() { return {"groom", "black", "brown", "blonde", "red", "white", "dyed"}; }

HairMaterial hairPreset(const std::string& name) {
    HairMaterial m;
    m.presetName = name;
    if (name == "groom") return m;  // the groom's channels, Arnold's defaults otherwise
    m.pigment = HairMaterial::Pigment::Material;
    if (name == "black") {
        m.melanin = 1.0f;
        m.melaninRedness = 0.0f;
    } else if (name == "brown") {
        m.melanin = 0.5f;
        m.melaninRedness = 0.25f;
        m.melaninRandomize = 0.1f;
    } else if (name == "blonde") {
        m.melanin = 0.12f;
        m.melaninRedness = 0.45f;
        m.melaninRandomize = 0.1f;
    } else if (name == "red") {
        m.melanin = 0.4f;
        m.melaninRedness = 0.95f;
        m.melaninRandomize = 0.08f;
    } else if (name == "white") {
        m.melanin = 0.01f;
        m.melaninRedness = 0.0f;
        m.melaninRandomize = 0.01f;
    } else if (name == "dyed") {
        // Bleached, then a vivid violet dye (the swatch's): the base color goes through Chiang's fit.
        m.melanin = 0.02f;
        m.melaninRedness = 0.0f;
        m.baseColor = {0.37f, 0.15f, 1.0f};
    } else {
        m.presetName.clear();
    }
    return m;
}

void applyGroomLook(HairMaterial& material, float lookSpecular, float lookRoughness, float lookTilt) {
    const HairLobes lobes = hairLobes(lookSpecular, lookRoughness, lookTilt);
    material.specularTint = {lobes.specularTint, lobes.specularTint, lobes.specularTint};
    material.roughness = lobes.roughness;
    material.shift = lobes.shiftDegrees;
}

// ---- Lights -----------------------------------------------------------------------------------

std::array<float, 3> lightDirection(float azimuthDegrees, float elevationDegrees) {
    const float a = radians(azimuthDegrees), e = radians(elevationDegrees);
    return normalized({std::sin(a) * std::cos(e), std::sin(e), std::cos(a) * std::cos(e)});
}

std::array<float, 3> rotateY(const std::array<float, 3>& d, float degrees) {
    const float a = radians(degrees), c = std::cos(a), s = std::sin(a);
    return {c * d[0] + s * d[2], d[1], -s * d[0] + c * d[2]};
}

std::vector<std::string> lightingPresetNames() { return {"studio", "daylight", "overcast", "sunset", "rim", "key", "env"}; }

LightingSetup lightingPreset(const std::string& name) {
    LightingSetup s;
    s.presetName = name;
    if (name == "studio") {
        s.lights = {makeLight("key", -35.0f, 35.0f, {1.0f, 0.97f, 0.92f}, 2.0f, true),
                    makeLight("fill", 55.0f, 10.0f, {0.85f, 0.9f, 1.0f}, 0.5f, true),
                    makeLight("rim", 160.0f, 30.0f, {1.0f, 1.0f, 1.0f}, 1.5f, true)};
        s.environment = Environment::Studio;
        s.environmentIntensity = 0.4f;  // over the default gradient (LightingSetup)
    } else if (name == "daylight") {
        s.lights = {makeLight("sun", -40.0f, 50.0f, {1.0f, 0.95f, 0.85f}, 5.0f, true)};
        s.environment = Environment::Sky;
        s.environmentIntensity = 1.0f;
        s.background = Background::Environment;
        s.backgroundIntensity = 0.6f;
    } else if (name == "overcast") {
        s.lights = {makeLight("sky", 0.0f, 70.0f, {1.0f, 1.0f, 1.0f}, 0.8f, true)};
        s.environment = Environment::Overcast;
        s.environmentIntensity = 1.6f;
        s.background = Background::Environment;
        s.backgroundIntensity = 0.5f;
    } else if (name == "sunset") {
        s.lights = {makeLight("sun", 130.0f, 8.0f, {1.0f, 0.55f, 0.25f}, 6.0f, true),
                    makeLight("sky fill", -60.0f, 40.0f, {0.5f, 0.6f, 0.9f}, 0.4f, true)};
        s.environment = Environment::Sunset;
        s.environmentIntensity = 1.0f;
        s.background = Background::Environment;
        s.backgroundIntensity = 0.7f;
    } else if (name == "key") {
        // The studio's key alone over black: the shading and its shadows by themselves (the
        // Arnold comparison's other half, arnold_compare.py --lights key).
        s.lights = {makeLight("key", -35.0f, 35.0f, {1.0f, 0.97f, 0.92f}, 2.0f, true)};
        s.environment = Environment::None;
        s.background = Background::Solid;
        s.backgroundColor = {0.0f, 0.0f, 0.0f};
    } else if (name == "env") {
        // The studio environment alone (arnold_compare.py --lights env).
        s.environment = Environment::Studio;
        s.environmentIntensity = 0.4f;
        s.background = Background::Solid;
        s.backgroundColor = {0.0f, 0.0f, 0.0f};
    } else if (name == "rim") {
        s.lights = {makeLight("key", -20.0f, 25.0f, {1.0f, 1.0f, 1.0f}, 0.6f, true),
                    makeLight("rim left", -140.0f, 25.0f, {1.0f, 0.9f, 0.8f}, 3.5f, true),
                    makeLight("rim right", 140.0f, 25.0f, {0.8f, 0.9f, 1.0f}, 3.5f, true)};
        s.environment = Environment::None;
        s.background = Background::Solid;
        s.backgroundColor = {0.02f, 0.02f, 0.025f};
    } else {
        s.presetName.clear();
        return lightingPreset("studio");
    }
    return s;
}

LightingSetup withLightingPreset(const LightingSetup& current, const std::string& name) {
    const LightingSetup preset = lightingPreset(name);
    LightingSetup out = current;
    out.lights = preset.lights;
    out.environment = preset.environment;
    out.environmentIntensity = preset.environmentIntensity;
    out.environmentRotation = preset.environmentRotation;
    out.presetName = preset.presetName;
    return out;
}

// ---- Environment --------------------------------------------------------------------------------

std::array<float, 3> EnvironmentMap::direction(float u, float v) {
    // u around y from -z through +x, v from the zenith (0) to the nadir (1).
    const float phi = (u - 0.5f) * 2.0f * kPi, theta = v * kPi;
    return {std::sin(theta) * std::sin(phi), std::cos(theta), -std::sin(theta) * std::cos(phi)};
}

namespace {

// A soft disk of light around `center` (unit): 1 inside `radius` (radians), fading out over
// `soft` more.
float disk(const std::array<float, 3>& d, const std::array<float, 3>& center, float radius, float soft) {
    const float c = std::clamp(d[0] * center[0] + d[1] * center[1] + d[2] * center[2], -1.0f, 1.0f);
    const float angle = std::acos(c);
    return 1.0f - std::clamp((angle - radius) / std::max(soft, 1e-4f), 0.0f, 1.0f);
}

RGB mix(const RGB& a, const RGB& b, float t) { return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t}; }

RGB radianceAt(Environment kind, const std::array<float, 3>& d) {
    const float up = d[1];  // -1 nadir .. 1 zenith
    switch (kind) {
        case Environment::None:
            return {0.0f, 0.0f, 0.0f};
        case Environment::Studio: {
            // A grey cyclorama: a lit ceiling, a dim floor, a large softbox high on the left, a
            // smaller one on the right, a strip behind.
            RGB c = mix({0.08f, 0.08f, 0.085f}, {0.35f, 0.35f, 0.36f}, 0.5f + 0.5f * up);
            const float left = disk(d, lightDirection(-50.0f, 45.0f), 0.25f, 0.2f);
            const float right = disk(d, lightDirection(70.0f, 20.0f), 0.15f, 0.15f);
            const float back = disk(d, lightDirection(175.0f, 35.0f), 0.3f, 0.25f);
            for (int k = 0; k < 3; ++k) c[k] += 4.0f * left + 2.0f * right * (k == 2 ? 1.1f : 0.95f) + 1.5f * back;
            return c;
        }
        case Environment::Sky: {
            // A clear sky: blue overhead paling to the horizon, a warm sun, a brown ground.
            const float sun = disk(d, lightDirection(-40.0f, 50.0f), 0.02f, 0.01f);
            const float glow = disk(d, lightDirection(-40.0f, 50.0f), 0.0f, 0.6f);
            RGB sky = up >= 0.0f ? mix({0.8f, 0.85f, 0.95f}, {0.25f, 0.45f, 0.95f}, std::pow(up, 0.6f))
                                 : mix({0.5f, 0.42f, 0.33f}, {0.3f, 0.26f, 0.2f}, std::min(1.0f, -up * 2.0f));
            if (up >= 0.0f) for (int k = 0; k < 3; ++k) sky[k] += 0.6f * glow * (k == 2 ? 0.6f : 1.0f);
            for (int k = 0; k < 3; ++k) sky[k] += 60.0f * sun * (k == 2 ? 0.8f : 1.0f);
            return sky;
        }
        case Environment::Overcast: {
            // Cloud cover: bright and even above, darker toward the horizon, a grey ground.
            if (up >= 0.0f) return mix({0.7f, 0.72f, 0.76f}, {1.4f, 1.42f, 1.45f}, up);
            return mix({0.45f, 0.45f, 0.45f}, {0.25f, 0.25f, 0.25f}, std::min(1.0f, -up * 2.0f));
        }
        case Environment::Sunset: {
            const auto sunDir = lightDirection(130.0f, 8.0f);
            const float sun = disk(d, sunDir, 0.025f, 0.01f);
            const float glow = disk(d, sunDir, 0.0f, 0.9f);
            RGB sky = up >= 0.0f ? mix({0.95f, 0.5f, 0.3f}, {0.15f, 0.15f, 0.4f}, std::pow(up, 0.5f))
                                 : mix({0.3f, 0.2f, 0.18f}, {0.12f, 0.1f, 0.1f}, std::min(1.0f, -up * 2.0f));
            for (int k = 0; k < 3; ++k) sky[k] += glow * glow * (k == 0 ? 1.5f : k == 1 ? 0.6f : 0.2f);
            for (int k = 0; k < 3; ++k) sky[k] += 40.0f * sun * (k == 0 ? 1.0f : k == 1 ? 0.5f : 0.2f);
            return sky;
        }
    }
    return {0.0f, 0.0f, 0.0f};
}

}  // namespace

EnvironmentMap makeEnvironment(Environment kind, int width, int height) {
    EnvironmentMap map;
    map.width = width;
    map.height = height;
    map.rgb.resize(static_cast<std::size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto d = EnvironmentMap::direction((x + 0.5f) / width, (y + 0.5f) / height);
            const RGB c = radianceAt(kind, d);
            float* out = map.rgb.data() + (static_cast<std::size_t>(y) * width + x) * 3;
            out[0] = c[0];
            out[1] = c[1];
            out[2] = c[2];
        }
    }
    return map;
}

std::array<RGB, 9> environmentSH(const EnvironmentMap& map) {
    std::array<RGB, 9> sh{};
    if (map.width == 0 || map.height == 0) return sh;
    // Project onto the real SH basis, then weight each band by the clamped cosine's (pi,
    // 2pi/3, pi/4) folded with the basis constants into the irradiance polynomial of
    // irradianceSH: 1, y, z, x, xy, yz, 3z^2-1, xz, x^2-y^2.
    std::array<double, 9> acc[3] = {};
    for (int y = 0; y < map.height; ++y) {
        const float theta = (y + 0.5f) / map.height * kPi;
        const float solidAngle = (2.0f * kPi / map.width) * (kPi / map.height) * std::sin(theta);
        for (int x = 0; x < map.width; ++x) {
            const auto d = EnvironmentMap::direction((x + 0.5f) / map.width, (y + 0.5f) / map.height);
            const RGB c = map.at(x, y);
            const float basis[9] = {0.282095f, 0.488603f * d[1], 0.488603f * d[2], 0.488603f * d[0],
                                    1.092548f * d[0] * d[1], 1.092548f * d[1] * d[2],
                                    0.315392f * (3.0f * d[2] * d[2] - 1.0f), 1.092548f * d[0] * d[2],
                                    0.546274f * (d[0] * d[0] - d[1] * d[1])};
            for (int k = 0; k < 3; ++k) {
                for (int i = 0; i < 9; ++i) acc[k][i] += static_cast<double>(c[k] * basis[i] * solidAngle);
            }
        }
    }
    // Irradiance E(n) = sum A_l L_lm Y_lm(n); A_0 = pi, A_1 = 2pi/3, A_2 = pi/4. Folding Y's
    // constants: the polynomial's coefficients below.
    const float c1 = 0.429043f, c2 = 0.511664f, c3 = 0.743125f, c4 = 0.886227f, c5 = 0.247708f;
    for (int k = 0; k < 3; ++k) {
        const auto& L = acc[k];
        sh[0][k] = static_cast<float>(c4 * L[0] - c5 * L[6]);  // constant (L00 and part of L20)
        sh[1][k] = static_cast<float>(2.0 * c2 * L[1]);        // y
        sh[2][k] = static_cast<float>(2.0 * c2 * L[2]);        // z
        sh[3][k] = static_cast<float>(2.0 * c2 * L[3]);        // x
        sh[4][k] = static_cast<float>(2.0 * c1 * L[4]);        // xy
        sh[5][k] = static_cast<float>(2.0 * c1 * L[5]);        // yz
        sh[6][k] = static_cast<float>(c3 * L[6]);              // z^2 (3z^2 - 1 handled: c3 L20 z^2, the -1 part in sh[0])
        sh[7][k] = static_cast<float>(2.0 * c1 * L[7]);        // xz
        sh[8][k] = static_cast<float>(c1 * L[8]);              // x^2 - y^2
    }
    return sh;
}

RGB irradianceSH(const std::array<RGB, 9>& sh, const std::array<float, 3>& n) {
    const float x = n[0], y = n[1], z = n[2];
    RGB e{};
    for (int k = 0; k < 3; ++k) {
        e[k] = sh[0][k] + sh[1][k] * y + sh[2][k] * z + sh[3][k] * x + sh[4][k] * x * y + sh[5][k] * y * z +
               sh[6][k] * z * z + sh[7][k] * x * z + sh[8][k] * (x * x - y * y);
        e[k] = std::max(0.0f, e[k]);
    }
    return e;
}

std::vector<DirectionalLight> environmentLights(const EnvironmentMap& map, int count) {
    std::vector<DirectionalLight> lights;
    if (map.width == 0 || map.height == 0 || count <= 0) return lights;
    // Luminance x solid angle per texel: the energy the cuts balance.
    std::vector<float> energy(static_cast<std::size_t>(map.width) * map.height);
    for (int y = 0; y < map.height; ++y) {
        const float theta = (y + 0.5f) / map.height * kPi;
        const float solidAngle = (2.0f * kPi / map.width) * (kPi / map.height) * std::sin(theta);
        for (int x = 0; x < map.width; ++x) {
            const RGB c = map.at(x, y);
            energy[static_cast<std::size_t>(y) * map.width + x] = (0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]) * solidAngle;
        }
    }
    struct Region {
        int x0, y0, x1, y1;  // [x0, x1) x [y0, y1)
        float energy;
    };
    auto sum = [&](int x0, int y0, int x1, int y1) {
        float e = 0.0f;
        for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) e += energy[static_cast<std::size_t>(y) * map.width + x];
        return e;
    };
    std::vector<Region> regions{{0, 0, map.width, map.height, sum(0, 0, map.width, map.height)}};
    while (static_cast<int>(regions.size()) < count) {
        // Split the region with the most energy (that is at least two texels across) along its
        // longer side where the energy halves. Widths count the texels' real extent (a row near
        // the pole is short).
        int best = -1;
        for (std::size_t i = 0; i < regions.size(); ++i) {
            const Region& r = regions[i];
            if (r.x1 - r.x0 < 2 && r.y1 - r.y0 < 2) continue;
            if (best < 0 || r.energy > regions[best].energy) best = static_cast<int>(i);
        }
        if (best < 0) break;
        Region r = regions[best];
        const float midTheta = ((r.y0 + r.y1) * 0.5f) / map.height * kPi;
        const float extentX = (r.x1 - r.x0) * std::sin(midTheta), extentY = static_cast<float>(r.y1 - r.y0);
        const bool alongX = (extentX >= extentY && r.x1 - r.x0 >= 2) || r.y1 - r.y0 < 2;
        float running = 0.0f;
        int cut = alongX ? r.x0 + 1 : r.y0 + 1;
        if (alongX) {
            for (int x = r.x0; x < r.x1 - 1; ++x) {
                running += sum(x, r.y0, x + 1, r.y1);
                if (running >= r.energy * 0.5f) { cut = x + 1; break; }
                cut = x + 2;
            }
            cut = std::clamp(cut, r.x0 + 1, r.x1 - 1);
            regions[best] = {r.x0, r.y0, cut, r.y1, sum(r.x0, r.y0, cut, r.y1)};
            regions.push_back({cut, r.y0, r.x1, r.y1, sum(cut, r.y0, r.x1, r.y1)});
        } else {
            for (int y = r.y0; y < r.y1 - 1; ++y) {
                running += sum(r.x0, y, r.x1, y + 1);
                if (running >= r.energy * 0.5f) { cut = y + 1; break; }
                cut = y + 2;
            }
            cut = std::clamp(cut, r.y0 + 1, r.y1 - 1);
            regions[best] = {r.x0, r.y0, r.x1, cut, sum(r.x0, r.y0, r.x1, cut)};
            regions.push_back({r.x0, cut, r.x1, r.y1, sum(r.x0, cut, r.x1, r.y1)});
        }
    }
    for (const Region& r : regions) {
        DirectionalLight light;
        light.name = "environment";
        std::array<double, 3> centroid{0.0, 0.0, 0.0}, color{0.0, 0.0, 0.0};
        for (int y = r.y0; y < r.y1; ++y) {
            const float theta = (y + 0.5f) / map.height * kPi;
            const float solidAngle = (2.0f * kPi / map.width) * (kPi / map.height) * std::sin(theta);
            for (int x = r.x0; x < r.x1; ++x) {
                const float e = energy[static_cast<std::size_t>(y) * map.width + x];
                const auto d = EnvironmentMap::direction((x + 0.5f) / map.width, (y + 0.5f) / map.height);
                const RGB c = map.at(x, y);
                for (int k = 0; k < 3; ++k) {
                    centroid[k] += static_cast<double>(d[k] * e);
                    color[k] += static_cast<double>(c[k] * solidAngle);
                }
            }
        }
        const double n = std::sqrt(centroid[0] * centroid[0] + centroid[1] * centroid[1] + centroid[2] * centroid[2]);
        if (n <= 0.0) continue;  // a region without light: no light stands for it
        for (int k = 0; k < 3; ++k) light.direction[k] = static_cast<float>(centroid[k] / n);
        const float luminance = static_cast<float>(0.2126 * color[0] + 0.7152 * color[1] + 0.0722 * color[2]);
        if (luminance <= 0.0f) continue;
        for (int k = 0; k < 3; ++k) light.color[k] = static_cast<float>(color[k]) / luminance;
        light.intensity = luminance;
        lights.push_back(light);
    }
    return lights;
}

RGB environmentRadiance(const EnvironmentMap& map, const std::array<float, 3>& d) {
    if (map.width == 0 || map.height == 0) return {0.0f, 0.0f, 0.0f};
    // The inverse of EnvironmentMap::direction: u around y from -z through +x, v from the zenith.
    const float u = std::atan2(d[0], -d[2]) / (2.0f * kPi) + 0.5f;
    const float v = std::acos(std::clamp(d[1], -1.0f, 1.0f)) / kPi;
    const float fx = u * map.width - 0.5f, fy = std::clamp(v * map.height - 0.5f, 0.0f, map.height - 1.0f);
    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - x0, ty = fy - y0;
    auto at = [&](int x, int y) {
        x = ((x % map.width) + map.width) % map.width;
        y = std::clamp(y, 0, map.height - 1);
        return map.at(x, y);
    };
    const RGB a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), e = at(x0 + 1, y0 + 1);
    RGB out{};
    for (int k = 0; k < 3; ++k) out[k] = (a[k] * (1.0f - tx) + b[k] * tx) * (1.0f - ty) + (c[k] * (1.0f - tx) + e[k] * tx) * ty;
    return out;
}

namespace {

// Hammersley's point set: the i-th of n, for the importance sampling below.
std::array<float, 2> hammersley(std::uint32_t i, std::uint32_t n) {
    std::uint32_t bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return {static_cast<float>(i) / static_cast<float>(n), static_cast<float>(bits) * 2.3283064365386963e-10f};
}

// A half vector of the GGX distribution of `alpha` (roughness squared) around the z axis
// (Karis 2013's ImportanceSampleGGX).
std::array<float, 3> sampleGGX(const std::array<float, 2>& xi, float alpha) {
    const float phi = 2.0f * kPi * xi[0];
    const float cosTheta = std::sqrt((1.0f - xi[1]) / (1.0f + (alpha * alpha - 1.0f) * xi[1]));
    const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
    return {sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta};
}

// An orthonormal frame around a unit normal.
void frameOf(const std::array<float, 3>& n, std::array<float, 3>& t, std::array<float, 3>& b) {
    const std::array<float, 3> up = std::abs(n[1]) < 0.999f ? std::array<float, 3>{0.0f, 1.0f, 0.0f} : std::array<float, 3>{1.0f, 0.0f, 0.0f};
    t = normalized({up[1] * n[2] - up[2] * n[1], up[2] * n[0] - up[0] * n[2], up[0] * n[1] - up[1] * n[0]});
    b = {n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0]};
}

}  // namespace

EnvironmentMap prefilterEnvironment(const EnvironmentMap& map, float roughness, int width, int height, int samples) {
    EnvironmentMap out;
    out.width = width;
    out.height = height;
    out.rgb.assign(static_cast<std::size_t>(width) * height * 3, 0.0f);
    if (map.width == 0 || map.height == 0) return out;
    const float alpha = std::max(roughness * roughness, 1e-3f);
    const bool mirror = roughness <= 1e-3f;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::array<float, 3> n = EnvironmentMap::direction((x + 0.5f) / width, (y + 0.5f) / height);
            RGB sum{};
            float weight = 0.0f;
            if (mirror) {
                sum = environmentRadiance(map, n);
                weight = 1.0f;
            } else {
                // The view along the normal (Karis's assumption): each GGX half vector gives a
                // reflected direction, weighted by its cosine.
                std::array<float, 3> t, b;
                frameOf(n, t, b);
                for (int i = 0; i < samples; ++i) {
                    const std::array<float, 3> h = sampleGGX(hammersley(static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(samples)), alpha);
                    std::array<float, 3> hw;
                    for (int k = 0; k < 3; ++k) hw[k] = t[k] * h[0] + b[k] * h[1] + n[k] * h[2];
                    const float vDotH = h[2];  // v = n
                    std::array<float, 3> l;
                    for (int k = 0; k < 3; ++k) l[k] = 2.0f * vDotH * hw[k] - n[k];
                    const float nDotL = l[0] * n[0] + l[1] * n[1] + l[2] * n[2];
                    if (nDotL <= 0.0f) continue;
                    const RGB c = environmentRadiance(map, l);
                    for (int k = 0; k < 3; ++k) sum[k] += c[k] * nDotL;
                    weight += nDotL;
                }
            }
            float* o = out.rgb.data() + (static_cast<std::size_t>(y) * width + x) * 3;
            for (int k = 0; k < 3; ++k) o[k] = weight > 0.0f ? sum[k] / weight : 0.0f;
        }
    }
    return out;
}

// ---- Surfaces -----------------------------------------------------------------------------------

std::vector<std::string> surfacePresetNames() {
    return {"clay", "scalp", "plastic", "metal", "skin", "skin light", "skin dark", "eye brown", "eye blue", "eye green"};
}

SurfaceMaterial surfacePreset(const std::string& name) {
    SurfaceMaterial m;
    m.presetName = name;
    if (name == "clay") {
        m.baseColor = {0.6f, 0.55f, 0.5f};
        m.roughness = 0.7f;
    } else if (name == "scalp") {
        // Skin under hair: darker than a bare scalp reads, a little rough and waxy.
        m.baseColor = {0.36f, 0.29f, 0.25f};
        m.roughness = 0.55f;
    } else if (name == "plastic") {
        m.baseColor = {0.8f, 0.1f, 0.08f};
        m.roughness = 0.3f;
    } else if (name == "metal") {
        m.baseColor = {0.95f, 0.93f, 0.88f};
        m.roughness = 0.25f;
        m.metallic = 1.0f;
    } else if (name == "skin" || name == "skin light" || name == "skin dark") {
        m.kind = SurfaceKind::Skin;
        // F0 0.028 (d'Eon 2007's measured skin): Burley's scale 0.35 of the 8 percent maximum.
        m.specular = 0.35f;
        m.roughness = 0.4f;
        m.roughness2 = 0.65f;
        m.lobe2Weight = 0.25f;
        m.baseColor = name == "skin light" ? RGB{0.9f, 0.72f, 0.62f} : name == "skin dark" ? RGB{0.32f, 0.2f, 0.14f} : RGB{0.75f, 0.52f, 0.42f};
        m.detailScale = 40.0f;
        m.detailStrength = 0.3f;
    } else if (name == "eye brown" || name == "eye blue" || name == "eye green") {
        m.kind = SurfaceKind::Eye;
        m.roughness = 0.05f;
        m.specular = 0.3f;  // the tear film: water's 2 percent
        m.irisColor = name == "eye blue" ? RGB{0.22f, 0.38f, 0.55f} : name == "eye green" ? RGB{0.25f, 0.4f, 0.22f} : RGB{0.2f, 0.1f, 0.05f};
    } else {
        m = surfacePreset("clay");
        m.presetName.clear();
    }
    return m;
}

bool setSurfaceParam(SurfaceMaterial& m, const std::string& name, const std::string& value) {
    struct FloatField {
        const char* name;
        float SurfaceMaterial::*member;
    };
    static const FloatField floats[] = {
        {"roughness", &SurfaceMaterial::roughness}, {"metallic", &SurfaceMaterial::metallic}, {"specular", &SurfaceMaterial::specular},
        {"occlusion", &SurfaceMaterial::occlusion}, {"normalStrength", &SurfaceMaterial::normalStrength},
        {"specularMapGain", &SurfaceMaterial::specularMapGain}, {"alphaCutoff", &SurfaceMaterial::alphaCutoff},
        {"detailScale", &SurfaceMaterial::detailScale},
        {"detailStrength", &SurfaceMaterial::detailStrength}, {"sssRadius", &SurfaceMaterial::sssRadius},
        {"sssStrength", &SurfaceMaterial::sssStrength}, {"roughness2", &SurfaceMaterial::roughness2},
        {"lobe2Weight", &SurfaceMaterial::lobe2Weight}, {"cavity", &SurfaceMaterial::cavity},
        {"transmission", &SurfaceMaterial::transmission}, {"sssTexturing", &SurfaceMaterial::sssTexturing},
        {"irisRadius", &SurfaceMaterial::irisRadius},
        {"pupilRadius", &SurfaceMaterial::pupilRadius}, {"limbalWidth", &SurfaceMaterial::limbalWidth},
        {"corneaIor", &SurfaceMaterial::corneaIor}, {"irisDetail", &SurfaceMaterial::irisDetail}, {"veins", &SurfaceMaterial::veins},
        {"eyeOcclusion", &SurfaceMaterial::eyeOcclusion}, {"tearLine", &SurfaceMaterial::tearLine}, {"wetness", &SurfaceMaterial::wetness}};
    struct ColorField {
        const char* name;
        RGB SurfaceMaterial::*member;
    };
    static const ColorField colors[] = {{"baseColor", &SurfaceMaterial::baseColor}, {"irisColor", &SurfaceMaterial::irisColor},
                                        {"scleraColor", &SurfaceMaterial::scleraColor}, {"sssMeanFreePath", &SurfaceMaterial::sssMeanFreePath}};
    struct MapField {
        const char* name;
        std::string SurfaceMaterial::*member;
    };
    static const MapField maps[] = {{"albedoMap", &SurfaceMaterial::albedoMap}, {"normalMap", &SurfaceMaterial::normalMap},
                                    {"roughnessMap", &SurfaceMaterial::roughnessMap}, {"metallicMap", &SurfaceMaterial::metallicMap},
                                    {"occlusionMap", &SurfaceMaterial::occlusionMap}, {"specularMap", &SurfaceMaterial::specularMap}};
    auto parseFloat = [&](float& out) {
        char* end = nullptr;
        const float v = std::strtof(value.c_str(), &end);
        if (end == value.c_str() || *end != '\0') return false;
        out = v;
        return true;
    };
    if (name == "kind") {
        if (value == "default") m.kind = SurfaceKind::Default;
        else if (value == "skin") m.kind = SurfaceKind::Skin;
        else if (value == "eye") m.kind = SurfaceKind::Eye;
        else return false;
        m.presetName.clear();
        return true;
    }
    if (name == "sssMode") {
        if (value == "preintegrated") m.sssMode = SurfaceMaterial::SssMode::Preintegrated;
        else if (value == "screen") m.sssMode = SurfaceMaterial::SssMode::ScreenSpace;
        else return false;
        m.presetName.clear();
        return true;
    }
    for (const FloatField& f : floats) {
        if (name != f.name) continue;
        if (!parseFloat(m.*f.member)) return false;
        m.presetName.clear();
        return true;
    }
    for (const ColorField& c : colors) {
        const std::string base = c.name;
        if (name.rfind(base + ".", 0) != 0 || name.size() != base.size() + 2) continue;
        const char channel = name.back();
        const int k = channel == 'r' ? 0 : channel == 'g' ? 1 : channel == 'b' ? 2 : -1;
        if (k < 0 || !parseFloat((m.*c.member)[k])) return false;
        m.presetName.clear();
        return true;
    }
    for (const MapField& f : maps) {
        if (name != f.name) continue;
        m.*f.member = value;
        m.presetName.clear();
        return true;
    }
    return false;
}

// ---- Look-up tables -----------------------------------------------------------------------------

std::vector<float> environmentBrdfLUT(int size, int samples) {
    std::vector<float> lut(static_cast<std::size_t>(size) * size * 2, 0.0f);
    for (int row = 0; row < size; ++row) {
        const float roughness = std::max((row + 0.5f) / size, 0.02f);
        const float alpha = roughness * roughness;
        // Smith's shadowing in Schlick's form with k = alpha / 2, the image-based lighting
        // remapping of Karis 2013 (the lobe's energy must match what the prefiltered map assumes).
        const float k = alpha / 2.0f;
        for (int col = 0; col < size; ++col) {
            const float nDotV = std::max((col + 0.5f) / size, 1e-3f);
            const std::array<float, 3> v{std::sqrt(1.0f - nDotV * nDotV), 0.0f, nDotV};
            float a = 0.0f, b = 0.0f;
            for (int i = 0; i < samples; ++i) {
                const std::array<float, 3> h = sampleGGX(hammersley(static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(samples)), alpha);
                const float vDotH = v[0] * h[0] + v[1] * h[1] + v[2] * h[2];
                const float nDotL = 2.0f * vDotH * h[2] - v[2];
                if (nDotL <= 0.0f) continue;
                const float nDotH = h[2];
                const float gv = nDotV / (nDotV * (1.0f - k) + k), gl = nDotL / (nDotL * (1.0f - k) + k);
                const float gVis = gv * gl * std::max(vDotH, 0.0f) / (nDotH * nDotV);
                const float fc = std::pow(1.0f - std::max(vDotH, 0.0f), 5.0f);
                a += (1.0f - fc) * gVis;
                b += fc * gVis;
            }
            float* o = lut.data() + (static_cast<std::size_t>(row) * size + col) * 2;
            o[0] = a / samples;
            o[1] = b / samples;
        }
    }
    return lut;
}

std::array<float, 2> environmentBrdfAt(const std::vector<float>& lut, int size, float roughness, float nDotV) {
    const float fy = std::clamp(roughness * size - 0.5f, 0.0f, size - 1.0f), fx = std::clamp(nDotV * size - 0.5f, 0.0f, size - 1.0f);
    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
    const int x1 = std::min(x0 + 1, size - 1), y1 = std::min(y0 + 1, size - 1);
    const float tx = fx - x0, ty = fy - y0;
    std::array<float, 2> out{};
    for (int k = 0; k < 2; ++k) {
        auto at = [&](int x, int y) { return lut[(static_cast<std::size_t>(y) * size + x) * 2 + k]; };
        out[k] = (at(x0, y0) * (1.0f - tx) + at(x1, y0) * tx) * (1.0f - ty) + (at(x0, y1) * (1.0f - tx) + at(x1, y1) * tx) * ty;
    }
    return out;
}

RGB burleyProfile(float r, const RGB& mfp) {
    // Christensen and Burley 2015, their eq. 2 with the albedo A folded out (it multiplies
    // the result elsewhere): R(r) = (e^(-r/d) + e^(-r/(3d))) / (8 pi d r), per channel.
    RGB out{};
    const float rr = std::max(r, 1e-6f);
    for (int k = 0; k < 3; ++k) {
        const float d = std::max(mfp[k], 1e-6f);
        out[k] = (std::exp(-rr / d) + std::exp(-rr / (3.0f * d))) / (8.0f * kPi * d * rr);
    }
    return out;
}

RGB burleyTransmittance(float t, const RGB& mfp) {
    RGB out{};
    const float tt = std::max(t, 0.0f);
    for (int k = 0; k < 3; ++k) out[k] = std::exp(-tt / std::max(mfp[k], 1e-6f));
    return out;
}

RGB skinDiffusionProfile(float r) { return burleyProfile(r, kSkinMeanFreePathMm); }

RGB skinTransmittance(float s) { return burleyTransmittance(s, kSkinMeanFreePathMm); }

ProfileStripTable profileStripTable(const RGB& mfp, float xMax, int size) {
    // The mass in the strip |x| <= X: the profile integrated in polar coordinates (2 pi r R(r)
    // is finite at 0) times the fraction of each circle of radius r inside the strip (all of
    // it when r <= X, else 2 asin(X / r) / pi). Each ring's mass goes to every column at once.
    ProfileStripTable table;
    table.xMax = xMax;
    table.mass.assign(static_cast<std::size_t>(std::max(size, 2)), RGB{});
    const int columns = static_cast<int>(table.mass.size());
    float dmax = 0.0f;
    for (float d : mfp) dmax = std::max(dmax, d);
    const float rMax = xMax + 30.0f * dmax;  // rings beyond the strip cross it too
    const int steps = 4000;
    for (int i = 0; i < steps; ++i) {
        // Rings denser near 0, where the profile is steep: r = rMax * u^2.
        const float u0 = static_cast<float>(i) / steps, u1 = static_cast<float>(i + 1) / steps;
        const float r0 = rMax * u0 * u0, r1 = rMax * u1 * u1, r = 0.5f * (r0 + r1);
        const RGB p = burleyProfile(r, mfp);
        RGB ring{};
        for (int k = 0; k < 3; ++k) ring[k] = 2.0f * kPi * r * p[k] * (r1 - r0);
        for (int c = 1; c < columns; ++c) {
            const float x = xMax * static_cast<float>(c) / (columns - 1);
            const float fraction = r <= x ? 1.0f : 2.0f * std::asin(x / r) / kPi;
            for (int k = 0; k < 3; ++k) table.mass[c][k] += ring[k] * fraction;
        }
    }
    return table;
}

RGB ProfileStripTable::at(float halfWidth) const {
    if (mass.empty()) return RGB{};
    const float f = std::clamp(halfWidth / std::max(xMax, 1e-6f), 0.0f, 1.0f) * (mass.size() - 1);
    const std::size_t i0 = static_cast<std::size_t>(f), i1 = std::min(i0 + 1, mass.size() - 1);
    const float t = f - i0;
    RGB out{};
    for (int k = 0; k < 3; ++k) out[k] = mass[i0][k] * (1.0f - t) + mass[i1][k] * t;
    return out;
}

namespace {

// The mass of the strip a <= x <= b of the plane (a < b), from the half-strip masses (the
// mass of 0..x is half that of |x'| <= x, and odd in x).
RGB stripMass(const ProfileStripTable& table, float a, float b) {
    auto half = [&](float x) {
        const RGB m = table.at(std::abs(x));
        const float sign = x < 0.0f ? -0.5f : 0.5f;
        return RGB{m[0] * sign, m[1] * sign, m[2] * sign};
    };
    const RGB ha = half(a), hb = half(b);
    return {hb[0] - ha[0], hb[1] - ha[1], hb[2] - ha[2]};
}

}  // namespace

std::vector<SssKernelSample> separableSssKernel(int samples, const RGB& mfp, float radius) {
    std::vector<SssKernelSample> kernel;
    const int n = std::max(samples | 1, 3);  // odd: a tap at the center
    const ProfileStripTable table = profileStripTable(mfp, radius);
    std::vector<float> offsets(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float t = -1.0f + 2.0f * static_cast<float>(i) / (n - 1);
        offsets[i] = radius * t * std::abs(t);
    }
    RGB total{};
    for (int i = 0; i < n; ++i) {
        // The cell: halfway to each neighbor (the ends reach the radius).
        const float a = i == 0 ? -radius : 0.5f * (offsets[i - 1] + offsets[i]);
        const float b = i == n - 1 ? radius : 0.5f * (offsets[i] + offsets[i + 1]);
        SssKernelSample sample{offsets[i], stripMass(table, a, b)};
        for (int k = 0; k < 3; ++k) total[k] += sample.weight[k];
        kernel.push_back(sample);
    }
    for (SssKernelSample& sample : kernel) {
        for (int k = 0; k < 3; ++k) sample.weight[k] = total[k] > 0.0f ? sample.weight[k] / total[k] : 0.0f;
    }
    return kernel;
}

std::vector<float> preintegratedSkinLUT(int width, int height, int steps) {
    std::vector<float> lut(static_cast<std::size_t>(width) * height * 3, 0.0f);
    // The profile's mass by distance along the ring (the default skin, mm): a fine table
    // for the first centimeters, a coarse one out to the flat row's huge sphere.
    const ProfileStripTable fine = profileStripTable(kSkinMeanFreePathMm, 60.0f, 2048);
    const ProfileStripTable coarse = profileStripTable(kSkinMeanFreePathMm, 2.0f * 1e4f, 2048);
    auto massTo = [&](float dist) { return dist <= 60.0f ? fine.at(dist) : coarse.at(dist); };
    for (int row = 0; row < height; ++row) {
        // The sphere's radius: flat on row 0 (curvature 0), 1 mm on the last.
        const float curvature = static_cast<float>(row) / std::max(height - 1, 1);
        const float r = curvature > 1e-4f ? 1.0f / curvature : 1e4f;
        for (int col = 0; col < width; ++col) {
            const float theta = std::acos(std::clamp(-1.0f + 2.0f * (col + 0.5f) / width, -1.0f, 1.0f));
            // Around the sphere's great circle through the point: the light each neighbor
            // gets (its cosine, clamped) weighed by the profile's mass over its cell of the
            // ring (the strip between the cell's ends' chord distances), over the total, so
            // flat skin gives Lambert's cosine.
            RGB sum{}, norm{};
            for (int s = 0; s < steps; ++s) {
                const float x0 = -kPi + 2.0f * kPi * s / steps, x1 = x0 + 2.0f * kPi / steps, x = 0.5f * (x0 + x1);
                const float d0 = 2.0f * r * std::abs(std::sin(0.5f * x0)), d1 = 2.0f * r * std::abs(std::sin(0.5f * x1));
                const RGB m0 = massTo(d0), m1 = massTo(d1);
                const bool across = x0 < 0.0f && x1 > 0.0f;  // the center cell holds both sides
                const float cosine = std::max(0.0f, std::cos(theta + x));
                for (int k = 0; k < 3; ++k) {
                    const float mass = across ? 0.5f * (m0[k] + m1[k]) : 0.5f * std::abs(m1[k] - m0[k]);
                    sum[k] += cosine * mass;
                    norm[k] += mass;
                }
            }
            float* o = lut.data() + (static_cast<std::size_t>(row) * width + col) * 3;
            for (int k = 0; k < 3; ++k) o[k] = norm[k] > 0.0f ? sum[k] / norm[k] : std::max(0.0f, std::cos(theta));
        }
    }
    return lut;
}

RGB preintegratedSkinAt(const std::vector<float>& lut, int width, int height, float nDotL, float curvature) {
    const float fx = std::clamp((nDotL * 0.5f + 0.5f) * width - 0.5f, 0.0f, width - 1.0f);
    const float fy = std::clamp(curvature * (height - 1), 0.0f, height - 1.0f);
    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
    const int x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
    const float tx = fx - x0, ty = fy - y0;
    RGB out{};
    for (int k = 0; k < 3; ++k) {
        auto at = [&](int x, int y) { return lut[(static_cast<std::size_t>(y) * width + x) * 3 + k]; };
        out[k] = (at(x0, y0) * (1.0f - tx) + at(x1, y0) * tx) * (1.0f - ty) + (at(x0, y1) * (1.0f - tx) + at(x1, y1) * tx) * ty;
    }
    return out;
}

std::vector<float> microNormalTexture(int size, std::uint32_t seed) {
    // Tileable value noise: a lattice of random heights at a few scales, bilinearly interpolated
    // with the lattice wrapping, summed with falling amplitudes; pores as small dark pits on top.
    auto hashAt = [&](int x, int y, int level) {
        std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u + level * 2246822519u + seed * 3266489917u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h ^= h >> 16;
        return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
    };
    std::vector<float> height(static_cast<std::size_t>(size) * size, 0.0f);
    float amplitude = 1.0f;
    for (int level = 0; level < 4; ++level) {
        const int cells = 8 << level;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const float fx = static_cast<float>(x) * cells / size, fy = static_cast<float>(y) * cells / size;
                const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
                float tx = fx - x0, ty = fy - y0;
                tx = tx * tx * (3.0f - 2.0f * tx);
                ty = ty * ty * (3.0f - 2.0f * ty);
                const float a = hashAt(x0 % cells, y0 % cells, level), b = hashAt((x0 + 1) % cells, y0 % cells, level);
                const float c = hashAt(x0 % cells, (y0 + 1) % cells, level), d = hashAt((x0 + 1) % cells, (y0 + 1) % cells, level);
                height[static_cast<std::size_t>(y) * size + x] += amplitude * ((a * (1.0f - tx) + b * tx) * (1.0f - ty) + (c * (1.0f - tx) + d * tx) * ty);
            }
        }
        amplitude *= 0.5f;
    }
    // Pores: the finest level's peaks pushed down into pits.
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float p = hashAt(x, y, 9);
            float& h = height[static_cast<std::size_t>(y) * size + x];
            if (p > 0.985f) h -= 0.6f;
        }
    }
    std::vector<float> rgb(static_cast<std::size_t>(size) * size * 3);
    const float slope = 1.5f;  // the height field's steepness, so the strength parameter has a sensible range
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            auto at = [&](int xx, int yy) { return height[static_cast<std::size_t>(((yy % size) + size) % size) * size + ((xx % size) + size) % size]; };
            const float dx = (at(x + 1, y) - at(x - 1, y)) * slope, dy = (at(x, y + 1) - at(x, y - 1)) * slope;
            const std::array<float, 3> n = normalized({-dx, -dy, 1.0f});
            float* o = rgb.data() + (static_cast<std::size_t>(y) * size + x) * 3;
            for (int k = 0; k < 3; ++k) o[k] = n[k] * 0.5f + 0.5f;
        }
    }
    return rgb;
}

}  // namespace groomlab::app
