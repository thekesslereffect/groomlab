# The standalone app against the Maya plugin: parity

Every artist-facing capability of the Maya plugin (the menu, the Groom Tools palette and its
tabs, the Graph Editor, the Attribute Editor template, the panels, the brush settings, the
hotkeys, the exports, the utilities, the `.groom` round trip) with its Maya entry point and
whether the standalone app (`hosts/app`) has it. The app's own UI is the floating glass over the
viewport ([DEVELOPING.md](DEVELOPING.md), "Standalone app"); it needs no equivalent of Maya's
docks, but every edit an artist makes in Maya should be possible in it.

Status: **present** (the app does it), **partial** (part of it, or another way), **missing**
(not yet; the "Still missing" list at the end ranks them), **Maya only** (by design: it depends
on Maya's own nodes, animCurves, its viewport camera, nHair, FBX, MtoA, the Channel Box, or more
than one Maya groom at once; the reason in the row). Shared logic lives in `engine/core`, so
both hosts run one implementation; where a row says "core: ..." that is the piece both call.

Summary: 160 rows. Before this pass: 66 present, 22 partial, 52 missing, 20 Maya only. After it:
89 present, 21 partial, 30 missing, 20 Maya only (the 26 rows marked *new* changed: 22 were
missing, 3 partial, and the number sliders were remade).

## Glossary: one name per thing in every host

Maya, the app and Blender use these names in menus, panels, tooltips, messages and the docs. Where a
host can't use the name (a Maya or Blender convention), the row says so. Node categories, node
labels, search words and number decimals come from the core (`node_catalog.h`, the specs'
`decimals`), and the keyboard shortcuts from one table (`shortcuts.h`), so hosts can't drift.

| Thing | Name everywhere | Not |
| --- | --- | --- |
| The window or overlay with the node graph | **Graph Editor** (the app's mode pill reads "Graph", its tooltip "Graph Editor") | Node Graph, node editor |
| The list of nodes beside it (app) | **Node List** | Nodes |
| The selected node's params | **Node Settings** (Maya: the Graph Editor's right panel; Blender: the node's sidebar) | Node, Params |
| The lighting, material and surfaces panel (app) | **Look Settings** (the mode is **Look**) | Look (for the panel) |
| The active tool's settings | **Brush** (Maya: the Groom Tools "Brush" card; app: the Brush panel) | Brush Settings |
| Pass a node's input straight through | **Bypass** (B); undo with Bypass again | Disable, Mute (Blender's M mutes the same way) |
| View the groom up to a node | **Focus** (V) | View, Solo |
| Another .groom file's result as a node (type `Reference` in files) | **Linked Groom** | Reference node |
| Reference photos and cameras (Maya Groom Tools tab) | **Photos** | Reference (tab) |
| Guides made from photos | **Guides from Photos (experimental)...** | Guides from Reference |
| The tab or panel of corrective sculpts | **Correctives** | Sculpt (tab) |
| The tab or panel of painted maps | Maya: **Paint** tab (it paints); app: **Maps** panel (it lists and paints them) | |
| Move a groom onto another mesh | **Transfer Groom to Scalp...** | Transfer Groom to Mesh |
| Make a groom | Maya and Blender: **Create Groom from Selection** (a mesh selected); app: **New Groom on Mesh...** (opens a mesh file: there is no selection) | Add GroomLab Groom |
| Map kinds | **guides**, **vertices**, **texture** (a "vertex map", a "texture map"); values **gray** (one value) or **color** (r g b a) | weight map, image map |
| HairColor's two ways | **Natural** (the `melanin` mode: pigment and redness, as real hair) and **Picked color** (the `color` mode) | melanin/color in panels; the param panel lists the raw choices |
| Symmetry | **Symmetry** with **World X** or **Topology** | Sym X, Mirror (Mirror is the node) |
| Numbers | the spec's `decimals` (none for counts and pixels, one for degrees, by the slider's span otherwise), a small number with two significant digits (0.035, up to six decimals); trailing zeros dropped; fractions as percentages with one decimal; units after the field (cm, µm, %, °, px, cm/s²) | 0.350 next to 0.35, 1.0000 |
| Node categories (Add Node) | Sources, Grow, Shape, Deform, Collide, Look, Motion, Feathers, Color, Convert, Fields & Curves, Utility, Games, from `node_catalog.cpp` | a host's own list |

