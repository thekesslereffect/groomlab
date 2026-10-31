// SPDX-License-Identifier: Apache-2.0
#pragma once

// The brush catalog: every tool an artist picks in a host's palette (Maya's Groom Tools tiles,
// the standalone app's brush rail), in one place, so both hosts show the same tools under the
// same names in the same order, with the same hints, default strengths and settings. The ids
// are the modes BrushToolSettings::setMode takes (stroke.h), plus "select" (the lasso) and
// "freeze" (the freeze mask), which the hosts start as their own tools. One stroke mode is left
// out on purpose: "clump" (the Paint brush on the clump attribute), which scripts still take.
//
// A tool's settings name what its card shows (BrushSettingSpec: the label and the hint every
// host uses, and the direction a number runs in): "radius", "strength" (signed for the tools
// that work both ways: lift, twist, attract, length, shape), "falloff" (0 hard: full
// strength to the brush's edge, 1 soft: fading from the center; BrushSettings::hardness is
// 1 - falloff), "falloffProfile" (the falloff as a curve, center to edge), "strengthProfile"
// (the shaping brushes' strength root to tip), "symmetry", "selectedOnly", "pressure", "collide" (keep off scalp),
// "frontFacingOnly" (what the scalp hides gets no weight), "stretch" (Comb),
// "targets" (what the shaping brushes act on: guides, parts, flow), "erases", "newGuides"
// (Place and Draw: match neighbors, length, segments), "spacing" (Place), "parts", "flow",
// "map" (Paint: the map, the value, the channels), "alpha" (Paint on a scalp map: the image stamped
// in the brush, its rotation and spacing; brush_alpha.h), "smoothStrength", "selection", "freezeMask",
// "tubeChannel" (Shape: width, flatness or twist), "tube" (Shape: the selected guides' tubes as a
// whole, Fill Tubes and Tube Blend).
// A setting's map (density, width...) is painted with Paint: + New Map > for a setting makes and
// links it (stroke.h, paintableSettings, paintParamMap).

#include <string>
#include <vector>

namespace groomlab {

// The palette's sections, in order: brushCategoryName gives the heading.
enum class BrushCategory { Create, Shape, Trim, Paint, Guides };
const char* brushCategoryName(BrushCategory category);
const std::vector<BrushCategory>& brushCategories();

struct BrushEntry {
    std::string id;         // the mode (BrushToolSettings::setMode), or select, freeze
    std::string label;      // what the tile and the rail show
    BrushCategory category;
    int order;              // the position in the catalog (0 first)
    std::string icon;       // the icon's stem: groomLab_<icon>.svg in every host
    float defaultStrength;  // what the strength starts at when the tool is picked
    bool signedStrength;    // the strength runs -1..1 (negative: the other way)
    std::string hint;       // one line: what the tool does, shown under its name
    std::vector<std::string> settings;  // the settings its card shows (BrushSettingSpec ids)
    // It works on a corrective (the Correctives tab shows it): the shaping brushes, whose strokes
    // record as point offsets, and Select. Not the tools that add or remove things, paint
    // attributes or tubes (BrushTool refuses those while a corrective is picked).
    bool corrective = false;
    bool hasSetting(const std::string& setting) const;
};

struct BrushSettingSpec {
    std::string id;
    std::string label;  // the caption every host shows
    std::string hint;   // the tooltip, with the direction a number runs in
};

// Every tool in the palette's order, grouped by category (Create, Shape, Trim, Paint, Guides).
const std::vector<BrushEntry>& brushCatalog();
// A tool by id; null for an unknown one.
const BrushEntry* brushEntry(const std::string& id);
// The settings the tools' cards show, by id.
const std::vector<BrushSettingSpec>& brushSettingSpecs();
const BrushSettingSpec* brushSettingSpec(const std::string& id);
// The tools of one category, in order.
std::vector<const BrushEntry*> brushesInCategory(BrushCategory category);

}  // namespace groomlab
