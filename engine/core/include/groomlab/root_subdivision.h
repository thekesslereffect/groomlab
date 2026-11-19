// SPDX-License-Identifier: Apache-2.0
#pragma once

// Subdividing a root tube (issue 85), the Tube tool's Subdivide and Paint subdivision density in
// every host.
//
// One big root drawn on the scalp is flood-filled with Voronoi cells, each a root tube of its own
// (root_tubes.h) at level 1 of the tube hierarchy. Where the density map (kSubdivisionDensityMap,
// a gray vertex map of the scalp: 1 full density, 0 the least) is high, the cells are smaller and
// more numerous. Each cell's guide follows the tube it was cut from: the path a strand rooted
// there takes inside the old tube (TubeSpine), so the cells keep the old guide's shape, spread
// and taper. Where the old root had a tube hierarchy under it, each cell is split as deep, each
// level made afresh from the new cell (a drawn root sliced in two, as + slices it).
//
// The groom remembers each subdivision (GuideHierarchy::rootSubdivisions, saved): the old
// outline, the cells' sites and the density they were cut at. Painting the map again and
// finishing (applySubdivisionDensity) cuts again only where the density changed: the sites there
// are thrown again, the rest kept, and a cell whose outline comes out the same keeps its guide and
// hierarchy untouched. The new cells take their level 1 shape from the cells they replace; the
// levels under them are made afresh from the new cells (none of the old shapes carried).
//
// The density is read from the scalp's color set of the map's name (the graph's Map nodes read
// vertex maps so: scalp_maps.h withVertexMaps); a scalp without one reads kDefaultSubdivisionDensity.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "groomlab/guide_hierarchy.h"
#include "groomlab/math.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {

// One gray vertex map paints every subdivision: kSubdivisionDensityMap, made at
// kDefaultSubdivisionDensity. (Cuts made with the older numbered maps, tubeSubdivisionDensity01, ...,
// read it when applied with it.) A cell of a cut cut again is a destructive cut: not remembered, the
// map leaves it be.
constexpr const char* kSubdivisionDensityMap = "tubeSubdivisionDensity";
constexpr float kDefaultSubdivisionDensity = 0.5f;  // the map's value when made, and where there is none

// A point in the plane a root is cut in (scene units).
struct CutPoint {
    double x = 0.0, y = 0.0;
};

struct RootSubdivisionOptions {
    // Each cell's size (about its width), a share of the old root's size (the square root of its
    // area), at density 0 and at density 1; in between, blended.
    float sizeAtZero = 0.5f;
    float sizeAtOne = 0.12f;
    std::uint64_t seed = 0x85;           // where the sites fall (the same seed: the same cells)
    // The density map the cut reads. applySubdivisionDensity with one: the subdivisions painted with
    // it (or a numbered map named after it) are cut again by it; "": each with its own. Cut again,
    // the new cells keep the shape the cells near them were groomed to, and their point count.
    std::string map = kSubdivisionDensityMap;
    // How dense overall, with the painted density: 1 as painted, 2 about twice the cells (each
    // smaller by sqrt 2), 0.5 half. A subdivision remembers it; applySubdivisionDensity with another
    // cuts its whole root again at that (0 or less: each keeps its own).
    float multiplier = 1.0f;
    // applySubdivisionDensity: cut the map's roots again all over, whether or not the density or
    // the multiplier changed (the card's Apply).
    bool force = false;
    int relax = 2;                       // passes evening the cells out (Lloyd), each half a step
};

// The density map named for subdivision number `n` (1 on): tubeSubdivisionDensity01, ...
std::string subdivisionDensityMapName(int n);
// The density map for cutting `guides` (root tubes, or tubes of their hierarchies): the map of the
// subdivision the first of them is a cell of (painting it again cuts that subdivision again), else
// a new one: the first subdivisionDensityMapName that no subdivision and none of `taken` (the
// groom's maps) uses. `fresh` says which.
std::string subdivisionDensityMapFor(const GuideHierarchy& h, const std::vector<GuideId>& guides,
                                     const std::vector<std::string>& taken, bool* fresh = nullptr);

// A flat cut: `outline` (a simple polygon, either way round) filled with Voronoi cells about
// `sites`, each cell the outline clipped to its site's side of every other site; each counter-
// clockwise, the points shared by neighbouring cells exactly equal (welded), so cells meet with no
// gap. A site outside the outline gets an empty cell. Corners nearer each other than `mergeBelow`
// (scene units; 0: none) are made one (their middle; an outline CV among them stays where it is),
// so no cell keeps a sliver of an edge. Pure 2D: the tests and the scalp cut use it.
std::vector<std::vector<CutPoint>> voronoiCells(const std::vector<CutPoint>& outline, const std::vector<CutPoint>& sites,
                                                double mergeBelow = 0.0);
// Sites for the cells inside `outline`, spaced about `size(p)` apart (dart throwing over a
// jittered grid finer than the smallest size, then `relax` half Lloyd steps), from `seed`. Sites
// in `kept` stay where they are and count as thrown; none land nearer them than the spacing.
std::vector<CutPoint> subdivisionSites(const std::vector<CutPoint>& outline, const std::function<float(const CutPoint&)>& size,
                                    std::uint64_t seed, int relax = 2, const std::vector<CutPoint>& kept = {});

struct SubdivisionResult {
    std::vector<GuideId> cells;  // the root tubes made (level 1)
    std::size_t removed = 0;     // the guides taken out (the old root, or the cells replaced, with their hierarchies)
    std::size_t kept = 0;        // cells left as they were (applySubdivisionDensity)
};

// Root tubes `guides` (rootTubesOf) each cut into cells at the density the scalp's map gives,
// replacing it (and its hierarchy) as above, and remembered. Throws std::invalid_argument for a
// selection with no root tube (a smaller tube of the hierarchy stands for its root tube).
SubdivisionResult subdivideRootTubes(GuideHierarchy& h, const ScalpMesh& scalp, const std::vector<GuideId>& guides,
                                     const RootSubdivisionOptions& options = {});
// Painting finished: each remembered subdivision whose density changed cut again where it changed
// (its unchanged cells kept), and `guides` (root tubes not yet cut; none: only the remembered
// ones) cut. Returns what changed (nothing: no density changed and no root given).
SubdivisionResult applySubdivisionDensity(GuideHierarchy& h, const ScalpMesh& scalp, const std::vector<GuideId>& guides = {},
                                          const RootSubdivisionOptions& options = {});
// The density the scalp's map (`map`) gives at `p` (its nearest point), 0 to 1:
// kDefaultSubdivisionDensity when the scalp has no such color set.
float subdivisionDensityAt(const ScalpMesh& scalp, const Vec3f& p, const std::string& map = kSubdivisionDensityMap);

}  // namespace groomlab
