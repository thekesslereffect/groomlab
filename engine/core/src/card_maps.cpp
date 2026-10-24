// SPDX-License-Identifier: Apache-2.0
// Card map exports: the sources, the presets (built in and the user's), the packer, and the image
// files it writes (TGA, PNG, TIFF, EXR).

#include "groomlab/card_maps.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "groomlab/parallel.h"
#include "groomlab/paths.h"
#include "groomlab/png.h"
#include "groomlab/portable_math.h"

namespace groomlab {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr int kMaxResolution = 8192;
constexpr std::size_t kRowGrain = 16;

// As the bake writes color (card_bake.cpp): the same float, so 8-bit maps are the same bytes.
float linearToSrgb(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x <= 0.0031308f ? 12.92f * x : 1.055f * pm::pow(x, 1.0f / 2.4f) - 0.055f;
}

std::uint8_t to8(float x) { return static_cast<std::uint8_t>(std::lround(std::clamp(x, 0.0f, 1.0f) * 255.0f)); }
std::uint16_t to16(float x) { return static_cast<std::uint16_t>(std::lround(std::clamp(x, 0.0f, 1.0f) * 65535.0f)); }

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trimmed(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

std::vector<std::string> splitPaths(const std::string& list) {
#ifdef _WIN32
    const char separator = ';';
#else
    const char separator = ':';
#endif
    std::vector<std::string> out;
    std::stringstream in(list);
    std::string dir;
    while (std::getline(in, dir, separator)) {
        if (!trimmed(dir).empty()) out.push_back(trimmed(dir));
    }
    return out;
}

enum Source { kDepth, kCoverage, kTangent, kFlow, kNormal, kColor, kAo, kRoot, kSeed, kId, kRootUv, kSpecular, kTranslucency, kIridescence };

// How far the translucency's strand count is softened: a few texels at 2K, scaled with the size.
int translucencyRadius(int size) { return std::max(1, static_cast<int>(std::lround(4.0f * static_cast<float>(size) / 2048.0f))); }

// The translucency source: how much hair the light goes through at each texel (the strands
// over it, softened over a few texels as light spreads in hair), 1 - exp(-layers / 2): one strand
// 0.39, two 0.63, four 0.86. Higher where the hair is denser.
std::vector<float> translucencyOf(const CardMaps& maps) {
    const int size = maps.size;
    const int radius = translucencyRadius(size);
    std::vector<float> a = maps.layers, tmp(a.size());
    const auto n = static_cast<std::size_t>(size);
    for (int pass = 0; pass < 2; ++pass) {  // two box blurs each way: close to a Gaussian
        parallelFor(0, n, [&](std::size_t y) {
            const std::size_t row = y * n;
            float sum = 0.0f;
            for (int x = -radius; x <= radius; ++x) sum += (x >= 0 && x < size) ? a[row + static_cast<std::size_t>(x)] : 0.0f;
            for (int x = 0; x < size; ++x) {
                tmp[row + static_cast<std::size_t>(x)] = sum / static_cast<float>(2 * radius + 1);
                const int add = x + radius + 1, drop = x - radius;
                if (add < size) sum += a[row + static_cast<std::size_t>(add)];
                if (drop >= 0) sum -= a[row + static_cast<std::size_t>(drop)];
            }
        }, kRowGrain);
        parallelFor(0, n, [&](std::size_t x) {
            float sum = 0.0f;
            for (int y = -radius; y <= radius; ++y) sum += (y >= 0 && y < size) ? tmp[static_cast<std::size_t>(y) * n + x] : 0.0f;
            for (int y = 0; y < size; ++y) {
                a[static_cast<std::size_t>(y) * n + x] = sum / static_cast<float>(2 * radius + 1);
                const int add = y + radius + 1, drop = y - radius;
                if (add < size) sum += tmp[static_cast<std::size_t>(add) * n + x];
                if (drop >= 0) sum -= tmp[static_cast<std::size_t>(drop) * n + x];
            }
        }, kRowGrain);
    }
    for (float& v : a) v = 1.0f - pm::exp(-std::max(v, 0.0f) / 2.0f);
    return a;
}

// The specular source: a strand's shine, brightest at the root and a little duller toward the
// tip (weathered ends), varied per strand (its seed: +-15%) and dimmed where the hair is
// occluded: (1 - 0.4 root) x (0.85 + 0.3 (seed - 0.5)) x (0.5 + 0.5 ao).
float specularAt(const CardMaps& maps, std::size_t p) {
    return std::clamp((1.0f - 0.4f * maps.root[p]) * (0.85f + 0.3f * (maps.seed[p] - 0.5f)) * (0.5f + 0.5f * maps.ao[p]), 0.0f, 1.0f);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Sources and channels

const std::vector<CardMapSource>& cardMapSources() {
    static const std::vector<CardMapSource> sources = {
        {"depth", "Depth", 1, "x", false, "How far the strands lie off the card plane: 0.5 on it, higher in front (Unreal's Depth; FiberShop's Height)"},
        {"coverage", "Coverage (alpha)", 1, "x", false, "Opacity: how much of the texel the strands cover"},
        {"tangent", "Tangent", 3, "xyz", false, "The strands' direction in card space as Unreal reads it: x across, y root to tip, z out of the card, each 0.5 + 0.5 x"},
        {"flow", "Flow (direction)", 2, "xy", false, "The strands' direction in the texture as a flow map: x across, y root to tip up the texture, 0.5 + 0.5 x (FiberShop's Direction)"},
        {"normal", "Normal", 3, "xyz", false, "Tangent-space normal of the strands as round fibers: out of the card at a strand's middle, turning to its sides at its edges"},
        {"color", "Color (albedo)", 3, "rgb", true, "The strands' color (sRGB in an sRGB file, else linear)"},
        {"ao", "Ambient occlusion", 1, "x", false, "How open the hair is (1) or how much hair lies further out (darker)"},
        {"root", "Root to tip", 1, "x", false, "0 at each strand's root, 1 at its tip (Unreal's CoordU)"},
        {"seed", "Seed (per strand)", 1, "x", false, "A random value per strand (Unreal's Seed)"},
        {"id", "ID (per clump)", 1, "x", false, "A random value per clump (the strands' clumpId; per card without one)"},
        {"rootuv", "Root UV", 2, "uv", false, "Where each strand grows on the scalp: its UV there (0 without scalp UVs)"},
        {"specular", "Specular", 1, "x", false, "Shine: brightest at the root, duller to the tip, varied per strand, dimmed where occluded"},
        {"translucency", "Translucency", 1, "x", false, "How much hair the light passes through: higher where the strands lie denser (a thickness map; invert for transmission)"},
        {"iridescence", "Iridescence", 1, "x", false, "The strands' iridescence (FeatherColor's channel; 0 when none)"},
    };
    return sources;
}

CardMapChannel parseCardMapChannel(const std::string& textIn) {
    CardMapChannel out;
    std::string text = trimmed(textIn);
    if (text.empty()) throw std::invalid_argument("an empty channel");
    if (!text.empty() && text.back() == '?') {
        out.optional = true;
        text = trimmed(text.substr(0, text.size() - 1));
    }
    if (text.size() > 2 && text.compare(0, 2, "1-") == 0) {
        out.invert = true;
        text = trimmed(text.substr(2));
    }
    // A constant.
    char* end = nullptr;
    const float number = std::strtof(text.c_str(), &end);
    if (!text.empty() && end && *end == '\0' && (std::isdigit(static_cast<unsigned char>(text[0])) || text[0] == '.')) {
        if (!(number >= 0.0f && number <= 1.0f)) throw std::invalid_argument("'" + textIn + "': a constant is 0 to 1");
        out.constant = number;
        if (out.optional) throw std::invalid_argument("'" + textIn + "': a constant is always there");
        return out;
    }
    const std::string lowered = lower(text);
    const std::size_t dot = lowered.find('.');
    const std::string name = lowered.substr(0, dot);
    const std::string letter = dot == std::string::npos ? std::string() : lowered.substr(dot + 1);
    const auto& sources = cardMapSources();
    std::string alias = name == "alpha" ? "coverage" : name == "albedo" ? "color" : name == "occlusion" ? "ao" : name;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (sources[i].name != alias) continue;
        out.source = static_cast<int>(i);
        if (letter.empty()) {
            if (sources[i].channels != 1) {
                throw std::invalid_argument("'" + textIn + "': " + sources[i].name + " has " + std::to_string(sources[i].channels) +
                                            " channels: name one (" + sources[i].name + "." + sources[i].channelNames.substr(0, 1) + ")");
            }
            return out;
        }
        static const std::string sets[] = {"rgba", "xyzw", "uv"};
        int component = -1;
        if (letter.size() == 1) {
            for (const std::string& set : sets) {
                const std::size_t at = set.find(letter[0]);
                if (at != std::string::npos) component = static_cast<int>(at);
            }
        }
        if (component < 0 || component >= sources[i].channels) {
            throw std::invalid_argument("'" + textIn + "': " + sources[i].name + " has no channel " + letter);
        }
        out.component = component;
        return out;
    }
    throw std::invalid_argument("'" + textIn + "': no map named " + name);
}

std::vector<std::string> cardMapChannelNames() {
    std::vector<std::string> out;
    for (const CardMapSource& s : cardMapSources()) {
        if (s.channels == 1) {
            out.push_back(s.name);
        } else {
            for (const char c : s.channelNames) out.push_back(s.name + "." + c);
        }
    }
    out.push_back("0");
    out.push_back("1");
    return out;
}

// ---------------------------------------------------------------------------------------------
// The maps: quantized, made smaller

CardTextures cardTexturesFromMaps(const CardMaps& m) {
    CardTextures t;
    t.size = m.size;
    const auto n = static_cast<std::size_t>(m.size);
    const std::size_t pixels = n * n;
    t.coverage.resize(pixels);
    t.depth.resize(pixels);
    t.occlusion.resize(pixels);
    t.tangent.resize(pixels * 3);
    t.normal.resize(pixels * 3);
    t.color.resize(pixels * 3);
    t.attribute.resize(pixels * 4);
    if (m.iridescent) t.iridescence.resize(pixels);
    parallelFor(0, n, [&](std::size_t y) {
        for (std::size_t p = y * n; p < (y + 1) * n; ++p) {
            t.coverage[p] = to8(m.coverage[p]);
            t.depth[p] = to8(m.depth[p]);
            t.occlusion[p] = to8(m.ao[p]);
            for (std::size_t k = 0; k < 3; ++k) {
                t.tangent[p * 3 + k] = to8(0.5f + 0.5f * m.tangent[p * 3 + k]);
                const float normal = m.normal[p * 3 + k];
                t.normal[p * 3 + k] = to8(0.5f + 0.5f * (k == 1 ? -normal : normal));  // DirectX's green, as a preset packs it
                t.color[p * 3 + k] = to8(linearToSrgb(m.color[p * 3 + k]));
            }
            t.attribute[p * 4 + 0] = to8(m.rootU[p]);
            t.attribute[p * 4 + 1] = to8(m.rootV[p]);
            t.attribute[p * 4 + 2] = to8(m.seed[p]);
            t.attribute[p * 4 + 3] = to8(m.root[p]);
            if (m.iridescent) t.iridescence[p] = to8(m.iridescence[p]);
        }
    }, kRowGrain);
    return t;
}

std::vector<std::uint8_t> cardNormalOpenGl(const CardTextures& t) {
    std::vector<std::uint8_t> out = t.normal;
    for (std::size_t i = 1; i < out.size(); i += 3) out[i] = static_cast<std::uint8_t>(255 - out[i]);
    return out;
}

CardMaps downsampleCardMaps(const CardMaps& m, int factor) {
    if (factor <= 1) return m;
    CardMaps out;
    out.size = std::max(1, m.size / factor);
    out.iridescent = m.iridescent;
    const auto n = static_cast<std::size_t>(out.size), big = static_cast<std::size_t>(m.size);
    const std::size_t pixels = n * n;
    std::vector<std::vector<float>*> scalars = {&out.depth, &out.ao, &out.rootU, &out.rootV, &out.seed, &out.root, &out.id, &out.iridescence};
    const std::vector<const std::vector<float>*> from = {&m.depth, &m.ao, &m.rootU, &m.rootV, &m.seed, &m.root, &m.id, &m.iridescence};
    for (auto* v : scalars) v->assign(pixels, 0.0f);
    out.coverage.assign(pixels, 0.0f);
    out.layers.assign(pixels, 0.0f);
    out.tangent.assign(pixels * 3, 0.0f);
    out.normal.assign(pixels * 3, 0.0f);
    out.color.assign(pixels * 3, 0.0f);
    const float count = static_cast<float>(factor * factor);
    parallelFor(0, n, [&](std::size_t y) {
        for (std::size_t x = 0; x < n; ++x) {
            const std::size_t p = y * n + x;
            // Weighted by coverage where the strands are, plainly where none are.
            float cover = 0.0f;
            for (int dy = 0; dy < factor; ++dy) {
                for (int dx = 0; dx < factor; ++dx) cover += m.coverage[(y * factor + static_cast<std::size_t>(dy)) * big + x * factor + static_cast<std::size_t>(dx)];
            }
            const bool weighted = cover > 1e-6f;
            float wsum = 0.0f;
            Vec3f tan, nor;
            for (int dy = 0; dy < factor; ++dy) {
                for (int dx = 0; dx < factor; ++dx) {
                    const std::size_t q = (y * factor + static_cast<std::size_t>(dy)) * big + x * factor + static_cast<std::size_t>(dx);
                    const float w = weighted ? m.coverage[q] : 1.0f;
                    wsum += w;
                    for (std::size_t s = 0; s < scalars.size(); ++s) (*scalars[s])[p] += (*from[s])[q] * w;
                    for (std::size_t k = 0; k < 3; ++k) out.color[p * 3 + k] += m.color[q * 3 + k] * w;
                    tan = tan + Vec3f{m.tangent[q * 3], m.tangent[q * 3 + 1], m.tangent[q * 3 + 2]} * w;
                    nor = nor + Vec3f{m.normal[q * 3], m.normal[q * 3 + 1], m.normal[q * 3 + 2]} * w;
                    out.layers[p] += m.layers[q];
                }
            }
            out.coverage[p] = cover / count;
            out.layers[p] /= count;
            const float inv = wsum > 0.0f ? 1.0f / wsum : 0.0f;
            for (auto* v : scalars) (*v)[p] *= inv;
            for (std::size_t k = 0; k < 3; ++k) out.color[p * 3 + k] *= inv;
            tan = tan.length() > 1e-6f ? tan.normalized() : Vec3f{0, 1, 0};
            nor = nor.length() > 1e-6f ? nor.normalized() : Vec3f{0, 0, 1};
            out.tangent[p * 3] = tan.x, out.tangent[p * 3 + 1] = tan.y, out.tangent[p * 3 + 2] = tan.z;
            out.normal[p * 3] = nor.x, out.normal[p * 3 + 1] = nor.y, out.normal[p * 3 + 2] = nor.z;
        }
    }, kRowGrain);
    return out;
}

// ---------------------------------------------------------------------------------------------
// Presets

namespace {

CardMapFile file(const std::string& map, std::vector<std::string> channels, const std::string& colorSpace = "auto") {
    CardMapFile f;
    f.map = map;
    f.channels = std::move(channels);
    f.colorSpace = colorSpace;
    return f;
}

CardMapPreset preset(const std::string& name, const std::string& description, const std::string& format, const std::string& normal,
                     std::vector<CardMapFile> files) {
    CardMapPreset p;
    p.name = name;
    p.description = description;
    p.format = format;
    p.bitDepth = 8;
    p.normal = normal;
    p.files = std::move(files);
    p.builtin = true;
    return p;
}

bool fileSafe(const std::string& s) {
    if (s.empty()) return false;
    for (const char c : s) {
        if (std::string("/\\:*?\"<>|").find(c) != std::string::npos || static_cast<unsigned char>(c) < 32) return false;
    }
    return true;
}

}  // namespace

const std::vector<CardMapPreset>& builtinCardMapPresets() {
    static const std::vector<CardMapPreset> presets = {
        // Unreal's groom asset, Cards LOD: the textures its Default layout takes (Hair Attributes
        // reads them), plus color, occlusion and the normal. The bake's own files, byte for byte.
        preset("Unreal Hair Cards",
               "Unreal's hair card textures, as Bake Maps writes them: Depth, Coverage, Tangent, Attribute (root U, root V, "
               "seed, root to tip), Color (with the iridescence in alpha when the groom has any), AO and Normal (DirectX)",
               "tga", "directx",
               {file("Depth", {"depth"}), file("Coverage", {"coverage"}), file("Tangent", {"tangent.x", "tangent.y", "tangent.z"}),
                file("Attribute", {"rootuv.u", "rootuv.v", "seed", "root"}),
                file("Color", {"color.r", "color.g", "color.b", "iridescence?"}), file("AO", {"ao"}),
                file("Normal", {"normal.x", "normal.y", "normal.z"})}),
        // Four textures for a hair card material of your own (Hair shading model, Masked): the
        // color with occlusion in alpha (sRGB on; alpha stays linear), the data (sRGB off), the
        // tangent and the normal.
        preset("Unreal (packed)",
               "Four textures for an Unreal hair card material of your own: Color (RGB) + AO (A), Data (depth, root to "
               "tip, seed, coverage: R G B A, sRGB off), Tangent and Normal (DirectX)",
               "tga", "directx",
               {file("Color", {"color.r", "color.g", "color.b", "ao"}), file("Data", {"depth", "root", "seed", "coverage"}),
                file("Tangent", {"tangent.x", "tangent.y", "tangent.z"}), file("Normal", {"normal.x", "normal.y", "normal.z"})}),
        // Unity HDRP's Hair shader graph: base color and alpha, a normal map (OpenGL's green), the
        // Lit mask map's layout (metallic 0, AO, detail: root to tip, smoothness: the specular),
        // the strand direction, and the rest for the graph (depth, ID, thickness).
        preset("Unity HDRP Hair",
               "Unity HDRP Hair: BaseColor (RGB, alpha coverage), Normal (OpenGL), Mask (0, AO, root to tip, smoothness from "
               "the specular), Tangent (the strand direction) and Data (depth, clump ID, translucency as thickness)",
               "png", "opengl",
               {file("BaseColor", {"color.r", "color.g", "color.b", "coverage"}),
                file("Normal", {"normal.x", "normal.y", "normal.z"}), file("Mask", {"0", "ao", "root", "specular"}),
                file("Tangent", {"tangent.x", "tangent.y", "tangent.z"}), file("Data", {"depth", "id", "translucency"})}),
        // Every map its own file (FiberShop's simple mode).
        preset("Separate maps (all)",
               "Every map its own file: Albedo, Alpha, Depth, Direction (flow), Tangent, Normal, Root, ID, Seed, AO, Specular, "
               "Translucency and Root UV (Iridescence when the groom has any)",
               "png", "opengl",
               {file("Albedo", {"color.r", "color.g", "color.b"}), file("Alpha", {"coverage"}), file("Depth", {"depth"}),
                file("Direction", {"flow.x", "flow.y"}), file("Tangent", {"tangent.x", "tangent.y", "tangent.z"}),
                file("Normal", {"normal.x", "normal.y", "normal.z"}), file("Root", {"root"}), file("ID", {"id"}), file("Seed", {"seed"}),
                file("AO", {"ao"}), file("Specular", {"specular"}), file("Translucency", {"translucency"}),
                file("RootUV", {"rootuv.u", "rootuv.v"}), file("Iridescence", {"iridescence?"})}),
    };
    return presets;
}

std::string cardMapPresetToJson(const CardMapPreset& p) {
    json j;
    j["name"] = p.name;
    if (!p.description.empty()) j["description"] = p.description;
    j["format"] = p.format;
    j["bitDepth"] = p.bitDepth;
    j["normal"] = p.normal;
    json files = json::array();
    for (const CardMapFile& f : p.files) {
        json jf;
        jf["map"] = f.map;
        jf["channels"] = f.channels;
        if (f.colorSpace != "auto") jf["colorSpace"] = f.colorSpace;
        files.push_back(jf);
    }
    j["files"] = files;
    return j.dump(2);
}

CardMapPreset cardMapPresetFromJson(const std::string& text) {
    json j;
    try {
        j = json::parse(text);
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("not JSON: ") + e.what());
    }
    if (!j.is_object()) throw std::runtime_error("a card map preset is a JSON object");
    CardMapPreset p;
    try {
        p.name = j.value("name", std::string());
        p.description = j.value("description", std::string());
        p.format = cardMapFormat(j.value("format", std::string("tga")));
        p.bitDepth = j.value("bitDepth", 8);
        p.normal = lower(j.value("normal", std::string("opengl")));
        if (!j.contains("files") || !j["files"].is_array()) throw std::runtime_error("no \"files\" list");
        for (const json& jf : j["files"]) {
            CardMapFile f;
            f.map = jf.value("map", std::string());
            f.colorSpace = lower(jf.value("colorSpace", std::string("auto")));
            if (jf.contains("channels")) {
                for (const json& c : jf["channels"]) f.channels.push_back(c.is_string() ? c.get<std::string>() : c.dump());
            }
            p.files.push_back(std::move(f));
        }
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("not a card map preset: ") + e.what());
    }
    if (p.format.empty()) throw std::runtime_error("unknown format " + j.value("format", std::string()));
    if (const std::string error = checkCardMapPreset(p); !error.empty()) throw std::runtime_error(error);
    return p;
}

