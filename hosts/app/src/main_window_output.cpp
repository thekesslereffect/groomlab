// SPDX-License-Identifier: Apache-2.0
// MainWindow: the GroomLab menu's output and housekeeping (main_window.h): Add Sample Mesh, Add
// (feathers, braids, colliders, wind, jiggle), Simulate > Add Simulate Node, Import > Alembic
// Curves, the Utilities (To Mesh, Bake Scale Maps, Find Missing Files), Export > Scales
// Instancer (USD), the Strand Count HUD; the themes switched live, File > Open Recent and autosave.
// Choices are asked in the viewport's glass prompt, never a dialog; a file dialog opens only in a
// window someone sits at (a scripted run takes the default place).

#include "main_window_internal.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QMenu>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include "graph_presets.h"
#include "guides_panel.h"
#include "look.h"
#include "maps_panel.h"
#include "mesh_io.h"
#include "overlay.h"
#include "theme.h"
#include "viewport.h"

#include "groomlab/presets.h"

namespace groomlab::app {

namespace {

// The shipped samples (hosts/maya/meshes: centimeters, Y up, facing +Z; CC BY 4.0, Nick Rutlinh):
// the file, and the group the groom grows on ("" the whole mesh), as Maya's groom.SAMPLES.
struct Sample {
    const char* name;
    const char* file;
    const char* group;
};
const Sample kSamples[] = {{"nika", "nika_head.obj", ""}, {"manny", "manny_head.obj", ""}, {"wolf", "wolf.obj", "wolf_body"}};

const Sample* sampleNamed(const QString& name) {
    for (const Sample& s : kSamples) {
        if (name == QLatin1String(s.name)) return &s;
    }
    return nullptr;
}

const QString kAutosaveMinutes = QStringLiteral("autosave/minutes");
const QString kAutosavePending = QStringLiteral("autosave/pending");  // {autosave file: the file it stands for ("" untitled)}

QVariantMap pendingAutosaves() { return QSettings().value(kAutosavePending).toMap(); }

void setPendingAutosaves(const QVariantMap& pending) { QSettings().setValue(kAutosavePending, pending); }

// "contour 3, down 1" (or "contour, down"): the designs and their shares (empty: equal).
void parseDesigns(const QString& text, QStringList* designs, std::vector<double>* weights) {
    bool anyWeight = false;
    std::vector<double> shares;
    for (const QString& part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QStringList words = part.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (words.isEmpty()) continue;
        *designs << words.front().toLower();
        bool ok = false;
        const double w = words.size() > 1 ? words[1].toDouble(&ok) : 1.0;
        anyWeight = anyWeight || ok;
        shares.push_back(ok ? w : 1.0);
    }
    if (anyWeight) *weights = shares;
}

}  // namespace

// ---- Add Sample Mesh ------------------------------------------------------------------------------

QStringList MainWindow::sampleMeshNames() {
    QStringList names;
    for (const Sample& s : kSamples) names << QLatin1String(s.name);
    return names;
}

QString MainWindow::sampleMeshFile(const QString& name) {
    const Sample* s = sampleNamed(name);
    const QString folder = QString::fromStdString(shippedMeshFolder());
    if (!s || folder.isEmpty()) return {};
    const QString path = QDir(folder).filePath(QLatin1String(s->file));
    return QFileInfo(path).isFile() ? path : QString();
}

bool MainWindow::addSampleMesh(const QString& name) {
    const Sample* s = sampleNamed(name);
    if (!s) {
        report(tr("No sample mesh '%1' (%2)").arg(name, sampleMeshNames().join(QStringLiteral(", "))));
        return false;
    }
    const QString file = sampleMeshFile(name);
    if (file.isEmpty()) {
        report(tr("The sample mesh %1 is not beside GroomLab (meshes/%1): reinstall GroomLab").arg(QLatin1String(s->file)));
        return false;
    }
    const QString group = QLatin1String(s->group);
    if (!newGroomOnMesh(file, group)) return false;
    if (!group.isEmpty()) {
        // The rest of the sample (the wolf's eyes, nails and mouth) a prop beside the groom, in place
        // of an earlier sample's.
        try {
            auto rest = std::make_shared<Surface>(readObjSurface(file.toStdString(), surfacePreset("clay"), group.toStdString()));
            rest->name = std::string(s->name) + "_parts";
            props_.erase(std::remove_if(props_.begin(), props_.end(), [&](const SurfacePtr& p) { return p->name == rest->name; }), props_.end());
            props_.push_back(rest);
            applyProps();
            refreshSurfaces();
        } catch (const std::exception& e) {
            report(tr("Cannot load the rest of the sample %1: %2").arg(name, QString::fromUtf8(e.what())), false);
        }
        frameAll();
    }
    return true;
}

// ---- Add and Simulate -------------------------------------------------------------------------------

bool MainWindow::addMotionNode(const QString& type) {
    if (!document_) {
        report(tr("No groom open to add a %1 node to").arg(type));
        return false;
    }
    // Maya's add_wind and add_jiggle defaults.
    const nlohmann::json params = type == QStringLiteral("Wind") ? nlohmann::json{{"direction", {1.0, 0.0, 0.0}}, {"strength", 0.3}}
                                                                   : nlohmann::json{{"strength", 1.0}};
    std::string made;
    const std::string error = document_->addMotionNode(type.toStdString(), params, &made);
    if (!error.empty()) {
        report(tr("Cannot add a %1 node: %2").arg(type, QString::fromStdString(error)));
        return false;
    }
    graphEdited();
    notify(type == QStringLiteral("Wind") ? tr("Added %1: play the timeline to see the wind").arg(QString::fromStdString(made))
                                          : tr("Added %1: the strands follow the scalp's motion").arg(QString::fromStdString(made)));
    return true;
}

bool MainWindow::addSimulateNode() {
    if (!document_) {
        report(tr("No groom open to simulate"));
        return false;
    }
    std::string made;
    const std::string error = document_->addSimulateNode(&made);
    if (!error.empty()) {
        report(tr("Cannot add a Simulate node: %1").arg(QString::fromStdString(error)));
        return false;
    }
    graphEdited();
    notify(tr("Added %1: play the timeline, or Simulate").arg(QString::fromStdString(made)));
    return true;
}

bool MainWindow::addCollider(const QString& prop) {
    if (!document_) {
        report(tr("No groom open to add a collider to"));
        return false;
    }
    const auto it = std::find_if(props_.begin(), props_.end(), [&](const SurfacePtr& p) { return !p->isEye && QString::fromStdString(p->name) == prop; });
    if (it == props_.end()) {
        report(tr("No prop mesh named %1: add one with File > Add Prop Mesh").arg(prop));
        return false;
    }
    std::string made;
    const std::string error = document_->addCollider(prop.toStdString(), &made);
    if (!error.empty()) {
        report(tr("Cannot add the collider: %1").arg(QString::fromStdString(error)));
        return false;
    }
    applyProps();  // (the session has the prop's mesh by its name)
    graphEdited();
    notify(tr("Added collider %1: the strands keep off %2").arg(QString::fromStdString(made), prop));
    return true;
}

QStringList MainWindow::featherDesigns() {
    QStringList designs;
    for (const PresetInfo& p : availablePresets()) {
        if (p.name.rfind("feathers_", 0) == 0) designs << QString::fromStdString(p.name.substr(9));
    }
    designs.removeDuplicates();
    return designs;
}

bool MainWindow::drawGuidesInto(const std::string& group) {
    if (!document_) return false;
    refreshGroups();
    const HierarchyPtr h = document_->session().authoredHierarchy();
    const GroupId id = h ? h->findGroup(group) : kNoGroup;
    if (id == kNoGroup) return false;
    BrushSettings& brush = brush_.settings().brush;
    brush.group = group;
    brush.level = h->groupDepth(id);
    chooseTool(QStringLiteral("draw"));
    return brush_.toolId() == "draw" && viewport_->toolMode() == Viewport::ToolMode::Brush;
}

bool MainWindow::addFeathers(const QStringList& designs, bool onGuides, const std::vector<double>& weights, const QString& group) {
    if (!document_) {
        report(tr("No groom open to add feathers to"));
        return false;
    }
    std::vector<std::pair<std::string, std::string>> texts;
    for (const QString& design : designs) {
        const std::string path = presetPath("feathers_" + design.toStdString(), presetDirectories());
        std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
        if (path.empty() || !in) {
            report(tr("No feather design '%1' (%2)").arg(design, featherDesigns().join(QStringLiteral(", "))));
            return false;
        }
        std::stringstream text;
        text << in.rdbuf();
        texts.emplace_back(design.toStdString(), text.str());
    }
    const std::string groupName = group.trimmed().isEmpty() ? std::string("feathers") : group.trimmed().toStdString();
    std::string made;
    const std::string error = document_->addFeathers(texts, weights, onGuides ? "guides" : "scatter", groupName, &made);
    if (!error.empty()) {
        report(tr("Cannot add feathers: %1").arg(QString::fromStdString(error)));
        return false;
    }
    graphEdited();
    if (!onGuides) {
        notify(tr("Added scattered %1 feathers, shaped by the guides").arg(designs.join(QStringLiteral(", "))));
        return true;
    }
    refreshGroups();
    const HierarchyPtr h = document_->session().authoredHierarchy();
    const GroupId id = h ? h->findGroup(groupName) : kNoGroup;
    const std::size_t count = id == kNoGroup ? 0 : h->guidesInGroup(id).size();
    const QString name = QString::fromStdString(groupName);
    if (count == 0 && drawGuidesInto(groupName)) {
        notify(tr("Added %1 feathers, one per guide of %2: draw guides from the skin, each one is a feather")
                   .arg(designs.join(QStringLiteral(", ")), name));
    } else {
        notify(tr("Added %1 feathers, one per guide of %2 (%3 now)").arg(designs.join(QStringLiteral(", ")), name).arg(count));
    }
    return true;
}

bool MainWindow::addBraid(const QString& style) {
    if (!document_) {
        report(tr("No groom open to braid"));
        return false;
    }
    std::string made;
    const std::string error = document_->addBraid(style.toStdString(), "braids", &made);
    if (!error.empty()) {
        report(tr("Cannot add a braid: %1").arg(QString::fromStdString(error)));
        return false;
    }
    refreshGroups();
    graphEdited();
    const HierarchyPtr h = document_->session().authoredHierarchy();
    const bool paths = h && h->findGroup("braids") != kNoGroup && !h->guidesInGroup(h->findGroup("braids")).empty();
    notify(paths ? tr("Added %1 (a %2 braid) along the braids group's guides").arg(QString::fromStdString(made), style)
                 : tr("Added %1 (a %2 braid): draw the braid paths in the braids group with the Draw tool").arg(QString::fromStdString(made), style));
    return true;
}

// ---- Import and Utilities ---------------------------------------------------------------------------

bool MainWindow::importAlembicCurves(const QString& path, const QString& object) {
    if (!document_) {
        report(tr("No groom open to import the curves into"));
        return false;
    }
    std::size_t curves = 0;
    const std::string error = document_->importAlembicCurves(path.toStdString(), object.toStdString(), 24, &curves);
    if (!error.empty()) {
        report(tr("Cannot import the Alembic curves: %1").arg(QString::fromStdString(error)));
        return false;
    }
    graphEdited();
    notify(tr("Imported %n curve(s) from %1 as the guides", "", static_cast<int>(curves)).arg(QFileInfo(path).fileName()));
    return true;
}

namespace {

// A file named after the groom beside it (<folder>/<stem><suffix>), an untitled one's in the temp folder.
QString besideGroom(const GroomDocument* document, const QString& suffix) {
    if (document && !document->path().empty()) {
        const QFileInfo groom(QString::fromStdString(document->path()));
        return groom.dir().filePath(groom.completeBaseName() + suffix);
    }
    return QDir::temp().filePath(QStringLiteral("groom") + suffix);
}

}  // namespace

bool MainWindow::strandsToMesh(const QString& path, bool ribbons, int sides, int maxStrands) {
    if (!document_) {
        report(tr("No groom open"));
        return false;
    }
    StrandMeshOptions options;
    options.style = ribbons ? StrandMeshOptions::Style::Ribbon : StrandMeshOptions::Style::Tube;
    options.sides = sides;
    std::size_t strands = 0;
    std::string error;
    {
        const BusyCursor wait;
        error = document_->writeStrandMesh(path.toStdString(), options, static_cast<std::size_t>(std::max(1, maxStrands)), &strands);
    }
    reevaluate(true);  // (the view back at the document's frame and density)
    if (!error.empty()) {
        report(tr("Cannot write the mesh: %1").arg(QString::fromStdString(error)));
        return false;
    }
    notify(tr("Wrote the mesh (%n strand(s)): %1", "", static_cast<int>(strands)).arg(path));
    return true;
}

bool MainWindow::bakeScaleMaps(const QString& directory, int resolution) {
    if (!document_) {
        report(tr("No groom open"));
        return false;
    }
    const QString folder = directory.isEmpty() ? besideGroom(document_.get(), QStringLiteral("_scales")) : directory;
    std::vector<std::string> files;
    std::string error;
    {
        const BusyCursor wait;
        error = document_->bakeScaleMaps(folder.toStdString(), document_->defaultBakeName(), resolution, &files);
    }
    reevaluate(true);
    if (!error.empty()) {
        report(tr("Cannot bake the scale maps: %1").arg(QString::fromStdString(error)));
        return false;
    }
    notify(tr("Baked the scales into %n map(s) in %1", "", static_cast<int>(files.size())).arg(folder));
    return true;
}

bool MainWindow::findMissingFiles(const QString& folder) {
    if (!document_) {
        report(tr("No groom open"));
        return false;
    }
    if (document_->missingFiles().empty()) {
        notify(tr("No missing files: every map and file the groom names is there"));
        return true;
    }
    GroomDocument::RelinkReport r;
    const std::string error = document_->findMissingFiles(folder.toStdString(), true, &r);
    if (!error.empty()) {
        report(tr("Cannot look for the missing files: %1").arg(QString::fromStdString(error)));
        return false;
    }
    for (const auto& [m, found] : r.relinked) report(tr("%1 %2 -> %3").arg(QString::fromStdString(m.kind), QString::fromStdString(m.key), QString::fromStdString(found)), false);
    for (const auto& [m, candidates] : r.ambiguous) {
        QStringList c;
        for (const std::string& s : candidates) c << QString::fromStdString(s);
        report(tr("%1 %2: several files fit, none relinked: %3").arg(QString::fromStdString(m.kind), QString::fromStdString(m.key), c.join(QStringLiteral(", "))), false);
    }
    for (const GroomDocument::MissingFile& m : r.stillMissing) {
        report(tr("%1 %2: no %3 in %4").arg(QString::fromStdString(m.kind), QString::fromStdString(m.key),
                                         QFileInfo(QString::fromStdString(m.stored)).fileName(), folder),
               false);
    }
    if (!r.relinked.empty()) {
        graphEdited();
        maps_->invalidate();
    }
    notify(tr("Relinked %1 missing file(s); %2 still missing, %3 with several matches")
               .arg(r.relinked.size())
               .arg(r.stillMissing.size())
               .arg(r.ambiguous.size()));
    return true;
}

bool MainWindow::exportScalesUsd(const QString& path) {
    if (!document_) {
        report(tr("No groom open to export"));
        return false;
    }
    ExportOptions options;
    options.frames.first = options.frames.last = document_->frame();
    options.usdScaleInstancer = true;
    return exportTo(QStringLiteral("usd"), path, options);
}

void MainWindow::setHud(bool on) {
    host_->setStatsWanted(on);
    if (on) refreshStats();
    if (interactive_) QSettings().setValue(QStringLiteral("ui/hud"), on);
}

bool MainWindow::hud() const { return host_->statsWanted(); }

// ---- The prompts ----------------------------------------------------------------------------------

QString MainWindow::pickSavePath(const QString& title, const QString& fallback, const QString& filter) {
    if (!interactive_) return fallback;
    return QFileDialog::getSaveFileName(this, title, fallback, filter);
}

void MainWindow::askFeathers() {
    if (!document_) return;
    // The two ways (engine/ui/choices.json's featherSource): scattered feathers, or one feather per
    // guide of a guide group, asked next. One per guide first (Enter) when the groom has a feathers
    // group already.
    const std::vector<std::string> featherGroups = document_->featherGuideGroups();
    const bool perGuide = !featherGroups.empty();
    auto label = [](const char* value, const char* field) {
        for (const ui::json& c : ui::Definitions::shipped().choices("featherSource")) {
            if (c.value("value", std::string()) == value) return QString::fromStdString(c.value(field, c.value("label", std::string())));
        }
        return QString::fromLatin1(value);
    };
    auto scattered = [this](const QString& text) {
        QStringList designs;
        std::vector<double> weights;
        parseDesigns(text, &designs, &weights);
        addFeathers(designs, false, weights);
    };
    auto onGuides = [this, featherGroups](const QString& text) {
        QStringList designs;
        std::vector<double> weights;
        parseDesigns(text, &designs, &weights);
        // The guide group: the groom's feathers group, another of its groups, or a new name.
        QStringList groups;
        for (const std::string& g : featherGroups) groups << QString::fromStdString(g);
        if (const HierarchyPtr h = document_ ? document_->session().authoredHierarchy() : nullptr) {
            for (const GuideGroup& g : h->groups()) groups << QString::fromStdString(g.name);
        }
        groups.removeDuplicates();
        const QString initial = featherGroups.empty() ? QStringLiteral("feathers") : QString::fromStdString(featherGroups.front());
        host_->showTextPrompt(tr("Feather Guide Group"),
                              tr("Each guide of this group is one feather, left out of the hair. A group of the groom (%1) or a new "
                                 "name: flight_left, flight_right, tail.")
                                  .arg(groups.isEmpty() ? tr("none yet") : groups.join(QStringLiteral(", "))),
                              initial, tr("Add Feathers"),
                              [this, designs, weights](const QString& group) { addFeathers(designs, true, weights, group); });
    };
    std::vector<OverlayHost::TextChoice> choices;
    if (perGuide) choices.emplace_back(label("scatter", "short"), scattered);
    choices.emplace_back(label("guides", "short"), onGuides);
    if (!perGuide) choices.emplace_back(label("scatter", "short"), scattered);
    host_->showTextPrompt(tr("Add Feathers"),
                          tr("One design, or several separated by commas with their shares (an assortment: contour 3, down 1). Designs: %1. "
                             "%2: over the scalp, laid the way the guides are combed. %3: each guide of a guide group is a "
                             "feather, drawn or brushed one by one.")
                              .arg(featherDesigns().join(QStringLiteral(", ")), label("scatter", "label"), label("guides", "label")),
                          perGuide ? QStringLiteral("flight") : QStringLiteral("contour"), std::move(choices));
}

void MainWindow::askBraid() {
    if (!document_) return;
    std::vector<OverlayHost::PromptChoice> choices;
    const std::pair<QString, QString> styles[] = {{tr("Three"), QStringLiteral("three")}, {tr("Fishtail"), QStringLiteral("fishtail")},
                                                  {tr("Rope"), QStringLiteral("rope")}, {tr("Cornrow"), QStringLiteral("cornrow")}};
    for (const auto& [label, style] : styles) choices.push_back({label, style == QStringLiteral("three"), [this, style = style] { addBraid(style); }});
    choices.push_back({tr("Cancel"), false, {}});
    host_->showPrompt(tr("Braid on Curves"),
                      tr("The braids group's guides become braid paths (draw them from the scalp with the Draw tool). Three: a classic "
                         "plait; Fishtail; Rope: two strands twisted; Cornrow: fed along the scalp."),
                      std::move(choices));
}

void MainWindow::askCollider() {
    if (!document_) return;
    QStringList meshes;
    for (const SurfacePtr& p : props_) {
        if (!p->isEye && p->mesh) meshes << QString::fromStdString(p->name);
    }
    if (meshes.isEmpty()) {
        if (!interactive_) {
            report(tr("Add a prop mesh first (File > Add Prop Mesh): the strands keep off it"));
            return;
        }
        const QString path = QFileDialog::getOpenFileName(this, tr("Collider Mesh"), QString(), tr("Meshes (*.obj)"));
        if (path.isEmpty() || !addProp(path, QString())) return;
        addCollider(QString::fromStdString(props_.back()->name));
        return;
    }
    if (meshes.size() == 1) {
        addCollider(meshes.front());
        return;
    }
    std::vector<OverlayHost::PromptChoice> choices;
    for (int i = 0; i < std::min<int>(4, static_cast<int>(meshes.size())); ++i) {
        const QString name = meshes[i];
        choices.push_back({name, i == 0, [this, name] { addCollider(name); }});
    }
    choices.push_back({tr("Cancel"), false, {}});
    host_->showPrompt(tr("Add Collider"), tr("Which prop mesh keeps the strands off it?"), std::move(choices));
}

void MainWindow::askStrandsToMesh() {
    if (!document_) return;
    auto write = [this](bool ribbons) {
        const QString path = pickSavePath(tr("To Mesh"), besideGroom(document_.get(), QStringLiteral("_mesh.obj")), tr("OBJ meshes (*.obj)"));
        if (!path.isEmpty()) strandsToMesh(path, ribbons);
    };
    host_->showPrompt(tr("To Mesh"),
                      tr("The groom as one mesh in an OBJ: the scales as their plates or feathers as barbs and rachis when it "
                         "has them, else the strands, as tubes round each strand or flat ribbons lying along the scalp."),
                      {{tr("Tubes"), true, [write] { write(false); }}, {tr("Ribbons"), false, [write] { write(true); }}, {tr("Cancel"), false, {}}});
}

void MainWindow::askImportAlembicCurves() {
    if (!document_) return;
    if (!interactive_) {
        report(tr("Import > Alembic Curves picks its file in a window"));
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, tr("Alembic Curves"), QString(), tr("Alembic files (*.abc)"));
    if (!path.isEmpty()) importAlembicCurves(path);
}

void MainWindow::askFindMissingFiles() {
    if (!document_) return;
    const std::vector<GroomDocument::MissingFile> missing = document_->missingFiles();
    if (missing.empty()) {
        notify(tr("No missing files: every map and file the groom names is there"));
        return;
    }
    if (!interactive_) {
        report(tr("%n missing file(s): Find Missing Files picks the folder to look in in a window", "", static_cast<int>(missing.size())));
        return;
    }
    const QString start = document_->path().empty() ? QString() : QFileInfo(QString::fromStdString(document_->path())).absolutePath();
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Folder to look for the missing files in"), start);
    if (!folder.isEmpty()) findMissingFiles(folder);
}

void MainWindow::askExportScalesUsd() {
    if (!document_) return;
    const QString path = pickSavePath(tr("Scales Instancer (USD)"), besideGroom(document_.get(), QStringLiteral("_scales.usda")), tr("USD files (*.usda *.usd)"));
    if (!path.isEmpty()) exportScalesUsd(path);
}

void MainWindow::askNewTheme() {
    host_->showTextPrompt(tr("New Theme from Current"),
                          tr("A copy of the %1 theme in your themes folder (%2), worn at once: edit its file, then Theme > Reload Theme.")
                              .arg(theme::themeLabel(theme::currentTheme()), QDir::toNativeSeparators(theme::userThemeDir())),
                          QStringLiteral("my_theme"), tr("Create"), [this](const QString& name) { newTheme(name); });
}

// ---- Themes ---------------------------------------------------------------------------------------

bool MainWindow::newTheme(const QString& name) {
    QString path;
    const QString error = theme::createTheme(name, &path);
    if (!error.isEmpty()) {
        report(tr("No new theme: %1").arg(error));
        return false;
    }
    chooseTheme(name.trimmed());
    if (interactive_) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    notify(tr("New theme %1: %2 (edit it, then Theme > Reload Theme)").arg(name.trimmed(), QDir::toNativeSeparators(path)));
    return true;
}

void MainWindow::reloadThemeFile() {
    const std::vector<QColor> before = theme::namedColors();
    const QString sheet = theme::styleSheet();
    theme::reloadTheme();
    restyle(before, sheet);
    notify(tr("Reloaded the %1 theme (%2)").arg(theme::themeLabel(theme::currentTheme()),
                                                QDir::toNativeSeparators(theme::themeFile(theme::currentTheme()))));
}

QString MainWindow::openThemesFolder() {
    const QString folder = theme::userThemeDir();
    if (folder.isEmpty() || !QDir().mkpath(folder)) {
        report(tr("No themes folder (set GROOMLAB_USER_THEMES)"));
        return {};
    }
    if (interactive_) QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    notify(tr("Themes folder: %1").arg(QDir::toNativeSeparators(folder)));
    return folder;
}

void MainWindow::restyle(const std::vector<QColor>& before, const QString& sheetBefore) {
    // The old theme's colors to the new one's, as the widgets' own sheets write them (#rrggbb,
    // #aarrggbb): the same theme function's color before and after.
    const std::vector<QColor> after = theme::namedColors();
    QHash<QString, QString> swap;
    for (std::size_t i = 0; i < std::min(before.size(), after.size()); ++i) {
        const QColor& a = before[i];
        const QColor& b = after[i];
        if (!a.isValid() || !b.isValid() || a == b) continue;
        for (const QColor::NameFormat f : {QColor::HexRgb, QColor::HexArgb}) {
            const QString key = a.name(f).toLower();
            if (!swap.contains(key)) swap.insert(key, b.name(f));
        }
    }
    static const QRegularExpression kHex(QStringLiteral("#[0-9a-fA-F]{8}(?![0-9a-fA-F])|#[0-9a-fA-F]{6}(?![0-9a-fA-F])"));
    auto swapped = [&](const QString& text) {
        QString out;
        qsizetype last = 0;
        for (QRegularExpressionMatchIterator it = kHex.globalMatch(text); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            out += text.mid(last, m.capturedStart() - last);
            const auto found = swap.constFind(m.captured().toLower());
            out += found != swap.constEnd() ? *found : m.captured();
            last = m.capturedEnd();
        }
        return out + text.mid(last);
    };
    const QString sheetAfter = theme::styleSheet();
    for (QWidget* w : QApplication::allWidgets()) {
        if (w == this) continue;
        const QString sheet = w->styleSheet();
        if (sheet.isEmpty()) continue;
        // A window wearing the theme's sheet and its own rules after it (Transfer, Guides from Photos).
        const QString next = !sheetBefore.isEmpty() && sheet.startsWith(sheetBefore) ? sheetAfter + swapped(sheet.mid(sheetBefore.size()))
                                                                                      : swapped(sheet);
        if (next != sheet) w->setStyleSheet(next);
    }
    setStyleSheet(sheetAfter);
    settings_.theme = theme::displayTheme();
    refreshTool();
    refreshStats();
    redisplay();
    for (QWidget* w : QApplication::allWidgets()) w->update();
}

// ---- Recent files -----------------------------------------------------------------------------------

void MainWindow::fillRecentMenu(QMenu* menu) {
    const QStringList recent = recentFiles();
    bool missing = false;
    for (const QString& path : recent) {
        const bool there = QFileInfo(path).isFile();
        missing = missing || !there;
        const QString name = QFileInfo(path).fileName();
        QAction* a = menu->addAction(there ? name : tr("%1 (missing)").arg(name));
        a->setObjectName(QStringLiteral("recentFile"));
        a->setToolTip(QDir::toNativeSeparators(path));
        a->setEnabled(there);
        connect(a, &QAction::triggered, this, [this, path] { confirmDiscard(tr("opening another groom"), [this, path] { openPath(path); }); });
    }
    if (recent.isEmpty()) menu->addAction(tr("No Recent Files"))->setEnabled(false);
    menu->addSeparator();
    QAction* remove = menu->addAction(tr("Remove Missing Files"));
    remove->setObjectName(QStringLiteral("recentRemoveMissing"));
    remove->setToolTip(tr("Take the files that are no longer there off the list"));
    remove->setEnabled(missing);
    connect(remove, &QAction::triggered, this, [this] { removeMissingRecentFiles(); });
    QAction* clear = menu->addAction(tr("Clear Recent Files"));
    clear->setObjectName(QStringLiteral("recentClear"));
    clear->setEnabled(!recent.isEmpty());
    connect(clear, &QAction::triggered, this, [this] { clearRecentFiles(); });
}

void MainWindow::removeMissingRecentFiles() {
    QStringList recent = recentFiles();
    recent.erase(std::remove_if(recent.begin(), recent.end(), [](const QString& p) { return !QFileInfo(p).isFile(); }), recent.end());
    QSettings().setValue(QStringLiteral("recentFiles"), recent);
    host_->setRecentFiles(recent);
}

void MainWindow::clearRecentFiles() {
    QSettings().setValue(QStringLiteral("recentFiles"), QStringList());
    host_->setRecentFiles({});
}

// ---- The run's settings -----------------------------------------------------------------------------

RunSettings::RunSettings(bool scripted) {
    if (const QString own = qEnvironmentVariable("GROOMLAB_SETTINGS_DIR"); !own.isEmpty()) {
        folder_ = own;
    } else if (scripted) {
        temp_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/groomlab_settings_XXXXXX"));
        valid_ = temp_->isValid();
        if (valid_) folder_ = temp_->path();
    }
    if (!folder_.isEmpty()) QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, folder_);
}

