// SPDX-License-Identifier: Apache-2.0
// The interactive tools without Maya (stroke.h): strokes on a GroomSession from screen points in
// a view (ToolView from a world-to-clip matrix) or from world points (as groomlab.brush_stroke),
// each committed as one StrokeEdit the host keeps for undo and redo.

#include "bind_common.h"

#include <cmath>
#include <sstream>

#include "groomlab/brush_catalog.h"
#include "groomlab/groom_session.h"
#include "groomlab/guide_selection.h"
#include "groomlab/scalp_paint.h"
#include "groomlab/stroke.h"
#include "groomlab/view_projection.h"

namespace groomlab::py_bind {

namespace {

std::array<double, 16> toMatrix(py::handle src) {
    std::vector<double> values;
    for (py::handle row : src) {
        if (py::isinstance<py::sequence>(row) && !py::isinstance<py::str>(row)) {
            for (py::handle v : row) values.push_back(v.cast<double>());
        } else {
            values.push_back(row.cast<double>());
        }
    }
    if (values.size() != 16) throw py::value_error("a matrix is 16 numbers (4 rows of 4, rows first)");
    std::array<double, 16> m{};
    std::copy(values.begin(), values.end(), m.begin());
    return m;
}

// A camera at `eye` looking at `target` (`up` up), vertical field of view `fov` degrees, as
// Maya's world-to-clip matrix: row vectors (clip = [x y z 1] * M), rows first, OpenGL clip space.
std::array<double, 16> lookAt(const Vec3f& eye, const Vec3f& target, const Vec3f& up, double fov, double aspect, double n,
                              double f) {
    const Vec3f fw = (target - eye).normalized();
    const Vec3f r = fw.cross(up).normalized();
    const Vec3f u = r.cross(fw);
    const double view[4][4] = {{r.x, r.y, r.z, -r.dot(eye)}, {u.x, u.y, u.z, -u.dot(eye)}, {-fw.x, -fw.y, -fw.z, fw.dot(eye)}, {0, 0, 0, 1}};
    const double t = 1.0 / std::tan(fov * 3.14159265358979323846 / 360.0);
    const double proj[4][4] = {{t / aspect, 0, 0, 0}, {0, t, 0, 0}, {0, 0, -(f + n) / (f - n), -2 * f * n / (f - n)}, {0, 0, -1, 0}};
    double pv[4][4] = {};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            for (int k = 0; k < 4; ++k) pv[i][j] += proj[i][k] * view[k][j];
        }
    }
    std::array<double, 16> m{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) m[static_cast<std::size_t>(row * 4 + col)] = pv[col][row];
    }
    return m;
}

PaintColor toColor(py::handle color) {
    std::vector<float> c;
    for (py::handle v : color) c.push_back(std::clamp(v.cast<float>(), 0.0f, 1.0f));
    if (c.size() == 3) c.push_back(1.0f);
    if (c.size() != 4) throw py::value_error("a color is (r, g, b) or (r, g, b, a)");
    return {c[0], c[1], c[2], c[3]};
}

// A brush curve as the settings take it (BrushToolSettings::setStrengthProfile): a ramp's JSON
// text, keys [(position, value), ...], or None (none).
std::string rampText(py::handle v) {
    if (v.is_none()) return {};
    if (py::isinstance<py::str>(v)) return v.cast<std::string>();
    std::ostringstream out;
    out << "[";
    bool first = true;
    for (py::handle key : v) {
        const py::sequence pair = py::reinterpret_borrow<py::sequence>(key);
        if (pair.size() != 2) throw py::value_error("a curve's keys are (position, value) pairs");
        out << (first ? "" : ",") << "[" << pair[0].cast<double>() << "," << pair[1].cast<double>() << "]";
        first = false;
    }
    out << "]";
    return out.str();
}

