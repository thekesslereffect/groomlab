// SPDX-License-Identifier: Apache-2.0
#include "groomlab/gltf_writer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>

#include <nlohmann/json.hpp>

#include "groomlab/cards.h"
#include "groomlab/export_view.h"
#include "groomlab/png.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// glTF constants.
constexpr int kFloat = 5126;
constexpr int kUnsignedShort = 5123;
constexpr int kUnsignedInt = 5125;
constexpr int kArrayBuffer = 34962;
constexpr int kElementArrayBuffer = 34963;

void check(bool ok, const std::string& what) {
    if (!ok) throw std::invalid_argument("glTF: " + what);
}

bool finite(float v) { return std::isfinite(v); }

// The binary buffer being built: every view starts on a 4-byte boundary.
struct Buffer {
    std::vector<std::uint8_t> bytes;
    json views = json::array();

    int view(const void* data, std::size_t size, int target) {
        while (bytes.size() % 4) bytes.push_back(0);
        const std::size_t offset = bytes.size();
        const auto* p = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), p, p + size);
        json v = {{"buffer", 0}, {"byteOffset", offset}, {"byteLength", size}};
        if (target) v["target"] = target;
        views.push_back(std::move(v));
        return static_cast<int>(views.size()) - 1;
    }
};

const char* alphaModeName(GltfMaterial::AlphaMode mode) {
    switch (mode) {
        case GltfMaterial::AlphaMode::Opaque: return "OPAQUE";
        case GltfMaterial::AlphaMode::Blend: return "BLEND";
        default: return "MASK";
    }
}

json parseExtras(const std::string& text, const std::string& what) {
    if (text.empty()) return json();
    json extras = json::parse(text, nullptr, false);
    check(!extras.is_discarded() && extras.is_object(), what + " extras must be a JSON object");
    return extras;
}

