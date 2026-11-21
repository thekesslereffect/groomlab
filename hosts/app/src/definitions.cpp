// SPDX-License-Identifier: Apache-2.0
#include "definitions.h"

#include <fstream>
#include <sstream>
#include <cctype>
#include <stdexcept>

#include <QAction>
#include <QFile>
#include <QMenu>
#include <QMenuBar>
#include <QStringList>

#include "groomlab/shortcuts.h"

namespace groomlab::app::ui {

namespace {

// The files every host reads (definitions.py's FILES).
constexpr const char* kFiles[] = {"actions", "menus", "palette", "lists", "choices", "marking_menu", "graph_ui", "properties"};

void merge(json& doc, json data, const std::string& name) {
    if (!data.is_object()) throw std::runtime_error("UI definitions: " + name + ".json is not an object");
    data.erase("$schema");
    data.erase("version");
    for (auto& [key, value] : data.items()) doc[key] = std::move(value);
}

bool met(const json* conditions, const std::set<std::string>& state) {
    if (!conditions || !conditions->is_array()) return true;
    for (const json& c : *conditions) {
        const std::string s = c.get<std::string>();
        if (!s.empty() && s[0] == '!') {
            if (state.count(s.substr(1))) return false;
        } else if (!state.count(s)) {
            return false;
        }
    }
    return true;
}

const json* member(const json& object, const char* key) {
    if (!object.is_object()) return nullptr;
    const auto it = object.find(key);
    return it == object.end() || it->is_null() ? nullptr : &*it;
}

std::string text(const json& object, const char* key) {
    const json* v = member(object, key);
    return v && v->is_string() ? v->get<std::string>() : std::string();
}

bool forHost(const json& entry, const std::string& host) {
    const json* hosts = member(entry, "hosts");
    if (!hosts) return true;
    for (const json& h : *hosts) {
        if (h.is_string() && h.get<std::string>() == host) return true;
    }
    return false;
}

bool hasAny(const json& entry, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        if (entry.contains(k)) return true;
    }
    return false;
}

const json& itemsOf(const json& container) {
    static const json empty = json::array();
    if (container.is_array()) return container;
    if (const json* items = member(container, "items")) return *items;
    return empty;
}

// The "choices" generator: an entry per option of choices.json's list, its action called with
// the option's value.
json choiceEntries(const Definitions& d, const json& entry) {
    const std::string field = entry.value("labelField", std::string("label"));
    json out = json::array();
    for (const json& c : d.choices(entry.at("choices").get<std::string>())) {
        out.push_back({{"id", entry.at("action")},
                       {"value", c.at("value")},
                       {"label", c.contains(field) ? c.at(field) : c.at("label")},
                       {"tip", c.value("tip", std::string())}});
    }
    return out;
}

json backdropEntries(const Definitions& d, const json& entry) {
    json out = json::array();
    for (const json& c : d.get("graph/backdropColors")) {
        out.push_back({{"id", entry.at("action")}, {"value", c.at("rgb")}, {"label", c.at("label")}});
    }
    return out;
}

}  // namespace

std::string fill(const std::string& s, const json& values) {
    if (!values.is_object() || values.empty() || s.find('{') == std::string::npos) return s;
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '{') {
            std::size_t j = i + 1;
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) ++j;
            if (j > i + 1 && j < s.size() && s[j] == '}') {
                const std::string name = s.substr(i + 1, j - i - 1);
                const auto it = values.find(name);
                if (it != values.end()) {
                    out += it->is_string() ? it->get<std::string>() : it->dump();
                    i = j + 1;
                    continue;
                }
            }
        }
        out += s[i++];
    }
    return out;
}

std::string hostText(const char* key, const std::string& host, std::initializer_list<const json*> sources) {
    const std::string plural = std::string(key) + "s";
    for (const json* source : sources) {
        if (!source || !source->is_object()) continue;
        if (const json* overrides = member(*source, plural.c_str())) {
            const auto it = overrides->find(host);
            if (it != overrides->end()) return it->get<std::string>();
        }
        const auto it = source->find(key);
        if (it != source->end()) return it->is_string() ? it->get<std::string>() : std::string();
    }
    return {};
}

// ---- The document ----------------------------------------------------------------------------

Definitions::Definitions(json merged) : doc_(std::move(merged)) {}

