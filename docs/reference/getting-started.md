# Getting started in Maya

Installing a build into Maya, a first groom from Python, and how the preview, Scatter, Interpolate and Resample behave.

User guide: [Installation](https://www.getgroomlab.com/docs/installation), [Quick start](https://www.getgroomlab.com/docs/quick-start), [Node graph](https://www.getgroomlab.com/docs/node-graph).

Install the module into a folder on `MAYA_MODULE_PATH`:

```bash
cmake --install build/win-maya2022 --config RelWithDebInfo --component GroomLab --prefix "C:/path/to/maya/modules"
```

That writes `GroomLab.mod` and, under `GroomLab/`: `plug-ins/GroomLab.mll`, `scripts/groomlab`,
`python/groomlab_core.cp37-win_amd64.pyd`, `presets`, `expressions`, `themes`, `icons`,
`extensions/groomLabTranslator.dll`, `procedurals/groomlab_procedural.dll` and
`bin/groomlab-cli.exe`. Another Maya version's build (`build/ninja-maya2025`...) installs its
binaries under `GroomLab/maya<year>/` (`plug-ins`, `python`, `extensions`) and its own section of
`GroomLab.mod`; the packaged release holds them all. Always
pass `--component GroomLab`; without it the install also copies Alembic and Imath headers and
libraries. Put the long form of the path on `MAYA_MODULE_PATH`.
Maya's plugin lookup fails on 8.3 short paths like `C:/Users/ABCDEF~1/...`. Then, in Maya:

```python
from maya import cmds
import groomlab

scalp = cmds.polySphere()[0]
guide = cmds.curve(degree=2, point=[(0, 1, 0), (0.6, 1.5, 0.2), (1.4, 1.2, 0.3)])
groom = groomlab.create_groom(scalp, guides=[guide])
cmds.setAttr(groom + ".previewDensity", 1.0)
```

- **The preview is the render, thinned:** `previewDensity` changes only how many strands show.
  A preview at density p shows the strands whose hashed ID falls under p, and each one is the
  render's strand of that ID, bit for bit (points, widths, every channel): clumps, neighbours,
  partners and the rest come out the same at every density. Nodes that read other strands (a
  Clump picking its centers, its axes, Twist's and Curl's clump middles, BlendNeighbors, Tangle's
  partners, a Braid's own paths, Merge's seam, Bands' and Resample's lengths, an expression's
  `$longest`, ToGuides, Plume's feathers lying over others) read only the **core**: the strands
  every preview holds. A Scatter's core is the strands whose hashed ID falls under its core share:
  3% of a big scatter, a power of two more for a smaller one so that it holds at least 5,000
  strands (all of a scatter under 5,000; all of one with `previewAll`, and of guides as strands).
  Below the core share a preview evaluates the whole core and shows only its own share: the core
  strands it does not show are dropped before anything draws, exports or counts them (the GPU
  preview runs them too and writes the shown ones alone). The one difference left is an
  expression's `$index` (a strand's place in the set, which a thinned set changes).
- **Viewport memory:** every node keeps a copy of the strands while previewing, and the draw
  buffers more, so the preview can be held to `previewPointBudget` million strand points (0, the
  default: no limit). Past it Scatter thins the preview on its own (the same strands, fewer of
  them), so raising density or segments can't exhaust memory. Renders and exports ignore it.
- **Even roots:** `Scatter.randomness` (1 by default: random) below 1 spreads the roots evenly
  first (blue noise: roots relaxed apart on the scalp's rest pose until the spacing settles; open
  edges push back, so no row piles up along a hairline), then puts each root that fraction of the
  way back toward its random spot: 0 is fully even, and lowering it evens the groom out smoothly.
  Strands keep their IDs. A preview relaxes every root as the render does (once: the relaxed
  roots are kept, for every preview density and the render alike), so it shows the render's
  positions. Both are local: a root moves by its neighbors alone and stops once it and they have
  settled, so an edit of the scalp moves roots only near it. Viewing a Scatter node draws its roots as dots on the scalp.
- **Root layout:** `Scatter.layout` `random` (the default: `randomness` as above; 0 evenly
  spread) or `uvGrid`: one root per cell of a `gridRows` x `gridColumns`
  grid over the scalp's UVs (0-1), at the cell's center or moved in it by up to `gridRandom` of a
  cell, on the first face (in face order) whose UVs hold it: rows of fur, scales' or feathers'
  tracts. Each root's ID comes from its cell, so the grid keeps its strands while the scalp
  deforms; a density map thins it (each root kept by map / its maximum), and the preview density
  picks among the same IDs. It needs the scalp's UVs (an error without them); overlapping UV
  shells give a cell to the first face only.
- **Fur along the guides only:** `Scatter.guideRadius` (0: off) keeps only the roots within that
  distance of a guide's root, on the rest scalp (the `guides` input: a Guides node, one group
  say), so drawn guides place the fur, a patch, a stripe or a mane, and nothing grows elsewhere.
