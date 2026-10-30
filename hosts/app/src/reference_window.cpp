// SPDX-License-Identifier: Apache-2.0
// The app's Photos window (Align Photo..., Guides from Photos): reference_window.h.
#include "reference_window.h"

#include <algorithm>
#include <cmath>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSpinBox>
#include <QTabBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <nlohmann/json.hpp>

#include "glass.h"
#include "groom_document.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/texture.h"
#include "main_window.h"
#include "offscreen.h"
#include "photos_panel.h"
#include "theme.h"

namespace groomlab::app {

namespace {

using json = nlohmann::json;
using Pixels = std::vector<std::array<float, 2>>;

const QColor kBackground(46, 48, 53);
const QColor kArrow(255, 158, 38);      // the flow arrows (the Reference Flow tool's orange)
const QColor kArrowNew(255, 230, 102);  // the one being drawn
const QColor kErase(255, 77, 77);
const QColor kBad(255, 70, 70);
const QColor kHairTint(80, 230, 120, 90);
const QColor kField(150, 215, 255, 210);
const QColor kInclude(80, 230, 120, 120);
const QColor kExclude(255, 80, 80, 120);
const char* const kGroup = "fromReference";

// A flag of a photo's "display" (the Photos tab's settings, JSON text): locked, keepFocal.
bool displayFlag(const ReferencePhoto& p, const char* key) {
    const json j = json::parse(p.display.empty() ? std::string("{}") : p.display, nullptr, false);
    return j.is_object() && j.value(key, false);
}

void drawPolyline(QPainter& painter, const Pixels& points) {
    QPolygonF poly;
    for (const auto& p : points) poly << QPointF(p[0], p[1]);
    if (poly.size() > 1) painter.drawPolyline(poly);
    else if (poly.size() == 1) painter.drawPoint(poly[0]);
}

void drawArrow(QPainter& painter, const ReferenceArrow& arrow, const QColor& color) {
    QPen pen(color, 2.0);
    pen.setCosmetic(true);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    drawPolyline(painter, arrow.line);
    if (arrow.hasHead) drawPolyline(painter, Pixels(arrow.head.begin(), arrow.head.end()));
}

json cameraJson(const RefCamera& c) {
    json rot = json::array();
    for (const auto& row : c.pose.rotation) rot.push_back({row[0], row[1], row[2]});
    return {{"rotation", rot}, {"position", {c.pose.position[0], c.pose.position[1], c.pose.position[2]}}, {"focal_px", c.focalPx}};
}

std::optional<RefCamera> cameraFrom(const json& j, int width, int height) {
    if (!j.is_object() || !j.contains("rotation") || !j.contains("position")) return std::nullopt;
    RefCamera c;
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t k = 0; k < 3; ++k) c.pose.rotation[i][k] = j["rotation"].at(i).at(k).get<double>();
    for (std::size_t i = 0; i < 3; ++i) c.pose.position[i] = j["position"].at(i).get<double>();
    c.focalPx = j.value("focal_px", 0.0);
    c.width = width;
    c.height = height;
    return c;
}

}  // namespace

// ---- PhotoView ----------------------------------------------------------------------------------

PhotoView::PhotoView(ReferenceWindow* window, QWidget* parent) : QWidget(parent), window_(window) {
    setObjectName(QStringLiteral("referencePhotoView"));
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumSize(220, 180);
}

void PhotoView::photoChanged() {
    userMoved_ = false;
    fit();
}

void PhotoView::fit() {
    const ReferencePhoto* p = window_->photo();
    if (!p || p->width <= 0 || p->height <= 0) {
        update();
        return;
    }
    scale_ = std::min(width() / static_cast<double>(p->width), height() / static_cast<double>(p->height)) * 0.96;
    offset_ = QPointF((width() - p->width * scale_) * 0.5, (height() - p->height * scale_) * 0.5);
    userMoved_ = false;
    update();
}

QPointF PhotoView::toPhoto(const QPointF& w) const { return (w - offset_) / scale_; }
QPointF PhotoView::toWidget(const QPointF& p) const { return offset_ + p * scale_; }

void PhotoView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (!userMoved_) fit();
}

