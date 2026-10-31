// SPDX-License-Identifier: Apache-2.0
// The app's UI from the shared definitions (engine/ui/*.json, docs/UI_DEFINITIONS.md):
//
// - the loader (definitions.h) resolves the shared cases (tests/engine/ui/resolve_cases.json) as
//   Maya's and Blender's definitions.py do, and reads the embedded files as the source tree has them;
// - every action id the definitions show for the app has a binding in app_actions.cpp, except the
//   ids listed below: ALLOWED_MISSING_APP is what is left to do for parity (the app hides those),
//   NOT_IN_APP what the app does its own way;
// - the menu bar is built from menus.json's "app.menuBar", unbound actions hidden, the actions
//   named as the other tests find them.

#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <QAction>
#include <QCoreApplication>
#include <QMenu>
#include <QMenuBar>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "app_actions.h"
#include "main_window.h"
#include "overlay.h"
#include "theme.h"
#include "definitions.h"

namespace groomlab::app {
namespace {

using nlohmann::json;

const std::string kCases = std::string(GROOMLAB_TEST_DATA) + "/../engine/ui/resolve_cases.json";
const std::string kUiFolder = std::string(GROOMLAB_TEST_DATA) + "/../../engine/ui";

// Ids the definitions show for the app that it does not run yet: hidden in the app. This is the
// parity checklist (docs: build/probe/app_parity.md, section 4); bind one in app_actions.cpp (and
// take it off here) as it lands.
const std::set<std::string> ALLOWED_MISSING_APP = {
    // The GroomLab menu's FBX exports: the meshes are written by the host's own FBX exporter (Maya's,
    // Blender's: the helper answers them as arrays, pipeline.cpp); the app has no FBX writer yet.
    // (Unity Feathers: its strands .abc and manifest could go without it, but not the rachis FBX.)
    "export.unrealCards", "export.unityCards", "export.unityFeathers", "export.scalesFbx",
    // The Graph Editor's param menu: a setting painted on the guides (a guide layer): the app's
    // GroomDocument::paintParam makes scalp maps only (vertices, texture) for a setting.
    "param.paintGuides",
};
// Ids the app does its own way (n/a in the parity audit).
const std::set<std::string> NOT_IN_APP = {
    "graph.copyToGrooms",  // one groom per window: Copy Graph / Paste Graph
};

// Resolved items as resolve_cases.json writes them (check_ui_definitions.py projects Maya's and
// Blender's the same way).
json project(const std::vector<ui::Item>& items) {
    json out = json::array();
    for (const ui::Item& item : items) {
        switch (item.kind) {
        case ui::Item::Kind::Divider:
            out.push_back("-");
            break;
        case ui::Item::Kind::Generator:
            out.push_back({{"generator", item.name}});
            break;
        case ui::Item::Kind::Submenu:
            out.push_back({{"submenu", item.label}, {"id", item.id}, {"tip", item.tip}, {"items", project(item.items)}});
            break;
        case ui::Item::Kind::Action: {
            json p = {{"id", item.id}, {"label", item.label}, {"tip", item.tip}};
            if (!item.value.is_null()) p["value"] = item.value;
            if (!item.shortcut.empty()) p["shortcut"] = item.shortcut;
            if (item.shortcutPart) p["shortcutPart"] = *item.shortcutPart;
            if (!item.icon.empty()) p["icon"] = item.icon;
            if (item.checkable) p["checkable"] = true;
            if (!item.enabled) p["enabled"] = false;
            out.push_back(p);
            break;
        }
        }
    }
    return out;
}

json readJson(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << path;
    std::stringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str());
}

const json& expectedFor(const json& expected, const std::string& host) {
    return expected.contains(host) ? expected.at(host) : expected.at("*");
}

std::vector<std::string> ids(const std::vector<ui::Item>& items) {
    std::vector<std::string> out;
    for (const ui::Item& i : items) out.push_back(i.kind == ui::Item::Kind::Divider ? "-" : i.id);
    return out;
}

// The menu's entries as shown: action texts without their keys, "-" for a separator.
std::vector<std::string> shown(const QMenu* menu) {
    std::vector<std::string> out;
    for (const QAction* a : menu->actions()) {
        if (!a->isVisible()) continue;
        out.push_back(a->isSeparator() ? "-" : a->text().section(QLatin1Char('\t'), 0, 0).toStdString());
    }
    return out;
}

QMenu* submenu(const QMenu* menu, const QString& title) {
    for (QAction* a : menu->actions()) {
        if (a->menu() && a->text() == title) return a->menu();
    }
    return nullptr;
}

TEST(AppUi, ResolveMatchesTheSharedCases) {
    const json cases = readJson(kCases);
    const ui::Definitions d(cases.at("doc"));
    for (const json& c : cases.at("cases")) {
        ui::ResolveOptions options;
        if (c.contains("state")) {
            if (c.at("state").is_null()) options.state.reset();
            else options.state = c.at("state").get<std::set<std::string>>();
        }
        if (c.contains("values") && !c.at("values").is_null()) options.values = c.at("values");
        if (c.contains("generators")) {
            for (const auto& [name, made] : c.at("generators").items()) {
                options.generators[name] = [made = made](const json&) { return made; };
            }
        }
        for (const char* host : {"maya", "blender", "app"}) {
            options.host = host;
            const json got = project(d.resolve(c.at("entries"), options));
            EXPECT_EQ(got, expectedFor(c.at("expected"), host)) << c.at("name").get<std::string>() << " on " << host;
        }
    }
    for (const json& c : cases.at("listed")) {
        std::map<std::string, std::string> generators;
        if (c.contains("generators")) generators = c.at("generators").get<std::map<std::string, std::string>>();
        for (const char* host : {"maya", "blender", "app"}) {
            EXPECT_EQ(json(d.listedIds(c.at("entries"), host, generators)), expectedFor(c.at("expected"), host))
                << c.at("name").get<std::string>() << " on " << host;
        }
    }
    // Blender's policy, in the app too: keep() hides an action, a submenu it empties goes.
    ui::ResolveOptions keep;
    keep.keep = [](const std::string& id) { return id != "a.one"; };
    keep.dropGenerators = true;
    EXPECT_EQ(ids(d.resolve(json::array({"a.one", "-", "a.two", {{"submenu", "S"}, {"items", {"a.one"}}}, {{"generator", "unfilled"}}}), keep)),
              (std::vector<std::string>{"a.two"}));
}

TEST(AppUi, TheEmbeddedFilesAreTheSourceTreesAndResolveForTheApp) {
    const ui::Definitions& d = ui::Definitions::shipped();
    EXPECT_EQ(d.document(), ui::Definitions::fromFolder(kUiFolder).document()) << "rebuild: the resources are older than engine/ui";
    EXPECT_EQ(d.action("export.alembic").at("label"), "Alembic...");
    EXPECT_EQ(d.choices("flowSink").at(0).at("label"), "Arrows");
    // Host filtering: Maya-only entries never show; the app's renderers are the app's alone.
    EXPECT_EQ(ids(d.resolve(d.get("menus/import"))), (std::vector<std::string>{"import.alembicCurves"}));
    EXPECT_EQ(ids(d.resolve(d.get("menus/render"))),
              (std::vector<std::string>{"render.prmanFrame", "render.prmanTurntable", "-", "render.vrayFrame", "render.vrayTurntable", "-",
                                        "render.prmanLocation", "render.vrayLocation"}));
    EXPECT_EQ(d.listedIds(d.get("menus/render"), "maya"),
              (std::vector<std::string>{"render.fromGroomFile", "render.asCurves", "render.hardware2", "render.prman",
                                        "render.vrayStandalone", "render.vrayHook", "render.vrayLoadScene"}));
    EXPECT_TRUE(d.listedIds(d.get("menus/render"), "blender").empty());
    // Per-host words: the app's label and tip, Maya's as they were.
    ui::ResolveOptions maya;
    maya.host = "maya";
    const auto main = d.resolve(d.get("menus/main"));
    const auto mayaMain = d.resolve(d.get("menus/main"), maya);
    auto find = [](const std::vector<ui::Item>& items, const std::string& id) {
        for (const ui::Item& i : items) {
            if (i.id == id) return i;
        }
        return ui::Item{};
    };
    EXPECT_EQ(find(main, "groom.create").label, "New Groom on Mesh...");
    EXPECT_EQ(find(mayaMain, "groom.create").label, "Create Groom from Selection");
    EXPECT_EQ(find(main, "studio").tip, "Lookdev lighting presets for the viewport, in any mode");
    EXPECT_EQ(find(mayaMain, "studio").tip, "A backdrop, Arnold area lights and a camera round the groom");
    const auto files = find(main, "groomFiles").items;
    EXPECT_EQ(find(files, "file.saveAs").tip.find("playback"), std::string::npos);
    EXPECT_NE(find(find(mayaMain, "groomFiles").items, "file.saveAs").tip.find("playback"), std::string::npos);
    // needs and the app's tips in a list's menu.
    const auto context = d.resolve(d.get("lists/maps/context"), {std::set<std::string>{"map", "map.texture"}});
    EXPECT_EQ(find(context, "map.reloadImage").tip, "Read the image again after editing it in another program");
    EXPECT_EQ(find(context, "map.makeGray").id, "");  // needs map.scalp and map.color
    // The app's Studio: the lighting presets generator, which the app fills.
    const auto studio = d.resolve(d.get("menus/studio"));
    ASSERT_FALSE(studio.empty());
    EXPECT_EQ(studio.front().kind, ui::Item::Kind::Generator);
    EXPECT_EQ(studio.front().name, "lightingPresets");
    // The menu bar's menus.
    std::vector<std::string> bar;
    for (const ui::Item& i : d.resolve(d.get("menus/app.menuBar"))) bar.push_back(i.label);
    EXPECT_EQ(bar, (std::vector<std::string>{"File", "Edit", "View", "GroomLab", "Help"}));
    EXPECT_TRUE(d.resolve(d.get("menus/app.menuBar"), maya).empty());
}

TEST(AppUi, DefinitionsBindEveryShownId) {
    MainWindow window;
    const ui::Bindings bindings = AppActions::bindings(window);
    const ui::Definitions& d = ui::Definitions::shipped();
    std::map<std::string, std::vector<std::string>> listed;  // id -> the containers listing it
    for (const auto& [path, entries] : d.containers()) {
        for (const std::string& id : d.listedIds(entries, "app")) listed[id].push_back(path);
    }
    std::vector<std::string> unexpected, stale, unlisted;
    for (const auto& [id, paths] : listed) {
        if (!bindings.count(id) && !ALLOWED_MISSING_APP.count(id) && !NOT_IN_APP.count(id)) {
            std::string where;
            for (const std::string& p : paths) where += (where.empty() ? "" : ", ") + p;
            unexpected.push_back(id + " (" + where + ")");
        }
    }
    EXPECT_TRUE(unexpected.empty()) << "ids the definitions show for the app with no binding in app_actions.cpp (bind them, or "
                                       "list them in ALLOWED_MISSING_APP): "
                                    << json(unexpected).dump();
    for (const std::set<std::string>* allowed : {&ALLOWED_MISSING_APP, &NOT_IN_APP}) {
        for (const std::string& id : *allowed) {
            if (bindings.count(id) || !listed.count(id)) stale.push_back(id);
        }
    }
    EXPECT_TRUE(stale.empty()) << "listed as missing but bound now, or not shown for the app: take them off " << json(stale).dump();
    for (const std::string& id : NOT_IN_APP) EXPECT_FALSE(ALLOWED_MISSING_APP.count(id)) << id;
    // Every binding: an action of the files the app shows somewhere, with something to run.
    for (const auto& [id, binding] : bindings) {
        EXPECT_TRUE(d.get("actions").contains(id)) << id << ": no such action in actions.json";
        if (!listed.count(id)) unlisted.push_back(id);
        EXPECT_TRUE(static_cast<bool>(binding.trigger)) << id;
    }
    EXPECT_TRUE(unlisted.empty()) << "bound but shown nowhere for the app: " << json(unlisted).dump();
    std::printf("parity checklist: %zu ids hidden in the app, %zu not the app's, %zu bound\n", ALLOWED_MISSING_APP.size(),
                NOT_IN_APP.size(), bindings.size());
}

TEST(AppUi, MenuBarFromTheDefinitionsHidesWhatTheAppDoesNotRun) {
    MainWindow window;
    QMenuBar bar;  // (a window of its own: its shortcuts never meet the window's)
    const auto made = buildDefinitionsMenuBar(&bar, window);
    std::vector<std::string> titles;
    for (QAction* a : bar.actions()) titles.push_back(a->text().toStdString());
    ASSERT_EQ(titles, (std::vector<std::string>{"File", "Edit", "View", "GroomLab", "Help"}));
    const QMenu* file = bar.actions()[0]->menu();
    EXPECT_EQ(shown(file), (std::vector<std::string>{"Open Groom...", "Open Recent", "New Groom on Mesh...", "-", "Save", "Save As...",
                                                     "Reload from File", "Autosave", "-", "Export", "Save Image...", "-", "Add Prop Mesh...",
                                                     "Add Eye...", "-", "Quit"}));
    // The GroomLab menu as Maya's, without Maya's alone (or the FBX exports the app cannot write).
    const QMenu* groomlab = bar.actions()[3]->menu();
    EXPECT_EQ(shown(groomlab), (std::vector<std::string>{"Groom Tools", "Graph Editor", "-", "New Groom on Mesh...", "Add Sample Mesh",
                                                         "Guides from Photos (experimental)...", "Groom Files", "Import", "-", "Add", "Simulate",
                                                         "Display", "Review", "Studio", "Render", "Export", "Utilities", "-", "Theme", "Help"}));
    EXPECT_EQ(shown(submenu(groomlab, QStringLiteral("Display"))),
              (std::vector<std::string>{"Show All Hair", "Hide All Hair", "-", "Show All Guides", "Hide All Guides"}));
    EXPECT_EQ(shown(submenu(groomlab, QStringLiteral("Add Sample Mesh"))), (std::vector<std::string>{"Nika (head)", "Manny (head)", "Wolf"}));
    EXPECT_EQ(shown(submenu(groomlab, QStringLiteral("Add"))),
              (std::vector<std::string>{"Feathers...", "Braid on Curves...", "Cards", "Collider", "Wind", "Jiggle"}));
    EXPECT_EQ(shown(submenu(groomlab, QStringLiteral("Simulate"))),
              (std::vector<std::string>{"Add Simulate Node", "Simulate Range", "Bake Simulation", "Clear Simulation"}));
    EXPECT_EQ(shown(submenu(groomlab, QStringLiteral("Utilities"))),
              (std::vector<std::string>{"To Mesh...", "Bake Node to Guides...", "Bake Scale Maps", "-", "Delete All Flow Arrows", "-",
                                        "Transfer Groom to Scalp...", "-", "Find Missing Files..."}));
    EXPECT_EQ(shown(submenu(groomlab, QStringLiteral("Export"))),
              (std::vector<std::string>{"Alembic...", "USD...", "V-Ray Scene...", "-", "Unreal Groom...", "Unity Strands (.abc)...", "glTF Cards...",
                                        "Card Maps...", "-", "Scales Instancer (USD)..."}));
    EXPECT_EQ(shown(submenu(groomlab, QStringLiteral("Render"))),
              (std::vector<std::string>{"Render Frame in RenderMan", "Render Turntable in RenderMan", "-", "Render Frame in V-Ray",
                                        "Render Turntable in V-Ray", "-", "RenderMan Location...", "V-Ray Standalone Location..."}));
    const QMenu* studio = submenu(groomlab, QStringLiteral("Studio"));
    ASSERT_TRUE(studio);
    ASSERT_EQ(studio->actions().size(), static_cast<qsizetype>(lightingPresetNames().size()));
    EXPECT_EQ(studio->actions().front()->text(), QStringLiteral("Studio"));
    EXPECT_TRUE(studio->actions().front()->isCheckable());
    const QMenu* themes = submenu(groomlab, QStringLiteral("Theme"));
    ASSERT_TRUE(themes);
    EXPECT_EQ(shown(themes).size(), static_cast<std::size_t>(theme::themeNames().size()) + 4);  // the themes, -, New, Reload, Open Folder
    EXPECT_EQ(shown(themes).back(), "Open Themes Folder");
    // The actions other tests (and the window) find by name.
    for (const char* name : {"saveAs", "reload", "transfer", "guidesFromReference", "deleteGuides", "railLabels", "addProp", "addEye",
                             "saveImage", "symmetry", "supportGroomLab", "vrayFrame", "vrayTurntable", "vrayLocation", "prmanFrame",
                             "prmanTurntable", "prmanLocation", "quit", "discord", "mode_groom", "mode_export", "showAllGuides",
                             "hideAllGuides", "showAllHair", "hideAllHair", "toggleGuides"}) {
        EXPECT_TRUE(bar.findChild<QAction*>(QString::fromLatin1(name))) << name;
    }
    EXPECT_TRUE(bar.findChild<QAction*>(QStringLiteral("theme_") + theme::themeNames().front()));
    // Keys: the definitions' with a modifier and the bindings' own are live once per id, else shown
    // after a tab; tips are the app's; without a groom what needs one is off.
    ASSERT_TRUE(made.count("file.open") && made.count("file.save") && made.count("edit.undo") && made.count("edit.redo") && made.count("graph.open") && made.count("tool.brush") && made.count("file.saveAs"));
    EXPECT_EQ(made.at("file.open")->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_O));
    EXPECT_EQ(made.at("file.save")->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_S));
    EXPECT_EQ(made.at("edit.redo")->shortcuts(), (QList<QKeySequence>{QKeySequence(Qt::CTRL | Qt::Key_Y), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)}));
    EXPECT_EQ(made.at("graph.open")->shortcut(), QKeySequence(Qt::Key_Tab));
    ASSERT_TRUE(made.count("display.toggleGuides"));  // View > Guides: Alt+Shift+G (the core's display.guides)
    EXPECT_EQ(made.at("display.toggleGuides")->shortcut(), QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_G));
    EXPECT_FALSE(made.at("file.save")->isEnabled());
    EXPECT_FALSE(made.at("edit.undo")->isEnabled());
    EXPECT_EQ(made.at("edit.undo")->text(), QStringLiteral("Undo"));
    const QMenu* groomFiles = submenu(groomlab, QStringLiteral("Groom Files"));
    ASSERT_TRUE(groomFiles);
    bool saveShownWithKeys = false;
    for (QAction* a : groomFiles->actions()) {
        if (a->text() == QStringLiteral("Save to Groom File\tCtrl+S")) saveShownWithKeys = a->shortcut().isEmpty();
    }
    EXPECT_TRUE(saveShownWithKeys) << "the second Save shows its keys without taking them";
    EXPECT_EQ(made.at("tool.brush")->text(), QStringLiteral("Brush\tB"));  // a single key: the viewport's, shown only
    EXPECT_TRUE(made.at("tool.brush")->shortcut().isEmpty());
    EXPECT_EQ(made.at("file.saveAs")->toolTip().toStdString(), ui::Definitions::shipped().action("file.saveAs").at("tips").at("app").get<std::string>());
    // They run what they say: a mode, the rail labels, the theme's check.
    QAction* look = bar.findChild<QAction*>(QStringLiteral("mode_look"));
    ASSERT_TRUE(look);
    look->trigger();
    EXPECT_EQ(window.overlay()->mode(), OverlayHost::Mode::Look);
    ui::refreshActions(&bar);
    EXPECT_TRUE(look->isChecked());
    EXPECT_FALSE(bar.findChild<QAction*>(QStringLiteral("mode_groom"))->isChecked());
    EXPECT_EQ(bar.findChild<QAction*>(QStringLiteral("railLabels"))->isChecked(), window.railLabels());
    QAction* saved = bar.findChild<QAction*>(QStringLiteral("theme_") + theme::currentTheme());
    ASSERT_TRUE(saved);
    EXPECT_TRUE(saved->isChecked());
    // A menu holding a generator is filled again when it opens (the same entries, made afresh).
    QMenu* themesMenu = submenu(groomlab, QStringLiteral("Theme"));
    const std::vector<std::string> before = shown(themesMenu);
    emit themesMenu->aboutToShow();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_EQ(shown(themesMenu), before);
    EXPECT_TRUE(bar.findChild<QAction*>(QStringLiteral("theme_") + theme::currentTheme())->isChecked());
}

