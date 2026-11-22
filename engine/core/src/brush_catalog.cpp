// SPDX-License-Identifier: Apache-2.0
#include "groomlab/brush_catalog.h"

#include "groomlab/presets.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace groomlab {

namespace {

// The settings a sculpting brush's card always shows.
const std::vector<std::string> kBrushCommon = {"radius", "strength", "falloff", "falloffProfile", "symmetry", "selectedOnly",
                                               "pressure", "collide", "frontFacingOnly"};
// The shaping brushes' too: their strength from root to tip.
const std::vector<std::string> kShaping = {"radius", "strength", "falloff", "falloffProfile", "strengthProfile", "symmetry",
                                           "selectedOnly", "pressure", "collide", "frontFacingOnly"};

std::vector<std::string> with(std::vector<std::string> base, std::initializer_list<const char*> more) {
    for (const char* s : more) base.emplace_back(s);
    return base;
}

std::vector<std::string> without(std::vector<std::string> base, std::initializer_list<const char*> less) {
    for (const char* s : less) base.erase(std::remove(base.begin(), base.end(), s), base.end());
    return base;
}

std::vector<BrushEntry> makeCatalog() {
    using C = BrushCategory;
    std::vector<BrushEntry> entries = {
        // Create: the tools that make guides, parts and flow.
        {"place", "Place", C::Create, 0, "place", 1.0f, false,
         "Click the scalp for a guide; drag to place many, spaced. New guides copy their neighbors' shape. "
         "With a child group picked, places children by hand (each follows the nearest parent).",
         {"symmetry", "collide", "newGuides", "spacing", "soloNewGuides", "tube"}},
        {"draw", "Draw", C::Create, 0, "draw", 1.0f, false, "Press on the scalp and draw a guide's shape.",
         {"symmetry", "collide", "newGuides", "soloNewGuides", "tube"}},
        {"clone", "Clone", C::Create, 0, "clone", 1.0f, false,
         "Ctrl+click over guides to copy the ones rooted in the circle (or Pick Source, then click); then click "
         "anywhere to set copies down, each guide keeping its shape and children, turned to the scalp there. Drag "
         "before letting go to spin the copies.",
         {"radius", "symmetry", "selectedOnly", "collide", "frontFacingOnly", "clone"}},
        {"part", "Part", C::Create, 0, "part", 1.0f, false,
         "Draw a part freehand on the scalp: hair does not mix across it, and the Part node opens it.", {"symmetry", "parts"}},
        {"flow", "Flow", C::Create, 0, "flow", 1.0f, false,
         "Drag on the scalp to lay direction arrows; drag over arrows to comb them (Ctrl: erase); or place whorls and "
         "other sinks hair turns around. The Flow node lays short hair along them: vellus, fur, brows, no guides needed.",
         {"radius", "strength", "falloff", "falloffProfile", "pressure", "frontFacingOnly", "flow"}},
        // Shape.
        {"comb", "Comb", C::Shape, 0, "comb", 1.0f, false,
         "Drag guides along the stroke, roots pinned (Stretch: they get longer or shorter as you pull).",
         with(kShaping, {"stretch", "targets"})},
        {"slide", "Slide", C::Shape, 0, "slide", 1.0f, false,
         "Move roots along the scalp; guides keep their shape (fix a root without redrawing). Strength along the "
         "guide: how much of each guide follows its root (Root Only: the root alone moves, the rest stays put)",
         with(kBrushCommon, {"strengthProfile"})},
        {"smooth", "Smooth", C::Shape, 0, "smooth", 0.5f, false,
         "Even guides out: their kinks, lengths kept (Curve), or their lengths or shapes toward their neighbors' (Length, "
         "Shape), or all three (Straighten: relax guides toward the straight line along their root direction).",
         with(kShaping, {"smoothMode", "straighten", "targets"})},
        {"lift", "Lift", C::Shape, 0, "lift", 0.5f, true, "Stand guides up toward their root normal (negative: lay them down).",
         kShaping},
        {"twist", "Swing", C::Shape, 0, "twist", 0.5f, true, "Swing guides around their root (negative: the other way).",
         with(kShaping, {"targets"})},
        {"attract", "Attract", C::Shape, 0, "attract", 0.5f, true,
         "Pull tips together toward the brush center (negative: push apart).", with(kShaping, {"targets"})},
        {"shape", "Tube", C::Shape, 0, "shape", 0.5f, true,
         "Paint guides' tubes near the brush: Width grows them (Ctrl shrinks), Flatness flattens them toward a "
         "ribbon (Ctrl rounds them), Twist turns them about the guide (Ctrl the other way); Shift smooths what it "
         "paints along the guides. Children and Fill Tubes strands follow.",
         with(without(kShaping, {"collide"}), {"tubeChannel", "tube"})},
        // Trim.
        {"cut", "Cut", C::Trim, 0, "cut", 1.0f, false, "Trim guides where the brush touches them.",
         with(kBrushCommon, {"targets"})},
        {"length", "Length", C::Trim, 0, "length", 1.0f, true, "Grow guides evenly (negative: shorten).",
         with(kBrushCommon, {"targets"})},
        {"lengths", "Length Keys", C::Trim, 0, "lengths", 1.0f, false,
         "Click on the scalp to place keys and drag them to move them. Right-drag a key up or down (Maya: middle-drag) "
         "to scale the lengths of the guides round it; the scale blends smoothly between keys over the scalp and eases "
         "back to x1 away from them (a key left at x1 holds its guides). Ctrl+click deletes a key. "
         "Apply sets the guides; the keys are kept with the groom to place again.",
         {"symmetry", "lengthKeys"}},
        {"erase", "Erase", C::Trim, 0, "erase", 1.0f, false,
         "Remove what the brush touches: guides (with their children), parts and flow arrows (choose below). Not the "
         "scattered hair itself: to take hair away, paint the density map holding Ctrl (Paint tab > + New Map > for a "
         "setting > Density).",
         with(kBrushCommon, {"erases"})},
        // Paint.
        {"paint", "Paint", C::Paint, 0, "paint", 0.5f, false,
         "Paint the map picked in the list toward the value, or a color map toward the color in the channels ticked "
         "(Ctrl: a gray map toward 0, or 1 when the value is 0; a color map toward the inverse color; Shift+drag: "
         "smooth). Guide maps paint over the guides, scalp maps over the scalp. + New Map > for a setting makes a map "
         "a setting reads (Density, Width, Length...): Ctrl on the density map removes hair.",
         with(without(kBrushCommon, {"collide"}), {"map", "alpha", "smoothStrength"})},
        // Guides: picking and protecting them.
        {"select", "Select", C::Guides, 0, "select", 1.0f, false,
         "Lasso guides (Shift: toggle, Ctrl: deselect, Ctrl+Shift: add): the rest freeze, so brushes work on the "
         "selection; a pick of nothing unfreezes every guide. With symmetry, the mirror guides are picked too.",
         {"symmetry", "selection"}},
        {"freeze", "Freeze", C::Guides, 0, "freeze", 1.0f, false,
         "Paint the freeze mask: brushes leave frozen guides alone (they draw blue). Ctrl: unfreeze.",
         with(without(kBrushCommon, {"collide"}), {"freezeMask"})},
    };
    for (std::size_t i = 0; i < entries.size(); ++i) entries[i].order = static_cast<int>(i);
    for (BrushEntry& e : entries) {
        static const std::set<std::string> kCorrective = {"comb", "slide", "smooth", "lift", "twist", "attract",
                                                         "cut", "length", "select"};
        e.corrective = kCorrective.count(e.id) > 0;
    }
    return entries;
}

}  // namespace

