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
        {"file.reload", "File", "Ctrl+R", "Reload from the file (asks first when there are unsaved changes)", kApp},
        {"file.export", "File", "Ctrl+E", "Export", kApp},
        {"file.undo", "File", "Ctrl+Z / Ctrl+Y", "Undo / redo (Ctrl+Shift+Z redoes too)", kApp},
        {"file.undo", "File", "Ctrl+Z / Ctrl+Shift+Z", "Undo / redo the groom's edits", kBlender},
        {"file.quit", "File", "Ctrl+Q", "Quit (asks first when there are unsaved changes)", kApp},
        {"modes", "Modes", "Ctrl+1 / 2 / 3 / 4", "Groom / Look / Cards / Export mode", kApp},
        {"brush.pick", "Brush", "B", "The brush you used last", kApp},
        {"brush.radius", "Brush", "B + drag", "The brush radius", kAll, kSet},
        {"brush.radiusMiddle", "Brush", "Middle-drag", "The brush radius", kMaya | kApp},
        {"brush.strength", "Brush", "M + drag", "The brush strength", kAll, kSet},
        {"brush.strengthMiddle", "Brush", "Shift + middle-drag", "The brush strength", kMaya | kApp},
        {"brush.invert", "Brush", "Ctrl (stroke)", "The opposite effect (Ctrl+Length shrinks, Ctrl+Lift lays down...)", kAll},
        {"brush.smooth", "Brush", "Shift (stroke)", "Smooth instead", kAll},
        {"brush.solo", "Brush", "Shift + click", "Solo a guide and brush its group (off a guide: unfreeze all)", kAll},
        {"brush.select", "Brush", "S", "Select guides (Shift: toggle, Ctrl: deselect)", kApp | kBlender},
        {"brush.level", "Brush", "Page Up / Page Down", "The brush level (parents / children)", kAll, kSet},
        {"brush.symmetry", "Brush", "Alt+Shift+X", "Symmetry on or off", kMaya | kApp, kSet},
        {"brush.menu", "Brush", "Y + click", "The marking menu: every brush", kMaya | kApp, kSet},
        {"brush.menu", "Brush", "Y", "The pie menu: every brush", kBlender},
        {"brush.leave", "Brush", "Esc", "Leave the brush", kMaya},
        {"brush.leave", "Brush", "Esc / right-click", "Cancel the stroke; Esc again leaves the tool", kBlender},
        {"view.escape", "Viewport", "Esc", "Leave the brush (over the graph: back to the viewport)", kApp},
        {"view.graph", "Viewport", "Tab", "The Graph Editor (over it: add a node)", kApp},
        {"view.frame", "Viewport", "F", "Frame the groom", kApp},
        {"view.orbit", "Viewport", "Alt+drag", "Orbit (with any tool)", kApp},
        {"view.pan", "Viewport", "Alt+middle-drag / Alt+right-drag", "Pan / dolly the camera", kApp},
        {"view.wheel", "Viewport", "Mouse wheel", "Dolly the camera (over the graph: zoom it)", kApp},
        {"view.delete", "Viewport", "Delete", "The selected guides, node or wire", kApp},
        {"display.guides", "Viewport", "Alt+Shift+G", "Every groom's guides on or off (GroomLab > Display)", kMaya | kApp, kSet},
        {"graph.add", "Graph", "Tab", "Add a node at the cursor", kMaya | kBlender},
        {"graph.duplicate", "Graph", "Ctrl+D", "Duplicate the selected nodes", kMaya | kApp},
        {"graph.group", "Graph", "Ctrl+G / Ctrl+Shift+G", "Group into a Compound / ungroup", kMaya | kApp},
        {"graph.tidy", "Graph", "Ctrl+L", "Tidy the layout", kApp},
        {"graph.bypass", "Graph", "B", "Bypass the selected nodes (again: turn them back on)", kAll},
        {"graph.view", "Graph", "V", "View the groom up to the node", kAll},
        {"graph.frame", "Graph", "F", "Frame the selected nodes (none: the whole graph)", kMaya | kApp},
        {"graph.frameAll", "Graph", "A", "Frame the whole graph", kMaya | kApp},
        {"graph.rename", "Graph", "F2 / double-click", "Rename the node (a Compound: double-click opens it)", kApp},
        {"graph.delete", "Graph", "Delete", "Delete the selected nodes or link", kMaya | kApp},
        {"graph.cut", "Graph", "Alt+Shift+drag", "Cut every connection crossed", kMaya},
        {"graph.copy", "Graph", "Ctrl+C / Ctrl+V", "Copy / paste the selected nodes (their links between them too)", kApp},
        {"graph.select", "Graph", "Ctrl+click / Shift+click / drag", "Toggle / add a node to the selection; drag on empty space: select in a box", kApp},
        {"graph.navigate", "Graph", "Middle-drag / wheel", "Pan / zoom the graph", kMaya | kApp},
        {"graph.menu", "Graph", "Right-click", "The node's or the graph's menu", kMaya | kApp},
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
