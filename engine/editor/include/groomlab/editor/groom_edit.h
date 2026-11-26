// SPDX-License-Identifier: Apache-2.0
#pragma once

// The groom editor (engine/editor): what every host's editing of a groom shares, written once
// (the standalone app's document now; the Blender helper and Maya as their actions move in:
// docs/DEVELOPING.md, "The groom editor"). This header: one undo step of an edit, the history of
// them, and the host that puts a groom back as it was before or after a step.
//
// A step (GroomEdit) is what one user action changed: a param or a node's bypass, a stroke's or
// a guide edit's stored texts (StrokeEdit), the whole graph, and beside any of those its side
// effects (files written, moved or deleted, the guide groups' colors, the scalp's subdivision,
// Show Tubes' nodes). Each part holds its before and after, so a host undoes and redoes it by
// putting one or the other back (EditorHost::apply).

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/asset_files.h"
#include "groomlab/stroke.h"

namespace groomlab {

struct GroomEdit {
    // A param's edit: node.name from `before` to `after` (null: the param left out, its
    // default); with `bypass`, the node's bypass (booleans) instead.
    std::string node, name;
    nlohmann::json before, after;
    bool bypass = false;
    // A stroke or guide edit instead (the texts before and after), named by `label`.
    std::shared_ptr<const StrokeEdit> stroke;
    std::string label;
    // A structural edit instead: the whole graph before and after, named by `label`.
    bool whole = false;
    nlohmann::json graphBefore, graphAfter;
    // Besides (or alone: `side`, named by `label`): files written or deleted (their bytes before
    // and after, nullopt: no file), the guide groups' colors before and after.
    std::vector<FileState> files;
    bool side = false;
    bool colors = false;
    std::map<std::string, std::string> colorsBefore, colorsAfter;
    // Scalp Subdivision before and after (levels, where they came from), with `side`.
    struct Subdivision {
        int levels = 0;
        std::string from;
    };
    std::optional<std::pair<Subdivision, Subdivision>> subdivision;
    // Show Tubes' nodes before and after (a rename, a removal: the graph's edit).
    std::optional<std::pair<std::string, std::string>> braidTubes;
    // A whole-graph step that only moved things (`whole` too): undone and redone without
    // evaluating.
    bool layout = false;
    // The compound the node graph had open when it was made (a param or bypass edit names its
    // node in there); "" the top graph.
    std::string scope;

    // The step's name in Undo / Redo: "node.param" for a param's edit, else its label.
    std::string title() const;
};

// The undo history every host keeps of its steps: recorded in order, undone back from the last,
// redone forward; recording a step drops what could be redone. A step is applied by its host
// (EditorHost); the history only keeps them and where it stands.
class EditHistory {
public:
    // A new step after the ones done (the redo tail dropped).
    void record(GroomEdit edit);
    bool canUndo() const { return index_ > 0; }
    bool canRedo() const { return index_ < steps_.size(); }
    // The steps done (where the history stands): with the step to undo it changes with every
    // edit, undo and redo (a tool asks whether its press or release committed one).
    std::size_t undoIndex() const { return index_; }
    std::size_t size() const { return steps_.size(); }
    // The step undo or redo would apply; only when canUndo / canRedo.
    const GroomEdit& toUndo() const { return steps_[index_ - 1]; }
    const GroomEdit& toRedo() const { return steps_[index_]; }
    // "Undo <title>", "Redo <title>", or "" when there is none.
    std::string undoText() const;
    std::string redoText() const;
    // The history moved one step back or forward (the host applied it).
    void stepBack();
    void stepForward();
    // The last step recorded (to add what the same action changed besides); only when size() > 0.
    GroomEdit& last() { return steps_.back(); }
    // Whether the last step was recorded since the history stood at `undoBefore` and is the one
    // done last (nothing undone since): what an action's side effects join instead of a step of
    // their own.
    bool recordedSince(std::size_t undoBefore) const { return index_ > undoBefore && index_ == steps_.size(); }
    // The steps that could be redone, dropped.
    void dropRedo();
    // The last step taken out of the history (to fold it into the one before); only when size() > 0.
    GroomEdit takeLast();
    // Nothing to undo or redo (what came before can no longer come back).
    void clear();
    // At most `maxSteps` steps kept: the oldest past them dropped (they can no longer be undone).
    void limit(std::size_t maxSteps);
    // Step `i` of the history, oldest first (i < size()): undone back to undoIndex(), the rest
    // could be redone.
    const GroomEdit& step(std::size_t i) const { return steps_[i]; }

private:
    std::vector<GroomEdit> steps_;
    std::size_t index_ = 0;
};

// What applies a step to the groom a host edits: the app's document, the Blender helper's
// session, Maya's node (through its own undo queue).
class EditorHost {
public:
    virtual ~EditorHost() = default;
    // The groom as it was before the step (`after` false) or after it; "" or the error.
    virtual std::string apply(const GroomEdit& edit, bool after) = 0;
};

// Undo and redo: the host applies the step, the history moves past it. Nothing when there is
// none; "" or the host's error.
std::string undoStep(EditHistory& history, EditorHost& host);
std::string redoStep(EditHistory& history, EditorHost& host);

}  // namespace groomlab