## Files: .groom import, reload, save

| Capability | Maya entry point | App |
| --- | --- | --- |
| Open a groom file (graph, guides, layers, correctives, parts, flow, maps, card bake, caches) | GroomLab > Groom Files > Import Groom File... (`import_groom`) | present: File > Open Groom (`GroomDocument::open`); a Z-up file turned Y-up |
| New groom on a mesh | Create Groom from Selection (a mesh and curves) | present: File > New Groom on Mesh (OBJ, Alembic; an animated Alembic mesh becomes the scalp cache); curves as guides: partial (ImportCurves node only) |
| Reload from the groom file | Groom Files > Reload (`reload_groom`), AE File > Reload | present *new*: File > Reload from File (Ctrl+R) |
| Save to the groom file | Groom Files > Save (`save_groom`), AE File > Save | present: File > Save, Save As, the export panel's Save |
| Export a groom file with a scalp cache over a range | Groom Files > Save Groom File As... (`export_groom(scalp_cache=True)`) | partial: Save keeps the caches the file came with; the app writes no new scalp cache |
| Groom file info | `groom_file_info` | present (the CLI's `info`; the app's title and stats) |
| Render from the groom file (Arnold) | Render > Render from Groom File..., Render as Curves | Maya only: MtoA's render modes |
| Find missing files (relink by name from a folder) | Utilities > Find Missing Files... | missing |
| Scene-folder following (Save As moves the files) | `follow_scene_files` | Maya only: Maya's scene save callbacks (a groom's own files live beside it) |

## The node graph (Maya's Graph Editor)

