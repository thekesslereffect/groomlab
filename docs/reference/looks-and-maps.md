# Looks and maps

The procedural look, color patterns, painted and texture maps, the viewport look and themes.

User guide: [Node graph](https://www.getgroomlab.com/docs/node-graph), [Color patterns](https://www.getgroomlab.com/docs/color-patterns), [Painting maps](https://www.getgroomlab.com/docs/painting-maps), [Rendering](https://www.getgroomlab.com/docs/rendering), [Utilities](https://www.getgroomlab.com/docs/utilities).

## Procedural look

**Strands or guides.** The shaping nodes (Noise, Curl, Smooth, Twist, Bend,
Length, Collide, Gravity, Displace, Mirror, Flow, Cull) take either: fed strands they give strands, fed guides they give
guides, each keeping its ID, root and painted maps. So guides can be shaped in the graph with no
conversion, e.g. `ToGuides -> Noise -> Clump.centers` for wavy clumps. Their ports show the
kind they carry (strands, guides, or grey while unconnected), links are checked against it, and a
link that stops fitting after a change upstream is reported. Nodes that are about strands
(Scatter, Interpolate, Width, HairColor, Braid, Instance, Cards) keep fixed types; ToGuides and
ToStrands convert explicitly.

Look operators are added to the graph between Interpolate and Length. "Along the length"
parameters are ramps, written in JSON as `[[position, value], ...]` from root (0) to tip (1); a
plain number means a constant ramp. Ramps are smooth: a monotone cubic through the keys, flat at
peaks and valleys and never overshooting, so profiles don't put kinks in the hair (two keys give a
straight line). `{"keys": [...], "linear": true}` keeps straight segments (the curve editor's
right-click menu: Linear).

The operators one row each (what each does and its main parameters): [nodes.md](nodes.md).

For multi-level clumping, chain Clumps from small to large: the small clumps form first and the
large ones then carry them whole, so each lock reads as clumps of clumps (as `cinematic_hair` and
`everyday_sim` do). Large first, the small clumps' centers fall without regard to the locks and
pull strands across their borders. Each Clump writes its own `clumpChannel` (say `clumpId` for
the small, `lockId` for the large), which HairColor's `clumpChannel` can share randomness by.

Put `Collide` last when strands are combed or clumped into the scalp. Each point of its walk asks
how far above a surface it is, along the closest point's normal: with `method` `field` (the
default) that is a lookup in the mesh's sparse signed distance field (`distance_field.h`: cells of
about the offset, sampled from the mesh's own closest points where the hair goes and kept across
evaluations, so a brush dab or a param tweak upstream pays only lookups), and points land within a
few percent of the offset of where the exact closest point puts them (micrometres on a head in
centimetres). Where the field cannot tell as the mesh would (deep under the surface, across a
crease, the edge of the search) it asks the mesh. On the app's head at 100k strands a brush dab's
Collide went from 31 ms to 1.6, the first evaluation from 158 ms to 88
(`tools/bench/xgen_compare/results/2026-10-03_collide_field.md`). A scalp deformed away from its
rest pose (each frame of an animation is a mesh of its own) is asked directly, as `exact` does;
followScalp evaluates on the rest pose, which keeps its field.

**Collide with other meshes.** A `ColliderMesh` node brings any mesh the host registers into the
graph, by name (`collider` by default): connect it to Collide's `collider` input and strands are
pushed out of that mesh as well as the scalp, at every point of the same root-to-tip walk (a
shoulder, a collar, a hat). In code the meshes are `GroomData::meshes` (`GroomSession::setMesh(name,
mesh)`, `set_mesh` in Python), each registered as the external `mesh:<name>`; a ColliderMesh with
no mesh of its name yields nothing, and Collide then hits the scalp alone. They are not stored in
groom files. Deformers such as `Noise` and `Collide` stretch or shorten strands as they
push points about; put a `RestoreLength` after them, with the strands from before the deformers on
its `reference` input, to get the lengths back.

## Color patterns

Two color nodes after a HairColor color more than a tone a strand
(`engine/core/include/groomlab/color_patterns.h`): **Bands** (fur) and **FeatherColor** (feathers).
Each recolors what comes in (HairColor's colors, else the pigment's) and writes the RGB per point,
so the viewport, the app, Arnold (`hair_color`, varying), the cards' Color bake and Unreal's
`groom_color` all show it.

- **Bands along the strand** (Bands: agouti, ticked, shaded fur). `strength` (0-1; mappable,
  per point) mixes the band pigment (`melanin`, `redness`, `dye`, offset per strand by up to
  `randomness`, drawn as HairColor draws its own) in where `profile` says: one period of the
  pattern, root to tip, 0 the incoming color and 1 the band's, repeated `count` times along the
  strand; before the first period and past the last its ends hold, so a single band moves along
  rather than wrapping. Per strand (by ID) the pattern shifts by up to `offsetRandom` of a period
  and stretches by up to `widthRandom`, so bands do not line up across the coat. The bands go over
  the incoming per-point colors, a tip too: keep the profile at 0 near the tip for a dark tip
  past an agouti band. By point index, as `tipProfile` (an even Resample makes that the length).
