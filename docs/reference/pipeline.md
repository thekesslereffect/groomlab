# Pipeline

Alembic and USD export, where a scene's files live, paths and referencing, rest meshes, following the scalp, colliders, several scalps, linked grooms, transfer to a new scalp, utilities and groom states.

User guide: [Utilities](https://www.getgroomlab.com/docs/utilities), [USD layout](https://www.getgroomlab.com/docs/usd-layout), [Other renderers](https://www.getgroomlab.com/docs/other-renderers), [Motion & simulation](https://www.getgroomlab.com/docs/motion-and-simulation).

## Exporting Alembic

```python
groomlab.export_alembic(groom, "C:/cache/groom.abc", start=1, end=48)
```

In Maya it is GroomLab > Export > Alembic... (the playback range). This wraps the
`groomLabExportAlembic` command (`-file`, `-startFrame`, `-endFrame`, `-step`, `-density`; `-scale`,
`-pointsPerStrand` and `-vertexUVs` fit the file to an engine's importer, as Exporting strands for
Unity does):
- **Evaluation:** each frame is evaluated in a DG context, so the scene's current time never
  changes.
- **Density:** 1.0 by default (render density). `previewDensity` is ignored.
- **Render overrides:** `-renderOverrides true` (`render_overrides=True`; USD too) exports as a render
  has the groom: its `renderDensity` times `-density`, `renderWidthScale`, `renderLengthScale`.
- **Layout:** the file holds `/<groom transform>/<groom transform>Shape` as linear curves with a
  per-point `width` (diameter) and, per curve: `strandId` (uint64), every strand channel such
  as `clumpId` (float), `Cd` (linear RGB per curve: the color nodes' `colorR/G/B` channels, else
  from melanin channels, when present) and
  the native `uvs` (the root UV on the scalp, when the export is given the scalp). The first
  frame decides which channels every frame carries, as the USD export does.
- **Atomic:** every export (Alembic, Unreal, USD, groom files) is written to `<file>.tmp` and
  moved into place when it closes, so a failed export never leaves a half-written file.
- **Topology:** on a deforming scalp, give the groom a rest mesh (below) to keep the same
  strands on every frame; without one the strand count may change from frame to frame.
- **Frame rate:** frame times use the scene's time unit, so a reading scene needs the same frame
  rate to line up.

To inspect a file, run `groomlab-cli abc-info file.abc`.

## Exporting USD

```python
groomlab.export_usd(groom, "C:/cache/groom.usda", start=1, end=48)
```

In Maya it is GroomLab > Export > USD... (the playback range). This wraps `groomLabExportUsd`,
with the same flags and rules as the Alembic export. It writes a text layer with no USD library
needed; convert it with `usdcat` when size matters:
- **Prims:** `/<groom transform>` is an Xform and the default prim. Under it, `strands` is a
  linear, nonperiodic `BasisCurves`.
- **Samples:** points, `curveVertexCounts`, widths (per point, diameter) and extent, one time
  sample per frame.
- **Primvars:** `strandId` (int64, per curve); `st`, the root UV on the scalp, when the scalp
  has UVs; `displayColor` (linear RGB per curve): the color nodes' `colorR/G/B` channels, else
  from melanin channels, when present; and every strand channel, such as `clumpId`. Channel names become USD identifiers;
  two that collide (`a-b` and `a_b`) or one that is reserved (`st`, `strandId`) get a numbered
  suffix (`a_b_2`).
- **Size:** `.usda` is text and large (about 60 bytes per point per frame); convert with
  `usdcat` to `.usdc` for anything beyond a test.
- **Stage:** `timeCodesPerSecond` follows the scene; `metersPerUnit` is 0.01 (centimeters);
  `upAxis` follows Maya's.

## Where a scene's files live

Everything GroomLab writes for a scene goes into **its own folder beside the scene file**,
`groomlab/<scene name>` (so `shot_v001.ma` and `shot_v002.ma` in one folder never share files),
one subfolder per kind: `masks` (texture maps), `cards` (card and scalp bakes: the maps and
the atlas record), `exports` (Unreal cards, strand exports, groom files), `caches` (guide
caches, baked simulations), `renders` and `references` (reference photos). Inside, a groom's files are named by its **asset
name** (`groomlab.asset_name(groom)`): its full DAG path made file safe, so `|charA|hair` and
`|charB|hair` never meet in a file name, and namespaces never put a `:` in one
(`|charA|hair` and `charA:hair` both give `charA_hair`). Texture maps are
`masks/<asset>/<map>.png`, bakes `cards/<asset>/<asset>_<map>.tga`, and the export dialogs
start from `<asset>.<ext>`. The groom stores those paths relative to the scene
(`groomlab/hero/masks/hair/crown.png`), so **send the scene with its `groomlab` folder and
everything works on the other machine**: masks read, baked cards show, exports sit where they
were. Scenes from before this layout (`groomlab/masks/hair_crown.png`) keep reading their files
where they are. Bake Maps, Bake Scalp and Paint... go there without asking; the export and cache
dialogs open there. `groomlab.asset_folder(kind)` gives the folder (made if missing).

Files are never shared by accident:
- A **new** file never replaces one that is there: a new texture map, a conversion to a texture
  or a default bake whose file is taken (a deleted groom's, another groom's) gets a free name
  beside it (`density_2.png`, `hair_2_Color.tga`).
- A file **two grooms name** (a duplicated groom copies its maps' paths; two references of one
  rig resolve to the same files) is copied for the groom about to write it, which is repointed
  at its copy in the same undo step: a paint stroke, a Fill, a rename (it copies instead of
  moving) or a bake never changes the other groom's hair.
- The Paint brush always paints the file of the painted groom's own map, whichever groom was
  active when the tool was set up.

**Save Scene As** (and the first save of an untitled scene) copies the files the grooms name
into the new scene's own folder (`groomlab/<old name>/...` to `groomlab/<new name>/...`; other
relative paths keep their path under the new folder) and repoints the grooms (and their card
mesh and hair cap materials) there before the file is written, so the saved scene is whole and
independent of the one it came from. A file already at a destination with other content is
never adopted: the copy takes a free name and a Script Editor warning says so. While a scene is
untitled there is no "beside the scene", so the folder is `<project>/groomlab/untitled`: Create
Groom, a new texture map and Paint > As Texture ask you to save the scene first (Save Scene
As..., or go on untitled for now), and a warning says where the files go until you do.

