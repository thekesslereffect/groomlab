// SPDX-License-Identifier: Apache-2.0
// GroomSession (docs/SESSION.md): a groom kept evaluated from Python, as any host keeps one:
// inputs through setters that do nothing for the same input, views (strands as buffer views,
// the hierarchy, versions, bounds, errors), previews and stats. `Groom` is the same class.

#include "bind_common.h"

#include "groomlab/cards.h"
#include "groomlab/flow.h"
#include "groomlab/groom_file.h"
#include "groomlab/groom_session.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/hair_color.h"
#include "groomlab/parts.h"
#include "groomlab/paths.h"
#include "groomlab/simulation.h"

namespace groomlab::py_bind {

namespace {

template <class T>
std::shared_ptr<T> mutableCopyless(const std::shared_ptr<const T>& p) {
    return std::const_pointer_cast<T>(p);
}

py::dict simStatusDict(const SimStatus& s) {
    py::dict d;
    d["node"] = s.node;
    d["start_frame"] = s.startFrame;
    py::list ranges;
    for (const auto& [first, last] : s.ranges) ranges.append(py::make_tuple(first, last));
    d["ranges"] = ranges;
    d["frames"] = s.frames;
    d["live"] = s.live;
    d["baked"] = s.baked;
    d["bake_stale"] = s.bakeStale;
    d["baked_first"] = s.bakedFirst;
    d["baked_last"] = s.bakedLast;
    d["baked_file"] = s.bakedFile;
    d["step_ms"] = s.stepMs;
    d["bytes"] = s.bytes;
    d["stamp"] = formatSimulationStamp(s.stamp);
    return d;
}

py::dict versionsDict(const ViewVersions& v) {
    py::dict d;
    d["evaluation"] = v.evaluation;
    d["strands"] = v.strands;
    d["hierarchy"] = v.hierarchy;
    d["cards"] = v.cards;
    return d;
}

GroomSession& sessionFromFile(GroomSession& s, const GroomFile& file) {
    std::vector<std::string> dirs;
    if (!file.directory.empty()) dirs.push_back(file.directory);
    dirs.insert(dirs.end(), file.searchPaths.begin(), file.searchPaths.end());
    s.setPathResolver(searchPathResolver(std::move(dirs)));
    // Its frame rate, as GroomScene and the C API set it: frames (a Simulate node's steps, the
    // scalp's history) are the file's, not 24 a second.
    if (file.framesPerSecond > 0.0) s.setFramesPerSecond(file.framesPerSecond);
    return s;
}

}  // namespace

void bindSession(py::module_& m) {
    py::class_<PyView>(m, "ViewResult",
                       "What GroomSession.view() made: the strands, the guides, parts, flow and cards it used, "
                       "versions that move only when something changed, bounds and errors.")
        .def_property_readonly("strands",
                               [](const PyView& v) -> py::object {
                                   if (!v.result.strands) return py::none();
                                   return py::cast(std::make_shared<StrandSet>(*v.result.strands));
                               },
                               "A StrandSet sharing the view's arrays (positions_view() & co. read them without a copy), or None.")
        .def_property_readonly("viewed_node", [](const PyView& v) { return v.result.viewedNode; })
        .def_property_readonly("viewed_roots", [](const PyView& v) { return v.result.viewedRoots; })
        .def_property_readonly(
            "clump_centers",
            [](const PyView& v) {
                std::vector<std::array<float, 3>> out;
                if (v.result.clumpCenters) {
                    for (const Vec3f& p : *v.result.clumpCenters) out.push_back({p.x, p.y, p.z});
                }
                return out;
            },
            "A viewed Clump's clump centers, each its root on the scalp: [x, y, z] each.")
        .def_property_readonly("scalp", [](const PyView& v) { return mutableCopyless(v.result.scalp); },
                               "The scalp evaluated (with the vertex maps as color sets). Do not mutate it.")
        .def_property_readonly("hierarchy",
                               [](const PyView& v) -> py::object {
                                   if (!v.result.hierarchy) return py::none();
                                   return py::cast(std::make_shared<GuideHierarchy>(*v.result.hierarchy));
                               },
                               "A copy of the active hierarchy (posed, corrected; the preview while set), or None.")
        .def_property_readonly("authored_hierarchy",
                               [](const PyView& v) -> py::object {
                                   if (!v.result.authoredHierarchy) return py::none();
                                   return py::cast(std::make_shared<GuideHierarchy>(*v.result.authoredHierarchy));
                               },
                               "A copy of the stored hierarchy rooted on the scalp, or None.")
        .def_property_readonly("cards", [](const PyView& v) { return mutableCopyless(v.result.cards); },
                               "want_cards: the Cards nodes' cards laid out in one atlas (a CardMesh), or None.")
        .def_property_readonly("card_strands",
                               [](const PyView& v) -> py::object {
                                   if (!v.result.cardStrands) return py::none();
                                   return py::cast(std::make_shared<StrandSet>(*v.result.cardStrands));
                               })
        .def_property_readonly("has_parts", [](const PyView& v) { return v.result.parts != nullptr; })
        .def_property_readonly("has_flow", [](const PyView& v) { return v.result.flow != nullptr; })
        .def_property_readonly("bounds",
                               [](const PyView& v) -> py::object {
                                   if (!v.result.bounds.valid) return py::none();
                                   return py::make_tuple(toTuple(v.result.bounds.lo), toTuple(v.result.bounds.hi));
                               },
                               "((x, y, z) low, (x, y, z) high) of the strands and active guides, or None.")
        .def_property_readonly("versions", [](const PyView& v) { return versionsDict(v.result.versions); },
                               "{evaluation, strands, hierarchy, cards}: each moves only when that changed.")
        .def_property_readonly("errors", [](const PyView& v) { return v.result.errors; })
        .def_property_readonly("messages",
                               [](const PyView& v) {
                                   py::list out;
                                   for (const SessionMessage& msg : v.result.messages) out.append(py::make_tuple(msg.text, msg.error));
                                   return out;
                               },
                               "[(text, is_error)] to say once (the session repeats nothing).");

    py::class_<GroomSession>(m, "GroomSession",
                             "A groom kept evaluated: hand it inputs through set_* (each does nothing for the same "
                             "input), then view() or evaluate(). The graph cache, rooted guides, bound parts and flow, "
                             "the rest pose and the cards carry over, so only what changed re-evaluates. "
                             "Groom is the same class.")
        .def(py::init<>(), "The default graph and no inputs.")
        .def(py::init<GroomData>(), py::arg("data"), "Raises GraphError for a bad graph, RuntimeError for bad hierarchy data.")
        .def_static(
            "from_groom_file",
            [](const std::string& path) {
                const GroomFile file = readGroomFile(path);
                auto s = std::make_unique<GroomSession>(file.data);
                sessionFromFile(*s, file);
                return s;
            },
            py::arg("path"),
            "A session of a .groom file's data at the file's frame rate, relative paths resolving as the file's (its "
            "folder, then its search paths). Its caches (scalp, guides, simulation bakes) are not opened: GroomScene "
            "does that. Raises GroomFileError, GraphError.")
        // ---- Stored inputs
        .def("set_graph_json", [](GroomSession& s, py::handle graph) { s.setGraphJson(jsonText(graph)); }, py::arg("graph"),
             "The graph (str or dict). Raises GraphError, keeping the old graph.")
        .def("set_hierarchy_json", &GroomSession::setHierarchyJson, py::arg("json"), "Guides as stored (GuideHierarchy.to_json()).")
        .def("set_hierarchy",
             [](GroomSession& s, std::shared_ptr<GuideHierarchy> h) {
                 s.setHierarchy(h ? std::make_shared<const GuideHierarchy>(*h) : nullptr);
             },
             py::arg("hierarchy"), "Guides as stored (a copy is taken; set it again after editing).")
        .def("set_parts_json", &GroomSession::setPartsJson, py::arg("json"))
        .def("set_flow_json", &GroomSession::setFlowJson, py::arg("json"))
        .def("set_correctives_json", &GroomSession::setCorrectivesJson, py::arg("json"))
        .def("set_sculpts_json", &GroomSession::setSculptsJson, py::arg("json"),
             "The Sculpt nodes' edits ({node: {'guides': {id: [t, ax, ay, scale, ...]}}}; '' none).")
        .def_property("sculpt_target", &GroomSession::sculptTarget, &GroomSession::setSculptTarget,
                      "The Sculpt node brushes shape ('' none): the view's hierarchy is its output meanwhile.")
        .def(
            "sculpt_input",
            [](GroomSession& s, const std::string& node) {
                const SculptInput in = s.sculptInput(node);
                py::dict d;
                d["guides"] = in.guides ? py::cast(std::make_shared<GuideSet>(*in.guides)) : py::none();
                d["scalp"] = in.scalp ? py::cast(std::make_shared<ScalpMesh>(*in.scalp)) : py::none();
                d["strength"] = in.strength;
                return d;
            },
            py::arg("node"), "What Sculpt node `node` is fed: {'guides', 'scalp', 'strength'}. Raises RuntimeError.")
        .def("set_corrective_weights", &GroomSession::setCorrectiveWeights, py::arg("weights"),
             "{slot: weight}: layers with a slot take these instead of their envelope.")
        .def("clear_corrective_weights", &GroomSession::clearCorrectiveWeights)
        .def_property_readonly("input_errors", &GroomSession::inputErrors, "Stored inputs that did not parse: '<label>: <why>'.")
        .def_property_readonly("graph_error", &GroomSession::graphError)
        // ---- Scalp and maps
        .def("set_scalp", [](GroomSession& s, std::shared_ptr<ScalpMesh> mesh) { s.setScalp(std::move(mesh)); }, py::arg("scalp"),
             "The scalp (world space; may carry a rest pose and color sets). Do not mutate it afterwards.")
        .def("set_scalp_fingerprint", &GroomSession::setScalpFingerprint, py::arg("fingerprint"),
             "The topology the groom was made on (ScalpMesh.topology_fingerprint(); 0: not known): while the scalp set "
             "has another, evaluations say so in their errors.")
        .def(
            "set_scalp_mesh",
            [](GroomSession& s, py::handle points, py::handle triangles, py::handle uvs, py::handle restPoints) {
                auto mesh = std::make_shared<ScalpMesh>(toPoints(points), toTriangles(triangles));
                if (!uvs.is_none()) mesh->setCornerUVs(toFloatTuples<2>(uvs));
                if (!restPoints.is_none()) mesh->setRestPoints(toPoints(restPoints));
                s.setScalp(std::move(mesh));
            },
            py::arg("points"), py::arg("triangles"), py::arg("uvs") = py::none(), py::arg("rest_points") = py::none(),
            "The scalp from buffers (numpy arrays, array.array, memoryviews or sequences): points (n, 3) float, "
            "triangles (m, 3) int, optional corner UVs (3m, 2) and rest points (n, 3).")
        .def(
            "update_scalp_points",
            [](GroomSession& s, py::handle points, py::handle restPoints) {
                if (restPoints.is_none()) {
                    s.updateScalpPoints(toPoints(points));
                } else {
                    const std::vector<Vec3f> rest = toPoints(restPoints);
                    s.updateScalpPoints(toPoints(points), &rest);
                }
            },
            py::arg("points"), py::arg("rest_points") = py::none(),
            "The scalp's points at another time, on the same topology (its UVs and colors kept, its closest-point tree "
            "refitted); rest_points replace the rest pose. Raises ValueError for another count.")
        .def_property_readonly("scalp", [](GroomSession& s) { return mutableCopyless(s.scalp()); },
                               "The scalp as evaluated (with the vertex maps, refined by scalp_subdivision). Do not mutate it.")
        .def_property_readonly("cage_scalp", [](GroomSession& s) { return mutableCopyless(s.cageScalp()); },
                               "The scalp with its vertex maps before it is refined (what vertex maps are painted on).")
        .def_property_readonly("scalp_subdivision", &GroomSession::scalpSubdivision,
                               "Catmull-Clark levels the scalp is refined by (the Scalp node's subdivision, match render).")
        .def_property_readonly("scalp_subdivisions", &GroomSession::scalpSubdivisions,
                               "Each scalp's own levels by name ('scalp' the groom's own): several scalps refine each at its own.")
        .def("set_scalp_maps_json", [](GroomSession& s, py::handle maps) { s.setScalpMapsJson(maps.is_none() ? std::string() : jsonText(maps)); },
             py::arg("maps"), "Painted maps (str or dict; scalp_maps.h): vertex maps laid on the scalp, the map catalog.")
        .def_property_readonly("scalp_maps_json", [](const GroomSession& s) { return s.scalpMaps().toJson(); })
        .def_property_readonly("scalp_maps_error", &GroomSession::scalpMapsError)
        // ---- Moving inputs
        .def("set_guides", [](GroomSession& s, std::shared_ptr<GuideSet> g) { s.setGuides(std::move(g)); }, py::arg("guides"))
        .def("set_drivers", [](GroomSession& s, std::shared_ptr<GuideSet> g) { s.setDrivers(std::move(g)); }, py::arg("drivers"),
             "Driver curves for DriveGuides nodes as they are now (world space, stable IDs; None: none).")
        .def_property_readonly("drivers", [](const GroomSession& s) {
            return s.drivers() ? py::cast(std::make_shared<GuideSet>(*s.drivers())) : py::none();
        })
        .def("set_driver_bind_json", &GroomSession::setDriverBindJson, py::arg("json"),
             "The driver curves as bound (bind_drivers()'s text; '' not bound).")
        .def_property_readonly("driver_bind_json", &GroomSession::driverBindJson)
        .def("bind_drivers", &GroomSession::bindDrivers, py::arg("frame") = 0,
             "The drivers as they are now on the scalp now, as a bind's text (call it at the bind frame, then "
             "set_driver_bind_json it and keep it with the groom; '' when there are no drivers).")
        .def("set_guide_poses", [](GroomSession& s, std::shared_ptr<GuideSet> p) { s.setGuidePoses(std::move(p)); }, py::arg("poses"))
        .def("set_controls", &GroomSession::setControls, py::arg("controls"))
        .def("set_mesh", [](GroomSession& s, const std::string& name, std::shared_ptr<ScalpMesh> mesh) { s.setMesh(name, std::move(mesh)); },
             py::arg("name"), py::arg("mesh"), "A mesh for the ColliderMesh node of that name (None removes it).")
        .def("set_search_paths",
             [](GroomSession& s, std::vector<std::string> dirs) {
                 s.setPathResolver(dirs.empty() ? PathResolver{} : searchPathResolver(std::move(dirs)));
             },
             py::arg("directories"), "Where relative file paths (maps, caches) resolve, in order.")
        .def("set_card_maps", &GroomSession::setCardMaps, py::arg("card_maps"), "The last bake ('Atlas=<path>;Strands=<stamp>;...').")
        .def("set_cache_intermediates", &GroomSession::setCacheIntermediates, py::arg("enabled"), "Off for one-shot render evaluation.")
        .def_property("follow_scalp", &GroomSession::followScalp, &GroomSession::setFollowScalp,
                      "Follow the scalp: evaluate on its rest pose, then carry each strand to the scalp by its root's rigid "
                      "frame (no effect without a rest pose or at it). Off by default.")
        .def_property_readonly("following", &GroomSession::following,
                               "The last view followed the scalp (follow_scalp on and the scalp away from its rest pose).")
        .def_property("scale_with_scalp", &GroomSession::scaleWithScalp, &GroomSession::setScaleWithScalp,
                      "Scale the groom with its scalp: evaluated groom_scale times its size, sqrt(scalp area / "
                      "scale_reference), the area on the scalp's rest pose. On by default.")
        .def_property(
            "scale_source", [](const GroomSession& s) { return std::string(scaleSourceName(s.scaleSource())); },
            [](GroomSession& s, const std::string& name) { s.setScaleSource(scaleSourceFromName(name)); },
            "What groom_scale follows: \"area\" (the scalp's surface area: survives Freeze Transformations), "
            "\"transform\" (the scale set_scale_transform hands in: survives remodelling) or \"off\".")
        .def_property("scale_transform", &GroomSession::scaleTransform, &GroomSession::setScaleTransform,
                      "The scalp's transform scale as the host measures it (scale_source \"transform\"); 0: none.")
        .def_property("scale_transform_reference", &GroomSession::scaleTransformReference,
                      &GroomSession::setScaleTransformReference,
                      "The transform scale the groom is 1x at (set 0: the first one handed in).")
        .def_property_readonly("scale_notice", &GroomSession::scaleNotice,
                               "The last line saying the groom scaled because the scalp's area changed (\"\": none).")
        .def_property_readonly("scale_notice_serial", &GroomSession::scaleNoticeSerial,
                               "How many such notices there were (a host says each once).")
        .def_property("scale_reference", &GroomSession::scaleReference, &GroomSession::setScaleReference,
                      "The scalp area the groom is 1x at (set 0: the first area measured; reads the one in use).")
        .def_property_readonly("scale_area", &GroomSession::scaleArea, "The scalp's area as last measured.")
        .def_property_readonly("groom_scale", &GroomSession::groomScale,
                               "What the groom is scaled by now (1 while scale_with_scalp is off).")
        .def_property_readonly("reads_time", &GroomSession::readsTime,
                               "The result depends on the time through what the session knows (Wind, ImportCurves, Jiggle, "
                               "fading correctives, a scalp history); a host adds what it knows moves (a deforming scalp, "
                               "guide caches, keyed controls).")
        .def_property_readonly("data", &GroomSession::data, py::return_value_policy::reference_internal)
        // ---- Previews
        .def("set_preview_hierarchy",
             [](GroomSession& s, std::shared_ptr<GuideHierarchy> h, bool final) {
                 s.setPreviewHierarchy(h ? std::make_shared<GuideHierarchy>(*h) : nullptr, final);
             },
             py::arg("hierarchy"), py::arg("final") = false,
             "A brush stroke's working guides in place of the stored ones (None clears); final: poses and correctives already applied.")
        .def(
            "set_preview_vertex_map",
            [](GroomSession& s, const std::string& name, py::handle values, int channels) {
                s.setPreviewVertexMap(name, values.is_none() ? nullptr : std::make_shared<const std::vector<float>>(toFloats(values)), channels);
            },
            py::arg("name"), py::arg("values"), py::arg("channels") = 1, "A vertex map's live values in place of the stored ones (None clears).")
        .def("set_live_stroke", &GroomSession::setLiveStroke, py::arg("on"),
             "A stroke is live (sculpt density). Off: True when a view took the stroke's density (view again).")
        .def_property_readonly("has_preview", &GroomSession::hasPreview)
        // ---- Evaluation
        .def(
            "view",
            [](GroomSession& s, float density, double time, double pointBudget, const std::string& viewNode, float sculptDensity,
               bool buildCards, bool wantCards, float cardDensity, bool gameExport) {
                ViewRequest request;
                request.density = density;
                request.time = time;
                request.pointBudget = pointBudget;
                request.viewNode = viewNode;
                request.sculptDensity = sculptDensity;
                request.buildCards = buildCards;
                request.wantCards = wantCards;
                request.cardDensity = cardDensity;
                request.gameExport = gameExport;
                PyView out;
                {
                    py::gil_scoped_release release;
                    out.result = s.evaluate(request);
                }
                return out;
            },
            py::arg("density") = 1.0f, py::arg("time") = 0.0, py::arg("point_budget") = 0.0, py::arg("view_node") = std::string{},
            py::arg("sculpt_density") = 1.0f, py::arg("build_cards") = false, py::arg("want_cards") = false,
            py::arg("card_density") = 1.0f, py::arg("game_export") = false,
            "Evaluates and returns a ViewResult (game_export: for a game engine, the Output's rootDepth left off). Never raises for the graph: failures are in errors.")
        .def(
            "evaluate",
            [](GroomSession& g, float density, double seconds, bool buildCards, bool gameExport) -> py::object {
                StrandsPtr strands;
                {
                    py::gil_scoped_release release;
                    strands = g.evaluate(density, seconds, buildCards, gameExport);
                }
                if (!strands) return py::none();
                return py::cast(std::make_shared<StrandSet>(*strands));
            },
            py::arg("density") = 1.0f, py::arg("seconds") = 0.0, py::arg("build_cards") = false, py::arg("game_export") = false,
            "The strands at a density and time (a copy-on-write copy). Raises GraphError when the graph fails.")
        .def(
            "evaluate_node",
            [](GroomSession& s, const std::string& name, float density, double seconds) {
                Value v;
                {
                    py::gil_scoped_release release;
                    v = s.evaluateNode(name, density, seconds);
                }
                return castValue(v);
            },
            py::arg("name"), py::arg("density") = 1.0f, py::arg("seconds") = 0.0,
            "One node by name ('' the output) with the last view's inputs, in a scratch copy of the graph.")
        .def(
            "hierarchy",
            [](GroomSession& g) -> py::object {
                HierarchyPtr h;
                {
                    py::gil_scoped_release release;
                    h = g.hierarchy();
                }
                if (!h) return py::none();
                return py::cast(GuideHierarchy(*h));
            },
            "A copy of the hierarchy on the current scalp with poses and correctives applied, or None.")
        .def("stats", [](const GroomSession& s) { return jsonLoads(s.stats().toJson()); },
             "What the last view did, as groomLabGraphStats reports it (dict).")
        .def("scalp_history_frames", &GroomSession::scalpHistoryFrames, py::arg("view_node") = std::string{},
             py::arg("viewport") = -1,
             "Earlier scalp frames the graph reads (Jiggle): hand them over with a host's scalp history. viewport 1: "
             "for the viewport (a renderOnly Jiggle off there), 0: a render or export (a viewportOnly one off), -1: either.")
        .def_property_readonly("has_cards_node", &GroomSession::hasCardsNode)
        .def("hair_look",
             [](const GroomSession& s) {
                 const HairLook look = s.hairLook();
                 py::dict d;
                 d["specular"] = look.specular;
                 d["roughness"] = look.roughness;
                 d["tilt"] = look.tilt;
                 d["node"] = look.node;
                 return d;
             })
        .def("node_times", &GroomSession::nodeTimes, "[(node name, milliseconds)] of the last run of each node.")
        .def("node_notes", &GroomSession::nodeNotes, "[(node name, note)] from the last evaluation.")
        // ---- Simulation (simulation.h)
        .def_property("frames_per_second", &GroomSession::framesPerSecond, &GroomSession::setFramesPerSecond,
                      "The rate Simulate nodes step at: frame f is at frame_to_seconds(f, fps) (24 by default; an NTSC rate "
                      "made exact).")
        .def(
            "set_simulation_feed",
            [](GroomSession& s, py::object feed) {
                if (feed.is_none()) {
                    s.setSimulationFeed({});
                    return;
                }
                auto fn = std::make_shared<py::object>(std::move(feed));
                s.setSimulationFeed([fn](double seconds) {
                    py::gil_scoped_acquire gil;
                    const py::object fed = (*fn)(seconds);
                    return fed.is_none() || fed.cast<bool>();  // False: it cannot give that time now
                });
            },
            py::arg("feed"),
            "feed(seconds) sets the moving inputs (scalp points, guide poses, colliders) at another time through the "
            "set_* methods, as before a view: Simulate nodes step over frames with it. It returns False when it cannot give "
            "that time now (having set nothing). None: frames on the way are stepped toward the inputs as they are.")
        .def("simulate_nodes", &GroomSession::simulateNodes, "The Simulate nodes making the output, upstream first.")
        .def(
            "simulate_to",
            [](GroomSession& s, int frame, py::object progress) {
                SimProgress report;
                std::shared_ptr<py::object> fn;
                if (!progress.is_none()) {
                    fn = std::make_shared<py::object>(std::move(progress));
                    report = [fn](int done, int count) {
                        py::gil_scoped_acquire gil;
                        const py::object keep = (*fn)(done, count);
                        return keep.is_none() || keep.cast<bool>();
                    };
                }
                SimRun run;
                {
                    py::gil_scoped_release release;
                    run = s.simulateTo(frame, report);
                }
                py::dict d;
                d["frames"] = run.frames;
                d["cancelled"] = run.cancelled;
                d["milliseconds"] = run.milliseconds;
                d["errors"] = run.errors;
                return d;
            },
            py::arg("frame"), py::arg("progress") = py::none(),
            "Simulate Range: every Simulate node stepped on to `frame` from its last frame (the feed giving the frames "
            "on the way). progress(done, count) returning False stops. {frames, cancelled, milliseconds, errors}.")
        .def("invalidate_simulation", &GroomSession::invalidateSimulation, py::arg("node") = std::string{},
             "Drops a Simulate node's frames ('': every node's); a bake stays.")
        .def(
            "simulation_status",
            [](const GroomSession& s) {
                py::list out;
                for (const SimStatus& status : s.simulationStatus()) out.append(simStatusDict(status));
                return out;
            },
            "Each Simulate node: start_frame, ranges [(first, last)] cached, frames, live, baked, bake_stale, "
            "baked_first, baked_last, baked_file, step_ms, bytes, stamp.")
        .def(
            "simulated_frame",
            [](const GroomSession& s, const std::string& node, int frame) -> py::object {
                const GuidesPtr guides = s.simulatedFrame(node, frame);
                if (!guides) return py::none();
                return py::cast(std::make_shared<GuideSet>(*guides));
            },
            py::arg("node"), py::arg("frame"), "A copy of a node's simulated guides at a frame, or None when not cached.")
        .def("simulation_stamp", [](const GroomSession& s, const std::string& node) { return formatSimulationStamp(s.simulationStamp(node)); },
             py::arg("node"), "The settings stamp a bake of the node records now (16 hex digits).")
        .def("clear_baked_simulation", [](GroomSession& s, const std::string& node) { s.setBakedSimulation(node, nullptr); },
             py::arg("node"))
        .def_property_readonly("evaluation_count", &GroomSession::evaluationCount,
                               "Operator evaluations of the current graph since it was set.")
        .def_property_readonly("rest_evaluations", &GroomSession::restEvaluations,
                               "How many times the rest pose was evaluated (Instance nodes pinned or fitted to rest lengths).");
    m.attr("Groom") = m.attr("GroomSession");

    // The Simulate node's one-click settings (simulation.h simulatePresets), every host's.
    m.def(
        "simulate_presets",
        [] {
            py::list out;
            for (const SimPreset& preset : simulatePresets()) {
                py::dict d;
                d["name"] = preset.name;
                d["title"] = preset.title;
                d["description"] = preset.description;
                out.append(d);
            }
            return out;
        },
        "The Simulate node's presets: [{name, title, description}] (default, fur).");
    m.def(
        "simulate_preset_params",
        [](const std::string& name) {
            try {
                return simulatePresetParamsJson(name);
            } catch (const std::invalid_argument& e) {
                throw py::value_error(e.what());
            }
        },
        py::arg("name"), "The params a Simulate preset sets, as a node's params JSON (text); ValueError for another name.");

    // The look's lobes (hair_color.h hairLobes): what create_hair_shader gives aiStandardHair.
    m.def(
        "hair_lobes",
        [](float specular, float roughness, float tilt) {
            const HairLobes lobes = hairLobes(specular, roughness, tilt);
            py::dict d;
            d["specular_tint"] = lobes.specularTint;
            d["roughness"] = lobes.roughness;
            d["azimuthal_roughness"] = lobes.azimuthalRoughness;
            d["ior"] = lobes.ior;
            d["shift"] = lobes.shiftDegrees;
            return d;
        },
        py::arg("specular"), py::arg("roughness"), py::arg("tilt") = kDefaultHairTilt,
        "aiStandardHair's lobes for HairColor's specular, roughness and tilt: {specular_tint (specular / "
        "HAIR_FULL_SPECULAR, up to 1), roughness (HAIR_ROUGHNESS_MIN + HAIR_ROUGHNESS_RANGE x roughness), "
        "azimuthal_roughness, ior, shift (the tilt, degrees)}.");
    m.attr("HAIR_FULL_SPECULAR") = kHairFullSpecular;
    m.attr("HAIR_ROUGHNESS_MIN") = kHairRoughnessMin;
    m.attr("HAIR_ROUGHNESS_RANGE") = kHairRoughnessRange;
    // HairColor's highlight defaults: its params' and the strands' without the channels.
    m.attr("DEFAULT_HAIR_SPECULAR") = kDefaultHairSpecular;
    m.attr("DEFAULT_HAIR_ROUGHNESS") = kDefaultHairRoughness;
    m.attr("DEFAULT_HAIR_TILT") = kDefaultHairTilt;
    // Hair colors (hair_color.h): a melanin's, HairColor's per-strand variation, the default.
    m.def(
        "melanin_color",
        [](float melanin, float redness, std::array<float, 3> dye) { return melaninColor(melanin, redness, dye); },
        py::arg("melanin"), py::arg("redness"), py::arg("dye") = std::array<float, 3>{1.0f, 1.0f, 1.0f},
        "The hair color (linear RGB, aiStandardHair's base-color sense) of `melanin` and `redness` times `dye`: "
        "HairColor's color in mode melanin, before its randomness.");
    m.def("vary_hair_color", &varyHairColor, py::arg("rgb"), py::arg("hue_turn"), py::arg("saturation_scale"),
          py::arg("value_scale"),
          "A color varied as HairColor varies a strand's: the hue turned (turns) and the saturation scaled in HSL, "
          "then all of it scaled by value_scale; clamped to 0-1.");
    m.def("hair_appearance_color", &hairAppearanceColor, py::arg("base_color"),
          "What a hair base color (HairColor's RGB, what a hair shader takes) looks like: the matte color it "
          "matches under even white light (USD displayColor, Alembic Cd, Unreal's groom_color, the cards' maps).");
    m.def("hair_base_of_appearance", &hairBaseOfAppearance, py::arg("appearance"),
          "The hair base color that looks like `appearance`: hair_appearance_color's inverse.");
    m.attr("DEFAULT_HAIR_COLOR") = defaultHairColor();
    m.attr("ARNOLD_BASE_COLOR_GAMMA") = kArnoldBaseColorGamma;
    // Wet hair (hair_color.h wetShading): what create_hair_shader's wet network builds for Arnold.
    m.def(
        "wet_shading",
        [](float wetness) {
            const WetShading wet = wetShading(wetness);
            py::dict d;
            d["absorption"] = wet.absorption;
            d["specular"] = wet.specular;
            d["roughness"] = wet.roughness;
            return d;
        },
        py::arg("wetness"),
        "Wet hair at `wetness` (0 dry .. 1 soaked): {absorption (the fiber's absorption times it: its color to "
        "this power, melanin times its square root), specular (the R lobe's tint times it), roughness (Arnold's "
        "roughness times it)}.");
    m.attr("WET_ABSORPTION") = kWetAbsorption;
    m.attr("WET_SPECULAR") = kWetSpecular;
    m.attr("WET_ROUGHNESS") = kWetRoughness;
}

}  // namespace groomlab::py_bind
