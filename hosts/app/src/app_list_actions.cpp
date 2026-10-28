// SPDX-License-Identifier: Apache-2.0
// What the Groom Tools' lists' bars and right-click menus run (engine/ui/lists.json's groups,
// maps and correctives, as Maya's tool_tabs.py GuidesTab, PaintTab and CorrectivesBox ACTIONS):
// the panels' edits (each one undo step), and what asks first over the viewport's prompt (never a
// dialog): New Group's name, Delete Group, Promote / Demote, Mirror Group's side, Reparent's
// parent, a texture's resolution, deleting a map something reads. A color or a file is picked in
// Qt's own non-modal color dialog and the file dialog (interactive runs only, as the window's Open).

#include <QColorDialog>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QUrl>

#include "app_actions.h"
#include "app_shell.h"
#include "brush_session.h"
#include "correctives_panel.h"
#include "groom_document.h"
#include "groom_tools.h"
#include "guides_panel.h"
#include "main_window.h"
#include "maps_panel.h"
#include "overlay.h"

namespace groomlab::app {

namespace {

using ui::json;

QString qs(const std::string& s) { return QString::fromStdString(s); }

const QString kImageFilter = QObject::tr("Images (*.png *.jpg *.jpeg *.tga *.bmp *.psd *.gif *.hdr *.tif *.tiff *.exr);;PNG (*.png);;All files (*.*)");

// Shows a file in the system's file browser (Explorer: its folder with the file selected); a
// file that is not there shows its folder, if that is (Maya's reveal_file). Returns what was shown.
QString revealFile(const QString& path) {
    const QFileInfo info(path);
    if (info.isFile()) {
#ifdef _WIN32
        if (QProcess::startDetached(QStringLiteral("explorer"), {QStringLiteral("/select,"), QDir::toNativeSeparators(info.absoluteFilePath())})) {
            return info.absoluteFilePath();
        }
#endif
        QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
        return info.absolutePath();
    }
    const QFileInfo folder(info.absolutePath());
    if (!folder.isDir()) return {};
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder.absoluteFilePath()));
    return folder.absoluteFilePath();
}

}  // namespace

std::set<std::string> AppActions::listState(MainWindow& w) {
    std::set<std::string> s = state(w);
    if (w.document_) {
        const std::set<std::string> groups = w.guidesPanel_->menuState();
        const std::set<std::string> maps = w.maps_->menuState();
        s.insert(groups.begin(), groups.end());
        s.insert(maps.begin(), maps.end());
        if (!w.correctives_->currentLayer().empty()) s.insert("corrective");
    }
    return s;
}

