// SPDX-License-Identifier: Apache-2.0
#include "asset_files.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>

#include <maya/MFnDagNode.h>
#include <maya/MFnDependencyNode.h>
#include <maya/MGlobal.h>
#include <maya/MItDependencyNodes.h>
#include <maya/MPlug.h>
#include <maya/MString.h>

#include "groom_shape.h"
#include "maya_inputs.h"
#include "groomlab/asset_files.h"
#include "groomlab/card_files.h"
#include "groomlab/scalp_maps.h"

namespace groomlab::maya {

namespace fs = std::filesystem;

namespace {

// The path as compared: canonical where it exists, generic separators, case folded on Windows.
std::string comparable(const std::string& path) {
    if (path.empty()) return {};
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::u8path(path), ec);
    if (ec) p = fs::u8path(path).lexically_normal();
    std::string out = p.generic_u8string();
#ifdef _WIN32
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return out;
}

// Calls fn(node, shape) for every groomLabShape but `groom`.
template <typename Fn>
bool anyOtherGroom(const MObject& groom, Fn&& fn) {
    for (MItDependencyNodes it(MFn::kPluginShape); !it.isDone(); it.next()) {
        MObject node = it.thisNode();
        if (node == groom) continue;
        MFnDependencyNode dep(node);
        auto* shape = dynamic_cast<GroomShape*>(dep.userNode());
        if (!shape) continue;
        if (fn(node, *shape)) return true;
    }
    return false;
}

// A card bake's file prefix (<folder>/<name>_, resolved for the node) from its cardMaps; "" without one.
std::string bakePrefixOf(const MObject& node) {
    const std::string stored = cardMapsPrefix(MFnDependencyNode(node).findPlug("cardMaps", true).asString().asChar());
    if (stored.empty()) return {};
    return resolveNodePath(node, stored + "x");  // x: a placeholder file name
}

// Whether `full` is a file of the bake with prefix `prefixFile` (<folder>/<name>_x: the prefix
// plus a placeholder character, so it resolves like a file).
bool inBake(const std::string& full, const std::string& prefixFile) {
    if (prefixFile.empty()) return false;
    const std::string prefix = comparable(prefixFile);
    const std::string file = comparable(full);
    const std::string stem = prefix.substr(0, prefix.size() - 1);  // the placeholder dropped
    return file.size() > stem.size() && file.compare(0, stem.size(), stem) == 0 &&
           file.find('/', stem.size()) == std::string::npos;
}

}  // namespace

std::string fileSafeName(const std::string& text) {
    std::string out;
    std::size_t start = 0;
    while (start < text.size() && text[start] == '|') ++start;
    for (std::size_t i = start; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        out += std::isalnum(c) || c == '_' || c == '.' || c == '-' ? static_cast<char>(c) : '_';
    }
    while (!out.empty() && out.back() == '.') out.pop_back();
    return out.empty() ? std::string("groom") : out;
}

std::string assetName(const MDagPath& groom) {
    MDagPath transform = groom;
    if (transform.node().hasFn(MFn::kShape)) transform.pop();
    return fileSafeName(transform.fullPathName().asChar());
}

std::string assetFolder(const std::string& kind) {
    MString named;
    MGlobal::executeCommand("file -q -sceneName", named);
    const std::string scene = named.asChar();
    std::string base;
    if (!scene.empty()) {
        const fs::path p = fs::u8path(scene);
        base = p.parent_path().generic_u8string() + "/groomlab/" + p.stem().u8string();
    } else {
        MString root;
        MGlobal::executeCommand("workspace -q -rootDirectory", root);
        std::string r = root.asChar();
        while (!r.empty() && (r.back() == '/' || r.back() == '\\')) r.pop_back();
        base = r + "/groomlab/untitled";
    }
    return kind.empty() ? base : base + "/" + kind;
}

bool fileUsedByOtherGroom(const MObject& groom, const std::string& full) {
    return anyOtherGroom(groom, [&](const MObject& node, GroomShape& shape) {
        for (const auto& [name, entry] : shape.scalpMaps().maps) {
            if (entry.kind == "texture" && !entry.texture.empty() && sameFile(resolveNodePath(node, entry.texture), full)) {
                return true;
            }
        }
        return inBake(full, bakePrefixOf(node));
    });
}

bool bakePrefixUsedByOtherGroom(const MObject& groom, const std::string& prefix) {
    return anyOtherGroom(groom, [&](const MObject& node, GroomShape&) {
        const std::string theirs = bakePrefixOf(node);
        return !theirs.empty() && comparable(theirs) == comparable(prefix + "x");
    });
}

std::string forkTextureFile(const MDagPath& groom, const std::string& mapName, const std::string& full) {
    const std::string folder = assetFolder("masks") + "/" + assetName(groom);
    std::error_code ec;
    fs::create_directories(fs::u8path(folder), ec);
    const std::string target = freeFilePath(folder + "/" + mapName + ".png");
    if (fs::exists(fs::u8path(full), ec)) {
        noteFileWritten();
        fs::copy_file(fs::u8path(full), fs::u8path(target), ec);
        if (ec) throw std::runtime_error("could not copy " + full + " to " + target + ": " + ec.message());
    }
    MGlobal::displayInfo(MString("GroomLab: ") + mapName.c_str() + "'s file is shared with another groom; this groom " +
                         "now paints its own copy, " + target.c_str());
    return target;
}

std::vector<std::uint8_t> fileBytes(const std::string& path) {
    // Its size, then one read (not a byte at a time).
    std::ifstream in(fs::u8path(path), std::ios::binary | std::ios::ate);
    if (!in) return {};
    const std::streamoff size = in.tellg();
    if (size <= 0) return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    bytes.resize(static_cast<std::size_t>(in.gcount()));
    return bytes;
}

namespace {
std::atomic<std::uint64_t> writes{0};
}

std::uint64_t fileWrites() { return writes.load(); }

void noteFileWritten() { ++writes; }

void putFileBytes(const std::string& path, const std::vector<std::uint8_t>& bytes, const char* who) {
    std::error_code ec;
    const fs::path folder = fs::u8path(path).parent_path();
    if (!folder.empty()) fs::create_directories(folder, ec);
    noteFileWritten();
    std::ofstream out(fs::u8path(path), std::ios::binary | std::ios::trunc);
    if (!out) {
        MGlobal::displayWarning(MString(who) + ": can't write " + path.c_str());
        return;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace groomlab::maya
