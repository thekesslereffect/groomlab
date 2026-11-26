// SPDX-License-Identifier: Apache-2.0
#include "groom_tools.h"

#include <algorithm>
#include <functional>
#include <memory>

#include <QRegularExpression>
#include <QAbstractButton>
#include <QAction>
#include <QAbstractItemView>
#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVariant>
#include <QVBoxLayout>

#include "groomlab/brush_catalog.h"

#include "brush_panel.h"
#include "brush_session.h"
#include "correctives_panel.h"
#include "groom_document.h"
#include "guides_panel.h"
#include "maps_panel.h"
#include "glass.h"
#include "theme.h"
#include "tube_panel.h"

namespace groomlab::app {

namespace {

const ui::Definitions& defs() { return ui::Definitions::shipped(); }

QString qs(const std::string& s) { return QString::fromStdString(s); }

// At most this many tiles to a row, however wide the dock.
constexpr int kMaxTileColumns = 5;

// Scroll bar `bar` to `value` and keep it there for `followMs` while what was just shown is laid
// out: the card's lists and blocks fill in over a few events, and the range only grows to `value`
// as they do. Moved meanwhile by anything but the user (the focus passed on down the panel, a
// layout): back. `still()` false (the content changed again) or the user's own scrolling (wheel,
// drag, keys: an action of the bar's) stops it. Returns whether it is still being followed (where
// the bar is meant to be is then `value`, not where it is on the way).
std::function<bool()> scrollWhenLaidOut(QScrollBar* bar, int value, std::function<bool()> still, int followMs) {
    auto following = std::make_shared<bool>(true);
    std::function<bool()> pending = [following] { return *following; };
    if (value <= 0 && followMs <= 0) {
        bar->setValue(bar->minimum());  // (the top: there whatever is laid out)
        *following = false;
        return pending;
    }
    auto connections = std::make_shared<std::vector<QMetaObject::Connection>>();
    auto done = [following, connections] {
        if (!*following) return;
        *following = false;
        for (const QMetaObject::Connection& c : *connections) QObject::disconnect(c);
    };
    auto apply = [bar, value, still, following, done] {
        if (!*following) return;
        if (still && !still()) {
            done();
            return;
        }
        bar->setValue(std::min(value, bar->maximum()));
    };
    connections->push_back(QObject::connect(bar, &QScrollBar::rangeChanged, bar, [apply](int, int) { apply(); }));
    connections->push_back(QObject::connect(bar, &QScrollBar::valueChanged, bar, [apply](int) { apply(); }));
    connections->push_back(QObject::connect(bar, &QScrollBar::actionTriggered, bar, [done](int) { done(); }));
    apply();
    QTimer::singleShot(followMs, bar, [apply, done] {
        apply();
        done();
    });
    return pending;
}

constexpr int kScrollFollowMs = 1500;

QString textAt(const std::string& path) {
    const ui::json* t = defs().find(path);
    return t && t->is_string() ? qs(t->get<std::string>()) : QString();
}

// A tile's tip: the tool's name and the catalog's whole hint, then (a brush's) the gestures
// (palette texts.gestures, the app's words) as a small list, each "keys: what".
QString tileTip(const groomlab::BrushEntry& e) {
    QString html = QStringLiteral("<p style='margin:0'><b>%1</b></p><p style='margin:4px 0 0 0'>%2</p>").arg(qs(e.label).toHtmlEscaped(), qs(e.hint).toHtmlEscaped());
    if (!e.hasSetting("radius")) return html;
    QString gestures = textAt(std::string("palette/texts/labels/") + ui::kHost + "/gestures");
    if (gestures.isEmpty()) gestures = textAt("palette/texts/gestures");
    html += QStringLiteral("<table style='margin-top:6px' cellspacing='0' cellpadding='1'>");
    for (const QString& part : gestures.split(QStringLiteral(" · "), Qt::SkipEmptyParts)) {
        const int colon = part.indexOf(QStringLiteral(": "));
        const QString keys = colon > 0 ? part.left(colon) : part, what = colon > 0 ? part.mid(colon + 2) : QString();
        html += QStringLiteral("<tr><td style='padding-right:8px; white-space:nowrap'><b>%1</b></td><td>%2</td></tr>")
                    .arg(keys.trimmed().toHtmlEscaped(), what.trimmed().toHtmlEscaped());
    }
    return html + QStringLiteral("</table>");
}

QLabel* label(const QString& text, const char* role, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setProperty("role", role);
    if (QLatin1String(role) == QLatin1String("hint") || QLatin1String(role) == QLatin1String("about")) l->setWordWrap(true);
    return l;
}

QString iconPath(const std::string& stem) { return QStringLiteral(":/groomlab/icons/groomLab_%1.svg").arg(qs(stem)); }
QString uiIcon(const char* name) { return QStringLiteral(":/groomlab/icons/ui_%1.svg").arg(QLatin1String(name)); }

// An icon tinted `rest` (and `on` while checked), dim when disabled.
QIcon tinted(const QString& resource, const QColor& rest, const QColor& on, int size, qreal dpr) {
    QIcon icon;
    icon.addPixmap(tintedIcon(resource, rest, size, dpr), QIcon::Normal, QIcon::Off);
    icon.addPixmap(tintedIcon(resource, on, size, dpr), QIcon::Normal, QIcon::On);
    icon.addPixmap(tintedIcon(resource, on, size, dpr), QIcon::Active, QIcon::On);
    icon.addPixmap(tintedIcon(resource, theme::textMuted(), size, dpr), QIcon::Disabled, QIcon::Off);
    icon.addPixmap(tintedIcon(resource, theme::textMuted(), size, dpr), QIcon::Disabled, QIcon::On);
    return icon;
}

QToolButton* glyphButton(const char* icon, const QString& tip, QWidget* parent) {
    auto* b = new QToolButton(parent);
    b->setProperty("role", "glyph");
    b->setAutoRaise(true);
    b->setToolTip(tip);
    b->setIconSize(QSize(16, 16));
    b->setIcon(tinted(uiIcon(icon), theme::textDim(), theme::text(), 16, parent ? parent->devicePixelRatioF() : 1.0));
    b->setFixedSize(26, 26);
    return b;
}

// The dock's menus run the menu bar's actions: their QActions take another name, so the
// window's actions (found by name: tests, scripts) stay the menu bar's own. Kept so as the
// menus fill again when they open (a generator's entries).
void renameActions(QMenu* menu, const QString& prefix) {
    for (QAction* a : menu->findChildren<QAction*>()) {
        const QString name = a->objectName();
        if (!name.isEmpty() && !name.startsWith(prefix)) a->setObjectName(prefix + name);
    }
    for (QMenu* sub : menu->findChildren<QMenu*>()) {
        const QString name = sub->objectName();
        if (!name.isEmpty() && !name.startsWith(prefix)) sub->setObjectName(prefix + name);
        if (sub->property("renames").toBool()) continue;
        sub->setProperty("renames", true);
        QObject::connect(sub, &QMenu::aboutToShow, sub, [sub, prefix] { renameActions(sub, prefix); });
    }
}

void keepRenamed(QMenu* menu, const QString& prefix = QStringLiteral("groomTools_")) {
    renameActions(menu, prefix);
    if (menu->property("renames").toBool()) return;
    menu->setProperty("renames", true);
    QObject::connect(menu, &QMenu::aboutToShow, menu, [menu, prefix] { renameActions(menu, prefix); });
}

// A hosted panel's own scroll area kept as tall as its content (the dock scrolls as one column):
// recomputed when the content's layout changes or the area's width does.
class ScrollFitter : public QObject {
public:
    explicit ScrollFitter(QScrollArea* area) : QObject(area), area_(area) {
        area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        area->installEventFilter(this);
        if (area->widget()) area->widget()->installEventFilter(this);
        fit();
    }
    bool eventFilter(QObject* watched, QEvent* event) override {
        const QEvent::Type t = event->type();
        if (t == QEvent::LayoutRequest || t == QEvent::Resize || t == QEvent::Show) fit();
        return QObject::eventFilter(watched, event);
    }

private:
    void fit() {
        QWidget* content = area_->widget();
        if (!content || fitting_) return;
        fitting_ = true;
        const int width = std::max(area_->viewport()->width(), 1);
        int h = content->hasHeightForWidth() ? content->heightForWidth(width) : -1;
        if (h <= 0) h = content->sizeHint().height();
        h += 2 * area_->frameWidth();
        if (area_->minimumHeight() != h || area_->maximumHeight() != h) area_->setFixedHeight(h);
        fitting_ = false;
    }
    QScrollArea* area_;
    bool fitting_ = false;
};

}  // namespace

// ---- ToolGrid ----------------------------------------------------------------------------------------

ToolGrid::ToolGrid(QWidget* parent) : QWidget(parent) {
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
}

void ToolGrid::setButtons(std::vector<QAbstractButton*> buttons, int maxColumns) {
    all_ = std::move(buttons);
    buttons_ = all_;
    for (QAbstractButton* b : all_) b->setParent(this);
    maxColumns_ = std::max(1, maxColumns);
    place();
    updateGeometry();
}

void ToolGrid::setShown(const std::function<bool(QAbstractButton*)>& keep) {
    std::vector<QAbstractButton*> shown;
    for (QAbstractButton* b : all_) {
        if (keep(b)) shown.push_back(b);
    }
    if (shown == buttons_) return;
    buttons_ = std::move(shown);
    for (QAbstractButton* b : all_) b->setVisible(std::find(buttons_.begin(), buttons_.end(), b) != buttons_.end());
    place();
    updateGeometry();
}

void ToolGrid::setMaxColumns(int maxColumns) {
    maxColumns_ = std::max(1, maxColumns);
    place();
    updateGeometry();
}

QSize ToolGrid::tile() const {
    QSize t(1, 1);
    for (const QAbstractButton* b : buttons_) t = t.expandedTo(b->minimumSize());
    return t;
}

int ToolGrid::columnsFor(int width) const {
    const int fit = (width + spacing_) / std::max(1, tile().width() + spacing_);
    return std::clamp(fit, 1, maxColumns_);
}

int ToolGrid::heightForWidth(int width) const {
    if (buttons_.empty()) return 0;
    const int columns = columnsFor(width);
    const int rows = (static_cast<int>(buttons_.size()) + columns - 1) / columns;
    return rows * tile().height() + (rows - 1) * spacing_;
}

QSize ToolGrid::sizeHint() const {
    const QSize t = tile();
    const int columns = std::min<int>(6, std::max<int>(1, static_cast<int>(buttons_.size())));
    const int width = columns * t.width() + (columns - 1) * spacing_;
    return {width, heightForWidth(width)};
}

// (The grid's height follows its width, heightForWidth; at least a tile wide, as tall as at
// its preferred width: a narrow dock's extra rows do not make the window's minimum taller.)
QSize ToolGrid::minimumSizeHint() const { return {tile().width(), heightForWidth(sizeHint().width())}; }

void ToolGrid::place() {
    if (buttons_.empty()) return;
    // The width says how many fit in a row (at the tiles' minimum); the row's width is then split
    // evenly between them, the leftover pixels spread so every row ends at the grid's edge.
    const QSize t = tile();
    const int across = std::max(width(), t.width());
    const int columns = columnsFor(across);
    for (std::size_t i = 0; i < buttons_.size(); ++i) {
        const int row = static_cast<int>(i) / columns, column = static_cast<int>(i) % columns;
        const int left = column * (across + spacing_) / columns, right = (column + 1) * (across + spacing_) / columns - spacing_;
        buttons_[i]->setGeometry(left, row * (t.height() + spacing_), std::max(right - left, t.width()), t.height());
    }
}

void ToolGrid::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (event->oldSize().width() != event->size().width() &&
        columnsFor(event->oldSize().width()) != columnsFor(event->size().width())) {
        updateGeometry();  // another row count: the height follows
    }
    place();
}

