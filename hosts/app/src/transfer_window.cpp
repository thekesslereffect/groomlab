// SPDX-License-Identifier: Apache-2.0
#include "transfer_window.h"

#include <algorithm>
#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "groom_document.h"
#include "main_window.h"
#include "mesh_io.h"
#include "offscreen.h"
#include "renderer.h"
#include "theme.h"
#include "tool_view.h"

namespace groomlab::app {

namespace {

QColor sideColor(MarkerSide side) { return markerSideColor(side); }

Bounds boundsOf(const ScalpMesh& mesh) {
    Bounds b;
    for (const Vec3f& p : mesh.points()) {
        if (!b.valid) {
            b.lo = b.hi = p;
            b.valid = true;
            continue;
        }
        b.lo = {std::min(b.lo.x, p.x), std::min(b.lo.y, p.y), std::min(b.lo.z, p.z)};
        b.hi = {std::max(b.hi.x, p.x), std::max(b.hi.y, p.y), std::max(b.hi.z, p.z)};
    }
    return b;
}

}  // namespace

// ---- TransferWindow -----------------------------------------------------------------------------

TransferWindow::TransferWindow(MainWindow* main, QWidget* parent) : QWidget(parent, Qt::Window), main_(main) {
    setWindowTitle(tr("Transfer Groom to Scalp"));
    setObjectName(QStringLiteral("transferWindow"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 10, 10, 10);
    outer->setSpacing(6);

    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Move the groom onto another mesh: guides, parts, flow and painted maps")), 1);
    auto* browse = new QPushButton(tr("New Scalp..."));
    browse->setToolTip(tr("The mesh the groom moves onto (.obj, .abc), shown in the right view"));
    connect(browse, &QPushButton::clicked, this, [this] { chooseNewScalp(); });
    top->addWidget(browse);
    outer->addLayout(top);

    auto* views = new QHBoxLayout;
    for (MarkerSide side : {MarkerSide::Old, MarkerSide::New}) {
        auto* column = new QVBoxLayout;
        auto* head = new QHBoxLayout;
        auto* title = new QLabel;
        title->setStyleSheet(QStringLiteral("color: %1; font-weight: bold;").arg(sideColor(side).name()));
        head->addWidget(title, 1);
        auto* frame = new QPushButton(tr("Frame"));
        frame->setToolTip(tr("Fit the mesh in this view"));
        head->addWidget(frame);
        column->addLayout(head);
        auto* view = new MarkerView(this, side);
        column->addWidget(view, 1);
        connect(frame, &QPushButton::clicked, view, [view] { view->frame(); });
        views->addLayout(column, 1);
        (side == MarkerSide::Old ? left_ : right_) = view;
        (side == MarkerSide::Old ? leftTitle_ : rightTitle_) = title;
    }
    outer->addLayout(views, 1);

    auto* hint = new QLabel(tr("Click a point on each mesh: pair N is the Nth point on the left and the Nth on the right, "
                               "in any order. Drag a point to slide it, Ctrl+click to delete it and its partner, Alt to "
                               "move the camera, Ctrl+Z / Ctrl+Y to undo and redo."));
    hint->setWordWrap(true);
    outer->addWidget(hint);

    auto* options = new QHBoxLayout;
    options->addWidget(new QLabel(tr("Method")));
    methodBox_ = new QComboBox;
    const std::pair<TransferMethod, QString> methods[] = {
        {TransferMethod::Closest, tr("Closest point")},
        {TransferMethod::Bounds, tr("Fit bounding boxes")},
        {TransferMethod::MarkersAlign, tr("Markers: align (move, turn, scale)")},
        {TransferMethod::MarkersWarp, tr("Markers: warp (follows proportions)")},
        {TransferMethod::UV, tr("Matching UVs")}};
    for (const auto& [m, label] : methods) methodBox_->addItem(label, QString::fromLatin1(transferMethodName(m)));
    methodBox_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);  // narrow screens
    methodBox_->setMinimumContentsLength(16);
    methodBox_->setToolTip(tr("Where guides, parts, flow and maps land on the new mesh: its closest point (after fitting "
                              "the bounding boxes), marker pairs (align: 3+, warp: 4+ spread over the head), or the same "
                              "UV on a mesh laid out alike"));
    connect(methodBox_, &QComboBox::currentIndexChanged, this, [this, hint] {
        const bool on = markerEditing();
        for (QWidget* w : {static_cast<QWidget*>(symmetry_), static_cast<QWidget*>(clearButton_),
                           static_cast<QWidget*>(importButton_), static_cast<QWidget*>(hint)}) {
            w->setEnabled(on);
        }
        keepLengths_->setEnabled(method() != TransferMethod::Closest);
        edited(false);
    });
    options->addWidget(methodBox_);
    symmetry_ = new QCheckBox(tr("Symmetry"));
    symmetry_->setToolTip(tr("A click also adds its mirror across the mesh's middle (x), snapped onto the mesh, as the "
                             "next point on that side; deleting either deletes both"));
    options->addWidget(symmetry_);
    options->addStretch(1);
    undoButton_ = new QPushButton(tr("Undo"));
    undoButton_->setToolTip(tr("Undo the last marker edit (Ctrl+Z)"));
    connect(undoButton_, &QPushButton::clicked, this, [this] { undo(); });
    options->addWidget(undoButton_);
    redoButton_ = new QPushButton(tr("Redo"));
    redoButton_->setToolTip(tr("Redo (Ctrl+Y)"));
    connect(redoButton_, &QPushButton::clicked, this, [this] { redo(); });
    options->addWidget(redoButton_);
    clearButton_ = new QPushButton(tr("Clear"));
    clearButton_->setToolTip(tr("Delete every marker point (undoable)"));
    connect(clearButton_, &QPushButton::clicked, this, [this] { clearPoints(); });
    options->addWidget(clearButton_);
    importButton_ = new QPushButton(tr("Import Markers..."));
    importButton_->setToolTip(tr("Marker pairs from a JSON file (Maya's transferMarkers, a groom file's "
                                 "\"transferMarkers\"), replacing the points"));
    connect(importButton_, &QPushButton::clicked, this, [this] { chooseMarkersFile(); });
    options->addWidget(importButton_);
    outer->addLayout(options);

    auto* checks = new QHBoxLayout;
    keepLengths_ = new QCheckBox(tr("Keep guide lengths"));
    keepLengths_->setToolTip(tr("Guides turn with the new head but keep their lengths (off: they scale with it)"));
    checks->addWidget(keepLengths_);
    fitSize_ = new QCheckBox(tr("Fit groom to the new scalp's size"));
    fitSize_->setToolTip(tr("The new scalp taken as the old one resized: the groom's scale grows with its area (lengths, "
                            "widths and clumps follow, the strand count stays, guides scale about their roots). Off: the "
                            "groom keeps the size shown"));
    checks->addWidget(fitSize_);
    maps_ = new QCheckBox(tr("Project painted maps"));
    maps_->setChecked(true);
    maps_->setToolTip(tr("Vertex and texture maps carried onto the new mesh by the same method (a texture map's new "
                         "image goes beside the groom)"));
    checks->addWidget(maps_);
    checks->addStretch(1);
    outer->addLayout(checks);

    auto* bottom = new QHBoxLayout;
    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("transferStatus"));
    bottom->addWidget(status_, 1);
    transferButton_ = new QPushButton(tr("Transfer"));
    transferButton_->setDefault(true);
    connect(transferButton_, &QPushButton::clicked, this, [this] { transfer(); });
    bottom->addWidget(transferButton_);
    auto* close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    bottom->addWidget(close);
    outer->addLayout(bottom);
    message_ = new QLabel;
    message_->setWordWrap(true);
    message_->setObjectName(QStringLiteral("transferMessage"));
    outer->addWidget(message_);

