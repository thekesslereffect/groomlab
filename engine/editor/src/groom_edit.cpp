// SPDX-License-Identifier: Apache-2.0
#include "groomlab/editor/groom_edit.h"

namespace groomlab {

std::string GroomEdit::title() const { return stroke || whole || side ? label : node + "." + name; }

void EditHistory::record(GroomEdit edit) {
    steps_.resize(index_);  // what could be redone is gone
    steps_.push_back(std::move(edit));
    index_ = steps_.size();
}

std::string EditHistory::undoText() const { return canUndo() ? "Undo " + toUndo().title() : std::string(); }

std::string EditHistory::redoText() const { return canRedo() ? "Redo " + toRedo().title() : std::string(); }

void EditHistory::stepBack() {
    if (index_ > 0) --index_;
}

void EditHistory::stepForward() {
    if (index_ < steps_.size()) ++index_;
}

void EditHistory::dropRedo() { steps_.resize(index_); }

GroomEdit EditHistory::takeLast() {
    GroomEdit e = std::move(steps_.back());
    steps_.pop_back();
    index_ = steps_.size();
    return e;
}

void EditHistory::clear() {
    steps_.clear();
    index_ = 0;
}

void EditHistory::limit(std::size_t maxSteps) {
    if (steps_.size() <= maxSteps) return;
    const std::size_t drop = steps_.size() - maxSteps;
    steps_.erase(steps_.begin(), steps_.begin() + static_cast<std::ptrdiff_t>(drop));
    index_ = index_ > drop ? index_ - drop : 0;
}

std::string undoStep(EditHistory& history, EditorHost& host) {
    if (!history.canUndo()) return {};
    std::string error = host.apply(history.toUndo(), false);
    history.stepBack();
    return error;
}

std::string redoStep(EditHistory& history, EditorHost& host) {
    if (!history.canRedo()) return {};
    std::string error = host.apply(history.toRedo(), true);
    history.stepForward();
    return error;
}

}  // namespace groomlab
