// SPDX-License-Identifier: Apache-2.0
#include "transfer_cmd.h"

#include <filesystem>
#include <memory>

#include <maya/MArgDatabase.h>
#include <maya/MArgList.h>
#include <maya/MDagPath.h>
#include <maya/MColorArray.h>
#include <maya/MFnDagNode.h>
#include <maya/MFnMesh.h>
#include <maya/MIntArray.h>
#include <maya/MGlobal.h>
#include <maya/MPlug.h>
#include <maya/MSelectionList.h>
#include <maya/MStringArray.h>

#include "asset_files.h"
#include "command_args.h"
#include "guarded.h"
#include "groom_shape.h"
#include "groomlab/flow.h"
#include "groomlab/groom_file.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/parts.h"
#include "groomlab/scalp_paint.h"
#include "groomlab/asset_files.h"
#include "groomlab/texture.h"
#include "maya_inputs.h"
#include "groomlab/scalp_maps.h"

namespace groomlab::maya {

namespace {

const char* kScalp[] = {"-sc", "-scalp"};
const char* kFit[] = {"-fb", "-fitBounds"};
const char* kMaps[] = {"-mps", "-maps"};
const char* kMapsFrom[] = {"-mfr", "-mapsFrom"};
const char* kMap[] = {"-mn", "-mapName"};  // "-mp"/"-map" are refused by MSyntax
const char* kMethod[] = {"-mth", "-method"};
const char* kMarkers[] = {"-mk", "-markers"};
const char* kKeepLengths[] = {"-kl", "-keepLengths"};
const char* kSnapMarkers[] = {"-snm", "-snapMarkers"};

// The mesh shape `name` names (a transform or the shape).
bool meshPath(const MString& name, MDagPath* path) {
    MSelectionList sel;
    if (sel.add(name) != MS::kSuccess || sel.getDagPath(0, *path) != MS::kSuccess) return false;
    return path->hasFn(MFn::kMesh) || (path->extendToShape() == MS::kSuccess && path->hasFn(MFn::kMesh));
}

}  // namespace

const char* TransferCmd::kName = "groomLabTransfer";

void* TransferCmd::creator() { return new TransferCmd(); }

MSyntax TransferCmd::newSyntax() {
    MSyntax s;
    s.addFlag(kScalp[0], kScalp[1], MSyntax::kString);
    s.addFlag(kFit[0], kFit[1]);
    s.addFlag(kMaps[0], kMaps[1]);
    s.addFlag(kMapsFrom[0], kMapsFrom[1], MSyntax::kString);
    s.addFlag(kMap[0], kMap[1], MSyntax::kString);
    s.makeFlagMultiUse(kMap[0]);
    s.addFlag(kMethod[0], kMethod[1], MSyntax::kString);
    s.addFlag(kMarkers[0], kMarkers[1], MSyntax::kString);
    s.addFlag(kKeepLengths[0], kKeepLengths[1]);
    s.addFlag(kSnapMarkers[0], kSnapMarkers[1]);
    s.setObjectType(MSyntax::kSelectionList, 1, 1);
    s.useSelectionAsDefault(true);
    s.enableQuery(false);
    s.enableEdit(false);
    return s;
}

std::vector<std::string> TransferCmd::projectMaps(ScalpMaps& maps, const std::set<std::string>& only, const ScalpMapping& mapping,
                                                  const MObject& sourceNode, const MDagPath& groom) {
    // The projection is the core's (projectScalpMaps, the app's too); this host says where a
    // texture's file is and writes a projected image beside the scene, kept for undo.
    MapProjectionFiles files;
    files.resolve = [&sourceNode](const std::string& stored) { return resolveNodePath(sourceNode, stored); };
    files.write = [this, &groom](const std::string& name, const TexturePaint& paint) {
        const std::string folder = assetFolder("masks") + "/" + assetName(groom);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(folder), ec);
        const std::string target = freeFilePath(folder + "/" + name + ".png");
        paint.save(target);
        written_.push_back({target, fileBytes(target)});
        return portableMayaPath(target);
    };
    std::vector<std::string> warnings;
    const std::vector<std::string> projected = projectScalpMaps(maps, only, mapping, files, &warnings);
    for (const std::string& warning : warnings) MGlobal::displayWarning(MString(kName) + ": " + warning.c_str());
    return projected;
}

