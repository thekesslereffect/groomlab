// SPDX-License-Identifier: Apache-2.0
#include "node_graph.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <tuple>

#include <QClipboard>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QComboBox>
#include <QCursor>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QMimeData>
#include <QPushButton>
#include <QSpinBox>
#include <QPlainTextEdit>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextEdit>
#include <QTimer>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QToolTip>
#include <QMenu>
#include <QLabel>
#include <QLineF>
#include <QLineEdit>
#include <QGraphicsDropShadowEffect>
#include <QListWidget>
#include <QStyledItemDelegate>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "glass.h"
#include "groom_document.h"
#include "theme.h"
#include "viewport.h"

#include "groomlab/graph.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_inserts.h"
#include "groomlab/graph_pins.h"
#include "groomlab/graph_portals.h"
#include "groomlab/node_catalog.h"
#include "groomlab/node_library.h"

namespace groomlab::app {

namespace {

// The Add Node search's rows: the type's name in the text color at the left, its menu category
// quiet at the right after a dot of the category's color (the row's text is "Name    Category").
class SearchRowDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QSize s = QStyledItemDelegate::sizeHint(option, index);
        s.setHeight(std::max(s.height(), theme::rowHeight()));
        return s;
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRectF row = QRectF(option.rect).adjusted(1, 1, -1, -1);
        const bool selected = option.state & QStyle::State_Selected;
        const bool hover = option.state & QStyle::State_MouseOver;
        if (selected || hover) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(selected ? theme::checkedFill(theme::blue()) : theme::uiColor("hover"));
            painter->drawRoundedRect(row, theme::smallRadius(), theme::smallRadius());
        }
        const QString text = index.data(Qt::DisplayRole).toString();
        const int split = text.indexOf(QStringLiteral("    "));
        const QString name = split < 0 ? text : text.left(split);
        const QString category = split < 0 ? QString() : text.mid(split).trimmed();
        const QRect inner = option.rect.adjusted(10, 0, -10, 0);
        int right = inner.right();
        if (!category.isEmpty()) {
            const QFont small = theme::uiFont(11, 500);
            const QFontMetrics fm(small);
            const int w = fm.horizontalAdvance(category);
            painter->setFont(small);
            painter->setPen(theme::textMuted());
            painter->drawText(QRect(right - w, inner.top(), w, inner.height()), Qt::AlignVCenter | Qt::AlignRight, category);
            const QColor dot = index.data(Qt::ForegroundRole).value<QBrush>().color();
            painter->setPen(Qt::NoPen);
            painter->setBrush(dot);
            painter->drawEllipse(QPointF(right - w - 8, inner.center().y() + 0.5), 3.0, 3.0);
            right -= w + 18;
        }
        const QFont words = theme::uiFont(13, selected ? 600 : 500);
        painter->setFont(words);
        painter->setPen(selected ? theme::checkedText(theme::text()) : theme::text());
        painter->drawText(QRect(inner.left(), inner.top(), std::max(0, right - inner.left()), inner.height()), Qt::AlignVCenter | Qt::AlignLeft,
                          QFontMetrics(words).elidedText(name, Qt::ElideRight, std::max(0, right - inner.left())));
        painter->restore();
    }
};

constexpr qreal kNodeWidth = 176.0;  // (a name of ten letters beside the time badge at the readable scale)
constexpr qreal kHeader = 28.0;
constexpr qreal kRow = 18.0;
constexpr qreal kBodyPadding = 8.0;
constexpr qreal kPortRadius = 5.0;
constexpr qreal kColumn = 230.0;  // the auto layout's columns and rows
constexpr qreal kRowGap = 120.0;
constexpr qreal kMinScale = 0.1, kMaxScale = 3.0;  // the view's zoom (the wheel, Alt+right-drag)

// Dark words on a bright header (yellow, white, emerald), white on the others; a tinted header
// (a wash of the category color) takes the theme's text.
// The header's words and marks: the text color over a wash or no fill, readable over a filled one.
QColor headerText(const QColor& fill) { return theme::headerStyle() == QLatin1String("filled") ? theme::readableOn(fill) : theme::text(); }

// Where pasted and duplicated nodes go: beside what they were copied from (Maya's paste offset).
constexpr qreal kPasteOffsetX = 40.0, kPasteOffsetY = 60.0;

// A backdrop's title bar and its resize grip (the corner), scene units, as Maya's BackdropItem.
constexpr qreal kBackdropHeader = 28.0, kBackdropGrip = 14.0;
// A pin (the wires' reroute points): its dot's radius, its nub's distance right of its center, and
// the grid Snap puts nodes and pins on (the docked canvas's lines).
constexpr qreal kPinRadius = 6.0, kPinNub = 11.0, kGrid = 24.0;
}  // namespace

NodeGraphView::NodeGraphView(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_NoSystemBackground, true);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(false);

    // The add-node search: a glass box with a filter line and the matching types.
    search_ = new GlassPanel(this, 14.0);
    // A popup: the raised tone over the canvas (its list too), so it floats.
    search_->setColor(theme::glassy() ? theme::glassSolid() : theme::uiColor("raised"));
    search_->setProperty("role", "popup");
    if (!theme::glassy()) {
        // It floats: a soft shadow under it (a child of the view, so no translucent window needed).
        auto* shadow = new QGraphicsDropShadowEffect(search_);
        shadow->setBlurRadius(24.0);
        shadow->setOffset(0.0, 8.0);
        shadow->setColor(QColor(0, 0, 0, 115));
        search_->setGraphicsEffect(shadow);
    }
    auto* layout = new QVBoxLayout(search_);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);
    searchEdit_ = new QLineEdit(search_);
    searchEdit_->setPlaceholderText(tr("Add a node..."));
    searchEdit_->setFont(theme::uiFont(13, 500));
    searchList_ = new QListWidget(search_);
    searchList_->setFont(theme::uiFont(13, 500));
    searchList_->setItemDelegate(new SearchRowDelegate(searchList_));
    searchList_->setMouseTracking(true);
    // The current type's description under the list (Maya's search shows it there too).
    searchAbout_ = new QLabel(search_);
    searchAbout_->setWordWrap(true);
    searchAbout_->setFont(theme::uiFont(11, 500));
    searchAbout_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textMuted().name()));
    searchAbout_->setMinimumHeight(44);
    searchAbout_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    layout->addWidget(searchEdit_);
    layout->addWidget(searchList_, 1);
    layout->addWidget(searchAbout_);
    search_->resize(320, 380);
    search_->hide();
    connect(searchList_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item, QListWidgetItem*) {
        if (item) searchAbout_->setText(item->toolTip());
    });
    connect(searchEdit_, &QLineEdit::textChanged, this, &NodeGraphView::filterSearch);
    connect(searchEdit_, &QLineEdit::returnPressed, this, &NodeGraphView::addFromSearch);
    connect(searchList_, &QListWidget::itemActivated, this, [this](QListWidgetItem*) { addFromSearch(); });
    searchEdit_->installEventFilter(this);
    searchList_->installEventFilter(this);

    // The name prompt (Save as Preset): a title, a line, Enter or Escape; no dialog.
    prompt_ = new GlassPanel(this, 14.0);
    prompt_->setColor(theme::glassSolid());
    auto* promptLayout = new QVBoxLayout(prompt_);
    promptLayout->setContentsMargins(14, 12, 14, 12);
    promptLayout->setSpacing(8);
    promptTitle_ = new QLabel(prompt_);
    promptTitle_->setFont(theme::uiFont(14, 800));
    promptTitle_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::text().name()));
    promptEdit_ = new QLineEdit(prompt_);
    promptEdit_->setFont(theme::uiFont(14, 600));
    auto* hint = new QLabel(tr("Enter saves, Esc cancels"), prompt_);
    hint->setFont(theme::uiFont(11, 600));
    hint->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textMuted().name()));
    promptLayout->addWidget(promptTitle_);
    promptLayout->addWidget(promptEdit_);
    promptLayout->addWidget(hint);
    prompt_->resize(300, 104);
    prompt_->hide();
    connect(promptEdit_, &QLineEdit::returnPressed, this, &NodeGraphView::acceptPrompt);
    promptEdit_->installEventFilter(this);
    setAcceptDrops(true);  // .glnode files dropped in: imported into the node library and added
}

// ---- The name prompt ----------------------------------------------------------------------------

void NodeGraphView::promptName(const QString& title, const QString& placeholder, std::function<void(const QString&)> done) {
    promptDone_ = std::move(done);
    promptTitle_->setText(title);
    promptEdit_->setPlaceholderText(placeholder);
    promptEdit_->clear();
    prompt_->move((width() - prompt_->width()) / 2, std::max(0, height() / 2 - prompt_->height()));
    prompt_->show();
    prompt_->raise();
    promptEdit_->setFocus();
}

bool NodeGraphView::promptOpen() const { return !prompt_->isHidden(); }

void NodeGraphView::acceptPrompt() {
    if (!promptOpen()) return;
    const QString text = promptEdit_->text().trimmed();
    prompt_->hide();
    setFocus();
    std::function<void(const QString&)> done;
    done.swap(promptDone_);
    if (done && !text.isEmpty()) done(text);
}

void NodeGraphView::setPromptText(const QString& text) { promptEdit_->setText(text); }

void NodeGraphView::setDocument(GroomDocument* document) {
    document_ = document;
    laidOut_.clear();
    selected_.clear();
    selection_.clear();
    selectedWire_ = -1;
    fitted_ = false;
    viewTouched_ = false;
    refresh();
}

void NodeGraphView::setViewport(Viewport* viewport) { viewport_ = viewport; }

void NodeGraphView::refresh() {
    rebuild();
    if (!fitted_ && !nodes_.empty() && width() > 0) frameReadable();
    update();
}

void NodeGraphView::rebuild() {
    nodes_.clear();
    wires_.clear();
    backdrops_.clear();
    pins_.clear();
    portals_.clear();
    if (!document_) return;
    // The backdrops as the graph keeps them (title, a rectangle, an optional color; folded).
    const nlohmann::json& graph = document_->viewGraph();
    if (graph.contains("backdrops") && graph["backdrops"].is_array()) {
        for (const nlohmann::json& b : graph["backdrops"]) {
            if (!b.is_object()) continue;
            auto number = [&](const char* key, double fallback) { return b.contains(key) && b[key].is_number() ? b[key].get<double>() : fallback; };
            Backdrop drop;
            drop.title = QString::fromStdString(b.value("title", std::string("Backdrop")));
            drop.full = QRectF(number("x", 0.0), number("y", 0.0), std::max(1.0, number("w", 200.0)), std::max(1.0, number("h", 150.0)));
            drop.folded = groomlab::backdropFolded(b);
            drop.rect = drop.full;
            if (drop.folded) {
                const LayoutBox box = foldedBackdrop({"", drop.full.x(), drop.full.y(), drop.full.width(), drop.full.height()}, kBackdropHeader);
                drop.rect = QRectF(box.x, box.y, box.w, box.h);
            }
            drop.color = theme::nodeBody().lighter(140);
            if (b.contains("color") && b["color"].is_array() && b["color"].size() == 3 && b["color"][0].is_number()) {
                drop.color = QColor(std::clamp(b["color"][0].get<int>(), 0, 255), std::clamp(b["color"][1].get<int>(), 0, 255),
                                    std::clamp(b["color"][2].get<int>(), 0, 255));
            }
            backdrops_.push_back(std::move(drop));
        }
    }
    if (selectedBackdrop_ >= static_cast<int>(backdrops_.size())) selectedBackdrop_ = -1;
    const GraphEdit edit(graph.dump());  // (which nodes have Show Tubes)
    for (const GroomDocument::NodeInfo& info : document_->nodes()) {
        Node node;
        node.name = info.name;
        node.type = info.type;
        node.tubes = edit.braidTubesOffered(info.name);
        node.viewable = edit.viewable(info.name);
        node.bypass = info.bypass;
        node.evaluation = info.evaluation;
        // The node's ports as it is set (its exposed params' after its own: Field, Curve or Value).
        if (const nlohmann::json* spec = document_->nodeSpec(info.name)) {
            node.category = spec->value("category", std::string());
            for (const nlohmann::json& p : spec->value("inputs", nlohmann::json::array())) {
                node.inputs.push_back({p.value("name", std::string()), p.value("type", std::string()), p.value("optional", false)});
            }
            // Outputs as connected (a Curves node gives what feeds it: GraphEdit::outputType).
            std::vector<std::string> types;
            try {
                types = edit.outputTypes(info.name);
            } catch (const std::exception&) {
            }
            const nlohmann::json outputs = spec->value("outputs", nlohmann::json::array());
            for (std::size_t o = 0; o < outputs.size(); ++o) {
                const std::string type = o < types.size() ? types[o] : outputs[o].value("type", std::string());
                node.outputs.push_back({outputs[o].value("name", std::string()), type, false});
            }
            for (const nlohmann::json& p : spec->value("params", nlohmann::json::array())) {
                if (p.contains("port") && p["port"].is_object()) node.linkableParams.push_back(p.value("name", std::string()));
            }
        }
        if (info.type == "Output") node.category = "output";
        if (info.type == "Compound" || info.type == "Script") {
            const nlohmann::json library = document_->libraryInfo(info.name);
            if (library.is_object()) {
                node.libraryLabel = library.value("label", std::string());
                node.updateAvailable = library.value("updateAvailable", false);
            }
        }
        // A node whose type shows under its ports (a name that is not the type, one port a side)
        // gets a row for it: the type never runs into the port names.
        const bool typeRow = node.type != node.name && node.inputs.size() <= 1 && node.outputs.size() <= 1;
        const std::size_t rows = std::max<std::size_t>(std::max(node.inputs.size(), node.outputs.size()), 1) + (typeRow ? 1 : 0);
        // (A Clump a button wider: its header holds Clump Colors too; a Braid or a node giving
        // guides, Show Tubes.)
        node.size = QSizeF(kNodeWidth + (hasOwnToggle(node) ? 23.0 : 0.0), kHeader + kBodyPadding * 2 + rows * kRow);
        float x = 0.0f, y = 0.0f;
        if (document_->nodePosition(info.name, &x, &y)) {
            node.pos = QPointF(x, y);
        } else if (const auto it = laidOut_.find(info.name); it != laidOut_.end()) {
            node.pos = it->second;
        } else {
            node.pos = QPointF(std::nan(""), 0.0);  // laid out below
        }
        nodes_.push_back(std::move(node));
    }
    autoLayout();
    // Nodes on a folded backdrop are hidden (its title bar stands for them).
    for (Node& n : nodes_) {
        for (std::size_t b = 0; b < backdrops_.size(); ++b) {
            if (backdrops_[b].folded && backdrops_[b].full.contains(QRectF(n.pos, n.size).center())) n.foldedIn = static_cast<int>(b);
        }
    }
    auto index = [this](const std::string& name) {
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            if (nodes_[i].name == name) return static_cast<int>(i);
        }
        return -1;
    };
    // The portals' dots: each entry by its source output, then its exits (graph_portals.h).
    for (const nlohmann::json& p : graph.value("portals", nlohmann::json::array())) {
        if (!p.is_object()) continue;
        Portal entry;
        entry.name = p.value("name", std::string());
        entry.pos = QPointF(p.value("x", 0.0), p.value("y", 0.0));
        entry.from = index(p.value("from", std::string()));
        if (entry.from < 0) continue;
        const std::string output = p.value("output", std::string());
        for (std::size_t o = 0; o < nodes_[entry.from].outputs.size(); ++o) {
            if (nodes_[entry.from].outputs[o].name == output) entry.output = static_cast<int>(o);
        }
        entry.color = theme::portColor(nodes_[entry.from].outputs.empty() ? std::string() : nodes_[entry.from].outputs[entry.output].type);
        portals_.push_back(entry);
        const nlohmann::json exits = p.value("exits", nlohmann::json::array());
        for (std::size_t e = 0; e < exits.size(); ++e) {
            Portal exit = entry;
            exit.exit = static_cast<int>(e);
            exit.pos = QPointF(exits[e].value("x", 0.0), exits[e].value("y", 0.0));
            portals_.push_back(exit);
        }
    }
    if (selectedPortal_ >= static_cast<int>(portals_.size())) selectedPortal_ = -1;
    for (const GroomDocument::ConnectionInfo& c : document_->connections()) {
        Wire wire;
        wire.from = index(c.from);
        wire.to = index(c.to);
        if (wire.from < 0 || wire.to < 0) continue;
        const Node& from = nodes_[wire.from];
        const Node& to = nodes_[wire.to];
        wire.output = 0;
        for (std::size_t i = 0; i < from.outputs.size(); ++i) {
            if (from.outputs[i].name == c.output) wire.output = static_cast<int>(i);
        }
        wire.input = -1;
        for (std::size_t i = 0; i < to.inputs.size(); ++i) {
            if (to.inputs[i].name == c.input) wire.input = static_cast<int>(i);
        }
        if (wire.input < 0) continue;  // a Field port the spec does not list: not drawn
        wire.hidden = from.foldedIn >= 0 && from.foldedIn == to.foldedIn;
        // Through a portal: it comes out of an exit's dot (its pins are not drawn then).
        if (const auto exit = portalOfLink(graph, c.to, c.input)) {
            for (std::size_t i = 0; i < portals_.size(); ++i) {
                if (portals_[i].name == exit->portal && portals_[i].exit == exit->exit) wire.portal = static_cast<int>(i);
            }
        }
        // The pins it runs through (each made once: a bundle's wires share theirs).
        for (const std::string& id : wire.portal >= 0 ? std::vector<std::string>() : pinRoute(graph, c.to, c.input)) {
            int at = -1;
            for (std::size_t i = 0; i < pins_.size(); ++i) {
                if (pins_[i].id == id) at = static_cast<int>(i);
            }
            if (at < 0) {
                Pin pin;
                pin.id = id;
                double x = 0.0, y = 0.0;
                pinPosition(graph, id, &x, &y);
                pin.pos = QPointF(x, y);
                pin.color = theme::portColor(from.outputs.empty() ? std::string() : from.outputs[wire.output].type);
                pin.from = wire.from;
                pin.output = wire.output;
                at = static_cast<int>(pins_.size());
                pins_.push_back(std::move(pin));
            }
            wire.pins.push_back(at);
        }
        wire.path = wirePathFor(wire);
        wires_.push_back(wire);
    }
    if (selectedWire_ >= static_cast<int>(wires_.size())) selectedWire_ = -1;
    if (selectedPin_ >= static_cast<int>(pins_.size())) selectedPin_ = -1;
    selection_.erase(std::remove_if(selection_.begin(), selection_.end(), [&](const std::string& n) { return index(n) < 0; }), selection_.end());
    if (!selected_.empty() && index(selected_) < 0) {
        selected_ = selection_.empty() ? std::string() : selection_.back();
        emit selectionChanged(selected_);
    }
    // A viewed node gone (removed, renamed): the output shows again.
    if (!viewed_.empty() && document_->scope().empty() && viewed_.find('/') == std::string::npos && index(viewed_) < 0) setViewedNode({});
}

void NodeGraphView::autoLayout() {
    // Nodes without a position: columns by how far they are from the sources (the longest path
    // in), rows in the graph's order; placed to the right of what has a position already. The
    // core's layout (graph_inserts.h layoutPositions, Maya's and Blender's too) at this view's spacing.
    std::vector<std::string> unplaced;
    for (const Node& node : nodes_) {
        if (std::isnan(node.pos.x())) unplaced.push_back(node.name);
    }
    if (unplaced.empty()) return;
    // Over the viewport (a tall view), a long chain wraps into blocks of columns, one under the
    // other, so the whole graph fits at a readable scale; docked (a wide, short strip, as Maya's
    // Graph Editor), it runs in one row and frameReadable shows its look end.
    GraphLayout layout;
    layout.column = kColumn;
    layout.row = kRowGap;
    layout.columnsPerBlock = viewport_ ? 7 : 0;
    std::map<std::string, std::pair<double, double>> placed;
    if (document_) {
        try {
            placed = layoutPositions(GraphEdit(document_->viewGraph().dump()), layout, &unplaced);
        } catch (const std::exception&) {  // (a graph the core cannot read: one column below)
        }
    }
    int row = 0;
    for (Node& node : nodes_) {
        if (!std::isnan(node.pos.x())) continue;
        const auto at = placed.find(node.name);
        node.pos = at != placed.end() ? QPointF(at->second.first, at->second.second) : QPointF(0.0, row++ * kRowGap);
        laidOut_[node.name] = node.pos;
    }
}

QPointF NodeGraphView::toScene(const QPointF& widget) const { return (widget - pan_) / scale_; }

QPointF NodeGraphView::toWidget(const QPointF& scene) const { return scene * scale_ + pan_; }