void PhotoView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), kBackground);
    ReferencePhoto* p = window_->photo();
    if (!p) {
        painter.setPen(QColor(200, 200, 200));
        painter.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, tr("Import a photo (Import Photo... above)"));
        return;
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.save();
    painter.setTransform(QTransform(scale_, 0, 0, scale_, offset_.x(), offset_.y()));
    const QRectF photoRect(0, 0, p->width, p->height);
    if (!p->picture.isNull()) painter.drawImage(photoRect, p->picture);
    else painter.fillRect(photoRect, QColor(70, 70, 70));
    if (window_->maskTint() || window_->fieldLines()) {
        if (const ReferenceViewPreview* preview = window_->preview()) {
            const QRectF crop(preview->crop[0], preview->crop[1], preview->crop[2], preview->crop[3]);
            if (window_->maskTint()) painter.drawImage(crop, window_->previewTint());
            if (window_->fieldLines()) {
                QPen pen(kField, 1.2);
                pen.setCosmetic(true);
                painter.setPen(pen);
                for (const auto& l : preview->flowLines) painter.drawLine(QPointF(l[0], l[1]), QPointF(l[2], l[3]));
            }
        }
    }
    for (const RefMaskStroke& s : p->editor.maskStrokes()) {
        QPen pen(s.include ? kInclude : kExclude, 2.0 * s.radius);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        drawPolyline(painter, s.points);
    }
    {
        QPen pen(QColor(255, 255, 255, static_cast<int>(255 * window_->overlayOpacity())), 1.0);
        pen.setCosmetic(true);
        painter.setPen(pen);
        for (const auto& l : window_->overlayLines()) painter.drawLine(QPointF(l[0], l[1]), QPointF(l[2], l[3]));
    }
    for (const ReferenceArrow& a : window_->arrows()) drawArrow(painter, a, kArrow);
    if ((drag_ == Drag::Stroke || drag_ == Drag::Erase) && path_.size() >= 2)
        drawArrow(painter, referenceArrow(path_, drag_ == Drag::Stroke ? p->width / 40.0f : 0.0f), drag_ == Drag::Stroke ? kArrowNew : kErase);
    if (drag_ == Drag::Mask && !path_.empty()) {
        QPen pen(include_ ? kInclude : kExclude, 2.0 * radius_);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        drawPolyline(painter, path_);
    }
    painter.restore();
    // The landmarks, as the mesh view's dots; with a solve, a line to where each mesh point lands.
    const MarkerLists& lists = p->editor.landmarks();
    const auto& points = lists.points(kLandmarkPhoto);
    const ReferenceLandmarkSolve& solve = p->solve;
    const MarkerHover& hover = window_->hover();
    for (std::size_t i = 0; i < points.size(); ++i) {
        const QPointF at = toWidget(QPointF(points[i].point.x, points[i].point.y));
        const bool bad = solve.ok && i < solve.bad.size() && solve.bad[i];
        if (solve.ok && i < solve.projected.size() && solve.projected[i][0] >= 0.0) {
            const QPointF lands = toWidget(QPointF(solve.projected[i][0], solve.projected[i][1]));
            painter.setPen(QPen(bad ? kBad : markerSideColor(kLandmarkMesh), 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawLine(at, lands);
            painter.drawEllipse(lands, 2.5, 2.5);
        }
        const bool lit = hover && hover->second == i && (hover->first == kLandmarkPhoto || lists.matched(hover->first, i));
        paintMarkerDot(painter, at, static_cast<int>(i) + 1, bad ? kBad : markerSideColor(kLandmarkPhoto),
                       lists.matched(kLandmarkPhoto, i), lit);
    }
    if ((window_->mode() == ReferenceWindow::Mode::Hair || window_->mode() == ReferenceWindow::Mode::Backdrop) && hoverPos_ &&
        drag_ == Drag::None) {
        painter.setPen(QPen(QColor(255, 255, 255, 180), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(*hoverPos_, window_->brushSize(), window_->brushSize());
    }
}

void PhotoView::mousePressEvent(QMouseEvent* event) {
    const QPointF pos = event->position();
    last_ = pos;
    const bool alt = event->modifiers() & Qt::AltModifier;
    const bool ctrl = event->modifiers() & Qt::ControlModifier;
    if (event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && alt)) {
        drag_ = Drag::Pan;
        return;
    }
    ReferencePhoto* p = window_->photo();
    if (event->button() != Qt::LeftButton || !p) return;
    const QPointF uv = toPhoto(pos);
    switch (window_->mode()) {
        case ReferenceWindow::Mode::Landmarks: {
            const int near = window_->photoPointNear(uv, 10.0 / scale_);
            if (ctrl) {
                if (near >= 0) window_->deleteLandmark(kLandmarkPhoto, static_cast<std::size_t>(near));
                drag_ = Drag::Owned;
                return;
            }
            if (near >= 0) {
                drag_ = Drag::Point;
                dragIndex_ = static_cast<std::size_t>(near);
                firstMove_ = true;
                return;
            }
            dragIndex_ = window_->addLandmark(kLandmarkPhoto, Vec3f(static_cast<float>(uv.x()), static_cast<float>(uv.y()), 0.0f));
            drag_ = Drag::Point;
            firstMove_ = false;  // sliding the new point joins its add
            return;
        }
        case ReferenceWindow::Mode::Flow:
            drag_ = ctrl ? Drag::Erase : Drag::Stroke;
            path_ = {{static_cast<float>(uv.x()), static_cast<float>(uv.y())}};
            break;
        default:
            drag_ = Drag::Mask;
            include_ = (window_->mode() == ReferenceWindow::Mode::Hair) != ctrl;
            radius_ = static_cast<float>(window_->brushSize() / scale_);
            path_ = {{static_cast<float>(uv.x()), static_cast<float>(uv.y())}};
            break;
    }
    update();
}

void PhotoView::mouseMoveEvent(QMouseEvent* event) {
    const QPointF pos = event->position();
    const QPointF delta = pos - last_;
    last_ = pos;
    hoverPos_ = pos;
    const QPointF uv = toPhoto(pos);
    ReferencePhoto* p = window_->photo();
    switch (drag_) {
        case Drag::Pan:
            offset_ += delta;
            userMoved_ = true;
            update();
            return;
        case Drag::Point:
            window_->moveLandmark(kLandmarkPhoto, dragIndex_, Vec3f(static_cast<float>(uv.x()), static_cast<float>(uv.y()), 0.0f),
                                  firstMove_);
            firstMove_ = false;
            return;
        case Drag::Stroke:
        case Drag::Erase:
        case Drag::Mask: {
            const auto& back = path_.back();
            const double spacing = drag_ == Drag::Mask ? std::max(0.5, radius_ * 0.3) : (p ? p->width / 300.0 : 1.0);
            if (std::hypot(uv.x() - back[0], uv.y() - back[1]) >= spacing)
                path_.push_back({static_cast<float>(uv.x()), static_cast<float>(uv.y())});
            update();
            return;
        }
        case Drag::Owned:
            return;
        case Drag::None:
            break;
    }
    if (window_->mode() == ReferenceWindow::Mode::Landmarks && p) {
        const int near = window_->photoPointNear(uv, 10.0 / scale_);
        if (near >= 0) window_->setHover(std::make_pair(kLandmarkPhoto, static_cast<std::size_t>(near)));
        else if (window_->hover() && window_->hover()->first == kLandmarkPhoto) window_->setHover(std::nullopt);
    }
    update();
}

void PhotoView::mouseReleaseEvent(QMouseEvent*) {
    const Drag drag = drag_;
    drag_ = Drag::None;
    ReferencePhoto* p = window_->photo();
    if (!p) return;
    switch (drag) {
        case Drag::Point:
            window_->edited();
            break;
        case Drag::Stroke: {
            double length = 0.0;
            for (std::size_t i = 1; i < path_.size(); ++i)
                length += std::hypot(path_[i][0] - path_[i - 1][0], path_[i][1] - path_[i - 1][1]);
            if (path_.size() >= 2 && length >= p->width / 100.0) window_->addFlowStroke(path_);
            break;
        }
        case Drag::Erase:
            window_->eraseFlow(path_, static_cast<float>(std::max(p->width / 200.0, 10.0 / scale_)));
            break;
        case Drag::Mask:
            window_->addMaskStroke(path_, radius_, include_);
            break;
        default:
            break;
    }
    path_.clear();
    update();
}

void PhotoView::wheelEvent(QWheelEvent* event) {
    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0.0) return;
    const QPointF at = event->position();
    const QPointF uv = toPhoto(at);
    scale_ = std::clamp(scale_ * std::pow(1.15, steps), 0.02, 64.0);
    offset_ = at - uv * scale_;
    userMoved_ = true;
    update();
}

void PhotoView::leaveEvent(QEvent*) {
    hoverPos_.reset();
    if (window_->hover() && window_->hover()->first == kLandmarkPhoto) window_->setHover(std::nullopt);
    update();
}

void PhotoView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_F && event->modifiers() == Qt::NoModifier) {
        fit();
        return;
    }
    QWidget::keyPressEvent(event);
}

// ---- ReferenceWindow ----------------------------------------------------------------------------

