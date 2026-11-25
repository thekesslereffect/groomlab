// SPDX-License-Identifier: Apache-2.0
#include "photos_panel.h"

#include <algorithm>
#include <cmath>

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVector3D>
#include <QVBoxLayout>

#include "groomlab/pin_morph.h"
#include "groomlab/reference_edit.h"

#include "glass.h"
#include "groom_document.h"
#include "card_parts.h"
#include "list_bar.h"
#include "param_editors.h"
#include "theme.h"

namespace groomlab::app {

namespace {

using nlohmann::json;

const ui::Definitions& defs() { return ui::Definitions::shipped(); }
QString qs(const std::string& s) { return QString::fromStdString(s); }

json cameraJson(const RefCamera& c) {
    json rot = json::array();
    for (const auto& row : c.pose.rotation) rot.push_back({row[0], row[1], row[2]});
    return {{"rotation", rot}, {"position", {c.pose.position[0], c.pose.position[1], c.pose.position[2]}}, {"focal_px", c.focalPx}};
}

std::optional<RefCamera> cameraFrom(const json& j, int width, int height) {
    if (!j.is_object() || !j.contains("rotation") || !j.contains("position")) return std::nullopt;
    try {
        RefCamera c;
        for (std::size_t i = 0; i < 3; ++i)
            for (std::size_t k = 0; k < 3; ++k) c.pose.rotation[i][k] = j["rotation"].at(i).at(k).get<double>();
        for (std::size_t i = 0; i < 3; ++i) c.pose.position[i] = j["position"].at(i).get<double>();
        c.focalPx = j.value("focal_px", 0.0);
        c.width = width;
        c.height = height;
        return c;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

ReferencePhotoEditor::State stateOf(const json& photo) {
    try {
        return referencePhotoStateFromJson(photo.contains("state") && photo["state"].is_object() ? photo["state"].dump() : std::string());
    } catch (const std::exception&) {
        return {};
    }
}

// The focal length guessed for a photo never lined up: a 50 mm lens on a 36 mm film back.
constexpr double kDefaultFocalMm = 50.0;
double focalGuess(double width) { return std::max(width, 1.0) * kDefaultFocalMm / 36.0; }

QLabel* hint(QWidget* parent, const char* role = "hint") {
    auto* l = new QLabel(parent);
    l->setProperty("role", role);
    l->setWordWrap(true);
    return l;
}

}  // namespace

PhotosPanel::PhotosPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("photosPanel"));
    const ui::json& lists = defs().get("lists/photos");
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(2);
    // The header: Photos, how many, Import Photo... (a glyph); the about line in its tip.
    header_ = new ListHeader(tr("Photos"), this);
    header_->setObjectName(QStringLiteral("photosHeader"));
    header_->setToolTip(QString::fromStdString(lists.value("about", std::string())).remove(QRegularExpression(QStringLiteral("<[^>]*>"))));
    bar_ = new ListBar(header_);
    header_->addGlyph(bar_);
    outer->addWidget(header_);

    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("referenceList"));
    plainRows(list_);
    list_->setToolTip(tr("The reference photos. Click: look through it (again: back to the camera before). The settings, in "
                         "front / behind, the eye, the lock and the menu: on the row"));
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_, &QListWidget::currentRowChanged, this, [this](int) {
        if (updating_) return;
        // Looking through a photo: the picked one is looked through instead (a click on the one
        // looked through still goes back to the camera before).
        if (const int i = current(); i >= 0 && !looking_.isEmpty() && name(i) != looking_) emit lookThroughRequested(i, true);
        refreshControls();
    });
    // lists.json photos.click: a click on a photo looks through it; on the one looked through,
    // back to the camera before.
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) { clickRow(list_->row(item)); });
    connect(list_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (QMenu* menu = contextMenu(pos)) menu->popup(list_->viewport()->mapToGlobal(pos));
    });
    outer->addWidget(list_);
    empty_ = new QLabel(tr("No photos yet: import one to groom to (each photo is a camera)."), this);
    empty_->setProperty("role", "hint");
    empty_->setWordWrap(true);
    outer->addWidget(empty_);

    // A photo's settings (opacity, the lens, where the alignment stands) in a popup under the
    // row's sliders icon; in front / behind, the eye and the lock are the row's icons; Rename,
    // Align Photo, Select Camera and Remove its menu (lists.json's photos context).
    settingsPopup_ = new PopupPanel(this, 260);
    settingsPopup_->setObjectName(QStringLiteral("photoSettingsPopup"));
    box_ = settingsPopup_;
    QVBoxLayout* layout = settingsPopup_->rows();
    settingsPopup_->onShow([this] { refreshControls(); });
    opacity_ = new ValueSlider(settingsPopup_);
    opacity_->setObjectName(QStringLiteral("referenceOpacity"));
    opacity_->setCaption(tr("Opacity"));
    opacity_->setRange(0.0, 1.0);
    opacity_->setLimits(0.0, 1.0);
    opacity_->setDecimals(2);
    opacity_->setStep(0.05);
    opacity_->setValue(0.6);
    opacity_->setFill(theme::blue());
    opacity_->setToolTip(tr("How opaque the photo is: see the mesh through it"));
    opacity_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(opacity_, &ValueSlider::valueChanged, this, [this](double v) {
        if (updating_ || current() < 0) return;
        Display d = display(current());
        d.opacity = v;
        setDisplay(current(), d);
    });
    layout->addWidget(opacity_);

    // The photo camera's focal length (mm on a 36 mm film back), by hand: Keep focal length turns on,
    // so a solve keeps it. A locked camera keeps its own.
    focal_ = new ValueSlider(settingsPopup_);
    focal_->setObjectName(QStringLiteral("referenceFocal"));
    focal_->setCaption(tr("Focal length"));
    focal_->setRange(10.0, 200.0);
    focal_->setLimits(2.5, 3500.0);
    focal_->setDecimals(1);
    focal_->setStep(1.0);
    focal_->setValue(50.0);
    focal_->setFill(theme::blue());
    focal_->setToolTip(tr("The photo camera's focal length (mm): Align Photo works it out from the point pairs; set it by hand when "
                          "you know the lens (Keep focal length turns on, so the alignment keeps it). A locked camera keeps its own"));
    focal_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(focal_, &ValueSlider::valueChanged, this, [this](double mm) {
        const int i = current();
        if (updating_ || i < 0 || display(i).locked) return;
        const QSize size = imageSize(i);
        std::optional<RefCamera> c = camera(i);
        if (!c && placeCamera(i)) c = camera(i);
        if (!c || size.isEmpty()) return;
        c->focalPx = mm * size.width() / 36.0;
        setCamera(i, *c);
        if (Display d = display(i); !d.keepFocal) {  // (the alignment keeps the focal length set by hand)
            d.keepFocal = true;
            setDisplay(i, d);
        }
        if (!looking_.isEmpty() && name(i) == looking_) emit lookThroughRequested(i, true);  // the view through it, again
        alignAbout_->setText(alignText());
    });
    focal_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(focal_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        auto* menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->addAction(tr("Reset Focal Length (%1 mm)").arg(kDefaultFocalMm), this, [this] { resetFocal(); });
        menu->popup(focal_->mapToGlobal(pos));
    });
    layout->addWidget(focal_);

    alignAbout_ = hint(settingsPopup_);
    alignAbout_->setObjectName(QStringLiteral("referenceAlignAbout"));
    layout->addWidget(alignAbout_);

    // Morph to Photos (the app's own): the scalp shaped so the lined-up photos' pairs land
    // (KeenTools-style pins), folded under the list.
    auto* morphBody = new QWidget(this);
    layout = new QVBoxLayout(morphBody);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    {
        auto* row = new QHBoxLayout();
        row->setSpacing(4);
        morph_ = new QPushButton(tr("Morph to Photos"), this);
        morph_->setObjectName(QStringLiteral("referenceMorph"));
        morph_->setToolTip(tr("Shape the scalp so every lined-up photo's pairs land on their photo points (two photos "
                              "of a pair set its depth too); the groom follows by its UVs. Always from the scalp as it "
                              "was before the first morph"));
        connect(morph_, &QPushButton::clicked, this, [this] {
            QString error;
            const auto mesh = morphedScalp(morphStiffness_->value(), morphSymmetry_->isChecked(), &error);
            if (!mesh) {
                emit message(error);
                return;
            }
            emit morphRequested(mesh, tr("the photos"));
        });
        row->addWidget(morph_, 1);
        morphSymmetry_ = new QCheckBox(tr("Symmetry"), this);
        morphSymmetry_->setChecked(true);
        morphSymmetry_->setToolTip(tr("Morph both sides alike: each pair mirrored across the scalp's middle (x = 0)"));
        row->addWidget(morphSymmetry_);
        morphStiffness_ = new QDoubleSpinBox(this);
        morphStiffness_->setRange(0.0, 0.95);
        morphStiffness_->setSingleStep(0.05);
        morphStiffness_->setDecimals(2);
        morphStiffness_->setPrefix(tr("Stiffness "));
        morphStiffness_->setToolTip(tr("0 lands every pair exactly; higher keeps the scalp's shape, the pairs falling short"));
        row->addWidget(morphStiffness_);
        layout->addLayout(row);
        unmorph_ = new QPushButton(tr("Reset Morph"), this);
        unmorph_->setObjectName(QStringLiteral("referenceUnmorph"));
        unmorph_->setToolTip(tr("The scalp as it was before the first morph"));
        connect(unmorph_, &QPushButton::clicked, this, [this] {
            if (morphCage_) emit morphRequested(morphCage_, tr("its unmorphed scalp"));
        });
        layout->addWidget(unmorph_);
    }

    morphSection_ = new Section(tr("Morph to photos"), morphBody, false, this);
    morphSection_->setObjectName(QStringLiteral("photosMorphSection"));
    morphSection_->header()->setToolTip(tr("Shape the scalp so the lined-up photos' pairs land on their photo points"));
    outer->addWidget(morphSection_);
    outer->addStretch(1);
    refresh();
}