QPointF NodeGraphView::inputPos(const Node& node, int port) const {
    return QPointF(node.pos.x(), node.pos.y() + kHeader + kBodyPadding + kRow * (port + 0.5));
}

QPointF NodeGraphView::outputPos(const Node& node, int port) const {
    return QPointF(node.pos.x() + node.size.width(), node.pos.y() + kHeader + kBodyPadding + kRow * (port + 0.5));
}

QPainterPath NodeGraphView::routed(const QPointF& from, const QPointF& to, qreal below, bool through) const {
    // The wire style's route (the core's, every host's): smooth, square or rounded.
    QPainterPath path(from);
    for (const WireStep& s : wireRoute(wireStyle_, from.x(), from.y(), to.x(), to.y(), below, through)) {
        if (s.cubic) path.cubicTo(QPointF(s.c1x, s.c1y), QPointF(s.c2x, s.c2y), QPointF(s.x, s.y));
        else path.lineTo(s.x, s.y);
    }
    return path;
}

QPainterPath NodeGraphView::wirePath(const QPointF& from, const QPointF& to, qreal below) const { return routed(from, to, below, false); }

QPointF NodeGraphView::wireStart(const Wire& wire) const {
    const Node& from = nodes_[wire.from];
    if (from.foldedIn >= 0) {  // out of a folded backdrop: its right edge
        const QRectF& r = backdrops_[from.foldedIn].rect;
        return QPointF(r.right(), r.center().y());
    }
    return outputPos(from, wire.output);
}

QPointF NodeGraphView::wireEnd(const Wire& wire) const {
    const Node& to = nodes_[wire.to];
    if (to.foldedIn >= 0) {  // into a folded backdrop: its left edge
        const QRectF& r = backdrops_[to.foldedIn].rect;
        return QPointF(r.left(), r.center().y());
    }
    return inputPos(to, wire.input);
}

QPainterPath NodeGraphView::wirePathFor(const Wire& wire) const {
    const Node& from = nodes_[wire.from];
    const Node& to = nodes_[wire.to];
    if (wire.portal >= 0 && wire.portal < static_cast<int>(portals_.size())) {
        // Out of its portal's exit: from the dot to the input.
        return routed(portals_[wire.portal].pos + QPointF(kPinNub, 0.0), wireEnd(wire), std::nan(""), true);
    }
    if (!wire.pins.empty()) {
        // Through its pins: each piece leaves level and arrives level (as Maya's, and Unreal's
        // reroute nodes), wherever the pins lie.
        std::vector<QPointF> points{wireStart(wire)};
        for (int p : wire.pins) points.push_back(pins_[p].pos);
        points.push_back(wireEnd(wire));
        QPainterPath path(points.front());
        for (std::size_t i = 1; i < points.size(); ++i) path.connectPath(routed(points[i - 1], points[i], std::nan(""), true));
        return path;
    }
    const qreal below = std::max(from.pos.y() + from.size.height(), to.pos.y() + to.size.height());
    return wirePath(wireStart(wire), wireEnd(wire), below);
}

int NodeGraphView::pinAt(const QPointF& scene, bool* nub) const {
    for (int i = static_cast<int>(pins_.size()) - 1; i >= 0; --i) {
        const QPointF d = scene - pins_[i].pos;
        const bool onNub = d.x() > kPinRadius * 0.5 && d.x() < kPinNub + 5.0 && std::abs(d.y()) < kPinRadius + 3.0;
        if (onNub || d.x() * d.x() + d.y() * d.y() <= (kPinRadius + 3.0) * (kPinRadius + 3.0)) {
            if (nub) *nub = onNub;
            return i;
        }
    }
    return -1;
}

int NodeGraphView::pieceAt(const Wire& wire, const QPointF& scene) const {
    std::vector<QPointF> points{wireStart(wire)};
    for (int p : wire.pins) points.push_back(pins_[p].pos);
    points.push_back(wireEnd(wire));
    int best = 0;
    qreal nearest = std::numeric_limits<qreal>::max();
    for (std::size_t k = 1; k < points.size(); ++k) {
        const QPainterPath piece = routed(points[k - 1], points[k], std::nan(""), true);
        for (int i = 0; i <= 16; ++i) {
            const QPointF d = piece.pointAtPercent(i / 16.0) - scene;
            const qreal d2 = d.x() * d.x() + d.y() * d.y();
            if (d2 < nearest) nearest = d2, best = static_cast<int>(k) - 1;
        }
    }
    return best;
}

QPointF NodeGraphView::snappedIf(const QPointF& scene) const {
    if (!snapping_) return scene;
    return QPointF(std::round(scene.x() / kGrid) * kGrid, std::round(scene.y() / kGrid) * kGrid);
}

std::vector<std::string> NodeGraphView::pinIds() const {
    std::vector<std::string> ids;
    for (const Pin& p : pins_) ids.push_back(p.id);
    return ids;
}

QPointF NodeGraphView::pinPos(const std::string& pin) const {
    for (const Pin& p : pins_) {
        if (p.id == pin) return p.pos;
    }
    return QPointF(std::nan(""), 0.0);
}

std::string NodeGraphView::selectedPin() const { return selectedPin_ >= 0 ? pins_[selectedPin_].id : std::string(); }

void NodeGraphView::selectPin(const std::string& pin) {
    selectedPin_ = -1;
    for (std::size_t i = 0; i < pins_.size(); ++i) {
        if (pins_[i].id == pin) selectedPin_ = static_cast<int>(i);
    }
    if (selectedPin_ >= 0) {
        selectedWire_ = selectedBackdrop_ = -1;
        selectNode({});
    }
    update();
}

int NodeGraphView::nodeIndex(const std::string& name) const {
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

bool NodeGraphView::isSelected(const std::string& name) const {
    return std::find(selection_.begin(), selection_.end(), name) != selection_.end();
}

QRectF NodeGraphView::nodeRect(const std::string& node) const {
    const int i = nodeIndex(node);
    if (i < 0) return {};
    return QRectF(toWidget(nodes_[i].pos), nodes_[i].size * scale_);
}

void NodeGraphView::fitAll() {
    if (nodes_.empty() || width() <= 0 || height() <= 0) return;
    QRectF bounds;
    for (const Node& n : nodes_) bounds |= QRectF(n.pos, n.size);
    for (const Backdrop& b : backdrops_) bounds |= b.rect;
    bounds.adjust(-40, -40, 40, 40);
    // Room for the chrome around (the overlay's top row and pills; docked, a small margin).
    const QRectF view(fitMargins_.left(), fitMargins_.top(), std::max(100, width() - fitMargins_.left() - fitMargins_.right()),
                      std::max(100, height() - fitMargins_.top() - fitMargins_.bottom()));
    // Never so small that the names cannot be read: a bigger graph is centered at a readable size.
    scale_ = std::clamp(std::min(view.width() / bounds.width(), view.height() / bounds.height()), kReadableScale, 1.0);
    pan_ = view.center() - bounds.center() * scale_;
    // Taller than the view at a readable size (a docked strip): the top rows from the top (the
    // main chain and its headers), not the middle of the graph with the cards cut in half.
    if (bounds.height() * scale_ > view.height()) pan_.setY(view.top() - bounds.top() * scale_);
    fitted_ = true;
    update();
}

void NodeGraphView::frameReadable() {
    if (nodes_.empty() || width() <= 0 || height() <= 0) return;
    fitAll();
    QRectF bounds;
    for (const Node& n : nodes_) bounds |= QRectF(n.pos, n.size);
    for (const Backdrop& b : backdrops_) bounds |= b.rect;
    const QRectF view(fitMargins_.left(), fitMargins_.top(), std::max(100, width() - fitMargins_.left() - fitMargins_.right()),
                      std::max(100, height() - fitMargins_.top() - fitMargins_.bottom()));
    if (bounds.adjusted(-40, -40, 40, 40).width() * scale_ <= view.width()) return;  // it all fits
    // Too long to read whole: the look end, where look nodes go (Maya's frame_readable).
    auto nodeOf = [this](const char* type) -> const Node* {
        for (const Node& n : nodes_) {
            if (n.type == type) return &n;
        }
        return nullptr;
    };
    const Node* output = nodeOf("Output");
    const Node* start = nodeOf("Interpolate");
    const qreal outputRight = output ? output->pos.x() + output->size.width() : bounds.right();
    const qreal shown = view.width() / scale_;  // scene units across the view
    qreal x = outputRight + 40.0 - shown / 2.0;  // the output near the right edge
    if (start && outputRight - start->pos.x() + 80.0 <= shown) x = (start->pos.x() + outputRight) / 2.0;
    pan_.setX(view.center().x() - x * scale_);
    // No node cut in half at the left edge: one that would be is left out of view (its wire leads
    // to it), the view moved past it.
    for (const Node& n : nodes_) {
        const qreal left = n.pos.x() * scale_ + pan_.x(), right = (n.pos.x() + n.size.width()) * scale_ + pan_.x();
        if (left < view.left() && right > 0.0) pan_.setX(pan_.x() - right - 12.0);
    }
}

void NodeGraphView::frameSelected() {
    int index = -1;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].name == selected_) index = static_cast<int>(i);
    }
    if (index < 0 || width() <= 0 || height() <= 0) {
        fitAll();
        return;
    }
    viewTouched_ = true;  // a node framed: a resize keeps it in view as it is
    const QRectF bounds = QRectF(nodes_[index].pos, nodes_[index].size).adjusted(-60, -60, 60, 60);
    const QRectF view(fitMargins_.left(), fitMargins_.top(), std::max(100, width() - fitMargins_.left() - fitMargins_.right()),
                      std::max(100, height() - fitMargins_.top() - fitMargins_.bottom()));
    scale_ = std::clamp(std::min(view.width() / bounds.width(), view.height() / bounds.height()), kReadableScale, 1.0);
    pan_ = view.center() - bounds.center() * scale_;
    fitted_ = true;
    update();
}

void NodeGraphView::bringIntoView(const std::string& node) {
    const QRectF card = nodeRect(node).adjusted(-16, -24, 16, 16);
    if (card.isEmpty() || width() <= 0 || height() <= 0) return;
    const QRectF view = QRectF(rect());
    QPointF shift;
    if (card.left() < view.left()) shift.setX(view.left() - card.left());
    else if (card.right() > view.right()) shift.setX(std::max(view.right() - card.right(), view.left() - card.left()));
    if (card.top() < view.top()) shift.setY(view.top() - card.top());
    else if (card.bottom() > view.bottom()) shift.setY(std::max(view.bottom() - card.bottom(), view.top() - card.top()));
    if (shift.isNull()) return;
    pan_ += shift;
    viewTouched_ = true;
    update();
}

void NodeGraphView::setFitMargins(const QMargins& margins) {
    if (margins == fitMargins_) return;
    fitMargins_ = margins;
    if (fitted_ && isVisible()) frameReadable();
}

void NodeGraphView::selectNode(const std::string& name) {
    const std::vector<std::string> only = name.empty() ? std::vector<std::string>() : std::vector<std::string>{name};
    if (selectedBackdrop_ >= 0) {
        selectedBackdrop_ = -1;
        update();
    }
    if (name == selected_ && selection_ == only) return;
    selection_ = only;
    selectedWire_ = -1;
    update();
    if (name == selected_) return;
    selected_ = name;
    emit selectionChanged(selected_);
}

void NodeGraphView::setSelection(const std::vector<std::string>& names) {
    std::vector<std::string> kept;
    for (const std::string& n : names) {
        if (nodeIndex(n) >= 0 && std::find(kept.begin(), kept.end(), n) == kept.end()) kept.push_back(n);
    }
    selection_ = kept;
    selectedWire_ = -1;
    selectedBackdrop_ = -1;
    update();
    const std::string current = kept.empty() ? std::string() : kept.back();
    if (current == selected_) return;
    selected_ = current;
    emit selectionChanged(selected_);
}

void NodeGraphView::selectInRect(const QRectF& widgetRect, bool add) {
    const QRectF scene(toScene(widgetRect.topLeft()), toScene(widgetRect.bottomRight()));
    std::vector<std::string> names = add ? selection_ : std::vector<std::string>();
    for (const Node& n : nodes_) {
        if (n.foldedIn < 0 && QRectF(n.pos, n.size).intersects(scene.normalized()) && std::find(names.begin(), names.end(), n.name) == names.end()) {
            names.push_back(n.name);
        }
    }
    // The current node stays current when it is still selected (its params keep showing).
    if (!selected_.empty()) {
        const auto it = std::find(names.begin(), names.end(), selected_);
        if (it != names.end()) {
            names.erase(it);
            names.push_back(selected_);
        }
    }
    setSelection(names);
}

int NodeGraphView::nodeAt(const QPointF& scene) const {
    for (int i = static_cast<int>(nodes_.size()) - 1; i >= 0; --i) {
        if (nodes_[i].foldedIn < 0 && QRectF(nodes_[i].pos, nodes_[i].size).contains(scene)) return i;
    }
    return -1;
}

bool NodeGraphView::portAt(const QPointF& scene, int* node, int* port, bool* output) const {
    const qreal reach = kPortRadius * 2.2;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const Node& n = nodes_[i];
        if (n.foldedIn >= 0) continue;  // hidden on a folded backdrop
        for (std::size_t p = 0; p < n.outputs.size(); ++p) {
            if (QLineF(outputPos(n, static_cast<int>(p)), scene).length() <= reach) {
                *node = static_cast<int>(i);
                *port = static_cast<int>(p);
                *output = true;
                return true;
            }
        }
        for (std::size_t p = 0; p < n.inputs.size(); ++p) {
            if (QLineF(inputPos(n, static_cast<int>(p)), scene).length() <= reach) {
                *node = static_cast<int>(i);
                *port = static_cast<int>(p);
                *output = false;
                return true;
            }
        }
    }
    return false;
}

int NodeGraphView::wireAt(const QPointF& scene) const {
    QPainterPathStroker stroker;
    stroker.setWidth(10.0);
    for (std::size_t i = 0; i < wires_.size(); ++i) {
        if (!wires_[i].hidden && stroker.createStroke(wires_[i].path).contains(scene)) return static_cast<int>(i);
    }
    return -1;
}

bool NodeGraphView::wireLink(int index, std::string* from, std::string* output, std::string* to, std::string* input) const {
    if (index < 0 || index >= wireCount()) return false;
    const Wire& w = wires_[index];
    const Node& a = nodes_[w.from];
    const Node& b = nodes_[w.to];
    *from = a.name;
    *output = w.output == 0 || w.output >= static_cast<int>(a.outputs.size()) ? std::string() : a.outputs[w.output].name;
    *to = b.name;
    *input = b.inputs[w.input].name;
    return true;
}

int NodeGraphView::wireUnderNode(int node) const {
    if (node < 0 || node >= nodeCount()) return -1;
    for (const Wire& w : wires_) {
        if (w.from == node || w.to == node) return -1;  // a node with wires of its own stays where its wires are
    }
    // The wires any part of the card lies over, the one passing nearest its middle (Maya's
    // edge_under_node): sampled along each wire.
    const QRectF card(nodes_[node].pos, nodes_[node].size);
    const QPointF middle = card.center();
    int best = -1;
    qreal nearest = std::numeric_limits<qreal>::max();
    for (std::size_t i = 0; i < wires_.size(); ++i) {
        bool over = false;
        qreal d = std::numeric_limits<qreal>::max();
        for (int k = 0; k <= 48; ++k) {
            const QPointF p = wires_[i].path.pointAtPercent(k / 48.0);
            over = over || card.contains(p);
            d = std::min(d, QLineF(middle, p).length());
        }
        if (over && d < nearest) {
            nearest = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

QRectF NodeGraphView::bypassButtonRect(const Node& node) const {
    return QRectF(node.pos.x() + node.size.width() - 26, node.pos.y() + 5, 18, 18);
}

QRectF NodeGraphView::inputsButtonRect(const Node& node) const {
    // Left of the View eye when the node has one (Expand Inputs, View, Bypass), else of Bypass.
    return QRectF(node.pos.x() + node.size.width() - (viewable(node) ? 72.0 : 49.0), node.pos.y() + 5.0, 18.0, 18.0);
}

QRectF NodeGraphView::viewButtonRect(const Node& node) const {
    // Between Expand Inputs and Bypass: the header's toggles read Expand, View, Bypass.
    return bypassButtonRect(node).translated(-23.0, 0.0);
}

QRectF NodeGraphView::clumpColorsButtonRect(const Node& node) const {
    // Left of the leftmost toggle: Clump Colors, Expand Inputs, View, Bypass.
    const QRectF left = !node.linkableParams.empty() ? inputsButtonRect(node) : viewable(node) ? viewButtonRect(node) : bypassButtonRect(node);
    return left.translated(-23.0, 0.0);
}

bool NodeGraphView::clumpColorsButtonAt(const QPointF& scene, int* node) const {
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (clumpColorable(nodes_[i]) && clumpColorsButtonRect(nodes_[i]).adjusted(-2, -2, 2, 2).contains(scene)) {
            *node = static_cast<int>(i);
            return true;
        }
    }
    return false;
}

bool NodeGraphView::braidTubesButtonAt(const QPointF& scene, int* node) const {
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (showsTubes(nodes_[i]) && braidTubesButtonRect(nodes_[i]).adjusted(-2, -2, 2, 2).contains(scene)) {
            *node = static_cast<int>(i);
            return true;
        }
    }
    return false;
}

void NodeGraphView::setBraidTubeNodes(std::vector<std::string> nodes) {
    if (nodes == braidTubes_) return;
    braidTubes_ = std::move(nodes);
    update();
}

void NodeGraphView::setClumpColoredNodes(std::vector<std::string> nodes) {
    if (nodes == clumpColored_) return;
    clumpColored_ = std::move(nodes);
    update();
}

bool NodeGraphView::viewable(const Node& node) const { return node.viewable; }

bool NodeGraphView::viewButtonAt(const QPointF& scene, int* node) const {
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (viewable(nodes_[i]) && viewButtonRect(nodes_[i]).adjusted(-2, -2, 2, 2).contains(scene)) {
            *node = static_cast<int>(i);
            return true;
        }
    }
    return false;
}

int NodeGraphView::backdropTitleAt(const QPointF& scene, bool* grip) const {
    for (int i = static_cast<int>(backdrops_.size()) - 1; i >= 0; --i) {
        const QRectF& r = backdrops_[i].rect;
        if (QRectF(r.right() - kBackdropGrip, r.bottom() - kBackdropGrip, kBackdropGrip, kBackdropGrip).contains(scene)) {
            if (grip) *grip = true;
            return i;
        }
        if (QRectF(r.topLeft(), QSizeF(r.width(), kBackdropHeader)).contains(scene)) {
            if (grip) *grip = false;
            return i;
        }
    }
    return -1;
}

int NodeGraphView::backdropAt(const QPointF& scene) const {
    for (int i = static_cast<int>(backdrops_.size()) - 1; i >= 0; --i) {
        if (backdrops_[i].rect.contains(scene)) return i;
    }
    return -1;
}

bool NodeGraphView::bypassButtonAt(const QPointF& scene, int* node) const {
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (bypassButtonRect(nodes_[i]).contains(scene)) {
            *node = static_cast<int>(i);
            return true;
        }
    }
    return false;
}

bool NodeGraphView::inputsButtonAt(const QPointF& scene, int* node) const {
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (!nodes_[i].linkableParams.empty() && inputsButtonRect(nodes_[i]).contains(scene)) {
            *node = static_cast<int>(i);
            return true;
        }
    }
    return false;
}

// ---- Painting -------------------------------------------------------------------------------------

