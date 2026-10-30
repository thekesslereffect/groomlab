// SPDX-License-Identifier: Apache-2.0
#pragma once

// The Groom Tools' lists' actions, as Maya's tool_tabs.py lays them out, from the shared UI
// definitions (engine/ui/lists.json through definitions.h): a list's bar as one row (the main
// actions as labelled chips on the left, the rest as small line glyphs on the right; a submenu's
// button opens its menu) and its right-click menu (the entries whose "needs" hold for the row
// clicked). The window's bindings (app_actions.h) say what each id runs.
//
//     auto* bar = new ListBar(this);
//     bar->build(defs.get("lists/groups/bar"), bindings, options);
//     QMenu* menu = listMenu(this, defs.get("lists/groups/context"), bindings, options, {"group"});
//     menu->popup(globalPos);  // (deleted when it closes)

#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <QPointer>
#include <QWidget>

#include "definitions.h"

class QAbstractButton;
class QIcon;
class QLabel;
class QMenu;
class QToolButton;

namespace groomlab::app {

class ListBar : public QWidget {
    Q_OBJECT
public:
    explicit ListBar(QWidget* parent = nullptr);
    // Which of a bar's entries a row takes: every one; the labeled ones alone (a list's header:
    // + New..., Import..., Fix Frame as line glyphs, the others as chips); the glyphs alone (the
    // picked item's box: rename, convert, delete...).
    enum class Part { All, Labeled, Glyphs };
    // The row from a bar's entries (made again: an earlier build goes).
    void build(const ui::json& entries, const ui::Bindings& bindings, const ui::FillOptions& options, Part part = Part::All);
    // The button of an action id (a submenu's by its "id"); null when the bar has none.
    QToolButton* button(const std::string& id) const;
    std::vector<std::string> ids() const;  // in the row's order
    void refresh();  // the buttons' enabled states from their bindings
    // One row (chips left, glyphs right) when the width takes it, else the glyphs on a second row
    // (right-aligned): a narrow dock never clips the last glyph.
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    int rowWidth(bool chips) const;
    int rowHeight() const;
    void place();
    std::vector<std::pair<std::string, QToolButton*>> buttons_;
    std::vector<std::pair<QPointer<QAbstractButton>, std::function<bool()>>> enables_;
};

// A list's right-click menu from `entries` and the conditions that hold for the row clicked
// (`state`): a menu the caller pops up (it deletes itself when it closes).
QMenu* listMenu(QWidget* parent, const ui::json& entries, const ui::Bindings& bindings, const ui::FillOptions& options,
                const std::set<std::string>& state);
// A line glyph (rename, select, delete, up, down, convert, solo: Maya's widgets.GLYPHS) in the
// theme's text color.
QIcon glyphIcon(const std::string& glyph, qreal devicePixelRatio, int size = 18);
// A list's "about" sentence (rich text, wrapped, the theme's muted text).
QLabel* aboutLabel(const std::string& html, QWidget* parent);
// The window's bindings and fill options a list's bar and menus use (no live keys: the menu bar
// holds them).
struct ListActions {
    ui::Bindings bindings;
    ui::FillOptions options;
    bool ready() const { return !bindings.empty(); }
};

}  // namespace groomlab::app