std::shared_ptr<const ScalpMesh> PhotosPanel::morphedScalp(double stiffness, bool symmetric, QString* error) {
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return std::shared_ptr<const ScalpMesh>();
    };
    if (!document_) return fail(tr("No groom open"));
    if (morphDocument_ != document_ || !morphCage_) {  // the scalp before any morph: kept from now on
        morphDocument_ = document_;
        morphCage_ = document_->file().data.scalp;
        morphRefined_ = document_->refinedFileScalp();
    }
    if (!morphCage_ || !morphRefined_) return fail(tr("The groom has no scalp"));
    std::vector<PinView> views;
    std::vector<MorphPin> pins;
    const json all = photos();
    const float size = std::max(1e-6f, [&] {
        Vec3f lo = morphRefined_->points().front(), hi = lo;
        for (const Vec3f& p : morphRefined_->points())
            lo = Vec3f(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)),
            hi = Vec3f(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
        return (hi - lo).length();
    }());
    for (int i = 0; i < count(); ++i) {
        const std::optional<RefCamera> cam = camera(i);
        if (!cam || !(cam->focalPx > 0.0)) continue;  // not lined up: says nothing of the scalp's shape
        const MarkerLists landmarks = stateOf(all[i]).landmarks;
        if (landmarks.pairCount() == 0) continue;
        PinView view;
        view.pose = cam->pose;
        view.focalPx = cam->focalPx;
        view.width = all[i].value("width", 0);
        view.height = all[i].value("height", 0);
        views.push_back(view);
        const int index = static_cast<int>(views.size()) - 1;
        for (std::size_t k = 0; k < landmarks.pairCount(); ++k) {
            const MarkerPoint& photo = landmarks.point(kLandmarkPhoto, k);
            const MarkerPoint& mesh = landmarks.point(kLandmarkMesh, k);
            // The pin's point on the unmorphed scalp: its spot on the surface, so a pair picked on a
            // morphed scalp stays a point of the original one.
            const bool onSurface = mesh.root.face >= 0 && static_cast<std::size_t>(mesh.root.face) < morphRefined_->triangleCount();
            const Vec3f rest = onSurface ? morphRefined_->position(mesh.root) : mesh.point;
            MorphPin* pin = nullptr;
            for (MorphPin& p : pins)
                if ((Vec3f(static_cast<float>(p.mesh[0]), static_cast<float>(p.mesh[1]), static_cast<float>(p.mesh[2])) - rest).length() <= 1e-4f * size)
                    pin = &p;
            if (!pin) {
                pins.push_back({{rest.x, rest.y, rest.z}, {}});
                pin = &pins.back();
            }
            pin->sights.push_back({index, photo.point.x, photo.point.y});
        }
    }
    if (pins.empty()) return fail(tr("No lined-up photo has pairs: Align Photo, pick pairs and line it up first"));
    std::vector<CameraVec3> points;
    for (const Vec3f& p : morphCage_->points()) points.push_back({p.x, p.y, p.z});
    PinMorphOptions options;
    options.stiffness = stiffness;
    options.symmetric = symmetric;
    const PinMorphResult r = morphToPins(points, pins, views, options);
    if (!r.ok) return fail(QString::fromStdString(r.error));
    std::vector<Vec3f> moved;
    for (const CameraVec3& p : r.points) moved.emplace_back(static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]));
    emit message(tr("Morphed to %1 pins: %2 px off before, %3 px after").arg(pins.size()).arg(r.rmsBeforePx, 0, 'f', 1)
                     .arg(r.rmsAfterPx, 0, 'f', 1));
    return std::make_shared<const ScalpMesh>(morphCage_->withPoints(std::move(moved)));
}

