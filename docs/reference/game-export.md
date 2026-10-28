# Game export

Game cards, feather cards, Resample by length or curvature, and exports for Unreal, Unity and glTF.

User guide: [Game export](https://www.getgroomlab.com/docs/game-export).

## Game cards (live)

Groom the hair as usual, then turn it into cards for a game engine that keep its look: the
cards follow its clumps and shape, and their textures are baked from its own strands (color,
coverage, direction), so the carded groom reads like the groom.

Add a **Cards** node (Graph Editor > Add Node > Games) where the look should be carded, or run
`groomlab.create_cards(groom, lod="mid")`, which adds one before the output and shows the cards.
The node's panel shows what its own cards cost (cards, triangles, textures) and has **Show cards**, **Show textures**,
**LOD** (Hero, Mid, Low), **Bake Maps**, **Bake Scalp** (both into the scene's `groomlab/<scene>/cards/<groom>`
folder), **Export Unreal Cards...**, **Export Unity Cards...** and **Export glTF Cards...** (Game cards for Unity,
Game cards as glTF, below).
- **The groom draws the cards; no mesh needed:** **Show cards** (the groom's `displayCards`;
  the A/B, `groomlab.compare_cards(groom)`) shows the cards *instead of* the strands; off, the
  strands. Guides stay either way. Both ways they draw with the strands' own hair shading
  (Maya's hair tube shading: diffuse, highlights along the card, translucence, the scene's
  lights and shadows, the viewport's ambient occlusion and other post effects, two-sided).
  Before a bake they are solid in the theme's card color; once baked, with the maps: the baked
  color x AO, cut out by
  the coverage map (an alpha test at 0.5, as masked cards are in engines: hard, depth-correct
  edges, nothing to sort), so the hair's silhouette shows instead of solid quads. With the
  viewport's **multisample anti-aliasing** on (Hardware 2.0 settings), the coverage becomes the
  share of each pixel's samples the card covers instead (alpha to coverage), so the cut-out edges
  and the thin strands are smoothed as the strands are. Their **shadows** are cut out the same
  way (whatever Transparent Shadows says). The viewport's **ambient occlusion** leaves the
  textured cards out: Viewport 2.0 draws the occlusion's depth with a shader of its own, which
  would darken boxes the shape of the cards' quads; the maps carry the bake's AO (Maya 2022 gives
  a plug-in's item no way to cut that pass out; for Maya's own transparency handling, make a
  live card mesh with `groomlab.create_card_mesh(groom)`, which gets a lambert with the maps once
  baked). **Show textures** (`displayCardTextures`, on by default;
  `groomlab.card_display(groom, textures=False)`) turns the maps off to look at the flat card
  geometry. A rebake shows at once (`cardMaps` on the groom records the maps, and maps rewritten
  on disk reload by themselves).
- **Out of date:** once a Cards node's cards change after a bake (the strands coming into it,
  or its params), the maps no longer fit them: that node gets an orange badge in the Graph
  Editor and its panel says "Maps out of date: rebake" (`groomlab.card_stats(groom)["stale"]`
  for any, `card_stats(groom)["nodes"][name]["stale"]` or `card_stats(groom, node=name)` per
  node). It is told per node: the bake records, in `cardMaps`, what each node's cards were made
  from (`Strands=<node>:<hash>,...`), so a look with no Cards node after it, or a change under
  one node only, never flags the others; a Cards node the bake never had is out of date too.
  Bake Maps again clears it.
- **The bake locks the atlas:** Bake Maps also writes `<name>_Atlas.json` next to the maps
  (named in `cardMaps`): where each card's texture sits, by the Cards node it came from and the
  strands it was made of. Cards rebuilt after that (guides brushed, the groom re-evaluated, the
  segments changed) take the cell they were baked with, matched among their own node's baked
  cards by the strands they share (layered Cards nodes card the same strands, so one node's
  cards never take another's cells; the cards of a cross each keep their own), so the maps keep
  fitting instead of every rebuild shuffling the atlas; a card of strands the bake never saw
  borrows a baked card's cell (a node added or renamed since, any node's). The next bake lays
  the atlas out afresh.
- **Cards follow the brush:** while a stroke is live the cards rebuild with it when a rebuild
  (the groom at `cardDensity` with its cards) takes a quarter second or less; heavier grooms
  catch up when the stroke ends. **Full density while sculpting** (Groom Tools > Tools; the
  groom's `sculptDensity` 1.0) keeps every strand of the preview up during a stroke instead of
  a lighter share, so nothing pops at the end of it.
- **A mesh only to export:** Export Unreal Cards makes a card mesh for the FBX and removes it.
  `groomlab.create_card_mesh(groom)` makes a live one on `outCards` for exporting by hand; while
  it exists it shows the cards in the cards view instead of the groom (its visibility follows
  the view), so they never show twice.
- **From the groom before it:** the node makes cards from the strands coming into it and passes
  them through unchanged, so place it where the look should be carded (e.g. before frizz or
  flyaways that cards can't show). Several Cards nodes make layers of cards in one atlas and
  one bake: the bake's resolution is the largest `atlasSize` any of them asks for (unless Bake
  Maps is given one) and the padding between textures the smallest.
- **Thinnest Strand** (`thinnestStrand`, in the atlas's pixels, 1 by default): the narrowest a strand
  bakes, solid. At 1 every strand is at least a pixel wide in the maps, so cards cut out at a
  coverage cutoff (alpha tested, the usual game setup) don't break up, but the cards read denser
  than the groom: a real hair is a fraction of a pixel, and the viewport's width coverage and
  renders show it fainter. At 0 the strands bake to scale, each as faint as it is thin, for cards
  drawn blended or dithered (alpha to coverage, temporal dithering), which then match the groom's
  look. Several Cards nodes share the bake at the largest of theirs. Only Cards nodes that make
  the output build cards: one bypassed, not connected to the output, or on a Switch's other
  input makes none; one inside a Compound on the way does (named `compound/inner` in the stats
  and the atlas). `groomlab.create_cards` and `cards_preset` change every Cards node of the
  graph (not those inside compounds), or only the one named (`node=`); the panel's LOD menu sets
  its own node.
- **Cards from guides** (`cardsFrom` `guides`, with the node's `guides` input linked to a Guides
  node; drawing that link in a Graph Editor sets it, and `groomlab.create_cards(groom,
  from_guides=True)` does both): each strand joins the guide whose root is nearest its own, and
  each guide holding `minStrands` or more is one card (or a cross, as `crossStyle` says) along the
  guide itself, its smooth curve from where it grows, as wide as those strands and baked from
  them, so the cards are where the guides are (a card per curve, as curve-to-card tools make
  them). Clumps, `cardsPerClump`, `layers`, `maxCards`' merging and `flyawayCards` then do
  nothing; feathers and braids are carded as ever. `strands` (the default) follows the strands
  as below: with few guides that gives many more, narrower cards, each along its own strands.
- **Cards per clump:** clumps come from the `clumpChannel` (`clumpId` by default, what Clump
  writes); without clumps, strands group by root into as many groups as `maxCards` allows
  (`maxCards` / (`cardsPerClump` x `layers`); 0: 60). Each clump then gets `cardsPerClump` x `layers` cards: its strands are banded by depth (how far
  out of the scalp they run) into `layers` (1 to 3) inner and outer shells, and each band is split
  (k-means over where the strands run in the clump's cross-section, at several heights) into
  groups of strands lying together, each group a card along its own median curve showing only its
  own strands. `1 x 1` is one flat card per clump (the low LOD); a hero groom wants 4 to 8 x 2.
  `maxCards` is the one budget, on the cards themselves (sub-cards, crossed and flyaway cards
  count), what the LOD presets set: over it, the clumps with the fewest strands per card give up
  sub-cards first, then neighbouring clumps merge to share wider cards. Merging many clumps of hair that
  turns (short fur combed by a flow) makes wide cards the hair runs across: keep one per clump
  there, with **Bake Scalp** for the base.
- **Cards lie the way the hair lies:** a card turns to its strands' principal direction across
  (a PCA of their offsets at each station, then one steady turn fitted through them up the card,
  at most 100 degrees; `fitTwist` 1, 0 keeps it facing out of the scalp), so a sheet of hair that
  rolls over gets a card that rolls with it instead of a flat blade its strands leave. Crossed
  cards (round tufts) keep their facings. Cards clear the scalp by their centre line, smoothly
  along their length, and each card's base (its root row) sits on the scalp under it, a strand's
  width above the skin across the whole row, however the card turns, bows (`curvature`) or
  ruffles above it (the mesh bends to it over the first tenth of the card, the UVs as they were;
  a card standing on edge puts its lowest vertex there). A ribbon lies on its strands' outer side (where a quarter of
  them run farther out of the scalp), so the groom's silhouette, its outermost strands, keeps its
  size. `ruffle` pushes alternate cards of a clump in and out along their normal (a share of their
  width, never farther than their strands lie off it) and turns them a little the other way, so
  layered cards never share a plane (no z-fighting, more volume); a lone card is never ruffled.
- **Separation** (its own section: `separation` for hair cards, off by default; `featherSeparation` for feather
  cards, off by default since GroomLab 0.34, nodes saved before keeping 0.05; `separationIterations`,
  8): cards closer than a gap to another card, or passing
  through it, are pushed apart along their normals (`separateCards`, `engine/core/src/
  card_separation.cpp`). The gap is a share of the narrower card's width (between a hair and a
  feather card, the larger share), so it fits any scale. Each pass moves every card along its own
  normal by one smooth curve (held at the root, free from a third of the way along) fitted to what
  its neighbours ask of its stations, so a card bends as a whole and never twists or kinks; only points within half the narrower card's width of the other count,
  and a pair keeps the side most of those lie on, so a card through another goes to that side.
  The cards of one down feather never push each other, and cards of different Cards nodes don't
  meet. Only the mesh moves (`Card::refShift` on the stations): the texture, baked from where the
  strands are, stays, and so does the bake's stale stamp. On a dense contour coat (414 feather
  cards) it takes intersecting card pairs from 56 to 5 for about 55 ms a rebuild.
- **The hair fits on its card, the card on the groom:** each card is as wide as nine in ten of
  its strands spread across it (in its plane), plus a `margin` (a tenth of the width each side)
  kept clear of the texture's edges, and as long as nine in ten of its strands reach (it carries on
  past the median tip); the farthest few are drawn along its edge or at its end rather than cut
  off, or widen it: a card as wide as its farthest strand balloons past the groom, and a lower
  level of detail's cards, holding more strands, would balloon more. A strand a narrower card
  (`widthScale` under 1) can't hold is drawn along its edge too. Cards sit a hair above the scalp wherever they lie on it, so the
  skin doesn't cut through them. Cards taper in the mesh (`widthProfile`, the strands' spread),
  but the texture keeps one scale along the card (its widest station's): narrower stations show
  the middle band of it, so a straight strand bakes straight and nothing stretches sideways.
- **Strays leave their card:** a strand that gets far from its group (a Flyaway, a strand the
  Clump let escape: 2.5 times the group's typical reach from its centre line) would make the
  card as wide as its wander, a wing off the clump. It is left out and the card is built from
  the rest; strays go to the flyaway cards first. Card widths are smoothed up the card, never
  narrower than the hair there, so outlines have no notches.
- **Nothing is dropped silently:** strands in groups too small for a card (`minStrands`) join the
  card of their clump they lie closest to along their length, which widens to hold them. Strands
  without a clump, or in clumps too small for one, and the strays above, get thin **flyaway
  cards** of their own, the farthest from any card first, up to `flyawayCards` (the hero preset
  gives 200), and the rest join the nearest card within reach; only strands far from every card
  with no flyaway budget left, and strays past the budget, are left out.
- **How close it is:** `cardFitDistances` (C++) measures how far the strands lie from the cards
  that draw them. On the test head (a sphere, two dozen loose clumps) the 95th percentile was
  1.17 clump radii with one flat card per clump and is 0.36 with the hero settings (the fit
  alone takes it to 0.98, sub-cards without the fit to 0.52).
- **Segments are only edge loops:** a card is made at a fixed resolution; `segments` picks how
  many of its rings become the mesh. Its texture's place in the atlas and the bake don't change,
  so changing the segments keeps the UVs and the baked maps valid.
- **Levels of detail:** `groomlab.cards_preset(groom, "hero" | "mid" | "low", triangles=None,
  node=None)` sets (on every Cards node, or on `node`) a triangle budget (about 50k, 15k and 4k) and the card count, cards per clump and layers
  (hero 6 x 2 with flyaway cards, mid 2 x 1, low one flat card per clump), segments, width,
  curvature, ruffle, texture variations and atlas size that go with it. Every level keeps the
  groom's silhouette (`widthScale` 1 at all three): fewer cards cover the same envelope, the
  coverage in their maps (`tools/bench/card_fidelity` measures it).
- **Texture space:** alike cards (length, width, how their strands lie, color) share one of
  `variations` textures, baked from the most typical of them; every other card shows it mirrored,
  so the repeat doesn't show. Textures are packed by their size in the world, so texel density is
  even and the atlas is used well; `variations` 0 gives every card its own texture. Each card is
  **tinted** to its own strands' color: it keeps their mean color root to tip (a tip color and
  bands too) and that of the strands its texture was baked from, and its vertices carry the ratio
  (`CardMesh::tints`), which the viewport multiplies the map by and the exports carry (the card
  mesh's color set `groomLabTint`, half the tint; glTF's `_TINT`), so shared textures show every
  card's own color.
- **Shading:** `volumeNormals` (on) gives the cards their strands' own normals, as the strands'
  lit drawing has them: out of the scalp at the card's roots, half their part along the card taken
  out, so cards light like the strands they stand for rather than as flat planes. The viewport
  shades them the same from either side (strands have no back to turn round), with the strands'
  ambient and highlight.
- **Crossed cards:** `crossStyle` sets cards per clump: `single` (one ribbon), `cross`
  (`crossCount` cards crossed around the clump's axis: 2 an X, 3 a star, up to 6) or `auto` (the
  default: a cross for round tufts standing off the scalp, spikes and fur tufts, which one ribbon
  would show as a flat blade; one ribbon for hair lying along the head). `maxCards` counts them
  all: over budget, the smallest clumps give up their extra cards first.
- **Braids:** strands a Braid node wove carry a `braidId` channel; with `braids` on (the default)
  each braid makes its own card set instead of being chopped across the clumps its strands came
  from: a cross of `crossCount` (at least 2) cards along the braid's axis for a hanging braid, or
  one ribbon lying along the scalp, facing out of it, for a cornrow (`braidFlat` 1). Braid cards
  get no sub-cards, layers, ruffle or twist fit, take their cards from `maxCards` first, and carry
  the braid's id on the card; strands the Braid left loose are carded as usual. Turn `braids` off
  (`groomlab.create_cards(groom, braids=False)`) to card braided strands by their clumps.
- **Segments by length:** `segmentsByLength` gives each card segments in proportion to its length
  (`segments` for the longest, at least `minSegments`), so short cards don't carry the edge loops
  long ones need. The UVs and baked maps don't change.
- **Controls:** `cardsPerClump`, `layers`, `fitTwist`, `ruffle`, `margin`, `flyawayCards`, `braids`,
  `segments` along, `widthSegments` across, `widthScale`, `widthProfile` (taper), `twist` (on top
  of the fit), `curvature` (bowed, needs 2+ width segments), `minStrands`, `maxCards`,
  `minWidth`, `atlasSize` (the bake's resolution); `groomlab.create_cards(groom, cards_per_clump=6,
  layers=2, flyaway_cards=200, ...)` sets them from Python. `cardDensity` is the strand density
  cards are made and baked at: 1.0, the whole groom, so every strand of a clump goes into its
  card and the bake sees all of them. The viewport's `previewDensity` and `previewPointBudget`
  never touch the cards (the viewport can show 5% of the hair while the cards, the card mesh,
  the bake and the export are all made from 100%); lower `cardDensity` only to rough cards in
  faster. Cards are built only while something uses them (**Show cards**, a bake, an export or
  a live mesh).

Bake Maps (`groomlab.bake_cards(groom, directory=None, resolution=None, occlusion=1.0)`; the folder
defaults to the groom's own in the scene's cards folder, `groomlab/<scene>/cards/<groom>/`, the files
named `<groom>_<map>`) renders the strands into the atlas as TGA files in Unreal's hair card layout:

| Map | Channels |
|---|---|
| `_Depth` | distance off the card plane (0.5 on it, higher in front) |
| `_Coverage` | opacity; fades in over the first 6% of each card so card roots blend |
| `_Tangent` | each strand's own direction in card space: x across (U), y root to tip (V), z out; 0.5 + 0.5 t (a feather's barbs run diagonal to the card) |
| `_Attribute` | R, G: root UV on the scalp; B: seed per strand; A: coordU, 0 root to 1 tip along each strand (a feather's barbs: from the shaft out) |
| `_Color` | base color, sRGB: the strands' color as the viewport shows it (the color nodes' RGB channels, per point along the strands where a tip color, Bands or FeatherColor vary it, else melanin); with FeatherColor's iridescence an RGBA map, its alpha the iridescence (255 full) |
| `_AO` | ambient occlusion from the hair around (1 open) |
| `_Normal` | the strands' normal as round fibers, in card tangent space: x across (U), y along V, z out, so a strand's middle faces out of the card and its edges turn to the sides; 0.5 + 0.5 n with DirectX's green (Unreal's: y down). A normal map, separate from `_Tangent` (the strands' direction): for a card material without Unreal's Hair shading model, whose Normal pin is the tangent |

Strands are drawn at their own width, at least a texel wide and opaque (`minStrandTexels`: a
viewport draws every strand at least a pixel wide, and a hair's width is a fraction of a texel, so
to scale they baked faint and the cards' cutoff left them out), where they actually lie
on the card (a short strand ends partway, it isn't stretched), front ones over back ones, and
colors spread a few pixels past them so mip maps don't pull in black. Every texture keeps its
edge rows and columns clear (the card's margin, and strands past an edge drawn along it), so
nothing bleeds into the neighbouring texture and the coverage's border is always zero. No card mesh is needed to
bake. **Show cards** (`groomlab.compare_cards(groom)`, which also turns on two-sided lighting:
cards are seen from both sides) then shows them against the strands, shaded as the strands are
(Maya's hair shading with the strands' normals, ambient and highlight, the same from either side,
each card's tint over the map, the coverage cut out crisply up close and kept, hashed per pixel,
as the texture shrinks), on DirectX 11 and the OpenGL Core Profile alike; a live card mesh, if
there is one, gets a material from the maps (color x half the AO, coverage as transparency).

**Bake the groom onto its scalp** (the node's **Bake Scalp**, or
`groomlab.bake_scalp(groom, directory=None, resolution=2048, reach=None, density=1.0)`): low LODs leave
skin showing between cards, so this bakes the hair near the skin, as seen from outside, into the
scalp mesh's UVs (it needs UVs): `<name>_ScalpColor` (the hair's color, spread over the whole map)
and `<name>_ScalpCoverage` (how much hair covers the skin: hair lying on it by its footprint, hair
standing up by the side it shows, softened over about the spacing between roots). It also makes
a **hair cap**: a copy of the scalp just above the skin, following it, textured with the maps
(`groomlab.scalp_cap(groom)`); it shows with the cards. Hair within `reach` of the skin counts
(default 3% of the scalp's size); the groom is read at `density`.

**For Unreal:** Export Unreal Cards (`groomlab.export_cards_unreal(groom, folder, name, lod=None)`)
bakes and writes `<name>_cards.fbx` (UV0 is the atlas, V root to tip, with a material pointing at
the maps; the mesh it makes for this is removed after), the seven maps, the scalp maps with the hair
cap `<name>_scalpCap.fbx` (when the scalp has UVs; `scalp=False` leaves them out), and
`<name>_cards.json` listing what each channel holds. `name` defaults to the groom's asset name,
with `_<lod>` when `lod` says which level of detail it is (`"hero"`, `"mid"`, `"low"`), so LODs
exported into one folder keep apart; the folder defaults to the scene's exports folder. The
export bakes a copy: the groom keeps showing its own bake (its `cardMaps`, card mesh and hair cap
materials are left as they were). Import the FBX as the groom's cards LOD (or a static mesh with a
hair card material) and assign the textures. The FBX carries what the viewport shades the cards
with: the strands' normals (locked) and each card's tint as the color set `groomLabTint` (half the
tint): in the material, Base Color = Color x 2 x Vertex Color (RGB, read as linear), and an Opacity
Mask of the Coverage map (dithered, or with alpha-to-coverage, keeps thin strands as the card
shrinks on screen). Unreal can also import the strands themselves
(Exporting for Unreal Engine).

## Feather cards

Games draw a feather as one alpha-tested card textured with a baked feather, many feathers
sharing a few textures. A Cards node does that for the feathers a Propagate node grows (strands
carrying `featherId`, `shaftU`, `side` and `isShaft`):

- **A card per feather:** a strip along the shaft of `featherSegments` segments (default 6), bending
  with it, lying in the vane plane (the way the barbs spread, toward the right vane) and as wide
  as both vanes reach at each station, plus the `margin`; it runs from the quill past the
  farthest barb tip. Asymmetric vanes keep the shaft off the texture's middle, at one place all
  along, so the rachis bakes straight. The texture is the feather's barbs and its rachis (a kept
  shaft, `keepShaft`, drawn at least 2.5 times a barb's width). Feathers are told apart by
  `featherId` and their shaft's root, so several Propagate nodes merged together stay apart.
- **The spine** (`featherSpine`, on by default): each card gets a column of vertices exactly on
  the shaft, so every segment is two quads, left edge to spine and spine to right edge, and the
  card can bend along the rachis. Each half follows its own vane as the groom has it: at each
  station, the way that vane's barbs head across the shaft (their samples there, the outer parts
  counting most) sets the half's fold out of the vane plane, so lifted vanes make a V opening up,
  drooping or cupped ones a V opening down, and one vane up with the other down shows as such.
  The fold is smoothed along the shaft, kept within 60 degrees, and flat where a vane has no
  barbs (the bare quill). Each edge vertex has its half's normal (the spine's lies between), so
  a cupped feather lights as one. The spine's U is where the rachis bakes (the off-center shaft
  line of asymmetric vanes), for shared and mirrored variations too. *The bake:* games bake a
  feather flat and fold the geometry, and so does this: each fold lifts the flat card's edge
  straight out of its plane (along the card's normal), so every vertex keeps the flat card's UV
  and a barb lying in its half shows exactly where it is (the flat bake's projection is exact
  for it); only Depth and Tangent are measured in each half's own frame, matching the normals
  the half shades with. Off: the flat strip of one quad per segment, exactly as before. Down
  feathers' crossed cards stay flat strips either way (a puff has no vanes to fold to, and each
  crossed card already bends along the shaft down its middle).
- **Flatten** (`featherFlatten`, 1 for new Cards nodes, 0 for nodes saved before GroomLab 0.34):
  how flat each card lies. At 1 the card keeps one plane along the shaft (its side the feather's
  mean) and each spine half one fold, so every quad is flat and the texture shows no facets along
  the quads' diagonals; the shaft still bends the card and the outline still follows the vanes,
  and the root row seats on the skin as a whole, parallel to the rows above. At 0 the card turns
  and folds with the barbs station by station, closer to the strands but with twisted quads
  (issue 26: 37% of one groom's quads folded more than 15 degrees, 1% at 1). Values between
  blend the two.
- **Down feathers** (barbs `around` the shaft) get `crossCount` crossed cards (two, an X) as wide
  as the puff, each showing the same baked puff, alternate ones mirrored.
- **Shared textures:** with `variations`, alike feathers (length, the vanes' outline, their
  asymmetry, the barbs' angle and curl, color) share one baked feather, the most typical of
  them; every other one shows it scaled to its own length (so the barbs keep their angle) with
  the shaft where the owner's is, mirrored left to right when its longer vane is on the other
  side (`mirror`; near-symmetric feathers alternate so the repeat doesn't show). Down feathers
  cluster apart from vaned ones; the two share `variations` by their numbers.
- **The maps:** Coverage, Color, Depth and AO come from the barbs and the rachis. The Tangent map
  holds each barb's own direction in card space (barbs at 45 degrees read (0.71, 0.71, 0) on the
  right vane, (-0.71, 0.71, 0) on the left), which is what Unreal's and Unity's hair shaders
  light along, so highlights run along the barbs, not up the card. The Attribute map's coordU
  (A) runs root to tip along each barb, out from the shaft, for the shaders' root-to-tip shading;
  a gradient along the feather is the card's own V (UV0 root to tip; glTF's TEXCOORD_1 y), and
  the feather's colors along its shaft (a color node reading `shaftU`) are baked into Color.
- **featherMode:** `auto` (default) cards feathers per feather and any hair reaching the node as
  hair; `perFeather` only the feathers; `off` only the hair (a hair Cards node after feathers that
  have a node of their own); `perVane` everything as hair (a card per `clumpId`: per vane or
  split, the old way). Plain hair is carded exactly as before.
- **Budget:** feathers take their cards from `maxCards` first (a down feather's cross counts
  every card); past it, the shortest feathers make no card. `featherMinLength` leaves out
  feathers shorter than that share of the longest (small down feathers at low LODs).

```python
made = groomlab.add_feathers(groom, "contour", cards=True, cards_lod="mid")  # feathers with cards of their own
node = groomlab.create_feather_cards(groom)          # or later: a Cards node on the feather branch
groomlab.create_cards(groom, lod="mid")               # the hair's cards
groomlab.card_node_kind(groom, node)                  # "feathers" ("hair", "both")
groomlab.card_stats(groom)["nodes"][node]["feathers"] # feathers carded by that node
groomlab.cards_preset(groom, "low")                   # hair nodes the hair preset, feather nodes the feather one
```

`create_feather_cards(groom, feathers=None, lod=None, segments=None, min_length=None,
variations=None, spine=None)` puts a Cards node (featherMode `perFeather`) right after the node giving the
feathers (default: the end of the branch `add_feathers` made, where the feathers meet the hair or
the output; an existing one there is reused) and sets Cards nodes after it that card feathers
too (`auto`) to `off`, so none is carded twice. The hair's and the feathers' Cards nodes share the
groom's one atlas and one bake; each node keeps its own cells, stats (`cards`, `triangles`,
`textures`, `feathers`) and stale flag (a node carding only feathers, or only hair, makes its
cards from those strands alone, so a change to the other never makes it stale). The viewport
draws them with the card effect like hair cards.

**Levels of detail:** `cards_preset` gives a Cards node only feathers reach (or set to
`perFeather`) the `FEATHER_CARD_PRESETS`, all with the spine: hero 6 segments along each feather
(about 30k triangles), mid 3 (8k, feathers under 15% of the longest left out), low 1, a simple V
of two quads per feather (2k, under 35% left out), with 24, 12 and 6 shared textures and a 4096,
2048 and 1024 atlas. A vaned feather's card costs 2 x `featherSegments` triangles, 4 x with the
spine (`groomlab.feather_card_triangles(segments, spine)`: hero 24, mid 12, low 4), and
`maxCards` is the triangle budget over that (hero 1250 cards, mid 666, low 500; a down feather's
cross counts each of its flat cards, which cost half, so the budget holds). A node both hair and
feathers reach gets the hair preset plus the feather one's segments, spine and minimum length,
and both budgets.

**Exports:** Unreal (FBX), Unity (FBX) and glTF card exports take the feather cards with the
hair's, in one mesh and one atlas: the same maps, alpha-masked at 0.5 and double-sided; the
manifests count them (`feathers`) and describe the spine (`featherCards`), and glTF's `lods`
bakes each level with the feather presets. The spine goes out as it is: three vertices a
station (left edge, spine, right edge) with the halves' normals, and glTF's TEXCOORD_1 x is 0 on
the left edge, 0.5 on the spine and 1 on the right edge wherever the rachis sits across the card,
so `abs(2x - 1)` is 0 on the rachis and 1 at the vanes' edges (stiff to free, for wind), as it is
from a hair card's middle to its edges.
(Feathers as strands instead: Feathers in Unreal, Feathers in Unity.)

## Segments by length or curvature (Resample node)

The **Resample** node's `mode` spends fewer points where they aren't needed, so the viewport,
renders and every export (Unreal, Alembic, USD, groom files) get lighter:

- **even** (default): every strand gets `segments`, evenly spaced.
- **length**: segments in proportion to each strand's length: `segments` for the longest (0: as
  many as they have), at least `minSegments` for the shortest. Short hair stops carrying the
  points long hair needs.
- **curvature**: as many segments as each strand needs to hold its shape, about one per
  `maxAngle` degrees it turns (15 by default; smaller keeps curls rounder), from `minSegments` for
  a straight strand up to `segments`, and placed closer together where it bends, farther apart
  on straight runs. `segments` stays the working resolution Scatter grows with, so curls and
  frizz are shaped in full detail first. With 40 segments, straighter presets drop from 30.5k to
  3-4k points with no visible change; a fully curly groom keeps nearly all of its points.

Put it where the shapes and lengths are final (after Interpolate takes the guides' shape and
lengths, and after Length). Widths and per-point channels follow the resampling; IDs, roots and
per-strand channels stay.

When the viewport shows a node other than the output (the Graph Editor's view/focus toggle), it
says so at the top: nodes after it, Width, HairColor, Cards, don't show until the output is
viewed again.

## Exporting for Unreal Engine

GroomLab > Export > Unreal Groom..., or `groomlab.export_unreal_groom(groom, path)`, writes
an Alembic file in Unreal's groom format:
- **Strands:** a `strands` curves object with `groom_version_major/minor` (1.5),
  `groom_group_id`, `groom_group_name`, `groom_id`, `groom_width` and `groom_root_uv`.
  `groom_id` is a dense index (0..N-1, in export order; Unreal wants a non-negative int), with
  the stable 64-bit strand ID beside it as `groomlab_strand_id_low/high` (two uint32).
  `groom_group_id` comes from a `groupId` strand channel when the groom has one (an Attribute
  node can write it), else 0. `groom_color` (linear RGB per vertex, the scope Epic's schema
  gives it) is written when the groom has a color node (the `colorR/G/B` point channels
  when they were written: a tip, Bands, FeatherColor; else the strand channels repeated along
  each strand); `groom_iridescence` (per vertex, 0-1) with FeatherColor's iridescence, and with
  `iridescence_as_ao=True` (`-iridescenceAo`, the app's **Iridescence as AO**) the same in
  `groom_ao`, which Unreal's Hair Attributes node hands a material
  ([docs/UNREAL_HAIR_MATERIAL.md](../UNREAL_HAIR_MATERIAL.md): the material recipe);
  `groom_roughness` (per curve, 0-1) from the `roughness` strand channel when there is one (a
  HairColor with its roughness set writes it); with a Clump node, `groom_clump_id`. Unreal reads color and
  roughness per curve or per vertex; these are per curve.
- **Guides:** the primary guides as a `guides` object with `groom_guide` = 1. These are what
  Unreal simulates.
- **Groups** (`groups="auto"`, the default): each groom group (the `groupId` strand channel:
  hair 0, each feather design its own, see [Feathers](feathers-braids-scales.md#feathers)) is a curves object of its own,
  `strands_<name>` (and `guides_<name>`), with `groom_group_id` = its id on every curve and
  `groom_group_name` = its name (the groom's name for group 0, the design's for feathers), so
  Unreal gives each group its own material, LODs and physics. `groups="<channel>"` groups by
  another channel; `groups=None` writes one `strands` object, `groom_group_id` per curve from
  `groupId`. A groom of one group (hair alone) writes the same file either way.
- **groom_id** is unique over the whole file: a dense 0..N-1 in export order, group after group
  (guides numbered among themselves). Strand IDs are 64-bit hashes that an int32 could not hold
  without collisions, so they are not truncated into it; they travel as
  `groomlab_strand_id_low/high` beside it. The numbering is per export: a groom cache keeps it
  from frame to frame only while the strands do (a rest mesh).
- **Feathers** (Feathers in Unreal, below): the shafts are left out when the feathers have a rachis
  (`shafts=None`, the default; `shafts=True` keeps them as strands); feather groups get no guides
  (`feather_guides=True`: their shafts); `lod="mid"`/`"low"` thins the barbs.
- **Frames:** the current frame makes a static groom; a frame range makes a groom cache.
- **Space:** the points and widths are written in Unreal's own space (Z up, left-handed,
  centimeters), turned from the scene's the way Unreal's FBX importer turns a mesh from the same
  scene: a Y-up scene's (x, y, z) becomes (x, z, y), a Z-up one's (x, -y, z) (Maya's up axis is
  read from the scene; Maya's centimeters whatever the UI unit; the app's file unit; Blender's
  meters times 100 times the unit scale). Root UVs, colors and IDs are as they are.
- **Importing:** import the .abc in Unreal as a Groom with the **default conversion settings**
  (Rotation 0, 0, 0; Scale 1, 1, 1): no rotation or flip is needed. The groom lines up with the
  head imported as FBX from the same scene (the FBX importer's defaults too). For hair that follows
  the character, import the head as a skeletal mesh and bind the groom to it with a Groom Binding
  asset (Create Binding on the groom, then the binding on the Groom component).

### Feathers in Unreal (the Meerkat eagle workflow)

Weta Digital's Unreal short *Meerkat* (Unreal Engine 4.26, 2020) has an eagle of about 2.5
million strands, groomed in Yeti and brought in as Alembic grooms. Feathers had to be rigged
for real time, so each feather's shaft (the rachis) was modelled as geometry in the eagle's
skeletal mesh, and the barbs were groom strands bound to it: the rachis carries the feather,
the barbs ride the rachis. Heavy shots swapped to a lower-resolution groom. GroomLab builds the
same setup from a feather groom:

1. **Groom the feathers** ([Feathers](feathers-braids-scales.md#feathers)): `groomlab.add_feathers(groom, ["contour", "flight"])`.
   Each design is its own groom group, and the presets give it a rachis width
   (`groomlab.set_rachis_width(groom, width, profile=None, sides=None)` changes every design's;
   `rachis_width(groom)` reads it). With a rachis the barbs grow from its surface.
2. **Make the rachis mesh**: `groomlab.create_rachis_mesh(groom)` makes a live Maya mesh on the
   groom's `outRachis` (a tube round every shaft, every feather at full density, following the
   groom as it changes). UV set `map1`: u around the tube, v root to tip; UV set `featherId`: u
   the feather's number (the barbs' `featherId`), v its groom group, so each
   feather's faces can be selected and weighted.
3. **Skin it with the body**: bind it to the character's skeleton with the body (feathers
   following the bones they sit on; a feather's vertices can share one influence so it moves
   rigidly), or combine it into the body mesh before skinning: in Unreal it must be part of the
   skeletal mesh the groom binds to. `groomlab.export_rachis(groom, "rachis.fbx")` (or `.abc`)
   writes it on its own.
4. **Export the groom**: `groomlab.export_unreal_groom(groom, "bird.abc")`: the hair and each
   feather design as groups, the barbs without their shafts, no guides for the feathers.
5. **In Unreal**: import the skeletal mesh (body and rachis) and the groom. Create a Groom
   Binding from the groom to that skeletal mesh (binding type **Skinning**): Unreal binds each
   strand root to the closest triangle of the mesh, and because every barb's root lies on its
   own rachis (never inside it, where the body's skin could be closer) the barbs follow their
   feathers. In the groom asset, turn simulation off for the feather groups (they ride the
   skinned rachis; the hair group keeps its physics and the primary guides) and give each group
   its material and LOD settings. Barbs carry `groom_root_uv` of their feather's base on the
   body (a body texture colors a feather through its root UV), `groom_color` from the color nodes and
   `groom_clump_id` per vane.
6. **Heavy shots**: `groomlab.export_unreal_groom(groom, "bird_low.abc", lod="low")` (or `"mid"`)
   writes a lighter groom to swap in: 25% (50%) of the barbs, picked by ID so the same ones
   every frame, each 1/sqrt(share) wider so the vanes keep their look (`barb_density` and
   `barb_width_scale` set them directly); the hair keeps `density`.

What only Unreal shows: how the binding, the per-group materials and LODs look on the skinned
character, and how the rachis's skin weights move the feathers. Sources:
[befores & afters, "Eagle vs. meerkat"](https://beforesandafters.com/2020/11/18/eagle-vs-meerkat-behind-weta-digitals-stunning-unreal-engine-short/),
[Unreal Engine spotlight on *Meerkat*](https://www.unrealengine.com/en-US/spotlights/behind-the-scenes-on-weta-digital-s-real-time-hair-and-fur-short-meerkat),
Epic's [Using Alembic for Grooms](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-alembic-for-grooms-in-unreal-engine)
and [Setting Up Bindings for Grooms](https://dev.epicgames.com/documentation/en-us/unreal-engine/setting-up-bindings-for-grooms-in-unreal-engine).

## Exporting strands for Unity

GroomLab > Export > Unity Strands (.abc)..., or
`groomlab.export_unity_strands(groom, path, scale=1.0, resample_to=None, start=None, end=None,
groups="auto", shafts=None, barb_points=None, lod=None, max_strands=64000)`, writes
Alembic curves for Unity's strand hair package,
[com.unity.demoteam.hair](https://github.com/Unity-Technologies/com.unity.demoteam.hair), which reads
them through [com.unity.formats.alembic](https://github.com/Unity-Technologies/com.unity.formats.alembic).
What that importer reads (`Editor/HairAssetBuilder.cs`, `Source/abci/Importer/aiCurves.cpp`) and how
the export fits it:
- **Points and widths:** positions, `nVertices` and the native `width` param per point (a diameter).
  A curve set with one root width of 0 or less loses all its widths (Unity falls back to its
  default diameter), so a strand whose root width is not above 0 gets its widest point's width
  at the root (else the groom's first root width); the rest of it is written as it is.
- **Root UVs per point:** Unity reads the curves' `uvs` expanded, one value per point, and drops
  the whole curve set when there are fewer UVs than points. `export_alembic` writes one UV per
  curve (Alembic's usual scope); this export repeats each strand's root UV (its place on the
  scalp's UVs) at every point, which is what the Hair Asset's **Root UV: Resolve From Curves**
  reads (at the root).
- **Points per strand:** the Hair Asset resamples to **Resample Resolution** points (16 by
  default, 3 to 128) and must when strands have different point counts. `resample_to` (3 to 128)
  gives every strand that many points evenly by arc length (widths follow), so **Resample
  Curves** can be off and the curves go in exactly as groomed.
- **Units:** `scale` multiplies points and widths. The default 1.0 keeps Maya's centimeters,
  which Unity's defaults expect: the Alembic importer's **Scale Factor** is 0.01 (centimeters
  to meters; it scales the points, not the widths) and the Hair Asset reads **Alembic Scale
  Positions** as **Data In Meters** (after that factor) and **Alembic Scale Diameters** as **Data
  In Centimeters**. With `scale=0.01` (meters in the file), set the Scale Factor to 1 and the
  diameters to Data In Meters. The importer's **Swap Handedness** (on) turns Maya's right-handed
  Y-up space into Unity's.
- **Other attributes** (`strandId`, `Cd`, strand channels) ride along; Unity ignores them.
  (Unreal's `groom_*` attributes are not written here.)
- **Groups** (`groups="auto"`, the default): each groom group (hair 0, each feather design its
  own, as for Unreal) is a curves object of its own, `/<groom>/<group>/<group>Shape`, named after
  the group (the groom's transform for the hair). The Hair Asset makes a strand group of each
  (`GetComponentsInChildren<AlembicCurves>`, in file order) when its **Alembic Asset Groups** is
  **Preserve**; the default **Combine** merges neighbouring curve sets of the same point count
  into one. A group of more than `max_strands` strands (64,000, `HairSim.MAX_STRAND_COUNT`; 0 or
  None: no limit) goes in even parts, `<group>_0`, `<group>_1`, ..., each a run of the group's
  strands in order, so the same groom gives the same parts. A groom of one group within the
  limit writes exactly what `groups=None` (one object, `/<groom>/<groom>Shape`) writes.
- **Barbs:** `barb_points` (3 to 128) resamples the feather groups' strands to their own point
  count (barbs are short: 4 to 8 points hold them), the hair keeping `resample_to`. The Hair
  Asset's resampling is one **Resample Resolution** for every group, so leave **Resample
  Curves** off to keep each group's own count (each group's strands must then share one count,
  which `resample_to` and `barb_points` give). `shafts`, `lod`, `barb_density` and
  `barb_width_scale` are export_unreal_groom's: the shafts are left out when the feathers have a
  rachis, and `lod="mid"`/`"low"` thins the barbs.
- **Frames:** the current frame by default; the Hair Asset takes its rest pose from the first
  sample.

In Unity: install com.unity.formats.alembic and com.unity.demoteam.hair (Package Manager > Add
package from git URL, `https://github.com/Unity-Technologies/com.unity.demoteam.hair.git`), drop
the .abc into the project (Import Curves on), then Create > Hair > Hair Asset, set its **Type** to
Alembic, assign the imported .abc prefab, and **Build strand groups**. Put a Hair Instance in the
scene with that asset (and a hair material) to simulate and render it.

### Feathers in Unity

The same setup as Feathers in Unreal: the rachis is skinned geometry in the character, the
barbs are strands riding it. GroomLab > Export > Unity Feathers..., or
`groomlab.export_unity_feathers(groom, folder=None, lod=None, name=None, points=16, barb_points=6,
density=1.0, shafts=None, rachis=True)`, writes into `folder` (default: the scene's exports folder):

- `<name>_strands.abc`: export_unity_strands with groups: the hair and each feather design a
  strand group (Unity's per-group material, LOD and physics), the barbs without their shafts,
  the hair at `points` and the barbs at `barb_points` points a strand, root UVs per point
  (each barb's is its feather's base on the body), root widths above 0, centimeters.
- `<name>_rachis.fbx`: `export_rachis(groom, path, unity=True)`, the rachis mesh in the Unity card
  export's conventions: **Y up, converted to meters** (UnitScaleFactor 100), so it imports at
  scale 1 and lands where the strands land after the Alembic importer's Scale Factor 0.01.
- `<name>_unity_feathers.json`: the files, the packages, every strand group in Unity's order
  (index, object, group, strands, points per strand) with the Hair Instance settings to give it,
  and the Hair Asset's settings. `lod="mid"`/`"low"` writes the lighter groom (`<name>` gets
  `_<lod>`).

How Unity attaches strands to a skinned mesh, from the packages' source: a Hair Instance's
**Skinning** settings (per strand group; they need
[com.unity.demoteam.digital-human](https://github.com/Unity-Technologies/com.unity.demoteam.digital-human),
0.2.1-preview or later for the root frame's twist) put a `SkinAttachment` of type **Mesh** on the
group's roots mesh (a vertex per strand root, its normal along the strand's first segment:
`HairInstanceBuilder.BuildMeshRoots`) and point it at a `SkinAttachmentTarget` on a
SkinnedMeshRenderer. `SkinAttachmentDataBuilder.BuildDataAttachToClosestVertex` finds each root's
**nearest vertex** of the target mesh (`meshVertexBSP.FindNearest`), then projects the root onto
the triangles around that vertex (barycentric coordinates plus the distance along the triangle's
normal, preferring triangles the projection falls inside) and carries it, and its frame, with
those triangles as the mesh skins. Nothing is projected along a UV or a direction: the root
must be nearest to vertices of the surface it should ride. With `rootOnShaft` every barb's root
lies on its own rachis's surface, so attach the feather groups to the rachis itself (a
SkinnedMeshRenderer of its own, skinned to the same skeleton), not to the body: a body vertex
nearer than any rachis vertex (a feather lying on the skin, a sparse tube) would take the barbs.

In Unity (2020.2 or later, the render pipeline's hair shaders for HDRP or URP):

1. **Packages**: com.unity.formats.alembic (2.2.2 or later), then from git
   `https://github.com/Unity-Technologies/com.unity.demoteam.hair.git` and
   `https://github.com/Unity-Technologies/com.unity.demoteam.digital-human.git` (Package Manager >
   Add package from git URL).
2. **The rachis**: skin `<name>_rachis.fbx` to the character's skeleton, in Maya before export
   (bind it with the body; a feather's vertices can share one influence so it moves rigidly; the
   `featherId` UV set tells feathers apart) or in your DCC of choice, and import it with the
   character (Convert Units on). Give its SkinnedMeshRenderer a **SkinAttachmentTarget**.
   Combining it into the body mesh works too when no body vertex lies nearer a barb root than the
   rachis does; a mesh of its own is the safe choice.
3. **The strands**: drop `<name>_strands.abc` into the project (Import Curves on, Scale Factor
   0.01, Swap Handedness on). Create > Hair > Hair Asset: **Type** Alembic, the .abc as its
   asset, **Alembic Asset Groups: Preserve**, **Alembic Scale Positions: Data In Meters**,
   **Alembic Scale Diameters: Data In Centimeters**, **Resample Curves** off (each group has its
   own point count), **Root UV: Resolve From Curves**, **Strand Diameter** and **Tip Scale:
   Resolve From Curves**; **Build strand groups**. The manifest lists the groups in the order
   the asset shows them.
4. **The Hair Instance**: add one with the asset, parented under the character. Per strand
   group (Strand Group Settings): the hair's **Skinning** attaches its roots to the body's
   SkinAttachmentTarget; each feather group's to the rachis's. Give each group its material.
   The barbs ride the rachis: hold them to their groomed shape with the group's **Physics**
   overrides (**Global Position** and **Global Rotation** at 1, **Gravity** 0), or put the
   feathers in a Hair Instance of their own with **Simulation** off (Executive settings, per
   instance) when nothing there should move on its own.
5. **Heavy shots**: `export_unity_feathers(groom, folder, lod="low")` writes the lighter strands
   (a quarter of the barbs, each twice as wide) to build a second Hair Asset from and swap in.

What only Unity shows: how the attachment holds up as the rachis deforms (it follows the
triangles around each root's nearest rachis vertex), the look of the per-group materials, and
whether the physics overrides hold the barbs as stiffly as wanted. Sources:
[`Editor/HairAssetBuilder.cs`](https://github.com/Unity-Technologies/com.unity.demoteam.hair/blob/master/Editor/HairAssetBuilder.cs)
(a strand group per `AlembicCurves`; Combine vs Preserve; resampling; widths dropped for a root at 0),
[`Runtime/HairAsset.Settings.cs`](https://github.com/Unity-Technologies/com.unity.demoteam.hair/blob/master/Runtime/HairAsset.Settings.cs)
(the Alembic and resolve settings and their defaults),
[`Runtime/HairSim.cs`](https://github.com/Unity-Technologies/com.unity.demoteam.hair/blob/master/Runtime/HairSim.cs)
(`MAX_STRAND_COUNT` 64000, 3 to 128 particles a strand),
[`Runtime/HairInstance.cs`](https://github.com/Unity-Technologies/com.unity.demoteam.hair/blob/master/Runtime/HairInstance.cs),
[`Runtime/HairInstanceBuilder.cs`](https://github.com/Unity-Technologies/com.unity.demoteam.hair/blob/master/Runtime/HairInstanceBuilder.cs)
(per-group skinning, the roots mesh) and digital-human's
[`Runtime/SkinAttachmentDataBuilder.cs`](https://github.com/Unity-Technologies/com.unity.demoteam.digital-human/blob/master/Runtime/SkinAttachmentDataBuilder.cs)
(nearest vertex, then its triangles).

## Game cards for Unity

The Cards node's **Export Unity Cards...**, GroomLab > Export > Unity Cards..., or
`groomlab.export_cards_unity(groom, directory=None, name=None, resolution=None, scalp=True, lod=None)`
writes into `directory` (default: the scene's exports folder), named like Export Unreal Cards
(`<asset>_<lod>`):
- `<name>_cards.fbx`: the card mesh, **Y up, in meters** (the FBX is converted from centimeters,
  so Unity imports it at scale 1 whatever Convert Units says), UV0 the atlas (V root to tip), a
  material `<name>_cards_mat` with the base color map (its alpha as transparency).
  `<name>_scalpCap.fbx` (`<name>_scalpCap_mat`) is the hair cap when the scalp has UVs.
- PNG maps (Unity's preferred format), baked from the same cards:

| Map | Channels |
|---|---|
| `_BaseColor` | RGB: base color (sRGB); A: coverage (alpha clipping at 0.5) |
| `_Mask` | HDRP's mask map layout. R: metallic (0; with FeatherColor's iridescence, the iridescence instead: a material reads it from there); G: ambient occlusion; B: root (0) to tip (1); A: smoothness (1 - the hair look's roughness) |
| `_AO` | ambient occlusion (1 open), gray: URP's occlusion map |
| `_Tangent` | strand direction in tangent space (R along +U, G along +V root to tip, B out), 0.5 + 0.5 t: HDRP Hair's strand direction |
| `_Depth` | distance off the card plane (0.5 on it), for pixel depth offset |
| `_Attribute` | R, G: root UV on the scalp; B: seed per strand; A: root to tip |
| `_Normal` | the strands' normal as round fibers in tangent space (R along +U, G along +V, B out), OpenGL's green: import as a Normal map |
| `_ScalpBaseColor` | the hair cap: RGB the hair's color on the scalp, A how much hair covers the skin |

- `<name>_unity.json`: the files, what each channel holds, import settings (BaseColor sRGB with
  Alpha Is Transparency; the others linear) and suggested materials: **HDRP** (a Hair Shader Graph
  or HDRP/Lit: Base Color Map, Alpha Clipping 0.5, Double-Sided, Mask Map, Normal Map, the Tangent
  map into Hair Strand Direction) and **URP** (Universal Render Pipeline/Lit: Base Map, Alpha
  Clipping 0.5, Render Face Both, Occlusion Map, Normal Map, Smoothness).

**Feathers** (Feather cards) go in the same FBX and atlas as the hair's cards: a strip (or, for
down, crossed cards) per feather with its baked barbs and rachis, folded along its spine
(`featherSpine`), the manifest's `feathers` counting them and `featherCards` describing the spine. The suggested materials suit them as they are: alpha clipped at 0.5 (a feather's
outline and the gaps between its barbs are in the coverage), double-sided (HDRP **Double-Sided**,
URP **Render Face: Both**: a strip is seen from both sides as the bird turns), and the Tangent map
runs along each barb (not up the card), so HDRP Hair's highlights run across the vane as on a real
feather. Cards sharing a texture may be mirrored in U (the other side's asymmetry).

As with Unreal, the export bakes its own copy: the groom's `cardMaps`, card mesh and hair cap are
left as they were (a live card mesh keeps its material). The FBX's UVs are the cards' current
atlas: after changing the cards since the groom's last Bake Maps, bake again first (the export
warns).

## Game cards as glTF

The Cards node's **Export glTF Cards...**, GroomLab > Export > glTF Cards..., or
`groomlab.export_cards_gltf(groom, path=None, lod=None, lods=None, resolution=None, binary=True,
cap=True, scale=0.01, alpha_mode="mask", alpha_cutoff=0.5)` (the `groomLabExportGltf` command)
writes the cards as glTF 2.0, for any engine or viewer that reads it (Godot, three.js, Babylon,
Blender, Unity with glTFast): a `.glb` with the maps inside, or with `binary=False` a `.gltf` with
`<stem>.bin` and `<stem>_<texture>.png` beside it. The writer is GroomLab's own
(`engine/core/src/gltf_writer.cpp`: JSON and binary buffers, compressed PNGs, no other library), so the
CLI and pipelines can use it too.
- **Space:** Y up, meters (`scale` 0.01 from Maya's centimeters; a Z-up scene is turned Y up).
- **Nodes:** one root node named after the groom, a mesh node under it per level of detail:
  `<name>_cards` (the cards as they are), or with `lods=["hero", "mid", "low"]` `<name>_hero` ...
  each baked from a copy of the graph with that preset (cards_preset); the groom is untouched.
  With `cap`, `<name>_scalpCap` (the scalp lifted just above the skin, with the scalp bake).
- **Vertices:** POSITION (with its bounds), NORMAL (the cards' strand normals, `volumeNormals`), TANGENT (+U across
  the card, w so that the bitangent runs root to tip), TEXCOORD_0 (the atlas, V flipped to
  glTF's top-left origin), TEXCOORD_1 (x across the card 0..1 by vertex column, 0.5 on a
  feather card's spine; y root 0 to tip 1) and `_TINT` (each card's tint, linear RGB 0..2, 1
  none: multiply the base color's RGB by it; not COLOR_0, which viewers would multiply in as it
  is). Indices
  are 16-bit, 32-bit past 65,535 vertices; triangles wind to face their normals.
- **Material** (PBR metallic-roughness), one per mesh:

| Texture | Channels |
|---|---|
| `<mesh>_BaseColor` | baseColorTexture. RGB: the baked color (sRGB); A: coverage |
| `<mesh>_ORM` | occlusionTexture (R: ambient occlusion) and metallicRoughnessTexture (G, B at 1: roughnessFactor is the hair look's roughness, metallicFactor 0) |
| `<mesh>_Normal` | normalTexture: the strands' normal as round fibers in tangent space (glTF's green, y up), with the TANGENT attribute's frame |

  `alphaMode` MASK with `alphaCutoff` 0.5 (hard, depth-correct edges, nothing to sort), or BLEND /
  OPAQUE with `alpha_mode`; double-sided. The hair cap blends. Each material's `extras.groomlab`
  names the channels and the hair look (roughness, specular).
- **Checked on write:** attribute counts, indices in range, no NaN or infinite values, unit
  normals and tangents, 4-byte aligned buffer views, textures and materials that exist.
