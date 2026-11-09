// SPDX-License-Identifier: Apache-2.0
// The GroomLab menu's output and housekeeping ids (app_actions.h; Maya's menu.py helpers, Blender's
// pipeline_ops.py): Add Sample Mesh, Add (feathers, braid, collider, wind, jiggle), Simulate > Add
// Simulate Node, Import > Alembic Curves, the Utilities, Export > Scales Instancer (USD), the Theme menu's New / Reload / Open Folder, File > Open Recent and Autosave.
// What asks first does it in the viewport's glass prompt (MainWindow::ask*), never a dialog.

#include "app_actions.h"

#include <QMenu>

#include "main_window.h"

namespace groomlab::app {

using ui::json;

void AppActions::addOutputBindings(ui::Bindings& b, MainWindow& w) {
    MainWindow* m = &w;
    auto run = [](std::function<void()> fn) { return [fn = std::move(fn)](const json&, bool) { fn(); }; };
    auto groomOpen = [m] { return m->document_ != nullptr; };

    // Add Sample Mesh: a new groom on the sample (asked first when the open one has unsaved changes).
    for (const QString& name : MainWindow::sampleMeshNames()) {
        b["sample." + name.toStdString()] = {run([m, name] {
                                                 m->confirmDiscard(QObject::tr("starting a new groom"), [m, name] { m->addSampleMesh(name); });
                                             }),
                                             {},
                                             [name] { return !MainWindow::sampleMeshFile(name).isEmpty(); }};
    }
    b["import.alembicCurves"] = {run([m] { m->askImportAlembicCurves(); }), {}, groomOpen};

    // Add and Simulate (Maya's add_feathers_dialog, add_braid_dialog, add_collider_action,
    // add_wind_action, add_jiggle_action, add_simulate_node).
    b["add.feathers"] = {run([m] { m->askFeathers(); }), {}, groomOpen};
    b["add.braid"] = {run([m] { m->askBraid(); }), {}, groomOpen};
    b["add.collider"] = {run([m] { m->askCollider(); }), {}, groomOpen};
    b["add.wind"] = {run([m] { m->addMotionNode(QStringLiteral("Wind")); }), {}, groomOpen};
    b["add.jiggle"] = {run([m] { m->addMotionNode(QStringLiteral("Jiggle")); }), {}, groomOpen};
    b["sim.add"] = {run([m] { m->addSimulateNode(); }), {}, groomOpen};
    b["nhair.exportGuides"] = {run([m] { m->exportGuidesForSimulation(); }), {}, groomOpen};
    b["nhair.loadCache"] = {run([m] { m->loadGuideCache(); }), {}, groomOpen};

    // Utilities and Export.
    b["strands.toMesh"] = {run([m] { m->askStrandsToMesh(); }), {}, groomOpen};
    b["scales.bakeMaps"] = {run([m] { m->bakeScaleMaps(); }), {}, groomOpen};
    b["follicles.bakeMap"] = {run([m] { m->bakeFollicleMap(); }), {}, groomOpen};
    b["files.findMissing"] = {run([m] { m->askFindMissingFiles(); }), {}, groomOpen};
    b["files.collect"] = {run([m] { m->collectFiles(); }), {}, [m] { return m->document_ != nullptr && !m->document_->path().empty(); }};
    b["export.scalesUsd"] = {run([m] { m->askExportScalesUsd(); }), {}, groomOpen};
    b["export.unrealCards"] = {run([m] { m->exportEngineCards(QString(), false); }), {}, groomOpen};
    b["export.unityCards"] = {run([m] { m->exportEngineCards(QString(), true); }), {}, groomOpen};
    b["export.scalesFbx"] = {run([m] { m->exportScalesFbx(QString()); }), {}, groomOpen};
    b["export.unityFeathers"] = {run([m] { m->exportUnityFeathers(QString()); }), {}, groomOpen};

    // Theme: New Theme from Current (its name asked), Reload Theme, Open Themes Folder.
    b["theme.new"] = {run([m] { m->askNewTheme(); }), {}, {}, {}, {}, QStringLiteral("themeNew")};
    b["theme.reload"] = {run([m] { m->reloadThemeFile(); }), {}, {}, {}, {}, QStringLiteral("themeReload")};
    b["theme.openFolder"] = {run([m] { m->openThemesFolder(); }), {}, {}, {}, {}, QStringLiteral("themeOpenFolder")};

    // File > Autosave: the interval (minutes; 0 off), checked as set.
    b["file.autosave"] = {[m](const json& value, bool) {
                              if (value.is_number()) m->setAutosaveMinutes(value.get<int>());
                          },
                          [m](const json& value) { return value.is_number() && value.get<int>() == m->autosaveMinutes(); },
                          {},
                          {},
                          {},
                          QStringLiteral("autosave_%1")};
}

void AppActions::addOutputGenerators(ui::Generators& g, MainWindow&) {
    // File > Autosave's intervals.
    g["autosaveIntervals"] = [](const json& entry) {
        const std::string action = entry.value("action", std::string("file.autosave"));
        json out = json::array();
        for (const int minutes : {0, 1, 5, 10, 15, 30}) {
            const QString label = minutes == 0   ? QObject::tr("Off")
                                  : minutes == 1 ? QObject::tr("Every Minute")
                                                 : QObject::tr("Every %1 Minutes").arg(minutes);
            const QString tip = minutes == 0 ? QObject::tr("No autosave: only Save keeps the changes")
                                             : QObject::tr("A groom with unsaved changes written to a .groom~ copy %1 (offered at the next "
                                                           "start after a crash)")
                                                   .arg(label.toLower());
            out.push_back({{"id", action}, {"value", minutes}, {"label", label.toStdString()}, {"tip", tip.toStdString()}});
        }
        return out;
    };
}

void AppActions::addOutputFillers(std::map<std::string, ui::MenuFiller>& f, MainWindow& w) {
    MainWindow* m = &w;
    // File > Open Recent: the recent files (filled again each time it opens).
    f["recentFiles"] = [m](QMenu* menu, const ui::Item&) { m->fillRecentMenu(menu); };
}

}  // namespace groomlab::app