void NodeGraphView::drawNode(QPainter& painter, const Node& node, bool selected) {
    const QRectF rect(node.pos, node.size);
    const bool failed = errors_.count(pathOf(node.name)) > 0;
    const QColor color = theme::nodeCategoryColor(node.category);
    const qreal radius = theme::nodeRadius();
    painter.setOpacity(node.bypass ? 0.5 : 1.0);
    // The body, then the header clipped to the top corners: filled with the category color, or
    // (a tinted theme) a wash of it under an accent bar, as the Maya Graph Editor draws it.
    QPainterPath body;
    body.addRoundedRect(rect, radius, radius);
    painter.setPen(Qt::NoPen);
    if (!viewport_ && theme::nodeShadow()) {
        // A soft shadow under the card (a few widening rounded rects, faint): the nodes sit over the canvas.
        for (int k = 3; k >= 1; --k) {
            painter.setBrush(QColor(0, 0, 0, 22));
            const qreal grow = 1.5 * k;
            painter.drawRoundedRect(rect.adjusted(-grow, -grow + 3.0, grow, grow + 3.0), radius + grow, radius + grow);
        }
    }
    painter.setBrush(theme::nodeBody());
    painter.drawPath(body);
    const QString header = theme::headerStyle();
    const bool minimal = header == QLatin1String("minimal");
    painter.save();
    painter.setClipPath(body);
    if (minimal) {
        // No header fill (the design's minimal cards): the category is the dot by the title.
    } else if (header == QLatin1String("tinted")) {
        // The category's color washing down from the top edge, fading out under the title.
        QLinearGradient wash(rect.topLeft(), QPointF(rect.x(), rect.y() + kHeader));
        QColor top = color, bottom = color;
        top.setAlpha(selected ? 112 : 96);
        bottom.setAlpha(30);
        wash.setColorAt(0.0, top);
        wash.setColorAt(1.0, bottom);
        painter.setBrush(wash);
        painter.drawRect(QRectF(rect.topLeft(), QSizeF(rect.width(), kHeader)));
        painter.setBrush(color);
        painter.drawRect(QRectF(rect.topLeft(), QSizeF(rect.width(), 2.0)));
    } else {
        painter.setBrush(color);
        painter.drawRect(QRectF(rect.topLeft(), QSizeF(rect.width(), kHeader)));
    }
    painter.restore();
    // A thin border where the theme gives the nodes one (its color other than the body's).
    if (const QColor border = theme::nodeBorder(); border.isValid() && border != theme::nodeBody()) {
        QPen edge(border, 1.0);
        edge.setCosmetic(true);  // one screen pixel at any zoom: the token's color, not smeared over two
        painter.setPen(edge);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), radius - 0.5, radius - 0.5);
    }
    // The name, with the type after it when the name says something else; on a minimal card
    // after a dot of the category's color.
    qreal titleX = rect.x() + 12;
    if (minimal) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(QPointF(rect.x() + 14, rect.y() + kHeader / 2.0), 4.0, 4.0);
        titleX = rect.x() + 24;
    }
    // The status badges in the title row, right to left before the header toggles (Expand Inputs,
    // Bypass): the cook time, the View eye, a warning triangle; the name gets the room left of them.
    const NodeBadges badges = nodeBadges(node.name);
    const bool filled = header == QLatin1String("filled");
    const qreal row = rect.y() + kHeader / 2.0;
    qreal x = (hasOwnToggle(node) ? clumpColorsButtonRect(node)
               : !node.linkableParams.empty() ? inputsButtonRect(node)
               : viewable(node) ? viewButtonRect(node)
                                : bypassButtonRect(node))
                  .left() -
              4.0;
    const QFont number = theme::numberFont(11);
    if (!badges.time.isEmpty()) {
        const qreal w = QFontMetricsF(number).horizontalAdvance(badges.time);
        QColor ink = badges.timeColor;
        if (filled) {  // on a filled header: its readable color, softer
            ink = headerText(color);
            ink.setAlpha(190);
        }
        painter.setFont(number);
        painter.setPen(ink);
        painter.drawText(QRectF(x - w - 1.0, rect.y(), w + 1.0, kHeader), Qt::AlignVCenter | Qt::AlignRight, badges.time);
        x -= w + 6.0;
    }
    if (badges.viewed && !viewable(node)) {
        // An eye: the viewport shows the graph up to here (View Up to Here; a viewable node's is its header toggle).
        const QPointF c(x - 7.0, row);
        QPainterPath eye;
        eye.moveTo(c + QPointF(-6.5, 0.0));
        eye.quadTo(c + QPointF(0.0, -6.5), c + QPointF(6.5, 0.0));
        eye.quadTo(c + QPointF(0.0, 6.5), c + QPointF(-6.5, 0.0));
        const QColor blue = filled ? headerText(color) : theme::blue();
        QPen pen(blue, 1.4);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(eye);
        painter.setPen(Qt::NoPen);
        painter.setBrush(blue);
        painter.drawEllipse(c, 2.0, 2.0);
        x -= 18.0;
    }
    if (badges.warning) {
        // A warning triangle: the node does nothing as set (its note in the tool tip and the panel).
        const QPointF c(x - 6.0, row);
        const qreal size = 12.0, h = size * 0.9;
        QPen pen(filled ? headerText(color) : theme::graphWarning(), 1.4);
        pen.setJoinStyle(Qt::RoundJoin);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPolygon(QPolygonF({c + QPointF(0.0, -h / 2.0), c + QPointF(size / 2.0, h / 2.0), c + QPointF(-size / 2.0, h / 2.0)}));
        painter.drawLine(c + QPointF(0.0, -h * 0.12), c + QPointF(0.0, h * 0.18));
        painter.setPen(Qt::NoPen);
        painter.setBrush(pen.color());
        painter.drawEllipse(c + QPointF(0.0, h * 0.34), 0.9, 0.9);
        x -= 17.0;
    }
    painter.setFont(theme::uiFont(14, 600));  // (drawn at the view's scale: about 11 px at the readable 0.8)
    painter.setPen(headerText(color));
    const QRectF title(titleX, rect.y(), std::max(24.0, x - titleX - 2.0), kHeader);
    painter.drawText(title, Qt::AlignVCenter | Qt::AlignLeft,
                     QFontMetricsF(painter.font()).elidedText(QString::fromStdString(node.name), Qt::ElideRight, title.width()));
    // When it runs, unless always: a tag under the card.
    if (node.evaluation == "viewportOnly" || node.evaluation == "renderOnly") {
        painter.setFont(theme::uiFont(11, 500));
        painter.setPen(theme::textMuted());
        painter.drawText(QRectF(rect.x() + 4, rect.bottom() + 2, rect.width() - 8, 16), Qt::AlignLeft | Qt::AlignVCenter,
                         node.evaluation == "viewportOnly" ? tr("viewport only") : tr("render only"));
    }
    // The bypass toggle in the header's corner: a bypass glyph (a slashed circle), dim until the
    // node is bypassed, then lit on a tile of its ink.
    const QRectF bypass = bypassButtonRect(node);
    {
        QColor ink = headerText(color);
        if (node.bypass) {
            QColor tile = ink;
            tile.setAlpha(46);
            painter.setPen(Qt::NoPen);
            painter.setBrush(tile);
            painter.drawRoundedRect(bypass, 4.0, 4.0);
        } else {
            ink.setAlpha(120);
        }
        QPen pen(ink, node.bypass ? 1.8 : 1.5);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        const QRectF ring = bypass.adjusted(4, 4, -4, -4);
        painter.drawEllipse(ring);
        const qreal k = ring.width() / 2.0 * 0.7071;
        painter.drawLine(ring.center() + QPointF(-k, k), ring.center() + QPointF(k, -k));
    }
    // The View toggle (an eye, as Maya's Graph Editor's Focus): the viewport shows the groom up to this
    // node; dim until it is the viewed one, then lit on a tile.
    if (viewable(node)) {
        const QRectF eyeRect = viewButtonRect(node);
        const bool on = !viewed_.empty() && viewed_ == pathOf(node.name);
        QColor ink = filled ? headerText(color) : (on ? theme::uiColor("accent") : theme::textDim());
        if (on) {
            QColor tile = ink;
            tile.setAlpha(60);
            painter.setPen(Qt::NoPen);
            painter.setBrush(tile);
            painter.drawRoundedRect(eyeRect, 4.0, 4.0);
        } else {
            ink.setAlpha(filled ? 140 : 170);
        }
        const QPointF c = eyeRect.center();
        QPainterPath eye;
        eye.moveTo(c + QPointF(-6.5, 0.0));
        eye.quadTo(c + QPointF(0.0, -6.0), c + QPointF(6.5, 0.0));
        eye.quadTo(c + QPointF(0.0, 6.0), c + QPointF(-6.5, 0.0));
        QPen pen(ink, on ? 1.8 : 1.5);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(eye);
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        painter.drawEllipse(c, 2.2, 2.2);
    }
    // A Clump's Clump Colors toggle (three dots of a clump, in three hues): the strands colored by
    // this node's clumps; dim until they are, then lit on a tile.
    if (clumpColorable(node)) {
        const QRectF dots = clumpColorsButtonRect(node);
        const bool on = clumpColored(node.name);
        QColor ink = filled ? headerText(color) : (on ? theme::uiColor("accent") : theme::textDim());
        if (on) {
            QColor tile = ink;
            tile.setAlpha(60);
            painter.setPen(Qt::NoPen);
            painter.setBrush(tile);
            painter.drawRoundedRect(dots, 4.0, 4.0);
        } else {
            ink.setAlpha(filled ? 140 : 170);
        }
        const QPointF c = dots.center();
        const QPointF at[3] = {c + QPointF(0.0, -3.4), c + QPointF(-3.6, 2.6), c + QPointF(3.6, 2.6)};
        const QColor hues[3] = {QColor(240, 110, 90), QColor(90, 200, 120), QColor(100, 150, 245)};
        for (int k = 0; k < 3; ++k) {
            if (on) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(hues[k]);
            } else {
                QPen pen(ink, 1.3);
                painter.setPen(pen);
                painter.setBrush(Qt::NoBrush);
            }
            painter.drawEllipse(at[k], 2.6, 2.6);
        }
    }
    // Show Tubes (a short tube: its open end and its two sides): a Braid's section tubes, or the
    // tubes of the guides a node gives, drawn in the viewport; dim until they are, then lit on a tile.
    if (showsTubes(node)) {
        const QRectF tube = braidTubesButtonRect(node);
        const bool on = braidTubesShown(node.name);
        QColor ink = filled ? headerText(color) : (on ? theme::uiColor("accent") : theme::textDim());
        if (on) {
            QColor tile = ink;
            tile.setAlpha(60);
            painter.setPen(Qt::NoPen);
            painter.setBrush(tile);
            painter.drawRoundedRect(tube, 4.0, 4.0);
        } else {
            ink.setAlpha(filled ? 140 : 170);
        }
        const QPointF c = tube.center();
        QPen pen(ink, 1.4);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        // Lying left to right: the far end's half ellipse, the two sides, the near end's whole one.
        const QRectF far(c.x() - 6.6, c.y() - 4.0, 4.4, 8.0), near(c.x() + 2.2, c.y() - 4.0, 4.4, 8.0);
        painter.drawArc(far, 90 * 16, 180 * 16);
        painter.drawLine(QPointF(far.center().x(), far.top()), QPointF(near.center().x(), near.top()));
        painter.drawLine(QPointF(far.center().x(), far.bottom()), QPointF(near.center().x(), near.bottom()));
        if (on) painter.setBrush(ink);
        painter.drawEllipse(near);
    }
    if (!node.linkableParams.empty()) {
        const QRectF inputs = inputsButtonRect(node);
        const bool expanded = std::all_of(node.linkableParams.begin(), node.linkableParams.end(), [&](const std::string& p) {
            return document_ && document_->paramExposed(node.name, p);
        });
        QColor ink = headerText(color);
        if (!expanded) ink.setAlpha(170);
        painter.setPen(QPen(ink, 1.6));
        for (qreal dy : {5.0, 9.0, 13.0}) {
            painter.drawLine(QPointF(inputs.left() + 4.0, inputs.top() + dy), QPointF(inputs.right() - 4.0, inputs.top() + dy));
        }
    }
    // The ports: inputs down the left edge, outputs down the right, named inside.
    painter.setFont(theme::uiFont(13, 500));
    for (std::size_t i = 0; i < node.inputs.size(); ++i) {
        const QPointF at = inputPos(node, static_cast<int>(i));
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme::portColor(node.inputs[i].type));
        painter.drawEllipse(at, kPortRadius, kPortRadius);
        painter.setPen(node.inputs[i].optional ? theme::textMuted() : theme::textDim());
        painter.drawText(QRectF(at.x() + 10, at.y() - kRow / 2, rect.width() / 2, kRow), Qt::AlignVCenter | Qt::AlignLeft,
                         QString::fromStdString(node.inputs[i].name));
    }
    for (std::size_t i = 0; i < node.outputs.size(); ++i) {
        const QPointF at = outputPos(node, static_cast<int>(i));
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme::portColor(node.outputs[i].type));
        painter.drawEllipse(at, kPortRadius, kPortRadius);
        painter.setPen(theme::textDim());
        painter.drawText(QRectF(at.x() - 10 - rect.width() / 2, at.y() - kRow / 2, rect.width() / 2, kRow), Qt::AlignVCenter | Qt::AlignRight,
                         QString::fromStdString(node.outputs[i].name));
    }
    if (node.type != node.name && node.inputs.size() <= 1 && node.outputs.size() <= 1) {
        // Room under the ports for the type, dim.
        painter.setPen(theme::textMuted());
        painter.setFont(theme::uiFont(11, 500));
        painter.drawText(QRectF(rect.x(), rect.bottom() - 16, rect.width(), 14), Qt::AlignCenter,
                         QString::fromStdString(node.libraryLabel.empty() ? nodeLabel(node.type) : node.libraryLabel));
    }
    if (node.updateAvailable) {
        // The node library has a newer copy of it: a mark on its left corner (right-click: Update Node).
        const QPointF corner(rect.left(), rect.top());
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme::blueLight());
        painter.drawEllipse(corner, 8.0, 8.0);
        painter.setFont(theme::uiFont(10, 900));
        painter.setPen(theme::readableOn(theme::blueLight()));
        painter.drawText(QRectF(corner.x() - 8.0, corner.y() - 8.0, 16.0, 16.0), Qt::AlignCenter, QString(QChar(0x2191)));
    }
    painter.setOpacity(1.0);
    // Failed (its evaluation threw): a red ring and a red "!" on the corner, as Maya's error badge.
    if (failed) {
        painter.setPen(QPen(theme::graphError(), 2.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(rect.adjusted(-3, -3, 3, 3), radius + 3, radius + 3);
        const QPointF corner(rect.right(), rect.top());
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme::graphError());
        painter.drawEllipse(corner, 9.0, 9.0);
        painter.setFont(theme::uiFont(12, 900));
        painter.setPen(theme::readableOn(theme::graphError()));
        painter.drawText(QRectF(corner.x() - 9.0, corner.y() - 9.0, 18.0, 18.0), Qt::AlignCenter, QStringLiteral("!"));
    }
    if (!viewed_.empty() && viewed_ == pathOf(node.name)) {
        // In Focus (the viewport shows the groom up to here): a glow of the accent around the card,
        // a solid ring and a tag over its corner, so it reads from across the graph.
        const QColor accent = theme::uiColor("accent");
        for (int k = 4; k >= 1; --k) {
            QColor glow = accent;
            glow.setAlpha(18 + 14 * (4 - k));
            painter.setPen(QPen(glow, 3.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(rect.adjusted(-2.0 - 2.5 * k, -2.0 - 2.5 * k, 2.0 + 2.5 * k, 2.0 + 2.5 * k), radius + 2.0 + 2.5 * k,
                                    radius + 2.0 + 2.5 * k);
        }
        painter.setPen(QPen(accent, 2.5));
        painter.drawRoundedRect(rect.adjusted(-3, -3, 3, 3), radius + 3, radius + 3);
        painter.setFont(theme::uiFont(10, 800));
        const QString tag = tr("FOCUS");
        const qreal w = QFontMetricsF(painter.font()).horizontalAdvance(tag) + 12.0;
        const QRectF chip(rect.left() + 10.0, rect.top() - 18.0, w, 15.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(accent);
        painter.drawRoundedRect(chip, 7.5, 7.5);
        painter.setPen(theme::readableOn(accent));
        painter.drawText(chip, Qt::AlignCenter, tag);
    }
    if (selected) {
        painter.setPen(QPen(theme::nodeSelected(), node.name == selected_ ? 2.5 : 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(rect.adjusted(-1, -1, 1, 1), radius + 1, radius + 1);
    }
}

QRectF NodeGraphView::backdropFoldButton(const Backdrop& b) const {
    return QRectF(b.rect.right() - 26.0, b.rect.top() + 5.0, 18.0, 18.0);
}

int NodeGraphView::portalAt(const QPointF& scene, bool* nub) const {
    for (int i = static_cast<int>(portals_.size()) - 1; i >= 0; --i) {
        const QPointF d = scene - portals_[i].pos;
        const bool onNub = portals_[i].exit >= 0 && d.x() > kPinRadius * 0.5 && d.x() < kPinNub + 5.0 && std::abs(d.y()) < kPinRadius + 3.0;
        if (onNub || d.x() * d.x() + d.y() * d.y() <= (kPinRadius + 4.0) * (kPinRadius + 4.0)) {
            if (nub) *nub = onNub;
            return i;
        }
    }
    return -1;
}

std::string NodeGraphView::pathOf(const std::string& name) const {
    // Inside a compound, its nodes evaluate by their path ("compound/inner": flattenCompounds).
    return document_ && !document_->scope().empty() && !name.empty() ? document_->scope() + "/" + name : name;
}

std::vector<std::pair<QString, QRectF>> NodeGraphView::scopeBar() const {
    // Inside a compound: "Graph > compound > inner" at the top left, each segment a chip.
    std::vector<std::pair<QString, QRectF>> out;
    if (!document_ || document_->scope().empty()) return out;
    std::vector<QString> names{tr("Graph")};
    for (const QString& part : QString::fromStdString(document_->scope()).split(QLatin1Char('/'))) names.push_back(part);
    const QFontMetricsF metrics(theme::uiFont(12, 700));
    qreal x = 12.0;
    const qreal y = fitMargins_.top() > 40 ? 52.0 : 10.0;
    for (const QString& name : names) {
        const QRectF chip(x, y, metrics.horizontalAdvance(name) + 20.0, 24.0);
        out.emplace_back(name, chip);
        x = chip.right() + 18.0;
    }
    return out;
}

void NodeGraphView::drawScopeBar(QPainter& painter) {
    // The path bar (scopeBar), each segment clickable to go back up to it; a thin frame of the
    // accent round the view says you are inside a compound.
    scopeBar_ = scopeBar();
    if (scopeBar_.empty()) return;
    QColor frame = theme::uiColor("accent");
    frame.setAlpha(140);
    painter.setPen(QPen(frame, 2.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF(rect()).adjusted(1, 1, -1, -1));
    painter.setFont(theme::uiFont(12, 700));
    for (std::size_t i = 0; i < scopeBar_.size(); ++i) {
        const auto& [name, chip] = scopeBar_[i];
        const bool last = i + 1 == scopeBar_.size();
        QColor fill = last ? theme::uiColor("accent") : theme::glassSolid();
        fill.setAlpha(last ? 230 : 220);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(chip, 12.0, 12.0);
        painter.setPen(last ? theme::readableOn(theme::uiColor("accent")) : theme::text());
        painter.drawText(chip, Qt::AlignCenter, name);
        if (!last) {
            painter.setPen(theme::textMuted());
            painter.drawText(QRectF(chip.right() + 4.0, chip.top(), 10.0, chip.height()), Qt::AlignCenter, QStringLiteral("›"));
        }
    }
    const QRectF& end = scopeBar_.back().second;
    painter.setFont(theme::uiFont(11, 500));
    painter.setPen(theme::textMuted());
    painter.drawText(QRectF(end.right() + 10.0, end.top(), 260.0, end.height()), Qt::AlignVCenter | Qt::AlignLeft, tr("U: up a level"));
}

void NodeGraphView::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    if (viewport_) {
        // Over the viewport: it dims behind the graph, so the cards and wires read over a busy groom.
        QColor dim = theme::backdrop();
        dim.setAlpha(110);
        painter.fillRect(rect(), dim);
    } else {
        // Docked: the theme's opaque canvas and its grid (graph.canvasStyle: lines, a line every 24
        // scene units and a stronger one every 5; dots, one at every crossing; or plain), thinned
        // out as the view zooms out so it never turns to a wash.
        painter.fillRect(rect(), theme::graphCanvas());
        const QString canvas = theme::canvasStyle();
        const bool dots = canvas == QLatin1String("dots");
        qreal step = (dots ? 20.0 : 24.0) * scale_;
        int every = 5;
        while (step < (dots ? 12.0 : 10.0)) {
            step *= dots ? 2.0 : 5.0;
            every = 1;
        }
        const QColor major = theme::graphGrid();
        QColor minor = major;
        minor.setAlphaF(major.alphaF() * 0.45f);
        // Line (or dot) k sits at pan + k * step; the first on screen is k = ceil(-pan / step).
        auto first = [&](qreal pan) { return static_cast<long long>(std::ceil(-pan / step)); };
        if (dots) {
            // A dot of the grid's color at each crossing, as the design's canvas (1 px at 100%).
            const qreal r = std::clamp(scale_, 0.75, 1.5);
            QList<QPointF> points;
            for (long long i = first(pan_.x());; ++i) {
                const qreal x = pan_.x() + static_cast<qreal>(i) * step;
                if (x > width() + r) break;
                for (long long j = first(pan_.y());; ++j) {
                    const qreal y = pan_.y() + static_cast<qreal>(j) * step;
                    if (y > height() + r) break;
                    points.push_back(QPointF(x, y));
                }
            }
            painter.setPen(QPen(major, 2.0 * r, Qt::SolidLine, Qt::RoundCap));
            painter.drawPoints(points.constData(), static_cast<int>(points.size()));
        } else if (canvas == QLatin1String("lines")) {
            painter.setRenderHint(QPainter::Antialiasing, false);
            auto lines = [&](qreal pan, int extent, bool vertical) {
                for (long long k = first(pan);; ++k) {
                    const qreal at = pan + static_cast<qreal>(k) * step;
                    if (at > extent) break;
                    painter.setPen(QPen(k % every == 0 ? major : minor, 1));
                    if (vertical) painter.drawLine(QPointF(at, 0), QPointF(at, height()));
                    else painter.drawLine(QPointF(0, at), QPointF(width(), at));
                }
            };
            lines(pan_.x(), width(), true);
            lines(pan_.y(), height(), false);
            painter.setRenderHint(QPainter::Antialiasing, true);
        }
    }
    painter.save();
    painter.translate(pan_);
    painter.scale(scale_, scale_);
    // The backdrops first: a tinted frame with its title bar, fold chevron and resize grip (as
    // Maya's), the groom still showing through; the selected one ringed. A folded one is its title
    // bar alone, saying how many nodes it holds.
    for (std::size_t i = 0; i < backdrops_.size(); ++i) {
        const Backdrop& b = backdrops_[i];
        const bool selected = static_cast<int>(i) == selectedBackdrop_;
        const qreal radius = theme::nodeRadius();
        QColor fill = b.color;
        fill.setAlpha(b.folded ? 150 : 46);
        painter.setPen(selected ? QPen(theme::nodeSelected(), 2.0) : QPen(QColor(b.color.red(), b.color.green(), b.color.blue(), 120), 1.5));
        painter.setBrush(fill);
        painter.drawRoundedRect(b.rect, radius, radius);
        QPainterPath frame;
        frame.addRoundedRect(b.rect, radius, radius);
        painter.save();
        painter.setClipPath(frame);
        QColor header = b.color;
        header.setAlpha(120);
        painter.fillRect(QRectF(b.rect.topLeft(), QSizeF(b.rect.width(), kBackdropHeader)), header);
        painter.restore();
        painter.setFont(theme::uiFont(14, 800));
        painter.setPen(theme::textSoft());
        const QRectF fold = backdropFoldButton(b);
        const QRectF title(b.rect.left() + 12, b.rect.top(), fold.left() - b.rect.left() - 16, kBackdropHeader);
        painter.drawText(title, Qt::AlignVCenter | Qt::AlignLeft, QFontMetricsF(painter.font()).elidedText(b.title, Qt::ElideRight, title.width()));
        // The fold chevron: down while open, right while folded.
        QPen chevron(theme::textSoft(), 1.8);
        chevron.setCapStyle(Qt::RoundCap);
        chevron.setJoinStyle(Qt::RoundJoin);
        painter.setPen(chevron);
        painter.setBrush(Qt::NoBrush);
        const QPointF c = fold.center();
        if (b.folded) painter.drawPolyline(QPolygonF({c + QPointF(-2.5, -5.0), c + QPointF(2.5, 0.0), c + QPointF(-2.5, 5.0)}));
        else painter.drawPolyline(QPolygonF({c + QPointF(-5.0, -2.5), c + QPointF(0.0, 2.5), c + QPointF(5.0, -2.5)}));
        if (b.folded) {
            int held = 0;
            for (const Node& n : nodes_) held += n.foldedIn == static_cast<int>(i) ? 1 : 0;
            painter.setFont(theme::uiFont(11, 600));
            painter.setPen(theme::textMuted());
            painter.drawText(QRectF(b.rect.left() + 12, b.rect.top() + kBackdropHeader - 4, b.rect.width() - 24, b.rect.height() - kBackdropHeader + 4),
                             Qt::AlignVCenter | Qt::AlignLeft, tr("%n node(s) inside", "", held));
            continue;
        }
        painter.setPen(QPen(b.color.lighter(130), 1.5));
        for (const qreal k : {4.0, 8.0, 12.0}) {
            painter.drawLine(QPointF(b.rect.right() - k, b.rect.bottom() - 2), QPointF(b.rect.right() - 2, b.rect.bottom() - k));
        }
    }
    // A portal's entry hangs off its source output by a short wire.
    for (const Portal& p : portals_) {
        if (p.exit >= 0 || p.from < 0 || nodes_[p.from].foldedIn >= 0) continue;
        QColor color = p.color;
        color.setAlpha(220);
        painter.setPen(QPen(color, 2.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(routed(outputPos(nodes_[p.from], p.output), p.pos - QPointF(kPinRadius, 0.0), std::nan(""), true));
    }
    // The wires, the one being dragged with them, then the nodes, the selected one last.
    for (std::size_t i = 0; i < wires_.size(); ++i) {
        const Wire& w = wires_[i];
        if (w.hidden) continue;  // inside a folded backdrop
        if (dragWireInputNode_ == w.to && dragWireInputPort_ == w.input && drag_ == Drag::Wire) continue;  // picked up
        if (drag_ == Drag::Bundle && w.from == dragWireFrom_ && w.output == dragWirePort_) continue;  // picked up with its output's others
        const bool selected = static_cast<int>(i) == selectedWire_ || static_cast<int>(i) == dropWire_ || static_cast<int>(i) == searchWire_;
        // The signal's color: the output's type; an output of any curves (or no known type) takes the
        // more specific type of the input it feeds (Strands into an output: the strands' color).
        std::string type = nodes_[w.from].outputs[w.output].type;
        if (w.input >= 0 && w.input < static_cast<int>(nodes_[w.to].inputs.size())) {
            const std::string& into = nodes_[w.to].inputs[w.input].type;
            const auto generic = [](const std::string& t) { return !theme::isPortType(t) || t == "Curves" || t == "curves"; };
            if (generic(type) && !generic(into)) type = into;
        }
        QColor color = theme::portColor(type);
        color.setAlpha(selected ? 255 : 220);
        QPen pen(selected ? theme::nodeSelected() : color, selected ? 3.0 : 2.0);
        if (wireDashed(static_cast<int>(i))) {
            // Into or out of a bypassed node: short dashes at the card's opacity.
            QColor faded = pen.color();
            faded.setAlphaF(faded.alphaF() * 0.5f);
            pen.setColor(faded);
            pen.setDashPattern({2.0, 1.5});
        }
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(w.path);
    }
    // The pins over the wires: a dot in the signal's color, its nub (where wires are dragged out)
    // to its right; the selected one ringed.
    for (std::size_t i = 0; i < pins_.size(); ++i) {
        const Pin& p = pins_[i];
        const bool selected = static_cast<int>(i) == selectedPin_;
        painter.setPen(QPen(selected ? theme::nodeSelected() : p.color.darker(160), 2.0));
        painter.setBrush(p.color);
        painter.drawEllipse(p.pos, kPinRadius, kPinRadius);
        painter.setPen(Qt::NoPen);
        painter.drawPolygon(QPolygonF({p.pos + QPointF(kPinNub - 3.0, -4.0), p.pos + QPointF(kPinNub + 3.0, 0.0),
                                       p.pos + QPointF(kPinNub - 3.0, 4.0)}));
    }
    // The portals' dots: a ring in the signal's color, named beside it (the entry by its source,
    // the exits where the links come out); an exit's nub drags another link of the signal out.
    for (std::size_t i = 0; i < portals_.size(); ++i) {
        const Portal& p = portals_[i];
        const bool selected = static_cast<int>(i) == selectedPortal_;
        painter.setPen(QPen(selected ? theme::nodeSelected() : p.color, 2.5));
        painter.setBrush(p.exit < 0 ? p.color.darker(180) : p.color);
        painter.drawEllipse(p.pos, kPinRadius + 1.0, kPinRadius + 1.0);
        if (p.exit >= 0) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(p.color);
            painter.drawPolygon(QPolygonF({p.pos + QPointF(kPinNub - 3.0, -4.0), p.pos + QPointF(kPinNub + 3.0, 0.0),
                                           p.pos + QPointF(kPinNub - 3.0, 4.0)}));
        }
        painter.setFont(theme::uiFont(11, 700));
        painter.setPen(theme::textSoft());
        const QString name = QString::fromStdString(p.name);
        const qreal w = QFontMetricsF(painter.font()).horizontalAdvance(name);
        // The entry's name right of it (its wire comes in from the left), an exit's left of it.
        const QRectF label = p.exit < 0 ? QRectF(p.pos.x() + kPinRadius + 5.0, p.pos.y() - 8.0, w + 2.0, 16.0)
                                        : QRectF(p.pos.x() - kPinRadius - 7.0 - w, p.pos.y() - 8.0, w + 2.0, 16.0);
        painter.drawText(label, Qt::AlignVCenter | Qt::AlignLeft, name);
    }
    if (drag_ == Drag::Wire && dragWireFrom_ >= 0) {
        QColor color = theme::portColor(nodes_[dragWireFrom_].outputs[dragWirePort_].type);
        painter.setPen(QPen(color, 2.0, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        const QPointF start = dragWirePin_ >= 0      ? pins_[dragWirePin_].pos
                              : dragWirePortal_ >= 0 ? portals_[dragWirePortal_].pos + QPointF(kPinNub, 0.0)
                                                     : outputPos(nodes_[dragWireFrom_], dragWirePort_);
        painter.drawPath(wirePath(start, dragWireTo_));
    }
    if (drag_ == Drag::Bundle && dragWireFrom_ >= 0) {
        // Every link of the output picked up at once (Shift+drag): from the cursor to each input.
        QColor color = theme::portColor(nodes_[dragWireFrom_].outputs[dragWirePort_].type);
        painter.setPen(QPen(color, 2.0, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        for (const Wire& w : wires_) {
            if (w.from == dragWireFrom_ && w.output == dragWirePort_) painter.drawPath(wirePath(dragWireTo_, wireEnd(w)));
        }
    }
    // The nodes, the selected ones last (the current one on top); those on folded backdrops hidden.
    for (const Node& n : nodes_) {
        if (!isSelected(n.name) && n.foldedIn < 0) drawNode(painter, n, false);
    }
    for (const Node& n : nodes_) {
        if (isSelected(n.name) && n.name != selected_ && n.foldedIn < 0) drawNode(painter, n, true);
    }
    if (const int current = nodeIndex(selected_); current >= 0 && nodes_[current].foldedIn < 0) drawNode(painter, nodes_[current], true);
    painter.restore();
    drawScopeBar(painter);
    // The rubber band.
    if (drag_ == Drag::Band && !band_.isNull()) {
        QColor fill = theme::nodeSelected();
        fill.setAlpha(28);
        painter.setPen(QPen(theme::nodeSelected(), 1.0, Qt::DashLine));
        painter.setBrush(fill);
        painter.drawRect(band_.normalized());
    }
    // Why the dragged wire cannot connect here, by the cursor.
    if (drag_ == Drag::Wire && !wireHint_.isEmpty()) {
        painter.setFont(theme::uiFont(12, 700));
        const QFontMetricsF metrics(painter.font());
        const QPointF at = toWidget(dragWireTo_) + QPointF(14, 14);
        QRectF box(at, QSizeF(std::min(360.0, metrics.horizontalAdvance(wireHint_) + 20.0), 26.0));
        if (box.right() > width() - 8) box.moveRight(width() - 8);
        QColor fill = theme::glassSolid();
        fill.setAlpha(235);
        painter.setPen(QPen(theme::graphError(), 1.2));
        painter.setBrush(fill);
        painter.drawRoundedRect(box, 8, 8);
        painter.setPen(theme::errorText());
        painter.drawText(box.adjusted(10, 0, -10, 0), Qt::AlignVCenter | Qt::AlignLeft,
                         metrics.elidedText(wireHint_, Qt::ElideRight, box.width() - 20));
    }
}

// ---- Mouse ---------------------------------------------------------------------------------------

void NodeGraphView::forwardToViewport(QEvent* event) {
    // The graph covers the viewport exactly, so the positions carry over as they are.
    if (viewport_) QCoreApplication::sendEvent(viewport_, event);
}

void NodeGraphView::mousePressEvent(QMouseEvent* event) {
    setFocus();
    if (searchOpen()) closeSearch();
    if (promptOpen()) {
        prompt_->hide();
        promptDone_ = nullptr;
    }
    last_ = event->position();
    pressScene_ = toScene(last_);
    // The path bar (inside a compound): a click goes back up to that level.
    if (event->button() == Qt::LeftButton) {
        const auto bar = scopeBar();
        for (std::size_t i = 0; i < bar.size(); ++i) {
            if (!bar[i].second.contains(last_)) continue;
            std::string path;
            const QStringList parts = QString::fromStdString(document_ ? document_->scope() : std::string()).split(QLatin1Char('/'));
            for (std::size_t k = 0; k < i; ++k) path += (path.empty() ? "" : "/") + parts[static_cast<int>(k)].toStdString();
            if (document_ && path != document_->scope()) {
                document_->setScope(path);
                selectNode({});
                fitted_ = false;
                laidOut_.clear();
                refresh();
            }
            event->accept();
            return;
        }
    }
    // Alt+click (no drag) on a wire puts a pin there: remembered here, decided on the release.
    altPressed_ = event->button() == Qt::LeftButton && event->modifiers() == Qt::AltModifier;
    altPress_ = last_;
    if (event->modifiers().testFlag(Qt::AltModifier) && viewport_) {
        drag_ = Drag::Camera;
        forwardToViewport(event);
        return;
    }
    event->accept();
    zoomEnded_ = false;
    if (event->button() == Qt::RightButton && event->modifiers().testFlag(Qt::AltModifier)) {
        // Alt+right-drag zooms, as Maya's viewports and Graph Editor: right or down zooms in.
        drag_ = Drag::Zoom;
        zoomPress_ = last_;
        zoomAnchor_ = pressScene_;
        zoomStart_ = scale_;
        setCursor(Qt::SizeVerCursor);
        return;
    }
    if (event->button() == Qt::MiddleButton || event->modifiers().testFlag(Qt::AltModifier)) {
        drag_ = Drag::Pan;
        return;
    }
    if (event->button() != Qt::LeftButton) return;
    selectedPin_ = -1;
    selectedPortal_ = -1;
    dragWirePin_ = -1;
    dragWirePortal_ = -1;
    int node = -1, port = -1;
    bool output = false;
    if (bypassButtonAt(pressScene_, &node)) {
        setBypass(nodes_[node].name, !nodes_[node].bypass);
        return;
    }
    if (clumpColorsButtonAt(pressScene_, &node)) {
        emit clumpColorsToggled(nodes_[node].name);
        return;
    }
    if (braidTubesButtonAt(pressScene_, &node)) {
        emit braidTubesToggled(nodes_[node].name);
        return;
    }
    if (viewButtonAt(pressScene_, &node)) {
        // The eye: the viewport shows the groom up to this node; on the viewed one, the output again.
        const std::string name = pathOf(nodes_[node].name);
        if (viewed_ == name) {
            setViewedNode({});
            emit message(tr("Viewing the graph's output again"));
        } else {
            setViewedNode(name);
            emit message(tr("Viewing up to %1 (click the eye again, or V: the output)").arg(QString::fromStdString(name)));
        }
        update();
        return;
    }
    if (inputsButtonAt(pressScene_, &node)) {
        toggleInputs(nodes_[node].name);
        return;
    }
    if (portAt(pressScene_, &node, &port, &output)) {
        if (output && event->modifiers().testFlag(Qt::ShiftModifier)) {
            // Shift+drag from an output: every link it feeds picked up at once, to drop on another
            // output (Substance Designer's): they read that one instead.
            bool any = false;
            for (const Wire& w : wires_) any = any || (w.from == node && w.output == port);
            if (any) {
                drag_ = Drag::Bundle;
                dragWireFrom_ = node;
                dragWirePort_ = port;
                dragWireInputNode_ = dragWireInputPort_ = -1;
                dragWireTo_ = pressScene_;
                update();
                return;
            }
        }
        if (output) {
            drag_ = Drag::Wire;
            dragWireFrom_ = node;
            dragWirePort_ = port;
            dragWireInputNode_ = dragWireInputPort_ = -1;
            dragWireTo_ = pressScene_;
        } else {
            // A fed input: its wire is picked up from its source end.
            for (const Wire& w : wires_) {
                if (w.to == node && w.input == port) {
                    drag_ = Drag::Wire;
                    dragWireFrom_ = w.from;
                    dragWirePort_ = w.output;
                    dragWireInputNode_ = node;
                    dragWireInputPort_ = port;
                    dragWireTo_ = pressScene_;
                    break;
                }
            }
        }
        update();
        return;
    }
    // A portal's dot: an exit's nub drags a link of its signal out, the rest of it moves it.
    {
        bool nub = false;
        if (const int dot = portalAt(pressScene_, &nub); dot >= 0) {
            if (nub && portals_[dot].from >= 0) {
                drag_ = Drag::Wire;
                dragWireFrom_ = portals_[dot].from;
                dragWirePort_ = portals_[dot].output;
                dragWireInputNode_ = dragWireInputPort_ = -1;
                dragWirePortal_ = dot;
                dragWireTo_ = pressScene_;
            } else {
                selectNode({});
                selectedWire_ = selectedBackdrop_ = -1;
                selectedPortal_ = dot;
                drag_ = Drag::Portal;
                dragPortal_ = dot;
                portalStart_ = portals_[dot].pos;
                if (document_) document_->beginLayout();
            }
            update();
            return;
        }
    }
    // A pin: its nub drags a wire of its signal out (Blueprints'), the rest of it moves it.
    bool nub = false;
    if (const int pin = pinAt(pressScene_, &nub); pin >= 0) {
        if (nub && pins_[pin].from >= 0) {
            drag_ = Drag::Wire;
            dragWireFrom_ = pins_[pin].from;
            dragWirePort_ = pins_[pin].output;
            dragWireInputNode_ = dragWireInputPort_ = -1;
            dragWirePin_ = pin;
            dragWireTo_ = pressScene_;
        } else {
            selectPin(pins_[pin].id);
            drag_ = Drag::Pin;
            dragPin_ = pin;
            pinStart_ = pins_[pin].pos;
            if (document_) document_->beginLayout();
        }
        update();
        return;
    }
    node = nodeAt(pressScene_);
    if (node >= 0) {
        const std::string& name = nodes_[node].name;
        const Qt::KeyboardModifiers mods = event->modifiers();
        if (mods.testFlag(Qt::ControlModifier)) {
            // Ctrl toggles the node in the selection; no move.
            std::vector<std::string> names = selection_;
            const auto it = std::find(names.begin(), names.end(), name);
            if (it != names.end()) names.erase(it);
            else names.push_back(name);
            setSelection(names);
            return;
        }
        drag_ = Drag::Move;
        dragNode_ = node;
        moveStart_ = nodes_[node].pos;
        shiftPress_ = mods.testFlag(Qt::ShiftModifier);
        shaken_ = false;
        shake_.reset();
        shakeClock_.start();
        // A node of the selection drags the whole selection; another one is selected alone. With
        // Shift the selection stays as it is until the release (a click adds the node to it), and a
        // drag carries the node and everything upstream of it (Houdini's).
        std::vector<std::string> carried;
        if (shiftPress_) {
            std::vector<std::string> roots = isSelected(name) ? selection_ : std::vector<std::string>{name};
            if (document_) {
                try {
                    carried = nodesWithUpstream(GraphEdit(document_->viewGraph().dump()), roots);
                } catch (const std::exception&) {
                    carried = roots;
                }
            }
        } else if (isSelected(name)) {
            std::vector<std::string> names = selection_;
            names.erase(std::find(names.begin(), names.end(), name));
            names.push_back(name);
            setSelection(names);
            carried = selection_;
        } else {
            selectNode(name);
            carried = {name};
        }
        moving_.clear();
        for (const std::string& n : carried) {
            const int i = nodeIndex(n);
            if (i >= 0 && nodes_[i].foldedIn < 0) moving_.push_back(i);
        }
        if (std::find(moving_.begin(), moving_.end(), node) == moving_.end()) moving_.push_back(node);
        // The backdrops holding moved nodes grow with them (Houdini's network boxes).
        growing_.clear();
        for (std::size_t b = 0; b < backdrops_.size(); ++b) {
            if (backdrops_[b].folded) continue;
            Growing grow{static_cast<int>(b), backdrops_[b].rect, {}};
            for (int i : moving_) {
                if (backdrops_[b].rect.contains(QRectF(nodes_[i].pos, nodes_[i].size).center())) grow.nodes.push_back(i);
            }
            if (!grow.nodes.empty()) growing_.push_back(std::move(grow));
        }
        if (document_) document_->beginLayout();
        return;
    }
    // A backdrop's title bar: its chevron folds it; else selected and moved with the nodes on it;
    // its corner: resized.
    bool grip = false;
    if (const int b = backdropTitleAt(pressScene_, &grip); b >= 0) {
        if (backdropFoldButton(backdrops_[b]).contains(pressScene_)) {
            setBackdropFolded(b, !backdrops_[b].folded);
            return;
        }
        selectBackdrop(b);
        grip = grip && !backdrops_[b].folded;
        drag_ = grip ? Drag::BackdropResize : Drag::Backdrop;
        dragBackdrop_ = b;
        backdropStart_ = backdrops_[b].full;
        carried_.clear();
        if (!grip) {
            for (std::size_t i = 0; i < nodes_.size(); ++i) {
                if (backdropStart_.contains(QRectF(nodes_[i].pos, nodes_[i].size).center())) carried_.push_back(static_cast<int>(i));
            }
        }
        if (document_) document_->beginLayout();
        return;
    }
    const int wire = wireAt(pressScene_);
    if (wire >= 0) {
        selectedWire_ = wire;
        selectedBackdrop_ = -1;
        selection_.clear();
        if (!selected_.empty()) {
            selected_.clear();
            emit selectionChanged(selected_);
        }
        emit status(tr("Wire selected: Tab adds a node into it, Delete removes it"));
        update();
        return;
    }
    // Empty space (a backdrop's body too): a rubber band (a click alone deselects, unless Ctrl or
    // Shift keeps the selection).
    drag_ = Drag::Band;
    band_ = QRectF(last_, last_);
    bandAdds_ = event->modifiers().testFlag(Qt::ControlModifier) || event->modifiers().testFlag(Qt::ShiftModifier);
    selectedWire_ = -1;
    if (!bandAdds_) selectNode({});
    update();
}

void NodeGraphView::growBackdrops() {
    // Each backdrop holding moved nodes grows to hold those still on it (a card pushed to its edge
    // pushes the edge out); one dragged off it leaves it.
    for (const Growing& g : growing_) {
        std::vector<LayoutBox> held;
        for (int i : g.nodes) {
            const QRectF card(nodes_[i].pos, nodes_[i].size);
            const QRectF reach = g.start.adjusted(-card.width() * 0.5, -card.height() * 0.5, card.width() * 0.5, card.height() * 0.5);
            if (reach.contains(card.center()) || backdrops_[g.backdrop].rect.contains(card.center())) {
                held.push_back({nodes_[i].name, card.x(), card.y(), card.width(), card.height()});
            }
        }
        const LayoutBox grown = grownBackdrop({"", g.start.x(), g.start.y(), g.start.width(), g.start.height()}, held, 24.0, kBackdropHeader);
        backdrops_[g.backdrop].rect = backdrops_[g.backdrop].full = QRectF(grown.x, grown.y, grown.w, grown.h);
    }
}

void NodeGraphView::mouseMoveEvent(QMouseEvent* event) {
    const QPointF now = event->position();
    const QPointF delta = now - last_;
    last_ = now;
    if (altPressed_ && (now - altPress_).manhattanLength() > 4.0) altPressed_ = false;  // a drag, not a click
    switch (drag_) {
        case Drag::Camera:
            forwardToViewport(event);
            return;
        case Drag::Pan:
            pan_ += delta;
            viewTouched_ = true;
            update();
            break;
        case Drag::Zoom: {
            // The zoom doubles every ~140 px right or down; the point pressed stays under the press.
            const qreal moved = (now.x() - zoomPress_.x()) + (now.y() - zoomPress_.y());
            scale_ = std::clamp(zoomStart_ * std::exp(moved * 0.005), kMinScale, kMaxScale);
            pan_ = zoomPress_ - zoomAnchor_ * scale_;
            viewTouched_ = true;
            update();
            break;
        }
        case Drag::Move:
            if (dragNode_ >= 0 && dragNode_ < static_cast<int>(nodes_.size())) {
                // The pressed node goes where the cursor took it (on the grid with Snap); the rest
                // of what it carries by the same step; the wires follow.
                const QPointF step = snappedIf(moveStart_ + (toScene(now) - pressScene_)) - nodes_[dragNode_].pos;
                for (int i : moving_) nodes_[i].pos += step;
                for (Wire& w : wires_) {
                    if (std::find(moving_.begin(), moving_.end(), w.from) != moving_.end() || std::find(moving_.begin(), moving_.end(), w.to) != moving_.end()) {
                        w.path = wirePathFor(w);
                    }
                }
                growBackdrops();
                // Shaken (left and right, quickly): the nodes are pulled out of their links, the drag
                // goes on (Houdini's).
                if (!shaken_ && document_ && shake_.add(shakeClock_.elapsed() / 1000.0, now.x(), now.y())) {
                    shaken_ = true;
                    std::vector<std::string> names;
                    std::vector<std::pair<std::string, QPointF>> at;
                    for (int i : moving_) {
                        names.push_back(nodes_[i].name);
                        at.emplace_back(nodes_[i].name, nodes_[i].pos);
                    }
                    const std::string pressed = nodes_[dragNode_].name;
                    document_->endLayout("move nodes");
                    int detached = 0;
                    const std::string error = document_->detachNodes(names, &detached);
                    if (error.empty() && detached > 0) {
                        emit status(tr("Shaken loose: %n node(s) out of their links (Ctrl+Z puts them back)", "", detached));
                        emit edited();
                        refresh();
                        // Where the drag has them (the detach put them below their new link).
                        moving_.clear();
                        for (const auto& [name, pos] : at) {
                            const int i = nodeIndex(name);
                            if (i < 0) continue;
                            nodes_[i].pos = pos;
                            moving_.push_back(i);
                        }
                        dragNode_ = nodeIndex(pressed);
                        for (Wire& w : wires_) w.path = wirePathFor(w);
                    }
                    document_->beginLayout();
                }
                // A lone node with no wires of its own over a wire: the wire lights up (dropped: it
                // goes in, as Maya's Graph Editor).
                dropWire_ = moving_.size() == 1 && dragNode_ >= 0 ? wireUnderNode(dragNode_) : -1;
                update();
            }
            break;
        case Drag::Wire:
        case Drag::Bundle:
            dragWireTo_ = toScene(now);
            if (drag_ == Drag::Wire) updateWireHint(dragWireTo_);
            update();
            break;
        case Drag::Pin:
            if (dragPin_ >= 0 && dragPin_ < static_cast<int>(pins_.size())) {
                pins_[dragPin_].pos = snappedIf(pinStart_ + (toScene(now) - pressScene_));
                for (Wire& w : wires_) {
                    if (std::find(w.pins.begin(), w.pins.end(), dragPin_) != w.pins.end()) w.path = wirePathFor(w);
                }
                update();
            }
            break;
        case Drag::Portal:
            if (dragPortal_ >= 0 && dragPortal_ < static_cast<int>(portals_.size())) {
                portals_[dragPortal_].pos = snappedIf(portalStart_ + (toScene(now) - pressScene_));
                for (Wire& w : wires_) {
                    if (w.portal == dragPortal_) w.path = wirePathFor(w);
                }
                update();
            }
            break;
        case Drag::Band:
            band_.setBottomRight(now);
            update();
            break;
        case Drag::Backdrop:
        case Drag::BackdropResize:
            if (dragBackdrop_ >= 0 && dragBackdrop_ < static_cast<int>(backdrops_.size())) {
                const QPointF moved = toScene(now) - pressScene_;
                Backdrop& b = backdrops_[dragBackdrop_];
                if (drag_ == Drag::BackdropResize) {
                    b.full.setSize(QSizeF(std::max(120.0, backdropStart_.width() + moved.x()), std::max(80.0, backdropStart_.height() + moved.y())));
                    b.rect = b.full;
                } else {
                    const QPointF step = backdropStart_.topLeft() + moved - b.full.topLeft();
                    b.full.moveTopLeft(backdropStart_.topLeft() + moved);
                    b.rect.translate(step);
                    for (int i : carried_) nodes_[i].pos += step;
                    for (Wire& w : wires_) {
                        if (std::find(carried_.begin(), carried_.end(), w.from) != carried_.end() ||
                            std::find(carried_.begin(), carried_.end(), w.to) != carried_.end()) {
                            w.path = wirePathFor(w);
                        }
                    }
                }
                update();
            }
            break;
        case Drag::None:
            break;
    }
    event->accept();
}

void NodeGraphView::mouseReleaseEvent(QMouseEvent* event) {
    const Drag drag = drag_;
    drag_ = Drag::None;
    const bool altClick = altPressed_ && event->button() == Qt::LeftButton;
    altPressed_ = false;
    if (altClick && document_) {
        // Alt+click on a wire (no drag): a pin there, as a double-click puts one.
        const QPointF scene = toScene(event->position());
        if (const int w = nodeAt(scene) < 0 && pinAt(scene) < 0 ? wireAt(scene) : -1; w >= 0) {
            if (drag == Drag::Camera) forwardToViewport(event);
            pinWire(w, scene);
            event->accept();
            return;
        }
    }
    if (drag == Drag::Camera) {
        forwardToViewport(event);
        return;
    }
    event->accept();
    if (drag == Drag::Zoom) {
        // The release that ends a zoom opens no menu (the context menu event that follows it).
        unsetCursor();
        zoomEnded_ = true;
        return;
    }
    if (drag == Drag::Band) {
        band_.setBottomRight(event->position());
        if (band_.width() * band_.width() + band_.height() * band_.height() > 16.0) selectInRect(band_, bandAdds_);
        band_ = QRectF();
        update();
        return;
    }
    if ((drag == Drag::Backdrop || drag == Drag::BackdropResize) && dragBackdrop_ >= 0 && document_) {
        // Where it was put, and the nodes it carried: one undo step, nothing evaluates.
        if (dragBackdrop_ < static_cast<int>(backdrops_.size()) && backdrops_[dragBackdrop_].full != backdropStart_) {
            const QRectF& r = backdrops_[dragBackdrop_].full;
            document_->setBackdropRect(dragBackdrop_, r.x(), r.y(), r.width(), r.height());
            for (int i : carried_) {
                document_->setNodePosition(nodes_[i].name, static_cast<float>(nodes_[i].pos.x()), static_cast<float>(nodes_[i].pos.y()));
                laidOut_.erase(nodes_[i].name);
            }
        }
        document_->endLayout(drag == Drag::Backdrop ? "move backdrop" : "resize backdrop");
        dragBackdrop_ = -1;
        carried_.clear();
        return;
    }
    if (drag == Drag::Portal && dragPortal_ >= 0 && document_) {
        const int dot = dragPortal_;
        dragPortal_ = -1;
        if (dot < static_cast<int>(portals_.size()) && portals_[dot].pos != portalStart_) {
            const Portal& p = portals_[dot];
            if (p.exit < 0) document_->movePortal(p.name, p.pos.x(), p.pos.y());
            else document_->movePortalExit(p.name, p.exit, p.pos.x(), p.pos.y());
        }
        document_->endLayout("move portal");
        return;
    }
    if (drag == Drag::Pin && dragPin_ >= 0 && document_) {
        // Dropped on another pin of its signal: they join (an undo step); else it stays where it
        // was put (a layout step).
        const int pin = dragPin_;
        dragPin_ = -1;
        if (pin < static_cast<int>(pins_.size()) && pins_[pin].pos != pinStart_) {
            int onto = -1;
            for (std::size_t i = 0; i < pins_.size(); ++i) {
                const QPointF d = pins_[i].pos - pins_[pin].pos;
                if (static_cast<int>(i) != pin && d.x() * d.x() + d.y() * d.y() <= 4.0 * kPinRadius * kPinRadius) onto = static_cast<int>(i);
            }
            if (onto >= 0) {
                document_->endLayout("move pin");
                const std::string error = document_->mergePins(pins_[onto].id, pins_[pin].id);
                if (error.empty()) {
                    emit status(tr("Pins joined: the wires run through one pin"));
                    emit edited();
                } else {
                    emit message(tr("Pins not joined: %1").arg(QString::fromStdString(error)));
                }
                refresh();
                return;
            }
            document_->movePin(pins_[pin].id, pins_[pin].pos.x(), pins_[pin].pos.y());
        }
        document_->endLayout("move pin");
        return;
    }
    if (drag == Drag::Move && dragNode_ >= 0 && document_) {
        const bool movedAtAll = dragNode_ < static_cast<int>(nodes_.size()) && (nodes_[dragNode_].pos - moveStart_).manhattanLength() > 0.0;
        if (shiftPress_ && !movedAtAll && !shaken_) {
            // Shift+click without a move: the node added to the selection (the current one).
            const std::string name = nodes_[dragNode_].name;
            std::vector<std::string> names = selection_;
            const auto it = std::find(names.begin(), names.end(), name);
            if (it != names.end()) names.erase(it);
            names.push_back(name);
            setSelection(names);
        }
        shiftPress_ = false;
        // Every moved node keeps where it was put, the backdrops grown round them: one undo step.
        if (movedAtAll) {
            for (int i : moving_) {
                document_->setNodePosition(nodes_[i].name, static_cast<float>(nodes_[i].pos.x()), static_cast<float>(nodes_[i].pos.y()));
                laidOut_.erase(nodes_[i].name);
            }
            for (const Growing& g : growing_) {
                const QRectF& r = backdrops_[g.backdrop].full;
                if (r != g.start) document_->setBackdropRect(g.backdrop, r.x(), r.y(), r.width(), r.height());
            }
        }
        document_->endLayout(moving_.size() == 1 ? "move " + nodes_[dragNode_].name : "move " + std::to_string(moving_.size()) + " nodes");
        growing_.clear();
        // A lone unwired node dropped on a wire goes into it (Maya's insert_on_wire).
        const int wire = dropWire_;
        const std::string name = nodes_[dragNode_].name;
        dropWire_ = -1;
        dragNode_ = -1;
        moving_.clear();
        GroomDocument::NodeLink link;
        if (wire >= 0 && wireLink(wire, &link.from, &link.output, &link.to, &link.input)) {
            const std::string error = document_->insertOnLink(name, link);
            if (error.empty()) {
                emit status(tr("Inserted %1 between %2 and %3")
                                .arg(QString::fromStdString(name), QString::fromStdString(link.from), QString::fromStdString(link.to)));
                emit edited();
            } else {
                emit message(tr("Not inserted: %1").arg(QString::fromStdString(error)));
            }
            refresh();
        }
        update();
        return;
    }
    if (drag == Drag::Bundle && document_) {
        // The output's links dropped on another output: they read it instead (where they fit).
        const QPointF at = toScene(event->position());
        int node = -1, port = -1;
        bool output = false;
        if (portAt(at, &node, &port, &output) && output && !(node == dragWireFrom_ && port == dragWirePort_)) {
            const Node& from = nodes_[dragWireFrom_];
            const Node& to = nodes_[node];
            int moved = 0;
            const std::string error = document_->moveOutputLinks(from.name, dragWirePort_ == 0 ? std::string() : from.outputs[dragWirePort_].name,
                                                                 to.name, port == 0 ? std::string() : to.outputs[port].name, &moved);
            if (error.empty()) {
                emit status(tr("%n link(s) moved to %1", "", moved).arg(QString::fromStdString(to.name)));
                emit edited();
            } else {
                emit message(QString::fromStdString(error));
            }
        } else if (!(node == dragWireFrom_ && port == dragWirePort_)) {
            emit message(tr("Drop the links on another node's output (the ports on its right)"));
        }
        dragWireFrom_ = dragWirePort_ = -1;
        refresh();
        return;
    }
    if (drag == Drag::Wire && document_) {
        const QPointF at = toScene(event->position());
        int node = -1, port = -1;
        bool output = false;
        std::string error;
        if (portAt(at, &node, &port, &output)) {
            if (output || (dragWireInputNode_ == node && dragWireInputPort_ == port)) {
                // On an output, or back where it was: nothing changes (a picked-up wire goes back).
                if (output && node != dragWireFrom_) error = tr("Drop the wire on an input (the ports on a node's left)").toStdString();
            } else if (const QString problem = connectionProblem(dragWireFrom_, dragWirePort_, node, port); !problem.isEmpty()) {
                error = problem.toStdString();  // refused: the wire as it was
            } else {
                const Node& from = nodes_[dragWireFrom_];
                const Node& to = nodes_[node];
                const bool first = dragWirePort_ == 0;
                if (dragWirePortal_ >= 0) {  // out of a portal's exit: through it
                    error = document_->linkFromPortal(portals_[dragWirePortal_].name, portals_[dragWirePortal_].exit, to.name, to.inputs[port].name);
                } else if (dragWirePin_ >= 0) {  // out of a pin: through the pins up to it
                    error = document_->linkFromPin(pins_[dragWirePin_].id, to.name, to.inputs[port].name);
                } else {
                    error = document_->connectNodes(from.name, first ? std::string() : from.outputs[dragWirePort_].name, to.name,
                                                    to.inputs[port].name);
                }
                if (error.empty()) emit edited();
            }
        } else if (const int dot = dragWirePortal_ < 0 && dragWirePin_ < 0 && dragWireInputNode_ >= 0 ? portalAt(at) : -1;
                   dot >= 0 && portals_[dot].exit >= 0) {
            // A picked-up input's wire dropped on a portal's exit: that input reads the portal.
            error = document_->linkFromPortal(portals_[dot].name, portals_[dot].exit, nodes_[dragWireInputNode_].name,
                                              nodes_[dragWireInputNode_].inputs[dragWireInputPort_].name);
            if (error.empty()) emit edited();
        } else if (const int pin = dragWirePin_ < 0 && dragWirePortal_ < 0 ? pinAt(at) : -1; pin >= 0) {
            // Onto a pin: a picked-up input's wire takes the pin's signal; a wire from an output
            // feeds every wire through the pin from there (an undo step).
            if (dragWireInputNode_ >= 0) {
                error = document_->linkFromPin(pins_[pin].id, nodes_[dragWireInputNode_].name,
                                               nodes_[dragWireInputNode_].inputs[dragWireInputPort_].name);
            } else {
                const Node& from = nodes_[dragWireFrom_];
                error = document_->feedPin(pins_[pin].id, from.name, dragWirePort_ == 0 ? std::string() : from.outputs[dragWirePort_].name);
                if (error.empty()) emit status(tr("The pin's wires come from %1 now").arg(QString::fromStdString(from.name)));
            }
            if (error.empty()) emit edited();
        } else if (nodeAt(at) >= 0) {
            // On a card but no port: nothing changes (a picked-up wire is not lost by a near miss).
            if (dragWireInputNode_ < 0) error = tr("Drop the wire on an input port").toStdString();
        } else if (dragWireInputNode_ >= 0) {
            error = document_->disconnectInput(nodes_[dragWireInputNode_].name, nodes_[dragWireInputNode_].inputs[dragWireInputPort_].name);
            if (error.empty()) emit edited();
        }
        if (!error.empty()) emit message(QString::fromStdString(error));
        dragWireFrom_ = dragWirePort_ = dragWireInputNode_ = dragWireInputPort_ = dragWirePin_ = dragWirePortal_ = -1;
        wireHint_.clear();
        refresh();
    }
}

QString NodeGraphView::connectionProblem(int fromNode, int fromPort, int toNode, int toPort) const {
    if (fromNode < 0 || toNode < 0 || fromNode >= nodeCount() || toNode >= nodeCount()) return {};
    const Node& from = nodes_[fromNode];
    const Node& to = nodes_[toNode];
    if (fromPort < 0 || fromPort >= static_cast<int>(from.outputs.size()) || toPort < 0 || toPort >= static_cast<int>(to.inputs.size())) return {};
    if (fromNode == toNode) return tr("A node can't feed itself");
    const Port& out = from.outputs[fromPort];
    const Port& in = to.inputs[toPort];
    try {
        if (!portTypesMatch(portTypeFromName(out.type), portTypeFromName(in.type))) {
            return tr("%1.%2 takes %3, %4 gives %5")
                .arg(QString::fromStdString(to.name), QString::fromStdString(in.name), QString::fromStdString(in.type).toLower(),
                     QString::fromStdString(from.name), QString::fromStdString(out.type).toLower());
        }
    } catch (const std::exception&) {
        // A type the core does not name: the document's own checks say.
    }
    // A loop: the target feeds the source already (directly or not).
    std::vector<int> stack{toNode};
    std::vector<bool> seen(nodes_.size(), false);
    while (!stack.empty()) {
        const int at = stack.back();
        stack.pop_back();
        if (at == fromNode) return tr("That would make a loop: %1 feeds %2 already").arg(QString::fromStdString(to.name), QString::fromStdString(from.name));
        if (seen[at]) continue;
        seen[at] = true;
        for (const Wire& w : wires_) {
            if (w.from == at) stack.push_back(w.to);
        }
    }
    return {};
}

void NodeGraphView::updateWireHint(const QPointF& scene) {
    int node = -1, port = -1;
    bool output = false;
    wireHint_.clear();
    if (dragWireFrom_ < 0 || !portAt(scene, &node, &port, &output)) return;
    if (output) {
        if (node != dragWireFrom_) wireHint_ = tr("Outputs don't connect to outputs: drop it on an input");
        return;
    }
    wireHint_ = connectionProblem(dragWireFrom_, dragWirePort_, node, port);
}

void NodeGraphView::wheelEvent(QWheelEvent* event) {
    if (event->modifiers().testFlag(Qt::AltModifier) && viewport_) {
        forwardToViewport(event);
        return;
    }
    // Zoom about the cursor: the scene point under it stays put.
    const int steps = event->angleDelta().y() / 120;
    if (steps == 0) return;
    const qreal factor = std::pow(1.15, steps);
    const qreal next = std::clamp(scale_ * factor, kMinScale, kMaxScale);
    const QPointF at = event->position();
    const QPointF scene = toScene(at);
    scale_ = next;
    pan_ = at - scene * scale_;
    viewTouched_ = true;
    update();
    event->accept();
}

// ---- Keys and the search --------------------------------------------------------------------------

void NodeGraphView::setBypass(const std::string& node, bool bypass) {
    if (!document_) return;
    const std::string error = document_->setBypass(node, bypass);
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    emit edited();
    refresh();
}

void NodeGraphView::toggleInputs(const std::string& name) {
    if (!document_) return;
    const int index = nodeIndex(name);
    if (index < 0 || nodes_[index].linkableParams.empty()) return;
    const bool expose = !std::all_of(nodes_[index].linkableParams.begin(), nodes_[index].linkableParams.end(),
                                     [&](const std::string& param) { return document_->paramExposed(name, param); });
    std::vector<std::string> changed = nodes_[index].linkableParams;
    if (!expose) {
        changed.erase(std::remove_if(changed.begin(), changed.end(), [&](const std::string& param) {
            return !document_->paramInputSource(name, param).empty();
        }), changed.end());
    }
    const std::string error = document_->exposeParams(name, changed, expose);
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        refresh();
        return;
    }
    emit edited();
    emit status(expose ? tr("Exposed all inputs on %1").arg(QString::fromStdString(name))
                       : tr("Collapsed all inputs on %1").arg(QString::fromStdString(name)));
    refresh();
}

void NodeGraphView::setEvaluation(const std::string& node, const std::string& mode) {
    if (!document_) return;
    const std::string error = document_->setEvaluation(node, mode);
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    emit edited();
    refresh();
}

void NodeGraphView::setNodeTimes(const std::vector<std::pair<std::string, double>>& times) {
    times_.clear();
    for (const auto& [name, ms] : times) times_[name] = ms;
    update();
}

QString NodeGraphView::timeText(double ms) {
    if (!(ms >= 0.05)) return {};
    return (ms < 10.0 ? QString::number(ms, 'f', 1) : QString::number(std::lround(ms))) + QStringLiteral(" ms");
}

QColor NodeGraphView::timeColor(double ms) { return ms >= kSlowNodeMs ? theme::graphWarning() : theme::textMuted(); }

NodeGraphView::NodeBadges NodeGraphView::nodeBadges(const std::string& name) const {
    NodeBadges b;
    if (const auto t = times_.find(pathOf(name)); t != times_.end()) {
        b.time = timeText(t->second);
        b.timeColor = timeColor(t->second);
    }
    b.failed = errors_.count(pathOf(name)) > 0;
    b.warning = notes_.count(pathOf(name)) > 0 && !b.failed;
    b.viewed = !name.empty() && pathOf(name) == viewed_;
    if (const int i = nodeIndex(name); i >= 0) b.bypassed = nodes_[i].bypass;
    return b;
}

bool NodeGraphView::wireDashed(int index) const {
    if (index < 0 || index >= static_cast<int>(wires_.size())) return false;
    const Wire& w = wires_[index];
    return nodes_[w.from].bypass || nodes_[w.to].bypass;
}

void NodeGraphView::setNodeErrors(const std::vector<std::pair<std::string, std::string>>& errors) {
    errors_.clear();
    for (const auto& [name, error] : errors) errors_[name] = error;
    update();
}

std::string NodeGraphView::nodeError(const std::string& node) const {
    const auto it = errors_.find(pathOf(node));
    return it == errors_.end() ? std::string() : it->second;
}

void NodeGraphView::setViewedNode(const std::string& node) {
    if (node == viewed_) return;
    viewed_ = node;
    update();
    emit viewNodeChanged(viewed_);
}

void NodeGraphView::toggleViewSelected() {
    // V: the selected node shown; again (or with nothing selected), the output.
    if (selected_.empty() || pathOf(selected_) == viewed_) {
        if (!viewed_.empty()) emit message(tr("Viewing the graph's output again"));
        setViewedNode({});
        return;
    }
    const int i = nodeIndex(selected_);
    if (i < 0) return;
    if (!viewable(nodes_[i])) {
        emit message(tr("%1 gives no strands to view: pick a node with a strands output").arg(QString::fromStdString(selected_)));
        return;
    }
    setViewedNode(pathOf(selected_));
    emit message(tr("Viewing up to %1 (V again: the output)").arg(QString::fromStdString(selected_)));
}

void NodeGraphView::copySelected() {
    if (!document_) return;
    std::vector<std::string> names = selection_;
    if (names.empty() && !selected_.empty()) names.push_back(selected_);
    const std::string text = document_->copyNodes(names);
    if (text.empty()) {
        emit message(tr("Select the nodes to copy (the output node is one of a kind)"));
        return;
    }
    QGuiApplication::clipboard()->setText(QString::fromStdString(text));
    emit message(tr("Copied %n node(s)", "", static_cast<int>(nlohmann::json::parse(text)["nodes"].size())));
}

void NodeGraphView::paste() {
    if (!document_) return;
    // Where the cursor is (the copied nodes' top-left there), else beside where they were copied
    // from; links only among the pasted nodes, none to the originals' sources.
    const std::string text = QGuiApplication::clipboard()->text().toStdString();
    float dx = static_cast<float>(kPasteOffsetX), dy = static_cast<float>(kPasteOffsetY);
    const QPointF cursor = mapFromGlobal(QCursor::pos());
    const nlohmann::json data = nlohmann::json::parse(text, nullptr, false);
    if (underMouse() && rect().contains(cursor.toPoint()) && data.is_object() && data.value("nodes", nlohmann::json()).is_array()) {
        double left = std::numeric_limits<double>::max(), top = std::numeric_limits<double>::max();
        for (const nlohmann::json& n : data["nodes"]) {
            const nlohmann::json ui = n.value("ui", nlohmann::json::object());
            if (!ui.is_object()) continue;
            left = std::min(left, ui.value("x", 0.0));
            top = std::min(top, ui.value("y", 0.0));
        }
        if (left < std::numeric_limits<double>::max()) {
            const QPointF at = snappedIf(toScene(cursor));
            dx = static_cast<float>(at.x() - left);
            dy = static_cast<float>(at.y() - top);
        }
    }
    std::vector<std::string> made;
    const std::string error = document_->pasteNodes(text, dx, dy, &made);
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    emit edited();
    refresh();
    setSelection(made);
}

void NodeGraphView::deleteSelected() {
    if (!document_) return;
    std::string error;
    if (selectedPortal_ >= 0 && selectedPortal_ < static_cast<int>(portals_.size())) {
        const Portal p = portals_[selectedPortal_];
        selectedPortal_ = -1;
        removePortal(p.name, p.exit);  // an exit, or (its entry) the whole portal
        return;
    }
    if (selectedBackdrop_ >= 0) {
        removeBackdrop(selectedBackdrop_);
        return;
    }
    if (selectedPin_ >= 0 && selectedPin_ < static_cast<int>(pins_.size())) {
        error = document_->removePin(pins_[selectedPin_].id);
        selectedPin_ = -1;
        if (!error.empty()) emit message(QString::fromStdString(error));
        else emit edited();
        refresh();
        return;
    }
    if (selectedWire_ >= 0 && selectedWire_ < static_cast<int>(wires_.size())) {
        const Wire& w = wires_[selectedWire_];
        error = document_->disconnectInput(nodes_[w.to].name, nodes_[w.to].inputs[w.input].name);
        selectedWire_ = -1;
    } else if (!selection_.empty()) {
        error = document_->removeNodes(selection_);
    } else {
        return;
    }
    if (!error.empty()) emit message(QString::fromStdString(error));
    else emit edited();
    refresh();
}

void NodeGraphView::detachNodes(const std::vector<std::string>& names) {
    if (!document_ || names.empty()) return;
    int detached = 0;
    const std::string error = document_->detachNodes(names, &detached);
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    if (detached == 0) emit message(tr("Nothing to detach: no links"));
    emit edited();
    refresh();
}

bool NodeGraphView::event(QEvent* event) {
    // A node's tooltip: its error, else its note (the badges' text).
    if (event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        const QPointF scene = toScene(QPointF(help->pos()));
        int node = -1;
        QString tip;
        if (inputsButtonAt(scene, &node)) {
            tip = tr("Expand Inputs: show or hide all connectable parameter ports");
        } else if (clumpColorsButtonAt(scene, &node)) {
            tip = tr("Clump Colors: color the strands by this node's clumps (its clumpChannel); again: as they were");
        } else if (braidTubesButtonAt(scene, &node)) {
            tip = tr("Show Tubes: draws the braid's section tubes in the viewport");
        } else if (viewButtonAt(scene, &node)) {
            tip = tr("View: show the groom up to this node in the viewport (V); again: the output");
        } else if (bypassButtonAt(scene, &node)) {
            tip = tr("Bypass: the node passes its input through unchanged");
        } else if ((node = nodeAt(scene)) >= 0) {
            const std::string error = nodeError(nodes_[node].name), note = nodeNote(nodes_[node].name);
            tip = !error.empty() ? QString::fromStdString(error) : QString::fromStdString(note);
        }
        if (tip.isEmpty()) QToolTip::hideText();
        else QToolTip::showText(help->globalPos(), tip, this);
        return true;
    }
    return QWidget::event(event);
}

void NodeGraphView::setNodeNotes(const std::vector<std::pair<std::string, std::string>>& notes) {
    notes_.clear();
    for (const auto& [name, note] : notes) notes_[name] = note;
    update();
}

std::string NodeGraphView::nodeNote(const std::string& node) const {
    const auto it = notes_.find(pathOf(node));
    return it == notes_.end() ? std::string() : it->second;
}

// ---- The edits a node's menu offers --------------------------------------------------------------

void NodeGraphView::renameSelected() {
    if (!document_ || selected_.empty()) return;
    const std::string name = selected_;
    promptName(tr("Rename %1").arg(QString::fromStdString(name)), tr("Node name"), [this, name](const QString& to) {
        const std::string error = document_->renameNode(name, to.trimmed().toStdString());
        if (!error.empty()) {
            emit message(QString::fromStdString(error));
            return;
        }
        selected_ = to.trimmed().toStdString();
        selection_ = {selected_};
        emit edited();
        refresh();
        emit nodeRenamed(name, selected_);
        emit selectionChanged(selected_);
    });
    setPromptText(QString::fromStdString(name));
}

void NodeGraphView::duplicateSelected() {
    if (!document_ || selected_.empty()) return;
    // Beside them, each copy reading what its node reads (the links among them made again).
    std::vector<std::string> names = selection_;
    if (names.empty()) names.push_back(selected_);
    std::vector<std::string> made;
    const std::string error = document_->duplicateNodes(names, static_cast<float>(kPasteOffsetX), static_cast<float>(kPasteOffsetY), &made);
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    emit edited();
    refresh();
    setSelection(made);
}

void NodeGraphView::setSelectedAsOutput() {
    if (!document_ || selected_.empty()) return;
    const std::string error = document_->setOutputNode(selected_);
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    emit edited();
    refresh();
}

void NodeGraphView::tidyLayout() {
    if (!document_) return;
    const std::string error = document_->tidyLayout();
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    laidOut_.clear();
    fitted_ = false;
    emit edited();
    refresh();
}

void NodeGraphView::groupSelected() {
    if (!document_) return;
    std::vector<std::string> names = selection_;
    if (names.empty() && !selected_.empty()) names.push_back(selected_);
    if (names.empty()) {
        emit message(tr("Select the nodes to group"));
        return;
    }
    // Nodes the view laid out keep where they show, so the compound goes among them and opening
    // it puts them back there.
    for (const std::string& n : names) {
        const int i = nodeIndex(n);
        float x = 0.0f, y = 0.0f;
        if (i >= 0 && !document_->nodePosition(n, &x, &y)) {
            document_->setNodePosition(n, static_cast<float>(nodes_[i].pos.x()), static_cast<float>(nodes_[i].pos.y()));
        }
    }
    std::string made;
    const std::string error = document_->collapseNodes(names, &made);
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    emit edited();
    refresh();
    selectNode(made);
    emit message(tr("Grouped %n node(s) into %1 (double-click or Ctrl+Shift+G opens it)", "", static_cast<int>(names.size()))
                     .arg(QString::fromStdString(made)));
}

void NodeGraphView::ungroupSelected() {
    if (!document_) return;
    std::vector<std::string> compounds;
    for (const std::string& n : selection_) {
        const int i = nodeIndex(n);
        if (i >= 0 && nodes_[i].type == "Compound") compounds.push_back(n);
    }
    if (compounds.empty()) {
        emit message(tr("Select the Compound nodes to ungroup"));
        return;
    }
    std::vector<std::string> made;
    const std::string error = document_->expandNodes(compounds, &made);
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    emit edited();
    refresh();
    setSelection(made);
}

void NodeGraphView::addBackdrop(const QPointF& at) {
    if (!document_) return;
    // Around the selected nodes (Maya's margins: room for the title above), else a frame at the point.
    QRectF rect;
    for (const std::string& n : selection_) {
        const int i = nodeIndex(n);
        if (i >= 0) rect |= QRectF(nodes_[i].pos, nodes_[i].size);
    }
    if (!rect.isNull()) {
        rect.adjust(-30, -50, 30, 30);
    } else {
        const QPointF center = toScene(std::isnan(at.x()) ? QPointF(width() / 2.0, height() / 2.0) : at);
        rect = QRectF(center.x() - 200, center.y() - 120, 400, 240);
    }
    int index = -1;
    const std::string error = document_->addBackdrop("Backdrop", rect.x(), rect.y(), rect.width(), rect.height(), &index);
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    refresh();
    selectBackdrop(index);
    renameBackdrop(index);  // named at once (Escape keeps "Backdrop")
}

void NodeGraphView::selectBackdrop(int index) {
    selectNode({});
    selectedWire_ = -1;
    selectedBackdrop_ = index >= 0 && index < backdropCount() ? index : -1;
    update();
}

QString NodeGraphView::backdropTitle(int index) const { return index >= 0 && index < backdropCount() ? backdrops_[index].title : QString(); }

QColor NodeGraphView::backdropColor(int index) const { return index >= 0 && index < backdropCount() ? backdrops_[index].color : QColor(); }

QRectF NodeGraphView::backdropRect(int index) const { return index >= 0 && index < backdropCount() ? backdrops_[index].rect : QRectF(); }

QRectF NodeGraphView::backdropTitleBar(int index) const {
    if (index < 0 || index >= backdropCount()) return {};
    const QRectF& r = backdrops_[index].rect;
    return QRectF(toWidget(r.topLeft()), QSizeF(r.width(), kBackdropHeader) * scale_);
}

QRectF NodeGraphView::backdropGrip(int index) const {
    if (index < 0 || index >= backdropCount()) return {};
    const QRectF& r = backdrops_[index].rect;
    return QRectF(toWidget(QPointF(r.right() - kBackdropGrip, r.bottom() - kBackdropGrip)), QSizeF(kBackdropGrip, kBackdropGrip) * scale_);
}

void NodeGraphView::renameBackdrop(int index) {
    if (!document_ || index < 0 || index >= backdropCount()) return;
    promptName(tr("Backdrop title"), tr("Title"), [this, index](const QString& title) {
        const std::string error = document_->setBackdropTitle(index, title.toStdString());
        if (!error.empty()) emit message(QString::fromStdString(error));
        refresh();
    });
    setPromptText(backdrops_[index].title);
}

void NodeGraphView::setBackdropColor(int index, const QColor& color) {
    if (!document_) return;
    const std::string error = document_->setBackdropColor(index, color.red(), color.green(), color.blue());
    if (!error.empty()) emit message(QString::fromStdString(error));
    refresh();
}

void NodeGraphView::removeBackdrop(int index) {
    if (!document_) return;
    const std::string error = document_->removeBackdrop(index);
    if (!error.empty()) emit message(QString::fromStdString(error));
    selectedBackdrop_ = -1;
    refresh();
}

void NodeGraphView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || event->modifiers().testFlag(Qt::AltModifier)) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    const QPointF scene = toScene(event->position());
    const int node = nodeAt(scene);
    if (node >= 0 && nodes_[node].type == "Script") {
        // A script node opens its editor.
        drag_ = Drag::None;
        selectNode(nodes_[node].name);
        openScriptEditor(nodes_[node].name);
    } else if (node >= 0 && nodes_[node].type == "Compound" && scene.y() - nodes_[node].pos.y() > kHeader) {
        // A Compound opens: the graph inside it shown to edit (U or the path bar: back out). Its
        // header renames it, as any node's.
        drag_ = Drag::None;
        openCompound(nodes_[node].name);
    } else if (const int dot = node < 0 ? portalAt(scene) : -1; dot >= 0) {
        drag_ = Drag::None;
        renamePortal(portals_[dot].name);
    } else if (node >= 0 && scene.y() - nodes_[node].pos.y() <= kHeader) {
        drag_ = Drag::None;
        selectNode(nodes_[node].name);
        renameSelected();
    } else if (const int b = node < 0 ? backdropTitleAt(scene) : -1; b >= 0) {
        drag_ = Drag::None;
        renameBackdrop(b);
    } else if (const int w = node < 0 && pinAt(scene) < 0 ? wireAt(scene) : -1; w >= 0 && document_) {
        // A pin where the wire was double-clicked (Unreal's reroute node): the wire runs through it.
        drag_ = Drag::None;
        pinWire(w, scene);
    }
    event->accept();
}

NodeGraphView::MenuTarget NodeGraphView::fillContextMenu(QMenu* menu, const QPointF& at) {
    menuPoint_ = at;
    const QPointF scene = toScene(at);
    const int node = nodeAt(scene);
    MenuTarget target = MenuTarget::Canvas;
    menuWire_ = menuPortal_ = menuPortNode_ = menuPortInput_ = -1;
    int portNode = -1, port = -1;
    bool output = false;
    if (portAt(scene, &portNode, &port, &output) && !output) {
        // An input port: its own menu (Pass Through as Output).
        target = MenuTarget::Port;
        menuPortNode_ = portNode;
        menuPortInput_ = port;
    } else if (const int dot = node < 0 ? portalAt(scene) : -1; dot >= 0) {
        target = MenuTarget::Portal;
        menuPortal_ = dot;
        selectedPortal_ = dot;
    } else if (const int w = node < 0 && pinAt(scene) < 0 ? wireAt(scene) : -1; w >= 0) {
        target = MenuTarget::Wire;
        menuWire_ = w;
        selectedWire_ = w;
        update();
    } else if (node >= 0) {
        // A node of the selection keeps the selection (Delete and Copy act on it all), the node
        // right-clicked the current one: the menu's actions act on it.
        target = MenuTarget::Node;
        if (!isSelected(nodes_[node].name)) {
            selectNode(nodes_[node].name);
        } else if (selected_ != nodes_[node].name) {
            std::vector<std::string> names = selection_;
            names.erase(std::find(names.begin(), names.end(), nodes_[node].name));
            names.push_back(nodes_[node].name);
            setSelection(names);
        }
    } else if (const int b = backdropAt(scene); b >= 0) {
        target = MenuTarget::Backdrop;
        selectBackdrop(b);
    }
    if (menuFiller_ && document_) menuFiller_(menu, target, at);
    return target;
}

void NodeGraphView::contextMenuEvent(QContextMenuEvent* event) {
    event->accept();
    // The release that ended an Alt+right-drag zoom (or Alt held): no menu.
    if (zoomEnded_ || drag_ == Drag::Zoom || event->modifiers().testFlag(Qt::AltModifier)) {
        zoomEnded_ = false;
        return;
    }
    if (!document_) return;
    QMenu menu(this);
    fillContextMenu(&menu, QPointF(event->pos()));
    if (!menu.isEmpty()) menu.exec(event->globalPos());
}

void NodeGraphView::keyPressEvent(QKeyEvent* event) {
    if (event->modifiers() == Qt::NoModifier && event->key() == Qt::Key_U && document_ && !document_->scope().empty()) {
        leaveCompound();  // up out of the compound (Houdini's U)
        event->accept();
        return;
    }
    if (event->modifiers() == Qt::NoModifier && event->key() == Qt::Key_I) {
        const int current = nodeIndex(selected_);
        if (current >= 0 && nodes_[current].type == "Compound") openCompound(selected_);  // into it (Houdini's I)
        event->accept();
        return;
    }
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        if (event->key() == Qt::Key_X && event->modifiers().testFlag(Qt::ShiftModifier)) {
            detachNodes(selection_);  // Detach (Maya's and Blender's key too)
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_G) {
            if (event->modifiers().testFlag(Qt::ShiftModifier)) ungroupSelected();
            else groupSelected();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_D) {
            duplicateSelected();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_L) {
            tidyLayout();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_C) {
            copySelected();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_V) {
            paste();
            event->accept();
            return;
        }
    }
    switch (event->key()) {
        case Qt::Key_Tab:
            openSearch();
            event->accept();
            return;
        case Qt::Key_Escape:
            if (searchOpen()) closeSearch();
            else emit hideRequested();
            event->accept();
            return;
        case Qt::Key_F:
            frameSelected();  // (the window's F reaches it as a shortcut: View > Frame All)
            event->accept();
            return;
        case Qt::Key_A:
            fitAll();
            event->accept();
            return;
        case Qt::Key_F2:
            if (!selected_.empty()) renameSelected();
            event->accept();
            return;
        case Qt::Key_B: {
            // Every selected node, each to the current node's opposite (as Maya's B).
            const int current = nodeIndex(selected_);
            if (current >= 0) {
                const bool on = !nodes_[current].bypass;
                const std::vector<std::string> names = selection_;
                for (const std::string& n : names) setBypass(n, on);
            }
            event->accept();
            return;
        }
        case Qt::Key_V:
            if (event->modifiers() == Qt::NoModifier) {
                toggleViewSelected();
                event->accept();
                return;
            }
            break;
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            if (!document_ || (selectedWire_ < 0 && selection_.empty() && selectedBackdrop_ < 0 && selectedPin_ < 0 && selectedPortal_ < 0)) break;
            deleteSelected();
            event->accept();
            return;
        default:
            break;
    }
    QWidget::keyPressEvent(event);
}

bool NodeGraphView::searchOpen() const { return !search_->isHidden(); }

void NodeGraphView::openSearch() {
    const QPointF at = mapFromGlobal(QCursor::pos());
    openSearchAt(rect().contains(at.toPoint()) ? at : QPointF(width() / 2.0, height() / 2.0));
}

void NodeGraphView::openSearchAt(const QPointF& where) {
    searchScene_ = toScene(where);
    // Over a wire: the node added goes into it (lit while the search is open).
    searchWire_ = nodeAt(searchScene_) < 0 ? wireAt(searchScene_) : -1;
    if (searchWire_ < 0) searchWire_ = selectedWire_;  // a wire selected first: the node goes into it
    update();
    // As tall as the editor allows (the list gives way, the description under it always shows),
    // placed where it fits: below the cursor, else above it.
    search_->resize(search_->width(), std::clamp(height() - 8, 200, 380));
    int top = static_cast<int>(where.y());
    if (top + search_->height() > height()) top = std::max(0, static_cast<int>(where.y()) - search_->height());
    search_->move(std::clamp(static_cast<int>(where.x()), 0, std::max(0, width() - search_->width())),
                  std::clamp(top, 0, std::max(0, height() - search_->height())));
    searchEdit_->clear();
    if (document_) document_->refreshLibrary();  // nodes saved or dropped in since
    filterSearch({});
    search_->show();
    search_->raise();
    searchEdit_->setFocus();
}

// ---- Compound nodes ------------------------------------------------------------------------------

void NodeGraphView::openSaveNode(const std::string& node) {
    if (!document_ || nodeIndex(node) < 0) return;
    delete saveNode_;
    saveFields_.clear();
    saveNodeName_ = node;
    const nlohmann::json library = document_->libraryInfo(node);
    saveIdTyped_ = library.is_object();
    nlohmann::json definition;
    if (const nlohmann::json params = document_->param(node, "definition"); params.is_object()) definition = params;
    auto text = [&](const char* key) { return QString::fromStdString(definition.is_object() ? definition.value(key, std::string()) : std::string()); };
    saveNode_ = new GlassPanel(this, 14.0);
    saveNode_->setObjectName(QStringLiteral("saveNodeForm"));
    saveNode_->setColor(theme::glassSolid());
    auto* layout = new QVBoxLayout(saveNode_);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(6);
    auto* title = new QLabel(tr("Save %1 as a node").arg(QString::fromStdString(node)), saveNode_);
    title->setFont(theme::uiFont(14, 800));
    title->setStyleSheet(QStringLiteral("color: %1;").arg(theme::text().name()));
    layout->addWidget(title);
    auto* about = new QLabel(tr("To your node library (%1): Add Node lists it in every groom, in Maya and Blender too.")
                                 .arg(QString::fromStdString(nodeUserDir())), saveNode_);
    about->setWordWrap(true);
    about->setFont(theme::uiFont(11, 500));
    about->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textMuted().name()));
    layout->addWidget(about);
    auto line = [&](const QString& name, const QString& placeholder, const QString& value) {
        auto* edit = new QLineEdit(value, saveNode_);
        edit->setPlaceholderText(placeholder);
        edit->setFont(theme::uiFont(13, 500));
        layout->addWidget(edit);
        saveFields_[name] = edit;
        return edit;
    };
    QString label = text("label");
    if (label.isEmpty()) {
        for (std::size_t i = 0; i < node.size(); ++i) {
            const QChar c = QChar(node[i]);
            if (i > 0 && c.isUpper()) label += QLatin1Char(' ');
            label += i == 0 ? c.toUpper() : c;
        }
    }
    QLineEdit* labelEdit = line(QStringLiteral("label"), tr("Label (the name menus show)"), label);
    auto* category = new QComboBox(saveNode_);
    category->setEditable(true);
    category->addItem(tr("My Nodes"));
    for (const NodeCategory& c : nodeCategories()) category->addItem(QString::fromStdString(c.title));
    category->setCurrentText(text("category").isEmpty() ? tr("My Nodes") : text("category"));
    category->setToolTip(tr("Where Add Node lists it: a built-in category, or type your own"));
    layout->addWidget(category);
    saveFields_[QStringLiteral("category")] = category;
    line(QStringLiteral("description"), tr("What it does, in a line"), text("description"));
    QLineEdit* authorEdit = line(QStringLiteral("author"), tr("Author (optional)"), text("author"));
    QLineEdit* idEdit = line(QStringLiteral("id"), tr("Id: yourname.node_name, never changed once shared"),
                             library.is_object() ? QString::fromStdString(library.value("id", std::string())) : QString());
    auto* version = new QSpinBox(saveNode_);
    version->setRange(1, 1000000);
    version->setPrefix(tr("Version "));
    layout->addWidget(version);
    saveFields_[QStringLiteral("version")] = version;
    line(QStringLiteral("license"), tr("License (optional: CC0-1.0, CC-BY-4.0, your own words)"), text("license"));
    saveError_ = new QLabel(saveNode_);
    saveError_->setWordWrap(true);
    saveError_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::graphError().name()));
    layout->addWidget(saveError_);
    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    auto* cancel = new QPushButton(tr("Cancel"), saveNode_);
    auto* save = new QPushButton(tr("Save"), saveNode_);
    buttons->addWidget(cancel);
    buttons->addWidget(save);
    layout->addLayout(buttons);
    // The id follows the label and author until typed; the version is the library's next.
    const int own = library.is_object() ? library.value("version", 0) : 0;
    auto suggest = [this, labelEdit, authorEdit, idEdit, version, own] {
        if (!saveIdTyped_) {
            idEdit->setText(QString::fromStdString(suggestNodeId(labelEdit->text().toStdString(), authorEdit->text().toStdString())));
        }
        const std::optional<LibraryNode> existing = findLibraryNode(idEdit->text().trimmed().toStdString());
        version->setValue(std::max(existing ? existing->version : 0, own) + 1);
    };
    connect(labelEdit, &QLineEdit::textEdited, this, suggest);
    connect(authorEdit, &QLineEdit::textEdited, this, suggest);
    connect(idEdit, &QLineEdit::textEdited, this, [this] { saveIdTyped_ = true; });
    connect(idEdit, &QLineEdit::editingFinished, this, suggest);
    suggest();
    connect(save, &QPushButton::clicked, this, [this] { acceptSaveNode(); });
    connect(cancel, &QPushButton::clicked, this, [this] {
        saveNode_->hide();
        setFocus();
    });
    saveNode_->resize(360, saveNode_->sizeHint().height());
    saveNode_->move(std::max(0, (width() - saveNode_->width()) / 2), std::max(0, (height() - saveNode_->height()) / 3));
    saveNode_->show();
    saveNode_->raise();
    labelEdit->setFocus();
}

bool NodeGraphView::saveNodeOpen() const { return saveNode_ && !saveNode_->isHidden(); }

void NodeGraphView::setSaveNodeField(const QString& field, const QString& value) {
    const auto it = saveFields_.find(field);
    if (it == saveFields_.end()) return;
    if (auto* edit = qobject_cast<QLineEdit*>(it->second)) {
        edit->setText(value);
        emit edit->textEdited(value);
    } else if (auto* box = qobject_cast<QComboBox*>(it->second)) {
        box->setCurrentText(value);
    } else if (auto* spin = qobject_cast<QSpinBox*>(it->second)) {
        spin->setValue(value.toInt());
    }
}

QString NodeGraphView::saveNodeField(const QString& field) const {
    const auto it = saveFields_.find(field);
    if (it == saveFields_.end()) return {};
    if (auto* edit = qobject_cast<QLineEdit*>(it->second)) return edit->text();
    if (auto* box = qobject_cast<QComboBox*>(it->second)) return box->currentText();
    if (auto* spin = qobject_cast<QSpinBox*>(it->second)) return QString::number(spin->value());
    return {};
}

QString NodeGraphView::acceptSaveNode() {
    if (!saveNodeOpen() || !document_) return tr("no Save as Node form is open");
    nlohmann::json header = {{"version", saveNodeField(QStringLiteral("version")).toInt()}};
    for (const char* key : {"label", "category", "description", "author", "id", "license"}) {
        header[key] = saveNodeField(QString::fromLatin1(key)).trimmed().toStdString();
    }
    if (header["category"] == tr("My Nodes").toStdString()) header["category"] = "";
    std::string error = nodeIdError(header["id"].get<std::string>());
    std::string path;
    if (error.empty()) error = document_->saveAsNode(saveNodeName_, header, &path);
    if (!error.empty()) {
        saveError_->setText(QString::fromStdString(error));
        return QString::fromStdString(error);
    }
    saveNode_->hide();
    refresh();
    emit edited();
    emit message(tr("Saved %1 to %2").arg(QString::fromStdString(header["label"].get<std::string>()), QString::fromStdString(path)));
    setFocus();
    return {};
}

void NodeGraphView::saveToLibrary(const std::string& node) {
    if (!document_) return;
    const nlohmann::json library = document_->libraryInfo(node);
    if (!library.is_object()) return;
    const int version = std::max(library.value("version", 0), library.value("latest", 0)) + 1;
    std::string path;
    const std::string error = document_->saveAsNode(node, {{"version", version}}, &path);
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    refresh();
    emit edited();
    emit message(tr("Saved version %1 to %2").arg(version).arg(QString::fromStdString(path)));
}

void NodeGraphView::updateLibraryNode(const std::string& node) {
    if (!document_) return;
    const std::string error = document_->updateLibraryNode(node);
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    refresh();
    emit edited();
}

std::vector<std::string> NodeGraphView::importNodeFiles(const QStringList& paths, const QPointF& at) {
    std::vector<std::string> made;
    if (!document_) return made;
    QPointF where = toScene(std::isnan(at.x()) ? QPointF(width() / 2.0, height() / 2.0) : at);
    for (const QString& path : paths) {
        std::string id;
        std::string error = document_->importNode(path.toStdString(), &id);
        std::string name;
        if (error.empty()) error = document_->addNode(libraryNodeType(id), static_cast<float>(where.x()), static_cast<float>(where.y()), &name);
        if (!error.empty()) {
            emit message(QString::fromStdString(error));
            continue;
        }
        made.push_back(name);
        where += QPointF(30.0, 30.0);
    }
    if (!made.empty()) {
        refresh();
        selectNode(made.back());
        emit edited();
    }
    return made;
}

namespace {

QStringList nodeFilesOf(const QMimeData* mime) {
    QStringList files;
    if (!mime || !mime->hasUrls()) return files;
    for (const QUrl& url : mime->urls()) {
        if (url.isLocalFile() && url.toLocalFile().endsWith(QStringLiteral(".glnode"), Qt::CaseInsensitive)) files << url.toLocalFile();
    }
    return files;
}

}  // namespace

void NodeGraphView::dragEnterEvent(QDragEnterEvent* event) {
    if (!nodeFilesOf(event->mimeData()).isEmpty()) event->acceptProposedAction();
    else QWidget::dragEnterEvent(event);
}

void NodeGraphView::dropEvent(QDropEvent* event) {
    const QStringList files = nodeFilesOf(event->mimeData());
    if (files.isEmpty()) {
        QWidget::dropEvent(event);
        return;
    }
    event->acceptProposedAction();
    importNodeFiles(files, event->position());
}

// ---- Script nodes --------------------------------------------------------------------------------

std::string NodeGraphView::newScriptNode(const QPointF& at) {
    if (!document_) return {};
    const QPointF where = toScene(std::isnan(at.x()) ? QPointF(width() / 2.0, height() / 2.0) : at);
    std::string name;
    const std::string error = document_->addScriptNode(static_cast<float>(where.x()), static_cast<float>(where.y()), &name);
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return {};
    }
    refresh();
    selectNode(name);
    emit edited();
    openScriptEditor(name);
    return name;
}

void NodeGraphView::openScriptEditor(const std::string& node) {
    if (!document_ || nodeIndex(node) < 0) return;
    const nlohmann::json definition = document_->param(node, "definition");
    if (!definition.is_object()) return;
    delete scriptEditor_;
    scriptFields_.clear();
    scriptNode_ = node;
    scriptEditor_ = new GlassPanel(this, 14.0);
    scriptEditor_->setObjectName(QStringLiteral("scriptNodeEditor"));
    scriptEditor_->setColor(theme::glassSolid());
    auto* layout = new QVBoxLayout(scriptEditor_);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(6);
    auto* title = new QLabel(tr("Script node %1").arg(QString::fromStdString(node)), scriptEditor_);
    title->setFont(theme::uiFont(14, 800));
    title->setStyleSheet(QStringLiteral("color: %1;").arg(theme::text().name()));
    layout->addWidget(title);
    auto* about = new QLabel(tr("Live: each change is checked after a pause and the node takes it when it checks. "
                                "Its params show in the param panel; Save to Library shares it with Maya and Blender."),
                             scriptEditor_);
    about->setWordWrap(true);
    about->setFont(theme::uiFont(11, 500));
    about->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textMuted().name()));
    layout->addWidget(about);
    auto* label = new QLineEdit(QString::fromStdString(definition.value("label", std::string())), scriptEditor_);
    label->setPlaceholderText(tr("Label (the name menus show)"));
    layout->addWidget(label);
    scriptFields_[QStringLiteral("label")] = label;
    QStringList writes;
    for (const nlohmann::json& w : definition.value("writes", nlohmann::json::array())) {
        if (w.is_string()) writes << QString::fromStdString(w.get<std::string>());
    }
    auto* written = new QLineEdit(writes.join(QStringLiteral(", ")), scriptEditor_);
    written->setPlaceholderText(tr("What it writes: P, width, color, channel.name"));
    written->setToolTip(tr("The outputs the script may assign"));
    layout->addWidget(written);
    scriptFields_[QStringLiteral("writes")] = written;
    auto* code = new QPlainTextEdit(QString::fromStdString(definition.value("script", std::string())), scriptEditor_);
    code->setObjectName(QStringLiteral("scriptNodeCode"));
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSize(11);
    code->setFont(mono);
    code->setLineWrapMode(QPlainTextEdit::NoWrap);
    code->setTabStopDistance(QFontMetricsF(mono).horizontalAdvance(QLatin1Char(' ')) * 4);
    code->setMinimumHeight(260);
    layout->addWidget(code, 1);
    scriptFields_[QStringLiteral("script")] = code;
    scriptErrors_ = new QLabel(scriptEditor_);
    scriptErrors_->setObjectName(QStringLiteral("scriptNodeErrors"));
    scriptErrors_->setWordWrap(true);
    scriptErrors_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::graphError().name()));
    layout->addWidget(scriptErrors_);
    scriptStatus_ = new QLabel(scriptEditor_);
    scriptStatus_->setWordWrap(true);
    scriptStatus_->setFont(theme::uiFont(11, 500));
    scriptStatus_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::textMuted().name()));
    layout->addWidget(scriptStatus_);
    auto* buttons = new QHBoxLayout();
    auto* saveNew = new QPushButton(tr("Save as New"), scriptEditor_);
    saveNew->setToolTip(tr("Write it as a new node of your node library (an id the library does not have)"));
    auto* save = new QPushButton(tr("Save to Library"), scriptEditor_);
    save->setToolTip(tr("Write it to your node library, one version up when the library has it"));
    auto* close = new QPushButton(tr("Close"), scriptEditor_);
    buttons->addWidget(close);
    buttons->addStretch(1);
    buttons->addWidget(saveNew);
    buttons->addWidget(save);
    layout->addLayout(buttons);
    scriptPause_ = new QTimer(scriptEditor_);
    scriptPause_->setSingleShot(true);
    scriptPause_->setInterval(300);
    connect(scriptPause_, &QTimer::timeout, this, [this] { applyScriptEditor(); });
    connect(label, &QLineEdit::textEdited, scriptPause_, qOverload<>(&QTimer::start));
    connect(written, &QLineEdit::textEdited, scriptPause_, qOverload<>(&QTimer::start));
    connect(code, &QPlainTextEdit::textChanged, scriptPause_, qOverload<>(&QTimer::start));
    connect(save, &QPushButton::clicked, this, [this] { saveScriptEditor(false); });
    connect(saveNew, &QPushButton::clicked, this, [this] { saveScriptEditor(true); });
    connect(close, &QPushButton::clicked, this, [this] { closeScriptEditor(); });
    scriptEditor_->resize(std::min(width() - 20, 560), std::min(height() - 20, std::max(scriptEditor_->sizeHint().height(), 480)));
    scriptEditor_->move(std::max(0, width() - scriptEditor_->width() - 10), 10);
    scriptEditor_->show();
    scriptEditor_->raise();
    code->setFocus();
}

