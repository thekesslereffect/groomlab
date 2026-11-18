// SPDX-License-Identifier: Apache-2.0
#pragma once

// Root-first tubes (issue 61) and the tube hierarchy (issue 62), the Tube tool's work in every host
// (first written for Maya in Python: hosts/maya/scripts/groomlab/root_tubes.py, now a thin wrapper
// over these).
//
// Root-first tubes: the artist clicks a root's outline on the scalp one CV at a time; closing it
// makes a tube guide rooted at the outline's middle, its tube's root section that outline exactly
// (a library shape of its own, kOwnedTubeShapePrefix<n>, removed with its last guide), going out
// along a drawn path or straight off the scalp. The tube's root footprint is that outline fixed on
// the scalp (TubeSpine::setRootOnScalp: combing never moves it), and Interpolate's fillTubes gives
// every strand rooted in it to that tube. The CVs are kept on the scalp with the guide
// (HierarchyGuide::rootOutline, saved), apart from its shape: only drawing the root and editing it
// set them.
//
// The tube hierarchy: a base group of tubes (level 1) and below it groups that split each tube in
// two (ChildGenerationParams::split), level by level up to kMaxTubeLevels, level n's group a child
// group of level n - 1's named <base>_L<n>; shaping a tube carries the smaller tubes inside it.
//
// Every edit here works on a hierarchy evaluated on `scalp` (the scalp its guides root on) and
// leaves it evaluated there; the host stores it as one undo step.

#include <array>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "groomlab/guide_hierarchy.h"
#include "groomlab/math.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/tube.h"

