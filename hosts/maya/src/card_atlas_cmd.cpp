// SPDX-License-Identifier: Apache-2.0
#include "card_atlas_cmd.h"

#include <algorithm>
#include <filesystem>

#include <maya/MArgDatabase.h>
#include <maya/MDagPath.h>
#include <maya/MFnDagNode.h>
#include <maya/MFnDependencyNode.h>
#include <maya/MPlug.h>
#include <maya/MSelectionList.h>
#include <maya/MStringArray.h>

#include "asset_files.h"
#include "command_args.h"
#include "groomlab/card_blocks.h"
#include "groomlab/card_maps.h"
#include "guarded.h"
#include "hair_shading.h"
#include "maya_inputs.h"

namespace groomlab::maya {

namespace fs = std::filesystem;

const char* CardAtlasCmd::kName = "groomLabCardAtlas";

void* CardAtlasCmd::creator() { return new CardAtlasCmd(); }

MSyntax CardAtlasCmd::newSyntax() {
    MSyntax s;
    s.addFlag("-m", "-maps", MSyntax::kString);
    s.addFlag("-pv", "-preview", MSyntax::kString);
    s.addFlag("-sh", "-shapes", MSyntax::kString);
    s.addFlag("-n", "-name", MSyntax::kString);
    s.addFlag("-pre", "-preset", MSyntax::kString);
    s.addFlag("-r", "-resolution", MSyntax::kLong);
    s.addFlag("-fmt", "-format", MSyntax::kString);
    s.addFlag("-bd", "-bitDepth", MSyntax::kLong);
    s.addFlag("-pad", "-padding", MSyntax::kLong);
    s.addFlag("-ss", "-supersample", MSyntax::kLong);
    s.addFlag("-nt", "-nameTemplate", MSyntax::kString);
    s.addFlag("-ao", "-occlusion", MSyntax::kDouble);
    s.addFlag("-seg", "-segments", MSyntax::kLong);
    s.addFlag("-acr", "-across", MSyntax::kLong);
    s.addFlag("-bnd", "-bend", MSyntax::kDouble);
    s.addFlag("-tw", "-twist", MSyntax::kDouble);
    s.addFlag("-mt", "-meters");
    s.addFlag("-fc", "-fitCards");
    s.setObjectType(MSyntax::kSelectionList, 1);
    s.useSelectionAsDefault(true);
    return s;
}

MStatus CardAtlasCmd::doIt(const MArgList& args) {
    return guarded("CardAtlasCmd::doIt", [&]() -> MStatus {
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;
        // The blocks: the grooms named, in order, each its cardBlock.
        MSelectionList selection;
        db.getObjects(selection);
        std::vector<std::pair<std::string, std::string>> named;
        std::vector<MObject> nodes;
        for (unsigned i = 0; i < selection.length(); ++i) {
            MDagPath path;
            if (selection.getDagPath(i, path) != MS::kSuccess) continue;
            if (groomAt(kName, &path) != MS::kSuccess) return MS::kFailure;
            const MPlug plug = MFnDependencyNode(path.node()).findPlug("cardBlock", true);
            const std::string text = plug.isNull() ? std::string() : plug.asString().asChar();
            MDagPath transform = path;
            transform.pop();
            const std::string name = MFnDagNode(transform).name().asChar();
            if (text.find_first_not_of(" \t\r\n") == std::string::npos) return fail(kName, path.partialPathName() + " is no Card Atlas block");
            named.emplace_back(name, text);
            nodes.push_back(path.node());
        }
        if (nodes.empty()) return fail(kName, "name the Card Atlas' block grooms");
        const CardBlockAtlas atlas = cardBlockAtlasOf(named);
        // Every block's hair at full density.
        const auto hair = [&]() {
            std::vector<StrandsPtr> strands;
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                std::string error;
                StrandsPtr s = evaluateGroomNode(nodes[i], 1.0f, &error, nullptr, false, nullptr, true);
                if (!s && !error.empty()) displayWarning(MString("groomLabCardAtlas: ") + named[i].first.c_str() + ": " + error.c_str());
                strands.push_back(std::move(s));
            }
            return strands;
        };

        if (db.isFlagSet("-fitCards")) {
            // Fit Card to Hair: each block's text with its card fitted to its hair (and the layout
            // that follows), in the grooms' order; the caller writes them (undoably).
            CardBlockAtlas fitted = atlas;
            fitCardBlocks(fitted, hair());
            MStringArray result;
            for (const CardBlock& b : fitted.blocks) result.append(cardBlockToJson(b, fitted.settings).c_str());
            setResult(result);
            return MS::kSuccess;
        }

        if (db.isFlagSet("-shapes")) {
            MString path;
            db.getFlagArgument("-shapes", 0, path);
            CardShapeOptions options;
            if (db.isFlagSet("-segments")) db.getFlagArgument("-segments", 0, options.segments);
            if (db.isFlagSet("-across")) db.getFlagArgument("-across", 0, options.across);
            double value = 0.0;
            if (db.isFlagSet("-bend") && db.getFlagArgument("-bend", 0, value) == MS::kSuccess) options.bend = static_cast<float>(value);
            if (db.isFlagSet("-twist") && db.getFlagArgument("-twist", 0, value) == MS::kSuccess) options.twist = static_cast<float>(value);
            writeCardBlockShapes(path.asChar(), atlas, options, db.isFlagSet("-meters"));
            setResult(path);
            return MS::kSuccess;
        }

        const bool preview = db.isFlagSet("-preview");
        if (!preview && !db.isFlagSet("-maps")) return fail(kName, "say -maps, -preview or -shapes");
        MString folder;
        db.getFlagArgument(preview ? "-preview" : "-maps", 0, folder);
        double occlusion = 1.0;
        if (db.isFlagSet("-occlusion")) db.getFlagArgument("-occlusion", 0, occlusion);
        const std::vector<StrandsPtr> strands = hair();
        CardMapExport settings;
        int resolution = preview ? 512 : 0;
        if (db.isFlagSet("-resolution")) db.getFlagArgument("-resolution", 0, resolution);
        settings.resolution = resolution > 0 ? std::clamp(resolution, 16, 8192) : 0;
        CardMapPreset preset;
        std::string name = atlas.settings.name;
        if (preview) {
            const auto all = findCardMapPreset("Separate maps (all)");
            if (!all) throw std::runtime_error("no 'Separate maps (all)' preset");
            preset = *all;
            settings.format = "png";
            name = "preview";
        } else {
            MString value;
            std::string text = atlas.settings.preset;
            if (db.isFlagSet("-preset") && db.getFlagArgument("-preset", 0, value) == MS::kSuccess) text = value.asChar();
            if (!text.empty() && text.front() == '{') {
                preset = cardMapPresetFromJson(text);
            } else if (const auto found = findCardMapPreset(text)) {
                preset = *found;
            } else {
                throw std::runtime_error("no card map preset named '" + text + "'");
            }
            if (db.isFlagSet("-name") && db.getFlagArgument("-name", 0, value) == MS::kSuccess) name = value.asChar();
            if (db.isFlagSet("-format") && db.getFlagArgument("-format", 0, value) == MS::kSuccess) settings.format = value.asChar();
            if (db.isFlagSet("-bitDepth")) db.getFlagArgument("-bitDepth", 0, settings.bitDepth);
            if (db.isFlagSet("-padding")) db.getFlagArgument("-padding", 0, settings.padding);
            if (db.isFlagSet("-supersample")) db.getFlagArgument("-supersample", 0, settings.supersample);
            if (db.isFlagSet("-nameTemplate") && db.getFlagArgument("-nameTemplate", 0, value) == MS::kSuccess) {
                settings.nameTemplate = value.asChar();
            }
        }
        const CardMaps maps = bakeCardBlockMaps(atlas, strands, settings, static_cast<float>(occlusion), renderingSpace().toRec709);
        fs::create_directories(fs::u8path(folder.asChar()));
        MStringArray result;
        for (const std::string& p : writeCardMaps(maps, preset, settings, folder.asChar(), fileSafeName(name))) result.append(p.c_str());
        setResult(result);
        return MS::kSuccess;
    });
}

}  // namespace groomlab::maya
