// SPDX-License-Identifier: Apache-2.0
#pragma once

// GroomLab's UI structure, shared by every host: engine/ui/*.json (docs/UI_DEFINITIONS.md), read
// the way Maya's hosts/maya/scripts/groomlab/ui/definitions.py reads it, for host "app".
//
// The files say what the menus, the Groom Tools' tabs and sections, the lists' action bars and
// right-click menus, the option lists, the marking menu and the Graph Editor's menus hold: action
// ids with their labels, tooltips, order, dividers, shortcut ids, enable conditions and host masks.
// The app embeds them as Qt resources (:/groomlab/ui/menus.json...); app_actions.h binds each id
// to what it runs, and fillMenu builds a QMenu from a container and those bindings (unbound ids
// hidden, as Blender does).
//
//     const ui::Definitions& d = ui::Definitions::shipped();
//     d.action("export.alembic")["label"];                       // "Alembic..."
//     d.resolve(d.get("lists/maps/context"), {{"map"}});         // the entries for the app
//
// resolve() matches definitions.py's resolve exactly (tests/engine/ui/resolve_cases.json holds
// every loader to the same cases).

#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <QKeySequence>
#include <QList>
#include <QPointer>
#include <QString>

#include <nlohmann/json.hpp>

class QMenu;
class QMenuBar;
class QAction;
class QWidget;

namespace groomlab::app::ui {

using json = nlohmann::json;

inline constexpr const char* kHost = "app";

// A resolved entry, as definitions.py's resolve gives it.
struct Item {
    enum class Kind { Divider, Action, Submenu, Generator };
    Kind kind = Kind::Divider;
    std::string id;     // the action's id; a submenu's "id" (else its "ref")
    std::string label;  // {name}s filled; a submenu's title
    std::string tip;
    std::string shortcut;  // a shortcuts.h id ("" none)
    std::optional<int> shortcutPart;
    bool checkable = false;
    std::string icon, glyph;
    json value;  // a generated entry's value (null: none)
    bool enabled = true;
    std::string name;   // a generator's name (Kind::Generator)
    json entry;         // the entry as written (an action's: merged over its action)
    std::vector<Item> items;  // a submenu's
};

// A generator a host expands: the entries (as the files write them, with "value"s) for a generator
// entry ({"generator": "themes", "action": "theme.set"}).
using Generator = std::function<json(const json& entry)>;
using Generators = std::map<std::string, Generator>;

struct ResolveOptions {
    // The conditions that hold (an entry's "needs", else its action's, must all hold, else it is
    // left out; "enabled" ones set Item::enabled); nullopt ignores "needs" (an action bar).
    std::optional<std::set<std::string>> state = std::set<std::string>{};
    json values = json::object();  // fill {name} in labels and tips
    Generators generators;         // besides the built-in "choices" and "backdropColors"
    std::string host = kHost;
    // Whether an action shows (Blender's policy: it has a binding); null: every one.
    std::function<bool(const std::string& id)> keep;
    bool dropGenerators = false;  // a generator nothing fills is left out (else a Kind::Generator item)
};

class Definitions {
public:
    // The app's embedded files (:/groomlab/ui/*.json), merged; read once.
    static const Definitions& shipped();
    // A merged document of one's own ({"actions": ..., "menus": ..., ...}: the tests' cases).
    explicit Definitions(json merged);
    // The files of a folder (engine/ui), merged; throws std::runtime_error when one is missing.
    static Definitions fromFolder(const std::string& folder);

    const json& document() const { return doc_; }
    // A value by its slash path from the merged root ("menus/export", "palette/texts/gestures");
    // throws std::out_of_range when there is none. find(): null when there is none.
    const json& get(const std::string& path) const;
    const json* find(const std::string& path) const;
    const json& action(const std::string& id) const { return get("actions").at(id); }
    // An option list: [{"value", "label", "tip", ...}] in its order.
    const json& choices(const std::string& name) const { return get("choices").at(name).at("items"); }

