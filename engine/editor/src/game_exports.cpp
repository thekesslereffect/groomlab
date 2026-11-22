// SPDX-License-Identifier: Apache-2.0
#include "groomlab/editor/game_exports.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>

#include "groomlab/engine_meshes.h"
#include "groomlab/fbx_writer.h"

namespace groomlab {

namespace fs = std::filesystem;

std::string gameExportName(const GameExportOptions& o) {
    const std::string base = o.name.empty() ? std::string("groom") : o.name;
    return !o.lod.empty() && !o.nameGiven ? base + "_" + o.lod : base;
}

int gameExportResolution(int resolution) {
    return resolution > 0 ? std::clamp(resolution, kMinBakeResolution, kMaxCardBakeResolution) : 0;
}

std::vector<std::string> EngineCardsExport::files() const {
    std::vector<std::string> out = maps;
    if (!manifest.empty()) out.push_back(manifest);
    if (!fbx.empty()) out.push_back(fbx);
    if (!scalpCapFbx.empty()) out.push_back(scalpCapFbx);
    return out;
}

EngineCardsExport writeEngineCards(const CardBake& bake, const GroomStats& stats, const HairLook& look, const ScalpTextures* cap,
                                   const ScalpMesh* capScalp, const std::string& capWhyNot, bool unity, const GameExportOptions& o) {
    if (!bake.cards) throw std::runtime_error("the groom has no cards (a Cards node)");
    EngineCardsExport out;
    out.name = gameExportName(o);
    std::error_code ec;
    fs::create_directories(fs::u8path(o.directory), ec);
    const std::string dir = fs::u8path(o.directory).generic_u8string();
    const std::string& name = out.name;
    // The cards' maps: Unity's PNGs for its materials, else Unreal's TGAs (the atlas record beside
    // them is the groom's own).
    if (unity) {
        out.maps = writeUnityCardMaps(bake.textures, look.roughness, dir, name);
    } else {
        const CardBakePaths at = CardBakePaths::in(dir, name);
        writeCardBake(bake, at, false);
        for (const std::string& f : at.files()) {
            if (fs::u8path(f).extension() == ".tga") out.maps.push_back(f);
        }
    }
    // The hair cap's maps: the groom baked onto its scalp.
    const bool withCap = o.scalp && cap && capScalp;
    if (withCap) {
        for (const std::string& p : writeScalpCapMaps(*cap, unity, dir, name)) out.maps.push_back(p);
    } else if (o.scalp) {
        out.warnings.push_back(capWhyNot.empty() ? std::string("no hair cap: the scalp has no uvs") : capWhyNot);
    }
    const std::string ext = unity ? ".png" : ".tga";
    const std::string prefix = name + "_";
    for (const std::string& p : out.maps) {
        const std::string file = fs::u8path(p).filename().u8string();
        if (file.size() > prefix.size() + ext.size() && file.compare(0, prefix.size(), prefix) == 0) {
            out.byMap[file.substr(prefix.size(), file.size() - prefix.size() - ext.size())] = file;
        }
    }
    out.fbx = dir + "/" + name + "_cards.fbx";
    out.scalpCapFbx = withCap ? dir + "/" + name + "_scalpCap.fbx" : std::string();
    EngineCardsInfo info;
    info.mesh = name + "_cards.fbx";
    info.scalpCap = withCap ? name + "_scalpCap.fbx" : std::string();
    info.cards = stats.cards;
    info.triangles = stats.cardTriangles;
    info.textures = stats.cardTextures;
    info.feathers = stats.cardFeathers;
    info.maps = out.byMap;
    info.roughness = look.roughness;
    out.manifest = dir + "/" + name + (unity ? "_unity.json" : "_cards.json");
    {
        std::ofstream f(fs::u8path(out.manifest), std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("cannot write " + out.manifest);
        f << engineCardsManifestJson(unity, info);
    }
    if (!o.writeMeshes) return out;
    // The meshes (Unity's in meters, with a material on its base color map; Unreal's in
    // centimeters, the materials made in the engine).
    FbxSettings settings;
    settings.meters = unity;
    settings.creator = "GroomLab";
    FbxMesh cards = cardsFbxMesh(*bake.cards, name + "_cards");
    if (unity) {
        cards.material = name + "_cards_mat";
        if (out.byMap.count("BaseColor")) cards.baseColorMap = dir + "/" + out.byMap["BaseColor"];
    }
    writeFbx(out.fbx, {cards}, settings);
    if (withCap) {
        FbxMesh capMesh = scalpCapFbxMesh(*capScalp, name + "_scalpCap");
        if (unity) {
            capMesh.material = name + "_scalpCap_mat";
            if (out.byMap.count("ScalpBaseColor")) capMesh.baseColorMap = dir + "/" + out.byMap["ScalpBaseColor"];
        }
        writeFbx(out.scalpCapFbx, {capMesh}, settings);
    }
    return out;
}

GltfCardsExport writeGltfCards(const GltfCardsOptions& o, const HairLook& look, const GltfCardsSource& source) {
    CardGltfSettings settings;
    settings.alphaMode = gltfAlphaModeNamed(o.alphaMode);
    if (!(o.scale > 0.0f)) throw std::invalid_argument("scale must be above 0");
    settings.alphaCutoff = o.alphaCutoff;
    settings.scale = o.scale;
    settings.zUp = o.zUp;
    settings.rootToTip = o.rootToTip;
    settings.tangents = o.tangents;
    settings.colorToRec709 = o.colorToRec709;
    settings.roughness = look.roughness;
    settings.specular = look.specular;
    const std::string name = o.name.empty() ? std::string("groom") : o.name;
    const int resolution = gameExportResolution(o.resolution);
    GltfDocument doc;
    doc.rootName = name;
    if (!o.generator.empty()) doc.generator = o.generator;
    GltfCardsExport out;
    const std::vector<std::string> levels = o.lods.empty() ? std::vector<std::string>{std::string()} : o.lods;
    for (const std::string& lod : levels) {
        const CardBake bake = source.bake(lod, resolution, o.occlusion);
        if (!bake.cards) throw std::runtime_error("the groom has no cards (a Cards node)");
        settings.name = name + (lod.empty() ? "_cards" : "_" + lod);
        addCardsToGltf(doc, *bake.cards, bake.textures, settings);
        out.meshes.push_back(settings.name);
        out.cards = bake.cards->cards.size();
    }
    if (o.cap && source.capGroom) {
        // The groom's own graph at full density; the cap as every host writes it (gltf_writer.h:
        // its resolution, just above the skin).
        std::string whyNot;
        MeshPtr scalp;
        const StrandsPtr strands = source.capGroom(&scalp, &whyNot);
        if (!strands) {
            if (whyNot.empty()) whyNot = "the groom has no strands";
        } else if (!scalp || scalp->triangleCount() == 0) {
            whyNot = "the groom has no scalp";
        } else {
            try {
                CardGltfSettings cap = settings;
                cap.name = name + "_scalpCap";
                addScalpCapToGltf(doc, *strands, *scalp, o.capResolution, resolution, cap);
                out.meshes.push_back(cap.name);
            } catch (const std::exception& e) {  // a scalp without UVs: the cards still go
                whyNot = e.what();
            }
        }
        if (!whyNot.empty()) out.warnings.push_back("no hair cap (" + whyNot + ")");
    }
    out.files = writeGltf(o.path, doc, o.binary);
    return out;
}

}  // namespace groomlab
