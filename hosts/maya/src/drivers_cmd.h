// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <maya/MPxCommand.h>
#include <maya/MSyntax.h>

namespace groomlab::maya {

// `groomLabDrivers <groom>`: the groom's driver curves (driverCurves, read by DriveGuides nodes:
// drive_guides.h).
//   -bind [-frame N]  the drivers and the scalp as they are at frame N (the current frame by
//                     default), as the bind's text (DriverBind's JSON: each driver anchored on
//                     the scalp and carried to its rest pose); '' with no driver curves.
//                     groomlab.drivers.bind_drivers stores it in driverBind, undoably.
//   -count            how many driver curves are connected.
// Changes nothing itself.
class DriversCmd : public MPxCommand {
public:
    static const char* kName;
    static void* creator();
    static MSyntax newSyntax();
    MStatus doIt(const MArgList& args) override;
};

}  // namespace groomlab::maya
