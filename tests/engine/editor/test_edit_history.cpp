// SPDX-License-Identifier: Apache-2.0
// The groom editor's undo steps and history (engine/editor groom_edit.h): recording drops the
// redo tail, undo and redo apply a step through the host and move past it, an action's side
// effects join the step it recorded.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "groomlab/editor/groom_edit.h"

using namespace groomlab;

namespace {

GroomEdit paramEdit(const std::string& node, const std::string& name, double before, double after) {
    GroomEdit e;
    e.node = node;
    e.name = name;
    e.before = before;
    e.after = after;
    return e;
}

// A host whose groom is one number per param: what the steps put back.
struct NumberHost : EditorHost {
    std::map<std::string, double> values;
    std::vector<std::string> applied;
    std::string apply(const GroomEdit& edit, bool after) override {
        values[edit.node + "." + edit.name] = (after ? edit.after : edit.before).get<double>();
        applied.push_back((after ? "redo " : "undo ") + edit.title());
        return edit.name == "broken" ? "it broke" : "";
    }
};

}  // namespace

TEST(EditHistory, UndoAndRedoApplyTheStepsThroughTheHostInOrder) {
    EditHistory h;
    NumberHost host;
    EXPECT_FALSE(h.canUndo());
    EXPECT_EQ(undoStep(h, host), "");
    EXPECT_TRUE(host.applied.empty()) << "nothing to undo: the host is not asked";
    h.record(paramEdit("length", "length", 1.0, 2.0));
    h.record(paramEdit("noise", "amplitude", 0.1, 0.3));
    EXPECT_EQ(h.undoIndex(), 2u);
    EXPECT_EQ(h.undoText(), "Undo noise.amplitude");
    EXPECT_EQ(h.redoText(), "");
    EXPECT_EQ(undoStep(h, host), "");
    EXPECT_DOUBLE_EQ(host.values["noise.amplitude"], 0.1);
    EXPECT_EQ(h.undoIndex(), 1u);
    EXPECT_EQ(h.redoText(), "Redo noise.amplitude");
    EXPECT_EQ(undoStep(h, host), "");
    EXPECT_DOUBLE_EQ(host.values["length.length"], 1.0);
    EXPECT_FALSE(h.canUndo());
    EXPECT_EQ(redoStep(h, host), "");
    EXPECT_DOUBLE_EQ(host.values["length.length"], 2.0);
    EXPECT_EQ(host.applied, (std::vector<std::string>{"undo noise.amplitude", "undo length.length", "redo length.length"}));
    // A new step drops what could be redone.
    h.record(paramEdit("width", "width", 1.0, 0.5));
    EXPECT_FALSE(h.canRedo());
    EXPECT_EQ(h.size(), 2u);
    EXPECT_EQ(h.undoText(), "Undo width.width");
    // The host's error comes back; the history still moves past the step.
    h.record(paramEdit("x", "broken", 0.0, 1.0));
    EXPECT_EQ(undoStep(h, host), "it broke");
    EXPECT_EQ(h.undoIndex(), 2u);
}

TEST(EditHistory, AnActionsSideEffectsJoinTheStepItRecorded) {
    EditHistory h;
    const std::size_t before = h.undoIndex();
    EXPECT_FALSE(h.recordedSince(before));
    GroomEdit stroke;
    stroke.stroke = std::make_shared<StrokeEdit>();
    stroke.label = "comb stroke";
    h.record(stroke);
    EXPECT_TRUE(h.recordedSince(before));
    h.last().files.push_back({"masks/a.png", std::nullopt, std::string("png")});
    EXPECT_EQ(h.toUndo().files.size(), 1u);
    EXPECT_EQ(h.undoText(), "Undo comb stroke");
    // Undone since: no longer the step a side effect joins.
    NumberHost host;
    h.stepBack();
    EXPECT_FALSE(h.recordedSince(before));
    h.stepForward();
    // Two steps folded into one (the map and its wiring: the app's Paint on a setting).
    GroomEdit wiring;
    wiring.whole = true;
    wiring.label = "map noise.amplitude";
    wiring.graphBefore = nlohmann::json::object();
    wiring.graphAfter = {{"nodes", nlohmann::json::array()}};
    h.record(wiring);
    GroomEdit taken = h.takeLast();
    EXPECT_EQ(h.size(), 1u);
    EXPECT_EQ(h.undoIndex(), 1u);
    h.last().whole = true;
    h.last().graphAfter = taken.graphAfter;
    h.last().label = taken.label;
    EXPECT_EQ(h.undoText(), "Undo map noise.amplitude");
    h.dropRedo();
    h.clear();
    EXPECT_FALSE(h.canUndo());
    EXPECT_EQ(h.size(), 0u);
}

TEST(GroomEdit, TitleNamesTheParamOrTheLabel) {
    GroomEdit e = paramEdit("clump", "count", 10, 20);
    EXPECT_EQ(e.title(), "clump.count");
    e.side = true;
    e.label = "color of primary";
    EXPECT_EQ(e.title(), "color of primary");
}