// The document as glTF JSON plus its binary buffer; `images` receives each
// image's bytes when they go outside the buffer (a .gltf), else they go in.
json build(const GltfDocument& doc, Buffer& buffer, bool imagesInBuffer) {
    check(!doc.meshes.empty(), "no meshes");
    // The buffer's size, about (each view padded to 4 bytes), so it grows once.
    std::size_t estimate = 0;
    for (const GltfMesh& mesh : doc.meshes) {
        for (const GltfPrimitive& p : mesh.primitives) {
            estimate += p.positions.size() * sizeof(Vec3f) + p.normals.size() * sizeof(Vec3f) + p.tangents.size() * 16 +
                        p.uv0.size() * 8 + p.uv1.size() * 8 + p.colors.size() * 16 + p.tints.size() * 12 + p.indices.size() * 4 + 8 * 4;
        }
    }
    if (imagesInBuffer) {
        for (const GltfTexture& t : doc.textures) estimate += t.png.size() + 4;
    }
    buffer.bytes.reserve(estimate);
    json accessors = json::array();
    auto floats = [&](const float* data, std::size_t count, int components, const char* type, int target,
                      bool minMax) {
        const int view = buffer.view(data, count * static_cast<std::size_t>(components) * sizeof(float), target);
        json a = {{"bufferView", view}, {"componentType", kFloat}, {"count", count}, {"type", type}};
        if (minMax) {
            std::vector<float> lo(static_cast<std::size_t>(components), std::numeric_limits<float>::max());
            std::vector<float> hi(static_cast<std::size_t>(components), std::numeric_limits<float>::lowest());
            for (std::size_t i = 0; i < count; ++i) {
                for (int c = 0; c < components; ++c) {
                    const float v = data[i * static_cast<std::size_t>(components) + static_cast<std::size_t>(c)];
                    lo[static_cast<std::size_t>(c)] = std::min(lo[static_cast<std::size_t>(c)], v);
                    hi[static_cast<std::size_t>(c)] = std::max(hi[static_cast<std::size_t>(c)], v);
                }
            }
            a["min"] = lo;
            a["max"] = hi;
        }
        accessors.push_back(std::move(a));
        return static_cast<int>(accessors.size()) - 1;
    };

    json meshes = json::array(), nodes = json::array();
    std::vector<int> meshNodes;
    for (const GltfMesh& mesh : doc.meshes) {
        check(!mesh.primitives.empty(), "mesh '" + mesh.name + "' has no primitives");
        json primitives = json::array();
        for (const GltfPrimitive& p : mesh.primitives) {
            const std::size_t n = p.positions.size();
            const std::string where = "mesh '" + mesh.name + "': ";
            check(n > 0, where + "a primitive has no positions");
            check(!p.indices.empty() && p.indices.size() % 3 == 0, where + "indices must be whole triangles");
            check(p.normals.empty() || p.normals.size() == n, where + "normals do not match the positions");
            check(p.tangents.empty() || p.tangents.size() == n, where + "tangents do not match the positions");
            check(p.uv0.empty() || p.uv0.size() == n, where + "TEXCOORD_0 does not match the positions");
            check(p.uv1.empty() || p.uv1.size() == n, where + "TEXCOORD_1 does not match the positions");
            check(p.colors.empty() || p.colors.size() == n, where + "COLOR_0 does not match the positions");
            check(p.tints.empty() || p.tints.size() == n, where + "_TINT does not match the positions");
            check(p.material >= -1 && p.material < static_cast<int>(doc.materials.size()), where + "no such material");
            check(n <= std::numeric_limits<std::uint32_t>::max(), where + "too many vertices");
            for (const std::uint32_t i : p.indices) check(i < n, where + "an index is out of range");
            for (const Vec3f& v : p.positions) check(finite(v.x) && finite(v.y) && finite(v.z), where + "a position is not finite");
            for (const Vec3f& v : p.normals) {
                check(finite(v.x) && finite(v.y) && finite(v.z) && std::abs(v.length() - 1.0f) < 1e-3f,
                      where + "a normal is not a unit vector");
            }
            for (const auto& t : p.tangents) {
                check(finite(t[0]) && finite(t[1]) && finite(t[2]) && (t[3] == 1.0f || t[3] == -1.0f) &&
                          std::abs(std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]) - 1.0f) < 1e-3f,
                      where + "a tangent is not a unit vector with w +1 or -1");
            }
            for (const auto* set : {&p.uv0, &p.uv1}) {
                for (const auto& uv : *set) check(finite(uv[0]) && finite(uv[1]), where + "a UV is not finite");
            }
            for (const auto& c : p.colors) {
                check(finite(c[0]) && finite(c[1]) && finite(c[2]) && finite(c[3]), where + "a color is not finite");
            }
            for (const auto& c : p.tints) check(finite(c[0]) && finite(c[1]) && finite(c[2]), where + "a tint is not finite");

            json attributes;
            attributes["POSITION"] = floats(&p.positions[0].x, n, 3, "VEC3", kArrayBuffer, true);
            if (!p.normals.empty()) attributes["NORMAL"] = floats(&p.normals[0].x, n, 3, "VEC3", kArrayBuffer, false);
            if (!p.tangents.empty()) attributes["TANGENT"] = floats(p.tangents[0].data(), n, 4, "VEC4", kArrayBuffer, false);
            if (!p.uv0.empty()) attributes["TEXCOORD_0"] = floats(p.uv0[0].data(), n, 2, "VEC2", kArrayBuffer, false);
            if (!p.uv1.empty()) attributes["TEXCOORD_1"] = floats(p.uv1[0].data(), n, 2, "VEC2", kArrayBuffer, false);
            if (!p.colors.empty()) attributes["COLOR_0"] = floats(p.colors[0].data(), n, 4, "VEC4", kArrayBuffer, false);
            if (!p.tints.empty()) attributes["_TINT"] = floats(p.tints[0].data(), n, 3, "VEC3", kArrayBuffer, false);

            // Indices: 16-bit while every vertex fits (65535 itself is left to primitive restart).
            int view = 0, componentType = 0;
            if (n <= 65535) {
                std::vector<std::uint16_t> shorts(p.indices.begin(), p.indices.end());
                view = buffer.view(shorts.data(), shorts.size() * sizeof(std::uint16_t), kElementArrayBuffer);
                componentType = kUnsignedShort;
            } else {
                view = buffer.view(p.indices.data(), p.indices.size() * sizeof(std::uint32_t), kElementArrayBuffer);
                componentType = kUnsignedInt;
            }
            accessors.push_back({{"bufferView", view}, {"componentType", componentType}, {"count", p.indices.size()},
                                 {"type", "SCALAR"}});
            json primitive = {{"attributes", attributes}, {"indices", static_cast<int>(accessors.size()) - 1},
                              {"mode", 4}};
            if (p.material >= 0) primitive["material"] = p.material;
            primitives.push_back(std::move(primitive));
        }
        meshes.push_back({{"name", mesh.name}, {"primitives", primitives}});
        nodes.push_back({{"name", mesh.name}, {"mesh", static_cast<int>(meshes.size()) - 1}});
        meshNodes.push_back(static_cast<int>(nodes.size()) - 1);
    }
    std::vector<int> sceneNodes = meshNodes;
    if (!doc.rootName.empty()) {
        nodes.push_back({{"name", doc.rootName}, {"children", meshNodes}});
        sceneNodes = {static_cast<int>(nodes.size()) - 1};
    }

    auto textureRef = [&](int index, const std::string& what) {
        check(index < static_cast<int>(doc.textures.size()), what + " names a texture that does not exist");
        return json{{"index", index}, {"texCoord", 0}};
    };
    json materials = json::array();
    for (const GltfMaterial& m : doc.materials) {
        check(finite(m.metallic) && finite(m.roughness) && finite(m.alphaCutoff), "material '" + m.name + "' is not finite");
        json pbr = {{"baseColorFactor", m.baseColorFactor},
                    {"metallicFactor", std::clamp(m.metallic, 0.0f, 1.0f)},
                    {"roughnessFactor", std::clamp(m.roughness, 0.0f, 1.0f)}};
        if (m.baseColorTexture >= 0) pbr["baseColorTexture"] = textureRef(m.baseColorTexture, m.name);
        if (m.metallicRoughnessTexture >= 0) pbr["metallicRoughnessTexture"] = textureRef(m.metallicRoughnessTexture, m.name);
        json material = {{"name", m.name}, {"pbrMetallicRoughness", pbr}, {"alphaMode", alphaModeName(m.alphaMode)},
                         {"doubleSided", m.doubleSided}};
        if (m.normalTexture >= 0) material["normalTexture"] = textureRef(m.normalTexture, m.name);
        if (m.alphaMode == GltfMaterial::AlphaMode::Mask) material["alphaCutoff"] = std::clamp(m.alphaCutoff, 0.0f, 1.0f);
        if (m.occlusionTexture >= 0) {
            json occlusion = textureRef(m.occlusionTexture, m.name);
            occlusion["strength"] = std::clamp(m.occlusionStrength, 0.0f, 1.0f);
            material["occlusionTexture"] = occlusion;
        }
        if (json extras = parseExtras(m.extrasJson, "material '" + m.name + "'"); !extras.is_null()) material["extras"] = extras;
        materials.push_back(std::move(material));
    }

    json images = json::array(), textures = json::array();
    for (std::size_t i = 0; i < doc.textures.size(); ++i) {
        const GltfTexture& t = doc.textures[i];
        static const std::uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        check(t.png.size() > 8 && std::memcmp(t.png.data(), kSignature, 8) == 0, "texture '" + t.name + "' is not a PNG");
        json image = {{"name", t.name}};
        if (imagesInBuffer) {
            image["bufferView"] = buffer.view(t.png.data(), t.png.size(), 0);
            image["mimeType"] = "image/png";
        }
        images.push_back(std::move(image));
        textures.push_back({{"sampler", 0}, {"source", static_cast<int>(i)}, {"name", t.name}});
    }

    json asset = {{"version", "2.0"}, {"generator", doc.generator}};
    if (json extras = parseExtras(doc.extrasJson, "the asset's"); !extras.is_null()) asset["extras"] = extras;
    json gltf = {{"asset", asset},
                 {"scene", 0},
                 {"scenes", json::array({{{"nodes", sceneNodes}}})},
                 {"nodes", nodes},
                 {"meshes", meshes},
                 {"accessors", accessors}};
    if (!materials.empty()) gltf["materials"] = materials;
    if (!textures.empty()) {
        gltf["textures"] = textures;
        gltf["images"] = images;
        // Linear filtering with mip maps; clamped, as atlas cells should never wrap.
        gltf["samplers"] = json::array({{{"magFilter", 9729}, {"minFilter", 9987}, {"wrapS", 33071}, {"wrapT", 33071}}});
    }
    while (buffer.bytes.size() % 4) buffer.bytes.push_back(0);
    gltf["bufferViews"] = buffer.views;
    return gltf;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) out.push_back(static_cast<std::uint8_t>(v >> (8 * k)));  // little endian
}

