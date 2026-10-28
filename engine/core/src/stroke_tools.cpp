// SPDX-License-Identifier: Apache-2.0
// The tools as functions of data (stroke.h): settings, Place, Draw, Part and Flow, map dabs.

#include "groomlab/stroke.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "groomlab/graph_edit.h"
#include "groomlab/graph_io.h"
#include "groomlab/guide_selection.h"
#include "groomlab/scalp_maps.h"

namespace groomlab {

// ---- Settings ---------------------------------------------------------------------------------

std::array<float, 3> brushRingColor(const BrushToolSettings& s) {
    using Tool = BrushToolSettings::Tool;
    if (s.tool == Tool::Part) return {1.0f, 0.3f, 0.3f};
    if (s.tool == Tool::Flow) return {1.0f, 0.55f, 0.1f};
    if (s.tool != Tool::Brush) return {0.3f, 1.0f, 0.4f};  // Place and Draw make guides
    if (s.brush.mode == BrushMode::Erase) return {1.0f, 0.3f, 0.3f};
    return {1.0f, 0.8f, 0.2f};
}

std::array<float, 3> brushPathColor(const BrushToolSettings&) { return {1.0f, 0.3f, 0.3f}; }

std::array<float, 3> brushHoverColor(const BrushToolSettings& s) {
    if (s.brush.mode == BrushMode::Erase) return {1.0f, 0.35f, 0.3f};
    return {1.0f, 1.0f, 1.0f};
}

void BrushToolSettings::setMode(const std::string& name) {
    if (name == "place" || name == "draw" || name == "part" || name == "flow") {
        tool = name == "place" ? Tool::Place : name == "draw" ? Tool::Draw : name == "part" ? Tool::Part : Tool::Flow;
        return;
    }
    if (name == "width" || name == "density") throw std::invalid_argument(kRemovedParamBrushes);
    brush.mode = brushModeFromString(name);  // throws on unknown names
    if (name == "clump") brush.attribute = "clump";
    tool = Tool::Brush;
}

std::string BrushToolSettings::modeName() const {
    if (tool == Tool::Place) return "place";
    if (tool == Tool::Draw) return "draw";
    if (tool == Tool::Part) return "part";
    if (tool == Tool::Flow) return "flow";
    if (brush.mode == BrushMode::Attribute && brush.attribute == "clump") return "clump";
    return brushModeName(brush.mode);
}

void BrushToolSettings::setScreenRadius(float pixels) { screenRadius = std::max(1.0f, pixels); }

void BrushToolSettings::setSegments(int segments) { place.segments = std::max(1, segments); }

void BrushToolSettings::setPaintResolution(int pixels) { paintResolution = std::clamp(pixels, kMinPaintResolution, kMaxPaintResolution); }

void BrushToolSettings::setHardness(float value) { brush.hardness = std::clamp(value, 0.0f, 1.0f); }

namespace {

// A ramp from its JSON (graph_io.h's form), null for "".
std::shared_ptr<const Ramp> rampOf(const std::string& json, const char* what) {
    const bool blank = std::all_of(json.begin(), json.end(), [](unsigned char c) { return std::isspace(c); });
    if (blank) return nullptr;
    try {
        Ramp ramp = rampFromJson(json);
        if (ramp.keys.empty()) return nullptr;
        return std::make_shared<const Ramp>(std::move(ramp));
    } catch (const std::exception& e) {
        throw std::invalid_argument(std::string(what) + ": " + e.what());
    }
}

std::string rampJson(const std::shared_ptr<const Ramp>& ramp) {
    if (!ramp) return {};
    nlohmann::json keys = nlohmann::json::array();
    for (const auto& [t, v] : ramp->keys) keys.push_back({t, v});
    if (!ramp->straight) return keys.dump();
    return nlohmann::json{{"keys", keys}, {"linear", true}}.dump();
}

}  // namespace

void BrushToolSettings::setStrengthProfile(const std::string& json) {
    std::shared_ptr<const Ramp> ramp = rampOf(json, "strengthProfile");
    // 1 all along is the brush without a profile (and gives exactly its result).
    if (ramp && std::all_of(ramp->keys.begin(), ramp->keys.end(), [](const auto& k) { return k.second == 1.0f; })) ramp = nullptr;
    brush.strengthProfile = std::move(ramp);
}

void BrushToolSettings::setFalloffProfile(const std::string& json) { brush.falloffProfile = rampOf(json, "falloffProfile"); }

std::string BrushToolSettings::strengthProfileJson() const { return rampJson(brush.strengthProfile); }

std::string BrushToolSettings::falloffProfileJson() const { return rampJson(brush.falloffProfile); }

void BrushToolSettings::setSmoothStrength(float strength) { smoothStrength = std::max(0.0f, strength); }

void BrushToolSettings::setPaintColor(const PaintColor& color) {
    for (std::size_t k = 0; k < paintColor.size(); ++k) paintColor[k] = std::clamp(color[k], 0.0f, 1.0f);
}

void BrushToolSettings::setTargets(const std::string& names) {
    bool guides = false, parts = false, flow = false;
    std::size_t start = 0;
    while (start <= names.size()) {
        const std::size_t end = std::min(names.find(',', start), names.size());
        std::string word = names.substr(start, end - start);
        word.erase(std::remove_if(word.begin(), word.end(), [](unsigned char c) { return std::isspace(c); }), word.end());
        if (word == "guides") guides = true;
        else if (word == "parts") parts = true;
        else if (word == "flow") flow = true;
        else if (!word.empty() && word != "none") throw std::invalid_argument("-targets takes guides, parts, flow (comma separated) or none");
        start = end + 1;
    }
    brushGuides = guides;
    brushParts = parts;
    brushFlow = flow;
}

std::string BrushToolSettings::targetsName() const {
    std::string out;
    for (const auto& [on, name] : {std::pair{brushGuides, "guides"}, std::pair{brushParts, "parts"}, std::pair{brushFlow, "flow"}}) {
        if (!on) continue;
        if (!out.empty()) out += ",";
        out += name;
    }
    return out.empty() ? "none" : out;
}

void BrushToolSettings::prepareMask(const GuideHierarchy& h, std::set<std::string> hiddenGroups,
                                    const std::vector<GuideId>& selection, float softRadius) {
    brush.hiddenGroups = std::move(hiddenGroups);
    brush.mask = nullptr;
    if (!selectedOnly) return;
    selectionMask = std::make_shared<const std::map<GuideId, float>>(softSelection(h, selection, softRadius));
    brush.mask = selectionMask.get();  // empty selection: nothing to brush
}

int symmetryAxisFromName(const std::string& text) {
    if (text == "x") return 0;
    if (text == "y") return 1;
    if (text == "z") return 2;
    if (text.empty() || text == "none" || text == "off") return -1;
    throw std::invalid_argument("-symmetry takes x, y, z, topology or none");
}

void BrushToolSettings::setSymmetry(const std::string& name) {
    if (name == "topology") {
        brush.mirrorTopology = true;
        brush.mirrorAxis = -1;
        return;
    }
    brush.mirrorAxis = symmetryAxisFromName(name);  // throws for anything else
    brush.mirrorTopology = false;
}

std::string BrushToolSettings::symmetryName() const { return brush.mirrorTopology ? "topology" : symmetryAxisName(brush.mirrorAxis); }

const char* symmetryAxisName(int axis) { return axis == 0 ? "x" : axis == 1 ? "y" : axis == 2 ? "z" : "none"; }

// ---- Place, Draw, Part, Flow ------------------------------------------------------------------

std::string placeGroup(GuideHierarchy& h, const BrushSettings& b) {
    if (!b.group.empty()) return b.group;
    const int level = std::max(b.level, 0);
    for (const GuideGroup& g : h.groups()) {
        if (h.groupDepth(g.id) == level) return g.name;
    }
    if (level == 0) return "primary";
    for (const GuideGroup& g : h.groups()) {
        if (h.groupDepth(g.id) == level - 1) {
            const std::string name = g.name + "_children";
            if (h.findGroup(name) == kNoGroup) h.addGroup(name, g.id);
            return name;
        }
    }
    throw std::invalid_argument("no guides at level " + std::to_string(level - 1) + " to be the parents");
}

namespace {

PlaceSettings placeSettings(GuideHierarchy& h, const BrushToolSettings& s) {
    PlaceSettings p = s.place;
    p.group = placeGroup(h, s.brush);
    return p;
}

}  // namespace

std::size_t placeWithSymmetry(GuideHierarchy& h, const ScalpMesh& scalp, const BrushToolSettings& s, const SurfacePoint& at,
                              bool spaced) {
    const PlaceSettings p = placeSettings(h, s);
    std::vector<Vec3f> points{at.position};
    const StrokeSymmetry sym = StrokeSymmetry::of(s.brush, &scalp);
    if (sym.on()) {
        const Vec3f mirrored = sym.point(at.position);
        if ((mirrored - at.position).length() > 1e-4f) points.push_back(mirrored);
    }
    if (spaced) return placeGuides(h, scalp, p, points).size();
    for (const Vec3f& pt : points) placeGuide(h, scalp, p, scalp.closestPoint(pt));
    return points.size();
}

void drawWithSymmetry(GuideHierarchy& h, const ScalpMesh& scalp, const BrushToolSettings& s, const std::vector<Vec3f>& path) {
    const PlaceSettings p = placeSettings(h, s);
    drawGuide(h, scalp, p, path);
    const StrokeSymmetry sym = StrokeSymmetry::of(s.brush, &scalp);
    if (sym.on()) {
        std::vector<Vec3f> mirrored;
        for (const Vec3f& pt : path) mirrored.push_back(sym.point(pt));
        if ((mirrored.front() - path.front()).length() > 1e-4f) drawGuide(h, scalp, p, mirrored);
    }
}

void addPartWithSymmetry(PartSet& set, const ScalpMesh& scalp, const BrushToolSettings& s, const std::vector<Vec3f>& path) {
    set.add(s.partName, path, scalp);
    const StrokeSymmetry sym = StrokeSymmetry::of(s.brush, &scalp);
    if (sym.on()) {
        std::vector<Vec3f> mirrored;
        for (const Vec3f& p : path) mirrored.push_back(sym.point(p));
        float apart = 0.0f;
        for (std::size_t i = 0; i < path.size(); ++i) apart = std::max(apart, (mirrored[i] - path[i]).length());
        if (apart > 1e-3f) set.add(s.partName, mirrored, scalp);  // a part on the seam is its own mirror
    }
}

void flowAlongStroke(FlowSet& set, const ScalpMesh& scalp, const BrushToolSettings& s, const std::vector<Vec3f>& path,
                     const FlowWeightFn& weight, float spacing, bool erase) {
    if (path.empty()) return;
    const float k = std::clamp(s.brush.strength, 0.0f, 1.0f);
    const StrokeSymmetry sym = StrokeSymmetry::of(s.brush, &scalp);
    std::vector<Vec3f> dirs(path.size());
    for (std::size_t i = 0; i < path.size(); ++i) {
        dirs[i] = path[std::min(i + 1, path.size() - 1)] - path[i > 0 ? i - 1 : 0];
    }
    // Each stroke (and its mirror image) lays one line of pins: drawn as a curve.
    int nextLine = 0;
    for (const FlowPin& pin : set.pins) nextLine = std::max(nextLine, pin.line + 1);
    auto pass = [&](bool mirrored) {
        const int line = nextLine++;
        int seq = 0;
        auto flip = [&](const Vec3f& p) { return mirrored ? sym.point(p) : p; };
        auto flipDir = [&](const Vec3f& at, const Vec3f& d) { return mirrored ? sym.vector(at, d) : d; };
        std::vector<FlowPin> kept;
        for (const FlowPin& pin : set.pins) {
            if (pin.root.face < 0 || static_cast<std::size_t>(pin.root.face) >= scalp.triangleCount()) {
                kept.push_back(pin);
                continue;
            }
            const Vec3f pos = scalp.position(pin.root);
            float w = 0.0f;
            std::size_t best = 0;
            for (std::size_t i = 0; i < path.size(); ++i) {  // pins are weighed against the stroke as drawn
                const float wi = weight(flip(pos), i);
                if (wi > w) w = wi, best = i;
            }
            if (w <= 0.0f) {
                kept.push_back(pin);
                continue;
            }
            if (erase) continue;
            const Vec3f target = flipDir(path[best], dirs[best]);
            if (target.length() < 1e-9f) {
                kept.push_back(pin);
                continue;
            }
            Vec3f x, y, z;
            surfaceFrame(scalp, pin.root, x, y, z);
            const Vec3f current = x * pin.local.x + y * pin.local.y + z * pin.local.z;
            const Vec3f turned = current * (1.0f - w * k) + target.normalized() * (w * k);
            try {
                FlowPin turnedPin = FlowSet::makePin(scalp, SurfacePoint{pin.root, pos, z}, turned);
                turnedPin.line = pin.line;  // still part of its own stroke's curve
                turnedPin.seq = pin.seq;
                kept.push_back(turnedPin);
            } catch (const std::invalid_argument&) {
                kept.push_back(pin);
            }
        }
        set.pins = std::move(kept);
        if (erase || spacing <= 0.0f) return;
        std::vector<Vec3f> positions;
        for (const FlowPin& pin : set.pins) {
            if (pin.root.face >= 0 && static_cast<std::size_t>(pin.root.face) < scalp.triangleCount()) {
                positions.push_back(scalp.position(pin.root));
            }
        }
        float travelled = spacing;  // the stroke's start gets an arrow
        for (std::size_t i = 0; i < path.size(); ++i) {
            if (i > 0) travelled += (path[i] - path[i - 1]).length();
            if (travelled < spacing || dirs[i].length() < 1e-9f) continue;
            const SurfacePoint at = scalp.closestPoint(flip(path[i]));
            if (at.root.face < 0) continue;
            const bool crowded = std::any_of(positions.begin(), positions.end(), [&](const Vec3f& q) {
                return (q - at.position).length() < 0.75f * spacing;
            });
            if (crowded) continue;
            try {
                FlowPin pin = FlowSet::makePin(scalp, at, flipDir(path[i], dirs[i]));
                pin.line = line;
                pin.seq = seq++;
                set.pins.push_back(pin);
                positions.push_back(at.position);
                travelled = 0.0f;
            } catch (const std::invalid_argument&) {
            }
        }
    };
    pass(false);
    if (sym.on()) pass(true);
}

void flowSinksAt(FlowSet& set, const ScalpMesh& scalp, const BrushToolSettings& s, const std::vector<Vec3f>& points,
                 float radius, bool erase) {
    const float angle = erase ? 0.0f : flowSinkAngle(s.flowSink);
    const StrokeSymmetry sym = StrokeSymmetry::of(s.brush, &scalp);
    for (const Vec3f& p : points) {
        for (int side = 0; side < (sym.on() ? 2 : 1); ++side) {
            const Vec3f at = side ? sym.point(p) : p;
            if (side && (at - p).length() < 0.25f * radius) continue;  // on the seam: its own mirror
            if (erase) {
                set.sinks.erase(std::remove_if(set.sinks.begin(), set.sinks.end(),
                                               [&](const FlowSink& sink) {
                                                   return sink.root.face >= 0 &&
                                                          static_cast<std::size_t>(sink.root.face) < scalp.triangleCount() &&
                                                          (scalp.position(sink.root) - at).length() <= radius;
                                               }),
                                set.sinks.end());
                continue;
            }
            const SurfacePoint hit = scalp.closestPoint(at);
            if (hit.root.face < 0) continue;
            FlowSink sink;
            sink.root = hit.root;
            sink.angle = side ? -angle : angle;
            sink.radius = radius;
            set.sinks.push_back(sink);
        }
    }
}

// ---- Painting a param's map -------------------------------------------------------------------

const char* const kRemovedParamBrushes =
    "the Width and Density brushes are gone: paint the param's map with the Paint brush instead (Paint tab > + New Map "
    "> for a setting > Width or Density makes, links and picks it; Ctrl paints toward 0, which removes hair on a density "
    "map)";

float grayCtrlTarget(float value) { return value > 0.0f ? 0.0f : 1.0f; }

const std::vector<PaintableSetting>& paintableSettings() {
    static const std::vector<PaintableSetting> settings = {
        {"density", "Density", "Scatter", "density", "Where hair grows: white the node's density, black no hair (Ctrl removes hair)"},
        {"width", "Width", "Width", "width", "How thick the hair is: white the node's width, black none (Ctrl thins it)"},
        {"length", "Length", "Length", "keep", "How much of each strand is kept: white all of it, black none (Ctrl shortens)"},
        {"clump", "Clump strength", "Clump", "strength", "How tightly strands clump: white the node's strength, black loose"},
        {"curl", "Curl", "Curl", "affect", "Where the curls go: white curled, black straight"},
        {"noise", "Noise amplitude", "Noise", "amplitude", "How much noise: white the node's amplitude, black smooth"},
        {"flyaways", "Flyaways", "Flyaway", "fraction", "Where strays leave the flow: white the node's share of strays, black none"},
        {"guideWeight", "Guide weight", "Interpolate", "guideWeight",
         "How strongly the guides rooted there pull the hair around them: white fully, black they give way to their neighbours"},
    };
    return settings;
}

const PaintableSetting* paintableSetting(const std::string& id) {
    for (const PaintableSetting& s : paintableSettings()) {
        if (id == s.id) return &s;
    }
    return nullptr;
}

namespace {

nlohmann::json graphOf(const std::string& graphJson) {
    const bool none = graphJson.empty() || graphJson == "{}";
    nlohmann::json g = nlohmann::json::parse(none ? defaultGraphJson() : graphJson, nullptr, false);
    if (g.is_discarded() || !g.is_object()) throw std::runtime_error("the graph is not valid JSON");
    return g;
}

nlohmann::json* firstNodeWith(nlohmann::json& g, const char* key, const std::string& value) {
    const auto nodes = g.find("nodes");
    if (nodes == g.end() || !nodes->is_array()) return nullptr;
    for (nlohmann::json& n : *nodes) {
        if (n.is_object() && n.value(key, std::string()) == value) return &n;
    }
    return nullptr;
}

nlohmann::json& nodeNamed(nlohmann::json& g, const std::string& node) {
    nlohmann::json* n = firstNodeWith(g, "name", node);
    if (!n) throw std::runtime_error("the graph has no node named " + node);
    return *n;
}

// What drives a param: the map it reads ("" none), whether it reads something no brush paints
// (`foreign`: why), and the map's kind.
struct ParamSource {
    std::string map, kind, foreign;
};

ParamSource sourceOf(nlohmann::json& g, const nlohmann::json& node, const ScalpMaps& maps, const GuideHierarchy* guides,
                     const std::string& param) {
    ParamSource out;
    const std::string name = node.value("name", std::string());
    auto kindOf = [&](const std::string& map) {
        const auto it = maps.maps.find(map);
        if (it != maps.maps.end()) return it->second.kind;
        if (guides) {
            const std::vector<std::string> layers = guides->attributeNames();
            if (std::find(layers.begin(), layers.end(), map) != layers.end()) return std::string("guides");
        }
        return std::string();
    };
    // A Map node wired to the param's input.
    const auto connections = g.find("connections");
    if (connections != g.end() && connections->is_array()) {
        for (const nlohmann::json& c : *connections) {
            if (!c.is_object() || c.value("to", std::string()) != name || c.value("input", std::string()) != param) continue;
            const std::string from = c.value("from", std::string());
            for (const nlohmann::json& n : g["nodes"]) {
                if (!n.is_object() || n.value("name", std::string()) != from) continue;
                if (n.value("type", std::string()) != "Map") {
                    out.foreign = "is wired to " + from + ", not a painted map";
                    return out;
                }
                const nlohmann::json params = n.value("params", nlohmann::json::object());
                out.map = params.is_object() ? params.value("name", std::string("mask")) : std::string("mask");
                out.kind = kindOf(out.map);
                if (out.kind.empty()) out.foreign = "reads " + out.map + ", which is no map of the groom";
                return out;
            }
        }
    }
    const nlohmann::json params = node.value("params", nlohmann::json::object());
    const auto value = params.is_object() ? params.find(param) : params.end();
    if (!params.is_object() || value == params.end() || !value->is_object()) return out;  // a number: no map yet
    for (const char* key : {"colorSet", "guide"}) {
        if (value->contains(key) && (*value)[key].is_string()) {
            out.map = (*value)[key].get<std::string>();
            out.kind = std::string(key) == "guide" ? "guides" : kindOf(out.map);
            if (out.kind.empty()) out.foreign = "reads the color set " + out.map + ", which is no map of the groom";
            return out;
        }
    }
    if (value->contains("texture") && (*value)["texture"].is_string()) {
        const std::string file = (*value)["texture"].get<std::string>();
        for (const auto& [map, entry] : maps.maps) {
            if (entry.kind == "texture" && entry.texture == file) {
                out.map = map;
                out.kind = "texture";
                return out;
            }
        }
        out.foreign = "reads the image " + file + ", which is no map of the groom";
        return out;
    }
    out.foreign = "is driven by an expression or a channel";
    return out;
}

}  // namespace

std::string firstNodeOfType(const std::string& graphJson, const std::string& type) {
    nlohmann::json g = graphOf(graphJson);
    const nlohmann::json* n = firstNodeWith(g, "type", type);
    return n ? n->value("name", std::string()) : std::string();
}

ParamMapRef paramMapOf(const std::string& graphJson, const ScalpMaps& maps, const GuideHierarchy* guides, const std::string& node,
                       const std::string& param) {
    nlohmann::json g = graphOf(graphJson);
    const ParamSource source = sourceOf(g, nodeNamed(g, node), maps, guides, param);
    if (!source.foreign.empty()) throw std::runtime_error(node + "." + param + " " + source.foreign);
    return {source.map, source.kind};
}

std::string wireParamMap(const std::string& graphJson, const ScalpMaps& maps, const std::string& node, const std::string& param,
                         const std::string& map, const std::string& kind) {
    nlohmann::json g = graphOf(graphJson);
    nlohmann::json& n = nodeNamed(g, node);
    // White: 1 x the param's value, as it was (its default when the node leaves it out).
    double top = 1.0;
    try {
        const nlohmann::json complete = nlohmann::json::parse(completeGraphJson(g.dump()), nullptr, false);
        if (complete.is_object() && complete.contains("nodes")) {
            for (const nlohmann::json& c : complete["nodes"]) {
                if (!c.is_object() || c.value("name", std::string()) != node) continue;
                const nlohmann::json params = c.value("params", nlohmann::json::object());
                if (params.is_object() && params.contains(param) && params[param].is_number()) top = params[param].get<double>();
            }
        }
    } catch (const std::exception&) {
        // An incomplete graph: the map's white is 1.
    }
    nlohmann::json value;
    if (kind == "guides") {
        value = {{"guide", map}, {"min", 0.0}, {"max", top}};
    } else if (kind == "texture") {
        const auto it = maps.maps.find(map);
        if (it == maps.maps.end() || it->second.kind != "texture") throw std::runtime_error("the groom has no texture map named " + map);
        value = {{"texture", it->second.texture}, {"component", "r"}, {"min", 0.0}, {"max", top}};
    } else {
        value = {{"colorSet", map}, {"component", "r"}, {"min", 0.0}, {"max", top}};
    }
    if (!n.contains("params") || !n["params"].is_object()) n["params"] = nlohmann::json::object();
    n["params"][param] = std::move(value);
    // A Map node wired to the param would win over the map param: the wire goes.
    const auto connections = g.find("connections");
    if (connections != g.end() && connections->is_array()) {
        auto& list = *connections;
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const nlohmann::json& c) {
                                      return c.is_object() && c.value("to", std::string()) == node &&
                                             c.value("input", std::string()) == param;
                                  }),
                   list.end());
    }
    return g.dump();
}

