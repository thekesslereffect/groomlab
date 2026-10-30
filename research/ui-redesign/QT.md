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
brush_catalog.cpp and palette.py), arranged by these rules:

- **Pinned top**: groom header, tabs and the tool grid sit outside the QScrollArea; the tool card
  and the lists scroll under them (Scrolling: Pin tools).
- **Tool grid**: icon tiles keep a fixed size (34 px); the column count is
  `(width + spacing) / (tile + spacing)`, tiles left-aligned. Tiles never stretch with the panel.
- **Tool card**: icon + name + a ? glyph (its tooltip: the full hint and the gestures), one short
  line, then Radius / Strength / Falloff, Symmetry (only tools that mirror), the tool's own rows.
- **Options as chips**: Selected only, Pressure, Off scalp, Front only, Stretch, Falloff curve,
  Root to tip as one wrapping row of checkable chips (QToolButton[role="chip"] in a flow layout),
  always in the same order; a curve chip that's on shows its ramp editor under the row.
- **Chip rows** (Acts on, Erases, Paints, Mode, Show on, Guides Scale/Trim) put their name above
  the chips, never in the label column.
- **Sub-blocks** (New guides, Tube, Selected guides, Keys, Flow, Parts, Alpha): a small muted name
  with its one action on the right (Clear, Smooth, Clear all), then fill fields.
- **Tubes split by job**: the shape new guides take lives in the Place / Draw card (Tube); edits to
  the selected guides live in the Shape / Select card (Selected guides: shape + Apply, Width,
  Flatness, Twist, Smooth). Fill Tubes and Tube Blend belong to the Interpolate node, not the panel.
- **Guide groups**: one always-open list section under the card (color dot, name, count, eye; the
  header has Isolate and +), then Edit guides ▾, rename / select / delete glyphs and Segments. No
  Group combo at the top: clicking a row picks the group.
- **Other tabs** use the same list look: Maps (Paint), Layers (Correctives), Photos — a section
  whose header holds the add/import glyphs, a plain list, then the picked item's fields. The Paint
  tab ends with the Paint brush card; Correctives with the active tool's card.
- **Gone from the panel**: the Output strip (GroomLab menu; the app's menu bar corner), While
  sculpting (Attribute Editor / Properties), the gestures paragraph (? tooltip), duplicate
  Unfreeze All button (the tile stays), the repeated hint text.

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
