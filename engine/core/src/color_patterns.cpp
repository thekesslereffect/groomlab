// SPDX-License-Identifier: Apache-2.0
// The color nodes' patterns (color_patterns.h): bands along a strand, vane space and the
// patterns drawn in it, the iridescent tint.

#include "groomlab/color_patterns.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/portable_math.h"

#include <algorithm>
#include <cmath>

#include "groomlab/random.h"
#include "groomlab/strand_set.h"

namespace groomlab {

namespace {

constexpr const char* kSideChannel = "side";
constexpr const char* kFeatherIdChannel = "featherId";
constexpr const char* kIsShaftChannel = "isShaft";

const std::vector<float>* sized(const StrandSet& s, const char* name) {
    const std::vector<float>* c = s.strandChannel(name);
    return c && c->size() == s.strandCount() ? c : nullptr;
}

// A shape's weight at d (0 its center .. 1 its edge): the profile across it, cut at its edge as
// sharply as asked.
float shape(const VanePatternParams& p, float d) {
    if (!(d < 1.0f) && p.sharpness >= 1.0f) return 0.0f;
    const float soft = std::clamp(1.0f - p.sharpness, 0.0f, 1.0f);
    const float edge = soft < 1e-4f ? (d < 1.0f ? 1.0f : 0.0f) : 1.0f - smoothstep(1.0f - soft, 1.0f, d);
    if (edge <= 0.0f) return 0.0f;
    return std::clamp(p.profile(std::clamp(d, 0.0f, 1.0f)), 0.0f, 1.0f) * edge;
}

// Linear RGB <-> HSV (hue in turns).
std::array<float, 3> toHsv(const std::array<float, 3>& c) {
    const float hi = std::max({c[0], c[1], c[2]}), lo = std::min({c[0], c[1], c[2]});
    const float delta = hi - lo;
    float h = 0.0f;
    if (delta > 1e-8f) {
        if (hi == c[0]) h = (c[1] - c[2]) / delta;
        else if (hi == c[1]) h = 2.0f + (c[2] - c[0]) / delta;
        else h = 4.0f + (c[0] - c[1]) / delta;
        h /= 6.0f;
        if (h < 0.0f) h += 1.0f;
    }
    return {h, hi > 1e-8f ? delta / hi : 0.0f, hi};
}

std::array<float, 3> fromHsv(const std::array<float, 3>& hsv) {
    const float h = fract(hsv[0]) * 6.0f, s = hsv[1], v = hsv[2];
    // The smooth form (each channel from its distance round the hue circle), as the shaders have it.
    std::array<float, 3> c{};
    const float offsets[3] = {0.0f, 4.0f, 2.0f};
    for (int k = 0; k < 3; ++k) {
        const float x = std::fmod(h + offsets[k], 6.0f);
        const float ramp = std::clamp(std::abs(x - 3.0f) - 1.0f, 0.0f, 1.0f);
        c[k] = v * (1.0f - s + s * ramp);
    }
    return c;
}

}  // namespace

float bandAt(const Ramp& profile, float t, int bands, float scale, float phase) {
    const float n = static_cast<float>(std::max(bands, 1));
    const float x = t * n * scale + phase;
    if (x <= 0.0f) return profile(0.0f);
    if (x >= n) return profile(1.0f);
    return profile(fract(x));
}

VaneSpace::VaneSpace(const StrandSet& strands) : strands_(strands) {
    vaneU_ = sized(strands, kVaneUChannel);
    tipU_ = sized(strands, kVaneTipUChannel);
    tipV_ = sized(strands, kVaneTipVChannel);
    side_ = sized(strands, kSideChannel);
    feather_ = sized(strands, kFeatherIdChannel);
    shaft_ = sized(strands, kIsShaftChannel);
}

bool VaneSpace::inFeather(std::size_t s) const {
    return available() && (!feather_ || (*feather_)[s] >= 0.0f);
}

VanePoint VaneSpace::at(std::size_t s, std::size_t i, std::size_t n) const {
    VanePoint p;
    const float t = n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f;
    const float u0 = (*vaneU_)[s];
    p.u = u0 + ((*tipU_)[s] - u0) * t;
    p.v = (*tipV_)[s] * t;
    p.side = side_ ? static_cast<int>(std::lround((*side_)[s])) : 0;
    p.barb = !(shaft_ && (*shaft_)[s] > 0.5f);
    p.along = p.barb ? t : 0.0f;
    return p;
}

std::uint64_t VaneSpace::featherKey(std::size_t s) const {
    const StrandRoot& root = strands_.roots()[s];
    if (root.face >= 0) {
        return hashFloat(hashFloat(hashCombine(0xFEA7, static_cast<std::uint64_t>(root.face)), root.u), root.v);
    }
    return hashCombine(0xFEA8, static_cast<std::uint64_t>(feather_ ? static_cast<std::int64_t>((*feather_)[s]) : 0));
}

VaneNormals::VaneNormals(const StrandSet& strands) {
    for (int k = 0; k < 3; ++k) c_[k] = sized(strands, kVaneNormalChannels[k]);
    if (!c_[0] || !c_[1] || !c_[2]) c_[0] = c_[1] = c_[2] = nullptr;
}

Vec3f iridescenceRootNormal(const StrandSet& strands, std::size_t i, const ScalpMesh* scalp, const VaneNormals& vanes) {
    Vec3f up{0.0f, 1.0f, 0.0f};
    if (vanes.at(i, &up)) return up;
    if (scalp && i < strands.roots().size() && scalp->contains(strands.roots()[i])) return scalp->normal(strands.roots()[i]);
    if (strands.strandPointCount(i) >= 2) return (strands.strandPoints(i)[1] - strands.strandPoints(i)[0]).normalized();
    return up;
}

bool VaneNormals::at(std::size_t s, Vec3f* normal) const {
    if (!c_[0]) return false;
    const Vec3f n{(*c_[0])[s], (*c_[1])[s], (*c_[2])[s]};
    if (!(n.length() > 0.5f)) return false;
    *normal = n;
    return true;
}

const std::vector<std::string>& vanePatternNames() {
    static const std::vector<std::string> names{"none", "texture", "bars", "chevrons", "spots", "fringe", "eye"};
    return names;
}

bool parseVanePattern(const std::string& name, VanePattern* out) {
    static const VanePattern kinds[] = {VanePattern::None,     VanePattern::Texture, VanePattern::Bars, VanePattern::Chevrons,
                                        VanePattern::Spots,    VanePattern::Fringe,  VanePattern::Eye};
    const std::vector<std::string>& names = vanePatternNames();
    for (std::size_t k = 0; k < names.size(); ++k) {
        if (names[k] == name) {
            *out = kinds[k];
            return true;
        }
    }
    return false;
}

bool parseVaneSides(const std::string& name, VaneSides* out) {
    if (name == "both") *out = VaneSides::Both;
    else if (name == "left") *out = VaneSides::Left;
    else if (name == "right") *out = VaneSides::Right;
    else return false;
    return true;
}

bool onVane(VaneSides sides, int side) {
    return sides == VaneSides::Both || (sides == VaneSides::Left && side < 0) || (sides == VaneSides::Right && side > 0);
}

FeatherJitter featherJitter(const VanePatternParams& params, std::uint64_t featherKey) {
    FeatherJitter j;
    Rng rng(hashCombine(featherKey, params.seed ^ 0x7A77E5ull));
    const float a = rng.uniform(-1.0f, 1.0f), b = rng.uniform(-1.0f, 1.0f), c = rng.uniform(-1.0f, 1.0f);
    j.scale = std::max(0.05f, 1.0f + params.scaleJitter * b);
    j.rotation = params.rotationJitter * c;
    const float spacing = params.kind == VanePattern::Eye ? 0.5f * params.width * j.scale
                                                          : 1.0f / (std::max(params.count, 1e-3f) * j.scale);
    j.offset = params.jitter * a * spacing;
    return j;
}

float vanePatternMask(const VanePatternParams& p, const VanePoint& point, const FeatherJitter& j, std::uint64_t featherKey) {
    const float x = static_cast<float>(point.side == 0 ? 1 : point.side) * point.v / kVaneAspect;  // feather lengths
    const float y = point.u - (p.offset + j.offset);
    const float angle = p.angle + j.rotation;
    const float frequency = std::max(p.count, 1e-3f) * j.scale;
    const float width = std::clamp(p.width, 1e-4f, 1.0f);
    switch (p.kind) {
        case VanePattern::Bars:
        case VanePattern::Chevrons: {
            // Distance across the bar through this point, in periods; bars sit at whole periods.
            const float across = p.kind == VanePattern::Bars ? x : std::abs(x);
            const float phase = (y * pm::cos(angle) + across * pm::sin(angle)) * frequency;
            const float d = std::abs(fract(phase + 0.5f) - 0.5f) / (0.5f * width);
            return shape(p, d);
        }
        case VanePattern::Spots: {
            // A spot in every cell of a grid (cells a period square), each moved within its cell.
            const float gx = x * frequency, gy = y * frequency;
            const float cx = std::floor(gx), cy = std::floor(gy);
            float nearest = 1e9f;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const float ix = cx + static_cast<float>(dx), iy = cy + static_cast<float>(dy);
                    const std::uint64_t cell = hashCombine(hashCombine(hashCombine(featherKey, p.seed ^ 0x5B075ull),
                                                                       static_cast<std::uint64_t>(static_cast<std::int64_t>(ix))),
                                                           static_cast<std::uint64_t>(static_cast<std::int64_t>(iy)));
                    const float ox = 0.5f + 0.6f * (unitFloat(hashCombine(cell, 1)) - 0.5f);
                    const float oy = 0.5f + 0.6f * (unitFloat(hashCombine(cell, 2)) - 0.5f);
                    const float ddx = gx - (ix + ox), ddy = gy - (iy + oy);
                    nearest = std::min(nearest, std::sqrt(ddx * ddx + ddy * ddy));
                }
            }
            return shape(p, nearest / (0.5f * width));
        }
        case VanePattern::Fringe:
            if (!point.barb) return 0.0f;
            return shape(p, (1.0f - point.along) / width);
        case VanePattern::Eye: {
            // Round in feather lengths; the angle tilts an oval eye (its length along the shaft).
            const float ca = pm::cos(angle), sa = pm::sin(angle);
            const float ex = x * ca - y * sa, ey = x * sa + y * ca;
            const float radius = 0.5f * width * j.scale;
            return shape(p, std::sqrt(ex * ex + ey * ey) / radius);
        }
        default:
            return 0.0f;
    }
}

std::array<float, 2> vaneTextureCoordinates(const VanePoint& point, bool perSide) {
    if (perSide) return {std::clamp(point.v, 0.0f, 1.0f), std::clamp(point.u, 0.0f, 1.0f)};
    const float side = point.side < 0 ? -1.0f : 1.0f;
    return {std::clamp(0.5f + 0.5f * side * point.v, 0.0f, 1.0f), std::clamp(point.u, 0.0f, 1.0f)};
}

std::array<float, 3> iridescentTint(const std::array<float, 3>& color, float shift, float grazing) {
    std::array<float, 3> hsv = toHsv(color);
    hsv[0] += shift * std::clamp(grazing, 0.0f, 1.0f);
    return fromHsv(hsv);
}

}  // namespace groomlab