ParamMapBinding bindParamMap(const std::string& graphJson, const ScalpMaps& maps, const GuideHierarchy* guides,
                             std::size_t vertexCount, const std::string& node, const std::string& param) {
    ParamMapBinding out;
    out.node = node;
    const ParamMapRef source = paramMapOf(graphJson, maps, guides, node, param);
    if (!source.map.empty()) {
        out.map = source.map;
        out.kind = source.kind;
        return out;
    }
    if (vertexCount == 0) throw std::runtime_error("no scalp to paint the " + param + " map on");
    // A new map, white: 1 x the param's value, as it was.
    const std::vector<std::string> layers = guides ? guides->attributeNames() : std::vector<std::string>{};
    const std::string base = sanitizeMapName(node + "_" + param);
    out.map = base;
    for (int k = 1; maps.maps.count(out.map) || std::find(layers.begin(), layers.end(), out.map) != layers.end(); ++k) {
        out.map = base + std::to_string(k);
    }
    out.kind = "vertices";
    out.maps = withVertexMap(maps, out.map, std::vector<float>(vertexCount, 1.0f), kGrayMap);
    out.graphJson = wireParamMap(graphJson, out.maps, node, param, out.map, out.kind);
    return out;
}

// ---- Map strokes ------------------------------------------------------------------------------

