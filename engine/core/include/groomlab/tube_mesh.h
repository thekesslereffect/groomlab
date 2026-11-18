// SPDX-License-Identifier: Apache-2.0
#pragma once

// Modeled hair tubes read as GroomLab sees them (Utilities > Tubes to Curves and Convert Tubes to
// GroomLab Tubes), for every host (first written for Maya in Python: tube_mesh.py and utils.py, now
// thin wrappers over these). A mesh comes in as points and polygons; no host's scene is touched.
//
// A modeled tube is a quad grid rolled into a tube, open at its ends (no caps): edge loops across
// it (its sections), edge runs along it (its columns). Its end may close on one vertex (a pointed
// tip, triangles round it). Each tube is read as rings: ring k is section k's points, in the same
// column order on every ring, ring 0 at the root.
//
//     tubes = readTubes(points, polygons)       // a ModeledTube per shell (or TubeMeshError saying what to fix)
//     orientTube(tube, rootDistance)            // the root first: the end nearer the scalp, else the wider one
//     tube.mergeBevels()                        // close loops and close runs averaged to one
//     tube.columns()                            // a point list per edge run, root to tip: the shape curves
//     tube.volumeCurves(count)                  // the center curve and others filling the tube
//
// Tubes to Curves makes guides of those curves (rootedCurve puts each root on the scalp) and gives
// each tube's guides a value of their own in a guide map (tubeRegionValues). Convert Tubes to
// GroomLab Tubes makes a tube guide through each tube's middle (tubeGuidePath), its shape the root
// loop (tubeRootShape), its sections fitted to the modeled tube along it (fitTubeSections).
//
// The math runs in doubles, as the Python did.

#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace groomlab {

