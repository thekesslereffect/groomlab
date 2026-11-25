// SPDX-License-Identifier: Apache-2.0
#include "card_parts.h"

#include <algorithm>

#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QPushButton>
#include <QScreen>
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
    const QMargins m = contentsMargins();
    const QRect area = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
    if (natural_) {
        // Each item at its own width (never wider than the row), left to right, a new row when the
        // next one would run past the right edge.
        int x = area.x(), y = area.y(), line = 0, placed = 0;
        for (QLayoutItem* item : items_) {
            if (item->isEmpty()) continue;
            QSize s = item->sizeHint();
            s.setWidth(std::min(s.width(), std::max(1, area.width())));
            if (placed > 0 && x + s.width() > area.x() + area.width()) {
                x = area.x();
                y += line + gap_;
                line = 0;
            }
            if (apply) item->setGeometry(QRect(QPoint(x, y), s));
            x += s.width() + gap_;
            line = std::max(line, s.height());
            ++placed;
        }
        return placed ? y + line - rect.y() + m.bottom() : 0;
    }
    // An even grid of chips: as many equal columns (at most three) as the widest chip allows, each
    // chip a column wide, so rows line up and no chip is left alone at its own width.
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
    body_ = new QWidget(this);
    rows_ = new QVBoxLayout(body_);
    rows_->setContentsMargins(0, 0, 0, 0);
    rows_->setSpacing(4);
    column->addWidget(body_);
    heading_ = title;
}

void CardBlock::setFoldable(bool folded) {
    if (!fold_) {
        fold_ = new QPushButton(this);
        fold_->setCheckable(true);
        fold_->setFlat(true);
        fold_->setProperty("role", "section");
        fold_->setCursor(Qt::PointingHandCursor);
        fold_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        fold_->setToolTip(title_->toolTip());
        connect(fold_, &QPushButton::toggled, this, [this](bool open) { setFolded(!open); });
        head_->insertWidget(0, fold_, 1);
        title_->hide();
    }
    setFolded(folded);
}

void CardBlock::setFolded(bool folded) {
    folded_ = folded;
    body_->setVisible(!folded);
    if (!fold_) return;
    const QSignalBlocker block(fold_);
    fold_->setChecked(!folded);
    setSectionHeader(fold_, heading_, !folded);
}