void AppActions::addListBindings(ui::Bindings& b, MainWindow& w) {
    MainWindow* m = &w;
    auto run = [](std::function<void()> fn) { return [fn = std::move(fn)](const json&, bool) { fn(); }; };
    auto groomOpen = [m] { return m->document_ != nullptr; };
    auto has = [m](const char* condition) { return [m, condition] { return m->document_ && listState(*m).count(condition) > 0; }; };
    auto group = [m] { return m->guidesPanel_->currentGroup(); };

    // ---- Guide groups (lists.groups) -----------------------------------------------------------
    // + New Group: its name over the viewport, top-level or under the picked group; the brushes'
    // group from then on (Place and Draw add to it).
    b["group.new"] = {run([m, group] {
                          if (!m->document_) return;
                          const std::string parent = group();
                          std::string suggestion = "group";
                          if (const HierarchyPtr h = m->document_->session().authoredHierarchy()) {
                              for (int k = 1; h->findGroup(suggestion) != kNoGroup; ++k) suggestion = "group" + std::to_string(k);
                          }
                          auto add = [m](const std::string& under, const QString& name) {
                              if (!m->document_ || name.isEmpty()) return;
                              if (!m->guidesPanel_->addGroup(under, name.toStdString())) return;
                              const HierarchyPtr h = m->document_->session().authoredHierarchy();
                              BrushSettings& brush = m->brush_.settings().brush;
                              brush.group = name.toStdString();
                              brush.level = h ? h->groupDepth(h->findGroup(brush.group)) : 0;
                              m->guidesEdited();
                              m->notify(QObject::tr("New guides go into %1.").arg(name));
                          };
                          std::vector<OverlayHost::TextChoice> choices;
                          if (!parent.empty()) {
                              choices.emplace_back(QObject::tr("Under %1").arg(qs(parent)), [add, parent](const QString& name) { add(parent, name); });
                          }
                          choices.emplace_back(QObject::tr("Add"), [add](const QString& name) { add({}, name); });
                          m->host_->showTextPrompt(QObject::tr("New Group"),
                                                   parent.empty() ? QObject::tr("A new top-level guide group:")
                                                                  : QObject::tr("A new guide group: top-level (Add), or a child group of %1:").arg(qs(parent)),
                                                   qs(suggestion), std::move(choices));
                      }),
                      {}, groomOpen};
    b["group.rename"] = {run([m] { m->guidesPanel_->editCurrentName(); }), {}, has("group")};
    // Color...: Qt's color dialog, non-modal (the rest stays usable), live while it is open.
    b["group.color"] = {run([m, group] {
                            const std::string name = group();
                            if (!m->document_ || name.empty()) return;
                            if (!m->interactive_) {
                                m->report(QObject::tr("Color...: the color is picked in a dialog (an interactive window); scripts set it "
                                                      "with GuidesPanel::setGroupColor"));
                                return;
                            }
                            const auto own = m->document_->groupColors().find(name);
                            auto* dialog = new QColorDialog(QColor(qs(own != m->document_->groupColors().end() ? own->second : std::string("#3d7eff"))), m);
                            dialog->setAttribute(Qt::WA_DeleteOnClose);
                            dialog->setOption(QColorDialog::DontUseNativeDialog, true);
                            dialog->setWindowTitle(QObject::tr("Color of %1").arg(qs(name)));
                            QObject::connect(dialog, &QColorDialog::colorSelected, m, [m, name](const QColor& c) {
                                m->guidesPanel_->setGroupColor(name, c.name().toStdString());
                                m->refreshUndoActions();
                            });
                            dialog->show();
                        }),
                        {}, has("group")};
    b["group.defaultColor"] = {run([m, group] {
                                   m->guidesPanel_->setGroupColor(group(), {});
                                   m->refreshUndoActions();
                               }),
                               {}, has("group.color")};
    b["group.select"] = {run([m, group] {
                             const std::string name = group();
                             if (!m->document_ || name.empty()) return;
                             m->guidesPanel_->selectGroup(name);
                             m->notify(QObject::tr("Selected %1 guides of %2.").arg(m->document_->selection().size()).arg(qs(name)));
                         }),
                         {}, has("group")};
    b["group.resetSegments"] = {run([m, group] {
                                    const std::string name = group();
                                    if (!m->document_ || name.empty()) return;
                                    m->guidesPanel_->resetGroupSegments(name);
                                    m->refreshUndoActions();
                                }),
                                {}, has("group")};
    b["group.delete"] = {run([m, group] {
                             const std::string name = group();
                             if (!m->document_ || name.empty()) return;
                             QString text = QObject::tr("Delete the group %1 with its child groups and guides?").arg(qs(name));
                             QStringList users;
                             for (const auto& [node, param] : m->document_->groupUsers(name)) users << QStringLiteral("%1 (%2)").arg(qs(node), qs(param));
                             if (!users.isEmpty()) text += QObject::tr("\n\nThe graph names it: %1.").arg(users.join(QStringLiteral(", ")));
                             m->host_->showPrompt(QObject::tr("Delete Group"), text,
                                                  {{QObject::tr("Cancel"), false, {}}, {QObject::tr("Delete"), true, [m, name] {
                                                        if (m->guidesPanel_->removeGroup(name)) m->guidesEdited();
                                                    }}});
                         }),
                         {}, has("group")};
    // Promote / Demote...: one level up, or down under a group beside it.
    b["group.promoteDemote"] = {run([m, group] {
                                    const std::string name = group();
                                    if (!m->document_ || name.empty()) {
                                        m->report(QObject::tr("Pick a group to move first"));
                                        return;
                                    }
                                    const HierarchyPtr h = m->document_->session().authoredHierarchy();
                                    const int depth = h ? h->groupDepth(h->findGroup(name)) : 0;
                                    std::vector<OverlayHost::PromptChoice> choices{{QObject::tr("Cancel"), false, {}}};
                                    const std::vector<std::string> siblings = m->guidesPanel_->siblingsOf(name);
                                    for (std::size_t i = 0; i < siblings.size() && i < 4; ++i) {
                                        const std::string under = siblings[i];
                                        choices.push_back({QObject::tr("Under %1").arg(qs(under)), false, [m, name, under] {
                                                               if (m->guidesPanel_->demoteGroup(name, under)) m->guidesEdited();
                                                           }});
                                    }
                                    if (depth > 0) {
                                        choices.push_back({QObject::tr("Promote"), true, [m, name] {
                                                               if (m->guidesPanel_->promoteGroup(name)) m->guidesEdited();
                                                           }});
                                    }
                                    if (choices.size() == 1) {
                                        m->report(QObject::tr("%1 is a root group with no group beside it: nothing to move it up or under").arg(qs(name)));
                                        return;
                                    }
                                    m->host_->showPrompt(QObject::tr("Promote / Demote Group"),
                                                         QObject::tr("Move %1 one level up (Promote), or down under a group beside it (its guides follow the "
                                                                     "nearest guide of that group)?")
                                                             .arg(qs(name)),
                                                         std::move(choices));
                                }),
                                {}, groomOpen};
    b["guides.children"] = {run([m, group] {
                                if (m->guidesPanel_->generateChildren(group(), m->guidesPanel_->childrenCount())) m->guidesEdited();
                            }),
                            {}, groomOpen};
    b["guides.syncPos"] = {run([m] {
                               if (m->guidesPanel_->symmetrySync(true)) m->guidesEdited();
                           }),
                           {}, groomOpen};
    b["guides.syncNeg"] = {run([m] {
                               if (m->guidesPanel_->symmetrySync(false)) m->guidesEdited();
                           }),
                           {}, groomOpen};
    b["guides.mirrorSelection"] = {run([m] { m->guidesPanel_->mirrorSelection(); }), {}, groomOpen};
    // Mirror Group...: the picked group's root group, from the side asked.
    b["guides.mirrorGroup"] = {run([m, group] {
                                   if (!m->document_) return;
                                   std::string root = m->guidesPanel_->rootOf(group());
                                   const HierarchyPtr h = m->document_->session().authoredHierarchy();
                                   if (root.empty() && h && !h->groups().empty()) root = h->groups().front().name;
                                   if (root.empty()) {
                                       m->report(QObject::tr("The groom has no guide groups to mirror"));
                                       return;
                                   }
                                   auto mirror = [m, root](bool fromPositive) {
                                       return [m, root, fromPositive] {
                                           if (m->guidesPanel_->mirrorGroup(root, fromPositive)) m->guidesEdited();
                                       };
                                   };
                                   m->host_->showPrompt(QObject::tr("Mirror Group"),
                                                        QObject::tr("Copy the root group %1 from one side of x = 0 over the other (its children follow):").arg(qs(root)),
                                                        {{QObject::tr("Cancel"), false, {}},
                                                         {QObject::tr("-X → +X"), false, mirror(false)},
                                                         {QObject::tr("+X → -X"), true, mirror(true)}});
                               }),
                               {}, groomOpen};
    // Reparent...: the new parent guide's ID asked (a guide of the children's group's parent group).
    b["guides.reparent"] = {run([m] {
                                if (!m->document_) return;
                                const std::size_t n = m->document_->selection().size();
                                if (n == 0) {
                                    m->report(QObject::tr("No guides selected: select the children to move first"));
                                    return;
                                }
                                m->host_->showTextPrompt(QObject::tr("Reparent"),
                                                         QObject::tr("Move the %1 selected guides under the guide with this ID (a guide of their group's "
                                                                     "parent group):")
                                                             .arg(n),
                                                         QString(), QObject::tr("Reparent"), [m](const QString& text) {
                                                             bool ok = false;
                                                             const qulonglong id = text.toULongLong(&ok);
                                                             if (!ok) {
                                                                 m->report(QObject::tr("A guide ID is a whole number, not '%1'").arg(text));
                                                                 return;
                                                             }
                                                             if (m->guidesPanel_->reparentSelected(static_cast<GuideId>(id))) m->guidesEdited();
                                                         });
                            }),
                            {}, groomOpen};
    b["guides.clearSelection"] = {run([m] { m->guidesPanel_->clearSelection(); }), {}, groomOpen};

    // ---- Maps (lists.maps) ----------------------------------------------------------------------
    auto mapDone = [m](const std::string& error) {
        if (!error.empty()) m->report(qs(error));
        return error.empty();
    };
    b["map.new"] = {run([m] {
                        if (!m->document_) return;
                        m->groomTools_->showTab(QStringLiteral("paint"));
                        m->maps_->setNewMapFormShown(!m->maps_->newMapFormShown());
                    }),
                    {}, groomOpen};
    b["map.import"] = {run([m, mapDone] {
                           if (!m->document_ || !m->interactive_) return;
                           const QString folder = m->document_->path().empty() ? QString() : QFileInfo(qs(m->document_->path())).absolutePath();
                           const QString path = QFileDialog::getOpenFileName(m, QObject::tr("Import Image as Map"), folder, kImageFilter);
                           if (path.isEmpty()) return;
                           if (mapDone(m->maps_->importImage(path))) m->notify(QObject::tr("%1 reads %2").arg(qs(m->maps_->selectedMap()), path));
                       }),
                       {}, groomOpen};
    b["map.rename"] = {run([m] { m->maps_->editSelectedName(); }), {}, has("map")};
    // Convert to (the mapKind choices): a texture's resolution asked first.
    b["map.convert"] = {[m, mapDone](const json& value, bool) {
                            if (!m->document_ || !value.is_string()) return;
                            const std::string kind = value.get<std::string>();
                            const std::string name = m->maps_->selectedMap();
                            if (name.empty()) return;
                            if (m->maps_->selectedKind() == kind) {
                                m->notify(QObject::tr("%1 lives there already").arg(qs(name)));
                                return;
                            }
                            if (kind != "texture") {
                                mapDone(m->maps_->convertSelected(kind));
                                return;
                            }
                            std::vector<OverlayHost::PromptChoice> choices{{QObject::tr("Cancel"), false, {}}};
                            for (const json& c : ui::Definitions::shipped().choices("mapResolution")) {
                                const int size = c.value("value", 1024);
                                choices.push_back({qs(c.value("label", std::to_string(size))), size == 1024, [m, mapDone, size] {
                                                       mapDone(m->maps_->convertSelected("texture", size));
                                                   }});
                            }
                            m->host_->showPrompt(QObject::tr("Convert to Texture"), QObject::tr("%1 as an image at the scalp's UVs, this size:").arg(qs(name)),
                                                 std::move(choices));
                        },
                        {}, has("map")};
    b["map.makeGray"] = {run([m, mapDone] { mapDone(m->maps_->setSelectedChannels(kGrayMap)); }), {}, has("map.scalp")};
    b["map.makeColor"] = {run([m, mapDone] { mapDone(m->maps_->setSelectedChannels(kColorMap)); }), {}, has("map.scalp")};
    b["map.relink"] = {run([m, mapDone] {
                           if (!m->document_ || !m->interactive_) return;
                           const std::string name = m->maps_->selectedMap();
                           if (name.empty()) return;
                           QString start = qs(m->maps_->selectedMissing());
                           if (start.isEmpty()) start = qs(m->maps_->selectedFile());
                           QString folder = start.isEmpty() ? QString() : QFileInfo(start).absolutePath();
                           if (folder.isEmpty() || !QFileInfo(folder).isDir()) {
                               folder = m->document_->path().empty() ? QString() : QFileInfo(qs(m->document_->path())).absolutePath();
                           }
                           const QString path = QFileDialog::getOpenFileName(m, QObject::tr("Relink Image: %1").arg(qs(name)), folder, kImageFilter);
                           if (path.isEmpty()) return;
                           if (mapDone(m->maps_->relinkSelected(path))) m->notify(QObject::tr("%1 reads %2").arg(qs(name), path));
                       }),
                       {}, has("map")};
    b["map.reloadImage"] = {run([m, mapDone] {
                                const std::string name = m->maps_->selectedMap();
                                if (!mapDone(m->maps_->reloadSelected())) return;
                                const std::string missing = m->maps_->selectedMissing();
                                m->notify(missing.empty() ? QObject::tr("%1: image reloaded").arg(qs(name))
                                                          : QObject::tr("%1: the image is missing (%2)").arg(qs(name), qs(missing)));
                            }),
                            {}, has("map.texture")};
    b["map.showFile"] = {run([m] {
                             const std::string file = m->maps_->selectedFile();
                             if (file.empty()) return;
                             if (revealFile(qs(file)).isEmpty()) m->report(QObject::tr("Neither the image nor its folder is there: %1").arg(qs(file)));
                         }),
                         {}, has("map.texture")};
    // Delete: asked first when the graph reads the map (its readers go back to their own values).
    b["map.delete"] = {run([m, mapDone] {
                           const std::string name = m->maps_->selectedMap();
                           if (!m->document_ || name.empty()) return;
                           QStringList users;
                           for (const auto& [node, param] : m->document_->mapUsers(name)) users << qs(node) + QLatin1Char('.') + qs(param);
                           if (users.isEmpty()) {
                               mapDone(m->maps_->removeSelected());
                               return;
                           }
                           m->host_->showPrompt(QObject::tr("Delete Map"),
                                                QObject::tr("Delete %1? %2 go back to %3 own value (undo brings the map back).")
                                                    .arg(qs(name), users.join(QStringLiteral(", ")),
                                                         users.size() > 1 ? QObject::tr("their") : QObject::tr("its")),
                                                {{QObject::tr("Cancel"), false, {}}, {QObject::tr("Delete"), true, [m, mapDone, name] {
                                                      m->maps_->selectMap(name);
                                                      mapDone(m->maps_->removeSelected());
                                                  }}});
                       }),
                       {}, has("map")};
    // Delete Its File Too: on unless turned off (a saved setting, as Maya's optionVar).
    b["map.deleteFiles"] = {[m](const json&, bool on) {
                                m->maps_->setDeleteFiles(on);
                                if (m->interactive_) QSettings().setValue(QStringLiteral("maps/deleteFiles"), on);
                            },
                            [m](const json&) { return m->maps_->deleteFiles(); }};

    // ---- Correctives (lists.correctives) -------------------------------------------------------
    auto layer = [m] { return m->correctives_->currentLayer(); };
    b["corrective.new"] = {run([m] { m->correctives_->addLayer(true); }), {}, groomOpen};
    b["corrective.fixFrame"] = {run([m] { m->correctives_->addLayer(false, 6.0); }), {}, groomOpen};
    b["corrective.up"] = {run([m, layer] { m->correctives_->moveLayer(layer(), 1); }), {}, has("corrective")};
    b["corrective.down"] = {run([m, layer] { m->correctives_->moveLayer(layer(), -1); }), {}, has("corrective")};
    b["corrective.solo"] = {run([m, layer] { m->correctives_->soloLayer(layer()); }), {}, has("corrective")};
    b["corrective.delete"] = {run([m, layer] { m->correctives_->removeLayer(layer()); }), {}, has("corrective")};
    b["corrective.goTo"] = {run([m, layer] {
                                const std::string name = layer();
                                if (name.empty()) return;
                                const std::optional<double> frame = m->correctives_->layerFrame(name);
                                if (!frame) {
                                    m->notify(QObject::tr("%1 is on every frame").arg(qs(name)));
                                    return;
                                }
                                m->setFrame(*frame);
                            }),
                            {}, has("corrective")};
    b["corrective.keyWeight"] = {run([m, layer] { m->correctives_->keyWeight(layer()); }), {}, has("corrective")};
    b["corrective.clearKeys"] = {run([m, layer] { m->correctives_->clearKeys(layer()); }), {}, has("corrective")};
    b["corrective.rename"] = {run([m] { m->correctives_->editCurrentName(); }), {}, has("corrective")};
}

}  // namespace groomlab::app
