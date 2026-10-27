# Feathers, braids and scales

Braids, scales and feathers.

User guide: [Fur, feathers & scales](https://www.getgroomlab.com/docs/fur-and-feathers), [Color patterns](https://www.getgroomlab.com/docs/color-patterns).

## Braids

Draw curves from the scalp along where each braid should run. Add a **Braid** node in the Graph
Editor, select the curves and click the node's **Paths from Selected Curves** (they become guides
in its paths group; a Guides node for them is made and connected). By script:

```python
groomlab.add_braid(groom, ["braidCurve1"], style="three", width=0.15, crossings=2.5, region=0.3)
```

- **Paths:** the curves become the guide group `braids`, and a Guides node reads it into a
  Braid node placed after Interpolate. Noise and frizz later in the graph add flyaways.
- **Styles:** `three` (classic), `fishtail`, `rope` (two-strand twist) and `cornrow` (flat on
  the scalp). `dutch` raises the braid by crossing the lobes under.
- **Which strands:** those rooted within `region` of a braid's root. They leave the scalp over
  the first `gather` of their length, and over the last `tail` the lobes let go and the ends
  fan out. The hair is parted into the sections as it lies: a strand goes into the section on
  its side of the braid, so the sections leave the scalp without crossing each other.
- **Resolution:** a braided strand gets `resolution` points per weave repeat (12 by default, at
  most `maxSegments`), however few it came with, so the braid is drawn as a round, woven volume
  instead of a few flat zigzags. A Resample later in the graph keeps those points on braided
  strands (it resamples the rest); lower `resolution` for lighter exports (0: the strands' own
  points).
- **Seeing the weave:** `display` paths shows each braid's path and the center curve of each
  of its sections in place of the hair (a `braidSection` channel tells them apart: -1 the path),
  handy for tuning `crossings`, `width` and the feed; set it back to hair before rendering.
- **Feed** (`feed`, on for cornrows by default in `add_braid`): strands within `feedDistance`
  of anywhere along a path join it where they reach it (their own shape, e.g. guides combed
  toward the braid, or straight from the root) and run along it from there, as hair is picked
  up into cornrows and French braids; the root region (`region`) joins as well. `feedUntil`
  (0-1) picks up strands along that first share of the path only: past it the braid hangs free,
  a French braid becoming a plain braid off the head (0: only the root region joins). A strand
  picked up along the path joins the outer section on its side, as a braider adds hair, and with
  `feedGrowth` (1 by default) the braid thickens with the hair it has picked up: narrow where it
  starts, its full `width` where it holds all its hair (0: `widthProfile` alone). With
  `endAtPath` (on by default) each strand is fitted to the path: however long it is and
  wherever it joined, it ends within the braid's last stretch (staggered over `tail`). Off,
  longer strands hang straight on past the path's end.
- **Ponytail braids:** draw the path from the ponytail base and give it a large `region`
  (0.45, say). With `alongScalp` (on by default) hair drawn in from afar sweeps over the scalp
  into the base (pressed a quarter of the braid's width above it) instead of cutting straight
  through the head; off, it goes in straight lines.
- **Box braids:** no drawn paths: leave the Braid's `braids` input open and it picks its own
  paths among the strands, about `region` apart, and every strand joins the path nearest it (the
  `braids` preset).
- **Channels:** the node writes `braidId` per strand (the path's index in the paths group, or
  for paths of its own a number from the picked strand's ID, so an edit elsewhere renumbers no
  braid; -1 for strands not braided, strays included) and `braidFlat` (1 for a cornrow, lying on the
  scalp); the Cards node uses them to build braid cards.
- **Braid paths are not hair:** the other Guides nodes list the paths group in `exclude`, so
  Interpolate never uses a path as a hair guide.
- **Shape:** `width` along a `widthProfile` curve, `crossings` per unit of length, `lobeFill`,
  `strays` (loose strands) and `fuzz`.
- **Look:** `sections` weaves more strands (4-8 for a four- or five-strand braid or cornrow, 3+
  plies for a rope; 0 keeps the style's own). `depth` sets the braid's thickness out of the scalp
  against its width (below 1 flatter, above rounder). `phase` slides the weave along the braid
  (in repeats), `crossingsProfile` varies the crossings from start to end (a curve times `crossings`:
  loose at the top, tight at the tail, say), and `sectionTwist` twists each section's hair around
  itself per repeat, as real sections do.

## Scales

Scales (dragons, reptiles, pangolins) are rigid plates, not hair, and the strands lay them out:
Scatter, Flow, Bend and maps decide where each scale sits and which way it points, the strand's
length is the scale's length, its root width the scale's breadth, its HairColor its color. Every
grooming tool works on them unchanged. The `Scales` node (Look) puts one scale at each strand's
root; the preset `dragon_scales` is the whole chain, `scales_from_guides` and
`scales_from_flow` point them by guides and by flow arrows, `swept_spikes` bends them along the strands.

- **Placement:** at the root, its **+Y** along the strand's direction laid onto the skin, **+Z**
  the skin's normal, X across. `size` (mappable) scales the length (the strand's rest length,
  steady while strands stretch) and the breadth (its root width) together; without widths the
  breadth is `aspect` of the length. `scaleRandom` varies the size, `spinRandom` turns each about
  the normal (1: half a turn either way). `offset` is the height above the skin. One scale per
  strand.
- **Which way:** `aim` `tip` (the default) points each scale along its strand's chord, root to
  tip: where a strand as long as its plate puts the plate's tip, and steady under the noise and
  curl strands pick up toward their tips. `aim` `root` takes the strand's direction near its root
  (to the point a tenth of its length along, at least its first segment), for strands that curl
  back over their root.
- **Follow the strand:** `follow` `skin` (the default) lays each plate flat on the skin;
  `follow` `strand` bends it along its strand's curve: the base on the skin, then (blended in over
  the first quarter of the plate) every vertex about the strand's path at its distance along the
  plate, the plate's width axis carried along the strand without twist, its thickness kept, past
  the strand's tip straight on. Swept scales, spikes and quills bend with the guides, Flow's lift
  or a Curl; `conform` still keeps every vertex above the skin, and `follow` `strand` always aims
  by the root. A scale following its strand is kept per scale until its strand moves.