RunSettings::~RunSettings() = default;

// ---- Autosave -------------------------------------------------------------------------------------

int MainWindow::autosaveMinutes() const { return std::clamp(QSettings().value(kAutosaveMinutes, 5).toInt(), 0, 120); }

void MainWindow::setAutosaveMinutes(int minutes) {
    QSettings().setValue(kAutosaveMinutes, std::clamp(minutes, 0, 120));
    restartAutosave();
    notify(minutes > 0 ? tr("Autosave every %n minute(s)", "", minutes) : tr("Autosave off"));
}

void MainWindow::restartAutosave() {
    if (!autosaveTimer_) {
        autosaveTimer_ = new QTimer(this);
        connect(autosaveTimer_, &QTimer::timeout, this, [this] { autosaveNow(); });
    }
    const int minutes = autosaveMinutes();
    if (interactive_ && minutes > 0) autosaveTimer_->start(minutes * 60 * 1000);
    else autosaveTimer_->stop();
}

QString MainWindow::autosavePath() const {
    if (!document_) return {};
    const QString file = QString::fromStdString(document_->path());
    // Beside the file for a window someone sits at; an untitled groom's, and a scripted run's (the
    // tests, captures: never beside the test data), in the settings folder (that run's own).
    if (!file.isEmpty() && interactive_) return file + QLatin1Char('~');
    const QDir folder(QFileInfo(QSettings().fileName()).absolutePath());
    return folder.filePath(QStringLiteral("autosave/") + (file.isEmpty() ? QStringLiteral("untitled.groom") : QFileInfo(file).fileName()) +
                           QLatin1Char('~'));
}