const Definitions& Definitions::shipped() {
    static const Definitions shipped = [] {
        json doc = json::object();
        for (const char* name : kFiles) {
            QFile f(QStringLiteral(":/groomlab/ui/%1.json").arg(QLatin1String(name)));
            if (!f.open(QIODevice::ReadOnly)) throw std::runtime_error(std::string("UI definitions: no embedded ") + name + ".json");
            const QByteArray bytes = f.readAll();
            merge(doc, json::parse(bytes.constData(), bytes.constData() + bytes.size()), name);
        }
        return Definitions(std::move(doc));
    }();
    return shipped;
}

Definitions Definitions::fromFolder(const std::string& folder) {
    json doc = json::object();
    for (const char* name : kFiles) {
        std::ifstream in(folder + "/" + name + ".json", std::ios::binary);
        if (!in) throw std::runtime_error("UI definitions: no " + folder + "/" + name + ".json");
        std::stringstream ss;
        ss << in.rdbuf();
        merge(doc, json::parse(ss.str()), name);
    }
    return Definitions(std::move(doc));
}

const json* Definitions::find(const std::string& path) const {
    const json* node = &doc_;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!node->is_object()) return nullptr;
        const auto it = node->find(part);
        if (it == node->end()) return nullptr;
        node = &*it;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return node;
}

const json& Definitions::get(const std::string& path) const {
    const json* node = find(path);
    if (!node) throw std::out_of_range("UI definitions: no " + path);
    return *node;
}

std::vector<Item> Definitions::resolve(const json& entriesIn, const ResolveOptions& options) const {
    const json& entries = itemsOf(entriesIn);
    const bool every = !options.state.has_value();
    static const std::set<std::string> none;
    const std::set<std::string>& state = every ? none : *options.state;
    const std::string& host = options.host;
    const json& actions = get("actions");
    ResolveOptions sub = options;
    if (every) sub.state.reset();
    std::vector<Item> out;
    auto lastIsDivider = [&out] { return !out.empty() && out.back().kind == Item::Kind::Divider; };
    auto extend = [&](std::vector<Item>&& items) {
        for (Item& i : items) {
            if (i.kind != Item::Kind::Divider || (!out.empty() && !lastIsDivider())) out.push_back(std::move(i));
        }
    };

    for (const json& raw : entries) {
        if (raw.is_string() && raw.get<std::string>() == "-") {
            if (!out.empty() && !lastIsDivider()) out.push_back(Item{});
            continue;
        }
        const json entry = raw.is_object() ? raw : json{{"id", raw}};
        const bool isAction = entry.contains("id") && !hasAny(entry, {"submenu", "ref", "generator", "include"});
        static const json noBase = json::object();
        const json* basePtr = &noBase;
        if (isAction) {
            const auto it = actions.find(entry.at("id").get<std::string>());
            if (it != actions.end()) basePtr = &*it;
        }
        const json& base = *basePtr;
        if (!forHost(entry, host) || !forHost(base, host)) continue;
        const json* needs = entry.contains("needs") ? &entry.at("needs") : member(base, "needs");
        if (!every && !met(needs, state)) continue;

        if (const json* include = member(entry, "include")) {
            extend(resolve(get(include->get<std::string>()), sub));
            continue;
        }
        if (const json* generator = member(entry, "generator")) {
            const std::string name = generator->get<std::string>();
            json made;
            bool filled = true;
            if (const auto it = options.generators.find(name); it != options.generators.end()) made = it->second(entry);
            else if (name == "choices") made = choiceEntries(*this, entry);
            else if (name == "backdropColors") made = backdropEntries(*this, entry);
            else filled = false;
            if (!filled) {
                if (!options.dropGenerators) {
                    Item item;
                    item.kind = Item::Kind::Generator;
                    item.name = name;
                    item.entry = entry;
                    out.push_back(std::move(item));
                }
            } else {
                extend(resolve(made.is_null() ? json::array() : made, sub));
            }
            continue;
        }
        if (entry.contains("submenu") || entry.contains("ref")) {
            json container = json::object();
            if (const json* ref = member(entry, "ref")) {
                const json& node = get(ref->get<std::string>());
                container = node.is_object() ? node : json{{"items", node}};
            }
            const json& items = entry.contains("items") ? entry.at("items") : itemsOf(container);
            std::vector<Item> resolved = resolve(items, sub);
            bool any = false;
            for (const Item& i : resolved) any = any || i.kind != Item::Kind::Divider;
            if (!any) continue;
            Item item;
            item.kind = Item::Kind::Submenu;
            item.id = entry.contains("id") ? text(entry, "id") : text(entry, "ref");
            std::string label;
            if (const json* labels = member(entry, "labels"); labels && labels->contains(host)) label = labels->at(host).get<std::string>();
            if (label.empty()) label = text(entry, "submenu");
            if (label.empty()) label = hostText("label", host, {&container});
            item.label = fill(label, options.values);
            item.tip = fill(hostText("tip", host, {&entry, &container}), options.values);
            item.glyph = text(entry, "glyph");
            item.items = std::move(resolved);
            item.entry = entry;
            out.push_back(std::move(item));
            continue;
        }
        const std::string id = entry.at("id").get<std::string>();
        if (options.keep && !options.keep(id)) continue;
        json merged = base;
        for (const auto& [key, value] : entry.items()) merged[key] = value;
        Item item;
        item.kind = Item::Kind::Action;
        item.id = id;
        item.enabled = met(member(merged, "enabled"), state);
        std::string tip = hostText("tip", host, {&entry, &base});
        if (!item.enabled && merged.contains("tipDisabled")) tip = merged.at("tipDisabled").get<std::string>();
        item.label = fill(hostText("label", host, {&entry, &base}), options.values);
        item.tip = fill(tip, options.values);
        item.shortcut = text(merged, "shortcut");
        if (const json* part = member(merged, "shortcutPart")) item.shortcutPart = part->get<int>();
        if (const json* checkable = member(merged, "checkable")) item.checkable = checkable->is_boolean() && checkable->get<bool>();
        item.icon = text(merged, "icon");
        item.glyph = text(merged, "glyph");
        if (const json* value = member(merged, "value")) item.value = *value;
        item.entry = std::move(merged);
        out.push_back(std::move(item));
    }
    while (lastIsDivider()) out.pop_back();
    return out;
}