bool NodeGraphView::scriptEditorOpen() const { return scriptEditor_ && !scriptEditor_->isHidden(); }

void NodeGraphView::setScriptEditorField(const QString& field, const QString& value) {
    const auto it = scriptFields_.find(field);
    if (it == scriptFields_.end()) return;
    if (auto* edit = qobject_cast<QLineEdit*>(it->second)) {
        edit->setText(value);
        emit edit->textEdited(value);
    } else if (auto* code = qobject_cast<QPlainTextEdit*>(it->second)) {
        code->setPlainText(value);
    }
}

QString NodeGraphView::scriptEditorField(const QString& field) const {
    const auto it = scriptFields_.find(field);
    if (it == scriptFields_.end()) return {};
    if (auto* edit = qobject_cast<QLineEdit*>(it->second)) return edit->text();
    if (auto* code = qobject_cast<QPlainTextEdit*>(it->second)) return code->toPlainText();
    return {};
}

QString NodeGraphView::scriptEditorErrors() const { return scriptErrors_ && scriptEditorOpen() ? scriptErrors_->text() : QString(); }

QString NodeGraphView::applyScriptEditor() {
    if (!scriptEditorOpen() || !document_ || nodeIndex(scriptNode_) < 0) return tr("no script node editor is open");
    if (scriptPause_) scriptPause_->stop();
    nlohmann::json definition = document_->param(scriptNode_, "definition");
    if (!definition.is_object()) return tr("%1 is not a script node").arg(QString::fromStdString(scriptNode_));
    definition["label"] = scriptEditorField(QStringLiteral("label")).trimmed().toStdString();
    nlohmann::json writes = nlohmann::json::array();
    for (const QString& w : scriptEditorField(QStringLiteral("writes")).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        if (!w.trimmed().isEmpty()) writes.push_back(w.trimmed().toStdString());
    }
    definition["writes"] = writes;
    definition["script"] = scriptEditorField(QStringLiteral("script")).toStdString();
    // Its errors (each "line L, column C: ..."), the line of the first tinted; none: the node takes it.
    std::string error;
    if (definition != document_->param(scriptNode_, "definition")) error = document_->setScriptDefinition(scriptNode_, definition);
    auto* code = qobject_cast<QPlainTextEdit*>(scriptFields_[QStringLiteral("script")]);
    QList<QTextEdit::ExtraSelection> tinted;
    const QRegularExpressionMatch at = QRegularExpression(QStringLiteral("^line (\\d+), column")).match(QString::fromStdString(error));
    if (code && at.hasMatch()) {
        const QTextBlock block = code->document()->findBlockByNumber(at.captured(1).toInt() - 1);
        if (block.isValid()) {
            QTextEdit::ExtraSelection selection;
            selection.format.setBackground(QColor(theme::graphError().red(), theme::graphError().green(), theme::graphError().blue(), 60));
            selection.format.setProperty(QTextFormat::FullWidthSelection, true);
            selection.cursor = QTextCursor(block);
            tinted << selection;
        }
    }
    if (code) code->setExtraSelections(tinted);
    scriptErrors_->setText(QString::fromStdString(error));
    if (!error.empty()) {
        scriptStatus_->setText(tr("Not applied: the node keeps its last script that checked."));
        return QString::fromStdString(error);
    }
    refresh();
    emit edited();
    scriptStatus_->setText(tr("Applied: the node runs this script."));
    return {};
}