bool ToolGrid::event(QEvent* event) {
    if (event->type() == QEvent::LayoutRequest) place();
    return QWidget::event(event);
}

// ---- GroomTools ---------------------------------------------------------------------------------------

GroomTools::GroomTools(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("groomTools"));
    const ui::json& palette = defs().get("palette");
    auto* frame = new QVBoxLayout(this);
    frame->setContentsMargins(0, 0, 0, 0);
    frame->setSpacing(0);

    // One scroll area for the rest of the panel (the header pinned over it, below): the status,
    // the tabs and the tool grid on top, then the empty state, the tab's list and the card.
    scroll_ = new QScrollArea(this);
    scroll_->setObjectName(QStringLiteral("groomToolsScroll"));
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Only when the content is taller than the dock: an empty bar read as a second border.
    scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    frame->addWidget(scroll_, 1);
    auto* content = new QWidget(scroll_);
    content->setObjectName(QStringLiteral("groomToolsContent"));
    scroll_->setWidget(content);
    auto* outer = new QVBoxLayout(content);
    outer->setContentsMargins(12, 6, 12, 12);  // the cards 12 px in, as the Properties dock's (6 under the pinned header)
    outer->setSpacing(8);

    // ---- The top: the header, the status, the tabs and the tool grid.
    top_ = new QWidget(content);
    top_->setObjectName(QStringLiteral("groomToolsTop"));
    auto* top = new QVBoxLayout(top_);
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(6);
    outer->addWidget(top_);
    // The header: the groom picker, its eyes (the guides' a split button: the eye, and the arrow
    // opening how they draw), the Graph Editor, Find Command, More. Pinned over the scroll area
    // (issue 73: in view however far the panel scrolls), the pages' tabs under it (the Simple
    // layout, research/ui-redesign/SIMPLE.md: what you work on first, then how).
    const ui::json& header = palette.at("header");
    header_ = new QWidget(this);
    header_->setObjectName(QStringLiteral("groomToolsHeader"));
    auto* head = new QHBoxLayout(header_);
    head->setContentsMargins(12, 10, 12, 0);  // (the column's gutter, as the content's)
    head->setSpacing(2);
    frame->insertWidget(0, header_);
    // The groom picker: the open groom's name, its menu the document's grooms (the Outliner's).
    groomPicker_ = new QToolButton(header_);
    groomPicker_->setObjectName(QStringLiteral("groomName"));
    groomPicker_->setProperty("role", "title");
    groomPicker_->setAutoRaise(true);
    groomPicker_->setPopupMode(QToolButton::InstantPopup);
    groomPicker_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    groomPicker_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    groomPicker_->setStyleSheet(QStringLiteral("QToolButton { color: %1; font-size: 14px; font-weight: 600; text-align: left; padding: 2px 4px; "
                                               "border: none; border-radius: %2px; }"
                                               "QToolButton:hover { background: %3; }"
                                               "QToolButton::menu-indicator { image: none; width: 0px; }")
                                    .arg(theme::text().name())
                                    .arg(theme::smallRadius())
                                    .arg(theme::chip().name(QColor::HexArgb)));
    groomMenu_ = new QMenu(groomPicker_);
    groomMenu_->setObjectName(QStringLiteral("groomPickerMenu"));
    connect(groomMenu_, &QMenu::aboutToShow, this, [this] {
        groomMenu_->clear();
        for (int i = 0; i < static_cast<int>(grooms_.size()); ++i) {
            QAction* a = groomMenu_->addAction(grooms_[static_cast<std::size_t>(i)]);
            a->setCheckable(true);
            a->setChecked(i == groomIndex_);
            connect(a, &QAction::triggered, this, [this, i] {
                if (i != groomIndex_) emit groomPicked(i);
            });
        }
        if (grooms_.empty()) groomMenu_->addAction(tr("No groom open"))->setEnabled(false);
    });
    groomPicker_->setMenu(groomMenu_);
    head->addWidget(groomPicker_, 1);
    // The hair's eye: the strands alone (instances, cards too); the guides are the next eye's.
    eye_ = glyphButton("eye", tr("Show or hide the hair"), header_);
    eye_->setObjectName(QStringLiteral("hairVisible"));
    eye_->setCheckable(true);
    eye_->setChecked(true);
    connect(eye_, &QToolButton::toggled, this, [this](bool on) {
        eye_->setToolTip(on ? tr("Show or hide the hair") : tr("The hair is hidden: click to show it"));
        if (!updating_) emit hairVisibilityToggled(on);
    });
    head->addWidget(eye_);
    // The guides' eye: every guide group at once (Properties > Guides; Maya's displayGuides).
    // (lucide "spline": a curve between two points; hidden: the same at half strength, struck through.)
    guidesEye_ = glyphButton("guides", tr("Show or hide the guides, every guide group"), header_);
    guidesEye_->setObjectName(QStringLiteral("guidesVisible"));
    guidesEye_->setCheckable(true);
    {
        const qreal dpr = devicePixelRatioF();
        QIcon icon;
        icon.addPixmap(tintedIcon(uiIcon("guides"), theme::text(), 16, dpr), QIcon::Normal, QIcon::On);
        icon.addPixmap(tintedIcon(uiIcon("guides"), theme::text(), 16, dpr), QIcon::Active, QIcon::On);
        icon.addPixmap(tintedIcon(uiIcon("guides_off"), theme::textDim(), 16, dpr), QIcon::Normal, QIcon::Off);
        icon.addPixmap(tintedIcon(uiIcon("guides_off"), theme::text(), 16, dpr), QIcon::Active, QIcon::Off);
        guidesEye_->setIcon(icon);
    }
    guidesEye_->setChecked(true);
    connect(guidesEye_, &QToolButton::toggled, this, [this](bool on) {
        guidesEye_->setToolTip(on ? tr("Show or hide the guides, every guide group") : tr("The guides are hidden: click to show them"));
        if (!updating_) emit guidesVisibilityToggled(on);
    });
    head->addWidget(guidesEye_);
    // The arrow beside it: how the guides draw (Flow, Tubes, Instances, Smoothed, Width, Points,
    // Colors, then the tubes' display): the window fills the popup (guideDisplayPopup).
    guideDisplay_ = new PopupPanel(this, 270);
    guideDisplay_->setObjectName(QStringLiteral("guideDisplayPopup"));
    guidesDisplay_ = glyphButton("chevron_down", tr("How the guides draw: flow, tubes, instances, width, points, colors"), header_);
    guidesDisplay_->setObjectName(QStringLiteral("guidesDisplay"));
    guidesDisplay_->setFixedSize(14, 26);
    guidesDisplay_->setIconSize(QSize(10, 10));
    connect(guidesDisplay_, &QToolButton::clicked, this, [this] { guideDisplay_->showUnder(guidesDisplay_); });
    head->addWidget(guidesDisplay_);
    // While an AI agent is connected through the agent link: a chip saying so and its Stop.
    agentBadge_ = new QWidget(header_);
    agentBadge_->setObjectName(QStringLiteral("agentBadge"));
    {
        auto* row = new QHBoxLayout(agentBadge_);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(2);
        auto* pill = new QToolButton(agentBadge_);
        pill->setObjectName(QStringLiteral("agentBadgePill"));
        pill->setProperty("role", "chip");
        pill->setText(tr("Agent"));
        pill->setToolTip(tr("An AI agent is grooming through the agent link: each of its changes is one undo step"));
        pill->setCheckable(true);
        pill->setChecked(true);
        pill->setAttribute(Qt::WA_TransparentForMouseEvents);
        row->addWidget(pill);
        auto* stop = glyphButton("close", tr("Stop the agent link: the agent is disconnected and its link file removed"), agentBadge_);
        stop->setObjectName(QStringLiteral("agentStop"));
        connect(stop, &QToolButton::clicked, this, &GroomTools::agentStopRequested);
        row->addWidget(stop);
    }
    agentBadge_->setVisible(false);
    head->addWidget(agentBadge_);
    graph_ = glyphButton("graph", withShortcut(qs(header.at("graph").value("tip", std::string())), "view.graph"), header_);
    graph_->setObjectName(QStringLiteral("openGraph"));
    connect(graph_, &QToolButton::clicked, this, &GroomTools::graphRequested);
    head->addWidget(graph_);
    // Find Command: every command, brush and graph preset by name (Ctrl+K).
    find_ = glyphButton("search", qs(defs().action("commands.find").value("tip", std::string())) + tr(" (Ctrl+K)"), header_);
    find_->setObjectName(QStringLiteral("findCommand"));
    connect(find_, &QToolButton::clicked, this, &GroomTools::findRequested);
    head->addWidget(find_);
    more_ = glyphButton("more", qs(header.at("more").value("tip", std::string())), header_);
    more_->setObjectName(QStringLiteral("groomToolsMore"));
    more_->setPopupMode(QToolButton::InstantPopup);
    moreMenu_ = new QMenu(more_);
    moreMenu_->setObjectName(QStringLiteral("groomToolsMoreMenu"));
    more_->setMenu(moreMenu_);
    head->addWidget(more_);

    status_ = label(QString(), "status", top_);
    status_->setObjectName(QStringLiteral("groomToolsStatus"));
    status_->setWordWrap(true);
    status_->setVisible(false);
    top->addWidget(status_);

    // The pages (palette.tabs) as labeled tabs on one track (Tools · Paint · Photos, the app's
    // Outliner), pinned under the header.
    auto* pages = new QWidget(this);
    pages->setObjectName(QStringLiteral("groomPages"));
    auto* pagesRow = new QHBoxLayout(pages);
    pagesRow->setContentsMargins(12, 6, 12, 4);
    pagesRow->setSpacing(0);
    tabs_ = new QTabBar(pages);
    tabs_->setObjectName(QStringLiteral("groomTabs"));
    tabs_->setUsesScrollButtons(false);
    tabs_->setDrawBase(false);
    tabs_->setDocumentMode(true);
    tabs_->setProperty("role", "pills");
    tabs_->setExpanding(true);  // the track the panel's width, the tabs sharing it
    tabs_->setElideMode(Qt::ElideRight);
    pagesRow->addWidget(tabs_, 1);
    frame->insertWidget(1, pages);
    pages_ = pages;

    // The tool grid: the tiles by category (Paint's on the Paint tab), Unfreeze All beside Freeze.
    auto tileGrid = [this](BrushCategory category, QWidget* parent) {
        std::vector<QAbstractButton*> buttons;
        for (const groomlab::BrushEntry* e : brushesInCategory(category)) {
            // A tile's word fits its fixed width at the UI's text size: Length Keys shows as Keys
            // (its tooltip and the card say the full name).
            const QString word = e->id == "lengths" ? tr("Keys") : qs(e->label);
            buttons.push_back(makeTile(e->id, word, tileTip(*e)));
            if (e->id == "freeze") {
                // Invert Frozen, then Unfreeze All: buttons, not tools.
                auto* inv = new QToolButton(parent);
                inv->setObjectName(QStringLiteral("invertFrozenTile"));
                inv->setProperty("role", "tool");
                inv->setText(tr("Invert"));  // (its icon: refreshTiles)
                inv->setToolTip(tr("<b>Invert Frozen</b><br>Swap the frozen guides and the free ones: the frozen can be brushed, "
                                   "the rest freeze"));
                connect(inv, &QToolButton::clicked, this, &GroomTools::invertFrozenRequested);
                allTiles_.push_back(inv);
                buttons.push_back(inv);
                auto* b = new QToolButton(parent);
                b->setObjectName(QStringLiteral("unfreezeAllTile"));
                b->setProperty("role", "tool");
                b->setText(tr("Unfreeze"));  // (its icon: refreshTiles; the tooltip says it frees every guide)
                b->setToolTip(tr("<b>Unfreeze All</b><br>Clear the freeze mask: every guide can be brushed again (a click with the "
                                 "Select tool where no guide is does the same)"));
                connect(b, &QToolButton::clicked, this, &GroomTools::unfreezeAllRequested);
                allTiles_.push_back(b);
                buttons.push_back(b);
            }
        }
        auto* grid = new ToolGrid(parent);
        grid->setButtons(std::move(buttons), kMaxTileColumns);
        grids_.push_back(grid);
        return grid;
    };
    toolsGrid_ = new QWidget(top_);
    toolsGrid_->setObjectName(QStringLiteral("toolsGrid"));
    auto* tools = new QVBoxLayout(toolsGrid_);
    tools->setContentsMargins(0, 0, 0, 0);
    tools->setSpacing(8);
    for (BrushCategory category : brushCategories()) {
        if (category == BrushCategory::Paint) continue;  // beside the maps they paint
        // Each category a tray of its tiles, without its name (the tray's tooltip says it: the
        // tiles speak for it once learnt).
        auto* tray = new QFrame(toolsGrid_);
        tray->setObjectName(QStringLiteral("tray_") + QString::fromLatin1(brushCategoryName(category)).toLower());
        tray->setProperty("role", "tray");
        tray->setToolTip(QString::fromLatin1(brushCategoryName(category)));
        auto* trayLayout = new QVBoxLayout(tray);
        trayLayout->setContentsMargins(4, 4, 4, 4);
        trayLayout->setSpacing(4);
        QLabel* name = label(QString::fromLatin1(brushCategoryName(category)), "category", tray);
        name->hide();
        gridLabels_.push_back(name);
        trayLayout->addWidget(tileGrid(category, tray));
        tools->addWidget(tray);
    }
    top->addWidget(toolsGrid_);
    paintGrid_ = new QWidget(top_);
    paintGrid_->setObjectName(QStringLiteral("paintGrid"));
    paintGrid_->setProperty("role", "tray");  // a tray, as the Tools tab's
    paintGrid_->setAttribute(Qt::WA_StyledBackground, true);
    auto* paintTiles = new QVBoxLayout(paintGrid_);
    paintTiles->setContentsMargins(8, 8, 8, 8);
    // The Paint tab is painting (no tile: entering it starts Paint on the picked map); its tile
    // is kept, hidden, for the window's and the tests' tile("paint").
    for (const groomlab::BrushEntry* e : brushesInCategory(BrushCategory::Paint)) {
        QToolButton* b = makeTile(e->id, qs(e->label), tileTip(*e));
        b->setProperty("wide", true);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        paintTiles->addWidget(b);
    }
    top->addWidget(paintGrid_);
    paintGrid_->hide();

    // ---- Under the top: the empty state, the tab's list, the card.
    // No groom: one card saying what is needed (a mesh file: the viewport's start card's New Groom
    // on Mesh), the other ways to start under it (palette.empty: the samples, an import).
    empty_ = new QFrame(content);
    empty_->setObjectName(QStringLiteral("groomToolsEmpty"));
    empty_->setProperty("role", "tray");
    empty_->setAttribute(Qt::WA_StyledBackground, true);
    emptyItems_ = new QVBoxLayout(empty_);
    emptyItems_->setContentsMargins(12, 12, 12, 12);
    emptyItems_->setSpacing(8);
    emptyItems_->addWidget(label(tr("No groom yet"), "subheading", empty_));
    emptyItems_->addWidget(label(tr("A groom grows on a scalp mesh: pick its .obj or .abc file (New Groom on Mesh, in the viewport's start "
                                    "card), or start from a sample."),
                                 "about", empty_));
    outer->addWidget(empty_);

    work_ = new QWidget(content);
    work_->setObjectName(QStringLiteral("groomToolsWork"));
    auto* work = new QVBoxLayout(work_);
    work->setContentsMargins(0, 0, 0, 0);
    work->setSpacing(6);
    outer->addWidget(work_);

    // The tabs' lists, right under the grid (the window fills them: setGroupsContent...).
    for (const ui::json& tab : palette.at("tabs")) {
        const std::string id = tab.value("id", std::string());
        auto* holder = new QWidget(work_);
        holder->setObjectName(QStringLiteral("list_") + qs(id));
        holder->setLayout(new QVBoxLayout());
        // Each tab's list on a card (its header inside), as the Properties dock's sections.
        holder->setProperty("role", "tray");
        holder->setAttribute(Qt::WA_StyledBackground, true);
        holder->layout()->setContentsMargins(10, 4, 10, 10);
        work->addWidget(holder);
        lists_[id] = holder;
        tabIds_.push_back(id);
        const int index = tabs_->addTab(qs(tab.value("label", std::string())));
        tabs_->setTabToolTip(index, qs(tab.value("tip", std::string())));
        tabs_->setTabData(index, qs(id));
        // A tab's other sections (palette.sections: Correctives on Tools), each its own card under it.
        for (const ui::json& section : palette.at("sections")) {
            const std::string sid = section.value("id", std::string());
            const ui::json shown = section.value("tabs", ui::json::array());
            if (sid == "groups" || std::find(shown.begin(), shown.end(), id) == shown.end()) continue;
            auto* card = new QWidget(work_);
            card->setObjectName(QStringLiteral("list_") + qs(sid));
            card->setLayout(new QVBoxLayout());
            card->setProperty("role", "tray");
            card->setAttribute(Qt::WA_StyledBackground, true);
            card->layout()->setContentsMargins(10, 4, 10, 10);
            work->addWidget(card);
            lists_[sid] = card;
            sectionTabs_[sid] = id;
        }
    }

    // Over the list: the tool's card (what the picked tool does, right under its tile), the
    // tubes' blocks inside it; the tab's list (guide groups, maps) below it.
    card_ = new BrushPanel(work_);
    connect(card_, &BrushPanel::settingsChanged, this, &GroomTools::settingsChanged);
    connect(card_, &BrushPanel::guidesEdited, this, &GroomTools::guidesEdited);
    connect(card_, &BrushPanel::clearFlowRequested, this, &GroomTools::clearFlowRequested);
    connect(card_, &BrushPanel::lengthsPreviewed, this, &GroomTools::lengthsPreviewed);
    work->insertWidget(0, card_);  // (the tool's hint goes over it below)
    tubes_ = new TubePanel();
    connect(tubes_, &TubePanel::guidesEdited, this, &GroomTools::guidesEdited);
    connect(tubes_, &TubePanel::previewed, this, &GroomTools::tubesPreviewed);
    connect(tubes_, &TubePanel::settingsChanged, this, [this] {
        if (!updating_) emit settingsChanged();
    });
    connect(tubes_, &TubePanel::message, this, [this](const QString& text) { setStatus(text); });
    connect(tubes_, &TubePanel::displayChanged, this, &GroomTools::settingsChanged);  // (the brush's tubes drawn again)
    connect(tubes_, &TubePanel::rootModeChanged, this, &GroomTools::rootModeChanged);
    connect(tubes_, &TubePanel::tubeViewChanged, this, [this](const QString& group) {
        // (The pin shows on the list's level rows and in the card's header.)
        if (guides_) guides_->refresh();
        if (card_->hasSettings()) card_->setEditing(editingText(card_->tool()));
        emit tubeViewChanged(group);
    });
    card_->setTubes(tubes_);
    // The guides' display popup: the window's rows (Flow, Tubes, Instances, Smoothed, Width,
    // Points, Colors: guideDisplayRows), then the tubes' display (Maya's Tube card's Visibility).
    {
        auto* rows = new QWidget(guideDisplay_);
        rows->setObjectName(QStringLiteral("guideDisplayRows"));
        guideRows_ = new QVBoxLayout(rows);
        guideRows_->setContentsMargins(0, 0, 0, 0);
        guideRows_->setSpacing(6);
        guideDisplay_->add(rows);
        guideDisplay_->add(label(tr("Tubes"), "listTitle", guideDisplay_));
        tubes_->displayBlock()->setParent(guideDisplay_);
        guideDisplay_->add(tubes_->displayBlock());
        tubes_->displayBlock()->show();
    }
    noTool_ = label(tr("Pick a tool, then drag over the groom in the viewport."), "hint", work_);
    noTool_->setObjectName(QStringLiteral("noTool"));
    if (!theme::glassy()) {
        // A compact card in the tool card's place (no text outside a card).
        noTool_->setAttribute(Qt::WA_StyledBackground, true);
        noTool_->setStyleSheet(QStringLiteral("background: %1; border-radius: %2px; padding: 10px 12px; color: %3;")
                                   .arg(theme::card().name()).arg(theme::cardRadius()).arg(theme::textDim().name()));
    } else {
        noTool_->setContentsMargins(0, 4, 0, 2);
    }
    noTool_->setWordWrap(true);
    work->insertWidget(0, noTool_);

    connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
        refreshVisibility();
        if (index < 0 || index >= static_cast<int>(tabIds_.size())) return;
        const std::string tab = tabIds_[static_cast<std::size_t>(index)];
        emit tabChanged(qs(tab));
    });
    // Isolate: the picked group (and its child groups) at full color, the others dimmed (the eye
    // in the Guide groups header).
    isolate_ = makeGlyph(QStringLiteral("glyph_solo"), tr("Isolate: show the picked group (and its child groups) at full color and dim the others"),
                         this, true);
    isolate_->setObjectName(QStringLiteral("isolate"));
    connect(isolate_, &QToolButton::toggled, this, [this](bool) {
        if (!updating_) emit isolateChanged();
    });
    outer->addStretch(1);

    setTool({});
    refreshVisibility();
}