// Writes the pieces one after another (beside the target, then moved over it).
void writeFile(const fs::path& target, const std::vector<std::pair<const void*, std::size_t>>& pieces) {
    const fs::path tmp = tempPathFor(target);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("can't write " + tmp.u8string());
        for (const auto& [data, size] : pieces) out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!out) {
            out.close();
            std::error_code ec;
            fs::remove(tmp, ec);
            throw std::runtime_error("can't write " + tmp.u8string());
        }
    }
    replaceFile(tmp, target);
}

void writeFile(const fs::path& target, const std::vector<std::uint8_t>& bytes) { writeFile(target, {{bytes.data(), bytes.size()}}); }

std::string fileSafe(const std::string& name) {
    std::string out = name;
    for (char& c : out) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '-' || c == '.';
        if (!ok) c = '_';
    }
    return out;
}

// Scene units and axes to glTF's: meters (times `scale`), Y up.
Vec3f toGltf(const Vec3f& p, const CardGltfSettings& s) {
    const Vec3f q = s.zUp ? Vec3f(p.x, p.z, -p.y) : p;
    return q * s.scale;
}

Vec3f directionToGltf(const Vec3f& d, const CardGltfSettings& s) {
    const Vec3f n = (s.zUp ? Vec3f(d.x, d.z, -d.y) : d).normalized();
    return n.length() > 0.5f ? n : Vec3f(0.0f, 1.0f, 0.0f);  // a degenerate normal: up, never zero
}

