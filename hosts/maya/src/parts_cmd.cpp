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
#include "groomlab/parts.h"
#include "maya_inputs.h"

namespace groomlab::maya {

namespace {

const char* kFromCurves[] = {"-fc", "-fromCurves"};
const char* kNameFlag[] = {"-n", "-name"};
const char* kRemove[] = {"-rm", "-remove"};
const char* kClear[] = {"-cl", "-clear"};
const char* kList[] = {"-ls", "-list"};

// Samples per part curve: dense enough to follow a curve drawn on a head.
constexpr unsigned kPartSamples = 64;

std::string uniqueName(const PartSet& set, const std::string& base) {
    if (!set.find(base)) return base;
    for (int i = 1;; ++i) {
        const std::string name = base + std::to_string(i);
        if (!set.find(name)) return name;
    }
}

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

        if (db.isFlagSet(kFromCurves[0])) {
            const MeshPtr scalp = joinedScalpFromNode(fn.object());  // with the groom's other scalps
            if (!scalp || scalp->triangleCount() == 0) return fail(kName, "the groom has no scalp mesh");
            MString base = "part";
            if (db.isFlagSet(kNameFlag[0])) db.getFlagArgument(kNameFlag[0], 0, base);
            MStringArray added;
            for (unsigned i = 0; i < db.numberOfFlagUses(kFromCurves[0]); ++i) {
                MArgList a;
                db.getFlagArgumentList(kFromCurves[0], i, a);
                const MString curve = a.asString(0);
                MObject data;
                if (!curveWorldData(curve, data)) return fail(kName, curve + " is not a nurbsCurve");
                try {
                    const std::string name = uniqueName(set, base.asChar());
                    set.parts.push_back(PartSet::fromPoints(name, sampleCurveData(data, kPartSamples), *scalp));
                    added.append(name.c_str());
                } catch (const std::exception& e) {
                    return fail(kName, curve + ": " + e.what());
                }
            }
            setResult(added);
        } else if (db.isFlagSet(kRemove[0])) {
            MString name;
            db.getFlagArgument(kRemove[0], 0, name);
            const auto before = set.parts.size();
            set.parts.erase(std::remove_if(set.parts.begin(), set.parts.end(),
                                           [&](const Part& p) { return p.name == name.asChar(); }),
                            set.parts.end());
            if (set.parts.size() == before) return fail(kName, "no part named " + name);
        } else if (db.isFlagSet(kClear[0])) {
            set.parts.clear();
        } else {
            return fail(kName, "give -fromCurves, -remove, -clear or -list");
        }
    
        modifier_.newPlugValueString(plug, set.empty() ? MString("") : MString(set.toJson().c_str()));
        undoable_ = true;
        return redoIt();
    });
}

}  // namespace groomlab::maya