GroomTools::~GroomTools() {
    // The panels it hosts are its children, deleted by QWidget's destructor after this one has run:
    // their signals (a list cleared as it goes) must not reach the half-destroyed dock.
    for (QObject* sender : {static_cast<QObject*>(maps_ ? maps_->list() : nullptr), static_cast<QObject*>(maps_.data()),
                            static_cast<QObject*>(correctives_.data()), static_cast<QObject*>(guides_.data())}) {
        if (sender) QObject::disconnect(sender, nullptr, this, nullptr);
    }
}

QToolButton* GroomTools::makeTile(const std::string& id, const QString& text, const QString& tip) {
    auto* b = new QToolButton(this);
    b->setObjectName(QStringLiteral("tile_") + qs(id));
    b->setProperty("role", "tool");
    b->setProperty("tool", qs(id));
    b->setText(text);
    b->setCheckable(true);
    b->setToolTip(tip);
    connect(b, &QToolButton::clicked, this, [this, b, id] {
        b->setChecked(id == tool_);  // the window says which tool is on (setTool)
        setStatus({});
        emit toolChosen(qs(id));
    });
    tiles_[id].push_back(b);
    allTiles_.push_back(b);
    return b;
}

void GroomTools::setAgentConnected(bool connected) {
    if (agentBadge_) agentBadge_->setVisible(connected);
}

