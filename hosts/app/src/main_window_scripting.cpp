// SPDX-License-Identifier: Apache-2.0
// MainWindow: the UI, graph presets and params driven by name (scripted runs and the tests).

#include "main_window_internal.h"

#include <QScrollBar>
#include <QTimer>

#include <algorithm>
#include <map>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDockWidget>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QKeyEvent>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QTimer>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolButton>
#include <QTreeWidget>

#include "app_shell.h"
#include "correctives_panel.h"
#include "guides_panel.h"
#include "list_bar.h"
#include "groom_tools.h"
#include "cards_panel.h"
#include "export_panel.h"
#include "graph_presets.h"
#include "maps_panel.h"
#include "tube_panel.h"
#include "marking_menu.h"
#include "node_graph.h"
#include "overlay.h"
#include "photos_panel.h"
#include "properties_panel.h"
#include "param_panel.h"
#include "theme.h"
#include "viewport.h"

namespace groomlab::app {

// ---- The UI ----------------------------------------------------------------------------------

bool MainWindow::setUiMode(const QString& name) {
    const QString n = name.toLower();
    OverlayHost::Mode mode;
    if (n == QStringLiteral("groom")) mode = OverlayHost::Mode::Groom;
    else if (n == QStringLiteral("look")) mode = OverlayHost::Mode::Look;
    else if (n == QStringLiteral("cards")) mode = OverlayHost::Mode::Cards;
    else if (n == QStringLiteral("export")) mode = OverlayHost::Mode::Export;
    else {
        report(tr("No UI mode '%1' (groom, look, cards, export)").arg(name));
        return false;
    }
    if (mode != shell_->mode()) {
        shell_->setMode(mode);
        // The brushes belong to Groom mode: elsewhere the mouse is the camera's. Cards mode shows
        // the cards (when the graph makes some); leaving it shows the strands again.
        if (mode != OverlayHost::Mode::Groom) viewport_->setToolMode(Viewport::ToolMode::Camera);
        const bool cards = mode == OverlayHost::Mode::Cards && document_ && !document_->cardsNodes().empty();
        if (shell_->cardsPanel()->showCards() != cards) {
            shell_->cardsPanel()->setShowCards(cards);
            reevaluate();
        } else if (document_) {
            redisplay();  // the guides show in Groom mode alone
        }
        refreshStats();
    }
    return true;
}

void MainWindow::setGraphShown(bool shown) {
    if (shown && !document_) {
        report(tr("No groom open: nothing to show in the graph"));
        return;
    }
    graph_->setDocument(shown ? document_.get() : nullptr);
    if (shown) {  // the badges from the last evaluation
        graph_->setNodeTimes(document_->nodeTimes());
        graph_->setNodeNotes(document_->nodeNotes());
        graph_->setNodeErrors(document_->nodeErrors());
    }
    const bool hadFocus = graph_->hasFocus();
    shell_->setGraphShown(shown, shown ? graph_->nodeCount() : (document_ ? static_cast<int>(document_->nodes().size()) : 0));
    if (!shown && hadFocus) viewport_->setFocus();
}

bool MainWindow::graphShown() const { return shell_->graphShown(); }

bool MainWindow::showUiPanel(const QString& name) {
    const QString n = name.toLower();
    AppShell::Panel panel;
    if (n == QStringLiteral("guides")) panel = AppShell::Panel::Guides;
    else if (n == QStringLiteral("nodes")) panel = AppShell::Panel::Nodes;
    else if (n == QStringLiteral("maps")) panel = AppShell::Panel::Maps;
    else if (n == QStringLiteral("correctives")) panel = AppShell::Panel::Correctives;
    else if (n == QStringLiteral("look")) panel = AppShell::Panel::Look;
    else if (n == QStringLiteral("display")) panel = AppShell::Panel::Display;
    else if (n == QStringLiteral("brush")) panel = AppShell::Panel::Brush;
    else if (n == QStringLiteral("shortcuts") || n == QStringLiteral("help")) return showHelp(QStringLiteral("shortcuts"));
    else if (n == QStringLiteral("tubes")) {
        // The tool card's tube blocks (the Shape brush's when no tool shows them), scrolled to (Tools tab).
        setUiMode(QStringLiteral("groom"));
        shell_->groomToolsDock()->show();
        groomTools_->showTab(QStringLiteral("tools"));
        QWidget* tubes = groomTools_->tubePanel();
        if (tubes->isHidden()) chooseTool(QStringLiteral("shape"));
        groomTools_->refreshTubes();
        // Scrolled to its header once laid out (the section's top at the dock's top).
        auto scroll = [this, tubes] {
            QScrollArea* area = groomTools_->scrollArea();
            area->verticalScrollBar()->setValue(tubes->mapTo(area->widget(), QPoint(0, 0)).y());
        };
        QCoreApplication::processEvents();
        scroll();
        QTimer::singleShot(0, this, scroll);
        return true;
    }
    else if (n == QStringLiteral("properties") || n == QStringLiteral("properties-end")) {
        // The Properties dock with every section open (a screenshot's), at its top or its end.
        setUiMode(QStringLiteral("groom"));
        shell_->showPanel(AppShell::Panel::Display, true);
        properties_->expandAll(true);
        if (n.endsWith(QStringLiteral("-end"))) {
            // At its end once the open sections are laid out (now, and again after the layout settles).
            auto toEnd = [this] {
                if (QLayout* l = properties_->scrollArea()->widget()->layout()) l->activate();
                QScrollBar* bar = properties_->scrollArea()->verticalScrollBar();
                bar->setValue(bar->maximum());
            };
            toEnd();
            QTimer::singleShot(0, this, toEnd);
            QTimer::singleShot(150, this, toEnd);
        }
        return true;
    } else if (n == QStringLiteral("photos-look")) {
        // The Photos tab looking through the groom's first photo (a screenshot's).
        setUiMode(QStringLiteral("groom"));
        shell_->groomToolsDock()->show();
        groomTools_->showTab(QStringLiteral("photos"));
        if (photos_->count() == 0) {
            report(tr("The groom has no reference photos to look through"));
            return false;
        }
        photos_->select(0);
        lookThroughPhoto(0, true);
        return true;
    }
    else if (n == QStringLiteral("tools") || n == QStringLiteral("paint") || n == QStringLiteral("photos")) {
        // A Groom Tools tab by palette.json's id.
        setUiMode(QStringLiteral("groom"));
        shell_->groomToolsDock()->show();
        return groomTools_->showTab(n);
    } else {
        report(tr("No panel '%1' (guides, nodes, maps, correctives, look, display, properties, properties-end, brush, tools, tubes, paint, photos, photos-look, shortcuts)").arg(name));
        return false;
    }
    // The Groom Tools' sections and the Display options belong to Groom mode.
    if (panel == AppShell::Panel::Brush || panel == AppShell::Panel::Guides || panel == AppShell::Panel::Display ||
        panel == AppShell::Panel::Maps || panel == AppShell::Panel::Correctives) {
        setUiMode(QStringLiteral("groom"));
    }
    shell_->showPanel(panel, true);
    return true;
}

bool MainWindow::setUiSearch(const QString& text) {
    if (!document_) {
        report(tr("No groom open: no Graph Editor to search"));
        return false;
    }
    setGraphShown(true);
    graph_->openSearchAt(QPointF(graph_->width() * 0.35, graph_->height() * 0.25));  // (no cursor in a scripted run)
    graph_->setSearchText(text);
    return true;
}

bool MainWindow::setUiLookTab(const QString& name) {
    const QStringList words = name.toLower().split(QLatin1Char(','), Qt::SkipEmptyParts);
    const QString n = words.isEmpty() ? QString() : words.first().trimmed();
    LookCard::Tab tab;
    if (n == QStringLiteral("material")) tab = LookCard::Tab::Material;
    else if (n == QStringLiteral("lighting")) tab = LookCard::Tab::Lighting;
    else if (n == QStringLiteral("render")) tab = LookCard::Tab::Render;
    else {
        report(tr("No look tab '%1' (material, lighting, render; ',more' opens the full settings)").arg(name));
        return false;
    }
    setUiMode(QStringLiteral("look"));
    shell_->setPropertiesTab(static_cast<AppShell::PropertiesTab>(static_cast<int>(tab) + 1));
    shell_->lookCard()->setMoreShown(words.contains(QStringLiteral("more")));
    return true;
}

bool MainWindow::setUiHover(const QString& name) {
    const QString n = name.toLower();
    if (n != QStringLiteral("rail") && n != QStringLiteral("panels") && n != QStringLiteral("all") && n != QStringLiteral("labels")) {
        report(tr("No hover state '%1' (rail, panels, all, labels)").arg(name));
        return false;
    }
    // The tiles' names under their icons (not saved); the panels' names are their docks'.
    if (n != QStringLiteral("panels")) setRailLabels(true, false);
    return true;
}

bool MainWindow::setUiPrompt(const QString& name) {
    const QString n = name.toLower();
    if (n == QStringLiteral("unsaved") || n == QStringLiteral("reload")) {
        // As an interactive window asks (an edit made first when none is), nothing done on a choice.
        const bool was = interactive_;
        interactive_ = true;
        if (document_ && !document_->dirty() && !document_->nodes().empty()) {
            const std::string node = document_->nodes().front().name;
            float x = 0.0f, y = 0.0f;
            document_->nodePosition(node, &x, &y);
            document_->setNodePosition(node, x, y);  // (the file dirty, nothing moved)
        }
        if (n == QStringLiteral("unsaved")) confirmDiscard(tr("closing"), {});
        else askReload();
        interactive_ = was;
        return host_->promptShown();
    }
    if (n == QStringLiteral("info")) {
        host_->toast(tr("Saved strokes.groom"), false);
        return true;
    }
    if (n == QStringLiteral("error")) {
        host_->toast(tr("Cannot export: the folder is read-only. Pick another output folder in the Export panel "
                        "(renders/hair/a_long_folder_name/strokes.abc)"),
                     true);
        return true;
    }
    if (n == QStringLiteral("autosave")) {
        // The prompt the next start shows after a crash: the open groom (an edit made first) written
        // to its autosave and offered; the entry and the file gone again (the prompt stays shown).
        if (!document_) {
            report(tr("No groom open: no autosave to offer"));
            return false;
        }
        if (!document_->dirty() && !document_->nodes().empty()) {
            const std::string node = document_->nodes().front().name;
            float x = 0.0f, y = 0.0f;
            document_->nodePosition(node, &x, &y);
            document_->setNodePosition(node, x, y);
        }
        if (!autosaveNow() || !offerAutosave()) return false;
        dropAutosave();
        return host_->promptShown();
    }
    report(tr("No prompt '%1' (unsaved, reload, info, error, autosave)").arg(name));
    return false;
}

bool MainWindow::setUiMenu(const QString& name) {
    const QString n = name.toLower();
    if (n == QStringLiteral("marking")) {
        // The marking menu (Y + click) a little above the viewport's middle, Comb (its N) hovered.
        QApplication::processEvents();
        const QPointF middle = viewport_->mapTo(host_, QPointF(viewport_->width() / 2.0, viewport_->height() * 0.4));
        if (!showMarkingMenu(middle, false)) return false;
        markingMenu_->track(middle + QPointF(0.0, -60.0));
        return true;
    }
    if (n == QStringLiteral("add") || n == QStringLiteral("edit") || n == QStringLiteral("node")) {
        // The Graph Editor's Add Node menu (its first category open), its Edit menu, or the
        // selected node's right-click menu (the Interpolate node's when none is), popped up for
        // real and grabbed as setUiMenu("presets") does.
        if (!document_) {
            report(tr("No groom open: no graph menu to show"));
            return false;
        }
        if (!graphShown()) setGraphShown(true);
        QApplication::processEvents();
        uiMenus_.clear();
        auto keep = [this](QMenu* menu) {
            if (menu->isVisible()) uiMenus_.emplace_back(menu->grab(), mapFromGlobal(menu->pos()));
        };
        QMenu* menu = nullptr;
        QPoint at;
        if (n == QStringLiteral("node")) {
            if (graph_->selectedNode().empty()) graph_->selectNode(lastNodeOfType("Interpolate"));
            graph_->bringIntoView(graph_->selectedNode());
            const QRectF card = graph_->nodeRect(graph_->selectedNode());
            menu = new QMenu(this);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            graph_->fillContextMenu(menu, card.center());
            at = graph_->mapToGlobal(card.center().toPoint());
        } else {
            QToolButton* button = n == QStringLiteral("add") ? shell_->graphAddButton() : shell_->graphEditButton();
            menu = button->menu();
            at = button->mapToGlobal(QPoint(0, button->height() + 2));
        }
        if (!menu) return false;
        menu->popup(at);
        QApplication::processEvents();
        keep(menu);
        if (n == QStringLiteral("add") && !menu->actions().isEmpty() && menu->actions().front()->menu()) {
            menu->setActiveAction(menu->actions().front());
            QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
            QApplication::sendEvent(menu, &right);
            QApplication::processEvents();
            keep(menu->actions().front()->menu());
        }
        return !uiMenus_.empty();
    }
    if (n == QStringLiteral("groups") || n == QStringLiteral("maps") || n == QStringLiteral("correctives") ||
        n == QStringLiteral("editguides") || n == QStringLiteral("convert")) {
        // A Groom Tools list's right-click menu on its current row (lists.json's context), Edit
        // Guides' menu, or the maps' Convert to: the list shown (Groom mode, its tab or section),
        // the menu popped up for real and grabbed.
        if (!document_) {
            report(tr("No groom open: no list to show"));
            return false;
        }
        setUiMode(QStringLiteral("groom"));
        shell_->groomToolsDock()->show();
        const bool groups = n == QStringLiteral("groups") || n == QStringLiteral("editguides");
        groomTools_->showTab(groups ? QStringLiteral("guides") : n == QStringLiteral("correctives") ? n : QStringLiteral("paint"));
        QApplication::processEvents();
        uiMenus_.clear();
        QMenu* menu = nullptr;
        QPoint at;
        auto rowCenter = [](QTreeWidget* tree) {
            QTreeWidgetItem* item = tree->currentItem();
            if (!item && tree->topLevelItemCount() > 0) item = tree->topLevelItem(0);
            return item ? tree->visualItemRect(item).center() : QPoint(20, 20);
        };
        if (n == QStringLiteral("groups")) {
            QTreeWidget* tree = guidesPanel_->tree();
            tree->scrollToItem(tree->currentItem());
            const QPoint pos = rowCenter(tree);
            menu = guidesPanel_->contextMenu(pos);
            at = tree->viewport()->mapToGlobal(pos);
        } else if (n == QStringLiteral("editguides")) {
            menu = guidesPanel_->editsMenu();
            at = guidesPanel_->editsButton()->mapToGlobal(QPoint(0, guidesPanel_->editsButton()->height()));
        } else if (n == QStringLiteral("maps")) {
            QListWidget* list = maps_->list();
            QListWidgetItem* item = list->currentItem() ? list->currentItem() : list->item(0);
            const QPoint pos = item ? list->visualItemRect(item).center() : QPoint(20, 20);
            menu = maps_->contextMenu(pos);
            at = list->viewport()->mapToGlobal(pos);
        } else if (n == QStringLiteral("convert")) {
            QToolButton* convert = maps_->bar()->button("map.convert");
            if (!convert) return false;
            menu = convert->menu();
            at = convert->mapToGlobal(QPoint(0, convert->height()));
        } else {
            QTreeWidget* tree = correctives_->tree();
            QTreeWidgetItem* layer = nullptr;
            for (int i = 0; i < tree->topLevelItemCount() && !layer; ++i) {
                if (tree->topLevelItem(i)->data(0, Qt::UserRole).toString() == QLatin1String("layer")) layer = tree->topLevelItem(i);
            }
            if (!layer) {
                correctives_->addLayer(true);  // (one to show the menu of)
                for (int i = 0; i < tree->topLevelItemCount() && !layer; ++i) {
                    if (tree->topLevelItem(i)->data(0, Qt::UserRole).toString() == QLatin1String("layer")) layer = tree->topLevelItem(i);
                }
            }
            if (!layer) return false;
            const QPoint pos = tree->visualItemRect(layer).center();
            menu = correctives_->contextMenu(pos);
            at = tree->viewport()->mapToGlobal(pos);
        }
        if (!menu) return false;
        menu->popup(at);
        QApplication::processEvents();
        if (menu->isVisible()) uiMenus_.emplace_back(menu->grab(), mapFromGlobal(menu->pos()));
        return !uiMenus_.empty();
    }
    if (n.startsWith(QStringLiteral("bar:"))) {
        // A menu of the menu bar and the submenus down a path ("bar:GroomLab/Add", "bar:File/Open
        // Recent"), each popped up for real as the keys open them and grabbed.
        const QStringList path = name.mid(4).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QMenu* menu = nullptr;
        for (QAction* a : menuBar()->actions()) {
            if (!path.isEmpty() && a->menu() && a->text().remove(QLatin1Char('&')).compare(path.front(), Qt::CaseInsensitive) == 0) menu = a->menu();
        }
        if (!menu) {
            report(tr("No menu '%1' in the menu bar").arg(path.value(0)));
            return false;
        }
        uiMenus_.clear();
        auto keep = [this](QMenu* m) {
            if (m->isVisible()) uiMenus_.emplace_back(m->grab(), mapFromGlobal(m->pos()));
        };
        QApplication::processEvents();
        QAction* barAction = menu->menuAction();
        menu->popup(menuBar()->mapToGlobal(menuBar()->actionGeometry(barAction).bottomLeft()));
        QApplication::processEvents();
        for (int i = 1; i < path.size(); ++i) {
            QAction* next = nullptr;
            for (QAction* a : menu->actions()) {
                if (a->menu() && a->isVisible() && a->text().section(QLatin1Char('\t'), 0, 0).compare(path[i], Qt::CaseInsensitive) == 0) next = a;
            }
            if (!next) {
                report(tr("No submenu '%1' in %2").arg(path[i], path[i - 1]));
                menu->hide();
                return false;
            }
            keep(menu);
            menu->setActiveAction(next);
            QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
            QApplication::sendEvent(menu, &right);
            QApplication::processEvents();
            menu = next->menu();
        }
        keep(menu);
        return !uiMenus_.empty();
    }
    if (n != QStringLiteral("presets")) {
        report(tr("No menu '%1' (presets, add, edit, node, groups, editguides, maps, convert, correctives, bar:Menu/Submenu)").arg(name));
        return false;
    }
    if (document_ && !graphShown()) {
        setGraphShown(true);
        QApplication::processEvents();
    }
    // The menu popped up for real under the Presets button, the Add-ons submenu and its first preset's
    // opened as the keys open them (Right: the active item's submenu, its first item active;
    // Down: Insert after selected node), each grabbed at once: a popup closes when the window
    // is not the active one, so grabUi draws the grabs where the menus were.
    uiMenus_.clear();
    showPresetsMenu();
    QApplication::processEvents();
    auto key = [](QWidget* w, int k) {
        QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier);
        QApplication::sendEvent(w, &press);
        QApplication::processEvents();
    };
    auto keep = [this](QMenu* menu) {
        if (menu->isVisible()) uiMenus_.emplace_back(menu->grab(), mapFromGlobal(menu->pos()));
    };
    QAction* addons = nullptr;
    for (QAction* a : presetsMenu_->actions()) {
        if (a->menu() && a->text() == QStringLiteral("Add-ons")) addons = a;
    }
    if (addons && !addons->menu()->actions().empty()) {
        presetsMenu_->setActiveAction(addons);
        key(presetsMenu_, Qt::Key_Right);
        QMenu* sub = addons->menu();
        if (QMenu* first = sub->actions().front()->menu()) {
            sub->setActiveAction(sub->actions().front());
            key(sub, Qt::Key_Right);
            key(first, Qt::Key_Down);
            keep(presetsMenu_);
            keep(sub);
            keep(first);
        }
    }
    if (uiMenus_.empty()) keep(presetsMenu_);
    return !uiMenus_.empty();
}

