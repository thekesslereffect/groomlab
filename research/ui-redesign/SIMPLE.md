# Simple: GroomLab in Maya, calmer

The **Simple** page of this lab (the default; `#page=simple`) shows GroomLab where most people
use it: inside Maya. It is a rearrangement of what the Maya plugin has today
(`hosts/maya/scripts/groomlab/ui`), not new features. The look comes from the Charcoal tokens, and
the pieces are the ones `palette.py` and `widgets.py` already build. Once Maya is settled, the app
and Blender follow; their Groom Tools column is the same.

Rules from the user:

- Brushes stay **in their groups** (Create, Shape, Trim, Guides), **without group names**.
- Nothing the panels do today is dropped (guide edits and segments, every paint mode, the Photos page), except **Guides from Photos**, which goes.
- What a node already controls (color, cards, render overrides) leaves the Attribute Editor.
- No Style or Color screens. The look, color included, is the **node graph's** job.
- Design for **Maya first**.

Run: `npm run dev`, open http://localhost:5199. Hash links:

- `#page=simple&tool=place` picks a tool; `&layer=1` edits a corrective layer.
- `&tab=paint` / `&tab=photos` opens a page (`&photo=front` picks a photo).
- `&empty=1` shows the panel with no groom.
- `&search=1` opens Find Command.
- `&graph=1` opens the Graph Editor.

`npm run shots -- simple` writes each view to `shots/`.

## What Maya shows today

This is read from the code on 2026-11-24.

- **Main menu bar.** A torn-off **GroomLab** menu sits in Maya's main bar, built from `menus.json`
  "main". It has about 20 top-level entries and 10 submenus: Groom Tools, Graph Editor, New Groom,
  Card Atlas, Add Sample Mesh, Guides from Photos, Groom Files, Import, Add, Simulate (with Drivers
  and Legacy), Display, Review, Studio, Render, Export, Utilities (14), Theme, Hotkeys, Radial
  settings, Help. **There is no shelf.**
- **Groom Tools.** This is `palette.py`, a workspaceControl docked `area="left"`. It is one scroll.
  - A page rail along the top holds 4 icon pages (Tools, Paint, Correctives, Photos). The groom's
    eyes, Graph and More sit at the rail's right end.
  - Under the rail: the groom combobox, an update note, a status label, the empty state and a
    StarterCard.
  - Then the tool trays, one card per category, each with a header row. That is 20 tiles at
    56×50 in 4 columns: Create 5, Shape 7, Trim 4, Guides 4. In a docked panel the trays alone
    take about 6 tile rows plus 4 headers.
  - Then the **tool card**. For Comb it shows Radius, Strength, Falloff, switch rows for Pressure,
    Keep off scalp and Stretch, Falloff-curve and Root-to-tip chips with curve editors, "Applies
    to" (Guides/Parts/Flow) and a Mirror row with its axis box. Place and Draw add New guides
    (Length, Segments, Spacing, Match nearby, Solo new) and the whole Tube panel. Tube adds
    channels and 5 visibility switches.
  - Then the page's list (Guide groups, Maps, Correctives, Photos).
- **Attribute Editor** (`AEgroomLabShapeTemplate.mel`). Sections:
  - Groom: buttons, Preview Density, Point Budget, Strands, Locked.
  - Inputs.
  - Controls.
  - **Hair Color**.
  - Display, with Guides and Strand Shading subsections: 30-odd attributes.
  - Sculpting, Game Cards, and Rendering with Render Overrides.
- **Graph Editor** (`graph_editor.py`). It docks under the viewport on smaller screens and floats
  otherwise. Its toolbar holds: groom box, Pin, Add Node, Presets, wire-style button, the align
  buttons, a spacer, Snap, Live, Apply (shown when Live is off) and an Edit menu. The node's
  params sit on the right.
- **Elsewhere:** a radial menu (Alt+S with the GroomLab hotkeys), plus Reference, Card Atlas,
  Transfer, Expression and Script windows.

The panel already works. The problems are its length and some repeats:

- The tool card sits below about 330 px of trays, so in a docked panel at Maya's usual height it
  is half off screen.