void GroomTools::setSession(BrushSession* session) {
    session_ = session;
    card_->setSession(session);
    tubes_->setSession(session);
    refresh();
}

void GroomTools::setBindings(const ui::Bindings& bindings, const ui::FillOptions& given) {
    ui::FillOptions options = given;
    options.liveShortcuts = false;  // the menu bar holds the keys; here they show as text
    enables_.clear();
    // ---- More.
    moreMenu_->clear();
    ui::fillMenu(moreMenu_, defs().get("palette/header/more"), bindings, options);
    keepRenamed(moreMenu_);
    // ---- The empty state's buttons (palette.empty), after its title and sentence.
    while (emptyItems_->count() > 2) {
        QLayoutItem* item = emptyItems_->takeAt(2);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
    ui::ResolveOptions resolve = options.resolve;
    resolve.state.reset();
    resolve.keep = [&bindings](const std::string& id) { return bindings.count(id) > 0; };
    resolve.dropGenerators = true;
    for (const ui::Item& item : defs().resolve(defs().get("palette/empty/items"), resolve)) {
        // One card: what is needed (the line above: a mesh file); the viewport's start card holds
        // New Groom on Mesh (and Open), so no second primary here: a sample mesh or an import.
        if (item.kind == ui::Item::Kind::Action && item.id == "groom.create") continue;
        if (item.kind == ui::Item::Kind::Action) {
            auto* b = new QPushButton(qs(item.label), empty_);
            b->setObjectName(QStringLiteral("empty_") + qs(item.id));
            b->setProperty("role", item.id == "groom.create" || theme::glassy() ? "primary" : "disclosure");
            b->setToolTip(qs(item.tip));
            const ui::Binding binding = bindings.at(item.id);
            connect(b, &QPushButton::clicked, this, [binding, value = item.value] { binding.trigger(value, false); });
            emptyItems_->addWidget(b);
        } else if (item.kind == ui::Item::Kind::Submenu) {
            QToolButton* chip = menuChip(item, bindings, options, empty_);
            if (!theme::glassy()) chip->setProperty("disclosure", true);
            emptyItems_->addWidget(chip);
        }
    }
    // ---- The Parts block.
    card_->setBindings(bindings);
    refresh();
}

QToolButton* GroomTools::menuButton(const std::string& ref, const QString& text, const QString& tip, const ui::Bindings& bindings,
                                    const ui::FillOptions& options, QWidget* parent, const QString& prefix) {
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(tip);
    button->setPopupMode(QToolButton::InstantPopup);
    auto* popup = new QMenu(button);
    if (!ref.empty() && defs().find(ref)) ui::fillMenu(popup, defs().get(ref), bindings, options);
    keepRenamed(popup, prefix);
    button->setMenu(popup);
    return button;
}

QToolButton* GroomTools::menuChip(const ui::Item& item, const ui::Bindings& bindings, const ui::FillOptions& options, QWidget* parent) {
    QToolButton* chip = menuButton(item.entry.value("ref", std::string()), qs(item.label) + QStringLiteral("  ▾"), qs(item.tip), bindings,
                                   options, parent);
    chip->setObjectName(QStringLiteral("output_") + qs(item.id));
    chip->setProperty("role", "chip");
    chip->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    chip->menu()->setObjectName(QStringLiteral("outputMenu_") + qs(item.id));
    return chip;
}

void GroomTools::setGroomName(const QString& name) {
    name_ = name;
    groomPicker_->setText((name.isEmpty() ? tr("No groom") : name) + QStringLiteral("  ▾"));
    groomPicker_->setToolTip(name.isEmpty() ? tr("No groom open")
                                            : tr("The groom every tool works on: %1 (click: the document's grooms)").arg(name));
    refreshVisibility();
    refresh();
}

QString GroomTools::groomName() const { return name_; }

void GroomTools::setGrooms(const QStringList& names, int current) {
    grooms_ = names;
    groomIndex_ = current;
}

void GroomTools::setGroups(const std::vector<GroupRow>& groups) {
    groups_ = groups;
    refresh();
}

void GroomTools::setParts(const std::vector<std::string>& names) { card_->setParts(names); }

void GroomTools::setFlowCount(std::size_t arrows, std::size_t sinks) { card_->setFlowCount(arrows, sinks); }

void GroomTools::setTool(const std::string& id) {
    tool_ = id;
    refreshTiles();
    refreshCard();
}

std::string GroomTools::currentGroup() const {
    if (!session_) return {};
    const BrushSettings& b = session_->settings().brush;
    if (!b.group.empty()) return b.group;
    if (b.level < 0) return {};
    for (const GroupRow& g : groups_) {
        if (g.depth == b.level) return g.name;
    }
    return {};
}

std::string GroomTools::isolatedGroup() const { return isolate_->isChecked() ? currentGroup() : std::string(); }

QAbstractButton* GroomTools::symmetryBox() const { return card_->symmetryBox(); }
QComboBox* GroomTools::symmetryMode() const { return card_->symmetryMode(); }

void GroomTools::groupPicked(const std::string& name) {
    // A row clicked: the group the brushes edit and new guides go into (Maya's pick_group).
    if (updating_ || !session_ || !session_->document()) return;
    const HierarchyPtr h = session_->document()->session().authoredHierarchy();
    const GroupId id = h ? h->findGroup(name) : kNoGroup;
    if (id == kNoGroup) return;
    BrushSettings& b = session_->settings().brush;
    b.group = name;
    b.level = h->groupDepth(id);
    refresh();  // (a level of a tube hierarchy: its level view, refresh's)
    emit settingsChanged();
    emit isolateChanged();
}

void GroomTools::refreshTiles() {
    const qreal dpr = devicePixelRatioF();
    const int iconSize = 28;
    // Two-tone artwork in the tool's category color (the theme's icons.<category>) at rest. Playful
    // (toolOn "category") keeps it when chosen; the other themes use the accent's ink. Greyed when
    // disabled (a tool that can't edit the picked corrective).
    const bool glass = theme::glassy();
    auto toolIcon = [&](const std::string& stem, BrushCategory category) {
        const QString resource = iconPath(stem);
        const QPixmap rest = duotoneIcon(resource, theme::toolIconColor(category, stem), theme::iconInk(), iconSize, dpr);
        const QColor onInk = glass ? theme::uiColor("accentText") : theme::toolOnCategory() ? theme::towardText(theme::toolIconColor(category, stem), 0.25) : theme::accentInk();
        const QPixmap on = !glass && theme::toolOnCategory() ? rest : duotoneIcon(resource, onInk, onInk, iconSize, dpr);
        const QPixmap off = tintedIcon(resource, theme::textDisabled().isValid() ? theme::textDisabled() : theme::textMuted(), iconSize, dpr);
        QIcon icon;
        icon.addPixmap(rest, QIcon::Normal, QIcon::Off);
        icon.addPixmap(on, QIcon::Normal, QIcon::On);
        icon.addPixmap(on, QIcon::Active, QIcon::On);
        icon.addPixmap(off, QIcon::Disabled, QIcon::Off);
        icon.addPixmap(off, QIcon::Disabled, QIcon::On);
        return icon;
    };
    // Drawn again when the labels or the theme change.
    const QString look = QStringLiteral("%1:%2").arg(theme::currentTheme()).arg(tileLabels_);
    // On a corrective (or a Sculpt node) the tools that can't edit one are greyed, not hidden: the
    // tiles never move.
    const bool layer = editingLayer();
    for (auto& [id, buttons] : tiles_) {
        const groomlab::BrushEntry* e = brushEntry(id);
        for (QToolButton* b : buttons) {
            b->setChecked(id == tool_);
            b->setEnabled(!layer || (e && e->corrective) || id == "paint");
            if (b->property("iconLook").toString() == look) continue;
            b->setProperty("iconLook", look);
            b->setIcon(toolIcon(e ? e->icon : id, e ? e->category : BrushCategory::Guides));
            // toolOn "category": the chosen tile a soft wash of its category's color (else the sheet's accent).
            if (theme::toolOnCategory() && !theme::glassy()) {
                const QColor cat = theme::toolIconColor(e ? e->category : BrushCategory::Guides, e ? e->icon : id);
                auto wash = [](const QColor& a, const QColor& b, double t) {
                    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t, a.blueF() + (b.blueF() - a.blueF()) * t);
                };
                b->setStyleSheet(QStringLiteral("QToolButton:checked { background: %1; border: 1px solid %2; color: %3; }")
                                     .arg(wash(theme::card(), cat, 0.22).name(), wash(theme::card(), cat, 0.55).name(), theme::text().name()));
            } else {
                b->setStyleSheet(QString());
            }
        }
    }
    // Labeled: the name under the icon (Maya's tiles, 5 to a row); else icons alone (the names in
    // the tool tips). Either way one height and minimum width for every tile; the grid stretches
    // them to fill its rows.
    for (QToolButton* b : allTiles_) {
        b->setIconSize(QSize(iconSize, iconSize));
        const bool wide = b->property("wide").toBool();  // Paint's (hidden): the name beside the icon
        b->setToolButtonStyle(wide ? Qt::ToolButtonTextBesideIcon : tileLabels_ ? Qt::ToolButtonTextUnderIcon : Qt::ToolButtonIconOnly);
        const QSize size = tileLabels_ ? QSize(50, 56) : QSize(44, 44);
        b->setMinimumSize(wide ? QSize(0, 38) : size);
        b->setMaximumSize(QWIDGETSIZE_MAX, wide ? 38 : size.height());
        if (b->objectName() == QLatin1String("unfreezeAllTile") && b->property("iconLook").toString() != look) {
            b->setProperty("iconLook", look);
            b->setIcon(toolIcon("unfreeze", BrushCategory::Guides));
        }
        if (b->objectName() == QLatin1String("invertFrozenTile") && b->property("iconLook").toString() != look) {
            b->setProperty("iconLook", look);
            b->setIcon(toolIcon("invert", BrushCategory::Guides));
        }
        if (b->property("labels").isValid() && b->property("labels").toBool() == tileLabels_) continue;
        b->setProperty("labels", tileLabels_);
        b->style()->unpolish(b);
        b->style()->polish(b);
    }
    for (ToolGrid* g : grids_) g->setMaxColumns(kMaxTileColumns);
}

