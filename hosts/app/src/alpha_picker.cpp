// SPDX-License-Identifier: Apache-2.0
#include "alpha_picker.h"

#include <filesystem>
#include <map>

#include <QDesktopServices>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidgetAction>

#include "groomlab/brush_alpha.h"
#include "groomlab/texture.h"

namespace groomlab::app {

namespace fs = std::filesystem;

namespace {

constexpr int kThumb = 64;

// An alpha's thumbnail (white: full weight), kept until its file changes; a dashed empty square
// for none.
QIcon alphaIcon(const std::string& path) {
    if (path.empty()) {
        QPixmap pixmap(kThumb, kThumb);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setPen(QPen(QColor(140, 140, 140), 2, Qt::DashLine));
        painter.drawRect(4, 4, kThumb - 9, kThumb - 9);
        painter.drawLine(8, kThumb - 9, kThumb - 9, 8);
        return QIcon(pixmap);
    }
    static std::map<std::string, std::pair<std::uint64_t, QIcon>> cache;
    const std::uint64_t stamp = fileStamp(path);
    auto it = cache.find(path);
    if (it != cache.end() && it->second.first == stamp) return it->second.second;
    QIcon icon;
    try {
        const std::vector<std::uint8_t> pixels = loadBrushAlpha(path)->thumbnail(kThumb);
        const QImage image(pixels.data(), kThumb, kThumb, kThumb, QImage::Format_Grayscale8);
        icon = QIcon(QPixmap::fromImage(image.copy()));
    } catch (const std::exception&) {
        return alphaIcon({});
    }
    cache[path] = {stamp, icon};
    return icon;
}

std::string stemOf(const std::string& path) { return fs::u8path(path).stem().u8string(); }

bool isFile(const std::string& path) {
    std::error_code ec;
    return !path.empty() && fs::is_regular_file(fs::u8path(path), ec);
}

}  // namespace

AlphaPicker::AlphaPicker(QWidget* parent) : QWidget(parent) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    button_ = new QToolButton(this);
    button_->setObjectName(QStringLiteral("alphaButton"));
    button_->setIconSize(QSize(40, 40));
    button_->setPopupMode(QToolButton::InstantPopup);
    button_->setContextMenuPolicy(Qt::CustomContextMenu);
    name_ = new QLabel(this);
    name_->setObjectName(QStringLiteral("alphaName"));
    row->addWidget(button_);
    row->addWidget(name_, 1);

    popup_ = new QMenu(button_);
    auto* panel = new QWidget(popup_);
    auto* column = new QVBoxLayout(panel);
    column->setContentsMargins(6, 6, 6, 6);
    grid_ = new QListWidget(panel);
    grid_->setObjectName(QStringLiteral("alphaGrid"));
    grid_->setViewMode(QListView::IconMode);
    grid_->setIconSize(QSize(kThumb, kThumb));
    grid_->setGridSize(QSize(kThumb + 18, kThumb + 26));
    grid_->setResizeMode(QListView::Adjust);
    grid_->setMovement(QListView::Static);
    grid_->setWordWrap(true);
    grid_->setFixedSize((kThumb + 18) * 4 + 24, (kThumb + 26) * 3 + 8);
    connect(grid_, &QListWidget::itemClicked, this, &AlphaPicker::pick);
    column->addWidget(grid_);
    auto* buttons = new QHBoxLayout();
    auto* add = new QPushButton(tr("Add Alpha..."), panel);
    add->setToolTip(tr("Copy an image (PNG, JPEG, TGA, PSD...) into your alpha folder: every host lists it"));
    connect(add, &QPushButton::clicked, this, &AlphaPicker::addFromDialog);
    auto* folder = new QPushButton(tr("Open Folder"), panel);
    folder->setToolTip(tr("Your alpha folder (Documents/GroomLab/alphas): images dropped there are listed too"));
    connect(folder, &QPushButton::clicked, this, &AlphaPicker::openFolder);
    buttons->addWidget(add);
    buttons->addWidget(folder);
    buttons->addStretch(1);
    column->addLayout(buttons);
    auto* action = new QWidgetAction(popup_);
    action->setDefaultWidget(panel);
    popup_->addAction(action);
    connect(popup_, &QMenu::aboutToShow, this, &AlphaPicker::fill);
    button_->setMenu(popup_);

    connect(button_, &QToolButton::customContextMenuRequested, this, [this](const QPoint& at) {
        QMenu menu(this);
        QAction* clear = menu.addAction(tr("Clear Alpha"));
        clear->setEnabled(!path_.empty());
        connect(clear, &QAction::triggered, this, [this] { setPath({}); });
        connect(menu.addAction(tr("Add Alpha...")), &QAction::triggered, this, &AlphaPicker::addFromDialog);
        connect(menu.addAction(tr("Open Alpha Folder")), &QAction::triggered, this, &AlphaPicker::openFolder);
        menu.exec(button_->mapToGlobal(at));
    });
    showAlpha();
}

void AlphaPicker::fill() {
    grid_->clear();
    std::vector<BrushAlphaFile> entries = {{"None", "", false}};
    for (BrushAlphaFile& a : listBrushAlphas()) entries.push_back(std::move(a));
    for (const BrushAlphaFile& a : entries) {
        auto* item = new QListWidgetItem(alphaIcon(a.path), QString::fromStdString(a.name), grid_);
        item->setData(Qt::UserRole, QString::fromStdString(a.path));
        item->setToolTip(a.path.empty() ? tr("No alpha: the brush's falloff alone") : QString::fromStdString(a.path));
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        if (a.path == path_) grid_->setCurrentItem(item);
    }
}

void AlphaPicker::pick(QListWidgetItem* item) {
    popup_->close();
    if (item) setPath(item->data(Qt::UserRole).toString().toStdString());
}

void AlphaPicker::setPath(const std::string& path, bool notify) {
    const std::string kept = isFile(path) ? path : std::string();
    const bool changedNow = kept != path_;
    path_ = kept;
    showAlpha();
    if (notify && changedNow) emit changed(QString::fromStdString(path_));
}

void AlphaPicker::showAlpha() {
    button_->setIcon(alphaIcon(path_));
    name_->setText(path_.empty() ? tr("None") : QString::fromStdString(stemOf(path_)));
    button_->setToolTip((path_.empty() ? tr("No alpha") : QString::fromStdString(path_)) + tr(" (click: pick one; right-click: more)"));
}

std::string AlphaPicker::add(const std::string& file) {
    try {
        setPath(addBrushAlpha(file));
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}

void AlphaPicker::addFromDialog() {
    popup_->close();
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Add Alpha"), {}, tr("Images (*.png *.jpg *.jpeg *.tga *.bmp *.psd *.gif *.hdr *.pic *.pnm *.ppm *.pgm)"));
    for (const QString& f : files) {
        const std::string error = add(f.toStdString());
        if (!error.empty()) QMessageBox::warning(this, tr("Add Alpha"), QString::fromStdString(error));
    }
}

void AlphaPicker::openFolder() {
    popup_->close();
    const std::string folder = brushAlphaUserDir();
    if (folder.empty()) return;
    std::error_code ec;
    fs::create_directories(fs::u8path(folder), ec);
    QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(folder)));
}

}  // namespace groomlab::app