## Paths, referencing and moving projects

Grooms keep no absolute paths, so they work in referenced and namespaced scenes and after a
project moves:
- **Relative paths:** stored paths resolve against the scene's folder, then the Maya project
  root; a groom from a **referenced** file looks in that file's folder first (where its own
  scene kept its files), so a rig referenced into shots in other folders still finds its masks
  and bakes. This covers texture maps, mask images, card maps, `ImportCurves` files, guide
  caches and `groomFile`.
- **Environment variables:** `$VAR`, `${VAR}` and `%VAR%` are expanded when a path is read.
- **Helpers store portable paths:** `texture_map`, masks, card bakes, `import_guides_from_alembic`,
  `set_guide_cache` and `render_from_groom_file` store a path relative to the scene's folder, else
  to the project, when it is inside one (`groomlab.portable_path`).
- **Finding stragglers:** `groomlab.absolute_paths(groom)` lists the absolute paths still
  stored on a groom.
- **Checking a path:** `groomlab.resolve_path(path, node=None)` (or `groomLabPaths -resolve`,
  with `-node` for a node's own resolution) shows where a path resolves.
- **Groom files:** they resolve relative paths against their own folder, then their
  `searchPaths`, which `export_groom` fills with the project and scene folders. Point the search
  paths at the farm's copy of the project with a variable, for example `$SHOW_ROOT`.
- **Node errors:** a groom reports what went wrong in its `lastError` attribute
  (`groomlab.errors(groom)`), such as an unknown operator, a map that can't be read, bad guide
  data or a file from a newer GroomLab. The graph editor's status bar shows it after Apply.
  Evaluation never throws into Maya.

## Rest mesh

On a scalp with deformers (skin, blend shapes), strands are scattered from triangle areas, so
area changes would add and remove strands frame to frame. A rest mesh fixes that: scattering
and the guide and clump matching use the rest pose, and the strands still ride the deformed
scalp.

- **Automatic:** `create_groom` connects the scalp's `Orig` shape when the scalp has deformers.
- **By hand:** `groomlab.set_rest_mesh(groom, mesh=None)`, or **Set** on the Rest row of the
  Attribute Editor's Inputs (or the Graph Editor's Scalp node). It uses the `Orig` shape, or
  else a hidden copy of the scalp at the current frame. The mesh must have the scalp's vertices.
