<!-- SPDX-License-Identifier: Apache-2.0 -->
# Custom nodes: design

Status: phase 1 (compound nodes) and phase 2 (script nodes) built, 2026-10-18; plugins are a
proposal. The sections on the node file, the library, grooms carrying their nodes and editing
describe what phases 1 and 2 do.

## Goal

Anyone can make a node of their own, share it as one file, and anyone else can drop that file into
their library and use it like a built-in node, **without compiling anything**.

A custom node must work wherever a groom is evaluated, not only in Maya:

- Maya, the standalone app, Blender (through its helper);
- the Arnold and RenderMan procedurals, on a render farm with no Maya or Python;
- groomlab-cli, the C API, Linux.

So a custom node cannot depend on Maya's Python, and because people will share nodes with
strangers, running one must be safe: no file or network access, nothing that can hang a render.
It must also be deterministic (the same bits on any thread count and platform), as every node is.

## Where we start from

A built-in node today is a C++ class compiled into the core: it declares ports, a list of
`ParamSpec`s and one `evaluate()`, and pulls its math from the shared library (docs/MATH.md).
Everything else comes from the specs automatically: the UI in every host, saving, the Python and
C APIs, Add Node search, value ports, paintable maps, unit scaling. `tools/dev/new_node.py` makes
a new one (docs/EXTENDING.md). That stays the way to add heavy, core features.

Two things we already have make custom nodes cheap:

- **Compounds** (Ctrl+G) with **value ports**: a subgraph with its params exposed, stored in the
  groom as a `definition` (graph JSON) on a Compound node.
- **The expression language** (docs: Expressions): a sandboxed, deterministic language with
  variables (`P`, `N`, `t`, `length`, channels...), vectors, `if`/`else`, assignments, ~60 functions
  (noise, fbm, voronoi, curves, `map()`), already evaluated in the core, partly on the GPU. Today
  it only produces a *field* (a value per strand or point that drives a param) or a single value.

## Three kinds of custom node

| Kind | Made of | Can do | Speed | Status |
| --- | --- | --- | --- | --- |
| **Compound node** | Built-in nodes (a saved compound) | Anything a graph of nodes can | As the nodes inside | Phase 1 (built) |
| **Script node** | A script in the expression language, strands in and out | Move points, set widths and colors, write channels, per strand and per point | An interpreter over strands in parallel; CPU | Phase 2 (built) |
| **Compiled plugin** | WebAssembly, compiled once by its author | Heavy algorithms | Near native | Later, if needed |

### 1. Compound nodes (no code)

**Save as Node...** on a compound (Graph Editor right-click, both hosts) writes it to the library:
its subgraph, the params you exposed (with your labels, ranges, defaults and descriptions), a
name, a category and an icon. It then appears in Add Node and Tab search under its category (or
**My Nodes**), and behaves as one node.

This is mostly plumbing on what exists: a compound's definition plus a header.

### 2. Script nodes (custom logic)

A script node takes strands (and, optionally, guides), declares params in its header and runs a
**script** per strand and per point (the user docs: web/content/docs/custom-nodes.mdx, Script
nodes; the code: `script.h`, `script_program.h`, `expression.cpp`'s script mode, `script.cpp`,
`operators/script_op.cpp`):

```
# Wave: bends each strand sideways in a wave along its length.
per strand {
    $dir = normalize(tip - root);
    $ref = abs($dir.y) < 0.9 ? [0, 1, 0] : [1, 0, 0];
    $side = rotate(normalize(cross($dir, $ref)), $dir, radians($angle));
    $start = (rand + $phase) * 6.2832;
}
per point {
    P += $side * $amplitude * sin(t * $frequency * 6.2832 + $start) * t;
}
```

The language is the expression language (every expression keeps compiling and evaluating to the
same bits: the goldens and PresetDigests are unchanged), with a script mode in its parser:

- **Blocks:** `per strand { }` once per strand, then `per point { }` for each point root to tip;
  either may be left out, the strand block first. Statements: assignments (`=`, `+=`... and one
  component, `P.y = ...;`), `if`/`else if`/`else` (jumps: only the branch taken runs) and `for`.
- **Writable outputs**, each only when the header's `writes` lists it (else an error with line and
  column): `P` (per point only), `width` and `color` (per point; in the strand block every point's,
  which the point block then reads: written as `colorR/G/B` point channels and the strand's colorR/G/B,
  HairColor's), `channel.<name>` (a strand channel when assigned in the strand block, a point channel
  in the point block; one channel in one block; it must match a channel of that name coming in). Each
  reads as it is now (the incoming value until assigned).
