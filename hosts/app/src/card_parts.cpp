// SPDX-License-Identifier: Apache-2.0
#include "card_parts.h"

#include <algorithm>

#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QListView>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include "glass.h"
#include "theme.h"

namespace groomlab::app {

// ---- FlowLayout ------------------------------------------------------------------------------------

FlowLayout::FlowLayout(QWidget* parent, int spacing) : QLayout(parent), gap_(spacing) { setContentsMargins(0, 0, 0, 0); }

FlowLayout::~FlowLayout() {
    for (QLayoutItem* item : items_) delete item;
}

void FlowLayout::addItem(QLayoutItem* item) { items_.push_back(item); }

QLayoutItem* FlowLayout::itemAt(int index) const {
    return index >= 0 && index < count() ? items_[static_cast<std::size_t>(index)] : nullptr;
}

QLayoutItem* FlowLayout::takeAt(int index) {
    if (index < 0 || index >= count()) return nullptr;
    QLayoutItem* item = items_[static_cast<std::size_t>(index)];
    items_.erase(items_.begin() + index);
    return item;
}

int FlowLayout::heightForWidth(int width) const { return place(QRect(0, 0, width, 0), false); }

QSize FlowLayout::minimumSize() const {
    QSize size;
    for (const QLayoutItem* item : items_) {
        if (!item->isEmpty()) size = size.expandedTo(item->minimumSize());
    }
    const QMargins m = contentsMargins();
    return size + QSize(m.left() + m.right(), m.top() + m.bottom());
}

QSize FlowLayout::sizeHint() const {
    // One row, as wide as every item side by side.
    int w = 0, h = 0, n = 0;
    for (const QLayoutItem* item : items_) {
        if (item->isEmpty()) continue;
        const QSize s = item->sizeHint();
        w += s.width() + (n++ ? gap_ : 0);
        h = std::max(h, s.height());
    }
    const QMargins m = contentsMargins();
    return {w + m.left() + m.right(), h + m.top() + m.bottom()};
}

void FlowLayout::setGeometry(const QRect& rect) {
    QLayout::setGeometry(rect);
    place(rect, true);
}

int FlowLayout::place(const QRect& rect, bool apply) const {
    // An even grid of chips: as many equal columns (at most three) as the widest chip allows, each
    // chip a column wide, so rows line up and no chip is left alone at its own width.
    const QMargins m = contentsMargins();
    const QRect area = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
    std::vector<QLayoutItem*> shown;
    int widest = 0;
    for (QLayoutItem* item : items_) {
        if (item->isEmpty()) continue;
        shown.push_back(item);
        widest = std::max(widest, item->sizeHint().width());
    }
    if (shown.empty()) return 0;
    const int n = static_cast<int>(shown.size());
    int columns = std::clamp((area.width() + gap_) / std::max(1, widest + gap_), 1, std::min(maxColumns_, n));
    // A track's few segments on one row when each still gets most of its width (their padding gives).
    if (!lastRowStretch_ && n <= maxColumns_ && (area.width() - (n - 1) * gap_) / n >= widest * 85 / 100) columns = n;
    const int column = lastRowStretch_ ? std::max(widest, (area.width() - (columns - 1) * gap_) / columns) : (area.width() - (columns - 1) * gap_) / columns;
    // The last row's chips share its whole width: no hole at its end.
    const int lastRow = (n - 1) / columns, lastCount = n - lastRow * columns;
    const int lastColumn = std::max(widest, (area.width() - (lastCount - 1) * gap_) / std::max(1, lastCount));
    int y = area.y(), line = 0;
    for (std::size_t i = 0; i < shown.size(); ++i) {
        const int c = static_cast<int>(i) % columns;
        if (c == 0 && i > 0) {
            y += line + gap_;
            line = 0;
        }
        const QSize s = shown[i]->sizeHint();
        if (apply) {
            // A chip fills its column (a fixed-size button would sit at its own width in it).
            if (QWidget* w = shown[i]->widget(); w && w->sizePolicy().horizontalPolicy() != QSizePolicy::Expanding) {
                w->setSizePolicy(QSizePolicy::Expanding, w->sizePolicy().verticalPolicy());
            }
            const bool last = lastRowStretch_ && static_cast<int>(i) / columns == lastRow;
            const int width = last ? lastColumn : column;
            shown[i]->setGeometry(QRect(QPoint(area.x() + c * (width + gap_), y), QSize(width, s.height())));
        }
        line = std::max(line, s.height());
    }
    return y + line - rect.y() + m.bottom();
}

// ---- Section ---------------------------------------------------------------------------------------

Section::Section(const QString& title, QWidget* content, bool expanded, QWidget* parent)
    : QWidget(parent), title_(title), content_(content) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    header_ = new QPushButton(this);
    header_->setCheckable(true);
    header_->setFlat(true);
    header_->setProperty("role", "section");
    header_->setCursor(Qt::PointingHandCursor);
    connect(header_, &QPushButton::toggled, this, [this](bool on) {
        setExpanded(on);
        emit toggled(on);
    });
    layout->addWidget(header_);
    content_->setParent(this);
    layout->addWidget(content_);
    setExpanded(expanded);
}