// Triangles from quads, each wound so its face agrees with its vertices' normals.
std::vector<std::uint32_t> trianglesFacingNormals(const std::vector<Vec3f>& positions, const std::vector<Vec3f>& normals,
                                                  const std::vector<std::uint32_t>& triangles) {
    std::vector<std::uint32_t> out = triangles;
    for (std::size_t t = 0; t + 2 < out.size(); t += 3) {
        const Vec3f& a = positions[out[t]];
        const Vec3f face = (positions[out[t + 1]] - a).cross(positions[out[t + 2]] - a);
        const Vec3f n = normals[out[t]] + normals[out[t + 1]] + normals[out[t + 2]];
        if (face.dot(n) < 0.0f) std::swap(out[t + 1], out[t + 2]);
    }
    return out;
}

std::string lookExtras(const CardGltfSettings& s, const char* kind) {
    json extras = {{"groomlab", {{"kind", kind},
                                 {"hairLook", {{"roughness", s.roughness}, {"specular", s.specular}}},
                                 {"channels",
                                  {{"baseColor", "RGB: the strands' color (sRGB); A: coverage (opacity, cut at alphaCutoff)"},
                                   {"orm", "R: ambient occlusion (1 open); G, B: 1 (the roughness and metallic factors apply as they are)"},
                                   {"tint", "_TINT (vertex attribute, linear RGB, 0..2, 1 none): each card's own strands' color over "
                                            "its shared texture's; multiply baseColor's RGB by it"}}}}}};
    return extras.dump();
}

