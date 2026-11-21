// SPDX-License-Identifier: Apache-2.0
#pragma once

// What each action id of the shared UI definitions (engine/ui/actions.json, docs/UI_DEFINITIONS.md)
// runs in the app: a binding over the main window (Maya's menu.ACTIONS and window ACTIONS tables,
// Blender's actions.BINDINGS). definitions.h builds the menus from the definitions and this
// table; an id the definitions show for the app without a binding here is hidden, and
// tests/hosts/app/test_definitions.cpp lists those ids (ALLOWED_MISSING_APP: what is left to do
// for parity with Maya and Blender).
//
// The app's menu bar (menus.json "app.menuBar": File, Edit, View, the GroomLab menu as Maya's and
// Blender's, Help) comes from buildDefinitionsMenuBar:
//
//     QMenuBar* bar = menuBar();  // MainWindow::buildMenuBar
//     const auto made = buildDefinitionsMenuBar(bar, *this);
//     undo_ = made.at("edit.undo");  // ... the actions the window refreshes itself
//     // and after each edit (refreshUndoActions): ui::refreshActions(bar);

#include <map>
#include <set>
#include <string>
#include <vector>

#include "definitions.h"
#include "marking_menu.h"
#include "param_panel.h"

class QAction;
class QMenu;
class QMenuBar;
class QDialog;

namespace groomlab::app {

class MainWindow;

struct AppActions {
    // Action id -> what it runs, with its checked, enabled and text getters.
    static ui::Bindings bindings(MainWindow& window);
    // The generators the app expands into entries: "themes" (the embedded themes), "presets" (the
    // graph presets by category, each Apply / Insert), "lightingPresets" (the Look card's lighting
    // presets, GroomLab > Studio in the app).
    static ui::Generators generators(MainWindow& window);
    // The generators the app fills a menu with itself: "themeMenu" (the Theme submenu, as
    // menus.json's "theme"), "panels" (View > Panels: the docks' toggles and their sections, MainWindow::fillPanelsMenu).
    static std::map<std::string, ui::MenuFiller> fillers(MainWindow& window);
    // The conditions that hold ("groom" with a groom open, "catalog").
    static std::set<std::string> state(MainWindow& window);
    // Everything fillMenu needs for the window's menus (the shortcut owners shared by them all).
    static ui::FillOptions fillOptions(MainWindow& window);

    // ---- The radial menu (marking_menu.json's radialMenu: Alt+S over the viewport) -----------
    // Its rings, as GroomBear's and RadialZ's: tools, actions (one without a binding here left out;
    // a checkable one with its state) and submenus at their compass points, Brush Options made from
    // radialMenu.brushOptions for the brush picked now (its switches and values with their state;
    // none: the submenu disabled). runMarkingMenuItem runs a picked one: a tool in Groom mode
    // (Select as the Select tool, selecting the guides to work on), an action by its binding, an
    // option set on the brush (a switch flipped, a value picked) and the Groom Tools shown again.
    static std::vector<MarkingMenu::Item> radialMenuItems(MainWindow& window);
    static void runMarkingMenuItem(MainWindow& window, const MarkingMenu::Item& item);
    // GroomLab > Radial Menu Settings...: a small window (objectName radialMenuSettings, one at a time)
    // setting the radial menu's shortcut (a key with its modifiers; a clash with a menu's shortcut said)
    // and its size, live (MainWindow::setRadialMenuShortcut, setRadialMenuScale).
    static QDialog* showRadialMenuSettings(MainWindow& window);

    // ---- The Graph Editor (app_graph_actions.cpp: graph_ui.json, as Maya's graph_editor.py) ------
    // Its bar's menus (Add Node, Presets, Edit) and the right-click menus of the graph's nodes,
    // backdrops and canvas and of the param panels' params, built from the definitions; the graph's
    // status line. Once, when the window is built.
    static void wireGraphEditor(MainWindow& window);
    // The node menu's conditions for a node (Maya's NodeItem.menu_state: node.viewable,
    // node.sculpt, node.bakeable, node.mappable, node.script, node.compound, node.library,
    // node.updateAvailable) with the window's; the param menu's (param.linkable, param.mappable).
    static std::set<std::string> nodeState(MainWindow& window, const std::string& node);
    static std::set<std::string> paramState(MainWindow& window, const std::string& node, const std::string& param);
    // A param's right-click menu (graph_ui.json's paramMenu) for node.param, filled into `menu`.
    static void fillParamMenu(MainWindow& window, QMenu* menu, const std::string& node, const std::string& param);
    // The node's actions by its type (graph_ui.json's nodeActions) the param panels pin under its
    // title: each bound id, run on that node (selected in the graph first).
    static std::vector<ParamPanel::NodeAction> nodeActions(MainWindow& window, const std::string& node);
    // The canvas's right-click menu: the search, the Add Node menu's entries, then Add Backdrop,
    // Tidy Layout and Frame All (at `at`, view coordinates: where new nodes and backdrops go).
    static ui::json canvasMenu();
    // Bake Node to Guides... for a node: the root group asked in the viewport's text prompt
    // ("baked"), then baked (GroomDocument::bakeNodeToGuides).
    static void askBakeNode(MainWindow& window, const std::string& node);

    // ---- The Groom Tools' lists (app_list_actions.cpp: lists.json, as Maya's tool_tabs.py) -------
    // The conditions the lists' bars and right-click menus need, from the panels' picked rows (one
    // function, as Maya's menu_state): "group", "group.color" (the guide groups), "map",
    // "map.scalp", "map.color", "map.texture" (the maps), "corrective" (a corrective picked), with
    // the window's (state()).
    static std::set<std::string> listState(MainWindow& window);

private:
    static void addListBindings(ui::Bindings& b, MainWindow& window);
    // The Photos tab's (app_photo_actions.cpp: lists.json's photos) and the review HUD.
    static void addPhotoBindings(ui::Bindings& b, MainWindow& window);
    static void addGraphBindings(ui::Bindings& b, MainWindow& window);
    static void addGraphGenerators(ui::Generators& g, MainWindow& window);
    static void addGraphFillers(std::map<std::string, ui::MenuFiller>& f, MainWindow& window);
    // The GroomLab menu's output and housekeeping (app_output_actions.cpp): Add Sample Mesh, Add,
    // Simulate > Add Simulate Node, Import, the Utilities, Export > Scales Instancer, the
    // Theme menu's New / Reload / Open Folder, File > Open Recent ("recentFiles") and Autosave
    // ("autosaveIntervals").
    static void addOutputBindings(ui::Bindings& b, MainWindow& window);
    static void addOutputGenerators(ui::Generators& g, MainWindow& window);
    static void addOutputFillers(std::map<std::string, ui::MenuFiller>& f, MainWindow& window);
};

// The menu bar from menus.json's "app.menuBar" with the window's bindings; returns {action id:
// QAction} (the first of each id: undo_ and the other actions the window refreshes itself).
std::map<std::string, QAction*> buildDefinitionsMenuBar(QMenuBar* bar, MainWindow& window);

}  // namespace groomlab::app
