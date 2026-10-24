// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <maya/MTypeId.h>

namespace groomlab::maya {

// 0x00000000-0x0007FFFF is Autodesk's range for local, undistributed plugins.
// Request a registered block from Autodesk before shipping outside the studio.
constexpr unsigned int kGroomLabShapeTypeId = 0x0007F100;
constexpr unsigned int kGroomLabFrameDataTypeId = 0x0007F101;  // GroomFrameData (outFrame)
constexpr unsigned int kGroomLabMarkerSetTypeId = 0x0007F102;  // groomLabMarkerSet (Transfer Groom window)
constexpr unsigned int kGroomLabFileTypeId = 0x0007F103;       // groomLabFile (a groom's file, for the File Path Editor)

}  // namespace groomlab::maya
