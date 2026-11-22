// SPDX-License-Identifier: Apache-2.0
#include "groomlab/editor/tool_settings.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "groomlab/brush.h"
#include "groomlab/flow.h"
#include "groomlab/scalp_paint.h"

namespace groomlab {

using nlohmann::json;

namespace {

float number(const json& v, const std::string& key) {
    if (!v.is_number()) throw std::invalid_argument("setting " + key + " is a number");
    const double d = v.get<double>();
    if (!std::isfinite(d)) throw std::invalid_argument("setting " + key + " must be finite");
    return static_cast<float>(d);
}

bool boolean(const json& v, const std::string& key) {
    if (!v.is_boolean()) throw std::invalid_argument("setting " + key + " is true or false");
    return v.get<bool>();
}

int whole(const json& v, const std::string& key) {
    if (!v.is_number_integer()) throw std::invalid_argument("setting " + key + " is a whole number");
    return v.get<int>();
}

std::string text(const json& v, const std::string& key) {
    if (v.is_null()) return {};
    if (!v.is_string()) throw std::invalid_argument("setting " + key + " is a string");
    return v.get<std::string>();
}

// A ramp as JSON or its text; null or "" for none.
std::string ramp(const json& v) { return v.is_string() ? v.get<std::string>() : v.is_null() ? std::string() : v.dump(); }

}  // namespace

void applyToolSettings(BrushToolSettings& settings, const json& given, const ToolSpace& space) {
    if (!given.is_object()) throw std::invalid_argument("the settings are an object");
    BrushToolSettings s = settings;
    const auto lengthIn = [&](double v) { return space.lengthIn ? space.lengthIn(v) : static_cast<float>(v); };
    std::string symmetry;
    double symmetryCenter = 0.0;
    bool symmetrySet = false;
    if (space.symmetryOut) {
        space.symmetryOut(s.brush.mirrorAxis, s.brush.mirrorCenter, symmetry, symmetryCenter);
    } else {
        symmetry = symmetryAxisName(s.brush.mirrorAxis);
        symmetryCenter = s.brush.mirrorCenter;
    }
    if (s.brush.mirrorTopology) symmetry = "topology";  // (a center alone keeps it)
    // The mode first: the others are the mode's settings.
    if (given.contains("mode")) s.setMode(text(given["mode"], "mode"));
    for (const auto& [key, v] : given.items()) {
        if (key == "mode") continue;
        if (key == "radius") s.setScreenRadius(number(v, key));
        else if (key == "strength") s.brush.strength = number(v, key);
        else if (key == "falloff") s.setHardness(1.0f - number(v, key));
        else if (key == "hardness") s.setHardness(number(v, key));
        else if (key == "falloffProfile") s.setFalloffProfile(ramp(v));
        else if (key == "strengthProfile") s.setStrengthProfile(ramp(v));
        else if (key == "level") s.brush.level = whole(v, key);
        else if (key == "group") s.brush.group = text(v, key);
        else if (key == "propagate") s.brush.propagate = boolean(v, key);
        else if (key == "length") s.place.length = lengthIn(number(v, key));
        else if (key == "segments") s.setSegments(whole(v, key));
        else if (key == "tubeRadius") {
            const float r = lengthIn(number(v, key));
            s.place.tube.width = {r, r};
        } else if (key == "tube" || key == "placeTube") {
            // The new guides' tube (Maya's placeTube): shape, width, aspect, twist (its JSON or text).
            const json tube = v.is_string() ? json::parse(v.get<std::string>(), nullptr, false) : v;
            if (!tube.is_object()) throw std::invalid_argument("setting tube is an object: shape, width, aspect, twist");
            setPlacementTube(s.place, space.tubeIn ? space.tubeIn(tube) : tube.dump());
        } else if (key == "tubeChannel") s.brush.tubeChannel = tubeChannelFromString(text(v, key));
        else if (key == "smoothMode") s.brush.smoothMode = smoothModeFromString(text(v, key));
        else if (key == "spacing") s.place.spacing = lengthIn(number(v, key));
        else if (key == "matchNeighbors") s.place.matchNeighbors = boolean(v, key);
        else if (key == "soloNewGuides") s.place.soloNewGuides = boolean(v, key);
        else if (key == "selectedOnly") s.selectedOnly = boolean(v, key);
        else if (key == "symmetry") {
            symmetry = text(v, key);
            if (symmetry.empty()) symmetry = "none";
            symmetrySet = true;
        } else if (key == "symmetryCenter") {
            symmetryCenter = number(v, key);
            symmetrySet = true;
        } else if (key == "attribute") s.brush.attribute = text(v, key);
        else if (key == "value") s.brush.value = number(v, key);
        else if (key == "pointwise") s.brush.pointwise = boolean(v, key);
        else if (key == "collide") s.brush.collide = boolean(v, key);
        else if (key == "frontFacingOnly" || key == "maskByScalp") s.maskByScalp = boolean(v, key);
        else if (key == "stretch") s.brush.stretch = boolean(v, key);
        else if (key == "straighten") s.brush.straighten = boolean(v, key);
        else if (key == "pressure") s.pressure = boolean(v, key);
        else if (key == "partName") s.partName = text(v, key).empty() ? std::string("part") : text(v, key);
        else if (key == "flowSink") {
            const std::string sink = text(v, key);
            if (!sink.empty()) flowSinkAngle(sink);  // throws for an unknown kind
            s.flowSink = sink;
        } else if (key == "targets") {
            if (v.is_array()) {
                std::string names;
                for (const json& n : v) names += (names.empty() ? "" : ",") + text(n, key);
                s.setTargets(names.empty() ? std::string("none") : names);
            } else {
                s.setTargets(text(v, key));
            }
        } else if (key == "eraseGuides") s.eraseGuides = boolean(v, key);
        else if (key == "eraseParts") s.eraseParts = boolean(v, key);
        else if (key == "eraseFlow") s.eraseFlow = boolean(v, key);
        else if (key == "paintKind") {
            const std::string kind = text(v, key);
            if (kind != "guides" && kind != "vertices" && kind != "texture") throw std::invalid_argument("paintKind is guides, vertices or texture");
            s.paintKind = kind;
        } else if (key == "paintTexture") s.paintTexture = text(v, key);
        else if (key == "paintResolution") s.setPaintResolution(whole(v, key));
        else if (key == "paintColor") {
            if (!v.is_array() || (v.size() != 3 && v.size() != 4)) throw std::invalid_argument("paintColor is [r, g, b] or [r, g, b, a]");
            PaintColor c{0.0f, 0.0f, 0.0f, 1.0f};
            for (std::size_t i = 0; i < v.size(); ++i) c[i] = number(v[i], key);
            s.setPaintColor(c);
        } else if (key == "paintChannels") {
            try {
                s.paintChannels = channelMask(text(v, key).empty() ? std::string("rgba") : text(v, key));
            } catch (const std::invalid_argument& e) {
                throw std::invalid_argument(std::string("paintChannels: ") + e.what());
            }
        } else if (key == "paintMode") s.brush.paintMode = paintModeFromString(text(v, key));
        else if (key == "paintWholeFaces") s.paintWholeFaces = boolean(v, key);
        else if (key == "smoothStrength") s.setSmoothStrength(number(v, key));
        else if (key == "alpha") {
            try {
                s.setAlpha(text(v, key));
            } catch (const std::invalid_argument&) {
                throw;
            } catch (const std::exception& e) {
                throw std::invalid_argument(std::string("alpha: ") + e.what());
            }
        } else if (key == "alphaRotation") s.setAlphaRotation(text(v, key));
        else if (key == "alphaAngle") s.alphaAngle = number(v, key);
        else if (key == "alphaSpacing") s.setAlphaSpacing(number(v, key));
        else if (key == "alphaInvert") s.alphaInvert = boolean(v, key);
        else throw std::invalid_argument("no tool setting '" + key + "'");
    }
    if (symmetrySet && symmetry == "topology") {
        s.setSymmetry("topology");  // across the scalp's own surface: no axis to turn into the groom's
    } else if (symmetrySet) {
        if (space.symmetryIn) {
            space.symmetryIn(symmetry, symmetryCenter, s.brush.mirrorAxis, s.brush.mirrorCenter);
        } else {
            s.brush.mirrorAxis = symmetryAxisFromName(symmetry);
            s.brush.mirrorCenter = static_cast<float>(symmetryCenter);
        }
        s.brush.mirrorTopology = false;
    }
    settings = std::move(s);
}

void setToolSetting(BrushToolSettings& s, const std::string& id, const json& value, const ToolSpace& space) {
    applyToolSettings(s, json{{id, value}}, space);
}

json toolSettingsJson(const BrushToolSettings& s, const ToolSpace& space) {
    const auto lengthOut = [&](float v) { return space.lengthOut ? space.lengthOut(v) : static_cast<double>(v); };
    std::string axis;
    double center = 0.0;
    if (space.symmetryOut) {
        space.symmetryOut(s.brush.mirrorAxis, s.brush.mirrorCenter, axis, center);
    } else {
        axis = symmetryAxisName(s.brush.mirrorAxis);
        center = s.brush.mirrorCenter;
    }
    json j;
    j["radius"] = s.screenRadius;
    j["strength"] = s.brush.strength;
    j["falloff"] = 1.0f - s.brush.hardness;
    j["hardness"] = s.brush.hardness;
    j["falloffProfile"] = s.brush.falloffProfile ? json::parse(s.falloffProfileJson()) : json();
    j["strengthProfile"] = s.brush.strengthProfile ? json::parse(s.strengthProfileJson()) : json();
    j["level"] = s.brush.level;
    j["group"] = s.brush.group;
    j["propagate"] = s.brush.propagate;
    j["length"] = lengthOut(s.place.length);
    j["segments"] = s.place.segments;
    j["tubeRadius"] = lengthOut(s.place.tube.width.empty() ? 0.0f : s.place.tube.width.front());
    j["tube"] = space.tubeOut ? space.tubeOut(placementTubeJson(s.place)) : json::parse(placementTubeJson(s.place));
    j["tubeChannel"] = tubeChannelName(s.brush.tubeChannel);
    j["smoothMode"] = smoothModeName(s.brush.smoothMode);
    j["spacing"] = lengthOut(s.place.spacing);
    j["matchNeighbors"] = s.place.matchNeighbors;
    j["soloNewGuides"] = s.place.soloNewGuides;
    j["selectedOnly"] = s.selectedOnly;
    j["symmetry"] = s.brush.mirrorTopology ? std::string("topology") : axis;
    j["symmetryCenter"] = center;
    j["attribute"] = s.brush.attribute;
    j["value"] = s.brush.value;
    j["pointwise"] = s.brush.pointwise;
    j["collide"] = s.brush.collide;
    j["frontFacingOnly"] = s.maskByScalp;
    j["stretch"] = s.brush.stretch;
    j["straighten"] = s.brush.straighten;
    j["pressure"] = s.pressure;
    j["partName"] = s.partName;
    j["flowSink"] = s.flowSink;
    j["targets"] = s.targetsName();
    j["eraseGuides"] = s.eraseGuides;
    j["eraseParts"] = s.eraseParts;
    j["eraseFlow"] = s.eraseFlow;
    j["paintKind"] = s.paintKind;
    j["paintTexture"] = s.paintTexture;
    j["paintResolution"] = s.paintResolution;
    j["paintColor"] = s.paintColor;
    j["paintChannels"] = channelLetters(s.paintChannels);
    j["paintMode"] = paintModeName(s.brush.paintMode);
    j["paintWholeFaces"] = s.paintWholeFaces;
    j["smoothStrength"] = s.smoothStrength;
    j["alpha"] = s.alphaPath;
    j["alphaRotation"] = s.alphaRotationName();
    j["alphaAngle"] = s.alphaAngle;
    j["alphaSpacing"] = s.alphaSpacing;
    j["alphaInvert"] = s.alphaInvert;
    return j;
}

float dragToolSetting(BrushToolSettings& s, const std::string& setting, float start, float dx, float pixelScale) {
    if (setting == "radius") {
        const float scale = pixelScale > 0.0f ? pixelScale : 1.0f;
        s.setScreenRadius(std::clamp(start + dx, 5.0f * scale, 2000.0f * scale));
        return s.screenRadius;
    }
    if (setting == "strength") {
        s.brush.strength = std::clamp(start + dx * 0.01f, -10.0f, 10.0f);
        return s.brush.strength;
    }
    throw std::invalid_argument("a drag sets the radius or the strength, not '" + setting + "'");
}

void pickTool(BrushToolSettings& s, const std::string& id, FreezeKept& kept) {
    const bool freezing = pickedTool(s) == "freeze";
    if (id == "freeze") {
        if (!freezing) kept = {s.brush.attribute, s.brush.value, s.paintKind, s.paintTexture};
        s.setMode("paint");
        s.brush.freeze = "freeze";
        s.brush.attribute = "freeze";
        s.brush.value = 1.0f;
        s.paintKind = "guides";
        s.paintTexture.clear();
        return;
    }
    BrushToolSettings().setMode(id);  // throws for an unknown id, before anything changes
    if (freezing) {
        s.brush.attribute = kept.attribute;
        s.brush.value = kept.value;
        s.paintKind = kept.kind;
        s.paintTexture = kept.texture;
    }
    s.setMode(id);
}

std::string pickedTool(const BrushToolSettings& s) {
    if (s.tool == BrushToolSettings::Tool::Brush && s.brush.mode == BrushMode::Attribute && s.brush.attribute == "freeze" &&
        s.paintKind == "guides") {
        return "freeze";
    }
    return s.modeName();
}

}  // namespace groomlab