// Settings from keyword arguments named as groomlab.brush_stroke and groomlab.brush_tool name
// them (mode first, then the rest). Raises TypeError for an unknown one.
void applySettings(BrushToolSettings& s, const py::dict& kwargs) {
    if (kwargs.contains("mode")) s.setMode(kwargs["mode"].cast<std::string>());
    for (const auto& item : kwargs) {
        const std::string key = item.first.cast<std::string>();
        const py::handle v = item.second;
        if (key == "mode") continue;
        if (key == "radius") s.brush.radius = v.cast<float>();
        else if (key == "screen_radius") s.screenRadius = v.cast<float>();
        else if (key == "strength") s.brush.strength = v.cast<float>();
        else if (key == "level") s.brush.level = v.cast<int>();
        else if (key == "group") s.brush.group = v.is_none() ? std::string() : v.cast<std::string>();
        else if (key == "propagate") s.brush.propagate = v.cast<bool>();
        else if (key == "length") s.place.length = v.cast<float>();
        else if (key == "segments") s.place.segments = v.cast<int>();
        else if (key == "tube_radius") s.place.tube.width = {v.cast<float>(), v.cast<float>()};
        else if (key == "spacing") s.place.spacing = v.cast<float>();
        else if (key == "match_neighbors") s.place.matchNeighbors = v.cast<bool>();
        else if (key == "solo_new_guides") s.place.soloNewGuides = v.cast<bool>();
        else if (key == "tube") {  // the placement tube: a dict as groomlab.get_tube gives one, or its JSON
            if (!v.is_none()) {
                setPlacementTube(s.place, py::isinstance<py::str>(v) ? v.cast<std::string>()
                                                                    : py::module_::import("json").attr("dumps")(v).cast<std::string>());
            }
        }
        else if (key == "selected_only") s.selectedOnly = v.cast<bool>();
        else if (key == "symmetry") s.setSymmetry(v.is_none() ? std::string() : v.cast<std::string>());
        else if (key == "symmetry_center") s.brush.mirrorCenter = v.cast<float>();
        else if (key == "attribute") {
            if (!v.is_none()) s.brush.attribute = v.cast<std::string>();
        } else if (key == "value") s.brush.value = v.cast<float>();
        else if (key == "collide") s.brush.collide = v.cast<bool>();
        else if (key == "stretch") s.brush.stretch = v.cast<bool>();
        else if (key == "tube_channel") s.brush.tubeChannel = tubeChannelFromString(v.cast<std::string>());
        else if (key == "hardness") s.brush.hardness = std::clamp(v.cast<float>(), 0.0f, 1.0f);
        else if (key == "falloff_profile") s.setFalloffProfile(rampText(v));
        else if (key == "strength_profile") s.setStrengthProfile(rampText(v));
        else if (key == "part_name") s.partName = v.cast<std::string>();
        else if (key == "flow_sink") {
            const std::string sink = v.is_none() ? std::string() : v.cast<std::string>();
            if (!sink.empty()) flowSinkAngle(sink);  // throws for an unknown kind
            s.flowSink = sink;
        } else if (key == "targets") s.setTargets(v.cast<std::string>());
        else if (key == "paint_kind") {
            const std::string kind = v.cast<std::string>();
            if (kind != "guides" && kind != "vertices" && kind != "texture") throw py::value_error("paint_kind is guides, vertices or texture");
            s.paintKind = kind;
        } else if (key == "paint_texture") s.paintTexture = v.is_none() ? std::string() : v.cast<std::string>();
        else if (key == "paint_resolution") s.paintResolution = v.cast<int>();
        else if (key == "smooth_strength") s.smoothStrength = std::max(0.0f, v.cast<float>());
        else if (key == "paint_color") {
            if (!v.is_none()) s.paintColor = toColor(v);
        } else if (key == "paint_channels") s.paintChannels = channelMask(v.is_none() ? std::string("rgba") : v.cast<std::string>());
        else if (key == "paint_mode") s.brush.paintMode = paintModeFromString(v.cast<std::string>());
        else if (key == "pressure") s.pressure = v.cast<bool>();
        else if (key == "mask_by_scalp") s.maskByScalp = v.cast<bool>();
        else if (key == "erase_guides") s.eraseGuides = v.cast<bool>();
        else if (key == "erase_parts") s.eraseParts = v.cast<bool>();
        else if (key == "erase_flow") s.eraseFlow = v.cast<bool>();
        else throw py::type_error("unknown stroke setting '" + key + "'");
    }
}

