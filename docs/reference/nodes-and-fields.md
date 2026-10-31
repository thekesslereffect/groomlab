# Expressions, fields and generated guides

Parameter expressions, fields driving params from other nodes, instancing geometry on strands, procedural guides and sculpting generated guides.

User guide: [Expressions](https://www.getgroomlab.com/docs/expressions), [Node graph](https://www.getgroomlab.com/docs/node-graph), [Fur, feathers & scales](https://www.getgroomlab.com/docs/fur-and-feathers), [Custom nodes](https://www.getgroomlab.com/docs/custom-nodes).

## Expressions

An **Expression** node (Add Node > Fields & Curves) turns a formula into a value per strand for
any exposed param. In the Graph Editor its panel is a small code editor:
- **Completion:** type `$` (or Ctrl+Space) for every variable, channel and function, each with
  what it is; hover a name for the same. Your own variables complete first.
- **A block, not a line:** `$name = ...;` lines make locals for the lines below, `#` or `//`
  start a comment, and the last line is the value:
  ```
  # 1 near a point, fading out by the radius
  $d = sqrt(($x - $center.x)^2 + ($y - $center.y)^2 + ($z - $center.z)^2);
  1 - smoothstep($radius * 0.5, $radius, $d)
  ```
  Names take a `$` as in XGen, or none (`length` still works).
- **Checked as you type:** after a pause the text is compiled; a mistake shows under the code
  with its line tinted (a missing `;` says so), and only a text that checks out is applied. Leaving
  the field or Ctrl+Enter applies it anyway. Mistakes found when the groom evaluates (a channel
  the strands don't carry) show there too.
- **Your own variables:** **+ Number** and **+ Vector** add variables the expression reads as
  `$amount` or `$center.x` (`.y`, `.z`; `.r .g .b` too), each with a slider or three fields in the
  panel, so a look is dialed without touching the code. Dragging a slider shows the groom change
  as you drag (outside undo; letting go is one undo step). They are stored on the node
  (`variables`, JSON: `{"amount": 0.5, "center": [0, 1, 0]}`).
- **Keyable variables:** the diamond beside a number makes it an attribute on the groom,
  `ctl_<node>_<name>` (Channel Box, and the Attribute Editor's **Controls**): key it, animate
  it, drive it from a rig; the slider then sets the attribute. Stored as
  `{"value": 0.5, "control": "expression1_amount"}` (the value where no attribute sets it: groom
  files, other hosts); the diamond again makes it a plain number at its current value.
- **The library** (the **Library** menu): examples ship with GroomLab (long strands, top of
  the head, stripes, tip fade, a band along each strand, random per clump, a few random strands,
  noise patches, near a point, straight strands, standing strands), and **Save to Library...**
  keeps yours (one form: the name and what it does; an existing name is replaced only once you
  confirm). Picking one loads its text, variables and range in one undo step, asking first when
  it would replace an expression you typed.
- **Where they are kept:** a node's expression and variables are part of the groom's graph, so
  they are saved in the scene file (`.ma`/`.mb`) and travel with it; nothing goes in the
  scene's `groomlab` folder (that is for images: masks, bakes, exports). The library is for
  reuse across scenes: it is read from every folder on `GROOMLAB_EXPRESSION_PATH` (a studio's
  shared set), then the user's `<Maya user app dir>/groomlab/expressions` (where Save writes, so
  a saved expression is there in every new scene), then the shipped examples; a name found
  earlier hides a later one. Graph presets work the same way (`GROOMLAB_PRESET_PATH`, the user's
  `groomlab/presets`, the shipped ones).
- **From scripts:** `groomlab.expressions` (`list_expressions`, `save_expression`,
  `apply_expression(groom, node, name)`, `check(text, variables)`) and
  `cmds.groomLabExpression(check=text, variables=json)`, which returns what is wrong or "".

Mappable params set to an expression (the **Map** button > Expression) get the same editor,
three lines tall.

## Fields: driving params from other nodes

Any mappable param can be exposed as an input and driven by a **field**: a value per strand
coming from another node (per point, where the field varies along the strand: an `Expression`
of `t` or of a point channel, and anything `FieldRemap` or `FieldCombine` build on them;
see Per-point maps under [Maps](looks-and-maps.md#maps)).
- **Exposing:** right-click the node and choose **Expose Param as Input**, or right-click the
  param's name in the side panel. The node gets a diamond port for it.
- **Driving it:** connect a field node there. The param's own number multiplies the field: a 0-1
  mask on a density of 100 gives 0-100, so masks and noise stay 0-1 and the amount stays on the
  param (a param whose own value is a map takes the field alone). A driven param shows what
  drives it; its own value stands alone again once the link is removed.

| Node | Outputs |
| --- | --- |
| `Map` | A map painted in Groom Tools > Paint as a Field for any mappable param, found by `name` (the groom tells the graph where it lives: on the guides, the scalp's vertices or in a texture, and its file), or an image file's path, read at the scalp's UVs. Five outputs: `luminance` (the first: what a link naming no output reads) and `r`, `g`, `b`, `a`, each one channel of a color map (masks packed in one map: one Map node feeds clump strength from R and frizz from G); a gray map gives its value on luminance, r, g and b and 1 on a, a guide layer its value on all five. Feed Param connects it (from luminance), Paint... opens it; `min`, `max` (min above max inverts it). A strand channel as a node is an `Expression` (`$name`) |
| `FieldNoise` | Smooth 3D noise over the scalp, from `min` to `max` (`frequency`, `octaves`, `offset`, `seed`) |
| `FieldRemap` | A field reshaped: `[inMin, inMax]` through a `curve` to `[outMin, outMax]` (inMin above inMax flips the input) |
| `FieldCombine` | Two fields, or a field and a number `b`: multiply, add, subtract, min, max or mix |
| `Control` | A value set on the groom: the keyable attribute `ctl_<name>` (channel box, animatable), `default` until set. Right-click a mappable param and choose **Drive with Slider**, or `groomlab.add_control(groom, node, param)` |

One texture or noise can drive many params, each reshaped by its own `FieldRemap`. On Scatter
density, the field's maximum sets the scatter density.

```python
model = graph.GraphModel.from_groom(groom)
model.expose_param("length", "keep")
noise = model.add_node("FieldNoise", params={"frequency": 3.0, "min": 0.4, "max": 1.0})
model.connect(noise, "length", "keep")
model.save(groom)
```

In graph JSON, a node lists its exposed params in `"paramInputs"` and the links name the param
as the input. A keyed param is listed in `"paramControls"` with the control that sets it:
`{"keep": "length_keep"}`, or three names for a vector (`["wind_directionX", ...]`); its
`"params"` value is used where no control is set.

**Several outputs.** A node can have more than one output (the Map node: `luminance`, `r`, `g`,
`b`, `a`, and `rgb`, its color, for a color param that takes one: ParamSpec `colorField`, the
spec JSON's `"colorField": true`, read with `ColorField` in field.h; HairColor's `color`, `dye`
and `tipColor`, Bands' `dye` and `undercoatDye`, FeatherColor's `dye` and `downColor`; the param's
own color times the map's at each root; a number param reads its luminance). The Graph Editor draws one labelled port per output down the node's right edge; drag
a link from the one to read. It runs once for all of them. A link names the output it reads in
`"output"`; without it, it reads the first, so graphs from before read what they always did (a
Map node's old `channel` param is ignored now: such a node reads luminance until its links are
dragged from a channel's port):

```json
{"from": "packed", "to": "clump", "input": "strength", "output": "r"}
```

```python
model.connect("packed", "clump", "strength", output="r")   # GraphModel; None or omitted: the first
model.output_of("clump", "strength")                        # "r"
model.output_names("packed")                                # ["luminance", "r", "g", "b", "a", "rgb"]
```

## Instancing geometry on strands

The `Instance` node puts a mesh on the strands, for snow, dandruff, dew, burrs or beads:

```python
flake = cmds.polyPlatonicSolid(name="snowflake")[0]
cmds.setAttr(flake + ".visibility", False)            # only the instances render
groomlab.add_instances(groom, flake, probability=0.08, scale=0.03, orient="random", t_range=(0.7, 1.0))
```

- **Placement:** each strand gets up to `count` instances. Each is kept with `probability`, a
  number or any map, so you can paint where the snow falls. It sits between `tMin` and `tMax`
  along the strand.
- **Look:** `orient` is `strand` (along it), `up`, `root` or `random`: what the prototype's
  **+Y** follows, so model a prototype with its long axis up +Y. `scale` is also mappable, and
  `scaleRandom`, `spinRandom` and `offset` add variation.
- **Size along the strand:** with `scaleByWidth` (on by default) each instance's size is
  multiplied by the strand's width where it sits over the strand's root width, so it follows the
  Width node's profile: smaller toward thin tips, and following width changes over time. A
  strand with no width at its root keeps the size as it is. Off, every instance has its `scale`.
- **Sized to the strand (`sizeMode`):** `fixed` (the default) makes `scale` a size in scene
  units, the same on every axis. `fitStrand` makes each instance span its strand, for leaves,
  feathers or scales on strands: along +Y it is `fitScale` (mappable, 1 = the whole strand) times
  the strand's **rest** length, and across (X and Z) `aspect` (mappable) times that, times the
  width factor when `scaleByWidth` is on; `scaleRandom` varies both alike. Model the prototype 1
  unit long up +Y from its base at the origin (and 1 unit wide for `aspect` to be its width over
  its length); put it at the root with `tMin` = `tMax` = 0. The length comes from the rest pose
  (below), never the length now, so the size holds while strands stretch and from frame to frame
  instead of pulsing with a simulation. `fitScale` is its own param rather than `scale` read as
  a fraction, since `scale` is a length: its unit shows in the editor, and presets fitted to
  another scalp scale it, which a fraction must not be.
  ```python
  groomlab.add_instances(groom, "leaf", probability=1.0, t_range=(0, 0), size_mode="fitStrand",
                         fit_scale=1.0, aspect=0.25, orient="strand")
  ```
- **Following the strands:** instances remember their strand and position along it, so nodes
  after the Instance node (curl, collide, simulation) carry them along, and preview density
  thins them with the strands. `add_instances` puts the node just before the output.
- **Stretching strands (`pinToRest`):** an instance sits at a fraction of its strand's length
  (between `tMin` and `tMax`), so a strand that stretches (simulated guides, a guide cache,
  correctives) slides it outward. With `pinToRest` on, it keeps the distance from the root that
  fraction gives on the strand at rest instead, and sits at the tip when the strand is shorter
  than that now. The rest pose is the groom without anything that moves: the scalp's rest pose
  (Rest mesh; its points without one), the guides as authored (no simulation, cache or
  corrective), no colliders, still Jiggle, and Wind and ImportCurves at time 0. It is evaluated
  only while an Instance node pins (`pinToRest`) or fits (`fitStrand`), once per edit, not per
  frame, and the viewport, exports, Arnold and the procedural all use it.
  Flat guides (connected curves) are taken as they are, so their animation is part of the rest.
- **Viewport:** each instance draws its prototype mesh, shaded in the theme's instance color
  (`displayInstances`), and follows edits to that mesh. A prototype that is not in the scene, or
  instances past a two-million-triangle budget, draw as small wire markers instead.
- **Arnold:** each prototype mesh becomes a hidden polymesh with its own shading group, and an
  `instancer` places the copies. The procedural (groom-file renders) finds prototypes by name in
  the scene. With deformation blur the instances blur with the hair: `instance_matrix` gets a key
  per motion step (see Motion blur in [Rendering with Arnold](rendering.md#rendering-with-arnold)). A fitted instance's matrix carries its per-axis scale in
  every key.
- **USD:** a `PointInstancer` beside the curves, with placeholder prototype prims to reference
  geometry into; `scales` is per axis (a fitted instance's X, Y, Z sizes).
- **Variants:** several prototype names separated by spaces (`"rock pebble shell"`) give each
  instance one of them, by its ID and the `seed` (`variantBy` `random`), or by the `variant` param
  (`map`: 0-1 over the list, mappable: a painted map, a channel, an expression).
- **Bent along the strand (`deform` `alongStrand`, Yeti's instance deform):** each copy bends
  along its strand instead of sitting rigid: every vertex's place along the prototype's long axis
  (`deformAxis`, +Y by default) is laid onto the strand's arc length from where the instance sits,
  across it in the strand's frame carried along from there (no twist of its own), +Z facing the
  feather's vane (Propagate's `vaneNormalX/Y/Z`) or else the skin at the root; `spin`, `offset`,
  `scale`/`fitStrand` and `scaleByWidth` apply as before. Leaves on vines (`vine_leaves`),
  modelled feathers following their shafts, ribbons. A bent copy is real geometry, made the way
  scales are (scales.h): the viewport draws it, Arnold renders it in the scales' polymesh (its
  prototype's shading group, the scales' user data, deformation blur per motion key) instead of
  the instancer, and USD and Alembic exports (Maya, the CLI, Blender) write it in the scales'
  mesh. `deform` `none` (the default) is exactly as before.
  ```python
  groomlab.add_instances(groom, "leaf", probability=1.0, count=6, t_range=(0.1, 0.95), size_mode="fitStrand",
                         fit_scale=0.16, aspect=0.45, spin_random=0.1, deform="alongStrand")
  ```

## Procedural guides

Strands grown and shaped in the graph can become guide curves, Yeti-style, for anything that
takes guides:
- **`ToGuides`:** turns strands into guides: their core (the strands every preview holds, so the
  guides are the same at every preview density: all of a scatter of under 5,000 strands, or one
  never thinned). `fraction` keeps a stable share of those, chosen by ID, and their channels come
  along as guide attributes (point channels as per-point layers).
- **Uses:** feed them either into Clump's `centers` for procedural clumping, or into Interpolate
  as procedural guides.

The pattern, for secondary clumps:
1. A sparse Scatter is interpolated from the groom's guides, waved with Curl, and turned into
   guides by `ToGuides`. Those are the big clump centers.
2. A second Clump, its `centers` open, picks its own centers from the clumped result (`clumpBy`
   `size`: about how far apart, in scene units) for secondary clumps. (`cinematic_hair` gets its
   two levels from two such Clumps instead.)

## Sculpting generated guides

The guides a graph makes can be brushed in place, as Houdini's guide levels are: sparse hand
guides -> an even `Scatter` + `Interpolate` -> `ToGuides` (dense, evenly spaced guides) ->
**`Sculpt`** -> the next `Interpolate`, a Clump's `centers` and so on.

- **Pick the node:** select the Sculpt node in the Graph Editor, its row **Node: sculpt1** in
  Groom Tools > Correctives, or its menu's **Brush This Node's Guides**. The viewport then
  draws the node's output as the guides, and the shaping brushes (Comb, Smooth, Lift, Length,
  Twist, Straighten, Attract, Cut; symmetry as for the guides) reshape
  them. Place, Draw, Erase, Paint and Part are for the guides themselves: pick the
  Base groom.
- **Kept in the node:** each stroke is one undo step of the groom's `sculptData` (by node name,
  then guide ID); only guides a stroke moved hold an edit. The scene saves it; groom files keep it
  (`"sculpts"`), so renders, the procedural, the CLI and the app see it.
- **Follows the graph:** an edit is the turn of each segment in its own frame and its length
  factor, by normalized arc length, not positions. Comb the sparse guides, change the density,
  add a Noise upstream: the dense guides are made again (with the same IDs, from their roots) and
  the strokes add the same bends on top (a 30 degree hook near the tip stays a 30 degree hook),
  nothing snapping back to where it was. Guides the graph no longer makes keep their edits until
  their ID comes back.
- **Renames, copies, deletion:** the Graph Editor carries the edits with a renamed node, gives a
  duplicate a copy and drops a deleted node's, in the edit's own undo step. **Clear Edits** (the
  node's panel, its menu, or the Correctives row's right-click) drops them.
- **While brushing** only the Sculpt node and what follows it evaluate (its input stays cached);
  the dabs are the CPU's (the GPU preview does not draw Sculpt strokes).

```python
from groomlab import sculpts
sculpts.set_target(groom, "sculpt1")    # brushes shape sculpt1's guides
sculpts.edited_guides(groom, "sculpt1") # how many hold an edit
sculpts.clear_edits(groom, "sculpt1")   # its guides as fed again (one undo step)
sculpts.set_target(groom, None)         # back to the base groom
```

Limits: guides only (not strands); a Sculpt node inside a compound is not brushed from the UI;
sculpting while the groom follows a moving scalp away from its rest pose shows the guides at rest.
