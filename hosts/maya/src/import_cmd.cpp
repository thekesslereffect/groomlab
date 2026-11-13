// SPDX-License-Identifier: Apache-2.0
#include "import_cmd.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <tuple>

#include <maya/MArgDatabase.h>
#include <maya/MArgList.h>
#include <maya/MColorArray.h>
#include <maya/MDagModifier.h>
#include <maya/MDagPath.h>
#include <maya/MFloatArray.h>
#include <maya/MFloatPointArray.h>
#include <maya/MFnDagNode.h>
#include <maya/MFnMesh.h>
#include <maya/MFnMeshData.h>
#include <maya/MFnSet.h>
#include <maya/MFnTransform.h>
#include <maya/MGlobal.h>
#include <maya/MIntArray.h>
#include <maya/MPlug.h>
#include <maya/MSelectionList.h>

#include <nlohmann/json.hpp>

#include "asset_files.h"
#include "command_args.h"
#include "guarded.h"
#include "groomlab/asset_files.h"
#include "groomlab/card_files.h"
#include "groomlab/correctives.h"
#include "groomlab/groom_session.h"
#include "groomlab/paths.h"
#include "groomlab/retriangulate.h"
#include "groomlab/scalp_maps.h"
#include "groomlab/scalp_paint.h"
#include "maya_inputs.h"

