// SPDX-License-Identifier: Apache-2.0
// Viewport geometry (display.h) from Python: a DisplayBuilder turns a GroomSession's view and
// display settings into the buffers Maya's viewport draws, as buffer views (memoryview,
// numpy.asarray) a Python host uploads to its own renderer.

#include "bind_common.h"

#include <cstring>

#include "groomlab/display.h"
#include "groomlab/random.h"

namespace groomlab::py_bind {

namespace {

const char* const kParts[] = {"strands", "instances", "guides", "selection", "parts", "flow", "cards", "scalpMap", "strandMesh"};
const char* const kLayers[] = {"dormantWire", "dormantFilled", "activeWire", "selection"};

DisplayPart partNamed(const std::string& name) {
    for (std::size_t i = 0; i < kDisplayPartCount; ++i) {
        if (name == kParts[i]) return static_cast<DisplayPart>(i);
    }
    throw py::value_error("no display part '" + name + "' (strands, instances, guides, selection, parts, flow, cards, scalpMap, strandMesh)");
}

// A geometry's stream as a read-only view with `width` floats (or indices) per row.
template <class T>
ArrayView streamView(py::object owner, const std::vector<T>& v, py::ssize_t width) {
    return makeView(std::move(owner), v.data(), {static_cast<py::ssize_t>(v.size()) / width, width}, true);
}

// An item as Python sees it: the builder's item when it was asked for (the geometry is shared).
struct PyItem {
    DisplayItem item;
};

// Owns a vector the views read (computed arrays: recolored colors, dots).
struct FloatArray {
    std::vector<float> values;
};

py::object frameDict(const DisplayFrame& frame) {
    py::dict items;
    for (const DisplayItem& item : frame.items) items[item.name] = PyItem{item};
    return std::move(items);
}

}  // namespace

void bindDisplay(py::module_& m) {
    py::class_<FloatArray, std::shared_ptr<FloatArray>>(m, "_FloatArray");

    py::class_<DisplayTheme>(m, "DisplayTheme", "The overlays' colors (linear RGB): selection, frozen guides, parts, flow...")
        .def(py::init<>())
        .def_readwrite("point_size", &DisplayTheme::pointSize)
        .def_readwrite("selected", &DisplayTheme::selected)
        .def_readwrite("soft_selected", &DisplayTheme::softSelected)
        .def_readwrite("frozen", &DisplayTheme::frozen)
        .def_readwrite("parts", &DisplayTheme::parts)
        .def_readwrite("flow", &DisplayTheme::flow)
        .def_readwrite("flow_field", &DisplayTheme::flowField)
        .def_readwrite("tubes", &DisplayTheme::tubes)
        .def_readwrite("instances", &DisplayTheme::instances)
        .def_readwrite("cards", &DisplayTheme::cards)
        .def_readwrite("clump_centers", &DisplayTheme::clumpCenters)
        .def_readwrite("version", &DisplayTheme::version, "Bump it on a change: the guides build again.");

    py::class_<DisplaySettings>(m, "DisplaySettings",
                                "The display attributes (Maya: the groom shape's): strand colors and modes, ribbons, "
                                "lighting, sculpt display, guides, overlays, cards, the theme.")
        .def(py::init<>())
        .def_readwrite("hair_color", &DisplaySettings::hairColor)
        .def_readwrite("strand_look_colors", &DisplaySettings::strandLookColors,
                       "Lit strands in each strand's own color (HairColor's channels) where they vary (False: hair_color).")
        .def_readwrite("color_mode", &DisplaySettings::colorMode,
                       "0 solid, 1 colorSet, 2 attribute, 3 random, 4 clump, 5 density.")
        .def_readwrite("color_source", &DisplaySettings::colorSource)
        .def_readwrite("color_min", &DisplaySettings::colorMin)
        .def_readwrite("color_max", &DisplaySettings::colorMax)
        .def_readwrite("ribbons", &DisplaySettings::ribbons)
        .def_readwrite("ribbon_width_scale", &DisplaySettings::ribbonWidthScale,
                       "The ribbons' widths times this, and the lines' with width_coverage.")
        .def_readwrite("width_coverage", &DisplaySettings::widthCoverage,
                       "Strand lines with each point's opacity the share of a pixel its width covers (the host's shaders).")
        .def_readwrite("clump_lod", &DisplaySettings::clumpLod,
                       "Screen-space level of detail of the strand lines (strand_lod.h; the host's drawing, per view).")
        .def_readwrite("clump_lod_error", &DisplaySettings::clumpLodError, "The level of detail's coverage error allowed.")
        .def_readwrite("lighting", &DisplaySettings::lighting)
        .def_readwrite("sculpt_display", &DisplaySettings::sculptDisplay, "While a stroke is live: 0 as ever, 1 dimmed, 2 hidden.")
        .def_readwrite("cards_view", &DisplaySettings::cardsView)
        .def_readwrite("instances", &DisplaySettings::instances)
        .def_readwrite("guides", &DisplaySettings::guides)
        .def_readwrite("tubes", &DisplaySettings::tubes)
        .def_readwrite("guide_width", &DisplaySettings::guideWidth)
        .def_readwrite("guide_color_mode", &DisplaySettings::guideColorMode, "1 by the painted layer guide_color_layer; else (0) by group.")
        .def_readwrite("guide_color_layer", &DisplaySettings::guideColorLayer)
        .def_readwrite("isolate_group", &DisplaySettings::isolateGroup)
        .def_readwrite("guide_points", &DisplaySettings::guidePoints, "0 off, 1 roots and tips, 2 all.")
        .def_readwrite("guide_smoothing", &DisplaySettings::guideSmoothing)
        .def_readwrite("hidden_guide_groups", &DisplaySettings::hiddenGuideGroups)
        .def_readwrite("guide_group_colors", &DisplaySettings::guideGroupColors, "'name:#rrggbb ...' (sRGB).")
        .def_readwrite("flow", &DisplaySettings::flow)
        .def_readwrite("draw_cards", &DisplaySettings::drawCards)
        .def_readwrite("card_textures", &DisplaySettings::cardTextures)
        .def_readwrite("theme", &DisplaySettings::theme);

    py::class_<DisplayGeometry, std::shared_ptr<DisplayGeometry>>(
        m, "DisplayGeometry",
        "One render item's buffers (read-only views): positions (n, 3) float32, indices (m, 2) lines or (m, 3) "
        "triangles uint32, colors (n, 4) or empty, normals, tangents (n, 3), uvs (n, uv_floats): (n, 1) u root to tip "
        "for lit strands and ribbons, (n, 2) for cards, scales and the scalp map; bitangents (n, 3) carry the cards' "
        "and scales' shading normal (empty for strands).")
        .def_property_readonly("uv_floats", [](const DisplayGeometry& g) { return g.uvFloats; })
        .def_property_readonly("positions", [](py::object self) { return streamView(self, self.cast<const DisplayGeometry&>().positions, 3); })
        .def_property_readonly("indices",
                               [](py::object self) {
                                   const DisplayGeometry& g = self.cast<const DisplayGeometry&>();
                                   return streamView(self, g.indices, g.triangles ? 3 : 2);
                               })
        .def_property_readonly("colors", [](py::object self) { return streamView(self, self.cast<const DisplayGeometry&>().colors, 4); })
        .def_property_readonly("normals", [](py::object self) { return streamView(self, self.cast<const DisplayGeometry&>().normals, 3); })
        .def_property_readonly("tangents", [](py::object self) { return streamView(self, self.cast<const DisplayGeometry&>().tangents, 3); })
        .def_property_readonly("bitangents",
                               [](py::object self) { return streamView(self, self.cast<const DisplayGeometry&>().bitangents, 3); })
        .def_property_readonly("uvs",
                               [](py::object self) {
                                   const DisplayGeometry& g = self.cast<const DisplayGeometry&>();
                                   return streamView(self, g.uvs, static_cast<py::ssize_t>(g.uvFloats));
                               })
        .def_property_readonly("triangles", [](const DisplayGeometry& g) { return g.triangles; })
        .def_property_readonly("surface", [](const DisplayGeometry& g) { return g.surface; })
        .def_property_readonly("card_textures", [](const DisplayGeometry& g) { return g.cardTextures; })
        .def_property_readonly("overlay", [](const DisplayGeometry& g) { return g.overlay; })
        .def_property_readonly("map_texture", [](const DisplayGeometry& g) { return g.mapTexture; })
        .def_property_readonly("lit", &DisplayGeometry::lit)
        .def_property_readonly("empty", &DisplayGeometry::empty)
        .def_property_readonly("vertex_count", &DisplayGeometry::vertexCount)
        .def_property_readonly("digest", &DisplayGeometry::digest, "A hash of every stream's bytes and the flags.");

    py::class_<PyItem>(m, "DisplayItem",
                       "A render item: its geometry (None when not kept), style (color, line width, layer) and "
                       "versions that move when it was built again or recolored.")
        .def_property_readonly("name", [](const PyItem& p) { return std::string(p.item.name); })
        .def_property_readonly("part", [](const PyItem& p) { return std::string(kParts[static_cast<std::size_t>(p.item.part)]); })
        .def_property_readonly("geometry",
                               [](const PyItem& p) -> py::object {
                                   if (!p.item.geometry) return py::none();
                                   return py::cast(std::const_pointer_cast<DisplayGeometry>(p.item.geometry));
                               })
        .def_property_readonly("recolored",
                               [](const PyItem& p) -> py::object {
                                   if (!p.item.recolored) return py::none();
                                   auto values = std::make_shared<FloatArray>(FloatArray{p.item.recolored->rgba()});
                                   const py::object owner = py::cast(values);
                                   return py::cast(streamView(owner, values->values, 4));
                               },
                               "New colors (n, 4) for the geometry's points when only they changed, else None.")
        .def_property_readonly("color", [](const PyItem& p) { return p.item.color; })
        .def_property_readonly("line_width", [](const PyItem& p) { return p.item.lineWidth; })
        .def_property_readonly("layer", [](const PyItem& p) { return std::string(kLayers[static_cast<std::size_t>(p.item.layer)]); })
        .def_property_readonly("geometry_version", [](const PyItem& p) { return p.item.geometryVersion; })
        .def_property_readonly("color_version", [](const PyItem& p) { return p.item.colorVersion; })
        .def_property_readonly("rebuilt", [](const PyItem& p) { return p.item.rebuilt; })
        .def_property_readonly("recolor", [](const PyItem& p) { return p.item.recolor; })
        .def_property_readonly("shown", [](const PyItem& p) { return p.item.shown; })
        .def_property_readonly("vertex_count", [](const PyItem& p) { return p.item.vertexCount; })
        .def("__repr__", [](const PyItem& p) {
            return "<DisplayItem " + std::string(p.item.name) + " vertices=" + std::to_string(p.item.vertexCount) +
                   (p.item.rebuilt ? " rebuilt" : "") + (p.item.recolor ? " recolor" : "") + ">";
        });

    py::class_<DisplayBuilder>(m, "DisplayBuilder",
                               "Builds the viewport's buffers from a GroomSession view and DisplaySettings, again only "
                               "for what changed: update() returns {item name: DisplayItem}.")
        .def(py::init<>())
        .def(
            "update",
            [](DisplayBuilder& b, const PyView& view, const DisplaySettings& settings, bool stroke, py::object selection,
               std::uint64_t selectionHash, py::object prototypes, py::object scalpMap, bool scalpMapTexture,
               std::uint64_t scalpMapTextureGeneration) {
                DisplayInputs in = DisplayInputs::fromView(view.result, stroke);
                std::map<GuideId, float> weights;
                if (!selection.is_none()) weights = selection.cast<std::map<GuideId, float>>();
                in.selection = [weights] { return weights; };
                in.selectionHash = selectionHash;
                std::map<std::string, InstancePrototype> meshes;
                if (!prototypes.is_none()) {
                    for (const auto& [name, mesh] : prototypes.cast<py::dict>()) {
                        const py::tuple t = mesh.cast<py::tuple>();
                        if (t.size() != 3) throw py::value_error("a prototype is (points, normals, triangles)");
                        InstancePrototype p;
                        p.points = toPoints(t[0]);
                        p.normals = toPoints(t[1]);
                        for (const auto& tri : toTriangles(t[2])) {
                            for (const std::int32_t v : tri) p.triangles.push_back(static_cast<std::uint32_t>(v));
                        }
                        if (p.normals.size() != p.points.size()) throw py::value_error("a prototype needs a normal per point");
                        p.signature = hashCombine(bufferDigest(p.points.data(), p.points.size() * sizeof(Vec3f)),
                                                  bufferDigest(p.triangles.data(), p.triangles.size() * 4));
                        p.found = true;
                        meshes[name.cast<std::string>()] = std::move(p);
                    }
                    in.prototypes = &meshes;
                }
                if (!scalpMap.is_none()) in.scalpMap = scalpMap.cast<ScalpMapView>();
                in.scalpMapTexture = scalpMapTexture;
                in.scalpMapTextureGeneration = scalpMapTextureGeneration;
                {
                    py::gil_scoped_release release;
                    b.update(in, settings);
                }
                return frameDict(b.frame());
            },
            py::arg("view"), py::arg("settings"), py::arg("stroke") = false, py::arg("selection") = py::none(),
            py::arg("selection_hash") = 0, py::arg("prototypes") = py::none(), py::arg("scalp_map") = py::none(),
            py::arg("scalp_map_texture") = false, py::arg("scalp_map_texture_generation") = 0,
            "The items for a GroomSession.view() result. stroke: a brush stroke is live (sculpt_display). selection: "
            "{guide id: weight} (1 selected, below soft), with a selection_hash that changes with it. prototypes: "
            "{name: (points, normals, triangles)} for the Instance nodes' meshes (per face-vertex). scalp_map: a "
            "ScalpMapView (GroomSession.scalp_map_view) shown on the scalp; scalp_map_texture: the host holds its "
            "image as a texture (and scalp_map_texture_generation counts the textures it made).")
        .def("items", [](const DisplayBuilder& b) { return frameDict(b.frame()); }, "The items of the last update.")
        .def_property_readonly("rebuilt_parts",
                               [](const DisplayBuilder& b) {
                                   py::list out;
                                   for (std::size_t i = 0; i < kDisplayPartCount; ++i) {
                                       if (b.frame().rebuilt[i]) out.append(kParts[i]);
                                   }
                                   return out;
                               })
        .def_property_readonly("guide_dots",
                               [](const DisplayBuilder& b) {
                                   auto values = std::make_shared<FloatArray>();
                                   const GuideDots& d = b.frame().dots;
                                   for (std::size_t i = 0; i < d.points.size(); ++i) {
                                       values->values.insert(values->values.end(), {d.points[i].x, d.points[i].y, d.points[i].z,
                                                                                    d.colors[i][0], d.colors[i][1], d.colors[i][2]});
                                   }
                                   const py::object owner = py::cast(values);
                                   return streamView(owner, values->values, 6);
                               },
                               "Guide point dots (k, 6): x y z r g b, drawn facing the camera at point_size pixels.")
        .def_property_readonly("point_size", [](const DisplayBuilder& b) { return b.frame().pointSize; })
        .def_property_readonly("overlay_pull", [](const DisplayBuilder& b) { return b.frame().overlayPull; },
                               "How far overlays are pulled toward the camera (scene units): the longest strand.")
        .def("invalidate", [](DisplayBuilder& b, const std::string& part) { b.invalidate(partNamed(part)); }, py::arg("part"),
             "The part builds again on the next update.")
        .def("invalidate_all", &DisplayBuilder::invalidateAll)
        .def("set_retain", &DisplayBuilder::setRetain, py::arg("retain"),
             "Off: an item holds its geometry only in the update that built it.");

    py::class_<ScalpMapView>(m, "ScalpMapView", "What a scalp shows of a painted map (GroomSession.scalp_map_view).")
        .def_readonly("name", &ScalpMapView::name)
        .def_readonly("kind", &ScalpMapView::kind)
        .def_readonly("channels", &ScalpMapView::channels)
        .def_readonly("channel", &ScalpMapView::channel)
        .def_readonly("texture", &ScalpMapView::texture)
        .def_readonly("version", &ScalpMapView::version);
    auto session = py::reinterpret_borrow<py::class_<GroomSession>>(py::type::of<GroomSession>());
    session.def("scalp_map_view", &GroomSession::scalpMapView, py::arg("name"), py::arg("channel") = std::string("rgb"),
                "A painted map (or guide layer) as the scalp shows it, for DisplayBuilder.update(scalp_map=...).");

    m.def(
        "buffer_digest",
        [](py::buffer b) {
            const py::buffer_info info = b.request();
            return bufferDigest(info.ptr, static_cast<std::size_t>(info.size * info.itemsize));
        },
        py::arg("buffer"), "The core's hash of a buffer's bytes (as DisplayGeometry.digest hashes each stream).");
    m.def("hair_highlight",
          [](float specular, float roughness) {
              HairLook look;
              look.specular = specular;
              look.roughness = roughness;
              const HairHighlight h = hairHighlight(look);
              return py::make_tuple(h.color, h.power);
          },
          py::arg("specular"), py::arg("roughness"), "The lit hair's highlight color and power for a HairColor look.");
    m.def("root_to_tip_ramp", [](unsigned width) { const auto p = rootToTipRamp(width); return py::bytes(reinterpret_cast<const char*>(p.data()), p.size()); },
          py::arg("width") = 32u, "The lit hair's root-to-tip brightness ramp: width RGBA8 texels.");
    m.def(
        "combine_card_maps",
        [](py::buffer color, py::buffer coverage, py::object ao, std::size_t width, std::size_t height) {
            const py::buffer_info c = color.request(), v = coverage.request();
            const std::size_t bytes = width * height * 4;
            if (static_cast<std::size_t>(c.size * c.itemsize) != bytes || static_cast<std::size_t>(v.size * v.itemsize) != bytes) {
                throw py::value_error("the maps must be width x height RGBA8");
            }
            py::buffer_info a;
            const std::uint8_t* occ = nullptr;
            if (!ao.is_none()) {
                a = ao.cast<py::buffer>().request();
                if (static_cast<std::size_t>(a.size * a.itemsize) != bytes) throw py::value_error("the AO map must be width x height RGBA8");
                occ = static_cast<const std::uint8_t*>(a.ptr);
            }
            const std::vector<std::uint8_t> rgba =
                combineCardMaps(static_cast<const std::uint8_t*>(c.ptr), static_cast<const std::uint8_t*>(v.ptr), occ, width, height);
            return py::bytes(reinterpret_cast<const char*>(rgba.data()), rgba.size());
        },
        py::arg("color"), py::arg("coverage"), py::arg("ao"), py::arg("width"), py::arg("height"),
        "The cards' map texture (RGBA8): the color darkened by half the AO, the coverage in the alpha.");
    m.def(
        "overlay_pull_limits",
        [](py::handle points, py::handle camera, float pull, const std::vector<std::shared_ptr<ScalpMesh>>& occluders) {
            Vec3f eye;
            if (!loadVec3(camera, eye)) throw py::type_error("camera: a Vec3 or three numbers");
            std::vector<std::shared_ptr<const ScalpMesh>> meshes(occluders.begin(), occluders.end());
            return overlayPullLimits(toPoints(points), eye, pull, meshes);
        },
        py::arg("points"), py::arg("camera"), py::arg("pull"), py::arg("occluders"),
        "Per point: the share of `pull` it may move toward the camera before one of the occluder meshes.");
}

}  // namespace groomlab::py_bind
