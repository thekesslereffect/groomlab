// SPDX-License-Identifier: Apache-2.0
// The document's grooms (main_window.h "The document's grooms"): several grooms in one document,
// each on its own scalp, one active; the others drawn as plain strands and their scalps; the
// Groom Tools' Outliner tab; Save writing them all into one file.
#include "main_window_internal.h"

#include <filesystem>
#include <set>

#include <QFileDialog>
#include <QFileInfo>

#include "groomlab/asset_files.h"
#include "groomlab/groom_file.h"
#include "groomlab/groom_package.h"
#include "groom_tools.h"
#include "look.h"
#include "mesh_io.h"
#include "outliner_panel.h"
#include "viewport.h"

namespace groomlab::app {

namespace {

// A groom's strands as plain lines (each strand's segments), for the renderers' other grooms.
std::shared_ptr<const DisplayGeometry> strandLines(const StrandsPtr& strands) {
    if (!strands || strands->strandCount() == 0) return nullptr;
    auto g = std::make_shared<DisplayGeometry>();
    const std::vector<Vec3f>& points = strands->positions();
    const std::vector<std::size_t>& offsets = strands->offsets();
    g->positions.reserve(points.size() * 3);
    for (const Vec3f& p : points) {
        g->positions.push_back(p.x);
        g->positions.push_back(p.y);
        g->positions.push_back(p.z);
    }
    g->indices.reserve((points.size() - std::min(points.size(), strands->strandCount())) * 2);
    for (std::size_t s = 0; s < strands->strandCount(); ++s) {
        const std::size_t begin = offsets[s], end = offsets[s + 1];
        for (std::size_t i = begin; i + 1 < end; ++i) {
            g->indices.push_back(static_cast<std::uint32_t>(i));
            g->indices.push_back(static_cast<std::uint32_t>(i + 1));
        }
    }
    if (g->indices.empty()) return nullptr;
    return g;
}

// What the Outliner calls a groom's scalp: the mesh it was made on (its file's name), else "scalp".
QString scalpName(const GroomDocument& g) {
    const std::string& source = g.file().source;
    if (!source.empty()) {
        const std::string stem = std::filesystem::u8path(source.substr(0, source.find('|'))).stem().u8string();
        if (!stem.empty()) return QString::fromStdString(stem);
    }
    return QObject::tr("scalp");
}

}  // namespace

GroomDocument* MainWindow::groomAt(int index) const {
    if (!document_ || index < 0 || index >= static_cast<int>(grooms_.size())) return nullptr;
    return index == activeGroom_ ? document_.get() : grooms_[static_cast<std::size_t>(index)].get();
}

bool MainWindow::anyGroomDirty() const {
    if (groomsChanged_) return true;
    for (int i = 0; i < groomCount(); ++i) {
        if (const GroomDocument* g = groomAt(i); g && g->dirty()) return true;
    }
    return false;
}

std::string MainWindow::freeGroomName(const std::string& base) const {
    std::set<std::string> taken;
    for (int i = 0; i < groomCount(); ++i) {
        if (const GroomDocument* g = groomAt(i)) taken.insert(g->displayName());
    }
    const std::string root = base.empty() ? std::string("groom") : base;
    std::string name = root;
    for (int k = 2; taken.count(name); ++k) name = root + std::to_string(k);
    return name;
}

bool MainWindow::activateGroom(int index) {
    if (!document_ || index < 0 || index >= groomCount()) return false;
    if (index == activeGroom_) return true;
    if (refuseWhileBusy(tr("switching grooms"))) return false;
    const double time = document_->time();
    detachDocument();
    grooms_[static_cast<std::size_t>(activeGroom_)] = std::move(document_);
    activeGroom_ = index;
    document_ = std::move(grooms_[static_cast<std::size_t>(index)]);
    document_->setTime(time);  // (the grooms share the timeline)
    hiddenGrooms_.erase(document_.get());
    framed_ = true;  // (attachDocument would frame the view on the groom: it stays as it was)
    attachDocument();
    framed_ = true;
    refreshOutliner();
    notify(tr("Working on %1").arg(QString::fromStdString(document_->displayName())));
    return true;
}

bool MainWindow::addGroomsFromFile(const QString& path) {
    if (!document_) return openGroom(path);
    if (refuseWhileBusy(tr("adding grooms"))) return false;
    std::vector<std::unique_ptr<GroomDocument>> added;
    try {
        working(tr("Adding the grooms of %1...").arg(QFileInfo(path).fileName()));
        const BusyCursor wait;
        const int count = GroomDocument::groomCount(path.toStdString());
        for (int i = 0; i < count; ++i) added.push_back(GroomDocument::open(path.toStdString(), i));
    } catch (const std::exception& e) {
        report(tr("Cannot add the grooms of %1: %2").arg(path, QString::fromUtf8(e.what())));
        return false;
    }
    if (GroomDocument* first = groomAt(0); first && first->name().empty()) first->setName(first->displayName());
    const std::string stem = fileSafeName(QFileInfo(path).completeBaseName().toStdString());
    for (auto& g : added) {
        // Its name in the file, else the file's; its files stay where they are until the document is
        // saved (Save takes them into the document's folders, as Save As does).
        g->setName(freeGroomName(g->name().empty() ? stem : fileSafeName(g->name())));
        g->setTime(document_->time());
        grooms_.push_back(std::move(g));
    }
    groomsChanged_ = true;
    framed_ = false;  // Frame All on every groom
    activateGroom(static_cast<int>(grooms_.size()) - 1);
    refreshTitle();
    notify(tr("Added %n groom(s) from %1", "", static_cast<int>(added.size())).arg(QFileInfo(path).fileName()));
    return true;
}

bool MainWindow::addGroomOnMesh(const QString& meshPath) {
    if (!document_) return newGroomOnMesh(meshPath);
    if (QFileInfo(meshPath).suffix().compare(QStringLiteral("groom"), Qt::CaseInsensitive) == 0) return addGroomsFromFile(meshPath);
    if (refuseWhileBusy(tr("adding a groom"))) return false;
    std::unique_ptr<GroomDocument> groom;
    try {
        working(tr("Adding a groom on %1...").arg(QFileInfo(meshPath).fileName()));
        const BusyCursor wait;
        const std::string mesh = meshPath.toStdString();
        groom = QFileInfo(meshPath).suffix().compare(QStringLiteral("abc"), Qt::CaseInsensitive) == 0
                    ? GroomDocument::newOnAlembic(mesh)
                    : GroomDocument::newOnMesh(readMeshFile(mesh), mesh);
    } catch (const std::exception& e) {
        report(tr("Cannot make a groom on the mesh: %1 (%2)").arg(QString::fromUtf8(e.what()), meshPath));
        return false;
    }
    // Every groom of several named (their files go in folders of their names): the first by the
    // name it went by.
    if (GroomDocument* first = groomAt(0); first && first->name().empty()) first->setName(first->displayName());
    groom->setName(freeGroomName(fileSafeName(QFileInfo(meshPath).completeBaseName().toStdString())));
    groom->setPathFrom(document_->path());
    groom->setTime(document_->time());
    grooms_.push_back(std::move(groom));
    groomsChanged_ = true;
    activateGroom(static_cast<int>(grooms_.size()) - 1);
    refreshTitle();
    return true;
}

bool MainWindow::removeGroom(int index) {
    if (groomCount() <= 1 || index < 0 || index >= groomCount()) return false;
    if (refuseWhileBusy(tr("removing a groom"))) return false;
    if (index == activeGroom_ && !activateGroom(index == 0 ? 1 : 0)) return false;
    const GroomDocument* gone = grooms_[static_cast<std::size_t>(index)].get();
    const QString name = gone ? QString::fromStdString(gone->displayName()) : QString();
    hiddenGrooms_.erase(gone);
    grooms_.erase(grooms_.begin() + index);
    if (activeGroom_ > index) --activeGroom_;
    groomsChanged_ = true;
    refreshOtherGrooms();
    refreshOutliner();
    refreshTitle();
    viewport_->update();
    notify(tr("Removed %1 from the document (save to keep it so)").arg(name));
    return true;
}

bool MainWindow::renameGroom(int index, const QString& name) {
    GroomDocument* g = groomAt(index);
    const std::string wanted = name.trimmed().toStdString();
    if (!g || wanted.empty()) return false;
    if (wanted == g->displayName()) return true;
    if (freeGroomName(wanted) != wanted) {
        report(tr("Another groom is called %1: pick another name").arg(name.trimmed()));
        refreshOutliner();
        return false;
    }
    g->setName(wanted);
    refreshOutliner();
    refreshTitle();
    return true;
}

void MainWindow::setGroomVisible(int index, bool visible) {
    const GroomDocument* g = groomAt(index);
    if (!g || index == activeGroom_) return;
    if (visible) {
        hiddenGrooms_.erase(g);
    } else {
        hiddenGrooms_.insert(g);
    }
    refreshOtherGrooms();
    refreshOutliner();
    viewport_->update();
}

bool MainWindow::groomVisible(int index) const {
    const GroomDocument* g = groomAt(index);
    return g && (index == activeGroom_ || !hiddenGrooms_.count(g));
}

void MainWindow::refreshOtherGrooms() {
    std::vector<OtherGroomView> views;
    std::vector<GroomRenderer::OtherGroom> lines;
    if (document_ && grooms_.size() > 1) {
        ViewRequest request = viewRequest();
        request.stopAt.clear();
        for (std::size_t i = 0; i < grooms_.size(); ++i) {
            GroomDocument* g = grooms_[i].get();
            if (static_cast<int>(i) == activeGroom_ || !g || hiddenGrooms_.count(g)) continue;
            const OtherGroomView* was = nullptr;
            for (const OtherGroomView& v : otherViews_) {
                if (v.document == g) was = &v;
            }
            OtherGroomView view;
            view.document = g;
            g->setTime(document_->time());
            try {
                const ViewResult& r = g->evaluate(request);
                view.strands = r.strands;
                view.scalp = r.scalp ? r.scalp : g->file().data.scalp;
            } catch (const std::exception&) {
                view.scalp = g->file().data.scalp;  // (its errors are its own: shown when it is active)
            }
            view.lines = was && was->strands == view.strands ? was->lines : strandLines(view.strands);
            if (was && was->scalp == view.scalp && was->surface) {
                view.surface = was->surface;
            } else if (view.scalp) {
                auto surface = std::make_shared<Surface>();
                surface->name = "outliner:" + g->displayName();
                surface->mesh = view.scalp;
                surface->material = surfacePreset("scalp");
                view.surface = surface;
            }
            if (view.lines) lines.push_back({view.lines, {0.42f, 0.42f, 0.45f, 1.0f}});  // (dimmer than the active groom)
            views.push_back(std::move(view));
        }
    }
    otherViews_ = std::move(views);
    viewport_->renderer().setOtherGrooms(lines);
    if (headless_) headless_->renderer.setOtherGrooms(lines);
    setRendererProps();
}

void MainWindow::refreshOutliner() {
    if (!outliner_) return;
    std::vector<OutlinerPanel::Entry> entries;
    for (int i = 0; i < groomCount(); ++i) {
        const GroomDocument* g = groomAt(i);
        if (!g) continue;
        OutlinerPanel::Entry e;
        e.name = QString::fromStdString(g->displayName());
        e.scalp = scalpName(*g);
        e.active = i == activeGroom_;
        e.visible = groomVisible(i);
        e.dirty = g->dirty();
        entries.push_back(e);
    }
    outliner_->setEntries(entries);
}

void MainWindow::writeDocument(const std::string& path) {
    if (grooms_.size() <= 1) {
        document_->save(path);
        groomsChanged_ = false;
        return;
    }
    std::vector<GroomFile> files;
    const std::string folder = std::filesystem::absolute(std::filesystem::u8path(path)).parent_path().generic_u8string();
    for (int i = 0; i < groomCount(); ++i) {
        files.push_back(groomAt(i)->prepareSave(path));
        files.back().directory = folder;  // (prepareSave's paths: relative to the file)
    }
    try {
        // The core's writer, every host's (groom_package.h): each groom's files in its own folder of
        // the file's (masks/<groom>, cards/<groom>, caches/<groom>), paths relative to the file.
        writeGroomsToFile(path, files);
    } catch (const std::exception& e) {
        throw std::runtime_error(e.what());
    }
    for (int i = 0; i < groomCount(); ++i) groomAt(i)->savedAs(path, files[static_cast<std::size_t>(i)]);
    groomsChanged_ = false;
    refreshOutliner();
}

void MainWindow::installOutliner() {
    outliner_ = new OutlinerPanel();
    groomTools_->setOutlinerContent(outliner_);
    connect(outliner_, &OutlinerPanel::groomActivated, this, [this](int index) { activateGroom(index); });
    connect(outliner_, &OutlinerPanel::visibilityChanged, this, [this](int index, bool on) { setGroomVisible(index, on); });
    connect(outliner_, &OutlinerPanel::renameRequested, this, [this](int index, const QString& name) { renameGroom(index, name); });
    connect(outliner_, &OutlinerPanel::removeRequested, this, [this](int index) { removeGroom(index); });
    connect(outliner_, &OutlinerPanel::addRequested, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Add Groom"), QString(),
                                                          tr("Meshes and groom files (*.obj *.abc *.groom);;Meshes (*.obj *.abc);;Groom files (*.groom)"));
        if (!path.isEmpty()) addGroomOnMesh(path);
    });
}

}  // namespace groomlab::app
