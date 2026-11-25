// SPDX-License-Identifier: Apache-2.0
#pragma once

// The Groom Tools' small parts, as the redesign lays them out (research/ui-redesign/QT.md, "Groom
// Tools layout"): the tool card's blocks (a small semibold title over a faint hairline, its
// actions on the title's right, then its rows), chips that wrap to the width, line glyphs, a
// list's header (its name, a count, glyphs) and the picked item's box under a list, and the
// folding section (Photos' Guides from photos).

#include <algorithm>
#include <functional>
#include <vector>

#include <QFrame>
#include <QLayout>
#include <QString>
#include <QWidget>

class QAbstractItemView;
class QHBoxLayout;
class QIcon;
class QLabel;
class QMenu;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace groomlab::app {

// ---- FlowLayout ------------------------------------------------------------------------------------
// Items left to right, wrapping to the next row when the width runs out (a row of chips).
class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget* parent = nullptr, int spacing = 4);
    void setMaxColumns(int columns) { maxColumns_ = std::max(1, columns); }  // (3 unless said)
    void setGap(int gap) { gap_ = gap; invalidate(); }
    void setLastRowStretch(bool stretch) { lastRowStretch_ = stretch; invalidate(); }  // (on unless said)
    // Natural: each item at its own width, left to right, wrapping when the width runs out (a row
    // of pills); else an even grid of columns (off unless said).
    void setNatural(bool natural) { natural_ = natural; invalidate(); }
    ~FlowLayout() override;
    void addItem(QLayoutItem* item) override;
    int count() const override { return static_cast<int>(items_.size()); }
    QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    QSize minimumSize() const override;
    QSize sizeHint() const override;
    void setGeometry(const QRect& rect) override;

private:
    int place(const QRect& rect, bool apply) const;
    std::vector<QLayoutItem*> items_;
    int gap_ = 4;
    int maxColumns_ = 3;
    bool lastRowStretch_ = true;
    bool natural_ = false;
};

// ---- Section ---------------------------------------------------------------------------------------
// A titled section that folds away: its header shows or hides the content (Maya's widgets.Section).
class Section : public QWidget {
    Q_OBJECT
public:
    Section(const QString& title, QWidget* content, bool expanded = false, QWidget* parent = nullptr);
    void setExpanded(bool expanded);
    bool expanded() const { return expanded_; }
    QPushButton* header() const { return header_; }
    QWidget* content() const { return content_; }

signals:
    void toggled(bool expanded);

private:
    QString title_;
    QPushButton* header_ = nullptr;
    QWidget* content_ = nullptr;
    bool expanded_ = false;
};

// ---- CardBlock -------------------------------------------------------------------------------------
// One block of the tool card (Brush, the tool's own, Applies to): its title (small, semibold, dim)
// over a faint hairline, actions at the title's right, then its rows. Hidden blocks take no room.
class CardBlock : public QFrame {
    Q_OBJECT
public:
    explicit CardBlock(const QString& title, QWidget* parent = nullptr);
    QVBoxLayout* rows() const { return rows_; }
    void addAction(QWidget* action);  // on the title row's right, in order
    QString title() const;
    QLabel* titleLabel() const { return title_; }
    // Foldable (Maya's Block.set_foldable): the title a header with a chevron that shows or hides
    // the rows (the actions stay on its right); `folded` to start with.
    void setFoldable(bool folded);
    void setFolded(bool folded);
    bool folded() const { return folded_; }
    QPushButton* foldHeader() const { return fold_; }
    // The words the title shows ("Selected tubes · 12"); title() stays the word it was given.
    void setHeading(const QString& text);

private:
    QLabel* title_ = nullptr;
    QPushButton* fold_ = nullptr;
    QHBoxLayout* head_ = nullptr;
    QWidget* body_ = nullptr;
    QVBoxLayout* rows_ = nullptr;
    QString heading_;
    bool folded_ = false;
};