| Capability | Maya entry point | App |
| --- | --- | --- |
| The graph as node cards and wires, pan and zoom, frame | Graph Editor canvas; F, A | present: Tab or the Graph pill; F fits; the graph floats over the viewport |
| Add a node (categories) | Add Node menu; Tab search | present: Tab search (every type, by name, the category colored); a menu by category: partial (the search lists every type) |
| Node search at the cursor | Tab, right-click (`NodeSearch`) | present |
| Wire ports (type check, cycle check, replace what fed) | drag | present (core: `GraphEdit::connect`) |
| Pick a wire up, disconnect | drag a fed input | present |
| Multi-output nodes (a Map's r g b a) | ports per output | present |
| Delete node / wire | Del | present |
| Bypass (Disable) | header toggle, B | present (header dot, B) |
| Focus: view up to a node (viewNode; on a Clump its centers as dots, drawn by the shared DisplayBuilder) | header eye, V | missing |
| Rename a node | context menu Rename... | present *new*: double-click the header or the menu (core: `GraphEdit::renameNode`) |
| Duplicate a node | Ctrl+D | present *new* (core: `GraphEdit::addNode` with the params) |
| Set as output | context menu | present *new* (core: `GraphEdit::setOutputNode`) |
| Copy / paste nodes with their links | Ctrl+C / Ctrl+V | missing (Copy Graph / Paste Graph move whole graphs) |
| Rubber-band multi-selection | drag on empty canvas | missing (one node at a time; a drag pans) |
| Group into / ungroup a Compound | Ctrl+G, Ctrl+Shift+G | missing (the collapse logic is Maya's Python GraphModel; a core port is the way) |
| Tidy layout | More > Tidy Layout | present *new*: Ctrl+L or the canvas menu (core: `GraphEdit::clearPositions`) |
| Node positions kept with the groom | `commit_layout` | present (the graph's `"ui"`, shared with Maya) |
| Backdrops (drawn, moved, resized, renamed, colored) | Add Backdrop Around Selection | partial *new*: drawn with their title and color (a preset's, Maya's); not made or moved here |
| Presets menu: a submenu per category, descriptions, fitted to the scalp, required maps made, controls set | Presets menu (`presets.py`) | present *new*: the Presets pill beside the Graph pill, View > Graph Presets (core: `presets.h`; the same files) |
| Insert a preset after the selected node (its look as one Compound, fitted; the Add-ons presets) | each preset's submenu > Insert After Selected Node | present *new*: the same item in the Presets pill's submenus (core: `insertPresetJson`) |
| Save as Preset | Presets > Save as Preset... | present *new*: a glass prompt, saved to the shared user folder |
| Copy Graph / Paste Graph as text | Presets > Copy / Paste Graph | present *new* (the paste reports what was fitted and made) |
| Paste report: what the groom lacks (maps, color sets, files, colliders, groups, parts) | the status bar | partial: maps made and fit reported; the missing inputs are not listed |
| Node error and warning badges | red / orange badges | partial: errors toast and the stats line; no per-node badge |
| Node evaluation time badges | ms with heat colors | present *new*: ms in each header |
| Param summary on the card (values that differ from defaults) | the card body | missing |
| Live / Apply toggle | Live checkbox | present by design: every edit applies live, one undo step |
| Undo / Redo named after the step | More menu, Ctrl+Z/Y | present (top-right, Ctrl+Z, Ctrl+Y, Ctrl+Shift+Z) |
| Units (cm/mm/m, widths, density) | More > Units | missing |
| Theme switch | More > Theme | present (View > Theme: the shipped themes, the Maya host's files; applies on the next start) |
| GPU preview toggle | More > Preview Drags on the GPU | present (Display panel) |
| Reload from groom | More > Reload from Groom | present by design (the view follows the document) |
| Seed warnings for nodes sharing a seed | status bar | partial: the core's `seedWarnings` is there; the app does not show them |
| Scatter guides auto-connect when given a map | automatic | missing |
| Expose a param as an input port | context menu, param menu | present (the map source's Port; a ramp's Curve port) |
| Bake a node to guides | context menu Bake Node to Guides... | missing |
| Cards node actions (stats, show cards/textures, LOD, bake, exports) | node action buttons | present: Cards mode (stats, Strands/Cards/Maps, LOD, Bake, the exports in Export mode) |
| Map node actions (feed a param, paint, relink) | node action buttons | partial: a map is picked from a param's source; painting from the Maps panel; relink missing |
| Braid paths from curves, Instance and Scales prototype from a mesh | node action buttons | Maya only: Maya curves and meshes (an Instance's or Scales' prototype is a scene mesh) |
| Create Hair Shader (aiStandardHair) | HairColor node action | Maya only: MtoA shaders (the app's look is the viewport's) |
| Scalp node: pick / rest pose | node box | partial: rest pose from the file; pick = New Groom on Mesh or Transfer |
| Guides node: the groups summary and "Groups..." | node box | present (the guides panel) |

## Parameters (the Graph Editor's panel, the Attribute Editor)

| Capability | Maya entry point | App |
| --- | --- | --- |
| Number params with a soft-range slider and a field | `SoftSlider` | present *new*: one slider, the number edited inline, the span widening past it |
| Slider right-click: reset to default, set range, fit range | slider menu | missing |
| Values in display units with a suffix | `units.py` | missing |
| Bool, vec3, choice, string params | editors | present |
| A Compound's exposed params | the Attribute Editor, the Graph Editor's panel | present *new* (core: `GraphEdit::nodeSpecJson`, the per-node spec) |
| The node's description under its name | the Attribute Editor template | present *new* |
| Name suggestions (channels, color sets, parts, maps, groups, controls) | editable dropdowns | partial: maps and color sets offered for map sources; the rest typed |
| Guides.group / exclude with a group picker | "+" menu | partial: typed; the guides panel lists the groups |
| Ramp editor (keys, presets, linear, flip) | `RampEditor` | present (presets: constant, up, down, ease, bell, taper; no flip) |
| Color params | color fields | present (a swatch and picker, sRGB shown, linear stored) |
| Mappable params: value / texture / color set / attribute / expression / painted map, component and range | `MappableEditor` | present (a strand attribute source: partial, through an expression) |
| Field-driven params ("x driver") | the panel | present (the port says what feeds it) |
| Paint a param (make a map named after the node and param, wire it, open Paint) | param menu > Paint (a Map node) | present *new*: the Paint pill on a mappable param's row (a vertex map as the param's map source, the Paint brush on it, one undo step; core: the map and painting APIs) |
| Drive with a slider (a Control node) | param menu | missing (add a Control node and wire it by hand) |
| Keys on params (set key, auto key, key colors, S) | param menu, Channel Box | Maya only: animCurves |
| The Controls section (ctl_ attributes) | AE Controls | Maya only: Maya attributes (a preset's control values are applied to the nodes themselves) |
| Expression editor: multi-line, completion, hover help, live check, variables, library | `expression_editor.py` | partial: a line for the expression (checked by the session), variables as JSON; no completion or library |
| Keyable expression variables | the editor's diamond | Maya only: keyable attributes |
| previewDensity, point budget | AE Groom | present (Display panel) |
| Lock the groom against brushes | AE `locked` | missing |
| Display: guides, flow, tubes, instances, strand color modes | AE Display | partial: guides, points, width, strand colors (solid, color set, attribute, random, clump, density), the scalp map; flow, tubes and instances overlays not drawn as such |
| Display: guide colors by group / painted layer, group colors | AE Display > Guides | present (Guide Color: Group or Painted Layer; each group its own color) |
| Display: strand shading (lit, shadows, ribbons, width scale) | AE Display > Strand Shading | present (lookdev: lit ribbons, shadows, width scale; lines with Kajiya-Kay) |
| Display: width as coverage (`displayWidthCoverage`, `displayCoverageDensity`) | AE Display > Strand Shading | present (Display panel's Width coverage and Coverage at full density: the line path's strand lines, hashed, coverage masks on the viewport's multisampled target) |
| Display: screen-space level of detail (`displayClumpLod`, `clumpLodError`) | AE Display > Strand Shading | present (Display panel's Clump LOD, `--display clumpLod=off`, `clumpLodError=x`: the line path's strand lines by class, impostors, the blended pass; the lookdev ribbons draw every strand) |
| Sculpting: display while sculpting, sculpt density, soft selection radius | AE Sculpting | partial: sculpt density and the dimmed strands; soft selection radius missing |
| Game Cards display | AE Game Cards | present (Cards mode) |
| Render LOD (distance, near/far, min density, width compensation, camera cull) | AE Rendering > Level of Detail | Maya only: evaluated against the render camera (the core's `lod.h` is there for a later viewport use) |
| Follow the scalp | AE `followScalp` | present (the timeline's Follow) |
| Colliders: pick, select, remove | AE Inputs | present *new*: every prop loaded beside the groom is a collider by its name (a ColliderMesh node names it); Maya's picking is Maya's |

## Groom Tools: the brushes (Tools tab) and their settings

| Capability | Maya entry point | App |
| --- | --- | --- |
| Place, Draw, Part, Flow, Comb, Slide, Smooth, Lift, Swing, Attract, Tube, Cut, Length, Erase, Paint | the tool tiles (`brush_tool`) | present: the brush rail; both palettes are built from the core's brush catalog (`brush_catalog.h`: one order, Create, Shape, Trim, Paint, Guides, one set of names, hints and settings) |
| Select (click, box; Shift toggle, Ctrl deselect, Ctrl+Shift add) | Select tile | present (box; lasso: partial, a box) |
| Freeze mask (paint, unfreeze all) | Freeze tile | missing (the core's brush honors a `freeze` layer; no tool to paint it) |
| Radius, Strength (signed per tool), Falloff (0 hard, 1 soft, in both) | the card | present (the bar and the brush panel) |
| Smooth strength (Paint) | the card | missing |
| Symmetry X (and center) | the card | present (Sym X, the center in the panel) |
| Selected only, soft selection | the card | partial: selected only; the soft radius missing |
| Pen pressure | the card | present (tablet events) |
| Keep off scalp (collide), Front facing only (one term in both) | the card | present |
| New guides: match neighbors, length, segments, spacing | the card | present |
| Parts box: name, list, from curves, remove, clear | the card | present *new*: the part name in the brush panel, the list, Remove and Clear in the guides panel; from curves: Maya only |
| Flow box: arrows / whorl / gather / part out, clear | the card | present *new* (the sink kind in the brush panel; Clear flow in the guides panel) |
| Stretch (Comb), Straighten (Smooth), Erases, Applies to (guides/parts/flow) | the card | present |
| Display while sculpting (as they are / dimmed / guides only, full density) | the card | partial: the dimmed strands at the sculpt density |
| Gestures: middle-drag radius, Shift+middle strength, Shift+click / drag selects the guides in the ring (painting: Shift smooths), Ctrl the opposite | the viewport | present |
| Hover highlight of the guides to be touched | the viewport | present |
| Brush level (Page Up / Down) | hotkeys | present *new* |
| Isolate a group | the palette | present |
| Groom picker / pin, show or hide the groom | the palette | Maya only: one groom per app window |
| The starter card, status line | the palette | present by design (toasts and the stats line) |
| Theme menu | the palette | present (View > Theme; see the graph section) |

## Guide groups and layers (Tools tab's section, the Paint tab)

| Capability | Maya entry point | App |
| --- | --- | --- |
| Group tree: counts, the brush group, hide/show | `GuidesTab` | present (the guides panel) |
| New group (root or child), rename, delete | the tree | present |
| Select a group's guides | the tree | present |
| Group color (the swatch: its own, else its automatic color) | the tree | present |
| Segments: resample the selected or every guide | the slider | present *new* (Resample N segments) |
| Generate children (count) | Edit Guides > Children (the count inline beside it) | present (Generate children with its count) |
| Sync symmetry (+X to -X and back) | Edit Guides > Sync | present *new* (from +X; core: `mirrorAll`) |
| Mirror selection, mirror a group | Edit Guides | present *new*: a root group mirrored; the selection: partial (mirror the group) |
| Reparent, promote, demote | Edit Guides | partial *new*: promote; reparent and demote missing |
| Guides from curves, guides to curves | Edit Guides, Utilities | Maya only: Maya curves (the app opens curves through an ImportCurves node) |
| Lock / unlock a child guide | API | missing |
| Per-guide attributes (set, list, remove, rename) | API, the Paint tab's guide maps | present (guide layers in the Maps panel) |
| Maps list: name, kind, gray or color, missing-image mark | the Paint tab | present (the Maps panel) |
| New map (guides / vertices / texture, gray or color, resolution) | + New Map | present (filled with 1.0 by default, as Maya's) |
| Rename, delete a map (the graph's readers follow) | the list | present |
| Convert a map between kinds | Convert to | missing |
| Make color / make gray | context menu | missing |
| Relink / reload a texture's image, show in Explorer | context menu | missing |
| Fill a map (value or color, selected guides only) | Fill | missing |
| Paint a map (value, color, channels, Ctrl inverse, Shift smooth) | the Paint tile | present |
| Show a map on guides / strands / scalp, the scalp channel | Show on | present: on the scalp, and as strand colors (color set mode); on the guides: partial |
| "Read by" (the params reading a map) | the line | missing |
| Map view while painting, restored after | automatic | partial: the painted map stays shown on the scalp |

## Correctives (the Sculpt tab)

| Capability | Maya entry point | App |
| --- | --- | --- |
| The layer stack with weights and whether on | `CorrectivesBox` | present *new* (the correctives panel) |
| New layer on every frame; a fix at this frame with a fade | + New, Fix Frame | present *new* |
| The layer the brushes sculpt into (shot sculpting at the frame) | click a row (`shots.set_target`) | present *new* (Sculpt into; core: the stroke host's shot sculpt) |
| Enable, rename, weight, delete | the row | present *new* |
| Move up / down, solo | buttons | missing |
| Keyed weights, Key Weight Here, Clear Keys, Go to Its Frame | the row, context menu | Maya only: animCurves on the transform (a file's weight curves are read) |

## Reference photos and cameras (the Reference tab)

| Capability | Maya entry point | App |
| --- | --- | --- |
| Import a photo as a camera with an image plane; look through it; opacity, depth, focal length, lock | `reference.py`, `tool_tabs.py` ReferenceTab | The Photos tab (`photos_panel.h`): the photo drawn over the viewport through its camera |
| Align Photo: photo and mesh points side by side, the camera solved live | `ui/reference_window.py` (Align stage; core: `reference_edit.h`, `camera_solve.h`) | The Photos window (`reference_window.h`), the same stages |

## Look, lighting and review

| Capability | Maya entry point | App |
| --- | --- | --- |
| The hair's look (HairColor: melanin, redness, tints, specular, roughness, tilt; lobes) | the HairColor node, Create Hair Shader | present: the look card's swatches and its melanin, redness and roughness sliders edit the groom's last HairColor node (undoable, saved with the groom; the node panel has the rest); a viewport-only override lives under More settings, tagged "viewport only" |
| Lighting presets, lights, environment, exposure, tone map, background | Review > Turntable's lights (Maya's) | present (the look card: beyond Maya's viewport) |
| Density heatmap | Review > Density Heatmap | present (the Density color mode) |
| Strand count HUD | Review > Strand Count HUD | present (the stats line) |
| Turntable | Review > Turntable | present *new*: Tools > Turntable turns the camera |
| Render turntable (Arnold batch, mp4) | Review > Render Turntable... | Maya only: Arnold's batch render (the app's Render refines in the viewport) |
| Per-node times, card stats, errors | `groom_stats`, `card_stats`, `errors` | present *new*: the graph's ms badges; the cards panel's stats; errors as toasts |

## Cards and the exports

| Capability | Maya entry point | App |
| --- | --- | --- |
| Cards node settings, LOD presets, stats, stale warning | the Cards node | present (Cards mode; core: `card_presets.h`) |
| Bake the card maps | Bake Maps | present |
| Bake the scalp (the hair cap) | Bake Scalp | partial: the glTF export writes the cap; no separate bake |
| Show cards / textures | Show cards, Show textures | present |
| Alembic strands over a range | Export > Alembic... | present (Export mode, the frames and density) |
| USD strands | Export > USD... | present |
| Unreal groom (groups, guides, feathers' shafts, barbs) | Export > Unreal Groom... | present (core: `engine_export.h`) |
| Unity strands | Export > Unity Strands... | present |
| Unity feathers (barbs, rachis FBX, manifest) | Export > Unity Feathers... | Maya only: the rachis FBX |
| Unreal cards (FBX and maps), Unity cards (FBX) | the Cards node, Export | Maya only: FBX (the app writes the bake's maps and glTF) |
| glTF cards | Export > glTF Cards... | present |
| Map PNG export | `export_map` | missing |
| Strands to curves / mesh, guides to curves | Utilities | Maya only: Maya geometry (the exports carry the strands out) |
| Live card mesh, rachis mesh in the scene | API | Maya only: Maya meshes |

## Simulation and motion

| Capability | Maya entry point | App |
| --- | --- | --- |
| Simulate the guides with nHair, presets, bake, remove | Simulation menu | Maya only: nHair, nucleus |
| Export the guides for a simulation | Simulation > Export Guides... | partial: the Unreal export writes the primary guides; no guide-only cache |
| Load a guide cache (poses by guide ID) | Simulation > Load Guide Cache... | partial: a groom file's guide cache opens and plays; picking another cache missing |
| Wind, Jiggle (the graph's motion) | Add Node > Motion | present |
| Simulate node: play, Simulate Range, bake, clear | Simulate menu (the main progress bar, Esc cancels), `groomlab.sim` | present (the timeline's Simulate, Bake, Clear and its strip of frames) |
| Add > Feathers, Braid on Curves, Cards, Collider, Wind, Jiggle | GroomLab > Add | partial: the Tab search adds any node; Cards mode's Add Cards node; feathers, braids and colliders come from presets or the search |
| Help: Docs, Keyboard Shortcuts, About | GroomLab > Help | present *new*: the menu's Help (floating pages over the viewport, the docs folder or the web) |
| Support GroomLab (opens https://ko-fi.com/getgroomlab), also in About | GroomLab > Help > Support GroomLab | present (Help > Support GroomLab) |
| First run: an empty-state card (Open, New groom on mesh, a preset, recent files, drag and drop) | the GroomLab menu, Maya's own recent files | present *new* (the app's own) |
| Follow the scalp through a cache | `followScalp` | present (the timeline) |

## Utilities

| Capability | Maya entry point | App |
| --- | --- | --- |
| Transfer the groom to another scalp (fit, maps, color sets) | Utilities > Transfer Groom to Scalp... | present *new*: Tools > Transfer Groom to Mesh (core: `transfer`, `projectScalpMaps`, which Maya's command projects with too); color sets: Maya only |
| Transfer methods (closest, bounds, markers align/warp, uv) and marker pairs | the Transfer window's two marker views (click, drag, Ctrl+click, Symmetry X, its own undo) | present: the Transfer window (two views, points clicked on each mesh, symmetry, undo; Import Markers... for a JSON file); the point model is the core's `TransferMarkerEditor`, shared with Maya |
| Copy the graph to other grooms | Utilities > Copy Graph to Other Grooms... | Maya only: several Maya grooms (Copy Graph / Paste Graph carries a look between files) |
| Bake a node to guides | Utilities > Bake Node to Guides... | missing |
| Find missing files | Utilities > Find Missing Files... | missing |
| Install hotkeys, marking menu (Y), B/M drags | GroomLab > Install Hotkeys | partial: B drag, Shift+middle, S, Esc, Tab, F, Page Up/Down are built in; no marking menu or user-bindable commands |
| Theme follow in the viewport | automatic | present by design (the shipped themes are embedded) |

## Still missing, in order of artist value

1. Node graph: rubber-band multi-select, copy and paste of nodes, group into a Compound (the
   collapse logic to move from Maya's Python into the core first).
2. Maps: convert between kinds, make color or gray, fill, relink or reload a texture's image,
   "read by".
3. Expression editor: completion, hover help and the library (the core's `expression.h` has the
   check; the shipped examples are files).
4. Param conveniences: Drive with a slider (a Control node), slider range menu, display units.
5. Guides: group colors and hue by group, lock and unlock children, reparent and demote, isolate
   a level, the Freeze tool and the soft selection radius.
6. Correctives: solo and reorder.
7. Backdrops made, moved and resized in the app (drawn today).
8. Focus (view up to a node), per-node error badges, the param summary on the cards, seed
   warnings shown.
9. Bake a node to guides; Find missing files; a map exported as PNG; a scalp cache written with
   the groom file; another guide cache picked.
10. Units, custom themes (the app reads the shipped ones), a lasso selection.