void CardBlock::setHeading(const QString& text) {
    heading_ = text;
    title_->setText(text);
    if (fold_) setSectionHeader(fold_, heading_, !folded_);
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

void pillStyle(QToolButton* button) {
    button->setProperty("role", "pill");
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

QToolButton* makePill(const QString& text, const QString& tip, QWidget* parent) {
    QToolButton* pill = makeChip(text, tip, parent);
    pillStyle(pill);
    return pill;
}

QWidget* pillRow(QWidget* parent, FlowLayout** flow, const QString& label) {
    QWidget* row = chipRow(parent, flow, label);
    (*flow)->setNatural(true);
    (*flow)->setGap(6);
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

// ---- List rows -------------------------------------------------------------------------------------

PopupPanel::PopupPanel(QWidget* parent, int width) : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint) {
    setProperty("role", "popupPanel");
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedWidth(width);
    // A card on the panel (the menus' look: the field's fill, the popups' edge).
    setStyleSheet(QStringLiteral("QFrame[role=\"popupPanel\"] { background: %1; border: 1px solid %2; border-radius: %3px; }")
                      .arg(theme::uiColor("field").isValid() ? theme::uiColor("field").name() : theme::card().name(),
                           theme::uiColor("popupEdge").isValid() ? theme::uiColor("popupEdge").name() : theme::divider().name())
                      .arg(theme::cardRadius()));
    rows_ = new QVBoxLayout(this);
    rows_->setContentsMargins(8, 8, 8, 8);
    rows_->setSpacing(6);
}

void PopupPanel::add(QWidget* widget) { rows_->addWidget(widget); }

void PopupPanel::add(QLayout* layout) { rows_->addLayout(layout); }

void PopupPanel::showUnder(QWidget* anchor) {
    for (const auto& fill : fills_) fill();
    adjustSize();
    // As tall as its rows at its own (fixed) width: a row of pills wraps there (adjustSize asks
    // the height for the rows' one-line width).
    if (rows_->hasHeightForWidth()) resize(width(), rows_->totalHeightForWidth(width()));
    QPoint at = anchor->mapToGlobal(QPoint(anchor->width() - width(), anchor->height() + 2));
    // No room on the left of the anchor inside its window (a header button near the dock's edge):
    // the popup's left edge at the window's instead.
    if (QWidget* window = anchor->window()) at.setX(std::max(at.x(), window->mapToGlobal(QPoint(0, 0)).x() + 4));
    if (QScreen* screen = anchor->screen()) {
        const QRect room = screen->availableGeometry();
        at.setX(std::clamp(at.x(), room.left(), std::max(room.left(), room.right() - width())));
        if (at.y() + height() > room.bottom()) at.setY(anchor->mapToGlobal(QPoint(0, 0)).y() - height() - 2);
    }
    move(at);
    show();
}

QToolButton* rowGlyph(const QString& icon, const QString& tip, QWidget* parent, const QString& text, bool on) {
    auto* b = new QToolButton(parent);
    b->setProperty("role", "glyph");
    b->setAutoRaise(true);
    b->setToolTip(tip);
    b->setIconSize(QSize(14, 14));
    const qreal dpr = parent ? parent->devicePixelRatioF() : 1.0;
    if (on) {
        QIcon accent(tintedIcon(QStringLiteral(":/groomlab/icons/ui_%1.svg").arg(icon), theme::accentInk(), 14, dpr));
        b->setIcon(accent);
    } else {
        b->setIcon(lineIcon(icon, dpr, 14));
    }
    if (!text.isEmpty()) {
        b->setText(text);
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setStyleSheet(QStringLiteral("QToolButton { color: %1; font-size: 11px; padding: 0 2px; }").arg(theme::textMuted().name()));
        b->setFixedHeight(22);
        b->setMinimumWidth(22);
    } else {
        b->setFixedSize(22, 22);
    }
    return b;
}

QToolButton* popupButton(const QString& icon, const QString& tip, PopupPanel* panel, std::function<void()> before, QWidget* parent,
                         const QString& text) {
    QToolButton* b = rowGlyph(icon, tip, parent, text);
    QObject::connect(b, &QToolButton::clicked, b, [b, panel, before] {
        if (before) before();
        if (panel) panel->showUnder(b);
    });
    return b;
}

QToolButton* rowMenuButton(const QString& tip, std::function<void(QMenu*)> fill, std::function<void()> before, QWidget* parent) {
    QToolButton* b = rowGlyph(QStringLiteral("more"), tip, parent);
    auto* menu = new QMenu(b);
    menu->setToolTipsVisible(true);
    QObject::connect(menu, &QMenu::aboutToShow, menu, [menu, fill, before] {
        if (before) before();
        for (QMenu* sub : menu->findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly)) sub->deleteLater();
        menu->clear();
        if (fill) fill(menu);
    });
    b->setMenu(menu);
    b->setPopupMode(QToolButton::InstantPopup);
    b->setStyleSheet(QStringLiteral("QToolButton::menu-indicator { image: none; width: 0px; }"));
    return b;
}

QWidget* rowStrip(const std::vector<QWidget*>& icons, QWidget* parent) {
    auto* strip = new QWidget(parent);
    strip->setObjectName(QStringLiteral("rowStrip"));
    auto* row = new QHBoxLayout(strip);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    for (QWidget* w : icons) {
        if (!w) continue;
        w->setParent(strip);
        row->addWidget(w);
    }
    strip->setAttribute(Qt::WA_TranslucentBackground, true);
    return strip;
}

ListRow::ListRow(const QString& text, const QString& tag, const std::vector<QWidget*>& icons, QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("listRow"));
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(6, 0, 0, 0);
    row->setSpacing(4);
    label_ = new QLabel(text, this);
    label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    label_->setToolTip(text);
    row->addWidget(label_, 1);
    tag_ = new QLabel(tag, this);
    tag_->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(theme::textMuted().name()));
    tag_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    tag_->setVisible(!tag.isEmpty());
    row->addWidget(tag_);
    row->addWidget(rowStrip(icons, this));
    setAttribute(Qt::WA_TranslucentBackground, true);
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