void Section::setExpanded(bool expanded) {
    expanded_ = expanded;
    {
        const QSignalBlocker blocker(header_);
        header_->setChecked(expanded);
    }
    setSectionHeader(header_, title_, expanded);
    content_->setVisible(expanded);
}

// ---- CardBlock -------------------------------------------------------------------------------------

CardBlock::CardBlock(const QString& title, QWidget* parent) : QFrame(parent) {
    setProperty("role", "block");
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 6, 0, 0);
    column->setSpacing(4);
    head_ = new QHBoxLayout();
    head_->setContentsMargins(0, 0, 0, 0);
    head_->setSpacing(0);
    // As written, as every section title (the list titles, Properties' cards); title() is the word it was given.
    title_ = new QLabel(title, this);
    title_->setProperty("role", "blockTitle");
    title_->setProperty("title", title);
    title_->setMinimumHeight(22);
    head_->addWidget(title_, 1);
    column->addLayout(head_);
    rows_ = new QVBoxLayout();
    rows_->setContentsMargins(0, 0, 0, 0);
    rows_->setSpacing(4);
    column->addLayout(rows_);
}

void CardBlock::addAction(QWidget* action) {
    action->setParent(this);
    head_->addWidget(action);
}

QString CardBlock::title() const { return title_->property("title").toString(); }

QToolButton* makeChip(const QString& text, const QString& tip, QWidget* parent) {
    auto* chip = new QToolButton(parent);
    chip->setProperty("role", "chip");
    chip->setText(text);
    chip->setCheckable(true);
    chip->setToolButtonStyle(Qt::ToolButtonTextOnly);
    if (!tip.isEmpty()) chip->setToolTip(tip);
    return chip;
}

QWidget* chipRow(QWidget* parent, FlowLayout** flow, const QString& label) {
    auto* row = new QWidget(parent);
    auto* column = new QVBoxLayout(row);
    column->setContentsMargins(0, label.isEmpty() ? 0 : 2, 0, 0);
    column->setSpacing(3);
    if (!label.isEmpty()) {
        auto* name = new QLabel(label, row);
        name->setProperty("role", "chipLabel");
        column->addWidget(name);
    }
    auto* chips = new QWidget(row);
    *flow = new FlowLayout(chips, 4);
    column->addWidget(chips);
    return row;
}

QWidget* segmentRow(QWidget* parent, FlowLayout** flow, const QString& label) {
    // A one-of choice: its chips as segments on a sunken track (the style sheet's segTrack), the
    // chosen one raised in a neutral tone; a chip row's toggles stay chips (any of them on).
    QWidget* row = chipRow(parent, flow, label);
    if (QWidget* chips = (*flow)->parentWidget()) {
        chips->setProperty("role", "segTrack");
        chips->setAttribute(Qt::WA_StyledBackground, true);
    }
    (*flow)->setContentsMargins(2, 2, 2, 2);
    (*flow)->setGap(2);
    (*flow)->setMaxColumns(4);
    (*flow)->setLastRowStretch(false);  // a track keeps its column grid
    return row;
}