QString NodeGraphView::saveScriptEditor(bool asNew) {
    if (!scriptEditorOpen() || !document_) return tr("no script node editor is open");
    QString error = applyScriptEditor();
    if (!error.isEmpty()) return error;
    std::string path;
    error = QString::fromStdString(document_->saveScriptNode(scriptNode_, asNew, &path));
    if (!error.isEmpty()) {
        scriptErrors_->setText(error);
        return error;
    }
    refresh();
    emit edited();
    const nlohmann::json definition = document_->param(scriptNode_, "definition");
    scriptStatus_->setText(tr("Saved version %1 to %2").arg(definition.value("version", 1)).arg(QString::fromStdString(path)));
    emit message(scriptStatus_->text());
    return {};
}

void NodeGraphView::closeScriptEditor() {
    if (scriptPause_ && scriptPause_->isActive()) applyScriptEditor();  // a pending change kept
    if (scriptEditor_) scriptEditor_->hide();
    setFocus();
}

void NodeGraphView::closeSearch() {
    search_->hide();
    searchWire_ = -1;
    setFocus();
    update();
}

int nodeSearchRank(const QString& search, const nlohmann::json& spec) {
    // The core's (node_catalog.h, every host's search).
    return groomlab::nodeSearchRank(search.toStdString(), nodeSearchWords(spec.dump()));
}

