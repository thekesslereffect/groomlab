// SPDX-License-Identifier: Apache-2.0
#include "main_window.h"

#include <algorithm>
#include <cmath>
#include <thread>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QLineEdit>
#include <QDockWidget>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QResizeEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QUrl>

#include "app_actions.h"
#include "app_shell.h"
#include "icon_rail.h"
#include "groom_tools.h"
#include "cards_panel.h"
#include "correctives_panel.h"
#include "export_panel.h"
#include "maps_panel.h"
#include "glass.h"
#include "guides_panel.h"
#include "node_graph.h"
#include "overlay.h"
#include "photos_panel.h"
#include "theme.h"
#include "update_check.h"
#include "viewport.h"

#include "groomlab/node_catalog.h"
#include "groomlab/shortcuts.h"

namespace groomlab::app {

MainWindow::MainWindow() {
    setWindowTitle(QStringLiteral("GroomLab"));
    resize(1440, 900);
    theme::loadFonts();
    // The theme on the whole window: the menu bar and its menus, the tool bar, the docks.
    setStyleSheet(theme::styleSheet());
    // The viewport's overlays (guide levels, selection, frozen, parts...) in the theme's colors.
    settings_.theme = theme::displayTheme();
    {
        // The brush ring in the theme's tone (viewport.brushRing, else the accent's ink), linear.
        const QColor named = theme::viewportColor("brushRing");
        const QColor ring = named.isValid() ? named : theme::accentInk();
        auto lin = [](qreal c) { return static_cast<float>(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4)); };
        brush_.setRingTone({lin(ring.redF()), lin(ring.greenF()), lin(ring.blueF())});
    }
    viewport_ = new Viewport();
    // The central column: the viewport (its overlays) over the timeline's bar, between the side docks.
    auto* central = new QWidget(this);
    central->setObjectName(QStringLiteral("viewportColumn"));
    auto* column = new QVBoxLayout(central);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    host_ = new OverlayHost(viewport_, central);
    column->addWidget(host_, 1);
    setCentralWidget(central);
    shell_ = new AppShell(this, host_);
    column->addWidget(shell_->timeline());
    // The Properties tabs are the modes: Cards and Export theirs, the groom's and the look's tabs Groom.
    connect(shell_, &AppShell::propertiesTabPicked, this, [this](int tab) {
        const auto t = static_cast<AppShell::PropertiesTab>(tab);
        const QString mode = t == AppShell::PropertiesTab::Cards    ? QStringLiteral("cards")
                             : t == AppShell::PropertiesTab::Export ? QStringLiteral("export")
                                                                    : QStringLiteral("groom");
        setUiMode(mode);
    });
    graph_ = shell_->graph();
    viewport_->setBrushSession(&brush_);
    // The GPU look preview runs in the viewport's frames (and the headless benchmarks'), on by
    // default where the device has an engine.
    gpu_.setEnabled(GpuLookPreview::preference(true));
    viewport_->renderer().setGpuPreview(&gpu_);
    // Tools > Turntable (View > Review): the camera turns a degree a frame while on.
    turntable_ = new QTimer(this);
    turntable_->setInterval(16);
    connect(turntable_, &QTimer::timeout, this, [this] { adjustCamera(1.0f, 0.0f, 0.0f); });
    buildPanels();
    wireOverlay();
    wireTimelineCardsAndExport();
    // The menu bar once every panel its bindings read exists; then the menus' states.
    buildMenuBar();
    refreshUndoActions();
    // The stats line follows the frames at most a few times a second and once they stop: the
    // floating widgets sit over the viewport's texture, and while one of them changes Qt paints
    // and uploads the whole window's widgets again (about 10 ms at 1440p), so a line changed every
    // frame would cost every frame that much.
    statsTimer_ = new QTimer(this);
    statsTimer_->setSingleShot(true);
    statsTimer_->setInterval(kStatsIdleMs);
    connect(statsTimer_, &QTimer::timeout, this, [this] { refreshStats(); });
    connect(viewport_, &Viewport::frameDrawn, this, [this] {
        scheduleStats();
        // While frames follow each other (a refine's), the timer below may wait behind them:
        // a commit evaluated lands after this frame (a posted call goes before timers).
        if (commits_.ready()) {
            QMetaObject::invokeMethod(this, [this] {
                if (commits_.ready() && !gpu_.dragging()) landCommit(true);
            }, Qt::QueuedConnection);
        }
        // (A dab evaluated on the worker the same way.)
        if (dabs_.ready()) {
            QMetaObject::invokeMethod(this, [this] {
                if (dabs_.ready()) landDab(true);
            }, Qt::QueuedConnection);
        }
    });
    // A dab on the worker lands once evaluated (polled as the commits are).
    dabTimer_ = new QTimer(this);
    dabTimer_->setInterval(2);
    dabTimer_->setTimerType(Qt::PreciseTimer);  // (a coarse one waits for Windows' 15.6 ms tick)
    connect(dabTimer_, &QTimer::timeout, this, [this] {
        if (dabs_.ready()) landDab(true);
    });
    // A commit on the worker lands once evaluated (polled: the worker knows nothing of Qt).
    commitTimer_ = new QTimer(this);
    commitTimer_->setInterval(4);
    commitTimer_->setTimerType(Qt::PreciseTimer);  // (a coarse one waits for Windows' 15.6 ms tick)
    connect(commitTimer_, &QTimer::timeout, this, [this] {
        // (Not under a GPU drag, whose runs read the session's document: once it ends.)
        if (commits_.ready() && !gpu_.dragging()) landCommit(true);
    });
    // The worker's loops leave a quarter of the cores to the drawing (as Maya's commits do).
    commits_.setThreads(std::max(1u, std::thread::hardware_concurrency() * 3 / 4));
    dabs_.setThreads(std::max(1u, std::thread::hardware_concurrency() * 3 / 4));
    // The brush: each dab evaluates the preview and shows it; the release shows the committed
    // stroke and makes it undoable. With dabs off the UI thread (setAsyncDabs), a CPU stroke's
    // dabs after its press evaluate on a worker and each result shows as it lands.
    connect(viewport_, &Viewport::strokeStarted, this, [this] { pressShown_ = false; });
    connect(viewport_, &Viewport::strokeDabbed, this, [this] {
        if (pressShown_ && dabAsync()) return;
        QElapsedTimer timer;
        timer.start();
        reevaluate(true);
        lastDabMs_ = timer.nsecsElapsed() / 1e6;
        pressShown_ = brush_.stroking();
    });
    // The release commits what the tool holds: no dab after it. One still on the worker is not
    // waited for (the commit evaluates the view's document, not the stroke's): it lands unshown,
    // or the next evaluation here takes it in first.
    connect(viewport_, &Viewport::strokeReleasing, this, [this] { dabWanted_ = false; });
    connect(viewport_, &Viewport::strokeEnded, this, [this] { guidesEdited(); });
    connect(viewport_, &Viewport::brushSettingsChanged, this, [this] {
        groomTools_->refresh();
        refreshTool();
    });
    connect(viewport_, &Viewport::selectionChanged, this, [this] {
        redisplay();
        guidesPanel_->invalidate();
        groomTools_->refreshTubes();  // the Tubes section's targets
    });
    connect(viewport_, &Viewport::deleteRequested, this, [this] { guidesPanel_->deleteSelected(); });
    connect(viewport_, &Viewport::message, this, [this](const QString& text) { report(text); });
    connect(viewport_, &Viewport::toolModeChanged, this, [this](Viewport::ToolMode mode) {
        if (mode == Viewport::ToolMode::Brush && locked_ && locked_->isChecked()) {
            // A locked groom ignores brushes (Maya's locked attribute): the camera again.
            report(tr("The groom is locked: unlock it (Properties > Groom > Locked) to brush it"));
            viewport_->setToolMode(Viewport::ToolMode::Camera);
            return;
        }
        if (mode != Viewport::ToolMode::Brush && maps_) {
            maps_->endPainting();  // done painting: its map off the scalp, the strands as they were
            endPaintView();
        }
        cameraMode_->setChecked(mode == Viewport::ToolMode::Camera);
        brushMode_->setChecked(mode == Viewport::ToolMode::Brush);
        selectMode_->setChecked(mode == Viewport::ToolMode::Select);
        refreshTool();
        if (mode == Viewport::ToolMode::Brush) {
            notify(tr("Brush: drag to stroke, Ctrl is the opposite, Shift+drag smooths, Ctrl+Shift+drag paints guides into "
                      "the selection (the rest freeze; over no guide, all unfreeze); B+drag (or middle-drag) sets the radius, "
                      "M+drag the strength; Alt+Shift+X symmetry; Y+click the marking menu; Alt orbits; Esc leaves"));
            if (document_) reevaluate();  // warms the stroke document
        } else if (mode == Viewport::ToolMode::Select) {
            notify(tr("Select: click or box guides (the rest freeze; nothing picked unfreezes all); Shift toggles, Ctrl "
                      "deselects, Ctrl+Shift adds; Delete removes them"));
        }
    });
    // Tab in a text field moves the focus rather than opening the graph (eventFilter).
    qApp->installEventFilter(this);
    refreshTitle();
    refreshTool();
    refreshStats();
    viewport_->setFocus();
}

