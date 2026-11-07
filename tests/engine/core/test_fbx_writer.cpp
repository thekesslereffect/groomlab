// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <string>

#include "groomlab/fbx_writer.h"

using namespace groomlab;
namespace fs = std::filesystem;

namespace {

std::string bytesOf(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::uint32_t u32(const std::string& s, std::size_t at) {
    std::uint32_t v = 0;
    std::memcpy(&v, s.data() + at, 4);
    return v;
}

// Walks the node records as an FBX reader does (each end offset, property block, nested list
// closed by a null record): every node's path ("Objects/Geometry/LayerElementUV"...).
void walk(const std::string& s, std::size_t at, std::size_t end, const std::string& parent, std::multiset<std::string>& out) {
    while (at + 13 <= end) {
        const std::uint32_t next = u32(s, at), props = u32(s, at + 4), propsLength = u32(s, at + 8);
        const auto nameLength = static_cast<unsigned char>(s[at + 12]);
        if (next == 0) return;  // the null record closing the list
        ASSERT_LE(next, end);
        const std::string name = s.substr(at + 13, nameLength);
        out.insert(parent + name);
        const std::size_t children = at + 13 + nameLength + propsLength;
        (void)props;
        if (children < next) walk(s, children, next, parent + name + "/", out);
        at = next;
    }
}

}  // namespace

// A quad and a triangle, their uvs, a second uv set, a color set, normals and a material: a binary
// FBX 7.4 whose records nest as the format has them, Y up, its footer in place.
TEST(FbxWriter, WritesABinaryFbxOfTheMeshes) {
    FbxMesh m;
    m.name = "cards";
    m.points = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {2, 0, 0}};
    m.faceCounts = {4, 3};
    m.faceIndices = {0, 1, 2, 3, 1, 4, 2};
    for (int c = 0; c < 7; ++c) {
        m.uvs.insert(m.uvs.end(), {0.1f * c, 0.2f});
        m.normals.push_back({0, 0, 1});
    }
    m.uvSets.emplace_back("aoWorld", std::vector<float>(14, 0.5f));
    m.colors.emplace_back("groomLabTint", std::vector<float>(28, 0.5f));
    m.material = "cards_mat";
    m.baseColorMap = "cards_BaseColor.png";
    const std::string path = (fs::temp_directory_path() / "groomlab_fbx_writer" / "cards.fbx").generic_u8string();
    writeFbx(path, {m}, FbxSettings{true, "GroomLab tests"});
    const std::string s = bytesOf(path);
    ASSERT_GT(s.size(), 300u);
    EXPECT_EQ(s.compare(0, 23, std::string("Kaydara FBX Binary  \0\x1a\0", 23)), 0);
    EXPECT_EQ(u32(s, 23), 7400u);
    EXPECT_EQ(s.compare(s.size() - 16, 16, std::string("\xf8\x5a\x8c\x6a\xde\xf5\xd9\x7e\xec\xe9\x0c\xe3\x75\x8f\x29\x0b", 16)), 0);
    std::multiset<std::string> nodes;
    walk(s, 27, s.size(), "", nodes);
    for (const char* n : {"FBXHeaderExtension", "FileId", "GlobalSettings", "Definitions", "Objects", "Connections", "Objects/Geometry",
                          "Objects/Model", "Objects/Material", "Objects/Texture", "Objects/Video", "Objects/Geometry/Vertices",
                          "Objects/Geometry/PolygonVertexIndex", "Objects/Geometry/LayerElementNormal", "Objects/Geometry/LayerElementColor",
                          "Objects/Geometry/LayerElementMaterial"}) {
        EXPECT_EQ(nodes.count(n), 1u) << n;
    }
    EXPECT_EQ(nodes.count("Objects/Geometry/LayerElementUV"), 2u) << "UVMap and aoWorld";
    EXPECT_EQ(nodes.count("Objects/Geometry/Layer"), 2u);
    EXPECT_EQ(nodes.count("Connections/C"), 5u) << "model, geometry, material, texture, video";
    // A mesh whose arrays don't match its corners is refused.
    FbxMesh bad = m;
    bad.uvs.pop_back();
    EXPECT_THROW(writeFbx(path, {bad}), std::invalid_argument);
}