- **Inputs:** everything an expression reads (`t`, `pt`, `rand`, `id`, `u`, `v`, `N`, `length`,
  `index`, channels by name, `frame`, `time`, `map()`...), plus `tangent` (as the strand came in),
  `prev.P` (the point before as this run left it; the root's own at the root), `root`, `tip`,
  `pointCount`, `pointIndex`, `pointP` (the point as it came in) and `$param` for every header
  param (a mappable number read per strand or per point through `ScalarField`, as a built-in
  node's; a ramp as `$name(x)`; an enum as its index; a color as a vector). Functions:
  `strandPoint(s)`, `guidePoint(s)` (the optional guides input's guide rooted nearest the strand,
  found with a `PointIndex`; the strand's own point without guides), `scalpNormal(p)`,
  `closestOnScalp(p)` (the scalp's BVH). Fill Tubes' `tubeU`/`tubeV` are channels, read by name.
- **Locals** live in slots: 0 at each strand's start, kept from point to point (a running value
  root to tip); a strand block's local reads in the point block. A local's type is fixed by its
  first assignment. Locals may hide an expression variable (as in expressions) but not a script
  input (`P`, `width`, `tangent`...); params are read only.
- **Bounded loops only:** `for $i in A .. B { }` runs `$i` from A to B - 1; each bound a whole
  number written out (at most 100000) or a whole-number param with a max (read up to the max).
  Nested loops are fine; there is no recursion, file, clock or network access.
- **A step budget** of 10000 interpreter steps per block run (one strand's, one point's): past it
  the run stops and the node errors ("the per point block ran past its budget..."), never a hang.

What a script node cannot do: add or remove strands or points, or read other strands (neighbours:
see the open questions). Not built yet: the GPU preview (a graph ending in a Script node is not a
GPU chain, `findLookChain` says "Script, which the GPU does not run", and drags and brushing over
it evaluate on the CPU), editing params in the app's editor, editing in Blender.

**How it runs:** `script::compile` makes the script two lists of the expression ops (one per
block) with five ops of the script's own (`Load`, `Store`, `Jump`, `JumpIfZero`, `ScriptCall`) and
a slot layout (built-in inputs and outputs, params, written channels, locals); the expression
ops run through the same handlers (`applyOp`). The Script operator (graph type `Script`, its
`definition` param the node file) compiles once per definition (again per evaluation only when the
script reads the time or a map, whose values bind at compile time), clones its input, runs strands
side by side (`parallelFor`, each strand alone: the same bits on any thread count) and writes only
what `writes` names: a script without P hands on its input's very points, so the graph sees
`pointsKept` (as after a Width edit). `evaluateIncremental` makes again only the strands its input
changed in (not when a mappable param or the script reads `longest`, a whole-set value, or the
guides changed). A definition that cannot be read or compiled (or of a newer format) hands its
strands on with a note "Error: script node '<label>' cannot run...": a groom never fails to open by
it. Script params get the node's `affect` mask like any strands-to-strands node.

### 3. Compiled plugins, later

For a heavy algorithm that would be slow in a script (a custom solver, a mesh-to-hair method),
a **WebAssembly** module rather than a DLL: authors compile once (C++, Rust, Zig...) to one `.wasm`
file that runs on every platform and host, sandboxed, through a small versioned API (read strands
and params, write strands). A DLL would break across compilers and GroomLab versions and could do
anything to a machine, a poor fit for sharing. Only worth building if phases 1 and 2 leave real
gaps.

## The node file

One file per node, `<id>.glnode` (JSON, UTF-8), readable and diffable. Format 1, as phase 1 reads
and writes it (`engine/core/include/groomlab/node_library.h`; the shipped examples are in
`engine/assets/nodes/`):

```json
{
  "glnode": 1,
  "id": "nick.soft_frizz",
  "version": 2,
  "label": "Soft Frizz",
  "category": "look",
  "description": "A few strands kinked toward the tips and a gentle wave through the rest.",
  "author": "Nick Rutlinh",
  "license": "CC0-1.0",
  "icon": "",
  "size": 20,
  "inputs": [{"name": "strands", "type": "Strands"}],
  "output": {"name": "out", "type": "Curves"},
  "params": [
    {"name": "amount", "target": "frizz.amplitude", "label": "Frizz", "default": 0.12,
     "min": 0, "softMax": 0.5, "unit": "length", "mappable": true,
     "description": "How far the frizzy strands kink, in scene units"}
  ],
  "graph": {"version": 1, "nodes": [...], "connections": [...], "output": "wave"}
}
```

| Field | Required | Meaning |
| --- | --- | --- |
| `glnode` | yes | The format, a whole number. A file of a newer format than the build reads is refused ("update GroomLab"). |
| `id` | yes | Two or more parts of `a-z 0-9 _ -` joined by dots (`studio.hair.soft_frizz`). Never changes once shared. The file is named `<id>.glnode`. |
| `version` | yes | A whole number from 1; goes up with each save of the same id. |
| `label` | yes | What the menus and the node card show. |
| `category` | no | A built-in category's id or title (any case: `look`, `Look`, `Fields & Curves`) lists the node there; other text is a category of its own (after the built-in ones); none lists it under **My Nodes**. |
| `description`, `author`, `license`, `icon` | no | Text. The license is free-form. The icon is not shown yet. |
| `size` | no | The scalp size it was made on (presetScalpSize, scene units). Added to a groom, the node is fitted from it to the groom's scalp as a preset is (lengths, widths, densities, per-length rates inside; each param's default and range with its inner value). |
| `inputs` | no | The compound's inputs, `{"name", "type", "optional"}` (types as ports: Strands, Guides, Curves, Mesh, Field, Curve, Value). Each `Input` node inside stands for one of them. None: a node with no inputs. |
| `output` | no | `{"name", "type"}`: the graph's output node's type, written by Save as Node and checked on load (a Curves graph may say Strands or Guides). |
| `params` | no | The params the node shows, in order. `name` (a word), `target` (the inner `node.param` it sets), and optional `label`, `default` (set on the inner param when the file loads; checked against its kind), `min`/`max`/`softMin`/`softMax`, `unit`, `mappable`, `description`. |
| `graph` | yes | The compound's subgraph in graph_io.h's schema (every param written, node layout in `ui`). |

`checkNodeFile` checks a file and says what is wrong and where (`params[2]: the graph has no node
'x'`, `graph: unknown operator type 'Y'`, `inputs: ...`): not JSON, a newer format, a bad id or
version, no label, a field of the wrong kind, a graph that does not load, an Input node standing
for an input not listed, a param named like the node's own (`definition`, `repeat`) or an input, a
target the graph lacks, a default of the wrong kind, an output type that is not the graph's.

### Script node files

A script node has `"writes"` and `"script"` in place of `"graph"`, and its params are specs of
their own rather than targets (`node_library.h`: `scriptNodeHeader`, `scriptNodeErrorsJson`,
`scriptNodeTemplate`):

```json
{
  "glnode": 1,
  "id": "groomlab.wave",
  "version": 1,
  "label": "Wave",
  "category": "Scripts",
  "description": "Bends each strand sideways in a wave along its length...",
  "author": "GroomLab",
  "license": "Apache-2.0",
  "size": 20,
  "inputs": [{"name": "strands", "type": "Strands"}],
  "output": {"name": "out", "type": "Strands"},
  "params": [
    {"name": "amplitude", "type": "length", "label": "Amplitude", "default": 0.4, "min": 0,
     "softMax": 2, "mappable": true, "description": "How far the wave swings at the tip"},
    {"name": "frequency", "type": "number", "label": "Waves", "default": 3, "min": 0, "softMax": 10}
  ],
  "writes": ["P"],
  "script": "# Wave: ...\nper strand { ... }\nper point { ... }\n"
}
```

| Field | Required | Meaning |
| --- | --- | --- |
| `inputs` | no | Strands first (`{"name": "strands", "type": "Strands"}`, the default); then at most one Guides input (for `guidePoint`, optional). |
| `output` | no | `{"name", "type": "Strands"}`. |
| `params` | no | Each: `name` (a word; not a name scripts read already, such as `length`, `width`, `t`, nor a script function's, `definition`, `affect` or an input's), `type`: `number` (with an optional `unit`: length, width, fraction, angle, perLength, density...), `length`, `int`, `bool`, `enum` (with `choices`, a list of text; the script reads the index), `color` (`[r, g, b]`) or `ramp` (`[[position, value], ...]`, read as `$name(x)`); `default` of that kind (absent: 0, false, the first choice, white, a 0-1 ramp); `min`/`max`/`softMin`/`softMax` (number, length, int; an int's `max` lets it bound a loop); `mappable` (number, length); `label`; `description`. Each becomes a `ParamSpec` (`"(mappable)"` added to its description). With `size`, a node added from the library has its length (and width, perLength, density) params' defaults and ranges fitted to the groom's scalp. |
| `writes` | no | What the script may assign: `"P"`, `"width"`, `"color"`, `"channel.<name>"`. |
| `script` | yes | The script (text). |

`checkNodeFile` checks a script node's file as it is (its header, then its script compiled: a
script error reads `script: <message> (line 3, column 9)`). `scriptNodeErrorsJson` gives an editor
the first error as `{"part": "json"|"header"|"script", "message", "line", "column"}` (line and column
in the script; 0 for the header). The library lists a script node with `"kind": "script"`; a node
added from it is a **Script** node (graph type `Script`) holding the file as its `definition`
(every header param's default written into the node's params, as `completeNode` writes every
node's).

## The library

- **Where:** every folder on `GROOMLAB_NODE_PATH` (the OS path separator between them), then the
  user's folder (`GROOMLAB_USER_NODES`, else `Documents/GroomLab/nodes`: the profile's on Windows,
  `$HOME`'s elsewhere; shared by Maya, the app and Blender; made by the first save), then the folder
  shipped with the host (`setShippedNodeDir`: Maya's module `nodes/`, the app's `nodes/` beside its
  executable, the Blender helper's `bin/nodes`; all from `engine/assets/nodes`). An earlier folder hides
  a later one with the same `id`. The CLI and the render procedurals set no folder: grooms carry
  their nodes.
- **The core does the work** (`node_library.h`), every host the same: `libraryNodes()` (cached, read
  again when a folder's files change and as an Add Node menu opens: `libraryNodesJson()`, which also
  lists the files that cannot be used, with why), `nodeFileFromCompound`, `saveNodeFile`,
  `importNodeFile`, `GraphEdit::addNode("node:<id>")`, `GraphEdit::updateLibraryNode`.
- **Install:** drop a `.glnode` into a folder, or **Import Node...** (the bottom of Maya's Add Node
  menu, the app's canvas menu), or drag the file onto the Graph Editor (both hosts).
- **Share:** send the file. A **Node Library** page on the website, later, could list community
  nodes (moderation and trust are a later question).

## Grooms carry their nodes

A node added from the library (`GraphEdit::addNode` with type `node:<id>`) is a **Compound** whose
`definition` is the checked file: the file's fields as they are, plus the `expose` map its params'
targets make, the params' defaults set on the inner nodes and the file fitted to the groom's scalp
(`size`). Every existing compound tool works on it (Ungroup, the inner cache, value ports), the
groom stores it as compounds are stored, and every host and renderer that reads grooms already
evaluates it: no library is read to open or render a groom.

- **Shown as itself:** `nodeSpecJson` gives such a node its `label` and `description`, and
  `"library": {"id", "version", "latest", "updateAvailable", "embedded", "category", "author",
  "license", "icon"}`; its params take the header's labels, descriptions and ranges.
- **Update:** when the library has a higher `version` of the id, `updateAvailable` is true: Maya's
  card shows an arrow on its corner and its panel **Update to vN**, the app's card the arrow and
  right-click **Update Node**. `updateLibraryNode` takes the library's copy (one undo step in each
  host): the exposed params it still has keep their values, links into inputs it no longer has go.
  Never automatic.
- **Graceful:** a library node whose copy cannot run (an operator type this build lacks inside, or
  none embedded: a graph naming `"type": "node:<id>"` with no copy and no library file) hands its
  first input on, with a note starting "Error: library node '<id>' cannot run..." saying why; its
  ports come from its definition (else one Strands input). A groom never fails to open by it. A
  compound of the user's own with a broken definition stays an error, as before.
- A graph may name a node by type, `"type": "node:<id>"` (hand-written graphs): loading takes the
  library's copy then (or the pass-through above).

## Editing

- The node's params show like any built-in node's (from its header).
- **Save as Node...** (right-click a Compound, or its panel's button) opens a form in the side panel
  (Maya) or over the graph (the app), never a dialog: label, category (the built-in ones and those
  in the library, or typed), description, author, id (follows label and author until typed),
  version (the library's next), license, and which exposed params to keep (Maya; all by default:
  one left out keeps its value inside). Saving writes `<id>.glnode` to the user's folder and the
  compound takes the saved copy (`updateLibraryNode` with that id), one undo step.
- **Show Inner Param** (Maya, right-click a Compound) exposes or hides an inner node's param on the
  compound: what Save as Node keeps.
- Neither host has a view inside a compound (double-click ungroups in the app, does nothing in
  Maya), so **Save to Library** is on the library node itself (right-click, or its panel): the node
  as it is now, its values the new defaults, written one version up. To change what is inside,
  Ungroup, edit, Group again and Save as Node with the same id (the version counts up).
- Blender lists the library in **Shift+A > Compound Nodes** and adds and evaluates its nodes
  through the helper (`library_nodes`, `edit_graph` `add_node` with a `node:<id>` type and
  `update_library_node`); saving a compound as a node is Maya's and the app's for now.
- Script nodes: **New Script Node** (Maya's Add Node menu, the app's canvas menu) adds one from
  `scriptNodeTemplate`, wired after the selected node, and opens its editor. **Edit Node...**
  (right-click, its panel; double-click in the app) opens it again. Maya's editor is in the Graph
  Editor's side panel (`groomlab/ui/script_editor.py`, never a dialog): the header as a form
  (label, category, description, author, id, version, writes), the params as a table (name, type,
  default, min, max, mappable, label, choices; add and remove), the script in a code editor (line
  numbers, highlighting, completion) and the errors by line and column (the line tinted, a click
  goes there). It is live: a change is checked 300 ms after the last keystroke
  (`script_node_errors`) and, when it checks, the node takes it (params the header no longer has
  go; a param at its old default takes the new one) and the groom applies; the panel stays while
  the node is edited. **Save to Library** writes the file one version past the library's; **Save as
  New** writes it under the header's id, refused when the library has it. The app's editor is a
  glass panel over the graph with the label, writes and script (live, errors by line and column,
  the line tinted) and the same two saves; params are edited in Maya for now. Blender lists script
  nodes in **Shift+A > Script Nodes** and evaluates them; editing there is later.

## Rules that keep it safe and stable

- **Sandboxed:** scripts reach only their inputs, params, maps and the math library.
- **Bounded:** literal loop bounds and a step budget; an over-budget node errors, never hangs.
- **Deterministic:** the portable math, no clocks, no unordered sums across strands.
- **Versioned:** `glnode` (the file format) and the language have versions; GroomLab reads every
  older one. New functions may be added; existing ones never change meaning.
- **Graceful:** a node file that fails to load becomes a pass-through node with an error saying
  why, so a groom never fails to open because of it.

## Phases

1. **Compound nodes (built):** the `.glnode` format, the library folders, Save as Node..., Add Node
   listing, grooms embedding their nodes, update marks. Maya and the app; Blender lists and
   evaluates them. Three shipped examples: Soft Frizz, Grey Streaks, Wind-swept Ends.
2. **Script nodes (built):** the language extensions (writable outputs, blocks, the new inputs,
   bounded loops, the step budget), the Script operator with incremental updates, the editor (Maya
   in full, the app's script editor), Blender's listing and evaluation, docs, and four shipped
   examples under Scripts: Wave, Kink, Taper by Channel, Random Length. Later: the GPU preview of
   scripts, the app's params table, editing in Blender, neighbour queries.
3. **WebAssembly plugins:** only if phases 1 and 2 leave real gaps.

## Decisions (2026-10-18 review)

- Scripts modify existing strands and points only; adding or removing them is later.
- The license field on a shared node is free-form (optional).
- A community node page on the website: someday, not now.
- Names: **Compound node** (a saved compound) and **Script node**; the file extension `.glnode`.

## Open questions

1. **Neighbour queries** (a script reading nearby strands): not in phase 2; next. See below.

### What neighbour queries would allow

A script could ask "which strands are near this one, and where are they?" (read from the node's
input, so the answer never depends on the order strands are processed in), through functions such
as `neighbours(radius, count)`, `neighbourP(i, t)` and `density(radius)`. That opens:

- **Custom clumping:** gather strands toward the average of their neighbours, clump by any rule
  (by a channel, by age, by distance to a part line), or clump sizes that vary with local density.
- **Spacing and separation:** push strands apart where they bunch up, or pull strays back in.
- **Coherence:** noise, curl or direction averaged over neighbours, so nearby strands move together
  (locks that hold) while far ones differ.
- **Density-aware effects:** thin, frizz or lighten where hair is sparse; matte or darken where
  dense; find stray strands that sit far from all others.
- **Group behaviours:** twist or braid small groups around their shared center, flow that aligns
  each strand with its neighbours.

The cost: a spatial index built per evaluation (fast, but not free) and more work per point, so
such scripts run slower and are CPU-only at first. It is a contained addition on top of phase 2,
so it can come right after it.
