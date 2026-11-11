// SPDX-License-Identifier: Apache-2.0
// Alembic I/O, when the module is built with groomlab_alembic: curve writers
// (plain and Unreal groom), readers, the animated scalp reader, GroomScene
// and the ImportCurves operator. Without it, `has_alembic` is False.

#include "bind_common.h"

#if GROOMLAB_PY_HAS_ALEMBIC
#include "groomlab/alembic_io.h"
#include "groomlab/alembic_operators.h"
#include "groomlab/engine_export.h"
#include "groomlab/groom_file.h"
#include "groomlab/groom_scene.h"
#include "groomlab/sim_cache.h"
#endif

namespace groomlab::py_bind {

void bindAlembic(py::module_& m) {
#if GROOMLAB_PY_HAS_ALEMBIC
    m.attr("has_alembic") = true;
    registerAlembicOperators(OperatorRegistry::instance());
    py::register_exception<AlembicError>(m, "AlembicError", PyExc_RuntimeError);

    py::class_<AlembicCurvesWriter>(m, "AlembicCurvesWriter",
                                    "Writes strands as animated Alembic curves: per-point width, per-curve strandId. "
                                    "Frames are uniformly spaced; use as a context manager or call close().")
        .def(py::init([](const std::string& path, const std::string& name, double start, double step) {
                 return std::make_unique<AlembicCurvesWriter>(path, name, start, step);
             }),
             py::arg("path"), py::arg("name"), py::arg("start_seconds") = 0.0, py::arg("seconds_per_frame") = 1.0 / 24.0)
        .def("write_frame", &AlembicCurvesWriter::writeFrame, py::arg("strands"), py::arg("scalp") = nullptr, py::arg("scales") = nullptr,
             py::call_guard<py::gil_scoped_release>(), "Appends a frame; the scalp supplies root UVs when it has them.")
        .def("frame_count", &AlembicCurvesWriter::frameCount)
        .def("close", &AlembicCurvesWriter::close, py::call_guard<py::gil_scoped_release>())
        .def("__enter__", [](AlembicCurvesWriter& w) -> AlembicCurvesWriter& { return w; }, py::return_value_policy::reference)
        .def("__exit__", [](AlembicCurvesWriter& w, py::handle, py::handle, py::handle) { w.close(); });

    py::class_<UnrealGroomWriter>(m, "UnrealGroomWriter",
                                  "Writes a groom in Unreal Engine's Alembic groom conventions: strands, optional guides, "
                                  "groom_id, groom_width and groom_root_uv (with scalp UVs). The points and widths go in "
                                  "Unreal's space (Z up, left-handed, centimeters) from the groom's: `up_axis` \"Y\" "
                                  "(x, y, z) -> (x, z, y), \"Z\" -> (x, -y, z), times `centimeters_per_unit`, so the "
                                  "importer's default conversion lines it up with an FBX of the same scene.")
        .def(py::init([](const std::string& path, const std::string& name, const std::string& group, double start, double step,
                         const std::string& upAxis, double centimetersPerUnit) {
                 auto w = std::make_unique<UnrealGroomWriter>(path, name, group, start, step);
                 w->setSourceSpace(upAxis, centimetersPerUnit);
                 return w;
             }),
             py::arg("path"), py::arg("name"), py::arg("group_name"), py::arg("start_seconds") = 0.0,
             py::arg("seconds_per_frame") = 1.0 / 24.0, py::arg("up_axis") = "Y", py::arg("centimeters_per_unit") = 1.0)
        .def("write_frame", &UnrealGroomWriter::writeFrame, py::arg("strands"), py::arg("guides") = nullptr, py::arg("scalp") = nullptr,
             py::call_guard<py::gil_scoped_release>())
        .def("frame_count", &UnrealGroomWriter::frameCount)
        .def("close", &UnrealGroomWriter::close, py::call_guard<py::gil_scoped_release>())
        .def("__enter__", [](UnrealGroomWriter& w) -> UnrealGroomWriter& { return w; }, py::return_value_policy::reference)
        .def("__exit__", [](UnrealGroomWriter& w, py::handle, py::handle, py::handle) { w.close(); });

    py::class_<AlembicCurvesFrame>(m, "AlembicCurvesFrame", "One sample of an Alembic curves object: time and strands.")
        .def_readonly("seconds", &AlembicCurvesFrame::seconds)
        .def_readonly("strands", &AlembicCurvesFrame::strands);
    m.def(
        "barb_lod",
        [](const std::string& lod, std::optional<double> density, std::optional<double> widthScale) {
            EngineExportOptions e;
            setBarbLod(e, lod, density, widthScale);
            return std::make_pair(static_cast<double>(e.barbDensity), static_cast<double>(e.barbWidthScale));
        },
        py::arg("lod") = std::string(), py::arg("density") = py::none(), py::arg("width_scale") = py::none(),
        "(barb density, barb width scale) of a lighter feather export, every host's (engine_export.h setBarbLod); "
        "ValueError for another lod or a share outside (0, 1].");
    m.def("read_alembic_curves",
          [](const std::string& path, const std::string& name) {
              py::gil_scoped_release release;
              return readAlembicCurves(path, name);
          },
          py::arg("path"), py::arg("name") = std::string{},
          "Every sample of the curves object `name`Shape (or the first curves object). Raises AlembicError.");
    m.def("read_alembic_curves_at",
          [](const std::string& path, double seconds, const std::string& objectName) {
              py::gil_scoped_release release;
              return readAlembicCurvesAt(path, seconds, objectName);
          },
          py::arg("path"), py::arg("seconds") = 0.0, py::arg("object_name") = std::string{},
          "All curves in the archive at one time, world space, as guides. Raises AlembicError.");

    py::class_<AlembicMeshReader>(m, "AlembicMeshReader", "An animated polygon mesh's world-space points from an Alembic cache.")
        .def(py::init<const std::string&, const std::string&>(), py::arg("path"), py::arg("object_name") = std::string{})
        .def_property_readonly("mesh_name", &AlembicMeshReader::meshName)
        .def("points_at", &AlembicMeshReader::pointsAt, py::arg("seconds"), py::call_guard<py::gil_scoped_release>(),
             "Vertex positions at a time (linear between samples).");

    py::class_<AlembicMeshWriter>(m, "AlembicMeshWriter",
                                  "Writes an animated polygon mesh (a scalp cache a groom file references): the first "
                                  "frame's faces and UVs, each frame's points. Use as a context manager or call close().")
        .def(py::init<const std::string&, const std::string&, double, double>(), py::arg("path"), py::arg("name"),
             py::arg("start_seconds") = 0.0, py::arg("seconds_per_frame") = 1.0 / 24.0)
        .def("write_frame", &AlembicMeshWriter::writeFrame, py::arg("mesh"), py::call_guard<py::gil_scoped_release>(),
             "Appends a frame (the first frame's point count; raises AlembicError otherwise).")
        .def("frame_count", &AlembicMeshWriter::frameCount)
        .def("close", &AlembicMeshWriter::close, py::call_guard<py::gil_scoped_release>())
        .def("__enter__", [](AlembicMeshWriter& w) -> AlembicMeshWriter& { return w; }, py::return_value_policy::reference)
        .def("__exit__", [](AlembicMeshWriter& w, py::handle, py::handle, py::handle) { w.close(); });

    py::class_<GroomScene>(m, "GroomScene",
                           "A groom file ready to evaluate at any time: its scalp and guide caches are opened once and "
                           "the graph cache carries across frames.")
        .def(py::init<const std::string&>(), py::arg("groom_path"), "Raises GroomFileError, AlembicError or GraphError.")
        .def(py::init<GroomFile>(), py::arg("file"))
        .def_property_readonly("file", &GroomScene::file, py::return_value_policy::reference_internal)
        .def_property_readonly("frames_per_second", &GroomScene::framesPerSecond)
        .def("scalp_at",
             [](GroomScene& s, double seconds) -> std::shared_ptr<ScalpMesh> {
                 MeshPtr mesh;
                 {
                     py::gil_scoped_release release;
                     mesh = s.rootScalpAt(seconds);
                 }
                 return std::const_pointer_cast<ScalpMesh>(mesh);
             },
             py::arg("seconds"), "The scalp at a time (from the cache when there is one), joined with the groom's other scalps (what strands' roots index).")
        .def("evaluate",
             [](GroomScene& s, double seconds, float density) -> py::object {
                 StrandsPtr strands;
                 {
                     py::gil_scoped_release release;
                     strands = s.evaluate(seconds, density);
                 }
                 if (!strands) return py::none();
                 return py::cast(std::make_shared<StrandSet>(*strands));
             },
             py::arg("seconds") = 0.0, py::arg("density") = 1.0f, "Strands at a time in seconds.")
        .def_property_readonly("session", [](GroomScene& s) -> GroomSession& { return s.session(); },
                               py::return_value_policy::reference_internal,
                               "The session the groom evaluates in (simulate_to, simulation_status...).")
        .def("set_time", &GroomScene::setTime, py::arg("seconds"), py::call_guard<py::gil_scoped_release>(),
             "The time's inputs (cached scalp, guide poses) into the session without evaluating.")
        .def("node_times", &GroomScene::nodeTimes)
        .def("node_notes", &GroomScene::nodeNotes,
             "[(node, note)] from the last evaluation: what nodes have to say that is not an error (a Jiggle "
             "without an animated scalp cache passes its strands through).");

    m.def(
        "write_simulation_cache",
        [](const GroomSession& session, const std::string& node, const std::string& path, int first, int last) {
            py::gil_scoped_release release;
            return writeSimulationCache(session, node, path, first, last);
        },
        py::arg("session"), py::arg("node"), py::arg("path"), py::arg("first"), py::arg("last"),
        "Bakes frames first..last of a Simulate node's cache as an Alembic guide cache (strandId: the guide's ID). "
        "Returns the frames written; raises RuntimeError for a frame not simulated.");
    m.def(
        "attach_simulation_cache",
        [](GroomSession& session, const std::string& node, const std::string& path, const std::string& object,
           const std::string& stamp) {
            session.setBakedSimulation(node, readSimulationCache(path, object, parseSimulationStamp(stamp)));
        },
        py::arg("session"), py::arg("node"), py::arg("path"), py::arg("object") = std::string{}, py::arg("stamp") = std::string{},
        "Reads a node's frames from a bake (while its settings are the stamp's; '' always). Raises AlembicError.");
#else
    m.attr("has_alembic") = false;
#endif
}

}  // namespace groomlab::py_bind
