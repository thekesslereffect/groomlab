// SPDX-License-Identifier: Apache-2.0
#pragma once

// Transfer Groom to Scalp, every host's (Maya's groomLabTransfer, the Blender helper's transfer,
// the app's transferToScalp): the options prepared the one way, then the groom's guides, parts and
// flow moved through the mapping. Each host keeps its own undo and how it stores the texts; the
// maps go through projectScalpMaps (scalp_paint.h).

#include <cstddef>
#include <string>
#include <vector>

#include "groomlab/scalp_mesh.h"
#include "groomlab/transfer.h"

namespace groomlab {

// The options as every host prepares them: fitting the size while the groom scales with its scalp
// keeps the guides' lengths (the groom's scale grows them; the mapping only turns them); the
// markers snapped onto both scalps; the method's need of markers checked (markerRequirement).
// Throws std::invalid_argument with the reason.
TransferOptions preparedTransferOptions(TransferOptions options, bool fitSize, bool scaleWithScalp, const ScalpMesh& from,
                                        const ScalpMesh& to);

// The groom's texts after the move (empty in, empty out).
struct GroomTransferTexts {
    std::string hierarchyJson, partsJson, flowJson;
    std::size_t guidesMoved = 0;
};
// The guides (moved as shown: at the groom's scale `groomScale`), the parts and the flow through
// `mapping`; what fell off the new scalp, and the roots that found no matching UV, in `warnings`.
GroomTransferTexts transferGroomTexts(const std::string& hierarchyJson, const std::string& partsJson, const std::string& flowJson,
                                      const ScalpMapping& mapping, double groomScale, std::vector<std::string>* warnings = nullptr);

}  // namespace groomlab
