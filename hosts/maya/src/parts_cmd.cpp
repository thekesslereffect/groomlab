// SPDX-License-Identifier: Apache-2.0
#include "parts_cmd.h"

#include <algorithm>
#include <string>

#include <maya/MArgDatabase.h>
#include <maya/MArgList.h>
#include <maya/MDagPath.h>
#include <maya/MFnDagNode.h>
#include <maya/MGlobal.h>
#include <maya/MPlug.h>
#include <maya/MSelectionList.h>
#include <maya/MStringArray.h>

#include "command_args.h"
#include "guarded.h"
#include "groom_shape.h"
#include "groomlab/brush.h"
#include "groomlab/flow.h"
#include "groomlab/parts.h"
#include "groomlab/stroke.h"
#include "maya_inputs.h"

namespace groomlab::maya {

namespace {

const char* kFromCurves[] = {"-fc", "-fromCurves"};
const char* kNameFlag[] = {"-n", "-name"};
const char* kRemove[] = {"-rm", "-remove"};
const char* kClear[] = {"-cl", "-clear"};
const char* kList[] = {"-ls", "-list"};
// Mirror to Other Side (issue 105): "positive" or "negative", that side copied over the other, across
// -symmetry (x, y, z: a plane at -center; topology: the scalp's own), the parts or, with -flow, the
// flow arrows and sinks.
const char* kMirror[] = {"-mir", "-mirror"};
const char* kSymmetry[] = {"-sym", "-symmetry"};
const char* kCenter[] = {"-c", "-center"};
const char* kFlow[] = {"-fl", "-flow"};

// Samples per part curve: dense enough to follow a curve drawn on a head.
constexpr unsigned kPartSamples = 64;

}  // namespace

const char* PartsCmd::kName = "groomLabParts";

void* PartsCmd::creator() { return new PartsCmd(); }

MSyntax PartsCmd::newSyntax() {
    MSyntax s;
    s.addFlag(kFromCurves[0], kFromCurves[1], MSyntax::kString);
    s.makeFlagMultiUse(kFromCurves[0]);
    s.addFlag(kNameFlag[0], kNameFlag[1], MSyntax::kString);
    s.addFlag(kRemove[0], kRemove[1], MSyntax::kString);
    s.addFlag(kClear[0], kClear[1]);
    s.addFlag(kList[0], kList[1]);
    s.addFlag(kMirror[0], kMirror[1], MSyntax::kString);
    s.addFlag(kSymmetry[0], kSymmetry[1], MSyntax::kString);
    s.addFlag(kCenter[0], kCenter[1], MSyntax::kDouble);
    s.addFlag(kFlow[0], kFlow[1]);
    s.setObjectType(MSyntax::kSelectionList, 1, 1);
    s.useSelectionAsDefault(true);
    s.enableQuery(false);
    s.enableEdit(false);
    return s;
}

MStatus PartsCmd::doIt(const MArgList& args) {
    return groomlab::maya::guarded("PartsCmd::doIt", [&]() -> MStatus {
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;
        MDagPath groom;
        if (!groomArgument(kName, db, &groom)) return MS::kFailure;
        MFnDagNode fn(groom);
    
        MPlug plug = fn.findPlug("partData", true);
        PartSet set;
        try {
            set = PartSet::fromJson(plug.asString().asChar());
        } catch (const std::exception& e) {
            return fail(kName, e.what());
        }
    
        if (db.isFlagSet(kList[0])) {
            MStringArray names;
            for (const Part& p : set.parts) names.append(p.name.c_str());
            setResult(names);
            return MS::kSuccess;
        }
        // An edit: as a part stroke, not on a locked groom (nor one from a referenced file).
        if (!editableGroom(kName, groom)) return MS::kFailure;

        if (db.isFlagSet(kMirror[0])) return mirror(db, fn);

        if (db.isFlagSet(kFromCurves[0])) {
            const MeshPtr scalp = joinedScalpFromNode(fn.object());  // with the groom's other scalps
            if (!scalp || scalp->triangleCount() == 0) return fail(kName, "the groom has no scalp mesh");
            MString base = "part";
            if (db.isFlagSet(kNameFlag[0])) db.getFlagArgument(kNameFlag[0], 0, base);
            MStringArray added;
            for (const MString& curve : multiStrings(db, kFromCurves[0])) {
                MObject data;
                if (!curveWorldData(curve, data)) return fail(kName, curve + " is not a nurbsCurve");
                try {
                    // Every host's (parts.h: named base, base1..., resampled evenly, too short refused).
                    const std::string name = set.add(base.asChar(), sampleCurveData(data, kPartSamples), *scalp, kPartSamples);
                    added.append(name.c_str());
                } catch (const std::exception& e) {
                    return fail(kName, curve + ": " + e.what());
                }
            }
            setResult(added);
        } else if (db.isFlagSet(kRemove[0])) {
            MString name;
            db.getFlagArgument(kRemove[0], 0, name);
            if (!set.remove(name.asChar())) return fail(kName, "no part named " + name);
        } else if (db.isFlagSet(kClear[0])) {
            set.parts.clear();
        } else {
            return fail(kName, "give -fromCurves, -remove, -clear, -mirror or -list");
        }
    
        modifier_.newPlugValueString(plug, set.empty() ? MString("") : MString(set.toJson().c_str()));
        undoable_ = true;
        return redoIt();
    });
}

MStatus PartsCmd::mirror(const MArgDatabase& db, const MFnDagNode& fn) {
    const std::string from = flagString(db, kMirror[0], "positive").asChar();
    if (from != "positive" && from != "negative") return fail(kName, "-mirror takes positive or negative");
    // Parts and flow are stored bound to the cage (the scalp as the host gives it): mirrored there.
    const MeshPtr scalp = joinedScalpFromNode(fn.object());
    if (!scalp || scalp->triangleCount() == 0) return fail(kName, "the groom has no scalp mesh");
    BrushToolSettings settings;
    try {
        settings.setSymmetry(flagString(db, kSymmetry[0], "x").asChar());
    } catch (const std::exception& e) {
        return fail(kName, e.what());
    }
    settings.brush.mirrorCenter = static_cast<float>(flagDouble(db, kCenter[0], 0.0));
    const StrokeSymmetry sym = StrokeSymmetry::of(settings.brush, scalp.get());
    const int side = from == "positive" ? 1 : -1;
    const bool flow = db.isFlagSet(kFlow[0]);
    MPlug plug = fn.findPlug(flow ? "flowData" : "partData", true);
    OverlayMirrorResult done;
    std::string text;
    try {
        if (flow) {
            FlowSet set = FlowSet::fromJson(plug.asString().asChar());
            done = mirrorFlowToOtherSide(set, *scalp, sym, side);
            text = set.empty() ? std::string() : set.toJson();
        } else {
            PartSet set = PartSet::fromJson(plug.asString().asChar());
            done = mirrorPartsToOtherSide(set, *scalp, sym, side);
            text = set.empty() ? std::string() : set.toJson();
        }
    } catch (const std::exception& e) {
        return fail(kName, e.what());
    }
    setResult(MString((std::to_string(done.copied) + " mirrored, " + std::to_string(done.removed) + " replaced").c_str()));
    modifier_.newPlugValueString(plug, MString(text.c_str()));
    undoable_ = true;
    return redoIt();
}

}  // namespace groomlab::maya
