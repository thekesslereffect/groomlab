// SPDX-License-Identifier: Apache-2.0
#pragma once

// The radial menu over the viewport, as GroomBear's radial menu for Houdini and RadialZ for
// ZBrush, from engine/ui/marking_menu.json's radialMenu: rings of pills around the cursor, eight
// compass points each. A ring's item is a tool (a brush), an action of the definitions, a brush
// option (a setting's switch, or one of its values) or a submenu, whose own ring opens around
// its pill: Masking (S), Visibility (W), Brushes A (NW), Brushes B (NE) and Brush Options (E, the
// picked brush's, made each time it opens: AppActions::radialMenuItems).
//
// Alt+S (MainWindow's shortcut, popupHeldKey): the root ring opens around the cursor at once, no
// click; move toward an item and let S go to pick it. A held button works the same (popup with
// held, track, release: drag toward an item: a submenu opens around its pill once the cursor is
// over it or past it (its ring then reads directions from there, as a marking menu's flick); let
// go over a tool, an action or an option to pick it). Let go without moving, or in a submenu over
// nothing, and the menu stays open for clicks: a click picks or opens, a click in the middle (or
// Esc, or the right button) goes back a ring, at the root closes it; a click away closes it.
// Moving back into the middle of the ring a submenu came from goes back too.
//
// Painted with QPainter on a transparent child of the overlay host as big as the host (nothing
// behind the items is covered; it exists only while it shows), in the theme's tokens: solid panel
// pills with a thin edge, the hovered one filled with its color (a tool's category color), the
// rings left behind dimmed, the one open on top.
//
//     auto* menu = new MarkingMenu(host);
//     menu->setItems(AppActions::radialMenuItems(window));
//     menu->popupHeldKey(at, Qt::Key_S);  // Alt+S held
//     menu->popup(at, true);          // or a held button
//     menu->track(cursor);            // ... it moved
//     menu->release(cursor);          // ... let go: picked() when over a tool, action or option

#include <string>
#include <vector>

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QWidget>

namespace groomlab::app {

class MarkingMenu : public QWidget {
    Q_OBJECT
public:
    struct Item {
        enum class Kind { Tool, Action, Option, Submenu };
        Kind kind = Kind::Tool;
        std::string id;       // a tool's id ("comb"), an action's ("mask.full"), an option's setting ("stretch")
        std::string value;    // Option: the setting's value it picks ("all"); "" a switch
        QString label, tip;
        QString icon;         // an icon resource ("" none)
        QColor color;         // the hovered fill (a tool's category color); invalid: the theme's
        int position = -1;    // 0 N, 1 NE, 2 E ... 7 NW (marking_menu.json's position)
        bool checkable = false, checked = false;  // a switch or a value: drawn with its state
        bool enabled = true;  // false: drawn dim, not picked (Brush Options with no options)
        std::vector<Item> items;  // Submenu: its ring
    };
    // The compass point's index (N 0 ... NW 7), -1 for an unknown name.
    static int positionIndex(const std::string& name);

    explicit MarkingMenu(QWidget* host);  // covers the host, hidden

    void setItems(std::vector<Item> items);  // the root ring
    const std::vector<Item>& items() const { return items_; }
    // What the items work on (the groom's name), shown faintly over the root ring's middle; "" none.
    void setContext(const QString& text) { context_ = text; }
    const QString& context() const { return context_; }

    // Shown with the root ring's middle at `at` (the host's coordinates). `held`: a button is down,
    // so a drag and its release pick (track, release); else it waits for clicks.
    void popup(QPointF at, bool held);
    // Shown by a held hotkey (Alt+S: `key` Qt::Key_S): the items follow the cursor at once, no button
    // needed; letting the key go over a tool, an action or an option (the cursor moved) picks it,
    // else the menu stays for clicks.
    void popupHeldKey(QPointF at, int key);
    void track(QPointF at);    // the held button moved (the host's coordinates)
    void release(QPointF at);  // ... and was let go: picks what is under it, or stays open
    void dismiss();
    bool shown() const { return isVisible(); }
    bool held() const { return held_; }

    // The ring open now (the root at depth 0): its items, its middle and each item's box (this
    // widget's coordinates, the same as the host's).
    int depth() const { return static_cast<int>(levels_.size()) - 1; }
    const std::vector<Item>& ring() const;
    QPointF center() const;
    QRectF itemRect(int index) const;
    // The ring's item under `at` (the host's coordinates): a pill under it, else the item whose
    // direction from the middle is nearest, past the dead zone; -1 none.
    int itemAt(QPointF at) const;
    int hovered() const { return hovered_; }
    // The ring's index of the item with this id (a submenu's: its label), -1 none.
    int indexOf(const std::string& id) const;
    void open(int index);  // the ring's submenu opened around its pill
    void open(int index, QPointF at);  // ... around a point (where the cursor opened it)
    void back();           // the ring it came from (at the root: dismissed)
    static constexpr qreal kDeadZone = 18.0;
    // The size: 1 as designed, up to kMaxScale (a big screen); the pills, their text and icons, the
    // reach and the dead zone all grow with it. The mouse wheel over the menu changes it too
    // (scaleChanged: the window keeps it).
    static constexpr qreal kMinScale = 0.75, kMaxScale = 2.5;
    void setScale(qreal scale);
    qreal scale() const { return scale_; }
    qreal deadZone() const { return kDeadZone * scale_; }

signals:
    void picked(const MarkingMenu::Item& item);
    void dismissed();
    void scaleChanged(qreal scale);  // the wheel resized it

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;  // the held hotkey (popupHeldKey)
    void wheelEvent(QWheelEvent* event) override;  // the size (setScale)
    bool eventFilter(QObject* watched, QEvent* event) override;  // the host's resize

private:
    struct Level {
        std::vector<int> path;  // the submenus' indices from the root to this ring
        QPointF center;
        std::vector<QRectF> rects;  // per item
        int opened = -1;            // the submenu open from it
    };
    const std::vector<Item>& itemsOf(const Level& level) const;
    void layoutLevel(Level& level);
    void hover(QPointF at);
    void pick(int index);

    std::vector<Item> items_;
    QString context_;
    std::vector<Level> levels_;  // the root first, the ring open last
    QPointF cursor_, pressAt_;
    int hovered_ = -1;
    bool held_ = false;
    int key_ = 0;         // the hotkey held since popupHeldKey (0 none)
    qreal scale_ = 1.0;
    bool moved_ = false;  // the held button went past the dead zone from where it was pressed
};

}  // namespace groomlab::app