MapDab MapDab::of(const BrushToolSettings& s, const BrushSettings& b, int channels, bool inverted, float smoothScale) {
    MapDab d;
    d.smoothing = b.mode == BrushMode::Smooth;
    d.fromStrokeStart = !d.smoothing && b.paintMode != PaintMode::Replace;
    // Smoothing past strength 1 runs in passes (each a full blur at most).
    const float amount = d.smoothing ? s.smoothStrength * smoothScale : 0.0f;
    d.passes = d.smoothing ? std::max(1, static_cast<int>(std::ceil(amount - 1e-4f))) : 1;
    d.perPass = d.smoothing ? amount / static_cast<float>(d.passes) : b.strength;
    d.strength = b.strength;
    // A color map takes the paint color in the chosen channels; a gray map the value.
    d.target = channels == kColorMap ? s.paintTarget(inverted) : PaintColor{b.value, b.value, b.value, b.value};
    d.mask = s.paintChannels;
    d.mode = b.paintMode;
    d.falloff = falloffOf(b);
    d.mirrorAxis = b.mirrorAxis;
    d.mirrorCenter = b.mirrorCenter;
    d.mirrorTopology = b.mirrorTopology;
    return d;
}

namespace {

// One dab: what it reaches and its weights found once, for all its passes.
std::size_t dabOnce(MapCanvas& map, const ScalpMesh& scalp, const MapDab& d, const Vec3f& c, float radius,
                    const BrushWeightFn& w, bool threadSafe) {
    std::size_t changed = 0;
    if (map.vertices) {
        const VertexReach reach = vertexReach(scalp, c, radius, w);
        static const VertexNeighbors none;
        const VertexNeighbors& neighbors = map.neighbors ? *map.neighbors : none;
        for (int pass = 0; pass < d.passes; ++pass) {
            changed += d.smoothing ? smoothVertexReach(*map.vertices, map.channels, d.mask, scalp, neighbors, reach, d.perPass)
                                   : stampVertexReach(*map.vertices, map.channels, d.mask, scalp, reach, d.strength, d.target, d.mode,
                                                      d.fromStrokeStart ? map.strokeStartVertices : nullptr);
        }
    } else if (map.texture) {
        TexturePaint& paint = *map.texture;
        const TexturePaint::Reach reach = paint.reachOf(c, radius, w, threadSafe);
        for (int pass = 0; pass < d.passes; ++pass) {
            changed += d.smoothing ? paint.smoothReach(reach, d.perPass, d.mask)
                                   : paint.stampReach(reach, d.strength, d.target, d.mask, d.mode, d.fromStrokeStart);
        }
    }
    return changed;
}

}  // namespace