std::vector<std::string> Definitions::listedIds(const json& entriesIn, const std::string& host,
                                                const std::map<std::string, std::string>& generators) const {
    std::vector<std::string> ids;
    const json& actions = get("actions");
    for (const json& raw : itemsOf(entriesIn)) {
        if (raw.is_string() && raw.get<std::string>() == "-") continue;
        const json entry = raw.is_object() ? raw : json{{"id", raw}};
        if (!forHost(entry, host)) continue;
        if (const json* include = member(entry, "include")) {
            for (std::string& id : listedIds(get(include->get<std::string>()), host, generators)) ids.push_back(std::move(id));
        } else if (const json* generator = member(entry, "generator")) {
            std::string made;
            if (const auto it = generators.find(generator->get<std::string>()); it != generators.end()) made = it->second;
            if (made.empty()) made = text(entry, "action");
            if (!made.empty()) ids.push_back(made);
        } else if (entry.contains("submenu") || entry.contains("ref")) {
            const json* items = member(entry, "items");
            const json& list = items ? *items : (entry.contains("ref") ? itemsOf(get(text(entry, "ref"))) : itemsOf(json::array()));
            for (std::string& id : listedIds(list, host, generators)) ids.push_back(std::move(id));
        } else if (entry.contains("id")) {
            const std::string id = text(entry, "id");
            const auto it = actions.find(id);
            if (it == actions.end() || forHost(*it, host)) ids.push_back(id);
        }
    }
    return ids;
}