ReferenceWindow::ReferenceWindow(MainWindow* main, QWidget* parent) : QWidget(parent, Qt::Window), main_(main) {
    setWindowTitle(tr("Photos"));
    setObjectName(QStringLiteral("referenceWindow"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 10, 10, 10);
    outer->setSpacing(6);

    auto* top = new QHBoxLayout;
    auto* stages = new QButtonGroup(this);
    const std::pair<Stage, QString> stageLabels[] = {{Stage::Align, tr("Align")}, {Stage::Guides, tr("Guides (experimental)")}};
    const QString stageTips[] = {tr("Line the photos up with the mesh: landmark pairs, the camera"),
                                 tr("Guides from Photos: flow arrows and mask touch-ups on the lined-up photos, Generate")};
    for (int i = 0; i < 2; ++i) {
        auto* b = new QToolButton;
        b->setText(stageLabels[i].second);
        b->setCheckable(true);
        b->setToolTip(stageTips[i]);
        b->setObjectName(QStringLiteral("referenceStage%1").arg(i));
        stages->addButton(b);
        const Stage s = stageLabels[i].first;
        connect(b, &QToolButton::clicked, this, [this, s] { setStage(s); });
        top->addWidget(b);
        stageButtons_[i] = b;
    }
    top->addSpacing(12);
    tabs_ = new QTabBar;
    tabs_->setObjectName(QStringLiteral("referencePhotoTabs"));
    tabs_->setExpanding(false);
    tabs_->setDrawBase(false);
    tabs_->setUsesScrollButtons(true);
    connect(tabs_, &QTabBar::currentChanged, this, [this](int i) {
        if (i != current_) selectPhoto(i);
    });
    top->addWidget(tabs_, 1);
    auto* import = new QPushButton(tr("Import Photo..."));
    import->setToolTip(tr("A photo of the groom: its landmarks, arrows and camera are kept with the groom"));
    connect(import, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Import Photo"), QString(),
                                                          tr("Images (*.png *.jpg *.jpeg *.tif *.tiff *.tga *.bmp)"));
        if (!path.isEmpty()) importPhoto(path);
    });
    top->addWidget(import);
    removeButton_ = new QPushButton(tr("Remove"));
    removeButton_->setToolTip(tr("Forget this photo (its file stays on disk)"));
    connect(removeButton_, &QPushButton::clicked, this, [this] { removePhoto(current_); });
    top->addWidget(removeButton_);
    outer->addLayout(top);

    auto* body = new QHBoxLayout;
    body->setSpacing(6);
    auto* photoColumn = new QVBoxLayout;
    auto* tools = new QHBoxLayout;
    tools->setSpacing(4);
    auto* group = new QButtonGroup(this);
    const std::pair<Mode, QString> modes[] = {{Mode::Landmarks, tr("Landmarks")}, {Mode::Flow, tr("Flow")},
                                              {Mode::Hair, tr("Hair")}, {Mode::Backdrop, tr("Backdrop")}};
    const QString tips[] = {tr("Click a feature on the photo, then the same feature on the mesh (either order). Drag a point "
                               "to adjust it; Ctrl+click deletes it and its partner."),
                            tr("Drag along the hair from root to tip: a flow arrow the guides follow. Ctrl+drag erases."),
                            tr("Paint where the photo shows hair the mask missed (Ctrl: backdrop)"),
                            tr("Paint where the mask took backdrop for hair (Ctrl: hair)")};
    for (int i = 0; i < 4; ++i) {
        auto* b = new QToolButton;
        b->setText(modes[i].second);
        b->setCheckable(true);
        b->setToolTip(tips[i]);
        b->setObjectName(QStringLiteral("referenceMode%1").arg(i));
        group->addButton(b);
        const Mode m = modes[i].first;
        connect(b, &QToolButton::clicked, this, [this, m] { setMode(m); });
        tools->addWidget(b);
        modeButtons_[i] = b;
    }
    modeButtons_[0]->setChecked(true);
    tools->addStretch(1);
    auto* fit = new QToolButton;
    fit->setText(tr("Fit"));
    fit->setToolTip(tr("Fit the photo in its view (F). The wheel zooms, the middle button or Alt+drag pans."));
    tools->addWidget(fit);
    photoColumn->addLayout(tools);
    photoView_ = new PhotoView(this);
    connect(fit, &QToolButton::clicked, photoView_, [this] { photoView_->fit(); });
    photoColumn->addWidget(photoView_, 1);
    body->addLayout(photoColumn, 1);

    auto* meshColumn = new QVBoxLayout;
    auto* head = new QHBoxLayout;
    meshTitle_ = new QLabel;
    meshTitle_->setStyleSheet(QStringLiteral("color: %1; font-weight: bold;").arg(markerSideColor(kLandmarkMesh).name()));
    head->addWidget(meshTitle_, 1);
    auto* frame = new QPushButton(tr("Frame"));
    head->addWidget(frame);
    meshColumn->addLayout(head);
    meshView_ = new MarkerView(this, kLandmarkMesh);
    meshView_->setObjectName(QStringLiteral("referenceMeshView"));
    connect(frame, &QPushButton::clicked, meshView_, [this] { meshView_->frame(); });
    meshColumn->addWidget(meshView_, 1);
    body->addLayout(meshColumn, 1);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumWidth(250);
    scroll->setMaximumWidth(340);
    auto* panel = new QWidget;
    buildPanel(panel);
    scroll->setWidget(panel);
    body->addWidget(scroll);
    outer->addLayout(body, 1);

    hint_ = new QLabel;
    hint_->setWordWrap(true);
    outer->addWidget(hint_);
    auto* bottom = new QHBoxLayout;
    status_ = new QLabel;
    status_->setWordWrap(true);
    bottom->addWidget(status_, 1);
    undoButton_ = new QPushButton(tr("Undo"));
    connect(undoButton_, &QPushButton::clicked, this, [this] { undo(); });
    bottom->addWidget(undoButton_);
    redoButton_ = new QPushButton(tr("Redo"));
    connect(redoButton_, &QPushButton::clicked, this, [this] { redo(); });
    bottom->addWidget(redoButton_);
    auto* close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    bottom->addWidget(close);
    outer->addLayout(bottom);
    message_ = new QLabel;
    message_->setWordWrap(true);
    outer->addWidget(message_);

    const auto shortcut = [this](const QKeySequence& keys, bool redoIt) {
        auto* s = new QShortcut(keys, this);
        s->setContext(Qt::WindowShortcut);
        connect(s, &QShortcut::activated, this, [this, redoIt] { redoIt ? redo() : undo(); });
    };
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Z), false);
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Y), true);
    shortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), true);
    // The app's charcoal look, as the Transfer window's.
    setFont(theme::uiFont(13));
    setStyleSheet(theme::styleSheet() + QStringLiteral(
                      "QWidget#referenceWindow { background: %1; } QLabel, QCheckBox { color: %2; }"
                      "QPushButton, QToolButton { color: %2; background: %3; border: none; border-radius: 8px; padding: 5px 12px; }"
                      "QPushButton:hover, QToolButton:hover { background: %4; } QPushButton:disabled { color: %5; }"
                      "QToolButton:checked { background: %6; color: white; }"
                      "QPushButton:default { background: %6; color: white; }")
                      .arg(theme::glassSolid().name(QColor::HexRgb), theme::text().name(), theme::windowButton().name(),
                           theme::windowButtonHover().name(), theme::textMuted().name(), theme::blue().name()));
    setAttribute(Qt::WA_StyledBackground, true);
    resize(1240, 720);
    setStage(Stage::Align);
    reloadGroom();
}

