// SPDX-License-Identifier: Apache-2.0
// Modeled tubes read off a mesh (tube_mesh.h) as groomlab_core.tube_mesh: what Tubes to Curves and
// Convert Tubes to GroomLab Tubes work out, for a host's Python to put in its scene. Points are
// tuples of Python floats (doubles), as the first version in Maya's Python had them.

#include "bind_common.h"

#include <optional>

#include "groomlab/tube.h"
#include "groomlab/tube_mesh.h"

namespace pybind11::detail {

// A 3-sequence of numbers (or a Vec3) in, a tuple of 3 floats out.
template <>
struct type_caster<groomlab::Vec3d> {
    PYBIND11_TYPE_CASTER(groomlab::Vec3d, const_name("Tuple[float, float, float]"));
    bool load(handle src, bool) {
        if (!src) return false;
        if (PySequence_Check(src.ptr()) && !isinstance<str>(src)) {
            const auto seq = reinterpret_borrow<sequence>(src);
            if (seq.size() != 3) return false;
            try {
                value = {seq[0].cast<double>(), seq[1].cast<double>(), seq[2].cast<double>()};
            } catch (const cast_error&) {
                return false;
            }
            return true;
        }
        groomlab::Vec3f f;
        if (!groomlab::py_bind::loadVec3(src, f)) return false;
        value = {f.x, f.y, f.z};
        return true;
    }
    static handle cast(const groomlab::Vec3d& v, return_value_policy, handle) { return make_tuple(v.x, v.y, v.z).release(); }
};

// A 2-sequence of numbers in, a tuple of 2 floats out.
template <>
struct type_caster<groomlab::Vec2d> {
    PYBIND11_TYPE_CASTER(groomlab::Vec2d, const_name("Tuple[float, float]"));
    bool load(handle src, bool) {
        if (!src || !PySequence_Check(src.ptr()) || isinstance<str>(src)) return false;
        const auto seq = reinterpret_borrow<sequence>(src);
        if (seq.size() != 2) return false;
        try {
            value = {seq[0].cast<double>(), seq[1].cast<double>()};
        } catch (const cast_error&) {
            return false;
        }
        return true;
    }
    static handle cast(const groomlab::Vec2d& v, return_value_policy, handle) { return make_tuple(v.x, v.y).release(); }
};

}  // namespace pybind11::detail