- **Prototypes:** the built-in plates need no scene mesh: `groomlab:scale` (keeled and pointed,
  slightly domed, bevelled), `groomlab:scaleRound` (a rounded tip, no keel) and
  `groomlab:scaleShield` (broad and blunt, the belly's). Or a mesh of your own (Prototype from
  Selection): model it 1 unit long up **+Y** from its base at the origin, 1 unit wide along X,
  lying on the XY plane with its top up **+Z**. Several names are variants (above): the preset
  takes the keeled plate on the back and the shield on the belly by a map of the normal.
- **Overlap like roof tiles:** density and the strands' length make the overlap; `tile` (1 by
  default) tilts each scale about its base so its tip rises by its own plate height, over the
  next scale's base; `lift` adds degrees. `stack` raises each by a random fraction of its plate's
  height, so overlapping scales never share a depth (no z-fighting).
- **Conform:** `conform` (on) bends each scale onto the skin: every vertex laid onto the surface
  under it along the normal, at its height above the skin plus the offset, and pushed out where a
  fold would leave it inside, through the skin's signed distance field (the one Collide uses).
  The bent scales are kept per scale and bent again only when their strand, the skin or the
  prototype changed (about 0.75 s for 50,000 scales of ~100 vertices on 24 threads; a rebuild
  that keeps them all, 0.14 s).
- **Shader data:** each scale carries its strand's color along it (base to tip: a darker tip
  from HairColor's `tipStrength`), a random ID, the scalp's uv at its root (a place along the body)
  and its variant. Arnold gets them as user data on the scales' polymesh: `scaleColor` (per
  vertex), `scaleRootColor`, `scaleId` (0-1), `scaleUV` and `scaleVariant` (per face), for any
  surface shader (an `aiUserDataColor` reading `scaleColor` into an `aiStandardSurface`'s base
  color). `material` names the shading group they render with; empty, a mesh prototype's own,
  and the built-in plates a `standard_surface` reading `scaleColor`.
- **Strands:** they only lay the scales out, so neither the viewport nor Arnold shows them
  (`showStrands` on: both do).
- **Viewport:** the scales draw as lit meshes in their own colors (DirectX 11 and OpenGL).
  `preview` `auto` draws flat quads instead past the instances' two-million-triangle budget,
  `quads` always (fast while grooming); renders always take the meshes.
- **Motion blur:** on a deforming body (with a rest mesh) the scales' mesh gets a key of points
  and normals per motion step, in the MtoA translator and the procedural.