std::string checkCardMapPreset(const CardMapPreset& p) {
    if (trimmed(p.name).empty()) return "the preset has no name";
    if (p.files.empty()) return "the preset writes no files";
    if (cardMapFormat(p.format).empty()) return "unknown format '" + p.format + "'";
    const std::vector<int> depths = cardMapBitDepths(cardMapFormat(p.format));
    if (std::find(depths.begin(), depths.end(), p.bitDepth) == depths.end()) {
        return p.format + " has no " + std::to_string(p.bitDepth) + "-bit form";
    }
    if (p.normal != "opengl" && p.normal != "directx") return "the normal is opengl or directx, not '" + p.normal + "'";
    std::set<std::string> maps;
    for (const CardMapFile& f : p.files) {
        if (!fileSafe(f.map)) return "'" + f.map + "' can't be in a file name";
        if (!maps.insert(lower(f.map)).second) return "two files named " + f.map;
        if (f.channels.empty() || f.channels.size() > 4) return f.map + ": 1 to 4 channels, not " + std::to_string(f.channels.size());
        if (f.colorSpace != "auto" && f.colorSpace != "srgb" && f.colorSpace != "linear") {
            return f.map + ": the color space is auto, srgb or linear, not '" + f.colorSpace + "'";
        }
        bool optional = false;
        for (const std::string& c : f.channels) {
            try {
                const CardMapChannel parsed = parseCardMapChannel(c);
                if (optional && !parsed.optional) return f.map + ": a channel only there when the groom has it ('?') comes last";
                optional = optional || parsed.optional;
            } catch (const std::invalid_argument& e) {
                return f.map + ": " + e.what();
            }
        }
    }
    return {};
}