std::map<std::string, json> Definitions::containers() const {
    std::map<std::string, json> out;
    std::function<void(const std::string&, const json&)> walk = [&](const std::string& path, const json& node) {
        if (node.is_array()) {
            for (const json& e : node) {
                if (e.is_string() || (e.is_object() && hasAny(e, {"id", "submenu", "ref", "include", "generator"}))) {
                    out[path] = node;
                    return;
                }
            }
            return;
        }
        if (node.is_object()) {
            if (const json* items = member(node, "items"); items && items->is_array() && path != "markingMenu/overflow") {
                walk(path, *items);
                return;
            }
            for (const auto& [key, value] : node.items()) walk(path + "/" + key, value);
        }
    };
    for (const char* top : {"menus", "palette", "lists", "graph"}) {
        if (const json* node = member(doc_, top)) walk(top, *node);
    }
    for (const char* gone : {"palette/tabs", "palette/sections", "palette/starter/steps", "palette/starter/lookTypes"}) out.erase(gone);
    json tail = json::array();
    if (const json* t = find("markingMenu/tail")) {
        for (const json& e : *t) {
            if (e.is_string()) tail.push_back(e);
        }
    }
    out["markingMenu/tail"] = tail;
    // The radial menu's actions (its rings' and the brush options'), each once, as plain ids.
    json radial = json::array();
    std::set<std::string> seen;
    auto add = [&](const json& entry) {
        if (const json* action = member(entry, "action"); action && action->is_string() && seen.insert(action->get<std::string>()).second) {
            radial.push_back(*action);
        }
    };
    std::function<void(const json&)> visit = [&](const json& items) {
        if (!items.is_array()) return;
        for (const json& e : items) {
            if (!e.is_object()) continue;
            add(e);
            if (const json* inner = member(e, "items")) visit(*inner);
        }
    };
    if (const json* items = find("radialMenu/items")) visit(*items);
    if (const json* tools = find("radialMenu/brushOptions/tools"); tools && tools->is_object()) {
        for (const auto& [tool, rows] : tools->items()) {
            if (!rows.is_array()) continue;
            for (const json& row : rows) {
                if (row.is_object()) add(row);
            }
        }
    }
    out["radialMenu"] = radial;
    return out;
}

// ---- Qt menus ------------------------------------------------------------------------------------

QString shortcutText(const std::string& id, std::optional<int> part) {
    const QString keys = QString::fromStdString(shortcutKeys(id, kShortcutApp));
    if (!part) return keys;
    const QStringList parts = keys.split(QStringLiteral(" / "));
    return *part < parts.size() ? parts[*part] : QString();
}

namespace {

// What the menus of one fillMenu / fillMenuBar call share.
struct Context {
    Bindings bindings;
    FillOptions options;
    const Definitions* definitions = nullptr;
};

// An action's state, a child of the action: its binding and item, to refresh from.
class ActionState : public QObject {
public:
    ActionState(QAction* action, std::shared_ptr<Context> context, Item item, QString keysText)
        : QObject(action), action_(action), context_(std::move(context)), item_(std::move(item)), keysText_(std::move(keysText)) {}

