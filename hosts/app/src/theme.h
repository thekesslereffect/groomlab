// SPDX-License-Identifier: Apache-2.0
#pragma once

// The app's look: GroomLab's themes (hosts/maya/themes/*.json; Charcoal by default: calm neutral
// greys, flat, thin borders, as the Maya host wears it) in the standalone app's docked window
// (app_shell.h: menu bar, tool bar, solid docks, clean and dark like Plasticity). Glass themes
// (Playful, Polar) keep their translucent pieces over the viewport; every theme's docks are solid.
// This module holds the colors, the fonts (Inter for the words and, with tabular digits, the
// numbers; JetBrains Mono for code, hex colors and paths; bundled as resources), the sizes, the viewport's overlay colors (displayTheme), the layout of what stays
// over the viewport (pure geometry, so the tests check it without a window), the brush tiles'
// order and the Qt style sheet the window's widgets take. No widgets here: the painted ones are
// in glass.h.

#include <string>
#include <vector>

#include "groomlab/brush_catalog.h"
#include "groomlab/display.h"

#include <QColor>
#include <QFont>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>

namespace groomlab::app::theme {

// ---- The theme ------------------------------------------------------------------------------------
// The shipped themes are embedded at build time (resources :/groomlab/themes/<name>.json, the
// Maya host's files); the user's are files in the folders of GROOMLAB_THEME_PATH and the user's
// themes folder (userThemeDir), looked up in that order before the embedded ones, as the Maya
// host's theme.py does. One is current for the whole app: main() sets the saved one (or --theme's)
// before the window is built, and GroomLab > Theme switches it live (MainWindow::restyle: the
// window's style sheet made again, the widgets' own sheets given the new colors, everything
// repainted; the painted widgets read the theme when they paint).
QStringList themeNames();                  // "charcoal", "playful", "polar", "studio" and the user's (sorted)
QString themeLabel(const QString& name);   // "Charcoal"
QString defaultTheme();                    // "charcoal" (themes without "extends" still extend Playful)
QString currentTheme();                    // defaultTheme() until setTheme
bool setTheme(const QString& name);        // false (nothing changes) for a theme there is no file of
QString savedTheme();                      // QSettings ui/theme, defaultTheme() when unset or unknown
void saveTheme(const QString& name);
bool glassy();  // the theme's ui.glass (default true): translucent panels, else solid ones with an edge
// The user's themes folder: GROOMLAB_USER_THEMES, else <the Maya user folder>/groomlab/themes (the
// user presets' sibling, where the Maya host keeps them); "" when there is no home folder.
QString userThemeDir();
QStringList themeSearchDirs();             // GROOMLAB_THEME_PATH's folders, then userThemeDir()
QString themeFile(const QString& name);    // the file a theme is read from (a resource path when embedded); "" none
void reloadTheme();                        // the files read again (Theme > Reload Theme, after editing one)
// Theme > New Theme from Current: the current theme filled in (no "extends", its "name" set) as
// <userThemeDir>/<name>.json, to edit; *path the file. "" or why not (a bad name, one there already).
QString createTheme(const QString& name, QString* path = nullptr);
// Every color the theme gives the widgets (the functions below, the ui keys, the categories and
// ports), in one fixed order: what restyle maps the old theme's colors to the new one's by.
std::vector<QColor> namedColors();

// ---- Colors (the palette, then the glass and text tones) -----------------------------------------
// The palette, the UI tones, the graph's and the icons' colors are read from the current theme's
// file, so the Maya host and the app share one theme file. A glass theme keeps the mockup's own
// glass and text tones (tuned for translucency over the viewport); a solid one takes them from
// its ui section (panel, border, text, textDim).
QColor red();
QColor yellow();
QColor blue();
QColor emerald();
QColor purple();
QColor white();
QColor glass();        // the floating panels: rgba(31,32,37,0.72); solid: ui.panel
QColor glassSolid();   // small labels that must stay readable: rgba(31,32,37,0.86); solid: ui.panel
QColor glassPanel();   // a working panel, a little more opaque: rgba(31,32,37,0.93); solid: ui.panel
QColor glassEdge();    // the solid panels' thin border (ui.outline, else ui.border); invalid for glass
QColor text();         // #F3F1EC; solid: ui.text
QColor textSoft();     // #D9D5CD, the inactive mode names
QColor textDim();      // #B8B4AC, slider captions
QColor textMuted();    // #8F8B84, shortcuts and counts; solid: ui.textDim
QColor track();        // a slider's unfilled track: white at 12%; solid: ui.hover
QColor chip();         // a small button at rest: white at 8%; solid: ui.raised
QColor chipSoft();     // a quieter one (More settings): white at 6%; solid: ui.raised
QColor chipChecked();  // a glass button checked (the Graph pill): white at 18%; solid: ui.hover
QColor divider();      // white at 12%; solid: the edge (ui.outline, else ui.border)
QColor renderRed();    // the Render pill: #E5383B; solid: the accent (the primary action)
QColor backdrop();     // the viewport's default clear color: #17181B; solid: ui.background
QColor ink();          // dark text on a bright fill: #1B1A17
QColor blueLight();    // icons tinted by category on glass: lighter than the fill colors
QColor redLight();
QColor emeraldSoft();  // the Sym X chip's text when on
QColor emeraldChip();  // ... and its fill
QColor readableOn(const QColor& fill);  // ink on a bright fill, white on a dark one
// A segmented control's chosen item: its own colors on glass; a solid theme's selection tone.
QColor segmentFill(const QColor& own);
QColor segmentText(const QColor& own);
// A checked or pressed chip (a toggle, not the primary action): its own colors on glass; on a
// solid theme the selection tone (ui.selection) and its text (ui.selectionText).
QColor checkedFill(const QColor& own);
QColor checkedText(const QColor& own);
// A slider's fill, as the Maya host's ui.sliders: a theme with one slider color (Charcoal's
// silver, Polar's and Studio's accent) gives every slider that color; a cycle (Playful's four
// primaries) keeps the color the slider asks for.
QColor sliderFill(const QColor& own);
// A filled action button, the primary action (chrome, not meaning): its own colors on glass; on
// a solid theme the accent pill and the accent's text.
QColor actionFill(const QColor& own);
QColor actionText(const QColor& own);
// Words that say something went wrong or needs a look (an export's failure, a node's error
// line): the theme's ui.error and ui.warning, readable on the panels.
QColor errorText();
QColor warningText();
QColor linkText();           // a link in rich text (About, the update notice)
QColor windowButton();       // a window's plain button (Transfer, Guides from Photos): ui.raised
QColor windowButtonHover();  // ... hovered: ui.hover
// The theme's "ui" color of that key ("panel", "hover", "accent", "accentText"...); invalid for
// an unknown one.
QColor uiColor(const std::string& key);

// ---- Fonts -------------------------------------------------------------------------------------
// Registers the bundled fonts once (idempotent); false when the resources are missing, in
// which case uiFont falls back to the system's sans-serif.
bool loadFonts();
// The theme's ui.font ("Inter", bundled, when the theme names none): the one place the UI's
// family is named (the style sheet's too).
QString uiFamily();
QString monoFamily();  // "JetBrains Mono"
// A UI font of `pixelSize` and `weight` (QFont::Weight: 400 regular ... 800 extra bold).
QFont uiFont(int pixelSize, int weight = QFont::Medium);
// Mono: what is typed as code or a name (hex colors, expressions, file paths and templates).
QFont monoFont(int pixelSize, int weight = QFont::Medium);
// A number shown in the UI (slider values, stats, frames, timings): the UI font with tabular
// digits (OpenType "tnum"), so a changing value keeps its width.
QFont numberFont(int pixelSize, int weight = QFont::Medium);

// ---- Sizes ---------------------------------------------------------------------------------------
// The theme's ui.radius (controls: fields, buttons, tiles; a glass theme's no more than 8), a
// smaller one (check boxes, list rows: radius - 2, at least 2) and the panels' and cards'
// (radius + 2); ui.rowHeight, every control's height (26 unless the theme says).
int controlRadius();
int smallRadius();
int panelRadius();
int rowHeight();

// ---- Brushes --------------------------------------------------------------------------------------
// The rail's tools are the core's brush catalog (brush_catalog.h: the ids, labels, categories
// and hints both hosts share), colored as playful.json's icons are (create emerald, shape blue,
// trim red, paint yellow; the guide tools, Select and Freeze, white).
using BrushCategory = groomlab::BrushCategory;
struct BrushEntry {
    std::string name;  // the mode BrushToolSettings::setMode takes
    std::string icon;  // hosts/app/icons/groomLab_<icon>.svg
    std::string tip;   // "Comb: drag guide points along the stroke..."
    BrushCategory category;
};
// Every brush mode in the catalog's order, grouped by category (a divider between groups): the
// catalog without Select (the rail's own tool). Freeze is BrushSession::setTool's ("freeze").
const std::vector<BrushEntry>& brushes();
const BrushEntry* brush(const std::string& name);  // null for an unknown mode
QColor categoryColor(BrushCategory category);      // the fill (the active tool, the ring)
QColor categoryIconColor(BrushCategory category);  // the tint of an inactive tool's icon
// A tool icon's two tones, as the Maya host recolors the SVGs (widgets.icon_colors): its group's
// color (icons.create/shape/trim/paint; the guide tools Select and Freeze take Create's and
// Paint's, as Maya groups them) and the ink (icons.ink: hair and neutral parts).
QColor toolIconColor(BrushCategory category, const std::string& icon);
QColor iconInk();
QString brushTitle(const std::string& name);       // the catalog's label ("comb" -> "Comb"); a word capitalized otherwise

// ---- The node graph ------------------------------------------------------------------------------
// A node header's color by the operator's category ("source", "grow", "shape", "look",
// "field", "utility", "color", "motion", "feathers"; "" utility), as playful.json's graph section.
QColor nodeCategoryColor(const std::string& category);
// A port's color by its type name ("strands", "guides", "mesh", "field", "curve", "curves").
QColor portColor(const std::string& type);
QColor nodeBody();  // #2A2C31 (opaque)
// The Graph Editor's opaque canvas and its grid (graph.canvas, graph.grid; else the backdrop and
// a lighter line).
QColor graphCanvas();
QColor graphGrid();
QColor nodeBorder();       // graph.nodeBorder: drawn when it differs from the body
QColor nodeSelected();     // graph.selected: the selected node's ring and wire
// The graph's status colors, as Maya's Graph Editor draws them: a failing node's badge
// (graph.error), a warning, a note (it does nothing as set), a param driven by a port, and the
// attribute chips (a param that reads a strand attribute, one that writes it).
QColor graphError();
QColor graphWarning();
QColor graphNote();
QColor graphDriven();
QColor graphReads();
QColor graphWrites();
qreal nodeRadius();        // graph.cornerRadius (14 in Playful)
// graph.headerStyle: "tinted" (a wash and an accent bar), "minimal" (no header fill, a category
// dot by the title) or "filled" (the default: the header in the category color).
QString headerStyle();
bool tintedHeaders();      // headerStyle() == "tinted"
// graph.canvasStyle: "lines" (the default: a grid of lines), "dots" (a dot at every grid
// crossing) or "plain"; drawn in graph.grid.
QString canvasStyle();

// ---- The viewport's overlay ------------------------------------------------------------------------
// The theme's viewport section (guide levels, selection, frozen, parts, flow, tubes...) as the
// core's DisplayTheme: linear RGB, palette names resolved, as the Maya host's theme.apply_viewport
// sends groomLabDisplayTheme. Missing keys take Maya's fallbacks.
DisplayTheme displayTheme();

// ---- Layout --------------------------------------------------------------------------------------
// The look card's width.
struct OverlayMetrics {
    int lookCardWidth = 272;
};
// What stays over the viewport once the window has docks (overlay.h): the active tool's name
// (top-left), Undo and Redo (top-right), the stats (bottom-left, Groom) and the refine line
// (bottom-right, Look), the toast above them, the prompt and the empty-state card centered. Each
// size is the piece's hint; the rects stay inside the view and never meet each other (except the
// prompt and the card, which sit over the rest).
struct ViewportOverlayRequest {
    QSize view;
    QSize toolLabel;  // empty: none
    QSize history;    // Undo and Redo (top-right)
    QSize stats;
    QSize refine;
    QSize toast;
    QSize prompt;
    QSize emptyCard;
};
struct ViewportOverlayLayout {
    QRect toolLabel, history, stats, refine, toast, prompt, emptyCard;
};
ViewportOverlayLayout layoutViewportOverlay(const ViewportOverlayRequest& request, int margin = 12);

// ---- Windows on small screens ---------------------------------------------------------------------
// A window's size for a screen: `wanted`, no more than `share` of the screen's usable area
// (QScreen::availableGeometry: logical pixels, so a 1920x1080 display at 150% text is 1280x720
// less the taskbar), and never below `minimum` unless the screen itself is smaller.
QSize fitToAvailable(QSize wanted, const QRect& available, double share = 0.92, QSize minimum = QSize(640, 400));
// (glass.h fitWindowToScreen applies it to a window.)

// ---- The style sheet ------------------------------------------------------------------------------
// The window's widgets: the menu bar, the tool bar, the docks (solid, a 1 px edge, compact titles),
// tab bars and splitters, and the ordinary widgets inside them (spin boxes, combos, check boxes,
// trees, scroll bars, menus, tool tips): flat, the theme's tones (a solid theme's raised fields,
// small radii; a glass theme's washes), the accent for what is on.
QString styleSheet();

}  // namespace groomlab::app::theme