- **Removing:** `groomlab.clear_rest_mesh(groom)`, or **Clear** on the Rest row.

## Follow the scalp

`followScalp` (Attribute Editor > Inputs > Follow Scalp, `groomlab.set_follow_scalp(groom, True)`,
off by default): the groom is evaluated on the rest mesh and each strand is carried to the
scalp as it is by its root's rigid frame (the root face's longest edge at rest made
perpendicular to the smooth normal at the root, the normal, their cross product; a rotation and
a translation, no scale). The evaluation at rest stays cached while only the scalp moves, so a
deforming groom costs the carrying per frame and per motion key (core: 100k strands, 51 ms a
frame normally, 8 ms followed).

- **Needs a rest mesh;** without one, or with the scalp at its rest pose, the groom is exactly
  what it is unfollowed (the Script Editor says so once when there is no rest mesh).
- **Taken back to rest through their roots,** so they keep their world place: guide curves,
  simulated guides (`simGuides`), guide caches, a brush stroke's working guides. The stored
  guides, parts, flow and painted maps are laid on the rest mesh.
- **Read at rest as they are** (they then turn with the head): colliders (Collide against a
  ColliderMesh), ImportCurves, Wind's direction and gusts, world-space noise and anything else
  that reads world positions. Jiggle's scalp history is turned into rest space (each vertex's
  motion by its own frame), so it lags as the head moves.