std::vector<std::uint8_t> interleave(int size, const std::vector<std::uint8_t>& rgb, const std::vector<std::uint8_t>& alpha) {
    const std::size_t n = static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
    check(rgb.size() == 3 * n && alpha.size() == n, "the baked maps do not match their size");
    std::vector<std::uint8_t> out(4 * n);
    for (std::size_t i = 0; i < n; ++i) {
        out[4 * i] = rgb[3 * i];
        out[4 * i + 1] = rgb[3 * i + 1];
        out[4 * i + 2] = rgb[3 * i + 2];
        out[4 * i + 3] = alpha[i];
    }
    return out;
}

}  // namespace

namespace {

// A .glb in its pieces: the header and JSON chunk, then the binary chunk's header and the buffer.
struct GlbParts {
    std::vector<std::uint8_t> head;  // the file's header and the JSON chunk
    std::vector<std::uint8_t> binHeader;
    Buffer buffer;
};

GlbParts glbParts(const GltfDocument& document) {
    GlbParts g;
    json gltf = build(document, g.buffer, true);
    if (!g.buffer.bytes.empty()) gltf["buffers"] = json::array({{{"byteLength", g.buffer.bytes.size()}}});
    std::string text = gltf.dump();
    while (text.size() % 4) text.push_back(' ');  // the JSON chunk pads with spaces
    const std::size_t total = 12 + 8 + text.size() + (g.buffer.bytes.empty() ? 0 : 8 + g.buffer.bytes.size());
    check(total <= std::numeric_limits<std::uint32_t>::max(), "the file would be over 4 GB");
    g.head.reserve(20 + text.size());
    put32(g.head, 0x46546C67u);  // "glTF"
    put32(g.head, 2);
    put32(g.head, static_cast<std::uint32_t>(total));
    put32(g.head, static_cast<std::uint32_t>(text.size()));
    put32(g.head, 0x4E4F534Au);  // "JSON"
    g.head.insert(g.head.end(), text.begin(), text.end());
    if (!g.buffer.bytes.empty()) {
        put32(g.binHeader, static_cast<std::uint32_t>(g.buffer.bytes.size()));
        put32(g.binHeader, 0x004E4942u);  // "BIN\0"
    }
    return g;
}

}  // namespace

std::vector<std::uint8_t> encodeGlb(const GltfDocument& document) {
    GlbParts g = glbParts(document);
    std::vector<std::uint8_t> out = std::move(g.head);
    out.reserve(out.size() + g.binHeader.size() + g.buffer.bytes.size());
    out.insert(out.end(), g.binHeader.begin(), g.binHeader.end());
    out.insert(out.end(), g.buffer.bytes.begin(), g.buffer.bytes.end());
    return out;
}

std::vector<std::string> writeGltf(const std::string& path, const GltfDocument& document, bool binary) {
    const fs::path target = fs::u8path(path);
    if (binary) {  // the pieces straight to the file, not copied into one array first
        const GlbParts g = glbParts(document);
        writeFile(target, {{g.head.data(), g.head.size()},
                           {g.binHeader.data(), g.binHeader.size()},
                           {g.buffer.bytes.data(), g.buffer.bytes.size()}});
        return {path};
    }
    Buffer buffer;
    json gltf = build(document, buffer, false);
    const std::string stem = target.stem().u8string();
    const fs::path folder = target.parent_path();
    std::vector<std::string> written{path};
    std::vector<std::pair<fs::path, const std::vector<std::uint8_t>*>> files;
    if (!buffer.bytes.empty()) {
        const std::string bin = stem + ".bin";
        gltf["buffers"] = json::array({{{"byteLength", buffer.bytes.size()}, {"uri", bin}}});
        files.emplace_back(folder / fs::u8path(bin), &buffer.bytes);
    }
    for (std::size_t i = 0; i < document.textures.size(); ++i) {
        const std::string image = stem + "_" + fileSafe(document.textures[i].name) + ".png";
        gltf["images"][i]["uri"] = image;
        files.emplace_back(folder / fs::u8path(image), &document.textures[i].png);
    }
    const std::string text = gltf.dump(2);
    for (const auto& [file, bytes] : files) {
        writeFile(file, *bytes);
        written.push_back(file.u8string());
    }
    writeFile(target, std::vector<std::uint8_t>(text.begin(), text.end()));
    return written;
}