    const auto shortcut = [this](const QKeySequence& keys, void (TransferWindow::*slot)()) {
        auto* s = new QShortcut(keys, this);
        s->setContext(Qt::WindowShortcut);
        connect(s, &QShortcut::activated, this, [this, slot] { (this->*slot)(); });
    };
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Z), &TransferWindow::undoStep);
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Y), &TransferWindow::redoStep);
    shortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), &TransferWindow::redoStep);
    // The app's charcoal look (the panels' style sheet over a solid window).
    setFont(theme::uiFont(13));
    setStyleSheet(theme::styleSheet() + QStringLiteral(
                      "QWidget#transferWindow { background: %1; } QLabel, QCheckBox { color: %2; }"
                      "QPushButton { color: %2; background: %3; border: none; border-radius: 8px; padding: 5px 12px; }"
                      "QPushButton:hover { background: %4; } QPushButton:disabled { color: %5; }"
                      "QPushButton:default { background: %6; color: white; }")
                      .arg(theme::glassSolid().name(QColor::HexRgb), theme::text().name(), theme::windowButton().name(),
                           theme::windowButtonHover().name(), theme::textMuted().name(), theme::blue().name()));
    setAttribute(Qt::WA_StyledBackground, true);
    resize(1000, 660);
    methodBox_->setCurrentIndex(0);
    keepLengths_->setEnabled(false);
    symmetry_->setEnabled(false);
    clearButton_->setEnabled(false);
    importButton_->setEnabled(false);
    hint->setEnabled(false);
    reloadGroom();
}