MStatus TransferCmd::mapsFromOtherGroom(const MDagPath& groom, const std::string& source, const std::set<std::string>& only,
                                        TransferMethod method) {
    if (usesMarkers(method)) return fail(kName, "maps from another groom are projected by closest, bounds or uv");
    MSelectionList sel;
    MDagPath sourcePath;
    if (sel.add(source.c_str()) != MS::kSuccess || sel.getDagPath(0, sourcePath) != MS::kSuccess) {
        return fail(kName, (source + " does not exist").c_str());
    }
    MFnDagNode sourceFn(sourcePath);
    if (!dynamic_cast<GroomShape*>(sourceFn.userNode()) && sourcePath.extendToShape() == MS::kSuccess) sourceFn.setObject(sourcePath);
    if (!dynamic_cast<GroomShape*>(sourceFn.userNode())) return fail(kName, (source + " is not a groom").c_str());
    MFnDagNode fn(groom);
    const MeshPtr from = toScalpMesh(sourceFn.findPlug("inMesh", true).asMObject());
    const MeshPtr to = toScalpMesh(fn.findPlug("inMesh", true).asMObject());
    if (!from || from->triangleCount() == 0) return fail(kName, (source + " has no scalp to project maps from").c_str());
    if (!to || to->triangleCount() == 0) return fail(kName, "the groom has no scalp to project maps onto");
    ScalpMaps theirs, mine;
    MPlug mapsPlug = fn.findPlug("scalpMaps", true);
    try {
        theirs = ScalpMaps::fromJson(sourceFn.findPlug("scalpMaps", true).asString().asChar());
        mine = ScalpMaps::fromJson(mapsPlug.asString().asChar());
    } catch (const std::exception& e) {
        return fail(kName, e.what());
    }
    for (const std::string& name : only) {
        if (!theirs.maps.count(name)) MGlobal::displayWarning(MString(kName) + ": " + source.c_str() + " has no scalp map named " + name.c_str());
    }
    std::vector<std::string> projected;
    try {
        TransferOptions options;
        options.method = method;
        projected = projectMaps(theirs, only, ScalpMapping(*from, *to, options), sourcePath.node(), groom);
    } catch (const std::exception& e) {
        return fail(kName, e.what());
    }
    MStringArray result;
    for (const std::string& name : projected) {
        mine.maps[name] = theirs.maps[name];
        result.append(name.c_str());
    }
    if (!projected.empty()) modifier_.newPlugValueString(mapsPlug, mine.toJson().c_str());
    undoable_ = true;
    setResult(result);
    return redoIt();
}