std::string cardMapPresetUserDir() {
    if (const std::optional<std::string> own = environmentVariable("GROOMLAB_USER_CARD_PRESETS"); own && !own->empty()) return *own;
#ifdef _WIN32
    const std::optional<std::string> home = environmentVariable("USERPROFILE");
#else
    const std::optional<std::string> home = environmentVariable("HOME");
#endif
    if (!home || home->empty()) return {};
    return (fs::u8path(*home) / "Documents" / "GroomLab" / "card_presets").generic_u8string();
}

std::vector<std::string> cardMapPresetDirs() {
    std::vector<std::string> dirs;
    if (const std::optional<std::string> path = environmentVariable("GROOMLAB_CARD_PRESET_PATH")) dirs = splitPaths(*path);
    if (const std::string user = cardMapPresetUserDir(); !user.empty()) dirs.push_back(user);
    return dirs;
}

std::vector<CardMapPreset> cardMapPresets() {
    std::vector<CardMapPreset> out = builtinCardMapPresets();
    std::set<std::string> seen;
    for (const CardMapPreset& p : out) seen.insert(lower(p.name));
    std::vector<CardMapPreset> users;
    for (const std::string& dir : cardMapPresetDirs()) {
        std::vector<fs::path> files;
        std::error_code ec;
        for (fs::directory_iterator it(fs::u8path(dir), ec), end; !ec && it != end; it.increment(ec)) {
            if (lower(it->path().extension().u8string()) == ".json") files.push_back(it->path());
        }
        std::sort(files.begin(), files.end());
        for (const fs::path& path : files) {
            std::ifstream in(path, std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            try {
                CardMapPreset p = cardMapPresetFromJson(text.str());
                if (!seen.insert(lower(p.name)).second) continue;  // a built-in's or an earlier folder's
                p.path = path.generic_u8string();
                users.push_back(std::move(p));
            } catch (const std::exception&) {
                // Not a preset: skipped.
            }
        }
    }
    std::stable_sort(users.begin(), users.end(), [](const CardMapPreset& a, const CardMapPreset& b) { return lower(a.name) < lower(b.name); });
    out.insert(out.end(), users.begin(), users.end());
    return out;
}

std::optional<CardMapPreset> findCardMapPreset(const std::string& name) {
    const std::vector<CardMapPreset> all = cardMapPresets();
    for (const CardMapPreset& p : all) {
        if (p.name == name) return p;
    }
    for (const CardMapPreset& p : all) {
        if (lower(p.name) == lower(trimmed(name))) return p;
    }
    return std::nullopt;
}

namespace {

std::string presetFileName(const std::string& name) {
    std::string out;
    for (const char c : trimmed(name)) {
        const auto u = static_cast<unsigned char>(c);
        out += (std::isalnum(u) || c == '-' || c == '_' || c == ' ' || c == '(' || c == ')' || u >= 128) ? c : '_';
    }
    return out + ".json";
}

}  // namespace

std::string saveCardMapPreset(const CardMapPreset& presetIn) {
    CardMapPreset p = presetIn;
    p.name = trimmed(p.name);
    for (const CardMapPreset& b : builtinCardMapPresets()) {
        if (lower(b.name) == lower(p.name)) throw std::runtime_error("'" + p.name + "' is a built-in preset: save it under another name");
    }
    if (lower(p.name) == "custom") throw std::runtime_error("'Custom' is the panel's own: save the preset under another name");
    if (const std::string error = checkCardMapPreset(p); !error.empty()) throw std::runtime_error(error);
    const std::string dir = cardMapPresetUserDir();
    if (dir.empty()) throw std::runtime_error("no user folder to save card presets in (no home folder)");
    std::error_code ec;
    fs::create_directories(fs::u8path(dir), ec);
    // A preset of that name saved before under another file name is replaced too.
    for (const CardMapPreset& old : cardMapPresets()) {
        if (!old.builtin && lower(old.name) == lower(p.name) && fs::u8path(old.path).parent_path() == fs::u8path(dir)) {
            fs::remove(fs::u8path(old.path), ec);
        }
    }
    const fs::path path = fs::u8path(dir) / fs::u8path(presetFileName(p.name));
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("can't write " + path.generic_u8string());
    p.builtin = false;
    out << cardMapPresetToJson(p) << "\n";
    if (!out) throw std::runtime_error("can't write " + path.generic_u8string());
    return path.generic_u8string();
}

bool deleteCardMapPreset(const std::string& name) {
    const std::string dir = cardMapPresetUserDir();
    if (dir.empty()) return false;
    bool removed = false;
    std::error_code ec;
    for (const CardMapPreset& p : cardMapPresets()) {
        if (p.builtin || lower(p.name) != lower(trimmed(name))) continue;
        if (fs::u8path(p.path).parent_path() != fs::u8path(dir)) continue;  // another folder's: not the user's to delete here
        removed = fs::remove(fs::u8path(p.path), ec) || removed;
    }
    return removed;
}

// ---------------------------------------------------------------------------------------------
// Settings

int cardMapAutoPadding(int resolution) { return std::max(8, static_cast<int>(std::lround(8.0 * resolution / 2048.0))); }

int cardMapSupersample(int resolution, int requested) {
    int factor = requested >= 4 ? 4 : requested >= 2 ? 2 : 1;
    while (factor > 1 && resolution * factor > kMaxResolution) factor /= 2;
    return factor;
}

std::string cardMapFormat(const std::string& text) {
    std::string f = lower(trimmed(text));
    if (!f.empty() && f[0] == '.') f = f.substr(1);
    if (f == "tga" || f == "png" || f == "exr") return f;
    if (f == "tif" || f == "tiff") return "tiff";
    return {};
}

std::vector<int> cardMapBitDepths(const std::string& format) {
    const std::string f = cardMapFormat(format);
    if (f == "tga") return {8};
    if (f == "png") return {8, 16};
    if (f == "tiff") return {8, 16, 32};
    if (f == "exr") return {16, 32};
    return {};
}

CardBakeSettings cardMapBakeSettings(const CardBakeSettings& base, const CardMapExport& settings, int resolution) {
    CardBakeSettings out = base;
    const int factor = cardMapSupersample(resolution, settings.supersample);
    const int padding = settings.padding >= 0 ? settings.padding : cardMapAutoPadding(resolution);
    out.size = resolution * factor;
    out.dilate = padding * factor;
    out.minStrandTexels = base.minStrandTexels * static_cast<float>(factor);
    return out;
}

std::string cardMapFileName(const std::string& nameTemplate, const std::string& name, const std::string& map, int resolution) {
    std::string out = nameTemplate.empty() ? std::string("{name}_{map}") : nameTemplate;
    auto replace = [&](const std::string& token, const std::string& value) {
        for (std::size_t at = out.find(token); at != std::string::npos; at = out.find(token, at + value.size())) {
            out.replace(at, token.size(), value);
        }
    };
    replace("{name}", name);
    replace("{map}", map);
    replace("{res}", std::to_string(resolution));
    return out;
}

// ---------------------------------------------------------------------------------------------
// Packing and writing

namespace {

std::string extensionOf(const std::string& format) { return format == "tiff" ? ".tif" : "." + format; }

std::vector<float> packFile(const CardMaps& maps, const CardMapFile& file, const std::string& normal, const std::string& format,
                            int* channelsOut, std::vector<float>* translucency) {
    std::vector<CardMapChannel> channels;
    for (const std::string& text : file.channels) {
        CardMapChannel c = parseCardMapChannel(text);
        if (c.optional && c.source == kIridescence && !maps.iridescent) continue;  // the groom has none
        channels.push_back(c);
    }
    if (channels.empty()) {
        *channelsOut = 0;
        return {};
    }
    const int written = channels.size() == 2 ? 3 : static_cast<int>(channels.size());
    *channelsOut = written;
    const bool srgb = file.colorSpace == "srgb" || (file.colorSpace == "auto" && format != "exr");
    const bool directx = normal == "directx";
    for (const CardMapChannel& c : channels) {
        if (c.source == kTranslucency && translucency->empty()) *translucency = translucencyOf(maps);
    }
    const auto n = static_cast<std::size_t>(maps.size);
    const std::size_t pixels = n * n;
    std::vector<float> out(pixels * static_cast<std::size_t>(written), 0.0f);
    const auto stride = static_cast<std::size_t>(written);
    for (std::size_t ci = 0; ci < channels.size(); ++ci) {
        const CardMapChannel c = channels[ci];
        const auto k = static_cast<std::size_t>(c.component);
        const std::vector<float>& tl = *translucency;
        parallelFor(0, n, [&](std::size_t y) {
            for (std::size_t p = y * n; p < (y + 1) * n; ++p) {
                float v = c.constant;
                switch (c.source) {
                case kDepth: v = maps.depth[p]; break;
                case kCoverage: v = maps.coverage[p]; break;
                case kTangent: v = 0.5f + 0.5f * maps.tangent[p * 3 + k]; break;
                case kFlow: {
                    const float x = maps.tangent[p * 3], yy = maps.tangent[p * 3 + 1];
                    const float len = std::sqrt(x * x + yy * yy);
                    const float f = len > 1e-6f ? (k == 0 ? x : yy) / len : (k == 0 ? 0.0f : 1.0f);
                    v = 0.5f + 0.5f * f;
                    break;
                }
                case kNormal: {
                    const float f = maps.normal[p * 3 + k];
                    v = 0.5f + 0.5f * ((k == 1 && directx) ? -f : f);
                    break;
                }
                case kColor: v = srgb ? linearToSrgb(maps.color[p * 3 + k]) : maps.color[p * 3 + k]; break;
                case kAo: v = maps.ao[p]; break;
                case kRoot: v = maps.root[p]; break;
                case kSeed: v = maps.seed[p]; break;
                case kId: v = maps.id[p]; break;
                case kRootUv: v = k == 0 ? maps.rootU[p] : maps.rootV[p]; break;
                case kSpecular: v = specularAt(maps, p); break;
                case kTranslucency: v = tl[p]; break;
                case kIridescence: v = maps.iridescence[p]; break;
                default: break;
                }
                out[p * stride + ci] = c.invert ? 1.0f - v : v;
            }
        }, kRowGrain);
    }
    return out;
}

}  // namespace

std::vector<float> packCardMapFile(const CardMaps& maps, const CardMapFile& file, const std::string& normal, const std::string& format,
                                   int* channels) {
    std::vector<float> translucency;
    return packFile(maps, file, normal, cardMapFormat(format), channels, &translucency);
}

std::vector<std::string> writeCardMaps(const CardMaps& maps, const CardMapPreset& preset, const CardMapExport& settings,
                                       const std::string& folder, const std::string& name) {
    if (const std::string error = checkCardMapPreset(preset); !error.empty()) throw std::runtime_error(error);
    const std::string format = cardMapFormat(settings.format.empty() ? preset.format : settings.format);
    if (format.empty()) throw std::runtime_error("unknown format '" + settings.format + "': tga, png, tiff or exr");
    const std::vector<int> depths = cardMapBitDepths(format);
    int bitDepth = settings.bitDepth;
    if (bitDepth == 0) bitDepth = std::find(depths.begin(), depths.end(), preset.bitDepth) != depths.end() ? preset.bitDepth : depths.front();
    if (std::find(depths.begin(), depths.end(), bitDepth) == depths.end()) {
        throw std::runtime_error(format + " has no " + std::to_string(bitDepth) + "-bit form");
    }
    std::error_code ec;
    if (!folder.empty()) fs::create_directories(fs::u8path(folder), ec);
    std::vector<std::string> paths;
    std::vector<float> translucency;  // made once, for every file that takes it
    for (const CardMapFile& file : preset.files) {
        int channels = 0;
        const std::vector<float> pixels = packFile(maps, file, preset.normal, format, &channels, &translucency);
        if (channels == 0) continue;  // only channels the groom doesn't have
        const std::string base = cardMapFileName(settings.nameTemplate, name, file.map, maps.size);
        if (!fileSafe(base)) throw std::runtime_error("'" + base + "' can't be a file name");
        const std::string path = (fs::u8path(folder) / fs::u8path(base + extensionOf(format))).generic_u8string();
        writeImageFile(path, format, bitDepth, maps.size, maps.size, channels, pixels);
        paths.push_back(path);
    }
    return paths;
}

// ---------------------------------------------------------------------------------------------
// JSON for hosts

std::string cardMapPresetsJson() {
    json out = json::array();
    for (const CardMapPreset& p : cardMapPresets()) {
        json j = json::parse(cardMapPresetToJson(p));
        j["description"] = p.description;
        j["builtin"] = p.builtin;
        j["path"] = p.path;
        for (json& f : j["files"]) {
            if (!f.contains("colorSpace")) f["colorSpace"] = "auto";
        }
        out.push_back(j);
    }
    return out.dump();
}

std::string cardMapSourcesJson() {
    json sources = json::array();
    for (const CardMapSource& s : cardMapSources()) {
        sources.push_back({{"name", s.name}, {"label", s.label}, {"channels", s.channels}, {"channelNames", s.channelNames},
                           {"color", s.color}, {"description", s.description}});
    }
    json out;
    out["sources"] = sources;
    out["channels"] = cardMapChannelNames();
    out["formats"] = json::array();
    for (const char* f : {"tga", "png", "tiff", "exr"}) out["formats"].push_back({{"name", f}, {"bitDepths", cardMapBitDepths(f)}});
    return out.dump();
}

// ---------------------------------------------------------------------------------------------
// Image files

namespace {

void writeBytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(fs::u8path(path), std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write '" + path + "'");
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("cannot write '" + path + "'");
}

template <typename T>
void put(std::vector<std::uint8_t>& out, T value) {
    std::uint8_t bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));  // little-endian hosts (x86, ARM)
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

void putString(std::vector<std::uint8_t>& out, const std::string& s) {
    out.insert(out.end(), s.begin(), s.end());
    out.push_back(0);
}

// A float as a 16-bit half (round to nearest even).
std::uint16_t toHalf(float f) {
    std::uint32_t x;
    std::memcpy(&x, &f, 4);
    const std::uint32_t sign = (x >> 16) & 0x8000u;
    const std::uint32_t biased = (x >> 23) & 0xFFu;
    std::uint32_t mant = x & 0x7FFFFFu;
    if (biased == 0xFFu) return static_cast<std::uint16_t>(sign | 0x7C00u | (mant ? 0x200u : 0u));  // inf, NaN
    const int exp = static_cast<int>(biased) - 127 + 15;
    if (exp >= 31) return static_cast<std::uint16_t>(sign | 0x7C00u);  // too big: inf
    if (exp <= 0) {  // a denormal half, or 0
        if (exp < -10) return static_cast<std::uint16_t>(sign);
        mant |= 0x800000u;
        const auto shift = static_cast<std::uint32_t>(14 - exp);
        std::uint32_t half = mant >> shift;
        const std::uint32_t rest = mant & ((1u << shift) - 1u), mid = 1u << (shift - 1u);
        if (rest > mid || (rest == mid && (half & 1u))) ++half;
        return static_cast<std::uint16_t>(sign | half);
    }
    std::uint32_t half = sign | (static_cast<std::uint32_t>(exp) << 10) | (mant >> 13);
    const std::uint32_t rest = mant & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u))) ++half;  // a carry into the exponent is right
    return static_cast<std::uint16_t>(half);
}