namespace groomlab {

constexpr const char* kRootTubeGroup = "rootTubes";  // where new root tubes go with no group picked
constexpr int kMaxTubeLevels = 9;                     // the hierarchy's deepest level (1: the base)
constexpr const char* kTubeLevelSuffix = "_L";        // level n's group: <base>_L<n>
constexpr float kRootSnapPixels = 12.0f;              // how near on screen a CV snaps to a neighbour's

// ---- Path math (pure: the tools and the tests use it too) ------------------------------------------

// The area centroid of a closed outline (points roughly on a plane: in its Newell plane), the mean
// where it has no area.
Vec3f outlineCentroid(const std::vector<Vec3f>& outline);
// `count` points evenly along a polyline (2 or more points).
std::vector<Vec3f> resamplePolyline(const std::vector<Vec3f>& points, std::size_t count);
// How far (radians) a polyline turns, all told.
float polylineTurning(const std::vector<Vec3f>& points);
// How far (radians) `direction` leans off `normal`.
float leanOff(const Vec3f& direction, const Vec3f& normal);
// How many spans a drawn guide gets: about one every root size (`size`) along it and enough that
// none turns more than 15 degrees, both times `density`, 3 to 200.
int rootPathSpans(const std::vector<Vec3f>& points, float size, float density = 1.0f);
// A root tube's guide points: from `root` straight off the scalp (`length`, `segments` spans), or
// along the drawn `path` (world points from the root out; its first taken to the root) smoothed
// over the root's `size`, as many evenly spaced points as its length and turns call for (times
// `density`), kept softly clear of `scalp` when given; its first span leaning at most 50 degrees
// off `normal`. Throws std::invalid_argument for a path or length of nothing.
std::vector<Vec3f> rootTubeGuidePath(const Vec3f& root, const Vec3f& normal, const std::vector<Vec3f>& path, float length,
                                     int segments, float size, float density = 1.0f, const ScalpMesh* scalp = nullptr);

// ---- On the scalp ------------------------------------------------------------------------------------

// The scalp's nearest point to `p` and its smooth normal there (ScalpMesh::normal).
struct ScalpPointNormal {
    Vec3f point, normal;
};
ScalpPointNormal scalpPointNormal(const ScalpMesh& scalp, const Vec3f& p);
// A drawn root's middle on the scalp (where its guide starts, the Tube tool's green point): the apex
// of the patch `outline` draws, the scalp's outermost point along the patch's mean normal through its
// centroid, so a root drawn over much of a head starts on its top, not inside.
ScalpPointNormal rootOutlineCenter(const ScalpMesh& scalp, const std::vector<Vec3f>& outline);

// ---- Root tubes --------------------------------------------------------------------------------------

struct RootTubeOptions {
    float length = 1.0f;   // a guide straight off the scalp (no path): its length ...
    int segments = 12;     // ... and spans
    float taper = 0.5f;    // the tip's width, a share of the root's
    float density = 1.0f;  // a drawn guide's points, times this
    std::string group;     // the root group it goes into ("": kRootTubeGroup), made when missing
};

// A root tube grown from its root's `outline` (world points on or near the scalp, 3 to
// kMaxTubeShapePoints, in order round it) along `path` (empty: straight off the scalp). Returns
// its guide's ID. Throws std::invalid_argument for too few or too many CVs, or nothing to grow.
GuideId makeRootTube(GuideHierarchy& h, const ScalpMesh& scalp, const std::vector<Vec3f>& outline,
                     const std::vector<Vec3f>& path, const RootTubeOptions& options = {});
// Root tube `guide` given a new outline (as rootTubeOutline gives it, moved): the guide moved whole
// to root at its middle, its shape the new outline, its root section fitted to it and the rest of
// the tube scaled, flattened and turned as much (its taper and what brushes painted kept); its
// child tubes follow.
void editRootTube(GuideHierarchy& h, const ScalpMesh& scalp, GuideId guide, const std::vector<Vec3f>& outline);
// The root tubes' guide IDs (of `guides` when given: those among them, or the root tubes their
// tubes descend from), in ID order.
std::vector<GuideId> rootTubesOf(const GuideHierarchy& h, const std::vector<GuideId>& guides = {});
// A root tube's CVs as world points on the scalp: as drawn or last edited (HierarchyGuide::
// rootOutline), else (one made before they were kept, or the scalp changed under them) its tube's
// root section (rootTubeSectionOutline).
std::vector<Vec3f> rootTubeOutline(const GuideHierarchy& h, const ScalpMesh& scalp, GuideId guide);
// A root tube's root footprint as world points: its shape's control points placed in its root
// section laid flat on the scalp, taken onto the scalp along the footprint's normal (`onScalp`).
std::vector<Vec3f> rootTubeSectionOutline(const GuideHierarchy& h, const ScalpMesh& scalp, GuideId guide, bool onScalp = true);
// How a slice lies in its parent's tube (a check): at each of `samples` places along it (t evenly,
// the root's first), its tube's outline in its parent's section coordinates at the same t (the
// parent's section at t: its center plus u and v times its section axes; `off`, how far off that
// section's plane, scene units). Its parent's outline is tubeOutline(parent's tube).
struct SliceFitSample {
    float t = 0.0f;
    std::vector<SectionPoint> outline;
    std::vector<float> off;
    float turn = 0.0f;  // its sections' u axis against its parent's there (radians, about the parent's tangent)
};
// `within`: an ancestor's sections instead of its parent's (its level 1 root's, say).
std::vector<SliceFitSample> sliceFit(const GuideHierarchy& h, GuideId slice, int samples = 11, GuideId within = kNoGuide);
// Root tube `guide`'s CVs kept as `outline` (world points, taken onto the scalp), its tube as it
// is: an outline read from elsewhere (Maya's first root tubes kept theirs on the scene). Throws
// std::invalid_argument for a guide that is not a root tube or fewer than 3 CVs.
void keepRootTubeOutline(GuideHierarchy& h, const ScalpMesh& scalp, GuideId guide, const std::vector<Vec3f>& outline);
// A root tube's root middle on the scalp (its outline's centroid taken onto it) and the normal there.
ScalpPointNormal rootTubeCenter(const GuideHierarchy& h, const ScalpMesh& scalp, GuideId guide);
// `outline` carried over the scalp from its middle at `start` to `to`: each CV turned with the
// scalp's normal from there to here, set down at `to` and taken onto the scalp (the shape keeps its
// size on a curved head). CVs whose index is in `held` stay (sealed onto a neighbour).
std::vector<Vec3f> carryOutline(const ScalpMesh& scalp, const std::vector<Vec3f>& outline, const Vec3f& start, const Vec3f& to,
                                const std::set<int>& held = {});
// The indices of `outline`'s CVs lying on one of `others` (one of its CVs or edges), within
// `tolerance` (negative: a thousandth of the outline's longest edge): sealed onto a neighbour.
std::vector<int> sealedCvs(const std::vector<Vec3f>& outline, const std::vector<std::vector<Vec3f>>& others,
                           float tolerance = -1.0f);
// `p` snapped onto the nearest of `outlines`: one of their CVs within `pixels` on screen
// (`toScreen`: a point's view coordinates), else the nearest point of their edges within `pixels`.
enum class RootSnap { None, Cv, Edge };
RootSnap snapRootCv(Vec3f& p, const std::vector<std::vector<Vec3f>>& outlines,
                    const std::function<std::array<float, 2>(const Vec3f&)>& toScreen, float pixels = kRootSnapPixels);
// A copy of root tube `guide` set down at `to` (with `start`, that point carried to `to` instead of
// its root's middle: several roots copied as one), its outline and guide carried there over the
// scalp, its tube a copy (a root shape of its own), and the tube hierarchy under it copied level by
// level. Into the same groups. Returns the new guide's ID. Throws std::invalid_argument for a
// guide that is not a root tube.
GuideId duplicateRootTube(GuideHierarchy& h, const ScalpMesh& scalp, GuideId guide, const Vec3f& to,
                          const Vec3f* start = nullptr);
// Root tubes `guides` removed with the tube hierarchy under each (their root shapes go with them,
// and a level left with no guides its group). Returns how many guides went.
std::size_t deleteRootTubes(GuideHierarchy& h, const ScalpMesh& scalp, const std::vector<GuideId>& guides);
// How far (a share of the root's size) a root tube's footprint lies off its kept outline made
// kRootTubeOverlap larger: 0 when its tube was fitted to it as it is now.
float rootFootprintError(const GuideHierarchy& h, const ScalpMesh& scalp, GuideId guide);

// ---- Slices of a tube ----------------------------------------------------------------------------------
// A tube cut into parts that tile it all along (the hierarchy's splits, a root cut into cells:
// root_subdivision.h): each part a slice of the tube, its root shape its part of the tube's, its
// sections the tube's scaled to it, its guide the path its middle takes inside the tube.

// A slice's sections drawn this far (a share of the tube's half-width) inside its part all round:
// neighbours a slight gap apart, never touching, whatever their size.
constexpr float kSliceGap = 0.02f;

// The tube a slice is cut from, as it lies (its guide evaluated on the scalp).
struct TubeSource {
    std::vector<Vec3f> points;
    Vec3f frameUp{1, 0, 0};
    TubeProfile tube;      // (its roll as the last evaluate worked it out)
    Vec3f normal, across;  // the scalp's normal at its root, and its footprint's way across
    GroupId group = kNoGroup;
    // A new slice's parent (sliceTube with no `into`): kNoGuide, a primary guide of `group` (a root
    // group); a guide, a child of it in `group` (a subdivision's cells, in the level below their root).
    GuideId parent = kNoGuide;
};
TubeSource tubeSourceOf(const GuideHierarchy& h, const ScalpMesh& scalp, GuideId guide);
// The slice of `source` over `polygon` (points in its root section's coordinates, in order round
// it): into guide `into` (a child of the source: the parent lays its footprint and turns its
// sections), or (kNoGuide) a new guide of the source's group, its footprint laid and its sections
// turned as the source's (HierarchyGuide::acrossTurn, rollOffsets) and `outline` (world points on
// the scalp, given) kept as its root's CVs. Returns the guide, or kNoGuide for a sliver.
// `deviation` (given): added along its guide past its root (its share of the way: 0 root, 1 tip),
// the shape the guides near it were given (a cut again keeping their grooming).
GuideId sliceTube(GuideHierarchy& h, const ScalpMesh& scalp, const TubeSource& source, const std::vector<SectionPoint>& polygon,
                  GuideId into = kNoGuide, const std::vector<Vec3f>& outline = {},
                  const std::function<Vec3f(float)>& deviation = {});
// The guide of the slice whose middle is (u, v) of `source`'s root section, `extent` its larger
// half-extent (scene units): rooted there on the scalp, then along the source's tube (sliceTube's).
// `alongOnly`: the root's pull joined along the tube only (a cell of a cut: across it stays its
// part of the tube where its neighbours meet it); else all of it (a slice of the hierarchy, as its
// parent's tube and hair join their root: GuideHierarchy's resolveFollower).
std::vector<Vec3f> slicePath(const ScalpMesh& scalp, const TubeSource& source, float u, float v, float extent, float scale = 1.0f,
                             bool alongOnly = true);
// A tube spine of the source laid as its tube lies (its footprint fixed on the scalp).
TubeSpine sourceSpine(const TubeSource& source, float scale = 1.0f);
// `p` taken onto the scalp along `normal` (the nearer hit either way), else its nearest point
// (the line misses, or hits far past it).
Vec3f alongNormal(const ScalpMesh& scalp, const Vec3f& p, const Vec3f& normal);

// Where drawn roots' tubes (root tubes and their slices) pass into each other: each pair of
// neighbours of one group (`guides`: those; none: every such tube), at each point past the root
// (the same share of the way along each), how deep one's outline reaches into the other's section
// (a share of that section's half-width). The deepest of each pair, deepest first; none when they
// tile. What the tests and the hosts' checks measure.
struct TubeOverlap {
    GuideId into = kNoGuide, from = kNoGuide;
    float at = 0.0f;     // the share of the way along (0 root, 1 tip)
    float depth = 0.0f;  // how deep, a share of `into`'s half-width
};
std::vector<TubeOverlap> tubeOverlaps(const GuideHierarchy& h, const std::vector<GuideId>& guides = {});

// ---- The tube hierarchy ------------------------------------------------------------------------------

// Level `level`'s group (1: the base) in base group `base`'s hierarchy.
std::string tubeLevelGroup(const std::string& base, int level);
struct TubeLevels {
    std::string base;                 // "": no hierarchy (the groom has no guides)
    std::vector<std::string> levels;  // each level's group, the base first
};
// The base group the hierarchy works on (the top group of the first of `guides`, else of group
// `picked`, else the first top group holding guides) and its levels.
TubeLevels tubeLevels(const GuideHierarchy& h, const std::vector<GuideId>& guides = {}, const std::string& picked = {});
// Splits tubes in two (Add): each leaf tube under `guides` (none: under every guide of the base
// group) gets two child tubes side by side across its longer side, sharing its root shape, in the
// next level's group. Returns how many tubes were split. Throws std::invalid_argument when there
// is nothing to split or the tubes are kMaxTubeLevels deep.
std::size_t addTubeLevel(GuideHierarchy& h, const ScalpMesh* scalp, const std::vector<GuideId>& guides = {},
                         const std::string& picked = {});
// Undoes a split (Remove). With `guides`: a selected tube that is split loses its deepest split
// tubes, a selected split tube goes back into its parent with its siblings. Without: the deepest
// level goes everywhere. A level left with no guides goes with its group. Returns how many guides
// went (0: a whole level). Throws std::invalid_argument when there is nothing to remove.
std::size_t removeTubeLevel(GuideHierarchy& h, const ScalpMesh* scalp, const std::string& base,
                            const std::vector<GuideId>& guides = {});
// Showing level `level` of `base`'s hierarchy: each level's group and whether it shows (that level
// alone; `locked`, the card's Lock view, changes nothing here: that level's tubes stay drawn through
// the display's tubeViewGroup, display.h). The host hides the others.
std::map<std::string, bool> tubeLevelVisibility(const GuideHierarchy& h, const std::string& base, int level, int locked = 0);

}  // namespace groomlab