- Every brush option shows at once.
- The pages are icon-only.
- The empty state, the starter card and the update note stack up at the top.
- Hair color is edited in both the AE and the graph.
- Save, Reload and visibility each have more than one home.

## What changes, piece by piece

### Groom Tools (`palette.py`)

| Today | Simple | Why |
|---|---|---|
| Icon-only page rail (Tools, Paint, Correctives, Photos); the groom box under it | The groom box and its buttons first (hair eye, guides eye ▾, Graph, **Find**, More), then **three labeled pages: Tools · Paint · Photos**. | You pick *what* you work on before *how*. Labels need no decoding. |
| One tray per category: a header row, then 4-wide two-tone tiles 56×50 | **The same trays, without their names.** Tiles go 5 wide (≈46×44) with **one-tone icons** (the chosen tool's icon in the accent, via the app's `tinted_icon`). Tiles keep Maya's look: no background, a hover tint, the chosen one on the soft accent with its edge (`QToolButton[role="tool"]`). Each tray's tooltip is its group name. | The groups stay (user rule). The names cost about 80 px and add nothing once the tiles are learnt. One tone makes the chosen tool the only coloured one. |
| Invert and Unfreeze as tiles | Unchanged | They're buttons in the Guides tray, as today. |
| The **Correctives page**: the same brushes again (filtered), the same card, the layer list | **Gone as a page; Correctives is its own section on the Tools page, under Guide groups, one line per layer like the groups and maps.** The header has **◆ Fix Frame** and **+**. Each row has the layer icon, the name and its edit count, then icons: **◐ 1.0** shows the weight and opens a popup (**Weight** and its **key**); **Solo**; and **⋯** (Rename…, Move Up / Down, Go to Its Frame, Key Weight Here, Clear Keys, then **Delete Corrective** in red). Clicking a layer makes the brushes edit it, and clicking it again or Base groom goes back. **Display while sculpting** is folded under the list. The brush card's header names the layer ("● primary on wind_pose", highlighted while it's a corrective). Brushes that can't edit a layer are **greyed, not hidden**. | The page only repeated the brushes and the card. Now the layers sit with the brushes, and the weight shows on every row without opening anything. |
| The tool card: an icon + name header, then every setting | **One header row**: the brush's **icon and name**, and on the right what the next stroke changes (**● primary on Base groom**). Then **Radius** and **Strength**, then one chip row: **⇆ Mirror** (as today), Pressure, Keep off scalp, and the brush's own switch (Comb: Stretch; Smooth: Straighten; Place/Draw: Match nearby). Next come the brush's own essentials: Place/Draw get Length and Segments; Erase gets what it removes; Clone gets Pick Source and Reset; Part gets the next name; Flow gets what a click lays; Tube gets Width/Flatness/Twist; Select gets Soft radius. **More settings** (folded) holds Falloff, Mirror across (World X / Scalp topology), Applies to, the two curves, Spacing and Solo new, and the tube display switches. | Most strokes only need size and strength, and B / M already drag those in the viewport. The rest is one click away. |
| Gestures listed in the card, plus a key strip | **Maya's Help Line** shows the active brush's keys ("Comb: Drag: comb · Shift: smooth · …"). **The panel's own key strip is removed.** | It's where Maya users already look, so the panel doesn't need to repeat it. |
| Empty state + StarterCard + update note stacked | **One** empty card: "Select the scalp mesh (and any guide curves), then create". It shows **what is selected now**, has one Create button (disabled until a mesh is picked), sample meshes as chips, and Import folded. The update note moves to Help ▸ Check for Updates. | Maya's way is select-then-act. One place to start, not three. |
| A list, then a separate box for the picked row (guide groups, maps, layers, photos) | **Every section sits on a card** (Guide groups, Correctives, Maps, Photos), its header inside, the same card as the brush trays and the brush's settings (Maya's `QWidget[role="tray"]`, as palette.py already puts each page's list). **One icon order on every list** (guide groups, correctives, maps, photos): the name, then muted text (a count, a tag), then the icons in this order, each only where the list has it: **settings popup** (segments, weight, ⓘ info, photo sliders) → **state toggles** (solo, in front / behind) → **eye** → **lock** → **⋯ always last, at the right edge**. **Every row's ⋯ menu starts with Rename… and ends with Delete in the theme's error red** (no rename or delete icons in the rows). **No row expands**: a row's settings open from an icon on it (segments, weight, map info, photo settings). Earlier the picked row opened in place: and its settings sit under it, indented behind a thin rule, with no box of their own. | One look for every section, so each is clearly its own block without labels. No second box repeats the name, and the settings sit next to what they change. |
| Guide groups: the list, then the picked group's box (rename / select / delete, Segments, Segments by length, Edit Guides ▾, Children per guide) | **One line per group, like the maps.** Each row has the colour dot, the name and the guide count, then icons: **⫶ 12** shows the segment count and opens a small popup (**Segments**, **By length**, and what it applies to: "every guide in primary" / "the 12 selected guides"); the **eye**; and **⋯ Edit Guides** last (Rename…, Select Guides, **Generate Children ▸ 1 / 2 / 3 / 4 / 6 / 8 per guide**, Mirror ▸, Reparent, Move to Group ▸, Copy/Paste, From Curves, Clear Selection, Reset Segments, Color, then **Delete Group** in red). The header keeps Isolate and +. | Nothing is lost, and the list stays three short lines, so Correctives comes into view under it. The segment count shows on every row without opening anything. |
| Paint page: a wide Paint tile, the maps, the card | **Opening the tab starts Paint on the picked map.** Leaving it gives back the brush you had before. The card's header has the Paint icon and name and the map it paints ("on density", or "· not used yet"). It has the **four modes**: Replace · Add · Subtract · Clamp Max. A gray map gets Value (Maximum for Clamp Max) and **Fill**; a color map gets the swatch, **R G B A** chips and Fill. Then Radius, Strength, and chips for Mirror, Pressure and **Whole faces**. **More settings** holds Falloff, Smooth (Shift) strength, the **stamp** (image; rotation Fixed / Follow Stroke / Random; angle; spacing; invert) and the falloff curve. | Paint is the page's only tool, so the tab *is* the tool. |
| Maps list and the picked map's box | **One line per map: its name, then icons: ⓘ, then ⋯ last.** **⋯** holds Rename…, **Convert to ▸** (guides / scalp vertices / texture, with what it is now), Relink / Reload Image and Show File for textures, and **Delete Map** in red. **ⓘ** opens a small popup with its kind, **Read by** as a link that opens the Graph Editor on that node, **Use for ▾** (or Also use for ▾), which lists the graph's mappable params by node and wires the map to one, and for color maps "Scalp shows" RGB / R / G / B / A. A map **nothing reads** gets a dot on its ⓘ. Color maps keep a small RGBA tag. | The list stays one line per map, so it reads at a glance. The details are one click away, and the dot flags a map that does nothing yet, because the graph owns the look. |
| Photos page: the list, the picked photo's box (Look through and Lock chips, In front/Behind, Opacity, Focal length, Align), Guides from photos (folded) | **One line per photo, nothing expands.** **Clicking a photo looks through its camera** (a camera mark on the row); **clicking it again leaves the camera**. Each row has icons: **sliders** (a popup with **Opacity** and **Focal length**; the lens is greyed while locked), **in front / behind**, the **eye**, the **lock**, and **⋯** (Rename…, **Align Photo…**, Select Camera, **Remove Photo** in red). **Guides from Photos is removed** (page, menu, search). | Looking through is what picking a photo is for. Each photo's settings are one click away on its own row, so the list never changes height. |
| Guide display in the AE (width, points, smoothing, colors, on top, isolate) | The header's guides button is a **split button**: the **icon shows or hides the guides**, and the **▾** next to it opens how they draw (Guides / Flow / Tubes / Instances, Width, Points (Off / Roots and tips / All), Colors (Group / Painted Layer), On top, Smoothed). Qt: a checkable `QToolButton` with `MenuButtonPopup`. | Showing or hiding guides is one click, as before. The display settings sit right next to it, behind the arrow. |

