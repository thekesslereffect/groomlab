// SPDX-License-Identifier: Apache-2.0
#include "groomlab/groom_transfer.h"

#include <cmath>
#include <stdexcept>

#include "groomlab/flow.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/parts.h"
#include "groomlab/transfer_markers_edit.h"

namespace groomlab {

TransferOptions preparedTransferOptions(TransferOptions options, bool fitSize, bool scaleWithScalp, const ScalpMesh& from,
                                        const ScalpMesh& to) {
    if (fitSize && scaleWithScalp) options.keepLengths = true;
    if (!options.markers.empty()) options.markers = snapTransferMarkers(options.markers, from, to);
    const std::string requirement = markerRequirement(options.method, options.markers.size());
    if (!requirement.empty()) throw std::invalid_argument(requirement);
    return options;
}

GroomTransferTexts transferGroomTexts(const std::string& hierarchyJson, const std::string& partsJson, const std::string& flowJson,
                                      const ScalpMapping& mapping, double groomScale, std::vector<std::string>* warnings) {
    GroomTransferTexts out;
    if (!hierarchyJson.empty()) {
        GuideHierarchy h = GuideHierarchy::fromJson(hierarchyJson);
        h.setScale(static_cast<float>(groomScale));  // moved as shown: the groom keeps its scale
        h.transfer(mapping);
        out.guidesMoved = h.guides().size();
        out.hierarchyJson = h.toJson();
    }
    if (!partsJson.empty()) {
        const PartSet parts = PartSet::fromJson(partsJson);
        const PartSet moved = parts.transferred(mapping);
        if (moved.parts.size() < parts.parts.size() && warnings) warnings->push_back("some parts fell apart on the new scalp and were dropped");
        out.partsJson = moved.empty() ? std::string() : moved.toJson();
    }
    if (!flowJson.empty()) {
        const FlowSet flow = FlowSet::fromJson(flowJson);
        const FlowSet moved = flow.transferred(mapping);
        if ((moved.pins.size() < flow.pins.size() || moved.sinks.size() < flow.sinks.size()) && warnings) {
            warnings->push_back("some flow arrows did not fit the new scalp and were dropped");
        }
        out.flowJson = moved.empty() ? std::string() : moved.toJson();
    }
    if (mapping.uvMisses() > 0 && warnings) {
        warnings->push_back(std::to_string(mapping.uvMisses()) + " roots found no matching UV on the new scalp and went to the closest point");
    }
    return out;
}

std::optional<TransferredScale> transferredScale(bool scaling, double groomScale, double oldArea, double newArea, bool fitSize) {
    if (!scaling || !(groomScale > 0.0) || !(newArea > 0.0)) return std::nullopt;
    TransferredScale out;
    out.scale = groomScale;
    if (fitSize && oldArea > 0.0) out.scale *= std::sqrt(newArea / oldArea);
    out.reference = newArea / (out.scale * out.scale);
    return out;
}

std::vector<std::string> dropVertexMaps(ScalpMaps& maps) {
    std::vector<std::string> dropped;
    for (auto it = maps.maps.begin(); it != maps.maps.end();) {
        if (it->second.kind == "vertices") {
            dropped.push_back(it->first);
            it = maps.maps.erase(it);
        } else {
            ++it;
        }
    }
    return dropped;  // (the maps' order: by name)
}

}  // namespace groomlab