// Uncompressed baseline TIFF, one strip: gray or RGB(A) (alpha unassociated), 8 or 16 bits
// unsigned or 32-bit float a sample, little-endian.
void writeTiff(const std::string& path, int bitDepth, int width, int height, int channels, const std::vector<float>& pixels) {
    const std::size_t samples = pixels.size();
    const std::size_t bytesPer = static_cast<std::size_t>(bitDepth / 8);
    const std::size_t dataBytes = samples * bytesPer;
    if (dataBytes > 0xFFFFFFF0ull) throw std::runtime_error("the image is too big for a TIFF");
    struct Entry {
        std::uint16_t tag, type;
        std::uint32_t count, value;
    };
    // The arrays a tag can't hold inline (more than 4 bytes), after the IFD; then the pixels.
    const bool arrays = channels > 2;
    std::vector<Entry> entries;
    const std::uint16_t kShort = 3, kLong = 4;
    const auto ch = static_cast<std::uint32_t>(channels);
    const std::size_t count = channels == 4 ? 12 : 11;
    const std::uint32_t ifdAt = 8, ifdBytes = static_cast<std::uint32_t>(2 + count * 12 + 4);
    const std::uint32_t bitsAt = ifdAt + ifdBytes, formatAt = bitsAt + (arrays ? 2 * ch : 0);
    const std::uint32_t dataAt = formatAt + (arrays ? 2 * ch : 0);
    const std::uint32_t inlineShort = static_cast<std::uint32_t>(bitDepth);
    const std::uint32_t sampleFormat = bitDepth == 32 ? 3u : 1u;
    entries.push_back({256, kLong, 1, static_cast<std::uint32_t>(width)});
    entries.push_back({257, kLong, 1, static_cast<std::uint32_t>(height)});
    entries.push_back({258, kShort, ch, arrays ? bitsAt : inlineShort});
    entries.push_back({259, kShort, 1, 1});                          // no compression
    entries.push_back({262, kShort, 1, channels >= 3 ? 2u : 1u});    // RGB, or gray (black is zero)
    entries.push_back({273, kLong, 1, dataAt});                      // the one strip
    entries.push_back({277, kShort, 1, ch});
    entries.push_back({278, kLong, 1, static_cast<std::uint32_t>(height)});
    entries.push_back({279, kLong, 1, static_cast<std::uint32_t>(dataBytes)});
    entries.push_back({284, kShort, 1, 1});                          // chunky
    if (channels == 4) entries.push_back({338, kShort, 1, 2});       // unassociated alpha
    entries.push_back({339, kShort, ch, arrays ? formatAt : sampleFormat});
    std::vector<std::uint8_t> out;
    out.reserve(dataAt + dataBytes);
    out.push_back('I');
    out.push_back('I');
    put<std::uint16_t>(out, 42);
    put<std::uint32_t>(out, ifdAt);
    put<std::uint16_t>(out, static_cast<std::uint16_t>(entries.size()));
    for (const Entry& e : entries) {
        put<std::uint16_t>(out, e.tag);
        put<std::uint16_t>(out, e.type);
        put<std::uint32_t>(out, e.count);
        if (e.type == kShort && e.count == 1) {
            put<std::uint16_t>(out, static_cast<std::uint16_t>(e.value));
            put<std::uint16_t>(out, 0);
        } else if (e.type == kShort && e.count <= 2) {  // two shorts inline (not written: 1, 3 or 4 channels)
            put<std::uint16_t>(out, static_cast<std::uint16_t>(e.value));
            put<std::uint16_t>(out, static_cast<std::uint16_t>(e.value));
        } else {
            put<std::uint32_t>(out, e.value);
        }
    }
    put<std::uint32_t>(out, 0);  // no next IFD
    if (arrays) {
        for (int c = 0; c < channels; ++c) put<std::uint16_t>(out, static_cast<std::uint16_t>(bitDepth));
        for (int c = 0; c < channels; ++c) put<std::uint16_t>(out, static_cast<std::uint16_t>(sampleFormat));
    }
    const std::size_t start = out.size();
    out.resize(start + dataBytes);
    std::uint8_t* data = out.data() + start;
    parallelFor(0, static_cast<std::size_t>(height), [&](std::size_t y) {
        const std::size_t rowSamples = static_cast<std::size_t>(width) * static_cast<std::size_t>(channels);
        for (std::size_t i = y * rowSamples; i < (y + 1) * rowSamples; ++i) {
            if (bitDepth == 8) {
                data[i] = to8(pixels[i]);
            } else if (bitDepth == 16) {
                const std::uint16_t v = to16(pixels[i]);
                std::memcpy(data + i * 2, &v, 2);
            } else {
                std::memcpy(data + i * 4, &pixels[i], 4);
            }
        }
    }, kRowGrain);
    writeBytes(path, out);
}