struct Vec3d {
    double x = 0.0, y = 0.0, z = 0.0;
    Vec3d operator+(const Vec3d& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3d operator-(const Vec3d& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3d operator*(double s) const { return {x * s, y * s, z * s}; }
    bool operator==(const Vec3d& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const Vec3d& o) const { return !(*this == o); }
    double dot(const Vec3d& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3d cross(const Vec3d& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    double length() const;
    Vec3d normalized() const;  // (0, 0, 0) for a vector of no length
};
struct Vec2d {
    double x = 0.0, y = 0.0;
};

constexpr double kTubeBevelRatio = 0.35;  // a gap between two loops (or two runs) under this share of the gaps beside it is a bevel
// How far out the outermost ring of volume curves may sit (a share of the way to the tube's
// surface), and how fast it gets there: a few curves stay near the center, more spread out to make
// room (the reach grows with the square root of the count, as an evenly filled disc's does).
constexpr double kTubeVolumeReach = 0.8;
constexpr double kTubeVolumeBase = 0.4;
constexpr double kTubeVolumeSpread = 0.1;
constexpr int kTubeCoverDirections = 64;  // coverScale measures in this many directions
constexpr double kTubeRootFade = 0.25;    // the share of a curve's length over which rootedCurve's move fades away
constexpr double kTubeRootLift = 0.35;    // how far a tube guide's second point stands off the scalp: a share of the way to the next loop
constexpr int kTubeShapeSamples = 4;      // a smoothed section's shape: points per edge of its loop
constexpr int kTubeFitPasses = 2;         // how often a tube guide is moved onto its fitted sections' centers
// Width compensation (learned on modeled locks whose sections round out away from the root): a
// converted tube keeps its root loop's shape all along, so where the modeled sections part from that
// shape, a section holding their area still leaves some of them outside it, and the tube reads
// thinner than the mesh. Each section is drawn just wide enough that at most this share of the
// modeled section lies outside it (coverScale): where the shapes agree, nothing is added (a round
// lock: 0 to 4%); where they part, more (a kite-shaped root on rounder sections: 10 to 18%). Never
// thinner than the volume, at most 35% wider, the root as modeled, evened along.
constexpr double kTubeUncovered = 0.05;
// The guide groups the conversions make (numbered when taken: unusedName) and the guide map that
// gives each tube's guides a value of their own.
constexpr const char* kTubeShapeGroup = "tubesShape";
constexpr const char* kTubeCenterGroup = "tubesCenter";
constexpr const char* kTubeGuideGroup = "tubes";
constexpr const char* kTubeRegionMap = "tubes";

// A shell that is not a tube GroomLab can read, saying what to fix.
class TubeMeshError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

using TubeRing = std::vector<Vec3d>;

// ---- Sections ----------------------------------------------------------------------------------------

// A section's area centroid (the middle of its volume there), in the plane it best lies in; the
// points' mean when it has no area (a pointed tip).
Vec3d sectionCenter(const TubeRing& ring);
// A section's mean distance from its center.
double sectionRadius(const TubeRing& ring);
// The point `turn` (0..1) of the way round a section's outline, by length, from its first point.
Vec3d ringPoint(const TubeRing& ring, double turn);
// How far out the outermost ring of `count` volume curves sits: 0.6 for 4, 0.72 for 10,
// kTubeVolumeReach from 16.
double volumeReach(int count);
// Where the curves other than the center one go when `count` curves fill a tube: (radius, turn)
// each, radius a share of the way from the center to the surface, turn a share of the way round.
// Rings round the center, evenly spaced out to volumeReach(count), each holding curves in
// proportion to its size (as a honeycomb would: 6, 12, 18...), spread evenly round it, every other
// ring turned half a step so the curves sit between those of the ring inside.
std::vector<std::pair<double, double>> volumeLayout(int count);
// A closed uniform cubic B-spline's points through control polygon `points`, `perSpan` each span,
// the first of span i at control point i's limit ((p[i-1] + 4 p[i] + p[i+1]) / 6): a smoothed
// mesh's open border, or the section a smooth surface draws round an edge loop.
TubeRing bsplineLoop(const TubeRing& points, int perSpan);
// Each vertex where it lies on the mesh's Catmull-Clark limit surface (the surface a smooth mesh
// preview draws, at any level, converges to it), open borders smooth curves (edges kept sharp
// there, as Maya's and OpenSubdiv's boundary rules do). One subdivision step makes every face a
// quad; the limit masks then put each vertex's child where infinitely many steps would.
std::vector<Vec3d> limitPoints(const std::vector<Vec3d>& points, const std::vector<std::vector<int>>& polygons);

// ---- Tubes -------------------------------------------------------------------------------------------

// One modeled tube: `rings` (ring k: section k's points, columns in the same order on every ring,
// ring 0 at the root), `name`, and whether its last ring is a pointed tip (`pointed`, all its
// points one vertex) or its first is (`pointedRoot`). `smoothRings`: the same vertices on the
// mesh's smooth surface (limitPoints), kept in step through every change (reverse, bevels: decided
// on `rings`, as modeled) until useSmooth puts them in place of `rings`.
struct ModeledTube {
    std::vector<TubeRing> rings;
    std::optional<std::vector<TubeRing>> smoothRings;
    std::optional<std::vector<TubeRing>> cageRings;  // once useSmooth: the rings as modeled (the smooth ones in `rings`)
    std::string name = "tube";
    bool pointed = false, pointedRoot = false;
    int mergedLoops = 0, mergedRuns = 0;
    // Where it came from and how it was read (readTubes, orientTube: a live link reads it again
    // the same way): which shell of its mesh, whether orienting swapped its ends, and its loops
    // and runs as modeled.
    int shell = 0;
    bool flipped = false;
    int readLoops = 0, readSides = 0;

    std::size_t sides() const { return rings.empty() ? 0 : rings.front().size(); }
    // Root and tip swapped.
    void reverse();
    // Read the tube on its smooth surface from now on (when it has one).
    void useSmooth();
    // Read on the smooth surface (useSmooth): its sections are smooth curves, not polygons.
    bool smooth() const { return cageRings.has_value(); }
    // (root, tip): each end section's mean radius.
    std::pair<double, double> endSizes() const;
    // Each section's area centroid, root to tip: the tube's center curve.
    std::vector<Vec3d> centers() const;
    // `count` curves filling the tube, root to tip: the center curve first, then the others on
    // rings round it (volumeLayout), each at the same place in every section, so they follow the
    // tube's shape and twist and stay inside it.
    std::vector<std::vector<Vec3d>> volumeCurves(int count) const;
    // Each ring's section as drawn, root to tip: read smooth (useSmooth), the curve the smooth
    // surface draws round that edge loop (a cubic B-spline round the loop through the modeled
    // rings: each control point the vertex's (above + 4 * it + below) / 6, the loop itself at an
    // open end), `perSpan` points per edge; else the loop's own points.
    std::vector<TubeRing> sectionLoops(int perSpan = kTubeShapeSamples) const;
    // A point list per edge run along the tube, root to tip (a pointed end: each run ends on its vertex).
    std::vector<std::vector<Vec3d>> columns() const;
    // Average away bevels: edge loops lying much closer together than the loops beside them become
    // one loop (their mean; the root's and the tip's loop kept as they are), and edge runs lying
    // much closer together round the tube than the runs beside them become one run. So the tube
    // reads as it would without its bevels. Returns (loops removed, runs removed).
    std::pair<int, int> mergeBevels(double ratio = kTubeBevelRatio);
};

// The tubes of a mesh: `points` its vertex positions, `polygons` each face's vertex indices. One
// ModeledTube per shell, named `name` (shells after the first: name_2, name_3...), its `shell` the
// shell's index. Ring 0 at the end that is open (either, both open). Throws TubeMeshError on a
// shell that is not an open quad tube, saying which and what to fix. With `smooth` each tube also
// carries its vertices on the smooth surface (ModeledTube::smoothRings, limitPoints).
std::vector<ModeledTube> readTubes(const std::vector<Vec3d>& points, const std::vector<std::vector<int>>& polygons,
                                   const std::string& name = "tube", bool smooth = false);
// Put the tube's root first: the end nearer the scalp (`rootDistance(point)`: a point's distance
// to it), else the wider end (an open end before a pointed one). Returns whether it swapped the
// ends (kept as ModeledTube::flipped).
bool orientTube(ModeledTube& tube, const std::function<double(const Vec3d&)>& rootDistance = {});
// A tube as the conversions take it, from readTubes: root first (orientTube), bevels merged with
// `ignoreBevels` (told apart as modeled: the smooth surface rounds them away), then on its smooth
// surface when it was read with one.
void prepareTube(ModeledTube& tube, bool ignoreBevels, const std::function<double(const Vec3d&)>& rootDistance = {});

// ---- Guides from tubes --------------------------------------------------------------------------------

// `points` with the root moved onto the scalp (`rootOnScalp`: the scalp's nearest point to
// points[0]), the move fading out over the first kTubeRootFade of the curve. A guide made from a
// curve is moved whole so its root lands on the scalp; each curve of a tube would move by its own
// amount (the middle of a root loop lies deeper than its edge) and the inner ones could leave the
// tube near its thin tip. Rooted here first, nothing moves them whole, and every curve keeps its
// place in the tube past the root.
std::vector<Vec3d> rootedCurve(const std::vector<Vec3d>& points, const Vec3d& rootOnScalp);
// A tube guide's points through a tube's section centers: its root on the scalp under the root
// section (`root`, the scalp's `normal` there), and its second point straight off the scalp from
// there (kTubeRootLift of the way to the next section), so the root section lies on the scalp as
// the modeled root loop does (a section is square to the guide).
std::vector<Vec3d> tubeGuidePath(const std::vector<Vec3d>& centers, const Vec3d& root, const Vec3d& normal);

// `count` distinct grays for tubes, one each: evenly spaced in (0, 1), handed out a golden step
// apart so neighbouring tubes (made one after another) differ by about a third or more.
std::vector<double> tubeRegionValues(int count);
// `count` tube values for tubes added to a map whose tubes have `used` (their distinct values,
// rounded to 4 places, 0 left out): tubeRegionValues of the whole count, the ones well clear of
// every value already used.
std::vector<double> extraTubeRegionValues(const std::vector<double>& used, int count);
// A guide map's value as the tube maps keep them: rounded to 4 places.
double roundedTubeRegionValue(double value);
// Whether `name` is a tube region map's: kTubeRegionMap, or it numbered (tubes2, tubes3...).
bool isTubeMapName(const std::string& name);
// `base`, or base2, base3...: the first that is not in `taken` (a new guide group or guide map).
std::string unusedName(const std::string& base, const std::vector<std::string>& taken);
// A library shape's name from `base` (a mesh's name): only the characters a shape name takes, at
// most 56, "_shape" added to a built-in shape's name (`builtin`), numbered (_2, _3...) while in
// `taken`.
std::string tubeShapeNameFor(const std::string& base, const std::vector<std::string>& taken,
                             const std::vector<std::string>& builtin);

// ---- Fitting GroomLab sections (tube.h) ---------------------------------------------------------------
// A section puts shape point (u, v) at center + rotate(twist) * (u * width, v * width * aspect) in
// the section's axes.

double signedArea(const std::vector<Vec2d>& points);

struct SectionFit {
    double twist = 0.0, width = 0.0, height = 0.0;
    Vec2d center;  // off the axes' origin, in the axes
};
// The section that best puts `shape`'s points (u, v) on `points` (each its own, in order), points
// in the section's axes at twist 0. `nearTwist`: the twist to stay closest to (the last section's), for
// the same fit a turn away. A section with no size (a pointed tip) keeps `nearTwist` (0 without).
SectionFit fitSection(const std::vector<Vec2d>& points, const std::vector<Vec2d>& shape, const double* nearTwist = nullptr);
// A section's points as a tube shape's control points: counter-clockwise, at most `limit` (evenly
// picked). `indices`, when given: which of `points` each is.
std::vector<Vec2d> shapePoints(const std::vector<Vec2d>& points, std::vector<std::size_t>* indices = nullptr,
                               std::size_t limit = 64);
// How GroomLab fitted a shape's points (`given`) into its square (`fitted`, maybe reordered): for
// each given point, its fitted point. Empty when they do not match point for point.
std::vector<Vec2d> matchFitted(const std::vector<Vec2d>& given, const std::vector<Vec2d>& fitted);
// `outline` (shape coordinates) as a section of that twist, width, height and center draws it.
std::vector<Vec2d> placeOutline(const std::vector<Vec2d>& outline, double twist, double width, double height,
                                const Vec2d& center = {});
// The point of closed polygon `placed` nearest q, as the matching point of `outline` (the same
// polygon in shape coordinates), and its distance.
std::pair<Vec2d, double> closestOnOutline(const Vec2d& q, const std::vector<Vec2d>& placed, const std::vector<Vec2d>& outline);
// Where a tube's surface crosses a plane (through `origin`, square to `normal`): for each of its
// `columns` (point lists along the tube, root to tip), the crossing nearest `origin` (the column's
// point nearest the plane when it does not cross it). The section the tube draws there, one point
// per column, in the columns' order.
TubeRing planeSection(const std::vector<std::vector<Vec3d>>& columns, const Vec3d& origin, const Vec3d& normal);
// How far a closed polygon (2D, round the origin) reaches from the origin in direction `angle`:
// the farthest crossing of that ray with it (nothing when the ray misses it).
std::optional<double> rayReach(const std::vector<Vec2d>& polygon, double angle);
// How much wider (both ways) `outline` must be drawn so that at most `uncovered` of `section`'s
// area lies outside it: both closed polygons round the origin, in the same plane. Measured
// direction by direction (kTubeCoverDirections, both star-shaped from the origin): the section's
// area outside the outline drawn k times as large is sum(max(0, r^2 - k^2)) over sum(r^2), r the
// section's reach over the outline's. Between `least` and `most` (never smaller: the volume holds;
// a shape very unlike the section is not blown up past the upper one).
double coverScale(const std::vector<Vec2d>& section, const std::vector<Vec2d>& outline, double uncovered = kTubeUncovered,
                  double least = 1.0, double most = 1.35);
// The section turned `twist` that just holds `points` (a loop as drawn, in the section's axes):
// its width and height each half the loop's extent along the turned axes, its center that extent's
// middle (its twist: `twist`). A shape fills its -1..1 box, so the section spans the loop.
SectionFit sectionBox(const std::vector<Vec2d>& points, double twist);
// Where on a polyline (points evenly spaced) p lies nearest: its param 0 to 1 by point index.
double polylineParam(const std::vector<Vec3d>& points, const Vec3d& p);

// A guide's points and its section axes at each (u, v: its frames turned by its roll).
struct TubeFrames {
    std::vector<Vec3d> points, u, v;
};
struct TubeFrame {
    Vec3d center, u, v;  // u and v unit and square
};
// The guide's section axes at param t.
TubeFrame frameAt(const TubeFrames& frames, double t);

// The tube's surface as point lists along it (ModeledTube::sectionLoops' points, column by
// column); `perVertex`: how many of them per vertex round a loop (vertex i: column i * perVertex).
std::vector<std::vector<Vec3d>> tubeSurfaceColumns(const ModeledTube& tube, int* perVertex = nullptr);

// The tube's root section as a shape, in the guide's section axes at `t`: a point per vertex of
// its root loop (at most kMaxTubeShapePoints, counter-clockwise), read smooth where each lies on
// the smooth surface and drawn as a smooth curve through them (as a smooth mesh preview rounds the
// loop), else the vertices as modeled with straight edges. `vertices`: the root loop vertex each
// point is.
struct TubeRootShape {
    std::vector<Vec2d> points;
    std::vector<std::size_t> vertices;
    bool smooth = false;
};
TubeRootShape tubeRootShape(const ModeledTube& tube, const TubeFrames& frames, double t);

struct TubeSectionFit {
    double t = 0.0, twist = 0.0, width = 0.0, height = 0.0;
    Vec3d center;
    double cover = 1.0;
};
// The section at each of the guide's points, where the modeled tube crosses the plane square to
// the guide there (planeSection: the surface as drawn, so a bend's leaning loops count as the
// tube's real thickness). The twist puts each shape point nearest the edge through its own vertex
// (`shapeUv`: the shape's point of each root loop vertex in `vertices`), so the tube turns as the
// modeled edges turn and no more. The root's shape is the root loop's, so the first `rootPoints`
// (the root and the point straight off the scalp) take the root loop itself and match it. Along
// the tube only its thickness counts, not its shape: each section centered on the cut, as flat as
// the cut is across the shape's axes (sectionBox), and sized so the shape's `outline` (its drawn
// curve, in shape coordinates; empty: as boxed) holds the cut's area: the modeled volume. With
// `uncovered`, each section's `cover`: how much wider it must be drawn for at most that share of
// the cut to lie outside it (coverScale; 1 at the root points: the root loop is the shape). A
// section with no size (a pointed tip) keeps the last twist.
std::vector<TubeSectionFit> fitTubeSections(const TubeFrames& frames, const ModeledTube& tube, const std::vector<Vec2d>& shapeUv,
                                            const std::vector<std::size_t>& vertices, std::size_t rootPoints = 0,
                                            const std::vector<Vec2d>& outline = {}, const std::optional<double>& uncovered = {});
// The sections' cover factors evened out along the tube (two passes of 1-2-1), the first `keep`
// (the root points) left at 1: the widths change gently where the shape parts from the modeled
// sections.
std::vector<double> smoothedCover(const std::vector<TubeSectionFit>& sections, std::size_t keep);
// Each section's aspect (height over width, 0.01 to 100), one with no width taking the last one's.
std::vector<double> sectionAspects(const std::vector<TubeSectionFit>& sections);

}  // namespace groomlab