- **Every root in the preview:** `Scatter.previewAll` (off by default) keeps all of the Scatter's
  roots in the viewport whatever the groom's `previewDensity`: for a few big pieces each strand
  carries (feather shafts, scales, leaves) that a 10% preview would leave bald. The shipped
  feather, scale and instance presets set it.
- **One seed per Scatter:** a strand's ID and root come from the Scatter's `seed`, the scalp
  face and its number on the face, so two Scatters with the same seed grow the same strands.
  The Graph Editor (and `GraphModel.add_node`) gives a new Scatter, Instance, Flyaway or any
  other seeded node a seed no node of its type has, and warns (without stopping the save) when
  two Scatters, Instances or Flyaways share one (`GraphModel.warnings()`). A `Merge` still gives
  the second look's colliding IDs new ones, but their roots coincide.
- **Interpolate blends without seams:** every guide reaches `reach` (1.5) times the distance to
  its `neighbors`-th (3) nearest guide, and a strand inside that reach weighs the guide by
  `((R - d) / (R d))^power`, which reaches zero exactly at the edge of the reach. Guides in
  dense areas reach less far than sparse ones, and a guide's reach does not depend on the
  strand, so the blend is smooth across the scalp: no pops as guides move, no seams where a
  strand's nearest guides change (plain nearest-k inverse distance has both). `neighbors` 1
  gives every strand its nearest guide alone, with hard edges. Guides are sampled at least as
  finely as the strands, so curly guides keep their detail. With `smoothGuides` (on) each guide
  is followed as the smooth curve the viewport draws through its points (a centripetal
  Catmull-Rom spline), so a bend drawn with few points stays round in the hair instead of
  turning at each point. `preserveLength` (on) scales
  each blended strand to its guides' blended length, up to 2x, since blending guides that point
  different ways shortens the result. `blend` `directions` (the default for new Interpolate
  nodes) builds each strand segment by segment along its guides' average direction at their
  average length instead, so strands between guides pointing apart keep their length without
  being scaled. `smoothness` (50%) is the one weighting control most grooms need: lower for
  crisp locks, higher for soft blending; at 50% `neighbors`, `reach` and `power` apply as set.
- **Interpolate follows the skin:** with a scalp connected, each guide's shape is carried from
  its root to the strand's along the scalp: turned from one surface normal to the other where the
  straight line between the roots hugs the skin (almost everywhere: a guide combed back stays
  combed back over a curved head), and through each bend of the path where it goes round a fold,
  a thin flap or the rim of a shell (the two sides' normals face apart, and the turn comes the way
  the skin went). `followSurface` (Follow surface, on) also measures each guide's reach along the
  scalp, so guides do not reach across an ear, a fold or a thin shell to strands that are close in
  space but far along the skin; on a smooth scalp it changes nothing (on the bench heads, points
  move by less than 1e-5 cm). The scalp is welded first, so vertices split along UV seams make no
  seam in the hair. The paths are made once per guide root and reach (about 1-2 ms for 500 guides,
  once a groom: a dab that does not move roots makes none, one that moves a few makes theirs).