MainWindow::~MainWindow() = default;

void MainWindow::setInteractive(bool on) {
    interactive_ = on;
    if (on) {
        maps_->setDeleteFiles(QSettings().value(QStringLiteral("maps/deleteFiles"), true).toBool());
        setHud(QSettings().value(QStringLiteral("ui/hud"), true).toBool());  // Review > Strand Count HUD as left
        // The first-groom card stays away once its x was clicked (buildPanels ran before this).
        starterDismissed_ = QSettings().value(QStringLiteral("ui/hideStarter"), false).toBool();
        host_->setStarterDismissed(starterDismissed_);
    }
    restartAutosave();  // (an interactive window's alone)
}

void MainWindow::buildMenuBar() {
    // The menu bar from the shared UI definitions (menus.json "app.menuBar": File, Edit, View, the
    // GroomLab menu, Help) with the window's bindings (app_actions.h); its shortcuts work from
    // anywhere in the window, and it and its menus wear the window's style sheet (the theme).
    QMenuBar* bar = menuBar();
    bar->setObjectName(QStringLiteral("menuBar"));
    const std::map<std::string, QAction*> made = buildDefinitionsMenuBar(bar, *this);
    undo_ = made.at("edit.undo");
    redo_ = made.at("edit.redo");
    save_ = made.at("file.save");
    saveAs_ = made.at("file.saveAs");
    reload_ = made.at("file.reload");
    export_ = made.at("file.export");
    saveImage_ = made.at("file.saveImage");
    cameraMode_ = made.at("tool.camera");
    brushMode_ = made.at("tool.brush");
    selectMode_ = made.at("tool.select");
    graphAction_ = made.at("graph.open");
    railLabels_ = made.at("view.railLabels");
    transfer_ = made.at("groom.transfer");
    reference_ = made.at("photos.guides");
    deleteGuides_ = made.at("guides.deleteSelected");
    symmetryAction_ = made.at("brush.symmetry");
    // The Groom Tools' More menu, empty state and Parts buttons: the same bindings.
    groomTools_->setBindings(AppActions::bindings(*this), AppActions::fillOptions(*this));
    buildMenuCorner();
    // The lists' bars and right-click menus (lists.json: guide groups, maps, correctives).
    {
        const ui::Bindings bindings = AppActions::bindings(*this);
        const ui::FillOptions options = AppActions::fillOptions(*this);
        guidesPanel_->setActions(bindings, options);
        maps_->setActions(bindings, options);
        correctives_->setActions(bindings, options);
        photos_->setActions(bindings, options);
    }
    // The Graph Editor's bar and menus (graph_ui.json: Add Node, Presets, Edit, the right-click
    // menus), once every binding they read exists.
    AppActions::wireGraphEditor(*this);
}