- **Exports:** the core's `ScaleMesh` (`engine/core/include/groomlab/scales.h`, Python
  `groomlab_core.build_scale_mesh`) holds every scale's frame, its bent mesh and its data, and
  every export reads it:
  - **Alembic** (`export_alembic`, `scales="mesh"` by default): `<groom>/scales/scalesShape`, an
    animated polymesh, normals and uvs per corner, arbitrary params `scaleColor` (per vertex),
    `scaleRootColor`, `scaleId`, `scaleUV`, `scaleVariant` (per face).
  - **USD** (`export_usd`, `scales="mesh" | "instancer" | "both" | "none"`): a `Mesh` with the
    same primvars, and/or a `PointInstancer` (`scaleInstances`) of the plates (written as meshes
    inside it) with each scale's tilted frame as position, orientation and scale and the data as
    per-instance primvars (`scaleRootColor`, `scaleId`, `scaleUV`, `scaleVariant`). Unreal 5's
    USD Importer turns it into instanced static meshes. An instancer loses the bend onto the skin:
    the mesh is the exact export. The whole USD layer is a versioned layout
    (`web/content/docs/usd-layout.mdx`, held by `tests/engine/core/test_usd_layout.cpp`).
  - **FBX** (`export_scales`, Export > Scales Mesh): a static mesh for Unreal (uv sets map1,
    scaleUV, scaleData; color sets scaleColor, scaleRootColor), from `scale_mesh(groom)`, which
    also leaves the mesh in the scene to skin; `export_unreal_groom` writes it beside the groom.
  - **Baked skin maps** (`bake_scale_maps`, Utilities > Bake Scale Maps, `groomlab-cli
    bake-scales`): tangent-space normal, 16-bit height and color (with coverage) on the body's
    uvs, 16 to 8192 texels, a tile each for UDIM uvs (`engine/core/include/groomlab/scale_maps.h`).
  - **Groom files** keep the Scales and Instance nodes' own meshes (`"prototypes"`), so the procedural and
    `groomlab-cli` build custom plates without the scene.

## Feathers

Feathers are grown, not modelled (as in Yeti): every strand fed to a **Propagate** node is a
**shaft**, and barbs grow off it. Barbs are ordinary strands, so the nodes after Propagate (Noise,
Curl, Width, HairColor, Clump, Cards...) shape and color them like hair.

- **Shafts from a scatter or from guides.** Any strands are shafts: a Scatter shaped by guides
  through Interpolate (feathers lie the way the guides are combed), or guides placed by hand (a
  Guides node, one feather per guide; Propagate takes guides as they are). Brush, comb and
  shape the shafts like any hair before Propagate.
- **The design.** Barbs grow from `start` to `end` along each shaft, `count` a side
  (`sides`: both, left, right, or `around`: twice the count spiralling round the shaft, for down
  and plumes), `segments` each. Each is `length` times `lengthProfile` long (the ramp along the
  shaft, root to tip, is the vane's outline; `asymmetry` makes the left vane shorter and the
  right longer, as in flight feathers), leans `angle` degrees from the shaft (times
  `angleProfile`), bends `curl` degrees within the vane over its length (times `curlProfile`,
  positive toward the tip), rises `lift` degrees out of the vane at the shaft and bends
  `droop` degrees down over its length (a cupped vane). `splits` gathers neighbouring barbs into
  that many groups per vane, their tips `splitAmount` of the way to the group's middle barb (the
  gaps of a ruffled feather). `lengthRandom` and `angleRandom` vary each barb (seeded by its
  ID); `width` sets the barbs' width (0: the shaft's where the barb grows); `keepShaft` keeps
  the shafts in the output.
- **A down zone.** `down` (0: none) turns the shaft's base, from `start` up to `down` along it,
  into down (an afterfeather: plumulaceous barbs): `downCount` barbs a side (around: twice),
  `downLength` times the vane's length there, `downSegments` segments, curling `downCurl` degrees
  over their length (each between 0.6 and 1.4 times it) and kinked `downFrizz` degrees at every
  segment in and out of the vane, more varied in length and angle than the vane's; the vane's
  barbs then run from `down` to `end`. Down barbs gather into no split (`clumpId`: one of their
  own per vane) and carry `isDown` 1 (the channel exists only with a down zone), which
  FeatherColor's `downColor` reads. Without one the feathers are as before, to the bit.
- **The vane plane.** Barbs spread in the plane of the shaft and the scalp's tangent across it
  at the shaft's root, so feathers lie flat on the body; the plane follows a curving shaft
  (parallel transport). A shaft standing straight along the normal faces world up (+Y, else +Z);
  a curve rooted nowhere faces away from the way it bends.
