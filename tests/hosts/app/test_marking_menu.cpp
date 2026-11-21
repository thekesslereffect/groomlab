// SPDX-License-Identifier: Apache-2.0
// The radial menu over the viewport (marking_menu.h, AppActions::radialMenuItems): its rings from
// engine/ui/marking_menu.json's radialMenu (Masking, Visibility, Brushes A and B, the picked
// brush's options), its layout (no pill over another, inside the viewport), submenus opening on a
// drag and on a click, back to the ring before, release-to-pick and click-to-pick, and Y + click in
// the window's viewport picking a brush, an option and an action. The offscreen platform; no dialog.

#include <set>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QDockWidget>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QShortcut>
#include <QDialog>
#include <QKeySequenceEdit>
#include <QSpinBox>
#include <QWheelEvent>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "app_actions.h"
#include "app_shell.h"
#include "brush_session.h"
#include "definitions.h"
#include "groomlab/brush.h"
#include "groomlab/brush_catalog.h"
#include "main_window.h"
#include "marking_menu.h"
#include "overlay.h"
#include "viewport.h"

namespace groomlab::app {
namespace {

using nlohmann::json;
using Item = MarkingMenu::Item;
using Kind = MarkingMenu::Item::Kind;

const std::string kSampleGroom = std::string(GROOMLAB_TEST_DATA) + "/strokes/strokes.groom";

void mouse(QWidget* w, QEvent::Type type, const QPointF& at, Qt::MouseButton button = Qt::LeftButton) {
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(button);
    QMouseEvent event(type, at, w->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : button, held, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &event);
}

void key(QWidget* w, QEvent::Type type, int k) {
    QKeyEvent event(type, k, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &event);
}

Item tool(const std::string& id, const char* position) {
    Item item;
    item.kind = Kind::Tool;
    item.id = id;
    item.label = QString::fromStdString(brushEntry(id)->label);
    item.position = MarkingMenu::positionIndex(position);
    return item;
}

Item submenu(const char* label, const char* position, std::vector<Item> items) {
    Item item;
    item.kind = Kind::Submenu;
    item.id = label;
    item.label = QString::fromLatin1(label);
    item.position = MarkingMenu::positionIndex(position);
    item.items = std::move(items);
    return item;
}

// Two rings by hand (no window): Brushes A (NW, three brushes) and Masking (S), and Cut at E.
std::vector<Item> handItems() {
    Item full;
    full.kind = Kind::Action;
    full.id = "mask.full";
    full.label = QStringLiteral("Full");
    full.position = MarkingMenu::positionIndex("W");
    return {submenu("Brushes A", "NW", {tool("draw", "W"), tool("comb", "NW"), tool("slide", "N")}),
            submenu("Masking", "S", {full, tool("select", "SW"), tool("freeze", "SE")}), tool("cut", "E")};
}

void expectLaidOut(const MarkingMenu& menu, const QRect& host) {
    for (std::size_t i = 0; i < menu.ring().size(); ++i) {
        const QRectF a = menu.itemRect(static_cast<int>(i));
        ASSERT_TRUE(a.isValid()) << menu.ring()[i].id;
        EXPECT_TRUE(QRectF(host).contains(a)) << menu.ring()[i].id << " outside the viewport";
        EXPECT_GT(QLineF(a.center(), menu.center()).length(), MarkingMenu::kDeadZone) << menu.ring()[i].id;
        for (std::size_t j = i + 1; j < menu.ring().size(); ++j) {
            EXPECT_FALSE(a.intersects(menu.itemRect(static_cast<int>(j)))) << menu.ring()[i].id << " over " << menu.ring()[j].id;
        }
    }
}

const Item* find(const std::vector<Item>& items, const std::string& id) {
    for (const Item& item : items) {
        if (item.id == id || item.label.toStdString() == id) return &item;
    }
    return nullptr;
}

TEST(AppMarkingMenu, ItemsFollowTheDefinitions) {
    MainWindow window;
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    const std::vector<Item> items = AppActions::radialMenuItems(window);
    const ui::Definitions& d = ui::Definitions::shipped();
    const json& root = d.get("radialMenu/items");
    ASSERT_EQ(items.size(), root.size());
    std::set<int> positions;
    for (std::size_t i = 0; i < root.size(); ++i) {
        EXPECT_EQ(items[i].kind, Kind::Submenu);
        EXPECT_EQ(items[i].label.toStdString(), root[i].at("label").get<std::string>());
        EXPECT_EQ(items[i].position, MarkingMenu::positionIndex(root[i].at("position").get<std::string>()));
        positions.insert(items[i].position);
    }
    EXPECT_EQ(positions.size(), root.size()) << "a compass point each";
    // Masking down, Visibility left, Brush Options right (as asked), the brushes up.
    const Item* masking = find(items, "Masking");
    const Item* visibility = find(items, "Visibility");
    const Item* options = find(items, "Brush Options");
    ASSERT_TRUE(masking && visibility && options);
    EXPECT_EQ(masking->position, MarkingMenu::positionIndex("S"));
    EXPECT_EQ(visibility->position, MarkingMenu::positionIndex("W"));
    EXPECT_EQ(options->position, MarkingMenu::positionIndex("E"));
    // Masking: Full, Empty, Select (the tool), Quick Select, Mask Brush (the freeze tool).
    ASSERT_TRUE(find(masking->items, "mask.full") && find(masking->items, "mask.empty") && find(masking->items, "mask.quickSelect"));
    ASSERT_TRUE(find(masking->items, "select") && find(masking->items, "freeze"));
    EXPECT_EQ(find(masking->items, "freeze")->label, QStringLiteral("Mask Brush"));
    // Visibility: Select, Show All, Invert, the hair and the guides (labelled by what they will do:
    // both show now).
    for (const char* id : {"visibility.select", "visibility.showAll", "visibility.invert"}) EXPECT_TRUE(find(visibility->items, id)) << id;
    const Item* hair = find(visibility->items, "display.toggleHair");
    const Item* guides = find(visibility->items, "display.toggleGuides");
    ASSERT_TRUE(hair && guides);
    EXPECT_FALSE(hair->checkable);
    EXPECT_EQ(hair->label, QStringLiteral("Hide Hair"));
    EXPECT_EQ(guides->label, QStringLiteral("Hide Guides"));
    // The brushes, each with its icon and category color.
    std::vector<std::string> a, b;
    for (const Item& item : find(items, "Brushes A")->items) a.push_back(item.id);
    for (const Item& item : find(items, "Brushes B")->items) b.push_back(item.id);
    EXPECT_EQ(std::set<std::string>(a.begin(), a.end()), (std::set<std::string>{"draw", "comb", "slide", "smooth", "shape", "flow", "place"}));
    EXPECT_EQ(std::set<std::string>(b.begin(), b.end()), (std::set<std::string>{"lift", "twist", "attract", "part", "cut", "erase"}));
    for (const Item& item : find(items, "Brushes A")->items) {
        EXPECT_FALSE(item.icon.isEmpty()) << item.id;
        EXPECT_TRUE(item.color.isValid()) << item.id;
    }
    // Every action is one the app binds.
    const ui::Bindings bound = AppActions::bindings(window);
    for (const Item* ring : {masking, visibility}) {
        for (const Item& item : ring->items) {
            if (item.kind == Kind::Action) EXPECT_TRUE(bound.count(item.id)) << item.id;
        }
    }
}

TEST(AppMarkingMenu, BrushOptionsFollowThePickedBrush) {
    MainWindow window;
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    auto options = [&] { return *find(AppActions::radialMenuItems(window), "Brush Options"); };
    // Comb: Stretch, a switch with its state.
    window.brush().setTool("comb");
    window.brush().settings().brush.stretch = false;
    Item comb = options();
    EXPECT_TRUE(comb.enabled);
    ASSERT_EQ(comb.items.size(), 1u);
    EXPECT_EQ(comb.items[0].kind, Kind::Option);
    EXPECT_EQ(comb.items[0].id, "stretch");
    EXPECT_TRUE(comb.items[0].checkable);
    EXPECT_FALSE(comb.items[0].checked);
    AppActions::runMarkingMenuItem(window, comb.items[0]);
    EXPECT_TRUE(window.brush().settings().brush.stretch) << "picked: flipped";
    EXPECT_TRUE(options().items[0].checked);
    // Smooth: All, Length, Shape, Curve, the one on checked.
    window.brush().setTool("smooth");
    Item smooth = options();
    std::vector<std::string> values;
    for (const Item& item : smooth.items) values.push_back(item.value);
    EXPECT_EQ(values, (std::vector<std::string>{"all", "length", "shape", "curve"}));
    EXPECT_TRUE(smooth.items[3].checked) << "Curve, as Smooth always was";
    AppActions::runMarkingMenuItem(window, smooth.items[1]);
    EXPECT_EQ(window.brush().settings().brush.smoothMode, SmoothMode::Length);
    // Tube: Draw Root, Edit Root, Width, Flatness, Twist, Always Visible.
    window.brush().setTool("shape");
    Item tube = options();
    std::vector<std::string> ids;
    for (const Item& item : tube.items) ids.push_back(item.value.empty() ? item.id : item.id + "=" + item.value);
    EXPECT_EQ(ids, (std::vector<std::string>{"tube.drawRoot", "tube.editRoot", "tubeChannel=width", "tubeChannel=flatness",
                                             "tubeChannel=twist", "display.tubesAlways"}));
    AppActions::runMarkingMenuItem(window, tube.items[3]);
    EXPECT_EQ(window.brush().settings().brush.tubeChannel, TubeChannel::Flatness);
    // A brush without options: the submenu there, disabled, saying so.
    window.brush().setTool("lift");
    Item lift = options();
    EXPECT_FALSE(lift.enabled);
    EXPECT_TRUE(lift.items.empty());
    EXPECT_TRUE(lift.tip.contains(QStringLiteral("Lift")));
}

TEST(AppMarkingMenu, LayoutKeepsItemsApartAndInside) {
    QWidget host;
    host.resize(1280, 900);
    MarkingMenu menu(&host);
    menu.setItems(handItems());
    host.show();
    const QPointF middle(640.0, 300.0);
    menu.popup(middle, false);
    ASSERT_TRUE(menu.shown());
    EXPECT_EQ(menu.geometry(), host.rect()) << "as big as the host";
    EXPECT_EQ(menu.depth(), 0);
    expectLaidOut(menu, host.rect());
    // Each item on its side of the middle.
    EXPECT_LT(menu.itemRect(menu.indexOf("Brushes A")).bottom(), middle.y());
    EXPECT_LT(menu.itemRect(menu.indexOf("Brushes A")).right(), middle.x());
    EXPECT_GT(menu.itemRect(menu.indexOf("Masking")).top(), middle.y());
    EXPECT_GT(menu.itemRect(menu.indexOf("cut")).left(), middle.x() + MarkingMenu::kDeadZone);
    // A submenu's ring, around its pill, laid out the same way.
    menu.open(menu.indexOf("Brushes A"));
    EXPECT_EQ(menu.depth(), 1);
    EXPECT_EQ(menu.ring().size(), 3u);
    expectLaidOut(menu, host.rect());
    // In a corner the middle moves in so every pill shows.
    menu.popup(QPointF(1270.0, 890.0), false);
    expectLaidOut(menu, host.rect());
    EXPECT_LT(menu.center().x(), 1270.0);
    EXPECT_LT(menu.center().y(), 890.0);
}

TEST(AppMarkingMenu, DragIntoASubmenuAndReleaseToPick) {
    QWidget host;
    host.resize(1280, 800);
    MarkingMenu menu(&host);
    menu.setItems(handItems());
    host.show();
    std::vector<std::string> picked;
    QObject::connect(&menu, &MarkingMenu::picked, [&picked](const Item& item) { picked.push_back(item.id); });
    const QPointF c(640.0, 320.0);
    // Press, drag right and let go: Cut (E), by its direction alone.
    menu.popup(c, true);
    EXPECT_TRUE(menu.held());
    menu.track(c + QPointF(40.0, 4.0));
    EXPECT_EQ(menu.hovered(), menu.indexOf("cut"));
    menu.release(c + QPointF(40.0, 4.0));
    EXPECT_EQ(picked, std::vector<std::string>{"cut"});
    EXPECT_FALSE(menu.shown());
    // Press, drag up-left onto Brushes A: its ring opens around the cursor; on up-left again: Comb.
    menu.popup(c, true);
    const QPointF pill = menu.itemRect(menu.indexOf("Brushes A")).center();
    menu.track(pill);
    ASSERT_EQ(menu.depth(), 1);
    EXPECT_EQ(menu.center(), pill);
    EXPECT_EQ(menu.hovered(), -1) << "the cursor in the new ring's middle";
    menu.track(pill + QPointF(-40.0, -40.0));
    EXPECT_EQ(menu.hovered(), menu.indexOf("comb"));
    menu.release(pill + QPointF(-40.0, -40.0));
    EXPECT_EQ(picked.back(), "comb");
    EXPECT_FALSE(menu.shown());
    // Into a submenu and let go there over nothing: it stays open for a click.
    menu.popup(c, true);
    menu.track(menu.itemRect(menu.indexOf("Masking")).center());
    ASSERT_EQ(menu.depth(), 1);
    menu.release(menu.center());
    EXPECT_TRUE(menu.shown());
    EXPECT_FALSE(menu.held());
    const QPointF brush = menu.itemRect(menu.indexOf("freeze")).center();
    mouse(&menu, QEvent::MouseMove, brush);
    EXPECT_EQ(menu.hovered(), menu.indexOf("freeze"));
    mouse(&menu, QEvent::MouseButtonPress, brush);
    EXPECT_EQ(picked.back(), "freeze");
    // Away and back to the middle, then let go: nothing, closed.
    menu.popup(c, true);
    menu.track(c + QPointF(60.0, 0.0));
    menu.track(c);
    menu.release(c);
    EXPECT_EQ(picked.size(), 3u);
    EXPECT_FALSE(menu.shown());
}

TEST(AppMarkingMenu, ClicksOpenGoBackAndClose) {
    QWidget host;
    host.resize(1280, 800);
    MarkingMenu menu(&host);
    menu.setItems(handItems());
    host.show();
    std::vector<std::string> picked;
    QObject::connect(&menu, &MarkingMenu::picked, [&picked](const Item& item) { picked.push_back(item.id); });
    const QPointF c(640.0, 320.0);
    // A click (no drag): open for clicks; a click on Masking opens it, on Full picks the action.
    menu.popup(c, true);
    menu.release(c + QPointF(2.0, 1.0));
    ASSERT_TRUE(menu.shown());
    mouse(&menu, QEvent::MouseButtonPress, menu.itemRect(menu.indexOf("Masking")).center());
    ASSERT_EQ(menu.depth(), 1);
    // Back to the root: Esc, a right click, a click in the middle; at the root they close it.
    key(&menu, QEvent::KeyPress, Qt::Key_Escape);
    EXPECT_EQ(menu.depth(), 0);
    EXPECT_TRUE(menu.shown());
    menu.open(menu.indexOf("Masking"));
    mouse(&menu, QEvent::MouseButtonPress, menu.center(), Qt::RightButton);
    EXPECT_EQ(menu.depth(), 0);
    menu.open(menu.indexOf("Masking"));
    mouse(&menu, QEvent::MouseButtonPress, menu.center());
    EXPECT_EQ(menu.depth(), 0);
    EXPECT_TRUE(menu.shown());
    // Moving back into the root's middle goes back too.
    menu.open(menu.indexOf("Masking"));
    mouse(&menu, QEvent::MouseMove, c);
    EXPECT_EQ(menu.depth(), 0);
    menu.open(menu.indexOf("Masking"));
    mouse(&menu, QEvent::MouseButtonPress, menu.itemRect(menu.indexOf("mask.full")).center());
    EXPECT_EQ(picked, std::vector<std::string>{"mask.full"});
    EXPECT_FALSE(menu.shown());
    // Esc at the root, and a click away from everything, close it.
    menu.popup(c, false);
    key(&menu, QEvent::KeyPress, Qt::Key_Escape);
    EXPECT_FALSE(menu.shown());
    menu.popup(c, false);
    mouse(&menu, QEvent::MouseButtonPress, c + QPointF(300.0, 300.0));
    EXPECT_FALSE(menu.shown());
    // A disabled item is not picked.
    std::vector<Item> items = handItems();
    items.back().enabled = false;  // Cut
    menu.setItems(items);
    menu.popup(c, false);
    mouse(&menu, QEvent::MouseButtonPress, menu.itemRect(menu.indexOf("cut")).center());
    EXPECT_EQ(picked.size(), 1u);
}

TEST(AppMarkingMenu, AltSInTheViewport) {
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QCoreApplication::processEvents();
    Viewport* view = window.overlay()->viewport();
    window.setRadialMenuShortcut(QKeySequence(Qt::ALT | Qt::Key_S));  // (the settings may hold another)
    auto* altS = window.findChild<QShortcut*>(QStringLiteral("radialMenuShortcut"));
    ASSERT_TRUE(altS);
    EXPECT_EQ(altS->key(), QKeySequence(Qt::ALT | Qt::Key_S));
    // No groom: no menu (and a message).
    emit altS->activated();
    EXPECT_FALSE(window.markingMenu()->shown());
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    ASSERT_EQ(view->toolMode(), Viewport::ToolMode::Camera);
    // Alt+S: the menu at once, no click; move onto Brushes A (its ring opens), up-left onto Comb and
    // let S go: the brush on.
    emit altS->activated();
    MarkingMenu* menu = window.markingMenu();
    ASSERT_TRUE(menu->shown());
    EXPECT_FALSE(menu->held());
    const QPointF pill = menu->itemRect(menu->indexOf("Brushes A")).center();
    mouse(menu, QEvent::MouseMove, pill);
    ASSERT_EQ(menu->depth(), 1);
    mouse(menu, QEvent::MouseMove, pill + QPointF(-40.0, -40.0));
    key(menu, QEvent::KeyRelease, Qt::Key_S);
    EXPECT_FALSE(menu->shown());
    EXPECT_EQ(view->toolMode(), Viewport::ToolMode::Brush);
    EXPECT_EQ(window.brush().toolId(), "comb");
    // S let go without moving: the menu stays for clicks; Alt+S again closes it.
    emit altS->activated();
    key(menu, QEvent::KeyRelease, Qt::Key_S);
    EXPECT_TRUE(menu->shown());
    emit altS->activated();
    EXPECT_FALSE(menu->shown());
    // Masking > Select: the Select tool, freezing (never toggled back to the camera).
    for (int round = 0; round < 2; ++round) {
        ASSERT_TRUE(window.showMarkingMenu(window.overlay()->rect().center(), false));
        menu->open(menu->indexOf("Masking"));
        mouse(menu, QEvent::MouseButtonPress, menu->itemRect(menu->indexOf("select")).center());
        EXPECT_EQ(view->toolMode(), Viewport::ToolMode::Select) << round;
        EXPECT_FALSE(window.brush().selectHides);
    }
    // Masking > Quick Select from the Comb brush: one pick, then the Comb brush again.
    view->setToolMode(Viewport::ToolMode::Brush);
    ASSERT_EQ(window.brush().toolId(), "comb");
    ASSERT_TRUE(window.showMarkingMenu(window.overlay()->rect().center(), false));
    menu->open(menu->indexOf("Masking"));
    mouse(menu, QEvent::MouseButtonPress, menu->itemRect(menu->indexOf("mask.quickSelect")).center());
    EXPECT_EQ(view->toolMode(), Viewport::ToolMode::Select);
    emit view->selectionChanged();
    EXPECT_EQ(view->toolMode(), Viewport::ToolMode::Brush);
    EXPECT_EQ(window.brush().toolId(), "comb");
    emit view->selectionChanged();  // (a later pick: nothing to go back to)
    EXPECT_EQ(view->toolMode(), Viewport::ToolMode::Brush);
    // Visibility > Select: the Select tool, hiding; another tool ends it.
    ASSERT_TRUE(window.showMarkingMenu(window.overlay()->rect().center(), false));
    menu->open(menu->indexOf("Visibility"));
    mouse(menu, QEvent::MouseButtonPress, menu->itemRect(menu->indexOf("visibility.select")).center());
    EXPECT_EQ(view->toolMode(), Viewport::ToolMode::Select);
    EXPECT_TRUE(window.brush().selectHides);
    view->setToolMode(Viewport::ToolMode::Camera);
    EXPECT_FALSE(window.brush().selectHides);
    // Visibility > Hide Hair: the hair off, the pill says Show Hair the next time.
    ASSERT_TRUE(window.hairVisible());
    ASSERT_TRUE(window.showMarkingMenu(window.overlay()->rect().center(), false));
    menu->open(menu->indexOf("Visibility"));
    mouse(menu, QEvent::MouseButtonPress, menu->itemRect(menu->indexOf("display.toggleHair")).center());
    EXPECT_FALSE(window.hairVisible());
    ASSERT_TRUE(window.showMarkingMenu(window.overlay()->rect().center(), false));
    menu->open(menu->indexOf("Visibility"));
    EXPECT_EQ(menu->ring()[static_cast<std::size_t>(menu->indexOf("display.toggleHair"))].label, QStringLiteral("Show Hair"));
    menu->dismiss();
}

TEST(AppMarkingMenu, MaskingAndVisibilityActions) {
    MainWindow window;
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    const ui::Bindings bound = AppActions::bindings(window);
    auto run = [&](const char* id) { bound.at(id).trigger(nullptr, false); };
    auto hierarchy = [&] { return window.document()->session().authoredHierarchy(); };
    auto count = [&](auto test) {
        std::size_t n = 0;
        for (const HierarchyGuide& g : hierarchy()->guides()) n += test(g) ? 1 : 0;
        return n;
    };
    auto frozen = [&](const HierarchyGuide& g) {
        float v = 0.0f;
        return hierarchy()->effectiveAttribute(g, "freeze", &v) && v >= 0.5f;
    };
    auto hidden = [&](const HierarchyGuide& g) { return guideHidden(*hierarchy(), g); };
    const std::size_t guides = hierarchy()->guides().size();
    ASSERT_GE(guides, 2u);
    // Empty: every guide frozen; Full: none.
    run("mask.empty");
    EXPECT_EQ(count(frozen), guides);
    run("mask.full");
    EXPECT_EQ(count(frozen), 0u);
    // One guide hidden (as Visibility > Select's lasso leaves it out), Invert, Show All.
    const GuideId first = hierarchy()->guides().front().id;
    window.document()->editGuides("hide", [first](GuideHierarchy& h) { h.setAttribute(first, kHiddenLayer, 1.0f); });
    // (its children hide with it: the layer is inherited)
    const std::size_t hiddenNow = count(hidden);
    EXPECT_GE(hiddenNow, 1u);
    EXPECT_LT(hiddenNow, guides);
    run("visibility.invert");
    EXPECT_EQ(count(hidden), guides - hiddenNow) << "what showed hides, what was hidden (children too) shows";
    EXPECT_FALSE(hidden(*hierarchy()->guide(first)));
    run("visibility.showAll");
    EXPECT_EQ(count(hidden), 0u);
    EXPECT_EQ(count(frozen), 0u) << "the freeze mask left as it was";
}

TEST(AppMarkingMenu, VisibilityGroupsListTheGuideGroups) {
    MainWindow window;
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    auto groupsRing = [&] {
        const std::vector<Item> items = AppActions::radialMenuItems(window);
        const Item* visibility = find(items, "Visibility");
        EXPECT_TRUE(visibility);
        const Item* groups = visibility ? find(visibility->items, "guideGroups") : nullptr;
        EXPECT_TRUE(groups);
        return groups ? *groups : Item();
    };
    const auto h = window.document()->session().authoredHierarchy();
    ASSERT_TRUE(h && !h->groups().empty());
    const std::string first = h->groups().front().name;
    Item groups = groupsRing();
    EXPECT_EQ(groups.label, QStringLiteral("Groups"));
    ASSERT_EQ(groups.items.size(), std::min<std::size_t>(h->groups().size(), 8));
    const Item* row = find(groups.items, first);
    ASSERT_TRUE(row);
    EXPECT_EQ(row->kind, Kind::Option);
    EXPECT_TRUE(row->checkable && row->checked) << "shown";
    // Picked: hidden (as the Guides panel's eye), drawn unchecked the next time; picked again: shown.
    AppActions::runMarkingMenuItem(window, *row);
    EXPECT_TRUE(window.document()->hiddenGroups().count(first));
    groups = groupsRing();
    EXPECT_FALSE(find(groups.items, first)->checked);
    AppActions::runMarkingMenuItem(window, *find(groups.items, first));
    EXPECT_FALSE(window.document()->hiddenGroups().count(first));
    // Past eight groups the eighth place opens the rest.
    window.document()->editGuides("groups", [](GuideHierarchy& g) {
        for (int i = 0; i < 10; ++i) g.addGroup("extra" + std::to_string(i));
    });
    groups = groupsRing();
    ASSERT_EQ(groups.items.size(), 8u);
    EXPECT_EQ(groups.items.back().kind, Kind::Submenu);
    EXPECT_EQ(groups.items.back().position, 7);
    std::size_t listed = 0;
    std::function<void(const Item&)> walk = [&](const Item& ring) {
        for (const Item& item : ring.items) {
            if (item.kind == Kind::Submenu) walk(item);
            else ++listed;
        }
    };
    walk(groups);
    EXPECT_EQ(listed, window.document()->session().authoredHierarchy()->groups().size());
}

}  // namespace
}  // namespace groomlab::app

