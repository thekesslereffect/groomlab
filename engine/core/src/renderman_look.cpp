// SPDX-License-Identifier: Apache-2.0
#include "groomlab/portable_math.h"
#include "groomlab/renderman_look.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>

#include "groomlab/export_view.h"
#include "groomlab/groom_session.h"

namespace groomlab::renderman {

namespace {

constexpr float kEumelanin[3] = {0.419f, 0.697f, 1.37f};
constexpr float kPheomelanin[3] = {0.187f, 0.4f, 1.05f};
constexpr float kDegrees = 57.29577951308232f;

}  // namespace

std::array<float, 3> pxrHairColorFiber(float melanin, float redness) {
    const float m = std::clamp(melanin, 0.0f, 1.0f);
    const float r = std::clamp(redness, 0.0f, 1.0f);
    const float w = kPxrHairColorConcentration * m * m;
    std::array<float, 3> c{};
    for (int k = 0; k < 3; ++k) c[k] = (1.0f - r) * pm::exp(-w * kEumelanin[k]) + r * pm::exp(-w * kPheomelanin[k]);
    return c;
}

HairColorPigment pxrHairColorPigment(float melanin, float redness, float wetness) {
    const float wet = wetShading(wetness).absorption;  // the absorption's factor
    const float r = std::clamp(redness, 0.0f, 1.0f);
    HairColorPigment out;
    const float scale = kPxrMelaninScale + kPxrMelaninScaleRedness * r;
    out.melanin = std::clamp(scale * std::clamp(melanin, 0.0f, 1.0f) * std::sqrt(wet), 0.0f, 1.0f);
    // The redness: PxrHairColor mixes the two pigments' colors, GroomLab (and Arnold) their
    // absorptions. The mix of PxrHairColor's two colors nearest the mixed absorption's color.
    const float w = kPxrHairColorConcentration * out.melanin * out.melanin;
    float num = 0.0f, den = 0.0f;
    for (int k = 0; k < 3; ++k) {
        const float eu = pm::exp(-w * kEumelanin[k]), pheo = pm::exp(-w * kPheomelanin[k]);
        const float want = pm::exp(-w * ((1.0f - r) * kEumelanin[k] + r * kPheomelanin[k]));
        num += (want - eu) * (pheo - eu);
        den += (pheo - eu) * (pheo - eu);
    }
    out.redness = den > 1e-8f ? std::clamp(num / den, 0.0f, 1.0f) : r;
    return out;
}

MarschnerLobes pxrMarschnerLobes(const HairLobes& lobes) {
    MarschnerLobes out;
    out.specularGainR = kPxrSpecularGainR * lobes.specularTint;
    out.coneAngleR = hairLongitudinalWidth(lobes.roughness) * kDegrees;
    out.coneAngleTT = 0.5f * out.coneAngleR;
    out.coneAngleTRT = 2.0f * out.coneAngleR;
    out.offset = -lobes.shiftDegrees;
    out.ior = lobes.ior;
    return out;
}

std::string Primvar::declaration() const {
    const char* detailName = detail == Detail::Constant ? "constant" : detail == Detail::Uniform ? "uniform" : "vertex";
    const char* typeName = type == Type::Float ? "float" : type == Type::Int ? "int" : type == Type::Color ? "color" : "float[2]";
    return std::string(detailName) + " " + typeName + " " + name;
}

std::size_t Primvar::valueCount() const {
    switch (type) {
        case Type::Int: return ints.size();
        case Type::Color: return floats.size() / 3;
        case Type::Float2: return floats.size() / 2;
        default: return floats.size();
    }
}

std::vector<Primvar> curvePrimvars(const ExportView& view, const HairLook& look) {
    std::vector<Primvar> out;
    if (!view.strands) return out;
    const StrandSet& s = *view.strands;
    const std::size_t strands = view.strandCount(), points = view.pointCount();
    // Room for every primvar first: `add` hands out references that must stay valid.
    out.reserve(view.strandChannels.size() + 32);
    auto add = [&](std::string name, Detail detail, Type type) -> Primvar& {
        out.push_back(Primvar{std::move(name), detail, type, {}, {}});
        return out.back();
    };

    Primvar& id = add("strand_id", Detail::Uniform, Type::Int);
    Primvar& index = add("hair_index", Detail::Uniform, Type::Float);
    id.ints.resize(strands);
    index.floats.resize(strands);
    for (std::size_t i = 0; i < strands; ++i) {
        id.ints[i] = static_cast<std::int32_t>(static_cast<std::uint32_t>(s.ids()[i]));
        index.floats[i] = static_cast<float>((s.ids()[i] & 0xFFFFFFu) + 1u);
    }
    if (view.hasUVs()) {
        for (const char* name : {"st", "scalp_st"}) {
            Primvar& uv = add(name, Detail::Uniform, Type::Float2);
            uv.floats.reserve(2 * strands);
            for (const auto& r : view.rootUVs) uv.floats.insert(uv.floats.end(), {r[0], r[1]});
        }
    }

    // The names taken: a renderer's own, and this function's.
    const std::set<std::string> reserved = {"P", "width", "N", "Cs", "Os", "id", "st", "scalp_st", "strand_id",
                                            "hair_index", "hair_color", "opacity", "wetness", "iridescence",
                                            "rman_melanin", "rman_redness", "rman_dye", "rman_color", "rman_rgbShare",
                                            "rman_specularGainR", "rman_coneAngleR", "rman_coneAngleTT",
                                            "rman_coneAngleTRT", "rman_specularOffset"};
    for (const std::string& channel : view.strandChannels) {
        const std::vector<float>* values = view.strandChannel(channel);
        if (!values || reserved.count(channel) || channel.empty()) continue;
        add(channel, Detail::Uniform, Type::Float).floats = *values;
    }

    // The RGB: per point with the color nodes' per-point color, else per strand.
    const bool colorPerPoint = view.pointColors.size() == points && points > 0;
    const std::vector<std::array<float, 3>>& rgb = colorPerPoint ? view.pointColors : view.colors;
    const bool hasColor = !rgb.empty() && rgb.size() == (colorPerPoint ? points : strands);
    if (hasColor) {
        Primvar& c = add("hair_color", colorPerPoint ? Detail::Vertex : Detail::Uniform, Type::Color);
        c.floats.reserve(3 * rgb.size());
        for (const auto& v : rgb) c.floats.insert(c.floats.end(), {v[0], v[1], v[2]});
    }
    for (const char* name : {kOpacityChannel, kIridescenceChannel}) {
        if (const std::vector<float>* values = view.pointChannel(name)) add(name, Detail::Vertex, Type::Float).floats = *values;
    }
    bool wetPerPoint = false;
    const std::vector<float>* wetness = wetnessChannel(s, &wetPerPoint);
    if (wetness && wetPerPoint) add(kWetnessChannel, Detail::Vertex, Type::Float).floats = *wetness;
    // (A per-strand wetness went out with the strand channels.)

    // The look per strand, or per point where the wetness is (it darkens and sharpens the hair).
    const Detail lookDetail = wetness && wetPerPoint ? Detail::Vertex : Detail::Uniform;
    const std::size_t lookCount = lookDetail == Detail::Vertex ? points : strands;
    std::vector<std::size_t> strandOf(lookCount);
    for (std::size_t i = 0, p = 0; i < strands; ++i) {
        if (lookDetail == Detail::Uniform) {
            strandOf[i] = i;
            continue;
        }
        for (std::int32_t k = 0; k < view.counts[i]; ++k) strandOf[p++] = i;
    }
    auto wetAt = [&](std::size_t at) { return wetness ? (*wetness)[wetPerPoint ? at : strandOf[at]] : 0.0f; };

    const std::vector<float>* melanin = view.strandChannel(kMelaninChannel);
    const std::vector<float>* redness = view.strandChannel(kMelaninRednessChannel);
    Primvar& pm = add("rman_melanin", lookDetail, Type::Float);
    Primvar& pr = add("rman_redness", lookDetail, Type::Float);
    pm.floats.resize(lookCount);
    pr.floats.resize(lookCount);
    for (std::size_t at = 0; at < lookCount; ++at) {
        const std::size_t i = strandOf[at];
        const HairColorPigment pigment = pxrHairColorPigment(melanin ? (*melanin)[i] : kDefaultMelanin,
                                                             redness ? (*redness)[i] : kDefaultMelaninRedness, wetAt(at));
        pm.floats[at] = pigment.melanin;
        pr.floats[at] = pigment.redness;
    }
    add("rman_dye", Detail::Constant, Type::Color).floats = {look.dye[0], look.dye[1], look.dye[2]};
    if (hasColor) {
        // Where wet, the color as the wetness darkens it (hair_color.h wetFiberColor): per point
        // when the color or the wetness is.
        const bool perPoint = colorPerPoint || lookDetail == Detail::Vertex;
        Primvar& c = add("rman_color", perPoint ? Detail::Vertex : Detail::Uniform, Type::Color);
        const std::size_t n = perPoint ? points : strands;
        c.floats.reserve(3 * n);
        for (std::size_t i = 0, p = 0; i < strands; ++i) {
            const std::size_t count = perPoint ? static_cast<std::size_t>(view.counts[i]) : 1;
            for (std::size_t k = 0; k < count; ++k, ++p) {
                const std::array<float, 3>& v = colorPerPoint ? rgb[p] : rgb[i];
                const float w = !wetness ? 0.0f : wetPerPoint ? (*wetness)[p] : (*wetness)[i];
                const std::array<float, 3> shaded = w > 0.0f ? wetFiberColor(v, w) : v;
                c.floats.insert(c.floats.end(), {shaded[0], shaded[1], shaded[2]});
            }
        }
    }
    Primvar& share = add("rman_rgbShare", Detail::Uniform, Type::Float);
    const bool byRgb = look.colorsByRgb || look.colorBlends;
    share.floats.assign(strands, hasColor && byRgb ? 1.0f : 0.0f);

    const std::vector<float>* specular = view.strandChannel(kSpecularChannel);
    const std::vector<float>* roughness = view.strandChannel(kRoughnessChannel);
    const std::vector<float>* tilt = view.strandChannel(kTiltChannel);
    Primvar& gain = add("rman_specularGainR", lookDetail, Type::Float);
    Primvar& coneR = add("rman_coneAngleR", lookDetail, Type::Float);
    Primvar& coneTT = add("rman_coneAngleTT", lookDetail, Type::Float);
    Primvar& coneTRT = add("rman_coneAngleTRT", lookDetail, Type::Float);
    for (Primvar* p : {&gain, &coneR, &coneTT, &coneTRT}) p->floats.resize(lookCount);
    for (std::size_t at = 0; at < lookCount; ++at) {
        const std::size_t i = strandOf[at];
        HairLobes lobes = hairLobes(specular ? (*specular)[i] : look.specular, roughness ? (*roughness)[i] : look.roughness,
                                    tilt ? (*tilt)[i] : look.tilt);
        const float w = wetAt(at);
        if (w > 0.0f) lobes = wetLobes(lobes, w);
        const MarschnerLobes m = pxrMarschnerLobes(lobes);
        gain.floats[at] = m.specularGainR;
        coneR.floats[at] = m.coneAngleR;
        coneTT.floats[at] = m.coneAngleTT;
        coneTRT.floats[at] = m.coneAngleTRT;
    }
    // The cuticle tilt as PxrMarschnerHair's specularOffset, which takes no connection: the look's,
    // for the network's author (and a host's) to set.
    add("rman_specularOffset", Detail::Constant, Type::Float).floats = {pxrMarschnerLobes(hairLobes(look)).offset};
    return out;
}

ConstantLook constantLook(const std::vector<Primvar>& primvars) {
    ConstantLook out;
    // The mean of a primvar's values, per component (1 for a float, 3 for a color); false when absent.
    auto mean = [&](const char* name, float* into, std::size_t components) {
        for (const Primvar& p : primvars) {
            if (p.name != name || p.floats.empty() || p.floats.size() % components != 0) continue;
            const std::size_t n = p.floats.size() / components;
            for (std::size_t k = 0; k < components; ++k) {
                double sum = 0.0;
                for (std::size_t i = 0; i < n; ++i) sum += p.floats[i * components + k];
                into[k] = static_cast<float>(sum / static_cast<double>(n));
            }
            return true;
        }
        return false;
    };
    mean("rman_melanin", &out.melanin, 1);
    mean("rman_redness", &out.redness, 1);
    mean("rman_dye", out.dye.data(), 3);
    mean("rman_color", out.color.data(), 3);
    mean("rman_rgbShare", &out.rgbShare, 1);
    mean("rman_specularGainR", &out.specularGainR, 1);
    mean("rman_coneAngleR", &out.coneAngleR, 1);
    mean("rman_coneAngleTT", &out.coneAngleTT, 1);
    mean("rman_coneAngleTRT", &out.coneAngleTRT, 1);
    mean("rman_specularOffset", &out.specularOffset, 1);
    return out;
}

namespace {

// A number as the RIB writes it (Maya's prman_scene.number): 0 for one under 1e-12, else %.9g.
std::string ribNumber(double x) {
    if (std::abs(x) < 1e-12) return "0";
    char text[32];
    std::snprintf(text, sizeof text, "%.9g", x);
    return text;
}

}  // namespace

std::string hairNetworkRib(const std::string& prefix, float tilt, const std::string& indent) {
    struct Reader {
        const char *key, *varname, *type, *defaultParam;
        double r, g, b;  // the default (a float's in r); no default param: none
    };
    static const Reader kReaders[] = {{"melanin", "rman_melanin", "float", nullptr, 0, 0, 0},
                                      {"redness", "rman_redness", "float", nullptr, 0, 0, 0},
                                      {"dye", "rman_dye", "color", "defaultColor", 1, 1, 1},
                                      {"color", "rman_color", "color", nullptr, 0, 0, 0},
                                      {"share", "rman_rgbShare", "float", nullptr, 0, 0, 0},
                                      {"gainR", "rman_specularGainR", "float", "defaultFloat", 1.75, 0, 0},
                                      {"coneR", "rman_coneAngleR", "float", "defaultFloat", 8, 0, 0},
                                      {"coneTT", "rman_coneAngleTT", "float", "defaultFloat", 4, 0, 0},
                                      {"coneTRT", "rman_coneAngleTRT", "float", "defaultFloat", 16, 0, 0},
                                      {"opacity", "opacity", "float", "defaultFloat", 1, 0, 0}};
    const std::string& p = prefix;
    std::string out;
    for (const Reader& r : kReaders) {
        out += indent + "Pattern \"PxrPrimvar\" \"" + p + "pv_" + r.key + "\" \"string varname\" [\"" + r.varname + "\"] \"string type\" [\"" + r.type + "\"]";
        if (r.defaultParam) {
            const bool isColor = std::string(r.type) == "color";
            out += std::string(" \"") + (isColor ? "color " : "float ") + r.defaultParam + "\" ";
            out += isColor ? "[" + ribNumber(r.r) + " " + ribNumber(r.g) + " " + ribNumber(r.b) + "]" : "[" + ribNumber(r.r) + "]";
        }
        out += "\n";
    }
    out += indent + "Pattern \"PxrHairColor\" \"" + p + "pigment\" \"int mode\" [0] \"reference float melanin\" [\"" + p +
           "pv_melanin:resultF\"] \"reference float redness\" [\"" + p + "pv_redness:resultF\"] \"reference color dye\" [\"" + p +
           "pv_dye:resultRGB\"] \"float randomMelanin\" [0] \"float strayDensity\" [0] \"string hairIndexPrimvar\" [\"hair_index\"]\n";
    out += indent + "Pattern \"PxrHairColor\" \"" + p + "rgb\" \"int mode\" [1] \"reference color Color\" [\"" + p +
           "pv_color:resultRGB\"] \"color randomColor\" [0 0 0] \"float strayDensity\" [0]\n";
    for (const char* lobe : {"Diff", "TT", "TRT"}) {
        std::string lower(lobe);
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        out += indent + "Pattern \"PxrMix\" \"" + p + "mix_" + lower + "\" \"reference color color1\" [\"" + p + "pigment:result" + lobe +
               "\"] \"reference color color2\" [\"" + p + "rgb:result" + lobe + "\"] \"reference float mixer\" [\"" + p + "pv_share:resultF\"]\n";
    }
    out += indent + "Bxdf \"PxrMarschnerHair\" \"" + p + "hair\" \"reference color diffuseColor\" [\"" + p +
           "mix_diff:resultRGB\"] \"reference color specularColorTT\" [\"" + p + "mix_tt:resultRGB\"] \"reference color specularColorTRT\" [\"" + p +
           "mix_trt:resultRGB\"] \"reference float specularGainR\" [\"" + p + "pv_gainR:resultF\"] \"float specularGainTT\" [2]" +
           " \"float specularGainTRT\" [1.2] \"reference float specularConeAngleR\" [\"" + p +
           "pv_coneR:resultF\"] \"reference float specularConeAngleTT\" [\"" + p + "pv_coneTT:resultF\"] \"reference float specularConeAngleTRT\" [\"" +
           p + "pv_coneTRT:resultF\"] \"float specularOffset\" [" + ribNumber(-static_cast<double>(tilt)) +
           "] \"float specularIor\" [1.55] \"reference float presence\" [\"" + p + "pv_opacity:resultF\"]\n";
    return out;
}

}  // namespace groomlab::renderman