// ---- Graph presets --------------------------------------------------------------------------------

void MainWindow::showPresetsMenu() {
    // graph_ui.json's presetMenu (AppActions::wireGraphEditor fills it): under the Graph Editor's
    // Presets button while the editor shows, else at the pointer (the empty card's Start from a
    // preset, the start hint's Presets).
    QToolButton* button = shell_->graphPresetsButton();
    if (graphShown() && button->isVisible()) presetsMenu_->popup(button->mapToGlobal(QPoint(0, button->height() + 2)));
    else presetsMenu_->popup(QCursor::pos());
}

bool MainWindow::startFromPreset(const QString& nameOrPath, const QString& meshPath) {
    QString mesh = meshPath;
    if (mesh.isEmpty()) {
        // A mesh asked for on a click (never in a scripted run: no dialog may stop it).
        if (!interactive_) {
            report(tr("A preset needs a groom: make one on a mesh first (New Groom on Mesh), then apply %1").arg(nameOrPath));
            return false;
        }
        mesh = QFileDialog::getOpenFileName(this, tr("A Mesh for the Preset"), QString(), tr("Meshes (*.obj *.abc)"));
        if (mesh.isEmpty()) return false;
    }
    if (!newGroomOnMesh(mesh)) return false;
    return applyPreset(nameOrPath);
}