void PhotosPanel::setDocument(GroomDocument* document) {
    document_ = document;
    looking_.clear();
    refresh();
}

void PhotosPanel::setActions(const ui::Bindings& bindings, const ui::FillOptions& options) {
    bindings_ = bindings;
    options_ = options;
    options_.liveShortcuts = false;
    // Import (and Add Camera where a host has it) in the header (glyphs); a photo's icons and menu
    // on its row.
    bar_->build(defs().get("lists/photos/bar"), bindings_, options_, ListBar::Part::Labeled);
    if (QToolButton* b = bar_->button("photo.import")) b->setObjectName(QStringLiteral("referenceImport"));
    refresh();
}

void PhotosPanel::clickRow(int index) {
    // A click on a photo: look through its camera; on the one looked through: back to the camera
    // before (the photo unpicked).
    if (index < 0 || index >= count()) return;
    if (!looking_.isEmpty() && name(index) == looking_) {
        emit lookThroughRequested(index, false);
        {
            const QSignalBlocker block(list_);
            list_->clearSelection();
            list_->setCurrentRow(-1);
        }
        refresh();
        return;
    }
    const bool looking = !looking_.isEmpty();
    select(index);  // (looking through another photo already: the pick looks through this one)
    if (!looking || name(index) != looking_) emit lookThroughRequested(index, true);
    refresh();
}