- **Interpolate keeps clearance:** a guide's shape is carried rigidly, so where the skin under a
  strand bulges more than under its guide (ears, cheeks, the neck, the back of the skull) the
  strand would go into the scalp. With `keepClearance` (Keep clearance, on; shown with a scalp
  connected, take shape) each strand is walked from the root through the scalp's signed distance
  field (the one Collide asks): a point lower than 1/2048 of the scalp's size above the skin
  (about 0.17 mm on a head, a hair's width or two) is lifted along the field's normal just onto
  the skin and the points after it follow, each segment's length kept; past the bulge each point
  is led back toward its groomed path (the offset across it halved over about 1/32 of the
  scalp's size, about 1 cm on a head), so hair crossing an ear drapes over it, touching it, and
  comes back to its groomed path below it instead of standing off the head. Points already clear
  of the skin are not moved (a strand clear of the skin is the same to the bit); points beside the
  scalp (past an open edge) or deeper than 1/8 of its size (the far side of an open cap) are left
  alone. One guide on a head gives strands that never go into the scalp, without a Collide (on a
  head with an ear, 4265 points inside it carried as they are, 1 keeping clearance). It
  costs about 10-30 ms at 500k strands x 16 points (Interpolate alone about 35-45 ms) and 2-3 ms a
  brush dab at preview density. The walk takes the field's answers alone (where the field does not
  blend, the closest point among the triangles around the closest points it holds there: the
  mesh's own for 99.7% of such points on the app's head), so the GPU preview walks it too, from the
  field as tables (made and uploaded once per scalp: about 56 MB and half a second for a head-sized
  test scalp): strokes and Interpolate's drags run on the GPU from
  Interpolate (the walk about 8-13 ms at 500k strands), its strands the CPU's within the GPU's
  rounding. The field's grid is the world's (half a cell off its whole multiples) and the
  distances go by the scalp's size rounded to an eighth of an octave, so editing one side of a
  scalp leaves the strands on the other side as they were, to the bit. Over a scalp deformed away
  from its rest pose the walk is the CPU's, the nodes after it the GPU's.
- **Interpolate keeps to painted regions:** `regionMap` (Regions; empty, off, by default) names
  regions painted on the scalp in colors (XGen's region maps): a color map from Groom Tools >
  Paint, else a color set of that name on the scalp, else an image file at the scalp's UVs. A
  strand blends only the guides rooted in its own region, weighed as if they were the only guides
  (each guide's reach among its region's), so a region's blend is as smooth as a whole scalp's and
  the edge between two regions is hard, like a part, along an area instead of a line. The regions
  are the distinct colors at the guides' roots (colors within 0.1 of each other in RGB are one;
  a guide in soft paint between two regions, its color a blend of theirs, joins the nearer), so
  no exact colors need painting and every region has guides. A strand joins the region whose
  color is nearest its root's when that is within 0.1, or when its color lies within 0.1 of the
  line between two regions' colors (soft paint between them: the nearer); a strand in a color no
  guide is rooted on (a region painted without guides), or not rooted on the scalp, blends the
  nearest guides of any region, as without a region map. Paint regions in distinct colors (red,
  green, blue, yellow...): a color that is a mix of two others reads as the soft edge between
  them. Each strand's region number goes to `regionChannel` (`region`; -1 for none), which Clump,
  BlendNeighbors and Tangle read through their own `regionChannel` (`region` by default) to stay
  within regions too (the viewport's `strandColorMode` `clump` with `strandColorSource` `region`
  shows a color per region). Parts, `followSurface`, `limitDistance` and `keepClearance` work within each
  region as on the whole scalp. A brush dab reshaping guides binds nothing again; a guide moved
  within its region binds again only the strands near it; moved into another region, every strand
  is bound again, and a strand changing region (a guide rooted in a color no guide was) makes
  Interpolate evaluate in full. The GPU preview blends from the same binding.
- **Segments live on the Resample node:** Scatter only places roots. The **Resample** node's
  `segments` (16 by default; 0 keeps each strand's) is the strands' resolution: strands (or
  guides) through it are resampled evenly to it, and every Scatter grows its strands with it in
  the first place (the most any Resample asks for; 8 when there is none), whichever node the
  viewport shows, so nodes before it, like Interpolate, already work at that resolution. Put it
  after Interpolate (the default graph has it before the Output). Its `mode` is `even` by
  default: every strand gets `segments` (what the GPU look preview hands on); `curvature` spends
  fewer points for render and export: `segments` is the most a strand gets, straighter strands
  get fewer (down to `minSegments`), placed where they bend; `length` gives shorter strands fewer
  ([Segments by length or curvature](game-export.md#segments-by-length-or-curvature-resample-node)).

The groom's graph lives in its `graph` attribute as JSON. `{}` means the default
graph (hair out of the box, `hair_basics` without its clumps: Scalp → Scatter → Interpolate ←
Guides → Length → Noise → HairColor → Width → Resample → Output). Use
`groomlab.get_graph` and `groomlab.set_graph` to edit it; the schema is described in
[graph_io.h](../../engine/core/include/groomlab/graph_io.h).
