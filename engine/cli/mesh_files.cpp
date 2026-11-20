// SPDX-License-Identifier: Apache-2.0
#include "mesh_files.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "groomlab/alembic_io.h"
#include "groomlab/retriangulate.h"

namespace groomlab::cli {

namespace {

// One face corner "v", "v/vt", "v//vn" or "v/vt/vn": 1-based, negative from the end; 0: absent.
struct Corner {
    long v = 0, vt = 0;
};

Corner parseCorner(const std::string& token, const std::string& where) {
    Corner c;
    char* end = nullptr;
    c.v = std::strtol(token.c_str(), &end, 10);
    if (end == token.c_str()) throw MeshFileError(where + ": bad face corner '" + token + "'");
    if (*end == '/' && end[1] != '/' && end[1] != '\0') {
        const char* vt = end + 1;
        c.vt = std::strtol(vt, &end, 10);
        if (end == vt) throw MeshFileError(where + ": bad face corner '" + token + "'");
    }
    return c;
}

std::string lowerExtension(const std::string& path) {
    std::string ext = std::filesystem::u8path(path).extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return ext;
}

}  // namespace

std::shared_ptr<const ScalpMesh> parseObjMesh(const std::string& text, const std::string& label) {
    std::vector<Vec3f> points;
    std::vector<ScalpMesh::UV> uvs, cornerUVs;
    std::vector<ScalpMesh::Triangle> triangles;
    std::vector<std::int32_t> polygons;  // triangles per face: what Catmull-Clark refines (subdivision.h)
    bool everyCornerHasUV = true;
    std::istringstream in(text);
    std::string line;
    std::size_t lineNo = 0;
    auto where = [&] { return label + ":" + std::to_string(lineNo); };
    auto resolve = [&](long index, std::size_t count, const char* what) -> std::size_t {
        const long resolved = index < 0 ? static_cast<long>(count) + index : index - 1;
        if (resolved < 0 || static_cast<std::size_t>(resolved) >= count) {
            throw MeshFileError(where() + ": " + what + " index " + std::to_string(index) + " out of range");
        }
        return static_cast<std::size_t>(resolved);
    };
    while (std::getline(in, line)) {
        ++lineNo;
        std::istringstream fields(line);
        std::string key;
        if (!(fields >> key) || key[0] == '#') continue;
        if (key == "v") {
            float x = 0, y = 0, z = 0;
            if (!(fields >> x >> y >> z)) throw MeshFileError(where() + ": bad vertex");
            points.push_back({x, y, z});
        } else if (key == "vt") {
            float u = 0, v = 0;
            if (!(fields >> u)) throw MeshFileError(where() + ": bad texture coordinate");
            fields >> v;
            uvs.push_back({u, v});
        } else if (key == "f") {
            std::vector<Corner> corners;
            std::string token;
            while (fields >> token) corners.push_back(parseCorner(token, where()));
            if (corners.size() < 3) throw MeshFileError(where() + ": a face needs 3 corners");
            std::vector<std::int32_t> vertices;
            std::vector<ScalpMesh::UV> faceUVs;
            for (const Corner& c : corners) {
                vertices.push_back(static_cast<std::int32_t>(resolve(c.v, points.size(), "vertex")));
                if (c.vt != 0) {
                    faceUVs.push_back(uvs[resolve(c.vt, uvs.size(), "texture coordinate")]);
                } else {
                    everyCornerHasUV = false;
                }
            }
            polygons.push_back(static_cast<std::int32_t>(corners.size() - 2));
            // Split as every host splits it (retriangulate.h: Maya's, a quad across its shorter
            // diagonal), so a groom made here meets the same triangles in Maya, Blender and the app.
            std::vector<Vec3f> facePoints;
            for (const std::int32_t v : vertices) facePoints.push_back(points[static_cast<std::size_t>(v)]);
            for (const std::array<int, 3>& tri : polygonTriangles(facePoints)) {
                triangles.push_back({vertices[static_cast<std::size_t>(tri[0])], vertices[static_cast<std::size_t>(tri[1])],
                                     vertices[static_cast<std::size_t>(tri[2])]});
                if (everyCornerHasUV) {
                    for (const int k : tri) cornerUVs.push_back(faceUVs[static_cast<std::size_t>(k)]);
                }
            }
        }
    }
    if (triangles.empty()) throw MeshFileError(label + ": the mesh has no faces");
    auto mesh = std::make_shared<ScalpMesh>(std::move(points), std::move(triangles));
    if (everyCornerHasUV && !cornerUVs.empty()) mesh->setCornerUVs(std::move(cornerUVs));
    mesh->setPolygons(std::move(polygons));
    return mesh;
}

std::shared_ptr<const ScalpMesh> readMeshFile(const std::string& path, const std::string& object) {
    const std::string ext = lowerExtension(path);
    const std::string label = std::filesystem::u8path(path).filename().u8string();
    if (ext == ".obj") {
        std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
        if (!in) throw MeshFileError("cannot open '" + path + "'");
        std::stringstream buffer;
        buffer << in.rdbuf();
        return parseObjMesh(buffer.str(), label);
    }
    if (ext == ".abc") {
        try {
            AlembicMeshReader reader(path, object);
            double start = 0.0;
            const bool animated = reader.timeRange(&start, nullptr);
            const auto mesh = reader.meshAt(animated ? start : 0.0);
            if (!mesh || mesh->triangleCount() == 0) throw MeshFileError(label + ": the mesh has no triangles");
            return mesh;
        } catch (const AlembicError& e) {
            throw MeshFileError(label + ": " + e.what());
        }
    }
    throw MeshFileError("'" + label + "' is not a mesh file this reads (.obj or .abc)");
}

}  // namespace groomlab::cli
