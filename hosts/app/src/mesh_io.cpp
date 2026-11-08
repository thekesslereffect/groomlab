// SPDX-License-Identifier: Apache-2.0
#include "mesh_io.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include "groomlab/alembic_io.h"
#include "groomlab/retriangulate.h"

namespace groomlab::app {

namespace {

std::string lowerExtension(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    const std::size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::string folderOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

std::string readText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw MeshFileError("cannot open '" + path + "'");
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// One face corner "v", "v/vt", "v//vn" or "v/vt/vn": 1-based, negative from the end; 0: absent.
struct Corner {
    long v = 0, vt = 0, vn = 0;
};

Corner parseCorner(const std::string& token, const std::string& label, std::size_t line) {
    Corner c;
    char* end = nullptr;
    c.v = std::strtol(token.c_str(), &end, 10);
    if (end == token.c_str()) throw MeshFileError(label + ":" + std::to_string(line) + ": bad face corner '" + token + "'");
    if (*end == '/') {
        const char* vt = end + 1;
        if (*vt != '/' && *vt != '\0') {
            c.vt = std::strtol(vt, &end, 10);
            if (end == vt) throw MeshFileError(label + ":" + std::to_string(line) + ": bad face corner '" + token + "'");
        }
        if (*end == '/') {
            const char* vn = end + 1;
            if (*vn != '\0') {
                c.vn = std::strtol(vn, &end, 10);
                if (end == vn) throw MeshFileError(label + ":" + std::to_string(line) + ": bad face corner '" + token + "'");
            }
        }
    }
    return c;
}

// What the OBJ text holds: the mesh, normals per corner (when every corner had one), the
// object's name and the material library and material it names.
struct ObjContents {
    std::shared_ptr<ScalpMesh> mesh;
    std::vector<Vec3f> cornerNormals;
    std::string name, mtllib, usemtl;
};

// `only`: the faces of that group (`g` or `o`) alone; `except`: every face but that group's (the
// points no face kept dropped).
ObjContents parseObj(const std::string& text, const std::string& label, const std::string& only = {}, const std::string& except = {}) {
    ObjContents out;
    std::string group;  // the `g` or `o` the faces are under
    std::vector<Vec3f> points, normals;
    std::vector<ScalpMesh::UV> uvs;
    std::vector<ScalpMesh::Triangle> triangles;
    std::vector<std::int32_t> polygons;  // triangles per face: what Catmull-Clark refines (subdivision.h)
    std::vector<ScalpMesh::UV> cornerUVs;
    std::vector<Vec3f> cornerNormals;
    bool everyCornerHasUV = true, everyCornerHasNormal = true;
    std::istringstream in(text);
    std::string line;
    std::size_t lineNo = 0;
    auto resolve = [&](long index, std::size_t count, const char* what) -> std::size_t {
        const long resolved = index < 0 ? static_cast<long>(count) + index : index - 1;
        if (resolved < 0 || static_cast<std::size_t>(resolved) >= count) {
            throw MeshFileError(label + ":" + std::to_string(lineNo) + ": " + what + " index " + std::to_string(index) +
                                " out of range");
        }
        return static_cast<std::size_t>(resolved);
    };
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream fields(line);
        std::string key;
        if (!(fields >> key) || key[0] == '#') continue;
        if (key == "v") {
            float x = 0, y = 0, z = 0;
            if (!(fields >> x >> y >> z)) throw MeshFileError(label + ":" + std::to_string(lineNo) + ": bad vertex");
            points.push_back({x, y, z});
        } else if (key == "vt") {
            float u = 0, v = 0;
            if (!(fields >> u)) throw MeshFileError(label + ":" + std::to_string(lineNo) + ": bad texture coordinate");
            fields >> v;
            uvs.push_back({u, v});
        } else if (key == "vn") {
            float x = 0, y = 0, z = 0;
            if (!(fields >> x >> y >> z)) throw MeshFileError(label + ":" + std::to_string(lineNo) + ": bad normal");
            normals.push_back({x, y, z});
        } else if (key == "f") {
            if ((!only.empty() && group != only) || (!except.empty() && group == except)) continue;
            std::vector<Corner> corners;
            std::string token;
            while (fields >> token) corners.push_back(parseCorner(token, label, lineNo));
            if (corners.size() < 3) throw MeshFileError(label + ":" + std::to_string(lineNo) + ": a face needs 3 corners");
            std::vector<std::int32_t> vertices;
            std::vector<ScalpMesh::UV> faceUVs;
            std::vector<Vec3f> faceNormals;
            for (const Corner& c : corners) {
                vertices.push_back(static_cast<std::int32_t>(resolve(c.v, points.size(), "vertex")));
                if (c.vt != 0) {
                    faceUVs.push_back(uvs[resolve(c.vt, uvs.size(), "texture coordinate")]);
                } else {
                    everyCornerHasUV = false;
                }
                if (c.vn != 0) {
                    faceNormals.push_back(normals[resolve(c.vn, normals.size(), "normal")]);
                } else {
                    everyCornerHasNormal = false;
                }
            }
            polygons.push_back(static_cast<std::int32_t>(corners.size() - 2));
            // Split as Maya splits it (retriangulate.h: a quad across its shorter diagonal), so a
            // groom made on this mesh meets the same triangles on the mesh in Maya.
            std::vector<Vec3f> facePoints;
            for (const std::int32_t v : vertices) facePoints.push_back(points[static_cast<std::size_t>(v)]);
            for (const std::array<int, 3>& tri : polygonTriangles(facePoints)) {
                triangles.push_back({vertices[tri[0]], vertices[tri[1]], vertices[tri[2]]});
                for (const int k : tri) {
                    if (everyCornerHasUV) cornerUVs.push_back(faceUVs[static_cast<std::size_t>(k)]);
                    if (everyCornerHasNormal) cornerNormals.push_back(faceNormals[static_cast<std::size_t>(k)]);
                }
            }
        } else if (key == "o" || key == "g") {
            group.clear();
            std::getline(fields >> std::ws, group);
            if (out.name.empty()) out.name = group;
        } else if (key == "mtllib") {
            std::getline(fields >> std::ws, out.mtllib);
        } else if (key == "usemtl") {
            if (out.usemtl.empty()) std::getline(fields >> std::ws, out.usemtl);
        }
        // s: nothing a scalp needs.
    }
    if (points.empty() || triangles.empty()) {
        throw MeshFileError(label + (only.empty() ? std::string(": no faces") : ": no faces in group " + only));
    }
    if (!only.empty() || !except.empty()) {
        // The points the kept faces use, in their order.
        std::vector<std::int32_t> remap(points.size(), -1);
        std::vector<Vec3f> used;
        for (ScalpMesh::Triangle& t : triangles) {
            for (std::int32_t& v : t) {
                std::int32_t& r = remap[static_cast<std::size_t>(v)];
                if (r < 0) {
                    r = static_cast<std::int32_t>(used.size());
                    used.push_back(points[static_cast<std::size_t>(v)]);
                }
                v = r;
            }
        }
        points = std::move(used);
        if (!only.empty()) out.name = only;
    }
    out.mesh = std::make_shared<ScalpMesh>(std::move(points), std::move(triangles));
    out.mesh->setPolygons(std::move(polygons));
    if (everyCornerHasUV && !cornerUVs.empty()) out.mesh->setCornerUVs(std::move(cornerUVs));
    if (everyCornerHasNormal && !cornerNormals.empty()) out.cornerNormals = std::move(cornerNormals);
    return out;
}

// The maps of each material in an MTL text, by material name, as the SurfaceMaterial fields.
struct MtlMaterial {
    SurfaceMaterial material;
    bool hasKd = false;
};

std::map<std::string, MtlMaterial> parseMtl(const std::string& text, const std::string& folder) {
    std::map<std::string, MtlMaterial> out;
    std::istringstream in(text);
    std::string line, current;
    // A map's path: the rest of the line after its options (`-bm 1.0`, `-s 1 1 1`, ...: a dash
    // word and its numbers), relative to the library's folder unless absolute.
    auto mapPath = [&](std::istringstream& fields) {
        std::vector<std::string> tokens;
        std::string token;
        while (fields >> token) tokens.push_back(token);
        std::size_t first = 0;
        while (first < tokens.size() && tokens[first].size() > 1 && tokens[first][0] == '-' && !std::isdigit(static_cast<unsigned char>(tokens[first][1]))) {
            ++first;
            while (first < tokens.size() && (std::isdigit(static_cast<unsigned char>(tokens[first][0])) || tokens[first][0] == '.' ||
                                             (tokens[first][0] == '-' && tokens[first].size() > 1 && std::isdigit(static_cast<unsigned char>(tokens[first][1]))))) {
                ++first;
            }
        }
        std::string path;
        for (std::size_t i = first; i < tokens.size(); ++i) path += (i > first ? " " : "") + tokens[i];
        std::replace(path.begin(), path.end(), '\\', '/');
        const bool absolute = !path.empty() && (path[0] == '/' || (path.size() > 1 && path[1] == ':'));
        return absolute || folder.empty() ? path : folder + path;
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream fields(line);
        std::string key;
        if (!(fields >> key) || key[0] == '#') continue;
        std::string lower = key;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower == "newmtl") {
            std::getline(fields >> std::ws, current);
            out[current] = MtlMaterial();
            continue;
        }
        if (current.empty()) continue;
        MtlMaterial& m = out[current];
        if (lower == "kd") {
            float r = 1, g = 1, b = 1;
            if (fields >> r >> g >> b) {
                m.material.baseColor = {r, g, b};
                m.hasKd = true;
            }
        } else if (lower == "map_kd") {
            m.material.albedoMap = mapPath(fields);
        } else if (lower == "map_bump" || lower == "bump" || lower == "norm") {
            m.material.normalMap = mapPath(fields);
        } else if (lower == "map_ks") {
            m.material.specularMap = mapPath(fields);
        } else if (lower == "map_pr") {
            m.material.roughnessMap = mapPath(fields);
        } else if (lower == "map_pm") {
            m.material.metallicMap = mapPath(fields);
        } else if (lower == "map_ka") {
            m.material.occlusionMap = mapPath(fields);
        }
    }
    return out;
}

}  // namespace

