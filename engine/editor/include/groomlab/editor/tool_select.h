// SPDX-License-Identifier: Apache-2.0
#pragma once

// The groom editor's Select tool (engine/editor), every host's: which guides a click, a box or a
// lasso in a view picks, and when a pick soloes the selection, worked out once.

#include <array>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "groomlab/brush.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/guide_selection.h"
#include "groomlab/view_projection.h"

namespace groomlab {

// What a pick may take.
struct GuidePick {
    int level = -1;                         // the guides of this level only (-1: any)
    std::string group;                      // the guides of this group only ("": any)
    std::set<std::string> hiddenGroups;     // hidden groups' guides are not picked
    bool rootsOnly = false;                 // a lasso picks a guide by its root only
    float clickPixels = 12.0f;              // a click picks the guide nearest it, closer than this
    StrokeSymmetry symmetry;                // on: each picked guide's mirror guide too
};

// The guides a pick in `view` takes, every host's Select: `lasso` in the view's pixels, one point a
// click, two a box's corners, more a lasso; only what the view shows (a guide point behind `scalp`
// is not picked); with symmetry on, each picked guide's mirror guide too.
std::vector<GuideId> pickGuidesInView(const GuideHierarchy& h, const ToolView& view, const std::shared_ptr<const ScalpMesh>& scalp,
                                      const std::vector<std::array<float, 2>>& lasso, const GuidePick& pick);

// Whether a pick soloes the selection (Select's solo: every guide not selected frozen, or hidden):
// when it picked something, or a plain pick of nothing (which frees every guide again); a pick of
// nothing with a modifier changes nothing.
bool pickSoloes(const std::vector<GuideId>& picked, SelectMode mode);

}  // namespace groomlab