void MainWindow::buildMenuCorner() {
    // No corner on the menu bar: the Properties dock's tabs are the modes (Attributes, Material,
    // Lighting and Render groom and look; Cards and Export their own), the Render tab holds the
    // render actions (menus.json's output.render, also in the GroomLab menu) and the Export tab
    // the export. menuCorner_, modeSwitch_ and the corner buttons stay null.
    const ui::Bindings bindings = AppActions::bindings(*this);
    ui::ResolveOptions resolve = AppActions::fillOptions(*this).resolve;
    resolve.state.reset();
    resolve.keep = [&bindings](const std::string& id) { return bindings.count(id) > 0; };
    resolve.dropGenerators = true;
    auto* card = new QWidget();
    card->setObjectName(QStringLiteral("renderActions"));
    auto* column = new QVBoxLayout(card);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(6);
    auto* title = new QLabel(tr("Render out"), card);
    title->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; font-weight: 600; margin-top: 4px;").arg(theme::text().name()));
    column->addWidget(title);
    for (const ui::Item& item : ui::Definitions::shipped().resolve(ui::Definitions::shipped().get("menus/output.render"), resolve)) {
        if (item.kind == ui::Item::Kind::Divider) {
            column->addSpacing(4);
            continue;
        }
        if (item.kind != ui::Item::Kind::Action) continue;
        auto* b = new QPushButton(QString::fromStdString(item.label), card);
        b->setObjectName(QStringLiteral("render_") + QString::fromStdString(item.id));
        b->setToolTip(QString::fromStdString(item.tip));
        b->setProperty("role", "disclosure");  // (outlined: an action that opens a render)
        const ui::Binding binding = bindings.at(item.id);
        connect(b, &QPushButton::clicked, this, [binding, value = item.value] { binding.trigger(value, false); });
        column->addWidget(b);
    }
    shell_->lookCard()->addRenderWidget(card);
    // The Properties pages' shortcuts in the rail's tooltips (the actions' live keys: Ctrl+1 Attributes...).
    shell_->setFoldKeys(QKeySequence(Qt::CTRL | Qt::Key_BracketLeft).toString(QKeySequence::NativeText),
                        QKeySequence(Qt::CTRL | Qt::Key_BracketRight).toString(QKeySequence::NativeText));
    shell_->setPageKeys([&bindings](const std::string& id) {
        const auto it = bindings.find(id);
        return it == bindings.end() || it->second.keys.isEmpty() ? QString() : it->second.keys.first().toString(QKeySequence::NativeText);
    });
}

