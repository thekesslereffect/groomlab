// SPDX-License-Identifier: Apache-2.0
// The marking menu over the viewport (marking_menu.h, AppActions::markingMenuItems): its items
// from engine/ui/marking_menu.json as Maya's hotkeys.py builds them, its layout (no item over
// another, inside the viewport, the list beside the radial items when it does not fit under
// them), release-to-pick and click-to-pick, and Y + click in the window's viewport picking a
// tool. The offscreen platform; no dialog.

#include <set>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QDockWidget>
#include <QKeyEvent>
#include <QMouseEvent>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "app_actions.h"
#include "app_shell.h"
#include "definitions.h"
#include "groomlab/brush_catalog.h"
#include "main_window.h"
#include "marking_menu.h"
#include "overlay.h"
#include "viewport.h"

namespace groomlab::app {
namespace {

using nlohmann::json;
using Item = MarkingMenu::Item;

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

int indexOf(const MarkingMenu& menu, const std::string& id) {
    for (std::size_t i = 0; i < menu.items().size(); ++i) {
        if (menu.items()[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

// The items as the definitions give them, with no window (the tail's action a stand-in).
std::vector<Item> definitionItems() {
    std::vector<Item> items;
    const ui::Definitions& d = ui::Definitions::shipped();
    std::set<std::string> radial;
    for (const json& r : d.get("markingMenu/radial")) {
        Item item;
        item.kind = Item::Kind::Radial;
        item.id = r.at("tool").get<std::string>();
        item.label = QString::fromStdString(brushEntry(item.id)->label);
        item.position = MarkingMenu::positionIndex(r.at("position").get<std::string>());
        items.push_back(item);
        radial.insert(item.id);
    }
    for (const BrushEntry& e : brushCatalog()) {
        if (radial.count(e.id) || e.id == "select" || e.id == "freeze") continue;
        Item item;
        item.id = e.id;
        item.label = QString::fromStdString(e.label);
        items.push_back(item);
    }
    for (const char* id : {"select", "freeze"}) {
        Item item;
        item.kind = Item::Kind::Tail;
        item.id = id;
        item.label = QString::fromStdString(brushEntry(id)->label);
        items.push_back(item);
    }
    Item tools;
    tools.kind = Item::Kind::Tail;
    tools.id = "tools.open";
    tools.tool = false;
    tools.label = QStringLiteral("Groom Tools");
    items.push_back(tools);
    return items;
}

void expectLaidOut(const MarkingMenu& menu, const QRect& host) {
    for (std::size_t i = 0; i < menu.items().size(); ++i) {
        const QRectF a = menu.itemRect(static_cast<int>(i));
        ASSERT_TRUE(a.isValid()) << menu.items()[i].id;
        EXPECT_TRUE(QRectF(host).contains(a)) << menu.items()[i].id << " outside the viewport";
        for (std::size_t j = i + 1; j < menu.items().size(); ++j) {
            EXPECT_FALSE(a.intersects(menu.itemRect(static_cast<int>(j)))) << menu.items()[i].id << " over " << menu.items()[j].id;
        }
    }
}

TEST(AppMarkingMenu, ItemsFollowTheDefinitions) {
    MainWindow window;
    const std::vector<Item> items = AppActions::markingMenuItems(window);
    const ui::Definitions& d = ui::Definitions::shipped();
    // The radial brushes at their compass points, in the file's order.
    const json& radial = d.get("markingMenu/radial");
    ASSERT_GE(items.size(), radial.size());
    std::set<int> positions;
    for (std::size_t i = 0; i < radial.size(); ++i) {
        EXPECT_EQ(items[i].kind, Item::Kind::Radial);
        EXPECT_EQ(items[i].id, radial[i].at("tool").get<std::string>());
        EXPECT_EQ(items[i].position, MarkingMenu::positionIndex(radial[i].at("position").get<std::string>()));
        EXPECT_FALSE(items[i].icon.isEmpty()) << items[i].id;
        positions.insert(items[i].position);
    }
    EXPECT_EQ(positions.size(), radial.size()) << "a compass point each";
    EXPECT_EQ(items[0].label, QStringLiteral("Comb"));
    // Then the rest of the catalog in its order, Select and Freeze excluded (the tail has them).
    std::vector<std::string> overflow, expected, tail;
    for (const Item& item : items) {
        if (item.kind == Item::Kind::Overflow) overflow.push_back(item.id);
        if (item.kind == Item::Kind::Tail) tail.push_back(item.id);
    }
    for (const BrushEntry& e : brushCatalog()) {
        bool onRadial = false;
        for (const json& r : radial) onRadial = onRadial || r.at("tool").get<std::string>() == e.id;
        if (!onRadial && e.id != "select" && e.id != "freeze") expected.push_back(e.id);
    }
    EXPECT_EQ(overflow, expected);
    EXPECT_EQ(tail, (std::vector<std::string>{"select", "freeze", "tools.open"}));
    EXPECT_EQ(items.back().label, QStringLiteral("Groom Tools"));
    EXPECT_FALSE(items.back().tool);
    // Every tail action is one the app binds (the unbound would be left out).
    const ui::Bindings bound = AppActions::bindings(window);
    for (const Item& item : items) {
        if (!item.tool) EXPECT_TRUE(bound.count(item.id)) << item.id;
    }
}

TEST(AppMarkingMenu, LayoutKeepsItemsApartAndInside) {
    QWidget host;
    host.resize(1280, 900);
    MarkingMenu menu(&host);
    menu.setItems(definitionItems());
    host.show();
    const QPointF middle(640.0, 260.0);
    menu.popup(middle, false);
    ASSERT_TRUE(menu.shown());
    EXPECT_EQ(menu.geometry(), host.rect()) << "as big as the host";
    expectLaidOut(menu, host.rect());
    // Each radial item on its side of the middle: N above, E to the right, S below, W to the left.
    const QRectF n = menu.itemRect(indexOf(menu, "comb")), e = menu.itemRect(indexOf(menu, "smooth"));
    const QRectF s = menu.itemRect(indexOf(menu, "place")), w = menu.itemRect(indexOf(menu, "length"));
    EXPECT_LT(n.bottom(), middle.y());
    EXPECT_NEAR(n.center().x(), middle.x(), 1.0);
    EXPECT_GT(e.left(), middle.x() + MarkingMenu::kDeadZone);
    EXPECT_NEAR(e.center().y(), middle.y(), 1.0);
    EXPECT_GT(s.top(), middle.y());
    EXPECT_LT(w.right(), middle.x() - MarkingMenu::kDeadZone);
    // The list under the radial items, the tail last.
    const QRectF part = menu.itemRect(indexOf(menu, "part")), tools = menu.itemRect(indexOf(menu, "tools.open"));
    EXPECT_GT(part.top(), s.bottom());
    EXPECT_GT(tools.top(), menu.itemRect(indexOf(menu, "freeze")).top());
    // Near the bottom the list goes beside the radial items, still inside.
    menu.popup(QPointF(640.0, 820.0), false);
    expectLaidOut(menu, host.rect());
    EXPECT_GT(menu.itemRect(indexOf(menu, "part")).left(), menu.itemRect(indexOf(menu, "smooth")).right());
    // Near the right edge too (the list on the left then).
    menu.popup(QPointF(1250.0, 880.0), false);
    expectLaidOut(menu, host.rect());
    EXPECT_LT(menu.center().x(), 1250.0) << "the middle moved in from the corner";
    EXPECT_LT(menu.center().y(), 880.0);
    EXPECT_LT(menu.itemRect(indexOf(menu, "part")).right(), menu.itemRect(indexOf(menu, "length")).left());
}

TEST(AppMarkingMenu, ReleaseToPickAndClickToPick) {
    QWidget host;
    host.resize(1280, 720);
    MarkingMenu menu(&host);
    menu.setItems(definitionItems());
    host.show();
    std::vector<std::string> picked;
    QObject::connect(&menu, &MarkingMenu::picked, [&picked](const Item& item) { picked.push_back(item.id); });
    const QPointF c(640.0, 280.0);
    // Press, drag right and let go: the E item (Smooth), by its direction alone.
    menu.popup(c, true);
    EXPECT_TRUE(menu.held());
    menu.track(c + QPointF(40.0, 4.0));
    EXPECT_EQ(menu.hovered(), indexOf(menu, "smooth"));
    menu.release(c + QPointF(40.0, 4.0));
    EXPECT_EQ(picked, std::vector<std::string>{"smooth"});
    EXPECT_FALSE(menu.shown());
    // Up and right: NE (Slide); down and left: SW (Draw).
    menu.popup(c, true);
    EXPECT_EQ(menu.itemAt(c + QPointF(50.0, -50.0)), indexOf(menu, "slide"));
    EXPECT_EQ(menu.itemAt(c + QPointF(-30.0, 30.0)), indexOf(menu, "draw"));
    EXPECT_EQ(menu.itemAt(c + QPointF(5.0, 5.0)), -1) << "the dead zone in the middle";
    // Over a list row: that row, whatever the direction.
    const int attract = indexOf(menu, "attract");
    menu.track(menu.itemRect(attract).center());
    menu.release(menu.itemRect(attract).center());
    EXPECT_EQ(picked.back(), "attract");
    // Away and back to the middle, then let go: nothing.
    menu.popup(c, true);
    menu.track(c + QPointF(0.0, -80.0));
    menu.track(c);
    menu.release(c);
    EXPECT_EQ(picked.size(), 2u);
    EXPECT_FALSE(menu.shown());
    // A click (no drag): the menu stays, the next click picks (the tail's Groom Tools here).
    menu.popup(c, true);
    menu.release(c + QPointF(2.0, 1.0));
    ASSERT_TRUE(menu.shown());
    EXPECT_FALSE(menu.held());
    const QPointF tools = menu.itemRect(indexOf(menu, "tools.open")).center();
    mouse(&menu, QEvent::MouseMove, tools);
    EXPECT_EQ(menu.hovered(), indexOf(menu, "tools.open"));
    mouse(&menu, QEvent::MouseButtonPress, tools);
    EXPECT_EQ(picked.back(), "tools.open");
    EXPECT_FALSE(menu.shown());
    // Esc, and a click in the middle, close it.
    menu.popup(c, false);
    key(&menu, QEvent::KeyPress, Qt::Key_Escape);
    EXPECT_FALSE(menu.shown());
    menu.popup(c, false);
    mouse(&menu, QEvent::MouseButtonPress, c);
    EXPECT_FALSE(menu.shown());
    EXPECT_EQ(picked.size(), 3u);
}

TEST(AppMarkingMenu, YClickInTheViewportPicksATool) {
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    QCoreApplication::processEvents();
    Viewport* view = window.overlay()->viewport();
    const QPointF at(view->width() / 2.0, view->height() / 2.0);
    // No groom: no menu (and a message).
    key(view, QEvent::KeyPress, Qt::Key_Y);
    mouse(view, QEvent::MouseButtonPress, at);
    EXPECT_FALSE(window.markingMenu()->shown());
    mouse(view, QEvent::MouseButtonRelease, at);
    key(view, QEvent::KeyRelease, Qt::Key_Y);
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    ASSERT_EQ(view->toolMode(), Viewport::ToolMode::Camera);
    // Y held, press, drag up and let go: Comb (N), the brush on.
    key(view, QEvent::KeyPress, Qt::Key_Y);
    mouse(view, QEvent::MouseButtonPress, at);
    ASSERT_TRUE(window.markingMenu()->shown());
    EXPECT_TRUE(window.markingMenu()->held());
    mouse(view, QEvent::MouseMove, at + QPointF(0.0, -60.0));
    mouse(view, QEvent::MouseButtonRelease, at + QPointF(0.0, -60.0));
    key(view, QEvent::KeyRelease, Qt::Key_Y);
    EXPECT_FALSE(window.markingMenu()->shown());
    EXPECT_EQ(view->toolMode(), Viewport::ToolMode::Brush);
    EXPECT_EQ(window.brush().toolId(), "comb");
    // Without Y the left button strokes as before: no menu.
    mouse(view, QEvent::MouseButtonPress, at);
    EXPECT_FALSE(window.markingMenu()->shown());
    mouse(view, QEvent::MouseButtonRelease, at);
    // Select from the tail: the Select tool (never toggled back to the camera).
    for (int round = 0; round < 2; ++round) {
        ASSERT_TRUE(window.showMarkingMenu(window.overlay()->rect().center(), false));
        MarkingMenu* menu = window.markingMenu();
        mouse(menu, QEvent::MouseButtonPress, menu->itemRect(indexOf(*menu, "select")).center());
        EXPECT_EQ(view->toolMode(), Viewport::ToolMode::Select) << round;
    }
    // Groom Tools: the dock shown, in Groom mode.
    window.shell()->groomToolsDock()->hide();
    ASSERT_TRUE(window.showMarkingMenu(window.overlay()->rect().center(), false));
    MarkingMenu* menu = window.markingMenu();
    mouse(menu, QEvent::MouseButtonPress, menu->itemRect(indexOf(*menu, "tools.open")).center());
    EXPECT_TRUE(window.shell()->groomToolsDock()->isVisible());
}

}  // namespace
}  // namespace groomlab::app