std::vector<std::string> nodeSearchResults(const GroomDocument& document, const QString& text) {
    std::vector<std::tuple<int, int, int, std::string>> ranked;  // rank, category order, order, type
    for (const std::string& type : document.operatorTypes()) {
        const nlohmann::json* spec = document.spec(type);
        if (!spec || spec->value("hidden", false)) continue;
        const int rank = nodeSearchRank(text, *spec);
        if (rank < 0) continue;
        ranked.emplace_back(rank, spec->value("categoryOrder", std::numeric_limits<int>::max()),
                            spec->value("order", std::numeric_limits<int>::max()), type);
    }
    std::sort(ranked.begin(), ranked.end());
    std::vector<std::string> types;
    types.reserve(ranked.size());
    for (const auto& r : ranked) types.push_back(std::get<3>(r));
    return types;
}

void NodeGraphView::filterSearch(const QString& text) {
    searchList_->clear();
    searchAbout_->clear();
    if (!document_) return;
    // Maya's ranking; each type by its label, its menu category after it in the category's color.
    // The groom's painted maps after the types of their rank (Maya's MAP_ENTRY): a Map node
    // reading the map, found by its name (or map, mask, paint).
    struct Row {
        int rank, order;
        QString shown, about, data;
        QColor color;
    };
    std::vector<Row> rows;
    int order = 0;
    for (const std::string& type : nodeSearchResults(*document_, text)) {
        const nlohmann::json* spec = document_->spec(type);
        const std::string label = spec->value("label", std::string());
        const QString shown = QString::fromStdString(label.empty() ? nodeLabel(type) : label);
        rows.push_back({nodeSearchRank(text, *spec), order++,
                        shown + QStringLiteral("    ") + QString::fromStdString(spec->value("categoryTitle", std::string())),
                        QString::fromStdString(spec->value("description", std::string())), QString::fromStdString(type),
                        theme::nodeCategoryColor(spec->value("category", std::string())).lighter(130)});
    }
    for (const PaintedMap& m : document_->maps()) {
        const QString kind = m.kind == "guides" ? tr("a layer on the guides") : m.kind == "texture" ? tr("a texture") : tr("on the scalp's vertices");
        const QString about = tr("A Map node reading the painted map %1 (%2): wire it to a setting to drive it").arg(QString::fromStdString(m.name), kind);
        const nlohmann::json entry = {{"type", kMapEntry + m.name},
                                      {"label", "Map: " + m.name},
                                      {"keywords", "painted map mask paint texture painted maps"},
                                      {"categoryTitle", "Painted maps"},
                                      {"description", about.toStdString()}};
        const int rank = nodeSearchRank(text, entry);
        if (rank < 0) continue;
        rows.push_back({rank, order++, tr("Map: %1    Painted maps").arg(QString::fromStdString(m.name)), about,
                        QString::fromLatin1(kMapEntry) + QString::fromStdString(m.name), theme::nodeCategoryColor("utility").lighter(130)});
    }
    // The graph's portals: an exit of one placed here (then linked from its nub).
    for (const std::string& portal : portalNames(document_->viewGraph())) {
        const nlohmann::json entry = {{"type", "portal:" + portal}, {"label", "Portal: " + portal}, {"keywords", "portal teleport link exit"},
                                      {"categoryTitle", "Portals"}, {"description", "An exit of the portal " + portal + " here"}};
        const int rank = nodeSearchRank(text, entry);
        if (rank < 0) continue;
        rows.push_back({rank, order++, tr("Portal: %1    Portals").arg(QString::fromStdString(portal)),
                        tr("An exit of the portal %1 here: drag from its nub to an input").arg(QString::fromStdString(portal)),
                        QStringLiteral("portal:") + QString::fromStdString(portal), theme::nodeCategoryColor("utility").lighter(130)});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.rank != b.rank ? a.rank < b.rank : a.order < b.order; });
    for (const Row& r : rows) {
        auto* item = new QListWidgetItem(r.shown, searchList_);
        item->setForeground(r.color);
        item->setData(Qt::UserRole, r.data);
        item->setToolTip(r.about);
    }
    if (searchList_->count() > 0) searchList_->setCurrentRow(0);
    else searchAbout_->setText(tr("No node matches. Try what it does: symmetry, density, frizz, collision..."));
}