void GroomTools::setTileLabels(bool shown) {
    tileLabels_ = shown;
    refreshTiles();
}

void GroomTools::refreshCard() {
    // The Paint tab's card is Paint's; Photos has none; the others the tool's.
    const std::string tab = currentTab().toStdString();
    const std::string shown = tab == "paint" ? std::string("paint") : tab == "photos" ? std::string() : tool_;
    keepScroll(tab + "/" + shown);  // (before anything shown or hidden here is laid out)
    const bool hadTubes = !tubes_->isHidden();
    card_->setTool(shown);
    const bool open = groomOpen();
    card_->setVisible(open && card_->hasSettings());
    // Over the card: what the picked tool does (the catalog's hint) and the gestures every brush
    // shares; no tool: how to start.
    // One line or two: the hint's first sentence without its asides (the whole hint in its tooltip). Correctives
    // have their own hint under their list.
    const groomlab::BrushEntry* picked = card_->hasSettings() ? brushEntry(shown) : nullptr;
    QString hint = picked ? qs(picked->hint) : tr("Pick a tool, then drag over the groom in the viewport.");
    const QString whole = hint;
    static const QRegularExpression kAside(QStringLiteral(R"(\s*\([^()]*\))"));
    hint.remove(kAside);  // (asides in parentheses: the tooltip's)
    for (const QString& stop : {QStringLiteral(". "), QStringLiteral("; "), QStringLiteral(" (")}) {
        const int at = hint.indexOf(stop);
        if (at > 24) hint = hint.left(at) + QStringLiteral(".");
    }
    if (!hint.endsWith(QLatin1Char('.'))) hint += QLatin1Char('.');
    // The Shape and Trim tools smooth on Shift+drag and paint the selection on Ctrl+Shift+drag
    // (Create's, Paint's and Guides' don't say so).
    const bool smooths = picked && (picked->category == BrushCategory::Shape || picked->category == BrushCategory::Trim);
    noTool_->setText(smooths ? hint + tr(" Shift+drag smooths, Ctrl+Shift+drag selects.") : hint);
    noTool_->setToolTip(picked && hint.size() < whole.size() ? whole : QString());
    // The card's header is one line (the tool's icon and name, what the next stroke edits): the
    // tool's hint stays in the title's tooltip; no tool: the line over the panel.
    card_->setSubtitle(QString());
    card_->setTitleTip(picked ? whole : QString());
    card_->setEditing(picked ? editingText(shown) : QString());
    noTool_->setVisible(open && tab != "photos" && tab != "outliner" && !picked && !card_->hasSettings());
    if (!tubes_->isHidden() && (tubesStale_ || !hadTubes)) {
        tubesStale_ = false;
        tubes_->refresh();
    }
}

