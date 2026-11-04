// SPDX-License-Identifier: Apache-2.0
#include "theme.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <type_traits>
#include <utility>
#include <vector>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QSettings>

#include <nlohmann/json.hpp>

#include "groomlab/presets.h"

namespace groomlab::app::theme {

// ---- Colors --------------------------------------------------------------------------------------

namespace {

// The app wears Charcoal unless View > Theme (or --theme) says otherwise; a theme without
// "extends" fills its gaps from Playful, as the Maya host's theme.load does.
const QString kDefaultTheme = QStringLiteral("charcoal");
const QString kBaseTheme = QStringLiteral("playful");
const QString kThemeSetting = QStringLiteral("ui/theme");

// hosts/maya/themes/<name>.json, the Maya host's theme files, embedded at build time (resources of
// this library): the palette, the UI tones, the graph's and the icons' colors come from those
// files, so a theme edit reaches both hosts.
nlohmann::json readTheme(const QString& name) {
    QFile file(themeFile(name));
    nlohmann::json parsed = file.open(QIODevice::ReadOnly) ? nlohmann::json::parse(file.readAll().toStdString(), nullptr, false)
                                                           : nlohmann::json();
    return parsed.is_object() ? parsed : nlohmann::json::object();
}

void merge(nlohmann::json& base, const nlohmann::json& over) {
    for (auto it = over.begin(); it != over.end(); ++it) {
        if (it->is_object() && base.contains(it.key()) && base[it.key()].is_object()) merge(base[it.key()], *it);
        else base[it.key()] = *it;
    }
}

// A theme filled in from the one it extends (Playful unless it says otherwise), as the Maya
// host's theme.load does.
nlohmann::json loadTheme(const QString& name, int depth = 0) {
    nlohmann::json data = readTheme(name);
    const QString base = data.contains("extends") && data["extends"].is_string()
                             ? QString::fromStdString(data["extends"].get<std::string>())
                             : (name == kBaseTheme ? QString() : kBaseTheme);
    if (base.isEmpty() || depth > 4) return data;
    nlohmann::json out = loadTheme(base, depth + 1);
    merge(out, data);
    return out;
}

QString gTheme = kDefaultTheme;
int gGeneration = 0;  // reloadTheme: the files read again

const nlohmann::json& current() {
    static nlohmann::json data;
    static QString loaded;
    static int generation = -1;
    if (loaded != gTheme || generation != gGeneration || data.is_null()) {
        data = loadTheme(gTheme);
        loaded = gTheme;
        generation = gGeneration;
    }
    return data;
}

// A color as the theme writes one: "#RRGGBB", a palette name ("blue"), or a ui key naming one
// ("accent"); an invalid color for anything else.
QColor resolve(const std::string& value, int depth = 0) {
    if (!value.empty() && value[0] == '#') return QColor(QString::fromStdString(value));
    const nlohmann::json& t = current();
    for (const char* section : {"palette", "ui"}) {
        const auto s = t.find(section);
        if (s == t.end() || !s->is_object()) continue;
        const auto v = s->find(value);
        if (v != s->end() && v->is_string() && depth < 4) return resolve(v->get<std::string>(), depth + 1);
    }
    return QColor();
}

// The color at section.key (graph.categories.Look: {"graph", "categories"}, "Look").
QColor themed(std::initializer_list<const char*> path, const std::string& key) {
    const nlohmann::json* at = &current();
    for (const char* p : path) {
        const auto next = at->find(p);
        if (next == at->end() || !next->is_object()) return QColor();
        at = &*next;
    }
    const auto v = at->find(key);
    return v != at->end() && v->is_string() ? resolve(v->get<std::string>()) : QColor();
}

// The value at section.key, or `fallback` when the theme has none of that type.
template <typename T>
T value(const char* section, const char* key, T fallback) {
    const nlohmann::json& t = current();
    const auto s = t.find(section);
    if (s == t.end() || !s->is_object()) return fallback;
    const auto v = s->find(key);
    if (v == s->end()) return fallback;
    if constexpr (std::is_same_v<T, bool>) return v->is_boolean() ? v->get<bool>() : fallback;
    else return v->is_number() ? v->get<T>() : fallback;
}

// a toward b by t (0: a, 1: b).
QColor mix(const QColor& a, const QColor& b, double t) {
    return QColor::fromRgbF(static_cast<float>(a.redF() + (b.redF() - a.redF()) * t), static_cast<float>(a.greenF() + (b.greenF() - a.greenF()) * t),
                            static_cast<float>(a.blueF() + (b.blueF() - a.blueF()) * t));
}

}  // namespace

// ---- The theme ------------------------------------------------------------------------------------

QString userThemeDir() {
    const QString own = qEnvironmentVariable("GROOMLAB_USER_THEMES");
    if (!own.isEmpty()) return QDir::fromNativeSeparators(own);
    // Beside the user's presets: <the Maya user folder>/groomlab/themes, as the Maya host keeps them.
    const QString presets = QString::fromStdString(presetUserDir());
    if (presets.isEmpty()) return {};
    return QFileInfo(presets).dir().filePath(QStringLiteral("themes"));
}

QStringList themeSearchDirs() {
    QStringList dirs;
    for (const QString& d : qEnvironmentVariable("GROOMLAB_THEME_PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts)) {
        dirs << QDir::fromNativeSeparators(d.trimmed());
    }
    const QString user = userThemeDir();
    if (!user.isEmpty()) dirs << user;
    return dirs;
}

QString themeFile(const QString& name) {
    if (name.isEmpty() || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) return {};
    for (const QString& dir : themeSearchDirs()) {
        const QString path = QDir(dir).filePath(name + QStringLiteral(".json"));
        if (QFileInfo(path).isFile()) return path;
    }
    const QString embedded = QStringLiteral(":/groomlab/themes/%1.json").arg(name);
    return QFileInfo::exists(embedded) ? embedded : QString();
}

QStringList themeNames() {
    QStringList names;
    for (const QString& file : QDir(QStringLiteral(":/groomlab/themes")).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        names << file.chopped(5);
    }
    for (const QString& dir : themeSearchDirs()) {
        for (const QString& file : QDir(dir).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
            if (!names.contains(file.chopped(5))) names << file.chopped(5);
        }
    }
    names.sort();
    return names;
}

void reloadTheme() { ++gGeneration; }

QString createTheme(const QString& name, QString* pathOut) {
    static const QString kBad = QStringLiteral("\\/:*?\"<>|");
    if (name.trimmed().isEmpty() || std::any_of(name.begin(), name.end(), [](QChar c) { return kBad.contains(c); })) {
        return QStringLiteral("no theme named \"%1\": pick a name without \\ / : * ? \" < > |").arg(name);
    }
    const QString dir = userThemeDir();
    if (dir.isEmpty()) return QStringLiteral("no user folder for themes (set GROOMLAB_USER_THEMES)");
    const QString path = QDir(dir).filePath(name.trimmed() + QStringLiteral(".json"));
    if (QFileInfo::exists(path)) return QStringLiteral("a theme named %1 is there already: %2").arg(name, path);
    if (!QDir().mkpath(dir)) return QStringLiteral("cannot make the folder %1").arg(dir);
    nlohmann::json data = current();  // the active theme filled in: the copy stands on its own
    data["name"] = name.trimmed().toStdString();
    data.erase("extends");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QStringLiteral("cannot write %1").arg(path);
    file.write(QByteArray::fromStdString(data.dump(2) + "\n"));
    if (pathOut) *pathOut = path;
    return {};
}

QString themeLabel(const QString& name) {
    QStringList words = QString(name).replace(QLatin1Char('_'), QLatin1Char(' ')).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (QString& w : words) w[0] = w[0].toUpper();
    return words.join(QLatin1Char(' '));
}

QString defaultTheme() { return kDefaultTheme; }

QString currentTheme() { return gTheme; }

bool setTheme(const QString& name) {
    if (!themeNames().contains(name)) return false;
    gTheme = name;
    return true;
}

QString savedTheme() {
    const QString name = QSettings().value(kThemeSetting, kDefaultTheme).toString();
    return themeNames().contains(name) ? name : kDefaultTheme;
}

void saveTheme(const QString& name) { QSettings().setValue(kThemeSetting, name); }

bool glassy() { return value("ui", "glass", true); }

// The palette from the theme's file, then the glass and text tones: the mockup's own on glass,
// the theme's ui tones on a solid theme.
QColor red() { return themed({"palette"}, "red"); }
QColor yellow() { return themed({"palette"}, "yellow"); }
QColor blue() { return themed({"palette"}, "blue"); }
QColor emerald() { return themed({"palette"}, "emerald"); }
QColor purple() { return themed({"palette"}, "purple"); }
QColor white() { return themed({"palette"}, "white"); }
QColor uiColor(const std::string& key) { return themed({"ui"}, key); }
QColor glass() { return glassy() ? QColor(31, 32, 37, 184) : uiColor("panel"); }
QColor glassSolid() { return glassy() ? QColor(31, 32, 37, 219) : uiColor("panel"); }
QColor glassPanel() { return glassy() ? QColor(31, 32, 37, 236) : uiColor("panel"); }
QColor glassEdge() {
    if (glassy()) return QColor();
    const QColor outline = uiColor("outline");
    return outline.isValid() ? outline : uiColor("border");
}
QColor text() { return glassy() ? QColor(0xF3, 0xF1, 0xEC) : uiColor("text"); }
QColor textSoft() { return glassy() ? QColor(0xD9, 0xD5, 0xCD) : mix(uiColor("text"), uiColor("textDim"), 0.33); }
QColor textDim() { return glassy() ? QColor(0xB8, 0xB4, 0xAC) : mix(uiColor("text"), uiColor("textDim"), 0.66); }
QColor textMuted() {
    if (glassy()) return QColor(0x8F, 0x8B, 0x84);
    const QColor muted = uiColor("textMuted");
    return muted.isValid() ? muted : uiColor("textDim");
}
// The washes: white over the glass; a solid theme's own tones (the raised fields, the hover, the edge).
QColor track() { return glassy() ? QColor(255, 255, 255, 31) : uiColor("hover"); }
QColor chip() { return glassy() ? QColor(255, 255, 255, 20) : uiColor("raised"); }
QColor chipSoft() { return glassy() ? QColor(255, 255, 255, 15) : uiColor("raised"); }
QColor chipChecked() { return glassy() ? QColor(255, 255, 255, 46) : uiColor("hover"); }
QColor divider() { return glassy() ? QColor(255, 255, 255, 31) : glassEdge(); }
QColor renderRed() { return glassy() ? QColor(0xE5, 0x38, 0x3B) : accentFill(); }
QColor backdrop() { return glassy() ? QColor(0x17, 0x18, 0x1B) : uiColor("background"); }
QColor ink() { return QColor(0x1B, 0x1A, 0x17); }
QColor blueLight() { return glassy() ? QColor(0x6E, 0xA4, 0xFF) : blue().lighter(115); }
QColor redLight() { return glassy() ? QColor(0xFF, 0x7A, 0x7C) : red().lighter(115); }
QColor emeraldSoft() { return glassy() ? QColor(0x7E, 0xE2, 0xB8) : emerald().lighter(115); }
QColor emeraldChip() {
    QColor c = emerald();
    c.setAlpha(56);
    return c;
}
QColor readableOn(const QColor& fill) {
    auto channel = [](qreal c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    auto luminance = [&](const QColor& c) { return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) + 0.0722 * channel(c.blueF()); };
    const qreal l = luminance(fill);
    const qreal withInk = (l + 0.05) / (luminance(ink()) + 0.05), withWhite = 1.05 / (l + 0.05);
    return withInk > withWhite ? ink() : QColor(Qt::white);
}
QColor accentFill() {
    // A filled primary action: ui.accentFill when the theme has one (deeper than the accent, so
    // white text on it reads), else the accent.
    const QColor c = uiColor("accentFill");
    return c.isValid() ? c : uiColor("accent");
}
QColor viewportColor(const std::string& key) { return themed({"viewport"}, key); }

QColor segmentOn() {
    // The chosen cell on a sunken track (ui.segmentOn): raised toward the surface on a light theme
    // (white over the track), a step toward the text on a dark one.
    const QColor c = uiColor("segmentOn");
    if (c.isValid()) return c;
    return isLight() ? QColor(Qt::white) : towardText(uiColor("hover"), 0.08);
}
QColor swatchEdge() {
    // A color swatch's 1 px edge (ui.checkEdge on a light theme): white or black colors keep their shape.
    const QColor c = uiColor(isLight() ? "checkEdge" : "swatchEdge");
    return c.isValid() ? c : towardText(uiColor("raised"), isLight() ? 0.45 : 0.22);
}
QColor hudFill() {
    // The chips over the viewport (viewport.hudFill): they belong to the view, dark over its dark render
    // whatever the panels' theme.
    const QColor c = themed({"viewport"}, "hudFill");
    if (c.isValid()) return c;
    QColor f = isLight() ? QColor(0x1C, 0x1B, 0x19) : uiColor("panel");
    f.setAlpha(isLight() ? 226 : 214);
    return f;
}
QColor hudText() {
    const QColor c = themed({"viewport"}, "hudText");
    return c.isValid() ? c : isLight() ? QColor(0xF4, 0xF1, 0xEA) : text();
}
QColor hudTextDim() { return isLight() ? QColor(0xC4, 0xBF, 0xB4) : textMuted(); }
int checkRadius() { return std::clamp(value("ui", "checkRadius", smallRadius()), 0, 7); }
bool toolOnCategory() {
    // ui.toolOn "category": the chosen tool tile in its category's color (Playful's), else the accent.
    const nlohmann::json& ui = current().value("ui", nlohmann::json::object());
    return ui.is_object() && ui.value("toolOn", std::string()) == "category";
}

bool isLight() {
    // A light theme: its panel brighter than mid grey (the contrast steps then go toward dark).
    const QColor p = uiColor("panel");
    return p.isValid() && 0.2126 * p.redF() + 0.7152 * p.greenF() + 0.0722 * p.blueF() > 0.5;
}
QColor towardText(const QColor& c, double t) { return mix(c, text(), t); }
// The component tokens: the theme's own (ui.<key>), else derived from its base tones (never white),
// so a theme that sets only its base tones still draws every part.
QColor card() {
    // A section's card on the panel (ui.card): a step toward the fields, so a section reads as one block.
    if (glassy()) return QColor(255, 255, 255, 10);
    const QColor c = uiColor("card");
    return c.isValid() ? c : mix(uiColor("panel"), uiColor("raised"), 0.45);
}
QColor sunken() {
    // A track under segments and pill tabs (ui.sunken): toward the window's backdrop.
    const QColor c = uiColor("sunken");
    if (c.isValid()) return c;
    return isLight() ? mix(uiColor("panel"), text(), 0.07) : mix(backdrop(), uiColor("panel"), 0.3);
}
QColor accentSoft() {
    // What is chosen (a tool, a chip, a row): the accent as a soft wash of the card (ui.accentSoft).
    const QColor c = uiColor("accentSoft");
    return c.isValid() ? c : mix(card(), uiColor("accent"), isLight() ? 0.16 : 0.22);
}
QColor accentInk() {
    // Text and icons in the accent on the panels (ui.accentInk): the accent toward the text.
    const QColor c = uiColor("accentInk");
    return c.isValid() ? c : mix(uiColor("accent"), text(), 0.45);
}
QColor textDisabled() {
    if (glassy()) return QColor(0x6E, 0x6B, 0x66);
    const QColor c = uiColor("textDisabled");
    return c.isValid() ? c : textMuted();
}
QColor segmentFill(const QColor& own) { return checkedFill(own); }
QColor segmentText(const QColor& own) { return checkedText(own); }
QColor checkedFill(const QColor& own) {
    if (glassy()) return own;
    const QColor c = uiColor("selection");
    return c.isValid() ? c : uiColor("accent");
}
QColor checkedText(const QColor& own) {
    if (glassy()) return own;
    const QColor c = uiColor("selectionText");
    return c.isValid() ? c : uiColor("accentText");
}
QColor sliderFill(const QColor& own) {
    const auto ui = current().find("ui");
    if (ui == current().end() || !ui->is_object()) return own;
    const auto sliders = ui->find("sliders");
    if (sliders == ui->end() || !sliders->is_array() || sliders->size() != 1 || !(*sliders)[0].is_string()) return own;
    const QColor one = resolve((*sliders)[0].get<std::string>());
    return one.isValid() ? one : own;
}
QColor actionFill(const QColor& own) { return glassy() ? own : accentFill(); }
QColor actionText(const QColor& own) { return glassy() ? own : uiColor("accentText"); }
QColor errorText() {
    // The theme's ui.error, lightened on glass (the mockup's #FF7A7C over the viewport).
    const QColor c = uiColor("error");
    if (!c.isValid()) return redLight();
    return glassy() ? mix(c, QColor(Qt::white), 0.25) : c;
}
QColor warningText() {
    const QColor c = uiColor("warning");
    return c.isValid() ? c : yellow();
}
QColor linkText() { return glassy() ? QColor(0x7F, 0xB2, 0xFF) : mix(uiColor("accent"), uiColor("text"), 0.35); }
QColor windowButton() {
    const QColor c = uiColor("raised");
    return c.isValid() ? c : QColor(0x3A, 0x3C, 0x42);
}
QColor windowButtonHover() {
    const QColor c = uiColor("hover");
    return c.isValid() ? c : QColor(0x46, 0x48, 0x4F);
}

// ---- Fonts ---------------------------------------------------------------------------------------

namespace {

// The bundled variable fonts (google/fonts, SIL OFL 1.1: THIRD_PARTY_NOTICES.txt). Qt
// registers a variable font's named instances, so a weight picks the right one.
bool fontsLoaded = false, fontsFound = false;

}  // namespace

bool loadFonts() {
    if (fontsLoaded) return fontsFound;
    fontsLoaded = true;
    const int inter = QFontDatabase::addApplicationFont(QStringLiteral(":/groomlab/fonts/Inter[opsz,wght].ttf"));
    const int mono = QFontDatabase::addApplicationFont(QStringLiteral(":/groomlab/fonts/JetBrainsMono[wght].ttf"));
    fontsFound = inter >= 0 && mono >= 0;
    return fontsFound;
}

QString uiFamily() {
    // The theme's ui.font (Inter, the bundled one, unless the theme names another).
    const nlohmann::json& ui = current().value("ui", nlohmann::json::object());
    const std::string family = ui.is_object() ? ui.value("font", std::string()) : std::string();
    return family.empty() ? QStringLiteral("Inter") : QString::fromStdString(family);
}

QString monoFamily() { return QStringLiteral("JetBrains Mono"); }

QFont uiFont(int pixelSize, int weight) {
    loadFonts();
    QFont f(uiFamily());
    f.setStyleHint(QFont::SansSerif);
    // A solid theme's words are never heavier than semibold nor smaller than 11 px: heavy light
    // text blooms on a dark panel, and tiny text strains (a glass theme keeps its mockup's).
    if (!glassy()) {
        // (A title of 15 px and over may be bold: big text does not bloom.)
        weight = std::min(weight, static_cast<int>(pixelSize >= 15 ? QFont::Bold : QFont::DemiBold));
        pixelSize = std::max(pixelSize, 11);
    }
    f.setPixelSize(pixelSize);
    f.setWeight(static_cast<QFont::Weight>(weight));
    return f;
}

QFont monoFont(int pixelSize, int weight) {
    loadFonts();
    QFont f(monoFamily());
    f.setStyleHint(QFont::Monospace);
    f.setPixelSize(pixelSize);
    f.setWeight(static_cast<QFont::Weight>(weight));
    return f;
}

QFont numberFont(int pixelSize, int weight) {
    // The UI font with tabular (fixed-width) digits: a value that changes keeps its width.
    QFont f = uiFont(pixelSize, weight);
    f.setFeature(QFont::Tag("tnum"), 1);
    return f;
}

// ---- Sizes ----------------------------------------------------------------------------------------

int controlRadius() {
    // ui.radius; a glass theme's no rounder than its pills (8).
    const int r = std::clamp(value("ui", "radius", glassy() ? 8 : 4), 0, 16);
    return glassy() ? std::min(r, 8) : r;
}
int smallRadius() { return std::max(2, controlRadius() - 2); }
int cardRadius() { return std::clamp(value("ui", "cardRadius", panelRadius()), 0, 20); }
int panelRadius() { return controlRadius() + 2; }
int rowHeight() { return std::clamp(value("ui", "rowHeight", 26), 18, 40); }
int fontSize() { return glassy() ? 13 : std::clamp(value("ui", "fontSize", 12), 11, 14); }

QString headerStyle() {
    const nlohmann::json& graph = current().value("graph", nlohmann::json::object());
    const std::string style = graph.is_object() ? graph.value("headerStyle", std::string("filled")) : std::string("filled");
    return style == "tinted" || style == "minimal" ? QString::fromStdString(style) : QStringLiteral("filled");
}

QString canvasStyle() {
    const nlohmann::json& graph = current().value("graph", nlohmann::json::object());
    const std::string style = graph.is_object() ? graph.value("canvasStyle", std::string("lines")) : std::string("lines");
    return style == "dots" || style == "plain" ? QString::fromStdString(style) : QStringLiteral("lines");
}

// ---- Brushes --------------------------------------------------------------------------------------

const std::vector<BrushEntry>& brushes() {
    // The core's catalog in its order (Create, Shape, Trim, Paint, then Freeze), each tip its
    // label and hint.
    static const std::vector<BrushEntry> entries = [] {
        std::vector<BrushEntry> out;
        for (const groomlab::BrushEntry& e : brushCatalog()) {
            if (e.id == "select") continue;
            out.push_back({e.id, e.icon, e.label + ": " + e.hint, e.category});
        }
        return out;
    }();
    return entries;
}

const BrushEntry* brush(const std::string& name) {
    for (const BrushEntry& b : brushes()) {
        if (name == b.name) return &b;
    }
    return nullptr;
}

QColor categoryColor(BrushCategory category) {
    // The theme's icons (create: the tools that make guides); the guide tools white, as Select is.
    switch (category) {
        case BrushCategory::Create: return themed({"icons"}, "create");
        case BrushCategory::Shape: return themed({"icons"}, "shape");
        case BrushCategory::Trim: return themed({"icons"}, "trim");
        case BrushCategory::Paint: return themed({"icons"}, "paint");
        case BrushCategory::Guides: return white();
    }
    return blue();
}

QColor categoryIconColor(BrushCategory category) {
    // Lighter than the fills, as the mockup tints the rail's resting icons, so they read on glass;
    // a solid theme's icons as its file colors them.
    if (!glassy()) return category == BrushCategory::Guides ? text() : categoryColor(category);
    switch (category) {
        case BrushCategory::Create: return emerald();
        case BrushCategory::Shape: return blueLight();
        case BrushCategory::Trim: return redLight();
        case BrushCategory::Paint: return yellow();
        case BrushCategory::Guides: return text();
    }
    return blueLight();
}

QColor toolIconColor(BrushCategory category, const std::string& icon) {
    // The Guides tray's tools (Select, Freeze, Unfreeze) share one color: the paint group's (yellow in Playful).
    (void)icon;
    if (category == BrushCategory::Guides) category = BrushCategory::Paint;
    return categoryIconColor(category);
}

QColor iconInk() {
    const QColor c = themed({"icons"}, "ink");
    return c.isValid() ? c : text();
}

QString brushTitle(const std::string& name) {
    if (name.empty()) return {};
    if (const groomlab::BrushEntry* e = brushEntry(name)) return QString::fromStdString(e->label);
    QString title = QString::fromStdString(name);
    title[0] = title[0].toUpper();
    return title;
}

// ---- The node graph ------------------------------------------------------------------------------

QColor nodeCategoryColor(const std::string& category) {
    // The theme's graph.categories (the Graph Editor's titles), by the add-node menu's category
    // ids (node_catalog.h); a category the theme has no color for takes the Maya Graph Editor's
    // (its CATEGORY_COLORS).
    struct Entry {
        const char* id;
        const char* title;
        QColor fallback;
    };
    static const Entry entries[] = {
        {"source", "Sources", QColor(72, 190, 170)},       {"grow", "Grow", QColor(79, 209, 197)},
        {"shape", "Shape", QColor(90, 150, 240)},          {"deform", "Deform", QColor(110, 170, 250)},
        {"collide", "Collide", QColor(100, 180, 200)},     {"look", "Look", QColor(170, 120, 240)},
        {"motion", "Motion", QColor(120, 200, 255)},       {"feathers", "Feathers", QColor(226, 186, 90)},
        {"color", "Color", QColor(255, 122, 182)},         {"convert", "Convert", QColor(240, 140, 95)},
        {"field", "Fields & Curves", QColor(214, 122, 214)}, {"utility", "Utility", QColor(140, 148, 158)},
        {"games", "Games", QColor(90, 200, 230)},          {"output", "Output", QColor(255, 255, 255)},
    };
    const Entry* entry = &entries[11];  // anything else: Utility
    for (const Entry& e : entries) {
        if (category == e.id) entry = &e;
    }
    const QColor themedColor = themed({"graph", "categories"}, entry->title);
    return themedColor.isValid() ? themedColor : entry->fallback;
}

QColor portColor(const std::string& portType) {
    // The theme's graph.ports, by the port types' names (Strands, Guides, ...: either case here).
    std::string type = portType;
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const char* name = type == "strands" ? "Strands"
                       : type == "guides" ? "Guides"
                       : type == "mesh"   ? "Mesh"
                       : type == "field"  ? "Field"
                       : type == "curve"  ? "Curve"
                       : type == "value"  ? "Value"
                                          : "Curves";  // and anything else
    return themed({"graph", "ports"}, name);
}

QColor nodeBody() { return themed({"graph"}, "node"); }

bool isPortType(const std::string& portType) {
    std::string type = portType;
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* known : {"strands", "guides", "mesh", "field", "curve", "curves", "value"}) {
        if (type == known) return true;
    }
    return false;
}

QColor graphCanvas() {
    const QColor c = themed({"graph"}, "canvas");
    return c.isValid() ? c : backdrop();
}

QColor graphGrid() {
    const QColor c = themed({"graph"}, "grid");
    return c.isValid() ? c : towardText(graphCanvas(), 0.08);
}

QColor nodeBorder() { return themed({"graph"}, "nodeBorder"); }

QColor nodeSelected() {
    const QColor c = themed({"graph"}, "selected");
    return c.isValid() ? c : white();
}

namespace {

// graph.<key>, else `fallback` (a theme file without the key).
QColor graphColor(const char* key, const QColor& fallback) {
    const QColor c = themed({"graph"}, key);
    return c.isValid() ? c : fallback;
}

}  // namespace

QColor graphError() { return graphColor("error", red()); }
QColor graphWarning() { return graphColor("warning", yellow()); }
QColor graphNote() { return graphColor("note", textMuted()); }
QColor graphDriven() { return graphColor("driven", purple()); }
QColor graphReads() { return graphColor("reads", blue()); }
QColor graphWrites() { return graphColor("writes", QColor(0xF0, 0xA0, 0x50)); }

qreal nodeRadius() { return value("graph", "cornerRadius", 14.0); }

bool tintedHeaders() { return headerStyle() == QLatin1String("tinted"); }
bool nodeShadow() { return value("graph", "shadow", false); }

// ---- The viewport's overlay ------------------------------------------------------------------------

DisplayTheme displayTheme() {
    // theme.apply_viewport's mapping: each viewport.* color (hex or a palette name) as linear RGB.
    DisplayTheme t;
    const nlohmann::json& data = current();
    const nlohmann::json viewport = data.contains("viewport") && data["viewport"].is_object() ? data["viewport"] : nlohmann::json::object();
    auto linear = [](const QColor& c) {
        auto channel = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
        return DisplayTheme::RGB{channel(c.redF()), channel(c.greenF()), channel(c.blueF())};
    };
    auto color = [&](const char* key, const char* fallback) {
        QColor c = viewport.contains(key) && viewport[key].is_string() ? resolve(viewport[key].get<std::string>()) : QColor();
        if (!c.isValid()) c = resolve(fallback);
        return linear(c.isValid() ? c : QColor(Qt::white));
    };
    auto rgba = [&](const char* key, const char* fallback) {
        const DisplayTheme::RGB c = color(key, fallback);
        return DisplayTheme::RGBA{c[0], c[1], c[2], 1.0f};
    };
    if (viewport.contains("pointSize") && viewport["pointSize"].is_number()) t.pointSize = std::max(0.0f, viewport["pointSize"].get<float>());
    t.selected = rgba("selected", "white");
    t.softSelected = rgba("softSelected", "yellow");
    t.frozen = color("frozen", "blue");
    t.parts = rgba("parts", "red");
    t.flow = rgba("flow", "yellow");
    t.flowField = rgba("flowField", "#8FA3BF");
    t.tubes = rgba("tubes", "#8A93A0");
    t.instances = rgba("instances", "white");
    t.clumpCenters = color("clumpCenters", "#FF4FD8");
    return t;
}

// ---- Layout --------------------------------------------------------------------------------------

ViewportOverlayLayout layoutViewportOverlay(const ViewportOverlayRequest& r, int m) {
    ViewportOverlayLayout out;
    const int w = r.view.width(), h = r.view.height();
    // A piece no wider (or taller) than the view less the margins.
    auto fit = [&](QSize s) { return QSize(std::min(s.width(), std::max(0, w - 2 * m)), std::min(s.height(), std::max(0, h - 2 * m))); };
    const QSize tool = fit(r.toolLabel), stats = fit(r.stats), refine = fit(r.refine);
    if (!tool.isEmpty()) out.toolLabel = QRect(QPoint(m, m), tool);
    // Undo and Redo at the top-right; in a view too narrow for both, under the tool's name.
    const QSize history = fit(r.history);
    if (!history.isEmpty()) {
        out.history = QRect(QPoint(w - m - history.width(), m), history);
        if (out.history.intersects(out.toolLabel)) out.history.moveTop(out.toolLabel.bottom() + 1 + 8);
    }
    // The stats bottom-left, the refine line bottom-right; the refine line above the stats when
    // the view is too narrow for both on one line.
    if (!stats.isEmpty()) out.stats = QRect(QPoint(m, h - m - stats.height()), stats);
    if (!refine.isEmpty()) {
        out.refine = QRect(QPoint(w - m - refine.width(), h - m - refine.height()), refine);
        if (out.refine.intersects(out.stats)) out.refine.moveBottom(out.stats.top() - 1 - 6);
    }
    // The first-groom card bottom-left above the stats (and above the refine line when that one
    // sits under it); left out when it would reach the top row.
    const QSize starter = fit(r.starter);
    if (!starter.isEmpty()) {
        int above = h - m;
        for (const QRect& r2 : {out.stats, out.refine}) {
            if (!r2.isNull() && r2.left() < m + starter.width()) above = std::min(above, r2.top() - 8);
        }
        const QRect card(QPoint(m, above - starter.height()), starter);
        int top = m;
        for (const QRect& r2 : {out.toolLabel, out.history}) {
            if (!r2.isNull() && r2.left() < card.right()) top = std::max(top, r2.bottom() + 1 + 8);
        }
        if (card.top() >= top) out.starter = card;
    }
    // The toast centered above the bottom lines (and above the first-groom card where they would meet).
    int bottom = h - m;
    for (const QRect& r2 : {out.stats, out.refine}) {
        if (!r2.isNull()) bottom = std::min(bottom, r2.top() - 8);
    }
    const QSize toast = fit(r.toast);
    if (!toast.isEmpty()) {
        const int left = (w - toast.width()) / 2;
        if (!out.starter.isNull() && left <= out.starter.right()) {
            // No room for the toast above the card, under the top row (a short view): the toast
            // wins for its seconds.
            int below = m;
            for (const QRect& r2 : {out.toolLabel, out.history}) {
                if (!r2.isNull()) below = std::max(below, r2.bottom() + 1 + 8);
            }
            if (out.starter.top() - 8 - toast.height() < below) out.starter = QRect();
            else bottom = std::min(bottom, out.starter.top() - 8);
        }
        out.toast = QRect(QPoint(left, std::max(m, bottom - toast.height())), toast);
    }
    // The prompt a little above the middle, the card in the middle.
    const QSize prompt = fit(r.prompt), card = fit(r.emptyCard);
    if (!prompt.isEmpty()) out.prompt = QRect(QPoint((w - prompt.width()) / 2, std::max(m, (h - prompt.height()) / 2 - 40)), prompt);
    if (!card.isEmpty()) out.emptyCard = QRect(QPoint((w - card.width()) / 2, std::max(m, (h - card.height()) / 2)), card);
    return out;
}

// ---- The style sheet ------------------------------------------------------------------------------

QString styleSheet() {
    // The window's chrome first (solid in every theme: the menu bar, the tool bar, the docks with
    // their titles and tabs, the separators), then the ordinary widgets inside the docks and the
    // glass panels: no frames, the raised tone (a glass theme's white wash) for fields, the accent
    // for what is on. Tokens (@name@) take the theme's tones below.
    QString sheet = QStringLiteral(R"(
QWidget { color: @text@; font-family: "@uiFamily@"; font-size: @fontSize@px; background: transparent; }
QMainWindow { background: @background@; }
QMainWindow::separator { background: @background@; width: 3px; height: 3px; }
QMainWindow::separator:hover { background: @hover@; }
QMenuBar { background: @panel@; border-bottom: 1px solid @edge@; padding: 1px 4px; }
QMenuBar::item { background: transparent; color: @soft@; padding: 4px 9px; border-radius: @radius@px; }
QMenuBar::item:selected { background: @hover@; color: @text@; }
QMenuBar::item:pressed { background: @selection@; color: @selectionText@; }
QToolBar { background: @panel@; border: none; border-bottom: 1px solid @edge@; padding: 3px 8px; spacing: 6px; }
QToolBar::separator { background: @edge@; width: 1px; margin: 5px 4px; }
QDockWidget { color: @dim@; font-size: @smallSize@px; font-weight: 600; }
QDockWidget::title { background: @panel@; padding: 6px 12px; border-bottom: 1px solid @edge@; text-align: left; }
QDockWidget::close-button, QDockWidget::float-button { border: none; background: transparent; padding: 1px; }
QDockWidget::close-button:hover, QDockWidget::float-button:hover { background: @hover@; border-radius: 3px; }
#dockBody { background: @panel@; }
QTabWidget::pane { border: none; border-top: 1px solid @edge@; background: @panel@; }
QTabWidget::tab-bar { left: 4px; }
QTabBar { background: transparent; qproperty-drawBase: 0; }
QTabBar::tab { background: transparent; color: @dim@; padding: 6px 2px; margin: 0 12px 0 0; border: none; border-bottom: 2px solid transparent; font-weight: 500; }
QTabBar::tab:selected { color: @text@; border-bottom: 2px solid @accent@; font-weight: 600; }
QTabBar::tab:hover:!selected { color: @text@; }
/* The panels' own tabs (Properties, Groom Tools): pills on a sunken track, the chosen one raised. */
QTabBar[role="pills"] { background: @sunken@; border-radius: 9px; }
QTabBar[role="pills"]::tab { background: transparent; color: @muted@; padding: 5px 9px; margin: 3px 1px; border: none; border-radius: 6px; font-weight: 500; }
QTabBar[role="pills"]::tab:first { margin-left: 3px; }
QTabBar[role="pills"]::tab:last { margin-right: 3px; }
QTabBar[role="pills"]::tab:selected { background: @segOn@; color: @text@; font-weight: 600; }
QTabBar[role="pills"]::tab:hover:!selected { color: @text@; }
#groomTabs::tab { padding: 5px 6px; }
QMainWindow > QTabBar { background: @sunken@; border-radius: 9px; }
QMainWindow > QTabBar::tab { background: transparent; color: @muted@; padding: 5px 12px; margin: 3px 1px; border: none; border-radius: 6px; font-weight: 500; }
QMainWindow > QTabBar::tab:selected { background: @segOn@; color: @text@; font-weight: 600; }
QMainWindow > QTabBar::tab:hover:!selected { color: @text@; }
QSplitter::handle { background: @background@; }
QSplitter::handle:horizontal { width: 3px; }
QSplitter::handle:vertical { height: 3px; }
QToolTip { color: @text@; background: @field@; border: 1px solid @popupEdge@; border-radius: @radius@px; padding: 4px 8px; }
QMenu { background: @field@; border: 1px solid @popupEdge@; border-radius: @radiusLarge@px; padding: 4px; }
QMenu::item { padding: 5px 28px 5px 12px; border-radius: @radius@px; }
QMenu::item:selected { background: @selection@; color: @selectionText@; }
QMenu::item:disabled { color: @disabled@; }
QMenu::separator { height: 1px; background: @hover@; margin: 4px 8px; }
QGroupBox { border: 1px solid @groupEdge@; border-radius: @radiusLarge@px; margin-top: 14px; padding: 8px 4px 4px 4px; font-weight: 600; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: @dim@; }
QLabel { color: @soft@; }
QAbstractSpinBox, QLineEdit, QComboBox {
    background: @field@; border: 1px solid @fieldEdge@; border-radius: @radius@px; padding: 0 8px; min-height: @controlInner@px; max-height: @controlInner@px;
    selection-background-color: @selection@; selection-color: @selectionText@; }
QAbstractSpinBox:hover, QLineEdit:hover, QComboBox:hover { border: 1px solid @hoverEdge@; }
QLineEdit:read-only { color: @muted@; }
QLineEdit:read-only:hover { border: 1px solid @fieldEdge@; }
QAbstractSpinBox:focus, QLineEdit:focus, QComboBox:focus { border: 2px solid @accent@; }
QAbstractSpinBox:disabled, QLineEdit:disabled, QComboBox:disabled { color: @disabled@; background: @fieldOff@; }
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button { width: 14px; border: none; background: transparent; }
QAbstractSpinBox::up-arrow { image: url(:/groomlab/icons/ui_chevron_up.svg); width: 9px; height: 9px; }
QAbstractSpinBox::down-arrow { image: url(:/groomlab/icons/ui_chevron_down.svg); width: 9px; height: 9px; }
QAbstractSpinBox::up-arrow:disabled, QAbstractSpinBox::down-arrow:disabled { image: none; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox::down-arrow { image: url(:/groomlab/icons/ui_chevron_down.svg); width: 11px; height: 11px; margin-right: 4px; }
QComboBox QAbstractItemView { background: @field@; border: 1px solid @popupEdge@; border-radius: @radius@px; padding: 4px; selection-background-color: @selection@;
    selection-color: @selectionText@; outline: none; }
QCheckBox { spacing: 8px; }
QCheckBox::indicator { width: 14px; height: 14px; border-radius: @checkRadius@px; background: @check@; border: 1px solid @checkEdge@; }
QCheckBox::indicator:checked { background: @checkOn@; border-color: @checkOnEdge@; image: url(:/groomlab/icons/ui_check_light.svg); }
QCheckBox::indicator:disabled { background: @checkOff@; }
QCheckBox::indicator:checked:disabled { background: @hover@; border-color: @hover@; image: url(:/groomlab/icons/ui_check_light.svg); }
QCheckBox:disabled { color: @disabled@; }
QPushButton { background: @field@; border: 1px solid @fieldEdge@; border-radius: @radius@px; padding: 4px 12px; min-height: @buttonHeight@px; font-weight: 500; }
QPushButton:hover { background: @fieldHover@; }
QPushButton:pressed { background: @selection@; color: @selectionText@; }
QPushButton:focus { border: 2px solid @accent@; }
QPushButton:disabled { color: @disabled@; background: @fieldOff@; }
QSlider::groove:horizontal { height: 6px; border-radius: 3px; background: @track@; }
QSlider::sub-page:horizontal { background: @slider@; border-radius: 3px; }
QSlider::handle:horizontal { width: 14px; height: 14px; margin: -4px 0; border-radius: 7px; background: @text@; }
QScrollArea { border: none; }
QScrollBar:vertical { width: 8px; background: transparent; margin: 0; }
QScrollBar::handle:vertical { background: @scroll@; border-radius: 3px; min-height: 24px; margin: 1px 1px 1px 2px; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }
QScrollBar::handle:vertical:hover { background: @scrollHover@; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar:horizontal { height: 8px; background: transparent; margin: 0; }
QScrollBar::handle:horizontal { background: @scroll@; border-radius: 4px; min-width: 24px; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
QTreeWidget, QListWidget { background: @list@; border: 1px solid @fieldEdge@; border-radius: @radiusLarge@px; padding: 4px; outline: none; alternate-background-color: transparent; }
*[role="popup"] QListWidget, *[role="popup"] QTreeWidget { background: @field@; border: 1px solid @popupEdge@; }
QTreeWidget::item, QListWidget::item { padding: 3px; border-radius: @smallRadius@px; }
QTreeWidget::item:selected, QListWidget::item:selected { background: @selection@; color: @selectionText@; }
QTreeWidget::item:hover, QListWidget::item:hover { background: @fieldHover@; }
QHeaderView::section { background: transparent; color: @muted@; border: none; padding: 2px 4px; font-weight: 600; }
QTreeWidget::indicator { width: 14px; height: 14px; border-radius: @smallRadius@px; background: @check@; }
QTreeWidget::indicator:checked { background: @emerald@; image: url(:/groomlab/icons/ui_check_dark.svg); }
QTreeView::branch { background: transparent; }
)"
                                   R"(/* The Groom Tools (groom_tools.h), as Maya's: tiles, chips, the card, the tabs, sections. */
QLabel[role="heading"] { color: @muted@; font-size: @smallSize@px; font-weight: 600; letter-spacing: 0.4px; padding-top: 8px; border-top: 1px solid @edge@; }
QLabel[role="category"] { color: @dim@; font-size: @smallSize@px; font-weight: 600; padding: 0 0 2px 2px; }
QLabel[role="subheading"] { color: @dim@; font-size: @fontSize@px; font-weight: 500; padding-top: 8px; }
QLabel[role="about"] { color: @dim@; }
QLabel[role="hint"] { color: @muted@; font-size: @smallSize@px; }
QLabel[role="title"] { color: @text@; font-size: @titleSize@px; font-weight: 700; }
QLabel[role="status"] { color: @warning@; background: @field@; border-radius: @radius@px; padding: 5px 8px; }
QFrame[role="card"], QFrame[role="empty"] { background: @card@; border: none; border-radius: @cardRadius@px; }
/* A tray: a group of tiles (or a section) on a card, its title inside. */
QWidget[role="tray"] { background: @card@; border: none; border-radius: @cardRadius@px; }
QWidget[role="tray"] QLabel, QWidget[role="tray"] QCheckBox { background: transparent; }
QFrame[role="card"] QLabel, QFrame[role="card"] QCheckBox, QFrame[role="empty"] QLabel { background: transparent; }
QToolButton[role="tool"] { background: @toolRest@; border: 1px solid @toolEdge@; border-radius: @radius@px; padding: 4px 0 4px 0;
    color: @dim@; font-size: @tinySize@px; font-weight: 500; }
QFrame[role="tray"] QToolButton[role="tool"] { background: transparent; }
QToolButton[role="tool"][labels="true"] { background: @toolRestFill@; border-color: transparent; }
QToolButton[role="tool"]:hover { background: @fieldHover@; color: @text@; }
QToolButton[role="tool"]:checked, QFrame[role="tray"] QToolButton[role="tool"]:checked { background: @toolOn@; border-color: @toolOnEdge@; color: @toolOnText@; }
QToolButton[role="tool"]:disabled { color: @disabled@; }
QToolButton[role="tool"]:pressed:!checked, QFrame[role="tray"] QToolButton[role="tool"]:pressed:!checked, QWidget[role="tray"] QToolButton[role="tool"]:pressed:!checked { background: @hover@; }
QWidget[role="tray"] QToolButton[role="tool"]:hover:!checked { background: @fieldHover@; color: @text@; }
QToolButton[role="chip"] { background: @field@; border: 1px solid @fieldEdge@; border-radius: @radius@px; padding: 4px 8px; font-weight: 500; }
QToolButton[role="chip"]:hover { background: @fieldHover@; }
QToolButton[role="chip"]:pressed, QToolButton[role="chip"]:checked { background: @accentSoft@; color: @accentInk@; border-color: @accentSoft@; font-weight: 600; }
QToolButton[role="chip"]:disabled { color: @disabled@; }
QToolButton[role="chip"]::menu-indicator, QToolButton[role="glyph"]::menu-indicator { image: none; width: 0; }
QToolButton[role="glyph"], QToolButton[role="glyph"]:checked { background: transparent; border: none; border-radius: @radius@px; padding: 2px; }
QToolButton[role="glyph"]:hover { background: @hover@; }
QToolButton[role="glyph"]:checked { background: @selection@; }
QToolButton[role="glyph"]:disabled, QToolButton[role="glyph"]:checked:disabled { background: transparent; }
QToolButton[role="glyph"][armed="true"] { color: @warning@; padding: 2px 6px; }
/* The tool card (brush_panel.h): blocks under a faint rule, small semibold dim titles, chips that wrap. */
QFrame[role="toolCard"] { background: @card@; border: none; border-radius: @cardRadius@px; }
QFrame[role="block"] { background: transparent; border: none; border-top: 1px solid @cardRule@; border-radius: 0; }
QFrame[role="toolCard"] QLabel, QFrame[role="block"] QLabel { background: transparent; }
QLabel[role="blockTitle"] { color: @dim@; font-size: @smallSize@px; font-weight: 600; }
QLabel[role="chipLabel"] { color: @muted@; font-size: @smallSize@px; }
QLabel[role="rowLabel"] { color: @dim@; }
QFrame[role="toolCard"] QToolButton[role="chip"], QFrame[role="itemBox"] QToolButton[role="chip"], #paintControls QToolButton[role="chip"],
#photosPanel QToolButton[role="chip"] {
    background: @field@; border: 1px solid transparent; color: @dim@; padding: 3px 8px; font-weight: 500; font-size: @smallSize@px; }
QFrame[role="toolCard"] QToolButton[role="chip"]:hover, QFrame[role="itemBox"] QToolButton[role="chip"]:hover, #paintControls QToolButton[role="chip"]:hover,
#photosPanel QToolButton[role="chip"]:hover { background: @hover@; color: @text@; }
QFrame[role="toolCard"] QToolButton[role="chip"]:checked, QFrame[role="itemBox"] QToolButton[role="chip"]:checked, #paintControls QToolButton[role="chip"]:checked,
#photosPanel QToolButton[role="chip"]:checked { background: @accentSoft@; border-color: @accentSoft@; color: @accentInk@; font-weight: 600; }
/* A button that opens something (an editor below, a menu): outlined, never a toggle's fill. */
QPushButton[role="disclosure"] { background: transparent; border: 1px solid @hover@; color: @dim@; padding: 4px 10px; font-weight: 500; }
QPushButton[role="disclosure"]:hover, QPushButton[role="disclosure"]:pressed { background: @field@; color: @text@; }
QPushButton[role="disclosure"]::menu-indicator { image: none; width: 0; }
QToolButton[role="ghost"][outlined="true"] { border: 1px solid @hover@; border-radius: @radius@px; padding: 2px 20px 2px 8px; color: @soft@; }
QToolButton[role="ghost"][outlined="true"]:hover { background: @field@; color: @text@; }
QToolButton[role="ghost"][outlined="true"]::menu-button { border: none; border-left: 1px solid @hover@; width: 16px; }
QToolButton[role="chip"][disclosure="true"], QFrame[role="toolCard"] QToolButton[role="chip"][disclosure="true"] {
    background: transparent; border: 1px solid @hover@; color: @dim@; }
QToolButton[role="chip"][disclosure="true"]:hover, QFrame[role="toolCard"] QToolButton[role="chip"][disclosure="true"]:hover {
    background: @field@; color: @text@; }
QToolButton[role="chip"][disclosure="true"]:checked, QFrame[role="toolCard"] QToolButton[role="chip"][disclosure="true"]:checked {
    background: @field@; border-color: @dim@; color: @text@; font-weight: 500; }
/* A one-of choice (segmentRow): segments on a sunken track, the chosen one raised in a neutral tone. */
QWidget[role="segTrack"] { background: @sunken@; border-radius: 8px; }
QWidget[role="segTrack"] QToolButton[role="chip"], QFrame[role="toolCard"] QWidget[role="segTrack"] QToolButton[role="chip"],
#paintControls QWidget[role="segTrack"] QToolButton[role="chip"] { background: transparent; border: 1px solid transparent; color: @dim@; border-radius: 6px; padding: 3px 2px; }
QWidget[role="segTrack"] QToolButton[role="chip"]:hover, QFrame[role="toolCard"] QWidget[role="segTrack"] QToolButton[role="chip"]:hover,
#paintControls QWidget[role="segTrack"] QToolButton[role="chip"]:hover { background: transparent; color: @text@; }
QWidget[role="segTrack"] QToolButton[role="chip"]:checked, QFrame[role="toolCard"] QWidget[role="segTrack"] QToolButton[role="chip"]:checked,
#paintControls QWidget[role="segTrack"] QToolButton[role="chip"]:checked { background: @segOn@; border-color: @segOn@; color: @text@; font-weight: 600; }
QPushButton[role="ghost"], QToolButton[role="ghost"] { background: transparent; border: 1px solid transparent; color: @soft@; padding: 2px 6px;
    min-height: 0; font-weight: 500; }
QPushButton[role="ghost"]:hover, QToolButton[role="ghost"]:hover { background: @hover@; color: @text@; }
QPushButton[role="ghost"]:disabled, QToolButton[role="ghost"]:disabled { color: @disabled@; background: transparent; }
QPushButton[role="ghost"]::menu-indicator { image: none; width: 0; }
QToolButton[role="ghost"][popupMode="1"] { padding-right: 16px; }
QToolButton[role="ghost"]::menu-button { border: none; background: transparent; width: 14px; }
QToolButton[role="ghost"]::menu-arrow { image: url(:/groomlab/icons/ui_chevron_down.svg); width: 9px; height: 9px; }
/* The tabs' lists (over the card): a caps header, plain rows, the picked item's box. */
QLabel[role="listTitle"] { color: @text@; font-size: @fontSize@px; font-weight: 600; }
QLabel[role="listCount"] { color: @muted@; font-size: @smallSize@px; }
QWidget[role="listHeader"] { border: none; }
QTreeWidget[role="rows"], QListWidget[role="rows"] { background: transparent; border: none; padding: 0; }
QTreeWidget[role="rows"]::item, QListWidget[role="rows"]::item { padding: 3px 2px; }
QTreeWidget[role="rows"]::item { border-radius: 0; margin: 0; }
QListWidget[role="rows"]::item { border-radius: @smallRadius@px; }
QTreeWidget[role="rows"]::item:selected, QListWidget[role="rows"]::item:selected { background: @accentSoft@; color: @text@; }
QTreeWidget[role="rows"]::item:hover:!selected, QListWidget[role="rows"]::item:hover:!selected { background: @cardRule@; }
QFrame[role="itemBox"] { background: @card@; border: none; border-radius: @cardRadius@px; }
QWidget[role="tray"] QFrame[role="itemBox"] { background: transparent; border: none; border-top: 1px solid @cardRule@; border-radius: 0; }
QFrame[role="itemBox"] QLabel { background: transparent; }
/* The menu bar's corner (MainWindow::buildMenuCorner): the file name, Render (ghost), Export (the accent). */
QLabel[role="cornerName"] { color: @muted@; font-size: @smallSize@px; }
QToolButton[role="cornerGhost"], QToolButton[role="cornerPrimary"] { border: 1px solid transparent; border-radius: @radius@px; padding: 2px 10px;
    font-weight: 500; min-height: 18px; }
QToolButton[role="cornerGhost"] { background: transparent; color: @soft@; }
QToolButton[role="cornerGhost"]:hover, QToolButton[role="cornerGhost"]:pressed { background: @hover@; color: @text@; }
QToolButton[role="cornerPrimary"] { background: @accentFill@; color: @accentText@; border-color: @accentFill@; }
QToolButton[role="cornerPrimary"]:hover { background: @accent@; border-color: @accent@; }
QToolButton[role="cornerGhost"]::menu-indicator, QToolButton[role="cornerPrimary"]::menu-indicator { image: none; width: 0; }
QPushButton[role="primary"] { background: @accentFill@; color: @accentText@; border-color: @accentFill@; font-weight: 600; }
QPushButton[role="primary"]:hover { background: @accent@; border-color: @accent@; }
QPushButton[role="primary"]:disabled { background: @field@; color: @disabled@; border-color: @field@; }
QPushButton[role="section"], QPushButton[role="section"]:checked, QPushButton[role="section"]:pressed {
    background: transparent; border: none; border-radius: 0; color: @text@; font-size: @fontSize@px;
    font-weight: 600; text-align: left; padding: 6px 0 2px 0; min-height: 0; }
QPushButton[role="section"]:hover { color: @accentInk@; background: transparent; }
QPushButton[role="step"] { background: transparent; border: none; text-align: left; padding: 3px 4px; font-weight: 600; }
QPushButton[role="step"]:hover { background: @hover@; }
QPushButton[role="step"][done="true"] { color: @muted@; }
#groomTools QPushButton { padding: 4px 8px; }
#groomTools QPushButton[role="section"] { padding: 6px 0 2px 0; }
#groomTools QPushButton[role="step"] { padding: 3px 4px; }
/* The Properties dock (properties_panel.h), as Maya's Attribute Editor's folds: each section a card, its title inside, a label column. */
QWidget[role="propCard"] { background: @card@; border-radius: @cardRadius@px; }
QPushButton[role="propSection"], QPushButton[role="propSection"]:checked, QPushButton[role="propSection"]:pressed {
    background: transparent; border: none; border-radius: 0; color: @text@; font-size: @fontSize@px;
    font-weight: 600; text-align: left; padding: 9px 2px 9px 2px; min-height: 0; }
QPushButton[role="propSection"]:!checked { color: @dim@; }
QPushButton[role="propSection"]:hover { color: @text@; background: transparent; }
QPushButton[role="propSubsection"], QPushButton[role="propSubsection"]:checked, QPushButton[role="propSubsection"]:pressed {
    background: transparent; border: none; color: @dim@; font-size: @fontSize@px; font-weight: 500; text-align: left; padding: 8px 2px 4px 2px; }
QPushButton[role="propSubsection"]:hover { color: @text@; background: transparent; }
QLabel[role="propLabel"] { color: @dim@; }
/* A control's row as one field (properties_panel.cpp): the label inside at its left, the control at its right, flat in it. */
QFrame[role="propField"] { background: @field@; border: 1px solid @fieldEdge@; border-radius: @radius@px; }
QWidget[role="fieldList"] { background: @field@; border: 1px solid @fieldEdge@; border-radius: @radius@px; }
QWidget[role="fieldList"] QLabel { background: transparent; }
QFrame[role="emptyQuiet"] { background: transparent; border: none; }
QFrame[role="emptyQuiet"] QLabel { background: transparent; }
QFrame[role="propField"] QLabel { background: transparent; }
QFrame[role="propField"] QLabel[role="propLabel"] { color: @dim@; }
QFrame[role="propField"] QComboBox, QFrame[role="propField"] QLineEdit, QFrame[role="propField"] QAbstractSpinBox { background: transparent; border: 1px solid transparent; }
QFrame[role="propField"] QComboBox { padding-left: 0px; }
QFrame[role="propField"] QComboBox:hover, QFrame[role="propField"] QLineEdit:hover, QFrame[role="propField"] QAbstractSpinBox:hover { background: @hover@; border: 1px solid transparent; }
QFrame[role="propField"] QComboBox:focus, QFrame[role="propField"] QLineEdit:focus, QFrame[role="propField"] QAbstractSpinBox:focus { border: 2px solid @accent@; }
QFrame[role="propField"] QPushButton { background: @hover@; border: none; }
QLabel[role="keycap"] { background: @field@; color: @text@; border: 1px solid @edge@; border-bottom: 2px solid @background@; border-radius: 4px; padding: 0 6px; font-size: @smallSize@px; font-weight: 500; }
QLabel[role="keyOr"] { color: @muted@; font-size: @smallSize@px; }
QLabel[role="keyAction"] { color: @dim@; }
QLabel[role="keyGroup"] { color: @text@; font-size: @fontSize@px; font-weight: 600; padding-top: 10px; }
QWidget[role="tray"] QLabel[role="keyGroup"] { padding: 0 0 4px 0; }
QLabel[role="propLabel"]:disabled { color: @disabled@; }
QFrame[role="propRule"] { background: @cardRule@; border: none; }
)");
    // The theme's tones into the sheet: a glass theme's white washes (over the viewport or a dock),
    // a solid theme's raised fields with a 1 px edge and smaller radii (compact, flat).
    const bool glass = glassy();
    const QColor selection = uiColor("selection"), selectionText = uiColor("selectionText"), edge = glassEdge();
    const QColor border = uiColor("border");
    const QString edgeName = edge.isValid() ? edge.name() : border.isValid() ? border.name() : QStringLiteral("#2E2E2E");
    const int fontSize = theme::fontSize();
    const auto wash = [](int alphaPercent) { return QStringLiteral("rgba(255,255,255,%1)").arg(alphaPercent / 100.0); };
    const std::vector<std::pair<QString, QString>> tokens = {
        {QStringLiteral("@accentText@"), uiColor("accentText").name()},
        {QStringLiteral("@accentFill@"), accentFill().name()},
        {QStringLiteral("@accent@"), uiColor("accent").name()},
        {QStringLiteral("@disabled@"), textDisabled().name()},
        {QStringLiteral("@hoverEdge@"), glass ? wash(20) : towardText(uiColor("hover"), 0.12).name()},
        {QStringLiteral("@selectionText@"), (selectionText.isValid() ? selectionText : uiColor("accentText")).name()},
        {QStringLiteral("@selection@"), (selection.isValid() ? selection : uiColor("accent")).name()},
        {QStringLiteral("@slider@"), (glass ? uiColor("accent") : sliderFill(uiColor("accent"))).name()},
        // The tool tiles: a glass theme's washed tiles, the accent when on; a solid theme's icons
        // on the panel, the chosen one on the selection tone with an accent edge.
        {QStringLiteral("@toolRest@"), glass ? wash(8) : QStringLiteral("transparent")},
        {QStringLiteral("@toolEdge@"), QStringLiteral("transparent")},
        {QStringLiteral("@toolOnText@"), (glass ? uiColor("accentText") : text()).name()},
        {QStringLiteral("@toolOn@"), glass ? uiColor("accent").name() : accentSoft().name()},
        {QStringLiteral("@uiFamily@"), uiFamily()},
        {QStringLiteral("@fieldHeight@"), QString::number(rowHeight() - 8)},
        {QStringLiteral("@controlInner@"), QString::number(rowHeight() - 2)},
        {QStringLiteral("@buttonHeight@"), QString::number(rowHeight() - 10)},
        {QStringLiteral("@background@"), backdrop().name()},
        {QStringLiteral("@panel@"), uiColor("panel").name()},
        {QStringLiteral("@hover@"), uiColor("hover").name()},
        {QStringLiteral("@edge@"), edgeName},
        {QStringLiteral("@groupEdge@"), glass ? wash(10) : edgeName},
        // A light theme's fields keep a hairline (ui.fieldEdge) to stand off its light cards; else none.
        {QStringLiteral("@fieldEdge@"), uiColor("fieldEdge").isValid() && !glass ? uiColor("fieldEdge").name() : QStringLiteral("transparent")},
        {QStringLiteral("@fieldHover@"), glass ? wash(14) : uiColor("hover").name()},
        {QStringLiteral("@fieldOff@"), glass ? wash(4) : uiColor("panel").name()},
        {QStringLiteral("@field@"), glass ? wash(8) : uiColor("raised").name()},
        {QStringLiteral("@list@"), glass ? wash(5) : backdrop().name()},
        {QStringLiteral("@checkEdge@"), glass ? wash(14) : mix(uiColor("hover"), textMuted(), 0.8).name()},
        {QStringLiteral("@popupEdge@"), glass ? wash(16) : (uiColor("popupEdge").isValid() ? uiColor("popupEdge") : towardText(uiColor("raised"), 0.18)).name()},
        {QStringLiteral("@toolRestFill@"), glass ? wash(8) : QStringLiteral("transparent")},
        {QStringLiteral("@toolOnEdge@"), glass ? uiColor("accent").name() : mix(accentSoft(), uiColor("accent"), 0.45).name()},
        {QStringLiteral("@checkOff@"), glass ? wash(5) : uiColor("panel").name()},
        {QStringLiteral("@check@"), glass ? wash(10) : uiColor("raised").name()},
        {QStringLiteral("@track@"), glass ? wash(12) : uiColor("hover").name()},
        {QStringLiteral("@scrollHover@"), glass ? wash(30) : towardText(uiColor("hover"), 0.45).name()},
        {QStringLiteral("@scroll@"), glass ? wash(18) : towardText(uiColor("hover"), 0.25).name()},
        {QStringLiteral("@radiusLarge@"), QString::number(panelRadius())},
        {QStringLiteral("@radius@"), QString::number(controlRadius())},
        {QStringLiteral("@smallRadius@"), QString::number(smallRadius())},
        {QStringLiteral("@fontSize@"), QString::number(fontSize)},
        {QStringLiteral("@smallSize@"), QString::number(fontSize - 1)},
        {QStringLiteral("@tinySize@"), QString::number(std::max(11, fontSize - 2))},
        {QStringLiteral("@titleSize@"), QString::number(glass ? fontSize + 1 : fontSize + 3)},
        // The check mark in the accent's text color (a light accent: a dark tick).
        {QStringLiteral("@checkMark@"), uiColor("accentText").lightness() < 128 ? QStringLiteral(":/groomlab/icons/ui_check_dark.svg")
                                                                                 : QStringLiteral(":/groomlab/icons/ui_check_light.svg")},
        {QStringLiteral("@cardRule@"), glass ? wash(8) : mix(card(), uiColor("hover"), 0.6).name()},
        {QStringLiteral("@segOn@"), glass ? wash(20) : segmentOn().name()},
        {QStringLiteral("@checkRadius@"), QString::number(checkRadius())},
        // A checked box: the accent filled with the light tick on a light theme (a pale wash would hide it), the soft accent on a dark one.
        {QStringLiteral("@checkOnEdge@"), glass ? uiColor("accent").name() : (isLight() ? accentFill() : uiColor("accent")).name()},
        {QStringLiteral("@checkOn@"), glass ? wash(30) : (isLight() ? accentFill() : accentSoft()).name()},
        {QStringLiteral("@cardRadius@"), QString::number(cardRadius())},
        {QStringLiteral("@card@"), glass ? wash(4) : card().name()},
        {QStringLiteral("@sunken@"), glass ? wash(4) : sunken().name()},
        {QStringLiteral("@accentSoft@"), glass ? wash(20) : accentSoft().name()},
        {QStringLiteral("@accentInk@"), glass ? text().name() : accentInk().name()},
        {QStringLiteral("@accentEdge@"), glass ? wash(30) : mix(accentSoft(), uiColor("accent"), 0.45).name()},
        {QStringLiteral("@warning@"), warningText().name()},
        {QStringLiteral("@text@"), text().name()},
        {QStringLiteral("@soft@"), textSoft().name()},
        {QStringLiteral("@dim@"), textDim().name()},
        {QStringLiteral("@muted@"), textMuted().name()},
        {QStringLiteral("@emerald@"), emerald().name()},
    };
    for (const auto& [token, color] : tokens) sheet.replace(token, color);
    return sheet;
}

QSize fitToAvailable(QSize wanted, const QRect& available, double share, QSize minimum) {
    if (available.isEmpty()) return wanted;
    const int maxW = std::max(1, static_cast<int>(available.width() * share));
    const int maxH = std::max(1, static_cast<int>(available.height() * share));
    const int w = std::min(wanted.width(), maxW), h = std::min(wanted.height(), maxH);
    return {std::min(std::max(w, minimum.width()), available.width()), std::min(std::max(h, minimum.height()), available.height())};
}

std::vector<QColor> namedColors() {
    std::vector<QColor> out = {red(),        yellow(),         blue(),          emerald(),     purple(),       white(),       glass(),
                               glassSolid(), glassPanel(),     glassEdge(),     text(),        textSoft(),     textDim(),     textMuted(),
                               track(),      chip(),           chipSoft(),      chipChecked(), divider(),      renderRed(),   backdrop(),
                               blueLight(),  redLight(),       emeraldSoft(),   emeraldChip(), errorText(),    warningText(), linkText(),
                               windowButton(), windowButtonHover(), nodeBody(), graphCanvas(), graphGrid(),   nodeBorder(),  nodeSelected(),
                               graphError(), graphWarning(),   graphNote(),     graphDriven(), graphReads(),   graphWrites()};
    for (const char* key : {"panel", "background", "raised", "hover", "border", "outline", "accent", "accentText", "text", "textDim",
                            "error", "warning", "selection", "selectionText", "field", "card"}) {
        out.push_back(uiColor(key));
    }
    for (const BrushCategory c : groomlab::brushCategories()) {
        out.push_back(categoryColor(c));
        out.push_back(categoryIconColor(c));
    }
    for (const char* c : {"source", "grow", "shape", "look", "field", "utility", "color", "motion", "feathers"}) out.push_back(nodeCategoryColor(c));
    for (const char* p : {"strands", "guides", "mesh", "field", "curve", "curves"}) out.push_back(portColor(p));
    return out;
}
}  // namespace groomlab::app::theme