const char* brushCategoryName(BrushCategory category) {
    switch (category) {
        case BrushCategory::Create: return "Create";
        case BrushCategory::Shape: return "Shape";
        case BrushCategory::Trim: return "Trim";
        case BrushCategory::Paint: return "Paint";
        case BrushCategory::Guides: return "Guides";
    }
    return "";
}

const std::vector<BrushCategory>& brushCategories() {
    static const std::vector<BrushCategory> order = {BrushCategory::Create, BrushCategory::Shape, BrushCategory::Trim,
                                                     BrushCategory::Paint, BrushCategory::Guides};
    return order;
}

bool BrushEntry::hasSetting(const std::string& setting) const {
    return std::find(settings.begin(), settings.end(), setting) != settings.end();
}

const std::vector<BrushEntry>& brushCatalog() {
    static const std::vector<BrushEntry> catalog = makeCatalog();
    return catalog;
}

const BrushEntry* brushEntry(const std::string& id) {
    for (const BrushEntry& e : brushCatalog()) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

const char* brushSettingKindName(BrushSettingKind kind) {
    switch (kind) {
        case BrushSettingKind::Group: return "";
        case BrushSettingKind::Bool: return "bool";
        case BrushSettingKind::Int: return "int";
        case BrushSettingKind::Float: return "float";
    }
    return "";
}

namespace {

// A switch: on or off to start with.
BrushSettingSpec flag(BrushSettingSpec s, bool on) {
    s.kind = BrushSettingKind::Bool;
    s.defaultValue = on ? 1.0 : 0.0;
    return s;
}

// A slider: its default, the span it shows (soft) and what a typed value may reach.
BrushSettingSpec number(BrushSettingSpec s, BrushSettingKind kind, double value, double softMin, double softMax, double min, double max,
                        int decimals, const char* unit = "") {
    s.kind = kind;
    s.defaultValue = value;
    s.softMin = softMin;
    s.softMax = softMax;
    s.min = min;
    s.max = max;
    s.decimals = decimals;
    s.unit = unit;
    return s;
}

}  // namespace

const std::vector<BrushSettingSpec>& brushSettingSpecs() {
    using K = BrushSettingKind;
    // A value's default is BrushToolSettings' own and its limits hold what the settings' setters
    // keep (setScreenRadius, setSegments, setHardness, setSmoothStrength, setAlphaSpacing):
    // BrushCatalog.EveryValuesDefaultIsTheSettingsOwn.
    static const std::vector<BrushSettingSpec> specs = {
        number({"radius", "Radius", "The brush circle (middle-drag, or B held + drag, sets it)"}, K::Float, 60.0, 5.0, 400.0, 1.0,
               2000.0, 0, "pixels"),
        number({"strength", "Strength", "How much each stamp does (Comb: the fraction of the drag); negative reverses the "
                                        "tools that work both ways (Shift+middle-drag, or M held + drag, sets it)"},
               K::Float, 1.0, 0.0, 1.0, -10.0, 10.0, 2),
        number({"falloff", "Falloff", "How the brush fades toward its edge: 0 not at all (hard to the edge), 1 from the center out (soft)"},
               K::Float, 1.0, 0.0, 1.0, 0.0, 1.0, 2),
        {"falloffProfile", "Falloff curve", "The falloff drawn as a curve instead: the weight from the brush's center (left) "
                                            "to its edge (right); none: the Falloff slider's"},
        {"strengthProfile", "Root to tip", "How much of the strength each part of a guide gets, root (left) to tip (right): "
                                           "tips more than roots, or the reverse; flat: the whole guide alike"},
        {"symmetry", "Symmetry", "Mirror every stroke: across world X, or across the scalp's own topology (its left "
                                 "and right, whatever its pose; for heads turned or not quite symmetric)"},
        flag({"selectedOnly", "Selected only", "Only the selected guides, scaled by the soft selection"}, false),
        flag({"pressure", "Pen pressure", "A tablet's pressure scales the strength"}, true),
        flag({"collide", "Keep off scalp", "Guides never dip into the scalp"}, true),
        flag({"frontFacingOnly", "Front facing only", "What the scalp hides from the camera gets no weight: a stroke never reaches "
                                                      "guides, parts, flow arrows or paint on the far side of the head"},
             true),
        flag({"stretch", "Stretch", "On: guides get longer (or shorter) as you pull them, like ZBrush's Snake Hook: points are added or "
                   "removed so the segments keep their length. Off: they keep their length"}, false),
        flag({"straighten", "Straighten", "On: guides relax toward the straight line along their root direction. Off: only "
                                          "their kinks are taken out"},
             false),
        {"smoothMode", "Evens out", "What Smooth evens out: Curve the kinks along each guide; Length the guides' lengths toward "
                                       "their neighbors'; Shape their shapes toward their neighbors'; All of the three"},
        {"targets", "Acts on", "What the brush shapes: guides, part lines, flow arrows"},
        {"erases", "Erases", "What Erase removes: guides (with their children), parts, flow arrows"},
        {"newGuides", "New guides", "Match nearby guides' shape and maps, or the length and segments of a standing guide"},
        // New guides' values. The lengths are scene units on a scalp of kDefaultsScalpSize (unit "length").
        number({"length", "Length", "New guides' length (standing, or without neighbors to match)"}, K::Float, 0.5, 0.01, 50.0, 0.001,
               1e4, 3, "length"),
        number({"segments", "Segments", "Points along each new guide"}, K::Int, 12.0, 1.0, 32.0, 1.0, 256.0, 0),
        flag({"matchNeighbors", "Match nearby", "New guides take the shape, length and painted maps of the nearest guides: the "
                                                "group's own, or the groups around it while it has none. Off: they stand "
                                                "straight up, with no maps"},
             true),
        number({"spacing", "Spacing", "The distance between the guides a drag places"}, K::Float, 0.1, 0.0, 10.0, 0.0, 1e4, 3, "length"),
        flag({"soloNewGuides", "Solo new guides", "New guides stay unfrozen and every other guide is frozen, so the brushes "
                                                  "work on the new ones at once (the Select tool solos others, "
                                                  "Unfreeze All frees them all)"},
             false),
        {"clone", "Clone", "The source: Pick Source makes the next click copy the guides rooted in the circle (as "
                           "Ctrl+click does), how many it holds, and whether copies replace the guides where they land"},
        flag({"cloneReplace", "Replace guides under brush", "On: the guides of the copied levels already in the circle "
                                                            "where the copies land are removed first (frozen ones stay). "
                                                            "Off: the copies are added among them"},
             false),
        {"parts", "Parts", "The next part's name, and the parts drawn so far"},
        {"flow", "Flow", "What a click or drag lays: arrows, or a sink (brush-sized) hair turns around"},
        {"map", "Map", "The map painted, the value (or color and channels) it is painted toward"},
        number({"smoothStrength", "Smooth", "How much Shift+drag smooths a map per stamp (past 1: several passes)"}, K::Float, 1.0, 0.0,
               3.0, 0.0, 100.0, 2),
        {"alpha", "Alpha", "An image stamped inside the brush on scalp maps (speckles, noise, streaks): white paints, black "
                           "does not. Rotation turns it (fixed, along the stroke, random per stamp), Spacing sets how far "
                           "apart the stamps fall. Add your own images to the library (right-click)"},
        // The alpha's values: the spacing a share of the brush's diameter (hosts show percent).
        number({"alphaAngle", "Stamp angle", "The alpha's angle in degrees, counterclockwise (Follow Stroke: added to the "
                                             "stroke's way)"},
               K::Float, 0.0, 0.0, 360.0, -360.0, 360.0, 0, "angle"),
        number({"alphaSpacing", "Stamp spacing", "How far apart the stamps fall along a stroke, in percent of the brush's "
                                                 "diameter: low for a smooth trail, 100 and up for separate stamps"},
               K::Float, 0.25, 0.05, 1.0, 0.02, 4.0, 2, "fraction"),
        flag({"alphaInvert", "Invert", "Paint where the alpha is black instead"}, false),
        {"selection", "Selection", "How the lasso adds to or replaces the selection, and the soft radius"},
        {"freezeMask", "Freeze mask", "The frozen guides draw blue; Unfreeze All clears the mask"},
        {"tubeChannel", "Paints", "What the Tube brush paints on the tubes near it: their width, their flatness (toward "
                                  "a flat ribbon) or their twist about the guide; a negative strength does the opposite"},
        {"tube", "Tube", "The selected (or soloed) guides' tubes as a whole: their shape, width, flatness and twist, "
                         "the groom's shape library (custom outlines, drawn in the shape editor), the shape new guides "
                         "take (Place, Draw), and the groom's Fill Tubes (strands fill the tubes) and Tube Blend (the "
                         "soft edge round them)"},
        {"lengthKeys", "Keys", "The selected key's scale and the length it gives the guide nearest it, how guides take "
                               "a new length (scale: the shape scaled, curls kept; trim: cut or extended at the tip), "
                               "Delete Key, Clear, and Apply (sets the guides, one undo step) or Cancel. Only the "
                               "selected guides change, else the soloed ones (those not frozen)"},
    };
    return specs;
}

const BrushSettingSpec* brushSettingSpec(const std::string& id) {
    for (const BrushSettingSpec& s : brushSettingSpecs()) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

std::vector<const BrushEntry*> brushesInCategory(BrushCategory category) {
    std::vector<const BrushEntry*> out;
    for (const BrushEntry& e : brushCatalog()) {
        if (e.category == category) out.push_back(&e);
    }
    return out;
}

double brushLengthScale(double scalpSize) {
    if (!(scalpSize > 0.0)) return 1.0;
    const double k = unitScaleFactor("length", scalpSize / kDefaultsScalpSize).value_or(1.0);
    return k > 0.0 ? std::pow(10.0, std::round(std::log10(k))) : 1.0;
}

}  // namespace groomlab
