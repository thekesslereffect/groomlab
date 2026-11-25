// SPDX-License-Identifier: Apache-2.0
#include "scalp_maps_cmd.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <maya/MArgDatabase.h>
#include <maya/MArgList.h>
#include <maya/MDagPath.h>
#include <maya/MDoubleArray.h>
#include <maya/MFnDagNode.h>
#include <maya/MGlobal.h>
#include <maya/MPlug.h>
#include <maya/MSelectionList.h>
#include <maya/MStringArray.h>

#include "asset_files.h"
#include "groom_shape.h"
#include "groomlab/editor/map_edits.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/scalp_paint.h"
#include "groomlab/asset_files.h"
#include "groomlab/texture.h"
#include "command_args.h"
#include "guarded.h"
#include "maya_inputs.h"
#include "groomlab/scalp_maps.h"
#include "groomlab/udim.h"
#include "groomlab/stroke.h"

namespace groomlab::maya {

namespace {

const char* kList[] = {"-ls", "-list"};
const char* kKindOf[] = {"-ko", "-kindOf"};
const char* kAdd[] = {"-mk", "-make"};  // "-add" and "-new" are refused by MSyntax
const char* kKind[] = {"-k", "-kind"};
const char* kValue[] = {"-v", "-value"};
const char* kResolution[] = {"-res", "-resolution"};
const char* kTexture[] = {"-tx", "-texture"};
const char* kRemove[] = {"-rm", "-remove"};
const char* kRename[] = {"-rn", "-rename"};
const char* kFill[] = {"-f", "-fill"};
const char* kConvert[] = {"-cv", "-convert"};
const char* kValues[] = {"-val", "-values"};
const char* kColor[] = {"-clr", "-color"};
const char* kFillColor[] = {"-fc", "-fillColor"};
const char* kFillChannels[] = {"-fch", "-fillChannels"};
const char* kPaintMode[] = {"-pmd", "-paintMode"};
const char* kChannelsOf[] = {"-cho", "-channelsOf"};
const char* kSetChannels[] = {"-sch", "-setChannels"};
const char* kRelink[] = {"-rl", "-relink"};
const char* kCopyFrom[] = {"-cf", "-copyFrom"};
const char* kDeleteFile[] = {"-df", "-deleteFile"};
// -fill on the selected guides only (a guide layer: Fill Selected).
const char* kSelected[] = {"-sel", "-selected"};
// Paint on a setting (node, param): the map it reads, else -mapName (its own by default) made as
// -kind and linked, or -channel (r g b a) of a color map through its Map node. Returns the map,
// its kind and the channel ("" its value).
const char* kPaintParam[] = {"-pp", "-paintParam"};
const char* kMapName[] = {"-mn", "-mapName"};
const char* kChannel[] = {"-chn", "-channel"};
// With -paintParam: linked through a Map node wired into the param (New Map for a setting), not
// listed in the param itself.
const char* kMapNode[] = {"-mnd", "-mapNode"};
// The scalp's faces (polygon indices, space separated): -make makes the map 0 with -value (or
// -fillColor) on those faces only; -fill fills those faces only. A guide layer: the guides
// rooted on them.
const char* kFaces[] = {"-fcs", "-faces"};

// (Every host's: scalp_maps.h mapKindOf / mapChannelsOf.)
std::string kindOf(const ScalpMaps& maps, const GuideHierarchy& h, const std::string& name) { return mapKindOf(maps, &h, name); }

// 1 (gray) or 4 (color); guide layers are gray; 0 for no map.
int channelsOf(const ScalpMaps& maps, const GuideHierarchy& h, const std::string& name) { return mapChannelsOf(maps, &h, name); }

}  // namespace

const char* ScalpMapsCmd::kName = "groomLabScalpMaps";

void* ScalpMapsCmd::creator() { return new ScalpMapsCmd(); }

MSyntax ScalpMapsCmd::newSyntax() {
    MSyntax s;
    s.addFlag(kList[0], kList[1]);
    s.addFlag(kKindOf[0], kKindOf[1], MSyntax::kString);
    s.addFlag(kAdd[0], kAdd[1], MSyntax::kString);
    s.addFlag(kKind[0], kKind[1], MSyntax::kString);
    s.addFlag(kValue[0], kValue[1], MSyntax::kDouble);
    s.addFlag(kResolution[0], kResolution[1], MSyntax::kLong);
    s.addFlag(kTexture[0], kTexture[1], MSyntax::kString);
    s.addFlag(kRemove[0], kRemove[1], MSyntax::kString);
    s.addFlag(kRename[0], kRename[1], MSyntax::kString, MSyntax::kString);
    s.addFlag(kFill[0], kFill[1], MSyntax::kString);
    s.addFlag(kConvert[0], kConvert[1], MSyntax::kString);
    s.addFlag(kValues[0], kValues[1], MSyntax::kString);
    s.addFlag(kColor[0], kColor[1]);
    s.addFlag(kFillColor[0], kFillColor[1], MSyntax::kDouble, MSyntax::kDouble, MSyntax::kDouble, MSyntax::kDouble);
    s.addFlag(kFillChannels[0], kFillChannels[1], MSyntax::kString);
    s.addFlag(kPaintMode[0], kPaintMode[1], MSyntax::kString);
    s.addFlag(kChannelsOf[0], kChannelsOf[1], MSyntax::kString);
    s.addFlag(kSetChannels[0], kSetChannels[1], MSyntax::kString, MSyntax::kLong);
    s.addFlag(kRelink[0], kRelink[1], MSyntax::kString);
    s.addFlag(kCopyFrom[0], kCopyFrom[1], MSyntax::kString);
    s.addFlag(kDeleteFile[0], kDeleteFile[1]);
    s.addFlag(kSelected[0], kSelected[1]);
    s.addFlag(kPaintParam[0], kPaintParam[1], MSyntax::kString, MSyntax::kString);
    s.addFlag(kMapName[0], kMapName[1], MSyntax::kString);
    s.addFlag(kChannel[0], kChannel[1], MSyntax::kString);
    s.addFlag(kMapNode[0], kMapNode[1]);
    s.addFlag(kFaces[0], kFaces[1], MSyntax::kString);
    s.setObjectType(MSyntax::kSelectionList, 0, 1);
    s.useSelectionAsDefault(true);
    s.enableQuery(false);
    s.enableEdit(false);
    return s;
}

MStatus ScalpMapsCmd::doIt(const MArgList& args) {
    return groomlab::maya::guarded("ScalpMapsCmd::doIt", [&]() -> MStatus {
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;

        MDagPath groomPath;
        if (!groomArgument(kName, db, &groomPath)) return MS::kFailure;
        MFnDagNode groomFn(groomPath);
        auto* shape = dynamic_cast<GroomShape*>(groomFn.userNode());
        if (!shape) return fail(kName, "the groom node is not loaded");

        const int actions = int(db.isFlagSet(kList[0])) + int(db.isFlagSet(kKindOf[0])) + int(db.isFlagSet(kAdd[0])) +
                            int(db.isFlagSet(kRemove[0])) + int(db.isFlagSet(kRename[0])) + int(db.isFlagSet(kFill[0])) +
                            int(db.isFlagSet(kConvert[0])) + int(db.isFlagSet(kValues[0])) + int(db.isFlagSet(kChannelsOf[0])) +
                            int(db.isFlagSet(kSetChannels[0])) + int(db.isFlagSet(kRelink[0])) + int(db.isFlagSet(kPaintParam[0]));
        if (actions != 1) return fail(kName, "give exactly one action flag");

        // Questions about the maps: the shape's parses of scalpMaps and guideData (kept while they
        // are unchanged); nothing evaluates.
        if (db.isFlagSet(kList[0]) || db.isFlagSet(kKindOf[0]) || db.isFlagSet(kChannelsOf[0])) {
            std::string error;
            const ScalpMaps& stored = shape->scalpMaps(&error);
            if (!error.empty()) return fail(kName, error.c_str());
            const std::shared_ptr<const GuideHierarchy> layers = shape->storedHierarchy(&error);
            if (!layers) return fail(kName, error.c_str());
            if (db.isFlagSet(kKindOf[0])) {
                setResult(MString(kindOf(stored, *layers, flagString(db, kKindOf[0]).asChar()).c_str()));
            } else if (db.isFlagSet(kChannelsOf[0])) {
                setResult(channelsOf(stored, *layers, flagString(db, kChannelsOf[0]).asChar()));
            } else {
                MStringArray out;
                for (const std::string& layer : layers->attributeNames()) {
                    if (stored.maps.count(layer)) continue;  // a stored map of the same name wins
                    out.append((layer + "|guides||1").c_str());
                }
                for (const auto& [name, m] : stored.maps) {
                    out.append((name + "|" + m.kind + "|" + m.texture + "|" + std::to_string(m.channels == kColorMap ? 4 : 1)).c_str());
                }
                setResult(out);
            }
            return MS::kSuccess;
        }

        // The stored maps, the guides (their layers), and the scalp they are painted on.
        MPlug mapsPlug = groomFn.findPlug("scalpMaps", true);
        MPlug guidesPlug = groomFn.findPlug("guideData", true);
        ScalpMaps maps;
        try {
            maps = ScalpMaps::fromJson(mapsPlug.asString().asChar());
        } catch (const std::exception& e) {
            return fail(kName, e.what());
        }
        const MeshPtr scalp = shape->evaluatedCageScalp();  // vertex maps are stored per vertex of the cage
        refreshMayaPaths();  // texture files resolve against the scene's folder and project as they are now
        std::string error;
        std::shared_ptr<GuideHierarchy> h = toHierarchy(guidesPlug.asString().asChar(), scalp.get(), &error, groomScaleOf(shape->thisMObject()));
        if (!h) return fail(kName, error.c_str());

        if (db.isFlagSet(kValues[0])) {
            const std::string name = flagString(db, kValues[0]).asChar();
            const auto it = maps.maps.find(name);
            if (it == maps.maps.end() || it->second.kind != "vertices") return fail(kName, ("no vertex map named " + name).c_str());
            MDoubleArray out;
            for (const float v : it->second.values) out.append(v);
            setResult(out);
            return MS::kSuccess;
        }

        // Edits: what changes is written back at the end (maps, guides), each undoable.
        noteFileWritten();  // texture files may change: what kept their bytes reads them again
        bool mapsChanged = false, guidesChanged = false, relinked = false;
        const float value = static_cast<float>(std::clamp(db.isFlagSet(kValue[0]) ? [&] { double d = 0; db.getFlagArgument(kValue[0], 0, d); return d; }() : 0.0, 0.0, 1.0));
        int resolution = 1024;
        if (db.isFlagSet(kResolution[0])) db.getFlagArgument(kResolution[0], 0, resolution);
        resolution = std::clamp(resolution, BrushToolSettings::kMinPaintResolution, BrushToolSettings::kMaxPaintResolution);
        const std::string texturePath = flagString(db, kTexture[0]).asChar();
        // A color: -fillColor, else the value as gray (alpha 1).
        PaintColor color{value, value, value, 1.0f};
        if (db.isFlagSet(kFillColor[0])) {
            for (unsigned k = 0; k < 4; ++k) {
                double d = 0.0;
                db.getFlagArgument(kFillColor[0], k, d);
                color[k] = static_cast<float>(std::clamp(d, 0.0, 1.0));
            }
        }
        unsigned fillMask = kAllChannels;
        if (db.isFlagSet(kFillChannels[0])) {
            try {
                fillMask = channelMask(flagString(db, kFillChannels[0]).asChar());
            } catch (const std::invalid_argument& e) {
                return fail(kName, (std::string("-fillChannels: ") + e.what()).c_str());
            }
        }
        PaintMode paintMode = PaintMode::Replace;
        if (db.isFlagSet(kPaintMode[0])) {
            try {
                paintMode = paintModeFromString(flagString(db, kPaintMode[0]).asChar());
            } catch (const std::invalid_argument& e) {
                return fail(kName, (std::string("-paintMode: ") + e.what()).c_str());
            }
        }
        const MObject groomNode = groomPath.node();
        auto resolve = [&](const std::string& stored) { return resolveNodePath(groomNode, stored); };
        // A new file never replaces one that is there (another groom's, or a map removed
        // earlier): it gets a free name beside it, stored the way the given path was.
        auto freshTexture = [&](const std::string& stored) {
            const std::string full = resolve(stored);
            const std::string free = freeFilePath(full);
            return free == full ? stored : portableMayaPath(free);
        };
        const bool makeColor = db.isFlagSet(kColor[0]);
        // -faces: the polygons a make or fill is limited to (none given: everywhere).
        const bool onFaces = db.isFlagSet(kFaces[0]);
        std::vector<std::int32_t> faces;
        if (onFaces) {
            std::istringstream words(flagString(db, kFaces[0]).asChar());
            long long f = 0;
            while (words >> f) {
                if (f >= 0 && f <= std::numeric_limits<std::int32_t>::max()) faces.push_back(static_cast<std::int32_t>(f));
            }
            if (faces.empty()) return fail(kName, "-faces names no face");
            if (!scalp) return fail(kName, "the groom has no scalp mesh");
        }

        try {
            if (db.isFlagSet(kAdd[0])) {
                const std::string name = flagString(db, kAdd[0]).asChar();
                // Every host's New Map (the editor's createMap, map_edits.h): a texture map's image
                // written at -texture (a free name when taken, stored as given), its files' states kept
                // for Maya's undo.
                MapCreateRequest create;
                create.kind = flagString(db, kKind[0], "guides").asChar();
                create.color = makeColor;
                create.value = value;
                if (db.isFlagSet(kFillColor[0])) create.fill = color;
                create.resolution = resolution;
                if (onFaces) create.faces = faces;
                if (create.kind == "texture") {
                    if (texturePath.empty()) return fail(kName, "a texture map needs -texture <file>");
                    create.texturePath = resolve(texturePath);
                }
                MapFiles files;
                files.mapsFolder = assetFolder("masks") + "/" + assetName(groomPath);
                files.resolve = resolve;
                files.store = [&](const std::string& full) {
                    return samePathText(full, create.texturePath) ? texturePath : portableMayaPath(full);
                };
                MapCreate made = createMap(maps, h.get(), scalp.get(), name, create, files);
                if (made.guideLayer) {
                    applyCreatedLayer(*h, name, made);
                    guidesChanged = true;
                } else {
                    maps = std::move(made.maps);
                    mapsChanged = true;
                }
                for (FileState& f : made.files) files_.push_back(std::move(f));
            } else if (db.isFlagSet(kRemove[0]) || db.isFlagSet(kRename[0])) {
                // Every host's Remove and Rename (the editor's map_edits.h): the graph's readers let go
                // of the map or follow it, a texture map's image deleted (-deleteFile: GroomLab's,
                // nothing else reading it) or following the name (moved, or copied when another map
                // or groom reads it), its files' states kept for Maya's undo with the texts.
                MapFiles files;
                files.resolve = resolve;
                files.store = [](const std::string& full) { return portableMayaPath(full); };
                files.shared = [&](const std::string& full) { return fileUsedByOtherGroom(groomNode, full); };
                MPlug graphPlug = groomFn.findPlug("graph", true);
                const std::string graph = graphPlug.asString().asChar();
                std::string graphAfter;
                if (db.isFlagSet(kRemove[0])) {
                    const std::string name = flagString(db, kRemove[0]).asChar();
                    files.mapsFolder = assetFolder("masks") + "/" + assetName(groomPath);
                    MapRemove removed = removeMap(maps, h.get(), graph, name, db.isFlagSet(kDeleteFile[0]), files);
                    if (removed.guideLayer) {
                        h->removeAttribute(name);
                        guidesChanged = true;
                    } else {
                        maps = std::move(removed.maps);
                        mapsChanged = true;
                    }
                    for (FileState& f : removed.files) deleted_.push_back(std::move(f));  // (after: none)
                    graphAfter = std::move(removed.graph);
                    MStringArray unlinked;  // (node, param) pairs that read it, flat
                    for (const auto& [node, param] : removed.unlinked) {
                        unlinked.append(node.c_str());
                        unlinked.append(param.c_str());
                    }
                    setResult(unlinked);
                } else {
                    MString from, to;
                    db.getFlagArgument(kRename[0], 0, from);
                    db.getFlagArgument(kRename[0], 1, to);
                    files.mapsFolder = assetFolder("masks");  // (a duplicate's shared image in another groom's folder too)
                    MapRename renamed = renameMap(maps, h.get(), graph, from.asChar(), to.asChar(), files);
                    if (renamed.guideLayer) {
                        h->renameAttribute(from.asChar(), to.asChar());
                        guidesChanged = true;
                    } else {
                        maps = std::move(renamed.maps);
                        mapsChanged = true;
                    }
                    for (FileState& f : renamed.files) files_.push_back(std::move(f));
                    graphAfter = std::move(renamed.graph);
                    setResult(MString(renamed.file.c_str()));
                }
                if (!graphAfter.empty()) modifier_.newPlugValueString(graphPlug, graphAfter.c_str());
            } else if (db.isFlagSet(kFill[0])) {
                const std::string name = flagString(db, kFill[0]).asChar();
                const bool byColor = db.isFlagSet(kFillColor[0]);
                if (!db.isFlagSet(kValue[0]) && !byColor) return fail(kName, "-fill needs -value or -fillColor");
                // Every host's Fill (the editor's fillMap, map_edits.h): its checks, the fill, a texture
                // map's image rewritten (this groom's own copy when another groom or map reads it), the
                // files' states kept for Maya's undo.
                MapFillRequest fill;
                fill.value = value;
                if (byColor) fill.color = color;
                fill.mask = fillMask;
                fill.mode = paintMode;
                fill.selectedOnly = db.isFlagSet(kSelected[0]);
                if (fill.selectedOnly) fill.selected = readGuideSelection(groomNode);
                if (onFaces) fill.faces = faces;
                fill.resolution = resolution;
                MapFiles files;
                files.mapsFolder = assetFolder("masks") + "/" + assetName(groomPath);
                files.resolve = resolve;
                files.store = [](const std::string& full) { return portableMayaPath(full); };
                files.shared = [&](const std::string& full) { return fileUsedByOtherGroom(groomNode, full); };
                MapFill filled = fillMap(maps, h.get(), scalp.get(), name, fill, files);
                if (filled.kind == "guides") {
                    applyFilledLayer(*h, name, filled);
                    guidesChanged = true;
                } else if (filled.mapsChanged) {
                    maps = std::move(filled.maps);
                    mapsChanged = true;
                }
                if (!filled.file.empty()) {
                    MGlobal::displayInfo(MString("GroomLab: ") + name.c_str() + "'s file is shared; this groom now paints its own copy, " +
                                         resolve(filled.file).c_str());
                }
                for (FileState& f : filled.files) files_.push_back(std::move(f));
            } else if (db.isFlagSet(kConvert[0])) {
                const std::string name = flagString(db, kConvert[0]).asChar();
                const std::string from = kindOf(maps, *h, name);
                const std::string to = flagString(db, kKind[0]).asChar();
                if (from.empty()) return fail(kName, ("no map named " + name).c_str());
                if (to != "guides" && to != "vertices" && to != "texture") return fail(kName, "-kind takes guides, vertices or texture");
                if (!scalp) return fail(kName, "the groom has no scalp mesh");
                if (from == to) {
                    setResult(MString(to.c_str()));
                    return MS::kSuccess;
                }
                // Every host's Convert (the editor's convertMapKind, map_edits.h): the image written at
                // -texture (a free name when taken), stored as Maya stores paths, its files' states
                // kept for Maya's undo; the guide layer onto the hierarchy.
                std::string wanted;
                if (to == "texture") {
                    const std::string given = !texturePath.empty() ? texturePath : maps.maps.count(name) ? maps.maps[name].texture : std::string();
                    if (given.empty()) return fail(kName, "a texture map needs -texture <file>");
                    wanted = resolve(given);
                }
                MapFiles files;
                files.mapsFolder = assetFolder("masks") + "/" + assetName(groomPath);
                files.resolve = resolve;
                files.store = [](const std::string& full) { return portableMayaPath(full); };
                MapConvert converted = convertMapKind(maps, h.get(), *scalp, name, to, resolution, wanted, files);
                if (converted.colorLost) {
                    MGlobal::displayWarning(MString(kName) + ": " + name.c_str() +
                                            " is a color map; on the guides it keeps its luminance (a guide layer is gray)");
                }
                maps = std::move(converted.maps);
                applyConvertedLayer(*h, name, converted);
                for (FileState& f : converted.files) files_.push_back(std::move(f));
                if (to == "guides" || from == "guides") guidesChanged = true;
                mapsChanged = true;
                setResult(MString(to.c_str()));
            } else if (db.isFlagSet(kRelink[0])) {
                // The map reads another image (the editor's relinkMap, map_edits.h, every host's): a
                // PNG in place, or -copyFrom any image copied as a PNG to -texture (a free name), its
                // files' states kept for Maya's undo; texture params reading the old image follow.
                const std::string name = flagString(db, kRelink[0]).asChar();
                if (texturePath.empty()) return fail(kName, "-relink needs -texture <file>");
                MapImageRequest image;
                if (db.isFlagSet(kCopyFrom[0])) {
                    image.path = resolve(flagString(db, kCopyFrom[0]).asChar());
                    image.copy = true;
                    image.copyTo = resolve(texturePath);
                } else {
                    image.path = resolve(texturePath);
                    image.copy = false;
                }
                MapFiles files;
                files.mapsFolder = assetFolder("masks") + "/" + assetName(groomPath);
                files.resolve = resolve;
                const std::string given = resolve(texturePath);
                files.store = [&](const std::string& full) { return samePathText(full, given) ? texturePath : portableMayaPath(full); };
                MPlug graphPlug = groomFn.findPlug("graph", true);
                MapLink linked = relinkMap(maps, h.get(), graphPlug.asString().asChar(), name, image, files);
                if (linked.removeLayer) {
                    h->removeAttribute(name);
                    guidesChanged = true;
                }
                maps = std::move(linked.maps);
                mapsChanged = relinked = true;
                for (FileState& f : linked.files) files_.push_back(std::move(f));
                if (!linked.graph.empty()) modifier_.newPlugValueString(graphPlug, linked.graph.c_str());
                setResult(MString(linked.file.c_str()));
            } else if (db.isFlagSet(kPaintParam[0])) {
                // Every host's Paint on a setting (the editor's paintParam, map_edits.h): the map made
                // (its files' states kept for Maya's undo) and the param's link in one step.
                MString nodeArg, paramArg;
                db.getFlagArgument(kPaintParam[0], 0, nodeArg);
                db.getFlagArgument(kPaintParam[0], 1, paramArg);
                ParamPaintRequest paint;
                paint.kind = flagString(db, kKind[0], "guides").asChar();
                paint.name = flagString(db, kMapName[0]).asChar();
                paint.channel = flagString(db, kChannel[0]).asChar();
                paint.mapNode = db.isFlagSet(kMapNode[0]);
                if (onFaces) paint.faces = faces;
                paint.resolution = resolution;
                MapFiles files;
                files.mapsFolder = assetFolder("masks") + "/" + assetName(groomPath);
                files.resolve = resolve;
                files.store = [](const std::string& full) { return portableMayaPath(full); };
                MPlug graphPlug = groomFn.findPlug("graph", true);
                const std::string graph = graphPlug.asString().asChar();
                ParamPaint painted = paintParam(graph, maps, h.get(), scalp.get(), nodeArg.asChar(), paramArg.asChar(), paint, files);
                if (painted.created.guideLayer) {
                    applyCreatedLayer(*h, painted.map, painted.created);
                    guidesChanged = true;
                } else if (painted.made) {
                    maps = std::move(painted.created.maps);
                    mapsChanged = true;
                }
                for (FileState& f : painted.created.files) files_.push_back(std::move(f));
                if (!painted.graph.empty()) modifier_.newPlugValueString(graphPlug, painted.graph.c_str());
                MStringArray result;
                result.append(painted.map.c_str());
                result.append(painted.kind.c_str());
                result.append(painted.channel.c_str());
                setResult(result);
            } else if (db.isFlagSet(kSetChannels[0])) {
                MString nameArg;
                int wanted = 0;
                db.getFlagArgument(kSetChannels[0], 0, nameArg);
                db.getFlagArgument(kSetChannels[0], 1, wanted);
                const std::string name = nameArg.asChar();
                // Every host's Channels (the editor's setMapChannels, map_edits.h): this groom's own
                // copy of an image another groom reads, the files' states kept for Maya's undo.
                MapFiles files;
                files.mapsFolder = assetFolder("masks") + "/" + assetName(groomPath);
                files.resolve = resolve;
                files.store = [](const std::string& full) { return portableMayaPath(full); };
                files.shared = [&](const std::string& full) { return fileUsedByOtherGroom(groomNode, full); };
                MapChannels changed = setMapChannels(maps, h.get(), scalp.get(), name, wanted, resolution, files);
                if (changed.mapsChanged) {
                    maps = std::move(changed.maps);
                    mapsChanged = true;
                }
                for (FileState& f : changed.files) files_.push_back(std::move(f));
                setResult(changed.channels);
            }
        } catch (const std::exception& e) {
            return fail(kName, e.what());
        }

        if (mapsChanged) modifier_.newPlugValueString(mapsPlug, maps.toJson().c_str());
        if (guidesChanged) modifier_.newPlugValueString(guidesPlug, h->toJson().c_str());
        if ((!mapsChanged && !guidesChanged) || relinked) {
            // A texture rewritten in place, or another image read: the groom reads it again (the
            // Map node's cache key has the file's stamp).
            MPlug tick = groomFn.findPlug("brushTick", true);
            modifier_.newPlugValueInt(tick, tick.asInt() + 1);
        }
        undoable_ = true;
        MStatus done = modifier_.doIt();
        restoreFileStates(deleted_, true);
        return done;
    });
}

MStatus ScalpMapsCmd::redoIt() {
    return groomlab::maya::guarded("ScalpMapsCmd::redoIt", [&]() -> MStatus {
        noteFileWritten();  // texture files may change: what kept their bytes reads them again
        restoreFileStates(files_, true);
        MStatus done = modifier_.doIt();
        restoreFileStates(deleted_, true);
        return done;
    });
}

MStatus ScalpMapsCmd::undoIt() {
    return groomlab::maya::guarded("ScalpMapsCmd::undoIt", [&]() -> MStatus {
        noteFileWritten();  // texture files may change: what kept their bytes reads them again
        restoreFileStates(deleted_, false);  // before the map names it again
        restoreFileStates(files_, false);
        return modifier_.undoIt();
    });
}

}  // namespace groomlab::maya