void PhotosPanel::fillRowMenu(QMenu* menu) {
    ui::FillOptions options = options_;
    options.resolve.state = menuState();
    ui::fillMenu(menu, defs().get("lists/photos/context"), bindings_, options);
}

QWidget* PhotosPanel::rowFor(int index) {
    // A photo's row (ROW_ORDER): the camera mark while looking through it and its name, then its
    // settings (a popup), in front / behind, the eye, the lock (in the accent while locked), the menu.
    const Display d = display(index);
    const QString n = name(index);
    auto pick = [this, index] {
        if (current() != index) select(index);
    };
    QToolButton* settings = popupButton(QStringLiteral("sliders"), tr("Opacity and focal length"), settingsPopup_, pick, list_);
    settings->setObjectName(QStringLiteral("photoSettings_") + n);
    QToolButton* side = rowGlyph(d.front ? QStringLiteral("front") : QStringLiteral("behind"),
                                 d.front ? tr("In front of the mesh: click to draw it behind") : tr("Behind the mesh: click to draw it in front"),
                                 list_);
    side->setObjectName(QStringLiteral("photoSide_") + n);
    connect(side, &QToolButton::clicked, this, [this, index] {
        QTimer::singleShot(0, this, [this, index] {
            Display now = display(index);
            now.front = !now.front;
            setDisplay(index, now);
            refresh();
        });
    });
    QToolButton* eye = rowGlyph(d.visible ? QStringLiteral("eye") : QStringLiteral("eye_off"), tr("Show or hide the photo (the camera stays)"), list_);
    eye->setObjectName(QStringLiteral("photoEye_") + n);
    connect(eye, &QToolButton::clicked, this, [this, index] {
        QTimer::singleShot(0, this, [this, index] {
            Display now = display(index);
            now.visible = !now.visible;
            setDisplay(index, now);
            refresh();
        });
    });
    QToolButton* lock = rowGlyph(d.locked ? QStringLiteral("lock") : QStringLiteral("lock_open"),
                                 d.locked ? tr("Locked: the camera can't move. Click to unlock")
                                          : tr("Lock the camera: its position, rotation, focal length and the photo on it (tumbling "
                                               "through it does nothing)"),
                                 list_, {}, d.locked);
    lock->setObjectName(QStringLiteral("photoLock_") + n);
    connect(lock, &QToolButton::clicked, this, [this, index] {
        QTimer::singleShot(0, this, [this, index] {
            Display now = display(index);
            now.locked = !now.locked;
            setDisplay(index, now);
            refresh();
        });
    });
    QToolButton* more = rowMenuButton(tr("Rename, Align Photo, Select Camera, Remove"), [this](QMenu* menu) { fillRowMenu(menu); }, pick, list_);
    more->setObjectName(QStringLiteral("photoMenu_") + n);
    const bool viewing = !looking_.isEmpty() && n == looking_;
    auto* row = new ListRow((viewing ? QStringLiteral("\u25C9 ") : QString()) + n, QString(), {settings, side, eye, lock, more}, list_);
    if (viewing) row->label()->setStyleSheet(QStringLiteral("color: %1;").arg(theme::accentInk().name()));
    return row;
}

