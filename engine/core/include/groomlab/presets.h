// SPDX-License-Identifier: Apache-2.0
#pragma once

// Graph presets: graphs saved as JSON files, independent of any scene (hosts/maya/presets ships
// them; a user's own go in their preset folder). Every host reads the same files through this:
// the Maya host's Python (groomlab/presets.py, the Graph Editor's Presets menu) and the
// standalone app's node graph, so a preset saved in one shows in the other and fits the same way.
//
// A preset is a graph (graph_io.h) plus a few fields of its own, all optional:
//   "size"       the scalp size it was made on (the largest extent of the scalp's bounding box).
//                Applied to a bigger or smaller scalp, it is fitted (fitPresetJson): lengths and
//                widths scale with the size, densities with its inverse square, per-length rates
//                (curls, braid crossings...) with its inverse, so a groom made on a head looks
//                the same on a 1 cm test sphere.
//   "about"      {"title", "description", "category"}: what the Presets menu shows (the category
//                as a submenu, presetCategories; the description as the item's tooltip).
//   "requires"   {"maps": [{"name", "kind", "value", "color"}]}: painted maps the graph reads,
//                made when the groom has none of that name; "groups": [{"name", "draw"}]: guide
//                groups the graph reads (a Guides node's group), made empty when the groom has
//                none of that name. With "draw" true a host with a Draw tool picks it, drawing
//                into that group (a hand-placed look: each guide drawn is a feather).
//   "backdrops"  the node editors' backdrops framing parts of the graph (layout only).
//   "controls"   {"ctl_<name>": value or [x, y, z]}: the values of the groom's controls (Control
//                nodes, keyable Expression variables, keyed params), set after the graph is applied.
//   "exposed"    [{"node", "param", "name"}]: the look's key params, which the Compound made by
//                inserting the preset (insertPresetJson) shows on its own panel, in that order of
//                choice ("name" optional: the param's name). Entries naming a node outside the
//                look (its Scatter, Guides...) or a param its operator has not are skipped. A
//                preset without the list gets a default pick (insertPresetJson).
//
// Presets are looked up, in order, in every directory on GROOMLAB_PRESET_PATH, the user's
// preset folder (GROOMLAB_USER_PRESETS, else Maya's user folder: Documents/maya/groomlab/presets
// on Windows, where both hosts save), and the folder shipped with the product; a preset in an
// earlier directory hides one of the same name in a later one.

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace groomlab {

class ScalpMesh;

struct PresetInfo {
    std::string name;         // the file's stem: what the functions take
    std::string title;        // "about".title, else the name in words ("wet_hair" -> "Wet hair")
    std::string description;  // "about".description ("" none; "Cannot be read: ..." for a broken file)
    std::string category;     // "about".category ("" none)
    std::string path;
};

// The Presets menu's submenus, in this order (a category not listed follows them).
const std::vector<std::string>& presetCategories();
// A preset's own fields: not part of the graph a groom stores.
const std::vector<std::string>& presetFields();

// The user's preset folder: $GROOMLAB_USER_PRESETS when set, else Maya's user application folder
// (Documents/maya on Windows, Library/Preferences/Autodesk/maya on macOS, ~/maya on Linux) with
// groomlab/presets under it: the folder the Maya host saves to. "" when no home is known.
std::string presetUserDir();
// Where presets are looked up, in order: $GROOMLAB_PRESET_PATH's directories, `userDir` (when
// not empty), `shippedDir` (when not empty).
std::vector<std::string> presetSearchDirs(const std::string& userDir, const std::string& shippedDir);
// Every preset found in `dirs` (the first directory holding a name wins), sorted by name, each
// with what the menu shows of it (its "about").
std::vector<PresetInfo> listPresets(const std::vector<std::string>& dirs);
// The file of a preset by name in `dirs`; "" when none.
std::string presetPath(const std::string& name, const std::vector<std::string>& dirs);
// A preset's title when its file gives none: its name in words.
std::string presetTitle(const std::string& name);
// What the menu shows of the preset at `path`, named `name`.
PresetInfo presetInfo(const std::string& name, const std::string& path);
// A preset's name may name a file: letters, digits, spaces, _ and - (no path separators). "" or why not.
std::string presetNameError(const std::string& name);

// The scalp size every operator's spec defaults are authored at (presetScalpSize's measure, scene
// units: centimeters in Maya), as the shipped hair presets record theirs: a head.
constexpr double kDefaultsScalpSize = 20.0;

// The preset's JSON with its sizes scaled from the scalp size it records to `size`: params in
// length or width units (numbers, every component of a vector, and the min and max of maps,
// inside compounds too, and a compound's exposed params by the inner params they set) by
// size / recorded, densities by its inverse square, per-length rates (unit "perLength") by its
// inverse. A param's unit is its spec's (ParamSpec::unit) while its unitWhen holds (Noise's
// frequency in world space only); an Expression's variable that says its unit
// ({"value": 2, "unit": "length"}) fits by it. Field nodes plugged into a param stay: the param's
// own number times the field is what it takes (field.h), so their ranges are factors. A param of
// those units a node leaves at its spec default is set to the default fitted the same way (the
// preset was made with it). The "controls" fit as what they set: keyed params' as their params,
// keyable Expression variables' by their unit. Unchanged without both sizes. The "size" key is
// dropped either way. Throws std::runtime_error for text that is not a JSON object.
std::string fitPresetJson(const std::string& presetJson, std::optional<double> size);
// The default graph (defaultGraphJson, graph_io.h) with its nodes' defaults fitted from
// kDefaultsScalpSize to `size` (nullopt: as it is): what a host stores for a new groom on a scalp,
// so a groom made on a 2 m creature or a 2 cm test sphere looks as one made on a head does.
std::string defaultGraphJsonFor(std::optional<double> size);
// The params a new node of `type` starts with on a groom whose scalp is `size` (presetScalpSize;
// nullopt: as they are): `paramsJson` (a JSON object, "" none) as given, and each spec default it
// leaves out whose unit scales (length, width, density, perLength) fitted from
// kDefaultsScalpSize to `size` as a preset's params are. Returns a JSON object; std::runtime_error
// for params that are not one.
std::string fitNewNodeParamsJson(const std::string& type, const std::string& paramsJson, std::optional<double> size);
// The graph a groom stores from a preset: fitted to `size`, the preset's own fields dropped,
// its "backdrops" kept in the graph (every host's editor keeps the layout there, graph_io.h).
std::string presetGraphJson(const std::string& presetJson, std::optional<double> size);
// The control values a preset records ({"ctl_<name>": value}), fitted to `size`, as JSON text
// ("{}" for none).
std::string presetControlsJson(const std::string& presetJson, std::optional<double> size);
// A graph with a preset's control values set where the graph itself holds them: a Control
// node's "value" (by the node's "name"), a keyable Expression variable's "value", a keyed param's
// own value (by its "paramControls"); inside compounds too. What the host keeps as its own
// attributes (Maya's ctl_* plugs) is the host's to set. Returns the graph's JSON.
std::string applyPresetControlsToGraph(const std::string& graphJson, const std::string& controlsJson);

// A preset inserted into a graph instead of replacing it: the preset's look (every node between
// its sources and its Output: the chain the Output's strands come from, back to the Scatter,
// Interpolate, Flow or other grow or source node the look starts from, with the nodes feeding
// it such as guides made from its strands) becomes one Compound node named `name` (made unique; "":
// "look") placed after node `after` ("" or the graph's Output: before the Output), fed by it
// and feeding what it fed; what the look read of the preset's other nodes (its Scalp, its
// Guides) the Compound takes as inputs, wired to this graph's node of the same type. The
// preset is fitted to `size` first (fitPresetJson). The Compound exposes the look's key params
// (its definition's "expose", each set on the Compound to the inner node's fitted value), so it
// is edited, keyed and saved as one node: the preset's "exposed" list, else a default pick, one
// param per look node for up to six nodes, the nodes of the types Clump, Curl, Noise, Braid,
// Tangle, Flyaway, Gravity, Bend, Length, HairColor, Width first (in that order), then the
// others in the graph's order (no field or utility nodes): each one's first float or int param
// that is not advanced and holds a number (no map, no field port, not keyed). A name the
// Compound already has (definition, repeat, an input's, an earlier exposed param's) becomes
// node + Param ("clumpStrength").
// The nodes right of `after` move over to make room. Returns the graph's JSON; the Compound's
// name is written to *nameMade. Throws
// std::runtime_error (GraphError) when the preset has no look to insert, when `after` does not
// give strands, or when the look reads a node this graph has none of.
std::string insertPresetJson(const std::string& graphJson, const std::string& presetJson, std::optional<double> size,
                             const std::string& after, const std::string& name = {}, std::string* nameMade = nullptr);
// The name a preset's Compound takes from the preset's name: "cards_mid" -> "cardsMid".
std::string presetNodeName(const std::string& presetName);

// A painted map a preset's graph reads ("requires": {"maps": [...]}).
struct PresetMap {
    std::string name;
    std::string kind = "guides";  // vertices, texture or guides
    double value = 0.0;           // what it starts filled with
    std::vector<double> color;    // r g b (a): a color map filled with this; empty: a gray map
};
std::vector<PresetMap> presetRequiredMaps(const std::string& presetJson);
// A guide group a preset's graph reads ("requires": {"groups": [...]}), each name once.
struct PresetGroup {
    std::string name;
    bool draw = false;  // the host's Draw tool picked, drawing into it, once the preset is applied
};
std::vector<PresetGroup> presetRequiredGroups(const std::string& presetJson);

// The preset a groom's graph makes (Save as Preset, Copy Graph): its graph with "version" first,
// "size" (when given), "about" (the fields given, empty ones left out), then the nodes,
// connections and output, the "backdrops" last; `controlsJson` ("" none) as "controls".
std::string makePresetJson(const std::string& graphJson, std::optional<double> size, const std::string& title,
                           const std::string& description, const std::string& category, const std::string& controlsJson = {});
// ---- A look as text (Copy Graph / Paste Graph in every host) -----------------------------------
// Limits on a pasted text: characters, nodes in all (compounds' inner ones too), compounds nested.
constexpr std::size_t kMaxPresetText = 4 * 1024 * 1024;  // a big groom's graph is well under 100 KB
constexpr std::size_t kMaxPresetNodes = 2000;
constexpr int kMaxPresetDepth = 16;
// The preset a pasted text holds, as every host checks it before pasting (Maya's
// presets.parse_text, the Blender add-on, the app): JSON only (never run), a ``` code block's
// content, a .groom file's "graph" taken; at most kMaxPresetText characters, kMaxPresetNodes nodes
// and kMaxPresetDepth compounds deep; the shape of a graph (nodes named and typed, params objects,
// links between names, positions numbers, numbers finite), not from a newer GroomLab, its node
// types known (a library node's "node:<id>" too); nodes copied with Ctrl+C refused (they paste into
// a graph). Returns the preset's JSON (compact); throws std::invalid_argument saying why ("the
// clipboard holds no GroomLab graph...", three problems at most).
std::string presetFromText(const std::string& text);

// The scalp size a preset is made on and fitted to: the largest extent of the mesh's bounding
// box; nullopt for a mesh without points.
std::optional<double> presetScalpSize(const ScalpMesh& scalp);

// ---- Scaling a groom with its scalp (GroomSession::groomScale) -----------------------------------
// The one table of how a measure scales: what a param of `unit` (ParamSpec::unit) is multiplied by
// on a scalp `k` times as big (a preset fitted to it, a groom scaled with its scalp): length and
// width by k, density by 1 / k^2 (as many strands on the bigger skin), perLength by 1 / k. nullopt
// for a unit that does not scale (angle, turns, fraction, seconds, pixels, color, none).
std::optional<double> unitScaleFactor(const std::string& unit, double k);
// The graph's JSON (graph_io.h; blank or "{}": the default graph) with every param that measures
// something scaled by `k`, as fitPresetJson fits a preset to a scalp k times as big: params left at
// their spec defaults too, compounds' inner nodes, the min and max of maps, Expression variables
// that say their unit. What a groom scaled with its scalp evaluates (its stored graph stays as it
// is). The text as it is for k of 1 (within 1e-9), not finite or not positive, and for text that
// is not a JSON object (loadGraphJson says why).
std::string scaleGraphJson(const std::string& graphJson, double k);
// The factor each control's value takes on a groom `k` times its size, by control name (what
// GroomSession::setControls takes): a keyed param's controls by the param's unit (paramControls),
// a keyable Expression variable's control by the unit the variable says. Controls left out keep
// their values (a Control node's value is a factor of what it is plugged into). Empty for k of 1.
std::map<std::string, double> controlScaleFactors(const std::string& graphJson, double k);

}  // namespace groomlab