std::vector<std::array<float, 2>> toPixels(py::handle points) {
    std::vector<std::array<float, 2>> out;
    for (py::handle p : points) {
        const py::sequence xy = py::reinterpret_borrow<py::sequence>(p);
        if (xy.size() != 2) throw py::value_error("screen points are (x, y) pairs");
        out.push_back({xy[0].cast<float>(), xy[1].cast<float>()});
    }
    return out;
}

unsigned modifiers(bool ctrl, bool shift) { return (ctrl ? kStrokeCtrl : 0u) | (shift ? kStrokeShift : 0u); }

py::dict editTexts(const StrokeEdit& edit, bool after) {
    py::dict out;
    for (std::size_t i = 0; i < kStrokeFields; ++i) {
        const StrokeEdit::Text& t = edit.texts[i];
        if (!t.set || (!after && !t.hasBefore)) continue;
        out[strokeFieldName(static_cast<StrokeField>(i))] = after ? t.after : t.before;
    }
    return out;
}

}  // namespace

void bindStroke(py::module_& m) {
    py::class_<ToolView>(m, "ToolView",
                         "A view as the interactive tools read it: a world-to-clip matrix (Maya's convention: row "
                         "vectors, clip = [x y z 1] * M, rows first, OpenGL clip space) and the port's size in pixels "
                         "(origin bottom left); the camera is worked out from the matrix.")
        .def(py::init([](py::handle matrix, double width, double height) { return ToolView::fromMatrix(toMatrix(matrix), width, height); }),
             py::arg("matrix"), py::arg("width"), py::arg("height"))
        .def_static(
            "look_at",
            [](const Vec3f& eye, const Vec3f& target, double fov, double width, double height, const Vec3f& up, double n, double f) {
                return ToolView::fromMatrix(lookAt(eye, target, up, fov, width / height, n, f), width, height);
            },
            py::arg("eye"), py::arg("target"), py::arg("fov") = 40.0, py::arg("width") = 800.0, py::arg("height") = 600.0,
            py::arg("up") = Vec3f(0, 1, 0), py::arg("near") = 0.1, py::arg("far") = 1000.0,
            "A perspective camera at `eye` looking at `target`, vertical field of view `fov` degrees.")
        .def_property_readonly("matrix", [](const ToolView& v) { return v.projection.matrix(); })
        .def_property_readonly("width", [](const ToolView& v) { return v.projection.width(); })
        .def_property_readonly("height", [](const ToolView& v) { return v.projection.height(); })
        .def_readonly("eye", &ToolView::eye)
        .def_readonly("view_direction", &ToolView::viewDirection)
        .def_readonly("orthographic", &ToolView::orthographic)
        .def(
            "project",
            [](const ToolView& v, const Vec3f& p) -> py::object {
                float x = 0, y = 0;
                if (!v.project(p, x, y)) return py::none();
                return py::make_tuple(x, y);
            },
            py::arg("point"), "Port pixels (x, y) of a world point, or None behind the camera.")
        .def(
            "ray",
            [](const ToolView& v, float x, float y) -> py::object {
                Vec3f o, d;
                if (!v.ray(x, y, o, d)) return py::none();
                return py::make_tuple(o, d);
            },
            py::arg("x"), py::arg("y"), "(origin, unit direction) of the ray under a pixel, or None.");

    py::class_<BrushToolSettings>(m, "StrokeSettings",
                                  "A stroke's settings: the tool and brush (mode), radius (world units, scripted) and "
                                  "screen_radius (pixels, interactive), and what groomlab.brush_tool takes, by the same "
                                  "keyword names.")
        .def(py::init([](py::kwargs kwargs) {
            BrushToolSettings s;
            applySettings(s, kwargs);
            return s;
        }))
        .def("update", [](BrushToolSettings& s, py::kwargs kwargs) { applySettings(s, kwargs); }, "Sets the named settings.")
        .def_property("mode", &BrushToolSettings::modeName, &BrushToolSettings::setMode,
                      "comb, smooth, lift, length, tube, paint, clump, twist, curl, straighten, attract, erase, "
                      "cut, slide, place, draw, part or flow.")
        .def_property("targets", &BrushToolSettings::targetsName, &BrushToolSettings::setTargets)
        .def_property(
            "radius", [](const BrushToolSettings& s) { return s.brush.radius; }, [](BrushToolSettings& s, float r) { s.brush.radius = r; })
        .def_property(
            "strength", [](const BrushToolSettings& s) { return s.brush.strength; },
            [](BrushToolSettings& s, float v) { s.brush.strength = v; })
        .def_property(
            "level", [](const BrushToolSettings& s) { return s.brush.level; }, [](BrushToolSettings& s, int v) { s.brush.level = v; })
        .def_property(
            "group", [](const BrushToolSettings& s) { return s.brush.group; },
            [](BrushToolSettings& s, const std::string& v) { s.brush.group = v; })
        .def_property(
            "attribute", [](const BrushToolSettings& s) { return s.brush.attribute; },
            [](BrushToolSettings& s, const std::string& v) { s.brush.attribute = v; })
        .def_property(
            "symmetry", &BrushToolSettings::symmetryName, &BrushToolSettings::setSymmetry,
            "x, y, z (a world plane at symmetry_center), topology (across the scalp's own surface) or none.")
        .def_property(
            "strength_profile", &BrushToolSettings::strengthProfileJson,
            [](BrushToolSettings& s, py::handle v) { s.setStrengthProfile(rampText(v)); },
            "The shaping brushes' strength root (0) to tip (1): a ramp's JSON (\"\": the same all along).")
        .def_property(
            "falloff_profile", &BrushToolSettings::falloffProfileJson,
            [](BrushToolSettings& s, py::handle v) { s.setFalloffProfile(rampText(v)); },
            "The falloff as a curve, center (0) to edge (1): a ramp's JSON (\"\": the hardness's smooth falloff).")
        .def_property(
            "solo_new_guides", [](const BrushToolSettings& s) { return s.place.soloNewGuides; },
            [](BrushToolSettings& s, bool on) { s.place.soloNewGuides = on; },
            "Place, Draw: the new guides unfrozen and every other frozen, in the stroke's undo step.")
        .def_readwrite("screen_radius", &BrushToolSettings::screenRadius)
        .def_readwrite("paint_kind", &BrushToolSettings::paintKind)
        .def_readwrite("mask_by_scalp", &BrushToolSettings::maskByScalp)
        .def_readwrite("pressure", &BrushToolSettings::pressure)
        .def_readwrite("selected_only", &BrushToolSettings::selectedOnly)
        .def("__repr__", [](const BrushToolSettings& s) {
            std::ostringstream out;
            out << "StrokeSettings(mode='" << s.modeName() << "', radius=" << s.brush.radius << ", strength=" << s.brush.strength
                << ", screen_radius=" << s.screenRadius << ")";
            return out.str();
        });

    py::class_<StrokeEdit>(m, "StrokeEdit",
                           "One stroke's change, one undo step: the stored texts it set (after, and before once its "
                           "host committed it) and a texture map's changed tiles.")
        .def_readonly("mode", &StrokeEdit::mode)
        .def_property_readonly("after", [](const StrokeEdit& e) { return editTexts(e, true); },
                               "{guideData|partData|flowData|correctiveData|scalpMaps|shotLayer: text} after the stroke.")
        .def_property_readonly("before", [](const StrokeEdit& e) { return editTexts(e, false); })
        .def_readonly("texture_map", &StrokeEdit::textureMap)
        .def_property_readonly("texture_tiles", [](const StrokeEdit& e) { return e.texture.tiles.size(); })
        .def_property_readonly("bytes", &StrokeEdit::bytes)
        .def_property_readonly("empty", &StrokeEdit::empty);

    py::class_<TexturePaint, std::shared_ptr<TexturePaint>>(m, "TexturePaint",
                                                            "A gray or RGBA map at the scalp's UVs, painted in 3D.")
        .def(py::init([](const ScalpMesh& scalp, int width, int height, float value) {
                 return std::make_shared<TexturePaint>(scalp, width, height, value);
             }),
             py::arg("scalp"), py::arg("width"), py::arg("height"), py::arg("value") = 0.0f, "A blank gray map.")
        .def(py::init([](const ScalpMesh& scalp, int width, int height, py::tuple color) {
                 return std::make_shared<TexturePaint>(scalp, width, height, toColor(color));
             }),
             py::arg("scalp"), py::arg("width"), py::arg("height"), py::arg("color"), "A blank color map (r, g, b[, a]).")
        .def_property_readonly("width", &TexturePaint::width)
        .def_property_readonly("height", &TexturePaint::height)
        .def_property_readonly("channels", &TexturePaint::channels)
        .def("values", [](const TexturePaint& p) { return static_cast<const std::vector<float>&>(p.values()); },
             "channels values per texel, rows top-down (a copy).")
        .def("save", &TexturePaint::save, py::arg("path"), py::arg("padding") = 8, "Writes the map as a PNG.");

    py::class_<SessionStrokeHost>(m, "StrokeHost",
                                  "Strokes on a GroomSession: inputs read from it, live previews through its preview "
                                  "setters, each stroke's edit applied to it and kept for undo() and redo().")
        .def(py::init<GroomSession&>(), py::arg("session"), py::keep_alive<1, 2>())
        .def("set_selection", &SessionStrokeHost::setSelection, py::arg("guides"), py::arg("soft_radius") = 0.0f,
             "The selected guides selected_only strokes brush (soft selection within soft_radius).")
        .def("set_hidden_groups", &SessionStrokeHost::setHiddenGroups, py::arg("groups"))
        .def("set_guide_smoothing", &SessionStrokeHost::setGuideSmoothing, py::arg("smoothing"))
        .def("set_shot_sculpt", &SessionStrokeHost::setShotSculpt, py::arg("on"), py::arg("seconds") = 0.0, py::arg("frame") = 0,
             py::arg("layer") = std::string{}, "Strokes reshape the groom as shown and record a corrective at this time.")
        .def("set_corrective_weights", &SessionStrokeHost::setCorrectiveWeights, py::arg("weights"))
        .def("set_texture_paint", &SessionStrokeHost::setTexturePaint, py::arg("name"), py::arg("paint"),
             "The paint of texture map `name` (strokes on that map paint it; None forgets it).")
        .def("undo", &SessionStrokeHost::undo, "Takes back the last stroke; False when there is none.")
        .def("redo", &SessionStrokeHost::redo)
        .def_property_readonly("edits", &SessionStrokeHost::edits, "The strokes done (StrokeEdit), oldest first.")
        .def_property_readonly("warnings", &SessionStrokeHost::warnings)
        .def("clear_warnings", &SessionStrokeHost::clearWarnings);

    py::class_<BrushTool>(m, "BrushTool",
                          "The interactive brush, Place, Draw, Part and Flow tools driven by mouse events in a "
                          "ToolView's pixels: press, drag, release (or stroke() for a whole one). Ctrl: the opposite "
                          "action where it does not select guides (painting, Part, Flow: ctrlSelects off); Shift: a "
                          "click picks a level, a drag smooths. One edit per stroke.")
        .def(py::init([](py::kwargs kwargs) {
            auto tool = std::make_unique<BrushTool>();
            applySettings(tool->settings, kwargs);
            return tool;
        }))
        .def_property(
            "settings", [](BrushTool& t) -> BrushToolSettings& { return t.settings; },
            [](BrushTool& t, const BrushToolSettings& s) { t.settings = s; }, py::return_value_policy::reference_internal)
        .def(
            "press",
            [](BrushTool& t, SessionStrokeHost& host, const ToolView& view, float x, float y, float pressure, bool ctrl, bool shift) {
                py::gil_scoped_release release;
                t.press(host, view, x, y, pressure, modifiers(ctrl, shift));
            },
            py::arg("host"), py::arg("view"), py::arg("x"), py::arg("y"), py::arg("pressure") = 1.0f, py::arg("ctrl") = false,
            py::arg("shift") = false)
        .def(
            "drag",
            [](BrushTool& t, SessionStrokeHost& host, const ToolView& view, float x, float y, float pressure) {
                py::gil_scoped_release release;
                t.drag(host, view, x, y, pressure);
            },
            py::arg("host"), py::arg("view"), py::arg("x"), py::arg("y"), py::arg("pressure") = 1.0f)
        .def(
            "release",
            [](BrushTool& t, SessionStrokeHost& host, const ToolView& view) {
                StrokeRelease r;
                {
                    py::gil_scoped_release release;
                    r = t.release(host, view);
                }
                return std::string(r == StrokeRelease::Stroke ? "stroke" : "none");
            },
            py::arg("host"), py::arg("view"), "'stroke' or 'none'.")
        .def(
            "stroke",
            [](BrushTool& t, SessionStrokeHost& host, const ToolView& view, py::handle points, float pressure, bool ctrl, bool shift) {
                const std::vector<std::array<float, 2>> pixels = toPixels(points);
                py::gil_scoped_release release;
                return t.stroke(host, view, pixels, pressure, modifiers(ctrl, shift));
            },
            py::arg("host"), py::arg("view"), py::arg("points"), py::arg("pressure") = 1.0f, py::arg("ctrl") = false,
            py::arg("shift") = false, "A whole stroke from (x, y) pixels; True when it changed something.")
        .def(
            "pick_level",
            [](BrushTool& t, SessionStrokeHost& host, const ToolView& view, float x, float y) { return t.pickLevel(host, view, x, y); },
            py::arg("host"), py::arg("view"), py::arg("x"), py::arg("y"),
             "The level of the guide under (x, y) becomes the brush's (and its group); -2 when none is there.")
        .def(
            "affected_lines",
            [](BrushTool& t, SessionStrokeHost& host, const ToolView& view, float x, float y) {
                return t.affectedLines(host, view, x, y);
            },
            py::arg("host"), py::arg("view"), py::arg("x"), py::arg("y"),
             "What hovering at (x, y) lights up: the guides the brush would touch, as drawn.")
        .def_property_readonly("stroking", &BrushTool::stroking)
        .def_property_readonly("changed", &BrushTool::changed)
        .def_property_readonly("drawn_path", &BrushTool::drawnPath)
        .def_property_readonly("mode_name", &BrushTool::toolModeName);

    // The brush catalog (brush_catalog.h): what every host's palette is built from.
    m.def(
        "brush_catalog",
        [] {
            py::list out;
            for (const BrushEntry& e : brushCatalog()) {
                py::dict d;
                d["id"] = e.id;
                d["label"] = e.label;
                d["category"] = brushCategoryName(e.category);
                d["order"] = e.order;
                d["icon"] = e.icon;
                d["default_strength"] = e.defaultStrength;
                d["signed"] = e.signedStrength;
                d["hint"] = e.hint;
                d["settings"] = e.settings;
                out.append(d);
            }
            return out;
        },
        "Every tool of the palette in order: {id, label, category, order, icon, default_strength, signed, hint, settings}.");
    m.def(
        "brush_categories",
        [] {
            std::vector<std::string> names;
            for (BrushCategory c : brushCategories()) names.emplace_back(brushCategoryName(c));
            return names;
        },
        "The palette's sections in order: Create, Shape, Trim, Paint, Guides.");
    m.def(
        "brush_settings",
        [] {
            py::list out;
            for (const BrushSettingSpec& s : brushSettingSpecs()) {
                py::dict d;
                d["id"] = s.id;
                d["label"] = s.label;
                d["hint"] = s.hint;
                out.append(d);
            }
            return out;
        },
        "The settings the tools' cards show: {id, label, hint} (falloff: 0 hard, 1 soft).");

    m.def(
        "paintable_settings",
        [] {
            py::list out;
            for (const PaintableSetting& s : paintableSettings()) {
                py::dict d;
                d["id"] = s.id;
                d["label"] = s.label;
                d["type"] = s.type;
                d["param"] = s.param;
                d["hint"] = s.hint;
                out.append(d);
            }
            return out;
        },
        "The settings + New Map > for a setting offers, in order: {id, label, type, param, hint} (the node type and "
        "its mappable param; every host's short list, Other being every mappable param of the graph).");
    m.def("first_node_of_type", &firstNodeOfType, py::arg("graph_json"), py::arg("type"),
          "The name of the graph's first node of type `type`, \"\" when it has none.");
    m.def(
        "param_map",
        [](const std::string& graphJson, const std::string& scalpMapsJson, const std::string& node, const std::string& param,
           const GuideHierarchy* guides) {
            const ParamMapRef ref = paramMapOf(graphJson, ScalpMaps::fromJson(scalpMapsJson), guides, node, param);
            py::dict out;
            out["map"] = ref.map;
            out["kind"] = ref.kind;
            return out;
        },
        py::arg("graph_json"), py::arg("scalp_maps_json"), py::arg("node"), py::arg("param"), py::arg("guides") = nullptr,
        "The map node.param reads: {map, kind}, map \"\" when it reads none. Raises RuntimeError for no such node, or "
        "a param read from what no brush paints (an expression, a channel, an image that is no map of the groom).");
    m.def(
        "paint_param_map",
        [](SessionStrokeHost& host, BrushToolSettings& settings, const std::string& node, const std::string& param) {
            const ParamMapBinding b = paintParamMap(host, settings, node, param);
            py::dict out;
            out["node"] = b.node;
            out["map"] = b.map;
            out["kind"] = b.kind;
            out["made"] = !b.graphJson.empty();
            return out;
        },
        py::arg("host"), py::arg("settings"), py::arg("node"), py::arg("param"),
        "+ New Map > for a setting: the map node.param reads, or a white vertex map made and linked to it (one undo "
        "step on host), and settings set to Paint on it (value 1; Ctrl paints toward 0). {node, map, kind, made}.");
    m.def(
        "pick_guides",
        [](const GuideHierarchy& h, const std::vector<GuideId>& candidates, const ToolView& view, py::handle lasso, bool rootsOnly,
           float clickPixels) {
            if (!view.projection.valid()) throw py::value_error("the view has no matrix");
            return pickGuides(h, candidates, pixelProjector(view.projection), toPixels(lasso), rootsOnly, clickPixels, &view.projection);
        },
        py::arg("hierarchy"), py::arg("candidates"), py::arg("view"), py::arg("lasso"), py::arg("roots_only") = false,
        py::arg("click_pixels") = 12.0f,
        "The selection tool's pick in a view: the candidates with a point inside the lasso ((x, y) pixels; the roots "
        "only with roots_only), or for one point (a click) the guide with a point nearest it within click_pixels.");
    m.def(
        "combine_selection",
        [](const std::vector<GuideId>& selected, const std::vector<GuideId>& picked, const std::string& mode) {
            return combineSelection(selected, picked, selectModeFromName(mode));
        },
        py::arg("selected"), py::arg("picked"), py::arg("mode") = std::string("replace"),
        "The selection after a pick: replace, add, toggle or deselect.");
    m.def(
        "scripted_stroke",
        [](SessionStrokeHost& host, const BrushToolSettings& settings, const std::vector<Vec3f>& points) {
            py::gil_scoped_release release;
            return scriptedStroke(host, settings, points);
        },
        py::arg("host"), py::arg("settings"), py::arg("points"),
        "A stroke along world points (brush spheres of settings.radius), committed to the host. Returns the vertices or "
        "texels a map stroke changed.");
    m.def(
        "brush_stroke",
        [](SessionStrokeHost& host, const std::string& mode, const std::vector<Vec3f>& points, py::kwargs kwargs) {
            // As groomlab.brush_stroke hands them to groomLabBrushStroke: its defaults, then the arguments.
            BrushToolSettings s;
            s.setMode("comb");
            py::dict all;
            all["mode"] = mode;
            all["radius"] = 0.25f;
            all["strength"] = 1.0f;
            all["level"] = 0;
            all["propagate"] = false;
            all["length"] = 0.5f;
            all["segments"] = 12;
            all["tube_radius"] = 0.1f;
            all["selected_only"] = false;
            all["symmetry"] = py::none();
            all["symmetry_center"] = 0.0f;
            all["value"] = 1.0f;
            all["match_neighbors"] = true;
            all["collide"] = true;
            all["part_name"] = "part";
            all["stretch"] = false;
            all["targets"] = "guides";
            all["hardness"] = 0.0f;
            all["smooth_strength"] = 1.0f;
            all["paint_channels"] = "rgba";
            for (const auto& item : kwargs) all[item.first] = item.second;
            applySettings(s, all);
            py::gil_scoped_release release;
            return scriptedStroke(host, s, points);
        },
        py::arg("host"), py::arg("mode"), py::arg("points"),
        "groomlab.brush_stroke on a session: the same modes, keyword arguments and defaults, the same result.");
}

}  // namespace groomlab::py_bind
