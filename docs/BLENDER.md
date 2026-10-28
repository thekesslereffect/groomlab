# GroomLab for Blender

GroomLab in Blender 5.2 LTS (Python 3.13): groom with GroomLab's own tools as in Maya (the Groom
Tools palette, the brushes, the pie menu standing in for the marking menu, Maya's viewport
display), and let GroomLab's graph grow, shape and color the hair, the same groom Maya evaluates.
B1 is the helper process (sections 2-5), B2 the add-on (section 6), B3a GroomLab's own tools in
the helper (section 3, "Tools": the brushes, the guide hierarchy, selection, maps, undo, Maya's
display buffers, the groom's state), B3c the Graph Editor (section 7), B3b the Groom Tools in the
add-on (section 8); the user's guide is web/content/docs/blender.mdx.

## 1. Why two halves

Blender's add-ons are GPL: an add-on is a derivative of Blender's Python API, so whatever is
loaded into Blender's process as part of it (Python, or a native module it imports with ctypes or a
.pyd) must be GPL-compatible. GroomLab's core is Apache-2.0, which is GPL-3.0-compatible, but the
bridge stays two programs (the helper keeps the core's threads, GPU and crashes out of Blender's
process, and the core needs no Python build per Blender version):

| | What | License | Where it runs |
|---|---|---|---|
| **The add-on** (B2, `hosts/blender/addon`) | UI, Blender data in and out (hair curves, the scalp mesh, the strands object), the Graph Editor and its panels | GPL-3.0-or-later (its own LICENSE) | inside Blender |
| **groomlab-helper** (B1, `hosts/blender/helper`) | the core: GroomSession, the graph, presets, `.groom` files, exports | Apache-2.0 | its own process |

The add-on starts the helper as a separate executable and talks to it over pipes and shared
memory, as one program talks to another, at arm's length. Nothing of the core is ever
loaded into Blender: no ctypes, no Python module of ours in Blender's interpreter. The add-on is
open source and anyone may edit it.

## 2. The helper process

- `groomlab-helper[.exe] [--parent-pid <pid>] [--version]`. A console program; the add-on spawns it
  with `CREATE_NO_WINDOW` on Windows (no console window), stdin and stdout as pipes, stderr to a
  log (or DEVNULL).
- **One helper per Blender session**, holding any number of sessions (one per groom object).
- **Ends** on `shutdown`, at the end of stdin (Blender closed the pipe or died), or when the
  `--parent-pid` process ends (Blender crashed while another child of it holds the pipe). No
  dialog ever opens: Windows error boxes and abort dialogs are off; a crash ends the process with a
  code, and the add-on starts another (re-sending its sessions: the add-on keeps what it needs to,
  `save_groom` to a temp file being the simplest).
- **stdout is the protocol's alone**: at startup the helper moves fd 1 (printf, std::cout) to
  stderr, so a stray print of a library cannot break a line.

## 3. Protocol

Line-delimited JSON, UTF-8: one request object per line on stdin, one response per line on stdout,
in order (the helper answers each request before reading the next).

```json
{"id": 7, "cmd": "evaluate", "session": 1, "density": 0.25}
{"id": 7, "ok": true, "result": {...}}
{"id": 8, "ok": false, "error": {"code": "no_session", "message": "no session 9"}}
```

`id` is the client's (echoed; `null` for a line that is not JSON). Error codes: `bad_request`,
`unknown_command`, `protocol`, `no_session`, `not_found`, `type_mismatch`, `graph`, `parse`, `io`,
`unsupported`, `state`, `shared_memory`, `evaluation`, `out_of_memory`, `internal`. A
bad request never ends the helper.

