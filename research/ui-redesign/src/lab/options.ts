// SPDX-License-Identifier: Apache-2.0
// Every design choice the lab can flip. To try a new idea:
//   1. add a choice to a dimension below (or a new dimension),
//   2. branch on it where it matters: useOpt("tools") === "list" (or read the data-* attribute in CSS),
//   3. optionally add it to a direction (curated combo) at the bottom.
// Choices that cannot be built with Qt widgets + QSS (or a small custom-painted widget) don't belong here.

export type Choice = { id: string; label: string; hint?: string };
export type Dimension = { id: string; label: string; group: string; choices: Choice[]; qt?: string };

export const dimensions = [
  // ---- Look ----
  {
    id: "theme", label: "Theme", group: "Look",
    qt: "A theme JSON in hosts/maya/themes (ui keys); the app's QSS template reads it",
    choices: [
      { id: "charcoal", label: "Charcoal", hint: "Today's default, neutral greys" },
      { id: "graphite", label: "Graphite", hint: "Cool blue-grey, softer contrast" },
      { id: "carbon", label: "Carbon", hint: "Near-black, zinc, crisp" },
      { id: "umber", label: "Umber", hint: "Warm greys, easy on long sessions" },
      { id: "paper", label: "Paper", hint: "Light theme" },
    ],
  },
  {
    id: "accent", label: "Accent", group: "Look",
    qt: "ui.accent / ui.accentText",
    choices: [
      { id: "mono", label: "Mono", hint: "Off-white, today's look" },
      { id: "blue", label: "Blue" },
      { id: "emerald", label: "Emerald" },
      { id: "amber", label: "Amber" },
      { id: "violet", label: "Violet" },
      { id: "coral", label: "Coral" },
    ],
  },
  {
    id: "font", label: "Font", group: "Look",
    qt: "QFontDatabase::addApplicationFont, bundled TTF",
    choices: [
      { id: "figtree", label: "Figtree", hint: "Today's font" },
      { id: "inter", label: "Inter" },
      { id: "geist", label: "Geist" },
      { id: "system", label: "Segoe UI" },
    ],
  },
  {
    id: "numbers", label: "Numbers", group: "Look",
    qt: "QFont::setFeature(\"tnum\") (Qt 6.7+) or a mono font",
    choices: [
      { id: "tabular", label: "Tabular sans", hint: "Same font, fixed-width digits" },
      { id: "mono", label: "Mono", hint: "JetBrains Mono, today's look" },
    ],
  },
  {
    id: "density", label: "Density", group: "Look",
    qt: "min-height / padding / font-size in the QSS template",
    choices: [
      { id: "compact", label: "Compact", hint: "22px rows, 11px text" },
      { id: "default", label: "Default", hint: "26px rows, 12px text" },
      { id: "comfy", label: "Comfy", hint: "30px rows, 13px text" },
    ],
  },
  {
    id: "radius", label: "Corners", group: "Look",
    qt: "ui.radius → border-radius",
    choices: [
      { id: "0", label: "Square" },
      { id: "4", label: "4px" },
      { id: "6", label: "6px" },
      { id: "10", label: "10px" },
    ],
  },

  // ---- Layout ----
  {
    id: "layout", label: "Layout", group: "Layout",
    qt: "QMainWindow docks; Rail = a QToolBar (vertical) + a tool-options QToolBar over the viewport",
    choices: [
      { id: "docks", label: "Docks", hint: "Tools left, Properties right (today)" },
      { id: "rail", label: "Rail", hint: "Icon rail + tool bar over the viewport; one wide right panel" },
      { id: "split", label: "Split right", hint: "Thin rail left; Tools and Properties stacked on the right" },
    ],
  },
  {
    id: "chrome", label: "Panel chrome", group: "Layout",
    qt: "Seams: QDockWidget + 1px separators. Inset: QMainWindow margins + rounded QFrame per dock",
    choices: [
      { id: "seams", label: "Seams", hint: "Flush panels, 1px lines" },
      { id: "inset", label: "Inset", hint: "Rounded panels with gutters" },
    ],
  },
  {
    id: "output", label: "Export / Render", group: "Layout",
    qt: "Menu bar corner widget (QMenuBar::setCornerWidget) or buttons in the dock",
    choices: [
      { id: "corner", label: "Menu bar corner", hint: "Two buttons right of the menus" },
      { id: "panel", label: "Panel footer", hint: "Buttons at the bottom of Tools (today)" },
      { id: "menu", label: "Menus only" },
    ],
  },
  {
    id: "onboarding", label: "First-groom help", group: "Layout",
    choices: [
      { id: "viewport", label: "In viewport", hint: "A quiet card over the viewport" },
      { id: "card", label: "Panel card", hint: "Checklist in the Tools panel (today)" },
      { id: "hidden", label: "Hidden" },
    ],
  },
  {
    id: "hud", label: "Viewport HUD", group: "Layout",
    qt: "Child QWidgets over the viewport (the app's OverlayHost)",
    choices: [
      { id: "pills", label: "Pills" },
      { id: "plain", label: "Plain text" },
      { id: "hidden", label: "Hidden" },
    ],
  },

  // ---- Controls ----
  {
    id: "tools", label: "Tool palette", group: "Controls",
    qt: "QToolButton grid (ToolButtonTextUnderIcon / IconOnly) or a QListWidget",
    choices: [
      { id: "tiles", label: "Tiles", hint: "Icon + label grid (today)" },
      { id: "icons", label: "Icons", hint: "Icon grid, names in tooltips" },
      { id: "list", label: "List", hint: "Rows: icon, name, shortcut" },
    ],
  },
  {
    id: "toolIcons", label: "Tool icons", group: "Controls",
    qt: "Colored SVG as is, or recolored to one tone (theme.icons)",
    choices: [
      { id: "color", label: "Duotone" },
      { id: "mono", label: "Mono" },
    ],
  },
  {
    id: "rows", label: "Property rows", group: "Controls",
    qt: "QFormLayout (split / stacked) or one custom-painted field widget",
    choices: [
      { id: "split", label: "Label | control", hint: "Today" },
      { id: "fields", label: "Fields", hint: "Label inside the slider field (Blender-like)" },
      { id: "stacked", label: "Stacked", hint: "Label above the control" },
    ],
  },
  {
    id: "sliders", label: "Sliders", group: "Controls",
    qt: "QSlider + QLineEdit (track) or a custom-painted drag field (fill)",
    choices: [
      { id: "fill", label: "Fill field", hint: "Drag anywhere, value inside" },
      { id: "track", label: "Track + value", hint: "Today" },
    ],
  },
  {
    id: "booleans", label: "On / off", group: "Controls",
    qt: "QCheckBox (indicator styled in QSS) or a small painted switch",
    choices: [
      { id: "checkbox", label: "Checkbox" },
      { id: "switch", label: "Switch" },
    ],
  },
  {
    id: "sections", label: "Sections", group: "Controls",
    qt: "A header QToolButton (arrow) + body QWidget",
    choices: [
      { id: "caps", label: "Caps header", hint: "Small caps label + hairline" },
      { id: "cards", label: "Cards", hint: "Each section on a raised card" },
      { id: "plain", label: "Plain", hint: "Bold title, no lines" },
    ],
  },
  {
    id: "tabs", label: "Tabs", group: "Controls",
    qt: "QTabBar::tab (selected / hover) in QSS",
    choices: [
      { id: "segmented", label: "Segmented" },
      { id: "underline", label: "Underline", hint: "Today" },
      { id: "pills", label: "Pills" },
    ],
  },

  // ---- Groom Tools layout (the full panel: every control each tool has today) ----
  {
    id: "gtOptions", label: "Brush options", group: "Groom Tools",
    qt: "Checkable QToolButtons in a flow row (chips), a fold Section, or a fixed QGridLayout",
    choices: [
      { id: "chips", label: "Chips", hint: "Pressure / Front only / Off scalp… as one row of toggles" },
      { id: "fold", label: "Fold", hint: "Behind a collapsed 'Options' row that lists what's on" },
      { id: "grid", label: "Checkboxes", hint: "Today's checkboxes, in fixed places per tool" },
    ],
  },
  {
    id: "gtHints", label: "Tool hints", group: "Groom Tools",
    qt: "One elided QLabel + a ? glyph with the full tip and the gestures",
    choices: [
      { id: "short", label: "One line + ?", hint: "A short line; the full text and gestures behind ?" },
      { id: "full", label: "Full text", hint: "Today: the whole hint and the gestures paragraph" },
    ],
  },
  {
    id: "gtGroups", label: "Guide groups", group: "Groom Tools",
    qt: "A QListView section under the brush (or today's combo + folded section)",
    choices: [
      { id: "list", label: "Open list", hint: "Always-open list: click picks the group" },
      { id: "combo", label: "Combo + fold", hint: "Today: Group combo at the top, list folded at the bottom" },
    ],
  },
  {
    id: "gtPin", label: "Scrolling", group: "Groom Tools",
    qt: "Header + tabs (+ tool grid) outside the QScrollArea",
    choices: [
      { id: "tools", label: "Pin tools", hint: "Header, tabs and the tool grid stay; settings scroll" },
      { id: "header", label: "Pin header", hint: "Header and tabs stay; the rest scrolls" },
      { id: "none", label: "All scroll", hint: "Today" },
    ],
  },
  {
    id: "gtTubes", label: "Tubes", group: "Groom Tools",
    qt: "Split TubePanel: new-guide shape in Place/Draw, edits in Shape/Select, Fill/Blend to Interpolate",
    choices: [
      { id: "split", label: "Split by job", hint: "New-guide shape in Place/Draw; selected-guide edits in Shape/Select" },
      { id: "section", label: "One section", hint: "Today: one Tubes section for everything" },
    ],
  },

  // ---- Graph (painted with QPainter in both hosts: none of QSS's limits apply) ----
  {
    id: "nodeHeader", label: "Node header", group: "Graph",
    qt: "graph.headerStyle (tinted / filled exist; strip / minimal are new)",
    choices: [
      { id: "tinted", label: "Tinted", hint: "Category color at 16% behind the title (today)" },
      { id: "filled", label: "Filled", hint: "Solid category color, dark title" },
      { id: "strip", label: "Side strip", hint: "A colored bar down the left edge" },
      { id: "minimal", label: "Minimal", hint: "Just a colored dot by the title" },
    ],
  },
  {
    id: "nodeBody", label: "Node body", group: "Graph",
    qt: "graph.nodeBody (new): what the node card draws under the title",
    choices: [
      { id: "ports", label: "Ports", hint: "One row per port, labeled (today)" },
      { id: "values", label: "Ports + values", hint: "The node's key settings shown on the card" },
      { id: "compact", label: "Compact", hint: "Title only; ports on the edges, names in tooltips" },
    ],
  },
  {
    id: "wires", label: "Wires", group: "Graph",
    qt: "graph.wireStyle (new): QPainterPath cubicTo / lineTo with rounded corners",
    choices: [
      { id: "curved", label: "Curved" },
      { id: "angled", label: "Angled", hint: "Right angles with rounded corners" },
      { id: "straight", label: "Straight" },
    ],
  },
  {
    id: "wireColor", label: "Wire color", group: "Graph",
    qt: "graph.wireColors (new): type (graph.ports) or neutral",
    choices: [
      { id: "type", label: "By type", hint: "Each port type's color (today)" },
      { id: "neutral", label: "Neutral", hint: "Grey; the selected node's wires light up" },
    ],
  },
  {
    id: "ports", label: "Ports", group: "Graph",
    qt: "graph.portShape (new)",
    choices: [
      { id: "dots", label: "Dots" },
      { id: "rings", label: "Rings", hint: "Hollow rings in the type's color" },
      { id: "shapes", label: "Shapes", hint: "A shape per type: strands circle, guides diamond, mesh square" },
    ],
  },
  {
    id: "canvas", label: "Canvas", group: "Graph",
    qt: "graph.canvasStyle (new); graph.canvas / graph.grid colors",
    choices: [
      { id: "dots", label: "Dots" },
      { id: "lines", label: "Grid lines", hint: "Today" },
      { id: "plain", label: "Plain" },
    ],
  },
  {
    id: "nodeStatus", label: "Node status", group: "Graph",
    qt: "the cook times and View / Bypass / warning states the graph already tracks",
    choices: [
      { id: "badges", label: "Badges", hint: "Cook time, View, Bypass and warnings on the card" },
      { id: "none", label: "None" },
    ],
  },
] as const satisfies readonly Dimension[];