json PhotosPanel::photos() const {
    if (!document_ || document_->referencePhotos().empty()) return json::array();
    json all = json::parse(document_->referencePhotos(), nullptr, false);
    return all.is_array() ? all : json::array();
}

void PhotosPanel::store(const json& all) {
    if (!document_) return;
    document_->setReferencePhotos(all.empty() ? std::string() : all.dump());
    emit edited();
}

int PhotosPanel::count() const { return static_cast<int>(photos().size()); }

int PhotosPanel::current() const {
    const int row = list_->currentRow();
    return row >= 0 && row < count() ? row : -1;
}

void PhotosPanel::select(int index) {
    if (index < 0 || index >= list_->count()) return;
    list_->setCurrentRow(index);
    refreshControls();
}

QString PhotosPanel::name(int index) const {
    const json all = photos();
    return index >= 0 && index < static_cast<int>(all.size()) ? qs(all[index].value("name", std::string())) : QString();
}

QString PhotosPanel::imagePath(int index) const {
    const json all = photos();
    if (index < 0 || index >= static_cast<int>(all.size())) return QString();
    const std::string stored = all[index].value("image", std::string());
    return qs(document_ ? document_->referenceImageFile(stored) : stored);  // relative to the groom's folder
}

QSize PhotosPanel::imageSize(int index) const {
    const json all = photos();
    if (index < 0 || index >= static_cast<int>(all.size())) return {};
    return QSize(all[index].value("width", 0), all[index].value("height", 0));
}

