// SPDX-License-Identifier: Apache-2.0
#include "groomlab/fbx_writer.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <tuple>

namespace groomlab {

namespace {

namespace fs = std::filesystem;

// ---- Nodes and properties as FBX 7.4 binary encodes them ------------------------------------------

struct Prop {
    char type = 'I';
    std::string bytes;  // the payload after the type code
};

struct Node {
    std::string name;
    std::vector<Prop> props;
    std::vector<Node> children;

    Node& add(const std::string& n) {
        children.push_back(Node{n, {}, {}});
        return children.back();
    }
};

template <class T>
void raw(std::string& out, T v) {
    char b[sizeof(T)];
    std::memcpy(b, &v, sizeof(T));  // (little endian: every platform GroomLab builds for)
    out.append(b, sizeof(T));
}

Prop pI(std::int32_t v) {
    Prop p{'I', {}};
    raw(p.bytes, v);
    return p;
}
Prop pL(std::int64_t v) {
    Prop p{'L', {}};
    raw(p.bytes, v);
    return p;
}
Prop pD(double v) {
    Prop p{'D', {}};
    raw(p.bytes, v);
    return p;
}
Prop pC(bool v) {
    Prop p{'C', {}};
    p.bytes.push_back(v ? 1 : 0);
    return p;
}
Prop pS(const std::string& s) {
    Prop p{'S', {}};
    raw(p.bytes, static_cast<std::uint32_t>(s.size()));
    p.bytes += s;
    return p;
}
Prop pR(const std::string& s) {
    Prop p{'R', {}};
    raw(p.bytes, static_cast<std::uint32_t>(s.size()));
    p.bytes += s;
    return p;
}
Prop pAi(const std::vector<std::int32_t>& v) {
    Prop p{'i', {}};
    raw(p.bytes, static_cast<std::uint32_t>(v.size()));
    raw(p.bytes, static_cast<std::uint32_t>(0));  // not compressed
    raw(p.bytes, static_cast<std::uint32_t>(v.size() * 4));
    if (!v.empty()) p.bytes.append(reinterpret_cast<const char*>(v.data()), v.size() * 4);
    return p;
}
Prop pAd(const std::vector<double>& v) {
    Prop p{'d', {}};
    raw(p.bytes, static_cast<std::uint32_t>(v.size()));
    raw(p.bytes, static_cast<std::uint32_t>(0));
    raw(p.bytes, static_cast<std::uint32_t>(v.size() * 8));
    if (!v.empty()) p.bytes.append(reinterpret_cast<const char*>(v.data()), v.size() * 8);
    return p;
}

// An object's name as FBX stores it: "name\0\1Class".
std::string named(const std::string& name, const std::string& cls) { return name + std::string("\0\1", 2) + cls; }

// Properties70's P: name, type, label, flags, values.
void P(Node& props, const std::string& name, const std::string& type, const std::string& label, const std::string& flags,
       std::vector<Prop> values) {
    Node& p = props.add("P");
    p.props = {pS(name), pS(type), pS(label), pS(flags)};
    for (Prop& v : values) p.props.push_back(std::move(v));
}

void leaf(Node& parent, const std::string& name, Prop value) { parent.add(name).props.push_back(std::move(value)); }

constexpr std::size_t kSentinel = 13;  // a null record (FBX before 7.5: three 32-bit words and a byte)

void encode(std::string& out, const Node& n, bool last) {
    const std::size_t start = out.size();
    raw(out, static_cast<std::uint32_t>(0));  // end offset: patched below
    raw(out, static_cast<std::uint32_t>(n.props.size()));
    raw(out, static_cast<std::uint32_t>(0));  // property bytes: patched below
    out.push_back(static_cast<char>(n.name.size()));
    out += n.name;
    const std::size_t propsStart = out.size();
    for (const Prop& p : n.props) {
        out.push_back(p.type);
        out += p.bytes;
    }
    const auto propsLength = static_cast<std::uint32_t>(out.size() - propsStart);
    std::memcpy(&out[start + 8], &propsLength, 4);
    if (!n.children.empty()) {
        for (std::size_t i = 0; i < n.children.size(); ++i) encode(out, n.children[i], i + 1 == n.children.size());
        out.append(kSentinel, '\0');
    } else if (n.props.empty() && !last) {
        out.append(kSentinel, '\0');  // (as Blender's and the SDK's files have it)
    }
    const auto end = static_cast<std::uint32_t>(out.size());
    std::memcpy(&out[start], &end, 4);
}

// The fixed values FBX files carry for their id and time (as Blender's exporter writes them, which
// every FBX reader accepts with the footer below).
const char kFileId[] = "\x28\xb3\x2a\xeb\xb6\x24\xcc\xc2\xbf\xc8\xb0\x2a\xa9\x2b\xfc\xf1";
const char kFootId[] = "\xfa\xbc\xab\x09\xd0\xc8\xd4\x66\xb1\x76\xfb\x83\x1c\xf7\x26\x7e";
const char kFootMagic[] = "\xf8\x5a\x8c\x6a\xde\xf5\xd9\x7e\xec\xe9\x0c\xe3\x75\x8f\x29\x0b";
constexpr std::uint32_t kVersion = 7400;

void check(const FbxMesh& m) {
    std::size_t corners = 0;
    for (const int c : m.faceCounts) {
        if (c < 3) throw std::invalid_argument("fbx " + m.name + ": a polygon of fewer than 3 corners");
        corners += static_cast<std::size_t>(c);
    }
    if (corners != m.faceIndices.size()) throw std::invalid_argument("fbx " + m.name + ": the face counts and indices disagree");
    for (const int i : m.faceIndices) {
        if (i < 0 || static_cast<std::size_t>(i) >= m.points.size()) throw std::invalid_argument("fbx " + m.name + ": a corner names no point");
    }
    if (!m.uvs.empty() && m.uvs.size() != 2 * corners) throw std::invalid_argument("fbx " + m.name + ": its uvs are not 2 per corner");
    if (!m.normals.empty() && m.normals.size() != corners) throw std::invalid_argument("fbx " + m.name + ": its normals are not one per corner");
    for (const auto& [name, uv] : m.uvSets) {
        if (uv.size() != 2 * corners) throw std::invalid_argument("fbx " + m.name + ": uv set " + name + " is not 2 per corner");
    }
    for (const auto& [name, c] : m.colors) {
        if (c.size() != 4 * corners) throw std::invalid_argument("fbx " + m.name + ": color set " + name + " is not 4 per corner");
    }
}

std::vector<std::int32_t> sequence(std::size_t n) {
    std::vector<std::int32_t> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<std::int32_t>(i);
    return out;
}

}  // namespace

void writeFbx(const std::string& path, const std::vector<FbxMesh>& meshes, const FbxSettings& settings) {
    for (const FbxMesh& m : meshes) check(m);
    const double scale = settings.meters ? 0.01 : 1.0;
    const double unit = settings.meters ? 100.0 : 1.0;  // centimeters per unit

    Node root;
    // Header: versions, the fixed creation time the footer goes with, the creator.
    Node& header = root.add("FBXHeaderExtension");
    leaf(header, "FBXHeaderVersion", pI(1003));
    leaf(header, "FBXVersion", pI(static_cast<std::int32_t>(kVersion)));
    leaf(header, "EncryptionType", pI(0));
    Node& stamp = header.add("CreationTimeStamp");
    for (const auto& [k, v] : std::vector<std::pair<const char*, int>>{{"Version", 1000}, {"Year", 1970}, {"Month", 1}, {"Day", 1},
                                                                     {"Hour", 10}, {"Minute", 0}, {"Second", 0}, {"Millisecond", 0}}) {
        leaf(stamp, k, pI(v));
    }
    leaf(header, "Creator", pS(settings.creator));
    leaf(root, "FileId", pR(std::string(kFileId, 16)));
    leaf(root, "CreationTime", pS("1970-01-01 10:00:00:000"));
    leaf(root, "Creator", pS(settings.creator));

    // Y up, Z front, X right (the groom's own frame), in centimeters or meters.
    Node& global = root.add("GlobalSettings");
    leaf(global, "Version", pI(1000));
    Node& gp = global.add("Properties70");
    P(gp, "UpAxis", "int", "Integer", "", {pI(1)});
    P(gp, "UpAxisSign", "int", "Integer", "", {pI(1)});
    P(gp, "FrontAxis", "int", "Integer", "", {pI(2)});
    P(gp, "FrontAxisSign", "int", "Integer", "", {pI(1)});
    P(gp, "CoordAxis", "int", "Integer", "", {pI(0)});
    P(gp, "CoordAxisSign", "int", "Integer", "", {pI(1)});
    P(gp, "OriginalUpAxis", "int", "Integer", "", {pI(1)});
    P(gp, "OriginalUpAxisSign", "int", "Integer", "", {pI(1)});
    P(gp, "UnitScaleFactor", "double", "Number", "", {pD(unit)});
    P(gp, "OriginalUnitScaleFactor", "double", "Number", "", {pD(unit)});
    P(gp, "AmbientColor", "ColorRGB", "Color", "", {pD(0.0), pD(0.0), pD(0.0)});
    P(gp, "DefaultCamera", "KString", "", "", {pS("Producer Perspective")});
    P(gp, "TimeMode", "enum", "", "", {pI(11)});
    P(gp, "TimeSpanStart", "KTime", "Time", "", {pL(0)});
    P(gp, "TimeSpanStop", "KTime", "Time", "", {pL(46186158000LL)});
    P(gp, "CustomFrameRate", "double", "Number", "", {pD(-1.0)});

    std::int64_t next = 1000000;
    const std::int64_t documentId = next++;
    Node& documents = root.add("Documents");
    leaf(documents, "Count", pI(1));
    Node& document = documents.add("Document");
    document.props = {pL(documentId), pS("Scene"), pS("Scene")};
    Node& dp = document.add("Properties70");
    P(dp, "SourceObject", "object", "", "", {});
    P(dp, "ActiveAnimStackName", "KString", "", "", {pS("")});
    leaf(document, "RootNode", pL(0));
    root.add("References");

    // Objects and how they connect.
    Node objects;
    objects.name = "Objects";
    std::vector<std::tuple<std::string, std::int64_t, std::int64_t, std::string>> links;  // (kind, child, parent, property)
    int materials = 0, textures = 0;
    for (const FbxMesh& m : meshes) {
        const std::int64_t geometryId = next++, modelId = next++;
        // The geometry: its points, polygons (each polygon's last index negated: ~i), and per corner
        // its normals, uv sets and color sets, each a layer element of the layer of its index.
        Node& g = objects.add("Geometry");
        g.props = {pL(geometryId), pS(named(m.name, "Geometry")), pS("Mesh")};
        g.add("Properties70");
        leaf(g, "GeometryVersion", pI(124));
        std::vector<double> vertices;
        vertices.reserve(m.points.size() * 3);
        for (const Vec3f& p : m.points) vertices.insert(vertices.end(), {p.x * scale, p.y * scale, p.z * scale});
        leaf(g, "Vertices", pAd(vertices));
        std::vector<std::int32_t> polygons;
        polygons.reserve(m.faceIndices.size());
        std::size_t at = 0;
        for (const int c : m.faceCounts) {
            for (int k = 0; k < c; ++k, ++at) polygons.push_back(k + 1 == c ? ~m.faceIndices[at] : m.faceIndices[at]);
        }
        leaf(g, "PolygonVertexIndex", pAi(polygons));
        const std::size_t corners = m.faceIndices.size();
        auto element = [&](const std::string& kind, int index, const std::string& name, const std::string& mapping,
                           const std::string& reference) -> Node& {
            Node& e = g.add(kind);
            e.props = {pI(index)};
            leaf(e, "Version", pI(101));
            leaf(e, "Name", pS(name));
            leaf(e, "MappingInformationType", pS(mapping));
            leaf(e, "ReferenceInformationType", pS(reference));
            return e;
        };
        if (!m.normals.empty()) {
            Node& e = element("LayerElementNormal", 0, "", "ByPolygonVertex", "Direct");
            std::vector<double> n;
            n.reserve(corners * 3);
            for (const Vec3f& v : m.normals) n.insert(n.end(), {v.x, v.y, v.z});
            leaf(e, "Normals", pAd(n));
        }
        std::vector<std::pair<std::string, const std::vector<float>*>> uvs;
        if (!m.uvs.empty()) uvs.emplace_back("UVMap", &m.uvs);
        for (const auto& [name, uv] : m.uvSets) uvs.emplace_back(name, &uv);
        for (std::size_t k = 0; k < uvs.size(); ++k) {
            Node& e = element("LayerElementUV", static_cast<int>(k), uvs[k].first, "ByPolygonVertex", "IndexToDirect");
            leaf(e, "UV", pAd(std::vector<double>(uvs[k].second->begin(), uvs[k].second->end())));
            leaf(e, "UVIndex", pAi(sequence(corners)));
        }
        for (std::size_t k = 0; k < m.colors.size(); ++k) {
            Node& e = element("LayerElementColor", static_cast<int>(k), m.colors[k].first, "ByPolygonVertex", "IndexToDirect");
            leaf(e, "Colors", pAd(std::vector<double>(m.colors[k].second.begin(), m.colors[k].second.end())));
            leaf(e, "ColorIndex", pAi(sequence(corners)));
        }
        const bool material = !m.material.empty();
        if (material) {
            Node& e = element("LayerElementMaterial", 0, "", "AllSame", "IndexToDirect");
            leaf(e, "Materials", pAi({0}));
        }
        // The layers: layer k holds the k-th uv set and color set (layer 0 the normals and material too).
        const std::size_t layers = std::max<std::size_t>({std::size_t(1), uvs.size(), m.colors.size()});
        for (std::size_t k = 0; k < layers; ++k) {
            Node& layer = g.add("Layer");
            layer.props = {pI(static_cast<std::int32_t>(k))};
            leaf(layer, "Version", pI(100));
            auto use = [&](const std::string& type) {
                Node& le = layer.add("LayerElement");
                leaf(le, "Type", pS(type));
                leaf(le, "TypedIndex", pI(static_cast<std::int32_t>(k)));
            };
            if (k == 0 && !m.normals.empty()) use("LayerElementNormal");
            if (k < uvs.size()) use("LayerElementUV");
            if (k < m.colors.size()) use("LayerElementColor");
            if (k == 0 && material) use("LayerElementMaterial");
        }
        // The model holding it, at the origin.
        Node& model = objects.add("Model");
        model.props = {pL(modelId), pS(named(m.name, "Model")), pS("Mesh")};
        leaf(model, "Version", pI(232));
        Node& mp = model.add("Properties70");
        P(mp, "DefaultAttributeIndex", "int", "Integer", "", {pI(0)});
        P(mp, "InheritType", "enum", "", "", {pI(1)});
        leaf(model, "MultiLayer", pI(0));
        leaf(model, "MultiTake", pI(0));
        leaf(model, "Shading", pC(true));
        leaf(model, "Culling", pS("CullingOff"));
        links.emplace_back("OO", modelId, 0, "");
        links.emplace_back("OO", geometryId, modelId, "");
        if (material) {
            const std::int64_t materialId = next++;
            ++materials;
            Node& mat = objects.add("Material");
            mat.props = {pL(materialId), pS(named(m.material, "Material")), pS("")};
            leaf(mat, "Version", pI(102));
            leaf(mat, "ShadingModel", pS("phong"));
            leaf(mat, "MultiLayer", pI(0));
            Node& matp = mat.add("Properties70");
            P(matp, "DiffuseColor", "Color", "", "A", {pD(1.0), pD(1.0), pD(1.0)});
            P(matp, "DiffuseFactor", "Number", "", "A", {pD(1.0)});
            links.emplace_back("OO", materialId, modelId, "");
            if (!m.baseColorMap.empty()) {
                // The base color map: a texture of a video clip (the image), its path absolute and
                // relative to the FBX.
                const std::int64_t textureId = next++, videoId = next++;
                ++textures;
                std::error_code ec;
                const fs::path image = fs::absolute(fs::u8path(m.baseColorMap), ec);
                const fs::path relative = fs::u8path(m.baseColorMap).lexically_proximate(fs::absolute(fs::u8path(path), ec).parent_path());
                const std::string full = image.generic_u8string(), rel = relative.generic_u8string();
                const std::string texName = m.material + "_baseColor";
                Node& tex = objects.add("Texture");
                tex.props = {pL(textureId), pS(named(texName, "Texture")), pS("")};
                leaf(tex, "Type", pS("TextureVideoClip"));
                leaf(tex, "Version", pI(202));
                leaf(tex, "TextureName", pS(named(texName, "Texture")));
                Node& tp = tex.add("Properties70");
                P(tp, "UVSet", "KString", "", "", {pS("UVMap")});
                P(tp, "UseMaterial", "bool", "", "", {pI(1)});
                leaf(tex, "Media", pS(named(texName, "Video")));
                leaf(tex, "FileName", pS(full));
                leaf(tex, "RelativeFilename", pS(rel));
                Node& video = objects.add("Video");
                video.props = {pL(videoId), pS(named(texName, "Video")), pS("Clip")};
                leaf(video, "Type", pS("Clip"));
                Node& vp = video.add("Properties70");
                P(vp, "Path", "KString", "XRefUrl", "", {pS(full)});
                leaf(video, "UseMipMap", pI(0));
                leaf(video, "Filename", pS(full));
                leaf(video, "RelativeFilename", pS(rel));
                links.emplace_back("OO", videoId, textureId, "");
                links.emplace_back("OP", textureId, materialId, "DiffuseColor");
            }
        }
    }

    // What the file holds, by kind.
    Node& definitions = root.add("Definitions");
    leaf(definitions, "Version", pI(100));
    const int models = static_cast<int>(meshes.size());
    leaf(definitions, "Count", pI(1 + 2 * models + materials + 2 * textures));
    auto type = [&](const std::string& name, int count) {
        if (count == 0) return;
        Node& t = definitions.add("ObjectType");
        t.props = {pS(name)};
        leaf(t, "Count", pI(count));
    };
    type("GlobalSettings", 1);
    type("Model", models);
    type("Geometry", models);
    type("Material", materials);
    type("Texture", textures);
    type("Video", textures);
    root.children.push_back(std::move(objects));
    Node& connections = root.add("Connections");
    for (const auto& [kind, child, parent, property] : links) {
        Node& c = connections.add("C");
        c.props = {pS(kind), pL(child), pL(parent)};
        if (!property.empty()) c.props.push_back(pS(property));
    }
    Node& takes = root.add("Takes");
    leaf(takes, "Current", pS(""));

    // The file: the header, the top-level nodes, a null record, the footer.
    std::string out("Kaydara FBX Binary  \0\x1a\0", 23);
    raw(out, kVersion);
    for (std::size_t i = 0; i < root.children.size(); ++i) encode(out, root.children[i], i + 1 == root.children.size());
    out.append(kSentinel, '\0');
    out.append(kFootId, 16);
    out.append(4, '\0');
    std::size_t pad = ((out.size() + 15) & ~std::size_t(15)) - out.size();
    if (pad == 0) pad = 16;
    out.append(pad, '\0');
    raw(out, kVersion);
    out.append(120, '\0');
    out.append(kFootMagic, 16);

    std::error_code ec;
    if (fs::u8path(path).has_parent_path()) fs::create_directories(fs::u8path(path).parent_path(), ec);
    std::ofstream file(fs::u8path(path), std::ios::binary);
    if (!file) throw std::runtime_error("cannot write " + path);
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
    if (!file) throw std::runtime_error("cannot write " + path);
}

}  // namespace groomlab
