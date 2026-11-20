// SPDX-License-Identifier: Apache-2.0
#pragma once

// Transfer Groom to Scalp, every host's (Maya's groomLabTransfer, the Blender helper's transfer,
// the app's transferToScalp): the options prepared the one way, then the groom's guides, parts and
// flow moved through the mapping. Each host keeps its own undo and how it stores the texts; the
// maps go through projectScalpMaps (scalp_paint.h).

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "groomlab/scalp_maps.h"
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

// The groom's scale after a transfer, every host's: a groom scaled with its scalp (`scaling`, at
// scale `groomScale` k) keeps the size shown (the new scalp k times its 1x), or with `fitSize` grows
// with the area (k * sqrt(newArea / oldArea): the new scalp the old one resized). `scale` is that
// target, `reference` the scale reference it gives on the new scalp (newArea / scale^2). None: the
// reference stays as it is (a groom not scaled with its scalp, no area to measure).
struct TransferredScale {
    double scale = 1.0;
    double reference = 0.0;
};
std::optional<TransferredScale> transferredScale(bool scaling, double groomScale, double oldArea, double newArea, bool fitSize);

// A transfer that does not project the maps, every host's: vertex maps cannot fit another scalp,
// so they go (their names back, sorted); texture maps keep their image on the new scalp's UVs.
std::vector<std::string> dropVertexMaps(ScalpMaps& maps);

}  // namespace groomlab