QImage PhotosPanel::picture(int index) const {
    const QString path = imagePath(index);
    if (path.isEmpty()) return {};
    const QString key = path + QLatin1Char('|') + QString::number(QFileInfo(path).lastModified().toMSecsSinceEpoch());
    auto it = pictures_.find(key);
    if (it == pictures_.end()) it = pictures_.insert(key, QImage(path));
    return *it;
}

std::optional<RefCamera> PhotosPanel::camera(int index) const {
    const json all = photos();
    if (index < 0 || index >= static_cast<int>(all.size())) return std::nullopt;
    return cameraFrom(all[index].value("camera", json()), all[index].value("width", 0), all[index].value("height", 0));
}

void PhotosPanel::setCamera(int index, const RefCamera& c) {
    json all = photos();
    if (index < 0 || index >= static_cast<int>(all.size())) return;
    all[index]["camera"] = cameraJson(c);
    store(all);
}

PhotosPanel::Display PhotosPanel::display(int index) const {
    Display d;
    const json all = photos();
    if (index < 0 || index >= static_cast<int>(all.size())) return d;
    const json j = all[index].value("display", json::object());
    if (!j.is_object()) return d;
    d.opacity = std::clamp(j.value("opacity", d.opacity), 0.0, 1.0);
    d.front = j.value("depth", std::string("front")) != "behind";
    d.locked = j.value("locked", false);
    d.visible = j.value("visible", true);
    d.keepFocal = j.value("keepFocal", false);
    return d;
}

void PhotosPanel::setDisplay(int index, const Display& d) {
    json all = photos();
    if (index < 0 || index >= static_cast<int>(all.size())) return;
    all[index]["display"] = {{"opacity", d.opacity}, {"depth", d.front ? "front" : "behind"}, {"locked", d.locked},
                             {"visible", d.visible},   {"keepFocal", d.keepFocal}};
    store(all);
    refreshControls();
    emit displayChanged();
}

QString PhotosPanel::uniqueName(const QString& wanted, int except) const {
    const json all = photos();
    auto taken = [&](const QString& n) {
        for (int i = 0; i < static_cast<int>(all.size()); ++i) {
            if (i != except && qs(all[i].value("name", std::string())) == n) return true;
        }
        return false;
    };
    QString name = wanted.trimmed().isEmpty() ? QStringLiteral("photo") : wanted.trimmed();
    for (int k = 2; taken(name); ++k) name = wanted.trimmed() + QStringLiteral("_%1").arg(k);
    return name;
}

int PhotosPanel::importPhoto(const QString& path) {
    if (!document_) {
        emit message(tr("Open a groom first"));
        return -1;
    }
    const QImage image(path);
    if (image.isNull()) {
        emit message(tr("Cannot read the image %1").arg(path));
        return -1;
    }
    json photo = {{"name", uniqueName(QFileInfo(path).completeBaseName()).toStdString()},
                  {"image", [&] {
                       const std::string stored = document_->importReferenceImage(QFileInfo(path).absoluteFilePath().toStdString());
                       return stored.empty() ? QFileInfo(path).absoluteFilePath().toStdString() : stored;  // (kept where it is)
                   }()},
                  {"width", image.width()},
                  {"height", image.height()},
                  {"mask", nullptr}};
    photo["camera"] = cameraJson(placedReferenceCamera(document_, image.width(), image.height()));
    photo["state"] = json::parse(referencePhotoStateToJson({}));
    photo["display"] = {{"opacity", 0.6}, {"depth", "front"}, {"locked", false}, {"visible", true}};
    json all = photos();
    all.push_back(photo);
    store(all);
    refresh();
    select(static_cast<int>(all.size()) - 1);
    return static_cast<int>(all.size()) - 1;
}