std::vector<std::array<float, 4>> meshTangents(const std::vector<Vec3f>& positions, const std::vector<Vec3f>& normals,
                                               const std::vector<std::array<float, 2>>& uvs,
                                               const std::vector<std::uint32_t>& indices) {
    const std::size_t n = positions.size();
    check(normals.size() == n && uvs.size() == n, "tangents need a normal and a UV per position");
    std::vector<Vec3f> du(n), dv(n);
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        const std::uint32_t i0 = indices[t], i1 = indices[t + 1], i2 = indices[t + 2];
        check(i0 < n && i1 < n && i2 < n, "an index is out of range");
        const Vec3f e1 = positions[i1] - positions[i0], e2 = positions[i2] - positions[i0];
        const float s1 = uvs[i1][0] - uvs[i0][0], t1 = uvs[i1][1] - uvs[i0][1];
        const float s2 = uvs[i2][0] - uvs[i0][0], t2 = uvs[i2][1] - uvs[i0][1];
        const float det = s1 * t2 - s2 * t1;
        if (std::abs(det) < 1e-20f) continue;
        const float r = 1.0f / det;
        const Vec3f sdir = (e1 * t2 - e2 * t1) * r;  // dP/du
        const Vec3f tdir = (e2 * s1 - e1 * s2) * r;  // dP/dv
        for (const std::uint32_t i : {i0, i1, i2}) {
            du[i] += sdir;
            dv[i] += tdir;
        }
    }
    std::vector<std::array<float, 4>> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3f& nn = normals[i];
        Vec3f t = du[i] - nn * nn.dot(du[i]);
        if (t.length() < 1e-12f) {
            // No UV gradient here: any direction perpendicular to the normal.
            t = std::abs(nn.x) < 0.9f ? Vec3f(1, 0, 0).cross(nn) : Vec3f(0, 1, 0).cross(nn);
        }
        t = t.normalized();
        const float w = nn.cross(t).dot(dv[i]) < 0.0f ? -1.0f : 1.0f;
        out[i] = {t.x, t.y, t.z, w};
    }
    return out;
}