MStatus TransferCmd::doIt(const MArgList& args) {
    return groomlab::maya::guarded("TransferCmd::doIt", [&]() -> MStatus {
        noteFileWritten();  // texture files may change: what kept their bytes reads them again
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;
        MDagPath groom;
        if (!groomArgument(kName, db, &groom)) return MS::kFailure;
        MFnDagNode fn(groom);
        refreshMayaPaths();  // texture maps' files resolve against the scene's folder and project as they are now

        if (db.isFlagSet(kMapsFrom[0])) {
            std::set<std::string> only;
            for (const MString& map : multiStrings(db, kMap[0])) only.insert(map.asChar());
            TransferMethod method = db.isFlagSet(kFit[0]) ? TransferMethod::Bounds : TransferMethod::Closest;
            try {
                if (db.isFlagSet(kMethod[0])) method = transferMethodFromName(flagString(db, kMethod[0]).asChar());
            } catch (const std::exception& e) {
                return fail(kName, e.what());
            }
            return mapsFromOtherGroom(groom, flagString(db, kMapsFrom[0]).asChar(), only, method);
        }
        if (!db.isFlagSet(kScalp[0])) return fail(kName, "give the new scalp with -scalp");

        MString scalpName;
        db.getFlagArgument(kScalp[0], 0, scalpName);
        MDagPath scalpPath;
        {
            MSelectionList scalpSel;
            if (scalpSel.add(scalpName) != MS::kSuccess) return fail(kName, scalpName + " does not exist");
        }
        if (!meshPath(scalpName, &scalpPath)) return fail(kName, scalpName + " is not a mesh");
        MFnDagNode scalpFn(scalpPath);
        MPlug worldMesh = scalpFn.findPlug("worldMesh", true).elementByLogicalIndex(scalpPath.instanceNumber());

        MPlug inMesh = fn.findPlug("inMesh", true);
        const MPlug oldSource = inMesh.source();
        if (!oldSource.isNull() && oldSource == worldMesh) return fail(kName, "the groom is already on " + scalpName);
        const MeshPtr from = toScalpMesh(inMesh.asMObject());
        const MeshPtr to = toScalpMesh(worldMesh.asMObject());
        if (!from || from->triangleCount() == 0) return fail(kName, "the groom has no scalp to move from");
        if (!to || to->triangleCount() == 0) return fail(kName, scalpName + " has no faces");

        // The method and its markers: given (snapped onto the meshes, stored on the groom), else
        // the groom's stored ones.
        TransferOptions options;
        options.method = db.isFlagSet(kFit[0]) ? TransferMethod::Bounds : TransferMethod::Closest;
        options.keepLengths = db.isFlagSet(kKeepLengths[0]);
        MPlug markersPlug = fn.findPlug("transferMarkers", true);
        std::string markersJson;
        try {
            if (db.isFlagSet(kMethod[0])) options.method = transferMethodFromName(flagString(db, kMethod[0]).asChar());
            if (db.isFlagSet(kMarkers[0])) {
                options.markers = snapTransferMarkers(transferMarkersFromJson(flagString(db, kMarkers[0]).asChar()), *from, *to);
                markersJson = transferMarkersToJson(options.markers);
            } else if (usesMarkers(options.method)) {
                options.markers = transferMarkersFromJson(markersPlug.asString().asChar());
            }
        } catch (const std::exception& e) {
            return fail(kName, e.what());
        }
        if (db.isFlagSet(kSnapMarkers[0])) {
            setResult(MString(markersJson.c_str()));
            return MS::kSuccess;  // nothing changed: not undoable
        }
        std::vector<std::string> mappingWarnings;
        std::unique_ptr<ScalpMapping> mapping;
        try {
            mapping = std::make_unique<ScalpMapping>(*from, *to, options, &mappingWarnings);
        } catch (const std::exception& e) {
            return fail(kName, e.what());
        }
        for (const std::string& warning : mappingWarnings) MGlobal::displayWarning(MString(kName) + ": " + warning.c_str());
        MPlug guidePlug = fn.findPlug("guideData", true);
        MPlug partPlug = fn.findPlug("partData", true);
        MPlug flowPlug = fn.findPlug("flowData", true);
        MPlug mapsPlug = fn.findPlug("scalpMaps", true);
        std::string guideJson, partJson, flowJson, mapsJson;
        std::size_t moved = 0;
        try {
            const std::string stored = guidePlug.asString().asChar();
            if (!stored.empty()) {
                GuideHierarchy h = GuideHierarchy::fromJson(stored);
                h.setScale(groomScaleOf(groom.node()));  // moved as shown (the groom keeps its scale: transfer_groom)
                h.transfer(*mapping);
                moved = h.guides().size();
                guideJson = h.toJson();
            }
            const PartSet parts = PartSet::fromJson(partPlug.asString().asChar());
            if (!parts.empty()) {
                const PartSet out = parts.transferred(*mapping);
                if (out.parts.size() < parts.parts.size()) {
                    MGlobal::displayWarning(MString(kName) + ": some parts fell apart on the new scalp and were dropped");
                }
                partJson = out.empty() ? std::string() : out.toJson();
            }
            const FlowSet flow = FlowSet::fromJson(flowPlug.asString().asChar());
            if (!flow.empty()) {
                const FlowSet out = flow.transferred(*mapping);
                if (out.pins.size() < flow.pins.size() || out.sinks.size() < flow.sinks.size()) {
                    MGlobal::displayWarning(MString(kName) + ": some flow arrows did not fit the new scalp and were dropped");
                }
                flowJson = out.empty() ? std::string() : out.toJson();
            }
            if (mapping->uvMisses() > 0) {
                MGlobal::displayWarning(MString(kName) + ": " + std::to_string(mapping->uvMisses()).c_str() +
                                        " roots found no matching UV on the new scalp and went to the closest point");
            }
            if (db.isFlagSet(kMaps[0])) {
                ScalpMaps maps = ScalpMaps::fromJson(mapsPlug.asString().asChar());
                if (!projectMaps(maps, {}, *mapping, groom.node(), groom).empty()) mapsJson = maps.toJson();
            }
        } catch (const std::exception& e) {
            return fail(kName, e.what());
        }

        for (const char* live : {"guideCurves", "simGuides"}) {
            MPlug p = fn.findPlug(live, true);
            const bool connected = p.isArray() ? p.numConnectedElements() > 0 : p.isConnected();
            if (connected) {
                MGlobal::displayWarning(MString(kName) + ": " + live +
                                        " are connected and stay where they are; re-create them for the new scalp");
            }
        }

        if (!oldSource.isNull()) modifier_.disconnect(oldSource, inMesh);
        modifier_.connect(worldMesh, inMesh);
        MPlug restMesh = fn.findPlug("restMesh", true);
        if (!restMesh.source().isNull()) modifier_.disconnect(restMesh.source(), restMesh);
        // Moved on purpose: the new scalp is the groom's (its topology, as the guides are rooted on it now).
        modifier_.newPlugValueString(fn.findPlug("scalpTopology", true), scalpFingerprintText(to->topologyFingerprint()).c_str());
        if (!guideJson.empty()) modifier_.newPlugValueString(guidePlug, guideJson.c_str());
        if (partPlug.asString().length() > 0) modifier_.newPlugValueString(partPlug, partJson.c_str());
        if (flowPlug.asString().length() > 0) modifier_.newPlugValueString(flowPlug, flowJson.c_str());
        if (!mapsJson.empty()) modifier_.newPlugValueString(mapsPlug, mapsJson.c_str());
        if (!markersJson.empty()) modifier_.newPlugValueString(markersPlug, markersJson.c_str());
        undoable_ = true;
        setResult(static_cast<int>(moved));
        return redoIt();
    });
}