    void refresh() const {
        const auto it = context_->bindings.find(item_.id);
        if (it == context_->bindings.end()) return;
        const Binding& b = it->second;
        action_->setEnabled(item_.enabled && (!b.enabled || b.enabled()));
        if (item_.checkable) action_->setChecked(b.checked && b.checked(item_.value));
        if (b.text) {
            QString label = b.text();
            label.replace(QLatin1Char('&'), QStringLiteral("&&"));
            action_->setText(keysText_.isEmpty() ? label : label + QLatin1Char('\t') + keysText_);
        }
    }

private:
    QAction* action_;
    std::shared_ptr<Context> context_;
    Item item_;
    QString keysText_;
};

void refreshMenuActions(QMenu* menu) {
    for (QAction* a : menu->actions()) {
        for (QObject* child : a->children()) {
            if (auto* state = dynamic_cast<ActionState*>(child)) state->refresh();
        }
    }
}

// Generator items no filler fills taken out, then the dividers and submenus that leaves tidied
// (kept only between items; a submenu left empty goes).
std::vector<Item> prune(std::vector<Item> items, const std::map<std::string, MenuFiller>& fillers) {
    std::vector<Item> out;
    for (Item& i : items) {
        if (i.kind == Item::Kind::Generator && !fillers.count(i.name)) continue;
        if (i.kind == Item::Kind::Submenu) {
            i.items = prune(std::move(i.items), fillers);
            if (i.items.empty()) continue;
        }
        if (i.kind == Item::Kind::Divider && (out.empty() || out.back().kind == Item::Kind::Divider)) continue;
        out.push_back(std::move(i));
    }
    while (!out.empty() && out.back().kind == Item::Kind::Divider) out.pop_back();
    return out;
}

// Whether a container's own entries (and the lists it includes) hold a generator: its menu is
// filled again each time it opens.
bool holdsGenerator(const Definitions& d, const json& entries) {
    for (const json& e : itemsOf(entries)) {
        if (!e.is_object()) continue;
        if (e.contains("generator")) return true;
        if (const json* include = member(e, "include"); include && holdsGenerator(d, d.get(include->get<std::string>()))) return true;
    }
    return false;
}

// A submenu item's own entries: its "items", else its "ref"'s.
json submenuEntries(const Definitions& d, const Item& item) {
    if (const json* items = member(item.entry, "items")) return *items;
    if (const json* ref = member(item.entry, "ref")) return itemsOf(d.get(ref->get<std::string>()));
    return json::array();
}

QString escaped(const std::string& s) { return QString::fromStdString(s).replace(QLatin1Char('&'), QStringLiteral("&&")); }

bool liveKeys(const QString& keys) {
    // A key with a modifier is a window shortcut (Ctrl+O, Alt+Shift+X); single keys (B, S, F,
    // Delete) belong to the viewport and the graph, where text fields need them too.
    if (keys.isEmpty() || keys.contains(QLatin1Char(' '))) return false;
    if (!(keys.contains(QStringLiteral("Ctrl+")) || keys.contains(QStringLiteral("Alt+")) || keys.contains(QStringLiteral("Meta+"))))
        return false;
    const QKeySequence seq = QKeySequence::fromString(keys, QKeySequence::PortableText);
    return seq.count() == 1 && seq[0].key() != Qt::Key_unknown;
}

QString valueText(const json& value) {
    if (value.is_null()) return {};
    return QString::fromStdString(value.is_string() ? value.get<std::string>() : value.dump());
}

void build(QMenu* target, const std::vector<Item>& items, const std::shared_ptr<Context>& ctx,
           std::map<std::string, QAction*>* made);

// A submenu (or a top-level menu) of ours: tooltips shown, refreshed when it opens, filled again
// then when its entries hold a generator.
void manage(QMenu* menu, const json& entries, const std::shared_ptr<Context>& ctx) {
    menu->setToolTipsVisible(true);
    const bool dynamic = holdsGenerator(*ctx->definitions, entries);
    QObject::connect(menu, &QMenu::aboutToShow, menu, [menu, entries, ctx, dynamic] {
        if (dynamic) {
            for (QMenu* sub : menu->findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly)) sub->deleteLater();
            menu->clear();
            ResolveOptions options = ctx->options.resolve;
            build(menu, prune(ctx->definitions->resolve(entries, options), ctx->options.fillers), ctx, nullptr);
        }
        refreshMenuActions(menu);
    });
}

void build(QMenu* target, const std::vector<Item>& items, const std::shared_ptr<Context>& ctx,
           std::map<std::string, QAction*>* made) {
    for (const Item& item : items) {
        switch (item.kind) {
        case Item::Kind::Divider:
            target->addSeparator();
            break;
        case Item::Kind::Submenu: {
            auto* sub = new QMenu(escaped(item.label), target);
            sub->setObjectName(QStringLiteral("menu_") + QString::fromStdString(item.id));
            if (!item.tip.empty()) sub->menuAction()->setToolTip(QString::fromStdString(item.tip));
            target->addMenu(sub);
            const json entries = submenuEntries(*ctx->definitions, item);
            manage(sub, entries, ctx);
            build(sub, item.items, ctx, holdsGenerator(*ctx->definitions, entries) ? nullptr : made);
            break;
        }
        case Item::Kind::Generator:
            if (const auto it = ctx->options.fillers.find(item.name); it != ctx->options.fillers.end()) it->second(target, item);
            break;
        case Item::Kind::Action: {
            const auto bound = ctx->bindings.find(item.id);
            if (bound == ctx->bindings.end()) break;
            const Binding& b = bound->second;
            QString tip = QString::fromStdString(item.tip);
            QString keys;
            if (!item.shortcut.empty()) {
                keys = shortcutText(item.shortcut, item.shortcutPart);
                if (!item.shortcutPart && !tip.isEmpty() && !keys.isEmpty()) tip += QStringLiteral(" (%1)").arg(keys);
            }
            QList<QKeySequence> live;
            if (liveKeys(keys)) live.append(QKeySequence::fromString(keys, QKeySequence::PortableText));
            for (const QKeySequence& k : b.keys) {
                if (!live.contains(k)) live.append(k);
            }
            if (keys.isEmpty() && !b.keys.isEmpty()) keys = b.keys.first().toString(QKeySequence::NativeText);
            auto* action = new QAction(target);
            bool owner = false;
            if (ctx->options.liveShortcuts && !live.isEmpty() && ctx->options.shortcutOwners) {
                QPointer<QAction>& slot = (*ctx->options.shortcutOwners)[item.id];
                if (!slot) {
                    slot = action;
                    owner = true;
                    action->setShortcuts(live);
                    action->setShortcutContext(Qt::WindowShortcut);
                }
            }
            // The keys after a tab, unless the action's own shortcut shows them.
            const QString keysText = owner ? QString() : keys;
            const QString label = escaped(item.label);
            action->setText(keysText.isEmpty() ? label : label + QLatin1Char('\t') + keysText);
            if (!tip.isEmpty()) action->setToolTip(tip);
            if (!b.objectName.isEmpty()) {
                action->setObjectName(b.objectName.contains(QStringLiteral("%1")) ? b.objectName.arg(valueText(item.value)) : b.objectName);
            }
            if (item.checkable) action->setCheckable(true);
            const std::string id = item.id;
            const json value = item.value;
            const std::weak_ptr<Context> weak = ctx;
            QObject::connect(action, &QAction::triggered, action, [weak, id, value](bool checked) {
                const std::shared_ptr<Context> c = weak.lock();
                if (!c) return;
                const auto it = c->bindings.find(id);
                if (it != c->bindings.end() && it->second.trigger) it->second.trigger(value, checked);
            });
            target->addAction(action);
            auto* state = new ActionState(action, ctx, item, keysText);
            state->refresh();
            if (made && !made->count(item.id)) (*made)[item.id] = action;
            break;
        }
        }
    }
}

