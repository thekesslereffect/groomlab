// SPDX-License-Identifier: Apache-2.0
#pragma once
// The Clone tool (issue 91): the guides under the brush circle captured (a Ctrl+click, or the
// card's Pick Source and a click), then set down wherever the next click lands, as many times as
// wanted. The patch keeps its layout and each guide its shape, turned to the scalp where it lands;
// a drag before release spins the patch about the scalp's normal. Every host's: BrushTool runs it
// (BrushToolSettings::Tool::Clone), the hosts only draw and pass the card's switches.

#include <memory>
#include <vector>

#include "groomlab/guide_hierarchy.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {

struct StrokeSymmetry;

// What a capture took: a copy of the guides as they were (later strokes do not change it), the
// guides picked (the brush's level or group; their descendants come along when set down), and the
// circle they were picked in on the scalp.
struct GuideClip {
    std::shared_ptr<const GuideHierarchy> source;
    std::vector<GuideId> guides;  // in the hierarchy's order
    std::vector<Vec3f> normals;   // per guide: the scalp's normal at its root when captured
    Vec3f center, normal;         // the circle's middle on the scalp, the normal there
    float radius = 0.0f;          // the circle's radius there (world)
    bool empty() const { return guides.empty() || !source; }
};

// `guides` of `h` (those under the brush, as the host picked them) captured round `center` on
// `scalp` (the circle `radius` across there). Guides unknown to `h` are skipped.
GuideClip captureGuides(const GuideHierarchy& h, const ScalpMesh& scalp, const std::vector<GuideId>& guides,
                        const SurfacePoint& center, float radius);

struct CloneOptions {
    float angle = 0.0f;    // radians the patch turns about the scalp's normal where it lands
    bool replace = false;  // the guides of the clip's groups within its circle there removed first
    bool collide = true;   // the copies kept out of the scalp (BrushSettings::collide)
    std::string freeze = "freeze";  // frozen guides (that layer at 1) are never replaced
};

// The clip set down at `at`: each captured guide copied with its tube, painted layers and
// descendants (children follow the copy), its root carried from the clip's center as much as it
// lay from it (turned from the captured normal to `at`'s, then by `options.angle`) and put on the
// scalp, its shape turned to the scalp's normal there. Into the groups of the same names (a child
// copy's parent the nearest guide of the parent group when its own parent was not copied). A root
// tube (drawn root shape) is copied by duplicateRootTube, with the tube hierarchy under it, while
// `h` still holds it as captured. `mirror` (symmetry on): the copies are the mirror images of the
// patch set down at `at` instead (root tubes are not mirrored). Returns the new top guides' IDs.
std::vector<GuideId> pasteGuides(GuideHierarchy& h, const ScalpMesh& scalp, const GuideClip& clip, const SurfacePoint& at,
                                 const CloneOptions& options = {}, const StrokeSymmetry* mirror = nullptr);

// Where pasteGuides would put the clip's guides and their descendants at `at` (turned `angle`;
// mirrored by `mirror`), as polylines, at most `maxLines`, without making them: the hover's
// preview of a click there. (Root tubes show as their guides; collision with the scalp is not
// taken.)
std::vector<std::vector<Vec3f>> clonePreview(const ScalpMesh& scalp, const GuideClip& clip, const SurfacePoint& at, float angle,
                                             const StrokeSymmetry* mirror = nullptr, std::size_t maxLines = 4001);

}  // namespace groomlab