- **Guard hairs and undercoat.** `guards` (1: all) is the share of strands that are guard hairs:
  they take the bands and keep the tip color; the rest is the undercoat, the incoming strand color
  times `undercoatDye` at every point (no tip color). `guardsBy` `random` picks them by ID; `longest` takes the longest `guards` of
  the strands (by arc length, the length that parts them measured on the core strands, the same at
  every preview density: a pass over every strand, so the node then evaluates in full on a
  stroke, and the GPU look preview leaves drags of the nodes before it to the CPU,
  `look_chain.cpp`).
- **Feather patterns** (FeatherColor's `pattern`), drawn in vane space (Propagate's `vaneU`,
  `vaneTipU`, `vaneTipV`: the feather laid flat, see [Feathers](feathers-braids-scales.md#feathers)), so a bar crosses many barbs, each
  at its own place along the barb. Across the vane, `side x v` is divided by 3 (`kVaneAspect`, a
  typical feather's length over its half-width) before a shape is drawn, so shapes keep their
  proportions on such a feather. `bars` (straight across, tilted by `angle` degrees) and
  `chevrons` (a V mirrored across the shaft, its arms at `angle`): `count` per feather length,
  `width` the share of each period; `spots`: one in every cell of a grid `count` cells a feather
  length, moved within its cell, `width` across; `fringe`: the vane's edge, the last `width` of
  every barb; `eye`: centered on the shaft at `offset`, `width` of the feather's length across
  (`angle` tilts an oval one). `offset` shifts bars, chevrons and spots too; `profile` (a ramp
  from a shape's center to its edge) shapes them inside (rings of an eye spot, a hollow bar),
  `sharpness` their edges. Per feather (from its shaft's root on the scalp, stable)
  `offsetRandom` shifts the pattern by up to that share of its spacing (of its size for an eye),
  `scaleRandom` scales it, `angleRandom` turns it (degrees). `texture`: a painted image of the
  feather, the `texture` param (the whole feather laid flat: the shaft up the middle, the left
  vane on the left, the root at the bottom; read at `s = 0.5 + 0.5 side v`, `t = u`, its alpha
  the cover), or with `textureRight` one image per vane (`s = v`, the shaft along each image's
  left edge). A pattern takes its `melanin`, `redness` and `dye` (an image its own RGB) by
  `strength`, on the `vanes` named (left and right seen from the vane's top, the tip up; the
  shaft and barbs all around count as both). Strands without vane space (hair merged in) take
  none. One image per feather design: the FeatherColor sits inside the design's compound; the
  file's stamp joins the cache key, and Find Missing Files relinks it.
- **The down zone's color.** FeatherColor's `downStrength` mixes `downColor` into the barbs of a
  Propagate's down zone (`isDown`).
- **Iridescence** (structural color, angle-dependent). FeatherColor's `iridescence` (0-1; mappable, per point;
  where: `iridescenceOn` `all`, `leftVane`, `rightVane`, or where the feather pattern is,
  `pattern`, or is not, `notPattern`) writes the per-point `iridescence` channel; the shading
  mixes the fiber's color and its highlight's tint toward `iridescenceColor` with its hue turned
  by `iridescenceShift` turns times `1 - |V . N|` (N: the strand's normal, its vane's on a
  feather, Propagate's `vaneNormalX/Y/Z`, else the scalp's at the root, leaned along the strand as
  the viewport's strand frames are), `iridescentTint`. The viewport's strand
  and card fragments (`groomLabHairColor`, `groomLabCardColor`: the amount in the colors' alpha
  past 1, the view and normal routed through pass-throughs, both APIs), the app's hair shader
  (both shading paths: the per-point colors texture's alpha) and Arnold all shade it; the node's
  color and shift are the first iridescent FeatherColor's making the output
  (`GroomSession::hairLook`). Arnold: `create_hair_shader` builds the approximation the
  translator can drive, a ramp of five keys of the tint on `|Rd . root_normal|` (`aiStateVector`
  Rd, the `root_normal` user data, `aiDot`, `aiAbs`, `aiRampRgb`), mixed into `baseColor` and
  `specularTint` by the `iridescence` user data (`blendColors`); it takes the normal unleaned
  and a linear ramp between the keys (`Rd` is each ray's own direction, reflections' too), so it
  lands within a few hue degrees of the viewport face-on and edge-on and drifts a little between.
  Colors given as RGB (bands, patterns, tips) render lighter and less saturated in Arnold than
  in the app and the viewport: `aiStandardHair` turns its base color into an absorption and adds
  multiple scattering, where the app's hair shader takes the color as the fiber's transmittance.
  The patterns land in the same places; their contrast is lower.

**What each target gets.** Unreal strands: `groom_color` per vertex, `groom_iridescence` (and
`groom_ao` with `iridescence_as_ao`; [docs/UNREAL_HAIR_MATERIAL.md](../UNREAL_HAIR_MATERIAL.md)
is the material). Cards (Unreal, Unity, glTF): the patterns baked into the Color map, a feather's
card being its vane laid flat; the iridescence in Unreal's Color alpha and Unity's Mask red,
not in glTF (its `_BaseColor` alpha is the coverage). Unity strands and generic Alembic: `Cd` per
strand, so bands and patterns along a strand do not travel there, nor the iridescence. USD:
`displayColor` per strand, and every point channel (`colorR/G/B`, `iridescence`) as a vertex
primvar.

The presets `agouti_fur` (HairColor -> Bands: banded guard hairs, a black tip, a
plain undercoat) and `feathers_barred` (HairColor -> FeatherColor inside the design:
white bars across the left vane, an iridescent green right vane, a white downy base) put them
together; `feathers_spotted`, `feathers_eye_spot` and
`feathers_iridescent` show the other patterns.

## Maps

Any parameter marked *mappable* can take a map instead of a number. The map's 0–1 value (one
component: `r`, `g`, `b`, `a` or `luminance`) is remapped to `[min, max]`:

```python
groomlab.set_param(groom, "scatter", "density", groomlab.texture_map("C:/maps/density.png", max=300))
groomlab.set_param(groom, "clump", "strength", groomlab.color_set_map("clumpMask"))   # painted on the scalp
groomlab.set_param(groom, "noise", "amplitude", groomlab.attribute_map("frizzy", max=0.05))
```

| Map | Reads | Needs |
| --- | --- | --- |
| `texture_map` | An image at each root's UV. Formats: PNG, JPEG, TGA, BMP, PSD, GIF, HDR, PNM (not EXR/TIFF yet). 8-bit values are used as stored; maps are data, not colors. | Scalp UVs (current UV set) |
| `color_set_map` | A vertex map (Groom Tools > Paint) or a Maya color set on the scalp, interpolated at the root | The map or color set |
| `attribute_map` | A per-strand channel written upstream, e.g. by `Attribute` or guide attributes; a **point channel** of the name (a per-point painted map, HairColor's colors) is read per point instead. Before strands exist (Scatter density) it reads the painted map of that name. | The channel, or a painted map with the guides connected (`set_param` and the editor connect them) |
| `expression_map` | A per-strand expression (see [Expressions](nodes-and-fields.md#expressions): a block with locals, names with or without `$`), e.g. `smoothstep(-1, 1, $y)` or `$clumpId > 10 ? 1 : 0.3`; per point with `t`, `pt`, `px py pz`, `pointIndex` or a point channel, e.g. `1 - t` | The channels it names |
| `guide_map` | A guide layer (a map of kind guides, see Painted maps below; "painted map" in the Graph Editor): the strand channel after Interpolate, or the nearest guides blended at the root before it | Guides with the layer; `Scatter` needs its `guides` input |

**Painted maps (Groom Tools > Paint).** A map is a named 0-1 value (gray) or color (r g b a)
painted on the scalp, of one of three kinds, all painted with GroomLab's own Paint brush (no
Maya paint tool, no other package) while the groom follows each stroke:

| Kind | Lives | Paint over | Best for |
| --- | --- | --- | --- |
| **guides** | A layer on the guides (children inherit it, strands blend it from their guides) | the guides | regions, density, clumping; anything that should follow the guides |
| **vertices** | A value per vertex of the scalp mesh, kept with the groom in the scene (`scalpMaps`) | the scalp | detail finer than the guides, with nothing to keep on disk |
| **texture** | A gray (or, for a color map, RGBA) PNG at the scalp's UVs in the scene's masks folder, `groomlab/<scene>/masks/<groom>/` (stored relative to the scene) | the scalp | the finest detail; editing in an image editor; exporting to an engine |

A map **converts** between kinds at any time (**Convert to** in the Paint tab,
`groomlab.convert_map`): it is resampled onto the guides' roots, the vertices or the texels, and
its name stays, so whatever reads it keeps reading it. Rough a mask in on the guides, then turn
it into a 4k texture for Unreal.

All three kinds **survive a new scalp** ([Transfer to a new scalp](pipeline.md#transfer-to-a-new-scalp)): guide layers ride with the
guides, and vertex and texture maps are projected onto the new mesh by position (a new image
for a texture map; the old file is left as it is).

**Color maps.** A vertex or texture map can hold a color: red, green, blue and alpha (a guide
layer is gray). Make one with **New Map > Values: Color** (`add_map(..., color=(r, g, b))`) or
turn a gray map color (**Make Color** on the map's menu, `set_map_channels(groom, name, 4)`:
the value in r g b, alpha 1; back to gray keeps the luminance). The Paint tab then shows a color
swatch (click to pick) and **R G B A** toggles: the brush paints the color into the ticked
channels only and leaves the others as they are, Ctrl paints toward the inverse color (1 - r,
1 - g, ...) in those channels, as a gray map goes to 1 - value, Shift+drag smooths them, and
**Fill** fills the color there. A color map converts between vertices and texture with its
colors; onto the guides it keeps its luminance (the command warns). Two uses:

- **Packed masks.** Paint up to four masks in one map, one per channel, and read each from its
  output of the map's Map node (`r`, `g`, `b`, `a`): R the clump mask feeding Clump's strength,
  G the frizz mask feeding Noise's fraction, B the length mask feeding Length, all from one node. As a texture, that is one RGBA PNG to
  ship to an engine instead of three gray ones (`groomlab.export_map(groom, "packed", path)`
  writes any map as a PNG). `use_map(groom, "clump", "strength", "packed", channel="r")` and
  `paint_param(..., kind="texture", name="packed", channel="g")` set it up.
- **Painted hair color.** A HairColor node's `map` names a color map: its color at each root
  replaces the pigment's color (or multiplies it, `mode`), so painted color reaches the
  viewport, the cards' Color bake, Unreal's `groom_color`, USD `displayColor`, Alembic `Cd` and
  Arnold's `hair_color`. A name that is no painted map reads the scalp's color set of that name,
  else an image file at the scalp's UVs. The Graph Editor offers the groom's color maps and the
  scalp's color sets there.
- **Painted regions.** An Interpolate node's `regionMap` names a map painted in a color per
  region (red, green, blue...): each strand takes only the guides rooted in its own region, with
  a hard edge between regions, and Clump, BlendNeighbors and Tangle after it stay within regions
  too (see Interpolate keeps to painted regions in [Getting started](getting-started.md)). Soft paint needs no care: the regions are the
  colors at the guides' roots. The Graph Editor offers the groom's color maps and the scalp's
  color sets for it, as for HairColor's map.

The scalp shows a color map in color; **Show on Scalp**'s channel list (`scalpMapChannel`,
`show_map_on_scalp(groom, name, channel="g")`) shows one channel black to white. Scenes and
files from before keep working as they are: a map without `"channels"` in `scalpMaps` is gray,
and a color PNG read as a gray map is its luminance.

The graph reads a map through a **Map** node (Graph Editor > Add Node > Fields & Curves, or
right-click a mappable param > **Paint** > On Guides / On Scalp Vertices / As Texture): `name`
names the map and nothing else is set on the node: the groom tells the graph where each map lives
and, for a texture, its file, so converting a map changes nothing in the graph; a `name` that is
an image file's path reads that image at the scalp's UVs. `min` and `max` reshape it (min above
max inverts it). **Feed Param** on
the node lists every node's mappable params; pick one and the map feeds it (the param is exposed
as an input and connected; the panel warns while the node feeds nothing). The map stays 0-1
and the param's own value multiplies it: white on a density of 100 is 100, black is 0, and one
map can feed a density, a length and a width at once with each amount on its own param.
**Paint...** on the node opens Groom Tools > Paint on the map.

```python
groomlab.add_map(groom, "fur")                                   # a guide layer, 0 everywhere
groomlab.add_map(groom, "crown", kind="vertices")                # per scalp vertex
groomlab.add_map(groom, "detail", kind="texture", resolution=2048)  # groomlab/<scene>/masks/<groom>/detail.png
groomlab.paint_map(groom, "fur", value=1.0)                      # the Paint brush, the map shown
groomlab.use_map(groom, "scatter", "density", "fur")             # a Map node feeding density (x its 200)
groomlab.use_map(groom, "length", "keep", "fur")                 # the same node feeds Length's keep too
groomlab.paint_param(groom, "noise", "amplitude", kind="vertices")  # map noise_amplitude, white, feeding it; Ctrl lowers
groomlab.fill_map(groom, "fur", 0.5, selected_only=True)
groomlab.convert_map(groom, "fur", "texture", resolution=4096)   # the same map, now a PNG
groomlab.rename_map(groom, "fur", "coat")                        # Map nodes, params and views follow
groomlab.remove_map(groom, "coat")
groomlab.maps(groom)                                             # [{"name", "kind", "texture", "channels"}]
groomlab.add_map(groom, "packed", kind="texture", color=(0, 0, 0))  # a color map (RGBA PNG), black
groomlab.paint_map(groom, "packed", color=(1, 1, 1), channels="r")  # paint the r mask only
groomlab.use_map(groom, "clump", "strength", "packed", channel="r")   # from the Map node's r output
groomlab.use_map(groom, "noise", "fraction", "packed", channel="g")   # the same node's g
groomlab.fill_map(groom, "packed", color=(0, 0, 0, 1), channels="b")  # clear one channel
groomlab.map_channels(groom, "packed")                           # 4 (1 for a gray map)
groomlab.set_map_channels(groom, "fur", 4)                       # gray -> color (not on the guides)
groomlab.set_param(groom, "color", "map", "dye")                 # painted hair color (the HairColor node)
groomlab.export_map(groom, "packed", "C:/engine/packed.png")     # any map as a PNG
groomlab.set_map_texture(groom, "fur", "C:/art/fur.psd")         # read an image: a PNG copy of it
groomlab.reload_map(groom, "fur")                                # after editing it outside Maya
groomlab.missing_files(groom)                                    # {"maps", "params", "attributes"}
groomlab.find_missing_files(groomlab.list_grooms(), "D:/recovered")  # relink by file name
```

**A map's image.** `set_map_texture` (right-click a map > **Relink Image...**) points a map at an
image. A PNG is linked in place, stored relative to the scene, and painting edits it. Any other
format (JPEG, TGA, PSD, BMP, GIF, HDR...) is copied into the masks folder as a PNG
(`masks/<groom>/<map>.png`, a free name when taken) and the map reads the copy: painting writes
PNG, so the source is never painted over (`copy=True` copies a PNG too; a map that names another
format refuses to be painted or filled). A guide layer or vertex map becomes a texture map. The
map keeps its channels: a gray map reads the image's luminance, a color map its r g b a. The
relink is one undo step, and undo removes the copy. `reload_map` (**Reload Image**) makes the
groom read its images again after an edit in another program; files that did not change stay
cached. `missing_files` lists what a groom names that is not there: texture maps, graph params
naming files (texture maps, a HairColor `map` or Map `name` naming an image, FeatherColor's
images, ImportCurves' Alembic file) and the guide cache
and groom file. `find_missing_files` (GroomLab > Utilities > **Find Missing Files...**) relinks
them to files of the same name in a folder and its subfolders, in one undo step: names compare
as the file system does (without case on Windows), the file whose folders match the stored
path's wins when several share a name, and a name that still fits several is reported as
ambiguous, not guessed. The Paint tab marks a map whose image is missing, and the Map node's
panel offers **Relink Image...**.

- **Groom Tools > Paint** has it all: the Paint brush, the list of maps with their kinds (a
  swatch marks color maps), New... (name, kind, gray or color, a texture's resolution), rename,
  **Convert to**, **Make Color** / **Make Gray** and delete (the small buttons over the list, or
  right-click a map), Value and Fill (a gray map) or the color swatch, R G B A and Fill (a color
  map), Fill Selected (guide layers), and Show on guides / strands / scalp, with the channel a
  color map shows ("Read by" names the params the map feeds). **Unfreeze All** is in
  the Freeze tool's settings. Painting a scalp map shows it on the scalp, black to white, and
  the groom updates with every stroke (a texture map's file is rewritten when the stroke ends);
  Ctrl paints toward 1 - value, Shift+drag smooths the map (blends each vertex or texel with
  its neighbors, by the **Smooth** strength under the tabs, in passes past 1;
  `brush_stroke(..., "smooth", attribute=name, paint_kind=..., smooth_strength=2)` by script);
  **Falloff** (every brush) fades the brush toward its edge (1) or paints full strength to it (0;
  `hardness` in scripts); each stroke is one undo step, a texture's file included.
- **Brush alphas** (Paint on a vertex or texture map): a grayscale image stamped inside the brush
  circle (white paints, black not), the falloff times the image where a point lands; the image
  fills the square around the circle. Picked from the library: `GROOMLAB_ALPHA_PATH`'s folders,
  the user's `Documents/GroomLab/alphas` (`GROOMLAB_USER_ALPHAS`; every host's), then the ones
  shipped with the host (`alphas/` beside the plug-in, the add-on's `bin/alphas`, the app's
  `alphas/`; drawn by `tools/dev/make_alphas.py`); a name in an earlier folder hides a later one,
  and Add Alpha... numbers a copy whose name is taken. Any image `loadTexture` reads; its alpha
  channel when it has transparency, else its luminance; mip levels so a small brush reads it
  without shimmer. Stamps fall `alpha_spacing` x the diameter apart along the cursor's path (in
  pixels; without an alpha, one dab per mouse event as before), each turned by `alpha_angle`:
  `alpha_rotation` "fixed", "stroke" (the stroke's screen direction plus the angle; a click alone
  stamps on release) or "random" (per stamp). `alpha_invert` uses 1 - the image. Symmetry's
  mirrored stamp weighs each point by the stamp at its mirror image (the pattern mirrored, also
  across topology). Scripted strokes (world points) and smoothing take no alpha.
  `groomlab.brush_tool("paint", alpha=path, ...)`, `brush_alphas()`, `add_brush_alpha(file)`,
  `brush_alpha_folder()`; the context's `-alpha`, `-alphaRotation`, `-alphaAngle`,
  `-alphaSpacing`, `-alphaInvert`.
- **No seams in painted maps:** every kind of map is painted on the surface, not in UV space. A
  dab reaches the scalp within its sphere that is connected to where it lands: both sides of a
  UV seam, never across a gap to another part of the scalp (the back of an ear). Smoothing a
  texture map blends each texel with its neighbors on the surface, so at a UV island's edge it
  blends with the texels across the seam in the other island; a vertex map's points split along
  a seam smooth as one. The viewport's live image of a texture stroke pads each island's edge 8
  texels out with its nearest texel (as the saved PNG pads), so filtering shows no dark lines.
- **Density from a map:** a Map node feeding `Scatter` runs before strands exist; a guide layer
  there blends the nearest guides at each root (the node gets the guides connected). Strokes
  then re-scatter, which costs some viewport speed on dense grooms.
- **Errors:** a map no guide carries, a vertex map painted on a scalp with another vertex count,
  or a missing texture file is an error on the Map node, not a silent 0.
- **Other map sources** stay: `guide_map(name)` reads a guide layer straight into a param,
  `color_set_map(name)` a vertex map or a Maya color set of the scalp, `texture_map(path)` any
  image, `attribute_map(name)` a strand channel (`paint_scalp_map` makes a vertex map and an
  Attribute node writing it into a channel of the same name).
- **Commands:** `groomLabScalpMaps -list | -kindOf | -channelsOf | -make <name> -kind <kind>
  [-color [-fillColor r g b a]] | -remove | -rename | -fill (-value v | -fillColor r g b a
  [-fillChannels rgba]) | -convert | -setChannels <name> 1|4 | -values`; the brush's
  `-paintColor r g b a` and `-paintChannels rgba` (stroke command and tool); and `groomLabGuides -listAttributes`, `-removeAttribute <name>`,
  `-renameAttribute <old> <new>` for guide layers.

**Per-point painted maps (along the guides).** A painted map can also hold one value per guide
point, painted along the guide: the Paint brush with `pointwise` set moves each point under it
toward the value by the brush's falloff there (a band along the guide), starting a guide's layer
from its per-guide value where it has none. Children take their parent's layer, resampled along
them, unless they carry their own; mirroring, transfer, rename and delete cover both kinds, and
`-listAttributes` lists both. `Interpolate` blends per-point layers into **point channels** of
the same name on the strands (each guide's layer by arc length, with the same guide weights),
and any `attribute_map` of that name then reads the strands per point (see below). In Python:
`HierarchyGuide.point_attributes`, `GuideHierarchy.set_point_attribute(id, name, values)`,
`Guide.point_attributes`, `BrushSettings.pointwise`.

- **Mappable parameters** include `Scatter.density`, `Clump.strength`, `Noise.amplitude`,
  `Curl.radius`, `Noise.fraction`, `Length.keep`, `Width.width`, `Blend.mix`,
  `HairColor.tipStrength`, `Flyaway.fraction` and `stray`, `Tangle.fraction` and `strength`,
  `HairColor.melanin`, `redness`, `greys`, `specular` and `roughness`, `Bands.strength`,
  `FeatherColor.iridescence`, `RenderData.opacity`, `Attribute.value`.
- **Per-point maps:** a map varies along each strand when it is an `attribute_map` naming a
  **point channel** of the strands (a per-point painted map after Interpolate, HairColor's
  `colorR/G/B`), or an expression using a point variable or point channel, or a field node built
  on one. `Width.width`, `Clump.strength`, `Noise.amplitude`, `Curl.radius`,
  `Blend.mix`, `HairColor.tipStrength`, `Bands.strength` and `FeatherColor.iridescence`
  read such maps per point inside their loops (per strand
  otherwise, at the same cost as before); other params read the root's value.
- **Width:** `width` (scene units, mappable per point) times `profile`, a ramp along the length by
  arc length (default 1 at the root to 0.3 at the tip). `{"expression": "1 - t", "max": 0.02}` on
  `width` with a flat profile tapers every strand from 0.02 to 0 along its arc length.
- **Density maps:** strands are scattered at the map's maximum density and each is kept with
  probability `map / max`, decided by its stable ID. Painting more density adds strands without
  moving existing ones.
- **Caching:** textures are cached and re-read when the file changes on disk; editing a map image
  updates the groom.
- **Expressions:** they compile once and run per strand, or per point when they use a point
  variable or point channel.
  - **Variables:** `u v` (root UV), `x y z` (root position), `nx ny nz` (root normal), `rand`
    (stable per strand), `index`, and any strand channel. The strand's shape: `length`,
    `longest` (the set's longest strand, of its core strands: the same at every preview density;
    `index` is the one variable a thinned preview changes), `reach` (root to tip), `straightness` (reach / length),
    `lift` (1 straight out of the scalp, 0 along it) and `tipx tipy tipz`; they read 0 where
    strands don't exist yet (Scatter density). Along the strand (per point): `t` (0 at the root to
    1 at the tip by arc length), `pt` (the same by point index), `px py pz` (the point's position),
    `pointIndex`, and any point channel by name (a strand channel of the same name wins). Where a
    param reads per strand, the point variables read the root's. A channel named like one of these
    is shadowed.
  - **Language:** SeExpr's, as Maya XGen uses it: `+ - * / % ^`, comparisons, `&& || !`,
    `c ? a : b`, the apply operator `x->f(a)`, `+=` and the like, `if (c) { ... } else { ... }`.
  - **Functions:** SeExpr's builtins (`noise snoise fbm turbulence voronoi cellnoise
    smoothstep(x, a, b) fit remap curve spline pick choose hsi rotate`...): the full list and what
    differs from XGen are on the website's Expressions page (web/content/docs/expressions.mdx).
  - **Errors:** a typo shows on the node with its position (`groomlab.errors`).
  - **The editor:** its map editor has an expression source.

On 24 cores, 100k strands × 16 points through Scatter, Interpolate (300 guides),
Clump (picking its own centers), Noise, Curl and Width evaluates in about 90 ms.

## Viewport look

The viewport can color strands and draw them as ribbons. Neither re-evaluates the groom:

| `strandColorMode` | Colors strands by |
| --- | --- |
| `solid` | One hair color (the default) |
| `colorSet` | The scalp color set `strandColorSource` at each root |
| `attribute` | The channel `strandColorSource` over [`strandColorMin`, `strandColorMax`], blue to red |
| `random` | A random color per strand |
| `clump` | A random color per value of the channel `strandColorSource` (default `clumpId`), one per clump; with `region`, one per painted region (Interpolate's `regionMap`) |
| `density` | Root density against the groom's median: blue a quarter or less, green typical, red 4x or more (bald spots, bunched roots) |

The Attribute Editor shows `strandColorMode`; `strandColorSource` is set by the Groom Tools (the
Paint tab's map display) or by script.

`displayRibbons` (off by default) draws strands at their true width: ribbons facing the camera as
wide as their `Width`, times `ribbonWidthScale`, never thinner than a pixel, the parts thinner than
a pixel fading with their width as the lines do. Use it for width and density look checks; it is
slower than lines close up (every pixel a strand covers is shaded).

`displayWidthCoverage` (on by default) draws the strand lines with their widths too: they stay a
pixel wide, and each point covers the share of the pixel its width (times `ribbonWidthScale`)
covers at its distance from the camera, so tapered tips fade, thin strands read lighter, and
strands a pixel wide or more stay solid: hair as dense as it renders, not as dense as a pixel-wide
line of every strand would make it. The coverage is drawn stochastically (hashed alpha testing):
each strand's pixel is drawn whole or not by a threshold of that strand's own, so the many strands
behind a pixel of a groom seen from afar add up as they do in the render instead of the front one
hiding the rest, and the pattern stays put on the strands (the same frame draws the same picture;
the camera moving carries it along). With the viewport's multisample anti-aliasing on, each strand
covers its share of a pixel's samples instead, which reads smooth; without it, strands thinner than
a tenth of a pixel stipple, and those covering much of a pixel (seen close) blend smooth. Strands
without widths (no Width node) draw opaque. Flat, colored and lit lines alike, on DirectX 11 and
OpenGL. `displayCoverageDensity` (on by default) takes a thinned preview to the render's density:
each strand a preview at `previewDensity` 0.1 shows stands for ten, and covers what ten would
together (never more than its pixel), so the preview reads as dense as the render (the whole groom
at the preview density's share: a source the preview never thins, imported curves or a Scatter with
`previewAll`, reads denser than it renders; a point budget's further thinning, lighter). Off,
every strand draws opaque, as before. In the Attribute Editor under Display > Strand Shading.

In `solid` mode strands and ribbons are lit (`displayLighting`, on by default) with GroomLab's own
hair shading (`displayHairShading` GroomLab, the default): Marschner's three lobes (the surface
highlight, the light through the fiber, the colored highlight off its back) from the strands'
pigment and the HairColor node's specular, roughness and tilt, as Arnold's `aiStandardHair` and the
standalone app show them, lit by the viewport's lights (directional, point, spot and area lights,
ambient lights; their color, intensity, decay and cone; light linking through the groom's shading
group or a light not lighting everything). The hair shadows itself (`displaySelfShadowing`, 1: as
opaque as rendered; 0 off): the GPU keeps a grid of its density, written again whenever the strands
move, and each strand is lit through the hair between it and each light, with the light scattered
on through that hair. Each strand shades a little apart from its neighbors
(`displayStrandVariation`: its highlight's tilt, width and strength and its pigment, from its ID;
0 none), on top of HairColor's specular and roughness randomness. The viewport's surfaces show a
light's intensity times their color, so the hair is drawn at pi times the render's radiance to sit
with them. `displayHairShading` Maya draws them as before with Maya's hair tube shading: Lambert
diffuse plus highlights along the strand, with screen-space ambient occlusion when it is on. With
`useMaterialColor` on (the default) and no material, or the `aiStandardHair` `create_hair_shader`
made from the groom, each strand shows its own color, as the render does: HairColor's map (a
painted map, a color set or an image), randomness and tip color, and Bands' and FeatherColor's
colors, per point along the strands where they vary (strands without color channels: `displayColor`). Another assigned material colors
every strand alike (an `aiStandardHair`'s `melanin` and `melaninRedness` approximated, or a
`baseColor` or `color`); with `useMaterialColor` off they all take `displayColor`.

| `displayShadows` | Strands |
| --- | --- |
| `Off` | Take no part in shadows |
| `Cast` | Cast shadows on the scalp, body and floor (the default) |
| `Cast and Receive` | Also shadow each other; dense strands speckle in a shadow map (Maya's hair shading; GroomLab's takes its own, `displaySelfShadowing`) |

The color modes stay flat: they are for reading data. At 200k preview strands the viewport orbits
at about 200 fps as lit lines and about 65 fps as lit ribbons with self-shadowing and ambient
occlusion, measured by the GUI viewport test on an RTX 3090.

**Guides.** Each guide group, child groups too, is drawn in a hue of its own, from a hue of the
groom's own (its name), so a parent and its children, siblings and two grooms differ; a group's
own color (`guideGroupColors`, the Groom Tools' swatch) wins. **Guide Color** (`guideColorMode`)
is `Group` (the default) or `Painted Layer`: a painted guide layer (`guideColorLayer`) from
black to white. `guidePoints`
draws round dots on each guide, in the guide's own color: `all` (the default), `roots and
tips`, or `off`: one dot on each point the guide has. Dots are `guideWidth` plus the theme's `pointSize` across, keep their size
on screen and hide behind the head; on a groom with many guides they thin to about 2,500
(roots and tips always). To rebuild the guides themselves to a point count, use
the **Segments** slider (Groom Tools > Tools, Guide groups & edits) or `groomlab.resample_guides` (points spaced along
the smooth curve through the old ones, so bends stay round).

## Themes

GroomLab's look comes from a theme: the Groom Tools and Graph Editor (colors, rounding, node
style, slider colors) and the viewport's guide, dot, selection, part and flow colors. Pick one from
the **Theme** menu (in the **...** menu of the Groom Tools or of the Graph Editor); the
choice is remembered.

- **Charcoal** (the default): calm neutral greys with one blue accent, thin borders, small
  corners; nodes with a colored dot by the title (no header fill) on a dot grid.
- **Playful**: charcoal with red, yellow, blue, emerald and white; flat, round, nodes with
  solid colored headers; tool icons emerald (create), blue (shape), red (trim) and yellow
  (paint). Every theme extends it.
- **Studio**: low-key greys with one quiet accent; small corners, tinted nodes, muted guides.
- **Polar**: near-black and monochrome after polar.sh (Night, Ash, Mist and Snow) with Ether
  indigo as the one accent; soft corners, tinted nodes, grey tool icons.
- **Your own:** **Theme > New Theme from Current...** copies the active theme into your themes
  folder (`<Maya user app dir>/groomlab/themes`) and opens it; edit the JSON, then **Theme >
  Reload Theme**. A theme may set only what it changes and `"extends"` another (default
  `playful`). Colors are hex (`"#2F7BF6"`), a name from its `palette` (`"blue"`), or `"accent"`.
  `GROOMLAB_THEME_PATH` adds folders (a studio's shared themes).

```json
{
  "name": "Night Owl",
  "extends": "playful",
  "palette": {"blue": "#7C5CFF"},
  "ui": {"background": "#141418", "radius": 16},
  "graph": {"headerStyle": "filled", "categories": {"Look": "red"}},
  "viewport": {"frozen": "blue", "pointSize": 7}
}
```

The keys: `palette` (named colors); `ui` (`background`, `panel`, `raised`, `hover`, `border`,
`text`, `textDim`, `textMuted` (the quietest text, section headers; `textDim` without it),
`accent`, `accentText`, `selection` (selected rows and the active tool's tile; the accent
without it), `warning`, `error`, `keyed`, `animated`, `driven`, `radius` (buttons and fields;
small parts 2 px less, panels 2 px more), `fontSize`, `rowHeight` (the height of every field,
combo box and button; their text's without it), `sliders`: the colors sliders cycle through);
`graph` (`canvas`, `grid`, `node`, `nodeBorder`, `text`, `textDim`, `selected`, `cornerRadius`,
`headerStyle` `filled`, `tinted` or `minimal` (a dot by the title), `canvasStyle` `dots`, `lines`
or `plain`, `shadow`, `error`, `warning` and `note` for node badges and status lines, `driven` for a
param a field or another node drives, `reads` and `writes` for the attribute chips, `categories`
by node category, `ports` by port type); `icons` (a color per tool group: `create`, `shape`, `trim`, `paint`, and `app` for the window icons; `ink` for hair and neutral parts; a shade and a tint of each come with it; a checked tool keeps its icon, its tile in the selection color with an accent border); `viewport` (`pointSize`,
`selected`, `softSelected`, `frozen`, `parts`, `flow`, `flowField`, `tubes`, `instances`).

```python
from groomlab.ui import theme
theme.list_themes(); theme.set_theme("studio"); theme.create_theme("mine"); theme.reload()
```