ReferenceWindow::~ReferenceWindow() {
    meshView_->releaseGpu();
    delete meshView_;
    meshView_ = nullptr;
}

void ReferenceWindow::buildPanel(QWidget* panel) {
    auto* outer = new QVBoxLayout(panel);
    outer->setContentsMargins(0, 0, 4, 0);
    outer->setSpacing(6);
    QVBoxLayout* layout = outer;  // the sections go here (Guides' into their box)
    const auto heading = [&layout](const QString& text) {
        auto* label = new QLabel(text);
        label->setStyleSheet(QStringLiteral("font-weight: bold; letter-spacing: 1px;"));
        layout->addWidget(label);
    };
    heading(tr("LANDMARKS"));
    solveLabel_ = new QLabel;
    solveLabel_->setWordWrap(true);
    layout->addWidget(solveLabel_);
    pairs_ = new QTreeWidget;
    pairs_->setHeaderLabels({tr("#"), tr("Photo"), tr("Mesh"), tr("Error")});
    pairs_->setRootIsDecorated(false);
    pairs_->setMinimumHeight(90);
    pairs_->setMaximumHeight(170);
    pairs_->header()->setStretchLastSection(false);
    for (int c : {0, 1, 3}) pairs_->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    pairs_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    layout->addWidget(pairs_);
    auto* row = new QHBoxLayout;
    row->addStretch(1);
    auto* clear = new QPushButton(tr("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] { clearLandmarks(); });
    row->addWidget(clear);
    layout->addLayout(row);
    auto* form = new QFormLayout;
    overlayBox_ = new QComboBox;
    overlayBox_->addItems({tr("Outline"), tr("Wireframe"), tr("Off")});
    overlayBox_->setToolTip(tr("The mesh drawn over the photo through the solved camera"));
    connect(overlayBox_, &QComboBox::currentIndexChanged, this, [this] {
        overlayKey_ = ~0ull;
        photoView_->update();
    });
    form->addRow(tr("Mesh over photo"), overlayBox_);
    opacity_ = new ValueSlider;
    opacity_->setObjectName(QStringLiteral("referenceOverlayOpacity"));
    opacity_->setCaption(tr("Opacity"));
    opacity_->setRange(0.0, 1.0);
    opacity_->setLimits(0.0, 1.0);
    opacity_->setDecimals(2);
    opacity_->setStep(0.05);
    opacity_->setValue(0.7);
    opacity_->setToolTip(tr("How opaque the mesh over the photo is"));
    connect(opacity_, &ValueSlider::valueChanged, photoView_, [this] { photoView_->update(); });
    form->addRow(opacity_);
    layout->addLayout(form);

    heading(tr("CAMERA"));
    form = new QFormLayout;
    focal_ = new ValueSlider;
    focal_->setObjectName(QStringLiteral("referenceFocal"));
    focal_->setCaption(tr("Focal Length"));
    focal_->setRange(10.0, 200.0);
    focal_->setLimits(2.5, 3500.0);
    focal_->setDecimals(1);
    focal_->setStep(1.0);
    focal_->setValue(50.0);
    focal_->setFill(theme::blue());
    focal_->setToolTip(tr("The photo camera's focal length (mm): the landmarks solve it (6 or more pairs); set it by hand when you "
                          "know the lens (Keep focal length turns on, the camera follows)"));
    connect(focal_, &ValueSlider::valueChanged, this, [this](double mm) {
        if (!updatingCamera_) setFocal(mm);
    });
    focal_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(focal_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        auto* menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->addAction(tr("Reset Focal Length (50 mm)"), this, [this] { resetFocal(); });
        menu->popup(focal_->mapToGlobal(pos));
    });
    form->addRow(focal_);
    layout->addLayout(form);
    row = new QHBoxLayout;
    keepFocal_ = new QCheckBox(tr("Keep focal length"));
    keepFocal_->setObjectName(QStringLiteral("referenceKeepFocal"));
    keepFocal_->setToolTip(tr("Solve the position and rotation only, keeping the focal length (4 pairs are enough; else 6, and the "
                              "focal length is solved too)"));
    connect(keepFocal_, &QCheckBox::toggled, this, [this](bool on) {
        if (!updatingCamera_) setKeepFocal(on);
    });
    row->addWidget(keepFocal_);
    row->addStretch(1);
    lock_ = new QToolButton;
    lock_->setText(tr("Lock"));
    lock_->setObjectName(QStringLiteral("referenceLock"));
    lock_->setCheckable(true);
    lock_->setToolTip(tr("Lock the camera where it is (its position, rotation and focal length): the landmarks no longer move it, "
                         "nor does tumbling through it in the viewport"));
    connect(lock_, &QToolButton::toggled, this, [this](bool on) {
        if (!updatingCamera_) setLocked(on);
    });
    row->addWidget(lock_);
    layout->addLayout(row);

    // Guides from Photos (the Guides stage only).
    guidesPanel_ = new QWidget;
    guidesPanel_->setObjectName(QStringLiteral("referenceGuidesPanel"));
    layout = new QVBoxLayout(guidesPanel_);
    layout->setContentsMargins(0, 6, 0, 0);
    layout->setSpacing(6);
    outer->addWidget(guidesPanel_);

    heading(tr("FLOW AND MASK"));
    flowLabel_ = new QLabel;
    flowLabel_->setWordWrap(true);
    layout->addWidget(flowLabel_);
    row = new QHBoxLayout;
    auto* clearArrows = new QPushButton(tr("Clear Arrows"));
    connect(clearArrows, &QPushButton::clicked, this, [this] { clearFlow(); });
    row->addWidget(clearArrows);
    auto* clearMaskButton = new QPushButton(tr("Clear Touch-ups"));
    connect(clearMaskButton, &QPushButton::clicked, this, [this] { clearMask(); });
    row->addWidget(clearMaskButton);
    row->addStretch(1);
    layout->addLayout(row);
    row = new QHBoxLayout;
    maskTint_ = new QCheckBox(tr("Mask tint"));
    maskTint_->setToolTip(tr("Tint the hair mask the guides are grown in (automatic and refined, with the touch-ups)"));
    connect(maskTint_, &QCheckBox::toggled, photoView_, [this] { photoView_->update(); });
    row->addWidget(maskTint_);
    fieldLines_ = new QCheckBox(tr("Flow field"));
    fieldLines_->setToolTip(tr("The strand directions read from the photo (with the arrows), as short lines"));
    connect(fieldLines_, &QCheckBox::toggled, photoView_, [this] { photoView_->update(); });
    row->addWidget(fieldLines_);
    row->addStretch(1);
    layout->addLayout(row);
    form = new QFormLayout;
    brush_ = new ValueSlider;
    brush_->setObjectName(QStringLiteral("referenceBrush"));
    brush_->setCaption(tr("Brush"));
    brush_->setRange(2.0, 120.0);
    brush_->setLimits(1.0, 1000.0);
    brush_->setInteger(true);
    brush_->setValue(14.0);
    brush_->setFormat([](double v) { return QString::number(std::lround(v)) + QStringLiteral(" px"); });
    brush_->setToolTip(tr("The touch-up brush's radius on the photo (pixels)"));
    form->addRow(brush_);
    layout->addLayout(form);

    heading(tr("GENERATE"));
    form = new QFormLayout;
    symmetry_ = new QComboBox;
    for (const auto& [label, key] : {std::pair<QString, QString>{tr("Auto"), "auto"}, {tr("None"), "none"},
                                     {tr("World X"), "x"}, {tr("Topology"), "topology"}})
        symmetry_->addItem(label, key);
    form->addRow(tr("Symmetry"), symmetry_);
    guideCount_ = new QSpinBox;
    guideCount_->setRange(10, 20000);
    guideCount_->setValue(300);
    form->addRow(tr("Guide count"), guideCount_);
    maxLength_ = new QDoubleSpinBox;
    maxLength_->setRange(0.0, 100000.0);
    maxLength_->setDecimals(2);
    maxLength_->setSpecialValueText(tr("Auto"));
    connect(maxLength_, &QDoubleSpinBox::valueChanged, this, [this] { previewKey_ = ~0ull; });
    form->addRow(tr("Max length"), maxLength_);
    layout->addLayout(form);
    generateButton_ = new QPushButton(tr("Generate"));
    generateButton_->setDefault(true);
    generateButton_->setToolTip(tr("Grow guides from the lined-up photos into the fromReference group (again: replaced)"));
    connect(generateButton_, &QPushButton::clicked, this, [this] { generate(); });
    layout->addWidget(generateButton_);
    outer->addStretch(1);
}

QRhi* ReferenceWindow::rhi() {
    if (!rhiTried_) {
        rhiTried_ = true;
        rhi_ = createOffscreenRhi();
    }
    return rhi_.get();
}

QString ReferenceWindow::emptyViewText(MarkerSide) const { return tr("The groom has no scalp"); }

ReferencePhoto* ReferenceWindow::photo() {
    return current_ >= 0 && current_ < photoCount() ? photos_[static_cast<std::size_t>(current_)].get() : nullptr;
}

const ReferencePhoto* ReferenceWindow::photo() const {
    return current_ >= 0 && current_ < photoCount() ? photos_[static_cast<std::size_t>(current_)].get() : nullptr;
}

const MarkerLists& ReferenceWindow::markerLists() const { return photo() ? photo()->editor.landmarks() : empty_; }

void ReferenceWindow::reloadGroom() {
    GroomDocument* doc = main_ ? main_->document() : nullptr;
    // The scalp smoothed as the groom grows on it (Scalp Subdivision): shown and picked on.
    meshView_->setMesh(doc ? doc->file().data.scalp : nullptr, doc ? doc->refinedFileScalp() : nullptr);
    meshTitle_->setText(doc ? tr("Mesh: the groom's scalp") : tr("Mesh: (no groom open)"));
    loadPhotos();
}

void ReferenceWindow::refreshScalp() {
    GroomDocument* doc = main_ ? main_->document() : nullptr;
    if (!doc) return;
    const MeshPtr shown = doc->refinedFileScalp();
    if (meshView_->mesh() == doc->file().data.scalp && meshView_->shownMesh() == shown) return;
    meshView_->setMesh(doc->file().data.scalp, shown, false);
}

void ReferenceWindow::photosChanged() {
    const QString name = photo() ? photo()->name : QString();
    loadPhotos();
    for (int i = 0; i < photoCount(); ++i) {
        if (photos_[static_cast<std::size_t>(i)]->name == name) selectPhoto(i);
    }
}

void ReferenceWindow::loadPhotos() {
    photos_.clear();
    current_ = -1;
    hover_.reset();
    GroomDocument* doc = main_ ? main_->document() : nullptr;
    if (doc && !doc->referencePhotos().empty()) {
        const json all = json::parse(doc->referencePhotos(), nullptr, false);
        if (all.is_array())
            for (const json& j : all) {
                auto p = std::make_unique<ReferencePhoto>();
                p->name = QString::fromStdString(j.value("name", std::string()));
                p->image = QString::fromStdString(j.value("image", std::string()));
                p->width = j.value("width", 0);
                p->height = j.value("height", 0);
                p->mask = j.contains("mask") && j["mask"].is_string() ? QString::fromStdString(j["mask"].get<std::string>()) : QString();
                try {
                    p->editor.reset(referencePhotoStateFromJson(j.contains("state") ? j["state"].dump() : std::string()));
                    p->camera = cameraFrom(j.value("camera", json()), p->width, p->height);
                } catch (const std::exception&) {
                }
                p->picture = QImage(p->image);
                if (j.contains("display") && j["display"].is_object()) p->display = j["display"].dump();
                photos_.push_back(std::move(p));
            }
    }
    refreshTabs();
    selectPhoto(photos_.empty() ? -1 : 0);
}

void ReferenceWindow::store() {
    GroomDocument* doc = main_ ? main_->document() : nullptr;
    if (!doc) return;
    json all = json::array();
    for (const auto& p : photos_) {
        json j = {{"name", p->name.toStdString()}, {"image", p->image.toStdString()}, {"width", p->width}, {"height", p->height}};
        j["mask"] = p->mask.isEmpty() ? json() : json(p->mask.toStdString());
        j["camera"] = p->camera ? cameraJson(*p->camera) : json();
        j["state"] = json::parse(referencePhotoStateToJson(p->editor.state()));
        if (!p->display.empty()) j["display"] = json::parse(p->display, nullptr, false);
        all.push_back(std::move(j));
    }
    doc->setReferencePhotos(all.empty() ? std::string() : all.dump());
    emit photosStored();
}

void ReferenceWindow::refreshTabs() {
    const QSignalBlocker block(tabs_);
    while (tabs_->count()) tabs_->removeTab(0);
    for (const auto& p : photos_) tabs_->addTab(p->name);
    if (current_ >= 0) tabs_->setCurrentIndex(current_);
}

int ReferenceWindow::importPhoto(const QString& path) {
    if (!main_ || !main_->document()) {
        message_->setText(tr("Open a groom first"));
        return -1;
    }
    QImage picture(path);
    if (picture.isNull()) {
        message_->setText(tr("Cannot read the image %1").arg(path));
        return -1;
    }
    auto p = std::make_unique<ReferencePhoto>();
    p->name = QFileInfo(path).completeBaseName();
    p->image = QFileInfo(path).absoluteFilePath();
    p->width = picture.width();
    p->height = picture.height();
    p->picture = std::move(picture);
    photos_.push_back(std::move(p));
    store();
    refreshTabs();
    selectPhoto(photoCount() - 1);
    message_->setText(tr("%1 imported: click a feature on the photo, then the same feature on the mesh (4 pairs line the "
                         "camera up).").arg(photos_.back()->name));
    return photoCount() - 1;
}

bool ReferenceWindow::removePhoto(int index) {
    if (index < 0 || index >= photoCount()) return false;
    photos_.erase(photos_.begin() + index);
    store();
    current_ = -1;
    refreshTabs();
    selectPhoto(photos_.empty() ? -1 : std::min(index, photoCount() - 1));
    return true;
}

void ReferenceWindow::selectPhoto(int index) {
    current_ = index >= 0 && index < photoCount() ? index : -1;
    if (current_ >= 0 && tabs_->currentIndex() != current_) {
        const QSignalBlocker block(tabs_);
        tabs_->setCurrentIndex(current_);
    }
    hover_.reset();
    overlayKey_ = arrowsKey_ = previewKey_ = ~0ull;
    if (ReferencePhoto* p = photo(); p && !p->solve.ok && p->editor.landmarks().pairCount() >= kReferenceMinPairs) solve();
    removeButton_->setEnabled(current_ >= 0);
    photoView_->photoChanged();
    meshView_->update();
    refreshPanel();
}

double ReferenceWindow::focalGuess(const ReferencePhoto& p) const {
    return p.camera && p.camera->focalPx > 0.0 ? p.camera->focalPx : p.width * 50.0 / 36.0;
}

void ReferenceWindow::solve(bool findBad) {
    ReferencePhoto* p = photo();
    if (!p) return;
    p->solve = solveReferenceLandmarks(p->editor.landmarks(), p->width, p->height, focalGuess(*p), displayFlag(*p, "keepFocal"),
                                       p->camera ? &p->camera->pose : nullptr, findBad);
    if (p->solve.ok && displayFlag(*p, "locked")) {
        message_->setText(tr("%1 is locked: unlock it (Lock) for the landmarks to move it").arg(p->name));
    } else if (p->solve.ok) {
        p->camera = p->solve.camera;
    }
    ++solveCount_;
}

void ReferenceWindow::edited(bool storeIt) {
    solve();
    if (storeIt) store();
    photoView_->update();
    meshView_->update();
    refreshPanel();
}

void ReferenceWindow::setStage(Stage stage) {
    stage_ = stage;
    const bool guides = stage == Stage::Guides;
    stageButtons_[guides ? 1 : 0]->setChecked(true);
    for (QToolButton* b : modeButtons_) b->setVisible(guides);
    guidesPanel_->setVisible(guides);
    if (!guides && mode_ != Mode::Landmarks) {
        setMode(Mode::Landmarks);
        return;
    }
    refreshPanel();
}

void ReferenceWindow::setMode(Mode mode) {
    if (mode != Mode::Landmarks && stage_ != Stage::Guides) setStage(Stage::Guides);
    mode_ = mode;
    modeButtons_[static_cast<int>(mode)]->setChecked(true);
    hover_.reset();
    photoView_->update();
    meshView_->update();
    refreshPanel();
}

std::size_t ReferenceWindow::addLandmark(MarkerSide side, const Vec3f& point) {
    ReferencePhoto* p = photo();
    if (!p) return 0;
    MarkerPoint m = side == kLandmarkPhoto ? ReferencePhotoEditor::photoPoint(point.x, point.y) : MarkerPoint{StrandRoot{}, point, 0};
    if (side == kLandmarkMesh && meshView_->mesh()) snapMarkerPoint(*meshView_->mesh(), point, &m);
    const std::size_t index = p->editor.addLandmark(side, m);
    edited();
    return index;
}

std::size_t ReferenceWindow::addPicked(MarkerSide side, const MarkerPoint& picked) {
    ReferencePhoto* p = photo();
    if (!p) return 0;
    const std::size_t index = p->editor.addLandmark(side, picked);
    edited();
    return index;
}

std::vector<std::size_t> ReferenceWindow::deleteLandmark(MarkerSide side, std::size_t index) {
    ReferencePhoto* p = photo();
    if (!p || index >= p->editor.landmarks().count(side)) return {};
    std::vector<std::size_t> gone = p->editor.removeLandmark(side, index);
    hover_.reset();
    edited();
    return gone;
}

void ReferenceWindow::moveLandmark(MarkerSide side, std::size_t index, const Vec3f& point, bool newStep) {
    ReferencePhoto* p = photo();
    if (!p || index >= p->editor.landmarks().count(side)) return;
    MarkerPoint m = side == kLandmarkPhoto ? ReferencePhotoEditor::photoPoint(point.x, point.y) : MarkerPoint{StrandRoot{}, point, 0};
    if (side == kLandmarkMesh && meshView_->mesh()) snapMarkerPoint(*meshView_->mesh(), point, &m);
    movePicked(side, index, m, newStep);
}

void ReferenceWindow::movePicked(MarkerSide side, std::size_t index, const MarkerPoint& picked, bool newStep) {
    ReferencePhoto* p = photo();
    if (!p) return;
    p->editor.moveLandmark(side, index, picked, newStep);
    solve(false);  // live; the bad pairs and the storing when the drag ends (edited)
    photoView_->update();
    meshView_->update();
    refreshPanel();
}

void ReferenceWindow::clearLandmarks() {
    if (ReferencePhoto* p = photo()) {
        p->editor.clearLandmarks();
        edited();
    }
}

std::size_t ReferenceWindow::addFlowStroke(const Pixels& points, bool directed) {
    ReferencePhoto* p = photo();
    if (!p || points.size() < 2) return 0;
    const std::size_t index = p->editor.addStroke(points, directed);
    edited();
    return index;
}

std::size_t ReferenceWindow::eraseFlow(const Pixels& path, float radius) {
    ReferencePhoto* p = photo();
    if (!p) return 0;
    const std::size_t gone = p->editor.eraseStrokes(path, radius > 0.0f ? radius : p->width / 50.0f);
    if (gone) edited();
    return gone;
}

void ReferenceWindow::clearFlow() {
    if (ReferencePhoto* p = photo()) {
        p->editor.clearStrokes();
        edited();
    }
}

std::size_t ReferenceWindow::addMaskStroke(const Pixels& points, float radius, bool include) {
    ReferencePhoto* p = photo();
    if (!p || points.empty()) return 0;
    const std::size_t index = p->editor.addMaskStroke(points, radius, include);
    edited();
    return index;
}

void ReferenceWindow::clearMask() {
    if (ReferencePhoto* p = photo()) {
        p->editor.clearMaskStrokes();
        edited();
    }
}

void ReferenceWindow::setDisplayFlag(const char* key, bool on) {
    ReferencePhoto* p = photo();
    if (!p) return;
    json j = json::parse(p->display.empty() ? std::string("{}") : p->display, nullptr, false);
    if (!j.is_object()) j = json::object();
    j[key] = on;
    p->display = j.dump();
}

void ReferenceWindow::setKeepFocal(bool on) {
    if (!photo()) return;
    setDisplayFlag("keepFocal", on);
    edited();
}

bool ReferenceWindow::keepFocal() const { return photo() && displayFlag(*photo(), "keepFocal"); }

bool ReferenceWindow::setFocal(double mm) {
    ReferencePhoto* p = photo();
    if (!p || p->width <= 0) return false;
    if (displayFlag(*p, "locked")) {
        message_->setText(tr("%1 is locked: unlock it to change its focal length").arg(p->name));
        refreshPanel();
        return false;
    }
    if (!p->camera) p->camera = placedReferenceCamera(main_ ? main_->document() : nullptr, p->width, p->height);
    p->camera->focalPx = std::clamp(mm, 2.5, 3500.0) * p->width / 36.0;
    setDisplayFlag("keepFocal", true);  // (the solve keeps the focal length set by hand)
    edited();
    return true;
}

bool ReferenceWindow::resetFocal() {
    focal_->setRange(10.0, 200.0);
    return setFocal(50.0);
}

void ReferenceWindow::setLocked(bool on) {
    if (!photo()) return;
    setDisplayFlag("locked", on);
    if (on) {
        store();
        refreshPanel();
    } else {
        edited();  // the landmarks line it up again
    }
}

bool ReferenceWindow::locked() const { return photo() && displayFlag(*photo(), "locked"); }

bool ReferenceWindow::undo() {
    ReferencePhoto* p = photo();
    if (!p || !p->editor.undo()) return false;
    hover_.reset();
    edited();
    return true;
}

bool ReferenceWindow::redo() {
    ReferencePhoto* p = photo();
    if (!p || !p->editor.redo()) return false;
    hover_.reset();
    edited();
    return true;
}

int ReferenceWindow::photoPointNear(const QPointF& uv, double pixels) const {
    const ReferencePhoto* p = photo();
    if (!p) return -1;
    int best = -1;
    double bestD = pixels * pixels;
    const auto& points = p->editor.landmarks().points(kLandmarkPhoto);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const double d = std::pow(points[i].point.x - uv.x(), 2.0) + std::pow(points[i].point.y - uv.y(), 2.0);
        if (d <= bestD) {
            bestD = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void ReferenceWindow::setHover(MarkerHover hover) {
    if (hover == hover_) return;
    hover_ = hover;
    photoView_->update();
    meshView_->update();
}

void ReferenceWindow::setOverlay(Overlay overlay) { overlayBox_->setCurrentIndex(static_cast<int>(overlay)); }
ReferenceWindow::Overlay ReferenceWindow::overlay() const { return static_cast<Overlay>(overlayBox_->currentIndex()); }
double ReferenceWindow::overlayOpacity() const { return opacity_->value(); }
void ReferenceWindow::setMaskTint(bool on) { maskTint_->setChecked(on); }
void ReferenceWindow::setFieldLines(bool on) { fieldLines_->setChecked(on); }
bool ReferenceWindow::maskTint() const { return maskTint_->isChecked(); }
bool ReferenceWindow::fieldLines() const { return fieldLines_->isChecked(); }
double ReferenceWindow::brushSize() const { return brush_->value(); }

const std::vector<std::array<float, 4>>& ReferenceWindow::overlayLines() {
    const ReferencePhoto* p = photo();
    const std::uint64_t key = (solveCount_ * 4 + static_cast<std::uint64_t>(overlay())) * 1024 + static_cast<std::uint64_t>(current_ + 1);
    if (key != overlayKey_) {
        overlayKey_ = key;
        overlayLines_.clear();
        if (p && p->camera && overlay() != Overlay::Off && meshView_->mesh())
            overlayLines_ = referenceMeshOverlay(*meshView_->mesh(), *p->camera, overlay() == Overlay::Outline);
    }
    return overlayLines_;
}

const std::vector<ReferenceArrow>& ReferenceWindow::arrows() {
    const ReferencePhoto* p = photo();
    const std::uint64_t key = p ? p->editor.revision() * 1024 + static_cast<std::uint64_t>(current_ + 1) : 0;
    if (key != arrowsKey_) {
        arrowsKey_ = key;
        arrows_.clear();
        if (p)
            for (const RefHintStroke& s : p->editor.strokes()) arrows_.push_back(referenceArrow(s.points, s.directed ? p->width / 40.0f : 0.0f));
    }
    return arrows_;
}

const ReferenceViewPreview* ReferenceWindow::preview() {
    ReferencePhoto* p = photo();
    if (!p || !p->camera || !meshView_->mesh() || !(maskTint() || fieldLines())) return nullptr;
    const std::uint64_t key = (p->editor.revision() * 4096 + solveCount_) * 1024 + static_cast<std::uint64_t>(current_ + 1) +
                              static_cast<std::uint64_t>(maxLength_->value() * 1000.0) * 7919;
    if (key != previewKey_) {
        previewKey_ = key;
        preview_.reset();
        previewTint_ = QImage();
        try {
            ReferenceView view;
            view.camera = *p->camera;
            view.image = loadRefImage(p->image.toStdString(), 3);
            if (!p->mask.isEmpty()) view.mask = loadRefImage(p->mask.toStdString(), 1);
            view.hints = p->editor.strokes();
            view.maskStrokes = p->editor.maskStrokes();
            ReferenceGuideOptions options;
            options.maxLength = static_cast<float>(maxLength_->value());
            preview_ = previewReferenceView(*meshView_->mesh(), view, options);
            QImage alpha(preview_->mask.width, preview_->mask.height, QImage::Format_Alpha8);
            for (int y = 0; y < alpha.height(); ++y) {
                uchar* line = alpha.scanLine(y);
                for (int x = 0; x < alpha.width(); ++x)
                    line[x] = static_cast<uchar>(std::lround(255.0f * std::clamp(preview_->mask.at(x, y), 0.0f, 1.0f)));
            }
            previewTint_ = QImage(alpha.size(), QImage::Format_ARGB32_Premultiplied);
            previewTint_.fill(kHairTint);
            QPainter painter(&previewTint_);
            painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            painter.drawImage(0, 0, alpha);
        } catch (const std::exception& e) {
            message_->setText(tr("Mask preview: %1").arg(QString::fromUtf8(e.what())));
        }
    }
    return preview_ ? &*preview_ : nullptr;
}

QImage ReferenceWindow::previewTint() { return previewTint_; }

QString ReferenceWindow::requirement() const {
    GroomDocument* doc = main_ ? main_->document() : nullptr;
    if (!doc) return tr("No groom open");
    if (!doc->file().data.scalp) return tr("The groom has no scalp");
    for (const auto& p : photos_)
        if (p->camera) return {};
    return photos_.empty() ? tr("Import a photo") : tr("Line a photo up first: 4 landmark pairs");
}

void ReferenceWindow::setGuideCount(int count) { guideCount_->setValue(count); }
void ReferenceWindow::setMaxLength(double length) { maxLength_->setValue(length); }
void ReferenceWindow::setSymmetry(const QString& name) { symmetry_->setCurrentIndex(std::max(0, symmetry_->findData(name))); }

bool ReferenceWindow::generate() {
    const QString why = requirement();
    if (!why.isEmpty()) {
        message_->setText(why);
        return false;
    }
    GroomDocument* doc = main_->document();
    ReferenceGuidesResult result;
    try {
        std::vector<ReferenceView> views;
        for (const auto& p : photos_) {
            if (!p->camera) continue;
            ReferenceView view;
            view.camera = *p->camera;
            view.image = loadRefImage(p->image.toStdString(), 3);
            if (!p->mask.isEmpty()) view.mask = loadRefImage(p->mask.toStdString(), 1);
            view.hints = p->editor.strokes();
            view.maskStrokes = p->editor.maskStrokes();
            views.push_back(std::move(view));
        }
        ReferenceGuideOptions options;
        options.guideCount = guideCount_->value();
        options.maxLength = static_cast<float>(maxLength_->value());
        options.symmetry = referenceSymmetryFromName(symmetry_->currentData().toString().toStdString());
        // On the smoothed scalp the hair grows on (Scalp Subdivision), as the brushes place guides.
        result = guidesFromReference(*doc->refinedFileScalp(), views, options);
    } catch (const std::exception& e) {
        message_->setText(tr("Cannot grow the guides: %1").arg(QString::fromUtf8(e.what())));
        return false;
    }
    const std::string error = doc->editGuides("guides from reference", [&](GuideHierarchy& h) {
        const GroupId old = h.findGroup(kGroup);
        if (old != kNoGroup) h.removeGroup(old);
        const GroupId group = h.addGroup(kGroup);
        for (const ReferenceGuide& g : result.guides)
            if (g.points.size() >= 2) h.addGuide(group, g.points);
    });
    if (!error.empty()) {
        message_->setText(tr("Cannot add the guides: %1").arg(QString::fromStdString(error)));
        return false;
    }
    message_->setText(tr("%1 guides in fromReference (symmetry: %2; %3 roots had no hair over them, %4 were not seen).")
                          .arg(result.guides.size())
                          .arg(QString::fromLatin1(referenceSymmetryName(result.symmetry)))
                          .arg(result.rootsBald)
                          .arg(result.rootsUnseen));
    emit guidesChanged();
    refreshPanel();
    return true;
}

QString ReferenceWindow::statusText() const { return status_->text(); }
QString ReferenceWindow::messageText() const { return message_->text(); }
QString ReferenceWindow::solveText() const { return solveLabel_->text(); }

void ReferenceWindow::refreshPanel() {
    const ReferencePhoto* p = photo();
    updatingCamera_ = true;
    keepFocal_->setChecked(p && displayFlag(*p, "keepFocal"));
    lock_->setChecked(p && displayFlag(*p, "locked"));
    keepFocal_->setEnabled(p != nullptr);
    lock_->setEnabled(p != nullptr);
    focal_->setEnabled(p && !displayFlag(*p, "locked"));
    if (p && p->width > 0 && !focal_->editing()) focal_->setValue(focalGuess(*p) * 36.0 / p->width);
    updatingCamera_ = false;
    pairs_->clear();
    if (p) {
        const MarkerLists& lists = p->editor.landmarks();
        const auto& photoPts = lists.points(kLandmarkPhoto);
        const auto& meshPts = lists.points(kLandmarkMesh);
        for (std::size_t i = 0; i < std::max(photoPts.size(), meshPts.size()); ++i) {
            const bool bad = p->solve.ok && i < p->solve.bad.size() && p->solve.bad[i];
            auto* item = new QTreeWidgetItem(
                {QString::number(i + 1),
                 i < photoPts.size() ? QStringLiteral("%1, %2").arg(photoPts[i].point.x, 0, 'f', 0).arg(photoPts[i].point.y, 0, 'f', 0)
                                     : QStringLiteral("-"),
                 i < meshPts.size() ? QStringLiteral("%1, %2, %3")
                                          .arg(meshPts[i].point.x, 0, 'f', 1)
                                          .arg(meshPts[i].point.y, 0, 'f', 1)
                                          .arg(meshPts[i].point.z, 0, 'f', 1)
                                    : QStringLiteral("-"),
                 p->solve.ok && i < p->solve.errors.size() ? QStringLiteral("%1 px").arg(p->solve.errors[i], 0, 'f', 1) : QString()});
            if (bad)
                for (int c = 0; c < 4; ++c) item->setForeground(c, kBad);
            pairs_->addTopLevelItem(item);
        }
        const QString summary = QString::fromStdString(lists.summary("No landmarks"));
        if (p->solve.ok) {
            const long bad = std::count(p->solve.bad.begin(), p->solve.bad.end(), true);
            solveLabel_->setText(tr("%1; camera lined up: %2 px error, %3 mm%4.%5")
                                     .arg(summary)
                                     .arg(p->solve.rms, 0, 'f', 2)
                                     .arg(p->solve.camera.focalPx * 36.0 / std::max(1, p->width), 0, 'f', 0)
                                     .arg(p->solve.focalSolved ? tr(" (solved)") : tr(" (kept)"))
                                     .arg(bad ? tr(" %n pair(s) in red to fix first.", nullptr, static_cast<int>(bad)) : QString()));
        } else {
            solveLabel_->setText(summary + QStringLiteral(". ") + QString::fromStdString(p->solve.why));
        }
        flowLabel_->setText(tr("%n flow arrow(s), ", nullptr, static_cast<int>(p->editor.strokes().size())) +
                            tr("%n mask touch-up(s).", nullptr, static_cast<int>(p->editor.maskStrokes().size())));
        undoButton_->setEnabled(p->editor.canUndo());
        undoButton_->setToolTip(p->editor.canUndo() ? tr("Undo %1 (Ctrl+Z)").arg(QString::fromStdString(p->editor.undoText())) : tr("Undo (Ctrl+Z)"));
        redoButton_->setEnabled(p->editor.canRedo());
    } else {
        solveLabel_->setText(tr("No photo yet: Import Photo... above."));
        flowLabel_->clear();
        undoButton_->setEnabled(false);
        redoButton_->setEnabled(false);
    }
    const QString why = requirement();
    generateButton_->setEnabled(why.isEmpty());
    generateButton_->setToolTip(why.isEmpty() ? tr("Grow guides from the lined-up photos into the fromReference group") : why);
    static const char* const hints[] = {
        QT_TR_NOOP("Landmarks: click a feature on the photo, then the same feature on the mesh (either order); drag to adjust, "
                   "Ctrl+click deletes a pair. 4 pairs line the camera up."),
        QT_TR_NOOP("Flow: drag along the hair from root to tip; Ctrl+drag erases."),
        QT_TR_NOOP("Hair: paint where the mask missed hair (Ctrl: backdrop). Mask tint shows the mask."),
        QT_TR_NOOP("Backdrop: paint where the mask took backdrop for hair (Ctrl: hair).")};
    hint_->setText(tr(hints[static_cast<int>(mode_)]) + tr("  Wheel zooms, middle drag pans, Ctrl+Z / Ctrl+Y undo and redo."));
    status_->setText(tr("%n photo(s)", nullptr, photoCount()) +
                     (why.isEmpty() || stage_ != Stage::Guides ? QString() : QStringLiteral("  —  ") + why));
}

}  // namespace groomlab::app
