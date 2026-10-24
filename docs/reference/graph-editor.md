# Graph editor, presets and menu

The Graph Editor, the Groom Tools window, the shipped presets and the GroomLab menu.

User guide: [Node graph](https://www.getgroomlab.com/docs/node-graph), [Presets](https://www.getgroomlab.com/docs/presets).

## Graph editor, presets and menu

Loading the plugin adds a **GroomLab** menu:
- **Groom Tools** and **Graph Editor**: the two windows.
- **Create Groom from Selection** and **Guides from Photos (experimental)...**.
- **Groom Files** (Import Groom File..., Reload from Groom File, Save to Groom File, Save Groom File
  As...): grooms to and from `.groom` files, the standalone app's too (see [Importing and reloading
  groom files](cli.md#importing-and-reloading-groom-files)).
- **Import** (From XGen..., From Yeti..., Alembic Curves...: an Alembic file's curves as the groom's
  guides through an ImportCurves node, a groom made first on a selected mesh) and **Add** (Feathers..., Braid on Curves..., Cards,
  Collider, Wind, Jiggle).
- **Simulate** (Add Simulate Node, Simulate Range, Bake Simulation, Clear Simulation; nHair under
  **Legacy**), **Review** (Density Heatmap and Strand Count HUD, check items; Turntable, Render
  Turntable...), **Render** (Render from Groom File..., Render as Curves, and RenderMan Procedural
  while RenderMan for Maya is loaded: see [Rendering with RenderMan](rendering.md#rendering-with-renderman)) and **Export** (Alembic...,
  USD..., Unreal Groom..., the game cards, the scales).
- **Utilities**: guides and strands to and from Maya curves and meshes, baking a node to guides,
  Bake Scale Maps, transferring to another scalp, copying the graph to other grooms (see [Utilities](pipeline.md#utilities)).
- **Theme**, **Install GroomLab Hotkeys** and **Help** (Docs, Keyboard Shortcuts, Check for
  Updates..., Support GroomLab, About GroomLab).
- Every item's failure is said over the viewport in plain words with what to do next, the
  traceback in the Script Editor; "..." ends the items that open a dialog or window.

The groom's **Attribute Editor** shows what the groom is made of. **Inputs** comes first:
- **Scalp:** the mesh the hair grows on. **Pick** grows the groom on the selected mesh (a groom
  with guides asks **Closest Point** or **Fit and Move**; see [Transfer to a new scalp](pipeline.md#transfer-to-a-new-scalp)), and
  **Select** selects the mesh.
- **Rest pose:** only for a scalp that moves (deformers, a cache): the scalp before it deforms,
  where strands are placed so the same strands stay put on every frame. A groom on a deformed
  scalp takes it on its own ("auto"); **Set** and **Clear** are for other cases (see [Rest mesh](pipeline.md#rest-mesh)).
  A still scalp doesn't show the row.
- **Guides:** what the groom has, e.g. "119 guides in 1 group"; **Edit** opens the Groom Tools.
- **File:** the groom file the groom is linked to (imported from, saved to); **Reload** brings its
  edits back (one undo step), **Save** writes the groom to it.

**Controls** follows, with the keyable attributes that Control nodes and keyed params add (the
`ctl_` attributes; folded while the groom has none),
then the display and render settings. Plumbing the GroomLab tools set (`hiddenGuideGroups`,
`guideColorLayer`, `strandColorSource`, `shotSculpt`, `shotLayer`) is hidden there. The button row opens the **Groom Tools** and the **Graph
Editor**. By script:

```python
groomlab.set_scalp(groom, "newHead", fit=False)   # like Pick
groomlab.scalp_mesh(groom); groomlab.rest_mesh(groom)
groomlab.inputs_summary(groom)                    # scalp, rest, guides, groups, curves, sim, cache
```

The **Groom Tools** (first item in the menu; `groomlab.ui.palette.show(tab=...)` with `"tools"`,
`"paint"`, `"correctives"`, `"photos"` or `"guides"`: Tools with the guide groups open) is the grooming hub, docked beside the viewport. Its header
picks what the tools work on: the groom (the selected one, or one picked to stay on it), an eye
to show or hide it, a Graph Editor button and a **...** menu (Create Groom from Selection,
Theme); under it, the **Group** the brushes edit, with **Isolate**. With no groom to work on,
a **Start a groom** card takes its place: select the scalp mesh (and any guide curves) and
click **Create Groom from Selection**, or **Import** one (XGen, Yeti, Alembic curves, a groom file). Until the groom has guides and a look, a
**Your first groom** card lists those steps, ticks each off as the groom gets there, and takes
you to the next one on a click (its close button hides it for good). Then four tabs (Tools,
Paint, Correctives, Photos; `show(tab=...)` also takes the old ids `"sculpt"` and `"reference"`);
below the tabs and the Brush card, an **Output** section runs what comes after grooming: **Simulate
Range**, **Export** (Alembic, USD, Unreal, Unity, glTF, a groom file), **Render** (from a groom
file, as curves, a turntable), **Transfer...** and **Import** (the GroomLab menu's items, one table):
- **Tools:** every tool as an icon, in groups (the core's brush catalog, `brush_catalog.h`):
  Create (Place, Draw, Part, Flow), Shape (Comb, Slide, Smooth, Straighten, Lift, Twist, Curl,
  Attract, Direction, Noise, Tube), Trim (Cut, Length, Erase), Paint (Paint, Width, Density) and
  Guides (Select, Freeze, with the Unfreeze All tile beside Freeze). The tiles reflow to the
  panel's width. Click one and drag in the viewport over the groom.
- **Guide groups & edits** (a section of the Tools tab; click its title to open it): the guide
  groups, each row with a visibility checkbox, its name (double-click to rename; graph params
  naming it follow), a color swatch (click to give the group its own viewport color,
  `groomlab.set_guide_group_color`; right-click to clear), its guide count and level; the name's
  tooltip lists the graph nodes naming it. **+ New Group** and the small buttons over the list
  make a new group, rename, select the group's guides and delete; right-click a row for the
  same and Color. A group without a color of its own draws in an automatic hue of its own (the
  swatch shows it, dashed). Clicking a
  group makes it the brush group. **Segments** (a slider under it) shows the selected guides' point count, or every guide's with none
  selected, resamples them live while dragged and applies on release (one undo step). **Edit Guides** holds Children..., Sync +X → -X, Sync -X → +X, Mirror
  Selection, Mirror Group..., Reparent..., Promote / Demote..., From Curves and Clear Selection.
- **Paint:** the Paint brush and the maps of every kind (see Painted maps in [Maps](looks-and-maps.md#maps)).
- **Correctives:** Correctives (see [Correctives](guides-and-brushes.md#correctives-sculpt-layers-and-shot-fixes)).
- **Photos:** photo cameras to groom to, and Guides from Photos (see [Grooming from reference](guides-and-brushes.md#grooming-from-reference)).
- **Settings:** below the tabs, the active tool's name, what it does, and one card with only
  what that tool uses: radius, strength (signed for tools that work both ways), falloff, the
  Falloff curve and Root to tip curves, selected only, pen pressure, keep off scalp, mask by scalp (on: a stroke never reaches what the head hides from the camera, such as guides, parts, flow arrows or paint on its far side), and the tool's own options: New guides
  (Place and Draw: length, segments, spacing and neighbor matching), Solo new guides (Place and
  Draw), Parts (name, the list,
  **From Curves**, Remove, Clear All), Flow (**Place** arrows or a sink, **Clear All**), Stretch
  (Comb), Erases (Guides, Parts, Flow), Acts on (Guides, Parts, Flow for the shaping brushes),
  Selection (**Soft radius**) and Freeze mask (**Unfreeze All**). **While sculpting** (folded
  away under the card) sets how the strands draw during a stroke.
- **Group:** the guide group brushes edit and Place and Draw add new guides to, with **Symmetry**
  (World X or Topology; Alt+Shift+X with the hotkey set) beside it, shown whatever the tool; children follow
  their parents. **All groups** brushes every guide. **New Group...** makes a group, top-level
  or a child of another (`groomlab.add_guide_group(groom, name, parent=None)`); a **Guides**
  node in the Graph Editor reads it by name.
- **Staying in sync:** radius, strength and group changed in the viewport (middle-drag,
  B/M-drag, Shift+click a guide to solo it and take its group) show up here.
- **Icons:** the SVG icons live in the module's `icons` folder, on Maya's icon path for shelves
  too.

The **Graph Editor** is a dockable node editor for the selected groom:
- **Nodes:** each is a card with a category color, its name and type, and ports colored by
  type. The categories follow the order a groom is built in (the Add Node menu, the same in the app
  and Blender, from the core's `node_catalog.h` and each node's own registration): Sources (Scalp, Guides, ColliderMesh, ImportCurves,
  Linked Groom), Grow (Scatter, Interpolate, Cull), Shape (Length, Width, Resample, Direction, Flow,
  Part, Smooth, Mirror, Restore Length, Sculpt), Deform (Bend, Transform, Tie, Blend Neighbors,
  Gravity, Displace), Collide (Collide, Self Collide, Mesh Cut), Look (Clump, Twist, Braid, Noise,
  Curl, Flyaway, Static, Tangle, Wet, Instance, Scales), Motion (Wind, Jiggle, Simulate), Feathers
  (Propagate, Plume, Split), Color (Hair Color, Bands, Feather Color, Render Data), Convert (To Guides,
  To Strands, Strand Mesh), Fields & Curves, Utility (Value, Group, Blend, Attribute, Merge, Switch,
  Output) and Games (Cards). The menus show these spaced labels, each node's description as its
  tooltip. A port is hollow when free and filled when linked. A short summary shows the params that differ from their defaults.
- **Wiring:** drag from an output port to an input port to connect.
  - **Snapping:** compatible ports light up while you drag, and a drop near one snaps to it.
  - **Moving a link:** drag a linked input to move its link elsewhere, or drop it on empty space
    to remove it.
  - **Cutting links:** Alt+Shift+drag across links (Hypershade's gesture): the ones the line
    crosses turn red, and letting go disconnects them, as one edit. A cut that leaves the output
    unconnected is not applied until it is wired again; Ctrl+Z puts the graph back.
  - **Pins:** double-click a link to put a pin on it (Unreal's reroute nodes): the link runs
    through it, and dragging the pin routes the link, to tidy crossing wires. Links from one output
    can share pins: drop a pin on another pin of the same output and they join, the links running
    through it as a bundle. Delete takes a selected pin out (the link straightens there). Pins are
    layout, as node positions are: no undo step, nothing evaluates again, and the groom file keeps
    them (the "pins" of its graph; the app and Blender carry them but don't draw them yet).
  - **Refused links:** mismatched types and cycles are refused, with the reason in the status
    bar.
- **Toolbar:** the groom, **Add Node**, **Presets**, **Snap** (nodes and pins you move land on
  the grid's dots; remembered between sessions) and **Live** (with **Apply** while Live is
  off); the **...** menu holds Undo and Redo, Frame, Tidy Layout, Group into Compound (Ctrl+G), Ungroup
  Compound (Ctrl+Shift+G; also on a Compound's right-click menu and in its panel), Delete, Copy
  Graph and Paste Graph (also in the Presets menu: see Sharing a look as text), Units, Theme and
  Reload from Groom.
- **Nodes:** Add Node inserts after the selected node. Delete (the key) removes the selection.
  Tidy Layout re-arranges the graph. The canvas is open: pan (middle-drag or Alt+drag) and zoom (wheel) anywhere, past the nodes. A graph opens at a readable size: all of it when it fits, else Interpolate to Output (where look nodes go), or the output end. **Frame** (F) fits the selected nodes, or the whole graph; A fits everything.
- **Parameters:** the side panel edits the selected node, under its name, its type (in its
  category's color) and a line on what it does. Params read as words ("Guide radius"); the
  tooltip gives the param's name for scripts.
  - **Numbers:** each has a slider and a field. The slider covers a soft range guessed from the
    default: 0–1 for fractions, or a round number above larger values.
  - **Stretching the range:** type a value outside the range and it stretches to reach it.
    Right-click the slider to fit, set or reset the range. Stretched ranges are remembered per
    operator parameter across sessions.
  - **Dragging:** the groom updates live while you drag; releasing makes one undo step.
  - **Keys:** right-click a param > **Set Key**, or **S** with the mouse over it, keys it at the
    current frame. The first key links it to its own keyable attribute on the groom,
    `ctl_<node>_<param>` (the graph's `paramControls`; three channels for a vector, a color
    attribute for a color, a long with stepped keys for a switch), in one undo step, with no node
    on the canvas. Its field takes the Channel Box's colors (red: a key on this frame, pink:
    animated, yellow: driven by a Control node or a connection) and shows the value on each
    frame. An edit sets a key with Auto Key on, else the value until the time changes. **Delete
    Key**, **Remove Animation** (the value now back into the param) and **Select Attribute** are
    in the same menu. Params that set counts, segments, card topology or seeds can't be keyed
    (the schema's `animatable`: false). `groomlab.set_param_key`, `delete_param_key`,
    `remove_param_animation`, `param_key_state` by script.
  - **Mappable parameters:** the **Map** button beside the value switches it to a texture, a
    color set, a strand attribute, an expression or a **painted map** (a guide layer), and back. The yellow
    **Paint** chip at the row's end (as in the app), or right-click the name for **Paint**, offers On Guides / On Scalp Vertices / As Texture: a map of that kind, white, feeds the
    param through a Map node (white = the value now, black = 0), and Groom Tools > Paint opens on
    it (the brush lowers the param, Ctrl raises it).
  - **Node buttons:** nodes that need something from the scene have it under their params:
    **Cards** (Show cards, Show textures, LOD, Bake Maps, Bake Scalp, Export Unreal Cards), **Braid** (Paths from Selected Curves),
    **HairColor** (Create Hair Shader) and **Instance** (Prototype from Selection).
  - **Inputs:** the **Scalp** node shows the mesh it reads, with **Pick from Selection** (grow
    the groom on the selected mesh) and **Select**, and, for a moving scalp, the rest pose with
    **Set** and **Clear**. The **Guides** node shows the guides the groom has, with **Groups...** (the
    Groom Tools' Guide groups & edits). Plumbing params (the Scalp input, the Guides input and hierarchy)
    stay hidden unless changed.
- **Adding nodes:** press Tab or right-click the canvas to search. Type part of a name
  or of what it does ("remap curve"), then Enter adds it there. Names come first, then a few
  search words ("symmetry" finds Mirror, "frizz" finds Noise), params ("density" finds Scatter),
  descriptions and the category; each result shows its category and a line on what it does. With
  one node selected, the new node is wired after it (as Add Node wires it). With no groom
  selected, Tab says to select one.
- **Linking:** drag from a port to a port. Over a port that can't take the link, a tooltip and
  the status line say why ("guides.guides gives Guides; scatter.scalp takes Mesh"); dropping it
  there changes nothing, a picked-up link stays where it was.
- **Header toggles:** every node header has two, as in Yeti. **Focus** (eye) shows the groom
  up to that node in the viewport, and **Bypass** (power, B) passes the node's input straight
  through; the node dims. Focus on a **Clump** draws its clump centers as dots on the scalp over
  its result (in the theme's `clumpCenters` color).
- **Live dragging:** every slider in the side panel (params and an Expression node's variables)
  shows the groom change while you drag; letting go makes one undo step.
- **Real units:** lengths, widths and densities show in real units (**...** > Units):
  lengths in cm by default (mm, m, in), widths in µm (mm, cm), density per cm² (mm², in²). Groom
  data stays in scene units, which are centimeters (Maya's API works in centimeters whatever the
  UI unit), so nothing is rescaled; map min/max and the Groom Tools' new-guide length follow too.
  `groomlab.units.set_unit("width", "um")` by script.
- **Curves:** ramp params (Clump profile, FieldRemap's curve and others) get a curve editor.
  - **Editing:** drag keys, click to add one, right-click a key to remove it, or right-click
    elsewhere for presets (rising, bell, tip only, flip and more).
  - **Sharing a curve:** expose a ramp param as an input and plug a **Curve** node into it, so
    one curve drives several nodes. The Curve node adds `interpolation` (smooth, ease or linear), `flip`, `min` and
    `max` (output = min + (max - min) x curve).
  - **Reshaping values:** a Curve plugged into FieldRemap's curve reshapes any field (noise, a
    texture, a painted map).
- **Keys on the selection:** Ctrl+C / Ctrl+V / Ctrl+D copy, paste and duplicate nodes,
  keeping the links between them.
  - **V:** views a node; the viewport shows its strands, while renders and exports still
    use the output (`viewNode`).
  - **B:** bypasses a node; it passes its input through, for a with/without comparison.
- **Node feedback:** a node that fails shows a red badge with the error in its tooltip, and a red
  line under its header in the param panel when it is selected (in plain words). Each
  node shows how long it took the last time it ran (`groomLabGraphStats`). A warning badge
  flags a Cards node whose baked maps are stale, and a node with a note (a Jiggle with no scalp
  motion to react to). `cmds.groomLabGraphStats(groom, points=True)` (`groomlab.viewport_points`)
  returns the points of the strands the viewport shows, to compare exports with.
- **Backdrops:** right-click a node and choose Add Backdrop Around Selection to frame nodes.
  Moving the backdrop moves them, and double-click renames it. Backdrops are layout only.
- **Sliders:** right-click one for Reset to Default.
- **Applying:** with **Live** on, each valid edit applies as one undo step. With it off, press
  Apply. An invalid graph (a missing input or no Output) is shown but never applied.
- **Layout storage:** node positions live in the groom's `graphUi` attribute, so moving nodes
  never re-evaluates the groom.

**Presets** are graph JSON files. The Graph Editor's **Presets** button lists them in a submenu
per category, each with its description as the tooltip; pick one to load it (with its layout
and backdrops), or save the graph (**Save as Preset...**); there is no preset item in the
GroomLab menu. They are found in `GROOMLAB_PRESET_PATH`, then the user folder (`<Maya app
dir>/groomlab/presets`, or `GROOMLAB_USER_PRESETS`), then the ones shipped with the module. Your
own presets (no category) sit at the menu's top level.

**Sharing a look as text.** **Presets** > **Copy Graph** (also in the **...** menu) puts the
groom's whole graph on the clipboard as JSON text, for a chat, a forum or a file:

- **The text is a preset:** what Save as Preset writes (`version`, `size`, `about` titled after
  the groom in the "Shared" category, `nodes`, `connections`, `output`, `backdrops`), plus
  `controls`: the values of the groom's `ctl_` attributes (`{"ctl_curls": 0.7, "ctl_color_dye":
  [0.5, 0.4, 0.3]}`). Keys do not travel. Saved as a `.json` file in a presets folder, it loads
  like any preset, its control values too.
- **Insert After Selected Node** (each preset's submenu, beside **Apply**) keeps the groom's graph
  and adds the preset's look to it: every node between the preset's sources and its Output becomes
  one Compound (named after the preset, fitted to this scalp) placed after the selected node, or
  before the Output when nothing is selected, fed by it and feeding what it fed; one undo step.
  The **Add-ons** presets (`wet`, `frizz`, `flyaways`, `cards_mid`, `gravity`, `two_tone_tips`) are small looks made
  for inserting; any preset inserts the same way (`groomlab_core.insert_preset_json`, the core's
  `insertPresetJson`). The Compound exposes the look's key params (the preset's `"exposed"`
  list, below), each with its inner param's range, unit and group, so it is tweaked, keyed and
  saved as one node.
- **Paste Graph** replaces the shown groom's graph with the clipboard's: lengths, widths,
  densities and keyed lengths fitted to this scalp, the layout and the control values set, one
  undo step. A ```` ```json ```` code block around the text is fine.
- **What did not travel** is listed in the status bar: painted maps, color sets, files (textures,
  Alembic paths) that do not exist here, Instance prototypes, colliders, guide groups and parts.
  Nodes reading them use their defaults until they exist.
- **Safety:** the text is parsed as JSON only and checked against the operators (unknown node
  types, params and links are refused, nothing changes, and the reason shows in the status bar).
  Text over 4 MB or graphs over 2,000 nodes are refused. File paths on other machines
  (`\\host\share`) are left out, so a pasted graph never makes Maya reach a stranger's server.

The shipped presets are a small core set of starting points and lessons: each is one good-looking
example of one technique, in a small graph whose node names say what they do. The hair presets are
made on a 20 cm head, the fur presets on a wolf-sized body (100 cm), the feather designs on a
2-unit bird; all fit any scalp (see Presets fit the scalp, under [Flow arrows](guides-and-brushes.md#flow-arrows-guideless-short-hair)). Their clumps are
sized in scene units (Clump's `size`, with `clumpBy` size, fitted like any length), so a whole body
gets more clumps of the same size than a head, not the same number stretched over it.

| Category | Preset | What it shows |
| --- | --- | --- |
| Hair | `hair_basics` | The chain every groom builds on: Scatter, Interpolate, Clump (picking its own centers by `size`), a little Noise, HairColor, Width, Resample |
| Hair | `cinematic_hair` | The hero showcase: big locks and finer clumps inside them (two Clump levels picking their own centers by `size`, the strands waved by a Noise before each Clump: `lockWave`, `clumpWave`), restrained noise, frizz on a few strands near the tips (a Noise named `frizzNoise`), a few flyaways, uneven lengths and widths, color variation |
| Hair | `curly` | Ringlets: small clumps curled together by Curl with `coherence` `clump`, a little frizz, the segments curls need |
| Hair | `braids` | Box braids with no curves to draw: the Braid node picks its own paths, one per 2.6 cm of scalp (`region`, exposed as `braidSpacing`) |
| Hair | `parted_style` | A part drawn with the Part tool: Interpolate and Clump respect it, the Part node opens it |
| Hair | `tube_locks` | Tube locks (anime): Interpolate with `fillTubes` and `tubeBlend` fills each guide's tube, a Clump with `followTubes` and soft clumps, light Noise, HairColor, tapered Width, Resample; place guides, shape their tubes with the Shape brush |
| Fur | `creature_fur` | Fur for a whole animal: laid by flow arrows (Flow, no guides), soft tufts about a centimetre across, lighter tips |
| Fur | `agouti_fur` | The creature fur in wild-type banding: guard hairs dark, a pale band, a black tip, the bands wandering hair to hair over a plain undercoat (HairColor, then Bands with guard hairs) |
| Fur | `white_wool` | Dense, crimped white fleece in soft locks: a picked off-white (HairColor's `color` mode, so no pigment browns it), a little brightness variation, white tips |
| Fur | `flow_with_guides` | Flow fur and guided, clumped hair blended by a painted map (`guideAreas`, made black on apply: paint it white where the guides take over) |
| Feathers | `layered_feathers` | The contour design's feathers laid over one another like roof tiles and off the body (Plume) |
| Feathers | `hand_placed_feathers` | Flight feathers (hand-placed): one flight feather per guide of the `feathers` group, made empty on apply with the Draw tool drawing into it (`"requires": {"groups"}`), for wings and tails (see [Feathers](feathers-braids-scales.md#feathers)) |
| Feathers | `feathers_contour`, `feathers_down`, `feathers_flight`, `feathers_afterfeather`, `feathers_barred` | The Feathers tool's designs, as Compounds (see [Feathers](feathers-braids-scales.md#feathers); `feathers_barred`: [Color patterns](looks-and-maps.md#color-patterns)) |
| Feathers | `feathers_spotted`, `feathers_eye_spot`, `feathers_iridescent` | The barred design's other color patterns: pale spots on both vanes, a ringed eye spot that shimmers blue, a dark feather shimmering green to blue (stylized; see [Color patterns](looks-and-maps.md#color-patterns)) |
| Scales | `dragon_scales` | Scales laid out by strands: a Bend combs them down the body, Transform (`scale`, `scaleRandom`) and Width size them (bigger on the back), HairColor colors them (darker tips), a Scales node lays a built-in keeled plate on each root (shields on the belly), tiled and bent onto the skin |
| Scales | `scales_from_guides` | Scales pointed by guides (Interpolate): comb or brush the guides and the scales turn with them; a Bend lays them down the body until there are guides |
| Scales | `scales_from_flow` | Scales pointed by Flow, no guides: draw flow arrows and the scales follow |
| Scales | `swept_spikes` | Long, narrow plates in `follow` `strand`: spikes or quills based on the skin that bend along their guides' curves |
| Motion | `everyday_sim` | Shoulder-length hair whose guides go through Simulate (calm settings: some bend, damping, a soft root hold) before Interpolate |
| Motion | `wind_and_jiggle` | Wind and Jiggle after the look and before Collide: motion with no cache |
| Techniques | `expression_driven` | Expression nodes: longer hair at the crown with a keyable variable, harder clumps on long strands |
| Techniques | `painted_maps` | One color map packing three masks: R clumps, G frizz, B length (the map is made on apply) |
| Techniques | `blend_two_looks` | A Blend of two looks of the same strands, its `mix` driven by a Control (the keyable `ctl_curls`) |
| Techniques | `merge_layers` | Layers, compounds and switches: a sparse, longer layer grown from its own Scatter and waved by a Compound with exposed params (`wavyFrizz`), merged onto the base; a Switch (`pickLook`) picks the full look or a light one (a third of the base) |
| Techniques | `vine_leaves` | The Instance node: leaves bent along curling vines (`deform` `alongStrand`; stylized) |
| Games | `game_cards` | A Cards node at the mid level of detail, ready to bake and export |
| Add-ons | `wet` | For inserting: a Wet node (small clumps near the root merging into bigger ones toward the tip) and a darker color, as hair just out of the water |
| Add-ons | `frizz` | For inserting: kinky noise on a share of the strands, growing toward the tips |
| Add-ons | `flyaways` | For inserting: a few strands leave the flow near the tip and stand out |
| Add-ons | `cards_mid` | For inserting before the Output: a Cards node at the mid level of detail |
| Add-ons | `gravity` | For inserting: strands hang under their weight, stiff at the root (a Collide after it keeps them off the scalp) |
| Add-ons | `two_tone_tips` | For inserting: a picked root color (a skin tone) blending to white tips, each strand a little lighter or darker and its tip starting at its own place |

A preset file is a graph plus a few optional fields of its own, none of which reach the groom's
graph:

```json
{
  "version": 1,
  "size": 20.0,
  "about": {"title": "Painted maps", "category": "Techniques",
            "description": "One plain sentence: what the look is."},
  "requires": {"maps": [{"name": "groomMasks", "kind": "vertices", "color": [1.0, 0.1, 1.0]}]},
  "exposed": [{"node": "clump", "param": "strength", "name": "clumpStrength"}, {"node": "width", "param": "width"}],
  "nodes": [...], "connections": [...], "output": "output",
  "backdrops": [{"title": "a: sleek", "x": 944, "y": -40, "w": 240, "h": 168, "color": [70, 90, 120]}]
}
```

`size` is the scalp size it was made on (for fitting); `about` is what the menu shows (the
categories, in order: `Hair`, `Fur`, `Feathers`, `Scales`, `Motion`, `Techniques`, `Games`, `Add-ons`); `requires` lists
painted maps the graph reads, made as `add_map` makes them when the groom has none of that name,
and (`"groups": [{"name": "feathers", "draw": true}]`) guide groups the graph reads, made empty
when missing; with `draw` the Graph Editor (the app, Blender) then picks the Draw tool drawing
into that group (`hand_placed_feathers`: every guide drawn is a feather; `presets.required_groups`,
`presets.draw_group`; the core's `presetRequiredGroups`);
`exposed` lists the params the Compound shows when the preset is inserted (Insert after selected
node): a look node and its param each, `name` optional (the param's name; node and param,
`clumpStrength`, when the Compound has that name already), entries naming a node outside the
look skipped. Without the list a default pick shows: one param per look node for up to six,
the nodes of the types Clump, Wet, Curl, Noise, Braid, Tangle, Flyaway, Gravity, Bend, Length,
HairColor and Width first, each node's first float or int param that is not advanced and holds
a plain number. Every shipped preset with a look lists its own (1 to 8);
`backdrops` frame parts of the graph in the editor. `presets.check(preset)` lists what does not
fit the operators (unknown node types or params, values of the wrong kind or not among a param's
choices, links to ports that do not exist): the plugin ignores unknown params, so a test runs it
on every shipped preset.

The same editing is available from Python:

```python
from groomlab import graph, presets

model = graph.GraphModel.from_groom(groom)
model.add_node("Curl", name="curl")
model.insert_between("length", "width", "curl")
model.set_param("curl", "radius", 0.08)
model.save(groom)                  # validates, then writes the graph (undoable)

presets.save_preset(groom, "my_look", about={"title": "My look", "category": "Hair",
                                             "description": "Wavy, for the lead."})
presets.apply_preset(other_groom, "my_look")   # the graph, its layout and the maps it reads: one undo step
presets.list_presets(details=True)            # [{"name", "title", "description", "category", "path"}]
presets.check(presets.read_preset("my_look")) # [] when every node and param fits the operators

text = groomlab.copy_graph_text(groom)        # Copy Graph: the look as preset JSON, with control values
report = groomlab.paste_graph_text(other_groom, text)  # Paste Graph: checked, fitted, one undo step
report["missing"]   # {"maps", "colorSets", "files", "meshes", "colliders", "groups", "parts"} this groom lacks
report["message"]   # the line the Graph Editor's status bar shows
```

`paste_graph_text` raises `ValueError` (nothing changed) for text that is not a GroomLab graph.
Its report also holds `"title"`, `"nodes"`, `"fit"` (the sizes fitted from and to, or None),
`"controls"` (the control attributes set), `"controlsSkipped"` (and why: keyed on this groom,
say) and `"removed"` (params naming files on other machines, left out).

`graph.OperatorCatalog.from_maya()` lists every operator with its inputs, outputs and parameter
specs (the `groomLabOperators` command returns the same as JSON: `"outputs"` lists every output
port, `"output"` is the first).

**Compound nodes.** A compound packages part of a graph as one node. Select nodes and press
Group in the editor, or in Python:

```python
name = model.collapse(["noise", "curl"])           # one Compound node in their place
model.expose(name, "amount", "noise.amplitude")    # an inner param on the compound
model.set_param(name, "amount", 0.08)
model.expand(name)                                 # back to the nodes, exposed values kept
```

- **Inputs and output:** sources outside the group become the compound's inputs (`Input` nodes
  inside; one per source node and output it is read from). The one node feeding the rest of the
  graph is its output: a compound has one output, that node's first (links inside it may read
  any output of an inner node).
- **Storage:** the definition is stored readably in the graph JSON, and compounds nest.
- **Caching:** the outer graph caches a compound as one node.
- **Reuse:** presets carry compounds, so a compound saved in a preset is reusable across grooms.
- **Repeat:** `repeat` (1-64) runs the subgraph that many passes, each pass taking the previous
  pass's output as the compound's first strands input (the other inputs stay): a `Noise ->
  Smooth` compound repeated 4 times builds up like an iterative relax. An inner `Input` node
  named `iteration` of type `Field` yields the pass index (0, 1, ...) as a field, so a param
  exposed as an input (through a `FieldRemap`, say) can vary from pass to pass. With `repeat` 1
  the compound is what it was.
