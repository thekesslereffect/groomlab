// SPDX-License-Identifier: Apache-2.0
// StrandSet (with buffer views), InstanceSet, ScalpMesh, GuideSet and Texture.

#include "bind_common.h"

#include <limits>
#include <sstream>

#include "groomlab/cards.h"
#include "groomlab/guide_set.h"
#include "groomlab/instances.h"
#include "groomlab/scale_maps.h"
#include "groomlab/scales.h"
#include "groomlab/subdivision.h"
#include "groomlab/texture.h"

namespace groomlab::py_bind {

namespace {

StrandSet& strandsOf(py::handle self) { return self.cast<StrandSet&>(); }

void checkStrand(const StrandSet& s, std::size_t strand) {
    if (strand >= s.strandCount()) throw py::index_error("strand index out of range");
}

std::vector<Vec3f> strandPointList(const StrandSet& s, std::size_t strand) {
    checkStrand(s, strand);
    const Vec3f* p = s.strandPoints(strand);
    return std::vector<Vec3f>(p, p + s.strandPointCount(strand));
}

// A per-strand or per-point channel view (null when absent).
py::object channelView(py::object self, const std::vector<float>* values, bool readonly) {
    if (!values) return py::none();
    return py::cast(flatView(std::move(self), *values, readonly));
}

void assignFloats(std::vector<float>& dst, py::handle src, const char* what) {
    const std::vector<float> values = toFloats(src);
    if (values.size() != dst.size()) {
        throw py::value_error(std::string(what) + ": expected " + std::to_string(dst.size()) + " values, got " +
                              std::to_string(values.size()));
    }
    dst = values;
}

}  // namespace

void bindStrands(py::module_& m) {
    // ---- InstanceSet -----------------------------------------------------
    py::enum_<InstanceOrient>(m, "InstanceOrient", "How an instance's +Y is oriented.")
        .value("Strand", InstanceOrient::Strand)
        .value("Up", InstanceOrient::Up)
        .value("Root", InstanceOrient::Root)
        .value("Random", InstanceOrient::Random);

    py::class_<InstanceSet, std::shared_ptr<InstanceSet>> instanceSet(
        m, "InstanceSet", "Geometry instanced on strands (Instance operator): prototypes by name and where each sits.");
    py::class_<InstanceSet::Instance>(instanceSet, "Instance", "One instance: prototype, strand ID, place along it, orientation.")
        .def(py::init<>())
        .def_readwrite("prototype", &InstanceSet::Instance::prototype)
        .def_readwrite("strand", &InstanceSet::Instance::strand)
        .def_readwrite("id", &InstanceSet::Instance::id)
        .def_readwrite("t", &InstanceSet::Instance::t)
        .def_readwrite("orient", &InstanceSet::Instance::orient)
        .def_readwrite("direction", &InstanceSet::Instance::direction)
        .def_readwrite("spin", &InstanceSet::Instance::spin)
        .def_readwrite("scale", &InstanceSet::Instance::scale)
        .def_readwrite("offset", &InstanceSet::Instance::offset)
        .def_readwrite("scale_by_width", &InstanceSet::Instance::scaleByWidth)
        .def_readwrite("pin_to_rest", &InstanceSet::Instance::pinToRest)
        .def_readwrite("rest_distance", &InstanceSet::Instance::restDistance,
                       "Distance from the root on the rest pose (pinned instances; below 0: not known, at t)")
        .def_readwrite("fit_strand", &InstanceSet::Instance::fitStrand,
                       "Sized to its strand: scale is a fraction of the strand's rest length along +Y")
        .def_readwrite("aspect", &InstanceSet::Instance::aspect, "Fitted: size across (X, Z) over the size along +Y")
        .def_readwrite("rest_length", &InstanceSet::Instance::restLength,
                       "The strand's rest length (fitted instances; below 0: not known, its length now)")
        .def_readwrite("on_skin", &InstanceSet::Instance::onSkin, "A scale (a Scales node's): at the root, on the skin")
        .def_readwrite("variant", &InstanceSet::Instance::variant, "Which of its node's prototypes")
        .def_readwrite("material", &InstanceSet::Instance::material, "Index into materials (-1: the prototype's own)")
        .def_readwrite("conform", &InstanceSet::Instance::conform)
        .def_readwrite("lift", &InstanceSet::Instance::lift)
        .def_readwrite("tile", &InstanceSet::Instance::tile)
        .def_readwrite("stack", &InstanceSet::Instance::stack)
        .def_readwrite("deform", &InstanceSet::Instance::deform,
                       "Bent along its strand (the node's deform alongStrand): a mesh of its own, not a matrix")
        .def_readwrite("deform_axis", &InstanceSet::Instance::deformAxis, "Bent along its strand: the axis along it (0 x, 1 y, 2 z)")
        .def_property_readonly("meshed", &InstanceSet::Instance::meshed, "A scale or bent along its strand: a mesh of its own");
    py::class_<InstanceSet::Placed>(instanceSet, "Placed", "An instance placed on the final strands: origin and axes.")
        .def_readonly("prototype", &InstanceSet::Placed::prototype)
        .def_readonly("id", &InstanceSet::Placed::id)
        .def_readonly("index", &InstanceSet::Placed::index)
        .def_readonly("x", &InstanceSet::Placed::x)
        .def_readonly("y", &InstanceSet::Placed::y)
        .def_readonly("z", &InstanceSet::Placed::z)
        .def_readonly("origin", &InstanceSet::Placed::origin)
        .def_readonly("on_skin", &InstanceSet::Placed::onSkin)
        .def_readonly("meshed", &InstanceSet::Placed::meshed)
        .def_readonly("distance", &InstanceSet::Placed::distance)
        .def("matrix",
             [](const InstanceSet::Placed& p) {
                 float out[16];
                 p.matrix(out);
                 return std::vector<float>(out, out + 16);
             },
             "Row-major 4x4 matrix (rows x, y, z, origin) as 16 floats.");
    instanceSet.def(py::init<>())
        .def_readwrite("prototypes", &InstanceSet::prototypes)
        .def_readwrite("materials", &InstanceSet::materials)
        .def_readwrite("instances", &InstanceSet::instances)
        .def("__len__", &InstanceSet::size)
        .def_property_readonly("empty", &InstanceSet::empty)
        .def("prototype_index", &InstanceSet::prototypeIndex, py::arg("name"), "Index of a prototype, added when new.")
        .def("append", &InstanceSet::append, py::arg("other"))
        .def("content_hash", &InstanceSet::contentHash)
        .def("resolve", &InstanceSet::resolve, py::arg("strands"), py::arg("scalp") = nullptr,
             "Instances whose strand is in `strands`, placed on them (scales on `scalp`, the mesh they are rooted on).");

    // ---- StrandSet -------------------------------------------------------
    py::class_<StrandSet, std::shared_ptr<StrandSet>>(
        m, "StrandSet",
        "Structure-of-arrays set of strands: strand i owns points [offsets[i], offsets[i+1]). Copies are cheap "
        "(copy-on-write): reading never copies, and the mutable_* accessors clone a shared array on first use. "
        "The *_view() methods return read-only buffer views; mutable_*() return writable ones.")
        .def(py::init<>())
        .def(py::init<const StrandSet&>(), py::arg("other"), "A copy sharing every array until written.")
        .def("copy", [](const StrandSet& s) { return StrandSet(s); }, "A copy-on-write copy.")
        .def("__copy__", [](const StrandSet& s) { return StrandSet(s); })
        .def("__deepcopy__", [](const StrandSet& s, py::dict) { return StrandSet(s); }, py::arg("memo"))
        .def_property_readonly("strand_count", &StrandSet::strandCount)
        .def_property_readonly("point_count", &StrandSet::pointCount)
        .def_property_readonly("empty", &StrandSet::empty)
        .def("reserve", &StrandSet::reserve, py::arg("strands"), py::arg("points"))
        .def("clear", &StrandSet::clear)
        .def("resize_uniform", [](StrandSet& s, std::size_t strands, std::size_t points) { s.resizeUniform(strands, points); },
             py::arg("strands"), py::arg("points_per_strand"),
             "Replaces the contents with strands of equal point counts (IDs 0, positions zero, widths 1).")
        .def("resize", [](StrandSet& s, const std::vector<std::size_t>& points) { s.resize(points); }, py::arg("points_per_strand"),
             "Replaces the contents with strands of the given point counts.")
        .def("uniform_point_count", &StrandSet::uniformPointCount, "Points shared by every strand; 0 if they differ.")
        .def("max_point_count", &StrandSet::maxPointCount)
        .def("add_strand",
             [](StrandSet& s, StrandId id, py::handle points, py::handle widths, const StrandRoot& root) {
                 std::vector<float> w;
                 if (!widths.is_none()) w = toFloats(widths);
                 return s.addStrand(id, toPoints(points), w, root);
             },
             py::arg("id"), py::arg("points"), py::arg("widths") = py::none(), py::arg("root") = StrandRoot{},
             "Appends a strand and returns its index. widths defaults to 1.0 per point.")
        .def("strand_point_count", [](const StrandSet& s, std::size_t i) { checkStrand(s, i); return s.strandPointCount(i); }, py::arg("strand"))
        .def("strand_begin", [](const StrandSet& s, std::size_t i) { checkStrand(s, i); return s.strandBegin(i); }, py::arg("strand"),
             "Index of the strand's first point in the point arrays.")
        .def("strand_points", &strandPointList, py::arg("strand"), "The points of one strand as a list of Vec3.")
        .def("strand_points_view",
             [](py::object self, std::size_t i) {
                 const StrandSet& s = strandsOf(self);
                 checkStrand(s, i);
                 return makeView(self, reinterpret_cast<const float*>(s.strandPoints(i)),
                                 {static_cast<py::ssize_t>(s.strandPointCount(i)), 3}, true);
             },
             py::arg("strand"), "Read-only (n, 3) float view of one strand's points.")
        .def("set_strand_points",
             [](StrandSet& s, std::size_t i, py::handle points) {
                 checkStrand(s, i);
                 const std::vector<Vec3f> pts = toPoints(points);
                 if (pts.size() != s.strandPointCount(i)) throw py::value_error("point count must match the strand's");
                 Vec3f* dst = s.strandPoints(i);
                 for (std::size_t k = 0; k < pts.size(); ++k) dst[k] = pts[k];
             },
             py::arg("strand"), py::arg("points"), "Replaces one strand's points (same count).")
        .def("strand_widths",
             [](const StrandSet& s, std::size_t i) {
                 checkStrand(s, i);
                 const auto& w = s.widths();
                 return std::vector<float>(w.begin() + static_cast<std::ptrdiff_t>(s.strandBegin(i)),
                                           w.begin() + static_cast<std::ptrdiff_t>(s.strandBegin(i) + s.strandPointCount(i)));
             },
             py::arg("strand"))
        // Read-only lists.
        .def_property_readonly("ids", [](const StrandSet& s) { return s.ids(); }, "Strand IDs as a list.")
        .def_property_readonly("roots", [](const StrandSet& s) { return s.roots(); }, "Strand roots as a list.")
        .def_property_readonly("offsets", [](const StrandSet& s) { return s.offsets(); }, "strand_count + 1 point offsets.")
        .def_property_readonly("positions", [](const StrandSet& s) { return s.positions(); }, "Every point as a list of Vec3.")
        .def_property_readonly("widths", [](const StrandSet& s) { return s.widths(); }, "Per-point widths as a list.")
        // Read-only buffer views (no copy).
        .def("ids_view", [](py::object self) { return flatView(self, strandsOf(self).ids(), true); }, "Read-only uint64 view (strand_count,).")
        .def("roots_view", [](py::object self) { return makeView(self, strandsOf(self).roots().data(), {static_cast<py::ssize_t>(strandsOf(self).strandCount())}, true, kRootFormat); },
             "Read-only struct view of the roots (int32 face, float u, float v); indexable, or numpy-readable.")
        .def("offsets_view", [](py::object self) { return flatView(self, strandsOf(self).offsets(), true); }, "Read-only size_t view (strand_count + 1,).")
        .def("positions_view", [](py::object self) { return pointsView(self, strandsOf(self).positions(), true); }, "Read-only float32 view (point_count, 3).")
        .def("widths_view", [](py::object self) { return flatView(self, strandsOf(self).widths(), true); }, "Read-only float32 view (point_count,).")
        // Writable views: clone the array first when a copy shares it.
        .def("mutable_ids", [](py::object self) { return flatView(self, strandsOf(self).mutableIds(), false); }, "Writable uint64 view of the IDs.")
        .def("mutable_roots", [](py::object self) { auto& r = strandsOf(self).mutableRoots(); return makeView(self, r.data(), {static_cast<py::ssize_t>(r.size())}, false, kRootFormat); },
             "Writable struct view of the roots.")
        .def("mutable_positions", [](py::object self) { return pointsView(self, strandsOf(self).mutablePositions(), false); }, "Writable float32 view (point_count, 3).")
        .def("mutable_widths", [](py::object self) { return flatView(self, strandsOf(self).mutableWidths(), false); }, "Writable float32 view (point_count,).")
        // Bulk setters.
        .def("set_positions",
             [](StrandSet& s, py::handle points) {
                 const std::vector<Vec3f> pts = toPoints(points);
                 if (pts.size() != s.pointCount()) throw py::value_error("expected " + std::to_string(s.pointCount()) + " points");
                 s.mutablePositions() = pts;
             },
             py::arg("points"), "Replaces every point position (same count).")
        .def("set_widths", [](StrandSet& s, py::handle w) { assignFloats(s.mutableWidths(), w, "widths"); }, py::arg("widths"))
        .def("set_ids",
             [](StrandSet& s, const std::vector<StrandId>& ids) {
                 if (ids.size() != s.strandCount()) throw py::value_error("expected one ID per strand");
                 s.mutableIds() = ids;
             },
             py::arg("ids"))
        .def("set_roots",
             [](StrandSet& s, const std::vector<StrandRoot>& roots) {
                 if (roots.size() != s.strandCount()) throw py::value_error("expected one root per strand");
                 s.mutableRoots() = roots;
             },
             py::arg("roots"))
        .def("set_root", [](StrandSet& s, std::size_t i, const StrandRoot& r) { checkStrand(s, i); s.mutableRoots()[i] = r; },
             py::arg("strand"), py::arg("root"))
        // Channels.
        .def("add_strand_channel", [](StrandSet& s, const std::string& n, float d) { s.addStrandChannel(n, d); }, py::arg("name"), py::arg("default") = 0.0f,
             "Adds (or resets) a per-strand float channel filled with `default`.")
        .def("add_point_channel", [](StrandSet& s, const std::string& n, float d) { s.addPointChannel(n, d); }, py::arg("name"), py::arg("default") = 0.0f,
             "Adds (or resets) a per-point float channel filled with `default`.")
        .def("strand_channel", [](py::object self, const std::string& n) { return channelView(self, strandsOf(self).strandChannel(n), true); }, py::arg("name"),
             "Read-only view of a per-strand channel, or None.")
        .def("point_channel", [](py::object self, const std::string& n) { return channelView(self, strandsOf(self).pointChannel(n), true); }, py::arg("name"),
             "Read-only view of a per-point channel, or None.")
        .def("mutable_strand_channel", [](py::object self, const std::string& n) { return channelView(self, strandsOf(self).mutableStrandChannel(n), false); }, py::arg("name"))
        .def("mutable_point_channel", [](py::object self, const std::string& n) { return channelView(self, strandsOf(self).mutablePointChannel(n), false); }, py::arg("name"))
        .def("set_strand_channel",
             [](StrandSet& s, const std::string& n, py::handle values) {
                 std::vector<float>* c = s.mutableStrandChannel(n);
                 if (!c) c = &s.addStrandChannel(n);
                 assignFloats(*c, values, "strand channel");
             },
             py::arg("name"), py::arg("values"), "Sets a per-strand channel (added when missing) from a sequence or buffer.")
        .def("set_point_channel",
             [](StrandSet& s, const std::string& n, py::handle values) {
                 std::vector<float>* c = s.mutablePointChannel(n);
                 if (!c) c = &s.addPointChannel(n);
                 assignFloats(*c, values, "point channel");
             },
             py::arg("name"), py::arg("values"))
        .def("remove_strand_channel", &StrandSet::removeStrandChannel, py::arg("name"))
        .def("remove_point_channel", &StrandSet::removePointChannel, py::arg("name"))
        .def("strand_channel_names", &StrandSet::strandChannelNames)
        .def("point_channel_names", &StrandSet::pointChannelNames)
        // Instances and card layers.
        .def_property(
            "instances",
            [](const StrandSet& s) -> std::shared_ptr<InstanceSet> {
                return s.instances() ? std::const_pointer_cast<InstanceSet>(s.instances()) : nullptr;
            },
            [](StrandSet& s, std::shared_ptr<InstanceSet> inst) { s.setInstances(std::move(inst)); },
            "Geometry instanced on the strands, or None.")
        .def("card_layers",
             [](const StrandSet& s) {
                 std::vector<std::shared_ptr<CardMesh>> out;
                 for (const auto& l : s.cardLayers()) out.push_back(std::const_pointer_cast<CardMesh>(l));
                 return out;
             },
             "Card meshes built by Cards nodes upstream (with EvalContext.build_cards).")
        .def("add_card_layer", [](StrandSet& s, std::shared_ptr<CardMesh> layer) { s.addCardLayer(std::move(layer)); }, py::arg("layer"))
        // Whole-set operations.
        .def("append", &StrandSet::append, py::arg("other"), "Appends every strand of another set.")
        .def("with_density", &StrandSet::withDensity, py::arg("density"), "The strands kept at a preview density (stable by ID).")
        .def("subset", &StrandSet::subset, py::arg("strands"), "The given strand indices, in order, with their channels.")
        .def("content_hash", &StrandSet::contentHash)
        .def("validate",
             [](const StrandSet& s) {
                 std::string error;
                 if (!s.validate(&error)) throw py::value_error(error);
             },
             "Raises ValueError when array sizes and offsets are inconsistent.")
        .def("is_valid", [](const StrandSet& s) { return s.validate(nullptr); })
        .def("__len__", &StrandSet::strandCount)
        .def("__repr__", [](const StrandSet& s) {
            return "<StrandSet strands=" + std::to_string(s.strandCount()) + " points=" + std::to_string(s.pointCount()) + ">";
        });

    // ---- ScalpMesh -------------------------------------------------------
    py::class_<SurfacePoint>(m, "SurfacePoint", "A point on the scalp: root (face, u, v), position and smooth normal.")
        .def(py::init<>())
        .def_readwrite("root", &SurfacePoint::root)
        .def_readwrite("position", &SurfacePoint::position)
        .def_readwrite("normal", &SurfacePoint::normal)
        .def_property_readonly("valid", [](const SurfacePoint& p) { return p.root.face >= 0; })
        .def("__repr__", [](const SurfacePoint& p) {
            std::ostringstream s;
            s << "SurfacePoint(face=" << p.root.face << ", position=(" << p.position.x << ", " << p.position.y << ", " << p.position.z << "))";
            return s.str();
        });

    py::class_<ScalpMesh, std::shared_ptr<ScalpMesh>>(
        m, "ScalpMesh",
        "Triangulated scalp surface that strands and guides are rooted on. Points and triangles come from "
        "sequences or buffers ((n, 3) floats; flat or (m, 3) ints). Do not mutate a mesh after handing it to a "
        "Groom or ExternalInputs: its hash was taken then.")
        .def(py::init<>())
        .def(py::init([](py::handle points, py::handle triangles) {
                 return ScalpMesh(toPoints(points), toTriangles(triangles));
             }),
             py::arg("points"), py::arg("triangles"))
        .def_property_readonly("point_count", &ScalpMesh::pointCount)
        .def_property_readonly("triangle_count", &ScalpMesh::triangleCount)
        .def_property_readonly("points", [](const ScalpMesh& s) { return s.points(); }, "Vertex positions as a list of Vec3.")
        .def_property_readonly("triangles", [](const ScalpMesh& s) {
                std::vector<std::array<std::int32_t, 3>> out(s.triangles().begin(), s.triangles().end());
                return out;
            }, "Triangles as a list of (a, b, c) index tuples.")
        .def("points_view", [](py::object self) { return pointsView(self, self.cast<const ScalpMesh&>().points(), true); }, "Read-only float32 view (point_count, 3).")
        .def("triangles_view", [](py::object self) {
                const auto& t = self.cast<const ScalpMesh&>().triangles();
                return makeView(self, reinterpret_cast<const std::int32_t*>(t.data()), {static_cast<py::ssize_t>(t.size()), 3}, true);
            }, "Read-only int32 view (triangle_count, 3).")
        .def_property_readonly("total_area", &ScalpMesh::totalArea)
        .def("contains", &ScalpMesh::contains, py::arg("root"), "Whether a root lies on a valid triangle of this mesh.")
        .def("position", &ScalpMesh::position, py::arg("root"), "World position of a root.")
        .def("normal", &ScalpMesh::normal, py::arg("root"), "Smooth normal at a root.")
        .def("closest_point", &ScalpMesh::closestPoint, py::arg("point"),
             py::arg("max_distance") = std::numeric_limits<float>::infinity(),
             "Closest surface point (face -1 when nothing is within max_distance).")
        .def("closest_point_brute_force", &ScalpMesh::closestPointBruteForce, py::arg("point"))
        .def("intersect_ray",
             [](const ScalpMesh& s, const Vec3f& origin, const Vec3f& direction) -> py::object {
                 SurfacePoint hit;
                 float t = 0.0f;
                 if (!s.intersectRay(origin, direction, &hit, &t)) return py::none();
                 return py::make_tuple(hit, t);
             },
             py::arg("origin"), py::arg("direction"),
             "Nearest hit of the ray with the surface as (SurfacePoint, t), or None on a miss.")
        .def("content_hash", &ScalpMesh::contentHash)
        .def("topology_hash", &ScalpMesh::topologyHash, "Hash of the triangle list alone.")
        .def("topology_fingerprint", &ScalpMesh::topologyFingerprint,
             "What a groom file keeps of the scalp it was made on (scalp.topology): the point count and triangles.")
        .def("set_corner_uvs", [](ScalpMesh& s, py::handle uvs) { s.setCornerUVs(toFloatTuples<2>(uvs)); }, py::arg("uvs"),
             "Per-corner UVs: 3 per triangle in triangle order, as (u, v) pairs or a flat/(n, 2) float buffer.")
        .def_property_readonly("has_uvs", &ScalpMesh::hasUVs)
        .def_property_readonly("corner_uvs", [](const ScalpMesh& s) { return s.cornerUVs(); })
        .def("corner_uvs_view", [](py::object self) {
                const auto& uv = self.cast<const ScalpMesh&>().cornerUVs();
                return makeView(self, reinterpret_cast<const float*>(uv.data()), {static_cast<py::ssize_t>(uv.size()), 2}, true);
            })
        .def("uv_at", &ScalpMesh::uvAt, py::arg("root"), "UV at a root ((0, 0) without UVs).")
        .def("set_corner_colors", [](ScalpMesh& s, const std::string& name, py::handle colors) { s.setCornerColors(name, toFloatTuples<4>(colors)); },
             py::arg("name"), py::arg("colors"), "A named color set: 3 RGBA corners per triangle.")
        .def("has_color_set", &ScalpMesh::hasColorSet, py::arg("name"))
        .def("color_at", &ScalpMesh::colorAt, py::arg("name"), py::arg("root"), "RGBA of a color set at a root (zeros if absent).")
        .def("color_set_names", &ScalpMesh::colorSetNames)
        .def("corner_colors",
             [](const ScalpMesh& s, const std::string& name) -> py::object {
                 const auto* c = s.cornerColors(name);
                 if (!c) return py::none();
                 return py::cast(*c);
             },
             py::arg("name"))
        .def("set_rest_points", [](ScalpMesh& s, py::handle pts) { s.setRestPoints(pts.is_none() ? std::vector<Vec3f>{} : toPoints(pts)); }, py::arg("points"),
             "The same vertices in an undeformed pose (None or empty clears).")
        .def_property_readonly("has_rest", &ScalpMesh::hasRest)
        .def_property_readonly("rest_points", [](const ScalpMesh& s) { return s.restPoints(); })
        .def("rest_triangle_area", &ScalpMesh::restTriangleArea, py::arg("triangle"))
        .def("rest_total_area", &ScalpMesh::restTotalArea)
        .def("rest_position_or", &ScalpMesh::restPositionOr, py::arg("root"), py::arg("fallback"))
        .def("with_points", [](const ScalpMesh& s, py::handle pts) { return s.withPoints(toPoints(pts)); }, py::arg("points"),
             "The same topology, UVs, color sets and rest pose with new positions.")
        .def("set_polygons", [](ScalpMesh& s, std::vector<std::int32_t> counts) { s.setPolygons(std::move(counts)); },
             py::arg("triangles_per_polygon"),
             "The host's polygons the triangles came from: polygon i made the next counts[i] triangles (what "
             "Catmull-Clark refines; empty: every triangle a face).")
        .def_property_readonly("polygons", &ScalpMesh::polygons)
        .def("set_render_subdivision", &ScalpMesh::setRenderSubdivision, py::arg("levels"), py::arg("source") = std::string(),
             "The Catmull-Clark levels a renderer refines this mesh by: what match render takes.")
        .def_property_readonly("render_subdivision", &ScalpMesh::renderSubdivision)
        .def_property_readonly("render_subdivision_from", &ScalpMesh::renderSubdivisionFrom)
        .def_property_readonly("subdivision_level", &ScalpMesh::subdivisionLevel, "Levels this mesh was refined by (0: a cage).")
        .def("__repr__", [](const ScalpMesh& s) {
            return "<ScalpMesh points=" + std::to_string(s.pointCount()) + " triangles=" + std::to_string(s.triangleCount()) + ">";
        });
    m.def("subdivide_scalp", [](const std::shared_ptr<ScalpMesh>& cage, int levels) {
              const MeshPtr out = subdivideScalp(cage, levels);
              return std::const_pointer_cast<ScalpMesh>(out);
          },
          py::arg("cage"), py::arg("levels"),
          "The mesh refined by `levels` (0 to 3) of Catmull-Clark (subdivision.h): the cage itself for 0.");

    // ---- GuideSet --------------------------------------------------------
    py::class_<Guide>(m, "Guide", "A guide curve: ID, root, points (root first) and per-guide attributes.")
        .def(py::init<>())
        .def(py::init([](std::uint64_t id, py::handle points, const std::map<std::string, float>& attributes) {
                 Guide g;
                 g.id = id;
                 g.points = toPoints(points);
                 g.attributes = attributes;
                 return g;
             }),
             py::arg("id"), py::arg("points"), py::arg("attributes") = std::map<std::string, float>{})
        .def_readwrite("id", &Guide::id)
        .def_readwrite("root", &Guide::root)
        .def_property("points", [](const Guide& g) { return g.points; }, [](Guide& g, py::handle p) { g.points = toPoints(p); })
        .def_readwrite("attributes", &Guide::attributes)
        .def_readwrite("point_attributes", &Guide::pointAttributes,
                       "Per-point layers: {name: one value per point}; Interpolate writes them as point channels.")
        .def("__repr__", [](const Guide& g) { return "<Guide id=" + std::to_string(g.id) + " points=" + std::to_string(g.points.size()) + ">"; });

    py::class_<GuideSet, std::shared_ptr<GuideSet>>(m, "GuideSet",
                                                    "A flat set of guide curves (the Guides input of Interpolate and Clump).")
        .def(py::init<>())
        .def(py::init<const GuideSet&>(), py::arg("other"))
        .def("__len__", &GuideSet::size)
        .def_property_readonly("size", &GuideSet::size)
        .def_property_readonly("empty", &GuideSet::empty)
        .def("add",
             [](GuideSet& s, std::uint64_t id, py::handle points, const std::map<std::string, float>& attributes) {
                 Guide& g = s.add(id, toPoints(points));
                 g.attributes = attributes;
                 return s.size() - 1;
             },
             py::arg("id"), py::arg("points"), py::arg("attributes") = std::map<std::string, float>{},
             "Appends a guide and returns its index.")
        .def_property_readonly("guides", [](const GuideSet& s) { return s.guides(); }, "Every guide, as copies.")
        .def("guide",
             [](const GuideSet& s, std::size_t i) {
                 if (i >= s.size()) throw py::index_error("guide index out of range");
                 return s.guides()[i];
             },
             py::arg("index"), "A copy of guide `index`.")
        .def("set_guide",
             [](GuideSet& s, std::size_t i, const Guide& g) {
                 if (i >= s.size()) throw py::index_error("guide index out of range");
                 s.guides()[i] = g;
             },
             py::arg("index"), py::arg("guide"))
        .def("set_points",
             [](GuideSet& s, std::size_t i, py::handle points) {
                 if (i >= s.size()) throw py::index_error("guide index out of range");
                 s.guides()[i].points = toPoints(points);
             },
             py::arg("index"), py::arg("points"))
        .def("set_attribute",
             [](GuideSet& s, std::size_t i, const std::string& name, float value) {
                 if (i >= s.size()) throw py::index_error("guide index out of range");
                 s.guides()[i].attributes[name] = value;
             },
             py::arg("index"), py::arg("name"), py::arg("value"))
        .def("root_on_mesh", &GuideSet::rootOnMesh, py::arg("mesh"), "Roots every guide on its closest scalp point.")
        .def("orient_and_root", &GuideSet::orientAndRoot, py::arg("mesh"), "Flips tip-first guides, then roots them.")
        .def("content_hash", &GuideSet::contentHash)
        .def("__repr__", [](const GuideSet& s) { return "<GuideSet guides=" + std::to_string(s.size()) + ">"; });

    // ---- Texture ---------------------------------------------------------
    py::class_<Texture, std::shared_ptr<Texture>>(m, "Texture", "A float RGBA image (or a UDIM tile set) sampled with UVs.")
        .def(py::init([](int w, int h, py::handle rgba) { return std::make_shared<Texture>(w, h, toFloats(rgba)); }),
             py::arg("width"), py::arg("height"), py::arg("rgba"), "An image from w * h * 4 floats, rows top-down.")
        .def(py::init([](const std::string& udimPattern) { return std::make_shared<Texture>(udimPattern); }),
             py::arg("udim_pattern"), "A UDIM tile set read on demand; the pattern holds '<UDIM>'.")
        .def_property_readonly("width", &Texture::width)
        .def_property_readonly("height", &Texture::height)
        .def_property_readonly("is_udim", &Texture::isUdim)
        .def("sample", &Texture::sample, py::arg("u"), py::arg("v"), "Bilinear RGBA sample with repeat wrapping.")
        .def("bytes", &Texture::bytes, "Memory held now.")
        .def("error", &Texture::error, "Why the last tile failed to read (empty when none did).");
    m.def("load_texture",
          [](const std::string& path) { return std::const_pointer_cast<Texture>(loadTexture(path)); },
          py::arg("path"), "Loads an image through the process-wide cache. Raises TextureError.");
    m.def("file_stamp", &fileStamp, py::arg("path"), "Hash of a file's path, size and modification time (0 if missing).");

    // ---- Scales ----------------------------------------------------------
    py::class_<ScaleMesh::Scale>(m, "Scale", "One scale of a ScaleMesh: its frame and its shader data.")
        .def_readonly("id", &ScaleMesh::Scale::id)
        .def_readonly("strand", &ScaleMesh::Scale::strand)
        .def_readonly("prototype", &ScaleMesh::Scale::prototype)
        .def_readonly("variant", &ScaleMesh::Scale::variant)
        .def_readonly("material", &ScaleMesh::Scale::material)
        .def_readonly("first_vertex", &ScaleMesh::Scale::firstVertex)
        .def_readonly("vertex_count", &ScaleMesh::Scale::vertexCount)
        .def_readonly("random", &ScaleMesh::Scale::random)
        .def_readonly("root_uv", &ScaleMesh::Scale::rootUv)
        .def_readonly("color", &ScaleMesh::Scale::color)
        .def_readonly("quad", &ScaleMesh::Scale::quad)
        .def("matrix",
             [](const ScaleMesh::Scale& s) {
                 float out[16];
                 s.matrix(out);
                 return std::vector<float>(out, out + 16);
             },
             "Row-major 4x4 matrix (rows x, y, z, origin): tilted and raised, before the bend.");
    py::class_<ScaleMesh, std::shared_ptr<ScaleMesh>>(m, "ScaleMesh", "The scales' geometry (scales.h): bent onto the skin.")
        .def_readonly("points", &ScaleMesh::points)
        .def_readonly("normals", &ScaleMesh::normals)
        .def_readonly("colors", &ScaleMesh::colors)
        .def_readonly("uvs", &ScaleMesh::uvs)
        .def_readonly("triangles", &ScaleMesh::triangles)
        .def_readonly("scales", &ScaleMesh::scales)
        .def_property_readonly("vertex_count", &ScaleMesh::vertexCount)
        .def_property_readonly("triangle_count", &ScaleMesh::triangleCount);
    m.def("build_scale_mesh",
          [](const StrandSet& strands, std::shared_ptr<ScalpMesh> scalp) {
              return std::const_pointer_cast<ScaleMesh>(buildScaleMesh(strands, scalp));
          },
          py::arg("strands"), py::arg("scalp") = nullptr,
          "The scales among the strands' instances as one mesh (built-in prototypes only; None without scales).");
    m.def("builtin_prototypes", &builtinPrototypeNames, "The built-in scale plates' names.");
    m.def(
        "builtin_prototype",
        [](const std::string& name) -> py::object {
            const InstancePrototype* p = builtinPrototype(name);
            if (!p) return py::none();
            py::list points, normals, uvs, quads;
            for (const Vec3f& v : p->points) points.append(py::make_tuple(v.x, v.y, v.z));
            for (const Vec3f& v : p->normals) normals.append(py::make_tuple(v.x, v.y, v.z));
            for (const auto& uv : p->uvs) uvs.append(py::make_tuple(uv[0], uv[1]));
            // Its triangles two by two (a, b, c) (a, c, d): the quads (a, b, c, d).
            for (std::size_t t = 0; t + 5 < p->triangles.size(); t += 6)
                quads.append(py::make_tuple(p->triangles[t], p->triangles[t + 1], p->triangles[t + 2], p->triangles[t + 5]));
            py::dict out;
            out["points"] = points;
            out["normals"] = normals;
            out["uvs"] = uvs;
            out["triangles"] = std::vector<std::uint32_t>(p->triangles);
            out["quads"] = quads;
            return out;
        },
        py::arg("name"),
        "A built-in scale plate (groomlab:scale, groomlab:scaleRound, groomlab:scaleShield) as {points, normals, uvs, "
        "triangles, quads}: one closed piece of quads (two triangles each), None for any other name.");
    m.def("graph_mesh_prototypes", &graphMeshPrototypes, py::arg("graph_json"),
          "The scene meshes a graph's Instance nodes name (not the built-in plates): what a groom file stores.");
    m.def("missing_instance_prototypes",
          [](const std::string& prototype, py::object has) {
              return missingInstancePrototypes(prototype, [&](const std::string& name) {
                  return !has.is_none() && has(name).cast<bool>();
              });
          },
          py::arg("prototype"), py::arg("has") = py::none(),
          "The meshes an Instance's prototype param names that are not built in and that has(name) says the host "
          "lacks: what its note names.");
    m.def("bake_scale_maps",
          [](const ScalpMesh& skin, const ScaleMesh& scales, const std::string& prefix, int resolution, float maxHeight,
             int padding) {
              ScaleMapOptions options;
              options.resolution = resolution;
              options.maxHeight = maxHeight;
              options.padding = padding;
              const ScaleMaps maps = bakeScaleMaps(skin, scales, options);
              py::dict out;
              out["files"] = writeScaleMaps(maps, prefix);
              out["max_height"] = maps.maxHeight;
              out["udim"] = maps.udim;
              out["coverage"] = maps.texels ? static_cast<double>(maps.covered) / static_cast<double>(maps.texels) : 0.0;
              return out;
          },
          py::arg("skin"), py::arg("scales"), py::arg("prefix"), py::arg("resolution") = 2048, py::arg("max_height") = 0.0f,
          py::arg("padding") = 8,
          "Bakes the scales onto the skin's uvs (scale_maps.h) and writes <prefix>ScaleNormal/ScaleHeight/ScaleColor"
          "[.<tile>].png; returns {files, max_height, udim, coverage}.");
}

}  // namespace groomlab::py_bind