bool MainWindow::applyPreset(const QString& nameOrPath) {
    if (!document_) {
        report(tr("No groom open to apply the preset %1 to").arg(nameOrPath));
        return false;
    }
    std::string path = nameOrPath.toStdString();
    if (!QFileInfo(nameOrPath).isFile()) {
        path = presetPath(nameOrPath.toStdString(), presetDirectories());
        if (path.empty()) {
            report(tr("No preset named %1").arg(nameOrPath));
            return false;
        }
    }
    PresetApplied applied;
    const std::string error = applyPresetFile(*document_, path, &applied);
    if (!error.empty()) {
        report(tr("Cannot apply the preset %1: %2").arg(nameOrPath, QString::fromStdString(error)));
        return false;
    }
    graphEdited();
    if (graphShown()) {
        graph_->refresh();
        graph_->frameReadable();
    }
    if (!applied.groupsMade.empty()) refreshGroups();
    // A hand-placed look: the Draw tool draws into its group, every guide drawn grows one.
    if (!applied.drawGroup.empty() && drawGuidesInto(applied.drawGroup)) {
        notify(tr("Applied the preset %1. Draw guides from the skin into %2: each one grows the look")
                   .arg(QString::fromStdString(presetMessage(applied)), QString::fromStdString(applied.drawGroup)));
        return true;
    }
    notify(tr("Applied the preset %1").arg(QString::fromStdString(presetMessage(applied))));
    return true;
}