void GroomTools::keepScroll(const std::string& card) {
    if (scrollCardSet_ && card == scrollCard_) return;
    // (Not laid out again yet: where it is; a tool picked again while the last switch still holds:
    // where that one holds.)
    QScrollBar* bar = scroll_->verticalScrollBar();
    const int at = scrollPending_ && scrollPending_() ? scrollHeld_ : bar->value();
    const bool first = !scrollCardSet_;
    scrollCard_ = card;
    scrollCardSet_ = true;
    scrollPending_ = nullptr;
    if (first) return;
    scrollHeld_ = at;
    scrollPending_ = scrollWhenLaidOut(bar, at, [this, card] { return scrollCard_ == card; }, kScrollFollowMs);
}

void GroomTools::refreshTubes() {
    if (tubes_->isHidden()) {
        tubesStale_ = true;
        return;
    }
    tubes_->refresh();
}

void GroomTools::refresh() {
    if (session_) {
        updating_ = true;
        card_->refresh();
        // The guide groups' row follows the session's group (a level picked with Page Up / Down).
        if (guides_) {
            const std::string group = currentGroup();
            if (!group.empty() && guides_->currentGroup() != group) guides_->setCurrentGroup(group);
        }
        updating_ = false;
    }
    if (card_->hasSettings()) card_->setEditing(editingText(card_->tool()));  // (the group picked, a level)
    // Another group picked (a row, the Roots card, a script): a level of a tube hierarchy shows its
    // guides alone (the level view), the eyes as the artist left them; another group or all groups:
    // every group as its eye says (Maya's show_picked_level). The level kept in view stays.
    if (GroomDocument* doc = session_ ? session_->document() : nullptr) {
        const std::string group = currentGroup();
        if (doc != levelViewDocument_ || group != levelViewGroup_) {
            levelViewDocument_ = doc;
            levelViewGroup_ = group;
            tubes_->applyLevelView(group);
        }
    }
    refreshTubes();
    for (auto& [button, enabled] : enables_) {
        if (button) button->setEnabled(enabled());
    }
}