void MainWindow::fitMenuCorner() {
    // The menu bar sizes its corner widget when it lays itself out (a resize), not when the
    // corner's content changes: the name or the theme's font. Lay it out again at its new size.
    if (!menuCorner_) return;
    menuCorner_->ensurePolished();
    for (QWidget* w : menuCorner_->findChildren<QWidget*>()) w->ensurePolished();
    if (menuCorner_->layout()) menuCorner_->layout()->invalidate();
    menuCorner_->adjustSize();
    QResizeEvent relayout(menuBar()->size(), menuBar()->size());
    QCoreApplication::sendEvent(menuBar(), &relayout);
}

void MainWindow::fillPanelsMenu(QMenu* menu) {
    // View > Panels: the docks shown and hidden; then the sections inside them, raised; the layout
    // put back as it was at first.
    for (QDockWidget* dock : shell_->docks()) {
        QAction* toggle = dock->toggleViewAction();
        toggle->setObjectName(QStringLiteral("toggle_") + dock->objectName());
        menu->addAction(toggle);
    }
    menu->addSeparator();
    const std::pair<const char*, const char*> sections[] = {{"guides", "Guide Groups"}, {"maps", "Maps"}, {"correctives", "Correctives"},
                                                            {"brush", "Brush Settings"}, {"display", "Display"}};
    for (const auto& [name, label] : sections) {
        QAction* a = menu->addAction(tr(label), this, [this, n = QString::fromLatin1(name)] { showUiPanel(n); });
        a->setObjectName(QStringLiteral("panel_") + QLatin1String(name));
    }
    menu->addSeparator();
    menu->addAction(tr("Reset Layout"), this, [this] {
        for (QDockWidget* dock : shell_->docks()) dock->setFloating(false);
        setUiMode(QStringLiteral("groom"));
        shell_->groomToolsDock()->show();
        shell_->propertiesDock()->show();
        shell_->propertiesDock()->raise();
        shell_->resetLayout();
    })->setObjectName(QStringLiteral("resetLayout"));
}