bool MainWindow::insertPreset(const QString& nameOrPath) {
    if (!document_) {
        report(tr("No groom open to insert the preset %1 into").arg(nameOrPath));
        return false;
    }
    std::string path = nameOrPath.toStdString();
    if (!QFileInfo(nameOrPath).isFile()) {
        path = presetPath(nameOrPath.toStdString(), presetDirectories());
        if (path.empty()) {
            report(tr("No preset named %1").arg(nameOrPath));
            return false;
        }
    }
    const std::string after = graphShown() ? graph_->selectedNode() : std::string();
    PresetApplied applied;
    const std::string error = insertPresetFile(*document_, path, after, &applied);
    if (!error.empty()) {
        report(tr("Cannot insert the preset %1: %2").arg(nameOrPath, QString::fromStdString(error)));
        return false;
    }
    graphEdited();
    if (graphShown()) {
        graph_->refresh();
        graph_->selectNode(applied.nodeMade);
        graph_->frameReadable();
    }
    report(tr("Inserted %1 as %2 after %3 (Ctrl+Z undoes it)")
               .arg(QString::fromStdString(presetMessage(applied)), QString::fromStdString(applied.nodeMade),
                    after.empty() ? tr("the last node") : QString::fromStdString(after)),
           false);
    return true;
}

