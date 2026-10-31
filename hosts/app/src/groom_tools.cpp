// SPDX-License-Identifier: Apache-2.0
#include "groom_tools.h"

#include <algorithm>

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
#include <QMenu>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyle>
#include <QTabBar>
#include <QToolButton>
#include <QTreeView>
#include <QVariant>
#include <QVBoxLayout>

#include "groomlab/brush_catalog.h"

#include "brush_panel.h"
#include "brush_session.h"
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
constexpr int kMaxTileColumns = 4;

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

    // One scroll area for the whole panel: the header, the status, the tabs and the tool grid on
    // top, then the empty state, the tab's list and the card.
    scroll_ = new QScrollArea(this);
    scroll_->setObjectName(QStringLiteral("groomToolsScroll"));
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // (Always there: a bar coming and going would change the width the hosted panels fit to.)
    scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    frame->addWidget(scroll_, 1);
    auto* content = new QWidget(scroll_);
    content->setObjectName(QStringLiteral("groomToolsContent"));
    scroll_->setWidget(content);
    auto* outer = new QVBoxLayout(content);
    outer->setContentsMargins(10, 8, 10, 12);
    outer->setSpacing(6);

    // ---- The top: the header, the status, the tabs and the tool grid.
    top_ = new QWidget(content);
    top_->setObjectName(QStringLiteral("groomToolsTop"));
    auto* top = new QVBoxLayout(top_);
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(6);
    outer->addWidget(top_);
    const ui::json& header = palette.at("header");
    auto* head = new QHBoxLayout();
    head->setSpacing(2);
    groomName_ = new QLabel(top_);
    groomName_->setObjectName(QStringLiteral("groomName"));
    groomName_->setProperty("role", "title");
    groomName_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    groomName_->setToolTip(tr("The groom every tool works on: the open one"));
    head->addWidget(groomName_, 1);
    // The hair's eye: the strands alone (instances, cards too); the guides are the next eye's.
    eye_ = glyphButton("eye", tr("Show or hide the hair"), top_);
    eye_->setObjectName(QStringLiteral("hairVisible"));
    eye_->setCheckable(true);
    eye_->setChecked(true);
    connect(eye_, &QToolButton::toggled, this, [this](bool on) {
        eye_->setToolTip(on ? tr("Show or hide the hair") : tr("The hair is hidden: click to show it"));
        if (!updating_) emit hairVisibilityToggled(on);
    });
    head->addWidget(eye_);
    // The guides' eye: every guide group at once (Properties > Guides; Maya's displayGuides).
    guidesEye_ = glyphButton("groups", tr("Show or hide the guides, every guide group"), top_);
    guidesEye_->setObjectName(QStringLiteral("guidesVisible"));
    guidesEye_->setCheckable(true);
    guidesEye_->setChecked(true);
    connect(guidesEye_, &QToolButton::toggled, this, [this](bool on) {
        guidesEye_->setToolTip(on ? tr("Show or hide the guides, every guide group") : tr("The guides are hidden: click to show them"));
        if (!updating_) emit guidesVisibilityToggled(on);
    });
    head->addWidget(guidesEye_);
    graph_ = glyphButton("graph", withShortcut(qs(header.at("graph").value("tip", std::string())), "view.graph"), top_);
    graph_->setObjectName(QStringLiteral("openGraph"));
    connect(graph_, &QToolButton::clicked, this, &GroomTools::graphRequested);
    head->addWidget(graph_);
    more_ = glyphButton("more", qs(header.at("more").value("tip", std::string())), top_);
    more_->setObjectName(QStringLiteral("groomToolsMore"));
    more_->setPopupMode(QToolButton::InstantPopup);
    moreMenu_ = new QMenu(more_);
    moreMenu_->setObjectName(QStringLiteral("groomToolsMoreMenu"));
    more_->setMenu(moreMenu_);
    head->addWidget(more_);
    top->addLayout(head);

    status_ = label(QString(), "status", top_);
    status_->setObjectName(QStringLiteral("groomToolsStatus"));
    status_->setWordWrap(true);
    status_->setVisible(false);
    top->addWidget(status_);

    // The tabs (palette.tabs), underlined as the Properties dock's.
    tabs_ = new QTabBar(top_);
    tabs_->setObjectName(QStringLiteral("groomTabs"));
    tabs_->setExpanding(false);
    tabs_->setUsesScrollButtons(false);
    tabs_->setElideMode(Qt::ElideRight);
    tabs_->setDrawBase(false);
    tabs_->setDocumentMode(true);
    top->addWidget(tabs_);

    // The tool grid: the tiles by category (Paint's on the Paint tab), Unfreeze All beside Freeze.
    auto tileGrid = [this](BrushCategory category, QWidget* parent) {
        std::vector<QAbstractButton*> buttons;
        for (const groomlab::BrushEntry* e : brushesInCategory(category)) {
            buttons.push_back(makeTile(e->id, qs(e->label), tileTip(*e)));
            if (e->id == "freeze") {
                // Unfreeze All: a button, not a tool.
                auto* b = new QToolButton(parent);
                b->setObjectName(QStringLiteral("unfreezeAllTile"));
                b->setProperty("role", "tool");
                b->setText(tr("Unfreeze All"));  // (its icon: refreshTiles)
                b->setToolTip(tr("<b>Unfreeze All</b><br>Clear the freeze mask: every guide can be brushed again (Shift+click with a "
                                 "brush where no guide is does the same)"));
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
    tools->setSpacing(3);
    for (BrushCategory category : brushCategories()) {
        if (category == BrushCategory::Paint) continue;  // beside the maps they paint
        QLabel* name = label(QString::fromLatin1(brushCategoryName(category)), "category", toolsGrid_);
        tools->addWidget(name);
        gridLabels_.push_back(name);
        tools->addWidget(tileGrid(category, toolsGrid_));
    }
    top->addWidget(toolsGrid_);
    paintGrid_ = new QWidget(top_);
    paintGrid_->setObjectName(QStringLiteral("paintGrid"));
    auto* paintTiles = new QVBoxLayout(paintGrid_);
    paintTiles->setContentsMargins(0, 0, 0, 0);
    // The Paint tab: Paint as one full-width button over the maps.
    for (const groomlab::BrushEntry* e : brushesInCategory(BrushCategory::Paint)) {
        QToolButton* b = makeTile(e->id, qs(e->label), tileTip(*e));
        b->setProperty("wide", true);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        paintTiles->addWidget(b);
    }
    top->addWidget(paintGrid_);

    // ---- Under the top: the empty state, the tab's list, the card.
    // No groom: how to start one (palette.empty; its buttons with the bindings).
    empty_ = new QFrame(content);
    empty_->setObjectName(QStringLiteral("groomToolsEmpty"));
    empty_->setProperty("role", "empty");
    emptyItems_ = new QVBoxLayout(empty_);
    emptyItems_->setContentsMargins(12, 12, 12, 12);
    emptyItems_->setSpacing(8);
    emptyItems_->addWidget(label(qs(palette.at("empty").value("title", std::string())), "title", empty_));
    emptyItems_->addWidget(label(tr("Open a groom (File > Open Groom, or drop a .groom on the window), or make one on a mesh."), "about", empty_));
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
        holder->layout()->setContentsMargins(0, 0, 0, 0);
        work->addWidget(holder);
        lists_[id] = holder;
        tabIds_.push_back(id);
        const int index = tabs_->addTab(qs(tab.value("label", std::string())));
        tabs_->setTabToolTip(index, qs(tab.value("tip", std::string())));
    }

    // Under the list: the tool's card, the tubes' blocks inside it.
    card_ = new BrushPanel(work_);
    connect(card_, &BrushPanel::settingsChanged, this, &GroomTools::settingsChanged);
    connect(card_, &BrushPanel::guidesEdited, this, &GroomTools::guidesEdited);
    connect(card_, &BrushPanel::clearFlowRequested, this, &GroomTools::clearFlowRequested);
    connect(card_, &BrushPanel::lengthsPreviewed, this, &GroomTools::lengthsPreviewed);
    work->addWidget(card_);
    tubes_ = new TubePanel();
    connect(tubes_, &TubePanel::guidesEdited, this, &GroomTools::guidesEdited);
    connect(tubes_, &TubePanel::previewed, this, &GroomTools::tubesPreviewed);
    connect(tubes_, &TubePanel::settingsChanged, this, [this] {
        if (!updating_) emit settingsChanged();
    });
    connect(tubes_, &TubePanel::message, this, [this](const QString& text) { setStatus(text); });
    card_->setTubes(tubes_);
    noTool_ = label(tr("Pick a tool, then drag over the groom in the viewport."), "hint", work_);
    noTool_->setObjectName(QStringLiteral("noTool"));
    noTool_->setContentsMargins(0, 8, 0, 4);
    work->addWidget(noTool_);

    connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
        refreshVisibility();
        if (index < 0 || index >= static_cast<int>(tabIds_.size())) return;
        const std::string tab = tabIds_[static_cast<std::size_t>(index)];
        emit tabChanged(qs(tab));
        // Correctives: a tool whose tile is gone here (it can't record into a corrective) stops.
        const groomlab::BrushEntry* e = brushEntry(tool_);
        if (tab == "correctives" && !tool_.empty() && !(e && e->corrective)) emit toolStopRequested();
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
        if (item.kind == ui::Item::Kind::Action) {
            auto* b = new QPushButton(qs(item.label), empty_);
            b->setObjectName(QStringLiteral("empty_") + qs(item.id));
            b->setProperty("role", "primary");
            b->setToolTip(qs(item.tip));
            const ui::Binding binding = bindings.at(item.id);
            connect(b, &QPushButton::clicked, this, [binding, value = item.value] { binding.trigger(value, false); });
            emptyItems_->addWidget(b);
        } else if (item.kind == ui::Item::Kind::Submenu) {
            emptyItems_->addWidget(menuChip(item, bindings, options, empty_));
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
    groomName_->setText(name.isEmpty() ? tr("No groom open") : name);
    groomName_->setToolTip(name.isEmpty() ? tr("No groom open") : tr("The groom every tool works on: %1").arg(name));
    refreshVisibility();
    refresh();
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

QCheckBox* GroomTools::symmetryBox() const { return card_->symmetryBox(); }
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
    refresh();
    emit settingsChanged();
    emit isolateChanged();
}

void GroomTools::refreshTiles() {
    const qreal dpr = devicePixelRatioF();
    const int iconSize = 24;
    // The icons in their two tones (the group's color and the ink, as Maya's); checked, a glass
    // theme's in the accent's text (on the accent), a solid theme's as at rest (on the selection).
    const bool glass = theme::glassy();
    auto toolIcon = [&](const std::string& stem, BrushCategory category) {
        const QString resource = iconPath(stem);
        const QPixmap rest = duotoneIcon(resource, theme::toolIconColor(category, stem), theme::iconInk(), iconSize, dpr);
        const QPixmap on = glass ? tintedIcon(resource, theme::uiColor("accentText"), iconSize, dpr) : rest;
        const QPixmap off = tintedIcon(resource, theme::textMuted(), iconSize, dpr);
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
    for (auto& [id, buttons] : tiles_) {
        const groomlab::BrushEntry* e = brushEntry(id);
        for (QToolButton* b : buttons) {
            b->setChecked(id == tool_);
            if (b->property("iconLook").toString() == look) continue;
            b->setProperty("iconLook", look);
            b->setIcon(toolIcon(e ? e->icon : id, e ? e->category : BrushCategory::Guides));
        }
    }
    // Labeled: the name under the icon on a raised tile (Maya's tiles); else icons alone (the
    // names in the tool tips). Either way one height and minimum width for every tile; the grid
    // stretches them to fill its rows.
    for (QToolButton* b : allTiles_) {
        b->setIconSize(QSize(iconSize, iconSize));
        const bool wide = b->property("wide").toBool();  // Paint's: the panel's width, the name beside the icon
        b->setToolButtonStyle(wide ? Qt::ToolButtonTextBesideIcon : tileLabels_ ? Qt::ToolButtonTextUnderIcon : Qt::ToolButtonIconOnly);
        const QSize size = tileLabels_ ? QSize(56, 54) : QSize(38, 38);
        b->setMinimumSize(wide ? QSize(0, 38) : size);
        b->setMaximumSize(QWIDGETSIZE_MAX, wide ? 38 : size.height());
        if (b->objectName() == QLatin1String("unfreezeAllTile") && b->property("iconLook").toString() != look) {
            b->setProperty("iconLook", look);
            b->setIcon(toolIcon("unfreeze", BrushCategory::Guides));
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
    const bool hadTubes = !tubes_->isHidden();
    card_->setTool(shown);
    const bool open = groomOpen();
    card_->setVisible(open && card_->hasSettings());
    noTool_->setVisible(open && !card_->hasSettings() && tab != "photos");
    if (!tubes_->isHidden() && (tubesStale_ || !hadTubes)) {
        tubesStale_ = false;
        tubes_->refresh();
    }
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
    if (auto* maps = qobject_cast<MapsPanel*>(content)) card_->setPaintControls(maps->paintControls());
    fitScroll(content);
}

void GroomTools::setCorrectivesContent(QWidget* content) {
    lists_.at("correctives")->layout()->addWidget(content);
    fitScroll(content);
}

void GroomTools::setPhotosContent(QWidget* content) { lists_.at("photos")->layout()->addWidget(content); }

void GroomTools::setGroupsContent(QWidget* content) {
    lists_.at("tools")->layout()->addWidget(content);
    if (auto* guides = qobject_cast<GuidesPanel*>(content)) {
        guides_ = guides;
        guides->header()->addGlyph(isolate_, 0);  // Isolate: the eye, before New Group
        connect(guides, &GuidesPanel::groupClicked, this, [this](const std::string& name) { groupPicked(name); });
    }
    fitScroll(content);
}

QAbstractButton* GroomTools::tile(const std::string& id) const {
    if (id == "unfreeze") {
        for (QToolButton* b : allTiles_) {
            if (b->objectName() == QLatin1String("unfreezeAllTile")) return b;
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
    tabs_->setVisible(open);
    // The grid: the categories' on Tools and Correctives, the Paint tile on Paint, none on Photos.
    toolsGrid_->setVisible(open && (tab == "tools" || tab == "correctives"));
    // Correctives: only the tools that work on a corrective (a category with none of them goes).
    const bool layers = tab == "correctives";
    for (std::size_t i = 0; i < grids_.size(); ++i) {
        grids_[i]->setShown([layers](QAbstractButton* b) {
            const groomlab::BrushEntry* e = brushEntry(b->property("tool").toString().toStdString());
            return !layers || (e && e->corrective);
        });
        if (i < gridLabels_.size()) gridLabels_[i]->setVisible(!grids_[i]->shown().empty());
    }
    paintGrid_->setVisible(open && tab == "paint");
    for (auto& [id, holder] : lists_) holder->setVisible(id == tab);
    eye_->setEnabled(open);
    guidesEye_->setEnabled(open);
    graph_->setEnabled(open);
    refreshCard();
}

}  // namespace groomlab::app