- **Also carried:** Instance nodes' fixed directions, the Cards nodes' cards (each by its first
  strand's root) and Propagate's rachis rings (each by the scalp point nearest its base).
- Exports, the Arnold translator (one evaluation at rest for all motion keys), groom files
  (`"followScalp": true`), the procedural and the CLI follow too. Core:
  `GroomSession::setFollowScalp`, Python `GroomSession.follow_scalp` (`follow_scalp.h`).

## Colliders

Any mesh can keep the hair off it: a body, a collar, a hat, another character.

```python
groomlab.add_collider(groom, "collar")            # ColliderMesh "collar" + Collide before the output
groomlab.add_collider(groom, "body", name="torso", wire=False)  # the mesh input alone: wire it yourself
groomlab.colliders(groom)                          # [{"name", "mesh", "index"}]
groomlab.remove_collider(groom, "collar")
```

- **How it is wired:** the mesh's world mesh goes onto the groom's `colliderMeshes` (an array of
  `{colliderMesh, colliderName}`), and the graph reads it as the external mesh `mesh:<name>`
  through a **ColliderMesh** node of that name (Graph Editor > Add Node > Sources). `add_collider`
  adds one and a **Collide** node (`collide_<name>`) just before the output, with the scalp on
  its `scalp` input and the ColliderMesh on its `collider` input; move the Collide node earlier in
  the Graph Editor when the collision should happen before clumping or noise. The name defaults
  to the mesh's transform name (rename the mesh and the collider keeps its name).
- **Everywhere the groom is evaluated:** the viewport, the cards, renders (the Arnold
  translator) and exports (Alembic, USD, Unreal) all read the same colliders at the current
  frame, so an animated body keeps the hair out as it moves. Groom files store no meshes: a
  `.groom` rendered on the farm collides only with its scalp (cache the collider with the scalp,
  or bake the groom to Alembic instead).
- **Attribute Editor:** the **Inputs** section lists the colliders with **Select** and
  **Remove**; **Pick** makes the selected mesh a collider. **Remove** (and
  `remove_collider`) drops the ColliderMesh node and the Collide nodes it alone fed (their
  strands pass straight through).

## Several scalps

A groom can grow on more meshes than its scalp: separate ears, a body and a head, eyebrow patches.

```python
groomlab.add_scalp(groom, "earL")             # a Scalp, Scatter and Interpolate for it, merged in
groomlab.add_scalp(groom, "brows", name="brows", wire=False)  # the mesh alone: a Scalp node reads it
groomlab.scalps(groom)                         # [{"name", "mesh", "index", "rest"}]
groomlab.remove_scalp(groom, "earL")
```

- **How it is wired:** the mesh's world mesh goes onto the groom's `extraScalps` (an array of
  `{extraScalpMesh, extraScalpName, extraScalpRestMesh}`, joined after `inMesh` in index order).
  A **Scalp** node whose `input` is the name reads that mesh alone (the Graph Editor's Scalp
  panel has a menu of the groom's scalps and a mesh field with the eyedropper), so a Scatter fed
  it grows hair there. `add_scalp` adds a Scalp node (`scalp_<name>`), a Scatter with the groom's
  own Scatter's settings, an Interpolate on the groom's guides, and a Merge right after the
  groom's own Interpolate, so the look that follows applies to both.
- **One groom:** the scalps are joined into one surface (the groom's own scalp first, so what is
  rooted on it stays put): guides, parts, flow, painted maps (`add_scalp` and `remove_scalp`
  refit vertex maps), the rest pose, Follow the scalp and the simulation work across them; every
  strand's root is on the joined surface and follows its own mesh as it deforms.
- **Subdivision:** each scalp is refined at its own level: the Scalp nodes reading it (match
  render reads that mesh's own level), or the groom's own scalp's level for a scalp no Scalp node
  reads. Refining one scalp leaves the others' strands, maps and guide roots where they were.
- **Jiggle:** each scalp's earlier frames are read (Maya reads every scalp's mesh at the frames
  before), so hair on a moving ear or body jiggles as hair on the groom's own scalp does.
- **Everywhere:** renders (the translator and the procedural), exports and groom files
  (`"scalps"`), the CLI, the app and Blender.
- **Attribute Editor:** the **Inputs** section lists the other scalps with **Select** and
  **Remove**; the **Scalps** row's **Pick** adds one (its branch wired). Core: `GroomData::scalps`,
  `GroomSession::setScalps`, `scalp_pieces.h`.

## Linked Groom node (groom libraries)

A **Linked Groom** node (type `Reference` in `.groom` files and the Python API; Graph Editor > Add
Node > Sources) brings in another groom file's result: a
library look ("eyebrows.groom", "fur_patch.groom") reused, or instanced several times, in a
groom. Merge it with the groom's strands (a Merge node), or feed its `guides` output to an
Interpolate.

- **`path`:** the `.groom` file (relative paths resolve like a map's file: the scene's folder,
  then the project; a groom file's folder, then its search paths). It is read again when the file
  changes. The referenced groom is evaluated at the groom's own preview density, time and
  viewport or render setting, with its own painted maps and paths.
- **Outputs:** `strands` (its output) and `guides` (its guide hierarchy, posed and corrected,
  at its deepest level, and its guide curves).
- **`bind`:** `world` keeps them where the file has them (not rooted on this scalp: they do not
  follow it); `closestPoint` roots each strand at the closest point of this groom's scalp (or the
  mesh on the node's `scalp` input: one of several scalps), turned with the surface; `uv` roots it
  at the point with its UV in the file (both scalps sharing a UV layout), turned with the surface
  and its UVs. Rebound strands follow the scalp like the groom's own.
- **Errors:** a missing file, or one that references itself, is an error naming the file; no
  `path` is a note. Instance prototypes and card bakes stay the library's (use bind `world`).

## Transfer to a new scalp

Move a groom onto another mesh: a new sculpt, a retopologized head, or another character. It is
one undo step:

```python
missing = groomlab.transfer_groom(groom, "newHead")            # closest point, painted maps projected
groomlab.transfer_groom(groom, "bigCat", fit=True)             # a scalp elsewhere or of another size
groomlab.transfer_groom(groom, "newHead", maps=False)          # maps left as they are
groomlab.transfer_groom(groom, "newHead", color_sets=True)     # also the color sets the graph reads
groomlab.transfer_groom(groom, "newHead", method="markersWarp", # by marker pairs: old point, new point
                        markers=[("head.vtx[12]", "newHead.vtx[40]"), ...])
groomlab.transfer_groom(groom, "newHead", method="uv")          # both heads laid out alike in UV
```

**Transfer methods** (`method=`, the dialog's **Method**): where each guide root, part point, flow
arrow and map texel of the old scalp lands on the new one.

| Method | For | How |
|---|---|---|
| `closest` (default) | the same head, resculpted or retopologized, in place | the closest point of the new scalp |
| `bounds` (`fit=True`) | the same head elsewhere or of another size | the old scalp's bounding box moved and scaled (uniformly) onto the new one's, then the closest point |
| `markersAlign` | a head elsewhere, **turned**, of another size | 3 or more marker pairs give the best move, turn and uniform scale (least squares: Horn's method with Umeyama's scale), then the closest point |
| `markersWarp` | a head of **other proportions** (wider, longer, another character) | 4 or more pairs spread over the head (not on one plane) bend space smoothly so each old marker lands on its new one (a polyharmonic spline, kernel r³, with an affine part and a small smoothing term so close markers do not blow up), then the closest point. Fewer pairs, or pairs on one plane, fall back to align with a warning |
| `uv` | two heads with the same UV layout | each root found at its UV on the new scalp; roots without a UV match go to the closest point (counted in a warning) |

- **Markers:** a pair is a point of the old scalp and the same point of the new one (a vertex, or
  the middle of an edge, a face or a few components: `("head.vtx[12]", "newHead.f[40]")`, world
  points `((x, y, z), (x, y, z))`, or stored sides). Spread them over the head: forehead, temples,
  crown, nape, behind the ears. They are snapped onto both meshes (a face and barycentrics, so they
  follow a moved or deforming mesh, and the world point) and stored on the groom
  (`transferMarkers`; `.groom` files keep them as `"transferMarkers"`), so a transfer again reuses
  them: `markers=None` takes the stored ones. `groomlab.get_transfer_markers(groom)`,
  `set_transfer_markers(groom, pairs)` and `snap_markers(groom, newHead, pairs)` read and write them.
- **Guide shapes:** with every method but closest a guide is carried by the mapping at its root: the
  similarity's turn and scale for align, the warp's local stretch made into a turn and a uniform
  scale (its polar decomposition: guides turn and grow, they never shear) for warp, the change of
  the two meshes' UV tangent frames for uv. Then it is set upright on the new surface.
  `keep_lengths=True` keeps guide lengths and tube radii (they turn, they do not scale).

- **Guides:** each primary and locked guide keeps its shape and is re-rooted at the closest point
  of the new scalp. The shape turns with the surface normal there. Children follow their parents,
  and IDs, selection and painted maps are kept.
- **Parts:** they are re-bound the same way.
- **Fit:** `fit` (the `bounds` method) first moves the old scalp's bounding box onto the new
  one's and scales it uniformly. Guide lengths and tube radii scale with it.
- **Connections:** the groom's `inMesh` is reconnected to the new mesh. The old rest mesh is
  disconnected, and the new scalp's `Orig` shape is connected when it has deformers.
- **Maps:** guide layers travel with the guides. Vertex and texture maps are **projected** onto
  the new scalp (`maps=True`, the default): each point of the new scalp is taken back through the
  method's inverse to the old scalp (the fit's or the markers' inverse then the closest point, the
  warp's inverse fitted as a second spline, or the same UV), and the map's value there becomes its
  value, so a
  map lands where the guides do whatever the two meshes' vertices and UVs are. A vertex map gets
  values for the new vertices; a texture map gets a new image of the old one's size at the new
  scalp's UVs (gray or RGBA as before), written under the groom's masks folder with a free name
  (`<map>_2.png` when `<map>.png` is taken), and the map points at it. The old file is never
  written: other grooms, or the scene before the transfer, may read it. Undo puts the maps back
  and removes the new files. A map that cannot be projected (values that do not fit the old
  scalp, a missing image, a scalp without UVs) stays as it was, with a warning. With
  `maps=False` vertex maps keep the old vertices' values (they fit only a scalp of the same
  vertex count) and texture maps their image, read at the new scalp's UVs.
- **Color sets:** Maya color sets of the old mesh that the graph reads by name (a
  `color_set_map`, or a Map node naming one) stay with that mesh unless `color_sets=True`: then
  each is copied onto the new mesh with Maya's **Transfer Attributes** (closest point, after the
  same fit, or the markers' best move, turn and scale (a warp's bend is not followed); by UV for
  `uv`), and the new mesh's history is baked so nothing stays live. A new scalp with deformers
  is left alone (baking would bake its rig too): its color sets are reported to copy by hand.
- **What comes back:** `transfer_groom` returns what could not be carried: maps left behind and
  color sets the graph reads that the new scalp lacks. `report={}` also fills in `projected`
  (the maps projected) and `colorSets` (the color sets copied).
- **When to run it:** at the bind pose, since both meshes are read at the current frame.
- **Not moved:** connected guide curves and simulation curves stay where they are. Re-create
  them afterwards (you get a warning).
- **In Maya:** select the new mesh and click **Pick** on the Scalp row of the groom's Attribute
  Editor (Inputs), or **Pick from Selection** on the Graph Editor's Scalp node. A groom with
  guides asks **Closest Point** or **Fit and Move**. By script, `groomlab.set_scalp(groom, mesh,
  fit=False)` does the same: it transfers a groom with guides and just connects an empty one.
- **Command:** `groomLabTransfer -scalp <mesh> [-method <m>] [-markers <json>] [-keepLengths]
  [-fitBounds] [-maps] <groom>`. `-method` is one of the five (`-fitBounds`: bounds); `-markers`
  takes the pairs as JSON (`[{"old": {"point": [x, y, z]}, "new": {...}}]`, a side may give a
  `face`, `u`, `v` instead), stores them snapped on the groom, and without it a marker method reads
  the stored ones. `-snapMarkers` with `-scalp` and `-markers` changes nothing and returns the
  pairs snapped. `-maps` projects the
  painted maps; it is off for the command (scripts say what they want), and the Python API
  passes it by default. `groomLabTransfer -mapsFrom <otherGroom> [-mapName <name> ...]
  [-fitBounds] <groom>` projects another groom's maps (the ones named, else all) from its scalp
  onto this groom's, adding them to its maps and changing nothing else (Copy Graph uses it).
- **From the menu:** GroomLab > Utilities > **Transfer Groom to Scalp...** opens a window, never
  modal, laid out like Wrap's: two views side by side, the groom's current scalp on the left and the
  **New scalp** (the dropdown: changing it reloads the right view) on the right. Each view is a
  Maya modelEditor with a hidden camera of its own (in a `groomLabTransferUI` group, kept out of
  the undo queue, deleted with the window), isolated to its mesh, with Alt navigation and a
  **Frame** button. Below: **Method**, **Symmetry**, **Wireframe**, **Undo** / **Redo** /
  **Clear**, **Keep guide lengths**, **Project painted maps** (on), **Copy mesh color sets** (off),
  the status line (`6 pairs, 1 unmatched`, and why **Transfer** is disabled: align needs 3 pairs,
  warp 4) and **Transfer**, which says what moved, what was projected and what did not come along.
  The marker points (marker methods; with the others the views show the meshes only):
  - each side keeps its own ordered list: pair N is the Nth point on the left and the Nth on the
    right, placed in any order. Points draw as numbered locators (yellow left, cyan right) in a
    `groomLabTransferMarkers` group, each side's shown in its own view only; an unmatched point
    (its partner not placed yet) draws dim.
  - a click on a mesh adds a point at the end of that side's list (a ray from the view's
    `M3dView.viewToWorld` against the mesh: `MFnMesh.closestIntersection`, a face and
    barycentrics); a drag on a point slides it over the surface; **Ctrl+click** on a point deletes
    it and its partner at the same index, and both lists renumber; hovering a point lights it and
    its partner. The clicks reach the window through a Qt event filter on the two views only (the
    left button without Alt): Maya's current tool is never changed and the main viewport works
    as before.
  - **Symmetry** also adds the mirror of each click (across x = the mesh's bounding-box center,
    snapped onto the mesh) as the next point on that side; the two are a group: deleting either
    deletes both, and their partners.
  - **Ctrl+Z** / **Ctrl+Y** (or Ctrl+Shift+Z) in the window undo and redo marker edits: the
    window's own stack, not Maya's.
  - the complete pairs are stored on the groom on every edit (`transferMarkers`, snapped onto both
    meshes); unmatched points are kept only while the window is open.

  The point lists are the core's `TransferMarkerEditor` (`transfer_markers_edit.h`, bound to
  Python as `groomlab_core.TransferMarkerEditor`), the same model the app's window uses. Scripts
  drive the window without the mouse: `w = transfer_window.show(groom, scalp="newHead")`, then
  `w.add_point("old", (x, y, z))` (snapped onto the mesh), `w.delete_point(side, i)`,
  `w.move_point(side, i, point)`, `w.undo()`, `w.redo()`, `w.set_new_scalp(mesh)`, `w.pairs()`,
  `w.pending()`, `w.set_method(...)`, `w.transfer()`. `menu.transfer_dialog(scalp=...,
  method=..., markers=...)` transfers at once without the window.
