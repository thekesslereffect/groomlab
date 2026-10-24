// SPDX-License-Identifier: Apache-2.0
// Whole grooms and their files, parts, flow fields, render-time LOD, game
// cards and the USD writer.

#include "bind_common.h"

#include <set>
#include <sstream>

#include "groomlab/card_files.h"
#include "groomlab/card_maps.h"
#include "groomlab/card_presets.h"
#include "groomlab/cards.h"
#include "groomlab/flow.h"
#include "groomlab/frame_time.h"
#include "groomlab/groom.h"
#include "groomlab/groom_file.h"
#include "groomlab/lod.h"
#include "groomlab/parts.h"
#include "groomlab/paths.h"
#include "groomlab/png.h"
#include "groomlab/ptex.h"
#include "groomlab/scalp_maps.h"
#include "groomlab/usd_writer.h"
#include "groomlab/vrscene_writer.h"

namespace groomlab::py_bind {

namespace {

template <class T>
std::shared_ptr<T> mutableShared(const std::shared_ptr<const T>& p) {
    return std::const_pointer_cast<T>(p);
}

// CardBakeSettings with the scalp kept alive.
struct PyCardBakeSettings : CardBakeSettings {
    std::shared_ptr<ScalpMesh> scalpRef;
    void setScalp(std::shared_ptr<ScalpMesh> s) {
        scalpRef = std::move(s);
        scalp = scalpRef.get();
    }
};

ArrayView bytesView(py::object owner, const std::vector<std::uint8_t>& v, int size, int channels) {
    if (channels == 1) return makeView(std::move(owner), v.data(), {size, size}, true);
    return makeView(std::move(owner), v.data(), {size, size, channels}, true);
}

}  // namespace

void bindGroom(py::module_& m) {
    // ---- Parts ---------------------------------------------------------------
    py::class_<Part>(m, "Part", "A parting line on the scalp: a name and roots along the line.")
        .def(py::init<>())
        .def_readwrite("name", &Part::name)
        .def_readwrite("points", &Part::points)
        .def("__repr__", [](const Part& p) { return "<Part '" + p.name + "' points=" + std::to_string(p.points.size()) + ">"; });

    py::class_<PartSet>(m, "PartSet", "Parting lines bound to the scalp; hair does not mix across them.")
        .def(py::init<>())
        .def_readwrite("parts", &PartSet::parts)
        .def_property_readonly("empty", &PartSet::empty)
        .def("__len__", [](const PartSet& s) { return s.parts.size(); })
        .def_static("from_points", [](const std::string& name, py::handle pts, const ScalpMesh& scalp) { return PartSet::fromPoints(name, toPoints(pts), scalp); },
                    py::arg("name"), py::arg("points"), py::arg("scalp"), "A part through points near the scalp.")
        .def("add", [](PartSet& s, const std::string& base, py::handle pts, const ScalpMesh& scalp, std::size_t samples) { return s.add(base, toPoints(pts), scalp, samples); },
             py::arg("name"), py::arg("points"), py::arg("scalp"), py::arg("samples") = 64,
             "Adds a part along a stroke (resampled, bound to the scalp); returns its (unique) name.")
        .def("transferred", py::overload_cast<const ScalpMesh&, const ScalpMesh&, const ScalpFit&>(&PartSet::transferred, py::const_),
             py::arg("from_mesh"), py::arg("to_mesh"), py::arg("fit") = ScalpFit{}, "The same parts bound to another scalp.")
        .def("transferred", py::overload_cast<const ScalpMapping&>(&PartSet::transferred, py::const_), py::arg("mapping"),
             "The same parts bound to another scalp by a ScalpMapping.")
        .def("find",
             [](const PartSet& s, const std::string& name) -> py::object {
                 const Part* p = s.find(name);
                 return p ? py::cast(*p) : py::none();
             },
             py::arg("name"))
        .def("remove",
             [](PartSet& s, const std::string& name) {
                 for (auto it = s.parts.begin(); it != s.parts.end(); ++it) {
                     if (it->name == name) {
                         s.parts.erase(it);
                         return true;
                     }
                 }
                 return false;
             },
             py::arg("name"))
        .def("content_hash", &PartSet::contentHash)
        .def("to_json", &PartSet::toJson)
        .def_static("from_json", &PartSet::fromJson, py::arg("text"));

    py::class_<PartLines, std::shared_ptr<PartLines>> partLines(m, "PartLines", "Parts resolved on a scalp, for side tests and drawing.");
    py::class_<PartLines::Side>(partLines, "Side", "Which side of which part a point is on (side 0: not beside one).")
        .def_readonly("side", &PartLines::Side::side)
        .def_readonly("part", &PartLines::Side::part)
        .def_readonly("segment", &PartLines::Side::segment)
        .def_readonly("distance", &PartLines::Side::distance)
        .def("__repr__", [](const PartLines::Side& s) {
            return "<Side side=" + std::to_string(s.side) + " part=" + std::to_string(s.part) + " distance=" + std::to_string(s.distance) + ">";
        });
    partLines.def(py::init<const PartSet&, const ScalpMesh&>(), py::arg("parts"), py::arg("scalp"))
        .def("side_of", &PartLines::sideOf, py::arg("rest_point"), py::arg("only") = std::string{}, "Side of a rest-pose point.")
        .def_static("separated", &PartLines::separated, py::arg("a"), py::arg("b"), "Whether two sides are across the same part.")
        .def("away_from", &PartLines::awayFrom, py::arg("side"), py::arg("current_normal"))
        .def_property_readonly("empty", &PartLines::empty)
        .def_property_readonly("lines", [](const PartLines& p) { return p.lines(); }, "Each part's points in the current pose.")
        .def_property_readonly("names", [](const PartLines& p) { return p.names(); })
        .def("content_hash", &PartLines::contentHash);
    m.def("auto_parts", &findAutoParts, py::arg("strands"), py::arg("scalp"), py::arg("split_angle"), py::arg("min_length"),
          py::call_guard<py::gil_scoped_release>(),
          "The parts where the strands' flow splits on the scalp (a Part node's partsFrom auto): a PartSet "
          "named auto1, auto2... split_angle in degrees, min_length a share of the scalp's size.");

    // ---- Flow ------------------------------------------------------------------
    py::class_<FlowPin>(m, "FlowPin", "A direction arrow bound to the scalp (direction in the root triangle's frame).")
        .def(py::init<>())
        .def_readwrite("root", &FlowPin::root)
        .def_readwrite("local", &FlowPin::local)
        .def_readwrite("line", &FlowPin::line)
        .def_readwrite("seq", &FlowPin::seq);
    py::class_<FlowSink>(m, "FlowSink", "A point hair turns around within a radius: a whorl, a gather or a part.")
        .def(py::init<>())
        .def_readwrite("root", &FlowSink::root)
        .def_readwrite("angle", &FlowSink::angle)
        .def_readwrite("radius", &FlowSink::radius);
    m.def("rename_sculpt_node", &renameSculptNode, py::arg("json"), py::arg("old"), py::arg("new"),
          "A sculpts text with node `old`'s edits under `new`.");
    m.def("copy_sculpt_node", &copySculptNode, py::arg("json"), py::arg("source"), py::arg("copy"),
          "A sculpts text with `copy` given a copy of `source`'s edits.");
    m.def("move_sculpt_nodes", &moveSculptNodes, py::arg("json"), py::arg("moves"),
          "A sculpts text with each (old, new) node move applied in order (GraphEdit.moved_nodes), a compound's "
          "inner nodes' edits with it.");
    m.def("clear_sculpt_node", &clearSculptNode, py::arg("json"), py::arg("node"), "A sculpts text without `node`'s edits.");
    m.def("sculpted_nodes", &sculptedNodes, py::arg("json"), "The nodes holding edits in a sculpts text.");
    m.def("flow_sink_angle", &flowSinkAngle, py::arg("kind"), "The angle of a sink kind: whorl, whorlcw, converge, part.");

    py::class_<FlowSet>(m, "FlowSet", "Flow pins and sinks on the scalp; a FlowField resolves them on a pose.")
        .def(py::init<>())
        .def_readwrite("pins", &FlowSet::pins)
        .def("transferred", py::overload_cast<const ScalpMesh&, const ScalpMesh&, const ScalpFit&>(&FlowSet::transferred, py::const_),
             py::arg("from_mesh"), py::arg("to_mesh"), py::arg("fit") = ScalpFit{},
             "The pins and sinks re-rooted on another scalp, pins keeping their world direction.")
        .def("transferred", py::overload_cast<const ScalpMapping&>(&FlowSet::transferred, py::const_), py::arg("mapping"),
             "The pins and sinks re-rooted on another scalp by a ScalpMapping.")
        .def_readwrite("sinks", &FlowSet::sinks)
        .def_property_readonly("empty", &FlowSet::empty)
        .def_static("make_pin", &FlowSet::makePin, py::arg("scalp"), py::arg("at"), py::arg("direction"),
                    "A pin at a surface point pointing along a direction (projected onto the surface).")
        .def("add_pin",
             [](FlowSet& s, const ScalpMesh& scalp, const SurfacePoint& at, const Vec3f& direction, int line, int seq) {
                 FlowPin pin = FlowSet::makePin(scalp, at, direction);
                 pin.line = line;
                 pin.seq = seq;
                 s.pins.push_back(pin);
                 return s.pins.size() - 1;
             },
             py::arg("scalp"), py::arg("at"), py::arg("direction"), py::arg("line") = -1, py::arg("seq") = 0,
             "Makes and appends a pin; returns its index.")
        .def("add_sink",
             [](FlowSet& s, const StrandRoot& root, float angle, float radius) {
                 FlowSink sink;
                 sink.root = root;
                 sink.angle = angle;
                 sink.radius = radius;
                 s.sinks.push_back(sink);
                 return s.sinks.size() - 1;
             },
             py::arg("root"), py::arg("angle") = 70.0f, py::arg("radius") = 1.0f)
        .def("to_json", &FlowSet::toJson)
        .def_static("from_json", &FlowSet::fromJson, py::arg("text"))
        .def("content_hash", &FlowSet::contentHash);

    py::class_<FlowField, std::shared_ptr<FlowField>>(m, "FlowField", "A FlowSet on a scalp pose: the blended direction anywhere on it.")
        .def(py::init<const FlowSet&, const ScalpMesh&>(), py::arg("flow"), py::arg("scalp"))
        .def_property_readonly("empty", &FlowField::empty)
        .def("at", &FlowField::at, py::arg("position"), py::arg("normal"), "The unit direction hair lies in at a surface point.")
        .def_property_readonly("positions", [](const FlowField& f) { return f.positions(); })
        .def_property_readonly("directions", [](const FlowField& f) { return f.directions(); })
        .def_property_readonly("lines", [](const FlowField& f) { return f.lines(); })
        .def_property_readonly("seqs", [](const FlowField& f) { return f.seqs(); })
        .def_property_readonly("sink_positions", [](const FlowField& f) { return f.sinkPositions(); })
        .def_property_readonly("sink_normals", [](const FlowField& f) { return f.sinkNormals(); })
        .def_property_readonly("sink_angles", [](const FlowField& f) { return f.sinkAngles(); })
        .def_property_readonly("sink_radii", [](const FlowField& f) { return f.sinkRadii(); })
        .def("sink_direction",
             [](const FlowField& f, std::size_t s, const Vec3f& position, const Vec3f& normal) {
                 float w = 0.0f;
                 const Vec3f d = f.sinkDirection(s, position, normal, &w);
                 return py::make_tuple(d, w);
             },
             py::arg("sink"), py::arg("position"), py::arg("normal"), "(direction, weight) of one sink at a point.")
        .def("content_hash", &FlowField::contentHash);
    m.def("surface_frame",
          [](const ScalpMesh& scalp, const StrandRoot& root) {
              Vec3f x, y, z;
              surfaceFrame(scalp, root, x, y, z);
              return py::make_tuple(x, y, z);
          },
          py::arg("scalp"), py::arg("root"), "The world frame (x, y, z=normal) of a scalp point's triangle.");

    // ---- Groom -------------------------------------------------------------------
    py::class_<GroomData>(m, "GroomData",
                          "A groom's complete input: graph and hierarchy JSON, scalp, flat guides, guide poses, "
                          "parts/correctives/flow JSON, control values and the time.")
        .def(py::init<>())
        .def(py::init([](py::handle graph, py::handle hierarchy, std::shared_ptr<ScalpMesh> scalp, std::shared_ptr<GuideSet> guides) {
                 GroomData d;
                 d.graphJson = jsonText(graph);
                 d.hierarchyJson = hierarchy.is_none() ? std::string{} : hierarchy.cast<std::string>();
                 d.scalp = std::move(scalp);
                 d.guides = std::move(guides);
                 return d;
             }),
             py::arg("graph_json") = py::none(), py::arg("hierarchy_json") = py::none(), py::arg("scalp") = nullptr,
             py::arg("guides") = nullptr)
        .def_readwrite("graph_json", &GroomData::graphJson, "Empty or '{}': the default graph.")
        .def_readwrite("hierarchy_json", &GroomData::hierarchyJson, "GuideHierarchy.to_json(); empty: none.")
        .def_property("scalp", [](const GroomData& d) { return mutableShared(d.scalp); }, [](GroomData& d, std::shared_ptr<ScalpMesh> s) { d.scalp = std::move(s); })
        .def_property("guides", [](const GroomData& d) { return mutableShared(d.guides); }, [](GroomData& d, std::shared_ptr<GuideSet> g) { d.guides = std::move(g); },
                      "Flat guides (world space, rooted), or None.")
        .def_property("guide_poses", [](const GroomData& d) { return mutableShared(d.guidePoses); }, [](GroomData& d, std::shared_ptr<GuideSet> g) { d.guidePoses = std::move(g); },
                      "Hierarchy guides replaced by ID (e.g. simulated), or None.")
        .def_readwrite("parts_json", &GroomData::partsJson)
        .def_readwrite("correctives_json", &GroomData::correctivesJson)
        .def_readwrite("flow_json", &GroomData::flowJson)
        .def_readwrite("sculpts_json", &GroomData::sculptsJson, "The Sculpt nodes' edits (sculpt.h), by node name.")
        .def_readwrite("controls", &GroomData::controls, "Control node values by name.")
        .def_readwrite("follow_scalp", &GroomData::followScalp,
                       "Evaluate on the scalp's rest pose and carry each strand by its root's frame (groom files store it).")
        .def_property(
            "scale_source", [](const GroomData& d) { return std::string(scaleSourceName(d.scaleSource)); },
            [](GroomData& d, const std::string& name) { d.scaleSource = scaleSourceFromName(name); },
            "What the groom's scale follows: \"area\" (the scalp's surface area, the default), \"transform\" (its "
            "transform's scale alone) or \"off\" (groom files store it when not \"area\").")
        .def_readwrite("scale_transform_reference", &GroomData::scaleTransformReference,
                       "The transform scale the groom is 1x at (scale_source \"transform\"); 0: the first one measured.")
        .def_readwrite("scale_reference", &GroomData::scaleReference,
                       "The scalp area the groom is 1x at; 0: the first area measured (groom files store it when set).")
        .def_property(
            "meshes",
            [](const GroomData& d) {
                std::map<std::string, std::shared_ptr<ScalpMesh>> out;
                for (const auto& [name, mesh] : d.meshes) out[name] = mutableShared(mesh);
                return out;
            },
            [](GroomData& d, const std::map<std::string, std::shared_ptr<ScalpMesh>>& meshes) {
                d.meshes.clear();
                for (const auto& [name, mesh] : meshes) d.meshes[name] = mesh;
            },
            "Other meshes by name, read by ColliderMesh nodes (e.g. 'collider'); groom files store them ('colliders').")
        .def_readwrite("scalp_fingerprint", &GroomData::scalpFingerprint,
                       "The topology the groom was made on (ScalpMesh.topology_fingerprint(); a groom file's scalp.topology); "
                       "0: not known. A scalp of another is said in the evaluation's errors.")
        .def_readwrite("time", &GroomData::time, "Seconds.");

    m.def("apply_guide_poses", &applyGuidePoses, py::arg("hierarchy"), py::arg("poses"), py::arg("scalp") = nullptr,
          "Replaces the points of hierarchy guides found in `poses` (by ID) and re-evaluates their children.");

    // Groom is GroomSession: bound in bind_session.cpp.

    // ---- Groom files -------------------------------------------------------------------
    py::class_<CacheRef>(m, "CacheRef", "A reference to an Alembic cache: file (relative to the groom file) and object name.")
        .def(py::init<>())
        .def(py::init([](const std::string& file, const std::string& object) { return CacheRef{file, object}; }), py::arg("file"), py::arg("object") = std::string{})
        .def_readwrite("file", &CacheRef::file)
        .def_readwrite("object", &CacheRef::object)
        .def_property_readonly("empty", &CacheRef::empty);

    py::class_<CardBakeRef>(m, "CardBakeRef",
                            "A card bake's files: <prefix>Depth|Coverage|Tangent|Attribute|Color|AO|Normal.tga, <prefix>Atlas.json, "
                            "<prefix>Preview.png (prefix relative to the groom file), and the stamp of what the cards were made from.")
        .def(py::init<>())
        .def(py::init([](const std::string& prefix, const std::string& strands) { return CardBakeRef{prefix, strands}; }),
             py::arg("prefix"), py::arg("strands") = std::string{})
        .def_readwrite("prefix", &CardBakeRef::prefix)
        .def_readwrite("strands", &CardBakeRef::strands)
        .def_property_readonly("empty", &CardBakeRef::empty);

    py::class_<GroomFile>(m, "GroomFile",
                          "A .groom file: the GroomData, scalp and guide cache references, card bake, frame rate, units, "
                          "up axis, source and search paths.")
        .def(py::init<>())
        .def(py::init([](const GroomData& data) { GroomFile f; f.data = data; return f; }), py::arg("data"))
        .def_readwrite("data", &GroomFile::data)
        .def_readwrite("scalp_cache", &GroomFile::scalpCache)
        .def_readwrite("guide_cache", &GroomFile::guideCache)
        .def_readwrite("card_bake", &GroomFile::cardBake)
        .def_readwrite("transfer_markers", &GroomFile::transferMarkers)
        .def_readwrite("units", &GroomFile::units)
        .def_readwrite("up_axis", &GroomFile::upAxis, "\"Y\" or \"Z\": the world's up axis the points are in.")
        .def_readwrite("frames_per_second", &GroomFile::framesPerSecond)
        .def_readwrite("source", &GroomFile::source)
        .def_readwrite("search_paths", &GroomFile::searchPaths)
        .def_readwrite("directory", &GroomFile::directory, "The folder the file was read from (not stored).")
        .def("to_json", &groomFileToJson)
        .def_static("from_json", &groomFileFromJson, py::arg("text"), py::arg("base_dir") = std::string{}, "Raises GroomFileError.")
        .def("write", [](const GroomFile& f, const std::string& path) { writeGroomFile(path, f); }, py::arg("path"), "Raises GroomFileError.")
        .def_static("read", &readGroomFile, py::arg("path"), "Raises GroomFileError.");
    m.def("groom_file_to_json", &groomFileToJson, py::arg("file"));
    m.def("groom_file_from_json", &groomFileFromJson, py::arg("text"), py::arg("base_dir") = std::string{});
    m.def("write_groom_file", &writeGroomFile, py::arg("path"), py::arg("file"));
    m.def("read_groom_file", &readGroomFile, py::arg("path"));
    m.def("convert_up_axis", &convertUpAxis, py::arg("file"), py::arg("up_axis"),
          "Turns the file's points (scalp, rest pose, flat guides, the hierarchy's world shapes, colliders) and the graph's "
          "world-direction params into another up axis (\"Y\" or \"Z\"), a quarter turn about X. Raises GroomFileError "
          "on another axis.");
    m.def("frame_to_seconds", &frameToSeconds, py::arg("frame"), py::arg("fps"),
          "Frame `frame` at `fps` in seconds, by the rule every host keeps: frame / fps, an NTSC rate (23.976, 29.97) exact.");
    m.def("seconds_to_frame", &secondsToFrame, py::arg("seconds"), py::arg("fps"), "frame_to_seconds' inverse.");

    // Paths.
    m.def("expand_environment", &expandEnvironment, py::arg("path"), "$VAR, ${VAR} and %VAR% replaced from the environment.");
    m.def("is_absolute_path", &isAbsolutePath, py::arg("path"));
    m.def("resolve_path", &resolvePath, py::arg("path"), "Variables expanded; a relative path through the current resolver.");
    m.def("portable_path", &portablePath, py::arg("path"), py::arg("folders"), py::arg("beside") = false,
          "A path as every host stores it: relative to the first folder holding it, else (beside) to the first with "
          "\"..\" on the same drive, else absolute; variables and relative paths kept. Forward slashes.");
    m.def("path_from_groom_folder", &pathFromGroomFolder, py::arg("path"), py::arg("folder"),
          "A path stored relative to a groom file's folder: \"..\" beside it allowed.");

    // ---- LOD -------------------------------------------------------------------------------
    py::class_<LodCamera>(m, "LodCamera", "The camera in groom space: a row-vector 4x4 matrix (rows x, y, z axes, position) and half-FOV tangents.")
        .def(py::init<>())
        .def_property(
            "matrix",
            [](const LodCamera& c) {
                std::vector<std::vector<float>> rows(4, std::vector<float>(4));
                for (int r = 0; r < 4; ++r)
                    for (int k = 0; k < 4; ++k) rows[r][k] = c.matrix[r][k];
                return rows;
            },
            [](LodCamera& c, py::handle m) {
                const std::vector<float> flat = [&] {
                    std::vector<float> f;
                    if (py::isinstance<py::sequence>(m) && py::len(m) == 4 && py::isinstance<py::sequence>(py::reinterpret_borrow<py::sequence>(m)[0])) {
                        for (py::handle row : py::reinterpret_borrow<py::sequence>(m))
                            for (py::handle x : py::reinterpret_borrow<py::sequence>(row)) f.push_back(x.cast<float>());
                    } else {
                        f = toFloats(m);
                    }
                    return f;
                }();
                if (flat.size() != 16) throw py::value_error("matrix needs 16 values");
                for (int r = 0; r < 4; ++r)
                    for (int k = 0; k < 4; ++k) c.matrix[r][k] = flat[static_cast<std::size_t>(r * 4 + k)];
            },
            "4 rows of 4 (or 16 flat values).")
        .def_readwrite("tan_half_fov_x", &LodCamera::tanHalfFovX)
        .def_readwrite("tan_half_fov_y", &LodCamera::tanHalfFovY);
    py::class_<LodSettings>(m, "LodSettings", "Distance thinning and frustum culling settings.")
        .def(py::init<>())
        .def_readwrite("distance", &LodSettings::distance)
        .def_readwrite("near_distance", &LodSettings::nearDistance)
        .def_readwrite("far_distance", &LodSettings::farDistance)
        .def_readwrite("min_density", &LodSettings::minDensity)
        .def_readwrite("width_compensation", &LodSettings::widthCompensation)
        .def_readwrite("frustum", &LodSettings::frustum)
        .def_readwrite("margin", &LodSettings::margin)
        .def_readwrite("backface", &LodSettings::backface)
        .def_readwrite("backface_angle", &LodSettings::backfaceAngle);
    py::class_<LodPlan>(m, "LodPlan", "Which strands a LOD pass keeps and how their widths scale.")
        .def(py::init<>())
        .def_readwrite("kept", &LodPlan::kept)
        .def_readwrite("width_scale", &LodPlan::widthScale)
        .def_readwrite("culled", &LodPlan::culled)
        .def_readwrite("thinned", &LodPlan::thinned)
        .def_readwrite("backfacing", &LodPlan::backfacing);
    m.def(
        "plan_lod",
        [](const StrandSet& strands, const LodCamera& camera, const LodSettings& settings, const ScalpMesh* scalp) {
            return planLod(strands, camera, settings, scalp);
        },
        py::arg("strands"), py::arg("camera"), py::arg("settings"), py::arg("scalp") = static_cast<const ScalpMesh*>(nullptr),
        py::call_guard<py::gil_scoped_release>(),
        "Decides which strands to keep for a camera (backface culling reads the root normals off `scalp`).");
    m.def("apply_lod", &applyLod, py::arg("strands"), py::arg("plan"), py::call_guard<py::gil_scoped_release>(),
          "The kept strands, widths scaled (the plan may come from another motion key of the same strands).");

    // ---- Cards -----------------------------------------------------------------------------
    py::enum_<CardSettings::CrossStyle>(m, "CrossStyle", "Cards per clump: Auto, a Single ribbon, or Cross(ed) ribbons.")
        .value("Auto", CardSettings::CrossStyle::Auto)
        .value("Single", CardSettings::CrossStyle::Single)
        .value("Cross", CardSettings::CrossStyle::Cross);
    py::enum_<CardSettings::FeatherMode>(m, "FeatherMode", "How feathers are carded: Auto, PerFeather, PerVane (as hair) or Off.")
        .value("Auto", CardSettings::FeatherMode::Auto)
        .value("PerFeather", CardSettings::FeatherMode::PerFeather)
        .value("PerVane", CardSettings::FeatherMode::PerVane)
        .value("Off", CardSettings::FeatherMode::Off);
    py::class_<CardSettings>(m, "CardSettings", "How strands are grouped into cards and how the cards are shaped and share textures.")
        .def(py::init<>())
        .def_readwrite("channel", &CardSettings::channel)
        .def_readwrite("max_cards", &CardSettings::maxCards)
        .def_readwrite("cards_per_clump", &CardSettings::cardsPerClump)
        .def_readwrite("layers", &CardSettings::layers)
        .def_readwrite("margin", &CardSettings::margin)
        .def_readwrite("fit_twist", &CardSettings::fitTwist)
        .def_readwrite("ruffle", &CardSettings::ruffle)
        .def_readwrite("flyaway_cards", &CardSettings::flyawayCards)
        .def_readwrite("segments", &CardSettings::segments)
        .def_readwrite("width_scale", &CardSettings::widthScale)
        .def_readwrite("width_profile", &CardSettings::widthProfile)
        .def_readwrite("twist", &CardSettings::twist)
        .def_readwrite("curvature", &CardSettings::curvature)
        .def_readwrite("width_segments", &CardSettings::widthSegments)
        .def_readwrite("min_width", &CardSettings::minWidth)
        .def_readwrite("min_strands", &CardSettings::minStrands)
        .def_readwrite("cross_style", &CardSettings::crossStyle)
        .def_readwrite("cross_count", &CardSettings::crossCount)
        .def_readwrite("braids", &CardSettings::braids)
        .def_readwrite("segments_by_length", &CardSettings::segmentsByLength)
        .def_readwrite("min_segments", &CardSettings::minSegments)
        .def_readwrite("volume_normals", &CardSettings::volumeNormals)
        .def_readwrite("variations", &CardSettings::variations)
        .def_readwrite("mirror", &CardSettings::mirror)
        .def_readwrite("padding", &CardSettings::padding)
        .def_readwrite("feather_mode", &CardSettings::featherMode)
        .def_readwrite("feather_segments", &CardSettings::featherSegments)
        .def_readwrite("feather_min_length", &CardSettings::featherMinLength)
        .def_readwrite("feather_spine", &CardSettings::featherSpine);

    py::class_<CardMesh, std::shared_ptr<CardMesh>> cardMesh(m, "CardMesh", "Hair cards: vertices, normals, atlas UVs, quads and per-card data.");
    py::class_<CardMesh::Card>(cardMesh, "Card", "One card: its vertices, atlas cell, stations and member strands.")
        .def_readonly("first_vertex", &CardMesh::Card::firstVertex)
        .def_readonly("stations", &CardMesh::Card::stations)
        .def_property_readonly("cell", [](const CardMesh::Card& c) { return py::make_tuple(c.cell[0], c.cell[1], c.cell[2], c.cell[3]); }, "Atlas cell (u0, v0, u1, v1).")
        .def_property_readonly("center", [](const CardMesh::Card& c) { return c.center; })
        .def_property_readonly("side", [](const CardMesh::Card& c) { return c.side; })
        .def_property_readonly("normal", [](const CardMesh::Card& c) { return c.normal; })
        .def_readonly("width", &CardMesh::Card::width)
        .def_readonly("strands", &CardMesh::Card::strands, "Member strand indices.")
        .def_readonly("clump", &CardMesh::Card::clump, "The clump the card was made from (-1: a flyaway or braid card).")
        .def_readonly("layer", &CardMesh::Card::layer, "Its depth layer in the clump (0 innermost).")
        .def_readonly("flyaway", &CardMesh::Card::flyaway)
        .def_readonly("braid", &CardMesh::Card::braid, "The braid the card stands for (a Braid node's braidId; -1: none).")
        .def_readonly("feather", &CardMesh::Card::feather, "The feather the card stands for (its index among the node's feathers; -1: hair).")
        .def_readonly("across", &CardMesh::Card::across)
        .def_readonly("spine", &CardMesh::Card::spine, "A feather card with a column on its shaft (across 2: left edge, spine, right edge).")
        .def_readonly("spine_offset", &CardMesh::Card::spineOffset, "Where the spine sits across from the card's center (scene units along side).")
        .def_readonly("curvature", &CardMesh::Card::curvature)
        .def_readonly("variation_of", &CardMesh::Card::variationOf)
        .def_readonly("mirrored", &CardMesh::Card::mirrored);
    cardMesh.def(py::init<>())
        .def_property_readonly("positions", [](const CardMesh& c) { return c.positions; })
        .def_property_readonly("normals", [](const CardMesh& c) { return c.normals; })
        .def_readonly("u", &CardMesh::u)
        .def_readonly("v", &CardMesh::v)
        .def_readonly("quads", &CardMesh::quads, "4 vertex indices per quad, flat.")
        .def_readonly("cards", &CardMesh::cards)
        .def_readonly("padding", &CardMesh::padding)
        .def_readonly("atlas_size", &CardMesh::atlasSize)
        .def_readonly("thinnest_strand", &CardMesh::thinnestStrand)
        .def("positions_view", [](py::object self) { return pointsView(self, self.cast<const CardMesh&>().positions, true); })
        .def("normals_view", [](py::object self) { return pointsView(self, self.cast<const CardMesh&>().normals, true); })
        .def("u_view", [](py::object self) { return flatView(self, self.cast<const CardMesh&>().u, true); })
        .def("v_view", [](py::object self) { return flatView(self, self.cast<const CardMesh&>().v, true); })
        .def("quads_view", [](py::object self) {
                const auto& q = self.cast<const CardMesh&>().quads;
                return makeView(self, q.data(), {static_cast<py::ssize_t>(q.size() / 4), 4}, true);
            })
        .def_property_readonly("vertex_count", &CardMesh::vertexCount)
        .def_property_readonly("triangle_count", &CardMesh::triangleCount)
        .def_property_readonly("card_count", [](const CardMesh& c) { return c.cards.size(); })
        .def("texture_count", &CardMesh::textureCount)
        .def("__repr__", [](const CardMesh& c) { return "<CardMesh cards=" + std::to_string(c.cards.size()) + " vertices=" + std::to_string(c.vertexCount()) + ">"; });
    m.def("build_cards", [](const StrandSet& s, const ScalpMesh* scalp, const CardSettings& settings) { return buildCards(s, scalp, settings); },
          py::arg("strands"), py::arg("scalp"), py::arg("settings") = CardSettings{}, py::call_guard<py::gil_scoped_release>(),
          "Groups strands into cards and lays out the atlas.");
    m.def("combine_card_layers",
          [](const std::vector<std::shared_ptr<CardMesh>>& layers) {
              std::vector<std::shared_ptr<const CardMesh>> in(layers.begin(), layers.end());
              return combineCardLayers(in);
          },
          py::arg("layers"), "Several layers' cards in one mesh.");
    m.def("layout_cards", &layoutCards, py::arg("mesh"), py::arg("padding") = 0.004f, "Packs the atlas and rebuilds the vertices.");

    // The levels of detail every host offers (card_presets.h), as {lod: {param: value}}.
    m.def("card_lod_presets", [] {
        py::dict out;
        for (const CardLodPreset& p : cardLodPresets()) {
            py::dict d;
            d["widthScale"] = p.widthScale;
            d["triangles"] = p.triangles;
            d["cardsPerClump"] = p.cardsPerClump;
            d["layers"] = p.layers;
            d["flyawayCards"] = p.flyawayCards;
            d["ruffle"] = p.ruffle;
            d["segments"] = p.segments;
            d["widthSegments"] = p.widthSegments;
            d["curvature"] = p.curvature;
            d["variations"] = p.variations;
            d["atlasSize"] = p.atlasSize;
            out[py::str(p.lod)] = d;
        }
        return out;
    }, "The hair cards' levels of detail: {\"hero\" | \"mid\" | \"low\": {triangles, cardsPerClump, layers, ...}}.");
    m.def("feather_card_lod_presets", [] {
        py::dict out;
        for (const FeatherCardLodPreset& p : featherCardLodPresets()) {
            py::dict d;
            d["triangles"] = p.triangles;
            d["featherSegments"] = p.featherSegments;
            d["featherSpine"] = p.featherSpine;
            d["featherMinLength"] = p.featherMinLength;
            d["variations"] = p.variations;
            d["atlasSize"] = p.atlasSize;
            out[py::str(p.lod)] = d;
        }
        return out;
    }, "The feather cards' levels of detail, as card_lod_presets.");
    m.def("feather_card_triangles", &featherCardTriangles, py::arg("segments"), py::arg("spine") = true,
          "Triangles one vaned feather's card costs: 2 a segment, 4 with the spine.");
    m.def("card_preset_params",
          [](const std::string& lod, const std::string& kind, std::optional<int> triangles) {
              py::dict out;
              for (const auto& [name, value] : cardPresetParams(lod, kind, triangles)) out[py::str(name)] = castParamValue(value);
              return out;
          },
          py::arg("lod"), py::arg("kind") = "hair", py::arg("triangles") = py::none(),
          "The params a level of detail sets on a Cards node of `kind` (hair, feathers, both), maxCards from the budget.");
    // Map names, every host's rule, and a rename's readers and file (scalp_maps.h).
    m.def("valid_map_name", &validMapName, py::arg("name"), "Letters, digits and _, not starting with a digit.");
    m.def("map_name_error", &mapNameError, py::arg("name"), "Why a name cannot name a map (\"\" when it can).");
    m.def("sanitize_map_name", &sanitizeMapName, py::arg("name"), "A valid map name made from any text.");
    m.def("retarget_map_readers",
          [](const std::string& graphJson, const std::string& from, const std::string& to) {
              bool changed = false;
              std::string text = retargetMapReaders(graphJson, from, to, &changed);
              return py::make_tuple(text, changed);
          },
          py::arg("graph_json"), py::arg("old"), py::arg("new"),
          "(graph_json, changed): every reader of map `old` (Map nodes, map params, inside compounds) reading `new`.");
    m.def("renamed_map_texture", &renamedMapTexture, py::arg("stored"), py::arg("old"), py::arg("new"),
          "A texture map's file renamed with its map (\"\" when the file is not named after it).");
    // Ptex files (ptex.h): XGen's paint maps read into maps at a scalp's UVs or vertices.
    py::register_exception<PtexError>(m, "PtexError", PyExc_RuntimeError);
    m.def("ptex_info",
          [](const std::string& path) {
              const PtexImage image = readPtex(path);
              py::dict out;
              out["faces"] = image.faces.size();
              out["channels"] = image.channels;
              out["alpha_channel"] = image.alphaChannel;
              static const char* kTypes[] = {"uint8", "uint16", "half", "float"};
              out["data_type"] = kTypes[static_cast<int>(image.dataType)];
              int maxU = 0, maxV = 0, constant = 0;
              for (const auto& f : image.faces) {
                  maxU = std::max(maxU, f.ures);
                  maxV = std::max(maxV, f.vres);
                  constant += f.constant ? 1 : 0;
              }
              out["max_resolution"] = py::make_tuple(maxU, maxV);
              out["constant_faces"] = constant;
              // How many different texels (to 1/4096, up to 65536): the clumps of an XGen clump map.
              std::set<std::vector<int>> distinct;
              for (const auto& f : image.faces) {
                  for (std::size_t i = 0; i + image.channels <= f.pixels.size() && distinct.size() < 65536; i += image.channels) {
                      std::vector<int> key(image.channels);
                      for (int c = 0; c < image.channels; ++c) key[c] = static_cast<int>(std::lround(f.pixels[i + c] * 4096.0f));
                      distinct.insert(std::move(key));
                  }
              }
              out["distinct_values"] = distinct.size();
              return out;
          },
          py::arg("path"),
          "A .ptx file's faces, channels, data type, largest face resolution, constant faces and distinct texel "
          "values. Raises PtexError.");
    m.def("ptex_sample",
          [](const std::string& path, int face, float u, float v) {
              const PtexImage image = readPtex(path);
              std::vector<float> out(image.channels);
              image.sample(face, u, v, out.data());
              return out;
          },
          py::arg("path"), py::arg("face"), py::arg("u"), py::arg("v"),
          "Face `face` of a .ptx file at (u, v) in 0-1, bilinear: its channels.");
    m.def("ptex_to_png",
          [](const std::string& path, const std::vector<int>& faceCounts, const std::vector<int>& uvIds,
             const std::vector<float>& us, const std::vector<float>& vs, const std::string& out, int width, int height,
             int channels, int padding) {
              double covered = 0.0;
              PtexImage image;
              {
                  py::gil_scoped_release release;
                  image = readPtex(path);
                  PtexMesh mesh;
                  mesh.faceCounts = faceCounts;
                  mesh.uvIds = uvIds;
                  mesh.us = us;
                  mesh.vs = vs;
                  const std::vector<float> pixels = ptexToImage(image, mesh, width, height, channels, padding, &covered);
                  std::vector<std::uint8_t> bytes(pixels.size());
                  for (std::size_t i = 0; i < pixels.size(); ++i) {
                      bytes[i] = static_cast<std::uint8_t>(std::lround(std::clamp(pixels[i], 0.0f, 1.0f) * 255.0f));
                  }
                  writeCompressedPng(out, width, height, channels, bytes);
              }
              py::dict result;
              result["covered"] = covered;
              result["faces"] = image.faces.size();
              result["file_channels"] = image.channels;
              return result;
          },
          py::arg("path"), py::arg("face_counts"), py::arg("uv_ids"), py::arg("us"), py::arg("vs"), py::arg("out"),
          py::arg("width") = 1024, py::arg("height") = 1024, py::arg("channels") = 1, py::arg("padding") = 4,
          "A .ptx file painted at a mesh's UVs (polygon i is Ptex face i) into an 8-bit PNG at `out` (channels "
          "1 gray or 4 RGBA): {covered (share of pixels on a polygon), faces, file_channels}. Raises PtexError.");
    m.def("ptex_vertex_values",
          [](const std::string& path, const std::vector<int>& faceCounts, const std::vector<int>& vertexIds, int vertexCount,
             int channels) {
              py::gil_scoped_release release;
              PtexMesh mesh;
              mesh.faceCounts = faceCounts;
              mesh.vertexIds = vertexIds;
              mesh.vertexCount = vertexCount;
              return ptexToVertexValues(readPtex(path), mesh, channels);
          },
          py::arg("path"), py::arg("face_counts"), py::arg("vertex_ids"), py::arg("vertex_count"), py::arg("channels") = 1,
          "A .ptx file as values per mesh vertex (`channels` each): the mean of the vertex's corner texels.");
    // A card bake's files and its record (card_files.h).
    m.def("card_map_names", &cardMapNames, "Depth, Coverage, Tangent, Attribute, Color, AO, Normal: the maps a card bake writes.");
    m.def("card_bake_suffixes", &cardBakeSuffixes,
          "The file suffixes of a card bake after its prefix: the seven maps' .tga then Atlas.json.");
    m.def("card_bake_files", [](const std::string& prefix) { return CardBakePaths(prefix).files(); }, py::arg("prefix"),
          "A bake's files at a prefix (<folder>/<name>_): the seven maps then the atlas.");
    m.def("groom_file_card_bake_prefix", &groomFileCardBakePrefix, py::arg("stem"),
          "\"<stem>_cards/<stem>_\": where a groom file's bake sits, relative to the file's folder.");
    m.def("format_card_maps", &formatCardMaps, py::arg("prefix"), py::arg("stamp") = std::string(), py::arg("bake") = 0,
          "The cardMaps text of a bake at a stored prefix, its stamp and bake number.");
    m.def("parse_card_maps", &parseCardMaps, py::arg("text"), "A cardMaps text as {key: value}.");
    m.def("card_maps_prefix", &cardMapsPrefix, py::arg("text"), "The prefix a cardMaps text names (\"\": none).");
    m.def("with_card_maps_prefix", &withCardMapsPrefix, py::arg("text"), py::arg("prefix"),
          "The cardMaps text with its bake's paths moved to another prefix.");
    m.def("card_node_kind", &cardNodeKind, py::arg("graph_json"), py::arg("node"),
          "What a graph's Cards node cards: \"hair\", \"feathers\" or \"both\" (its featherMode, else what reaches it).");
    m.def("strands_reaching",
          [](const std::string& graphJson, const std::string& node) {
              const StrandsReaching r = strandsReaching(graphJson, node);
              return py::make_tuple(r.feathers, r.hair);
          },
          py::arg("graph_json"), py::arg("node"),
          "(feathers, hair): whether the strands reaching a node hold feathers (a node on the way grows them) and hair.");

    py::class_<PyCardBakeSettings>(m, "CardBakeSettings", "Card texture bake: size, scalp (root UVs), fallback color, occlusion.")
        .def(py::init<>())
        .def_readwrite("size", &PyCardBakeSettings::size)
        .def_property("scalp", [](const PyCardBakeSettings& s) { return s.scalpRef; }, &PyCardBakeSettings::setScalp)
        .def_readwrite("color", &PyCardBakeSettings::color)
        .def_readwrite("strand_width", &PyCardBakeSettings::strandWidth)
        .def_readwrite("occlusion", &PyCardBakeSettings::occlusion)
        .def_readwrite("dilate", &PyCardBakeSettings::dilate)
        .def_readwrite("root_fade", &PyCardBakeSettings::rootFade)
        .def_readwrite("min_strand_texels", &PyCardBakeSettings::minStrandTexels);
    py::class_<CardTextures>(m, "CardTextures", "Baked card textures (Unreal hair card layout), row 0 at the top.")
        .def_readonly("size", &CardTextures::size)
        .def_readonly("coverage", &CardTextures::coverage)
        .def_readonly("depth", &CardTextures::depth)
        .def_readonly("tangent", &CardTextures::tangent)
        .def_readonly("normal", &CardTextures::normal)
        .def_readonly("attribute", &CardTextures::attribute)
        .def_readonly("color", &CardTextures::color)
        .def_readonly("occlusion", &CardTextures::occlusion)
        .def("coverage_view", [](py::object self) { const auto& t = self.cast<const CardTextures&>(); return bytesView(self, t.coverage, t.size, 1); })
        .def("depth_view", [](py::object self) { const auto& t = self.cast<const CardTextures&>(); return bytesView(self, t.depth, t.size, 1); })
        .def("tangent_view", [](py::object self) { const auto& t = self.cast<const CardTextures&>(); return bytesView(self, t.tangent, t.size, 3); })
        .def("normal_view", [](py::object self) { const auto& t = self.cast<const CardTextures&>(); return bytesView(self, t.normal, t.size, 3); })
        .def("attribute_view", [](py::object self) { const auto& t = self.cast<const CardTextures&>(); return bytesView(self, t.attribute, t.size, 4); })
        .def("color_view", [](py::object self) { const auto& t = self.cast<const CardTextures&>(); return bytesView(self, t.color, t.size, 3); })
        .def("occlusion_view", [](py::object self) { const auto& t = self.cast<const CardTextures&>(); return bytesView(self, t.occlusion, t.size, 1); });
    m.def("bake_card_textures",
          [](const StrandSet& s, const CardMesh& cards, const PyCardBakeSettings& settings) { return bakeCardTextures(s, cards, settings); },
          py::arg("strands"), py::arg("cards"), py::arg("settings") = PyCardBakeSettings{}, py::call_guard<py::gil_scoped_release>());

    // Card map exports (card_maps.h): the presets (built in and the user's), the sources, and an
    // export of laid-out cards' maps as a preset packs them.
    auto loads = [](const std::string& text) { return py::module_::import("json").attr("loads")(text); };
    auto presetOf = [](const py::object& preset) {
        if (py::isinstance<py::str>(preset)) {
            const std::string name = preset.cast<std::string>();
            if (const auto found = findCardMapPreset(name)) return *found;
            throw py::value_error("no card map preset named '" + name + "'");
        }
        try {
            return cardMapPresetFromJson(py::module_::import("json").attr("dumps")(preset).cast<std::string>());
        } catch (const std::runtime_error& e) {
            throw py::value_error(e.what());
        }
    };
    m.def("card_map_presets", [loads] { return loads(cardMapPresetsJson()); },
          "Every card map preset: the built-in ones, then the user's (Documents/GroomLab/card_presets and "
          "GROOMLAB_CARD_PRESET_PATH), each {name, description, format, bitDepth, normal, builtin, path, files: "
          "[{map, channels: [R, G, B, A], colorSpace}]}.");
    m.def("card_map_sources", [loads] { return loads(cardMapSourcesJson()); },
          "The maps a card export packs from: {sources: [{name, label, channels, channelNames, color, description}], "
          "channels: every channel name a packing table offers, formats: [{name, bitDepths}]}.");
    m.def("save_card_map_preset", [presetOf](const py::object& preset) {
              try {
                  return saveCardMapPreset(presetOf(preset));
              } catch (const std::runtime_error& e) {
                  throw py::value_error(e.what());
              }
          },
          py::arg("preset"), "Saves a preset ({name, files, ...}) to the user's folder; returns its file.");
    m.def("delete_card_map_preset", &deleteCardMapPreset, py::arg("name"), "Deletes the user's preset of that name; whether it did.");
    m.def("check_card_map_preset", [](const py::object& preset) {
              try {
                  return checkCardMapPreset(cardMapPresetFromJson(py::module_::import("json").attr("dumps")(preset).cast<std::string>()));
              } catch (const std::runtime_error& e) {
                  return std::string(e.what());
              }
          },
          py::arg("preset"), "\"\" or what is wrong with a preset.");
    m.def("card_map_preset_user_dir", &cardMapPresetUserDir, "The user's card preset folder.");
    m.def("card_map_auto_padding", &cardMapAutoPadding, py::arg("resolution"), "The padding Auto gives at a resolution.");
    m.def("card_map_file_name", &cardMapFileName, py::arg("template"), py::arg("name"), py::arg("map"), py::arg("resolution"),
          "The file name (no extension) a naming template makes.");
    m.def("write_card_maps",
          [presetOf](const StrandSet& s, const CardMesh& cards, const PyCardBakeSettings& bake, const py::object& preset,
                     const std::string& directory, const std::string& name, const std::string& format, int bitDepth, int padding,
                     int supersample, const std::string& nameTemplate) {
              const CardMapPreset p = presetOf(preset);
              CardMapExport settings;
              settings.resolution = std::clamp(bake.size, 16, 8192);
              settings.format = format;
              settings.bitDepth = bitDepth;
              settings.padding = padding;
              settings.supersample = supersample;
              settings.nameTemplate = nameTemplate;
              py::gil_scoped_release release;
              CardMaps maps = bakeCardMaps(s, cards, cardMapBakeSettings(bake, settings, settings.resolution));
              if (maps.size > settings.resolution) maps = downsampleCardMaps(maps, maps.size / settings.resolution);
              return writeCardMaps(maps, p, settings, directory, name);
          },
          py::arg("strands"), py::arg("cards"), py::arg("settings"), py::arg("preset"), py::arg("directory"), py::arg("name"),
          py::arg("format") = std::string(), py::arg("bit_depth") = 0, py::arg("padding") = -1, py::arg("supersample") = 1,
          py::arg("name_template") = std::string("{name}_{map}"),
          "Bakes laid-out cards' maps at settings.size and writes them as a preset (a name or a dict) packs them; returns the paths.");

    py::class_<ScalpBakeSettings>(m, "ScalpBakeSettings", "Hair cap bake: the groom seen from outside in the scalp's UVs.")
        .def(py::init<>())
        .def_readwrite("size", &ScalpBakeSettings::size)
        .def_readwrite("reach", &ScalpBakeSettings::reach)
        .def_readwrite("color", &ScalpBakeSettings::color)
        .def_readwrite("strand_width", &ScalpBakeSettings::strandWidth)
        .def_readwrite("fill", &ScalpBakeSettings::fill);
    py::class_<ScalpTextures>(m, "ScalpTextures", "Baked scalp color and coverage.")
        .def_readonly("size", &ScalpTextures::size)
        .def_readonly("color", &ScalpTextures::color)
        .def_readonly("coverage", &ScalpTextures::coverage)
        .def("color_view", [](py::object self) { const auto& t = self.cast<const ScalpTextures&>(); return bytesView(self, t.color, t.size, 3); })
        .def("coverage_view", [](py::object self) { const auto& t = self.cast<const ScalpTextures&>(); return bytesView(self, t.coverage, t.size, 1); });
    m.def("bake_scalp_textures", &bakeScalpTextures, py::arg("strands"), py::arg("scalp"), py::arg("settings") = ScalpBakeSettings{},
          py::call_guard<py::gil_scoped_release>(), "Raises ValueError when the scalp has no UVs.");
    m.def("write_tga", [](const std::string& path, int w, int h, int channels, py::buffer pixels) {
              const py::buffer_info info = pixels.request();
              const auto* p = static_cast<const std::uint8_t*>(info.ptr);
              py::ssize_t n = info.itemsize;
              for (py::ssize_t s : info.shape) n *= s;
              writeTga(path, w, h, channels, std::vector<std::uint8_t>(p, p + n));
          },
          py::arg("path"), py::arg("width"), py::arg("height"), py::arg("channels"), py::arg("pixels"),
          "Writes an uncompressed TGA from a bytes-like object (1, 3 or 4 channels).");

    // ---- USD ---------------------------------------------------------------------------------
    py::class_<UsdCurvesWriter::Options>(m, "UsdOptions", "USD layer options: frame rate, meters per unit, up axis, comment.")
        .def(py::init<>())
        .def_readwrite("frames_per_second", &UsdCurvesWriter::Options::framesPerSecond)
        .def_readwrite("meters_per_unit", &UsdCurvesWriter::Options::metersPerUnit)
        .def_readwrite("up_axis", &UsdCurvesWriter::Options::upAxis)
        .def_readwrite("comment", &UsdCurvesWriter::Options::comment)
        .def_property(
            "scales",
            [](const UsdCurvesWriter::Options& o) {
                using S = UsdCurvesWriter::Options::Scales;
                return std::string(o.scales == S::Mesh ? "mesh" : o.scales == S::Instancer ? "instancer" : o.scales == S::Both ? "both" : "none");
            },
            [](UsdCurvesWriter::Options& o, const std::string& v) {
                using S = UsdCurvesWriter::Options::Scales;
                if (v == "mesh") o.scales = S::Mesh;
                else if (v == "instancer") o.scales = S::Instancer;
                else if (v == "both") o.scales = S::Both;
                else if (v == "none") o.scales = S::None;
                else throw py::value_error("scales must be mesh, instancer, both or none");
            },
            "How scales are written: mesh (bent, exact), instancer (a PointInstancer), both or none.");
    py::class_<UsdCurvesWriter>(m, "UsdCurvesWriter",
                                "Writes strands as a BasisCurves prim in a .usda layer, one time sample per frame. "
                                "Use as a context manager or call close().")
        .def(py::init([](const std::string& path, const std::string& name, const UsdCurvesWriter::Options& options) {
                 return std::make_unique<UsdCurvesWriter>(path, name, options);
             }),
             py::arg("path"), py::arg("name"), py::arg("options") = UsdCurvesWriter::Options{})
        .def("write_frame", &UsdCurvesWriter::writeFrame, py::arg("frame"), py::arg("strands"), py::arg("scalp") = nullptr,
             py::arg("scales") = nullptr,
             py::call_guard<py::gil_scoped_release>(), "Frames must increase; the scalp supplies root UVs.")
        .def("frame_count", &UsdCurvesWriter::frameCount)
        .def("close", &UsdCurvesWriter::close, py::call_guard<py::gil_scoped_release>())
        .def("__enter__", [](UsdCurvesWriter& w) -> UsdCurvesWriter& { return w; }, py::return_value_policy::reference)
        .def("__exit__", [](UsdCurvesWriter& w, py::handle, py::handle, py::handle) { w.close(); });
    m.def("usd_identifier", &usdIdentifier, py::arg("name"), "A valid USD prim name made from `name`.");

    // ---- V-Ray -------------------------------------------------------------------------------
    py::class_<VrsceneWriter::Options>(m, "VrsceneOptions",
                                       "V-Ray scene options: frame rate, meters per unit, up axis, comment, the look's "
                                       "highlight (set_look), min_pixel_width, compress.")
        .def(py::init<>())
        .def_readwrite("frames_per_second", &VrsceneWriter::Options::framesPerSecond)
        .def_readwrite("meters_per_unit", &VrsceneWriter::Options::metersPerUnit)
        .def_readwrite("up_axis", &VrsceneWriter::Options::upAxis)
        .def_readwrite("comment", &VrsceneWriter::Options::comment)
        .def_readwrite("min_pixel_width", &VrsceneWriter::Options::minPixelWidth)
        .def_readwrite("compress", &VrsceneWriter::Options::compress)
        .def(
            "set_look",
            [](VrsceneWriter::Options& o, float specular, float roughness, float tilt) {
                o.lobes = hairLobes(specular, roughness, tilt);
            },
            py::arg("specular") = kDefaultHairSpecular, py::arg("roughness") = kDefaultHairRoughness,
            py::arg("tilt") = kDefaultHairTilt,
            "The groom's highlight (HairColor's specular, roughness and tilt: GroomSession.hair_look()) for the "
            "hair shader.");
    py::class_<VrsceneWriter>(m, "VrsceneWriter",
                              "Writes strands and their hair shader as a V-Ray scene (.vrscene), values that change "
                              "between frames as interpolate() keys. Use as a context manager or call close().")
        .def(py::init([](const std::string& path, const std::string& name, const VrsceneWriter::Options& options) {
                 return std::make_unique<VrsceneWriter>(path, name, options);
             }),
             py::arg("path"), py::arg("name"), py::arg("options") = VrsceneWriter::Options{})
        .def(
            "write_frame",
            [](VrsceneWriter& w, double frame, const StrandSet& strands, const ScalpMesh* scalp,
               const std::optional<std::vector<double>>& matrix) {
                if (matrix && matrix->size() != 16) throw py::value_error("matrix must have 16 values");
                py::gil_scoped_release release;
                w.writeFrame(frame, strands, scalp, matrix ? matrix->data() : nullptr);
            },
            py::arg("frame"), py::arg("strands"), py::arg("scalp") = nullptr, py::arg("matrix") = py::none(),
            "Frames must increase; the scalp supplies root UVs; matrix: the object-to-world matrix (16 values, "
            "row-major, the translation last: Maya's), None for the identity.")
        .def("frame_count", &VrsceneWriter::frameCount)
        .def("close", &VrsceneWriter::close, py::call_guard<py::gil_scoped_release>())
        .def("__enter__", [](VrsceneWriter& w) -> VrsceneWriter& { return w; }, py::return_value_policy::reference)
        .def("__exit__", [](VrsceneWriter& w, py::handle, py::handle, py::handle) { w.close(); });
}

}  // namespace groomlab::py_bind