    // `entries` (a container: a list, or {"label", "tip", "items"}) for a host, as items to build.
    // Dividers are kept only between items; a submenu with nothing in it is left out.
    std::vector<Item> resolve(const json& entries, const ResolveOptions& options = {}) const;
    // Every action id `entries` can show on `host` whatever the state, referenced and included
    // lists followed; generated entries' ids from `generators` (name -> action id), else their
    // "action". For the binding checks.
    std::vector<std::string> listedIds(const json& entries, const std::string& host = kHost,
                                       const std::map<std::string, std::string>& generators = {}) const;
    // Every container of action entries in the files by slash path (Blender's containers()): the
    // menus, the Groom Tools' header, empty state and Output strip, the lists' bars and menus, the
    // marking menu's tail, the Graph Editor's menus.
    std::map<std::string, json> containers() const;

private:
    json doc_;
};

// `text` with {name} replaced by values[name] (names without a value stay as they are).
std::string fill(const std::string& text, const json& values);
// An entry's label or tip ("label", "tip") on `host`: from the first source with a per-host
// override ("labels": {"app": ...}) or the text itself; "" when none has it.
std::string hostText(const char* key, const std::string& host, std::initializer_list<const json*> sources);

// ---- Qt menus from the definitions --------------------------------------------------------------

// What an action id runs in the app (app_actions.h fills the table).
struct Binding {
    // Runs the action: `value` a generated entry's (a theme's name, a preset's path; null for
    // none), `checked` a checkable action's new state.
    std::function<void(const json& value, bool checked)> trigger;
    std::function<bool(const json& value)> checked;  // a checkable action's state (null: unchecked)
    std::function<bool()> enabled;                   // null: as the definitions say
    std::function<QString()> text;                   // a label that follows the state ("Undo Stroke")
    // Live shortcuts the app has for it beyond what the definitions give (a single key such as F
    // or Tab the definitions show as text only; a second key such as Ctrl+Shift+Z).
    QList<QKeySequence> keys;
    QString objectName;  // the QAction's name (tests find it by it); "%1" takes the item's value
};
using Bindings = std::map<std::string, Binding>;

// A generator the host fills itself (the Theme submenu, the node categories): called with the
// menu and the generator item to add its actions to the menu.
using MenuFiller = std::function<void(QMenu* menu, const Item& item)>;

struct FillOptions {
    ResolveOptions resolve;  // keep and dropGenerators are set from the bindings and fillers
    std::map<std::string, MenuFiller> fillers;
    // Live keyboard shortcuts: the definitions' keys that hold a modifier (Ctrl+O, Alt+Shift+X) and
    // a binding's own keys are set on the action (Qt::WindowShortcut), once per id across the menus
    // sharing `shortcutOwners`; off (or a key already owned), the keys show as text after the label.
    bool liveShortcuts = true;
    std::shared_ptr<std::map<std::string, QPointer<QAction>>> shortcutOwners;
};

// Fill `menu` from `entries` (a container) and `bindings`, as Maya's widgets.fill_menu does: each
// action's label (its keys after a tab: core shortcuts.h's for the app), tooltip (tooltips shown),
// check box and enabled state; ids without a binding are hidden, generators no filler fills too,
// and a submenu left empty goes. Each menu refreshes its actions' enabled, checked and text from
// the bindings when it opens; a menu holding a generator is filled again then (themes, presets).
// Returns {action id: QAction} (the first of an id; not the actions of a menu filled again, which
// go when it is).
std::map<std::string, QAction*> fillMenu(QMenu* menu, const json& entries, const Bindings& bindings,
                                         const FillOptions& options = {}, const Definitions& definitions = Definitions::shipped());

// The app's menu bar from menus.json's "app.menuBar" (File, Edit, View, then the GroomLab menu
// exactly as Maya's and Blender's "main", then Help). Returns {action id: QAction}.
std::map<std::string, QAction*> fillMenuBar(QMenuBar* bar, const json& entries, const Bindings& bindings,
                                            const FillOptions& options = {}, const Definitions& definitions = Definitions::shipped());

// A menu entry for a destructive action (actions.json "destructive": Delete, Remove), as fillMenu
// makes them: its text and a trash glyph in the theme's error red (a QWidgetAction; the action
// keeps its text, tip and enabled state, and triggers as any). `slot` runs when it triggers (may be
// empty: connect to triggered). Added to `menu`.
QAction* dangerAction(QMenu* menu, const QString& text, std::function<void()> slot = {}, const QString& tip = {});

// Every action fillMenu made under `menu` (its submenus too) takes its enabled, checked and text
// from its binding again: after an edit, so the live shortcuts of disabled actions stay off.
void refreshActions(QMenu* menu);
void refreshActions(QMenuBar* bar);

// The keys a shortcuts.h id gives the app ("" none), part `part` of "Ctrl+Z / Ctrl+Y" when given.
QString shortcutText(const std::string& id, std::optional<int> part = std::nullopt);

}  // namespace groomlab::app::ui