TEST(AppUi, MenusFromOtherContainersTakeTheAppsGeneratorsAndFillers) {
    MainWindow window;
    const ui::Definitions& d = ui::Definitions::shipped();
    const ui::Bindings bindings = AppActions::bindings(window);
    // The Groom Tools' More menu: Create Groom (the app's words) and the Theme submenu its filler makes.
    QMenu more;
    ui::fillMenu(&more, d.get("palette/header/more"), bindings, AppActions::fillOptions(window));
    EXPECT_EQ(shown(&more), (std::vector<std::string>{"New Groom on Mesh...", "-", "Theme"}));
    const QMenu* themes = submenu(&more, QStringLiteral("Theme"));
    ASSERT_TRUE(themes);
    EXPECT_EQ(shown(themes).size(), static_cast<std::size_t>(theme::themeNames().size()) + 4);
    // The Presets menu: a submenu per category of the shipped presets, each preset's Apply and
    // Insert, then Save as Preset, Copy and Paste Graph.
    QMenu presets;
    ui::fillMenu(&presets, d.get("graph/presetMenu"), bindings, AppActions::fillOptions(window));
    const std::vector<std::string> items = shown(&presets);
    ASSERT_GE(items.size(), 6u);
    EXPECT_EQ(std::vector<std::string>(items.end() - 5, items.end()),
              (std::vector<std::string>{"-", "Save as Preset...", "-", "Copy Graph", "Paste Graph"}));
    const QMenu* category = presets.actions().front()->menu();
    ASSERT_TRUE(category && !category->actions().isEmpty());
    const QMenu* preset = category->actions().front()->menu();
    ASSERT_TRUE(preset);
    EXPECT_EQ(shown(preset), (std::vector<std::string>{"Apply on a Mesh...", "Insert After Selected Node"}));  // no groom open
    EXPECT_FALSE(preset->actions().back()->isEnabled());
    EXPECT_FALSE(preset->menuAction()->toolTip().isEmpty());
    emit presets.aboutToShow();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_EQ(shown(&presets), items);
}

}  // namespace
}  // namespace groomlab::app