bool MainWindow::paintParam(const QString& node, const QString& param, const QString& kind) {
    if (!document_) {
        report(tr("No groom open: nothing to paint"));
        return false;
    }
    std::string map;
    bool made = false;
    const std::string error = document_->paintParam(node.toStdString(), param.toStdString(), &map, kind.toStdString(), &made);
    if (!error.empty()) {
        report(tr("Cannot paint %1.%2: %3").arg(node, param, QString::fromStdString(error)));
        return false;
    }
    if (made) graphEdited();
    // The Paint brush on the map (picked), toward 1: strokes paint the param back to its value,
    // Ctrl toward 0 (on a density map, no hair), as Maya's starts it.
    maps_->refresh();
    maps_->selectMap(map);
    maps_->setPaintValue(1.0);
    if (!maps_->paintSelected()) return false;  // brush mode (paintRequested)
    report(tr("%1 %2, which %3.%4 reads: Ctrl paints it toward 0%5, strokes back toward the value")
               .arg(made ? tr("Made") : tr("Painting"), QString::fromStdString(map), node, param,
                    param == QStringLiteral("density") ? tr(" (no hair there)") : QString()),
           false);
    return true;
}

bool MainWindow::savePreset(const QString& name, bool overwrite) {
    if (!document_) {
        report(tr("No groom open to save as a preset"));
        return false;
    }
    std::string path;
    const std::string error = savePresetFile(*document_, name.toStdString(), overwrite, &path);
    if (!error.empty()) {
        report(tr("Cannot save the preset %1: %2").arg(name, QString::fromStdString(error)));
        return false;
    }
    notify(tr("Saved the preset %1 to %2").arg(name, QString::fromStdString(path)));
    return true;
}

