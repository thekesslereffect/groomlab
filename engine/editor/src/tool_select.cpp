// SPDX-License-Identifier: Apache-2.0
#include "groomlab/editor/tool_select.h"

namespace groomlab {

std::vector<GuideId> pickGuidesInView(const GuideHierarchy& h, const ToolView& view, const std::shared_ptr<const ScalpMesh>& scalp,
                                      const std::vector<std::array<float, 2>>& lasso, const GuidePick& pick) {
    if (lasso.empty()) return {};
    const std::vector<GuideId> candidates = guidesAt(h, pick.level, pick.group, pick.hiddenGroups);
    std::vector<std::array<float, 2>> polygon;
    if (lasso.size() == 2) {  // a box from two corners
        const std::array<float, 2>& a = lasso[0];
        const std::array<float, 2>& b = lasso[1];
        polygon = {{a[0], a[1]}, {b[0], a[1]}, {b[0], b[1]}, {a[0], b[1]}};
    } else {
        polygon = lasso;
    }
    // The view's matrix when it has one (its bounds pass over guides off the lasso), else the host's projection.
    const bool matrix = view.projection.valid();
    const ViewProjector project = matrix ? pixelProjector(view.projection) : ViewProjector([&view](const Vec3f& p, std::array<float, 2>* out) {
        return view.project(p, (*out)[0], (*out)[1]);
    });
    // Only what the view shows: guide points behind the scalp are not picked.
    const ViewProjector visible = visibleProjector(project, scalpVisibility(scalp, view));
    std::vector<GuideId> picked = pickGuides(h, candidates, visible, polygon, pick.rootsOnly, pick.clickPixels, matrix ? &view.projection : nullptr);
    if (pick.symmetry.on() && !picked.empty()) picked = withMirrorGuides(h, picked, pick.symmetry);
    return picked;
}

bool pickSoloes(const std::vector<GuideId>& picked, SelectMode mode) { return !picked.empty() || mode == SelectMode::Replace; }

}  // namespace groomlab