### Tube and Roots: two brushes (`palette.py` TubePanel, `root_tube_card.py`)

Tube was three jobs in one brush. Its card stacked every block at once, each foldable (issue 70):
**Brush** (with Paints), **Visibility**, **Profile**, **Tube roots**, **Tube hierarchy** and
**Selected guides**, about 40 controls on one card. A first pass put a job switch (Sculpt · Roots ·
Shape) on the card, but the switch only showed and hid blocks: the viewport tool stayed the same,
and Roots makes guides while Sculpt and Shape edit them. **The user's decision (2026-11-25): two
brushes**, one data model (a root tube is a guide tube whose root profile is its drawn root).

```
[Tube icon] Tube                    ● primary on Base groom      (Shape group, id "shape")
Level  [1][2]               + − ⋯   ← the tube hierarchy (what you edit)
Paints [Width|Flatness|Twist] · Radius · Strength · Mirror, Pressure
More settings (folded): Falloff, curves
[Root][Mid][Tip] tiles, each showing its outline (Mid "—" = halfway)
outline editor (132 px) | shape picker · Apply · Hold % · Mirror, Smooth
⋯ the library (Rename…, New Shape, Duplicate, Delete Shape in red)
Selected tubes · 12 (folded): Width · Flatness · Twist · Smooth

[Roots icon] Roots                  ● primary on Base groom      (Create group, id "roots")
Level  [1][2]               + − ⋯
[Draw root|Edit roots|Select root]  Undo CV · Delete · Duplicate
Subdivide · paint density 🖌 · Multiplier · Apply
Snap to neighbours · Draw guide before extrude · Length / Segments or Point density · Tip width
```