void MainWindow::copyGraph() {
    if (!document_) {
        report(tr("No groom open to copy the graph of"));
        return;
    }
    QApplication::clipboard()->setText(QString::fromStdString(graphAsText(*document_)));
    report(tr("Copied the graph as text (%n nodes): paste it with Presets > Paste Graph on any groom, or save it as a .json preset", "",
              static_cast<int>(document_->nodes().size())),
           false);
}

void MainWindow::pasteGraph() {
    if (!document_) {
        report(tr("No groom open to paste a graph onto"));
        return;
    }
    PresetApplied applied;
    const std::string error = pasteGraphText(*document_, QApplication::clipboard()->text().toStdString(), &applied);
    if (!error.empty()) {
        report(QString::fromStdString(error));
        return;
    }
    graphEdited();
    if (graphShown()) {
        graph_->refresh();
        graph_->frameReadable();
    }
    notify(tr("Pasted %1 (Ctrl+Z undoes it)").arg(QString::fromStdString(presetMessage(applied))));
}

bool MainWindow::selectUiNode(const QString& name) {
    if (!document_) {
        report(tr("No groom open: no node '%1' to select").arg(name));
        return false;
    }
    bool found = false;
    for (const GroomDocument::NodeInfo& n : document_->nodes()) found = found || QString::fromStdString(n.name) == name;
    if (!found) {
        report(tr("No node '%1' in the graph").arg(name));
        return false;
    }
    if (!graphShown()) setGraphShown(true);
    graph_->selectNode(name.toStdString());
    graph_->bringIntoView(name.toStdString());
    return true;
}