- **What a barb carries.** An ID hashed from its shaft's ID, side and place (unique, the same
  every run), its shaft's root (so textures, color sets and painted maps read at the root sample
  the feather's base: one color per feather), its shaft's channels (HairColor's colors,
  `guideInfluence`, painted guide maps...; point channels at the point it grows from), and
  `featherId` (a number from the shaft's ID: an edit elsewhere renumbers no feather), `shaftU` (0 root .. 1 tip along the
  shaft), `side` (-1 left, +1 right, 0 around), `barbIndex` (its place along its side),
  `isShaft` (1 on the shafts, 0 on barbs: mask them with an attribute map) and `clumpId` (its
  vane, or its split, numbered from the shaft's ID too: HairColor's `clumpShare` varies color per vane; Cards makes a card per feather, see
  Feather cards), and its place in the feather laid flat (**vane space**, `color_patterns.h`):
  `vaneU` where it leaves the shaft and `vaneTipU`, `vaneTipV` where its tip reaches, u along the
  shaft (0 its root .. 1 the feather's farthest reach toward its tip), v out from it (0 on it ..
  1 the vane's widest reach, either side), as designed (curl and droop included, before splits,
  Noise and the rest move it); a point between sits at its place along the barb. A shaft runs
  up the middle (v 0). FeatherColor's patterns are drawn there. And the vane's normal where
  it grows (`vaneNormalX/Y/Z`), which the hosts take as a feather's strands' normal (their
  shading frames, the iridescence's angle) rather than the scalp's under the root.
  With a `groupId` above 0, every strand the node makes gets that `groupId` (the groom group
  engines split by; `groupName` names it), as `add_feathers` sets per design.
- **The rachis.** `shaftWidth` (0: none) makes each shaft a tube of that width at the root, times
  `shaftWidthProfile` along it (tapering to a quarter by default, never under 5%), with
  `shaftSides` sides (6; 2: a flat ribbon in the vane plane): geometry to skin into the
  character for game engines ([Feathers in Unreal](game-export.md#feathers-in-unreal-the-meerkat-eagle-workflow)). With a rachis the barbs start on its
  surface instead of its center line (`rootOnShaft`, on): each barb moves, as designed, to where
  its first segment leaves the tube, so the closest triangle to its root is its own rachis's.
  The feather presets give their designs a rachis width (exposed as `shaftWidth`).
- **Nodes after Propagate.** Shaping nodes (Noise, Curl, Bend, Smooth, Length, Width,
  Resample, Collide, Flyaway, Cull, HairColor, Bands, FeatherColor, Merge, Cards) work on barbs as on any strand; a
  barb's root is where it grows from the shaft (Length cuts, Bend turns and Collide walks from
  there). **Clump** clumps barbs within their own vane: picking its own centers it shares its
  `count` over the vanes, or by `size` puts as many on each vane as fit that far apart along it
  (at least one center each, spread along the vane by barb index), and
  pulls each barb toward the nearest center of its vane (the centers carry `featherId` and
  `side`), never across the shaft; shafts do not clump. Merge numbers b's feathers
  (`featherId`) after a's (from the next block of 2^22), as it does clumps. Nodes that find neighbours by root position
  (BlendNeighbors, Tangle, Braid's region, Interpolate) see every barb of a feather at its
  shaft's root: use them on the shafts, before Propagate.
- **Preview.** The viewport's point budget thins barbs by ID when needed (the render has them
  all); Scatter's preview density thins whole feathers.

| Operator | What it does | Main parameters |
| --- | --- | --- |
| `Propagate` | Grows barbs off every strand or guide it is fed (the shafts): feathers, ferns, plumes. Outputs strands: the shafts (`keepShaft`) and their barbs, with vane space; a down zone at the base | `sides`, `count`, `start`, `end`, `segments`, `length` (mappable, per shaft), `lengthProfile` (ramp), `asymmetry`, `angle` (mappable), `angleProfile`, `curl`, `curlProfile`, `lift`, `droop`, `splits`, `splitAmount`, `lengthRandom`, `angleRandom`, `down`, `downCount`, `downLength`, `downCurl`, `downFrizz`, `downSegments`, `width`, `keepShaft`, `shaftWidth`, `shaftWidthProfile` (ramp), `shaftSides`, `rootOnShaft`, `groupId`, `groupName`, `seed` |
| `Plume` | Lays feathers (Propagate's output, by `featherId`) over one another and off the body (Yeti's Plume): ranks them (`rankBy` `rootDistance` along `direction`, or with none each neighbouring pair against its own mean direction, `length`, `channel` or `random`; `invert`) and turns each up about its root, lowest first, until it lies `offset` above every lower feather under it (each a height map of its vane) and `bodyOffset` above the scalp and the `collider` input. `stiffness` 1 rigid, lower bends; roots stay on the skin; deterministic on any thread count; full evaluation | `offset`, `bodyOffset`, `rankBy`, `direction`, `rankChannel`, `invert`, `iterations`, `stiffness`, `maxAngle`, `depth`, `strength` (mappable, per feather), `seed` |
| `Split` | Routes each strand (or guide: guides in, guides out) to one of four outputs `a`, `b`, `c`, `d`, by their weights' shares: in `random` mode by strand ID (stable: a thinner preview keeps each strand's output), in `map` mode by `select`, a 0-1 value per strand (a painted map, a channel) that picks the share it falls in (weights 1 1 0 0: under 0.5 to a, the rest to b). Every strand goes to exactly one output. Several feather designs on one set of shafts, or any look split by a map | `mode`, `weightA`-`weightD`, `select` (mappable), `seed` |

**Designs and presets.** The presets `feathers_contour` (a rounded vane lying on the body, a few
splits), `feathers_down` (short shafts, fluffy curly barbs all around), `feathers_flight` (a
long, stiff, asymmetric vane) and `feathers_afterfeather` (a contour vane whose base turns to
fluff: a second Propagate grows loose, curly barbs at the base and a third grows barbules on
them, each branch with its own HairColor, joined by Merge) and `feathers_barred` (a contour
feather with a down zone, white bars across its left vane and an iridescent green right one: see
Color patterns) are whole grooms of feathers; each design in them is a Compound
(Propagate and the nodes shaping its barbs) with its main params exposed (`count`, `length`,
`angle`, `splits`, `width`...). Fitted to a scalp, the lengths inside the compound scale too.
`groomlab.add_feathers` puts a design into an existing groom:

```python
groomlab.add_feathers(groom, "contour")                                  # shafts: a Scatter shaped by the guides
groomlab.add_feathers(groom, "flight", source="guides", curves=curves)   # hand-placed: guide group "feathers"
made = groomlab.add_feathers(groom, ["contour", "down"], weights=[3, 1])  # a Split: 3/4 contour, 1/4 down
groomlab.add_feathers(groom, "flight", source=made["split"] + ".c")      # a third design on the Split's c
groomlab.feather_designs()                                               # ["afterfeather", "barred", "contour", ...]
```

It returns the nodes it made (`shafts`, `split`, `designs`, `merge`) and is one undo step. Each
design is a groom group of its own (its Propagate's `groupId` 1, 2, ... and `groupName` the
design's name; the same design added again joins its group; `groomlab.feather_groups(groom)`
lists them), so the Unreal export gives hair and each design their own group. The
feathers are merged with the hair before the output (`keep_hair=False`: they replace it);
`density` sets the scattered shafts' density (default: the design's). Hand-placed shafts' guide
group (`group`, default `feathers`) is left out of the Guides nodes growing the hair. Save your
own design as a preset named `feathers_<name>` (a graph whose first Compound is the design) and
`add_feathers(groom, "<name>")` uses it.