std::shared_ptr<Context> context(const Bindings& bindings, const FillOptions& options, const Definitions& definitions) {
    auto ctx = std::make_shared<Context>();
    ctx->bindings = bindings;
    ctx->options = options;
    ctx->definitions = &definitions;
    if (!ctx->options.shortcutOwners) ctx->options.shortcutOwners = std::make_shared<std::map<std::string, QPointer<QAction>>>();
    // Unbound ids are hidden (Blender's policy): the menus show only what the app runs.
    ctx->options.resolve.keep = [b = &ctx->bindings](const std::string& id) { return b->count(id) > 0; };
    return ctx;
}

// Keeps a context alive as long as the widget holding its menus.
class ContextHolder : public QObject {
public:
    ContextHolder(QObject* parent, std::shared_ptr<Context> context) : QObject(parent), context_(std::move(context)) {}

private:
    std::shared_ptr<Context> context_;
};

}  // namespace

std::map<std::string, QAction*> fillMenu(QMenu* menu, const json& entries, const Bindings& bindings, const FillOptions& options,
                                         const Definitions& definitions) {
    std::map<std::string, QAction*> made;
    const std::shared_ptr<Context> ctx = context(bindings, options, definitions);
    new ContextHolder(menu, ctx);
    manage(menu, itemsOf(entries), ctx);
    build(menu, prune(definitions.resolve(entries, ctx->options.resolve), ctx->options.fillers), ctx,
          holdsGenerator(definitions, entries) ? nullptr : &made);
    return made;
}

std::map<std::string, QAction*> fillMenuBar(QMenuBar* bar, const json& entries, const Bindings& bindings, const FillOptions& options,
                                            const Definitions& definitions) {
    std::map<std::string, QAction*> made;
    const std::shared_ptr<Context> ctx = context(bindings, options, definitions);
    new ContextHolder(bar, ctx);
    for (const Item& item : prune(definitions.resolve(entries, ctx->options.resolve), ctx->options.fillers)) {
        if (item.kind != Item::Kind::Submenu) continue;  // the bar holds menus
        auto* menu = new QMenu(escaped(item.label), bar);
        menu->setObjectName(QStringLiteral("menu_") + QString::fromStdString(item.id));
        if (!item.tip.empty()) menu->menuAction()->setToolTip(QString::fromStdString(item.tip));
        bar->addMenu(menu);
        const json own = submenuEntries(definitions, item);
        manage(menu, own, ctx);
        build(menu, item.items, ctx, holdsGenerator(definitions, own) ? nullptr : &made);
    }
    return made;
}

void refreshActions(QMenu* menu) {
    refreshMenuActions(menu);
    for (QMenu* sub : menu->findChildren<QMenu*>()) refreshMenuActions(sub);
}

void refreshActions(QMenuBar* bar) {
    for (QMenu* menu : bar->findChildren<QMenu*>()) refreshMenuActions(menu);
}

}  // namespace groomlab::app::ui