bool MainWindow::setDisplayText(const QString& text) {
    const int equals = text.indexOf(QLatin1Char('='));
    if (equals <= 0) {
        report(tr("Expected name=value for the display, got %1").arg(text));
        return false;
    }
    const QString name = text.left(equals).trimmed().toLower(), value = text.mid(equals + 1).trimmed();
    bool ok = true;
    auto flag = [&] {
        const QString v = value.toLower();
        ok = v == QStringLiteral("1") || v == QStringLiteral("0") || v == QStringLiteral("true") || v == QStringLiteral("false") ||
             v == QStringLiteral("on") || v == QStringLiteral("off");
        return v == QStringLiteral("1") || v == QStringLiteral("true") || v == QStringLiteral("on");
    };
    if (name == QStringLiteral("colormode")) {
        static const char* const modes[] = {"solid", "colorset", "attribute", "random", "clump", "density"};
        ok = false;
        for (int i = 0; i < 6; ++i) {
            if (value.toLower() == QLatin1String(modes[i])) {
                colorMode_->setCurrentIndex(i);
                ok = true;
            }
        }
    } else if (name == QStringLiteral("colorsource")) {
        colorSource_->setEditText(value);
    } else if (name == QStringLiteral("colormin")) {
        colorMin_->setValue(value.toDouble(&ok));
    } else if (name == QStringLiteral("colormax")) {
        colorMax_->setValue(value.toDouble(&ok));
    } else if (name == QStringLiteral("guides")) {
        guides_->setChecked(flag());
    } else if (name == QStringLiteral("tubes")) {
        tubes_->setChecked(flag());
    } else if (name == QStringLiteral("widthcoverage")) {
        widthCoverage_->setChecked(flag());
    } else if (name == QStringLiteral("coveragedensity")) {
        coverageDensity_->setChecked(flag());
    } else if (name == QStringLiteral("clumplod")) {
        clumpLod_->setChecked(flag());
    } else if (name == QStringLiteral("clumploderror")) {
        settings_.clumpLodError = std::clamp(value.toFloat(&ok), 0.01f, 0.45f);
        redisplay();
    } else if (name == QStringLiteral("guidepoints")) {
        guidePoints_->setCurrentIndex(std::clamp(value.toInt(&ok), 0, 2));
    } else if (name == QStringLiteral("scalpmap")) {
        const int index = scalpMap_->findData(value);
        ok = index >= 0 || value.isEmpty();
        scalpMap_->setCurrentIndex(std::max(0, index));
    } else if (name == QStringLiteral("scalpmapchannel")) {
        ok = scalpMapChannel_->findText(value) >= 0;
        if (ok) scalpMapChannel_->setCurrentText(value);
    } else if (name == QStringLiteral("cardsview")) {
        cardsView_->setChecked(flag());
    } else if (name == QStringLiteral("lookdev")) {
        setLookdev(flag());
    } else if (name == QStringLiteral("scalp")) {
        setShowScalp(flag());
    } else {
        ok = false;
    }
    if (!ok) {
        report(tr("Cannot set the display's %1 to %2").arg(name, value));
        return false;
    }
    return true;
}

