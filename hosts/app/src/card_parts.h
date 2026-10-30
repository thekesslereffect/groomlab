// SPDX-License-Identifier: Apache-2.0
#pragma once

// The Groom Tools' small parts, as the redesign lays them out (research/ui-redesign/QT.md, "Groom
// Tools layout"): the tool card's blocks (a small semibold title over a faint hairline, its
// actions on the title's right, then its rows), chips that wrap to the width, line glyphs, a
// list's header (its name, a count, glyphs) and the picked item's box under a list, and the
// folding section (Photos' Guides from photos).

#include <vector>

#include <QFrame>
#include <QLayout>
#include <QString>
#include <QWidget>

class QAbstractItemView;
class QHBoxLayout;
class QIcon;
class QLabel;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace groomlab::app {

// ---- FlowLayout ------------------------------------------------------------------------------------
// Items left to right, wrapping to the next row when the width runs out (a row of chips).
class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget* parent = nullptr, int spacing = 4);
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

private:
    QLabel* title_ = nullptr;
    QHBoxLayout* head_ = nullptr;
    QVBoxLayout* rows_ = nullptr;
};

// A checkable chip (QToolButton, role "chip"): the card's toggles and choices.
QToolButton* makeChip(const QString& text, const QString& tip, QWidget* parent);
// A row of chips that wraps to the width; with a `label`, the name sits above the chips (chips
// don't fit a label column). `flow` takes the chips.
QWidget* chipRow(QWidget* parent, FlowLayout** flow, const QString& label = {});
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
// A list or tree as plain rows: no frame, no header, no sideways bar (role "rows").
void plainRows(QAbstractItemView* view);
// A plain list as tall as its rows (at least `minRows`): it never scrolls inside the column.
void fitRows(QAbstractItemView* view, int minRows = 1);

}  // namespace groomlab::app
