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
#include <QListWidget>
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
#include "groomlab/graph_pins.h"
#include "groomlab/node_catalog.h"
#include "groomlab/node_library.h"

namespace groomlab::app {

namespace {

constexpr qreal kNodeWidth = 160.0;
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
    search_->setColor(theme::glassSolid());
    auto* layout = new QVBoxLayout(search_);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);
    searchEdit_ = new QLineEdit(search_);
    searchEdit_->setPlaceholderText(tr("Add a node..."));
    searchEdit_->setFont(theme::uiFont(14, 600));
    searchList_ = new QListWidget(search_);
    searchList_->setFont(theme::uiFont(13, 500));
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
    if (!document_) return;
    // The backdrops as the graph keeps them (title, a rectangle, an optional color).
    const nlohmann::json& graph = document_->graph();
    if (graph.contains("backdrops") && graph["backdrops"].is_array()) {
        for (const nlohmann::json& b : graph["backdrops"]) {
            if (!b.is_object()) continue;
            auto number = [&](const char* key, double fallback) { return b.contains(key) && b[key].is_number() ? b[key].get<double>() : fallback; };
            Backdrop drop;
            drop.title = QString::fromStdString(b.value("title", std::string("Backdrop")));
            drop.rect = QRectF(number("x", 0.0), number("y", 0.0), std::max(1.0, number("w", 200.0)), std::max(1.0, number("h", 150.0)));
            drop.color = theme::nodeBody().lighter(140);
            if (b.contains("color") && b["color"].is_array() && b["color"].size() == 3 && b["color"][0].is_number()) {
                drop.color = QColor(std::clamp(b["color"][0].get<int>(), 0, 255), std::clamp(b["color"][1].get<int>(), 0, 255),
                                    std::clamp(b["color"][2].get<int>(), 0, 255));
            }
            backdrops_.push_back(std::move(drop));
        }
    }
    if (selectedBackdrop_ >= static_cast<int>(backdrops_.size())) selectedBackdrop_ = -1;
    for (const GroomDocument::NodeInfo& info : document_->nodes()) {
        Node node;
        node.name = info.name;
        node.type = info.type;
        node.bypass = info.bypass;
        node.evaluation = info.evaluation;
        // The node's ports as it is set (its exposed params' after its own: Field, Curve or Value).
        if (const nlohmann::json* spec = document_->nodeSpec(info.name)) {
            node.category = spec->value("category", std::string());
            for (const nlohmann::json& p : spec->value("inputs", nlohmann::json::array())) {
                node.inputs.push_back({p.value("name", std::string()), p.value("type", std::string()), p.value("optional", false)});
            }
            for (const nlohmann::json& p : spec->value("outputs", nlohmann::json::array())) {
                node.outputs.push_back({p.value("name", std::string()), p.value("type", std::string()), false});
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
        const std::size_t rows = std::max<std::size_t>(std::max(node.inputs.size(), node.outputs.size()), 1);
        node.size = QSizeF(kNodeWidth, kHeader + kBodyPadding * 2 + rows * kRow);
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
    auto index = [this](const std::string& name) {
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            if (nodes_[i].name == name) return static_cast<int>(i);
        }
        return -1;
    };
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
        // The pins it runs through (each made once: a bundle's wires share theirs).
        for (const std::string& id : pinRoute(graph, c.to, c.input)) {
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
    if (!viewed_.empty() && index(viewed_) < 0) setViewedNode({});
}

void NodeGraphView::autoLayout() {
    // Nodes without a position: columns by how far they are from the sources (the longest path
    // in), rows in the graph's order; placed to the right of what has a position already.
    std::vector<int> depth(nodes_.size(), 0);
    if (document_) {
        std::map<std::string, int> index;
        for (std::size_t i = 0; i < nodes_.size(); ++i) index[nodes_[i].name] = static_cast<int>(i);
        const std::vector<GroomDocument::ConnectionInfo> connections = document_->connections();
        // Relax the longest paths: graphs are acyclic and small.
        for (std::size_t pass = 0; pass < nodes_.size() + 1; ++pass) {
            bool changed = false;
            for (const GroomDocument::ConnectionInfo& c : connections) {
                const auto a = index.find(c.from), b = index.find(c.to);
                if (a == index.end() || b == index.end()) continue;
                if (depth[b->second] < depth[a->second] + 1) {
                    depth[b->second] = depth[a->second] + 1;
                    changed = true;
                }
            }
            if (!changed) break;
        }
    }
    // Over the viewport (a tall view), a long chain wraps into blocks of columns, one under the
    // other, so the whole graph fits at a readable scale; docked (a wide, short strip, as Maya's
    // Graph Editor), it runs in one row and frameReadable shows its look end.
    const int kColumnsPerBlock = viewport_ ? 7 : std::numeric_limits<int>::max() / 2;
    std::map<int, int> rows;
    std::map<int, int> blockRows;  // the tallest column in each block, for the block's height
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (std::isnan(nodes_[i].pos.x())) blockRows[depth[i] / kColumnsPerBlock] = std::max(blockRows[depth[i] / kColumnsPerBlock], ++rows[depth[i]]);
    }
    rows.clear();
    std::map<int, qreal> blockTop;
    qreal top = 0.0;
    for (const auto& [block, count] : blockRows) {
        blockTop[block] = top;
        top += count * kRowGap + 60.0;
    }
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        Node& node = nodes_[i];
        if (!std::isnan(node.pos.x())) continue;
        const int row = rows[depth[i]]++;
        node.pos = QPointF((depth[i] % kColumnsPerBlock) * kColumn, blockTop[depth[i] / kColumnsPerBlock] + row * kRowGap);
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

QPainterPath NodeGraphView::wirePath(const QPointF& from, const QPointF& to, qreal below) const {
    QPainterPath path(from);
    if (to.x() >= from.x() + 30.0) {
        const qreal dx = std::max(60.0, std::abs(to.x() - from.x()) * 0.5);
        path.cubicTo(from + QPointF(dx, 0), to - QPointF(dx, 0), to);
        return path;
    }
    // Backwards (the input left of the output): out to the right, down under both nodes, back
    // along under them, up into the input from its left, so the wire never crosses the cards.
    const qreal y = std::isnan(below) ? std::max(from.y(), to.y()) + 60.0 : below + 24.0;
    constexpr qreal r = 40.0;
    path.cubicTo(from + QPointF(r, 0), QPointF(from.x() + r, y), QPointF(from.x(), y));
    path.lineTo(to.x(), y);
    path.cubicTo(QPointF(to.x() - r, y), to - QPointF(r, 0), to);
    return path;
}

QPainterPath NodeGraphView::wirePathFor(const Wire& wire) const {
    const Node& from = nodes_[wire.from];
    const Node& to = nodes_[wire.to];
    if (!wire.pins.empty()) {
        // Through its pins: each piece leaves level and arrives level (as Maya's, and Unreal's
        // reroute nodes), wherever the pins lie.
        std::vector<QPointF> points{outputPos(from, wire.output)};
        for (int p : wire.pins) points.push_back(pins_[p].pos);
        points.push_back(inputPos(to, wire.input));
        QPainterPath path(points.front());
        for (std::size_t i = 1; i < points.size(); ++i) {
            const QPointF a = points[i - 1], b = points[i];
            const qreal dx = std::max(50.0, std::abs(b.x() - a.x()) * 0.5);
            path.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b);
        }
        return path;
    }
    const qreal below = std::max(from.pos.y() + from.size.height(), to.pos.y() + to.size.height());
    return wirePath(outputPos(from, wire.output), inputPos(to, wire.input), below);
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
    std::vector<QPointF> points{outputPos(nodes_[wire.from], wire.output)};
    for (int p : wire.pins) points.push_back(pins_[p].pos);
    points.push_back(inputPos(nodes_[wire.to], wire.input));
    int best = 0;
    qreal nearest = std::numeric_limits<qreal>::max();
    for (std::size_t k = 1; k < points.size(); ++k) {
        const QPointF a = points[k - 1], b = points[k];
        const qreal dx = std::max(50.0, std::abs(b.x() - a.x()) * 0.5);
        QPainterPath piece(a);
        piece.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b);
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

std::vector<std::string> NodeGraphView::wirePins(int wire) const {
    std::vector<std::string> ids;
    if (wire < 0 || wire >= wireCount()) return ids;
    for (int p : wires_[wire].pins) ids.push_back(pins_[p].id);
    return ids;
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
        if (QRectF(n.pos, n.size).intersects(scene.normalized()) && std::find(names.begin(), names.end(), n.name) == names.end()) {
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
        if (QRectF(nodes_[i].pos, nodes_[i].size).contains(scene)) return i;
    }
    return -1;
}

bool NodeGraphView::portAt(const QPointF& scene, int* node, int* port, bool* output) const {
    const qreal reach = kPortRadius * 2.2;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const Node& n = nodes_[i];
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
        if (stroker.createStroke(wires_[i].path).contains(scene)) return static_cast<int>(i);
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
    return QRectF(node.pos.x() + node.size.width() - 49.0, node.pos.y() + 5.0, 18.0, 18.0);
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
    const bool failed = errors_.count(node.name) > 0;
    const QColor color = theme::nodeCategoryColor(node.category);
    const qreal radius = theme::nodeRadius();
    painter.setOpacity(node.bypass ? 0.5 : 1.0);
    // The body, then the header clipped to the top corners: filled with the category color, or
    // (a tinted theme) a wash of it under an accent bar, as the Maya Graph Editor draws it.
    QPainterPath body;
    body.addRoundedRect(rect, radius, radius);
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::nodeBody());
    painter.drawPath(body);
    const QString header = theme::headerStyle();
    const bool minimal = header == QLatin1String("minimal");
    painter.save();
    painter.setClipPath(body);
    if (minimal) {
        // No header fill (the design's minimal cards): the category is the dot by the title.
    } else if (header == QLatin1String("tinted")) {
        QColor wash = color;
        wash.setAlpha(34);
        painter.setBrush(wash);
        painter.drawRect(QRectF(rect.topLeft(), QSizeF(rect.width(), kHeader)));
        painter.setBrush(color);
        painter.drawRect(QRectF(rect.topLeft(), QSizeF(rect.width(), 3.0)));
    } else {
        painter.setBrush(color);
        painter.drawRect(QRectF(rect.topLeft(), QSizeF(rect.width(), kHeader)));
    }
    painter.restore();
    // A thin border where the theme gives the nodes one (its color other than the body's).
    if (const QColor border = theme::nodeBorder(); border.isValid() && border != theme::nodeBody()) {
        painter.setPen(QPen(border, 1.0));
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
    painter.setFont(theme::uiFont(13, minimal ? 700 : 800));
    painter.setPen(headerText(color));
    const QRectF title(titleX, rect.y(), rect.right() - titleX - (node.linkableParams.empty() ? 28 : 50), kHeader);
    painter.drawText(title, Qt::AlignVCenter | Qt::AlignLeft, QString::fromStdString(node.name));
    // The node's time in its last run, before the bypass toggle (a tenth of a millisecond and up).
    if (const auto t = times_.find(node.name); t != times_.end() && t->second >= 0.05) {
        painter.setFont(theme::numberFont(9));
        QColor dim = headerText(color);
        dim.setAlpha(170);
        painter.setPen(dim);
        const QString ms = t->second < 10.0 ? QString::number(t->second, 'f', 1) : QString::number(std::lround(t->second));
        painter.drawText(QRectF(rect.x(), rect.y(), rect.width() - (node.linkableParams.empty() ? 30 : 55), kHeader),
                         Qt::AlignVCenter | Qt::AlignRight, ms + QStringLiteral(" ms"));
    }
    // A note (the node does nothing as set): a grey "i" on the node's corner (left of the error
    // badge when the node failed too).
    if (notes_.count(node.name)) {
        const QPointF corner(rect.right() - (failed ? 20.0 : 0.0), rect.top());
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme::graphNote());
        painter.drawEllipse(corner, 8.0, 8.0);
        painter.setFont(theme::uiFont(10, 800));
        painter.setPen(theme::readableOn(theme::graphNote()));
        painter.drawText(QRectF(corner.x() - 8.0, corner.y() - 8.0, 16.0, 16.0), Qt::AlignCenter, QStringLiteral("i"));
    }
    // When it runs, unless always: a tag under the card.
    if (node.evaluation == "viewportOnly" || node.evaluation == "renderOnly") {
        painter.setFont(theme::uiFont(10, 600));
        painter.setPen(theme::textMuted());
        painter.drawText(QRectF(rect.x() + 4, rect.bottom() + 2, rect.width() - 8, 16), Qt::AlignLeft | Qt::AlignVCenter,
                         node.evaluation == "viewportOnly" ? tr("viewport only") : tr("render only"));
    }
    // The bypass toggle in the header's corner.
    const QRectF bypass = bypassButtonRect(node);
    painter.setPen(QPen(headerText(color), 1.6));
    painter.setBrush(node.bypass ? headerText(color) : Qt::NoBrush);
    painter.drawEllipse(bypass.adjusted(3, 3, -3, -3));
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
    painter.setFont(theme::uiFont(11, 600));
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
        painter.setFont(theme::uiFont(10, 600));
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
    // Viewed (View Up to Here): a tag over the card, the viewport shows the graph up to here.
    if (node.name == viewed_) {
        painter.setFont(theme::uiFont(10, 800));
        const QString tag = tr("VIEWING");
        const qreal w = QFontMetricsF(painter.font()).horizontalAdvance(tag) + 14.0;
        const QRectF pill(rect.x() + 10, rect.y() - 20, w, 16);
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme::blueLight());
        painter.drawRoundedRect(pill, 8, 8);
        painter.setPen(theme::readableOn(theme::blueLight()));
        painter.drawText(pill, Qt::AlignCenter, tag);
        painter.setPen(QPen(theme::blueLight(), 1.5, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(rect.adjusted(-5, -5, 5, 5), radius + 5, radius + 5);
    }
    if (selected) {
        painter.setPen(QPen(theme::nodeSelected(), node.name == selected_ ? 2.5 : 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(rect.adjusted(-1, -1, 1, 1), radius + 1, radius + 1);
    }
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
    // The backdrops first: a tinted frame with its title bar and resize grip (as Maya's), the
    // groom still showing through; the selected one ringed.
    for (std::size_t i = 0; i < backdrops_.size(); ++i) {
        const Backdrop& b = backdrops_[i];
        const bool selected = static_cast<int>(i) == selectedBackdrop_;
        const qreal radius = theme::nodeRadius();
        QColor fill = b.color;
        fill.setAlpha(46);
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
        const QRectF title(b.rect.left() + 12, b.rect.top(), b.rect.width() - 24, kBackdropHeader);
        painter.drawText(title, Qt::AlignVCenter | Qt::AlignLeft, QFontMetricsF(painter.font()).elidedText(b.title, Qt::ElideRight, title.width()));
        painter.setPen(QPen(b.color.lighter(130), 1.5));
        for (const qreal k : {4.0, 8.0, 12.0}) {
            painter.drawLine(QPointF(b.rect.right() - k, b.rect.bottom() - 2), QPointF(b.rect.right() - 2, b.rect.bottom() - k));
        }
    }
    // The wires, the one being dragged with them, then the nodes, the selected one last.
    for (std::size_t i = 0; i < wires_.size(); ++i) {
        const Wire& w = wires_[i];
        if (dragWireInputNode_ == w.to && dragWireInputPort_ == w.input && drag_ == Drag::Wire) continue;  // picked up
        const bool selected = static_cast<int>(i) == selectedWire_ || static_cast<int>(i) == dropWire_ || static_cast<int>(i) == searchWire_;
        QColor color = theme::portColor(nodes_[w.from].outputs[w.output].type);
        color.setAlpha(selected ? 255 : 220);
        painter.setPen(QPen(selected ? theme::nodeSelected() : color, selected ? 3.0 : 2.0));
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
    if (drag_ == Drag::Wire && dragWireFrom_ >= 0) {
        QColor color = theme::portColor(nodes_[dragWireFrom_].outputs[dragWirePort_].type);
        painter.setPen(QPen(color, 2.0, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        const QPointF start = dragWirePin_ >= 0 ? pins_[dragWirePin_].pos : outputPos(nodes_[dragWireFrom_], dragWirePort_);
        painter.drawPath(wirePath(start, dragWireTo_));
    }
    // The nodes, the selected ones last (the current one on top).
    for (const Node& n : nodes_) {
        if (!isSelected(n.name)) drawNode(painter, n, false);
    }
    for (const Node& n : nodes_) {
        if (isSelected(n.name) && n.name != selected_) drawNode(painter, n, true);
    }
    if (const int current = nodeIndex(selected_); current >= 0) drawNode(painter, nodes_[current], true);
    painter.restore();
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
    dragWirePin_ = -1;
    int node = -1, port = -1;
    bool output = false;
    if (bypassButtonAt(pressScene_, &node)) {
        setBypass(nodes_[node].name, !nodes_[node].bypass);
        return;
    }
    if (inputsButtonAt(pressScene_, &node)) {
        toggleInputs(nodes_[node].name);
        return;
    }
    if (portAt(pressScene_, &node, &port, &output)) {
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
        }
        update();
        return;
    }
    node = nodeAt(pressScene_);
    if (node >= 0) {
        const std::string& name = nodes_[node].name;
        const Qt::KeyboardModifiers mods = event->modifiers();
        if (mods.testFlag(Qt::ControlModifier) || mods.testFlag(Qt::ShiftModifier)) {
            // Ctrl toggles the node in the selection, Shift adds it; no move.
            std::vector<std::string> names = selection_;
            const auto it = std::find(names.begin(), names.end(), name);
            if (it != names.end() && mods.testFlag(Qt::ControlModifier)) names.erase(it);
            else if (it == names.end()) names.push_back(name);
            else {
                names.erase(it);
                names.push_back(name);  // Shift on a selected node: it becomes the current one
            }
            setSelection(names);
            return;
        }
        drag_ = Drag::Move;
        dragNode_ = node;
        moveStart_ = nodes_[node].pos;
        // A node of the selection drags the whole selection; another one is selected alone.
        if (isSelected(name)) {
            std::vector<std::string> names = selection_;
            names.erase(std::find(names.begin(), names.end(), name));
            names.push_back(name);
            setSelection(names);
        } else {
            selectNode(name);
        }
        return;
    }
    // A backdrop's title bar: selected and moved with the nodes on it; its corner: resized.
    bool grip = false;
    if (const int b = backdropTitleAt(pressScene_, &grip); b >= 0) {
        selectBackdrop(b);
        drag_ = grip ? Drag::BackdropResize : Drag::Backdrop;
        dragBackdrop_ = b;
        backdropStart_ = backdrops_[b].rect;
        carried_.clear();
        if (!grip) {
            for (std::size_t i = 0; i < nodes_.size(); ++i) {
                if (backdropStart_.contains(QRectF(nodes_[i].pos, nodes_[i].size).center())) carried_.push_back(static_cast<int>(i));
            }
        }
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
        update();
        return;
    }
    // Empty space: a rubber band (a click alone deselects, unless Ctrl or Shift keeps the selection).
    drag_ = Drag::Band;
    band_ = QRectF(last_, last_);
    bandAdds_ = event->modifiers().testFlag(Qt::ControlModifier) || event->modifiers().testFlag(Qt::ShiftModifier);
    selectedWire_ = -1;
    if (!bandAdds_) selectNode({});
    update();
}

void NodeGraphView::mouseMoveEvent(QMouseEvent* event) {
    const QPointF now = event->position();
    const QPointF delta = now - last_;
    last_ = now;
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
            if (dragNode_ >= 0) {
                // The selection moves together (the pressed node is one of it); the wires follow.
                std::vector<int> moved;
                for (std::size_t i = 0; i < nodes_.size(); ++i) {
                    if (static_cast<int>(i) == dragNode_ || isSelected(nodes_[i].name)) moved.push_back(static_cast<int>(i));
                }
                // The pressed node goes where the cursor took it (on the grid with Snap); the rest
                // of the selection by the same step.
                const QPointF step = snappedIf(moveStart_ + (toScene(now) - pressScene_)) - nodes_[dragNode_].pos;
                for (int i : moved) nodes_[i].pos += step;
                for (Wire& w : wires_) {
                    if (std::find(moved.begin(), moved.end(), w.from) != moved.end() || std::find(moved.begin(), moved.end(), w.to) != moved.end()) {
                        w.path = wirePathFor(w);
                    }
                }
                // A lone node with no wires of its own over a wire: the wire lights up (dropped: it
                // goes in, as Maya's Graph Editor).
                dropWire_ = moved.size() == 1 ? wireUnderNode(dragNode_) : -1;
                update();
            }
            break;
        case Drag::Wire:
            dragWireTo_ = toScene(now);
            updateWireHint(dragWireTo_);
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
        case Drag::Band:
            band_.setBottomRight(now);
            update();
            break;
        case Drag::Backdrop:
        case Drag::BackdropResize:
            if (dragBackdrop_ >= 0 && dragBackdrop_ < static_cast<int>(backdrops_.size())) {
                const QPointF moved = toScene(now) - pressScene_;
                QRectF& rect = backdrops_[dragBackdrop_].rect;
                if (drag_ == Drag::BackdropResize) {
                    rect.setSize(QSizeF(std::max(120.0, backdropStart_.width() + moved.x()), std::max(80.0, backdropStart_.height() + moved.y())));
                } else {
                    const QPointF step = backdropStart_.topLeft() + moved - rect.topLeft();
                    rect.moveTopLeft(backdropStart_.topLeft() + moved);
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
        // Where it was put, and the nodes it carried (not undo steps, as node moves).
        if (dragBackdrop_ < static_cast<int>(backdrops_.size()) && backdrops_[dragBackdrop_].rect != backdropStart_) {
            const QRectF& r = backdrops_[dragBackdrop_].rect;
            document_->setBackdropRect(dragBackdrop_, r.x(), r.y(), r.width(), r.height());
            for (int i : carried_) {
                document_->setNodePosition(nodes_[i].name, static_cast<float>(nodes_[i].pos.x()), static_cast<float>(nodes_[i].pos.y()));
                laidOut_.erase(nodes_[i].name);
            }
        }
        dragBackdrop_ = -1;
        carried_.clear();
        return;
    }
    if (drag == Drag::Pin && dragPin_ >= 0 && document_) {
        // Dropped on another pin of its signal: they join (an undo step); else it stays where it
        // was put (not an undo step, as node moves).
        const int pin = dragPin_;
        dragPin_ = -1;
        if (pin < static_cast<int>(pins_.size()) && pins_[pin].pos != pinStart_) {
            int onto = -1;
            for (std::size_t i = 0; i < pins_.size(); ++i) {
                const QPointF d = pins_[i].pos - pins_[pin].pos;
                if (static_cast<int>(i) != pin && d.x() * d.x() + d.y() * d.y() <= 4.0 * kPinRadius * kPinRadius) onto = static_cast<int>(i);
            }
            if (onto >= 0) {
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
        return;
    }
    if (drag == Drag::Move && dragNode_ >= 0 && document_) {
        // Every moved node keeps where it was put (not an undo step, as Maya's moves).
        if ((nodes_[dragNode_].pos - moveStart_).manhattanLength() > 0.0) {
            for (const Node& n : nodes_) {
                if (n.name != nodes_[dragNode_].name && !isSelected(n.name)) continue;
                document_->setNodePosition(n.name, static_cast<float>(n.pos.x()), static_cast<float>(n.pos.y()));
                laidOut_.erase(n.name);
            }
        }
        // A lone unwired node dropped on a wire goes into it (Maya's insert_on_wire).
        const int wire = dropWire_;
        const std::string name = nodes_[dragNode_].name;
        dropWire_ = -1;
        dragNode_ = -1;
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
                if (dragWirePin_ >= 0) {  // out of a pin: through the pins up to it
                    error = document_->linkFromPin(pins_[dragWirePin_].id, to.name, to.inputs[port].name);
                } else {
                    error = document_->connectNodes(from.name, first ? std::string() : from.outputs[dragWirePort_].name, to.name,
                                                    to.inputs[port].name);
                }
                if (error.empty()) emit edited();
            }
        } else if (const int pin = dragWirePin_ < 0 ? pinAt(at) : -1; pin >= 0) {
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
        dragWireFrom_ = dragWirePort_ = dragWireInputNode_ = dragWireInputPort_ = dragWirePin_ = -1;
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

void NodeGraphView::setNodeErrors(const std::vector<std::pair<std::string, std::string>>& errors) {
    errors_.clear();
    for (const auto& [name, error] : errors) errors_[name] = error;
    update();
}

std::string NodeGraphView::nodeError(const std::string& node) const {
    const auto it = errors_.find(node);
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
    if (selected_.empty() || selected_ == viewed_) {
        if (!viewed_.empty()) emit message(tr("Viewing the graph's output again"));
        setViewedNode({});
        return;
    }
    const int i = nodeIndex(selected_);
    if (i < 0) return;
    const bool strands = std::any_of(nodes_[i].outputs.begin(), nodes_[i].outputs.end(), [](const Port& p) {
        return p.type == "strands" || p.type == "Strands" || p.type == "curves" || p.type == "Curves";
    });
    if (!strands) {
        emit message(tr("%1 gives no strands to view: pick a node with a strands output").arg(QString::fromStdString(selected_)));
        return;
    }
    setViewedNode(selected_);
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
    std::vector<std::string> made;
    const std::string error = document_->pasteNodes(QGuiApplication::clipboard()->text().toStdString(), static_cast<float>(kPasteOffsetX),
                                                    static_cast<float>(kPasteOffsetY), &made);
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

bool NodeGraphView::event(QEvent* event) {
    // A node's tooltip: its error, else its note (the badges' text).
    if (event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        const QPointF scene = toScene(QPointF(help->pos()));
        int node = -1;
        QString tip;
        if (inputsButtonAt(scene, &node)) {
            tip = tr("Expand Inputs: show or hide all connectable parameter ports");
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
    const auto it = notes_.find(node);
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
        emit selectionChanged(selected_);
    });
    setPromptText(QString::fromStdString(name));
}

void NodeGraphView::duplicateSelected() {
    if (!document_ || selected_.empty()) return;
    if (selection_.size() > 1) {
        // Several: copied and pasted beside, with the links between them (not the clipboard's).
        std::vector<std::string> pasted;
        const std::string text = document_->copyNodes(selection_);
        const std::string error = text.empty() ? std::string("the output node is one of a kind")
                                               : document_->pasteNodes(text, static_cast<float>(kPasteOffsetX), static_cast<float>(kPasteOffsetY), &pasted);
        if (!error.empty()) {
            emit message(QString::fromStdString(error));
            return;
        }
        emit edited();
        refresh();
        setSelection(pasted);
        return;
    }
    std::string made;
    const std::string error = document_->duplicateNode(selected_, &made);
    if (!error.empty()) {
        emit message(QString::fromStdString(error));
        return;
    }
    emit edited();
    refresh();
    selectNode(made);
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
    } else if (node >= 0 && nodes_[node].type == "Compound") {
        // A Compound opens: its nodes back in the graph to see and edit (Ctrl+Z groups them again).
        drag_ = Drag::None;
        selectNode(nodes_[node].name);
        ungroupSelected();
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
        std::string from, output, to, input, made;
        wireLink(w, &from, &output, &to, &input);
        const QPointF at = snappedIf(scene);
        const std::string error = document_->addPin(to, input, at.x(), at.y(), pieceAt(wires_[w], scene), &made);
        if (error.empty()) {
            emit status(tr("Pinned %1.%2: drag the pin to route the wire, Delete removes it")
                            .arg(QString::fromStdString(to), QString::fromStdString(input)));
            emit edited();
            refresh();
            selectPin(made);
        } else {
            emit message(QString::fromStdString(error));
        }
    }
    event->accept();
}

NodeGraphView::MenuTarget NodeGraphView::fillContextMenu(QMenu* menu, const QPointF& at) {
    menuPoint_ = at;
    const QPointF scene = toScene(at);
    const int node = nodeAt(scene);
    MenuTarget target = MenuTarget::Canvas;
    if (node >= 0) {
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
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
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
            if (!document_ || (selectedWire_ < 0 && selection_.empty() && selectedBackdrop_ < 0 && selectedPin_ < 0)) break;
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
    update();
    search_->move(std::clamp(static_cast<int>(where.x()), 0, std::max(0, width() - search_->width())),
                  std::clamp(static_cast<int>(where.y()), 0, std::max(0, height() - search_->height())));
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

namespace {

// A param name as words: "guideRadius" reads "guide radius" (Maya's pretty(), lowercase).
QString spacedName(const QString& name) {
    QString out;
    for (int i = 0; i < name.size(); ++i) {
        const QChar ch = name[i];
        if (ch == QLatin1Char('_')) {
            out += QLatin1Char(' ');
            continue;
        }
        if (i > 0 && ch.isUpper() && (name[i - 1].isLower() || name[i - 1].isDigit())) out += QLatin1Char(' ');
        out += ch;
    }
    return out.toLower();
}

bool allIn(const QStringList& words, const QString& text, const QString& orIn) {
    for (const QString& w : words) {
        if (!text.contains(w) && !orIn.contains(w)) return false;
    }
    return true;
}

}  // namespace

int nodeSearchRank(const QString& search, const nlohmann::json& spec) {
    const QString text = search.trimmed().toLower();
    if (text.isEmpty()) return 5;
    const std::string type = spec.value("type", std::string());
    const QString name = QString::fromStdString(type).toLower();
    const std::string label = spec.value("label", std::string());
    const QString shown = QString::fromStdString(label.empty() ? nodeLabel(type) : label).toLower();
    if (name.startsWith(text) || shown.startsWith(text)) return 0;
    if (name.contains(text) || shown.contains(text) || name.contains(QString(text).remove(QLatin1Char(' ')))) return 1;
    const QStringList words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    // Rank 2's words: the search keywords and the menu category's title ("deform" finds Bend first).
    const QString keywords = (QString::fromStdString(spec.value("keywords", std::string())) + QLatin1Char(' ') +
                              QString::fromStdString(spec.value("categoryTitle", std::string())))
                                 .toLower();
    QString params, details;
    for (const nlohmann::json& p : spec.value("params", nlohmann::json::array())) {
        const QString pname = QString::fromStdString(p.value("name", std::string()));
        params += pname.toLower() + QLatin1Char(' ') + spacedName(pname) + QLatin1Char(' ');
        details += QString::fromStdString(p.value("description", std::string())).toLower() + QLatin1Char(' ');
    }
    const QString about =
        (QString::fromStdString(spec.value("categoryTitle", std::string())) + QLatin1Char(' ') + QString::fromStdString(spec.value("description", std::string())))
            .toLower();
    if (allIn(words, keywords, name)) return 2;
    if (allIn(words, params, name)) return 3;
    if (allIn(words, about, name)) return 4;
    const QString everything = name + QLatin1Char(' ') + keywords + QLatin1Char(' ') + params + QLatin1Char(' ') + about + QLatin1Char(' ') + details;
    if (allIn(words, everything, everything)) return 5;
    return -1;
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