QImage MainWindow::grabUi(const QImage& scene) {
    // The window's own background and the docks' separators, the menu bar, the tool bar and the
    // docked docks, then the viewport's frame where the central widget is and the overlay's
    // pieces over it in stacking order (they compose with alpha as on screen).
    // The frame is in device pixels (a 150% screen: 1.5 per logical pixel); the widgets render
    // in logical ones, so the image carries the ratio and the painter scales them to match.
    const qreal ratio = devicePixelRatioF();
    QImage image(size() * ratio, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);
    image.fill(theme::backdrop());
    QPainter painter(&image);
    render(&painter, QPoint(), QRegion(), QWidget::DrawWindowBackground);
    for (QObject* child : children()) {
        auto* w = qobject_cast<QWidget*>(child);
        if (!w || w == host_ || w->isWindow() || !w->isVisible()) continue;
        w->render(&painter, w->pos(), QRegion(), QWidget::DrawWindowBackground | QWidget::DrawChildren);
    }
    const QPoint at = host_->pos();
    const QSize pixels = host_->size() * ratio;
    QImage frame = scene.isNull() ? viewport_->grabFramebuffer() : scene;
    if (frame.isNull()) {
        frame = QImage(pixels, QImage::Format_ARGB32_Premultiplied);
        frame.fill(theme::backdrop());
    }
    frame = frame.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (frame.size() != pixels) frame = frame.scaled(pixels, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    painter.drawImage(QRectF(QPointF(at), QSizeF(host_->size())), frame);
    for (QObject* child : host_->children()) {
        auto* w = qobject_cast<QWidget*>(child);
        if (!w || w == viewport_ || !w->isVisible()) continue;
        w->render(&painter, at + w->pos(), QRegion(), QWidget::DrawChildren);
    }
    // The menus a scripted screenshot laid out (setUiMenu), as they would pop up.
    for (const auto& [pixmap, where] : uiMenus_) painter.drawPixmap(where, pixmap);
    return image;
}

void MainWindow::captureUi(const std::function<void(const QImage&)>& done) {
    if (QGuiApplication::platformName() == QLatin1String("offscreen")) {
        QTimer::singleShot(500, this, [this, done] {
            const QImage scene = renderOffscreen(host_->size() * host_->devicePixelRatioF(), 1);
            done(grabUi(scene));
        });
        return;
    }
    auto* once = new QObject(this);
    connect(viewport_, &Viewport::frameDrawn, once, [this, once, done] {
        once->deleteLater();
        QTimer::singleShot(500, this, [this, done] { done(grabUi()); });
    });
}

bool MainWindow::setParamText(const QString& text) {
    if (!document_) {
        report(tr("No groom open for %1").arg(text));
        return false;
    }
    const int dot = text.indexOf(QLatin1Char('.')), equals = text.indexOf(QLatin1Char('='));
    if (dot <= 0 || equals <= dot + 1) {
        report(tr("Expected node.param=value, got %1").arg(text));
        return false;
    }
    const std::string node = text.left(dot).toStdString(), name = text.mid(dot + 1, equals - dot - 1).toStdString();
    const QString valueText = text.mid(equals + 1);
    nlohmann::json value;
    try {
        value = nlohmann::json::parse(valueText.toStdString());
    } catch (const nlohmann::json::exception&) {
        value = valueText.toStdString();  // a bare word: a string
    }
    const std::string error = document_->setParam(node, name, value);
    if (!error.empty()) {
        report(tr("Cannot set %1: %2").arg(text, QString::fromStdString(error)));
        return false;
    }
    graphEdited();
    return true;
}

bool MainWindow::setBypassText(const QString& node) {
    if (!document_) {
        report(tr("No groom open to bypass %1").arg(node));
        return false;
    }
    const std::string error = document_->setBypass(node.toStdString(), true);
    if (!error.empty()) {
        report(tr("Cannot bypass %1: %2").arg(node, QString::fromStdString(error)));
        return false;
    }
    graphEdited();
    return true;
}

bool MainWindow::loadGraphFile(const QString& path) {
    if (!document_) {
        report(tr("No groom open for the graph %1").arg(path));
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        report(tr("Cannot read the graph %1").arg(path));
        return false;
    }
    const std::string error = document_->replaceGraph(file.readAll().toStdString());
    if (!error.empty()) {
        report(tr("Cannot use the graph %1: %2").arg(path, QString::fromStdString(error)));
        return false;
    }
    params_->setDocument(document_.get());
    graphParams_->setDocument(document_.get());
    if (graphShown()) graph_->setDocument(document_.get());
    refreshNodes();
    reevaluate();
    refreshUndoActions();
    refreshGroups();
    return true;
}

bool MainWindow::saveTo(const QString& path) {
    if (!document_) {
        report(tr("No groom open to save as %1").arg(path));
        return false;
    }
    try {
        document_->save(path.toStdString());
    } catch (const std::exception& e) {
        report(tr("Cannot save %1: %2").arg(path, QString::fromUtf8(e.what())));
        return false;
    }
    dropAutosave();  // (the changes are in the file now)
    report(tr("Saved %1").arg(path), false);
    refreshTitle();
    return true;
}

QString MainWindow::cameraDescription() const {
    const OrbitCamera& c = viewport_->camera();
    const QVector3D eye = c.eye(), target = c.target();
    return QStringLiteral("camera eye %1 %2 %3 target %4 %5 %6 fov %7")
        .arg(eye.x()).arg(eye.y()).arg(eye.z()).arg(target.x()).arg(target.y()).arg(target.z()).arg(c.fovDegrees());
}

}  // namespace groomlab::app