namespace groomlab::maya {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

const char* kFile[] = {"-f", "-file"};
const char* kInfo[] = {"-i", "-info"};
const char* kCheckScalp[] = {"-cs", "-checkScalp"};
const char* kMakeScalp[] = {"-ms", "-makeScalp"};
const char* kNameFlag[] = {"-n", "-name"};
const char* kRest[] = {"-r", "-rest"};
const char* kCaches[] = {"-cch", "-caches"};
const char* kIndex[] = {"-ix", "-index"};
const char* kExtraScalp[] = {"-sc", "-scalp"};

std::string sceneUpAxis() { return MGlobal::isZAxisUp() ? "Z" : "Y"; }

std::string generic(const std::string& path) { return path.empty() ? path : fs::u8path(path).generic_u8string(); }

bool exists(const std::string& path) {
    std::error_code ec;
    return !path.empty() && fs::exists(fs::u8path(path), ec);
}

// Groom `index` of the file (a file holds one groom or several: groom_file.h), read and turned
// into the scene's up axis (`fileUpAxis`: the axis it was written in; `names`: every groom's name,
// in the file's order). Throws GroomFileError, for a file in another unit than Maya's centimeters
// and an index past its grooms too.
GroomFile readForMaya(const std::string& path, int index = 0, std::string* fileUpAxis = nullptr,
                      std::vector<std::string>* names = nullptr) {
    std::vector<GroomFile> grooms = readGroomFiles(path);
    if (index < 0 || index >= static_cast<int>(grooms.size())) {
        throw GroomFileError(path + " holds " + std::to_string(grooms.size()) + " groom" + (grooms.size() == 1 ? "" : "s") +
                             ", no groom " + std::to_string(index));
    }
    if (names) {
        names->clear();
        for (const GroomFile& g : grooms) names->push_back(g.name);
    }
    GroomFile file = std::move(grooms[static_cast<std::size_t>(index)]);
    if (file.units != "cm") {
        throw GroomFileError(path + ": its units are " + file.units + "; GroomLab imports grooms in centimeters (Maya's own unit)");
    }
    if (fileUpAxis) *fileUpAxis = file.upAxis;
    convertUpAxis(file, sceneUpAxis());
    if (file.cardBake.empty()) {
        // A bake the app wrote beside the groom after saving it (groomlab/<stem>/cards/<stem>/<stem>_*,
        // or earlier versions' places), as the app adopts one: its stamp unknown (every Cards node
        // reads as stale until a bake).
        const CardBakePaths beside = CardBakePaths::foundBesideGroomFile(path);
        if (beside.exists()) file.cardBake.prefix = beside.prefix;
    }
    return file;
}

// A path the file stores, as its readers resolve it: variables expanded, a relative one against
// the groom file's folder, then its search paths (the first that holds it; else the folder).
std::string resolveFromFile(const GroomFile& file, const std::string& stored) {
    const std::string path = expandEnvironment(stored);
    if (path.empty() || isAbsolutePath(path)) return generic(path);
    std::vector<std::string> dirs;
    if (!file.directory.empty()) dirs.push_back(file.directory);
    for (const std::string& s : file.searchPaths) dirs.push_back(expandEnvironment(s));
    if (dirs.empty()) return generic(path);
    return generic(searchPathResolver(std::move(dirs))(path));
}

// Why `mesh` is not the file's scalp as the file's guides, parts and flow are rooted on it ("" when
// it is): as many vertices and the same triangles (faces triangulated as Maya does).
std::string scalpMismatch(const ScalpMesh* mesh, const ScalpMesh* fileScalp) {
    if (!fileScalp) return {};
    if (!mesh) return "there is no mesh";
    if (mesh->pointCount() != fileScalp->pointCount()) {
        return "it has " + std::to_string(mesh->pointCount()) + " vertices and the file's scalp " +
               std::to_string(fileScalp->pointCount());
    }
    // The same polygons split along other diagonals (another host's split of its quads): the
    // groom's roots move onto Maya's triangles (retriangulateGroomData).
    if (isRetriangulation(*fileScalp, *mesh)) return {};
    if (mesh->triangles() != fileScalp->triangles()) {
        return "its faces are not the file's scalp's (another topology or vertex order, " + std::to_string(mesh->triangleCount()) +
               " triangles against " + std::to_string(fileScalp->triangleCount()) + ")";
    }
    return {};
}

std::string mismatchMessage(const std::string& what, const std::string& path, const std::string& reason) {
    return what + " is not the scalp of " + path + ": " + reason +
           ". Import onto the mesh the groom was made on, or import with no mesh selected (the file's scalp is made) "
           "and move the groom with GroomLab > Utilities > Transfer Groom to Scalp.";
}

// A mesh's world-space scalp, by name (null when it is not a mesh).
MeshPtr meshByName(const MString& name) {
    MSelectionList list;
    MDagPath path;
    if (list.add(name) != MS::kSuccess || list.getDagPath(0, path) != MS::kSuccess) return nullptr;
    if (!path.hasFn(MFn::kMesh) && (path.extendToShape() != MS::kSuccess || !path.hasFn(MFn::kMesh))) return nullptr;
    const MPlug world = MFnDagNode(path).findPlug("worldMesh", true).elementByLogicalIndex(path.instanceNumber());
    return toScalpMesh(world.asMObject());
}

// The names of the color sets of the file's scalp that are its own (not painted vertex maps).
std::vector<std::string> ownColorSets(const GroomFile& file) {
    std::vector<std::string> out;
    if (!file.data.scalp) return out;
    for (const std::string& name : file.data.scalp->colorSetNames()) {
        const auto it = file.data.maps.find(name);
        if (it == file.data.maps.end() || it->second.kind != "vertices") out.push_back(name);
    }
    return out;
}

json floats(const std::vector<Vec3f>& points) {
    json out = json::array();
    for (const Vec3f& p : points) out.insert(out.end(), {p.x, p.y, p.z});
    return out;
}

json cacheInfo(const CacheRef& c) {
    if (c.empty()) return nullptr;
    return {{"file", generic(c.file)}, {"object", c.object}, {"found", exists(c.file)}};
}

std::string info(const GroomFile& file, const std::string& fileUpAxis) {
    json out;
    out["upAxis"] = file.upAxis;
    out["fileUpAxis"] = fileUpAxis;
    out["units"] = file.units;
    out["framesPerSecond"] = file.framesPerSecond;
    out["source"] = file.source;
    out["followScalp"] = file.data.followScalp;
    out["scaleSource"] = scaleSourceName(file.data.scaleSource);
    out["scaleTransformReference"] = file.data.scaleTransformReference;
    out["scaleReference"] = file.data.scaleReference;
    out["directory"] = generic(file.directory);
    out["searchPaths"] = file.searchPaths;
    if (const ScalpMesh* s = file.data.scalp.get()) {
        out["scalp"] = {{"points", s->pointCount()}, {"triangles", s->triangleCount()}, {"rest", s->hasRest()},
                        {"uvs", s->hasUVs()}, {"colorSets", ownColorSets(file)}};
        // The groom's other scalps (Add > Scalp's), in the order they join after the scalp.
        json scalps = json::array();
        for (const auto& [name, mesh] : file.data.scalps) {
            if (mesh) scalps.push_back({{"name", name}, {"points", mesh->pointCount()}, {"rest", mesh->hasRest()}});
        }
        out["scalps"] = scalps;
    } else {
        out["scalp"] = nullptr;
    }
    json maps = json::object();
    for (const auto& [name, m] : file.data.maps) {
        json entry = {{"kind", m.kind}, {"channels", m.channels == kColorMap ? 4 : 1}};
        if (m.kind == "texture") {
            const std::string resolved = resolveFromFile(file, m.texture);
            entry["texture"] = resolved;
            entry["found"] = exists(resolved);
        }
        maps[name] = std::move(entry);
    }
    out["maps"] = std::move(maps);
    out["controls"] = file.data.controls;
    out["scalpCache"] = cacheInfo(file.scalpCache);
    out["guideCache"] = cacheInfo(file.guideCache);
    if (!file.cardBake.empty()) {
        out["cardBake"] = {{"prefix", generic(file.cardBake.prefix)}, {"strands", file.cardBake.strands},
                           {"found", exists(file.cardBake.prefix + "Atlas.json")}};
    } else {
        out["cardBake"] = nullptr;
    }
    json correctives = json::array();
    if (!file.data.correctivesJson.empty()) {
        for (const CorrectiveLayer& l : Correctives::fromJson(file.data.correctivesJson).layers) {
            if (l.slot < 0) continue;
            json curve = json::array();
            for (const auto& [t, w] : l.weightCurve) curve.push_back({t, w});
            correctives.push_back({{"name", l.name}, {"slot", l.slot}, {"weight", l.weight}, {"weightCurve", std::move(curve)}});
        }
    }
    out["correctives"] = std::move(correctives);
    json flat = json::array();
    if (file.data.guides) {
        for (const Guide& g : file.data.guides->guides()) flat.push_back({{"id", g.id}, {"points", floats(g.points)}});
    }
    out["flatGuides"] = std::move(flat);
    return out.dump();
}

// The correctives as Maya keeps them: a layer reading one of the groom's weights (a slot) has no
// weight curve (the weight's keys are its curve; groomlab.import_groom keys them).
std::string correctivesForMaya(const std::string& text) {
    if (text.empty()) return text;
    Correctives c = Correctives::fromJson(text);
    bool changed = false;
    for (CorrectiveLayer& l : c.layers) {
        if (l.slot >= 0 && !l.weightCurve.empty()) {
            l.weightCurve.clear();
            changed = true;
        }
    }
    return changed ? c.toJson() : text;
}

}  // namespace

