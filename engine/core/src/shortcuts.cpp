// SPDX-License-Identifier: Apache-2.0
#include "groomlab/shortcuts.h"

namespace groomlab {

namespace {
constexpr unsigned kMaya = kShortcutMaya, kApp = kShortcutApp, kBlender = kShortcutBlender, kAll = kShortcutAll;
constexpr bool kSet = true;  // Maya: the GroomLab hotkey set binds it
}  // namespace

const std::vector<Shortcut>& shortcuts() {
    // One row per line: the docs site reads the app's rows from this file
    // (web/components/app-shortcuts.tsx), so keep each {id, context, keys, action, hosts} on one.
    static const std::vector<Shortcut> rows = {
        {"file.open", "File", "Ctrl+O", "Open a groom", kApp},
        {"file.new", "File", "Ctrl+N", "New groom on a mesh", kApp},
        {"file.save", "File", "Ctrl+S / Ctrl+Shift+S", "Save / Save as", kApp},
        {"file.reload", "File", "Ctrl+R", "Reload from disk", kApp},
        {"file.export", "File", "Ctrl+E", "Export", kApp},
        {"file.undo", "File", "Ctrl+Z / Ctrl+Y", "Undo / redo", kApp},
        {"file.undo", "File", "Ctrl+Z / Ctrl+Shift+Z", "Undo / redo the groom's edits", kBlender},
        {"file.quit", "File", "Ctrl+Q", "Quit", kApp},
        {"modes", "Modes", "Ctrl+1 / 2 / 3 / 4", "Switch mode", kApp},
        {"brush.pick", "Brush", "B", "Last brush", kApp},
        {"brush.radius", "Brush", "B + drag", "Brush radius", kAll, kSet},
        {"brush.radiusMiddle", "Brush", "Middle-drag", "Brush radius", kMaya | kApp},
        {"brush.strength", "Brush", "M + drag", "Brush strength", kAll, kSet},
        {"brush.strengthMiddle", "Brush", "Shift + middle-drag", "Brush strength", kMaya | kApp},
        {"brush.invert", "Brush", "Ctrl (stroke)", "Reverse the brush", kAll},
        {"brush.smooth", "Brush", "Shift + drag", "Smooth with any brush", kAll},
        {"brush.select", "Brush", "S", "Select guides", kApp | kBlender},
        {"brush.level", "Brush", "Page Up / Page Down", "Brush level up / down", kAll, kSet},
        {"brush.symmetry", "Brush", "Alt+Shift+X", "Toggle symmetry", kMaya | kApp, kSet},
        {"brush.menu", "Brush", "Y + click", "Brush marking menu", kMaya | kApp, kSet},
        {"brush.menu", "Brush", "Y", "Brush pie menu", kBlender},
        {"brush.leave", "Brush", "Esc", "Leave the brush", kMaya},
        {"brush.leave", "Brush", "Esc / right-click", "Cancel stroke / leave tool", kBlender},
        {"view.escape", "Viewport", "Esc", "Leave the brush", kApp},
        {"view.graph", "Viewport", "Tab", "Graph Editor / add node", kApp},
        {"view.frame", "Viewport", "F", "Frame the groom", kApp},
        {"view.orbit", "Viewport", "Alt+drag", "Orbit", kApp},
        {"view.pan", "Viewport", "Alt+middle-drag / Alt+right-drag", "Pan / dolly the camera", kApp},
        {"view.wheel", "Viewport", "Mouse wheel", "Dolly / zoom", kApp},
        {"view.delete", "Viewport", "Delete", "Delete selection", kApp},
        {"display.guides", "Viewport", "Alt+Shift+G", "Toggle guides", kMaya | kApp, kSet},
        {"graph.add", "Graph", "Tab", "Add a node at the cursor", kMaya | kBlender},
        {"graph.duplicate", "Graph", "Ctrl+D", "Duplicate the selected nodes", kMaya | kApp},
        {"graph.group", "Graph", "Ctrl+G / Ctrl+Shift+G", "Group into a Compound / ungroup", kMaya | kApp},
        {"graph.tidy", "Graph", "Ctrl+L", "Tidy the layout", kApp},
        {"graph.bypass", "Graph", "B", "Bypass nodes", kAll},
        {"graph.view", "Graph", "V", "View up to node", kAll},
        {"graph.frame", "Graph", "F", "Frame selection", kMaya | kApp},
        {"graph.frameAll", "Graph", "A", "Frame the whole graph", kMaya | kApp},
        {"graph.rename", "Graph", "F2 / double-click", "Rename node", kApp},
        {"graph.delete", "Graph", "Delete", "Delete the selected nodes or link", kMaya | kApp},
        {"graph.cut", "Graph", "Alt+Shift+drag", "Cut every connection crossed", kMaya},
        {"graph.copy", "Graph", "Ctrl+C / Ctrl+V", "Copy / paste nodes", kApp},
        {"graph.select", "Graph", "Ctrl+click / Shift+click / drag", "Select nodes", kApp},
        {"graph.navigate", "Graph", "Middle-drag / wheel", "Pan / zoom the graph", kMaya | kApp},
        {"graph.menu", "Graph", "Right-click", "Context menu", kMaya | kApp},
    };
    return rows;
}

std::vector<Shortcut> shortcutsFor(unsigned host) {
    std::vector<Shortcut> out;
    for (const Shortcut& s : shortcuts()) {
        if (s.hosts & host) out.push_back(s);
    }
    return out;
}

std::string shortcutKeys(const std::string& id, unsigned host) {
    for (const Shortcut& s : shortcuts()) {
        if (s.id == id && (s.hosts & host)) return s.keys;
    }
    return {};
}

}  // namespace groomlab