MStatus TransferCmd::redoIt() {
    return groomlab::maya::guarded("TransferCmd::redoIt", [&]() -> MStatus {
        noteFileWritten();  // texture files may change: what kept their bytes reads them again
        for (const WrittenFile& file : written_) putFileBytes(file.path, file.bytes, kName);
        return modifier_.doIt();
    });
}

MStatus TransferCmd::undoIt() {
    return groomlab::maya::guarded("TransferCmd::undoIt", [&]() -> MStatus {
        noteFileWritten();  // texture files may change: what kept their bytes reads them again
        const MStatus status = modifier_.undoIt();
        std::error_code ec;
        for (const WrittenFile& file : written_) std::filesystem::remove(std::filesystem::u8path(file.path), ec);
        return status;
    });
}

// ---------------------------------------------------------------------------

namespace {
const char* kSource[] = {"-src", "-source"};
const char* kColorSet[] = {"-cs", "-colorSet"};
}  // namespace

struct CopyColorSetsCmd::ColorSet {
    MString name;
    MColorArray colors;  // one per face-vertex, in face-vertex order
    MFnMesh::MColorRepresentation representation = MFnMesh::kRGBA;
    bool clamped = false;
};

const char* CopyColorSetsCmd::kName = "groomLabCopyColorSets";