// OpenEXR scanline file, ZIP compressed (16 rows a chunk), half or float channels: Y (gray), or
// B G R (and A), the order EXR keeps them in (by name).
void writeExr(const std::string& path, int bitDepth, int width, int height, int channels, const std::vector<float>& pixels) {
    const bool half = bitDepth == 16;
    const std::size_t bytesPer = half ? 2 : 4;
    // The file's channels by name, and which of ours each is.
    std::vector<std::pair<std::string, int>> named;
    if (channels == 1) {
        named = {{"Y", 0}};
    } else {
        named = {{"B", 2}, {"G", 1}, {"R", 0}};
        if (channels == 4) named.insert(named.begin(), {"A", 3});
    }
    std::vector<std::uint8_t> out;
    put<std::uint32_t>(out, 20000630u);  // the magic number
    put<std::uint32_t>(out, 2u);         // version 2, scanlines
    auto attribute = [&](const std::string& name, const std::string& type, const std::vector<std::uint8_t>& value) {
        putString(out, name);
        putString(out, type);
        put<std::int32_t>(out, static_cast<std::int32_t>(value.size()));
        out.insert(out.end(), value.begin(), value.end());
    };
    {
        std::vector<std::uint8_t> list;
        for (const auto& [name, index] : named) {
            putString(list, name);
            put<std::int32_t>(list, half ? 1 : 2);  // HALF, FLOAT
            list.push_back(0);                      // not perceptually linear
            list.push_back(0), list.push_back(0), list.push_back(0);
            put<std::int32_t>(list, 1);
            put<std::int32_t>(list, 1);
        }
        list.push_back(0);
        attribute("channels", "chlist", list);
    }
    attribute("compression", "compression", {3});  // ZIP
    std::vector<std::uint8_t> box;
    put<std::int32_t>(box, 0);
    put<std::int32_t>(box, 0);
    put<std::int32_t>(box, width - 1);
    put<std::int32_t>(box, height - 1);
    attribute("dataWindow", "box2i", box);
    attribute("displayWindow", "box2i", box);
    attribute("lineOrder", "lineOrder", {0});  // increasing y
    std::vector<std::uint8_t> one, center;
    put<float>(one, 1.0f);
    put<float>(center, 0.0f);
    put<float>(center, 0.0f);
    attribute("pixelAspectRatio", "float", one);
    attribute("screenWindowCenter", "v2f", center);
    attribute("screenWindowWidth", "float", one);
    out.push_back(0);  // the header's end

    constexpr int kRows = 16;
    const int chunks = (height + kRows - 1) / kRows;
    const std::size_t tableAt = out.size();
    out.resize(out.size() + static_cast<std::size_t>(chunks) * 8);
    // Each chunk compressed on its own, side by side.
    std::vector<std::vector<std::uint8_t>> blocks(static_cast<std::size_t>(chunks));
    parallelFor(0, static_cast<std::size_t>(chunks), [&](std::size_t chunk) {
        const int y0 = static_cast<int>(chunk) * kRows, y1 = std::min(height, y0 + kRows);
        std::vector<std::uint8_t> raw;
        raw.reserve(static_cast<std::size_t>(y1 - y0) * static_cast<std::size_t>(width) * named.size() * bytesPer);
        for (int y = y0; y < y1; ++y) {
            for (const auto& [name, index] : named) {
                for (int x = 0; x < width; ++x) {
                    const float v = pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) *
                                               static_cast<std::size_t>(channels) + static_cast<std::size_t>(index)];
                    if (half) {
                        put<std::uint16_t>(raw, toHalf(v));
                    } else {
                        put<float>(raw, v);
                    }
                }
            }
        }
        // ZIP: the bytes split into even and odd halves, then each byte as its difference from
        // the one before (plus 128), deflated; stored as they are when that is no smaller.
        std::vector<std::uint8_t> split(raw.size());
        std::size_t even = 0, odd = (raw.size() + 1) / 2;
        for (std::size_t i = 0; i < raw.size(); ++i) split[(i & 1u) ? odd++ : even++] = raw[i];
        for (std::size_t i = split.size(); i-- > 1;) split[i] = static_cast<std::uint8_t>(static_cast<int>(split[i]) - static_cast<int>(split[i - 1]) + 128 + 256);
        std::vector<std::uint8_t> packed = zlibCompress(split);
        std::vector<std::uint8_t>& block = blocks[chunk];
        const std::vector<std::uint8_t>& data = packed.size() < raw.size() ? packed : raw;
        put<std::int32_t>(block, y0);
        put<std::int32_t>(block, static_cast<std::int32_t>(data.size()));
        block.insert(block.end(), data.begin(), data.end());
    }, 1);
    for (int chunk = 0; chunk < chunks; ++chunk) {
        const auto at = static_cast<std::uint64_t>(out.size());
        std::memcpy(out.data() + tableAt + static_cast<std::size_t>(chunk) * 8, &at, 8);
        out.insert(out.end(), blocks[static_cast<std::size_t>(chunk)].begin(), blocks[static_cast<std::size_t>(chunk)].end());
    }
    writeBytes(path, out);
}

}  // namespace