void NodeGraphView::setSearchText(const QString& text) {
    if (!searchOpen()) openSearch();
    searchEdit_->setText(text);
}

QStringList NodeGraphView::searchRows() const {
    QStringList rows;
    for (int i = 0; i < searchList_->count(); ++i) rows << searchList_->item(i)->text();
    return rows;
}

std::vector<std::string> NodeGraphView::searchTypes() const {
    std::vector<std::string> types;
    for (int i = 0; i < searchList_->count(); ++i) types.push_back(searchList_->item(i)->data(Qt::UserRole).toString().toStdString());
    return types;
}

QString NodeGraphView::searchAbout() const { return searchAbout_->text(); }

void NodeGraphView::acceptSearch() { addFromSearch(); }

void NodeGraphView::addFromSearch() {
    QListWidgetItem* item = searchList_->currentItem();
    if (!item || !document_) return;
    std::string type = item->data(Qt::UserRole).toString().toStdString();
    const int wire = searchWire_;
    closeSearch();
    if (type.rfind("portal:", 0) == 0) {
        addPortalExit(type.substr(7), toWidget(searchScene_));
        return;
    }
    nlohmann::json params;
    std::string name;
    if (type.rfind(kMapEntry, 0) == 0) {
        // A painted map: a Map node reading it, named after it.
        name = type.substr(std::strlen(kMapEntry));
        params = {{"name", name}};
        type = "Map";
    }
    // Into the wire under the cursor, else after the selected node when the new one takes what it
    // gives (placed to its right); else at the cursor.
    addNodeOfType(type, searchScene_, true, wire, params, name);
}