**Hand-placed flight feathers (one feather per guide).** Add > Feathers... (every host) asks
"Scattered feathers (shaped by the guides)" or "One feather per guide (hand-placed)" (the words
of `engine/ui/choices.json`'s `featherSource`) and, for one per guide, the **Guide group**: the
groom's groups or a new name (`flight_left`, `flight_right`, `tail`), its feathers group first
(default `feathers`). One per guide is the default when curves are selected or the groom has a
feathers group already (`groomlab.feather_guide_groups(groom)`: the groups of the Guides nodes
feeding a feather design, and a `feathers` group). Each group gets its own Guides node and is left
out of the hair's guides; selected curves join it; a group without guides gets the Draw tool
drawing into it (Maya with a viewport, the app, Blender outside background mode). The preset
`hand_placed_feathers` ("Flight feathers (hand-placed)") is the same workflow from scratch: a
Guides node on group `feathers` into the flight design, its `"requires": {"groups": [{"name":
"feathers", "draw": true}]}` making the group empty when the groom lacks it and picking the Draw
tool drawing into it, so every guide drawn is one feather (a Guides node on groups that are made
but hold no guides gives an empty set, even on a groom with no guides at all, so the design grows
nothing instead of missing its input):

```python
groomlab.add_feathers(groom, "flight", source="guides", curves=left, group="flight_left")
groomlab.add_feathers(groom, "flight", source="guides", curves=tail, group="tail")
groomlab.feather_guide_groups(groom)            # ["flight_left", "tail"]
presets.apply_preset(groom, "hand_placed_feathers")  # the feathers group made, empty
```