bool PhotosPanel::rename(int index, const QString& wanted) {
    json all = photos();
    if (index < 0 || index >= static_cast<int>(all.size()) || wanted.trimmed().isEmpty()) return false;
    const QString name = wanted.trimmed();
    if (uniqueName(name, index) != name) {
        emit message(tr("Another photo is named %1").arg(name));
        return false;
    }
    const bool looking = looking_ == qs(all[index].value("name", std::string()));
    all[index]["name"] = name.toStdString();
    store(all);
    if (looking) looking_ = name;
    refresh();
    select(index);
    return true;
}

bool PhotosPanel::remove(int index) {
    json all = photos();
    if (index < 0 || index >= static_cast<int>(all.size())) return false;
    if (qs(all[index].value("name", std::string())) == looking_) emit lookThroughRequested(index, false);
    all.erase(all.begin() + index);
    store(all);
    refresh();
    if (count() > 0) select(std::min(index, count() - 1));
    return true;
}

RefCamera placedReferenceCamera(const GroomDocument* document, int width, int height) {
    // In front (+Z) of the groom, far enough for it to fill most of the photo's short side (Maya's
    // reference._place), looking down -Z, a 50 mm lens.
    const Bounds b = document ? document->sceneBounds() : Bounds();
    RefCamera c;
    c.width = width;
    c.height = height;
    c.focalPx = focalGuess(width);
    QVector3D center(0.0f, 0.0f, 0.0f);
    float radius = 1.0f;
    if (b.valid) {
        center = QVector3D(b.lo.x + b.hi.x, b.lo.y + b.hi.y, b.lo.z + b.hi.z) * 0.5f;
        radius = std::max(QVector3D(b.hi.x - b.lo.x, b.hi.y - b.lo.y, b.hi.z - b.lo.z).length() * 0.5f, 1e-3f);
    }
    const double half = std::min(std::atan(0.5 * std::max(width, 1) / c.focalPx), std::atan(0.5 * std::max(height, 1) / c.focalPx));
    const double distance = radius / std::sin(half) * 1.05;
    c.pose.position = {center.x(), center.y(), center.z() + distance};
    return c;
}

bool PhotosPanel::placeCamera(int index) {
    const QSize size = imageSize(index);
    if (index < 0 || index >= count() || size.isEmpty()) return false;
    setCamera(index, placedReferenceCamera(document_, size.width(), size.height()));
    return true;
}

// ---- Where the alignment stands ------------------------------------------------------------------

void PhotosPanel::resetFocal() {
    if (current() < 0 || display(current()).locked) return;
    focal_->setRange(10.0, 200.0);
    focal_->setValue(kDefaultFocalMm);
    emit focal_->valueChanged(kDefaultFocalMm);
}

bool PhotosPanel::keepFocal() const { return current() >= 0 && display(current()).keepFocal; }

QString PhotosPanel::alignText() const {
    const int i = current();
    if (i < 0) return tr("Import a photo to start.");
    const json all = photos();
    const MarkerLists landmarks = stateOf(all[i]).landmarks;
    const int width = all[i].value("width", 0), height = all[i].value("height", 0);
    const std::size_t complete = landmarks.pairCount(), need = keepFocal() ? kReferenceMinPairs : kReferenceMinPairsFocal;
    const std::optional<RefCamera> was = camera(i);
    if (complete >= kReferenceMinPairs) {
        // How well the pairs fit the camera they give (the live solve, as the Photos window shows it).
        const ReferenceLandmarkSolve s = solveReferenceLandmarks(landmarks, width, height, was && was->focalPx > 0.0 ? was->focalPx : focalGuess(width),
                                                                 keepFocal(), was ? &was->pose : nullptr, false);
        if (s.ok) {
            return tr("Lined up by %1 pairs: %2 px off on average, %3 mm.")
                .arg(complete)
                .arg(s.rms, 0, 'f', 2)
                .arg((was ? was->focalPx : s.camera.focalPx) * 36.0 / std::max(width, 1), 0, 'f', 1);
        }
    }
    if (complete > 0) {
        return tr("%1 of %2 pairs%3: Align Photo to add more.")
            .arg(complete)
            .arg(need)
            .arg(keepFocal() ? tr(" (focal length kept)") : tr(", or 4 keeping the focal length"));
    }
    return tr("Not lined up yet: Align Photo, then click a feature on the photo and the same one on the mesh.");
}