QIcon lineIcon(const QString& name, qreal dpr, int size) {
    const QString resource = QStringLiteral(":/groomlab/icons/ui_%1.svg").arg(name);
    QIcon icon;
    const QPixmap rest = tintedIcon(resource, theme::textDim(), size, dpr), on = tintedIcon(resource, theme::text(), size, dpr);
    const QPixmap off = tintedIcon(resource, theme::textMuted(), size, dpr);
    icon.addPixmap(rest, QIcon::Normal, QIcon::Off);
    icon.addPixmap(on, QIcon::Active, QIcon::Off);
    icon.addPixmap(on, QIcon::Normal, QIcon::On);
    icon.addPixmap(on, QIcon::Active, QIcon::On);
    icon.addPixmap(off, QIcon::Disabled, QIcon::Off);
    icon.addPixmap(off, QIcon::Disabled, QIcon::On);
    return icon;
}

QToolButton* makeGlyph(const QString& icon, const QString& tip, QWidget* parent, bool checkable) {
    auto* b = new QToolButton(parent);
    b->setProperty("role", "glyph");
    b->setAutoRaise(true);
    b->setCheckable(checkable);
    b->setToolTip(tip);
    b->setIconSize(QSize(15, 15));
    b->setIcon(lineIcon(icon, parent ? parent->devicePixelRatioF() : 1.0, 15));
    b->setFixedSize(24, 24);
    return b;
}

QPushButton* makeGhost(const QString& text, const QString& tip, QWidget* parent) {
    auto* b = new QPushButton(text, parent);
    b->setProperty("role", "ghost");
    b->setFlat(true);
    if (!tip.isEmpty()) b->setToolTip(tip);
    b->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    return b;
}

// ---- Lists -----------------------------------------------------------------------------------------

ListHeader::ListHeader(const QString& title, QWidget* parent) : QWidget(parent) {
    setProperty("role", "listHeader");
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 6, 0, 0);
    row->setSpacing(6);
    title_ = new QLabel(title, this);
    title_->setProperty("role", "listTitle");
    row->addWidget(title_);
    count_ = new QLabel(this);
    count_->setProperty("role", "listCount");
    count_->hide();
    row->addWidget(count_);
    row->addStretch(1);
    glyphs_ = new QHBoxLayout();
    glyphs_->setContentsMargins(0, 0, 0, 0);
    glyphs_->setSpacing(0);
    row->addLayout(glyphs_);
}

void ListHeader::setCount(int count) {
    count_->setVisible(count >= 0);
    count_->setText(QString::number(std::max(count, 0)));
}

void ListHeader::addGlyph(QWidget* glyph, int index) {
    glyph->setParent(this);
    glyphs_->insertWidget(index < 0 ? glyphs_->count() : std::min(index, glyphs_->count()), glyph);
    glyph->show();
}

QFrame* itemBox(QWidget* parent, QVBoxLayout** rows) {
    auto* box = new QFrame(parent);
    box->setProperty("role", "itemBox");
    auto* column = new QVBoxLayout(box);
    column->setContentsMargins(6, 4, 6, 6);
    column->setSpacing(4);
    *rows = column;
    return box;
}

void plainRows(QAbstractItemView* view) {
    view->setProperty("role", "rows");
    view->setFrameShape(QFrame::NoFrame);
    view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    if (auto* tree = qobject_cast<QTreeView*>(view)) tree->setHeaderHidden(true);
}

void fitRows(QAbstractItemView* view, int minRows) {
    int rows = 0, height = 0;
    const int fallback = std::max(view->fontMetrics().height() + 10, theme::rowHeight());
    if (auto* tree = qobject_cast<QTreeView*>(view)) {
        for (QModelIndex at = tree->model() ? tree->model()->index(0, 0) : QModelIndex(); at.isValid(); at = tree->indexBelow(at)) {
            const int h = tree->visualRect(at).height();
            height += h > 0 ? h : fallback;
            ++rows;
        }
    } else if (view->model()) {
        rows = view->model()->rowCount();
        for (int r = 0; r < rows; ++r) {
            const int h = view->sizeHintForRow(r);
            height += h > 0 ? h : fallback;
        }
    }
    if (rows < minRows) height += (minRows - rows) * fallback;
    const QMargins m = view->contentsMargins();
    view->setFixedHeight(height + m.top() + m.bottom() + 2 * view->frameWidth() + 2);
}

}  // namespace groomlab::app