void* CopyColorSetsCmd::creator() { return new CopyColorSetsCmd(); }

MSyntax CopyColorSetsCmd::newSyntax() {
    MSyntax s;
    s.addFlag(kSource[0], kSource[1], MSyntax::kString);
    s.addFlag(kColorSet[0], kColorSet[1], MSyntax::kString);
    s.makeFlagMultiUse(kColorSet[0]);
    s.setObjectType(MSyntax::kStringObjects, 1, 1);
    return s;
}

MStatus CopyColorSetsCmd::doIt(const MArgList& args) {
    return groomlab::maya::guarded("CopyColorSetsCmd::doIt", [&]() -> MStatus {
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;
        MStringArray objects;
        db.getObjects(objects);
        MDagPath source;
        if (objects.length() != 1 || !meshPath(objects[0], &target_)) return fail(kName, "give the mesh to copy onto");
        if (!db.isFlagSet(kSource[0]) || !meshPath(flagString(db, kSource[0]), &source)) {
            return fail(kName, "give the mesh to copy from with -source");
        }
        MFnMesh from(source), to(target_);
        if (from.numFaceVertices() != to.numFaceVertices() || from.numPolygons() != to.numPolygons()) {
            return fail(kName, "the meshes differ in topology");
        }
        MStringArray have;
        to.getColorSetNames(have);
        current_ = to.currentColorSetName().asChar();
        for (unsigned i = 0; i < db.numberOfFlagUses(kColorSet[0]); ++i) {
            MArgList use;
            db.getFlagArgumentList(kColorSet[0], i, use);
            auto set = std::make_shared<ColorSet>();
            set->name = use.asString(0);
            bool exists = false;
            for (unsigned k = 0; k < have.length(); ++k) exists = exists || have[k] == set->name;
            if (exists) continue;
            MStringArray theirs;
            from.getColorSetNames(theirs);
            bool found = false;
            for (unsigned k = 0; k < theirs.length(); ++k) found = found || theirs[k] == set->name;
            if (!found) return fail(kName, objects[0] + ": the source has no color set " + set->name);
            const MColor unset(0.0f, 0.0f, 0.0f, 1.0f);
            from.getFaceVertexColors(set->colors, &set->name, &unset);
            set->representation = from.getColorRepresentation(set->name);
            set->clamped = from.isColorClamped(set->name);
            sets_.push_back(set);
        }
        MStringArray made;
        for (const auto& set : sets_) made.append(set->name);
        setResult(made);
        return redoIt();
    });
}

MStatus CopyColorSetsCmd::redoIt() {
    return groomlab::maya::guarded("CopyColorSetsCmd::redoIt", [&]() -> MStatus {
        MFnMesh to(target_);
        MIntArray ids;
        for (const auto& set : sets_) {
            MString name = set->name;
            MStatus status = to.createColorSet(name, nullptr, set->clamped, set->representation);
            CHECK_MSTATUS_AND_RETURN_IT(status);
            status = to.setColors(set->colors, &name, set->representation);
            CHECK_MSTATUS_AND_RETURN_IT(status);
            if (ids.length() != set->colors.length()) {
                ids.setLength(set->colors.length());
                for (unsigned i = 0; i < ids.length(); ++i) ids[i] = static_cast<int>(i);
            }
            status = to.assignColors(ids, &name);
            CHECK_MSTATUS_AND_RETURN_IT(status);
        }
        if (current_.empty() && !sets_.empty()) to.setCurrentColorSetName(sets_.front()->name);
        return MS::kSuccess;
    });
}

MStatus CopyColorSetsCmd::undoIt() {
    return groomlab::maya::guarded("CopyColorSetsCmd::undoIt", [&]() -> MStatus {
        MFnMesh to(target_);
        for (auto it = sets_.rbegin(); it != sets_.rend(); ++it) to.deleteColorSet((*it)->name);
        if (!current_.empty()) to.setCurrentColorSetName(MString(current_.c_str()));
        return MS::kSuccess;
    });
}

}  // namespace groomlab::maya
