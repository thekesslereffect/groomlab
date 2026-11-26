// SPDX-License-Identifier: Apache-2.0
#pragma once

// GroomLab's own tools in the host (docs/BLENDER.md, "Tools"): the core's interactive brush
// (stroke.h's BrushTool over a SessionStrokeHost, as the standalone app's BrushSession drives it)
// in the host's views, guide selection, the guide hierarchy's edits, painted maps, one undo step
// per stroke or edit, and the display buffers the core builds for Maya's viewport (display.h),
// all in the host's space.

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/groom_file.h"
#include "groomlab/stroke.h"

namespace groomlab::helper {

struct Session;
class BlockReader;

// The tool commands (tool_set, stroke_begin, display...): each takes "session".
bool isToolCommand(const std::string& cmd);
// Runs one. True when `cmd` is one of them (its answer in `result`); false leaves it to the other
// commands. Throws RequestError.
bool runToolCommand(Session& s, const std::string& cmd, const nlohmann::json& args, BlockReader& blocks, nlohmann::json& result);
// tool_catalog: the palette's tools and settings (no session).
nlohmann::json toolCatalogJson();
// What get_state keeps of the tools (the selection, its soft radius, the hidden groups), and
// set_state puts back (guides that are not there left out).
nlohmann::json toolStateJson(Session& s);
void restoreToolState(Session& s, const nlohmann::json& state);

// The session's groom was replaced (a file loaded, a new groom, a state set): the tools start
// over (no stroke, no selection, an empty undo stack), keeping the tool and its settings.
void resetTools(Session& s);

// The painted maps of a groom file (vertex maps in its scalp's color sets, texture maps by file)
// handed to the session as its scalp maps, so they outlive the host's scalp (which carries no
// color sets): adopt() calls it for a file holding any.
void adoptMaps(Session& s, const GroomData& data);
// The session's painted maps into a file about to be written: the catalog, and the vertex maps as
// color sets of its scalp (as the app writes them).
void writeMaps(const Session& s, GroomFile& file);
// The viewport's guide group colors and hidden groups (the tools' state) into a file about to be
// written ("guideGroupColors", "hiddenGuideGroups": every host's .groom), and from a file adopted.
void writeGroupDisplay(const Session& s, GroomFile& file);
void adoptGroupDisplay(Session& s, const GroomFile& file);

// A texture map's file as the session reads it: the stored path resolved against the groom's
// folder and search paths ("" for none).
std::string resolveMapFile(const Session& s, const std::string& stored);
// Where new texture maps' images go: the session's mapsDir, else a folder of its own in the temp
// folder.
std::string mapsFolderOf(Session& s);

// One undo step on the session's stack (authoring.cpp's edits: the graph, correctives, Sculpt
// nodes' edits): the stored texts given, set as one edit named `label` (a stroke in flight dropped
// first). Throws what the session's setters throw (a GraphError for a graph that does not load),
// nothing changed then.
void commitTexts(Session& s, const std::string& label, const std::vector<std::pair<StrokeField, std::string>>& texts);
// One undo step that also changes what the stored texts do not hold (pipeline.cpp: a simulation's
// bakes, a transfer's scalp): the texts as commitTexts sets them, the files given to
// sideFileBefore since the last step (their bytes now and as the edit left them), and `apply`,
// which puts that other state as the edit left it (true: redo) or found it (false: undo). The
// caller has made the edit's other state already; `apply` is called only by undo and redo.
void commitStep(Session& s, const std::string& label, const std::vector<std::pair<StrokeField, std::string>>& texts,
                std::function<void(bool after)> apply);
// A file the next commitStep's edit is about to write, move or remove: its bytes as they are now.
void sideFileBefore(Session& s, const std::string& path);
// The edit failed: the files given to sideFileBefore put back as they were, nothing kept.
void dropSideFiles(Session& s);
// undo_state's answer.
nlohmann::json undoStateJson(Session& s);
// One undo step of a stroke edit made elsewhere (the agent tools', agent.h): `edit` applied to the
// session as stroke_end commits one (its texts, a texture map's tiles), and `apply` beside it as
// commitStep keeps it (null: nothing else). Throws what the session's setters throw, nothing changed.
void commitStroke(Session& s, StrokeEdit edit, std::function<void(bool after)> apply);
// The session's undo stack: the steps done (oldest first; each one's label its `mode`), the labels
// redo would bring back (the next one last); one step undone or redone (false: none to).
const std::vector<StrokeEdit>& doneSteps(Session& s);
const std::vector<std::string>& redoLabels(Session& s);
bool undoSession(Session& s);
bool redoSession(Session& s);
// The corrective layer strokes sculpt into ("" the base groom: no shot sculpting), as Maya's
// shotLayer and shotSculpt: strokes reshape the groom as shown and record into it at the
// session's time (SessionStrokeHost::setShotSculpt). Dropped by an undo or edit that removes it.
void setShotLayer(Session& s, const std::string& layer);
std::string shotLayer(Session& s);

}  // namespace groomlab::helper