namespace {

// The saved docks' layout: a new version (docks added, renamed or moved) starts from the default.
constexpr int kLayoutVersion = 1;

}  // namespace

void MainWindow::saveLayout() {
    QSettings settings;
    settings.setValue(QStringLiteral("ui/windowGeometry"), saveGeometry());
    settings.setValue(QStringLiteral("ui/windowState"), saveState(kLayoutVersion));
    // The side docks folded to their rails, and the widths they open at again.
    for (const bool left : {true, false}) {
        const QString side = left ? QStringLiteral("Left") : QStringLiteral("Right");
        settings.setValue(QStringLiteral("ui/fold") + side, shell_->folded(left));
        settings.setValue(QStringLiteral("ui/openWidth") + side, shell_->openWidth(left));
    }
}

bool MainWindow::restoreLayout() {
    const QSettings settings;
    const QByteArray state = settings.value(QStringLiteral("ui/windowState")).toByteArray();
    if (state.isEmpty()) return false;
    const QByteArray geometry = settings.value(QStringLiteral("ui/windowGeometry")).toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
    if (!restoreState(state, kLayoutVersion)) return false;
    layoutShown_ = true;  // the saved sizes, not the defaults
    shell_->layoutRestored();
    for (const bool left : {true, false}) {
        const QString side = left ? QStringLiteral("Left") : QStringLiteral("Right");
        shell_->restoreFold(left, settings.value(QStringLiteral("ui/fold") + side, false).toBool(),
                            settings.value(QStringLiteral("ui/openWidth") + side, 0).toInt());
    }
    return true;
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    if (layoutShown_) return;
    layoutShown_ = true;
    shell_->resetLayout();
}

void MainWindow::openDocs() {
    // The web docs (the app's page); without a browser the docs folder shipped beside the
    // executable, else the source tree's.
    if (QDesktopServices::openUrl(QUrl(QStringLiteral("https://getgroomlab.com/docs/app")))) {
        notify(tr("Opened the docs at getgroomlab.com/docs/app"));
        return;
    }
    const QString sourceDocs = QDir(QString::fromUtf8(GROOMLAB_SHIPPED_PRESETS)).absoluteFilePath(QStringLiteral("../../../docs"));
    for (const QString& dir : {QCoreApplication::applicationDirPath() + QStringLiteral("/docs"), QDir::cleanPath(sourceDocs)}) {
        if (QFileInfo(dir + QStringLiteral("/USER_GUIDE.md")).isFile()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
            notify(tr("Opened the docs folder %1").arg(dir));
            return;
        }
    }
    report(tr("No browser opened the docs: see https://getgroomlab.com/docs/app"));
}

void MainWindow::openDonate() {
    // GroomLab is free for any use, funded by donations.
    if (QDesktopServices::openUrl(QUrl(QString::fromLatin1(kDonateUrl)))) {
        notify(tr("Opened %1: thank you for supporting GroomLab").arg(QString::fromLatin1(kDonateUrl)));
        return;
    }
    report(tr("No browser opened: support GroomLab at %1").arg(QString::fromLatin1(kDonateUrl)));
}

