// SPDX-License-Identifier: Apache-2.0
#pragma once

// The groom editor's tool settings (engine/editor), every host's: the brush tool's settings set and
// read by id (the brush catalog's ids, Maya's groomLabBrushCtx flags by their long names, the Blender
// helper's settings keys), one parser for every host.

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "groomlab/stroke.h"

namespace groomlab {

// How a host's values become the groom's: its lengths (a scene's units, Blender's meters) and its
// axes (Blender's Z up). Null: as they are.
struct ToolSpace {
    std::function<float(double)> lengthIn;    // a host length as the groom's
    std::function<double(float)> lengthOut;   // a groom length as the host's
    // A symmetry plane given as an axis name and a center (host's) as the groom's axis and center;
    // and back.
    std::function<void(const std::string& axis, double center, int& groomAxis, float& groomCenter)> symmetryIn;
    std::function<void(int groomAxis, float groomCenter, std::string& axis, double& center)> symmetryOut;
    // A tube edit (editedTube's JSON, widths in host units) as the groom's text; and back.
    std::function<std::string(const nlohmann::json& tube)> tubeIn;
    std::function<nlohmann::json(const std::string& tube)> tubeOut;
};

// Settings set by id from a JSON object, every host's: radius (the ring's pixels), strength,
// falloff (1 - hardness) or hardness, falloffProfile / strengthProfile (ramps, or their JSON text;
// null for none), mode, level, group, propagate, length, segments, tubeRadius, tube (the new
// guides' tube), tubeChannel, smoothMode, spacing, matchNeighbors, soloNewGuides, selectedOnly,
// symmetry (x, y, z, topology, none) and symmetryCenter, attribute, value, pointwise, collide,
// frontFacingOnly (maskByScalp), stretch, straighten, pressure, partName, flowSink, targets (a name
// list or its text), eraseGuides / eraseParts / eraseFlow, paintKind, paintTexture,
// paintResolution, paintColor ([r, g, b(, a)]), paintChannels, paintMode, paintWholeFaces,
// smoothStrength, alpha, alphaRotation, alphaAngle, alphaSpacing, alphaInvert. All or nothing:
// throws std::invalid_argument for an id it does not have or a value a setting cannot take, `s` as
// it was.
void applyToolSettings(BrushToolSettings& s, const nlohmann::json& settings, const ToolSpace& space = {});
// One setting by id (applyToolSettings of {id: value}).
void setToolSetting(BrushToolSettings& s, const std::string& id, const nlohmann::json& value, const ToolSpace& space = {});
// Every setting applyToolSettings takes, as it reads them now (hardness and falloff both; the
// symmetry and lengths in the host's space).
nlohmann::json toolSettingsJson(const BrushToolSettings& s, const ToolSpace& space = {});

}  // namespace groomlab
