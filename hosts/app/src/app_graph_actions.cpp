// SPDX-License-Identifier: Apache-2.0
// The Graph Editor's actions and menus from the shared definitions (engine/ui/graph_ui.json), as
// Maya's Graph Editor builds them (hosts/maya/scripts/groomlab/ui/graph_editor.py: GraphEditor's
// ACTIONS and edit_generators, NodeItem's ACTIONS, menu_state and menu_generators, ParamPanel's
// param_actions): the dock's Add Node, Presets and Edit menus, the right-click menus of a node, a
// backdrop, the canvas and a param, and what each id runs in the app (app_actions.h).

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>

#include <QAction>
#include <QActionGroup>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QMenu>
#include <QTimer>
#include <QSettings>
#include <QToolButton>
#include <QUrl>

#include "app_actions.h"
#include "app_shell.h"
#include "correctives_panel.h"
#include "groom_document.h"
#include "groom_tools.h"
#include "main_window.h"
#include "node_graph.h"
#include "overlay.h"
#include "param_panel.h"
#include "theme.h"
#include "tube_panel.h"

#include "groomlab/node_catalog.h"
#include "groomlab/node_library.h"

namespace groomlab::app {

namespace {

using ui::json;

QString qs(const std::string& s) { return QString::fromStdString(s); }

// Where a right-click menu was asked in the graph (view coordinates; NaN: none, the bar's menus):
// New Script Node, Import Node, Add Backdrop and the node categories add there.
QPointF& menuPoint() {
    static QPointF at(std::numeric_limits<qreal>::quiet_NaN(), 0.0);
    return at;
}

// The node and param a param's menu was filled for (its param.* actions act on them).
std::pair<std::string, std::string>& paramTarget() {
    static std::pair<std::string, std::string> target;
    return target;
}

QString typeLabel(const json& spec, const std::string& type) {
    const std::string label = spec.value("label", std::string());
    return qs(label.empty() ? groomlab::nodeLabel(type) : label);
}

}  // namespace

// ---- Bindings ---------------------------------------------------------------------------------------

void AppActions::addGraphBindings(ui::Bindings& b, MainWindow& w) {
    MainWindow* m = &w;
    auto run = [](std::function<void()> fn) { return [fn = std::move(fn)](const json&, bool) { fn(); }; };
    auto groomOpen = [m] { return m->document_ != nullptr; };
    auto selected = [m] { return m->graph_->selectedNode(); };
    auto nodeOn = [m] { return m->document_ != nullptr && !m->graph_->selectedNode().empty(); };
    auto str = [](const json& value) { return value.is_string() ? value.get<std::string>() : std::string(); };

    // The node menu (graph_ui.json's nodeMenu): each acts on the node right-clicked (selected).
    // Brushes shape this Sculpt node's guides (the Correctives' target follows), or the base groom.
    b["node.brushSculpt"] = {[m, selected](const json&, bool on) {
                                 if (m->document_) m->correctives_->setTargetNode(on ? selected() : std::string());
                             },
                             [m, selected](const json&) { return m->document_ && !selected().empty() && m->document_->sculptTarget() == selected(); },
                             nodeOn};
    // A Sculpt node's strokes dropped (one undo step); a value names the node (a list's row).
    b["sculpt.clearEdits"] = {[m, selected, str](const json& value, bool) {
                                  const std::string node = value.is_string() ? str(value) : selected();
                                  if (!m->document_ || node.empty()) return;
                                  const std::string error = m->document_->clearSculptEdits(node);
                                  if (!error.empty()) m->report(qs(error));
                                  m->guidesEdited();
                              },
                              {}, nodeOn};
    b["node.bake"] = {run([m, selected] { askBakeNode(*m, selected()); }), {}, nodeOn};
    // GroomLab > Utilities > Bake Node to Guides...: the graph's selected node, else the output.
    b["node.bakeToGuides"] = {run([m, selected] {
                                  if (!m->document_) return;
                                  std::string node = m->graphShown() ? selected() : std::string();
                                  if (node.empty() || !nodeState(*m, node).count("node.bakeable")) {
                                      node = m->document_->graph().value("output", std::string());
                                  }
                                  askBakeNode(*m, node);
                              }),
                              {}, groomOpen};
    // Expose Param as Input: the param (the generator's value) an input port, or not.
    b["node.expose"] = {[m, selected, str](const json& value, bool on) {
                            if (!m->document_ || selected().empty()) return;
                            const std::string error = m->document_->exposeParam(selected(), str(value), on);
                            if (!error.empty()) m->report(qs(error));
                            m->graphEdited();
                        },
                        [m, selected, str](const json& value) { return m->document_ && m->document_->paramExposed(selected(), str(value)); },
                        nodeOn};
    b["node.update"] = {run([m, selected] { m->graph_->updateLibraryNode(selected()); }), {}, nodeOn};
    b["script.edit"] = {run([m, selected] { m->graph_->openScriptEditor(selected()); }), {}, nodeOn};
    b["script.saveToLibrary"] = {run([m, selected] {
                                     const std::string node = selected();
                                     m->graph_->openScriptEditor(node);
                                     const QString error = m->graph_->saveScriptEditor(false);
                                     if (!error.isEmpty()) m->report(error);
                                 }),
                                 {}, nodeOn};
    // Show Inner Param: a compound's inner param (the generator's "node.param") on the compound.
    b["compound.showInner"] = {[m, selected, str](const json& value, bool on) {
                                   if (!m->document_ || selected().empty()) return;
                                   const std::string error = m->document_->showInnerParam(selected(), str(value), on);
                                   if (!error.empty()) m->report(qs(error));
                                   m->graphEdited();
                               },
                               [m, selected, str](const json& value) {
                                   if (!m->document_) return false;
                                   const std::vector<std::string> shown = m->document_->shownInnerParams(selected());
                                   return std::find(shown.begin(), shown.end(), str(value)) != shown.end();
                               },
                               nodeOn};

    // The backdrop menu: the backdrop right-clicked (selected).
    auto backdropOn = [m] { return m->document_ != nullptr && m->graph_->selectedBackdrop() >= 0; };
    b["backdrop.rename"] = {run([m] { m->graph_->renameBackdrop(m->graph_->selectedBackdrop()); }), {}, backdropOn};
    b["backdrop.color"] = {[m](const json& value, bool) {
                               if (!value.is_array() || value.size() != 3) return;
                               m->graph_->setBackdropColor(m->graph_->selectedBackdrop(),
                                                           QColor(value[0].get<int>(), value[1].get<int>(), value[2].get<int>()));
                           },
                           [m](const json& value) {
                               if (!value.is_array() || value.size() != 3) return false;
                               return m->graph_->backdropColor(m->graph_->selectedBackdrop()) ==
                                      QColor(value[0].get<int>(), value[1].get<int>(), value[2].get<int>());
                           },
                           backdropOn};
    b["backdrop.delete"] = {run([m] { m->graph_->removeBackdrop(m->graph_->selectedBackdrop()); }), {}, backdropOn};
    // Add Backdrop: around the selection, else where the canvas was right-clicked.
    b["backdrop.add"] = {run([m] { m->graph_->addBackdrop(menuPoint()); }), {}, groomOpen};

    // The Add Node menu's library entries (graph_ui.json's addMenu).
    b["node.newScript"] = {run([m] {
                               if (!m->graphShown()) m->setGraphShown(true);
                               m->graph_->newScriptNode(menuPoint());
                           }),
                           {}, groomOpen};
    // Import Node...: the file dialog only on a click (scripts and tests call importNodeFiles).
    b["node.import"] = {run([m] {
                            const QPointF at = menuPoint();
                            const QStringList paths = QFileDialog::getOpenFileNames(m, QObject::tr("Import Node"), {}, QObject::tr("GroomLab nodes (*.glnode)"));
                            if (!paths.isEmpty()) m->graph_->importNodeFiles(paths, at);
                        }),
                        {}, groomOpen};
    b["node.openFolder"] = {run([m] {
        const QString folder = qs(nodeUserDir());
        if (folder.isEmpty()) {
            m->report(QObject::tr("No node folder: no home folder is known"));
            return;
        }
        QDir().mkpath(folder);
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    })};

    // A param's menu (graph_ui.json's paramMenu and paintMenu): the node and param it was filled for.
    auto paramOn = [m] { return m->document_ != nullptr && !paramTarget().first.empty(); };
    b["param.expose"] = {[m](const json&, bool on) {
                             if (!m->document_) return;
                             const auto [node, param] = paramTarget();
                             const std::string error = m->document_->exposeParam(node, param, on);
                             if (!error.empty()) m->report(qs(error));
                             // Not under the menu's own click: the panels are built again after it.
                             QTimer::singleShot(0, m, [m] { m->graphEdited(); });
                         },
                         [m](const json&) { return m->document_ && m->document_->paramExposed(paramTarget().first, paramTarget().second); },
                         paramOn};
    for (const auto& [id, kind] : {std::pair<const char*, const char*>{"param.paintVertices", "vertices"}, {"param.paintTexture", "texture"}}) {
        b[id] = {run([m, kind = QString::fromLatin1(kind)] {
                     const auto [node, param] = paramTarget();
                     QTimer::singleShot(0, m, [m, node = qs(node), param = qs(param), kind] { m->paintParam(node, param, kind); });
                 }),
                 {}, paramOn};
    }
    // Drive with Slider: a Control node feeding the param (its value the slider; selected after).
    b["param.driveWithSlider"] = {run([m] {
                                      if (!m->document_) return;
                                      const auto [node, param] = paramTarget();
                                      std::string made;
                                      const std::string error = m->document_->addControl(node, param, &made);
                                      if (!error.empty()) {
                                          m->report(qs(error));
                                          return;
                                      }
                                      m->shell_->setGraphStatus(QObject::tr("%1.%2 is driven by %3: its Default is the slider")
                                                                    .arg(qs(node), qs(param), qs(made)));
                                      QTimer::singleShot(0, m, [m, made] {
                                          m->graphEdited();
                                          if (m->graphShown()) {
                                              m->graph_->refresh();
                                              m->graph_->selectNode(made);
                                          }
                                      });
                                  }),
                                  {}, paramOn};
}

// ---- Generators (entries) and fillers (menus made here) ----------------------------------------------

void AppActions::addGraphGenerators(ui::Generators& g, MainWindow& w) {
    MainWindow* m = &w;
    // Expose Param as Input: the node's linkable params (those that take an input port).
    g["exposeParams"] = [m](const json& entry) {
        json out = json::array();
        const std::string node = m->graph_->selectedNode();
        const json* spec = m->document_ && !node.empty() ? m->document_->nodeSpec(node) : nullptr;
        if (!spec) return out;
        for (const json& p : spec->value("params", json::array())) {
            if (!p.contains("port") || !p["port"].is_object()) continue;
            const std::string name = p.value("name", std::string());
            out.push_back({{"id", entry.value("action", std::string("node.expose"))}, {"value", name}, {"label", name}, {"tip", p.value("description", std::string())}});
        }
        return out;
    };
    // Show Inner Param: a submenu per inner node of the compound, its params (Maya's
    // menu_generators: numbers, switches, vectors and words, not a channel or the definition).
    g["innerParams"] = [m](const json& entry) {
        json out = json::array();
        const std::string node = m->graph_->selectedNode();
        if (!m->document_ || node.empty()) return out;
        json found;
        const json& graph = m->document_->graph();
        if (graph.is_object() && graph.contains("nodes") && graph["nodes"].is_array()) {
            for (const json& n : graph["nodes"]) {
                if (n.is_object() && n.value("name", std::string()) == node) found = n;
            }
        }
        const json* compound = &found;
        if (!found.is_object() || compound->value("type", std::string()) != "Compound") return out;
        const json params = compound->value("params", json::object());
        const json definition = params.is_object() ? params.value("definition", json::object()) : json::object();
        const json inner = definition.is_object() ? definition.value("graph", json::object()) : json::object();
        for (const json& n : inner.is_object() ? inner.value("nodes", json::array()) : json::array()) {
            const std::string type = n.value("type", std::string()), name = n.value("name", std::string());
            const json* spec = m->document_->spec(type);
            if (!spec || type == "Input") continue;
            json items = json::array();
            for (const json& p : spec->value("params", json::array())) {
                const std::string kind = p.value("kind", std::string()), param = p.value("name", std::string());
                if (kind != "float" && kind != "int" && kind != "bool" && kind != "vec3" && kind != "string") continue;
                // (Not a param naming a channel: its "channel" set, a word or true.)
                const json channel = p.contains("channel") ? p["channel"] : json();
                const bool names = channel.is_boolean() ? channel.get<bool>() : channel.is_string() ? !channel.get<std::string>().empty() : !channel.is_null();
                if (param == "definition" || names) continue;
                items.push_back({{"id", entry.value("action", std::string("compound.showInner"))},
                                 {"value", name + "." + param},
                                 {"label", param},
                                 {"tip", p.value("description", std::string())}});
            }
            if (!items.empty()) out.push_back({{"submenu", name}, {"id", "inner/" + name}, {"items", items}});
        }
        return out;
    };
}

void AppActions::addGraphFillers(std::map<std::string, ui::MenuFiller>& f, MainWindow& w) {
    MainWindow* m = &w;
    // Add Node: a submenu per category in the menu's order, each type by its label, its
    // description the tip (Maya's node_categories); added where the canvas was right-clicked,
    // else after the selected node.
    f["nodeCategories"] = [m](QMenu* menu, const ui::Item&) {
        if (!m->document_) return;
        m->document_->refreshLibrary();  // nodes saved or dropped in since
        std::vector<std::tuple<int, int, std::string>> types;
        for (const std::string& type : m->document_->operatorTypes()) {
            const json* spec = m->document_->spec(type);
            if (!spec || spec->value("hidden", false)) continue;
            types.emplace_back(spec->value("categoryOrder", std::numeric_limits<int>::max()), spec->value("order", std::numeric_limits<int>::max()), type);
        }
        std::sort(types.begin(), types.end());
        std::map<std::string, QMenu*> menus;
        for (const auto& [category, order, type] : types) {
            const json& spec = *m->document_->spec(type);
            const std::string title = spec.value("categoryTitle", std::string("Other"));
            QMenu*& sub = menus[title];
            if (!sub) {
                sub = new QMenu(qs(title).replace(QLatin1Char('&'), QStringLiteral("&&")), menu);
                sub->setObjectName(QStringLiteral("addCategory_") + qs(title));
                sub->setToolTipsVisible(true);
                menu->addMenu(sub);
            }
            QAction* a = sub->addAction(typeLabel(spec, type));
            a->setData(qs(type));
            a->setToolTip(qs(spec.value("description", std::string())));
            QObject::connect(a, &QAction::triggered, m, [m, type = type] {
                if (!m->graphShown()) m->setGraphShown(true);
                const QPointF at = menuPoint();
                const bool placed = !std::isnan(at.x());
                m->graph_->addNodeOfType(type, placed ? m->graph_->sceneAt(at) : at, !placed);
            });
        }
    };
    // Edit > Units: lengths, widths and densities in real units (or scene units), saved per user.
    f["units"] = [m](QMenu* menu, const ui::Item&) {
        auto* units = new QMenu(QObject::tr("Units"), menu);
        units->setObjectName(QStringLiteral("unitsMenu"));
        units->menuAction()->setToolTip(QObject::tr("Show lengths, widths and densities in real units (the groom's scene units are centimeters)"));
        menu->addMenu(units);
        auto fill = [m, units] {
            units->clear();
            for (const auto& [kind, title] : {std::pair<const char*, QString>{"length", QObject::tr("Lengths")},
                                              {"width", QObject::tr("Widths")},
                                              {"density", QObject::tr("Density")}}) {
                QMenu* sub = units->addMenu(title);
                sub->setObjectName(QStringLiteral("units_") + QLatin1String(kind));
                auto* group = new QActionGroup(sub);
                const std::string current = unitChoice(kind);
                for (const UnitChoice& c : unitChoices(kind)) {
                    QAction* a = sub->addAction(c.key == "scene" ? QObject::tr("Scene units") : qs(c.label).trimmed());
                    a->setObjectName(QStringLiteral("unit_%1_%2").arg(QLatin1String(kind), qs(c.key)));
                    a->setCheckable(true);
                    a->setChecked(c.key == current);
                    group->addAction(a);
                    QObject::connect(a, &QAction::triggered, m, [m, kind = std::string(kind), key = c.key] {
                        setUnitChoice(kind, key);
                        m->graphParams_->refresh();
                        m->params_->refresh();
                        m->groomTools_->tubePanel()->refreshUnits();  // (Tubes' Width in the lengths' unit)
                    });
                }
            }
        };
        fill();
        QObject::connect(units, &QMenu::aboutToShow, units, fill);
    };
}

// ---- Conditions -------------------------------------------------------------------------------------

std::set<std::string> AppActions::nodeState(MainWindow& w, const std::string& node) {
    std::set<std::string> s = state(w);
    if (!w.document_) return s;
    std::string type;
    for (const GroomDocument::NodeInfo& n : w.document_->nodes()) {
        if (n.name == node) type = n.type;
    }
    if (type.empty()) return s;
    const json* spec = w.document_->nodeSpec(node);
    bool strands = false, mappable = false;
    if (spec) {
        for (const json& p : spec->value("outputs", json::array())) {
            const std::string t = p.value("type", std::string());
            strands = strands || t == "Strands" || t == "Guides";
        }
        for (const json& p : spec->value("params", json::array())) mappable = mappable || (p.contains("port") && p["port"].is_object());
    }
    if (strands && type != "Output") s.insert({"node.viewable", "node.bakeable"});
    s.insert("type." + type);  // (graph_ui.json's nodeActions: by the node's type)
    if (type == "Sculpt") s.insert("node.sculpt");
    if (mappable) s.insert("node.mappable");
    if (type == "Script") s.insert("node.script");
    if (type == "Compound") s.insert("node.compound");
    const json library = w.document_->libraryInfo(node);
    if (library.is_object()) {
        s.insert("node.library");
        if (library.value("updateAvailable", false)) s.insert("node.updateAvailable");
    }
    return s;
}

std::set<std::string> AppActions::paramState(MainWindow& w, const std::string& node, const std::string& param) {
    std::set<std::string> s = state(w);
    const json* spec = w.document_ ? w.document_->nodeSpec(node) : nullptr;
    if (!spec) return s;
    for (const json& p : spec->value("params", json::array())) {
        if (p.value("name", std::string()) != param) continue;
        if (p.contains("port") && p["port"].is_object()) {
            s.insert("param.linkable");
            if (p.value("mappable", false)) s.insert("param.mappable");
        }
    }
    return s;
}

// ---- The menus --------------------------------------------------------------------------------------

json AppActions::canvasMenu() {
    return json::array({{{"include", "graph/addMenu"}}, "-", "backdrop.add", "graph.tidy", "graph.frameAll"});
}

void AppActions::fillParamMenu(MainWindow& w, QMenu* menu, const std::string& node, const std::string& param) {
    paramTarget() = {node, param};
    ui::FillOptions options = fillOptions(w);
    options.resolve.state = paramState(w, node, param);
    options.liveShortcuts = false;
    ui::fillMenu(menu, ui::Definitions::shipped().get("graph/paramMenu"), bindings(w), options);
}

std::vector<ParamPanel::NodeAction> AppActions::nodeActions(MainWindow& w, const std::string& node) {
    MainWindow* m = &w;
    std::vector<ParamPanel::NodeAction> out;
    const ui::Definitions& d = ui::Definitions::shipped();
    const ui::json* entries = d.find("graph/nodeActions");
    if (!entries || !m->document_ || node.empty()) return out;
    const ui::Bindings b = bindings(w);
    ui::ResolveOptions options;
    options.state = nodeState(w, node);
    options.keep = [&b](const std::string& id) { return b.count(id) > 0; };
    options.dropGenerators = true;
    for (const ui::Item& item : d.resolve(*entries, options)) {
        if (item.kind != ui::Item::Kind::Action) continue;
        const ui::Binding binding = b.at(item.id);
        ParamPanel::NodeAction a;
        a.id = item.id;
        a.label = qs(item.label);
        a.tip = qs(item.tip);
        a.enabled = item.enabled;
        // On this node: the bindings act on the graph's selected one (the node list's panel shows
        // one the graph may not have selected).
        a.run = [m, node, binding, value = item.value] {
            if (m->graph_->selectedNode() != node) m->graph_->selectNode(node);
            if (binding.trigger) binding.trigger(value, false);
        };
        out.push_back(std::move(a));
    }
    return out;
}

void AppActions::askBakeNode(MainWindow& w, const std::string& node) {
    MainWindow* m = &w;
    if (!m->document_ || node.empty()) return;
    // The root group asked over the viewport (never a dialog): made when new, added to otherwise.
    m->host_->showTextPrompt(QObject::tr("Bake Node to Guides"),
                             QObject::tr("%1's strands become guides you can brush, in the root group:").arg(qs(node)), QStringLiteral("baked"),
                             QObject::tr("Bake"), [m, node](const QString& group) {
                                 if (!m->document_) return;
                                 std::size_t count = 0;
                                 const std::string error =
                                     m->document_->bakeNodeToGuides(node, group.trimmed().isEmpty() ? std::string("baked") : group.trimmed().toStdString(), 1.0f, &count);
                                 if (!error.empty()) {
                                     m->report(qs(error));
                                     return;
                                 }
                                 m->guidesEdited();
                                 m->notify(QObject::tr("Baked %1 guides from %2 into %3").arg(count).arg(qs(node), group.trimmed()));
                             });
}

void AppActions::wireGraphEditor(MainWindow& w) {
    MainWindow* m = &w;
    const ui::Definitions& d = ui::Definitions::shipped();
    // The bar's menus: kept, filled again as they open where they hold a generator (the node
    // types, the presets, the units). Their keys show after the labels: the graph's own keys run
    // them while it has the focus (no window shortcut takes Ctrl+G from the viewport).
    auto barMenu = [m, &d](const char* container, const char* name) {
        auto* menu = new QMenu(m);
        menu->setObjectName(QString::fromLatin1(name));
        ui::FillOptions options = fillOptions(*m);
        options.liveShortcuts = false;
        ui::fillMenu(menu, d.get(container), bindings(*m), options, d);
        return menu;
    };
    m->presetsMenu_ = barMenu("graph/presetMenu", "presetsMenu");
    m->presetsMenu_->setTitle(QObject::tr("Graph Presets"));
    QMenu* add = barMenu("graph/addMenu", "graphAddMenu");
    QMenu* edit = barMenu("graph/editMenu", "graphEditMenu");
    // (The bar's menus are for the bar alone: the canvas's and the nodes' are made per click.)
    QObject::connect(add, &QMenu::aboutToShow, add, [] { menuPoint() = QPointF(std::numeric_limits<qreal>::quiet_NaN(), 0.0); });
    m->shell_->graphAddButton()->setMenu(add);
    m->shell_->graphPresetsButton()->setMenu(m->presetsMenu_);
    m->shell_->graphEditButton()->setMenu(edit);
    // Snap, remembered between sessions (a test's app starts with it off).
    QToolButton* snap = m->shell_->graphSnapButton();
    if (m->interactive_) snap->setChecked(QSettings().value(QStringLiteral("graph/snap"), false).toBool());
    m->graph_->setSnapping(snap->isChecked());
    QObject::connect(snap, &QToolButton::toggled, snap, [m](bool on) {
        m->graph_->setSnapping(on);
        if (m->interactive_) QSettings().setValue(QStringLiteral("graph/snap"), on);
    });

    // The right-click menus of the graph: a node's, a backdrop's, the canvas's.
    m->graph_->setMenuFiller([m](QMenu* menu, NodeGraphView::MenuTarget target, const QPointF& at) {
        const ui::Definitions& d = ui::Definitions::shipped();
        ui::FillOptions options = fillOptions(*m);
        options.liveShortcuts = false;
        menuPoint() = target == NodeGraphView::MenuTarget::Canvas ? at : QPointF(std::numeric_limits<qreal>::quiet_NaN(), 0.0);
        QObject::connect(menu, &QObject::destroyed, [] { menuPoint() = QPointF(std::numeric_limits<qreal>::quiet_NaN(), 0.0); });
        switch (target) {
            case NodeGraphView::MenuTarget::Node: {
                const std::string node = m->graph_->selectedNode();
                options.resolve.state = nodeState(*m, node);
                const json library = m->document_->libraryInfo(node);
                if (library.is_object() && library.contains("latest")) options.resolve.values["latest"] = library["latest"];
                ui::fillMenu(menu, d.get("graph/nodeMenu"), bindings(*m), options, d);
                break;
            }
            case NodeGraphView::MenuTarget::Backdrop:
                ui::fillMenu(menu, d.get("graph/backdropMenu"), bindings(*m), options, d);
                break;
            case NodeGraphView::MenuTarget::Canvas: {
                ui::fillMenu(menu, canvasMenu(), bindings(*m), options, d);
                // The search first (Tab), where the menu was asked: on a wire, into it. Made again
                // when the menu refills itself as it opens.
                auto search = [m, menu, at] {
                    if (!menu->actions().isEmpty() && menu->actions().front()->objectName() == QStringLiteral("graphSearch")) return;
                    auto* a = new QAction(QObject::tr("Search Nodes...") + QStringLiteral("\tTab"), menu);
                    a->setObjectName(QStringLiteral("graphSearch"));
                    a->setToolTip(QObject::tr("Type to find a node (a name, or what it does) or a painted map; on a wire, it goes into the wire"));
                    QObject::connect(a, &QAction::triggered, m->graph_, [m, at] { m->graph_->openSearchAt(at); });
                    QAction* first = menu->actions().isEmpty() ? nullptr : menu->actions().front();
                    menu->insertAction(first, a);
                    if (first) menu->insertSeparator(first);
                };
                search();
                QObject::connect(menu, &QMenu::aboutToShow, menu, search);
                break;
            }
        }
    });
    // A param's right-click menu, in both param panels.
    for (ParamPanel* panel : {m->graphParams_, m->params_}) {
        panel->setParamMenuFiller([m](QMenu* menu, const std::string& node, const std::string& param) { fillParamMenu(*m, menu, node, param); });
        panel->setNodeActionsFiller([m](const std::string& node) { return nodeActions(*m, node); });
    }
    // The status line: what the last graph edit did (an error says why it was refused).
    QObject::connect(m->graph_, &NodeGraphView::status, m->shell_, [m](const QString& text) { m->shell_->setGraphStatus(text); });
    QObject::connect(m->graph_, &NodeGraphView::message, m->shell_, [m](const QString& text) { m->shell_->setGraphStatus(text); });
}

}  // namespace groomlab::app