bool MainWindow::chooseTheme(const QString& name) {
    if (!theme::themeNames().contains(name)) {
        report(tr("No theme named %1").arg(name));
        return false;
    }
    // Worn at once (every widget restyled) and saved for the next start.
    if (name != theme::currentTheme()) {
        const std::vector<QColor> before = theme::namedColors();
        const QString sheet = theme::styleSheet();
        theme::setTheme(name);
        restyle(before, sheet);
        fitMenuCorner();  // (the new theme's font and padding)
        shell_->leftRail()->refreshIcons();  // (the rails' icons in the new theme's tones)
        shell_->rightRail()->refreshIcons();
    }
    theme::saveTheme(name);
    for (QAction* a : findChildren<QAction*>()) {
        if (a->objectName().startsWith(QStringLiteral("theme_"))) a->setChecked(a->objectName() == QStringLiteral("theme_") + name);
    }
    notify(tr("Theme: %1").arg(theme::themeLabel(name)));
    return true;
}

void MainWindow::setRailLabels(bool shown, bool save) {
    shell_->setRailLabels(shown);
    if (railLabels_) railLabels_->setChecked(shown);
    if (save) QSettings().setValue(QStringLiteral("ui/toolLabels"), shown);
}

bool MainWindow::railLabels() const { return shell_->railLabels(); }

void MainWindow::restoreRailLabels() { setRailLabels(QSettings().value(QStringLiteral("ui/toolLabels"), true).toBool(), false); }