std::shared_ptr<const ScalpMesh> parseObjMesh(const std::string& text, const std::string& label, const std::string& group) {
    return parseObj(text, label, group).mesh;
}

std::shared_ptr<const ScalpMesh> readObjMesh(const std::string& path, const std::string& group) {
    return parseObjMesh(readText(path), path, group);
}

Surface parseObjSurface(const std::string& text, const std::string& mtlText, const std::string& label, const std::string& folder,
                        const SurfaceMaterial& material, const std::string& exceptGroup) {
    ObjContents contents = parseObj(text, label, {}, exceptGroup);
    Surface s;
    s.name = contents.name.empty() ? label : contents.name;
    s.mesh = contents.mesh;
    s.cornerNormals = std::move(contents.cornerNormals);
    s.material = material;
    if (!mtlText.empty()) {
        // The material given is the base: the library's maps and its color go over it.
        const std::map<std::string, MtlMaterial> materials = parseMtl(mtlText, folder);
        auto it = materials.find(contents.usemtl);
        if (it == materials.end() && !materials.empty()) it = materials.begin();
        if (it != materials.end()) {
            const SurfaceMaterial& m = it->second.material;
            if (it->second.hasKd) s.material.baseColor = m.baseColor;
            if (!m.albedoMap.empty()) s.material.albedoMap = m.albedoMap;
            if (!m.normalMap.empty()) s.material.normalMap = m.normalMap;
            if (!m.specularMap.empty()) s.material.specularMap = m.specularMap;
            if (!m.roughnessMap.empty()) s.material.roughnessMap = m.roughnessMap;
            if (!m.metallicMap.empty()) s.material.metallicMap = m.metallicMap;
            if (!m.occlusionMap.empty()) s.material.occlusionMap = m.occlusionMap;
        }
    }
    return s;
}

