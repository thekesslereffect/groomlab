// SPDX-License-Identifier: Apache-2.0
#include "drivers_cmd.h"

#include <cmath>

#include <maya/MAnimControl.h>
#include <maya/MArgDatabase.h>
#include <maya/MDGContext.h>
#include <maya/MDGContextGuard.h>
#include <maya/MDagPath.h>
#include <maya/MTime.h>

#include "command_args.h"
#include "groomlab/drive_guides.h"
#include "guarded.h"
#include "maya_inputs.h"

namespace groomlab::maya {

const char* DriversCmd::kName = "groomLabDrivers";

void* DriversCmd::creator() { return new DriversCmd(); }

MSyntax DriversCmd::newSyntax() {
    MSyntax s;
    s.addFlag("-b", "-bind");
    s.addFlag("-f", "-frame", MSyntax::kDouble);
    s.addFlag("-c", "-count");
    s.setObjectType(MSyntax::kSelectionList, 1, 1);
    s.useSelectionAsDefault(true);
    return s;
}

MStatus DriversCmd::doIt(const MArgList& args) {
    return guarded("DriversCmd::doIt", [&]() -> MStatus {
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;
        MDagPath path;
        if (!groomArgument("groomLabDrivers", db, &path)) return MS::kFailure;
        const MObject node = path.node();
        if (db.isFlagSet("-count")) {
            const GuidesPtr drivers = driverCurvesFromNode(node);
            setResult(drivers ? static_cast<int>(drivers->size()) : 0);
            return MS::kSuccess;
        }
        if (db.isFlagSet("-bind")) {
            double frame = MAnimControl::currentTime().as(MTime::uiUnit());
            if (db.isFlagSet("-frame")) db.getFlagArgument("-frame", 0, frame);
            // The drivers and the scalp (with its rest pose) as Maya evaluates them at that frame.
            const MTime time(frame, MTime::uiUnit());
            MDGContext context(time);
            MDGContextGuard guard(context);
            const GuidesPtr drivers = driverCurvesFromNode(node);
            const MeshPtr scalp = joinedScalpFromNode(node);
            if (!drivers) {
                setResult(MString());
                return MS::kSuccess;
            }
            const DriverBind bind = bindDrivers(*drivers, scalp.get(), static_cast<int>(std::lround(frame)));
            setResult(MString(bind.empty() ? "" : bind.toJson().c_str()));
            return MS::kSuccess;
        }
        displayError("groomLabDrivers: say -bind or -count");
        return MS::kFailure;
    });
}

}  // namespace groomlab::maya