const char* ImportGroomCmd::kName = "groomLabImportGroom";

void* ImportGroomCmd::creator() { return new ImportGroomCmd(); }

MSyntax ImportGroomCmd::newSyntax() {
    MSyntax s;
    s.addFlag(kFile[0], kFile[1], MSyntax::kString);
    s.addFlag(kInfo[0], kInfo[1]);
    s.addFlag(kCheckScalp[0], kCheckScalp[1], MSyntax::kString);
    s.addFlag(kMakeScalp[0], kMakeScalp[1]);
    s.addFlag(kNameFlag[0], kNameFlag[1], MSyntax::kString);
    s.addFlag(kRest[0], kRest[1]);
    s.addFlag(kCaches[0], kCaches[1], MSyntax::kBoolean);
    s.addFlag(kIndex[0], kIndex[1], MSyntax::kLong);
    s.addFlag(kExtraScalp[0], kExtraScalp[1], MSyntax::kString);
    s.setObjectType(MSyntax::kSelectionList, 0, 1);
    s.useSelectionAsDefault(false);
    s.enableQuery(false);
    s.enableEdit(false);
    return s;
}

MStatus ImportGroomCmd::doIt(const MArgList& args) {
    return guarded("ImportGroomCmd::doIt", [&]() -> MStatus {
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;
        if (!db.isFlagSet(kFile[0])) return fail(kName, "-file is required");
        const std::string path = generic(fs::absolute(fs::u8path(flagString(db, kFile[0]).asChar())).u8string());
        if (int(db.isFlagSet(kInfo[0])) + int(db.isFlagSet(kCheckScalp[0])) + int(db.isFlagSet(kMakeScalp[0])) > 1) {
            return fail(kName, "give one of -info, -checkScalp and -makeScalp");
        }
        GroomFile file;
        std::string fileUpAxis;
        std::vector<std::string> names;
        int index = 0;
        if (db.isFlagSet(kIndex[0])) db.getFlagArgument(kIndex[0], 0, index);
        try {
            file = readForMaya(path, index, &fileUpAxis, &names);
        } catch (const std::exception& e) {
            return fail(kName, e.what());
        }

        if (db.isFlagSet(kInfo[0])) {
            try {
                json out = json::parse(info(file, fileUpAxis));
                out["name"] = file.name;
                out["index"] = index;
                out["grooms"] = names;  // every groom's name, in the file's order (one: a file of one groom)
                setResult(MString(out.dump().c_str()));
            } catch (const std::exception& e) {
                return fail(kName, e.what());
            }
            return MS::kSuccess;
        }
        if (db.isFlagSet(kCheckScalp[0])) {
            const MString mesh = flagString(db, kCheckScalp[0]);
            const MeshPtr scalp = meshByName(mesh);
            if (!scalp) return fail(kName, mesh + " is not a mesh");
            setResult(MString(scalpMismatch(scalp.get(), file.data.scalp.get()).c_str()));
            return MS::kSuccess;
        }
        if (db.isFlagSet(kMakeScalp[0]) && db.isFlagSet(kExtraScalp[0])) {
            // One of the groom's other scalps (Add > Scalp's), as a mesh of its own.
            const std::string scalpName = flagString(db, kExtraScalp[0]).asChar();
            MeshPtr extra;
            for (const auto& [name, mesh] : file.data.scalps) {
                if (name == scalpName) extra = mesh;
            }
            if (!extra || extra->triangleCount() == 0) return fail(kName, MString(path.c_str()) + " holds no scalp named " + scalpName.c_str());
            const bool rest = db.isFlagSet(kRest[0]);
            if (rest && !extra->hasRest()) return fail(kName, MString(scalpName.c_str()) + " has no rest pose in " + path.c_str());
            mesh_ = rest ? std::make_shared<const ScalpMesh>(extra->restPose()) : extra;
            colorSets_.clear();
            for (const std::string& set : extra->colorSetNames()) {
                const auto it = file.data.maps.find(set);
                if (it == file.data.maps.end() || it->second.kind != "vertices") colorSets_.push_back(set);
            }
            exactPolygons_ = true;
            name_ = flagString(db, kNameFlag[0], (scalpName + (rest ? "ScalpRest" : "Scalp")).c_str());
            making_ = undoable_ = true;
            return redoIt();
        }
        if (db.isFlagSet(kMakeScalp[0])) {
            if (!file.data.scalp || file.data.scalp->triangleCount() == 0) return fail(kName, MString(path.c_str()) + " holds no scalp");
            const bool rest = db.isFlagSet(kRest[0]);
            if (rest && !file.data.scalp->hasRest()) return fail(kName, MString(path.c_str()) + "'s scalp has no rest pose");
            mesh_ = rest ? std::make_shared<const ScalpMesh>(file.data.scalp->restPose()) : file.data.scalp;
            colorSets_ = ownColorSets(file);
            // With other scalps the roots index the scalps' triangles joined: the scalp keeps the
            // file's triangles unless Maya splits its polygons as the file does.
            exactPolygons_ = !file.data.scalps.empty();
            name_ = flagString(db, kNameFlag[0], (fs::u8path(path).stem().u8string() + (rest ? "ScalpRest" : "Scalp")).c_str());
            making_ = undoable_ = true;
            return redoIt();
        }

        // Applying the file to a groom.
        MDagPath groomPath;
        if (!groomArgument(kName, db, &groomPath)) return MS::kFailure;
        const MObject node = groomPath.node();
        MFnDependencyNode fn(node);
        const MeshPtr scalp = rawScalpFromNode(node);
        if (!scalp) return fail(kName, groomPath.partialPathName() + " has no scalp mesh connected");
        std::string mismatch = scalpMismatch(scalp.get(), file.data.scalp.get());
        if (mismatch.empty() && file.data.scalp && !file.data.scalps.empty() && isRetriangulation(*file.data.scalp, *scalp)) {
            mismatch = "its quads are split along other diagonals than the file's, and the groom has other scalps";
        }
        // The file's quads split along other diagonals than Maya's: the roots onto Maya's triangles
        // (a groom of one scalp: with others the roots index them joined, and the scalp was made of
        // the file's own triangles).
        if (mismatch.empty() && file.data.scalp && file.data.scalps.empty() && isRetriangulation(*file.data.scalp, *scalp)) {
            try {
                retriangulateGroomData(file.data, *file.data.scalp, *scalp);
            } catch (const std::exception& e) {
                return fail(kName, e.what());
            }
            file.data.scalpFingerprint = scalp->topologyFingerprint();
        }
        if (!mismatch.empty()) {
            return fail(kName, mismatchMessage(std::string(groomPath.partialPathName().asChar()) + "'s scalp", path, mismatch).c_str());
        }
        refreshMayaPaths();
        const std::string asset = assetName(groomPath);
        json report = {{"copied", json::array()}, {"missing", json::array()}, {"bake", ""}};

        // A file copied for the groom: none when the destination holds the same bytes already.
        auto copyFor = [&](const std::string& from, const std::string& to) {
            std::vector<std::uint8_t> bytes = fileBytes(from);
            if (exists(to) && fileBytes(to) == bytes) return;
            report["copied"].push_back(to);
            written_.push_back({to, std::move(bytes)});
        };

        try {
            // Painted maps: the vertex maps from the scalp's color sets; each texture map's image
            // in the groom's masks folder (copied there, unless it is that folder's already).
            ScalpMaps maps = scalpMapsFromGroomData(file.data);
            const std::string masks = assetFolder("masks") + "/" + asset;
            // (the image as the file found it, where the map reads it now): a texture param naming
            // the image (the app's and Blender's painted params read their map by its file) reads
            // the copy the groom paints, not the file's.
            std::vector<std::pair<std::string, std::string>> movedImages;
            for (auto& [name, entry] : maps.maps) {
                if (entry.kind != "texture") continue;
                const std::string from = resolveFromFile(file, entry.texture);
                if (!exists(from)) {
                    report["missing"].push_back(name);
                    MGlobal::displayWarning(MString(kName) + ": " + name.c_str() + "'s image " + from.c_str() +
                                            " is not there; the map names it as it is");
                    entry.texture = portableMayaPath(from);
                    continue;
                }
                std::string to = masks + "/" + name + fs::u8path(from).extension().u8string();
                if (sameFile(fs::u8path(from).parent_path().u8string(), masks)) {
                    to = generic(from);  // the groom's own (the file came from this scene): read in place
                } else if (exists(to) && fileBytes(to) != fileBytes(from)) {
                    to = freeFilePath(to);
                }
                if (!sameFile(from, to)) copyFor(from, to);
                entry.texture = portableMayaPath(to);
                if (!sameFile(from, to)) movedImages.emplace_back(from, entry.texture);
            }
            std::string graphJson = file.data.graphJson.empty() ? "{}" : file.data.graphJson;
            if (!movedImages.empty()) {
                graphJson = followGraphFiles(graphJson, [&](const std::string& stored) {
                    const std::string full = resolveFromFile(file, stored);
                    for (const auto& [from, now] : movedImages) {
                        if (sameFile(full, from)) return now;
                    }
                    return stored;
                });
            }

            // The card bake: its files in the groom's cards folder (a free name when another bake
            // is there), cardMaps naming them with the file's stamp.
            std::string cardMaps;
            if (!file.cardBake.empty()) {
                const std::string from = generic(file.cardBake.prefix);
                if (!CardBakePaths(from).exists()) {
                    MGlobal::displayWarning(MString(kName) + ": the card bake " + from.c_str() +
                                            "* is not there; the groom keeps its own bake");
                } else {
                    const std::string folder = assetFolder("cards") + "/" + asset;
                    std::string to;
                    for (int n = 1;; ++n) {
                        to = folder + "/" + asset + (n > 1 ? "_" + std::to_string(n) : std::string()) + "_";
                        if (sameFile(from + "Atlas.json", to + "Atlas.json")) break;
                        bool free = !bakePrefixUsedByOtherGroom(node, to);
                        for (const std::string& f : cardBakeSuffixes()) {
                            free = free && (!exists(to + f) || !exists(from + f) || fileBytes(to + f) == fileBytes(from + f));
                        }
                        if (free) break;
                    }
                    for (const std::string& f : cardBakeSuffixes()) {
                        if (exists(from + f) && !sameFile(from + f, to + f)) copyFor(from + f, to + f);
                    }
                    static int bakes = 0;  // a new stamp each time: the viewport reads the maps again
                    cardMaps = formatCardMaps(portableMayaPath(to), file.cardBake.strands, 1000000 + ++bakes);
                    report["bake"] = to;
                }
            }

            modifier_.newPlugValueString(fn.findPlug("graph", true), graphJson.c_str());
            modifier_.newPlugValueString(fn.findPlug("guideData", true), file.data.hierarchyJson.c_str());
            modifier_.newPlugValueString(fn.findPlug("partData", true), file.data.partsJson.c_str());
            modifier_.newPlugValueString(fn.findPlug("flowData", true), file.data.flowJson.c_str());
            modifier_.newPlugValueString(fn.findPlug("correctiveData", true), correctivesForMaya(file.data.correctivesJson).c_str());
            modifier_.newPlugValueString(fn.findPlug("sculptData", true), file.data.sculptsJson.c_str());
            modifier_.newPlugValueString(fn.findPlug("scalpMaps", true), maps.toJson().c_str());
            if (!cardMaps.empty()) modifier_.newPlugValueString(fn.findPlug("cardMaps", true), cardMaps.c_str());
            modifier_.newPlugValueBool(fn.findPlug("followScalp", true), file.data.followScalp);
            // Scaling with the scalp: what it follows and what it is 1x at (none: the first measured).
            modifier_.newPlugValueShort(fn.findPlug("scaleSource", true), static_cast<short>(file.data.scaleSource));
            modifier_.newPlugValueDouble(fn.findPlug("scaleTransformReference", true), file.data.scaleTransformReference);
            modifier_.newPlugValueDouble(fn.findPlug("scaleReference", true), file.data.scaleReference);
            modifier_.newPlugValueString(fn.findPlug("cardBlock", true), file.cardBlock.c_str());
            // The last transfer's markers, for the next transfer.
            modifier_.newPlugValueString(fn.findPlug("transferMarkers", true),
                                         file.transferMarkers.empty() ? "" : transferMarkersToJson(file.transferMarkers).c_str());
            // The scalp the groom was made on: a scalp of another topology says so (GroomSession).
            modifier_.newPlugValueString(fn.findPlug("scalpTopology", true),
                                         file.data.scalpFingerprint ? scalpFingerprintText(file.data.scalpFingerprint).c_str() : "");
            bool caches = true;
            if (db.isFlagSet(kCaches[0])) db.getFlagArgument(kCaches[0], 0, caches);
            if (caches && !file.guideCache.empty()) {
                modifier_.newPlugValueString(fn.findPlug("guideCache", true), portableMayaPath(generic(file.guideCache.file)).c_str());
                modifier_.newPlugValueString(fn.findPlug("guideCacheObject", true), file.guideCache.object.c_str());
            }
            if (caches && !file.simCaches.empty()) {
                // The Simulate nodes' bakes, stored as the scene stores paths.
                std::map<std::string, SimCacheRef> sims = file.simCaches;
                for (auto& [node, ref] : sims) ref.file = portableMayaPath(generic(ref.file));
                modifier_.newPlugValueString(fn.findPlug("simCaches", true), simCachesToJson(sims).c_str());
            }
            // Linked to the file it came from (Reload, Save to Groom File: its own entry, by name; the
            // file's other grooms stay).
            modifier_.newPlugValueString(fn.findPlug("groomFile", true), portableMayaPath(path).c_str());
            const MPlug tick = fn.findPlug("brushTick", true);  // files read again (a map's image rewritten in place)
            modifier_.newPlugValueInt(tick, tick.asInt() + 1);
        } catch (const std::exception& e) {
            return fail(kName, e.what());
        }
        setResult(MString(report.dump().c_str()));
        undoable_ = true;
        return redoIt();
    });
}