export type DimId = (typeof dimensions)[number]["id"];
export type Options = { [K in DimId]: string };

// Curated combinations: starting points to compare, not the only ones.
export const directions: { id: string; label: string; hint: string; options: Partial<Options> }[] = [
  {
    id: "chosen", label: "Chosen", hint: "Picked 2026-10-30: Quiet docks, duotone icons, underline tabs, minimal nodes",
    options: {
      theme: "charcoal", accent: "blue", font: "inter", numbers: "tabular", density: "default", radius: "6",
      layout: "docks", chrome: "seams", output: "corner", onboarding: "viewport", hud: "plain",
      tools: "icons", toolIcons: "color", rows: "fields", sliders: "fill", booleans: "checkbox", sections: "caps", tabs: "underline",
      nodeHeader: "minimal", nodeBody: "ports", wires: "curved", wireColor: "type", ports: "dots", canvas: "dots", nodeStatus: "badges",
      gtOptions: "chips", gtHints: "short", gtGroups: "list", gtPin: "tools", gtTubes: "split",
    },
  },
  {
    id: "today", label: "Today", hint: "Close to the current app, for comparison",
    options: {
      theme: "charcoal", accent: "mono", font: "figtree", numbers: "mono", density: "default", radius: "10",
      layout: "docks", chrome: "seams", output: "panel", onboarding: "card", hud: "pills",
      tools: "tiles", toolIcons: "color", rows: "split", sliders: "track", booleans: "checkbox", sections: "cards", tabs: "underline",
      nodeHeader: "tinted", nodeBody: "ports", wires: "curved", wireColor: "type", ports: "dots", canvas: "lines", nodeStatus: "none",
      gtOptions: "grid", gtHints: "full", gtGroups: "combo", gtPin: "none", gtTubes: "section",
    },
  },
  {
    id: "quiet", label: "Quiet docks", hint: "Same docks, less noise: mono icons, fields, caps sections",
    options: {
      theme: "charcoal", accent: "blue", font: "inter", numbers: "tabular", density: "default", radius: "6",
      layout: "docks", chrome: "seams", output: "corner", onboarding: "viewport", hud: "plain",
      tools: "icons", toolIcons: "mono", rows: "fields", sliders: "fill", booleans: "checkbox", sections: "caps", tabs: "segmented",
      nodeHeader: "tinted", nodeBody: "ports", wires: "curved", wireColor: "type", ports: "dots", canvas: "dots", nodeStatus: "badges",
    },
  },
  {
    id: "rail", label: "Rail", hint: "Plasticity-like: icon rail, tool bar on the viewport, one panel",
    options: {
      theme: "carbon", accent: "violet", font: "geist", numbers: "tabular", density: "compact", radius: "6",
      layout: "rail", chrome: "inset", output: "corner", onboarding: "viewport", hud: "pills",
      tools: "icons", toolIcons: "mono", rows: "fields", sliders: "fill", booleans: "switch", sections: "caps", tabs: "segmented",
      nodeHeader: "strip", nodeBody: "compact", wires: "angled", wireColor: "neutral", ports: "rings", canvas: "plain", nodeStatus: "badges",
    },
  },
  {
    id: "studio", label: "Studio", hint: "Warm, roomy, stacked right side",
    options: {
      theme: "umber", accent: "amber", font: "figtree", numbers: "tabular", density: "comfy", radius: "10",
      layout: "split", chrome: "inset", output: "corner", onboarding: "viewport", hud: "plain",
      tools: "list", toolIcons: "mono", rows: "split", sliders: "fill", booleans: "switch", sections: "plain", tabs: "pills",
      nodeHeader: "filled", nodeBody: "values", wires: "curved", wireColor: "type", ports: "dots", canvas: "dots", nodeStatus: "badges",
    },
  },
  {
    id: "graphite", label: "Graphite", hint: "Cool and technical, square-ish, dense",
    options: {
      theme: "graphite", accent: "emerald", font: "inter", numbers: "tabular", density: "compact", radius: "4",
      layout: "docks", chrome: "seams", output: "corner", onboarding: "hidden", hud: "plain",
      tools: "icons", toolIcons: "color", rows: "fields", sliders: "fill", booleans: "checkbox", sections: "caps", tabs: "underline",
      nodeHeader: "minimal", nodeBody: "values", wires: "angled", wireColor: "type", ports: "shapes", canvas: "lines", nodeStatus: "badges",
    },
  },
];

// The starting design (and what the URL hash is relative to): the "Chosen" direction.
export const defaults: Options = {
  ...(Object.fromEntries(dimensions.map((d) => [d.id, d.choices[0].id])) as Options),
  ...directions.find((d) => d.id === "chosen")!.options,
};
