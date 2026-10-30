// SPDX-License-Identifier: Apache-2.0
#include "groom_tools.h"

#include <algorithm>

#include <QAbstractButton>
#include <QAction>
#include <QAbstractItemView>
#include <QCheckBox>
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
#include "glass.h"
#include "theme.h"
#include "tube_panel.h"

namespace groomlab::app {

namespace {

const ui::Definitions& defs() { return ui::Definitions::shipped(); }

QString qs(const std::string& s) { return QString::fromStdString(s); }

QString textAt(const std::string& path) {
    const ui::json* t = defs().find(path);
    return t && t->is_string() ? qs(t->get<std::string>()) : QString();
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
void renameActions(QMenu* menu) {
    for (QAction* a : menu->findChildren<QAction*>()) {
        const QString name = a->objectName();
        if (!name.isEmpty() && !name.startsWith(QLatin1String("groomTools_"))) a->setObjectName(QStringLiteral("groomTools_") + name);
    }
    for (QMenu* sub : menu->findChildren<QMenu*>()) {
        const QString name = sub->objectName();
        if (!name.isEmpty() && !name.startsWith(QLatin1String("groomTools_"))) sub->setObjectName(QStringLiteral("groomTools_") + name);
        if (sub->property("renames").toBool()) continue;
        sub->setProperty("renames", true);
        QObject::connect(sub, &QMenu::aboutToShow, sub, [sub] { renameActions(sub); });
    }
}

void keepRenamed(QMenu* menu) {
    renameActions(menu);
    if (menu->property("renames").toBool()) return;
    menu->setProperty("renames", true);
    QObject::connect(menu, &QMenu::aboutToShow, menu, [menu] { renameActions(menu); });
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
    buttons_ = std::move(buttons);
    for (QAbstractButton* b : buttons_) b->setParent(this);
    maxColumns_ = std::max(1, maxColumns);
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
    const int columns = std::min<int>(maxColumns_, std::max<int>(1, static_cast<int>(buttons_.size())));
    const int width = columns * t.width() + (columns - 1) * spacing_;
    return {width, heightForWidth(width)};
}

QSize ToolGrid::minimumSizeHint() const { return {tile().width(), heightForWidth(tile().width())}; }

void ToolGrid::place() {
    if (buttons_.empty()) return;
    const int width = std::max(this->width(), tile().width());
    const int columns = columnsFor(width);
    const int w = (width - (columns - 1) * spacing_) / columns;
    const int h = tile().height();
    for (std::size_t i = 0; i < buttons_.size(); ++i) {
        const int row = static_cast<int>(i) / columns, column = static_cast<int>(i) % columns;
        buttons_[i]->setGeometry(column * (w + spacing_), row * (h + spacing_), w, h);
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

// ---- Section ------------------------------------------------------------------------------------------

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

// ---- GroomTools ---------------------------------------------------------------------------------------

GroomTools::GroomTools(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("groomTools"));
    const ui::json& palette = defs().get("palette");
    auto* frame = new QVBoxLayout(this);
    frame->setContentsMargins(0, 0, 0, 0);
    frame->setSpacing(0);
    // Everything in one column that scrolls (Maya's Groom Tools).
    scroll_ = new QScrollArea(this);
    scroll_->setObjectName(QStringLiteral("groomToolsScroll"));
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // (Always there: a bar coming and going would change the width the hosted panels fit to.)
    scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    frame->addWidget(scroll_);
    auto* content = new QWidget(scroll_);
    content->setObjectName(QStringLiteral("groomToolsContent"));
    scroll_->setWidget(content);
    outer_ = new QVBoxLayout(content);
    outer_->setContentsMargins(10, 8, 10, 12);
    outer_->setSpacing(8);

    // ---- The header: the groom, the eyes (its hair, its guides), the Graph Editor, More.
    const ui::json& header = palette.at("header");
    auto* top = new QHBoxLayout();
    top->setSpacing(2);
    groomName_ = new QLabel(content);
    groomName_->setObjectName(QStringLiteral("groomName"));
    groomName_->setProperty("role", "title");
    groomName_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    groomName_->setToolTip(tr("The groom every tool works on: the open one"));
    top->addWidget(groomName_, 1);
    // The hair's eye: the strands alone (instances, cards too); the guides are the next eye's.
    eye_ = glyphButton("eye", tr("Show or hide the hair"), content);
    eye_->setObjectName(QStringLiteral("hairVisible"));
    eye_->setCheckable(true);
    eye_->setChecked(true);
    connect(eye_, &QToolButton::toggled, this, [this](bool on) {
        eye_->setToolTip(on ? tr("Show or hide the hair") : tr("The hair is hidden: click to show it"));
        if (!updating_) emit hairVisibilityToggled(on);
    });
    top->addWidget(eye_);
    // The guides' eye: every guide group at once (Properties > Guides; Maya's displayGuides).
    guidesEye_ = glyphButton("groups", tr("Show or hide the guides, every guide group"), content);
    guidesEye_->setObjectName(QStringLiteral("guidesVisible"));
    guidesEye_->setCheckable(true);
    guidesEye_->setChecked(true);
    connect(guidesEye_, &QToolButton::toggled, this, [this](bool on) {
        guidesEye_->setToolTip(on ? tr("Show or hide the guides, every guide group") : tr("The guides are hidden: click to show them"));
        if (!updating_) emit guidesVisibilityToggled(on);
    });
    top->addWidget(guidesEye_);
    graph_ = glyphButton("graph", withShortcut(qs(header.at("graph").value("tip", std::string())), "view.graph"), content);
    graph_->setObjectName(QStringLiteral("openGraph"));
    connect(graph_, &QToolButton::clicked, this, &GroomTools::graphRequested);
    top->addWidget(graph_);
    more_ = glyphButton("more", qs(header.at("more").value("tip", std::string())), content);
    more_->setObjectName(QStringLiteral("groomToolsMore"));
    more_->setPopupMode(QToolButton::InstantPopup);
    moreMenu_ = new QMenu(more_);
    moreMenu_->setObjectName(QStringLiteral("groomToolsMoreMenu"));
    more_->setMenu(moreMenu_);
    top->addWidget(more_);
    outer_->addLayout(top);

    // ---- The group brushes edit and new guides go into; Isolate; Symmetry.
    targetRow_ = new QWidget(content);
    targetRow_->setObjectName(QStringLiteral("targetRow"));
    auto* lines = new QVBoxLayout(targetRow_);
    lines->setContentsMargins(0, 0, 0, 0);
    lines->setSpacing(6);
    auto* row = new QHBoxLayout();
    row->setSpacing(8);
    row->addWidget(label(tr("Group"), "about", targetRow_));
    group_ = new QComboBox(targetRow_);
    group_->setObjectName(QStringLiteral("brushGroup"));
    group_->setToolTip(tr("The guide group brushes edit and new guides go into; All groups: every guide"));
    group_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    group_->setMinimumContentsLength(6);
    connect(group_, &QComboBox::currentIndexChanged, this, [this](int) { groupPicked(); });
    row->addWidget(group_, 1);
    isolate_ = new QCheckBox(tr("Isolate"), targetRow_);
    isolate_->setObjectName(QStringLiteral("isolate"));
    isolate_->setToolTip(tr("Show the group (and its child groups) at full color and dim the others"));
    connect(isolate_, &QCheckBox::toggled, this, [this](bool) {
        if (!updating_) emit isolateChanged();
    });
    row->addWidget(isolate_);
    lines->addLayout(row);
    auto* symmetryRow = new QHBoxLayout();
    symmetryRow->setSpacing(8);
    const groomlab::BrushSettingSpec* symmetrySpec = brushSettingSpec("symmetry");
    const QString symmetryTip = withShortcut(symmetrySpec ? qs(symmetrySpec->hint) : QString(), "brush.symmetry");
    symmetry_ = new QCheckBox(symmetrySpec ? qs(symmetrySpec->label) : tr("Symmetry"), targetRow_);
    symmetry_->setObjectName(QStringLiteral("symmetryOn"));
    symmetry_->setToolTip(symmetryTip + tr(". While it is on, a red line on the scalp shows the mirror plane while a brush is on."));
    symmetryMode_ = new QComboBox(targetRow_);
    symmetryMode_->setObjectName(QStringLiteral("symmetry"));
    symmetryMode_->setToolTip(symmetryTip);
    // World X and Topology as every host offers them; World Y and Z the app's own.
    for (const ui::json& item : defs().choices("symmetry")) {
        symmetryMode_->addItem(qs(item.value("label", std::string())), qs(item.value("value", std::string())));
        symmetryMode_->setItemData(symmetryMode_->count() - 1, qs(item.value("tip", std::string())), Qt::ToolTipRole);
        if (item.value("value", std::string()) == "x") {
            symmetryMode_->addItem(tr("World Y"), QStringLiteral("y"));
            symmetryMode_->setItemData(symmetryMode_->count() - 1, tr("Across the plane y = center"), Qt::ToolTipRole);
            symmetryMode_->addItem(tr("World Z"), QStringLiteral("z"));
            symmetryMode_->setItemData(symmetryMode_->count() - 1, tr("Across the plane z = center"), Qt::ToolTipRole);
        }
    }
    symmetryMode_->setEnabled(false);
    connect(symmetry_, &QCheckBox::toggled, this, [this](bool) { symmetryEdited(); });
    connect(symmetryMode_, &QComboBox::currentIndexChanged, this, [this](int) { symmetryEdited(); });
    symmetryRow->addWidget(symmetry_);
    symmetryRow->addWidget(symmetryMode_, 1);
    lines->addLayout(symmetryRow);
    // (The app's own: the world plane's center along its axis.)
    centerRow_ = new QWidget(targetRow_);
    auto* center = new QHBoxLayout(centerRow_);
    center->setContentsMargins(0, 0, 0, 0);
    center->setSpacing(8);
    center->addWidget(label(tr("Center"), "about", centerRow_));
    symmetryCenter_ = new ValueSlider(centerRow_);
    symmetryCenter_->setObjectName(QStringLiteral("symmetryCenter"));
    symmetryCenter_->setRange(-10.0, 10.0);
    symmetryCenter_->setLimits(-1e6, 1e6);
    symmetryCenter_->setDecimals(3);
    symmetryCenter_->setValueWidth(46);
    symmetryCenter_->setFill(theme::sliderFill(theme::blue()));
    symmetryCenter_->setToolTip(tr("Where the world plane sits along its axis (0: the origin)"));
    connect(symmetryCenter_, &ValueSlider::valueChanged, this, [this](double v) {
        if (updating_ || !session_) return;
        session_->settings().brush.mirrorCenter = static_cast<float>(v);
        emit settingsChanged();
    });
    center->addWidget(symmetryCenter_, 1);
    lines->addWidget(centerRow_);
    outer_->addWidget(targetRow_);

    status_ = label(QString(), "status", content);
    status_->setObjectName(QStringLiteral("groomToolsStatus"));
    status_->setWordWrap(true);
    status_->setVisible(false);
    outer_->addWidget(status_);

    // ---- No groom: how to start one (palette.empty; its buttons with the bindings).
    empty_ = new QFrame(content);
    empty_->setObjectName(QStringLiteral("groomToolsEmpty"));
    empty_->setProperty("role", "empty");
    emptyItems_ = new QVBoxLayout(empty_);
    emptyItems_->setContentsMargins(12, 12, 12, 12);
    emptyItems_->setSpacing(8);
    emptyItems_->addWidget(label(qs(palette.at("empty").value("title", std::string())), "title", empty_));
    emptyItems_->addWidget(label(tr("Open a groom (File > Open Groom, or drop a .groom on the window), or make one on a mesh."), "about", empty_));
    outer_->addWidget(empty_);

    // ---- A first groom, step by step (palette.starter).
    starter_ = new QFrame(content);
    starter_->setObjectName(QStringLiteral("starter"));
    starter_->setProperty("role", "empty");
    {
        const ui::json& starter = palette.at("starter");
        auto* column = new QVBoxLayout(starter_);
        column->setContentsMargins(12, 8, 6, 10);
        column->setSpacing(2);
        auto* titleRow = new QHBoxLayout();
        titleRow->addWidget(label(qs(starter.value("title", std::string())).toUpper(), "subheading", starter_), 1);
        auto* close = new QToolButton(starter_);
        close->setObjectName(QStringLiteral("starterDismiss"));
        close->setProperty("role", "glyph");
        close->setAutoRaise(true);
        close->setText(QStringLiteral("✕"));
        close->setToolTip(qs(starter.value("dismissTip", std::string())));
        connect(close, &QToolButton::clicked, this, &GroomTools::starterDismissRequested);
        titleRow->addWidget(close);
        column->addLayout(titleRow);
        for (const ui::json& step : starter.at("steps")) {
            const std::string id = step.value("id", std::string());
            auto* b = new QPushButton(qs(step.value("label", std::string())), starter_);
            b->setObjectName(QStringLiteral("starter_") + qs(id));
            b->setFlat(true);
            b->setProperty("role", "step");
            b->setToolTip(qs(step.value("tip", std::string())));
            connect(b, &QPushButton::clicked, this, [this, id] { emit starterStepRequested(qs(id)); });
            column->addWidget(b);
            starterRows_[id] = b;
        }
        starterTip_ = label(QString(), "hint", starter_);
        column->addWidget(starterTip_);
    }
    starter_->setVisible(false);
    outer_->addWidget(starter_);

    // ---- The work: everything that needs a groom.
    work_ = new QWidget(content);
    work_->setObjectName(QStringLiteral("groomToolsWork"));
    auto* work = new QVBoxLayout(work_);
    work->setContentsMargins(0, 0, 0, 0);
    work->setSpacing(8);
    outer_->addWidget(work_);

    // The tabs (palette.tabs), underlined as the Properties dock's, over the pages: only the
    // current page takes room.
    tabs_ = new QTabBar(work_);
    tabs_->setObjectName(QStringLiteral("groomTabs"));
    tabs_->setExpanding(false);
    tabs_->setUsesScrollButtons(false);
    tabs_->setElideMode(Qt::ElideRight);
    tabs_->setDrawBase(false);
    tabs_->setDocumentMode(true);
    work->addWidget(tabs_);

    // Tools: the tiles by category (Paint's in its own tab), Unfreeze All beside Freeze.
    auto* toolsPage = new QWidget(work_);
    toolsPage->setObjectName(QStringLiteral("toolsPage"));
    auto* tools = new QVBoxLayout(toolsPage);
    tools->setContentsMargins(0, 0, 0, 0);
    tools->setSpacing(4);
    auto tileGrid = [this](BrushCategory category, QWidget* parent) {
        std::vector<QAbstractButton*> buttons;
        for (const groomlab::BrushEntry* e : brushesInCategory(category)) {
            buttons.push_back(makeTile(e->id, qs(e->label), qs(e->hint)));
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
        grid->setButtons(std::move(buttons), 4);
        grids_.push_back(grid);
        return grid;
    };
    for (BrushCategory category : brushCategories()) {
        if (category == BrushCategory::Paint) continue;  // beside the maps they paint
        tools->addWidget(label(QString::fromLatin1(brushCategoryName(category)), "category", toolsPage));
        tools->addWidget(tileGrid(category, toolsPage));
    }
    // Paint: the Paint tile over the maps (MainWindow's MapsPanel, setPaintContent).
    auto* paintPage = new QWidget(work_);
    paintPage->setObjectName(QStringLiteral("paintPage"));
    auto* paint = new QVBoxLayout(paintPage);
    paint->setContentsMargins(0, 0, 0, 0);
    paint->setSpacing(6);
    paint->addWidget(tileGrid(BrushCategory::Paint, paintPage));
    paintHolder_ = new QWidget(paintPage);
    paintHolder_->setLayout(new QVBoxLayout());
    paintHolder_->layout()->setContentsMargins(0, 0, 0, 0);
    paint->addWidget(paintHolder_);
    // Correctives: the correctives panel (setCorrectivesContent).
    auto* correctivesPage = new QWidget(work_);
    correctivesPage->setObjectName(QStringLiteral("correctivesPage"));
    auto* correctives = new QVBoxLayout(correctivesPage);
    correctives->setContentsMargins(0, 0, 0, 0);
    correctives->setSpacing(4);
    correctives->addWidget(label(tr("CORRECTIVES"), "heading", correctivesPage));
    correctivesHolder_ = new QWidget(correctivesPage);
    correctivesHolder_->setLayout(new QVBoxLayout());
    correctivesHolder_->layout()->setContentsMargins(0, 0, 0, 0);
    correctives->addWidget(correctivesHolder_);
    // Photos: the photos panel (setPhotosContent: photos_panel.h).
    auto* photosPage = new QWidget(work_);
    photosPage->setObjectName(QStringLiteral("photosPage"));
    auto* photos = new QVBoxLayout(photosPage);
    photos->setContentsMargins(0, 0, 0, 0);
    photosHolder_ = new QWidget(photosPage);
    photosHolder_->setLayout(new QVBoxLayout());
    photosHolder_->layout()->setContentsMargins(0, 0, 0, 0);
    photos->addWidget(photosHolder_);
    const std::map<std::string, QWidget*> pages = {
        {"tools", toolsPage}, {"paint", paintPage}, {"correctives", correctivesPage}, {"photos", photosPage}};
    for (const ui::json& tab : palette.at("tabs")) {
        const std::string id = tab.value("id", std::string());
        const auto page = pages.find(id);
        if (page == pages.end()) continue;
        tabIds_.push_back(id);
        pages_.push_back(page->second);
        const int index = tabs_->addTab(qs(tab.value("label", std::string())));
        tabs_->setTabToolTip(index, qs(tab.value("tip", std::string())));
        work->addWidget(page->second);
    }
    connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
        for (std::size_t i = 0; i < pages_.size(); ++i) pages_[i]->setVisible(static_cast<int>(i) == index);
        refreshVisibility();
        if (index >= 0 && index < static_cast<int>(tabIds_.size())) emit tabChanged(qs(tabIds_[static_cast<std::size_t>(index)]));
    });
    for (std::size_t i = 0; i < pages_.size(); ++i) pages_[i]->setVisible(i == 0);

    // ---- The active tool: its name, what it does, its card.
    heading_ = label(QString(), "heading", work_);
    heading_->setObjectName(QStringLiteral("toolHeading"));
    work->addWidget(heading_);
    hint_ = label(QString(), "about", work_);
    hint_->setObjectName(QStringLiteral("toolHint"));
    work->addWidget(hint_);
    card_ = new BrushPanel(work_);
    connect(card_, &BrushPanel::settingsChanged, this, &GroomTools::settingsChanged);
    connect(card_, &BrushPanel::guidesEdited, this, &GroomTools::guidesEdited);
    connect(card_, &BrushPanel::clearFlowRequested, this, &GroomTools::clearFlowRequested);
    connect(card_, &BrushPanel::lengthsPreviewed, this, &GroomTools::lengthsPreviewed);
    work->addWidget(card_);

    // ---- The sections (palette.sections).
    std::map<std::string, QWidget*> bodies;
    {
        // The tubes as a whole (tube_panel.h): the targets' shape and sections, the library.
        tubes_ = new TubePanel();
        connect(tubes_, &TubePanel::guidesEdited, this, &GroomTools::guidesEdited);
        connect(tubes_, &TubePanel::graphEdited, this, &GroomTools::tubesGraphEdited);
        connect(tubes_, &TubePanel::previewed, this, &GroomTools::tubesPreviewed);
        connect(tubes_, &TubePanel::settingsChanged, this, [this] {
            if (!updating_) emit settingsChanged();
        });
        connect(tubes_, &TubePanel::message, this, [this](const QString& text) { setStatus(text); });
        bodies["tubes"] = tubes_;
    }
    {
        // How the strands draw while a stroke is live: set once, rarely changed.
        auto* display = new QWidget();
        auto* layout = new QVBoxLayout(display);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        const ui::json& choice = defs().get("choices/sculptDisplay");
        auto* line = new QHBoxLayout();
        line->setSpacing(8);
        line->addWidget(label(qs(choice.value("label", std::string())), "about", display));
        sculpt_ = new QComboBox(display);
        sculpt_->setObjectName(QStringLiteral("sculptDisplay"));
        sculpt_->setToolTip(qs(choice.value("tip", std::string())));
        for (const ui::json& item : defs().choices("sculptDisplay")) {
            sculpt_->addItem(qs(item.value("label", std::string())), qs(item.value("value", std::string())));
        }
        const int start = sculpt_->findData(qs(choice.value("default", std::string("dimStrands"))));
        sculpt_->setCurrentIndex(std::max(start, 0));
        connect(sculpt_, &QComboBox::currentIndexChanged, this, [this](int) {
            if (!updating_) emit sculptDisplayChanged();
        });
        line->addWidget(sculpt_, 1);
        layout->addLayout(line);
        fullDensity_ = new QCheckBox(tr("Full density"), display);
        fullDensity_->setObjectName(QStringLiteral("sculptFullDensity"));
        fullDensity_->setToolTip(tr("Keep every strand of the preview up while a stroke is live, instead of a lighter share of them (slower "
                                    "on heavy grooms, but nothing pops)"));
        connect(fullDensity_, &QCheckBox::toggled, this, [this](bool on) {
            if (updating_ || !session_) return;
            session_->sculptDensity = on ? 1.0f : 0.35f;
            emit sculptDisplayChanged();
        });
        layout->addWidget(fullDensity_);
        bodies["sculptDisplay"] = display;
    }
    groupsHolder_ = new QWidget();
    groupsHolder_->setLayout(new QVBoxLayout());
    groupsHolder_->layout()->setContentsMargins(0, 0, 0, 0);
    bodies["groups"] = groupsHolder_;
    outputHolder_ = new QWidget();
    outputHolder_->setObjectName(QStringLiteral("groomToolsOutput"));
    outputHolder_->setLayout(new QVBoxLayout());
    outputHolder_->layout()->setContentsMargins(0, 0, 0, 0);
    bodies["output"] = outputHolder_;
    for (const ui::json& s : palette.at("sections")) {
        const std::string id = s.value("id", std::string());
        const auto body = bodies.find(id);
        if (body == bodies.end()) continue;
        auto* section = new Section(qs(s.value("label", std::string())), body->second, s.value("expanded", false), work_);
        section->setObjectName(QStringLiteral("section_") + qs(id));
        sections_[id] = section;
        sectionRules_[id] = s;
        work->addWidget(section);
    }
    if (Section* tubes = section("tubes")) {
        connect(tubes, &Section::toggled, this, [this](bool open) {
            if (open) tubes_->refresh();
        });
    }
    outer_->addStretch(1);

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
    b->setToolTip(QStringLiteral("<b>%1</b><br>%2").arg(text.toHtmlEscaped(), tip.toHtmlEscaped()));
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
    // ---- The Output strip.
    while (QLayoutItem* item = outputHolder_->layout()->takeAt(0)) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
    outputChips_.clear();
    outputHolder_->layout()->addWidget(outputStrip(bindings, options));
    // ---- The Parts box.
    card_->setBindings(bindings);
    refresh();
}

QToolButton* GroomTools::actionChip(const ui::Item& item, const ui::Bindings& bindings, QWidget* parent) {
    auto* chip = new QToolButton(parent);
    chip->setObjectName(QStringLiteral("output_") + qs(item.id));
    chip->setProperty("role", "chip");
    chip->setText(qs(item.label));
    chip->setToolTip(qs(item.tip));
    chip->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    const ui::Binding binding = bindings.at(item.id);
    connect(chip, &QToolButton::clicked, this, [binding, value = item.value] { binding.trigger(value, false); });
    if (binding.enabled) enables_.emplace_back(chip, binding.enabled);
    return chip;
}

QToolButton* GroomTools::menuChip(const ui::Item& item, const ui::Bindings& bindings, const ui::FillOptions& options, QWidget* parent) {
    auto* chip = new QToolButton(parent);
    chip->setObjectName(QStringLiteral("output_") + qs(item.id));
    chip->setProperty("role", "chip");
    chip->setText(qs(item.label) + QStringLiteral("  ▾"));
    chip->setToolTip(qs(item.tip));
    chip->setPopupMode(QToolButton::InstantPopup);
    chip->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* popup = new QMenu(chip);
    popup->setObjectName(QStringLiteral("outputMenu_") + qs(item.id));
    const std::string ref = item.entry.value("ref", std::string());
    if (!ref.empty() && defs().find(ref)) ui::fillMenu(popup, defs().get(ref), bindings, options);
    keepRenamed(popup);
    chip->setMenu(popup);
    return chip;
}

QWidget* GroomTools::outputStrip(const ui::Bindings& bindings, const ui::FillOptions& options) {
    // palette.output: the steps after grooming one click away (the GroomLab menu's actions), as
    // chips that reflow to the dock's width.
    auto* grid = new ToolGrid();
    grid->setObjectName(QStringLiteral("outputStrip"));
    ui::ResolveOptions resolve = options.resolve;
    resolve.keep = [&bindings](const std::string& id) { return bindings.count(id) > 0; };
    resolve.dropGenerators = false;
    std::vector<QAbstractButton*> chips;
    for (const ui::Item& item : defs().resolve(defs().get("palette/output"), resolve)) {
        QToolButton* chip = nullptr;
        if (item.kind == ui::Item::Kind::Action) chip = actionChip(item, bindings, grid);
        else if (item.kind == ui::Item::Kind::Submenu) chip = menuChip(item, bindings, options, grid);
        if (!chip) continue;
        chip->setMinimumSize(100, 28);  // two to a row in a narrow dock, three in a wider one
        outputChips_.push_back(chip);
        chips.push_back(chip);
    }
    grid->setButtons(std::move(chips), 3);
    return grid;
}

void GroomTools::setGroomName(const QString& name) {
    name_ = name;
    groomName_->setText(name.isEmpty() ? tr("No groom open") : name);
    groomName_->setToolTip(name.isEmpty() ? tr("No groom open") : tr("The groom every tool works on: %1").arg(name));
    refreshStarter();
    refreshVisibility();
    refresh();
}

void GroomTools::setGroups(const std::vector<GroupRow>& groups) {
    {
        const QSignalBlocker blocker(group_);
        group_->clear();
        group_->addItem(tr("All groups"), QVariantList{-1, QString()});
        for (const GroupRow& g : groups) {
            group_->addItem(QString(4 * g.depth, QLatin1Char(' ')) + qs(g.name), QVariantList{g.depth, qs(g.name)});
        }
        // A new groom: Place starts the primary group.
        if (groups.empty()) group_->addItem(QStringLiteral("primary"), QVariantList{0, QStringLiteral("primary")});
    }
    refreshGroupChoice();
}

void GroomTools::setParts(const std::vector<std::string>& names) { card_->setParts(names); }

void GroomTools::setFlowCount(std::size_t arrows, std::size_t sinks) { card_->setFlowCount(arrows, sinks); }

void GroomTools::setStarterProgress(bool guidesPlaced, bool lookGiven) {
    guidesPlaced_ = guidesPlaced;
    lookGiven_ = lookGiven;
    refreshStarter();
}

void GroomTools::setStarterDismissed(bool dismissed) {
    starterDismissed_ = dismissed;
    refreshStarter();
}

bool GroomTools::starterShown() const { return !starter_->isHidden(); }

void GroomTools::refreshStarter() {
    const bool shown = groomOpen() && !starterDismissed_ && !(guidesPlaced_ && lookGiven_);
    starter_->setVisible(shown);
    if (!shown) return;
    QString next;
    for (const ui::json& step : defs().get("palette/starter/steps")) {
        const std::string id = step.value("id", std::string());
        QPushButton* b = starterRows_.at(id);
        const bool done = id == "guides" ? guidesPlaced_ : lookGiven_;
        const QString text = qs(step.value("label", std::string()));
        if (done) {
            b->setText(QStringLiteral("✓  ") + text);
        } else {
            b->setText((next.isEmpty() ? QStringLiteral("▶  ") : QStringLiteral("○  ")) + text);
            if (next.isEmpty()) next = qs(step.value("tip", std::string()));
        }
        b->setProperty("done", done);
        b->style()->unpolish(b);
        b->style()->polish(b);
    }
    starterTip_->setText(next);
}

void GroomTools::setTool(const std::string& id) {
    tool_ = id;
    refreshTiles();
    refreshCard();
}

std::string GroomTools::isolatedGroup() const {
    if (!isolate_->isChecked()) return {};
    const QVariantList entry = group_->currentData().toList();
    return entry.size() == 2 ? entry[1].toString().toStdString() : std::string();
}

void GroomTools::refreshTiles() {
    const qreal dpr = devicePixelRatioF();
    const int iconSize = tileLabels_ ? 22 : 20;
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
    // Labeled: the name under the icon on a raised tile (Maya's tiles); else icons alone, more to
    // a row, on the panel (the names in the tool tips).
    for (QToolButton* b : allTiles_) {
        b->setIconSize(QSize(iconSize, iconSize));
        b->setToolButtonStyle(tileLabels_ ? Qt::ToolButtonTextUnderIcon : Qt::ToolButtonIconOnly);
        // One size for every tile (the grids line up): four to a row in a ~280 px dock.
        b->setMinimumSize(tileLabels_ ? QSize(54, 50) : QSize(34, 34));
        if (b->objectName() == QLatin1String("unfreezeAllTile") && b->property("iconLook").toString() != look) {
            b->setProperty("iconLook", look);
            b->setIcon(toolIcon("unfreeze", BrushCategory::Guides));
        }
        if (b->property("labels").isValid() && b->property("labels").toBool() == tileLabels_) continue;
        b->setProperty("labels", tileLabels_);
        b->style()->unpolish(b);
        b->style()->polish(b);
    }
    for (ToolGrid* g : grids_) g->setMaxColumns(tileLabels_ ? 4 : 6);
}

void GroomTools::setTileLabels(bool shown) {
    tileLabels_ = shown;
    refreshTiles();
}

void GroomTools::refreshCard() {
    const groomlab::BrushEntry* e = tool_.empty() ? nullptr : brushEntry(tool_);
    heading_->setText(e ? qs(e->label).toUpper() : tr("TOOL"));
    hint_->setText(e ? qs(e->hint) : tr("Pick a tool, then drag over the groom in the viewport."));
    card_->setTool(tool_);
    card_->setVisible(e && card_->hasSettings());
    group_->setEnabled(tool_ != "part");  // Part draws on the scalp, in no group
    // Place, Draw and Shape open the Tubes section (Maya's), once per tool picked: folded away by
    // hand, it stays so while the tool does.
    if (e && e->hasSetting("tube") && tubesOpenedFor_ != tool_) {
        if (Section* tubes = section("tubes")) tubes->setExpanded(true);
        tubes_->refresh();
    }
    tubesOpenedFor_ = e && e->hasSetting("tube") ? tool_ : std::string();
    refreshSections();
}

void GroomTools::refreshTubes() {
    const Section* tubes = section("tubes");
    if (!tubes || tubes->expanded()) tubes_->refresh();
}

void GroomTools::refresh() {
    if (session_) {
        updating_ = true;
        card_->refresh();
        refreshGroupChoice();
        refreshSymmetry();
        fullDensity_->setChecked(session_->sculptDensity >= 0.999f);
        updating_ = false;
    }
    refreshTubes();
    for (auto& [button, enabled] : enables_) {
        if (button) button->setEnabled(enabled());
    }
}

void GroomTools::refreshGroupChoice() {
    if (!session_) return;
    const BrushSettings& b = session_->settings().brush;
    int index = -1;
    for (int i = 0; i < group_->count() && index < 0; ++i) {
        const QVariantList entry = group_->itemData(i).toList();
        if (entry.size() != 2) continue;
        if (!b.group.empty() ? entry[1].toString() == qs(b.group) : (entry[0].toInt() == b.level && (b.level < 0 || i > 0))) index = i;
    }
    if (index < 0 && b.group.empty() && b.level < 0) index = 0;
    if (index < 0) return;  // (a level with no group of its own: the choice stays)
    const QSignalBlocker blocker(group_);
    group_->setCurrentIndex(index);
}

void GroomTools::groupPicked() {
    if (updating_ || !session_) return;
    const QVariantList entry = group_->currentData().toList();
    if (entry.size() != 2) return;
    BrushSettings& b = session_->settings().brush;
    b.level = entry[0].toInt();
    b.group = entry[1].toString().toStdString();
    emit settingsChanged();
    emit isolateChanged();
}

void GroomTools::refreshSymmetry() {
    if (!session_) return;
    const std::string name = session_->settings().symmetryName();
    const bool on = name != "none";
    const QSignalBlocker a(symmetry_), b(symmetryMode_);
    symmetry_->setChecked(on);
    if (on) {
        const int index = symmetryMode_->findData(qs(name));
        if (index >= 0) symmetryMode_->setCurrentIndex(index);
    }
    symmetryMode_->setEnabled(on);
    const bool world = on && name != "topology";
    centerRow_->setVisible(world);
    symmetryCenter_->setValue(session_->settings().brush.mirrorCenter);
}

void GroomTools::symmetryEdited() {
    symmetryMode_->setEnabled(symmetry_->isChecked());
    if (updating_ || !session_) return;
    const std::string name = symmetry_->isChecked() ? symmetryMode_->currentData().toString().toStdString() : std::string("none");
    session_->settings().setSymmetry(name);
    updating_ = true;
    refreshSymmetry();
    updating_ = false;
    emit settingsChanged();
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
    const bool guides = id == "guides";
    if (guides) id = "tools";
    for (const ui::json& tab : defs().get("palette/tabs")) {
        for (const ui::json& alias : tab.value("aliases", ui::json::array())) {
            if (alias == id) id = tab.value("id", std::string());
        }
    }
    const auto found = std::find(tabIds_.begin(), tabIds_.end(), id);
    if (found == tabIds_.end()) return false;
    tabs_->setCurrentIndex(static_cast<int>(found - tabIds_.begin()));
    if (guides) sections_.at("groups")->setExpanded(true);
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
    paintHolder_->layout()->addWidget(content);
    fitScroll(content);
}

void GroomTools::setCorrectivesContent(QWidget* content) {
    correctivesHolder_->layout()->addWidget(content);
    fitScroll(content);
}

void GroomTools::setPhotosContent(QWidget* content) { photosHolder_->layout()->addWidget(content); }

void GroomTools::setGroupsContent(QWidget* content) {
    groupsHolder_->layout()->addWidget(content);
    fitScroll(content);
}

Section* GroomTools::section(const std::string& id) const {
    const auto it = sections_.find(id);
    return it == sections_.end() ? nullptr : it->second;
}

int GroomTools::sculptDisplay() const { return std::max(sculpt_->currentIndex(), 0); }

void GroomTools::setSculptDisplay(int display) {
    const QSignalBlocker blocker(sculpt_);
    sculpt_->setCurrentIndex(std::clamp(display, 0, sculpt_->count() - 1));
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
    empty_->setVisible(!open);
    work_->setVisible(open);
    targetRow_->setVisible(open);
    eye_->setEnabled(open);
    guidesEye_->setEnabled(open);
    graph_->setEnabled(open);
    refreshSections();
}

bool GroomTools::sectionShown(const ui::json& section, const std::string& tool, const std::string& tab) {
    auto lists = [&section](const char* key, const std::string& value) {
        const ui::json& ids = section.contains(key) ? section.at(key) : ui::json();
        if (!ids.is_array() || ids.empty()) return true;  // no such condition
        for (const ui::json& id : ids) {
            if (id.is_string() && id.get<std::string>() == value) return true;
        }
        return false;
    };
    return lists("tools", tool) && lists("tabs", tab);
}

void GroomTools::refreshSections() {
    // Tubes with Place, Draw, Shape and Select, While sculpting with the brushes that stroke, the guide
    // groups on the Tools tab, Output always (palette.json's tools and tabs, as Maya and Blender).
    const std::string tab = currentTab().toStdString();
    for (auto& [id, section] : sections_) {
        const auto rule = sectionRules_.find(id);
        section->setVisible(rule == sectionRules_.end() || sectionShown(rule->second, tool_, tab));
    }
}

}  // namespace groomlab::app