MStatus ImportGroomCmd::makeScalp() {
    // The nodes come from a modifier (undo takes them out, redo puts the same nodes back): the
    // commands after this one in import_groom's undo step (the groom's connections, a rest mesh
    // hidden) name these very nodes on redo.
    MStatus status;
    const ScalpMesh& m = *mesh_;
    const MObject transform = scalpModifier_.createNode("transform", MObject::kNullObj, &status);
    if (!status) return status;
    const MObject shape = scalpModifier_.createNode("mesh", transform, &status);
    if (!status) return status;
    scalpModifier_.renameNode(transform, name_);
    scalpModifier_.renameNode(shape, name_ + "Shape");
    status = scalpModifier_.doIt();
    if (!status) return status;
    made_ = MObjectHandle(transform);

    // The file's polygons (quads and n-gons, as the mesh it was made on had them: the app keeps
    // them), in its order and corner order; Maya splits them as it does, and where that is not the
    // file's split the import moves the roots onto Maya's triangles (retriangulateGroomData). A
    // file without polygons: a face per triangle, the guides' roots landing where they were.
    MFloatPointArray points(static_cast<unsigned>(m.pointCount()));
    for (std::size_t i = 0; i < m.pointCount(); ++i) {
        const Vec3f& p = m.points()[i];
        points.set(static_cast<unsigned>(i), p.x, p.y, p.z);
    }
    PolygonLoops loops = polygonLoops(m);
    if (exactPolygons_ && !loops.exact) loops = {};  // (an extra scalp: its triangles, where they were)
    if (loops.vertices.empty()) {
        for (std::size_t t = 0; t < m.triangleCount(); ++t) {
            loops.vertices.push_back({m.triangles()[t][0], m.triangles()[t][1], m.triangles()[t][2]});
            loops.corners.push_back({static_cast<int>(3 * t), static_cast<int>(3 * t + 1), static_cast<int>(3 * t + 2)});
        }
    }
    const auto faceCount = static_cast<unsigned>(loops.vertices.size());
    MIntArray counts(faceCount), connects, faces;
    std::vector<int> cornerOf;  // each face-vertex's triangle corner in the file
    for (unsigned f = 0; f < faceCount; ++f) {
        counts[f] = static_cast<int>(loops.vertices[f].size());
        for (std::size_t k = 0; k < loops.vertices[f].size(); ++k) {
            connects.append(loops.vertices[f][k]);
            faces.append(static_cast<int>(f));
            cornerOf.push_back(loops.corners[f][k]);
        }
    }
    const unsigned faceVertices = connects.length();
    // A UV per vertex and value (corners of a vertex with the same UV share it: seams stay seams).
    MFloatArray us, vs;
    MIntArray uvIds;
    if (m.hasUVs()) {
        std::map<std::tuple<int, float, float>, int> ids;
        uvIds.setLength(faceVertices);
        for (unsigned c = 0; c < faceVertices; ++c) {
            const ScalpMesh::UV& uv = m.cornerUVs()[static_cast<std::size_t>(cornerOf[c])];
            const auto key = std::make_tuple(connects[c], uv[0], uv[1]);
            auto it = ids.find(key);
            if (it == ids.end()) {
                it = ids.emplace(key, static_cast<int>(us.length())).first;
                us.append(uv[0]);
                vs.append(uv[1]);
            }
            uvIds[c] = it->second;
        }
    }
    // The mesh is made as data and given to the shape (a mesh with no input keeps it as its own).
    MFnMeshData dataFn;
    MObject data = dataFn.create(&status);
    if (!status) return status;
    MFnMesh meshFn;
    meshFn.create(static_cast<int>(points.length()), static_cast<int>(faceCount), points, counts, connects, us, vs, data, &status);
    if (!status) return status;
    if (m.hasUVs()) meshFn.assignUVs(counts, uvIds);
    // The scalp's own color sets (painted maps go to the groom's scalpMaps instead).
    for (const std::string& set : colorSets_) {
        const std::vector<ScalpMesh::Color>* corners = m.cornerColors(set);
        if (!corners || corners->size() != m.triangleCount() * 3) continue;
        MString setName(set.c_str());
        setName = meshFn.createColorSetWithName(setName);
        meshFn.setCurrentColorSetName(setName);
        MColorArray colors(faceVertices);
        for (unsigned c = 0; c < faceVertices; ++c) {
            const ScalpMesh::Color& col = (*corners)[static_cast<std::size_t>(cornerOf[c])];
            colors.set(MColor(col[0], col[1], col[2], col[3]), c);
        }
        meshFn.setFaceVertexColors(colors, faces, connects);
    }
    status = MFnDagNode(shape).findPlug("outMesh", true).setMObject(data);
    if (!status) return status;
    // The default shader, by the modifier too (undone and redone with the nodes).
    MDagPath shapePath;
    MDagPath::getAPathTo(shape, shapePath);
    scalpModifier_.commandToExecute("sets -edit -forceElement initialShadingGroup \"" + shapePath.fullPathName() + "\"");
    return scalpModifier_.doIt();
}

MStatus ImportGroomCmd::redoIt() {
    return guarded("ImportGroomCmd::redoIt", [&]() -> MStatus {
        if (making_) {
            // The first time the mesh is made; after an undo the modifier puts the same nodes back.
            const MStatus built = made_.isValid() ? scalpModifier_.doIt() : makeScalp();
            if (!built) return fail(kName, "could not make the scalp mesh");
            MDagPath path;
            MDagPath::getAPathTo(made_.object(), path);
            clearResult();
            setResult(path.fullPathName());
            return MS::kSuccess;
        }
        noteFileWritten();  // texture files may change: what kept their bytes reads them again
        for (const Written& w : written_) putFileBytes(w.path, w.bytes, kName);
        return modifier_.doIt();
    });
}

MStatus ImportGroomCmd::undoIt() {
    return guarded("ImportGroomCmd::undoIt", [&]() -> MStatus {
        if (making_) return scalpModifier_.undoIt();
        const MStatus status = modifier_.undoIt();
        noteFileWritten();
        for (const Written& w : written_) {
            std::error_code ec;
            fs::remove(fs::u8path(w.path), ec);
        }
        return status;
    });
}

}  // namespace groomlab::maya