void GroomTools::setHairVisible(bool visible) {
    updating_ = true;
    eye_->setChecked(visible);
    updating_ = false;
}

bool GroomTools::hairVisible() const { return eye_->isChecked(); }

void GroomTools::setGuidesVisible(bool visible) {
    updating_ = true;
    guidesEye_->setChecked(visible);
    updating_ = false;
}

bool GroomTools::guidesVisible() const { return guidesEye_->isChecked(); }

void GroomTools::setStatus(const QString& text) {
    status_->setText(text);
    status_->setVisible(!text.isEmpty());
}

QString GroomTools::status() const { return status_->text(); }

bool GroomTools::showTab(const QString& name) {
    std::string id = name.toLower().toStdString();
    if (id == "guides") id = "tools";
    for (const ui::json& tab : defs().get("palette/tabs")) {
        for (const ui::json& alias : tab.value("aliases", ui::json::array())) {
            if (alias == id) id = tab.value("id", std::string());
        }
    }
    const auto found = std::find(tabIds_.begin(), tabIds_.end(), id);
    if (found == tabIds_.end()) return false;
    tabs_->setCurrentIndex(static_cast<int>(found - tabIds_.begin()));
    return true;
}

QString GroomTools::currentTab() const {
    const int index = tabs_->currentIndex();
    return index >= 0 && index < static_cast<int>(tabIds_.size()) ? qs(tabIds_[static_cast<std::size_t>(index)]) : QString();
}