TransferWindow::~TransferWindow() {
    // The views' GPU objects go before the device they were made on.
    left_->releaseGpu();
    right_->releaseGpu();
    delete left_;
    delete right_;
    left_ = right_ = nullptr;
}

QRhi* TransferWindow::rhi() {
    if (!rhiTried_) {
        rhiTried_ = true;
        rhi_ = createOffscreenRhi();
    }
    return rhi_.get();
}

void TransferWindow::reloadGroom() {
    GroomDocument* doc = main_ ? main_->document() : nullptr;
    // The scalp smoothed as the groom grows on it (Scalp Subdivision): shown and picked on, the
    // markers stored on the scalp itself.
    left_->setMesh(doc ? doc->file().data.scalp : nullptr, doc ? doc->refinedFileScalp() : nullptr);
    editor_.reset(doc ? doc->transferMarkers() : std::vector<TransferMarker>{});
    hover_.reset();
    leftTitle_->setText(tr("Groom's scalp"));
    edited(false);
}

void TransferWindow::refreshScalp() {
    GroomDocument* doc = main_ ? main_->document() : nullptr;
    if (!doc) return;
    const MeshPtr shown = doc->refinedFileScalp();
    if (left_->mesh() == doc->file().data.scalp && left_->shownMesh() == shown) return;
    left_->setMesh(doc->file().data.scalp, shown, false);
}

bool TransferWindow::setNewScalpPath(const QString& path) {
    try {
        newScalp_ = readMeshFile(path.toStdString());
    } catch (const std::exception& e) {
        message_->setText(tr("Cannot read the mesh %1: %2").arg(path, QString::fromUtf8(e.what())));
        return false;
    }
    newScalpPath_ = path;
    right_->setMesh(newScalp_);
    rightTitle_->setText(tr("New scalp: %1").arg(QFileInfo(path).fileName()));
    message_->clear();
    edited(true);
    return true;
}

std::shared_ptr<const ScalpMesh> TransferWindow::meshOf(MarkerSide side) const { return view(side)->mesh(); }

void TransferWindow::setMethod(TransferMethod m) {
    const int i = methodBox_->findData(QString::fromLatin1(transferMethodName(m)));
    if (i >= 0) methodBox_->setCurrentIndex(i);
}

TransferMethod TransferWindow::method() const {
    return transferMethodFromName(methodBox_->currentData().toString().toStdString());
}

void TransferWindow::setSymmetry(bool on) { symmetry_->setChecked(on); }
bool TransferWindow::symmetry() const { return symmetry_->isChecked(); }

MarkerPoint TransferWindow::snapped(MarkerSide side, const Vec3f& world) const {
    MarkerPoint p{StrandRoot{}, world, 0};
    if (const auto mesh = meshOf(side)) snapMarkerPoint(*mesh, world, &p);
    return p;
}

std::size_t TransferWindow::addPoint(MarkerSide side, const Vec3f& world) { return addPicked(side, snapped(side, world)); }

std::size_t TransferWindow::addPicked(MarkerSide side, const MarkerPoint& picked) {
    std::size_t index = 0;
    const auto mesh = meshOf(side);
    if (symmetry() && mesh) {
        const Bounds box = boundsOf(*mesh);
        const float center = (box.lo.x + box.hi.x) * 0.5f;
        const MarkerPoint mirror = snapped(side, mirrorX(picked.point, center));
        index = editor_.addMirrored(side, picked, mirror, (box.hi - box.lo).length() * 1e-3f);
    } else {
        index = editor_.add(side, picked);
    }
    edited();
    return index;
}

std::vector<std::size_t> TransferWindow::deletePoint(MarkerSide side, std::size_t index) {
    const std::vector<std::size_t> gone = editor_.remove(side, index);
    hover_.reset();
    edited();
    return gone;
}

void TransferWindow::movePoint(MarkerSide side, std::size_t index, const Vec3f& world, bool newStep) {
    movePicked(side, index, snapped(side, world), newStep);
    edited();
}

void TransferWindow::movePicked(MarkerSide side, std::size_t index, const MarkerPoint& picked, bool newStep) {
    editor_.move(side, index, picked, newStep);
    left_->update();
    right_->update();
}