Surface readObjSurface(const std::string& path, const SurfaceMaterial& material, const std::string& exceptGroup) {
    const std::string text = readText(path);
    const std::string folder = folderOf(path);
    // The library is optional: a missing one leaves the material as given.
    std::string mtlText;
    {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("mtllib", 0) != 0) continue;
            std::istringstream fields(line);
            std::string key, name;
            fields >> key;
            std::getline(fields >> std::ws, name);
            if (!name.empty() && name.back() == '\r') name.pop_back();
            std::ifstream mtl(folder + name, std::ios::binary);
            if (mtl) {
                std::stringstream buffer;
                buffer << mtl.rdbuf();
                mtlText = buffer.str();
            }
            break;
        }
    }
    Surface s = parseObjSurface(text, mtlText, path, folder, material, exceptGroup);
    const std::size_t slash = path.find_last_of("/\\");
    if (s.name == path) s.name = slash == std::string::npos ? path : path.substr(slash + 1);
    return s;
}

std::shared_ptr<const ScalpMesh> readAlembicMesh(const std::string& path, const std::string& object, double seconds) {
    try {
        return AlembicMeshReader(path, object).meshAt(seconds);
    } catch (const AlembicError& e) {
        throw MeshFileError(e.what());
    }
}

std::shared_ptr<const ScalpMesh> readMeshFile(const std::string& path) {
    const std::string ext = lowerExtension(path);
    if (ext == "obj") return readObjMesh(path);
    if (ext == "abc") return readAlembicMesh(path);
    throw MeshFileError("'" + path + "': not an .obj or .abc mesh");
}

}  // namespace groomlab::app