void MainWindow::checkForUpdates() {
    const QString current = QCoreApplication::applicationVersion().isEmpty() ? QStringLiteral("0") : QCoreApplication::applicationVersion();
    const auto show = [this](const QString& text) {
        help_->setText(QString(text).replace(QStringLiteral("@link@"), theme::linkText().name()));
        shell_->setPanelTitle(AppShell::Panel::Help, tr("Updates"), theme::blue());
        shell_->showPanel(AppShell::Panel::Help, true);
    };
    show(tr("<p>Checking for updates...</p>"));
    if (!updates_) updates_ = new UpdateChecker(this);
    updates_->check(current, [this, show](const UpdateInfo& info) {
        QString text = QStringLiteral("<p>%1.</p>").arg(info.message.toHtmlEscaped());
        if (info.newer) {
            if (!info.notes.isEmpty()) {
                text += QStringLiteral("<p><b>%1</b><br>%2</p>")
                            .arg(tr("What's new"), info.notes.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
            }
            text += tr("<p><a style='color:@link@' href='%1'>Download GroomLab %2</a>, close GroomLab and unzip it in "
                       "place of this folder (your settings are kept).</p>")
                        .arg(info.url.toHtmlEscaped(), info.latest.toHtmlEscaped());
        }
        show(text.replace(QStringLiteral("@link@"), theme::linkText().name()));
        report(info.message, !info.ok);
    });
}

bool MainWindow::showHelp(const QString& page) {
    QString title, text;
    if (page == QStringLiteral("shortcuts")) {
        title = tr("Keyboard Shortcuts");
        // The one shortcut table every host shows (groomlab/shortcuts.h: the docs site reads the
        // same rows), the app's, a heading row per context (File, Modes, Brush, Viewport, Graph).
        // Keycaps (one per key, a row's alternatives under each other) beside the action, rows of
        // a context with one action merged ("B + drag", "Middle-drag": Brush radius), caps group
        // headers; filtered by the field over it.
        struct Row {
            std::string context, action;
            QStringList keys;
        };
        std::vector<Row> rows;
        const QString filter = helpFilter_ ? helpFilter_->text().trimmed() : QString();
        for (const Shortcut& s : shortcutsFor(kShortcutApp)) {
            if (!filter.isEmpty() && !QString::fromStdString(s.keys).contains(filter, Qt::CaseInsensitive) &&
                !QString::fromStdString(s.action).contains(filter, Qt::CaseInsensitive) &&
                !QString::fromStdString(s.context).contains(filter, Qt::CaseInsensitive)) {
                continue;
            }
            // Alternatives on their own caps ("Ctrl+Z / Ctrl+Y"); a run of one key's variants stays
            // one cap ("Ctrl+1 / 2 / 3 / 4").
            QStringList keys = QString::fromStdString(s.keys).split(QStringLiteral(" / "));
            if (std::all_of(keys.begin() + 1, keys.end(), [](const QString& k) { return k.size() <= 2; })) keys = {QString::fromStdString(s.keys)};
            auto same = std::find_if(rows.begin(), rows.end(), [&](const Row& r) { return r.context == s.context && r.action == s.action; });
            if (same != rows.end()) same->keys << keys;
            else rows.push_back({s.context, s.action, keys});
        }
        auto* grid = static_cast<QGridLayout*>(shortcutSheet_->layout());
        while (QLayoutItem* item = grid->takeAt(0)) {
            if (QWidget* w = item->widget()) w->deleteLater();
            delete item;
        }
        grid->setContentsMargins(0, 0, 0, 12);
        grid->setHorizontalSpacing(0);
        grid->setVerticalSpacing(8);
        grid->setColumnStretch(0, 1);
        // Each group a card (the style sheet's tray): its title, then a keycap column and the actions.
        int line = 0;
        std::string context;
        QVBoxLayout* card = nullptr;
        QWidget* cardHost = nullptr;
        for (const Row& r : rows) {
            if (r.context != context || !card) {
                context = r.context;
                cardHost = new QWidget(shortcutSheet_);
                if (!theme::glassy()) {
                    cardHost->setProperty("role", "tray");
                    cardHost->setAttribute(Qt::WA_StyledBackground, true);
                }
                card = new QVBoxLayout(cardHost);
                card->setContentsMargins(12, 10, 12, 12);
                card->setSpacing(6);
                auto* group = new QLabel(QString::fromStdString(r.context), cardHost);
                group->setProperty("role", "keyGroup");
                card->addWidget(group);
                grid->addWidget(cardHost, line++, 0);
            }
            // A row: the action, then its keys at the right on one line (an alternative after a dot).
            auto* rowLine = new QHBoxLayout();
            rowLine->setSpacing(8);
            auto* action = new QLabel(QString::fromStdString(r.action), cardHost);
            action->setProperty("role", "keyAction");
            action->setWordWrap(true);
            action->setMinimumHeight(20);
            rowLine->addWidget(action, 1);
            // Its keys at the right: one, or several stacked (the action never wraps beside them).
            auto* capColumn = new QVBoxLayout();
            capColumn->setSpacing(3);
            for (const QString& k : r.keys) {
                auto* cap = new QLabel(k, cardHost);
                cap->setProperty("role", "keycap");
                cap->setMinimumHeight(20);
                capColumn->addWidget(cap, 0, Qt::AlignRight);
            }
            rowLine->addLayout(capColumn);
            card->addLayout(rowLine);
        }
        if (rows.empty()) {
            auto* none = new QLabel(tr("No shortcut matches."), shortcutSheet_);
            none->setProperty("role", "hint");
            grid->addWidget(none, 0, 0, 1, 2);
        }
    } else if (page == QStringLiteral("about")) {
        title = tr("About GroomLab");
        text = tr("<p><b>GroomLab</b> %1</p><p>Hair, fur and feather grooming: the Maya plugin and this standalone app share "
                  "one core, and a .groom file travels between them.</p><p>Free for any use, commercial included. "
                  "<a style='color:@link@' href='%2'>Support GroomLab</a> with a donation.</p><p>Open source (Apache-2.0): "
                  "<a style='color:@link@' href='%3'>github.com/thekesslereffect/groomlab</a></p><p>Fonts: Inter and "
                  "JetBrains Mono (SIL OFL). Qt 6 (LGPL). See THIRD_PARTY_NOTICES.txt beside the executable.</p>")
                   .arg(QCoreApplication::applicationVersion().isEmpty() ? tr("(development build)") : QCoreApplication::applicationVersion(),
                        QString::fromLatin1(kDonateUrl), QString::fromLatin1(kRepoUrl));
    } else {
        report(tr("No help page '%1' (shortcuts, about)").arg(page));
        return false;
    }
    help_->setText(text.replace(QStringLiteral("@link@"), theme::linkText().name()));
    const bool sheet = page == QStringLiteral("shortcuts");
    if (helpFilter_) helpFilter_->setVisible(sheet);
    if (shortcutSheet_) shortcutSheet_->setVisible(sheet);
    help_->setVisible(!sheet);
    shell_->setPanelTitle(AppShell::Panel::Help, title, theme::blue());
    shell_->showPanel(AppShell::Panel::Help, true);
    return true;
}

void MainWindow::refreshNodes() {
    const QString current = nodes_->currentItem() ? nodes_->currentItem()->data(Qt::UserRole).toString() : QString();
    nodes_->blockSignals(true);
    nodes_->clear();
    int select = -1;
    if (document_) {
        for (const GroomDocument::NodeInfo& node : document_->nodes()) {
            // The type by its label ("Linked Groom" for a Reference node), as every host shows it.
            auto* item = new QListWidgetItem(QString::fromStdString(node.name + "  (" + nodeLabel(node.type) + ")") +
                                             (node.bypass ? tr("  bypassed") : QString()));
            item->setData(Qt::UserRole, QString::fromStdString(node.name));
            nodes_->addItem(item);
            if (QString::fromStdString(node.name) == current) select = nodes_->count() - 1;
        }
    }
    nodes_->blockSignals(false);
    if (select >= 0) nodes_->setCurrentRow(select);
}

void MainWindow::refreshUndoActions() {
    const bool has = document_ != nullptr;
    const bool canUndo = has && document_->canUndo(), canRedo = has && document_->canRedo();
    // The menus' enabled states, texts and checks from their bindings (Undo's text, what needs a groom).
    ui::refreshActions(menuBar());
    // The viewport's Undo and Redo: the step's label and keys in the tips.
    host_->setUndoRedo(canUndo, canRedo, undo_->text().section(QLatin1Char('\t'), 0, 0) + QStringLiteral(" (Ctrl+Z)"),
                       redo_->text().section(QLatin1Char('\t'), 0, 0) + QStringLiteral(" (Ctrl+Y)"));
    refreshTitle();
}

void MainWindow::refreshTitle() {
    // The groom's file first, a * while it has unsaved edits ("strokes.groom * — GroomLab"): the
    // window has no tool bar to show it in.
    QString title = QStringLiteral("GroomLab");
    QString name;
    bool dirty = false;
    if (document_) {
        name = document_->path().empty() ? tr("untitled.groom") : QFileInfo(QString::fromStdString(document_->path())).fileName();
        dirty = anyGroomDirty();
        title = name + (dirty ? QStringLiteral(" *") : QString()) + QStringLiteral(" — GroomLab");
    }
    setWindowTitle(title);
    shell_->setFileName(name, dirty);
    if (modeSwitch_) modeSwitch_->setEnabled(document_ != nullptr);
    refreshExportState();
}

void MainWindow::frameAll() {
    if (!document_ && props_.empty()) return;
    viewport_->frameAll(sceneBounds());
    viewport_->syncView();
}

void MainWindow::undo() {
    if (!document_ || !document_->canUndo() || refuseWhileBusy(tr("undoing"))) return;
    const std::string error = document_->undo();
    if (!error.empty()) report(QString::fromStdString(error));
    graphEdited();
    refreshGroups();
}

void MainWindow::redo() {
    if (!document_ || !document_->canRedo() || refuseWhileBusy(tr("redoing"))) return;
    const std::string error = document_->redo();
    if (!error.empty()) report(QString::fromStdString(error));
    graphEdited();
    refreshGroups();
}

void MainWindow::setPreviewDensity(double density) { density_->setValue(density); }

void MainWindow::setPointBudget(double thousands) { budget_->setValue(thousands); }

}  // namespace groomlab::app