namespace groomlab::py_bind {

namespace {

using RootDistance = std::function<double(const Vec3d&)>;

// A guide's frames as groomLabGuides -tubeFrames (and the other hosts' like) give them: a dict of
// "points", "u" and "v".
TubeFrames framesOf(const py::dict& frames) {
    TubeFrames out;
    out.points = frames["points"].cast<std::vector<Vec3d>>();
    out.u = frames["u"].cast<std::vector<Vec3d>>();
    out.v = frames["v"].cast<std::vector<Vec3d>>();
    return out;
}

py::tuple sectionTuple(const SectionFit& s) { return py::make_tuple(s.twist, s.width, s.height, s.center); }

// Fitted sections as dicts (t, twist, width, height, center, cover), and back.
py::dict sectionDict(const TubeSectionFit& s) {
    py::dict d;
    d["t"] = s.t;
    d["twist"] = s.twist;
    d["width"] = s.width;
    d["height"] = s.height;
    d["center"] = s.center;
    d["cover"] = s.cover;
    return d;
}

std::vector<TubeSectionFit> sectionsOf(const py::list& sections) {
    std::vector<TubeSectionFit> out;
    for (const py::handle item : sections) {
        const auto d = py::reinterpret_borrow<py::dict>(item);
        TubeSectionFit s;
        s.width = d["width"].cast<double>();
        s.height = d["height"].cast<double>();
        if (d.contains("cover")) s.cover = d["cover"].cast<double>();
        out.push_back(s);
    }
    return out;
}

}  // namespace

void bindTubeMesh(py::module_& m) {
    py::module_ t = m.def_submodule("tube_mesh", "Modeled hair tubes read off a mesh: their rings, curves and fitted sections.");

    t.attr("BEVEL_RATIO") = kTubeBevelRatio;
    t.attr("MAX_SHAPE_POINTS") = kMaxTubeShapePoints;
    t.attr("VOLUME_REACH") = kTubeVolumeReach;
    t.attr("VOLUME_BASE") = kTubeVolumeBase;
    t.attr("VOLUME_SPREAD") = kTubeVolumeSpread;
    t.attr("COVER_DIRECTIONS") = kTubeCoverDirections;
    t.attr("ROOT_FADE") = kTubeRootFade;
    t.attr("ROOT_LIFT") = kTubeRootLift;
    t.attr("SHAPE_SAMPLES") = kTubeShapeSamples;
    t.attr("FIT_PASSES") = kTubeFitPasses;
    t.attr("UNCOVERED") = kTubeUncovered;
    t.attr("SHAPE_GROUP") = kTubeShapeGroup;
    t.attr("CENTER_GROUP") = kTubeCenterGroup;
    t.attr("GUIDE_GROUP") = kTubeGuideGroup;
    t.attr("REGION_MAP") = kTubeRegionMap;

    py::register_exception<TubeMeshError>(t, "TubeMeshError", PyExc_ValueError);

    // ---- Sections ----
    t.def("section_center", &sectionCenter, py::arg("ring"),
          "A section's area centroid, in the plane it best lies in; the points' mean when it has no area (a pointed tip).");
    t.def("section_radius", &sectionRadius, py::arg("ring"), "A section's mean distance from its center.");
    t.def("ring_point", &ringPoint, py::arg("ring"), py::arg("turn"),
          "The point `turn` (0..1) of the way round a section's outline, by length, from its first point.");
    t.def("volume_reach", &volumeReach, py::arg("count"), "How far out the outermost ring of `count` volume curves sits.");
    t.def("volume_layout", &volumeLayout, py::arg("count"),
          "Where the curves other than the center one go when `count` curves fill a tube: [(radius, turn)].");
    t.def("bspline_loop", &bsplineLoop, py::arg("points"), py::arg("per_span"),
          "A closed uniform cubic B-spline's points through a control polygon, `per_span` each span.");
    t.def("limit_points", &limitPoints, py::arg("points"), py::arg("polygons"),
          "Each vertex where it lies on the mesh's Catmull-Clark limit surface, open borders smooth curves.");

    // ---- Tubes ----
    py::class_<ModeledTube>(t, "ModeledTube", py::dynamic_attr(),
                            "One modeled tube: rings (ring k: section k's points, columns in the same order on every ring, "
                            "ring 0 at the root), a name, and whether an end is pointed.")
        .def(py::init([](std::vector<TubeRing> rings, const std::string& name, bool pointed, bool pointedRoot,
                         std::optional<std::vector<TubeRing>> smoothRings) {
                 ModeledTube tube;
                 tube.rings = std::move(rings);
                 tube.smoothRings = std::move(smoothRings);
                 tube.name = name;
                 tube.pointed = pointed;
                 tube.pointedRoot = pointedRoot;
                 tube.readLoops = static_cast<int>(tube.rings.size());
                 tube.readSides = static_cast<int>(tube.sides());
                 return tube;
             }),
             py::arg("rings"), py::arg("name") = "tube", py::arg("pointed") = false, py::arg("pointed_root") = false,
             py::arg("smooth_rings") = py::none())
        .def_readwrite("rings", &ModeledTube::rings, "The rings, root to tip (a copy: lists of point tuples).")
        .def_readwrite("smooth_rings", &ModeledTube::smoothRings,
                       "The same vertices on the mesh's smooth surface, until use_smooth (None without).")
        .def_readonly("cage_rings", &ModeledTube::cageRings, "Once use_smooth: the rings as modeled (else None).")
        .def_readwrite("name", &ModeledTube::name)
        .def_readwrite("pointed", &ModeledTube::pointed, "Its last ring is a pointed tip.")
        .def_readwrite("pointed_root", &ModeledTube::pointedRoot, "Its first ring is a pointed tip.")
        .def_readonly("merged_loops", &ModeledTube::mergedLoops)
        .def_readonly("merged_runs", &ModeledTube::mergedRuns)
        .def_readwrite("shell", &ModeledTube::shell, "Which shell of its mesh it is.")
        .def_readwrite("flipped", &ModeledTube::flipped, "Whether orienting swapped its ends.")
        .def_property_readonly(
            "signature", [](const ModeledTube& tube) { return py::make_tuple(tube.readLoops, tube.readSides); },
            "(loops, sides) as it was read, before its bevels merged.")
        .def_property_readonly("sides", &ModeledTube::sides)
        .def_property_readonly("smooth", &ModeledTube::smooth, "Read on the smooth surface (use_smooth).")
        .def("reverse", &ModeledTube::reverse, "Root and tip swapped.")
        .def(
            "use_smooth",
            [](py::object self) {
                self.cast<ModeledTube&>().useSmooth();
                return self;
            },
            "Read the tube on its smooth surface from now on (when it has one); returns the tube.")
        .def("end_sizes", &ModeledTube::endSizes, "(root, tip): each end section's mean radius.")
        .def("centers", &ModeledTube::centers, "Each section's area centroid, root to tip: the tube's center curve.")
        .def("volume_curves", &ModeledTube::volumeCurves, py::arg("count"),
             "`count` curves filling the tube, root to tip: the center curve first, then the others on rings round it.")
        .def("section_loops", &ModeledTube::sectionLoops, py::arg("per_span") = kTubeShapeSamples,
             "Each ring's section as drawn, root to tip (read smooth: `per_span` points per edge).")
        .def("columns", &ModeledTube::columns, "A point list per edge run along the tube, root to tip.")
        .def("merge_bevels", &ModeledTube::mergeBevels, py::arg("ratio") = kTubeBevelRatio,
             "Average away bevels (close loops, close runs); returns (loops removed, runs removed).");

    t.def("read_tubes", &readTubes, py::arg("points"), py::arg("polygons"), py::arg("name") = "tube", py::arg("smooth") = false,
          "The tubes of a mesh (points, each face's vertex indices): a ModeledTube per shell. TubeMeshError (a ValueError) "
          "on a shell that is not an open quad tube, saying which and what to fix.");
    t.def(
        "orient_tube", [](ModeledTube& tube, const RootDistance& rootDistance) { return orientTube(tube, rootDistance); },
        py::arg("tube"), py::arg("root_distance") = py::none(),
        "Put the tube's root first: the end nearer the scalp (root_distance(point)), else the wider end. Returns whether "
        "it swapped the ends.");
    t.def(
        "prepare_tube",
        [](ModeledTube& tube, bool ignoreBevels, const RootDistance& rootDistance) { prepareTube(tube, ignoreBevels, rootDistance); },
        py::arg("tube"), py::arg("ignore_bevels") = true, py::arg("root_distance") = py::none(),
        "A tube as the conversions take it: root first, bevels merged with `ignore_bevels`, then on its smooth surface "
        "when it was read with one.");

    // ---- Guides from tubes ----
    t.def("rooted_curve", &rootedCurve, py::arg("points"), py::arg("root_on_scalp"),
          "`points` with the root moved onto the scalp, the move fading out over the first ROOT_FADE of the curve.");
    t.def("tube_guide_path", &tubeGuidePath, py::arg("centers"), py::arg("root"), py::arg("normal"),
          "A tube guide's points through a tube's section centers: its root on the scalp, its second point straight off it.");
    t.def("region_values", &tubeRegionValues, py::arg("count"),
          "`count` distinct grays for tubes, one each, neighbouring tubes far apart.");
    t.def("extra_region_values", &extraTubeRegionValues, py::arg("used"), py::arg("count"),
          "`count` tube values for tubes added to a map whose tubes have `used`.");
    t.def("rounded_region_value", &roundedTubeRegionValue, py::arg("value"), "A tube map's value as kept: rounded to 4 places.");
    t.def("is_tube_map_name", &isTubeMapName, py::arg("name"), "Whether `name` is a tube region map's (tubes, tubes2...).");
    t.def("unused_name", &unusedName, py::arg("base"), py::arg("taken"), "`base`, or base2, base3...: the first not in `taken`.");
    t.def("shape_name_for", &tubeShapeNameFor, py::arg("base"), py::arg("taken"), py::arg("builtin"),
          "A library shape's name from a mesh's name: a shape name's characters, not a built-in's, not in `taken`.");

    // ---- Fitting sections ----
    t.def("signed_area", &signedArea, py::arg("points"));
    t.def(
        "fit_section",
        [](const std::vector<Vec2d>& points, const std::vector<Vec2d>& shape, const std::optional<double>& nearTwist) {
            return sectionTuple(fitSection(points, shape, nearTwist ? &*nearTwist : nullptr));
        },
        py::arg("points"), py::arg("shape"), py::arg("near") = py::none(),
        "The section that best puts `shape`'s points on `points`: (twist, width, height, (cx, cy)).");
    t.def(
        "shape_points",
        [](const std::vector<Vec2d>& points, std::size_t limit) {
            std::vector<std::size_t> indices;
            std::vector<Vec2d> pts = shapePoints(points, &indices, limit);
            return py::make_tuple(pts, indices);
        },
        py::arg("points"), py::arg("limit") = kMaxTubeShapePoints,
        "A section's points as a tube shape's control points: (points, which of the given each is).");
    t.def(
        "match_fitted",
        [](const std::vector<Vec2d>& given, const std::vector<Vec2d>& fitted) -> py::object {
            const std::vector<Vec2d> match = matchFitted(given, fitted);
            if (match.empty()) return py::none();
            return py::cast(match);
        },
        py::arg("given"), py::arg("fitted"),
        "For each given shape point, its fitted point in the library's (maybe reordered); None when they do not match.");
    t.def("place_outline", &placeOutline, py::arg("outline"), py::arg("twist"), py::arg("width"), py::arg("height"),
          py::arg("center") = Vec2d{}, "`outline` (shape coordinates) as a section of that twist, width, height and center draws it.");
    t.def("closest_on_outline", &closestOnOutline, py::arg("q"), py::arg("placed"), py::arg("outline"),
          "The point of closed polygon `placed` nearest q, as the matching point of `outline`, and its distance.");
    t.def("plane_section", &planeSection, py::arg("columns"), py::arg("origin"), py::arg("normal"),
          "Where a tube's surface (its columns) crosses a plane: one point per column.");
    t.def("ray_reach", &rayReach, py::arg("polygon"), py::arg("angle"),
          "How far a closed polygon round the origin reaches in direction `angle` (None when the ray misses it).");
    t.def(
        "cover_scale",
        [](const std::vector<Vec2d>& section, const std::vector<Vec2d>& outline, double uncovered,
           const std::pair<double, double>& limits) {
            return coverScale(section, outline, uncovered, limits.first, limits.second);
        },
        py::arg("section"), py::arg("outline"), py::arg("uncovered") = kTubeUncovered, py::arg("limits") = std::make_pair(1.0, 1.35),
        "How much wider `outline` must be drawn so that at most `uncovered` of `section`'s area lies outside it.");
    t.def(
        "section_box",
        [](const std::vector<Vec2d>& points, double twist) {
            const SectionFit box = sectionBox(points, twist);
            return py::make_tuple(box.width, box.height, box.center);
        },
        py::arg("points"), py::arg("twist"), "The section turned `twist` that just holds `points`: (width, height, center).");
    t.def("polyline_param", &polylineParam, py::arg("points"), py::arg("p"),
          "Where on a polyline (points evenly spaced) p lies nearest: its param 0 to 1 by point index.");
    t.def(
        "frame_at",
        [](const py::dict& frames, double at) {
            const TubeFrame f = frameAt(framesOf(frames), at);
            return py::make_tuple(f.center, f.u, f.v);
        },
        py::arg("frames"), py::arg("t"), "A guide's section axes ({points, u, v}) at param t: (center, u, v).");
    t.def(
        "surface_columns",
        [](const ModeledTube& tube) {
            int per = 1;
            std::vector<std::vector<Vec3d>> columns = tubeSurfaceColumns(tube, &per);
            return py::make_tuple(columns, per);
        },
        py::arg("tube"), "The tube's surface as point lists along it, and how many of them per vertex round a loop.");
    t.def(
        "root_shape",
        [](const ModeledTube& tube, const py::dict& frames, double at) {
            const TubeRootShape shape = tubeRootShape(tube, framesOf(frames), at);
            py::dict where;
            for (std::size_t j = 0; j < shape.vertices.size(); ++j) where[py::int_(shape.vertices[j])] = j;
            return py::make_tuple(shape.points, shape.smooth, where);
        },
        py::arg("tube"), py::arg("frames"), py::arg("t"),
        "The tube's root section as a shape in the guide's section axes at t: (points, smooth, {root vertex: its point}).");
    t.def(
        "fit_sections",
        [](const py::dict& frames, const ModeledTube& tube, const std::vector<Vec2d>& shapeUv, const std::vector<std::size_t>& vertices,
           std::size_t rootPoints, const std::optional<std::vector<Vec2d>>& outline, const std::optional<double>& uncovered) {
            py::list out;
            const std::vector<Vec2d> drawn = outline ? *outline : std::vector<Vec2d>{};
            for (const TubeSectionFit& s : fitTubeSections(framesOf(frames), tube, shapeUv, vertices, rootPoints, drawn, uncovered)) {
                out.append(sectionDict(s));
            }
            return out;
        },
        py::arg("frames"), py::arg("tube"), py::arg("shape_uv"), py::arg("vertices"), py::arg("root_points") = 0,
        py::arg("outline") = py::none(), py::arg("uncovered") = py::none(),
        "The section at each of the guide's points, where the modeled tube crosses the plane square to the guide there: "
        "[{t, twist, width, height, center, cover}].");
    t.def(
        "smoothed_cover", [](const py::list& sections, std::size_t keep) { return smoothedCover(sectionsOf(sections), keep); },
        py::arg("sections"), py::arg("keep"), "The sections' cover factors evened out along the tube, the first `keep` left at 1.");
    t.def(
        "section_aspects", [](const py::list& sections) { return sectionAspects(sectionsOf(sections)); }, py::arg("sections"),
        "Each section's aspect (height over width, 0.01 to 100), one with no width taking the last one's.");
}

}  // namespace groomlab::py_bind
