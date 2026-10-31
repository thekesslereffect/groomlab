# UI definitions: one UI structure for every host

`engine/ui/*.json` says what GroomLab's menus, panels and lists hold, so Maya, the standalone app
and the Blender add-on show the same things in the same order with the same words. The files are
plain JSON (Apache-2.0 data): a host reads them with no engine loaded. Each host keeps its own
layout and widgets and binds the files' **action ids** to what they run.

| File | Holds |
| --- | --- |
| `actions.json` | Every action: `label`, `tip`, and optionally `labels` / `tips` (per-host words), `shortcut` (a `shortcuts.h` id), `shortcutPart`, `icon` (an icon stem), `checkable`, `hosts`, `needs`, `enabled`, `tipDisabled`. |
| `menus.json` | The GroomLab menu (`main`), its submenus (`samples`, `import`, `export`, `render`, `studio`, `theme`, `help`), the Groom Tools' Output lists (`output.import`, `output.export`...) and the app's menu bar (`app.menuBar`: File `app.file`, Edit `app.edit`, View `app.view`, then `main` as the GroomLab menu and `help`). |
| `palette.json` | The Groom Tools: tabs (with old ids as `aliases`), header tips and its More menu, the empty state, sections (each shown only while the active tool is one of its `tools` and the tab one of its `tabs`, when it lists them), the Output strip (Maya and Blender; the app shows Render and Export in its menu bar corner), the first-groom steps (the app shows them over the viewport), texts (gestures, symmetry tip, erase note, flow hints; `labels` gives a host its own words for one: `{"app": {"gestures": ...}}`). |
| `lists.json` | Each list's action bar (`bar`: entries with a `glyph` go on the right) and right-click menus: guide groups (and Edit Guides), parts, maps, correctives, photos and point pairs. |
| `choices.json` | Option lists: `{label, tip, default, items: [{value, label, tip, short}]}` (`short`: a button's shorter label) (flow sinks, symmetry, sculpt display, tube channels and shapes, map kinds, values, resolutions, feather sources, scalp and paint channels, photo depth, strand colors, evaluation). |
| `marking_menu.json` | The marking menu / pie: `radial` (position, tool), `overflow` (the rest of the brush catalog after a divider), `tail`. |
| `graph_ui.json` | The Graph Editor: category, port and key-state colors (a theme's `graph.categories` and `graph.ports` override them), backdrop colors, plumbing params, file filters, the node, backdrop, Edit, Add Node, Presets, param, key and Paint menus, and `nodeActions`: a node's actions by its type (`"needs": ["type.Cards"]`), pinned at the top of its settings in every host. |
| `properties.json` | The groom's settings in the order of Maya's Attribute Editor (`AEgroomLabShapeTemplate.mel`): `sections` (Groom, Inputs, Controls, Hair Color, Display > Guides / Strand Shading, Sculpting, Game Cards, Rendering > Render Overrides / Level of Detail), each with `items` (`"-"` a divider) and nested `sections`. An item: `id`, `label`, `tip` (`labels` / `tips` per host), `maya` (the attribute), `app` (the app's control), `blender` (the property), `hosts`; a custom block (`custom`: the MEL proc, as `AEgroomLabInputs`) lists the rows it shows as `parts`. |
| `ui.schema.json` | The JSON Schema every file follows (each file holds some of its top-level keys). |

## Entries

A container is a list of entries (or `{label, tip, items}`):

- `"-"`: a divider (kept only between items).
- `"export.alembic"`: an action, as `actions.json` has it.
- `{"id": "file.saveAs", "tip": "..."}`: an action with this place's own label, tip, `glyph`, `needs` or `hosts`.
- `{"submenu": "Export", "id": "export", "items": [...]}` or `{"submenu": ..., "ref": "menus/export"}`: a submenu; `{"ref": "menus/theme"}` takes the container's label and tip too.
- `{"include": "lists/maps/image"}`: another list's entries, in place.
- `{"generator": "name", "action": "id", ...}`: entries made at run time. `choices` (from
  `choices.json`, the option's value passed to the action) and `backdropColors` expand from the
  files; the rest are the host's: `studioPresets` (Maya's Arnold studio), `lightingPresets` (the
  app's Look lighting), `themes`, `themeMenu` (the Theme submenu), `panels` (the app's View > Panels),
  `units`, `nodeCategories`, `presets`, `exposeParams`, `innerParams`, `paramControl`.

`hosts` (`maya`, `blender`, `app`) limits an entry or action to those hosts (none: every host).
`labels` and `tips` (`{"app": "..."}`) give a host its own words where the shared ones name another
host's things (the selected mesh, the playback range, the Render View), on an action, an entry, a
submenu or a container: an entry's label is its place's override for the host, else its place's
`label`, else its action's override, else its action's `label` (tips the same way).
`needs` lists conditions the host knows (`map`, `map.texture`, `!map.color`, `node.compound`,
`param.linkable`, `group`, `photo`, `catalog`, `groom`...): an entry whose conditions do not
hold is left out (an action bar shows everything). `enabled` lists conditions for the enabled state
instead (`tipDisabled` is the tip then; `{why}` and other `{name}`s are filled from the host's values).
Placements are the same id listed in two containers (the GroomLab menu's `export` and the Output
strip's `output.export`).

## Binding the ids

Maya (`hosts/maya/scripts/groomlab/ui/definitions.py`) loads the files from the module's `ui/`
folder (installed beside `themes/`), else from the source tree's `engine/ui`, and resolves a
container for the host and a state (`definitions.resolve`). Each window keeps a table `{action id:
binding}`: a callable, a method name, or a function of the owner (`menu.ACTIONS`,
`ToolPalette.ACTIONS`, `GuidesTab.ACTIONS`, `NodeItem.ACTIONS`, `ParamPanel.param_actions`...);
`widgets.fill_menu` builds a Qt menu from a container and a table, `menu._build` the GroomLab menu
with `cmds.menuItem`. `maya.ui_scaling` checks that every id a Maya container lists has a binding,
and `ui.definitions` checks the files themselves (the schema, every id and reference, every action
used somewhere, and, with `groomlab_core` importable, the shortcut ids and tools against the core).

The app gets the files embedded as Qt resources (`:/groomlab/ui/menus.json`) and reads them with
`hosts/app/src/definitions.h` (`ui::Definitions`: Maya's semantics in C++, host `app`); it binds the
ids in `hosts/app/src/app_actions.cpp` (`AppActions::bindings`: action id -> a function of the main
window, with checked, enabled and text getters and the app's own keys), `ui::fillMenu` builds a
QMenu from a container (an id without a binding is hidden, as in Blender) and
`buildDefinitionsMenuBar` the menu bar from `app.menuBar`. `AppUi.DefinitionsBindEveryShownId`
(`tests/hosts/app/test_definitions.cpp`) fails on any id the app shows that is neither bound nor in
its `ALLOWED_MISSING_APP` list (the app's parity checklist). The Blender extension gets the files
as plain files in `GroomLabBlender/ui/` (`hosts/blender/addon/CMakeLists.txt`). Blender reads them
with `hosts/blender/addon/groomlab/definitions.py` (Maya's semantics, host `blender`) and binds the
ids in `actions.py` (`BINDINGS`: action id -> operator idname and properties); an id without a
binding is hidden, and `blender.addon_ui` fails on any id Blender shows that is neither bound nor in
its `ALLOWED_MISSING` list (the parity checklist). A new host action:
add it to `actions.json`, list it in a container, bind it in each host that shows it (or give it
`hosts`), and run `ctest -R "ui.definitions|maya.ui_scaling|AppUi"`. `ui.definitions` also holds
Maya's and Blender's loaders to the cases in `tests/engine/ui/resolve_cases.json`, which the app's
`AppUi.ResolveMatchesTheSharedCases` checks too: a change to how entries resolve goes into all three
loaders and the cases.

## Properties (the Attribute Editor)

`properties.json` is the groom's Attribute Editor for every host. Maya keeps its MEL template
(its look does not change); `ui.definitions` checks `AEgroomLabShapeTemplate.mel` against the file:
the same sections, controls, custom blocks and dividers in the same order, each section opened or
folded as Maya opens it, every MEL annotation as the item's tip, each label the attribute's nice
name (`setNiceNameOverride` in `groom_shape.cpp` included), the Hair Color parts as `ae.py`'s
`COLOR_ROWS` (and the mode each shows in), and each `blender` property one `props.py` defines. A
change to Maya's AE goes into both. The app builds its Properties dock (Groom workspace) from the
file (`hosts/app/src/properties_panel.h`): the item's label and tip for the app, its control by its
`app` key; an item without one is hidden, and `AppProperties.TheDockFollowsTheDefinitionsInMayasOrder`
(`tests/hosts/app/test_properties.cpp`) fails on any app item that is neither bound nor in
`ALLOWED_MISSING_PROPERTIES` (the parity checklist with Maya's AE).