**Handshake.** `hello {"protocol": 1}` answers `{"protocol": 1, "version": "0.1.0", "versionNumbers",
"platform", "sharedMemory": "windows"|"posix", "alembic", "groomSpace": {"upAxis": "Y", "units":
"cm"}, "features": {"tools": 1, "state": 1, "cardMaps": 1, "authoring": 1, "pipeline": 1, "photos": 1}}`; another protocol number is a
`protocol` error (the add-on and helper ship together; the add-on refuses a helper of another
protocol). `kProtocolVersion` (helper.h) rises only for a change an older add-on would misread; new
commands and fields keep it. `features` versions what came after protocol 1's first commands
(`kToolsVersion`: GroomLab's own tools, section "Tools"; `state`: get_state/set_state; `authoring`: section "Authoring"; `pipeline`: section "Pipeline"; `photos`: section "Photos"): an add-on
checks the one it draws.

### Arrays

Any array argument is either a JSON list (flat or nested; fine for a few numbers) or a reference
to shared memory:

```json
{"shm": "wnsm_3fa2c1d0", "offset": 0, "dtype": "<f4", "shape": [1225, 3]}
```

dtype is numpy's: `<f4 <f8 <i4 <u4 <i8 <u8` (converted to what the command takes). Responses
describe their arrays the same way, so the add-on reads one with
`np.ndarray(shape, dtype, buffer=shm.buf, offset=offset)` and hands it to Blender with
`foreach_set` (positions, radii, attributes): no per-point Python loop anywhere.

### Shared memory

- **Windows**: a named file mapping in the session's namespace, backed by the paging file
  (`CreateFileMappingW` / `OpenFileMappingW`). **Elsewhere**: POSIX `shm_open` + `mmap`.
  `shared_memory.h` is the abstraction. Python opens either with
  `multiprocessing.shared_memory.SharedMemory(name)` (on POSIX with `track=False`, Python 3.13, so
  its resource tracker does not unlink the helper's blocks).
- **Answers**: the helper writes into blocks of its own, one per session and purpose (`strands`,
  `guides`, `scalp`), named `groomlab_<pid>_s<session><purpose>_<n>`. Every array of an answer is
  64-byte aligned in the block. The arrays stay valid **until the next answer of the same
  purpose** in that session; copy what you keep. When an answer does not fit, the block is replaced
  by a bigger one (1.5x, whole MiB) with a new name: the add-on opens the new name and closes the
  old (on Windows the old mapping lives until the add-on closes it; on POSIX the name is unlinked
  and the add-on's mapping stays valid).
- **Requests**: the add-on creates a block of its own (`SharedMemory(create=True)`), writes the
  arrays, sends their references, and closes and unlinks it after the response. The helper opens it
  read-only for the request and lets go before answering.

### Spaces and units

The helper keeps each groom in **groom space: Y up, centimeters**, the space a `.groom` file is in
and Maya opens as it is. The host speaks its own space, set per session (`new_session {"space":
{"upAxis": "Z", "metersPerUnit": 1.0}}`, the default: Blender's Z up meters; `metersPerUnit` is the
scene's unit scale; `set_space` changes it). Every point, width and length crossing the pipe is
converted at the boundary (host_space.h), never inside the groom:

| Direction | Points | Widths |
|---|---|---|
| host to groom | Z-up `(x, y, z)` to Y-up `(x, z, -y)` (convertUpAxis' quarter turn, exact), times `100 * metersPerUnit`, in double, rounded once to float | |
| groom to host | the inverse | divided by `100 * metersPerUnit` |

What is converted where:

- **Converted** (host space in requests and answers): scalp points and rest points, guide points,
  strand positions and widths, evaluate's bounds.
- **Not converted**: UVs, colors and every other attribute (rootUV, color, melanin...), the graph
  and its params (their lengths are **centimeters**, as in Maya: a ParamSpec's `unit`, length,
  width, density or perLength, tells the add-on how to show them in scene units), presets (fitted
  to the scalp's size in centimeters, so a preset looks the same on a Blender head as on a Maya
  one), exports (written in groom space: Alembic Y up in centimeters, USD with `upAxis = "Y"`,
  `metersPerUnit = 0.01`; Blender's importers convert both; Unreal's groom in Unreal's own space,
  Z up, left-handed, centimeters: groom space's (x, y, z) as (x, z, y), which is Blender's
  (x, -y, z) times 100 times the unit scale, as Unreal's FBX importer brings in a mesh from
  Blender's FBX, so the groom imports with Unreal's default conversion lined up with the head).
- **`.groom` files**: `save_groom` writes groom space (`"upAxis": "Y"`, `"units": "cm"`), so the
  file opens in Maya identically and groomlab-cli evaluates it to the same bits as the helper did
  (tested). `save_groom {"upAxis": "Z"}` writes a Z-up file instead (convertUpAxis, exact); Maya and
  the app turn it back on import. `load_groom` reads either axis (convertUpAxis to Y); a file in
  other units than centimeters is refused, as Maya's import refuses it.
- `evaluate`, `get_scalp` and `get_guides` take `"space": "groom"` to answer groom-space bits as
  they are (tests, debugging).

The host's round trip is float rounding: a guide sent back unchanged comes within `1e-4` cm of
where it was, which the helper treats as unchanged (it keeps the stored points and root bit for
bit, and the cache).

### Commands

All but the process ones take `"session": <id>`.

| Command | Arguments | Result |
|---|---|---|
| `hello` | `protocol` | versions, platform, features (above) |
| `ping` | | `{"pong": true}` |
| `shutdown` | | answers, then the helper ends |
| `new_session` | `space`, `fps` | `{"session", "space"}` |
| `close_session` / `set_space` | `upAxis`, `metersPerUnit` | |
| `set_options` | `followScalp`, `mapsDir` | `{"followScalp", "mapsDir"}` (followScalp: the groom's option, saved in its file; mapsDir: where new texture maps' images go, default a folder of the session's in the temp folder) |
| `load_groom` | `path` | groom info: `fileUpAxis`, `fps`, `scalp` counts and topology, `flatGuides`, `hierarchyGuides`, `nodes`, `output`, `followScalp`, `scalpCache`/`guideCache` refs, `notes`, `inputErrors` |
| `save_groom` | `path`, `upAxis` ("Y") | `{"path", "upAxis", "units"}` |
| `groom_info` | | as `load_groom` |
| `set_scalp` | `points` (n,3), `faceCounts`, `faceIndices`, `uvs` (per face corner, 2), `restPoints`, `colorSets` `{name: (corners,4)}` | `{"points", "triangles", "topology", "matchesGroom", "guidesRerooted"}` |
| `update_scalp` | `points`, `restPoints` | the scalp's points at this frame on its topology (a deforming scalp) |
| `get_scalp` | `space` | `points`, `faceCounts`, `faceIndices`, `uvs`, `restPoints` arrays; `facesExact` (every polygon came back as one face) |
| `get_guides` | `source` ("auto", "flat", "hierarchy"), `space`, `pose` ("now", "rest") | `points` (n,3), `counts`, `ids` (<u8); `pose` answered |
| `set_guides` | `points`, `counts`, `ids` (0: new), `target` ("auto", "flat", "hierarchy"), `keepMissing`, `pose` ("now", "rest"), `group` (hierarchy: the group new guides join, "Pipeline") | `{"pose", "target", "guides", "changed", "added": [{"index", "id"}], "removed", "updated"}` |
| `set_time` | `frame` or `seconds`, `fps` | `{"seconds", "frame", "fps"}` (frame_time.h's rule) |
| `evaluate` | `mode` ("preview": the viewport's, renderOnly nodes off; "render": viewportOnly nodes off), `density`, `pointBudget`, `viewNode`, `sculptDensity` (1; while a stroke is live the strands step back to this fraction, the app's 0.35), `attributes` (names or "all"), `space`, `bounds` | strand arrays (below), `errors`, `messages`, `notes` {node: note}, incremental hints, `milliseconds`, `following`, `look` (below) |
| `stats` / `node_status` | | GroomStats JSON / notes, times, errors, inputErrors, graphError |
| `operator_specs` / `default_graph` | | every operator's spec (params with groups, advanced, units, showWhen...) / the default graph |
| `get_graph` / `set_graph` | `graph` (object or text) | the graph JSON |
| `graph_channels` | | `channels`: the attributes per node (graphChannelsJson, attributes.h): `incoming` (reaching it, each with its writer), `writes`, `overwrites`, `reads` (`reaches`, `optional`) |
| `list_nodes` | | `[{name, type, version, output}]` |
| `node_spec` | `node` | GraphEdit::nodeSpecJson: ports, params with their ParamSpec metadata (group, advanced, unit, showWhen, min/max...), `outdated` |
| `get_param` / `set_param` | `node`, `param`, `value` | the value (`set`: false for a spec default); set checks the kind as the editors do, and a map value's source, keys and expression ("Authoring" below) |
| `edit_graph` | `op`: add_node (`type`, `name`, `params`, `x`, `y`: defaults fitted to the scalp), remove_node, rename_node, connect (`from`, `to`, `input`, `output`: the user's link, `GraphEdit::connectByHand`: a Clump picking its centers fed centers takes them, `centersFrom` `inputPoints`), disconnect, set_output, upgrade_node, set_positions (`positions` {node: [x, y]}: each node's `"ui"`; `backdrops`; `pins`), set_bypass (`name`, `bypass`), set_evaluation (`name`, `mode`: always, viewportOnly or renderOnly); the undoable authoring ops (group, ungroup, expose_param, unexpose_param, compound_expose, compound_unexpose, insert_on_link, add_control, link_control, unlink_control: "Authoring" below) | `graphError` ("" when the graph loads); a graph that does not load yet takes every op (set_graph refuses it) |
| `list_presets` | | presets with title, description, category; the folders searched |
| `apply_preset` | `name` or `path` | the graph replaced, fitted to the scalp (`fitFactor`), controls applied; `missingMaps` the preset reads that the groom lacks (`name`, `kind`, `value`, `color`), which the add-on then makes with `map_create` (graph.make_required_maps, as Maya and the app make them); `groups` [{`name`, `draw`}] the guide groups it reads (presetRequiredGroups), which the add-on makes empty when missing (graph.make_required_groups) and, for `draw`, picks the Draw tool drawing into (graph.draw_into) |
| `insert_preset` | `name` or `path`, `after`, `nodeName` | the preset's look as a Compound after `after`; `node` |
| `export` | `format` ("alembic", "usd", "scalesUsd", "unrealGroom", "unityStrands"; the game formats in "Pipeline"), `path`, `frames` {first, last, step}, `density`, `name`, `scales`, the engine options ("Pipeline") | the current inputs at each frame: `{"path", "frames", "strands"}`, and `objects` for an engine's groom |
| `export_open` / `export_frame` / `export_close` | `format`, `path`, `startFrame`, `step`, its options / `export`, `frame` | the same frame by frame, the add-on feeding a deforming scalp between frames |
| `card_map_presets` | | `presets` (cardMapPresetsJson: built in, then the user's Documents/GroomLab/card_presets), `sources` (the maps, channel names, formats), `userDir` |
| `bake_card_maps` | `preset` (a name or a preset object), `directory`, `name`, `resolution`, `format`, `bitDepth`, `padding`, `supersample`, `nameTemplate`, `occlusion` | the Cards nodes' cards baked at the session's time and written as the preset packs them (card_maps.h): `paths`, `cards`, `size` |
| `save_card_map_preset` / `delete_card_map_preset` | `preset` / `name` | `path` / `deleted`: the user's presets, shared with Maya and the app |

**evaluate's arrays** (`result.arrays`): `positions` <f4 (P,3), `pointCounts` <i4 (S) (Blender's
curve sizes), `offsets` <i4 (S+1) (<i8 past 2^31 points), `ids` <u8 (S) (stable per strand across
frames and densities), `widths` <f4 (P) (diameters, host units: Blender's radius is half),
`attributes` {name: ref + `"scope": "strand"|"point"`} for the ones asked for. Every evaluation
lists `attributesAvailable` (name, scope, components): the graph's strand and point channels plus
the derived `rootUV` (2 per strand), `color` (linear RGB per strand), `pointColor` (per point, when
the color varies along strands) and, with FeatherColor's iridescence, `rootNormal` (3 per strand: the
feather's vane normal, else the scalp's under the root, else the first segment's direction; a
direction in host axes, not scaled: Arnold's `root_normal`). The color nodes' and RenderData's
channels come as they are: `melanin`, `melaninRedness` (per strand), `iridescence` (per point), `opacity` (per point), `specular`, `roughness`, `tilt` (per
strand, HairColor's highlight where set off its defaults).

**evaluate's `look`**: what Maya's `create_hair_shader` builds the groom's Arnold shader from
(GroomSession::hairLook): `node` (the HairColor making the output), `specular`, `roughness`, `tilt`
(its highlight; 0.3, 0.5 and 3 degrees without one), `dye` (the HairColor's base color over the pigment),
`colorsByRgb` and `colorBlends` (the RGB, not the melanin, is the color: any color node, HairColor
in either mode, or a Blend of two looks), `iridescent`,
`iridescenceColor`, `iridescenceShift` (the first iridescent FeatherColor's). The add-on's material is built from it (section 6,
"Rendering").

**Incremental hints.** `serial` names the strands answered (the same serial: the same strands);
`changesBase` and `changedStrands` (a count; null: unknown or all) say how they differ from the
serial before; `sameTopology` is true when the strand IDs and point counts are those of the last
answer, so the add-on keeps its curves object and only `foreach_set`s positions (and radii);
`nodesEvaluated` / `nodesIncremental` count the graph nodes the evaluation ran and those that
re-made only what changed (a guide edit: Interpolate and what follows, incrementally).

### Guides from Blender

Blender's hair curves are the groom's guides. Each curve carries a stable GroomLab ID (B2 stores
it in an int attribute on the curves; 0 for a curve the user just drew).

- **Flat guides** (`target: "flat"`, the default for a groom without a guide hierarchy): what Maya's
  guide curves are. `set_guides` rebuilds the set by ID: an ID it has keeps its painted attributes;
  a curve within 1e-4 cm of its stored shape keeps the stored points and root (no re-rooting, no
  re-evaluation); a changed or new curve is rooted on the scalp as Maya roots its guide curves
  (`orientAndRoot`: tip-first curves turned, moved rigidly onto the closest point); a new curve
  gets the next free ID (answered in `added`, which B2 writes back); an ID not sent is removed. On
  a scalp of other triangles (`set_scalp`), the guides are rooted on it again.
- **A guide hierarchy** (`target: "hierarchy"`, the default when a loaded groom has one and no
  flat guides: grooms made in Maya or the app): `get_guides` gives its editable guides, the
  primaries and locked children (following children are made from them); `set_guides` sets each
  one's world shape by ID (GuideHierarchy::setPoints: its children follow), adds new curves to the
  first root group, and removes editable guides not sent (unless `keepMissing`).

- **Guides kept at rest** (`pose: "rest"`, flat guides; what the add-on sends): Blender's guide
  curves do not deform with the scalp (an armature or shape keys deform the mesh, not the curves),
  so they are the guides on the scalp's **rest pose**. The helper keeps them as given (rooted on the
  rest pose: the scalp's `restPoints`) and hands the session the same guides carried to the scalp
  as it is now by their roots' rigid motion (follow_scalp.h), again on every `update_scalp`; at
  rest they are the same guides bit for bit. With `followScalp` on (`set_options`) the session takes
  them back to rest and carries the strands grown there; off, it grows on the deformed scalp from
  the carried guides. `get_guides {"pose": "rest"}` gives them back as given; `set_guides` without
  `pose` (Maya's guide curves, world space at the evaluated time) drops the rest copy.

Either way the groom saved is the one evaluated: Maya opens a Blender-saved groom with these
guides (flat guides as its guide curves).

### Tools (B3a: GroomLab's own brushes in the host)

So Blender grooms the way Maya and the app do, the helper runs the core's own tools: stroke.h's
`BrushTool` over a `SessionStrokeHost` of the session's groom (what the app's `BrushSession`
drives; `hosts/blender/helper/src/tools.cpp`), guide selection (guide_selection.h), the guide
hierarchy's edits, painted maps, one undo step per stroke or edit, and the viewport geometry
`DisplayBuilder` builds for Maya's Viewport 2.0 (display.h). The add-on sends mouse events and its
view, and draws what comes back with Blender's `gpu` module.

**Guides live in the hierarchy.** `new_groom {"session", "preset"?, "followScalp"?}` starts the
groom Maya's `create_groom(fit=True)` makes: the default graph with its lengths, widths and
densities fitted to the scalp's size (`defaultGraphJsonFor`; they are authored on a 20 cm head), or
the named preset's, fitted, as `apply_preset`; no guides, on the session's scalp; the session's fps and space stay. Answers
`{"groom": groom info, "guides": "hierarchy", "preset"?: apply_preset's answer}`. Place and Draw
create primaries in the hierarchy (group "primary" at level 0, children groups below it), with
the freeze mask, painted layers and tubes as in Maya. The flat path (`set_guides`) stays for the
B2 add-on; the tools edit the hierarchy only (a groom with flat guides grows from both). Loading a
groom (`load_groom`, `set_state`, `new_groom`) starts the tools over: no stroke, no selection, an
empty undo stack; the tool and its settings stay.

**Views.** Every command that reads pixels takes `"view"` (kept: later commands may leave it out):

```json
{"viewMatrix": [[...4], [...4], [...4], [...4]], "projectionMatrix": [[...]...],
 "objectMatrix": [[...]...]?, "perspectiveMatrix": [[...]...]?, "width": 800, "height": 600,
 "target": [x, y, z]?}
```

The matrices are Blender's as they print (`region_3d.view_matrix`, `region_3d.window_matrix`, or
`perspective_matrix` for both; rows first, column vectors, OpenGL clip space), `objectMatrix` the
scalp object's `matrix_world` (points cross the pipe in its local space; identity when left out),
`width`/`height` the region's size. Pixels are region pixels, origin bottom left (Blender's
`event.mouse_region_x/y`). The helper composes `projection * view * object * (groom to host)` and
hands its transpose to `ToolView::fromMatrix` (the core's row-vector convention), so the camera
(eye, orthographic) comes from the matrix. `target` (the view's pivot, `region_3d.view_location`
in the scalp's space) is where the ring sits when the cursor is off the scalp (else the scalp's
middle). Answers that read a view echo `"view": {"eye", "viewDirection", "orthographic",
"groomMatrix" (the core's 16 numbers), "width", "height"}`.

**Tool settings** are what Maya's palette sends `groomLabBrushCtx`, by its flags' long names, with
the catalog's setting ids as aliases. Lengths are **host units** (scene units, like the points),
the symmetry in the **host's axes**; strengths, values and pixels are as in Maya:

| Setting | Meaning |
|---|---|
| `radius` | pixels (the screen circle) |
| `strength`, `falloff` (0 hard .. 1 soft) or `hardness` (1 - falloff), `pressure` | as the palette |
| `level`, `group`, `propagate` | which guides the brush edits (Page Up/Down and Shift+click set them) |
| `selectedOnly`, `softSelectRadius` (host units) | brush the selection only, soft around it |
| `symmetry` ("x", "y", "z", "none"), `symmetryCenter` (host units) | Blender's y is the groom's -z, its z the groom's y |
| `collide`, `frontFacingOnly` (alias `maskByScalp`), `stretch` (Comb) | |
| `length`, `segments`, `tubeRadius`, `spacing` (host units), `matchNeighbors` | Place and Draw's new guides |
| `tube` {`shape` (round, ribbon, teardrop, blade or a library shape), `width` (host units, a number or a list root to tip), `aspect`, `twist` (radians)} | Place and Draw's new guides' tube (Maya's `placeTube`; `tool_get` answers it whole) |
| `tubeChannel` (width, flatness, twist) | what the Shape brush paints on the tubes |
| `targets` ("guides,parts,flow" or a list), `eraseGuides`, `eraseParts`, `eraseFlow` | what shaping brushes and Erase act on |
| `partName`, `flowSink` ("", whorl, whorlcw, converge, part) | Part and Flow |
| `attribute`, `value`, `pointwise`, `paintKind` (guides, vertices, texture), `paintTexture`, `paintResolution`, `paintColor` [r,g,b(,a)], `paintChannels` ("rgba"...), `smoothStrength` | Paint (and Smooth on a map) |
| `alpha` (an image's path, "" none), `alphaRotation` (fixed, stroke, random), `alphaAngle` (degrees), `alphaSpacing` (of the diameter), `alphaInvert` | Paint on a vertex or texture map: the brush alpha (`brush_alphas` lists the library, `add_brush_alpha` {file} copies one into the user's folder) |

| Command | Arguments | Result |
|---|---|---|
| `tool_catalog` | (no session) | `{"tools": [{id, label, category, order, icon, defaultStrength, signed, hint, settings}], "categories", "settings": [{id, label, hint}], "paintable": [{id, label, type, param, hint}]}` (brush_catalog.h: every host's palette; paintable: what New Map > For offers first, stroke.h paintableSettings) |
| `tool_set` | `tool` (a catalog id: place, draw, part, flow, comb, slide, smooth, straighten, lift, twist, curl, attract, shape, cut, length, erase, paint, select, freeze; or clump), `settings` {...} | `tool_get`'s answer |
| `tool_get` | | `{"tool", "mode", "settings" (all, host units), "shows" (the catalog's settings for the card), "ringColor", "pathColor", "hoverColor"}` |
| `stroke_begin` | `view`, `x`, `y`, `pressure` (1), `ctrl`, `shift`, `overlay` (true) | `{"stroking", "mode", "ring", "overlay", "view", "warnings"}` (BrushTool::press: Ctrl the opposite action, Shift a click picks the level or a drag smooths) |
| `stroke_drag` | `points` [[x, y] or [x, y, pressure], ...] (or `x`, `y`), `pressure`, `view`? | `{"stroking", "changed", "ring", "overlay", "warnings"}` |
| `stroke_end` | | `{"released": "stroke", "shift_click" or "none", "committed", "level"?, "group"?, "settings"? (a Shift+click soloed the guide and picked its level), "undo", "warnings"}` |
| `stroke_cancel` | | `{"cancelled"}` (the stroke dropped, nothing committed) |
| `hover` | `view`, `x`, `y`, `lines` (true), `overlay` (true) | `{"ring": {"valid", "center", "radius" (host units), "normal" (facing the camera), "onScalp", "screenRadius", "color"}, "overlay", "view"}` |
| `hover_clear` | | the overlay emptied (the cursor left the viewport) |
| `select` | `view`, `lasso` (1 point: a click within `clickPixels` (12); 2: a box's corners; more: a lasso), `mode` (replace, add, toggle, deselect), `rootsOnly` (false) | `{"selection", "count", "picked", "softSelectRadius", "version"}`: the guides of the brush's level or group, hidden groups left out |
| `selection_get` / `selection_set` | `ids`, or `group`, or `level`, or `all`; `mode`; `softSelectRadius` | the selection (not in the undo stack, as in Maya) |
| `pick_level` | `view`, `x`, `y` | `{"level" (-2: none), "group", "settings"}` |
| `set_hidden_groups` | `groups` [names] | hidden groups are not drawn, picked or brushed |
| `guides_info` | `guideColorSeed` (the groom's name) | `{"groups": [{id, name, parent, level, guides, hidden, color ("#rrggbb" or null: its own), autoColor ("#rrggbb": the color it draws in without one, from `guideColorSeed`, given here or by the last display)}], "levels", "guides", "primaries", "frozen", "layers", "hiddenGroups", "guideGroupColors", "selection", "hierarchyError"}` |
| `guides_edit` | `op`: add_group (`name`?, `parent`?), remove_group / promote_group (`group`), rename_group (`group`, `name`; its color follows), demote_group (`group`, `parent`), generate_children (`group` (the last), `count` (5), `selected`, `childGroup`, `rootSpread`, `tipSpread`, `twist`, `jitter`, `childTubeScale`, `seed`), delete_guides / lock / unlock / freeze / unfreeze (`ids` or the selection), set_attribute (`name`, `value`), unfreeze_all, remove_layer (`name`), mirror_all / mirror_group (`axis` host, `center` host units, `fromPositive`, `group`), reparent (`ids`, `parent`), resample (`segments` 1-256; `ids`, `group` or `level`, else the selection, else every guide: Maya's `-resample`, along the smooth curve through the points, frozen guides kept; picked guides (`ids`, the selection) get segments of their own, which a group, a level or every guide resampled later leaves alone; those also set their groups' segments for new guides), set_group_segments (`group`, `segments` 0-256: what its new guides get, 0 the tool's own), reset_group_segments (`group`: every guide of it back to its segments, those with their own too, frozen kept; `{"segments"}`), mirror_selection (`axis`, `center`, `add`; `ids` or the selection: Maya's `mirror_guide_selection`, a selection, no undo step), set_group_color (`group`, `color` "#rrggbb", [r,g,b] 0-1, or null for its automatic color: Maya's `guideGroupColors`) | `{"group"?, "added"?, "removed"?, "resampled"?, "picked"?, "selection"?, "color"?, "undo", "guides": guides_info}`; one undo step each (the app's editGuides) |
| `guide_segments` | `ids`, `group` or `level` (else the selection, else every guide) | `{"ids", "segments"}`: each guide's points - 1 (the Segments slider) |
| `guides_preview` | `segments`, targets as resample | `{"previewed"}`: the guides shown resampled (`setPreviewHierarchy`, from the stored guides each time), no change, no undo step; `guides_edit resample` commits |
| `guides_preview_end` | | `{"ended"}`: the stored guides shown again (after `guides_preview` or `tube_shape_preview`) |
| `tube_targets` | | `{"source" (selection, solo, none), "guides", "shapes", "tube" (the first's, or null)}`: what the Tubes section edits (Maya's `-tubeTargets`: the selection, else the soloed guides) |
| `tube_get` | `guide` (an ID) | `{"tube": {"shape", "width" (host units), "aspect", "twist" (radians)}}`, lists evenly spaced root to tip |
| `tube_set` | `shape`, `width` (host units), `aspect`, `twist` (each a number or a list root to tip), `samples`; targets `ids`, `group` or `level`, else the selection | `{"changed", "undo"}` (Maya's `-setTube`, `editedTube`) |
| `tube_scale` | `factor` (0 or more), targets | `{"changed", "undo"}`: every width times it, the taper kept |
| `tube_smooth` | `channel` (width, flatness, twist, all), `passes` (3), targets | `{"changed", "undo"}` (`smoothedTube`, the root's and tip's kept) |
| `tube_shapes` | | `{"shapes": [{name, builtin, points [[u, v]...], smooth, users (null for a built-in), outline?}]}`: the built-ins, then the groom's library |
| `tube_shape_add` / `tube_shape_set` | `name`, `points` [[u, v], ...] (3 or more; set: optional), `smooth` (true) | `{"users", "undo", "shapes"}`: the library edited (Maya's `-editTubeShape`), every guide taking the shape following |
| `tube_shape_rename` | `name`, `to` | the same |
| `tube_shape_remove` | `name` | the same; `users`: the guides that became round |
| `tube_shape_preview` | `name`, `points`, `smooth` | `{"previewing"}`: the shape's new points shown live (the editor's drag), no undo step; `tube_shape_set` commits, `guides_preview_end` drops it |
| `parts_list` | | `{"parts": [{name, points}], "flow": {"arrows", "sinks"}}` (the Flow card's count) |
| `part_add` | `points` (host space, a list or a shared memory array of (n, 3)), `name` ("part") | `{"name", "undo"}`: a part through them bound to the cage, resampled to 64 points, named name, name1... (Maya's `groomLabParts -fromCurves`) |
| `part_remove` | `name` | `{"removed", "undo"}` |
| `parts_clear` / `flow_clear` | | `{"cleared", "undo"}` (nothing to clear: no undo step) |
| `maps_list` | | `{"maps": [{name, kind (vertices, texture, guides), channels, texture?, file?, missing? (its file is not there), freezeMask?}], "folder"}` |
| `map_create` | `name`, `kind` (vertices, texture, guides), `color`, `value`, `fill` [r,g,b(,a)], `resolution` (1024) | `{"name", "kind", "file"?, "undo"}` (a texture map's PNG written in the maps folder) |
| `map_remove` | `name`, `deleteFile` (false) | `{"removed", "unlinked" [[node, param]...], "graphChanged", "fileDeleted", "undo"}`: Maya's `remove_map`. What reads the map lets go of it (its Map nodes removed, the params they fed read their own values again, map params naming it back to their defaults); a texture's file stays unless `deleteFile`, it is in the maps folder (GroomLab made it) and no other map reads it (undo writes it back) |
| `map_rename` | `name`, `to` | `{"name", "renamed", "graphChanged", "file"?, "undo"}`: Maya's `rename_map`; the readers follow (`retargetMapReaders`), a texture's file too when it is named after the map and in the maps folder (`renamedMapTexture`, a free name) |
| `map_convert` | `name`, `kind`, `resolution` (1024), `texture`? (a .png) | `{"name", "kind", "file"?, "warnings", "undo"}`: Maya's `convert_map` (guides, vertices, texture; the name kept; a new file never replaces one) |
| `map_set_channels` | `name`, `channels` (1 gray, 4 color) | `{"channels", "undo"}` (a guide layer stays gray) |
| `map_fill` | `name`, `value` or `color` [r,g,b(,a)], `channels` ("rgba"), `selected` (a guide layer on `ids` or the selection) | `{"name", "undo"}`: Maya's `fill_map`, any kind; a texture's file rewritten (its bytes are the undo step) |
| `map_import` | `path`, `name`? (else the file's), `color`? (else whether the image has color), `copy`? | `{"name", "kind", "channels", "file", "undo"}`: Maya's `import_map`. A PNG is read in place; another format (or a PNG with `copy`) is copied into the maps folder as `<name>.png` (the copy removed on undo) |
| `map_relink` | `name`, `path`, `copy`? | `{"name", "file", "undo"}`: Maya's `set_map_texture`, the map (any kind) reading the image as a texture map, its channels kept |
| `map_reload` | `name`? (else every texture map) | `{"files"}`: read again at the next evaluation after an edit outside (not an undo step) |
| `map_users` | `name` | `{"name", "nodes" (its Map nodes), "users" [[node, param]...]}` (Maya's `map_users`) |
| `map_for_param` | `node`, `param`, `kind` (vertices, texture, guides), `resolution` (1024) | `{"name", "kind", "made", "graphChanged", "tool", "undo"}`: New Map > For a setting, and the Graph Editor's Paint (On Guides, On Scalp Vertices, As Texture). The map node.param reads, else a white map `<node>_<param>` made and linked to it (the graph's map param; `guides`: a guide layer at 1 on every primary guide, children inheriting it) as one undo step; the Paint tool on it toward 1 (Ctrl: toward 0; on Scatter's density, no hair). The Width and Density brushes it replaced answer `tool_set` with an error naming it |
| `undo` / `redo` | | `undo_state` + `"done"` |
| `undo_state` | | `{"canUndo", "canRedo", "undoLabel" ("comb", "place", "generate children", "create map mask"...), "redoLabel", "undoDepth", "redoDepth"}` |
| `display` | `settings`, `have` {item: version}, `generation`, `map` (name or {name, channel}), `strandColors` {mode, source, min, max, perPoint} | the display buffers (below) |
| `get_state` | `scalp` (true; false; "maps": only while maps painted on the scalp's vertices live in its color sets, what the add-on asks for: the scalp as JSON is most of the state on a dense mesh), `textures` (true), `texts` (false) | `{"format": "groomlab.groom", "groom": the .groom text (groom space), "textures": [{name, stored, png (base64)}], "tools": {selection, softSelectRadius, hiddenGroups, groupColors}, "texts"?: {guideData, partData, flowData, correctiveData, scalpMaps}}` |
| `set_state` | `groom` (text or object), `textures`, `tools`, `keepScalp` (true) | groom info: the state adopted as `load_groom` adopts a file, its textures written into the maps folder (the maps repointed), the session's scalp kept |

**Strokes.** `stroke_begin`, any number of `stroke_drag`s, `stroke_end`: the app's press, moves
and release. Each `stroke_drag` previews through the session (`setPreviewHierarchy`, parts, flow,
maps): `display` shows the live guides without evaluating; `evaluate {"sculptDensity": 0.35}`
grows the strands from them at a fraction of the density, and the evaluation after `stroke_end`
has every strand again. `stroke_end` commits one `StrokeEdit` (the stored texts after the stroke:
guideData, partData, flowData, scalpMaps; a texture map's tiles), one undo step, the texts
applied to the session. A refused stroke (no scalp, no map) answers its `warnings`; a core error
drops the stroke and answers `stroke`. The Freeze tool is Paint on the guides' `freeze` layer
toward 1 (Ctrl: 0), every other brush leaving frozen guides alone (BrushSettings::freeze); leaving
it puts back what Paint painted (the app's rule).

**Undo.** One stack per session, beside Blender's: strokes, `guides_edit`, the `tube_*` edits,
the parts and flow edits and every map edit (`map_create`, `map_remove`, `map_for_param`,
`map_rename`, `map_convert`, `map_set_channels`, `map_fill`, `map_import`, `map_relink`) and the
authoring edits below (the graph's undoable `edit_graph` ops, `save_node`, the script node edits,
every corrective edit, `sculpt_clear`). `undo`/`redo` put back the stored texts bit for bit (texture maps: their tiles, the file written
again), and what an edit did beside them: the files it wrote, moved, copied or deleted (their bytes
before and after) and the guide groups' colors. Previews, selections (`mirror_selection`) and
`map_reload` are not steps. The group colors are kept with the tools' state (`get_state`'s
`tools.groupColors`), as Maya keeps `guideGroupColors` on its node; `display` draws them unless its
settings give their own `guideGroupColors`. The add-on maps Ctrl+Z / Ctrl+Shift+Z to them while a GroomLab tool is
active.

**Painted maps** live in the session (`ScalpMaps`): vertex maps per vertex of the cage, texture
maps as PNG files (made in the maps folder, painted in place, kept open between strokes while the
file is what they wrote), guide layers on the guides. A groom file's maps are adopted when it is
read (so they outlive the host's scalp, which has no color sets); `save_groom` and `get_state`
write them back as Maya reads them (the catalog, vertex maps as the scalp's color sets).
The maps folder (`set_options mapsDir`, else a folder of the session's in the temp folder) is
Blender's stand-in for Maya's masks folder: what `map_remove deleteFile` may delete and
`map_rename` may move. The images themselves travel in the .blend (`get_state`'s `textures`).

**Display buffers.** `display` runs a `DisplayBuilder` (one per session) on the guides as they
are now (`GroomSession::hierarchy()`: a live stroke's preview included), the last evaluation's
parts and flow, the session's scalp, the selection and the map shown, with Maya's display settings
(`settings`: `guides`, `tubes`, `tubeWires`, `flow`, `guideWidth`,
`guideColorMode` (group, layer), `guideColorLayer`, `guideColorSeed` (the groom's name: the
automatic colors' hue), `isolateGroup`, `guidePoints` (off,
ends, all), `guideSmoothing` (4), `hiddenGuideGroups` (default: the
session's), `guideGroupColors`, `theme` {pointSize, selected, softSelected, frozen, parts,
flow, flowField, tubes}). The answer, every array in the session's `display` block (valid until
the next `display`):

```json
{"generation": "groomlab_<pid>_s1display_1",
 "items": {
   "guides" | "tubes" | "selectedGuides" | "softSelectedGuides" | "parts" | "flowArrows" | "flowField" | "scalpMap"
   | "tubeWires" | "selectedTubeWires": {
     "version", "changed", "shown", "vertexCount", "primitive": "lines"|"triangles",
     "perVertexColors", "surface", "color": [r,g,b,a], "lineWidth", "layer",
     "arrays"?: {"positions" <f4 (n,3) host space, "indices" <u4 (m,2) lines | (m,3) triangles,
                 "colors"? <f4 (n,4), "normals"? <f4 (n,3)}},
   "dots": {"version", "changed", "count", "pointSize", "arrays"?: {"positions" (n,3), "colors" (n,3)}}},
 "overlayPull", "stroking", "selection",
 "brush": {"ring": {...}, "overlay": {"version", "vertexCount", "changed", "arrays"?}},
 "strandColors"?: {"mode", "version", "changed", "perPoint", "strands", "colors"? <f4 (S,3) | (P,3)}}
```

- `guides`: every guide as a smooth curve (`guideSmoothing` pieces per segment), colored per vertex
  as Maya colors them: by group (each group its own color, else its automatic hue) or by layer, other levels
  dimmed when one is isolated, **frozen guides in the theme's frozen blue**. `dots`: the guide
  points drawn as screen-facing dots of `pointSize` pixels (roots and tips, or all).
- `selectedGuides` / `softSelectedGuides`: the selection over the guides in the item's color
  (white, orange), `lineWidth` one wider. `parts` (red), `flowArrows` (orange arrows, sinks' rings
  and streamlines), `flowField` (the field over the scalp), `tubes` (shaded triangles with normals),
  `scalpMap` (the map shown, the scalp lifted a hair off its surface with per-vertex colors; a
  texture map sampled at its vertices; a live stroke's values while painting).
- `tubeWires` / `selectedTubeWires`: with `tubeWires` true (the host sends it while the Shape brush
  is on, as Maya draws them), every guide's tube the brush can shape (shown, not frozen:
  `activeTubeGuides`) as wire lines (`appendTubeWire`), the selected guides' apart (warm, opaque,
  `lineWidth` 2; the rest a light blue at 0.6 alpha), as Maya's draw override builds them. Empty
  (not `shown`) with `tubeWires` off.
- `brush`: the overlay `hover` and the strokes build: the ring (`brushRingColor`: yellow, red for
  Part and Erase, orange for Flow, green for Place and Draw, which show a 6-pixel dot), the guides
  the brush would touch (`brushHoverColor`, while hovering) and a Part or Flow stroke's path,
  per-vertex colored lines in host space.
- **Incremental**: an item's arrays come only when its `version` is not the one the host sends
  back in `have` ({"guides": 12, "dots": 12, "brush": 40, "strandColors": 3, ...}) under the same
  `generation` (another generation, a new helper's, sends everything). `changed` says which to
  upload; an unchanged item keeps its buffers. `layer` is Maya's draw priority (dormantWire,
  dormantFilled, activeWire, selection); `overlayPull` the hair's depth Maya pulls overlays by.
- `strandColors`: Maya's strand color modes (`mode`: solid, colorSet, attribute, random, clump,
  density; `source` the color set or channel, `min`/`max` the range) for the strands of the last
  `evaluate` (its order), per strand or per point; solid sends none (the hair's own color).

**State.** `get_state` is the groom as a `.groom` text in groom space (the scalp with the vertex
maps as color sets, the hierarchy, parts, flow, graph, maps catalog) plus each texture map's PNG
and the tools' selection and hidden groups: what the add-on keeps in the `.blend`. `set_state` gives it back to any session (a new helper's),
keeping the session's scalp; a vertex map travels per face corner, so a point no face uses comes
back 0 (as in every host).

### Authoring (Maya's Graph Editor, node library, correctives and Sculpt nodes)

What Maya's Graph Editor (`groomlab/graph.py`'s GraphModel, `params.py`), node library
(`nodes.py`), Save as Preset (`presets.py`), Correctives tab (`shots.py`) and Sculpt nodes
(`sculpts.py`) do, over the helper (`hosts/blender/helper/src/authoring.cpp`). Every edit of the
groom is one step on the session's undo stack (above): `"undo"` in the answer, `"graphChanged"`
when the graph changed (the add-on's tree follows, as after `map_for_param`), and `undo`/`redo`
answer `graphChanged` too. The tree-sync ops of `edit_graph` (add_node, connect...) stay outside the
stack: Blender's own undo restores their graph (section 7). A graph edit is refused (`graph`, nothing
changed) when it would leave a graph that loads one that does not; while the graph does not load yet
it is kept as `edit_graph` keeps it, without a step (`"undoable": false`).

| Command | Arguments | Result |
|---|---|---|
| `edit_graph` `op=group` | `names`, `name` ("": compound1...) | `{"name", "moved" [[from, to]...]}`: Group (Ctrl+G), `GraphEdit::collapse`; the Sculpt nodes' edits follow their nodes in the same step (`moveSculptNodes`) |
| `edit_graph` `op=ungroup` | `name` (a Compound) | `{"names", "moved"}`: `GraphEdit::expand` |
| `edit_graph` `op=expose_param` / `unexpose_param` | `name`, `param` | the param's input port (`paramInputs`: a Field port for a mappable number or a color field, Curve for a ramp, Value for the rest; refused for a param that sets the node's ports); unexposed, what fed it goes and the param keeps its value |
| `edit_graph` `op=compound_expose` / `compound_unexpose` | `name` (a Compound), `param` (the name it shows), `target` ("node.param" inside) | the compound's panel (`definition.expose`); unexposed, the inner param keeps the value the compound set last |
| `edit_graph` `op=insert_on_link` | `from`, `to`, `input`, `node`, `output` | the one link routed through `node`: its first input that takes from's output, `node` feeding `to.input` (Maya's `insert_on_link`, a node dropped on a wire) |
| `edit_graph` `op=add_control` | `node`, `param`, `name` (the param's), `default` (the param's value), `min` (0), `max` (1) | `{"node", "control"}`: a Control node (the slider) wired to the param, exposed first (params.py's `add_control`) |
| `edit_graph` `op=link_control` / `unlink_control` | `node`, `param`, `control` (`<node>_<param>`) | a keyed param (`paramControls`): the param reads control `<node>_<param>` (three, X Y Z or R G B, for a vector), which the host keys and gives with `set_controls`; refused for counts and seeds (`animatable` false), maps, Control nodes. Unlinked, the control's value now goes back into the param (`value`) |
| `edit_graph` `op=set_positions` | `positions` {node: [x, y]}, `backdrops` [{`title`, `x`, `y`, `w`, `h`, `color` [r, g, b] 0-255}] (all of them; [] none), `pins` {`points` [{`id`, `x`, `y`}], `routes` [{`from`, `output`?, `to`, `input`, `via` [pin ids]}]} (all of them; no routes: none) | the layout: node positions, the graph's `backdrops` and its `pins` (Maya's `save_layout`), kept as given; not an undo step |
| `set_param` | `node`, `param`, `value` | a map value (`texture`, `colorSet`, `attribute`, `expression`, `guide`; `component` r g b a luminance; `min`, `max`) is checked before the graph takes it: a mappable param, one source naming something, no other keys, an expression that compiles (`expression` error with its position); a vec3 param that takes a vector expression (`vectorExpression`) takes `{"expression"}` alone. Not an undo step (the add-on's widgets: Blender's undo) |
| `controls_list` | | `controls` [{`name`, `kind` (control, variable, keyed), `node`, `param`?, `variable`?, `default`, `min`?, `max`?, `value`, `set`}], `unused` (values for names the graph no longer reads) |
| `set_controls` | `controls` {name: number, or null to unset}, `replace` | `{"changed", "controls"}`: the controls' values now (`GroomSession::setControls`; Maya's `ctl_` attributes). Not an undo step: the host's properties and their fcurves hold them, given again each frame as `set_time` is; saved in the `.groom` (`controls`) |
| `nodes_folder` | | `{"userDir", "dirs", "shippedDir"}` (`nodeUserDir`: $GROOMLAB_USER_NODES, else Documents/GroomLab/nodes, shared with Maya and the app) |
| `node_file` | `node` (a Compound), header: `nodeId`, `version`, `label`, `category`, `description`, `author`, `license`, `icon`, `params`, `paramInfo` | `{"text", "file"}`: the .glnode Save as Node would write (`nodeFileFromCompound`); nothing saved |
| `save_node` | as `node_file`, `dir` (the user's folder) | `{"path", "id", "version", "node", "undo"}`: the file written (`saveNodeFile`), the compound then holding the saved copy (`updateLibraryNode` with its id, one undo step; the file stays on undo, as Maya's does) |
| `import_node` | `path`, `dir` | `{"path", "id", "version", "label", "kind"}`: a .glnode checked and copied (`importNodeFile`) |
| `script_template` | `label` ("My Script"), `author` | `{"definition"}`: a new script node's file whose id the library does not have yet (`scriptNodeTemplate`) |
| `script_errors` | `definition` (an object or its text) | `{"errors": [{"part": json, header or script, "message", "line", "column"}]}` (`scriptNodeErrorsJson`; [] when it checks) |
| `new_script_node` | `definition` (the template's), `label`, `author`, `name`, `after`, `x`, `y` | `{"name", "definition", "undo"}`: a Script node, fed by `after` and feeding what it fed (nodes.py's `add_script_node`) |
| `set_script` | `node`, `definition` | the script node's edited file (refused with `script` and what is wrong); params its header no longer has go |
| `save_script_node` | `node`, `asNew`, `dir` | `{"path", "version", "undo"}`: to the library one version past its copy there (asNew: version 1, refused when the library has its id); the node holds the saved copy |
| `save_preset` | `name`, `overwrite`, `about` {title, description, category} | `{"path", "name"}`: the graph (the default one if none) with the scalp's `size`, `about`, the layout and `backdrops`, in the user's preset folder (`presetUserDir`, where Maya saves; `exists` without `overwrite`). Not an undo step |
| `correctives_list` | | `layers` [{`name`, `enabled`, `guides` (with offsets), `weight`, `weightNow` (at the session's time, 0 off), `keyed`, `keys` [[frame, weight]...], `allFrames`, `frame` (a fix's or the strongest key's), `fadeIn`, `fadeOut` (frames), `holdBefore`, `holdAfter`, `slot`?}], `target`, `sculptNodes` |
| `corrective_new` | `name`, `frame` (the session's), `fade` / `fadeIn` / `fadeOut` (frames) or `fix` (6 frames), `allFrames` | `{"name", "layers", "target", "undo"}`: a new layer on top (shots.py's `add_layer`, the app's Add and Fix Frame): on every frame (`sculpt<n>`), or with fades a fix at the frame fading in and out (`shot<frame>`); named with letters, digits and _ (unique: `_2`...). It becomes what brushes sculpt into |
| `corrective_set` | `name`, `enabled`, `weight` (a keyed layer: keyed at the session's time, as auto key does), `keys` [[frame, weight]...] (the host's weight curve sampled, as Maya writes keyed weights into files; [] drops them), `allFrames`, `frame` / `fade` / `fadeIn` / `fadeOut` (re-timed as a fix) | `{"name", "changed", "layers", "target", "undo"}` |
| `corrective_rename` | `name`, `to` | refused when `to` is taken or not letters, digits and _ (the answer suggests one); the target follows |
| `corrective_move` / `corrective_solo` | `name`, `step` (+1 up the stack) / `name` | reordered / only this one on (on again: every one back on) |
| `corrective_delete` / `corrective_clear_keys` | `name` | removed (the target goes back to the base groom) / its keys or fades dropped: on every frame at its weight now |
| `sculpt_target` | `node` (a Sculpt node), `corrective`, or `base`; none: as it is | `{"target": {"kind": base, corrective or node, "name"}, "sculptNodes": [{"name", "editedGuides"}]}`: what brushes edit. A corrective: strokes reshape the groom as shown and record offsets into the layer at the session's time (shot sculpting, `SessionStrokeHost::setShotSculpt`; a layer with no weight then records nothing and the stroke answers a warning). A Sculpt node: shaping brushes reshape its output guides (`GroomSession::setSculptTarget`; `display` shows them as the guides), their edits kept in `sculptData`. Not an undo step; an undo or edit that takes the layer or node away puts the base groom back |
| `sculpt_clear` | `node` | `{"cleared", "sculptNodes", "undo"}`: the node's edits dropped, its guides as it is fed them (`clearSculptNode`) |

Differences from Maya: a corrective's weight is the layer's own (its value, its fades, or its
`keys` as the weight curve the core evaluates on every frame, exports and renders included),
not a keyable attribute driving a slot: the add-on samples a Blender fcurve into `keys`. Controls
are given values (`set_controls`) rather than read from attributes. Saving a node or preset writes
the file at once (Maya's too); only the graph part of Save as Node is undone.

### Pipeline (exports, simulation, colliders and other scalps, transfer, samples)

What Maya's `groomlab.game`, `exports`, `cards`, `sim`, `collision`, `extra_scalps` and `transfer`
modules do, over the helper (`hosts/blender/helper/src/pipeline.cpp`; the add-on's client is
`groomlab/pipeline.py`). The helper writes every file but FBX: Maya writes its FBX through Maya's
exporter, the add-on through Blender's, so the formats that need one answer **meshes** (below) with
the path to write each to. Exports evaluate at the session's time (`frame` sets it) at full density,
renderOnly nodes on, as Maya's exports do.

| Command | Arguments | Result |
|---|---|---|
| `export` `format=unrealGroom` / `unityStrands` | as alembic, plus `groups` ("auto": a curves object per groom group, a channel's name, "" one object), `shafts` (true/false; null: left out when the feathers have a rachis), `lod` (high, mid, low: the barbs thinned to 100/50/25% and widened by 1/sqrt), `barbDensity`, `barbWidthScale`; Unreal: `featherGuides`, `iridescenceAo`; Unity: `scale` (1), `pointsPerStrand` (0 or 3-128), `barbPoints`, `maxStrandsPerGroup` (64000; 0 none) | engine_export.h's EngineGroomWriter (Maya's `-unreal` / `-unity`): Unreal also writes the primary guides (the hierarchy's level 0), the points in Unreal's space (groom space's (x, y, z) as (x, z, y): import with the default conversion, Rotation 0 and Scale 1, and it lines up with the scalp's FBX; the groom is in the scalp object's local space, so apply the scalp's transforms, or leave it at the origin, before exporting both); `objects` [{object, group, groupId, feathers, strands, pointsPerStrand, minPoints, maxPoints}] (Maya's `-report`) |
| `export` `format=usd` / `scalesUsd` | `scales`: mesh (usd's default), instancer (scalesUsd's), both, none; alembic takes mesh or none | Maya's export_usd `scales`: the scales bent as a mesh, or a PointInstancer of their plates |
| `export` `format=gltfCards` | `path` (.glb, or .gltf with its .bin and PNGs: `binary`), `name` (the file's stem), `lods` ([hero, mid, low]: each a mesh `<name>_<lod>` baked from a copy of the graph, Maya's cards_preset), `resolution`, `cap` (true), `capResolution`, `scale` (0.01: groom centimeters to meters), `alphaMode` (mask, blend, opaque), `alphaCutoff`, `occlusion`, `rootToTip`, `tangents` | `{"files", "meshes": [names], "warnings"}` (gltf_writer.h, groomLabExportGltf); the graph is left as it was |
| `export` `format=unrealCards` / `unityCards` | `directory`, `name` ("groom", `_<lod>` with `lod`), `resolution`, `occlusion`, `scalp` (true: the hair cap's maps) | the maps (Unreal: `<name>_Depth|Coverage|Tangent|Attribute|Color|AO|Normal.tga` and `_ScalpColor|_ScalpCoverage.tga`, plus the atlas; Unity: `_BaseColor|Mask|AO|Tangent|Depth|Attribute|Normal.png` and `_ScalpBaseColor.png`), the manifest (`<name>_cards.json` / `<name>_unity.json`, Maya's text), `fbx` and `scalpCap` paths, `fbxSettings` (Unity: meters, Y up), `meshes` {cards, scalpCap} for the host to write: the cards (quads, the atlas UVs, the strands' normals, color `groomLabTint` at half the tint) and the hair cap (the scalp lifted 0.4% of its size along its normals) |
| `export` `format=scalesFbx` | `path` (.fbx), `unity` | `meshes` {scales}: the scales bent onto the skin (Maya's groomLabScaleMesh: UVs, uv sets scaleUV and scaleData, colors scaleColor and scaleRootColor, the scales' normals) |
| `export` `format=unityFeathers` | `directory`, `name`, `lod`, `points` (16), `barbPoints` (6), `density`, `shafts`, `rachis` (true), `maxStrandsPerGroup` | Maya's export_unity_feathers: `<name>_strands.abc` (a curves object per groom group), `<name>_unity_feathers.json`, `groups`, and `meshes` {rachis} (`<name>_rachis.fbx`) when the feathers have a rachis |
| `bake_scale_maps` | `directory`, `name`, `resolution` (2048), `maxHeight` (0: measured), `padding` (8) | `{"files", "maps" {ScaleNormal, ScaleHeight, ScaleColor: [paths]}, "pattern", "maxHeight", "udim", "tiles", "covered"}` (scale_maps.h) |
| `scale_mesh` | `space` | `meshes` {scales} as scalesFbx's, nothing written |
| `strand_mesh` | `style` (tube, ribbon), `sides` (4), `widthScale`, `minWidth`, `strandUVs`, `density`, `maxStrands` (100000, thinned by ID); or `nodes`: true | Strands to Mesh (strand_geometry.h buildStrandMesh): `meshes` {strands} with UVs root to tip (and uv set strandUVs), `strands`, `total`, `capped`; with `nodes` each StrandMesh node's mesh by its own options |
| `set_scalp_frames` | `first`, `points` (frames x the scalp's points, 3), `restPoints`; none: dropped | `{"first", "last", "frames"}`: the scalp at other frames, the Simulate nodes' feed (GroomSession::setSimulationFeed): a frame they step through (Simulate Range, a jump forward) reads the scalp of that frame. Not an undo step |
| `simulate_status` | | `nodes` (the Simulate nodes making the output), `status` [{node, startFrame, ranges, frames, live, baked, bakeStale, bakedFirst, bakedLast, bakedFile, stepMs, bytes, stamp}], `bakes` (the file's simCaches), `scalpFrames`, `running` |
| `simulate_range` | `last`, `budgetMs` (200; 0: to the end in one request) | Simulate Range in steps (GroomSession::simulateTo, stopped by its progress callback when the budget is spent, a frame at least): `{"running", "done", "frame", "target", "progress" 0-1, "frames", "milliseconds", "errors", "status"}`; the protocol answers anything between steps |
| `simulate_poll` / `simulate_cancel` | `budgetMs` / | the next step, as simulate_range answers / the job dropped (the frames stepped stay) |
| `simulate_bake` | `last`, `first` (each node's start frame), `node`, `path` (one node), `directory` (beside the groom file, else the temp folder), `name` | Bake Simulation (Maya's bake_simulation): simulated to `last`, each node's frames written as `<name>_<node>_sim.abc` (sim_cache.h) and read back as its bake, named in the file's simCaches; one undo step (the files stay): `written` {node: path}, `status`, `undo` |
| `simulate_clear` | `node`, `bakes` (true) | Clear Simulation: the frames dropped (GroomSession::invalidateSimulation) and the bakes, one undo step: `clearedBakes`, `status`, `undo` |
| `set_meshes` | `kind` (collider, prototype), `meshes` {name: mesh or null}, `replace` | colliders: the meshes ColliderMesh nodes read by name (GroomSession::setMeshes; a mesh as set_scalp takes it, with `restPoints`; an entry with `points` alone poses the one there), saved in the .groom's `colliders`; prototypes: the Instance nodes' meshes (InstancePrototype per face corner, `normals` and `uvs` per corner, turned into groom space: the host's up is the prototype's +Y), saved as `prototypes`. Answers meshes_info. Not an undo step (the host's inputs) |
| `set_scalps` | `scalps` [{name, mesh}] in the order they join | the groom's other scalps (GroomSession::setScalps; names letters, digits and _, not "scalp"), saved as `scalps`; answers meshes_info |
| `meshes_info` | | `colliders`, `prototypes`, `scalps`: [{name, points, triangles, rest}] |
| `transfer` | the new scalp as set_scalp takes it, `method` (closest, bounds, markersAlign, markersWarp, uv; `fit`: bounds), `markers` ([{old, new}] or [[old, new]], a side a host point or {point, face, u, v}; none: the stored ones), `keepLengths`, `fitSize`, `maps` (true), `snapMarkers` | Transfer Groom to Scalp (transfer.h, Maya's groomLabTransfer): guides, parts and flow re-rooted, painted maps projected (texture maps into new images in the maps folder; without `maps` vertex maps go, `missing`), the new scalp the session's with its topology, the groom's scale kept (or fitted: `fitSize`), the caches and bakes made on the old scalp dropped, flat guides rooted on it by position, the markers snapped and stored. One undo step: undo and redo answer `scalpChanged`. `{"method" (the one used), "guides", "projected", "missing", "warnings", "markers", "scalp", "undo"}`; `snapMarkers` answers the snapped `markers` alone, nothing changed |
| `transfer_markers` | `markers` | the stored pairs (points in the host's space); given, they are replaced (one undo step) |
| `sample_meshes` | | `{"dir", "samples": [{id, label, file, extentCm, name, part, license "CC BY 4.0", author, units "cm", upAxis "Y", facing "+Z"}]}`: Nika, Manny and the wolf (bin/meshes), the add-on imports them with Blender's OBJ importer |

**Meshes answered** (`result.meshes`, `{key: mesh}`): `points` (n,3) in the host's space (`space`:
"groom" for groom space), `faceCounts`, `faceIndices`, `corners`, and per face corner `uvs` (corners,2),
`normals` (corners,3, host axes), `uvSets` {name: (corners,2)}, `colors` {name: (corners,4) linear};
with the mesh's own fields (`fbx`, `material`, `cards`, `triangles`...). They lie in the session's meshes
arena (valid until its next answer). The add-on's `pipeline.write_fbx` makes each a temporary object
and writes it with `bpy.ops.export_scene.fbx` (Unity's in meters, Y up).

**Simulate in Blender.** As the timeline plays, each frame's `update_scalp` and `evaluate` step a live
Simulate node one frame (the core's own stepping). Simulate Range and a jump forward step several
frames in one request; `set_scalp_frames` gives the scalp at those frames (the add-on's
`pipeline.send_scalp_frames` plays them in Blender), without it they step toward the scalp as it is
now. Jiggle is still not fed (it needs the scalp's earlier frames as a history: GroomScene's
ScalpHistorySampler, not wired yet).

Differences from Maya: the FBX files are written by the host from the meshes answered (Maya's own
FBX exporter writes its scene meshes); the manifests are Maya's, word for word, except the Unity
one's units line. unrealGroom does not write the scales' FBX beside the .abc by itself (Maya's
`scales="auto"`): the add-on writes scalesFbx. A transfer leaves the session on the new mesh; the
add-on's Transfer Groom to Scalp then makes the target object own the groom (below). Other scalps
are only the meshes: the Scalp/Scatter/Merge wiring Maya's add_scalp does is a Graph Editor edit,
and set_scalps does not resize painted vertex maps for the added vertices (Maya's
_resize_vertex_maps). Bakes go to `directory` (the add-on: the .blend's groomlab/caches), not Maya's
scene caches folder.

**The add-on's menus over these** (`groomlab/pipeline_ops.py`, the work in `pipeline.py`): the
GroomLab menu's Export, Simulate, Add, Review and Utilities items, bound in `actions.py`.

- Graph inserts (Maya's add_simulate, add_collider, create_cards, add_feathers, add_braid) are made
  as Maya's GraphModel makes them, with `edit_graph` ops (`pipeline.GraphEdits`: add_node fits each
  node's defaults to the scalp), then the graph is kept on the scalp object, so Blender's undo takes
  them back (graph_tree's after_undo). Two commands back them: `cards_preset` and `preset_graph`
  (below). Curve objects become guides of a group with set_guides' `group`.
- Add > Collider keeps the colliders on the scalp object (`groomlab.colliders`: name, object) and
  `groom.sync` sends them again (set_meshes, in the scalp's space) whenever their pose changes
  (`groom.input_hooks`: a frame, an edit).
- Simulate Range is a modal operator: `send_scalp_frames` over the playback range, then
  simulate_range and simulate_poll in 100 ms steps on a timer, the progress in the status bar, Esc
  sends simulate_cancel. Bake Simulation is the same operator with `bake` (simulate_bake after).
- Transfer Groom to Scalp with `own` (the default) reads the target in its own object space and
  then moves the groom to it (`pipeline.move_groom`: the GroomLab properties, the Graph Editor's
  tree, the hair and guides objects parented to it, the ctl_ sliders and the correctives' and
  sliders' keys); the session and its uid stay. Undo keeps both sides together: Blender's undo
  restores the objects and an undo_post handler undoes the helper's transfer to match
  (`undo_moves`, by the transfer's place on the helper's stack); GroomLab's own Undo of the transfer
  moves the groom back (`follow_helper_undo`), Redo moves it again.

| Command | Arguments | Result |
|---|---|---|
| `cards_preset` | `lod` (hero, mid, low), `node` (one Cards node; default every one), `triangles` | Maya's cards_preset: each Cards node of the graph set for that level of detail by what it cards (card_presets.h: hair, feathers or both). Not an undo step (the host keeps the graph). `{"nodes", "params"}` |
| `preset_graph` | `name` or `path` | a preset's graph fitted to the session's scalp (presetGraphJson), nothing changed: `{"preset", "graph"}` (the add-on takes a feather design's Compound and its shafts' Scatter params from it) |
| `set_guides` `group` | `target` "hierarchy" | new guides (ids 0) join this guide group, a root group made when missing (Maya's guides_from_curves group=); with `keepMissing` the others stay |
| `bake_to_guides` | `node` ("" the output), `group` ("baked"), `density` (1) | Bake Node to Guides (Maya's groomLabGuides -fromNode): the node evaluated with the inputs as they are now, each of its strands (or guides) a primary guide of root group `group` (made when new, added to otherwise; a child group is refused), one undo step: `{"node", "group", "guides", "ids", "undo"}` |
| `import_alembic_curves` | `path`, `object` (only the curves under it), `samples` (24) | Import > Alembic Curves (Maya's import_guides_from_alembic): the graph's `guides` node replaced by an ImportCurves node reading the file, rooted on the scalp; the curves read as GroomLab's and Maya's Alembic files have them (Y up, centimeters). The file is read first (no curves: refused). Not an undo step (the host keeps the graph): `{"node", "curves", "path", "graphError"}` |

### Photos (reference cameras, the camera solve, Guides from Photos)

Maya's Groom Tools > Photos (`groomlab.reference`: a photo as a camera lined up with the mesh by
pairs of points) and Guides from Photos (`groomlab.from_reference`), over the helper
(`hosts/blender/helper/src/reference.cpp`; the add-on's side is `groomlab/photos.py`). The photos
are kept with the groom: its file's `referencePhotos`, in the standalone app's Photos window's
form (groom_file.h: each photo's name, image, size, mask, `camera` {rotation, position, focal_px},
`state` {landmarks, strokes, mask_strokes}), groom space, so a groom saved from Blender opens with its
photos in the app and the other way round. The host speaks its own space: a camera is `{"rotation"
(3x3 rows; its columns the camera's X right, Y up and Z back axes: it looks down -Z, as Maya's and
Blender's cameras), "position", "focalPx" (the photo's pixels)}`; the axes turn as directions, the eye
as a point, pixels stay pixels. Image points are pixels from the photo's top-left, v down.

| Command | Arguments | Result |
|---|---|---|
| `reference_photos` | | `photos`: [{name, image, width, height, mask, camera (host) or null, pairs [{image [u, v] or null, mesh [x, y, z] (host) or null, error?}], rms, strokes, maskStrokes (counts)}] |
| `set_reference_photos` | `photos` (as reference_photos answers; names unique) | the groom's photos replaced, each keeping the flow arrows, mask touch-ups and mask the stored one of its name has (the app draws them; the host does not). No undo step (the app's window stores the same way): `{"photos"}` |
| `solve_camera` | `pairs` (as above; a pair missing a side is left out), `width`, `height`, `focalPx` (the guess, or kept), `keepFocal`, `initial` (a camera: where it is now) | reference_edit.h's solveReferenceLandmarks: 4 complete pairs with the focal length kept (or fewer than 6), 6 or more solve it too. `{"rotation", "position", "focalPx", "focalSolved", "pairs", "rms", "errors" (per given pair, null for one left out), "bad" (pairs far worse than the rest)}`; too few pairs or a failed solve: `state` |
| `guides_from_photos` | `photos` (names; default every photo with a camera), `guideCount` (300), `symmetry` (auto, x, topology, none), `maxLength` (host units; 0 automatic; below 0 times the rim the photos show), `group` ("fromReference"), `replace` (true) | reference_guides.h's guidesFromReference on the session's scalp with each photo's image, mask, flow arrows and mask touch-ups (Maya's from_reference.generate): the guides into the guide group (replaced), one undo step. `{"guides", "group", "views", "symmetry", "rootsTried", "rootsUnseen", "rootsBald", "rootsShort", "maxLength", "look" {ok, melanin, redness, baseColor, rootAppearance, tipAppearance, curly, clumped, clumpSize, coverage}, "undo"}` |

**The add-on.** Each photo is a Blender camera (`Object.groomlab_photo`: the image path, size, the
point pairs, the last solve's error, Lock) with the photo as its background image (`FIT`, 60%
opaque, in front of the mesh), under the "GroomLab References" collection, its sensor fit
horizontal (36 mm), so its focal length in pixels is lens / 36 mm x the photo's width. Import Photo
aims it at the groom from the front (-Y). Look Through makes it the scene's camera and the 3D view's
(the render size set to the photo's, so the camera frame has the photo's shape; again: back to the
view and the camera before). Pick Points is a modal operator: a click in a 3D view looking through
the photo's camera marks a photo pixel (the photo's place in the camera frame, as the background
image fits it), a click in any other view marks the point on the scalp under it (a ray cast, kept in
the scalp object's space); pairs fill in turn (Maya's rule: the first pair missing that side) and
draw numbered. Solve sends the pairs and the camera (in the scalp object's space) to `solve_camera`
and takes the answer (its scale kept at 1). After every change the scene's photos go to
`set_reference_photos`; a groom file opened with photos and no cameras of their names gets them
(`photos.restore_from_groom`, from `reference_photos`). Guides from Photos (`groomlab.photos_guides`:
the GroomLab menu's item and the tab's button) sends the photos and runs `guides_from_photos` on
every photo camera (one placed by eye counts as lined up, as in Maya).

Differences from Maya: Maya's Photos window (the photo and the mesh side by side, flow arrows and
mask touch-ups drawn on the photo, the measured look applied to the graph) is not in Blender: the
arrows and touch-ups the app or Maya drew are kept and used, not drawn; the photo stays where it is
(Maya copies it into the scene's references folder); Add Camera (a camera that already shows a photo
on an image plane) and Select Camera are Maya's. A landmark pair with a side cleared to pick again
is written with a null there; the app reads each side's points in order, so such a pair's partners
shift there until it is picked again.

## 4. Install layout

`cmake --install <build> --component Blender --prefix <dir>` lays out the whole Blender extension,
both halves:

```
GroomLabBlender/              the extension folder (Blender installs it as extensions/<repo>/groomlab)
  blender_manifest.toml       id "groomlab", the project's version, platforms, permissions (files, network)
  LICENSE                     GPL-3.0-or-later: the add-on's Python
  __init__.py, groom.py ...   the add-on (hosts/blender/addon/groomlab)
  bin/
    groomlab-helper(.exe)     the helper
    tbb.dll                   its runtime DLLs (Windows)
    presets/                  the shipped graph presets (the helper looks beside itself, then ../presets)
    nodes/                    the shipped compound nodes (the same)
    meshes/                   the sample meshes (Nika, Manny, the wolf; CC BY 4.0): sample_meshes lists them
    LICENSE-HELPER.txt        the helper's terms: Apache-2.0, a separate program
    THIRD_PARTY_NOTICES.txt   Alembic, Imath, JSON for Modern C++, TBB...
```

The build assembles it in `build/<preset>/blender_extension/<config>/GroomLabBlender` with its zip
`GroomLab-Blender-<version>-<platform>.zip` beside it (target `groomlab_blender_extension`, in
ALL; `hosts/blender/addon/assemble_extension.cmake`): the manifest at the zip's root, as Blender's
Install from Disk and remote repositories take it. Releases: `tools/package_blender.py` (the same
install into `dist/`, checked with `blender --command extension validate`) and
`tools/publish_blender.py` (R2: `blender/GroomLab-Blender-<version>-<platform>.zip`, a `-latest`
copy, `blender/latest.json`, and `blender/index.json`, a Blender extensions repository listing so
users who add it as a remote repository get updates from Blender). The Maya release scripts are
unchanged.

User presets are shared with Maya and the app (presets.h: `GROOMLAB_USER_PRESETS`, else Maya's
user folder).

## 5. Tests

`blender.helper` (labels `quick;blender`): `tests/hosts/blender/test_helper.py` drives the built
helper with the client in `helper_client.py`, as the add-on will, under Blender's own Python (found
under Program Files; `GROOMLAB_BLENDER_PYTHON` picks another Python 3 with numpy). No Blender
process. It checks a preset groom on the test head (Blender space in, shared memory both ways)
against groomlab-cli's evaluation of the `.groom` the helper saved, hash for hash (the CLI's `hash`
recomputed in Python from the shared memory arrays), and the helper's Alembic export against the
CLI's; guides from the host (new, round-tripped unchanged, one sculpted and evaluated
incrementally, removed and redrawn, re-rooted on a remeshed scalp) and a groom file's hierarchy
guides edited by ID, each again against the CLI; params, graph edits, inserted and fitted presets;
the Z-up meters round trip through a Y-up centimeter file and a Z-up one; malformed requests answered while the helper carries on; the helper
ending with its stdin; guides kept at rest following a turned scalp (with and without
`followScalp`) and back at rest bit for bit. The pipeline (`PipelineTest`): unrealGroom and
unityStrands (resampled, frame by frame too), glTF cards with levels of detail from copies of the
graph, Unreal and Unity cards (maps, manifests, the meshes for FBX), the scales' mesh, maps and
instancer, Strands to Mesh; Simulate Range in 1 ms steps answering pings between them, fed a
swinging scalp's frames (its hair ends elsewhere than a still one's), bake and clear undone and
redone; colliders, prototypes and another scalp saved in the `.groom`; a transfer by bounds and by
markers undone and redone (`scalpChanged`); the sample meshes.

The tools (`ToolsTest`) are driven headlessly with a synthetic Blender camera (Z up, meters) over
the test head, and every stroke sequence is replayed by the core itself:
`tests/hosts/blender/stroke_reference.py` runs the bindings' `BrushTool` over a `StrokeHost` (what
the app's BrushSession drives) on the `.groom` the helper saved before the strokes, with the
helper's groom-space matrix, under the Python the bindings were built for
(`GROOMLAB_CORE_PYTHON`/`GROOMLAB_CORE_PYTHONPATH`, set by ctest when `groomlab_core_py` is built:
Maya's mayapy as a plain interpreter for the ninja presets; without them the comparisons are
skipped); the stored texts after each step must be the same, character for character. Covered:
the view (the groom matrix against numpy's, the ring on the scalp under the cursor and 50 px wide
there); Place (guides 4 cm along the normal), Comb with and without stretch, Draw; symmetry in
Blender's axes (x, and y as the groom's -z with its center negated; a placed mirrored pair combed
the mirrored way); Freeze holding guides through a comb and drawn frozen blue; lasso, box and click
selection with selectedOnly; Part, Flow (arrows, a whorl sink) and a comb of parts and flow;
painting a vertex map and a guide layer (compared), a texture map's file painted and put back by
undo and redo; generate children, lock, freeze, rename, hide, delete; undo and redo back to every
step's texts; the display buffers (frozen colors, the selection item, nothing re-sent for what
the host holds, another generation everything), the strand color modes, a live stroke's guides
without evaluation and the stroke density; the state round trip into another session (the same
texts, textures and strands, the CLI's hash of its saved file).

The add-on, in a headless Blender (`blender --background --factory-startup`), the build's
extension zip installed by `blender --command extension install-file` into a throwaway profile
(`BLENDER_USER_RESOURCES`/`_CONFIG`/`_SCRIPTS`/`_EXTENSIONS` in a temp folder:
`tests/hosts/blender/run_blender_test.py`), never the user's own Blender:

- `blender.extension` (quick): Blender's validator accepts the zip.
- `blender.addon`: a groom on the test head (the hair in Blender bit for bit the helper's
  evaluation; the saved `.groom` evaluated by groomlab-cli hash for hash); guides sculpted by script
  (moved: an incremental evaluation; drawn: the next ID written back and the root placed on the
  scalp; deleted); params through the panel's widgets in scene units (meters and a centimeter
  scene); a choice; nodes added, moved and removed; presets applied and inserted; the helper
  killed and the groom rebuilt from Blender; a shape-key lift and an armature turn the strands
  follow with stable IDs; Alembic and USD exports; a `.groom` opened with its own scalp; a `.blend`
  saved and opened again with another helper, the same hair.
- `blender.addon_free` (quick): with no key anywhere, Alembic and USD exports write their files,
  a saved `.groom` has no `"license"` and an old one with `"license": "learning"` opens and exports;
  Support GroomLab in the GroomLab menu and About opens https://ko-fi.com/getgroomlab; every panel
  drawn into a checking stand-in layout (each property and operator it names exists, no Learning
  note); the panel's operators; a Jiggle node marked.
- `blender.addon_render`: a Cycles CPU render (128 px) gets every strand (render density) and the
  viewport its preview density back; the hair shows in the image. The hair's attributes and
  material follow the color nodes' look (HairColor's RGB; a tip color per point, the iridescence
  ramp; HairColor's roughness and tilt; a material of the
  user's left alone; EEVEE set to Strip). The "GroomLab Hair Color" node group's output, rendered as an emission, is
  `shading.cycles_color` (the fitted absorption as a direct color) for three inputs. Width: sparse
  70 micron strands cover their projected area (0.975 of it: overlaps).
  Color: a melanin groom and a solid-dyed one, under the neutral light of the comparisons, render
  kick's average color for the same .groom within 6% (measured: within 2%).
- `blender.addon_graph`: the Graph Editor (section 7). The tree built from a preset
  is the helper's graph (nodes, typed ports in Maya's colors, links, positions); nodes added,
  linked, unlinked, renamed, muted, deleted and duplicated in the tree reach the helper and the
  hair; a link the graph refuses goes away; params set on the nodes in scene units (meters and a
  centimeter scene) and from the 3D view's panel; a ramp through its curve widget; the sidebar
  panel drawn (sections, Advanced folded and unfolded, showWhen exactly when its condition holds,
  the curve widget); presets applied (the preset's layout) and inserted (a Compound after the
  selected node with its exposed params, the nodes after it moved over); positions kept through a
  rebuild and an undo, Tidy Layout, saved in the `.groom` where Maya's editor reads them and read
  back from it; the graph's pins as reroutes (a bundle: one reroute, two links out; a route whose
  link is gone left out) and reroutes as pins (one put on a link: the same connection, routed, no
  evaluation and no undo step; moved: layout; fed from another output: both links through it
  rewired; fed from a mesh: refused, as it was; left with no link out: the link gone, the reroute
  kept, no pin, then a pin again; saved in the `.groom`); View and Set as Output; an outdated node (TestCApiLift at version 1, in
  `groomlab-helper-testhooks` only: tests/engine/capi/capi_versioned_op.cpp) upgraded; the header,
  menus and panels drawn; the tree removed with the groom.
- `blender.addon_graph_authoring`: the Graph Editor's authoring (section 7, "Authoring"), with its
  own node library and preset folders. Every action the graph menus list bound (or Blender has
  none), labels, tips and colors graph_ui.json's, the node, Edit and Add menus in Maya's order,
  Ctrl+G / Ctrl+Shift+G only in GroomLab graphs; group, show an inner param, the helper's undo and
  redo rebuilding the tree, ungroup; expose a param; a node dropped on a link inserted with
  `insert_on_link`; a slider's ctl_ property given to `set_controls`; a param keyed at two frames,
  Blender's value between them reaching the control, Remove Animation keeping it; a count refused;
  a painted map in Tab search added as a Map node; map sources (color set, attribute, painted map,
  cleared) and an expression's text block (an error shown and refused, a fix taken); backdrops
  added around nodes (their parent), recolored, renamed, moved with their nodes, a graph's backdrop
  made a frame, deleted, saved in the `.groom`; Copy Graph and Paste Graph (fenced text, bad text
  refused); Save as Node, Save to Library, a newer library file's Update Node, Import Node, Save as
  Preset (exists, Replace); a script node made, an error shown and refused, a fix and new label
  taken live, saved to the library.
- `blender.addon_pipeline`: the pipeline's client (section 3, "Pipeline"): Unreal and Unity cards
  and the scales written as FBX by Blender's exporter from the meshes the helper answers (the
  temporary objects gone after; the cards read back: quads with their tint), glTF cards, Strands to
  Mesh as an object under the scalp, unrealGroom and scalesUsd, the scales' mesh and baked maps;
  colliders and another scalp from Blender objects; a transfer onto a moved copy of the head (the
  roots 5 cm over) undone, markers snapped and stored; the sample meshes listed and Manny added at
  its real size, Z up.
- `blender.addon_tools`: the Groom Tools (section 8). The operators, the top bar
  menu in Maya's order, the pie menu on Y in the add-on keymap, the icons as previews; Create Groom
  making a hierarchy groom (no Blender guides object, the state kept at once); the palette drawn
  into the stand-in layout (the first-groom steps, the tiles by category, the card showing exactly
  the catalog's settings for the tool); tiles sending each tool and its settings
  (the catalog's default strength, each tool's own strength kept, unsigned tools clamped at 0);
  the modal tool's event handling (`ToolSession`) driven with synthetic events over a synthetic
  region and region_3d (the helper tests' camera): hover (the ring and its draw list), five Place
  clicks and a Comb drag with pen pressure, each step's guideData equal to a second session of the
  helper doing the same strokes from the same state, the hair in Blender the helper's evaluation;
  Ctrl+Z / Ctrl+Shift+Z and the palette's Undo/Redo; Esc dropping a live stroke; B and M + drag;
  Shift+click; Page Up/Down; S and the lasso, click and box selection; events off the viewport
  passed on; Freeze holding guides through a comb; Esc leaving the tool. The display: draw lists
  (guides per-vertex colored lines, the selection in the theme's white, frozen blue, tubes as lit
  triangles, nothing re-sent for what is held), drawn into a `GPUOffScreen` with the camera's
  matrices (`gpu.init()` works headless here; skipped where it does not) with a guide's middle lit
  where the camera sees it; strand colors (random) as the hair's active color attribute and back;
  the strands hidden. Paint: a vertex map made, painted through the tool (scalpMaps equal to the
  helper's own stroke), shown on the scalp; a guide map filled and removed; groups made, renamed,
  hidden, deleted and undone. The state: a killed helper and a `.blend` opened with a new helper
  give back the same texts and the same hair, and the tools work on it (the state kept after a
  stroke without the scalp). A hierarchy groom follows a shape-key lift (a frame reads the posed
  points alone, never the scalp's faces and UVs).
- `tests/hosts/blender/bench_addon.py` (no ctest; run it as an add-on test, through
  run_blender_test.py): where the add-on's time goes. A groom made on the test head, then a comb
  stroke, a live drag, a stroke's release, a param, a preset, a render's full density and back, a
  frame of a shape-keyed scalp, each broken into the add-on's Python, each helper request's round
  trip, the helper's evaluation, the hair's writing and Blender's depsgraph after.
  `GROOMLAB_BENCH_SUBDIV=2` subdivides the head (18k points, a production scalp's density),
  `GROOMLAB_BENCH_DENSITY` sets another preview density, `GROOMLAB_BENCH_PROFILE=1` adds cProfile.

## 6. B2: the add-on

`hosts/blender/addon` (GPL-3.0-or-later; `groomlab/` is the extension's Python, `LICENSE`,
`blender_manifest.toml.in`). A Blender 5.x extension (blender_version_min 5.0.0, tested on 5.2.2
LTS), platforms windows-x64 for now, permissions files. Distributed from our site, not extensions.blender.org.

| Module | What |
|---|---|
| `helper_client.py` | the client (pipes, shared memory, numpy); plain Python, no bpy: `tests/hosts/blender/helper_client.py` loads this very file |
| `process.py` | the helper's life: started on first use (`CREATE_NO_WINDOW`, `--parent-pid`, log in the temp folder), `hello` checked (protocol 1), started again when it died (`generation()` rises: every groom's session is rebuilt), shut down on unregister and `exit_pre` |
| `groom.py` | a groom: the scalp mesh owns it (`Object.groomlab`, role SCALP, a uid), its guides (a Hair Curves object, surface = the scalp, its UV map, hidden in renders) and its hair (a Curves object, locked), both parented to the scalp with identity transforms: everything crosses the pipe in the scalp object's local space, the session's space Z up with the scene's unit scale. Sync: the scalp (`set_scalp` for another mesh, `update_scalp` for another pose: the evaluated mesh, rest = the mesh before modifiers), the guides (`set_guides` pose rest by `groomlab_id`, IDs of new curves and the helper's rooting written back), time and fps, space, followScalp; `evaluate` (the viewport: the groom's Preview Density, 0.1 as Maya's previewDensity, and Point Budget, 500k points, both a subset of the render's strands; final renders every strand); the hair written with foreach_set (curves rebuilt only when the sizes change; `radius` = widths/2, `groomlab_strand_id` (INT32_2D), `groomlab_color`, `melanin`, `melaninRedness`, `surface_uv_coordinate`, and as the look needs them `groomlab_point_color` (POINT), `iridescence` (POINT), `groomlab_root_normal`); the material from `shading.py` |
| `shading.py` | the hair's material: Maya's Arnold look (`create_hair_shader`) with Principled Hair, fitted to render as kick does (section 6, "Rendering"), the pigment's look shifted per strand by HairColor's hue, saturation and value draws, HairColor's highlight (the roughness per strand where it varies, the tilt as Principled Hair's offset; its specular per strand is Arnold's alone, the material takes the node's own) and a RenderData node's opacity per point toward transparent; EEVEE set to Strip hair when a groom is made or opened; the panel's render notes |
| `graph.py` | the 3D view's compact graph: the node list (the main chain from the strands' source to the output first), the selected node's params from `node_spec` with values from the graph, widget per kind and unit, `showWhen`, set_param; add/remove/move along the chain with `edit_graph connect`; presets |
| `graph_tree.py`, `graph_editor.py` | the Graph Editor (B3c, section 7): the groom's graph as a node tree kept in step with the helper both ways; its operators, sidebar panels, menus, keys and node overlay |
| `props.py`, `ops.py`, `ui.py` | properties (a groom's settings and its graph JSON on the scalp object, saved in the .blend), operators, the sidebar tab "GroomLab" (groom, presets, graph) |
| `handlers.py` | `depsgraph_update_post` (guides or scalp edited: debounced, evaluated 0.04 s after the last change, or while changes keep coming (a slider drag, a stroke) every 1.5 x the last evaluation's own time, 0.06-0.3 s, from a timer; a Mesh update reads the scalp's faces and UVs again, the scalp object's geometry (a pose) its points alone), `frame_change_post`, `render_pre`/`render_complete`/`render_cancel`, `save_pre` (each groom kept as a `.groom` text in its scalp object), `load_post`, `undo_post`/`redo_post`, `exit_pre` |
| `starter.py` | starter guides for a new groom: farthest-point picks among area-weighted samples (seeded), each along the normal bending toward the ground, as long as the preset's Scatter length |
| `pipeline.py` | the pipeline's client (section 3, "Pipeline"): exports (FBX written from the meshes the helper answers), the scales, strands to mesh, Simulate Range and bakes with the scalp's frames, colliders, prototypes and other scalps from Blender objects, transfer and its markers, the sample meshes |
| `tools.py`, `tool_props.py`, `tool_ops.py`, `display.py`, `palette.py`, `menu.py`, `icons.py`, `icons/` | the Groom Tools (B3b, section 8): settings, the modal tool and pie menu, Maya's viewport display, the palette, the top bar menu, Maya's icons |

Decisions (the open questions of B1):

1. **Packaging**: a Blender extension (manifest at the zip's root, `bin/` inside); a remote
   repository listing (`blender/index.json`) gives Blender's own updates.
2. **Param units**: lengths and widths in scene units (Blender's length display; the session's
   `metersPerUnit` is the scene's unit scale), per-length rates per scene unit, densities per cm²
   (what hair density is quoted in), fractions as factor sliders. Presets and grooms stay in
   centimeters.
3. **Jiggle and Simulate**: Simulate steps frame by frame as the timeline plays, and Simulate Range
   (section 3, "Pipeline") is fed the scalp's frames; Jiggle is not fed yet (no scalp history from
   Blender): the panel marks it, the groom evaluates (Jiggle passes the strands through).
4. **Scalp caches**: the add-on plays Blender's own mesh; a file's scalp cache is not read.
5. **Painting, parts and flow**: B3a brings them (and every brush) to the helper (section 3,
   "Tools"); B3b's Groom Tools drive them (section 8).
6. **Concurrency**: one helper; a final render evaluates at render density in `render_pre`
   (Render > Lock Interface is turned on when a groom is made) and the preview comes back after,
   on Blender's main thread.
7. **Deforming scalps**: guides stay at rest (`pose: "rest"`, section 3), `followScalp` on by
   default for a groom made in Blender (a loaded groom keeps its file's setting). A modifier that
   changes the vertex count cannot be followed (the mesh before modifiers is used, with a note).
   Each frame of a deforming scalp re-evaluates from Interpolate on (the carried guides are not the
   rest guides to the bit); a core API taking rest guides directly would let following frames only
   carry the strands.
8. **Object scale**: the scalp's local space is the groom's; keep its object scale at 1 (presets
   fit to the mesh's own size).

### Rendering: Arnold's look in Cycles and EEVEE

The goal: a groom rendered in Blender looks like the same groom rendered with Arnold from Maya
(create_hair_shader's aiStandardHair, the Arnold procedural or MtoA translator): the same thickness
and the same colors. Measured with kick (Arnold 7.0, rendering in linear sRGB: `color_manager_ocio`
with `scene-linear Rec.709-sRGB`, see the Arnold color-space note) against Blender 5.2 headless on
the same `.groom`: an ortho camera, a distant light (Blender's Sun strength is Arnold's intensity:
both give a white Lambert plane cos/pi), a uniform gray world (= a skydome's color), Standard view.

**Width.** The widths cross the pipe as diameters in host units; the hair's `radius` is half
(a uniform scale of the scalp scales the strands' radius with it, as Arnold's does with the
groom's transform; a scale that is not uniform renders an average, the panel says so). Cycles'
coverage of the same strands matches Arnold's within 0.3% on sparse 70 and 500 micron strands
(ribbons and 3D curves alike, Arnold's ribbon and thick modes alike), within 1% on the presets,
and EEVEE's Strip within 0.3% (2% on short fur). **EEVEE's Strand shape (Blender's
default) draws every strand one pixel wide whatever its radius**: 8x too thick for 70 micron hair
50 cm across 600 px. Making or opening a groom sets Render > Curves > Shape to Strip; the panel
says so when it is Strand.

**Color.** Arnold's and Cycles' hair (both Chiang 2016) differ in their numbers, so the material
does not hand Principled Hair GroomLab's melanin: it reproduces what kick renders. Fitted from
renders of constant melanin (0.1 to 0.9, redness 0, 0.5, 1) and base colors at Arnold roughness
0.15, 0.3 and 0.45, each Arnold render's channels turned into the absorption Cycles needs to
render the same average (through Cycles' own absorption sweep on the same strands):

- melanin m, redness r: m^1.9 x ((1 - r) (2.27, 3.69, 7.00) + r (1.57, 3.21, 8.11)) x
  (1 + 0.4 (ra - 0.3)), ra being Arnold's roughness (0.1 + 0.4 x HairColor's roughness);
- a base color c (the dye over the pigment, or the RGB over no melanin): 1.175 x Chiang's
  (ln c / f(ra))^2;
- plus 0.0325 - 0.025 (ra - 0.3) on every strand (Arnold's light hair keeps less energy);
- the highlight: Arnold's is about 1.5x Cycles' and a little wider, so Cycles' roughness is
  0.25 + 0.5 ra and a black Principled Hair (its highlight alone) adds 1.5 x tint - 1 of it (tint:
  HairColor's specular / 0.3, at most 1; without a HairColor node specular 0.3 and roughness 0.5);
  Principled Hair's offset is HairColor's tilt (Arnold's shift).

rms error of the fit 1.4% (melanin) and 2.9% (base colors) in absorption. The "GroomLab Hair Color"
node group computes that absorption per channel from Melanin, Redness, Color and Roughness and
outputs the Principled Hair direct color with that absorption at Cycles' roughness (and an EEVEE
color). The material, like create_hair_shader: the RGB's look (`groomlab_point_color` per point,
else `groomlab_color`, over no melanin) when `colorsByRgb` or `colorBlends` (any color node);
else the pigment's (the strands' melanin and melaninRedness, the dye; 0.35 and 0.2 without the
channels); iridescence as
create_hair_shader's network (a five-key ramp of the structural color by |cos| of the angle
between the view and the root normal, over the RGB by the `iridescence` attribute). Two Material
Outputs: Cycles' and EEVEE's.

Results (average hair color, Cycles / Arnold, 400 px, watermark masked; tipped_fur and wool were
presets then): hair_basics 0.99-1.01,
hair_basics dyed (0.9, 0.35, 0.25) 0.98, a colorSource mix 0.98-1.04, tipped_fur 0.97,
agouti_fur 0.95-0.96, wool 1.07-1.09, feathers_iridescent 1.08-1.25; coverage 1.000-1.009.

What is approximated:

- **EEVEE** renders hair as a diffuse shade of its color, without highlights or light scattered
  between strands: its output's color is the one whose shade matches Cycles' average under a
  neutral light (a curve fitted on dense hair and on sparse fur, weighted 0.7 / 0.3). Measured
  (before HairColor colored by its RGB in mode melanin too):
  hair_basics 1.10-1.15, dyed 1.02-1.12, a colorSource mix 0.83-1.05, agouti_fur 0.85-1.03,
  tipped_fur 1.24-1.27, wool 1.20-1.22 of Arnold's average; no highlight.
- **A specular below 0.2** (Arnold tint under 2/3): Principled Hair's own highlight cannot be
  dimmed, so the hair is a little shinier than Arnold's (wool, specular 0.1: 8% brighter).
- **Iridescence** tints Arnold's highlight with the structural color too; Principled Hair's
  highlight stays white (feathers_iridescent: red 25% brighter).
- The fit is Arnold 7.0's standard_hair (Maya 2022's MtoA) at roughness 0.15-0.45 (HairColor
  roughness 0.1-0.9); outside that the formulas extrapolate.
- Blender's default view transform (AgX) is not the PNG kick writes: compare with Standard (or the
  same OCIO view on both sides). The comparisons used Cycles' Light Paths with 16 transmission
  bounces; Blender's defaults (12) rendered the same within 1% here.

Tools for the comparison (not shipped): render the same `.groom` with kick through
`groomlab_procedural` with create_hair_shader's network and the scene above, and in Blender
headless; compare the EXRs.

## 7. B3c: the Graph Editor

Maya's GroomLab Graph Editor (hosts/maya/scripts/groomlab/ui/graph_editor.py) in Blender's node
editor: a custom `NodeTree` type, **GroomLab Graph** (`GroomLabGraphTree`), one tree per groom
(`<scalp> Graph`, a fake user so the .blend keeps it; `gl_scalp` points at its groom). The 3D
view's Graph panel keeps its compact list and opens the editor (**Graph Editor**: a node editor
in a new window); choosing a groom (the scene's `groomlab.groom`, in either place) shows its tree
in the open GroomLab node editors, and picking another groom's tree in an editor makes it the
scene's groom.

**Nodes.** One node class per operator (`GroomLabNode_<Type>`), made from `operator_specs` when
the helper first answers and made again when its operators change: ports typed by port type,
one socket class per type in Maya's wire colors (Strands, Guides, Mesh, Field, Curve; Curves,
"strands or guides", grey, a first output of Curves shown as what feeds it); an exposed param's
port is a diamond. The operator's params are the node class's own properties, so each has its
spec's range (soft range: the slider), unit (lengths `DISTANCE`, scene units; fractions `FACTOR`;
colors `COLOR`), choices (an enum) and description (the tooltip). A Compound's (or Input's) ports
and params are its own (`node_spec`): its exposed params are a list of the 3D panel's widgets on
the node. Ramps are drawn with Blender's curve widget: a Float Curve node per ramp in a hidden
shader node group (`.GroomLab Ramps`), points and handle types (all Vector: `"linear"`) read back
into the ramp. Node bodies carry their category's color (Blender draws a custom node's header in
its theme's color: the category tint is on the body, with Maya's accent bar drawn over the header
by the editor's overlay, beside the error "!", note "i", View eye and evaluation times).

**Two-way sync.** The tree mirrors the helper's graph; node names are the graph's.
- Tree to helper (`NodeTree.update`, after any edit of the tree): the tree's structure (nodes,
  names, mutes, links) against the last one sent; a change becomes `edit_graph` ops: a new node
  (`gl_name` empty: the Add menu, a duplicate or paste, whose `copy()` names the source to copy
  params from) `add_node` at its position, a missing one `remove_node`, a renamed one
  `rename_node`, links `connect`/`disconnect` (every link, even before Blender validates it), a
  mute `set_bypass` (Bypass, as in Maya and the app). Then the groom evaluates and the tree is rebuilt from the
  graph (a timer: never inside the update), which drops what the graph refused.
- A node property's update is a `set_param` (scene units to centimeters); a Compound param's
  widget the same.
- Helper to tree: every change of the graph (`graph.refresh`: the 3D panel's edits, presets, a
  `.groom` opened, Update) rebuilds the tree in place: nodes kept by name and type, ports made
  again only when they changed, values set quietly, only the links that differ removed or made.
- Undo: Blender's undo restores the tree and the groom's graph text (`graph_json`); `undo_post`
  gives the helper that graph, with the tree's positions, and rebuilds.
- While node types are not registered (a .blend just opened) the tree is never sent: it is made
  again from the groom.

**Positions** are the graph's, as Maya and the app keep them: each node's `"ui": {"x", "y"}`,
Qt's y down, so a node's Blender location is `(x, -y)`. A move is not an edit: the nodes the user
moved since the tree last had them (`gl_at`) go to the helper (`edit_graph set_positions`, which
works on a graph that does not load yet) before the next edit, preset, save or `.blend` save;
nodes not moved take the graph's positions on a rebuild (an inserted preset moves the nodes after
it over). A preset applied or a file opened lays the tree out as its graph is; a graph without a
layout (an old file) is laid out in Maya's columns (`auto_layout`) and the layout written back.

**The sidebar** (N): the **Node** tab is Maya's param panel for the active node: its name (edit to
rename), type and description, **Upgrade** when the node is outdated (node_spec's version below
the operator's), **Update** when the library has a newer copy of a library node, its note and
error, then the params by section (`param_sections`: the main ones, each group when there are two
or more, Advanced last), folded per operator type for the session (Advanced folded by default),
`showWhen`, plumbing params hidden unless changed, an exposed param linked to a field shown with
its driver, and at the bottom the node menu. Each param row ends with its **param menu** (the ⌄
button: `graph/paramMenu`) and, for a mappable param, the map source button. A selected frame
shows the backdrop's title, colors and menu. The **GroomLab** tab has the groom, its status and the
presets browser. The header: the groom, **Edit** (`graph/editMenu`), **Presets**
(`graph/presetMenu`: the presets by category, each Apply or Insert After Selected Node; Save as
Preset...; Copy Graph; Paste Graph) and a **View** chip while a node is viewed.

**Menus from the UI definitions** (`graph_ui.py`): the node menu (right-click, and the sidebar),
the backdrop menu, Edit, Add, Presets and the param menu are drawn from `engine/ui/graph_ui.json`
as Maya's are (the extension's `ui/` folder; a source tree's `engine/ui`), resolved for the host
`blender` (`hosts`, `needs`, `enabled`, generators: `choices`, `backdropColors`, `exposeParams`,
`innerParams`, `presets`; `units`, `themeMenu` and `nodeCategories` give nothing here). Each action
id is bound to an operator (`graph_ui.bind`, in graph_editor.py and graph_authoring.py), whose
tooltip is the action's tip (`ActionTip`); submenus go through one generic menu
(`GROOMLAB_MT_graph_submenu`, the entries prepared when the parent draws). Ids with no Blender
counterpart are left out (`NOT_IN_BLENDER`: the GPU preview, Bake Node to Guides, painting a param
on the guides); the test checks every other id has a binding. The category, port and backdrop
colors and the plumbing params come from the same file; the Add menu shows each category with a
swatch of its color, then New Script Node, Import Node..., Open Nodes Folder (`graph/addMenu`).

**Authoring** (graph_authoring.py, over the helper's "Authoring" commands above):
- **Compounds**: Group into Compound (Ctrl+G, `edit_graph group`), Ungroup (Ctrl+Shift+G), Show
  Inner Param (`compound_expose` / `compound_unexpose`, named as Maya names it). Blender's own node
  groups are not used: a compound is one node, as in Maya's editor, which does not enter compounds
  either; ungroup to edit its nodes (Ctrl+Z groups them again).
- **Exposed params** (`expose_param` / `unexpose_param`), **Drive with Slider** (`add_control`) and
  **keyed params** (`link_control` / `unlink_control`). A control's value lives on the groom's
  scalp object as a custom property `ctl_<control>` (Maya's `ctl_` attributes on the groom node):
  Set Key links the param and keys the property(ies) at the current frame (`ctl_<node>_<param>`,
  three for a vector), Delete Key removes the key, Remove Animation removes the curve and unlinks
  (the value now goes back into the param). Blender evaluates the keys; a `frame_change_post` and
  `depsgraph_update_post` handler gives the properties that changed to `set_controls`. A keyed
  param's row shows its property (key it with I like any property); a Control node's panel shows
  its slider.
- **Map sources**: Map Source (texture, color set, attribute, painted map, expression; channel,
  min, max) is a `set_param` with a map value (checked by the helper), Clear sets the default back.
  An expression is edited in a text block `GroomLab <node>.<param>.expr`; a timer (while such text
  blocks exist) applies it when it changed, the helper's error kept and shown under the param.
- **Backdrops**: Blender frames (`NodeFrame`). A frame's label, custom color (sRGB 0-255),
  absolute location and width/height are a backdrop `{title, x, y, w, h, color}` (y down, as the
  nodes). They go with the positions (`push_positions`: frames made, deleted, moved, resized,
  renamed or recolored since the tree last had them, `tree["gl_backdrops"]`, all of them via
  `set_positions backdrops`); a rebuild makes one frame per graph backdrop (`shrink` off) and
  parents the nodes inside each, so moving it moves them. Node positions use `location_absolute`.
- **Pins**: Blender reroutes (`NodeReroute`), the reroute points Maya's editor draws links
  through (the graph's `pins`: `{points, routes}`, layout only). A link through reroutes is the
  plain connection from the GroomLab output they lead back to into its input, which evaluates as
  a direct link; the reroutes it passes are its route (`via`, source first), each reroute a pin
  (`gl_pin`, `pin<N>`). One reroute feeding several inputs is a bundle: one pin in several routes.
  Shift+right-drag across a link puts a reroute on it (the same connection: only the pins change,
  nothing evaluates, no undo step); dragging from a reroute's output links that signal on; plugging
  another output into a reroute feeds every link after it from there (connections, through
  `edit_graph` like any link; refused by one of them: the tree goes back to the graph, the reroute
  fed as it was). X on a reroute removes the links through it (Blender's), Ctrl+X straightens them.
  A reroute that routes no link (no input, or no link out) is no pin and stays as the user left it,
  links into it too, until it routes one; a pin the graph no longer has (its link removed or
  rewired elsewhere) goes on the next rebuild. Pins go with the positions as backdrops do
  (`push_positions`, `tree["gl_pins"]`, all of them via `set_positions pins`); a rebuild makes a
  reroute per pin the graph's routes pass (routes whose link is gone or comes from elsewhere, and
  those Blender cannot draw, a pin fed from two places, left out) and parents those inside a frame.
- **Node library**: Save as Node... (`save_node`), Save to Library (the same id, one version past
  the node's and the library's), Update Node (`edit_graph update_library_node`), Import Node...
  (`import_node`, then the node added), Open Nodes Folder (`nodes_folder`), Save as Preset...
  (`save_preset`). A library node's `node_spec` "library" is kept on the node (`gl_library_info`):
  the overlay draws Maya's ↑ mark on its left corner when `updateAvailable`.
- **Script nodes**: New Script Node (`new_script_node`, after the selected node), Edit Node...:
  the file in two text blocks, `GroomLab <node>.glscript` (the script) and `GroomLab
  <node>.header.json` (the rest); checked when they change (`script_errors`) and taken by the node
  when they check (`set_script`), the errors with their line in the sidebar; Save to Library / Save
  as New (`save_script_node`); Done removes the text blocks.
- **Copy Graph / Paste Graph**: the text is Maya's `copy_graph_text` (the preset Save as Preset
  writes, from `get_graph`, with the scalp's `size` from `get_scalp` in groom space, `about`, the
  backdrops and the set control values); on Blender's clipboard. Paste reads a preset's JSON (a
  chat's code fence taken off, a `.groom`'s graph too) and applies it as a preset from a temporary
  file (`apply_preset path`: fitted to this scalp, its controls), then its backdrops.
- **A node dropped on a link**: Blender splices it in; `sync_from_tree` sees the link a -> b.input
  replaced by a -> node -> b.input (`insertions`) and sends `insert_on_link` instead (one step on
  the helper's stack, the node's input picked as Maya picks it).
- **Tab search's painted maps**: the Add menu's **Painted Maps** category (`maps_list`) has a
  `Map: <name>` entry per map, a Map node whose `gl_init` gives its name and params, so Tab's
  search lists them as Maya's MAP_ENTRY does.
- After the helper's undo or redo (Groom Tools' Ctrl+Z) an answer with `graphChanged` refreshes
  the node list and rebuilds the tree (`tools.follow_graph`); Blender's own undo in the node editor
  restores the graph text as before (`after_undo`).

**Keys**, in GroomLab graphs only (each operator's poll; in another kind of node editor Blender's
own): Tab add (search; the Add menu is the core's categories, each node's description its
tooltip), F frame the selection or all, V View (Focus), B Bypass, Ctrl+D duplicate, Ctrl+G group,
Ctrl+Shift+G ungroup; Blender's Shift+A, X/Delete, Shift+D, M, F2, Ctrl+J and Home work as ever.

What differs from Maya's editor (Blender's node editor): the header color is the theme's (the
category is the body tint and the accent bar); a "strands or guides" port is grey, not half and
half; compounds are not entered (as in Maya); Tab over a wire does not insert (drop the node on
it); keys are Blender's (I over a property), not S; the GPU drag preview is not in Blender; the A key keeps Blender's select-all (Home
frames all); no Live/Apply switch (every edit applies).

## 8. B3b: the Groom Tools

Maya's Groom Tools in Blender, driving the helper's tools (section 3, "Tools"): the user chose
GroomLab's own brushes (not Blender's Curves Sculpt brushes) and Maya's palette, viewport
display, menu and keys.

| Module | What (Maya's original) |
|---|---|
| `tools.py` | the logic, no operators: the catalog, the palette's settings as `tool_set`'s (each tool's own strength, the catalog's default the first time; unsigned tools clamped at 0; the Group as level and group), the view from a region and region_3d (`view_matrix`, `window_matrix`, the scalp's `matrix_world`, the region's size, `view_location` in the scalp's space), hover, strokes (`stroke_drag` batched per mouse move, a live `evaluate {sculptDensity: 0.35}` at most every 40 ms or 1.5 x the last one's own time, the viewport alone redrawn on hover and drags, a full evaluation after `stroke_end`), selection, undo/redo, guides_edit, maps; the palette's lists (groups, maps, counts, undo labels) after each edit; the groom's state (`get_state {scalp: "maps"}`) 1 s after the edits pause (at once headless), in the .blend before a save, given back to a new helper (palette.py, hotkeys.py' step_level) |
| `tool_props.py` | `Scene.groomlab_tools` (the palette's settings by the catalog's labels and hints; saved with the .blend as Maya keeps them in optionVars), `Object.groomlab_tools` (the groom's display, Maya's groom attributes; its kept state), the add-on preferences (the pie's key) |
| `tool_ops.py` | the tool (`groomlab.tool` picks one, `groomlab.tool_modal` runs it in the viewport: `ToolSession` turns events into the helper's commands), undo, groups, guide edits, maps, the pie menu `GROOMLAB_MT_pie` and its keymap item (hotkeys.py, tool_tabs.py) |
| `display.py` | `display` into draw lists (numpy) and gpu batches, a POST_VIEW handler; strand color modes as the hair's `groomlab_display` color attribute (groom_draw_override.cpp) |
| `palette.py` | the sidebar's **Groom Tools** panel (Tools, Paint, Correctives and Photos tabs; Photos is `photos.py`'s, section 3 "Photos") and its folded subpanels in palette.json's order: Tubes, While sculpting, Guide groups & edits, Output, then Display (palette.py, tool_tabs.py) |
| `menu.py` | every menu the UI definitions describe, built from them: the top bar's **GroomLab** menu (menus.json "main"), the Groom Tools' More menu, Output and empty-state submenus, the lists' right-click menus (Blender's list item context menu, and a button beside each list); the shortcuts, the About popup (menu.py) |
| `definitions.py`, `actions.py` | the shared UI definitions (`engine/ui/*.json`, docs/UI_DEFINITIONS.md) read with no helper running, as Maya's definitions.py (host "blender"); `actions.BINDINGS` maps each action id to an operator and its properties. An id without a binding is hidden; `blender.addon_ui` lists the ones left (its ALLOWED_MISSING: the parity checklist) |
| `icons.py`, `icons/*.png` | Maya's icons (hosts/maya/icons/*.svg) as previews; the PNGs are rendered by `tools/make_blender_icons.py` (Pillow, the Playful theme's colors as widgets.icon gives them) and committed |

**Grooms.** Create Groom (`groomlab.add_groom`) makes the groom Maya's `create_groom` makes:
`new_groom` (the preset's graph fitted, no guides), the guides in the helper's hierarchy, no
Blender guides object (`guide_target = "hierarchy"`); its optional starter guides go into a
"primary" group (`set_guides target hierarchy` after `guides_edit add_group`). B2's Blender-curves
guides stay for grooms made before and for `.groom` files with flat guides (Maya's guide curves);
`groom.create_groom(..., guides="flat")` makes one (the B2 tests). No migration (nothing was
released with them). A `.groom` without flat guides opens as a hierarchy groom.

**State.** After the committed edits pause (1 s; not during a stroke; at once headless)
`get_state {scalp: "maps"}` is kept in memory (a helper that ends gives nothing back; one that
crashes within that second loses the last edit), written into `Object.groomlab_tools.state` before a save (`groom.before_save`);
`groom.build_hooks` give a new session (a restarted helper, a `.blend` opened) its groom:
`set_scalp`, then `set_state {keepScalp: true}`, then the graph as B2 keeps it. After a file opens
(not headless) the hierarchy grooms are evaluated once, so their guides draw at once.

**The tool in the viewport.** A modal operator for as long as a GroomLab tool is active (Maya's
tool context): window-level, so it sees events first and passes on what is not over a 3D view's
WINDOW region (headers, toolbar, sidebar and HUD over it are excluded; quad views map to their own
region_3d), navigation (middle mouse, wheel, numpad, NDOF) always. Mouse moves hover (the ring);
LMB strokes with `event.pressure`, Ctrl and Shift as Maya; B / M (and F, Shift+F) held + drag set the
radius / strength; S the Select tool (drag: lasso or box; click: one guide; Shift toggles, Ctrl
deselects); Page Up / Down the level; Y the pie; Ctrl+Z / Ctrl+Shift+Z over a viewport the helper's
undo; Esc or right-click cancel a live stroke, Esc again leaves. The status bar says the keys.
Blender's own undo stays Blender's (the helper's stack is separate, as section 3 says).

**The pie** (`wm.call_menu_pie`, Object Mode keymap, Y as Maya's marking menu key; the
preferences show the item to rebind): the radial positions of hotkeys.py's RADIAL, Blender
laying them out W, E, S, N, NW, NE, SW, SE; the overflow (the rest of the catalog, then Select,
Freeze and Groom Tools) is a list under Place in the south slot, where Maya's overflow hangs
under the marking menu.

**Display.** One `display` per redraw when something changed (an edit marks the groom; a new
evaluation's serial; a display setting), with `have` the versions held and the generation, so
only changed items come; their arrays are copied at once. Drawn in Maya's layer order (the scalp
map and tubes, guides, flow field, selection, soft selection, parts, flow arrows, dots), depth
tested against the scene but pulled toward the camera by the answer's `overlayPull` (the
projection nudged in NDC depth by `|P23| * pull / z²`), the brush on top. Colors: the Playful
theme's viewport colors (`theme`), sRGB as Blender's overlays take them. Tubes are lit by a fixed
light (Blender's builtin shaders). Strand colors cannot be written while drawing (Blender's data
is read-only there): a timer writes them.

Deviations Blender forces:

- The palette is a sidebar panel, not a floating dockable window; tiles are icon + label buttons
  (Blender buttons cannot put the label under the icon); the More menu has no theme menu.
- No middle-drag radius/strength (Blender's view rotation): B / M + drag only, as Maya's hotkeys.
- Blender postpones its autosave while a modal operator runs: the tool's does too; leave the tool
  (Esc) now and then in long sessions, or save.
- Not here: the
  Segments resample slider, the parts list (Part's Remove / From Curves) and Flow's Clear All
  (no helper command yet); Fill works on guide maps only.
- "Strands dimmed" while sculpting is not offered (Blender has no per-object viewport dimming);
  strand colors show in Solid shading through the active color attribute (Show in Viewport sets
  the shading to Attribute).