- **In the standalone app:** Tools > **Transfer Groom to Scalp...** opens the same window: the
  groom's scalp on the left, the mesh picked with **New Scalp...** (.obj, .abc) on the right, each
  view with its own orbit camera (Alt+left orbits, middle pans, right or the wheel dollies;
  **Frame**), the same point rules, **Symmetry** and **Ctrl+Z** / **Ctrl+Y**, and **Import
  Markers...** for pairs in a JSON file (the format above). The views draw the meshes with the
  app's renderer and pick with the core's ray test (the brushes'). The complete pairs are stored
  with the groom as you edit and saved in its file.

## Utilities

GroomLab > **Utilities** moves hair in and out of plain Maya geometry and a look between grooms.
Every item works on the active groom (the pinned one, else the selected one); a dialog asks for
choices only when you click the item. Each is one undo step, and each is a function of
`groomlab.utils` that takes its choices as arguments:

```python
from groomlab import utils
made = utils.guides_to_curves(groom)                      # selection=True, group="primary", degree=3...
utils.curves_to_guides(groom, made["group"])              # edited curves back into their guides
utils.strands_to_curves(groom, node=None, max_curves=20000, attributes=True)
utils.strands_to_mesh(groom, style="tube", sides=4, width_scale=1.0, max_strands=20000)
utils.bake_guides(groom, "toGuides", group="baked")       # = groomlab.bake.bake_guides
utils.transfer_to_scalp(groom, "newHead", fit=True, maps=True, color_sets=False)  # transfer_groom, with a report
utils.copy_graph(groom, ["hair2", "hair3"], guides="keep", maps=False)
```

- **Guides to Curves:** a NURBS curve per guide, as authored (the shapes the brushes edit), under
  a group `<groom>_guideCurves` with a transform per guide group, nested as the groups are
  (`keep_hierarchy=False`: flat). The selected guides, else every guide (`group=`: one group's).
  Each curve's transform carries `glGuideId` and `glGuideGroup` (strings). Degree 1 by default:
  the CVs are the guide's points exactly; `degree=3` makes a cubic through them.
- **Curves to Guides...:** the selected curves (or their group) back into the groom. A curve
  tagged `glGuideId` whose guide still exists reshapes that guide in place: same ID, children
  following, its layers kept; the curve's edit points become the guide's points (a degree 1
  curve's CVs). Curves whose shape did not change leave their guide alone, so an untouched child
  keeps following its edited parent. Every other curve (untagged, an ID the groom lacks, a second
  curve with the same ID) becomes a new primary guide of the chosen root group. So: Guides to
  Curves, edit with any Maya tool (CVs, lattices, a sculpt deformer, a sim), Curves to Guides.
  By command: `groomLabGuides -reshape <id> <curve> ...`.
- **Strands to Curves...:** the evaluated strands (the groom's output, or any node's; a node
  giving guides works too) as a curve per strand under `<groom>_strandCurves`, for XGen, other
  packages or deformers. At most `max_curves` (20,000): beyond it strands are thinned by strand
  ID, the ones a lower preview density keeps, so the same curves come every time and a warning
  says how many were left out. With **Strand attributes** each curve's shape carries
  `glStrandId`, `glWidth` (a value per point) and the strands' channels (`glColorR`, `glClumpId`,
  `glMask`...: a number per strand channel, an array per point channel). The curves are made in
  one go through the API: 20,000 in about 4 s.
- **Strands to Mesh...:** the strands as one polygon mesh with the default shader: **tube**
  (`sides` around each strand, as wide as its widths times `width_scale`, the tip closed) or
  **ribbon** (a flat strip as wide as the strand, lying along the scalp: turned by the scalp's
  normal at the root and carried along the strand). UVs: u across, v root (0) to tip (1); **Per-
  strand UVs** adds a UV set `strandUVs` with every strand in its own cell of a grid. Capped and
  thinned like the curves (`max_strands`); 20,000 strands make a 320,000-face mesh in under a
  second.
- **Bake Node to Guides...:** a graph node's strands or guides (ToGuides, a Noise on guides, a Scatter...)
  become primary guides of a root group, to brush, mirror and give children (see [Procedural
  guides](nodes-and-fields.md#procedural-guides)). Also on a node's right-click menu in the Graph Editor: **Bake Node to Guides...**.
- **Transfer Groom to Scalp...:** see Transfer to a new scalp. The dialog's **Project painted
  maps** (on) and **Copy mesh color sets the graph reads** (off) are `maps` and `color_sets`.
  `transfer_to_scalp` returns the counts that moved (guides, parts, flow arrows), what was
  dropped, the maps projected (`projected`), the color sets copied (`colorSets`) and what stays
  with the old mesh (`missing`).
- **Copy Graph to Other Grooms...:** the active groom's graph (its look) and control values onto
  the other selected grooms, all in one undo step. **Guides:** `keep` leaves each groom's own;
  `replace` gives it the source's too: copied onto the same scalp or one of the same topology
  (bound to its faces, they sit on the same faces there), moved across with the transfer onto
  another (bounding boxes fitted; the target's parts and flow stay its own). **Maps** the graph
  reads by name that a groom lacks are reported; with **Copy painted maps** they come along:
  onto the same scalp or one of the same topology copied as they are (a vertex map's values, a
  texture map as a copy of its image), onto another scalp projected by position as the transfer
  projects them (bounding boxes fitted; `groomLabTransfer -mapsFrom`). New images go under the
  groom's own masks folder with a fresh name, so the grooms never paint into one file. The
  report lists `copied` and `projected` maps per groom. A guide layer comes only with replaced
  guides.
- **Find Missing Files...:** the files the selected grooms (else every groom) name that are not
  there (texture maps, images and Alembic files in the graph, the guide cache, the groom file)
  relinked to files of the same name in a folder you pick and its subfolders, in one undo step
  (`find_missing_files`; see A map's image). The Script Editor lists each file relinked, still
  missing or ambiguous; nothing is asked when nothing is missing.
- **Every file a groom uses, for pipelines and the File Path Editor:** `file_dependencies(groom)`
  lists them ({kind, key, path as stored, resolved, exists}: texture maps, the graph's file params
  inside compounds too, the guide cache, the groom file, each Simulate node's bake, the card bake's
  files) and `repath_file(groom, kind, key, path)` points one elsewhere, stored portable (a card
  bake moves as a whole). Maya's File Path Editor lists them under **GroomLab**: each file has a
  `groomLabFile` node (`hosts/maya/src/file_node.h`: `fileName` the file as it resolves, connected
  to its groom by `groom`, `fileKind` and `fileKey` saying which file), kept in step on idle after
  the groom's files change and before every save (`groomlab.dependencies`, `sync_file_nodes`); a
  Repath or Replace String there goes back to the groom on idle, relative as GroomLab stores it.
  The nodes' upkeep makes no undo step; undoing the editor's change brings the groom's path back.
  A scene opened (or imported) from another folder or machine reads its nodes' saved paths as no
  edit: the grooms keep their own (relative) paths and the nodes are refreshed from them once it
  is open.
  Maya's own `file -query -list` and Archive Scene see only Maya's node types (file textures,
  references, audio): a pipeline collects GroomLab's files with `file_dependencies` over every
  groom (or `ls -type groomLabFile` and each node's `fileName`). Referenced grooms keep the nodes
  their file saved.
- **Collect Files:** `collect_files(grooms=None)` (GroomLab > Utilities > **Collect Files**)
  copies every file the grooms use from outside the scene's folder into its asset folder
  (`masks/<groom>` for maps and graph images, `caches/<groom>`, `cards/<groom>` for a whole card
  bake, `exports/<groom>` for the groom file; a UDIM set's tiles; a file of the same content there
  already reused, other content a free name) and repoints the grooms, relative: Archive Scene and
  any copy of the scene's folder then carry them. Files already inside stay; missing ones are
  reported (`{"copied", "missing", "inside"}`). The scene must be saved; one undo step.
- **Command:** `groomLabConvert` builds the curves and meshes (`-guideCurves`, `-strandCurves`,
  `-strandMesh`; see `hosts/maya/src/convert_cmd.h`).

## Groom states

- **Other groom states** use Maya's own tools: hide or show a groom in the Outliner, lock it
  with its `locked` attribute in the Attribute Editor (`groomlab.lock_groom`; a locked groom
  ignores brushes and part strokes), and pick it in the Groom Tools' groom list to keep working on it.