std::size_t paintMapDab(MapCanvas& map, const ScalpMesh& scalp, const MapDab& d, const Vec3f& center, float radius,
                        const BrushWeightFn& weight, bool threadSafe) {
    std::size_t changed = dabOnce(map, scalp, d, center, radius, weight, threadSafe);
    if (d.mirrorTopology) {  // at the partner spot on the painted mesh: a world sphere
        BrushSettings topology;
        topology.mirrorTopology = true;
        const StrokeSymmetry sym = StrokeSymmetry::of(topology, &scalp);
        const Vec3f m = sym.on() ? sym.point(center) : center;
        if ((m - center).length() > 1e-6f) changed += dabOnce(map, scalp, d, m, radius, sphereWeight(m, radius, d.falloff), true);
    } else if (d.mirrorAxis >= 0) {  // the mirrored dab: a world sphere (a screen circle is the cursor's)
        const Vec3f m = mirrorPoint(center, d.mirrorAxis, d.mirrorCenter);
        if ((m - center).length() > 1e-6f) changed += dabOnce(map, scalp, d, m, radius, sphereWeight(m, radius, d.falloff), true);
    }
    return changed;
}

std::size_t paintMapStroke(MapCanvas& map, const ScalpMesh& scalp, const MapDab& d, const std::vector<Vec3f>& path, float radius) {
    std::vector<float> vertexStart;
    const std::vector<float>* oldStart = map.strokeStartVertices;
    if (d.fromStrokeStart && map.vertices && !map.strokeStartVertices) {
        vertexStart = *map.vertices;
        map.strokeStartVertices = &vertexStart;
    }
    const bool beganTextureEdit = d.fromStrokeStart && map.texture && !map.texture->editing();
    if (beganTextureEdit) map.texture->beginEdit();
    // Dabs spaced a quarter radius apart along the path, in world space (as guide strokes).
    std::vector<Vec3f> dabs;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (i == 0) {
            dabs.push_back(path[0]);
            continue;
        }
        const Vec3f from = path[i - 1], to = path[i];
        const float length = (to - from).length();
        const int steps = std::max(1, static_cast<int>(std::ceil(length / (0.25f * radius))));
        for (int k = 1; k <= steps; ++k) dabs.push_back(from + (to - from) * (static_cast<float>(k) / static_cast<float>(steps)));
    }
    std::size_t changed = 0;
    for (const Vec3f& c : dabs) changed += paintMapDab(map, scalp, d, c, radius, sphereWeight(c, radius, d.falloff), true);
    map.strokeStartVertices = oldStart;
    if (beganTextureEdit) map.texture->endEdit();
    return changed;
}

int scalpMapChannels(const ScalpMaps& maps, const std::string& name) {
    const auto it = maps.maps.find(name);
    return it != maps.maps.end() && it->second.channels == kColorMap ? kColorMap : kGrayMap;
}

std::vector<float> vertexMapToPaint(const ScalpMaps& maps, const std::string& name, int channels, std::size_t pointCount,
                                    bool* restarted) {
    std::vector<float> values;
    const auto it = maps.maps.find(name);
    if (it != maps.maps.end() && it->second.kind == "vertices") values = it->second.values;
    if (restarted) *restarted = false;
    if (values.size() != pointCount * static_cast<std::size_t>(channels)) {
        if (restarted) *restarted = !values.empty();
        // Black (alpha 1 on a color map).
        values = convertMapChannels(std::vector<float>(pointCount, 0.0f), kGrayMap, channels);
    }
    return values;
}

ScalpMaps withVertexMap(ScalpMaps maps, const std::string& name, std::vector<float> values, int channels) {
    ScalpMapEntry entry{"vertices", std::move(values), ""};
    entry.channels = channels;
    maps.maps[name] = std::move(entry);
    return maps;
}

}  // namespace groomlab
