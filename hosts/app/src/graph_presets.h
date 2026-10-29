// SPDX-License-Identifier: Apache-2.0
#pragma once

// Graph presets as the app uses them (the core's presets.h: the same files Maya's Graph Editor
// lists, read the same way): where they are, applying one to the open groom (fitted to its
// scalp's size, the painted maps it reads made when missing, the graph replaced as one undo
// step), saving the groom's graph as a user preset, and the whole graph as text for the
// clipboard (Copy Graph, Paste Graph). No widgets: the tests drive it on a document.

#include <string>
#include <vector>

#include "groomlab/presets.h"

namespace groomlab::app {

class GroomDocument;

// The presets shipped with the app: the "presets" folder beside the executable (the build
// copies hosts/maya/presets there), else the source tree's folder (a development build).
std::string shippedPresetDir();
// The compound nodes shipped with the app (node_library.h): the "nodes" folder beside the
// executable, else the source tree's (hosts/maya/nodes).
std::string shippedNodeFolder();
// The brush alphas shipped with the app (brush_alpha.h), found the same way.
std::string shippedAlphaFolder();
// The sample meshes shipped with the app (GroomLab > Add Sample Mesh; CC BY 4.0): the "meshes"
// folder beside the executable, else the source tree's (hosts/maya/meshes).
std::string shippedMeshFolder();
// Where presets are looked up (presetSearchDirs: GROOMLAB_PRESET_PATH, the user's folder, the
// shipped ones).
std::vector<std::string> presetDirectories();
std::vector<PresetInfo> availablePresets();

// What applying a preset did.
struct PresetApplied {
    std::string title;
    std::size_t nodes = 0;
    std::vector<std::string> mapsMade;  // the painted maps the preset reads, made here
    std::vector<std::string> groupsMade;  // the guide groups the preset reads, made here (empty)
    std::string drawGroup;              // the group the preset's Draw tool draws into ("" none)
    bool fitted = false;                // sizes scaled to this scalp
    double fitFactor = 1.0;
    std::string nodeMade;               // inserting: the Compound's name
    std::string after;                  // inserting: the node it was placed after ("" before the Output)
};
// The preset file at `path` onto the document: "" or the error (nothing changes then, except
// maps made before the graph was refused).
std::string applyPresetFile(GroomDocument& document, const std::string& path, PresetApplied* applied = nullptr);
// A preset's text (a file's, or pasted) onto the document, as applyPresetFile; with `makeMaps`
// the painted maps and guide groups it requires are made (pasting makes none: it writes no files).
std::string applyPresetText(GroomDocument& document, const std::string& text, bool makeMaps, PresetApplied* applied = nullptr);
// The preset file at `path` inserted into the document's graph (the core's insertPresetJson: its
// look as one Compound named after the preset, after node `after`, or before the Output when
// `after` is ""), fitted to the scalp, the maps it reads made: one undo step. "" or the error.
std::string insertPresetFile(GroomDocument& document, const std::string& path, const std::string& after, PresetApplied* applied = nullptr);

// Saves the document's graph (its layout and backdrops included, the scalp's size recorded) as
// <presetUserDir()>/<name>.json; refused when the name is bad or the file exists and not
// `overwrite`. "" or the error; the path written to *pathOut.
std::string savePresetFile(const GroomDocument& document, const std::string& name, bool overwrite, std::string* pathOut = nullptr);

// The groom's whole graph as text to share (Copy Graph): the preset Save as Preset writes,
// titled after the groom (category "Shared"); it loads again as a .json preset.
std::string graphAsText(const GroomDocument& document);
// A text pasted (Copy Graph's, or a preset's JSON) onto the document (applyPresetText without
// the maps); the text is only ever read as JSON. "" or why it was refused.
std::string pasteGraphText(GroomDocument& document, const std::string& text, PresetApplied* applied = nullptr);
// One line about what applying did ("preset Wet hair: 12 nodes, sizes fitted to this scalp (x2.5), map wet made").
std::string presetMessage(const PresetApplied& applied);

}  // namespace groomlab::app