namespace groomlab::app {
namespace {

TEST(AppMarkingMenu, SizeScalesTheRingsAndTheWheelSetsIt) {
    QWidget host;
    host.resize(1200, 900);
    MarkingMenu menu(&host);
    menu.setItems(handItems());
    const QPointF c(600.0, 450.0);
    menu.popup(c, false);
    const int cut = menu.indexOf("cut");
    const QRectF small = menu.itemRect(cut);
    menu.setScale(2.0);
    const QRectF big = menu.itemRect(cut);
    EXPECT_NEAR(big.width(), small.width() * 2.0, 1.0);
    EXPECT_NEAR(big.height(), small.height() * 2.0, 1.0);
    EXPECT_NEAR(big.center().x() - c.x(), (small.center().x() - c.x()) * 2.0, 1.0) << "the reach grows too";
    EXPECT_DOUBLE_EQ(menu.deadZone(), MarkingMenu::kDeadZone * 2.0);
    menu.setScale(10.0);
    EXPECT_DOUBLE_EQ(menu.scale(), MarkingMenu::kMaxScale);
    menu.setScale(1.0);
    // The wheel: a tenth a notch, said.
    std::vector<qreal> said;
    QObject::connect(&menu, &MarkingMenu::scaleChanged, [&](qreal s) { said.push_back(s); });
    QWheelEvent up(c, menu.mapToGlobal(c), QPoint(), QPoint(0, 240), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&menu, &up);
    EXPECT_DOUBLE_EQ(menu.scale(), 1.2);
    ASSERT_EQ(said.size(), 1u);
    menu.dismiss();
}

TEST(AppMarkingMenu, SettingsSetTheShortcutAndTheSize) {
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QCoreApplication::processEvents();
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    const QKeySequence before = window.radialMenuShortcut();
    const qreal beforeScale = window.markingMenu()->scale();
    QDialog* dialog = AppActions::showRadialMenuSettings(window);
    ASSERT_TRUE(dialog);
    EXPECT_EQ(AppActions::showRadialMenuSettings(window), dialog) << "one at a time";
    auto* keys = dialog->findChild<QKeySequenceEdit*>(QStringLiteral("radialShortcut"));
    auto* size = dialog->findChild<QSpinBox*>(QStringLiteral("radialScale"));
    ASSERT_TRUE(keys && size);
    // Another shortcut: the menu opens on it, its key the one held.
    keys->setKeySequence(QKeySequence(Qt::CTRL | Qt::Key_Q));
    emit keys->editingFinished();
    EXPECT_EQ(window.radialMenuShortcut(), QKeySequence(Qt::CTRL | Qt::Key_Q));
    auto* shortcut = window.findChild<QShortcut*>(QStringLiteral("radialMenuShortcut"));
    ASSERT_TRUE(shortcut);
    EXPECT_EQ(shortcut->key(), QKeySequence(Qt::CTRL | Qt::Key_Q));
    emit shortcut->activated();
    MarkingMenu* menu = window.markingMenu();
    ASSERT_TRUE(menu->shown());
    key(menu, QEvent::KeyRelease, Qt::Key_Q);  // (let go without moving: it stays)
    EXPECT_TRUE(menu->shown());
    menu->dismiss();
    // Cleared: Alt+S again.
    keys->clear();
    emit keys->editingFinished();
    EXPECT_EQ(window.radialMenuShortcut(), QKeySequence(Qt::ALT | Qt::Key_S));
    // The size, live.
    size->setValue(150);
    EXPECT_DOUBLE_EQ(menu->scale(), 1.5);
    // Back as they were (the settings are the person's).
    window.setRadialMenuShortcut(before);
    window.setRadialMenuScale(beforeScale);
    dialog->close();
}

}  // namespace
}  // namespace groomlab::app