void TransferWindow::undoStep() { undo(); }
void TransferWindow::redoStep() { redo(); }

bool TransferWindow::undo() {
    if (!editor_.undo()) return false;
    hover_.reset();
    edited();
    return true;
}

bool TransferWindow::redo() {
    if (!editor_.redo()) return false;
    hover_.reset();
    edited();
    return true;
}

void TransferWindow::clearPoints() {
    editor_.clear();
    hover_.reset();
    edited();
}

bool TransferWindow::importMarkers(const QString& path) {
    std::vector<TransferMarker> markers;
    if (!main_ || !main_->readTransferMarkers(path, &markers)) {
        message_->setText(tr("Cannot read the markers file %1").arg(path));
        return false;
    }
    editor_.setPairs(markers);
    hover_.reset();
    edited();
    message_->setText(tr("Imported %n marker pairs", nullptr, static_cast<int>(markers.size())));
    return true;
}

std::vector<std::pair<MarkerSide, std::size_t>> TransferWindow::pending() const {
    std::vector<std::pair<MarkerSide, std::size_t>> out;
    for (MarkerSide side : {MarkerSide::Old, MarkerSide::New}) {
        for (std::size_t i = 0; i < editor_.count(side); ++i) {
            if (!editor_.matched(side, i)) out.emplace_back(side, i);
        }
    }
    return out;
}

void TransferWindow::setHover(MarkerHover hover) {
    if (hover == hover_) return;
    hover_ = hover;
    left_->update();
    right_->update();
}

QString TransferWindow::requirement() const {
    if (!main_ || !main_->document()) return tr("No groom open to transfer");
    if (!newScalp_) return tr("Pick the new scalp (New Scalp...)");
    return QString::fromStdString(markerRequirement(method(), editor_.pairCount()));
}

QString TransferWindow::statusText() const { return status_->text(); }

void TransferWindow::refreshStatus() {
    const QString why = requirement();
    QString text = markerEditing() ? QString::fromStdString(editor_.summary()) : tr("Markers are used by the marker methods");
    if (!why.isEmpty()) text += QStringLiteral("  —  ") + why;
    status_->setText(text);
    transferButton_->setEnabled(why.isEmpty());
    transferButton_->setToolTip(why.isEmpty() ? tr("Move the groom onto the new mesh") : why);
    undoButton_->setEnabled(markerEditing() && editor_.canUndo());
    redoButton_->setEnabled(markerEditing() && editor_.canRedo());
}

void TransferWindow::store() {
    GroomDocument* doc = main_ ? main_->document() : nullptr;
    if (!doc) return;
    std::vector<TransferMarker> pairs = editor_.pairs();
    const auto from = meshOf(MarkerSide::Old), to = meshOf(MarkerSide::New);
    if (from && to && !pairs.empty()) {
        try {
            pairs = snapTransferMarkers(pairs, *from, *to);
        } catch (const std::exception&) {
        }
    }
    if (pairs.size() == doc->transferMarkers().size() && pairs.empty()) return;
    doc->setTransferMarkers(std::move(pairs));
}

void TransferWindow::edited(bool storeIt) {
    if (storeIt) store();
    left_->update();
    right_->update();
    refreshStatus();
}

bool TransferWindow::transfer() {
    const QString why = requirement();
    if (!why.isEmpty()) {
        message_->setText(why);
        return false;
    }
    TransferOptions options;
    options.method = method();
    options.keepLengths = keepLengths_->isChecked();
    options.fitSize = fitSize_->isChecked();
    if (markerEditing()) options.markers = editor_.pairs();
    if (!main_->transferToMesh(newScalpPath_, options, maps_->isChecked())) {
        message_->setText(main_->lastReport());
        return false;
    }
    message_->setText(tr("Transferred the groom onto %1 (%2)")
                          .arg(QFileInfo(newScalpPath_).fileName(), QString::fromLatin1(transferMethodName(options.method))));
    reloadGroom();
    return true;
}

void TransferWindow::chooseNewScalp() {
    const QString path = QFileDialog::getOpenFileName(this, tr("New Scalp"), QString(), tr("Meshes (*.obj *.abc)"));
    if (!path.isEmpty()) setNewScalpPath(path);
}

void TransferWindow::chooseMarkersFile() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Marker Pairs"), QString(), tr("Markers (*.json)"));
    if (!path.isEmpty()) importMarkers(path);
}

}  // namespace groomlab::app