bool MainWindow::autosaveNow() {
    // Only between edits: the session is the worker's while a dab or a commit evaluates there.
    if (!document_ || !document_->dirty() || busy() || dabs_.pending() || commits_.pending() || paramDragging_) return false;
    const QString path = autosavePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    try {
        document_->writeRenderGroom(path.toStdString());
    } catch (const std::exception& e) {
        report(tr("Cannot autosave to %1: %2").arg(path, QString::fromUtf8(e.what())), false);
        return false;
    }
    autosaveFile_ = path;
    QVariantMap pending = pendingAutosaves();
    pending.insert(path, QString::fromStdString(document_->path()));
    setPendingAutosaves(pending);
    report(tr("Autosaved to %1").arg(path), false);
    return true;
}

void MainWindow::dropAutosave() {
    if (autosaveFile_.isEmpty()) return;
    QFile::remove(autosaveFile_);
    QVariantMap pending = pendingAutosaves();
    pending.remove(autosaveFile_);
    setPendingAutosaves(pending);
    autosaveFile_.clear();
}

bool MainWindow::offerAutosave() {
    QVariantMap pending = pendingAutosaves();
    for (auto it = pending.begin(); it != pending.end();) {
        const QString autosave = it.key(), original = it.value().toString();
        const QFileInfo copy(autosave);
        // Gone, or older than the file it stands for (saved since): nothing to offer.
        if (!copy.isFile() || (!original.isEmpty() && QFileInfo(original).isFile() && QFileInfo(original).lastModified() > copy.lastModified())) {
            if (copy.isFile()) QFile::remove(autosave);
            it = pending.erase(it);
            continue;
        }
        setPendingAutosaves(pending);
        const QString name = original.isEmpty() ? tr("An untitled groom") : QFileInfo(original).fileName();
        host_->showPrompt(tr("Recover Unsaved Changes?"),
                          tr("%1 has changes from %2 that were not saved (GroomLab closed before they were). Recover them from the autosave?")
                              .arg(name, QLocale(QLocale::English).toString(copy.lastModified(), QStringLiteral("d MMM, HH:mm"))),
                          {{tr("Recover"), true, [this, autosave] { recoverAutosave(autosave); }},
                           {tr("Discard"), false,
                            [autosave] {
                                QFile::remove(autosave);
                                QVariantMap p = pendingAutosaves();
                                p.remove(autosave);
                                setPendingAutosaves(p);
                            }},
                           {tr("Later"), false, {}}});
        return true;
    }
    setPendingAutosaves(pending);
    return false;
}

bool MainWindow::recoverAutosave(const QString& autosave) {
    if (refuseWhileBusy(tr("recovering a groom"))) return false;
    const QString original = pendingAutosaves().value(autosave).toString();
    std::unique_ptr<GroomDocument> recovered;
    try {
        working(tr("Recovering %1...").arg(original.isEmpty() ? tr("the untitled groom") : QFileInfo(original).fileName()));
        const BusyCursor wait;
        recovered = GroomDocument::open(autosave.toStdString());
    } catch (const std::exception& e) {
        report(tr("Cannot recover the groom from %1: %2").arg(autosave, QString::fromUtf8(e.what())));
        return false;
    }
    recovered->recoveredAs(original.toStdString());
    setDocument(std::move(recovered));
    autosaveFile_ = autosave;  // kept until the groom is saved or its changes let go of
    refreshTitle();
    notify(tr("Recovered %1 from its autosave: save it to keep the changes").arg(original.isEmpty() ? tr("the untitled groom") : QFileInfo(original).fileName()));
    return true;
}

}  // namespace groomlab::app