- **Picking Roots starts the root tool** (the brush context's "roots" mode), drawing a root. A mode's
  button again goes back to Draw. Another brush leaves root mode.
- **Tube keeps its id** ("shape"), so saved prefs keep working. Its Profile is always shown.
- A root tube **keeps its drawn root**: a Root profile goes to the other targets (the core's
  editTubes refuses it for root tubes), and the root picker shows "Drawn root". Owned shapes
  (`rootTube_<n>`) never show in the pickers or the library menu.
- **Mid and tip don't carry into split levels** (a split tube starts with neither: a blended tube
  would leave the root ring and the overlap check); the Profile says so when the targets are in
  split levels.
- **The tube display switches leave the card** (Shaded, Wireframe, Colors, Level colors, With
  every tool). They go to the header's guides ▾ menu under **Tubes**.

### Attribute Editor (`AEgroomLabShapeTemplate.mel`)

Every AE control, checked against the graph's nodes and the Groom Tools. Today: about 64 controls,
with 5 sections open (Groom, Inputs, Controls, Hair Color, Display). Simple: about 35, with 3 open.

| Section today | Simple | Where it goes |
|---|---|---|
| Groom: buttons, Preview Density, Point Budget, Strand Count, Locked | **Kept** | (the groom node's own state) |
| Inputs: scalp / rest / scalps / guides / file, Follow Scalp, Scale Source, Groom Scale | **Kept** | (what the groom is made from) |
| Controls (params promoted from the graph) | **Kept**, now the AE's main job | — |
| **Hair Color** (mode, melanin, redness, dye, greys, randomness…) | **Removed** | the **HairColor** node: it is that node's params shown a second time |
| Display ▸ Strand Shading: Use Material Color, Display Color | **Removed** | the **HairColor** node |
| **Game Cards**: Display Cards, Card Textures, Card Density | **Removed** | the **Cards** node, which makes the cards |
| Rendering ▸ **Render Overrides**: Render Density, Width Scale, Length Scale | **Removed** | **Cull**, **Width** or **Transform** nodes set to **Evaluate ▸ Render Only** (every node has that menu). The AE's own tooltip already calls Length Scale "a Transform node's scale". |
| Rendering ▸ Groom File | **Removed** | already in Inputs |
| **Sculpting**: Sculpt Display, Sculpt Density, Soft Select Radius | **Removed** | the Correctives section's "Display while sculpting"; the Select card's Soft radius (already there) |
| Display: Hair / Guides / Flow / Tubes / Instances; Display ▸ Guides: On Top, Width, Points, Smoothing, Isolate Group, Color Mode | **Removed** | the Groom Tools eyes (the guides eye ▾); Isolate in the Guide groups header |
| Display: Strand Colors + source | **Kept**, Display folded | viewport only |
| Display ▸ Strand Shading: Lighting, Hair Shading, Self Shadowing, Variation, Shadows, Width Coverage, Coverage Density, Clump LOD (+ error), Ribbons (+ width) | **Kept** as **Display ▸ Viewport Quality**, folded | viewport only: no node sets these |
| Rendering: Render Mode; Level of Detail (distance, near, far, min density, width compensation, camera cull + margin, backface cull + angle) | **Kept**, folded | renderer side, not graph data |

The attributes stay on the node, so scripts, the Channel Box and old scenes still work. Only the
AE template stops drawing them. No stored format changes.

### Graph Editor (`graph_editor.py`)

- The toolbar keeps Groom ▾, Pin, Add Node, Presets and a find-node box.
- **Live / Apply, Snap, the wire style and the align buttons move into the ⋯ menu.** The align
  buttons show as a small floating bar only while 2+ nodes are selected.
- It opens **docked under the viewport** by default on every screen size, matching Maya's
  Hypershade / Node Editor habit. Drag the tab out to float it.

### GroomLab menu (`menus.json` "main")

It goes from about 20 top-level entries to 14, grouped by job:

```
Groom Tools · Graph Editor · Find Command…
─
Create Groom from Selection
Start From ▸        Nika · Manny · Wolf · Buddy │ XGen · Yeti · Alembic Curves
─
Groom File ▸        Save · Save As · Import · Reload
Export ▸            Alembic · USD · V-Ray Scene · XGen IG │ Unreal Groom · Unreal Cards · Unity · glTF Cards · Card Maps
Render ▸            Arnold (groom file / curves) · Turntable │ RenderMan · V-Ray │ Studio Lighting ▸
Simulate ▸          Add Simulate Node · Simulate Range · Bake · Clear │ Drivers ▸ · Legacy nHair ▸
Utilities ▸         Card Atlas · Transfer · Copy Graph to Grooms │ conversions │ Find Missing · Collect Files
─
Show / Hide ▸       (was Display)
Settings ▸          Theme ▸ · Install Hotkeys · Radial Menu
Help ▸
```

What moved:

- **Add ▸** (Feathers, Braid, Bun, Cards, Collider, Scalp, Wind, Jiggle) adds nodes to the
  graph, so it lives in the Graph Editor's Add Node and Presets, and in Find Command.
- **Review ▸** (heatmap, HUD, turntable) joins Render.
- **Studio ▸** goes under Render.
- **Groom Files ▸** is renamed **Groom File ▸**.
- Every action id is unchanged. Only the menu tree moves.

### New: a GroomLab shelf, and Find Command

- **Shelf tab "GroomLab"** (optional; today there is none). It holds 7 buttons: Groom Tools,
  Graph Editor, Create Groom, Save Groom File, Simulate Range, Export Alembic, Find Command.
  Maya users look on a shelf first, and a shelf is something they can edit themselves.
- **Find Command** is a small search over all 293 actions, every brush and the 47 graph presets.
  It opens from the Groom Tools header, the GroomLab menu, the shelf, or a key in the GroomLab
  hotkey set. It is a frameless PySide dialog over a filtered list. This is how the menus can
  stay short: anything not on screen is two keystrokes away.

## The research behind it

| Finding | Applied here |
|---|---|
| **Hick–Hyman law**: decision time grows with the number of equal choices | The card shows 2 sliders and 1 chip row before "More", not about 12 controls. The menu has 14 grouped entries, not about 20 flat ones. |
| **Chunking** (Miller; Cowan, about 4 chunks) | The brushes stay in 4 named groups (user rule, and the research agrees): 20 tiles read as 4 chunks of about 5. |
| **Progressive disclosure** (Nielsen; Carroll) | "More settings" in the card, folded AE sections, Find Command for the long tail. Each fold says what it holds. |
| **Jakob's law**: people expect your tool to work like the rest of the app they're in | Maya-native homes: a menu in the main bar, a shelf, the AE for node attributes, the Help Line for hints, in-view messages for feedback, select-then-create to start. |
| **Recognition over recall** | Labeled page tabs, labeled tiles, the selection shown in the empty state, and maps that say what reads them. |
| **Fitts's law / spatial stability** (Scarr et al.) | The tiles never move or reflow between tools. The chip row stays in one fixed order, so muscle memory holds. The card sits right under the tiles. |
| **Expert path without clutter** (CommandMaps; marking menus) | The radial menu stays, Find Command is added, and every hotkey stays. |

## Not decided (the user's call)

1. Whether the **shelf** belongs in the plugin, or as an optional "Install Shelf" item next to
   Install Hotkeys.
2. Whether **Viewport Quality** and **Level of Detail** leave the AE too (for example to a GroomLab
   preferences window); today they are per groom.
3. Which settings count as a tool's **essentials** and which go to "More": the table above is a
   first pass.

## Port status

**Shared definitions** (`engine/ui`):
- `palette.json`: the pages are Tools · Paint · Photos; Correctives is a section of Tools (its old ids are aliases of tools).
- `lists.json`: each list's bar holds only its header actions. `context` is the row's ⋯ menu, Rename first and Delete last. `correctives.row` (Solo) and `photos.click` (look through) are new keys. `guidesBar` is gone.
- `actions.json`: `"destructive": true` marks Delete / Remove. Hosts draw those entries in the theme's error red. New `commands.find`; `photos.guides` is removed.
- `menus.json` main: regrouped into Start From, Groom File, Export, Render (Studio, Review), Simulate, Utilities (Card Atlas), Display, Settings, Help. Add stays.
- `properties.json`: Hair Color, Game Cards, Sculpting, the guide display and the display toggles are removed; the Groom File row is Blender-only; Strand Shading is now **Viewport Quality**, with Display folded.

Render Overrides and Use Material Color / Display Color were **kept**: they still take effect, so hiding them would hide live settings.

**Maya: done.**
- `palette.py`, `tool_tabs.py`, `widgets.py`: PopupPanel, row_glyph, menu_button, RowStrip, ListRow, TextRail, danger_action, and new glyphs.
- `find_command.py` (new).
- The AE template, without its Hair Color code in `ae.py`.
- `node_actions.py`: the Cards node gets a Card Density field.
- The Graph Editor bar: Snap and Live are in ⋯.
- `brush_tool.cpp`: a new `-helpText` flag puts each brush's keys in the Help Line.
- The first-groom checklist (StarterCard) is gone: one empty card instead. `palette/starter` in the JSON stays for the app and Blender, which still use it elsewhere.

**App: done.**
- The Groom Tools dock (`groom_tools`, `brush_panel`, `card_parts`, the four list panels, `tube_panel`), the guides display split button and popup, and Find Command (Ctrl+K, `find_command.cpp`).
- Destructive entries are red everywhere, the menu bar included. Map info's Use for calls the core's `paintParam` / `settingTargetsJson`.
- Differences from Maya:
  - No left icon rail: the labeled tabs are the only page switch (Shift+1..4, in their tooltips). Ctrl+[ folds the Groom Tools away entirely.
  - There is no "On top" chip, because the viewport has no such setting.
  - Morph to photos stays, folded under the photos.

**Blender: done.**
- The sidebar panel and the drawn GroomLab window are rebuilt, plus `find_command.py`.
- Row settings and the guide display open as Blender popovers. Deletes are drawn with `alert`.
- Sidebar tiles are icons only unless every full name fits.
- Not ported:
  - "On top" and "Instances", which Blender has no switch for.
  - Map "Use for", which needs a helper command.
  - The Help Line, since Blender's status bar already lists the keys.

**Follow-up (2026-11-25, every host):**
- Brush tiles are back to their two-tone artwork in each group's theme color.
- Playful is the default theme again; Charcoal's file still holds the shared components block.
- The card's toggles and the guide display chips are pills, as the mock's `Chip`: off is an outline with dim text, on is the selection fill with an accent edge.
- Blender's native sidebar uses its own toggle buttons in rows sized to their labels; its drawn window draws real pills.