// ---- Looking through ----------------------------------------------------------------------------------

void PhotosPanel::setLookingThrough(const QString& name) {
    if (looking_ == name) return;
    looking_ = name;
    refresh();  // (the rows' camera mark)
}

int PhotosPanel::lookingIndex() const {
    if (looking_.isEmpty()) return -1;
    for (int i = 0; i < count(); ++i) {
        if (name(i) == looking_) return i;
    }
    return -1;
}

std::set<std::string> PhotosPanel::menuState() const {
    std::set<std::string> state;
    if (document_) state.insert("groom");
    if (current() >= 0) state.insert("photo");
    return state;
}

QMenu* PhotosPanel::contextMenu(const QPoint& pos) {
    QListWidgetItem* item = list_->itemAt(pos);
    if (!item) return listMenu(this, defs().get("lists/photos/bar"), bindings_, options_, menuState());  // (no photo: Import)
    list_->setCurrentItem(item);
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("listContextMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    fillRowMenu(menu);
    return menu;
}

// ---- What it shows ------------------------------------------------------------------------------------

void PhotosPanel::refresh() {
    const json all = photos();
    const int was = list_->currentRow();
    updating_ = true;
    list_->clear();
    for (const json& p : all) {
        const QString path = qs(p.value("image", std::string()));
        // One line: the row widget shows the name and its icons (the item's own text empty).
        auto* item = new QListWidgetItem(QString());
        item->setSizeHint(QSize(0, 26));
        item->setData(Qt::UserRole, qs(p.value("name", std::string())));
        item->setToolTip(QStringLiteral("%1\n%2 x %3").arg(path).arg(p.value("width", 0)).arg(p.value("height", 0)));
        list_->addItem(item);
        list_->setItemWidget(item, rowFor(list_->count() - 1));
    }
    // The photo looked through is the one picked (looked through from anywhere: the viewport's
    // menu, a script); else the one picked before; none stays none (a photo left: no row falls
    // back to the first).
    const int looked = lookingIndex();
    const int pick = looked >= 0 ? looked : was >= 0 && list_->count() > 0 ? std::clamp(was, 0, list_->count() - 1) : -1;
    if (pick >= 0) {
        list_->setCurrentRow(pick);
    } else {
        list_->clearSelection();
        list_->setCurrentRow(-1);
    }
    updating_ = false;
    header_->setCount(list_->count());
    empty_->setVisible(list_->count() == 0);
    morphSection_->setVisible(list_->count() > 0);
    fitRows(list_);
    bar_->refresh();
    refreshControls();
}

void PhotosPanel::refreshControls() {
    const int i = current();
    const bool on = i >= 0;
    opacity_->setEnabled(on);
    updating_ = true;
    const Display d = display(i);
    if (!opacity_->editing()) opacity_->setValue(d.opacity);
    focal_->setEnabled(on && !d.locked);  // (a locked camera keeps its focal length: greyed)
    if (const std::optional<RefCamera> c = on ? camera(i) : std::nullopt; c && c->focalPx > 0.0 && !focal_->editing()) {
        focal_->setValue(c->focalPx * 36.0 / std::max(imageSize(i).width(), 1));
    }
    updating_ = false;
    bar_->refresh();
    alignAbout_->setText(alignText());
    if (morph_) {
        morph_->setEnabled(document_ != nullptr && count() > 0);
        unmorph_->setEnabled(morphCage_ != nullptr && morphDocument_ == document_);
    }
}

}  // namespace groomlab::app
