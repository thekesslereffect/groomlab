// SPDX-License-Identifier: Apache-2.0
#pragma once

// Guides rooted on the scalp: the last pass every guide the photo, stroke and comb tools make goes
// through (guidesFromReference, the agent tools' guides_from_strokes and comb_guides), so a guide
// always starts on the scalp and never runs through the head, whichever tool drew it.
//
// Each guide, a polyline of any origin (a lock line triangulated from photos, a stroke lifted onto
// the hair's shell, a guide grown or combed from a root):
//
// 1. The way: the end nearer the scalp is the root (a chain that starts farther from the scalp than
//    it ends, its last point within half the first's gap, is turned round: the rule HairGS keeps its
//    strand pieces by). A first point on the scalp already (within `snapShare` head radii) is the
//    root whatever the tip does (hair lying over the head ends near it), and so is the first point
//    of a chain that falls toward its end (ends lower along `up` by more than 0.05 head radii: hair
//    lying down the side of the head to the sideburn or the nape ends near the scalp's edge, root
//    first; on a six-photo test head two of fourteen drawn strokes were turned round wrongly without it).
// 2. The root on the scalp: a first point within `snapShare` head radii of it is moved onto it; one
//    farther, up to `maxGapShare` head radii, is connected to the nearest scalp point by a straight
//    segment (leaving along the scalp's normal there; `radial`: the scalp under it toward the head's
//    middle instead of the nearest point); one farther still (a lock
//    hanging beside the face whose both ends are far from the scalp) is dropped, not joined to a
//    scalp it never reached.
// 3. The head: where the guide enters the head mesh (the whole head when given, else the scalp),
//    a dip that comes out again no deeper than `pushShare` head radii is pushed back out to the
//    surface (a guide grazing the head: the synthetic swept-back heads' dips are 0.03-0.23 radii,
//    and cutting them lost a fifth of the hair's length); a deeper pass, or a tip that ends inside
//    (deeper than a hair's width, 2 snapShare), is cut where it went in. A guide left shorter than
//    `minLengthShare` head radii (or with fewer than 2 points) is dropped.
//
// Everything scales with the head's radius (a sphere fitted to the scalp when not given).

#include <cstdint>
#include <vector>

#include "groomlab/math.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {

struct GuideRootingOptions {
    float headRadius = 0.0f;       // the scale (scene units); 0: a sphere fitted to the scalp's points
    float snapShare = 0.02f;       // a root this close to the scalp is moved onto it (head radii)
    float maxGapShare = 0.4f;      // a root farther off the scalp than this is dropped (head radii; about a full head
                                   // of hair's thickness); between: connected
    float pushShare = 0.25f;       // a dip into the head that comes out again no deeper than this is pushed out, deeper
                                   // ones are cut (head radii)
    float minLengthShare = 0.08f;  // shorter guides are dropped (head radii)
    float keepOut = 0.0f;          // pushed points end this far outside the head (scene units; 0: snapShare / 4 head radii)
    bool flip = true;              // turn chains that end nearer the scalp than they start
    // The root under the first point toward `center` (the head's middle) instead of the nearest scalp
    // point: a stroke lifted from one photo whose depth may be off still roots under where it was drawn.
    bool radial = false;
    Vec3f center{0.0f, 0.0f, 0.0f};
    Vec3f up{0.0f, 1.0f, 0.0f};    // the world's up (gravity is -up)
};

// What a pass did, guide by guide (a guide may be both flipped and connected, say).
struct GuideRootingCounts {
    int guides = 0;     // given
    int flipped = 0;    // turned round (the tip was nearer the scalp)
    int snapped = 0;    // the root moved onto the scalp (it was within snapShare)
    int connected = 0;  // a segment added from the scalp to the root
    int pushed = 0;     // shallow dips into the head pushed out
    int cut = 0;        // cut where they entered the head
    int dropped = 0;    // left out: droppedGap + droppedShort
    int droppedGap = 0;    // the root too far off the scalp
    int droppedShort = 0;  // too short after the cut
    float headRadius = 0.0f;              // the scale used
    float snapGap = 0.0f, maxGap = 0.0f;  // the thresholds, scene units
    float pushDepth = 0.0f, minLength = 0.0f;
};

// One guide's outcome.
struct RootedGuide {
    std::vector<Vec3f> points;  // root first, the root on the scalp; empty when dropped
    SurfacePoint root;          // the scalp point it starts at (root.face < 0 when dropped)
    bool flipped = false, snapped = false, connected = false, pushed = false, cut = false;
    bool droppedGap = false, droppedShort = false;
    float rootGap = 0.0f;  // how far the root was off the scalp before (scene units)
};

// The thresholds in scene units (counts.headRadius and the gaps filled in), the head radius found
// when not given.
GuideRootingCounts guideRootingScale(const ScalpMesh& scalp, const GuideRootingOptions& options = {});

// One guide (2 points or more). `head`: the whole head the guide must stay out of (may be null: the
// scalp). `scale`: guideRootingScale's.
RootedGuide rootGuide(const std::vector<Vec3f>& points, const ScalpMesh& scalp, const ScalpMesh* head,
                      const GuideRootingOptions& options, const GuideRootingCounts& scale);

// Every guide in place: dropped ones removed, `kept` (when given) the original index of each kept
// guide, `roots` (when given) each kept guide's scalp point. The counts of what was done.
GuideRootingCounts rootGuides(std::vector<std::vector<Vec3f>>& guides, const ScalpMesh& scalp, const ScalpMesh* head = nullptr,
                              const GuideRootingOptions& options = {}, std::vector<std::size_t>* kept = nullptr,
                              std::vector<SurfacePoint>* roots = nullptr);

// Whether a guide as it is starts tip first by rootGuide's rule (its first point off the scalp and
// its last point nearer it): a check for guides made elsewhere.
bool guideStartsAtTip(const std::vector<Vec3f>& points, const ScalpMesh& scalp, const GuideRootingCounts& scale,
                      const GuideRootingOptions& options = {});

}  // namespace groomlab