// A checkable chip (QToolButton, role "chip"): the card's toggles and choices.
QToolButton* makeChip(const QString& text, const QString& tip, QWidget* parent);
// A row of chips that wraps to the width; with a `label`, the name sits above the chips (chips
// don't fit a label column). `flow` takes the chips.
QWidget* chipRow(QWidget* parent, FlowLayout** flow, const QString& label = {});
// A chip row for a one-of choice: the chips as segments on a sunken track.
QWidget* segmentRow(QWidget* parent, FlowLayout** flow, const QString& label = {});
// A pill (role "pill", the Simple layout's Chip: research/ui-redesign/src/simple/parts.tsx): a
// checkable toggle sized to its text, a row's height less 2, fully round; off an outline in the
// theme's line tone with dim text, on the selection fill under an accent edge. The tool card's
// toggles row and the guides' display popup.
QToolButton* makePill(const QString& text, const QString& tip, QWidget* parent);
void pillStyle(QToolButton* button);  // a button made elsewhere (a chip) worn as a pill
// A row of pills at their own widths, left to right 6 px apart, wrapping to the width.
QWidget* pillRow(QWidget* parent, FlowLayout** flow, const QString& label = {});
// A line glyph (a ui_<name>.svg icon in the theme's dim text, the text color while checked or
// hovered): the card's and the lists' small actions.
QIcon lineIcon(const QString& name, qreal devicePixelRatio, int size = 16);
QToolButton* makeGlyph(const QString& icon, const QString& tip, QWidget* parent, bool checkable = false);
// A small flat text button (role "ghost"): Edit as custom, Clear, Delete key.
QPushButton* makeGhost(const QString& text, const QString& tip, QWidget* parent);

// ---- Lists -----------------------------------------------------------------------------------------
// A list's header: its name (caps, as a section's), how many it holds, glyphs on the right.
class ListHeader : public QWidget {
    Q_OBJECT
public:
    explicit ListHeader(const QString& title, QWidget* parent = nullptr);
    void setCount(int count);  // < 0: none shown
    QLabel* titleLabel() const { return title_; }
    QLabel* countLabel() const { return count_; }
    // A glyph (or any small widget) on the right; `index` -1 after the others, 0 first.
    void addGlyph(QWidget* glyph, int index = -1);

private:
    QLabel* title_ = nullptr;
    QLabel* count_ = nullptr;
    QHBoxLayout* glyphs_ = nullptr;
};

// The picked item's own settings and actions under a list: a small bordered box (role "itemBox").
QFrame* itemBox(QWidget* parent, QVBoxLayout** rows);

// ---- List rows (research/ui-redesign/SIMPLE.md, Maya's widgets.py) ------------------------------
// Every list row is one line, its icons in one order (ROW_ORDER): muted text (a count, a tag) ->
// the settings popup -> state toggles (solo, in front / behind) -> the eye -> the lock -> the row's
// menu, always last. No row expands: a row's settings open from its icon as a small popup.

// A small popup card under a row's icon (a row's settings: segments, weight, map info, photo
// settings): the widgets added stay in it, shown again each time; it closes on a click outside.
class PopupPanel : public QFrame {
    Q_OBJECT
public:
    explicit PopupPanel(QWidget* parent = nullptr, int width = 240);
    QVBoxLayout* rows() const { return rows_; }
    void add(QWidget* widget);
    void add(QLayout* layout);
    // Run before it shows (the row's values filled in).
    void onShow(std::function<void()> fill) { fills_.push_back(std::move(fill)); }
    // Opened under `anchor` (its right edges lined up), kept on the screen.
    void showUnder(QWidget* anchor);

private:
    QVBoxLayout* rows_ = nullptr;
    std::vector<std::function<void()>> fills_;
};

// A row's small icon button (22 px, a 14 px ui_<icon> glyph); `text` beside it (a count); `on`:
// the glyph in the accent (a locked photo, a soloed layer).
QToolButton* rowGlyph(const QString& icon, const QString& tip, QWidget* parent, const QString& text = {}, bool on = false);
// A row icon opening `panel` under it; `before()` runs first (picks the row).
QToolButton* popupButton(const QString& icon, const QString& tip, PopupPanel* panel, std::function<void()> before,
                         QWidget* parent, const QString& text = {});
// The row's menu (ROW_ORDER: always last): "more", its menu made again as it opens by `fill`
// (`before()` first: picks the row).
QToolButton* rowMenuButton(const QString& tip, std::function<void(QMenu*)> fill, std::function<void()> before, QWidget* parent);
// A list row's icons on its right, in ROW_ORDER (the caller hands them in that order).
QWidget* rowStrip(const std::vector<QWidget*>& icons, QWidget* parent = nullptr);
// A QListWidget row (setItemWidget): its name, a muted tag, then the icons. Clicks on the name
// reach the list (the row is picked as ever); the icons take their own.
class ListRow : public QWidget {
    Q_OBJECT
public:
    ListRow(const QString& text, const QString& tag, const std::vector<QWidget*>& icons, QWidget* parent = nullptr);
    QLabel* label() const { return label_; }
    QLabel* tag() const { return tag_; }

private:
    QLabel* label_ = nullptr;
    QLabel* tag_ = nullptr;
};
// A list or tree as plain rows: no frame, no header, no sideways bar (role "rows").
void plainRows(QAbstractItemView* view);
// A plain list as tall as its rows (at least `minRows`): it never scrolls inside the column.
void fitRows(QAbstractItemView* view, int minRows = 1);

}  // namespace groomlab::app