void writeImageFile(const std::string& path, const std::string& formatIn, int bitDepth, int width, int height, int channels,
                    const std::vector<float>& pixels) {
    const std::string format = cardMapFormat(formatIn);
    if (channels != 1 && channels != 3 && channels != 4) throw std::runtime_error("writeImageFile: 1, 3 or 4 channels");
    if (width <= 0 || height <= 0 ||
        pixels.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * static_cast<std::size_t>(channels)) {
        throw std::runtime_error("writeImageFile: pixel count does not match the size");
    }
    const std::vector<int> depths = cardMapBitDepths(format);
    if (std::find(depths.begin(), depths.end(), bitDepth) == depths.end()) {
        throw std::runtime_error("no " + std::to_string(bitDepth) + "-bit " + (format.empty() ? formatIn : format) + " files");
    }
    if (format == "exr") return writeExr(path, bitDepth, width, height, channels, pixels);
    if (format == "tiff") return writeTiff(path, bitDepth, width, height, channels, pixels);
    if (format == "png" && bitDepth == 16) {
        std::vector<std::uint16_t> wide(pixels.size());
        parallelFor(0, pixels.size(), [&](std::size_t i) { wide[i] = to16(pixels[i]); }, 1 << 16);
        return writeCompressedPng16(path, width, height, channels, wide);
    }
    std::vector<std::uint8_t> bytes(pixels.size());
    parallelFor(0, pixels.size(), [&](std::size_t i) { bytes[i] = to8(pixels[i]); }, 1 << 16);
    if (format == "png") return writeCompressedPng(path, width, height, channels, bytes);
    writeTga(path, width, height, channels, bytes);
}

}  // namespace groomlab
