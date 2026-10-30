# From the lab to Qt

How each piece of the mock becomes Qt (5 or 6) in `hosts/app`. The Qt export tab writes the two
artifacts this starts from: a theme JSON for `hosts/maya/themes/` (same `ui` / `graph` keys
`hosts/app/src/theme.cpp` already reads) and a reference QSS of what the app's stylesheet template
(`theme.cpp` `styleSheet()`) should produce for that theme.

## Tokens

| Lab token (Tailwind class) | Theme key | Used for |
|---|---|---|
| `bg-background` | `ui.background` | window behind the docks, QMainWindow::separator |
| `bg-viewport` | viewport clear color | the 3D view |
| `bg-panel` | `ui.panel` | dock surfaces, menu bar |
| `bg-raised` | `ui.raised` | fields, buttons, menus, tooltips |
| `bg-hover` | `ui.hover` | hover; scroll bar handle |
| `bg-selection` | `ui.selection` | checked buttons, current list row, slider fill |
| `border-line` | `ui.border` / `ui.outline` | every 1px line |
| `text-fg` / `text-dim` / `text-muted` | `ui.text` / `ui.textDim` / (new) `ui.textMuted` | text tiers |
| `bg-accent` / `text-on-accent` | `ui.accent` / `ui.accentText` | primary button, focus border, track fill, active tab |
| `rounded-ctl` / `rounded-pnl` | `ui.radius` / radius + 2 | controls / panels and cards |
| density (`h-row`, `text-ui`) | (new) `ui.rowHeight`, `ui.fontSize` | min-height of every control |

`ui.textMuted` and `ui.rowHeight` are the two keys the theme files don't have yet.

## Widgets

| Lab part | Qt | Notes |
|---|---|---|
| Menu bar + corner buttons | `QMenuBar`, `setCornerWidget` | Export/Render in the corner keeps "no top toolbar" |
| Docks, seams chrome | `QDockWidget`, title bar hidden with `setTitleBarWidget(new QWidget)` | 1px `QMainWindow::separator` |
| Inset chrome | `QMainWindow` contents margins + each dock's content in a rounded `QFrame` | separator 6px, `background` color |
| Rail | vertical `QToolBar` of checkable `QToolButton`s in an exclusive `QButtonGroup` | tooltips carry name + tip |
| Brush strip over viewport | a child `QWidget` of the viewport (the app's OverlayHost) | `QHBoxLayout` of fields |
| Tool palette: tiles / icons / list | `QToolButton` grid (`ToolButtonTextUnderIcon` / `IconOnly`), or rows | `[role="tool"]:checked` in QSS |
| Mono tool icons | the same SVGs recolored at load (theme `icons` colors) | the app already recolors |
| Tabs: segmented / underline / pills | `QTabBar` | one QSS block each (in the export) |
| Section (caps / cards / plain) | header `QToolButton` (checkable, arrow) + body `QWidget` | caps: uppercase the text in code, letter-spacing via `QFont::setLetterSpacing`; cards: `QFrame[role="card"]` |
| Property row: split / stacked | `QFormLayout` / `QVBoxLayout` | label column ≈ 38% |
| Fill field (slider style "fill", rows "fields") | custom `QWidget`: `paintEvent` draws rounded rect, fill rect, label, value | ~200 lines; the app's `param_editors` already paint their own sliders. Behavior below |
| Track slider | `QSlider` (groove / sub-page / handle) + `QLineEdit` | today's style; the box edits the same way |

### Typing values (every slider)

- **Click** (press and release without moving 3px): an inline `QLineEdit` over the field, the
  value selected. **Drag**: scrubs relative to the press (Shift: 1/10 speed, Ctrl: steps of 10×).
- **Enter**, Tab or clicking away commits; **Esc** puts the old value back.
- Accepts a number or simple arithmetic (`0.5*2`, `1/3`, `,` as decimal point). Typed values may
  go past the slider's range (a soft range, as Maya's and Blender's fields); dragging stays inside it.
- Focused field: Enter / F2 to type, arrows nudge by one step (Shift: 10).
- Maya: the same widget in `ui/widgets.py`. Blender: its own number fields already work this way.
| Checkbox | `QCheckBox::indicator` | check image: `ui_check_dark/light.svg` |
| Switch | small `QAbstractButton` subclass painting track + knob | ~40 lines |
| Select | `QComboBox` + its `QAbstractItemView` | |
| Tooltip with shortcut | `QToolTip`, rich text with a dimmer `<span>` | |
| Inputs list (Scalp, Guides, …) | `QWidget` row: icon `QLabel`, name, value, one ghost `QToolButton` | replaces Transfer…/Edit/Add…/Reload/Save buttons |
| Guide groups list | `QListView` + delegate (color dot, name, count, eye) | |
| HUD chips | `QLabel`s with `border-radius` over the viewport | |
| Graph node card | the app's node graph painter: header tinted 16% with the category color, dot, 1px border | colors from theme `graph` keys |

## Groom Tools layout (the full panel)

The lab's Groom Tools hold every control each tool has today (src/data/toolSettings.ts, from
brush_catalog.cpp and palette.py). One model, every tab:

**Pick a tool → its card right under the grid → the list of things you work on.**

1. **Pinned top** (outside the QScrollArea): groom header, tabs, the tool grid. Tool tiles keep a
   fixed size (34 px); columns = `(width + spacing) / (tile + spacing)`, left-aligned, never stretched.
2. **The tool card**, always the same blocks in the same order (a block only shows when it has rows):
   - Title row: icon, name, a **?** glyph (tooltip: the full hint + the gestures), then one short line.
   - **Brush**: how the stroke feels. Radius, Strength (±), Falloff, Smooth; chips Pressure,
     Off scalp, Falloff curve, Root to tip (a curve chip that's on shows its ramp under the row).
   - **The tool's own block**, named after what it sets: Paint (Mode, Value + Fill, Stamp),
     New guides + Tube (Place / Draw: the profile picked, a small preview, Width / Flatness / Twist),
     Comb (Stretch), Paints + Profile + Selected guides (Shape: the shape picker with Apply, the
     cross-section editor (TubeShapeEditor, ≤200 px square), Edit as custom, Mirror, Smooth, and
     the library's new / rename / duplicate / delete glyphs in the block header),
     Selection + Selected guides (Select), Erases (Erase), Keys (Length Keys), Flow, Parts.
   - **Applies to**: where the stroke lands. Acts on, Mirror (symmetry), chips Selected only,
     Front only. The current group / map / layer is not repeated here: it is the picked row of the
     list below.
3. **The list** of the tab's things: Guide groups (Tools), Maps (Paint), Layers (Correctives),
   Photos. Section header = name, count, add/import glyphs. Plain rows (dot, name, meta, eye).
   Under the list, a small box with **the picked item's own** settings and actions (rename /
   select / delete glyphs; a group's Segments and Edit guides; a map's Show on and Read by; a
   layer's Weight + key).

Row styles: block titles are small semibold dim text over a faint rule; numbers are fill fields
with the name inside; chip rows put their name above the chips; Row label | control only for
combos/toggles (Mirror, Shape, Place, Stamp).

Gone from the panel: the Output strip (GroomLab menu; the app's menu bar corner), While sculpting
(Attribute Editor / Properties), the Group combo (pick in the list), the gestures paragraph (?
tooltip), the second Unfreeze All, the three-times-repeated hint, Fill Tubes / Tube Blend
(Interpolate node settings).

## Graph nodes

The app (`hosts/app/src/node_graph.cpp`) and Maya (`ui/graph_editor.py`) paint nodes with QPainter,
so QSS's limits don't apply to the graph. Each Graph option is one theme key under `graph`
(the Qt export writes them); both painters read the same key.

| Option | Theme key | Today | Painter work |
|---|---|---|---|
| Node header: tinted / filled / strip / minimal | `headerStyle` | tinted, filled read | strip: 3px rect on the left edge; minimal: dot only, no header fill or line |
| Node body: ports / values / compact | `nodeBody` (new) | ports | values: 1–2 key params per node (a `cardParams` list per node in the catalog); compact: 30px card, ports spread on the edges, names in the tooltip |
| Wires: curved / angled / straight | `wireStyle` (new) | curved | `QPainterPath::cubicTo`; angled = `lineTo` + `quadTo` corners (8px) |
| Wire color: by type / neutral | `wireColors` (new) | by type | neutral: `graph.textDim` at 60%, the selected node's wires `graph.text` at 2px |
| Ports: dots / rings / shapes | `portShape` (new) | dots | rings: 2px pen, panel fill; shapes: circle / diamond / square per port type |
| Canvas: dots / grid lines / plain | `canvasStyle` (new) | grid | |
| Node status: badges / none | `nodeStatus` (new) | timings shown elsewhere | cook time (yellow from 10 ms), View eye (`blue`), Bypass icon + 50% opacity + dashed wires, warning triangle |

All states (selected, bypassed, viewed, warning, slow) already exist in both hosts; only how
the card shows them changes. Blender's node editor draws its own nodes: there only the header
color (`node.color` with `use_custom_color`) can follow.

## What not to design here

QSS can't do: box-shadow, backdrop blur, CSS transitions, gradients other than `qlineargradient`,
`text-transform`, per-side border radius on some widgets, `gap`. The lab turns shadows and
transitions off inside `.gl-ui` so they don't sneak in. The Design lab panel itself is outside
`.gl-ui` and isn't part of the design.

## Port order (when a design is picked)

1. Theme JSON → `hosts/maya/themes/<name>.json`; add `textMuted` and `rowHeight` to `theme.cpp`.
2. QSS template in `theme.cpp`: tabs, sections, buttons' `[role]`s, field heights from `rowHeight`.
3. Fill field + switch widgets (shared by Properties, tool settings, node settings).
4. Layout changes (Inputs rows, corner buttons, onboarding over the viewport, rail if chosen).
5. Maya and Blender follow through `engine/ui` where they share structure.