int addCardsToGltf(GltfDocument& doc, const CardMesh& cards, const CardTextures& textures,
                   const CardGltfSettings& settings) {
    check(!cards.quads.empty() && cards.positions.size() == cards.normals.size() &&
              cards.u.size() == cards.positions.size() && cards.v.size() == cards.positions.size(),
          "the cards are not laid out");
    check(textures.size > 0, "the cards are not baked");
    const std::size_t n = cards.positions.size();
    GltfPrimitive p;
    p.positions.reserve(n);
    p.normals.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        p.positions.push_back(toGltf(cards.positions[i], settings));
        p.normals.push_back(directionToGltf(cards.normals[i], settings));
    }
    std::vector<std::array<float, 2>> mayaUVs(n);
    p.uv0.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        mayaUVs[i] = {cards.u[i], cards.v[i]};
        p.uv0[i] = {cards.u[i], 1.0f - cards.v[i]};
    }
    std::vector<std::uint32_t> triangles;
    triangles.reserve(cards.quads.size() / 4 * 6);
    for (std::size_t q = 0; q + 3 < cards.quads.size(); q += 4) {
        const std::uint32_t a = cards.quads[q], b = cards.quads[q + 1], c = cards.quads[q + 2], d = cards.quads[q + 3];
        triangles.insert(triangles.end(), {a, b, c, a, c, d});
    }
    p.indices = trianglesFacingNormals(p.positions, p.normals, triangles);
    if (settings.tangents) p.tangents = meshTangents(p.positions, p.normals, mayaUVs, p.indices);
    if (cards.tints.size() == n) p.tints = cards.tints;  // each card's own color over its shared texture's
    if (settings.rootToTip) {
        // Across 0..1 by column and root (0) to tip (1), per card, whatever cell or mirroring it
        // has: a feather card's spine (its middle column, wherever the rachis sits) is 0.5.
        p.uv1.assign(n, {0.0f, 0.0f});
        for (const CardMesh::Card& card : cards.cards) {
            const int across = std::max(card.across, 1);
            for (int k = 0; k < card.stations; ++k) {
                const float along = card.stations > 1 ? static_cast<float>(k) / static_cast<float>(card.stations - 1) : 0.0f;
                for (int e = 0; e <= across; ++e) {
                    const std::size_t i = card.firstVertex + static_cast<std::size_t>(k) * static_cast<std::size_t>(across + 1) +
                                          static_cast<std::size_t>(e);
                    if (i < n) p.uv1[i] = {static_cast<float>(e) / static_cast<float>(across), along};
                }
            }
        }
    }

    const std::vector<std::uint8_t> rgba = interleave(textures.size, textures.color, textures.coverage);
    std::vector<std::uint8_t> orm(3 * textures.occlusion.size());
    for (std::size_t i = 0; i < textures.occlusion.size(); ++i) {
        orm[3 * i] = textures.occlusion[i];
        orm[3 * i + 1] = 255;
        orm[3 * i + 2] = 255;
    }
    // The normal map with glTF's green (OpenGL's), when the bake has one.
    const bool hasNormal = textures.normal.size() == textures.tangent.size() && !textures.normal.empty();
    const std::vector<std::uint8_t> normal = hasNormal ? cardNormalOpenGl(textures) : std::vector<std::uint8_t>();
    // The maps encoded side by side.
    std::vector<PngImage> images = {{textures.size, textures.size, 4, &rgba}, {textures.size, textures.size, 3, &orm}};
    if (hasNormal) images.push_back({textures.size, textures.size, 3, &normal});
    std::vector<std::vector<std::uint8_t>> pngs = encodePngs(images);
    const int baseColor = static_cast<int>(doc.textures.size());
    doc.textures.push_back({settings.name + "_BaseColor", std::move(pngs[0])});
    const int ormTexture = static_cast<int>(doc.textures.size());
    doc.textures.push_back({settings.name + "_ORM", std::move(pngs[1])});
    int normalTexture = -1;
    if (hasNormal) {
        normalTexture = static_cast<int>(doc.textures.size());
        doc.textures.push_back({settings.name + "_Normal", std::move(pngs[2])});
    }

    GltfMaterial m;
    m.name = settings.name + "_Mat";
    m.baseColorTexture = baseColor;
    m.metallicRoughnessTexture = ormTexture;
    m.occlusionTexture = ormTexture;
    m.normalTexture = normalTexture;
    m.metallic = 0.0f;
    m.roughness = settings.roughness;
    m.alphaMode = settings.alphaMode;
    m.alphaCutoff = settings.alphaCutoff;
    m.doubleSided = true;  // cards are seen from both sides
    m.extrasJson = lookExtras(settings, "cards");
    p.material = static_cast<int>(doc.materials.size());
    doc.materials.push_back(std::move(m));

    GltfMesh mesh;
    mesh.name = settings.name;
    mesh.primitives.push_back(std::move(p));
    doc.meshes.push_back(std::move(mesh));
    return static_cast<int>(doc.meshes.size()) - 1;
}

