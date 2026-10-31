// SPDX-License-Identifier: Apache-2.0
// The Graph Editor as Maya's (hosts/maya/scripts/groomlab/ui/graph_editor.py): its bar's menus and
// the right-click menus built from graph_ui.json (app_graph_actions.cpp), the operations they run
// (Expose, Show Inner Param, Drive with Slider, Bake Node to Guides, backdrops), a node dropped on a
// wire and Tab over a wire, painted maps in the search, Alt+right-drag zoom, a readable framing,
// real units.

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

#include <QAction>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QImage>
#include <QScrollArea>
#include <QToolButton>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "app_actions.h"
#include "app_shell.h"
#include "groom_document.h"
#include "main_window.h"
#include "node_graph.h"
#include "overlay.h"
#include "param_panel.h"
#include "theme.h"

namespace groomlab::app {
namespace {

using nlohmann::json;

const std::string kGroom = std::string(GROOMLAB_TEST_DATA) + "/strokes/strokes.groom";

std::string nodeOf(const GroomDocument& doc, const std::string& type) {
    for (const GroomDocument::NodeInfo& n : doc.nodes()) {
        if (n.type == type) return n.name;
    }
    return {};
}

// The menu's entries as shown: texts without their keys, "-" for a separator.
std::vector<std::string> shown(const QMenu* menu) {
    std::vector<std::string> out;
    for (const QAction* a : menu->actions()) {
        if (!a->isVisible()) continue;
        out.push_back(a->isSeparator() ? "-" : a->text().section(QLatin1Char('\t'), 0, 0).toStdString());
    }
    return out;
}

bool has(const std::vector<std::string>& items, const std::string& item) { return std::find(items.begin(), items.end(), item) != items.end(); }

QAction* action(const QMenu* menu, const QString& text) {
    for (QAction* a : menu->actions()) {
        if (a->text().section(QLatin1Char('\t'), 0, 0) == text) return a;
    }
    return nullptr;
}

QMenu* submenu(const QMenu* menu, const QString& text) {
    QAction* a = action(menu, text);
    return a ? a->menu() : nullptr;
}

void mouse(QWidget* w, QEvent::Type type, const QPointF& at, Qt::MouseButton button, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(button);
    QMouseEvent event(type, at, w->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : button, held, mods);
    QCoreApplication::sendEvent(w, &event);
}

// A link of the document: the first whose source feeds strands into `to`'s type.
bool linkInto(const GroomDocument& doc, const std::string& toType, GroomDocument::ConnectionInfo* out) {
    const std::string to = nodeOf(doc, toType);
    for (const GroomDocument::ConnectionInfo& c : doc.connections()) {
        if (c.to == to) {
            *out = c;
            return true;
        }
    }
    return false;
}

struct Window {
    MainWindow window;
    Window() {
        window.resize(1600, 1000);
        window.show();
        EXPECT_TRUE(window.openGroom(QString::fromStdString(kGroom)));
        window.setGraphShown(true);
        QCoreApplication::processEvents();
    }
    GroomDocument* doc() { return window.document(); }
    NodeGraphView* graph() { return window.shell()->graph(); }
};

TEST(AppGraphEditor, TheBarsMenusComeFromTheDefinitions) {
    Window w;
    AppShell* shell = w.window.shell();
    // Add Node: the node categories (graph_ui.json's addMenu), then the library's entries.
    QMenu* add = shell->graphAddButton()->menu();
    ASSERT_NE(add, nullptr);
    emit add->aboutToShow();
    const std::vector<std::string> adds = shown(add);
    ASSERT_GT(adds.size(), 6u);
    EXPECT_TRUE(add->actions().front()->menu() != nullptr) << "a category submenu first";
    EXPECT_EQ(std::vector<std::string>(adds.end() - 3, adds.end()), (std::vector<std::string>{"New Script Node", "Import Node...", "Open Nodes Folder"}));
    // A category's node adds after the selected node (Maya's Add Node), wired into its chain.
    const std::string interpolate = nodeOf(*w.doc(), "Interpolate");
    w.graph()->selectNode(interpolate);
    QMenu* category = add->actions().front()->menu();
    ASSERT_FALSE(category->actions().isEmpty());
    const int before = static_cast<int>(w.doc()->nodes().size());
    category->actions().front()->trigger();
    EXPECT_EQ(static_cast<int>(w.doc()->nodes().size()), before + 1);
    // Edit (the three dots): Maya's editMenu with the Units and Theme submenus.
    QMenu* edit = shell->graphEditButton()->menu();
    ASSERT_NE(edit, nullptr);
    emit edit->aboutToShow();
    const std::vector<std::string> edits = shown(edit);
    for (const char* item : {"Frame Selection", "Frame All", "Tidy Layout", "Group into Compound", "Copy Graph", "Paste Graph", "Units", "Theme",
                             "Preview Drags on the GPU", "Reload from File"}) {
        EXPECT_TRUE(has(edits, item)) << item;
    }
    QMenu* units = submenu(edit, QStringLiteral("Units"));
    ASSERT_NE(units, nullptr);
    EXPECT_EQ(shown(units), (std::vector<std::string>{"Lengths", "Widths", "Density"}));
    // Presets: in the Graph Editor (Maya's), the categories, then Save as Preset, Copy and Paste Graph.
    QMenu* presets = shell->graphPresetsButton()->menu();
    ASSERT_NE(presets, nullptr);
    emit presets->aboutToShow();
    const std::vector<std::string> items = shown(presets);
    ASSERT_GE(items.size(), 5u);
    EXPECT_EQ(std::vector<std::string>(items.end() - 5, items.end()),
              (std::vector<std::string>{"-", "Save as Preset...", "-", "Copy Graph", "Paste Graph"}));
}

TEST(AppGraphEditor, TheNodeMenuFollowsTheNodesKind) {
    Window w;
    NodeGraphView* graph = w.graph();
    const std::string scatter = nodeOf(*w.doc(), "Scatter");
    ASSERT_FALSE(scatter.empty());
    graph->refresh();
    QMenu menu;
    EXPECT_EQ(graph->fillContextMenu(&menu, graph->nodeRect(scatter).center()), NodeGraphView::MenuTarget::Node);
    EXPECT_EQ(graph->selectedNode(), scatter) << "a right-click selects the node";
    const std::vector<std::string> items = shown(&menu);
    for (const char* item : {"Set as Output", "Bypass", "Evaluate", "Rename...", "Duplicate", "Bake Node to Guides...", "Expose Param as Input",
                             "Add Backdrop Around Selection", "Delete"}) {
        EXPECT_TRUE(has(items, item)) << item;
    }
    for (const char* item : {"Brush This Node's Guides", "Clear Sculpt Edits", "Edit Node...", "Show Inner Param", "Save as Node..."}) {
        EXPECT_FALSE(has(items, item)) << item << ": not this node's kind";
    }
    // Expose Param as Input: the param an input port (checked after).
    QMenu* expose = submenu(&menu, QStringLiteral("Expose Param as Input"));
    ASSERT_NE(expose, nullptr);
    emit expose->aboutToShow();
    ASSERT_FALSE(expose->actions().isEmpty());
    QAction* first = expose->actions().front();
    const std::string param = first->text().toStdString();
    EXPECT_FALSE(first->isChecked());
    first->trigger();
    EXPECT_TRUE(w.doc()->paramExposed(scatter, param)) << param;
    // A Sculpt node: Brush This Node's Guides (what brushes shape) and Clear Sculpt Edits.
    std::string sculpt;
    ASSERT_EQ(w.doc()->addNode("Sculpt", 0.0f, 600.0f, &sculpt), "");
    graph->refresh();
    QMenu sculptMenu;
    graph->fillContextMenu(&sculptMenu, graph->nodeRect(sculpt).center());
    QAction* brush = action(&sculptMenu, QStringLiteral("Brush This Node's Guides"));
    ASSERT_NE(brush, nullptr);
    EXPECT_NE(action(&sculptMenu, QStringLiteral("Clear Sculpt Edits")), nullptr);
    EXPECT_TRUE(brush->isCheckable());
    brush->trigger();
    EXPECT_EQ(w.doc()->sculptTarget(), sculpt);
}

TEST(AppGraphEditor, NodeStatusBadgesInTheTitleRow) {
    // The design's status badges: the cook time muted, in the warning color from 10 ms; the View eye;
    // a warning triangle for a note (an error keeps its red ring instead); a bypassed node dimmed,
    // its toggle lit and its wires dashed.
    Window w;
    NodeGraphView* graph = w.graph();
    GroomDocument* doc = w.doc();
    const std::string scatter = nodeOf(*doc, "Scatter");
    const std::string interpolate = nodeOf(*doc, "Interpolate");
    ASSERT_FALSE(scatter.empty());
    ASSERT_FALSE(interpolate.empty());
    EXPECT_EQ(NodeGraphView::timeText(0.01), QString()) << "nothing worth a badge";
    EXPECT_EQ(NodeGraphView::timeText(0.42), QStringLiteral("0.4 ms"));
    EXPECT_EQ(NodeGraphView::timeText(9.94), QStringLiteral("9.9 ms"));
    EXPECT_EQ(NodeGraphView::timeText(12.4), QStringLiteral("12 ms"));
    EXPECT_EQ(NodeGraphView::timeColor(9.9), theme::textMuted());
    EXPECT_EQ(NodeGraphView::timeColor(NodeGraphView::kSlowNodeMs), theme::graphWarning());
    graph->setNodeTimes({{scatter, 0.4}, {interpolate, 25.0}});
    EXPECT_EQ(graph->nodeBadges(scatter).time, QStringLiteral("0.4 ms"));
    EXPECT_EQ(graph->nodeBadges(scatter).timeColor, theme::textMuted());
    EXPECT_EQ(graph->nodeBadges(interpolate).time, QStringLiteral("25 ms"));
    EXPECT_EQ(graph->nodeBadges(interpolate).timeColor, theme::graphWarning()) << "slow: the warning color";
    // View Up to Here: an eye on that node alone.
    graph->setViewedNode(scatter);
    EXPECT_TRUE(graph->nodeBadges(scatter).viewed);
    EXPECT_FALSE(graph->nodeBadges(interpolate).viewed);
    graph->setViewedNode({});
    EXPECT_FALSE(graph->nodeBadges(scatter).viewed);
    // A note: the warning triangle; an error over it: the red ring alone.
    graph->setNodeNotes({{interpolate, "does nothing as set"}});
    EXPECT_TRUE(graph->nodeBadges(interpolate).warning);
    EXPECT_FALSE(graph->nodeBadges(scatter).warning);
    graph->setNodeErrors({{interpolate, "failed"}});
    EXPECT_TRUE(graph->nodeBadges(interpolate).failed);
    EXPECT_FALSE(graph->nodeBadges(interpolate).warning);
    graph->setNodeErrors({});
    // The wires into and out of a bypassed node dashed, the others solid.
    auto checkWires = [&](const std::string& node) {
        int touching = 0;
        for (int i = 0; i < graph->wireCount(); ++i) {
            std::string from, output, to, input;
            EXPECT_TRUE(graph->wireLink(i, &from, &output, &to, &input));
            const bool touches = from == node || to == node;
            if (touches) ++touching;
            EXPECT_EQ(graph->wireDashed(i), touches && graph->nodeBadges(node).bypassed) << from << " -> " << to;
        }
        return touching;
    };
    EXPECT_GT(checkWires(scatter), 0);
    // The header's bypass toggle still clicks where it was (the title row's right end).
    const QRectF card = graph->nodeRect(scatter);
    const qreal scale = graph->viewScale();
    const QPointF toggle(card.right() - 17.0 * scale, card.top() + 14.0 * scale);
    auto bypassed = [&] {
        for (const GroomDocument::NodeInfo& n : doc->nodes()) {
            if (n.name == scatter) return n.bypass;
        }
        return false;
    };
    ASSERT_FALSE(bypassed());
    mouse(graph, QEvent::MouseButtonPress, toggle, Qt::LeftButton);
    mouse(graph, QEvent::MouseButtonRelease, toggle, Qt::LeftButton);
    QCoreApplication::processEvents();
    ASSERT_TRUE(bypassed()) << "the bypass toggle";
    graph->refresh();
    EXPECT_TRUE(graph->nodeBadges(scatter).bypassed);
    EXPECT_GT(checkWires(scatter), 0);
    // Every badge at once paints.
    graph->setNodeNotes({{scatter, "note"}});
    graph->setViewedNode(scatter);
    QImage canvas(graph->size(), QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    graph->render(&canvas);
    EXPECT_EQ(qAlpha(canvas.pixel(2, 2)), 255);
    graph->setViewedNode({});
    EXPECT_EQ(doc->setBypass(scatter, false), "");
}

TEST(AppGraphEditor, ExpandInputsButtonTogglesEveryConnectableParam) {
    Window w;
    NodeGraphView* graph = w.graph();
    GroomDocument* doc = w.doc();
    const std::string scatter = nodeOf(*doc, "Scatter");
    ASSERT_FALSE(scatter.empty());
    std::vector<std::string> params;
    for (const json& p : doc->nodeSpec(scatter)->value("params", json::array())) {
        if (p.contains("port") && p["port"].is_object()) params.push_back(p.value("name", std::string()));
    }
    ASSERT_GT(params.size(), 1u);
    const QRectF card = graph->nodeRect(scatter);
    const qreal scale = graph->viewScale();
    const QPointF button(card.right() - 40.0 * scale, card.top() + 14.0 * scale);
    mouse(graph, QEvent::MouseButtonPress, button, Qt::LeftButton);
    for (const std::string& param : params) EXPECT_TRUE(doc->paramExposed(scatter, param)) << param;
    std::string field;
    ASSERT_EQ(doc->addNode("FieldNoise", 0.0f, 0.0f, &field), "");
    ASSERT_EQ(doc->connectNodes(field, "", scatter, "density"), "");
    graph->refresh();
    ASSERT_EQ(doc->disconnectInput(scatter, "density"), "");
    mouse(graph, QEvent::MouseButtonPress, button, Qt::LeftButton);
    for (const std::string& param : params) EXPECT_FALSE(doc->paramExposed(scatter, param)) << param;
    mouse(graph, QEvent::MouseButtonPress, button, Qt::LeftButton);
    for (const std::string& param : params) EXPECT_TRUE(doc->paramExposed(scatter, param)) << param;
    EXPECT_EQ(doc->connectNodes(field, "", scatter, "density"), "") << "re-expanding restores a reconnectable port";
}

TEST(AppGraphEditor, ShowInnerParamPutsAnInnerParamOnTheCompound) {
    Window w;
    GroomDocument* doc = w.doc();
    std::string compound;
    ASSERT_EQ(doc->collapseNodes({nodeOf(*doc, "Scatter")}, &compound), "");
    w.graph()->refresh();
    QMenu menu;
    w.graph()->fillContextMenu(&menu, w.graph()->nodeRect(compound).center());
    QMenu* inner = submenu(&menu, QStringLiteral("Show Inner Param"));
    ASSERT_NE(inner, nullptr) << "a compound's menu";
    emit inner->aboutToShow();
    ASSERT_FALSE(inner->actions().isEmpty());
    QMenu* node = inner->actions().front()->menu();
    ASSERT_NE(node, nullptr) << "a submenu per inner node";
    QAction* param = node->actions().front();
    param->trigger();
    const std::vector<std::string> shownParams = doc->shownInnerParams(compound);
    ASSERT_EQ(shownParams.size(), 1u);
    EXPECT_EQ(shownParams.front(), node->title().toStdString() + "." + param->text().toStdString());
    // Unchecked: no longer on the compound.
    EXPECT_EQ(doc->showInnerParam(compound, shownParams.front(), false), "");
    EXPECT_TRUE(doc->shownInnerParams(compound).empty());
}

TEST(AppGraphEditor, BackdropAndCanvasMenus) {
    Window w;
    NodeGraphView* graph = w.graph();
    graph->selectNode({});
    graph->addBackdrop(QPointF(40, 40));
    ASSERT_EQ(graph->backdropCount(), 1);
    if (graph->promptOpen()) graph->acceptPrompt();
    QMenu menu;
    EXPECT_EQ(graph->fillContextMenu(&menu, graph->backdropTitleBar(0).center()), NodeGraphView::MenuTarget::Backdrop);
    EXPECT_EQ(shown(&menu), (std::vector<std::string>{"Rename...", "Color", "Delete Backdrop"}));
    QMenu* color = submenu(&menu, QStringLiteral("Color"));
    ASSERT_NE(color, nullptr);
    emit color->aboutToShow();
    action(color, QStringLiteral("Green"))->trigger();
    EXPECT_EQ(graph->backdropColor(0), QColor(70, 120, 80));
    action(&menu, QStringLiteral("Delete Backdrop"))->trigger();
    EXPECT_EQ(graph->backdropCount(), 0);
    // The canvas: the search first, the Add Node menu's entries, then the layout's.
    const QPointF empty(graph->width() - 30.0, graph->height() - 30.0);
    QMenu canvas;
    ASSERT_EQ(graph->fillContextMenu(&canvas, empty), NodeGraphView::MenuTarget::Canvas);
    const std::vector<std::string> items = shown(&canvas);
    ASSERT_GT(items.size(), 8u);
    EXPECT_EQ(items.front(), "Search Nodes...");
    EXPECT_EQ(std::vector<std::string>(items.end() - 3, items.end()), (std::vector<std::string>{"Add Backdrop Around Selection", "Tidy Layout", "Frame All"}));
    // A category's node goes where the canvas was right-clicked, unwired.
    QMenu* category = nullptr;
    for (QAction* a : canvas.actions()) {
        if (a->menu() && !category) category = a->menu();
    }
    ASSERT_NE(category, nullptr);
    const std::size_t before = w.doc()->connections().size();
    category->actions().front()->trigger();
    const std::string made = graph->selectedNode();
    ASSERT_FALSE(made.empty());
    EXPECT_EQ(w.doc()->connections().size(), before) << "placed, not wired";
    float x = 0.0f, y = 0.0f;
    ASSERT_TRUE(w.doc()->nodePosition(made, &x, &y));
    EXPECT_NEAR(x, graph->sceneAt(empty).x(), 1.0);
}

TEST(AppGraphEditor, TheParamMenuExposesPaintsAndDrivesWithASlider) {
    Window w;
    GroomDocument* doc = w.doc();
    // The default graph's Length node: a mappable number of its own.
    const std::string clump = nodeOf(*doc, "Length");
    ASSERT_FALSE(clump.empty());
    std::string mappable;
    for (const json& p : doc->nodeSpec(clump)->value("params", json::array())) {
        if (mappable.empty() && p.value("mappable", false) && p.value("kind", std::string()) == "float") mappable = p.value("name", std::string());
    }
    ASSERT_FALSE(mappable.empty());
    QMenu menu;
    AppActions::fillParamMenu(w.window, &menu, clump, mappable);
    const std::vector<std::string> items = shown(&menu);
    EXPECT_TRUE(has(items, "Expose as Input"));
    EXPECT_TRUE(has(items, "Drive with Slider"));
    QMenu* paint = submenu(&menu, QStringLiteral("Paint"));
    ASSERT_NE(paint, nullptr);
    EXPECT_EQ(shown(paint), (std::vector<std::string>{"On Scalp Vertices", "As Texture"})) << "guide layers are not the app's for a setting yet";
    // Drive with Slider: a Control node feeding the param (exposed), its Default the slider.
    action(&menu, QStringLiteral("Drive with Slider"))->trigger();
    const std::string control = nodeOf(*doc, "Control");
    ASSERT_FALSE(control.empty());
    EXPECT_TRUE(doc->paramExposed(clump, mappable));
    EXPECT_EQ(doc->paramInputSource(clump, mappable), control);
    EXPECT_EQ(doc->param(control, "name"), json(mappable));
    // The param panel's rows ask the same menu.
    QMenu row;
    QCoreApplication::processEvents();
    ParamPanel panel;
    panel.setDocument(doc);
    panel.showNode(clump);
    panel.rebuildNow();
    panel.setParamMenuFiller([&](QMenu* m, const std::string& node, const std::string& param) { AppActions::fillParamMenu(w.window, m, node, param); });
    panel.fillParamMenu(&row, mappable);
    EXPECT_TRUE(has(shown(&row), "Drive with Slider"));
}

TEST(AppGraphEditor, ANodeDroppedOnAWireGoesIntoIt) {
    auto doc = GroomDocument::open(kGroom);
    ASSERT_TRUE(doc);
    ASSERT_NE(doc->spec("Noise"), nullptr);
    NodeGraphView view;
    view.resize(1400, 500);
    view.setDocument(doc.get());
    GroomDocument::ConnectionInfo link;
    ASSERT_TRUE(linkInto(*doc, "Output", &link));
    // A lone Noise node far below, dragged by its title onto the wire into the output.
    std::string noise;
    ASSERT_EQ(doc->addNode("Noise", 0.0f, 2000.0f, &noise), "");
    view.refresh();
    int wire = -1;
    for (int i = 0; i < view.wireCount(); ++i) {
        std::string from, output, to, input;
        view.wireLink(i, &from, &output, &to, &input);
        if (from == link.from && to == link.to) wire = i;
    }
    ASSERT_GE(wire, 0);
    const QPointF middle = view.wirePathOf(wire).pointAtPercent(0.5) * view.viewScale() + view.viewPan();
    const QRectF card = view.nodeRect(noise);
    const QPointF grab(card.center().x(), card.top() + 8.0);
    mouse(&view, QEvent::MouseButtonPress, grab, Qt::LeftButton);
    mouse(&view, QEvent::MouseMove, middle + (grab - card.center()), Qt::LeftButton);
    EXPECT_EQ(view.dropWire(), wire) << "the wire under the node lights up";
    mouse(&view, QEvent::MouseButtonRelease, middle + (grab - card.center()), Qt::LeftButton);
    bool in = false, out = false;
    for (const GroomDocument::ConnectionInfo& c : doc->connections()) {
        in = in || (c.from == link.from && c.to == noise);
        out = out || (c.from == noise && c.to == link.to && c.input == link.input);
    }
    EXPECT_TRUE(in && out) << "inserted between " << link.from << " and " << link.to;
    // Undo takes the insertion back as one step.
    ASSERT_EQ(doc->undo(), "");
    bool back = false;
    for (const GroomDocument::ConnectionInfo& c : doc->connections()) back = back || (c.from == link.from && c.to == link.to);
    EXPECT_TRUE(back);
}

TEST(AppGraphEditor, TabOverAWireAddsIntoItAndTheSearchListsPaintedMaps) {
    auto doc = GroomDocument::open(kGroom);
    ASSERT_TRUE(doc);
    NodeGraphView view;
    view.resize(1400, 500);
    view.setDocument(doc.get());
    GroomDocument::ConnectionInfo link;
    ASSERT_TRUE(linkInto(*doc, "Output", &link));
    int wire = -1;
    for (int i = 0; i < view.wireCount(); ++i) {
        std::string from, output, to, input;
        view.wireLink(i, &from, &output, &to, &input);
        if (from == link.from && to == link.to) wire = i;
    }
    ASSERT_GE(wire, 0);
    const QPointF middle = view.wirePathOf(wire).pointAtPercent(0.5) * view.viewScale() + view.viewPan();
    view.openSearchAt(middle);
    EXPECT_EQ(view.searchWire(), wire);
    view.setSearchText(QStringLiteral("noise"));
    ASSERT_FALSE(view.searchTypes().empty());
    EXPECT_EQ(view.searchTypes().front(), "Noise");
    view.acceptSearch();
    const std::string made = view.selectedNode();
    bool in = false, out = false;
    for (const GroomDocument::ConnectionInfo& c : doc->connections()) {
        in = in || (c.from == link.from && c.to == made);
        out = out || (c.from == made && c.to == link.to);
    }
    EXPECT_TRUE(in && out) << made << " into the wire Tab was pressed over";
    // A painted map: a Map node reading it, named after it.
    ASSERT_EQ(doc->createMap("frizzMask", "vertices", false, 1.0f), "");
    view.openSearchAt(QPointF(20, 20));
    view.setSearchText(QStringLiteral("frizzmask"));
    ASSERT_FALSE(view.searchTypes().empty());
    EXPECT_EQ(view.searchTypes().front(), "map:frizzMask");
    EXPECT_TRUE(view.searchRows().front().startsWith(QStringLiteral("Map: frizzMask")));
    view.acceptSearch();
    const std::string map = view.selectedNode();
    EXPECT_EQ(map, "frizzMask");
    EXPECT_EQ(doc->param(map, "name"), json("frizzMask"));
}

TEST(AppGraphEditor, AltRightDragZoomsAndOpensNoMenu) {
    auto doc = GroomDocument::open(kGroom);
    ASSERT_TRUE(doc);
    NodeGraphView view;
    view.resize(1200, 400);
    view.setDocument(doc.get());
    const qreal scale = view.viewScale();
    const QPointF at(600, 200);
    const QPointF scene = view.sceneAt(at);
    mouse(&view, QEvent::MouseButtonPress, at, Qt::RightButton, Qt::AltModifier);
    mouse(&view, QEvent::MouseMove, at + QPointF(70, 70), Qt::RightButton, Qt::AltModifier);
    EXPECT_NEAR(view.viewScale(), std::min(3.0, scale * std::exp(140 * 0.005)), 1e-6) << "right and down zoom in";
    EXPECT_NEAR(view.sceneAt(at).x(), scene.x(), 1e-6) << "the point pressed stays put";
    mouse(&view, QEvent::MouseMove, at - QPointF(70, 70), Qt::RightButton, Qt::AltModifier);
    EXPECT_LT(view.viewScale(), scale) << "left and up zoom out";
    mouse(&view, QEvent::MouseButtonRelease, at - QPointF(70, 70), Qt::RightButton, Qt::AltModifier);
    int asked = 0;
    view.setMenuFiller([&](QMenu*, NodeGraphView::MenuTarget, const QPointF&) { ++asked; });
    QContextMenuEvent menu(QContextMenuEvent::Mouse, (at - QPointF(70, 70)).toPoint());
    QCoreApplication::sendEvent(&view, &menu);
    EXPECT_EQ(asked, 0) << "the release that ended the zoom opens no menu";
}

TEST(AppGraphEditor, TheGraphOpensAtAReadableZoomOnItsLookEnd) {
    auto doc = GroomDocument::open(kGroom);
    ASSERT_TRUE(doc);
    NodeGraphView view;
    view.setFitMargins(QMargins(24, 24, 24, 24));
    view.resize(900, 260);  // a short bottom dock
    view.setDocument(doc.get());
    EXPECT_GE(view.viewScale(), NodeGraphView::kReadableScale) << "names readable in a short dock";
    const QRectF output = view.nodeRect(nodeOf(*doc, "Output"));
    EXPECT_TRUE(QRectF(view.rect()).contains(output)) << "the output end in view";
    view.fitAll();
    EXPECT_GE(view.viewScale(), NodeGraphView::kReadableScale) << "A: as readable";
}

TEST(AppGraphEditor, BakeNodeToGuidesAsksTheGroupInTheViewport) {
    Window w;
    GroomDocument* doc = w.doc();
    const std::string scatter = nodeOf(*doc, "Scatter");
    w.graph()->selectNode(scatter);
    QMenu menu;
    w.graph()->fillContextMenu(&menu, w.graph()->nodeRect(scatter).center());
    QAction* bake = action(&menu, QStringLiteral("Bake Node to Guides..."));
    ASSERT_NE(bake, nullptr);
    bake->trigger();
    OverlayHost* host = w.window.overlay();
    ASSERT_TRUE(host->promptShown()) << "a prompt over the viewport, never a dialog";
    EXPECT_EQ(host->promptLine(), QStringLiteral("baked"));
    host->setPromptLine(QStringLiteral("fromScatter"));
    ASSERT_TRUE(host->choosePrompt(QStringLiteral("Bake")));
    const HierarchyPtr h = doc->session().authoredHierarchy();
    ASSERT_TRUE(h);
    const GroupId group = h->findGroup("fromScatter");
    ASSERT_NE(group, kNoGroup);
    EXPECT_EQ(h->groupDepth(group), 0);
    EXPECT_EQ(doc->undoText(), "Undo bake " + scatter);
    std::size_t count = 0;
    EXPECT_NE(doc->bakeNodeToGuides(scatter, "", 1.0f, &count), "") << "a group needs a name";
}

TEST(AppGraphEditor, ANodesActionsSitOverItsParams) {
    // graph_ui.json's nodeActions by the node's type, under its title and over the rows' scroll area
    // (in view however far they scroll), as Maya's and Blender's node panels pin them.
    Window w;
    GroomDocument* doc = w.doc();
    std::string sim;
    ASSERT_EQ(doc->addNode("Simulate", 0.0f, 600.0f, &sim), "");
    w.graph()->refresh();
    w.graph()->selectNode(sim);
    QCoreApplication::processEvents();
    ParamPanel* panel = nullptr;
    for (ParamPanel* p : w.window.findChildren<ParamPanel*>()) {
        if (p->node() == sim) panel = p;
    }
    ASSERT_NE(panel, nullptr);
    EXPECT_EQ(panel->actionIds(), (std::vector<std::string>{"sim.range", "sim.bake", "sim.clear"}));
    EXPECT_FALSE(panel->actionsBar()->isHidden());
    QPushButton* clear = panel->actionButton("sim.clear");
    ASSERT_NE(clear, nullptr);
    EXPECT_EQ(clear->text(), QStringLiteral("Clear Simulation"));
    auto* scroll = panel->findChild<QScrollArea*>();
    ASSERT_NE(scroll, nullptr);
    EXPECT_FALSE(scroll->isAncestorOf(clear)) << "over the scroll area, not in it";
    clear->click();
    QCoreApplication::processEvents();
    // A Scatter has none: the bar goes.
    const std::string scatter = nodeOf(*doc, "Scatter");
    w.graph()->selectNode(scatter);
    QCoreApplication::processEvents();
    ASSERT_EQ(panel->node(), scatter);
    EXPECT_TRUE(panel->actionIds().empty());
    EXPECT_TRUE(panel->actionsBar()->isHidden());
}

TEST(AppGraphEditor, UnitsShowRealMeasures) {
    const std::string before = unitChoice("width");
    setUnitChoice("width", "um");
    const DisplayUnit um = displayUnit("width", false, 4);
    EXPECT_DOUBLE_EQ(um.factor, 1e4);
    EXPECT_EQ(um.suffix, QStringLiteral(" µm"));
    EXPECT_EQ(um.decimals, 0);
    setUnitChoice("width", "scene");
    EXPECT_DOUBLE_EQ(displayUnit("width", false, 4).factor, 1.0) << "scene units: bare";
    EXPECT_TRUE(displayUnit("width", false, 4).suffix.isEmpty());
    setUnitChoice("length", "mm");
    EXPECT_EQ(displayUnit("perLength", false, 3).suffix, QStringLiteral(" /mm"));
    setUnitChoice("length", "scene");
    setUnitChoice("width", before);
}

}  // namespace
}  // namespace groomlab::app