void GroomTools::fitScroll(QWidget* content) {
    // A panel made for a dock of its own, fitted to the column: no margins of its own (the
    // column has them), combos as narrow as a few letters, lists without a sideways bar, its
    // scroll area as tall as its content.
    QScrollArea* area = content->findChild<QScrollArea*>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget* w : {content, area ? area->widget() : nullptr}) {
        if (w && w->layout()) w->layout()->setContentsMargins(0, 0, 0, 0);
    }
    for (QComboBox* combo : content->findChildren<QComboBox*>()) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(6);
    }
    for (QAbstractScrollArea* list : content->findChildren<QAbstractItemView*>()) list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // A tree's first column takes the room; the counts after it as wide as they need.
    for (QTreeView* tree : content->findChildren<QTreeView*>()) {
        QHeaderView* header = tree->header();
        if (header->count() < 2 || header->sectionResizeMode(0) != QHeaderView::Stretch) continue;
        for (int i = 1; i < header->count(); ++i) header->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    }
    if (area) new ScrollFitter(area);
}

void GroomTools::setPaintContent(QWidget* content) {
    lists_.at("paint")->layout()->addWidget(content);
    // Mode, Value + Fill and the color: the Paint card's (its Paint block).
    if (auto* maps = qobject_cast<MapsPanel*>(content)) {
        maps_ = maps;
        card_->setPaintControls(maps->paintControls());
        card_->setWholeFaces(maps->wholeFacesBox());  // (the Paint card's toggles row: Whole faces)
        // The Paint card's header names the map it paints.
        connect(maps->list(), &QListWidget::currentItemChanged, this, [this] { refreshCard(); });
    }
    fitScroll(content);
}

void GroomTools::setCorrectivesContent(QWidget* content) {
    lists_.at("correctives")->layout()->addWidget(content);
    if (auto* correctives = qobject_cast<CorrectivesPanel*>(content)) {
        correctives_ = correctives;
        // A corrective picked or let go: the tiles that can't edit one greyed (the tool that can't
        // stops), the card's header naming the layer.
        connect(correctives, &CorrectivesPanel::targetChanged, this, [this] {
            refreshTiles();
            refreshCard();
            const groomlab::BrushEntry* e = brushEntry(tool_);
            if (editingLayer() && !tool_.empty() && tool_ != "paint" && !(e && e->corrective)) emit toolStopRequested();
        });
    }
    fitScroll(content);
}

void GroomTools::setPhotosContent(QWidget* content) { lists_.at("photos")->layout()->addWidget(content); }

void GroomTools::setOutlinerContent(QWidget* content) {
    lists_.at("outliner")->layout()->addWidget(content);
    fitScroll(content);
}

void GroomTools::setGroupsContent(QWidget* content) {
    lists_.at("tools")->layout()->addWidget(content);
    if (auto* guides = qobject_cast<GuidesPanel*>(content)) {
        guides_ = guides;
        guides->header()->addGlyph(isolate_, 0);  // Isolate: the eye, before New Group
        connect(guides, &GuidesPanel::groupClicked, this, [this](const std::string& name) { groupPicked(name); });
        connect(guides, &GuidesPanel::tubeViewRequested, this, [this](const std::string& group) { tubes_->setTubeView(group); });
        connect(guides, &GuidesPanel::displayChanged, this, [this] { refreshCard(); });  // (a group's color)
    }
    fitScroll(content);
}

QAbstractButton* GroomTools::tile(const std::string& id) const {
    if (id == "unfreeze" || id == "invert") {
        const QLatin1String name(id == "unfreeze" ? "unfreezeAllTile" : "invertFrozenTile");
        for (QToolButton* b : allTiles_) {
            if (b->objectName() == name) return b;
        }
    }
    const auto it = tiles_.find(id);
    return it == tiles_.end() || it->second.empty() ? nullptr : it->second.front();
}

std::vector<QAbstractButton*> GroomTools::tiles() const { return {allTiles_.begin(), allTiles_.end()}; }

void GroomTools::refreshVisibility() {
    const bool open = groomOpen();
    const std::string tab = currentTab().toStdString();
    empty_->setVisible(!open);
    work_->setVisible(open);
    pages_->setVisible(open && tabsShown_);
    tabs_->setVisible(open && tabsShown_);
    // The grid: the categories' trays on Tools; none on Paint (the tab is painting) and Photos.
    toolsGrid_->setVisible(open && tab == "tools");
    for (std::size_t i = 0; i < grids_.size(); ++i) {
        grids_[i]->setShown([](QAbstractButton*) { return true; });
        if (i < gridLabels_.size()) gridLabels_[i]->parentWidget()->setVisible(!grids_[i]->shown().empty());
    }
    paintGrid_->setVisible(false);
    // The tab's list and its sections' cards (Correctives under the guide groups on Tools).
    for (auto& [id, holder] : lists_) {
        const auto section = sectionTabs_.find(id);
        holder->setVisible(section != sectionTabs_.end() ? section->second == tab : id == tab);
    }
    // The card under the tools, then the list (Paint: the Paint card over the maps it paints).
    if (auto* work = qobject_cast<QVBoxLayout*>(work_->layout())) {
        if (work->indexOf(noTool_) != 0 || work->indexOf(card_) != 1) {
            work->removeWidget(card_);
            work->removeWidget(noTool_);
            work->insertWidget(0, noTool_);
            work->insertWidget(1, card_);
        }
    }
    eye_->setEnabled(open);
    guidesEye_->setEnabled(open);
    guidesDisplay_->setEnabled(open);
    graph_->setEnabled(open);
    refreshCard();
}

bool GroomTools::editingLayer() const {
    return correctives_ && (!correctives_->targetLayer().empty() || !correctives_->targetNode().empty());
}

QString GroomTools::editingText(const std::string& tool) const {
    if (tool == "paint") {
        // Paint: the map it paints (and whether the graph reads it yet).
        const std::string map = maps_ ? maps_->selectedMap() : std::string();
        if (map.empty()) return tr("no map yet: + under Maps");
        const bool unused = maps_->usersText().startsWith(tr("Not used"));
        return tr("on <b>%1</b>%2").arg(qs(map).toHtmlEscaped(), unused ? tr(" · not used yet") : QString());
    }
    // The picked guide group (its color's dot), on the base groom or a corrective (in the accent).
    const std::string group = currentGroup();
    QColor dot = theme::textMuted();
    if (guides_ && !group.empty()) {
        const QColor c = guides_->groupColor(group);
        if (c.isValid()) dot = c;
    }
    QString layer;
    if (correctives_) {
        const std::string node = correctives_->targetNode(), corrective = correctives_->targetLayer();
        if (!node.empty()) layer = qs(node);
        else if (!corrective.empty()) layer = qs(corrective);
    }
    const QString on = layer.isEmpty() ? tr("Base groom")
                                       : QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::accentInk().name(), layer.toHtmlEscaped());
    // A level of a tube hierarchy kept in view while another's guides are picked: said here, so the
    // brush is seen to comb these guides under those tubes.
    QString tubes;
    const GroomDocument* doc = session_ ? session_->document() : nullptr;
    const std::string viewed = doc ? doc->tubeViewGroup() : std::string();
    if (!viewed.empty() && viewed != group) {
        const int level = tubes_ ? tubes_->tubeLevelOf(viewed) : 0;
        tubes = QStringLiteral("  ·  tubes: %1").arg(level ? tr("Level %1").arg(level) : qs(viewed).toHtmlEscaped());
    }
    return QStringLiteral("<span style='color:%1'>●</span> %2 <span style='color:%3'>on</span> %4%5")
        .arg(dot.name(), (group.empty() ? tr("All groups") : qs(group)).toHtmlEscaped(), theme::textMuted().name(), on, tubes);
}

}  // namespace groomlab::app