int addScalpCapToGltf(GltfDocument& doc, const ScalpMesh& scalp, const ScalpTextures& textures, float lift,
                      const CardGltfSettings& settings) {
    check(scalp.hasUVs(), "the scalp has no UVs");
    check(textures.size > 0, "the scalp is not baked");
    const auto& points = scalp.points();
    const auto& triangles = scalp.triangles();
    const auto& uvs = scalp.cornerUVs();
    // Area-weighted vertex normals, then the vertices split where the UVs do.
    std::vector<Vec3f> vertexNormals(points.size());
    for (const auto& t : triangles) {
        const Vec3f face = (points[static_cast<std::size_t>(t[1])] - points[static_cast<std::size_t>(t[0])])
                               .cross(points[static_cast<std::size_t>(t[2])] - points[static_cast<std::size_t>(t[0])]);
        for (const std::int32_t i : t) vertexNormals[static_cast<std::size_t>(i)] += face;
    }
    GltfPrimitive p;
    std::vector<std::array<float, 2>> mayaUVs;
    std::map<std::tuple<std::int32_t, float, float>, std::uint32_t> made;
    for (std::size_t t = 0; t < triangles.size(); ++t) {
        for (int c = 0; c < 3; ++c) {
            const std::int32_t point = triangles[t][static_cast<std::size_t>(c)];
            const ScalpMesh::UV uv = uvs[3 * t + static_cast<std::size_t>(c)];
            const auto key = std::make_tuple(point, uv[0], uv[1]);
            auto it = made.find(key);
            if (it == made.end()) {
                const Vec3f normal = vertexNormals[static_cast<std::size_t>(point)].normalized();
                const Vec3f n = normal.length() > 0.5f ? normal : Vec3f(0, 1, 0);
                p.positions.push_back(toGltf(points[static_cast<std::size_t>(point)] + n * lift, settings));
                p.normals.push_back(directionToGltf(n, settings));
                p.uv0.push_back({uv[0], 1.0f - uv[1]});
                mayaUVs.push_back(uv);
                it = made.emplace(key, static_cast<std::uint32_t>(p.positions.size() - 1)).first;
            }
            p.indices.push_back(it->second);
        }
    }
    p.indices = trianglesFacingNormals(p.positions, p.normals, p.indices);
    if (settings.tangents) p.tangents = meshTangents(p.positions, p.normals, mayaUVs, p.indices);

    const int baseColor = static_cast<int>(doc.textures.size());
    doc.textures.push_back({settings.name + "_BaseColor",
                            encodePng(textures.size, textures.size, 4, interleave(textures.size, textures.color, textures.coverage))});
    GltfMaterial m;
    m.name = settings.name + "_Mat";
    m.baseColorTexture = baseColor;
    m.roughness = settings.roughness;
    // The cap blends: its coverage fades the hair color into the skin under it.
    m.alphaMode = settings.alphaMode == GltfMaterial::AlphaMode::Opaque ? GltfMaterial::AlphaMode::Opaque
                                                                        : GltfMaterial::AlphaMode::Blend;
    m.doubleSided = false;
    m.extrasJson = json{{"groomlab", {{"kind", "scalpCap"},
                                      {"channels", {{"baseColor", "RGB: the hair's color on the scalp (sRGB); A: how much hair covers the skin"}}}}}}
                       .dump();
    p.material = static_cast<int>(doc.materials.size());
    doc.materials.push_back(std::move(m));

    GltfMesh mesh;
    mesh.name = settings.name;
    mesh.primitives.push_back(std::move(p));
    doc.meshes.push_back(std::move(mesh));
    return static_cast<int>(doc.meshes.size()) - 1;
}

int scalpCapResolution(int capResolution, int resolution) {
    if (capResolution > 0) return std::clamp(capResolution, 16, 16384);
    return resolution > 0 ? resolution : 2048;
}

float scalpCapLift(const ScalpMesh& scalp) {
    const auto& points = scalp.points();
    if (points.empty()) return 0.0f;
    Vec3f lo = points.front(), hi = lo;
    for (const Vec3f& p : points) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    return 0.004f * std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
}

int addScalpCapToGltf(GltfDocument& doc, const StrandSet& strands, const ScalpMesh& scalp, int capResolution, int resolution,
                      const CardGltfSettings& settings) {
    ScalpBakeSettings bake;
    bake.size = scalpCapResolution(capResolution, resolution);
    const ScalpTextures textures = bakeScalpTextures(strands, scalp, bake);
    CardGltfSettings cap = settings;
    cap.rootToTip = false;
    return addScalpCapToGltf(doc, scalp, textures, scalpCapLift(scalp), cap);
}

}  // namespace groomlab
