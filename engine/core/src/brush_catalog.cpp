// SPDX-License-Identifier: Apache-2.0
#include "groomlab/brush_catalog.h"

#include <algorithm>

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
         "Move roots along the scalp; guides keep their shape (fix a root without redrawing).", kBrushCommon},
        {"smooth", "Smooth", C::Shape, 0, "smooth", 0.5f, false, "Take kinks out, lengths kept.", with(kShaping, {"targets"})},
        {"straighten", "Straighten", C::Shape, 0, "straighten", 0.5f, false,
         "Relax guides toward the straight line along their root direction.", with(kShaping, {"targets"})},
        {"lift", "Lift", C::Shape, 0, "lift", 0.5f, true, "Stand guides up toward their root normal (negative: lay them down).",
         kShaping},
        {"twist", "Twist", C::Shape, 0, "twist", 0.5f, true, "Swing guides around their root (negative: the other way).",
         with(kShaping, {"targets"})},
        {"curl", "Curl", C::Shape, 0, "curl", 0.5f, true, "Curl guides toward the tip (negative: the other way).", kShaping},
        {"attract", "Attract", C::Shape, 0, "attract", 0.5f, true,
         "Pull tips together toward the brush center (negative: push apart).", with(kShaping, {"targets"})},
        {"direction", "Direction", C::Shape, 0, "direction", 0.5f, false,
         "Turn guides about their roots toward the way you stroke, lengths kept (Ctrl: away from it).", kShaping},
        {"noise", "Noise", C::Shape, 0, "noise", 0.5f, true,
         "Add smooth random jitter along guides, the same pattern each stroke builds up (negative or Ctrl: smooth it out).",
         kShaping},
        {"shape", "Shape", C::Shape, 0, "shape", 0.5f, true,
         "Paint guides' tubes near the brush: Width grows them (Ctrl shrinks), Flatness flattens them toward a "
         "ribbon (Ctrl rounds them), Twist turns them about the guide (Ctrl the other way); Shift smooths what it "
         "paints along the guides. Children and Fill Tubes strands follow.",
         with(without(kShaping, {"collide"}), {"tubeChannel", "tube"})},
        // Trim.
        {"cut", "Cut", C::Trim, 0, "cut", 1.0f, false, "Trim guides where the brush touches them.",
         with(kBrushCommon, {"targets"})},
        {"length", "Length", C::Trim, 0, "length", 1.0f, true, "Grow guides evenly (negative: shorten).",
         with(kBrushCommon, {"targets"})},
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

const std::vector<BrushSettingSpec>& brushSettingSpecs() {
    static const std::vector<BrushSettingSpec> specs = {
        {"radius", "Radius", "The brush circle (middle-drag, or B held + drag, sets it)"},
        {"strength", "Strength", "How much each stamp does (Comb: the fraction of the drag); negative reverses the "
                                 "tools that work both ways (Shift+middle-drag, or M held + drag, sets it)"},
        {"falloff", "Falloff", "How the brush fades toward its edge: 0 not at all (hard to the edge), 1 from the center out (soft)"},
        {"falloffProfile", "Falloff curve", "The falloff drawn as a curve instead: the weight from the brush's center (left) "
                                            "to its edge (right); none: the Falloff slider's"},
        {"strengthProfile", "Root to tip", "How much of the strength each part of a guide gets, root (left) to tip (right): "
                                           "tips more than roots, or the reverse; flat: the whole guide alike"},
        {"symmetry", "Symmetry", "Mirror every stroke: across world X, or across the scalp's own topology (its left "
                                 "and right, whatever its pose; for heads turned or not quite symmetric)"},
        {"selectedOnly", "Selected only", "Only the selected guides, scaled by the soft selection"},
        {"pressure", "Pen pressure", "A tablet's pressure scales the strength"},
        {"collide", "Keep off scalp", "Guides never dip into the scalp"},
        {"frontFacingOnly", "Front facing only", "What the scalp hides from the camera gets no weight: a stroke never reaches "
                                                 "guides, parts, flow arrows or paint on the far side of the head"},
        {"stretch", "Stretch", "On: guides get longer (or shorter) as you pull them. Off: they keep their length"},
        {"targets", "Acts on", "What the brush shapes: guides, part lines, flow arrows"},
        {"erases", "Erases", "What Erase removes: guides (with their children), parts, flow arrows"},
        {"newGuides", "New guides", "Match nearby guides' shape and maps, or the length and segments of a standing guide"},
        {"spacing", "Spacing", "The distance between the guides a drag places"},
        {"soloNewGuides", "Solo new guides", "New guides stay unfrozen and every other guide is frozen, so the brushes "
                                             "work on the new ones at once (Shift+click guides with a brush to solo them, "
                                             "Unfreeze All to free them all)"},
        {"parts", "Parts", "The next part's name, and the parts drawn so far"},
        {"flow", "Flow", "What a click or drag lays: arrows, or a sink (brush-sized) hair turns around"},
        {"map", "Map", "The map painted, the value (or color and channels) it is painted toward"},
        {"smoothStrength", "Smooth", "How much Shift+drag smooths a map per stamp (past 1: several passes)"},
        {"alpha", "Alpha", "An image stamped inside the brush on scalp maps (speckles, noise, streaks): white paints, black "
                           "does not. Rotation turns it (fixed, along the stroke, random per stamp), Spacing sets how far "
                           "apart the stamps fall. Add your own images to the library (right-click)"},
        {"selection", "Selection", "How the lasso adds to or replaces the selection, and the soft radius"},
        {"freezeMask", "Freeze mask", "The frozen guides draw blue; Unfreeze All clears the mask"},
        {"tubeChannel", "Paints", "What the Shape brush paints on the tubes near it: their width, their flatness (toward "
                                  "a flat ribbon) or their twist about the guide; a negative strength does the opposite"},
        {"tube", "Tube", "The selected (or soloed) guides' tubes as a whole: their shape, width, flatness and twist, "
                         "the groom's shape library (custom outlines, drawn in the shape editor), the shape new guides "
                         "take (Place, Draw), and the groom's Fill Tubes (strands fill the tubes) and Tube Blend (the "
                         "soft edge round them)"},
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

}  // namespace groomlab