std::string NodeGraphView::addNodeOfType(const std::string& type, const QPointF& at, bool wire, int onWire, const nlohmann::json& params,
                                         const std::string& name) {
    if (!document_) return {};
    GroomDocument::AddOptions options;
    options.name = name;
    options.params = params;
    QPointF where = at;
    if (std::isnan(where.x())) where = toScene(QPointF(width() / 2.0, height() / 2.0)) - QPointF(kNodeWidth / 2.0, kHeader);
    GroomDocument::NodeLink link;
    if (onWire >= 0 && wireLink(onWire, &link.from, &link.output, &link.to, &link.input)) {
        options.onLink = link;
    } else if (wire && selection_.size() == 1 && nodeIndex(selected_) >= 0) {
        options.after = selected_;
        // Placed after it (to its right) when nowhere was asked.
        if (std::isnan(at.x())) where = nodes_[nodeIndex(selected_)].pos + QPointF(kColumn / 2.0, kRowGap);
    }
    std::string made, wiredAfter, note;
    const std::string error =
        document_->addNode(type, static_cast<float>(where.x()), static_cast<float>(where.y()), &made, options, &wiredAfter, &note);
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return {};
    }
    if (!note.empty()) emit message(QString::fromStdString(note));
    else if (!wiredAfter.empty()) emit status(tr("Added %1 after %2").arg(QString::fromStdString(made), QString::fromStdString(wiredAfter)));
    else emit status(tr("Added %1").arg(QString::fromStdString(made)));
    emit edited();
    refresh();
    selectNode(made);
    return made;
}

// ---- Arranging by hand, portals, compounds (graph_arrange.h, graph_portals.h) --------------------

void NodeGraphView::alignSelected(const std::string& mode) {
    if (!document_) return;
    std::vector<LayoutBox> boxes;
    for (const std::string& n : selection_) {
        const int i = nodeIndex(n);
        if (i >= 0) boxes.push_back({n, nodes_[i].pos.x(), nodes_[i].pos.y(), nodes_[i].size.width(), nodes_[i].size.height()});
    }
    if (boxes.size() < 2) {
        emit message(tr("Select two nodes or more to align"));
        return;
    }
    std::map<std::string, std::pair<double, double>> placed;
    try {
        placed = alignBoxes(boxes, mode);
    } catch (const std::exception& e) {
        emit message(QString::fromStdString(e.what()));
        return;
    }
    const std::string error = document_->editLayout("align " + mode, [&](nlohmann::json& g) {
        for (nlohmann::json& n : g["nodes"]) {
            const auto at = placed.find(n.value("name", std::string()));
            if (at != placed.end()) n["ui"] = {{"x", at->second.first}, {"y", at->second.second}};
        }
    });
    if (!error.empty()) emit message(QString::fromStdString(error));
    for (const auto& [name, at] : placed) laidOut_.erase(name);
    refresh();
}

void NodeGraphView::tidySelection() {
    if (!document_) return;
    if (selection_.size() < 2) {
        tidyLayout();
        return;
    }
    std::vector<LayoutBox> boxes;
    for (const std::string& n : selection_) {
        const int i = nodeIndex(n);
        if (i >= 0) boxes.push_back({n, nodes_[i].pos.x(), nodes_[i].pos.y(), nodes_[i].size.width(), nodes_[i].size.height()});
    }
    std::map<std::string, std::pair<double, double>> placed;
    try {
        placed = arrangeSelection(GraphEdit(document_->viewGraph().dump()), boxes, kColumn, kRowGap);
    } catch (const std::exception& e) {
        emit message(QString::fromStdString(e.what()));
        return;
    }
    document_->editLayout("tidy selection", [&](nlohmann::json& g) {
        for (nlohmann::json& n : g["nodes"]) {
            const auto at = placed.find(n.value("name", std::string()));
            if (at != placed.end()) n["ui"] = {{"x", at->second.first}, {"y", at->second.second}};
        }
    });
    for (const auto& [name, at] : placed) laidOut_.erase(name);
    refresh();
}

void NodeGraphView::selectWire(int wire) {
    selectNode({});
    selectedBackdrop_ = selectedPin_ = selectedPortal_ = -1;
    selectedWire_ = wire >= 0 && wire < wireCount() ? wire : -1;
    update();
}

void NodeGraphView::setWireStyle(const std::string& style) {
    const std::vector<std::string> styles = wireStyles();
    wireStyle_ = std::find(styles.begin(), styles.end(), style) != styles.end() ? style : std::string("smooth");
    for (Wire& w : wires_) w.path = wirePathFor(w);
    update();
}

void NodeGraphView::openCompound(const std::string& node) {
    if (!document_) return;
    const std::string error = document_->enterCompound(node);
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    selectNode({});
    laidOut_.clear();
    fitted_ = false;
    viewTouched_ = false;
    refresh();
    emit status(tr("Inside %1: edit its nodes; U or the path bar goes back out").arg(QString::fromStdString(node)));
}

void NodeGraphView::leaveCompound() {
    if (!document_ || document_->scope().empty()) return;
    const std::string scope = document_->scope();
    const std::string left = scope.substr(scope.rfind('/') == std::string::npos ? 0 : scope.rfind('/') + 1);
    document_->leaveCompound();
    laidOut_.clear();
    fitted_ = false;
    viewTouched_ = false;
    refresh();
    selectNode(left);  // the compound just left, selected
}

std::string NodeGraphView::scopePath() const { return document_ ? document_->scope() : std::string(); }

bool NodeGraphView::menuPort(std::string* node, std::string* input) const {
    if (menuPortNode_ < 0 || menuPortNode_ >= nodeCount() || menuPortInput_ < 0 ||
        menuPortInput_ >= static_cast<int>(nodes_[menuPortNode_].inputs.size())) {
        return false;
    }
    if (node) *node = nodes_[menuPortNode_].name;
    if (input) *input = nodes_[menuPortNode_].inputs[menuPortInput_].name;
    return true;
}

std::string NodeGraphView::menuPortal(int* exit) const {
    if (menuPortal_ < 0 || menuPortal_ >= static_cast<int>(portals_.size())) return {};
    if (exit) *exit = portals_[menuPortal_].exit;
    return portals_[menuPortal_].name;
}

void NodeGraphView::pinWire(int wire, const QPointF& scene) {
    if (!document_ || wire < 0 || wire >= wireCount()) return;
    std::string from, output, to, input, made;
    wireLink(wire, &from, &output, &to, &input);
    const QPointF at = snappedIf(scene);
    const std::string error = document_->addPin(to, input, at.x(), at.y(), pieceAt(wires_[wire], scene), &made);
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    emit status(tr("Pinned %1.%2: drag the pin to route the wire, Delete removes it").arg(QString::fromStdString(to), QString::fromStdString(input)));
    emit edited();
    refresh();
    selectPin(made);
}

void NodeGraphView::teleportWire(int wire) {
    if (!document_ || wire < 0 || wire >= wireCount()) return;
    std::string from, output, to, input;
    wireLink(wire, &from, &output, &to, &input);
    const Wire& w = wires_[wire];
    // The entry just right of the output, the exit just left of the input.
    const QPointF entry = outputPos(nodes_[w.from], w.output) + QPointF(40.0, 0.0);
    const QPointF exit = inputPos(nodes_[w.to], w.input) - QPointF(56.0, 0.0);
    const std::string error = document_->teleportLink(to, input, exit.x(), exit.y(), entry.x(), entry.y());
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    selectedWire_ = -1;
    emit status(tr("%1 teleported: drag a portal dot to place it; drag from an exit's nub for another link of it, double-click renames it")
                    .arg(QString::fromStdString(from)));
    emit edited();
    refresh();
}

void NodeGraphView::renamePortal(const std::string& portal) {
    if (!document_) return;
    promptName(tr("Portal name"), tr("Name"), [this, portal](const QString& name) {
        const std::string error = document_->renamePortal(portal, name.trimmed().toStdString());
        if (!error.empty()) emit message(QString::fromStdString(error));
        refresh();
    });
    setPromptText(QString::fromStdString(portal));
}

void NodeGraphView::removePortal(const std::string& portal, int exit) {
    if (!document_) return;
    const std::string error = exit < 0 ? document_->removePortal(portal) : document_->removePortalExit(portal, exit);
    if (!error.empty()) emit message(QString::fromStdString(error));
    else emit status(exit < 0 ? tr("Portal removed: its links drawn as wires again") : tr("Portal exit removed: its links drawn as wires again"));
    selectedPortal_ = -1;
    refresh();
}

void NodeGraphView::addPortalExit(const std::string& portal, const QPointF& widget) {
    if (!document_) return;
    const QPointF at = snappedIf(toScene(widget));
    const std::string error = document_->addPortalExit(portal, at.x(), at.y());
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    emit status(tr("An exit of %1: drag from its nub to an input").arg(QString::fromStdString(portal)));
    refresh();
}

void NodeGraphView::setPassThrough(const std::string& node, const std::string& input, bool on) {
    if (!document_) return;
    const std::string error = document_->setPassThrough(node, input, on);
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    if (on) emit status(tr("%1 hands its %2 on: the thru_%2 output").arg(QString::fromStdString(node), QString::fromStdString(input)));
    emit edited();
    refresh();
}

void NodeGraphView::setBackdropFolded(int index, bool folded) {
    if (!document_ || index < 0 || index >= backdropCount()) return;
    const std::string error = document_->setBackdropFolded(index, folded);
    if (!error.empty()) emit message(QString::fromStdString(error));
    // A folded backdrop's nodes leave the selection (they are hidden).
    refresh();
    std::vector<std::string> kept;
    for (const std::string& n : selection_) {
        const int i = nodeIndex(n);
        if (i >= 0 && nodes_[i].foldedIn < 0) kept.push_back(n);
    }
    setSelection(kept);
    selectBackdrop(index);
}

bool NodeGraphView::backdropFolded(int index) const { return index >= 0 && index < backdropCount() && backdrops_[index].folded; }

bool NodeGraphView::nodeHidden(const std::string& node) const {
    const int i = nodeIndex(node);
    return i >= 0 && nodes_[i].foldedIn >= 0;
}

std::vector<NodeGraphView::PortalDot> NodeGraphView::portalDots() const {
    std::vector<PortalDot> out;
    for (const Portal& p : portals_) out.push_back({p.name, p.exit, p.pos});
    return out;
}

QPointF NodeGraphView::portPoint(const std::string& node, const std::string& port, bool output) const {
    const int i = nodeIndex(node);
    if (i < 0) return QPointF(std::nan(""), 0.0);
    const std::vector<Port>& ports = output ? nodes_[i].outputs : nodes_[i].inputs;
    for (std::size_t p = 0; p < ports.size(); ++p) {
        if (ports[p].name == port) return toWidget(output ? outputPos(nodes_[i], static_cast<int>(p)) : inputPos(nodes_[i], static_cast<int>(p)));
    }
    return QPointF(std::nan(""), 0.0);
}

bool NodeGraphView::eventFilter(QObject* watched, QEvent* event) {
    if (watched == promptEdit_ && event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        prompt_->hide();
        promptDone_ = nullptr;
        setFocus();
        return true;
    }
    // Escape in the search closes it; Tab there stays in the search.
    if ((watched == searchEdit_ || watched == searchList_) && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            closeSearch();
            return true;
        }
        if (key->key() == Qt::Key_Down && watched == searchEdit_ && searchList_->count() > 0) {
            searchList_->setCurrentRow(std::min(searchList_->currentRow() + 1, searchList_->count() - 1));
            return true;
        }
        if (key->key() == Qt::Key_Up && watched == searchEdit_ && searchList_->count() > 0) {
            searchList_->setCurrentRow(std::max(searchList_->currentRow() - 1, 0));
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void NodeGraphView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if ((!fitted_ || !viewTouched_) && !nodes_.empty()) frameReadable();
}

void NodeGraphView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!fitted_ && !nodes_.empty()) frameReadable();
}

}  // namespace groomlab::app
