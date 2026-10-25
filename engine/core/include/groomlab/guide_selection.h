// SPDX-License-Identifier: Apache-2.0
#pragma once

// Guide selection (GT-5): by level or group, by lasso in a view, soft
// selection around it, and mirrored across a symmetry plane. Selections are
// guide IDs; soft selections add a 0-1 weight per guide, which brushes use
// as a mask (BrushSettings::mask).

#include <array>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "groomlab/guide_hierarchy.h"
#include "groomlab/view_projection.h"

namespace groomlab {

// Guides of a hierarchy level (-1: every level) or, when `group` is set, of that group.
std::vector<GuideId> guidesAt(const GuideHierarchy& h, int level, const std::string& group = {});
// The same, less the guides of hidden groups (they cannot be picked).
std::vector<GuideId> guidesAt(const GuideHierarchy& h, int level, const std::string& group,
                              const std::set<std::string>& hiddenGroups);

// Maps a world point to view coordinates; false when it is not in front of the camera.
using ViewProjector = std::function<bool(const Vec3f& world, std::array<float, 2>* view)>;

// Candidates with a point inside the polygon (view coordinates): the root
// only, or any point of the guide.
std::vector<GuideId> guidesInLasso(const GuideHierarchy& h, const std::vector<GuideId>& candidates,
                                   const ViewProjector& project, const std::vector<std::array<float, 2>>& polygon,
                                   bool rootsOnly = true);

// Projects world points to whole view pixels as a selection tool reads them
// (ViewProjection::projectPixel: inside the view volume only, truncated, as Maya's worldToView).
ViewProjector pixelProjector(const ViewProjection& view);
// What a selection in `view` sees of `scalp` (ScalpVisibility from the view's camera); null
// without a scalp or a camera.
std::shared_ptr<const ScalpVisibility> scalpVisibility(const std::shared_ptr<const ScalpMesh>& scalp, const ToolView& view);
// `project` refusing the points the scalp hides (`occlusion` null: `project` as it is): a pick
// through it takes only what the view shows, never a guide (or the part of one) behind the head.
ViewProjector visibleProjector(ViewProjector project, std::shared_ptr<const ScalpVisibility> occlusion);

// The selection tool's pick in a view: with a lasso (3 points or more, spanning more than 3
// pixels), the candidates with a point inside it (the root only with `rootsOnly`); otherwise a
// click at lasso[0]: the candidate with a point nearest it, closer than `clickPixels` (the first
// such in candidate order on equal distances). `bounds`, when given, is the matrix `project`
// reads (pixelProjector of it): a guide whose projected bounds lie off the lasso's is passed
// over without projecting its points. The same guides either way.
std::vector<GuideId> pickGuides(const GuideHierarchy& h, const std::vector<GuideId>& candidates,
                                const ViewProjector& project, const std::vector<std::array<float, 2>>& lasso,
                                bool rootsOnly, float clickPixels, const ViewProjection* bounds = nullptr);

// The guide point nearest pixel (x, y) within `radius` pixels and in front of the camera (along
// the ray `origin`, `direction` under the cursor): among points within a pixel of the nearest on
// screen, the one nearest the camera. With `occlusion`, points the scalp hides do not count
// (asked only of points that could be picked). What a brush stroke starts on, and Shift+click's
// level pick. False when no point is in reach.
struct GuidePointHit {
    const HierarchyGuide* guide = nullptr;
    Vec3f point;
};
bool nearestGuidePoint(const GuideHierarchy& h, const ToolView& view, float x, float y, float radius,
                       const Vec3f& origin, const Vec3f& direction, const ScalpVisibility* occlusion,
                       GuidePointHit* hit);

// How a pick changes a selection: replaced by the picked guides; the picked ones added (after
// the selection, in order); toggled (each picked guide in turn taken out when selected, else
// added at the end); or the picked ones taken out.
enum class SelectMode { Replace, Add, Toggle, Deselect };
// "replace", "add", "toggle", "deselect"; throws std::invalid_argument.
SelectMode selectModeFromName(const std::string& name);
std::vector<GuideId> combineSelection(const std::vector<GuideId>& selected, const std::vector<GuideId>& picked, SelectMode mode);

// Selected guides weigh 1; other guides of the same groups weigh by the
// distance from their root to the nearest selected root, falling off
// smoothly to 0 at `radius` (0: no falloff, hard selection).
std::map<GuideId, float> softSelection(const GuideHierarchy& h, const std::vector<GuideId>& selected, float radius);

// For each selected guide, the guide of the same group whose root is nearest
// its root mirrored across the plane (axis 0/1/2 = center), the last in the
// hierarchy's order among equally near ones; none when nothing lies within
// `tolerance`. Each group's roots are searched through a PointIndex.
std::vector<GuideId> mirrorSelection(const GuideHierarchy& h, const std::vector<GuideId>& selected, int axis,
                                     float center, float tolerance);

}  // namespace groomlab
