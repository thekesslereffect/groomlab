# GroomLab user guide

This guide walks through the six workflows GroomLab is built for, in the order a groom usually
takes shape. Each step works from Maya's UI (the **GroomLab** menu and the windows it opens), and
most have a Python call for scripts and pipelines. For the full reference (every node, parameter,
command and file format) see the web docs at https://getgroomlab.com/docs and the
[reference](reference/README.md).

**Install.** GroomLab runs in Maya 2022 to 2027 on Windows, with Arnold (MtoA). Maya 2022 is the
tested version. The builds for 2023 to 2027 are a beta: built for each version, not yet tested
inside it. One download holds every build: unzip it, start Maya and drag
`install.mel` onto the viewport (the package's README_INSTALL.txt walks through it). Restart Maya
once so Arnold finds GroomLab.

**Free.** GroomLab is free for any use, commercial included: every feature and every export, no
key and no activation. It is funded by donations: GroomLab > Help > Support GroomLab, or
https://ko-fi.com/getgroomlab.

```python
from maya import cmds
import groomlab
```

## 1. Start a groom

You work on a groom in three places:
- **The Attribute Editor** shows what the groom is made of: its **Inputs** (scalp, rest mesh,
  guides, colliders), its **Controls** (keyable sliders), and its display and render settings.
- **The Groom Tools** (**GroomLab > Groom Tools**) hold everything you do to the guides: brushes,
  guide groups, painted maps, correctives and reference photos.
- **The Graph Editor** (**GroomLab > Graph Editor**) holds the look: a graph of nodes, like Yeti's.

Steps:
1. Select the scalp mesh and choose **GroomLab > Create Groom from Selection**
   (`groomlab.create_groom(scalp, fit=True)`). You get hair out of the box.
2. Open the Groom Tools (or click **Edit** on the Guides row of the Attribute Editor's Inputs). It
   docks beside the viewport. The **Tools** tab has every grooming tool as an icon, with its
   settings below.
3. Pick **Place** and click on the scalp to add a guide, or drag to lay down many. New guides copy
   the shape of the guides near them. The strands follow as you work.

Good to know:
- **The default graph:** Scalp → Scatter → Interpolate (← Guides) → Length (uneven ends) → Noise →
  HairColor → Width → Resample → Output. Its numbers are made for a head (a 20 cm scalp), and Create
  Groom fits them to your scalp: on a 2 m creature the strands are 10 times as long, wide and far
  apart, so the hair looks the same in proportion. Presets and new nodes are fitted the same way.
  Without `fit=True`, the Python call keeps the head's numbers.
- **Units:** GroomLab works in centimeters (Maya's internal unit), whatever unit the UI shows. The
  hair grows on the scalp's world mesh, at the same strands per unit area: a scalp scaled 10x gets
  100x the strands. A preset applied before you scaled the scalp keeps its lengths; apply it again
  to fit the new size.
- **Viewport density:** the viewport shows 10% of the strands (`previewDensity`). They are the
  render's own strands, with the same clumps and curls; renders and exports take them all.
- **Unsaved scenes are fine:** painted maps, bakes and exports go to the project's
  `groomlab/untitled` folder and move beside the scene the first time you save it.
- **Deforming scalp:** if the scalp has skin or blend shapes, the groom connects its undeformed
  shape as a rest mesh, so the strands stay put on every frame. Otherwise click **Set** on the Rest
  row of the Attribute Editor's Inputs, or **Pick** and then an undeformed copy of the scalp.
- **Smoothed scalps:** when Arnold subdivides the head (Catclark) or you view it smoothed (the 3
  key), hair grown on the cage would float or sink. The Scalp node's `subdivision` param
  (`matchRender` by default) refines the scalp the same way the render does; `off` uses the cage,
  `1` to `3` set the levels yourself. Changing the level never changes the layout: the same
  strands, clumps and parts, just sitting on the smooth skin. The node's note in the Graph Editor
  says what it did.
- **Several scalps** (separate ears, a head and a body, eyebrow patches): click **Pick** on the
  **Scalps** row of the Attribute Editor's Inputs, then the mesh (`groomlab.add_scalp(groom,
  mesh)`). It gets its own Scalp node (its `input` names the scalp), Scatter and Interpolate,
  merged into the groom's strands, so the look applies to both. Guides, brushes, parts, flow and
  maps work on it as on the main scalp. **Remove** on its row takes it out again.
- **Guides reach along the skin:** Interpolate measures guide reach along the scalp
  (`followSurface`, on), so a guide behind an ear doesn't shape the hair across it. With
  `keepClearance` (on) strands never go into the scalp: where the skin bulges under a strand (an
  ear, a cheek) it is lifted just onto the skin and comes back to its groomed path past the bulge,
  so hair drapes over an ear instead of standing off it. Use a Collide for other meshes (a body, a
  collar).
- **Speed:** 100k strands through a full look graph evaluate in under 0.1 s, and tweaking one
  parameter redraws in about 5 ms.

## 2. Build the hierarchy and sculpt guides (Tonic-style)

Guides form a hierarchy: **primary** guides you place, and **children** that fill around each
parent and follow it when it changes.

Steps:
1. **Place primaries** with **Place** (click, or drag to scatter) and **Draw** (draw a guide's
   shape from the scalp).
2. **Generate children:** select some guides with **Select**, set the count beside **Edit
   Guides** (in **Guide groups & edits**, Tools tab) and choose **Edit Guides > Children**. With
   nothing selected it uses the group picked in **Group** at the top of the Groom Tools. Do it again
   on children for grandchildren.
3. **Shape them** with the brushes. **Comb** is the main tool: drag over guides and they follow
   the stroke, roots pinned.
4. **Pick what you brush** in **Group** at the top of the Groom Tools: a group (primaries, a child
   group) or **All groups**. Children always follow their parents, and with a child group picked
   the brushes shape children one by one. Place and Draw then add children by hand, each following
   the nearest parent.

### The tools

The tools are the same in Maya, the standalone app and Blender, in the same order:

| Section | Tools |
| --- | --- |
| Create | Place, Draw, Part, Flow |
| Shape | Comb, Slide, Smooth, Straighten, Lift, Twist, Curl, Attract, Direction, Noise, Shape |
| Trim | Cut, Length, Erase |
| Paint | Paint, Width, Density |
| Guides | Select, Freeze (and **Unfreeze All** beside it) |

- **Comb** drags guides along the stroke (**Stretch** lets them lengthen or shorten). **Slide**
  moves roots and keeps the shape. **Smooth**, **Straighten**, **Lift**, **Twist**, **Curl** and
  **Attract** refine; a negative strength reverses the ones that work both ways. **Shape** paints
  guides' tubes near the brush: their width, their flatness or their twist (negative: the opposite).
- **Direction** turns guides about their roots toward the way you stroke (negative: away), keeping
  their lean and length.
- **Noise** adds smooth random jitter along guides. Each stroke builds up the same pattern; a
  negative strength smooths it out.
- **Cut** trims where the brush touches, **Length** grows or shortens evenly, and
  **Erase** removes guides (with their children), parts or flow arrows.
- **Width** paints how thick the hair is and **Density** where it grows. Each paints the map its
  node reads (the Width node's `width`, the Scatter node's `density`). The first stroke makes that
  map (white, so nothing changes yet) and wires it up; undo once for the stroke, once more for the
  wiring. Strokes paint toward **Value**; Ctrl paints thinner hair, or takes hair away.
- **Paint** paints the map picked in the Paint tab (see Paint maps in section 3).

### Brush settings

They sit below the tabs and change with the tool.
- **Radius and strength:** hold B (radius) or M (strength) and drag, once the GroomLab hotkeys are
  installed (**GroomLab > Install GroomLab Hotkeys**). Or middle-drag for the radius and
  Shift+middle-drag for the strength. Tablet pressure scales the strength. **GroomLab > Help >
  Keyboard Shortcuts** lists every key and marks the ones that need the GroomLab hotkey set;
  installing the hotkeys shows which keys changed.
- **Falloff:** 0 is hard to the brush's edge, 1 soft from the center. Tick **Falloff curve** to
  draw the falloff instead, center on the left, edge on the right.
- **Root to tip:** tick it and draw how much of the strength each part of a guide gets, root on the
  left, tip on the right. Use it to style the ends without disturbing the roots, or the reverse.
  Right-click the curve for presets.
- **Front facing only:** what the head hides gets no stroke. **Keep off scalp:** guides never dip
  into the skin.
- **Shift+drag** smooths, with any brush. **Ctrl** selects guides with the guide brushes (below);
  painting maps or Freeze it does the opposite.

### Solo and freeze: work on one guide at a time

Frozen guides draw blue and brushes leave them alone.
- **Solo new guides** (Place and Draw, off by default): turn it on and each guide you place stays
  live while all the others freeze, so the next stroke shapes only the new one.
- **Ctrl+click** with a guide brush to solo the guides inside its ring: they join the selection
  and the rest freeze. **Ctrl+drag** paints guides in with the ring; **Ctrl+Shift+drag** lassos them.
- **Ctrl+click** where the ring holds no guide (bare scalp or empty space) to unfreeze them all,
  or click **Unfreeze All** beside the Freeze tool.
- **Freeze** paints frozen guides by hand (Ctrl unfreezes).
- Each of these is one undo step. With **Symmetry** on, the mirror guides stay live too.

### Selection

- **Select** lassoes or clicks guides: Shift toggles, Ctrl deselects, Ctrl+Shift adds. What you
  select stays live and the rest freeze, so brushes work on the selection. A click on nothing
  unfreezes every guide.
- **Soft radius** (in the Selection box) gives the selection a falloff. **Selected only** in the
  brush settings limits a brush to the selection.
- **Edit Guides** has **Mirror Selection** and **Clear Selection**.

### Symmetry

- **Mirror a finished side:** **Edit Guides > Sync +X → -X** (or **-X → +X**), or **Mirror
  Group...**.
- **Keep it symmetric as you work:** turn on **Symmetry** in the **Group** row at the top of the
  Groom Tools (or **Alt+Shift+X** with the GroomLab hotkey set). Every stroke is
  mirrored across world X, or, with **Topology** picked, across the scalp's own left and right. Use
  Topology for a head that is turned, tilted, animated or not quite symmetric. Brushes, Place, Draw,
  Part, Flow, Select and map painting all follow it.

### Parts and regions

- **Part:** pick **Part** and draw along the parting on the scalp; it is added when you release.
  Hair stops blending across the line, and a Part node opens the gap (its `width` sets how far).
  The Parts box lists and removes parts; **From Curves** makes parts from selected curves.
- **Parts from the comb:** if you combed the parting into the guides, set the Part node's
  `partsFrom` to `auto` instead of drawing. It finds the lines the hair flows away from on both
  sides and opens them like drawn parts. Raise `splitAngle` (90 degrees) to find only sharp
  partings, lower it for soft ones; `minLength` drops short splits like the center of a whorl.
  These parts only open the gap: Interpolate and Clump still blend across them (draw the part if
  they must not).
- **Regions:** to keep whole areas apart (bangs from the sides), paint a color map in the Paint tab,
  one color per region, and name it in Interpolate's `regionMap`. Each strand then takes only its own
  region's guides, and Clump, BlendNeighbors and Tangle stay within regions too.

### Groups and restructuring

**Guide groups & edits** (Tools tab) lists the groups your guides live in: `primary`, the child
groups, `braids` for braid paths. It shows each group's count, level and the graph nodes that read
it, and can select, rename (the graph follows), hide or delete a group. **+ New Group** makes your
own (say `bangs`); pick it in **Group** and Place and Draw put new guides there. Each group is drawn in a
color of its own (its swatch; click it to pick another). In the Graph Editor, a Guides node's `group` and `exclude` pick
from these names.
- **Reparent...** makes selected children follow another parent.
- **Promote / Demote...** moves a whole group up or down a level, keeping every guide's shape.
- `groomlab.lock_guides` detaches a child from its parent; `unlock_guides` reattaches it.

### Correctives and photos

- **Sculpt nodes** brush the guides the graph made (sparse guides -> Scatter + Interpolate ->
  ToGuides -> **Sculpt** -> the next Interpolate): select the Sculpt node in the Graph Editor, or
  its row **Node: sculpt1** in the Correctives list, and the shaping brushes reshape its guides
  (drawn as the guides meanwhile). Each stroke is one undo step, kept in the node as bends and
  length changes along each guide, by its ID: comb the sparse guides later and the sculpted
  guides keep their hooks and combs on top of the new shapes. **Clear Edits** (the node's panel or
  menu) drops them. In the app: the Correctives panel's **Sculpt into** lists the Sculpt nodes.
- **Correctives** (Correctives tab) are sculpt layers over the base groom, like Yeti's: pick a
  corrective and strokes are recorded on it, leaving the guides alone. Each weight is keyable;
  **Fix Frame** makes a shot fix that blends in and out around the current frame. See
  https://getgroomlab.com/docs/motion-and-simulation.
- **Groom to photos** (Photos tab): **Import Photo...** makes each photo a camera aimed at the
  head. **Align Photo...** opens the Photos window: the photo and the mesh side by side; click a
  feature on the photo (an eye corner, the nose tip), then the same feature on the mesh. From four
  pairs the camera lines up as you click (six or more solve the focal length too, unless **Keep
  focal length**); each pair shows how many pixels off it lands. Or line it up by eye, looking
  through it. **Lock** it, set its **Opacity** and
  **In front** or **Behind**, and brush the guides to the photo while looking through it
  (`groomlab.reference`).

## 3. Look development (Yeti-style)

The look is a graph of nodes after the guides: scatter roots, interpolate them from the guides,
then clump, curl, color and so on.

Steps:
1. Open **GroomLab > Graph Editor**.
2. Start from a preset: **Presets** lists looks (Hair, Fur, Feathers, Scales) and small graphs
   that each teach one idea (Motion, Techniques, Games). Hover one for what it shows. **Apply**
   replaces the graph; **Insert After Selected Node** adds the preset's look as one Compound after
   the node you selected.
3. Add or tweak nodes: **Add Node** puts a node after the selected one (its menus are the same
   categories as the app and Blender; **Tab** finds a node by what it does: symmetry finds Mirror,
   frizz finds Noise); drag between ports to wire.
   With **Live** on, each edit applies at once as one undo step.
4. Render a test with HairColor's **Create Hair Shader** (section 6).

New nodes are fitted to your scalp's size, like presets: a Noise on a cat or on a giant starts at a
sensible size. The **Add-ons** presets (`wet`, `frizz`, `flyaways`, `cards_mid`, `gravity`) are
made for Insert: each adds one thing to your look. Ungroup the Compound to see inside.

If something is off, the groom says why: `groomlab.errors(groom)`, the Graph Editor's status bar,
or the Script Editor (a missing map, a typo in an expression, bad guide data).

### Clumping

Add a **Clump**. By default it picks its own clump centers among the strands (`centersFrom`
`pick`):
- `clumpBy` `count`: about `count` clumps (300). `clumpBy` `size`: clumps about `size` apart, so a
  body gets more than a head.
- `randomness`: 0 evenly spread, 1 random, up to 2 for more clumps and gaps. `centerWeight`
  (mappable) says which strands may be centers.
- **Your own centers:** connect guides or strands to the `centers` input; this sets `centersFrom`
  to `inputPoints`. `inputPoints` uses only where the centers are rooted, and each clump follows
  the hair grown there. `inputCurves` follows the center curves themselves: groomed clump curves,
  or a Noise on the centers that waves each clump as one. With `pick`, a connected `centers` is
  ignored; an input mode with nothing connected leaves the strands unclumped. The node's note says
  so.
- **Clumps of clumps:** chain Clumps from small to large. The small ones form first and the large
  ones carry them whole. Give each its own `clumpChannel` (`clumpId` for the small, `lockId` for
  the large) so a Twist, Cards or HairColor's `clumpShare` can follow the one it names.
- **Long hair:** a strand longer than its clump's axis gathers at the tip (`pastTip` `gather`), or
  carries on past it (`extend`) so long hair keeps its length.
- **Paint clumping:** right-click `strength` > **Paint > On Guides**. Children and strands inherit
  what you paint.
- **Parting around whiskers, horns, eyes:** a negative `strength` (down to -1) pushes strands
  within `repelRadius` away from the centers. Feed the whiskers' guides as `centers` with
  `centersFrom` `inputCurves`, and keep the radius small. Type a negative value, or right-click the
  slider for **Set Range**.
- **Locks and ribbons:** `lockRadius` gathers strands into round locks instead of onto the center
  (dreads, wet strings); a **Twist** node after it turns them into ropes. `flatness` squashes clumps
  into flat ribbons (wet hair); `flatten` says which way they lie.

### Shape and detail

- **Noise** for waves and frizz: a small `fraction` with a high `frequency` gives frizz;
  `mode` `kinky` makes sharp random kinks.
- **Curl:** `shape` makes helixes, waves, zigzags (kinky hair) or coils (coily hair, a few strand
  widths across: put a Width before it). `coherence` `clump` curls each clump as one (ringlets,
  after a Clump). `frequencyProfile` tightens or loosens the curls along the strand; `spacingRandom`
  lets them bunch and open a little. `weight` stretches the turns where hair hangs below them
  (along `weightDirection`): ringlets loose at the root and tight at the tip, more on long hair,
  hardly at all on hair lying flat or pointing up. Curl adds points where a strand has too few for a round turn
  (`tolerance`): each segment cut evenly along the smooth strand, so it never aliases.
- **Bend** bends hair up off the scalp or down onto it (`along` `normal`), or combs it toward a
  `direction` laid onto the scalp (`along` `direction`; `angle` 180 with `start` and `end` 0 lays
  it all the way, never past). **Gravity** lets it hang from the root (a Collide after it drapes it
  over the scalp), **Displace** lifts it off the scalp. Their `direction` also takes a vector
  expression (the **f(x)** button).
- **Smooth** takes out bumps up to `scale` (a share of the strand's length): small values take out
  kinks, 1 straightens the strand. It looks the same at any point count.
- **Length** trims strands: `keep` is the share kept from the root (mappable); `fraction` of the
  strands are trimmed at random, each losing up to `randomness` of its length. To
  lengthen, use Transform's `scale`.
- **Width** sets thickness; its `randomness` varies it per strand.
- **Collide**, best last, keeps strands off the scalp and any collider. It keeps a distance field,
  so brushing with it in the graph stays fast; `method` `exact` is slower and the reference. By
  default a strand that meets a surface folds flat onto it. For a softer meeting (hair settling on
  a floor or a shoulder), raise `softness`: strands start curving away that far above the offset
  instead of kinking where they touch. `smoothing` (a length along the strand) spreads the push
  back toward the root, so the strand bends onto the surface over that length and lies along it,
  keeping its length. Neither ever leaves a point inside the surface. Both act on the collider
  (the floor, a shoulder); the scalp, which the hair grows from, keeps its firm push.
- **SelfCollide** stops hair passing through itself: where strands are packed tighter than
  `packing` (the share of space hair may fill), they are pushed apart, so layers rest on one
  another and hair gains volume where it piles up (behind the ears, over a collar, at the crown).
  Hair with room does not move. Lower `packing` for more volume; `strength` and `profile` (along
  the strand) scale the push. It reads every strand, so it evaluates whole when you brush before
  it (the GPU preview keeps that interactive); put it after the shaping nodes and before the look.
- **Mirror** between the Guides node and Interpolate grooms one half and mirrors it; after the look
  it mirrors the finished hair. `mode` `add` keeps both sides.

**GPU preview while dragging:** when the graph ends in nodes the graphics card runs (Interpolate,
Length, Clump, Wet, Noise, Curl, Width, Part, Gravity, Flyaway, Collide, Wind, Blend, SelfCollide,
Static, Bend, Direction, Transform, Displace, Twist, Smooth, RestoreLength, BlendNeighbors, Tangle,
Flow, Jiggle, MeshCut, then an even Resample), dragging their sliders, seeds, vectors or profile keys
(Wet's wetness; SelfCollide's strength, packing and profile; Static's strength, randomness and
profile; Transform's translate, rotate and pivot; a Bend, Direction or Displace direction; Smooth's
strength, scale and profile; BlendNeighbors' strength and profile; Tangle's strength, start, span
and reach; Flow's lift, bend, rollRandom and seed; Jiggle's strength, frequency, damping and
history; MeshCut's offset; any of these nodes' `affect`) draws on the GPU without evaluating the
groom, so hundreds of thousands of strands stay interactive. A node masked by its `affect` (a Group,
a painted map, an expression) stays on the GPU too, its result mixed back by the
mask as it is when you let go. (A direction given per strand by an expression, or a Direction with
its inclination, roll and strength all mapped, evaluates as usual, as does a Jiggle's history
dragged past the frames Maya sampled for it, and a Curl that adds points. So do sliders that pick
which strands a node reads: BlendNeighbors' neighbors, Tangle's fraction, radius and seed.) A Cull,
a Split (any of its outputs) or a Mirror after the look keeps the drag on the GPU too: the nodes before it
run there and their strands are kept or mirrored as it last kept or mirrored them; its own sliders,
and a Cull or Split keeping by anything but a number, a texture or a color set, evaluate as usual.
Brushing does too, SelfCollide and Static included, masked nodes as well. Letting go evaluates as
usual, so renders and exports don't change. (SelfCollide, Static and Flow can tip one way or the other on the tiniest
difference in the hair they take, so after a brush stroke or a drag of a node before them a few
strands may settle a hair's breadth elsewhere when you let go; a Flow strand where the arrows about
cancel, or one standing straight out of the scalp with no arrows, can go another way.)
If your graphics card misbehaves, turn off **Preview Drags on the GPU** in the Graph Editor's more
menu.

### Colliders

To keep hair off a body, a collar or another character, click **Pick** on the Collider row of the
Attribute Editor's Inputs, then the mesh (`groomlab.add_collider(groom, mesh)`). Esc cancels. This
adds a **ColliderMesh** node and a **Collide** node before the output; move the Collide earlier
when strands should collide before clumping or noise. The collider follows an animated mesh in the
viewport, renders and exports, and has the same `subdivision` param as Scalp. **Remove** on its row
takes it out. In the Graph Editor, a ColliderMesh or Scalp node's panel has a **Mesh** menu and an
eyedropper to rewire it.

### Library looks: the Linked Groom node

A **Linked Groom** node (Add Node > Sources) brings in another `.groom` file's strands or guides: an
eyebrow or fur patch groomed once and reused. Merge it with the groom's strands. Set its `path`
(**...** browses); it reloads when the file changes. `bind` `world` keeps the look where the file
has it; `closestPoint` or `uv` roots it on this scalp so it follows the skin (`uv` when both scalps
share a UV layout).

### Color and realism

- **Flyaway** adds stray hairs. **Tangle** mats strands together (wet hair, after a tight Clump);
  strands stick only within their own clump (`clumpChannel`, `clumpId`; empty: across clumps) and within `reach`.
- **Static** is dry winter hair: the hair is charged and repels itself, so it lifts off the head,
  flares out toward the tips and clumped locks fray into their strands. `strength` is how charged
  (0 leaves the hair as it was), `randomness` lets some strands fly farther than others, and
  `profile` puts the charge along the strand (rising toward the tips by default: roots barely
  move). It reads every strand, so it evaluates whole when you brush before it (the GPU preview
  keeps that interactive); put it after Clump.
- **Wet** makes wet hair from one `wetness` slider: small clumps near the root merging into bigger
  ones toward the tip, as surface tension gathers wet strands. `clumpSize` is the biggest clumps'
  size, `levels` how many sizes nest. Animate wetness from 1 to 0 to dry the hair out. Wet hair
  also looks wet, with no setting of its own: darker, more saturated and glossier (a sharper,
  stronger highlight) in the viewport, the app and Arnold (`create_hair_shader` reads the
  `wetness` channel). An **Attribute** node named `wetness` wets strands by a map without clumping.
- **HairColor** sets the color, as RenderMan's PxrHairColor does: in `mode` melanin (natural) the
  pigment (`melanin`, `redness`) dyed by a `dye`; in `mode` color an RGB `color` rendered as picked
  (white hair: white). A `map` (a painted map, a color set or an image: the color in mode color, a
  dye in mode melanin), greys, per-strand randomness (`melaninRandom` or `valueRandom`,
  `hueRandom`, `saturationRandom`) shared per clump by `clumpShare`, and a tip color,
  and the highlight (its Highlight section): `specular` (how bright), `roughness` (how broad), each
  varied per strand by `specularRandom` and `roughnessRandom`, and `tilt`, the cuticle tilt in
  degrees (3 by default), which moves the shine and its colored second shine apart along the strand.
  The viewport, the app, Arnold, Blender and RenderMan all render these.
- **RenderData** after it sets the opacity.
- The `cinematic_hair` preset combines them.

**One color for every renderer.** HairColor's result is one RGB per strand, which Arnold renders as
`aiStandardHair`'s base color (in mode melanin, the color whose absorption is the melanin's) and
the viewport shades the same way. Natural and dyed strands mix in one groom by a Merge of two
HairColor branches, or by a map.

### Paint maps

To paint a parameter (say Scatter's density), right-click it in the Graph Editor and choose
**Paint** > **On Guides**, **On Scalp Vertices** or **As Texture**. A white map is made (so nothing
changes yet), a **Map** node feeds the param, and the Paint tab opens with the brush ready: strokes
lower the param, Ctrl raises it, Shift+drag smooths.
- **Three kinds of map:** on the guides (a layer children inherit; it survives a new scalp), on
  the scalp's vertices, or a texture (a PNG at the scalp's UVs, the finest, editable in any image
  editor; painting carries across UV seams). **New Map...** in the Paint tab asks for a name and a
  kind; **Convert to** changes the kind and keeps the name, so whatever reads it keeps reading it.
- **Color maps:** a scalp map can hold a color (**Values: Color**, or **Make Color**). Paint hair
  color with it (a HairColor's `map`), or pack several masks in one texture: the Map node has a
  port per channel (`r`, `g`, `b`, `a`), so one map can feed clumping from R and frizz from G.
- **One map, several params:** **Feed Param** on the Map node, or
  `groomlab.use_map(groom, node, param, "mask")`.
- **Images:** right-click a map for **Relink Image...**, **Reload Image** (after editing it in
  another program) and **Show in Explorer**. Save the scene first: texture maps live in
  `groomlab/<scene name>/masks/<groom>/` beside it.

Other things can drive a mappable parameter too: a color set, a strand attribute, or an
**expression** such as `smoothstep(-1, 1, $y)` (type `$` for the variables). An **Expression** node
gives multi-line expressions with your own slider variables and a library of examples; see
https://getgroomlab.com/docs/expressions.

### Restrict, blend, drive, animate

- **Restrict a node to some strands:** a **Group** node names a set (a sphere or box, a map or
  expression past a threshold, the strands near some guide groups) as a 0-1 channel. Map any
  shaping node's **Affect** (under Common) to that channel and it works only there.
- **Viewport or render only:** right-click a node > **Evaluate** > **Viewport Only** or **Render
  Only**.
- **Two looks, one slider (wet to dry):** branch after Interpolate into two looks and join them with
  a **Blend** node. `mix` blends them (0: a, 1: b); its `profile` sets it along the length. Give
  each branch its own HairColor and the color blends too.
- **Drive with a slider:** right-click a parameter > **Drive with Slider**. A **Control** node
  drives it, and the groom gets a keyable attribute in the channel box
  (`groomlab.add_control(groom, node, param)`).
- **Animate a parameter:** right-click it > **Set Key**, or press **S** over its field. It gets a
  keyable attribute on the groom (`ctl_<node>_<param>`), edited in Maya's Graph Editor like any
  animation. Its field turns red on a key, pink between keys, yellow when a Control drives it.
  Densities, counts, segments and seeds can't be keyed: strand counts must stay the same on every
  frame for motion blur and caches.

### Attributes

An attribute is a value each strand (or each point) carries by name, such as `clumpId`,
`region`, `melanin` or a Group's mask. Nodes pass them down the graph.
- **Who writes them:** a Clump writes its `clumpChannel` (`clumpId`), an Attribute node its `name`,
  a Group its group, an Interpolate `region` (with a `regionMap`) and `guideInfluence` (with a
  `limitDistance`), Wet `clumpId`, `clumpId1`... and `wetness`, Flyaway `flyaway`, HairColor the
  color attributes. Maps painted on the guides become attributes of the same name at the
  Interpolate.
- **Who reads them:** a Curl or Twist by clump, Cards, HairColor's `clumpShare`, a Clump, Tangle or
  BlendNeighbors keeping to a `regionChannel`, any mappable param set to a strand attribute, an
  Expression (`$clumpId`).
- **In the editors:** each such param says **Writes** or **Reads**. A reading param lists only the
  attributes that reach its node, each with the node that writes it (`clumpId (from clumpLarge)`);
  a name nothing upstream writes gets a grey note (the node's **i**), and the node ignores it. A
  writing param warns when its name is already written upstream (`overwrites clumpId from
  clumpLarge`): from there on the strands carry the new values.
- **Chained Clumps:** give each its own name (`lockId` for the large clumps, `clumpId` for the
  small), then point a Twist, Cards or HairColor at the one it should follow.

### Braids

Draw curves for the paths, then `groomlab.add_braid(groom, curves, style="three")` (or **GroomLab >
Add > Braid on Curves...**). With `feed` on (the default for cornrows), strands join the braid where
they reach the path: cornrows, French braids. With nothing on its `braids` input, a Braid picks its
own paths about `region` apart: box braids with nothing drawn (the **Box braids** preset).

### Reuse and share

- **Compound:** select nodes and press **Ctrl+G**, then expose the parameters that matter.
  **Ctrl+Shift+G** ungroups.
- **Presets:** **Presets > Save as Preset...** saves the graph with its compounds.
- **Copy Graph / Paste Graph** put a look on the clipboard as text and paste it onto another groom,
  fitted to its scalp. The status bar names what it reads that the other groom lacks (maps, color
  sets, meshes).

### Look checks

- `strandColorMode` `clump` colors the strands by clump; `attribute` shows a channel.
- The viewport draws each strand's width as pixel coverage (`displayWidthCoverage`, on), so thin
  tips fade and a thinned preview reads as dense as the full groom. `displayRibbons` shows strands
  at their true width (ribbons facing the camera, at least a pixel wide).
- Seen from afar, each clump draws only as many strands as cover its pixels (`displayClumpLod`,
  on; `clumpLodError` 0.175). Renders and exports always take every strand.

### Feathers

Every strand fed to a **Propagate** node (Add Node > Feathers) is a shaft that grows barbs. The
barbs are ordinary strands for the nodes after it.
- **From the menu:** **GroomLab > Add > Feathers...** asks the design and how they grow:
  **Scattered feathers (shaped by the guides)**, or **One feather per guide (hand-placed)** in a
  guide group you pick or name (`feathers`, `flight_left`, `tail`). With curves selected, each joins
  the group as a hand-placed feather; with none, the Draw tool draws into the group. The preset
  **Flight feathers (hand-placed)** starts a wing or tail the same way.
- **From Python:** `groomlab.add_feathers(groom, "contour")` scatters shafts that lie with your
  guides. Designs: `contour`, `down`, `flight`, `afterfeather`, `barred`. Hand-placed:
  `add_feathers(groom, "flight", source="guides", curves=curves)`. Assortments:
  `add_feathers(groom, ["contour", "down"], weights=[3, 1])` (a **Split** node routes the shafts).
- **Designing:** the design is a Compound. On Propagate, `lengthProfile` draws the vane's outline,
  `angle` and `angleProfile` how far barbs lean, `curl` bends them, `droop` cups the vane, `splits`
  makes the gaps real feathers have, and `down` turns the base into soft fluff.
- More: https://getgroomlab.com/docs/fur-and-feathers.

### Color patterns

Two nodes after a HairColor give a strand more than one tone, both in Add Node > Color. They render
the same in the viewport, the standalone app and Arnold, and bake into game cards. Arnold renders
them lighter and softer (light scatters through many hairs), in the same places.
- **Bands** (fur: agouti, ticked, shaded): each strand takes the band pigment by `strength` where
  `profile` says, root to tip. `count` repeats it (ticked fur: three or four); `offsetRandom` and
  `widthRandom` keep bands from lining up across the coat. `guards` is the share of guard hairs;
  the rest is undercoat. A black tip is HairColor's `tipColor`. See the `agouti_fur` preset.
- **FeatherColor** draws a `pattern` in each feather's flat space: `bars`, `chevrons`, `spots`,
  `fringe`, `eye`, or `texture` (paint the feather as an image). `offsetRandom`, `scaleRandom` and
  `angleRandom` keep feathers from matching.
- **Iridescence:** raise FeatherColor's `iridescence` and strands shimmer toward
  `iridescenceColor` as they face you. **Create Hair Shader** builds an approximation in Arnold;
  game cards carry it in the Color map's alpha and Unreal strands as `groom_iridescence`
  ([docs/UNREAL_HAIR_MATERIAL.md](UNREAL_HAIR_MATERIAL.md) shows the material).
- More: https://getgroomlab.com/docs/color-patterns.

### Scales

Dragons, lizards and pangolins wear rigid plates, but you groom them the same way: the strands lay
the scales out and the **Scales** node puts a plate on each.
1. **Start from a preset:** Graph Editor > **Presets** > **Scales** > **Dragon scales** on your body
   mesh. Only the scales show; the strands just place them.
2. **Point them:** a scale points the way its strand lies. Bend the strands toward a direction, lay Flow
   arrows, or brush guides (the **Scales from guides** and **Scales from flow** presets). For strands that
   curl back over their root, set `aim` to `root`.
3. **Size and spacing:** a scale is as long as its strand and as wide as its root width (Transform's
   `scale`, Width, the Scales node's `size`); Scatter's `density` spaces them. Make them longer than
   the spacing and they overlap like roof tiles (`tile`; `lift` tilts them more).
4. **Shape:** `prototype` is the plate: `groomlab:scale`, `groomlab:scaleRound`,
   `groomlab:scaleShield`, or your own mesh (**Prototype from Selection**; model it 1 unit long up
   +Y, 1 wide, top facing +Z). `follow` `strand` bends each plate along its strand (spikes, quills:
   the **Swept spikes** preset).
5. **Color** with HairColor, as for hair. Past two million triangles the viewport draws flat quads
   (`preview`); renders always take the real plates.
6. **Out:** Alembic and USD write the scales as a mesh; **Export > Scales Mesh (FBX)...**,
   **Scales Instancer (USD)...** (light and editable, but each plate is unbent) and **Bake Scale
   Maps** (normal, height and color maps for distant or tiny scales). See
   https://getgroomlab.com/docs/rendering.

## 4. Start from existing curves

- **Maya curves:** select the scalp and the curves, then **Create Groom from Selection**. Or add
  them to a groom with **Edit Guides > From Curves** in the Groom Tools.
- **Alembic curves** (from Houdini, or XGen exported to Alembic):
  `groomlab.import_guides_from_alembic(groom, path)`.
- Imported curves become primary guides; carry on with sections 2 and 3.
- **Edit guides with Maya's own tools:** **GroomLab > Utilities > Guides to Curves** makes a curve
  per guide. Reshape them any way you like (CVs, deformers, a sculpt), select them and choose
  **Curves to Guides...**: each curve reshapes its own guide, and any other curve becomes a new one.
- **Hair as geometry:** **Strands to Curves...** and **Strands to Mesh...** turn the strands into
  Maya curves or one mesh (capped at 20,000 strands by default). **Bake Node to Guides...** turns a
  node's strands into guides you can brush.
- **The same look on other grooms:** select the groom you like first, then the others, and choose
  **Utilities > Copy Graph to Other Grooms...**.
- **A groom onto another head:** click **Pick** on the Scalp row of the Attribute Editor's Inputs,
  then the new scalp (`groomlab.set_scalp(groom, scalp)`). Guides, parts and painted maps move
  onto it, even onto a retopologized head with other UVs. Choose **Fit and Move** when the other
  head is elsewhere or another size (`fit=True`). **Utilities > Transfer Groom to Scalp...** does
  the same with options for maps and color sets, and reports what moved.
- **A head turned, or of other proportions:** **Transfer Groom to Scalp...** opens a window with
  two views: the groom's scalp on the left, the **New scalp** on the right. Choose a marker
  **Method**: **Markers: align** (3 or more pairs: the head moved, turned, scaled) or **Markers:
  warp** (4 or more pairs spread over the head: a wider or longer head, another character). Then
  click points on the meshes: the 1st point on the left pairs with the 1st on the right, the 2nd
  with the 2nd, in any order (forehead, temples, crown, nape, behind the ears). A point without its
  partner yet is drawn dim (unmatched) until you click its partner on the other side.
  - Drag a point to slide it over the surface; **Ctrl+click** a point to delete it and its
    partner (the numbers close up on both sides); hover a point to light it and its partner.
  - **Symmetry** adds the mirrored point (across the middle of the mesh) right after each
    click; deleting one of the two deletes both.
  - **Ctrl+Z** / **Ctrl+Y** in the window undo and redo the points (not Maya's undo).
  - **Alt** moves each view's camera as in any viewport; **Frame** fits the mesh.
  - The status line counts pairs and unmatched points, and says why **Transfer** is not ready
    yet. The complete pairs are stored on the groom as you go; unmatched points last while the
    window is open.

  **Matching UVs** moves each guide to its UV on a head laid out alike. **Keep guide lengths**
  turns guides without scaling them. By script: `utils.transfer_to_scalp(groom, "newHead",
  method="markersWarp", markers=[("head.vtx[12]", "newHead.vtx[40]"), ...])`. The standalone
  app's **Tools > Transfer Groom to Scalp...** is the same window (**New Scalp...** picks the mesh
  file; **Import Markers...** reads pairs from a JSON file).

**GroomLab > Add** puts common building blocks on the selected groom: **Feathers...**, **Braid on
Curves**, **Cards** (Hero, Mid, Low), **Collider**, **Wind**, **Jiggle** and **Simulation**.

## 5. Motion and simulation

### Motion without a simulation

**Add Node > Motion > Wind** bends the hair in a gusting, travelling wind; **Jiggle** makes it trail
the head's motion, overshoot and settle (`groomlab.add_wind(groom)`, `groomlab.add_jiggle(groom)`).
Play the timeline. Nothing is cached: every frame is worked out from the time and the scalp's last
frames, so shots scrub and render on the farm the same. Bypass (B) or delete either and the groom is
exactly as before.

**Looping** (a game idle): turn on the node's `loop` and set `loopStart` and `loopEnd`
(`groomlab.add_wind(groom, loop=True, loop_start=1, loop_end=49)`). Wind repeats seamlessly over
the range. Jiggle reads the frames before `loopStart` from the end of the range, so animate the head
as a cycle over the same frames. Render `loopStart` to `loopEnd` - 1 and play them on repeat.
Simulate has no loop.

### GroomLab's simulation

1. **GroomLab > Simulate > Add Simulate Node** puts a Simulate node between the guides
   and Interpolate (`groomlab.sim.add_simulate(groom)`).
2. Play the timeline: the guides swing under gravity and the head's motion, stay off the scalp, and
   the strands follow.
3. **Bake Simulation** for renders: it simulates the playback range and writes the frames to the
   scene's caches folder, which exports, groom files and Arnold read.

- **Settle first:** set `startFrame` a few frames before the shot so the hair comes to rest.
- **Less floppy:** raise `bend` (keeps the groomed curves), `rootStiffness` (how firmly it's held;
  `rootStiffnessProfile` along the length) or `damping`, or lower `inertia` (how much of the head's
  motion the hair feels).
- **Scrubbing** back shows frames already simulated; a jump forward simulates the frames between.
  Changing a setting simulates again from the start frame. **Simulate Range** fills the range (Esc
  stops); **Clear Simulation** drops the frames and bakes.
- **Water:** turn on `water` and set `waterLevel` (or connect a mesh to the `water` input). Hair
  below it floats (`buoyancy`), slows (`waterDrag`) and drifts with `currentSpeed` along
  `currentDirection`. `rain` wets hair without any force.
- **Wet hair:** the simulation writes a `wetness` channel (0 dry to 1 soaked; it dries over
  `dryTime` seconds). Wet hair hangs heavier (`wetWeight`) and shades wet, as after a Wet node. The look can read it as a map, for
  example a Clump's `strength` set to `{"attribute": "wetness", "min": 0.35, "max": 0.95}`.
- **The standalone app** shows **Simulate**, **Bake** and **Clear** on its timeline when the graph
  has a Simulate node. Save the groom after baking: the file names the bake.
- More: https://getgroomlab.com/docs/motion-and-simulation.

### Other simulators

- **nHair (legacy):** **GroomLab > Simulate > Legacy > Simulate Guides (nHair)...** on the primary
  guides builds nHair curves with follicles; children follow their simulated parents. **Remove
  Simulation** returns to the groom as authored.
- **Houdini and others:** **Export Guides for Simulation...** writes curves that carry their guide
  ID as `strandId` (keep that attribute through the simulation). Bring the result back with
  **Load Guide Cache...**.

### Playback and motion blur

- **Motion blur:** on a deforming scalp, give the groom a rest mesh (section 1) so strands match
  between motion samples.
- **Cached Playback:** a groom caches like any mesh. A groom whose frames depend on earlier frames
  (a Simulate without a bake over the playback range, a Jiggle) is left out and evaluated as it
  plays; bake the simulation to have it cached. When simulating live, set **Playback speed > Play
  every frame** (Preferences > Time Slider), or use Simulate Range.

## 6. Render and publish

- **In the scene:** render with Arnold as usual. The groom renders at full density, with
  deformation motion blur when it's on. HairColor's **Create Hair Shader** makes an
  `aiStandardHair` from the groom's color (by melanin, by RGB, or both strand by strand), or assign
  your own with `groomlab.assign_shading_group`. The strand-by-strand mix uses built-in Arnold
  nodes only; it has not been rendered on Arnold GPU yet.
- **Render overrides:** **Rendering > Render Overrides** in the Attribute Editor change the render
  alone: `renderDensity` (a share of the strands, the same look), `renderWidthScale` and
  `renderLengthScale` (each strand scaled about its root). The viewport stays as groomed. Exports
  take them when asked (`render_overrides=True`).
- **On the farm:** **GroomLab > Render > Render from Groom File...** makes exported `.ass` files
  hold a small procedural that evaluates the groom at render time, without Maya. The farm needs
  `GroomLab/procedurals` on `ARNOLD_PLUGIN_PATH`, as the module sets up.
- **Caches for other packages:** **GroomLab > Export > Alembic...** or **USD...** (curves with
  widths, root UVs and strand channels). Or from the command line:
  `groomlab-cli eval hair.groom -o hair.usda -f 1-48`.
- **To and from the standalone app:** **GroomLab > Groom Files > Save Groom File As...** (or **Save
  to Groom File** once it is linked) writes a `.groom` the app opens. After editing it there, **Groom Files >
  Reload from Groom File** (or **Reload** in the Attribute Editor's Inputs) brings the edits back in
  one undo step. A groom made in the app comes in with **Groom Files > Import Groom File...**:
  select its scalp first if the scene has it, or let GroomLab make the scalp from the file.
- **Game cards:** a **Cards** node (**GroomLab > Add > Cards**, `groomlab.create_cards(groom,
  lod="mid")`) turns the groom into textured cards; **Export > Unreal Cards...**, **Unity
  Cards...** or **glTF Cards...** write the mesh and its baked maps. Cards are always built from the
  whole groom, never the viewport's preview density. **Show cards** draws them in the viewport,
  each tinted to its own strands' color. In Unreal, multiply the Color map by 2 x the mesh's vertex
  color (the `groomLabTint` color set) to get the same colors.
- **Feather cards:** `groomlab.create_feather_cards(groom, lod="mid")` (or `add_feathers(...,
  cards=True)`) makes one alpha-tested strip per feather, folded down its shaft
  (`featherSpine`); Hero, Mid and Low set the budget.
- **Strands in engines:** **Export > Unreal Groom...** and **Unity Strands (.abc)...**. For
  feathers, the shaft becomes a mesh to skin with the character and the barbs strands rooted on it
  (`groomlab.export_unreal_groom`, `export_rachis`; **Export > Unity Feathers...**). See
  https://getgroomlab.com/docs/game-export.
- **Moving scenes:** grooms keep project-relative paths, so scenes can be referenced, namespaced
  and moved between projects. `groomlab.absolute_paths(groom)` lists anything that would break.
- **Dailies:** **GroomLab > Review > Render Turntable...** renders the groom going round
  in Arnold in the background, into `groomlab/<scene name>/renders` (and an `.mp4` with ffmpeg).

## Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| No strands | `groomlab.errors(groom)` says why. Also check the scalp is connected (`inMesh`) and the graph has an Output. |
| A node shows a grey **i** | It does nothing as set: a Part with no parts drawn, a Flow with no arrows, a Clump in an input mode with nothing connected, Interpolate without guides. Its note says why. Not an error. |
| Strands pop on a deforming scalp | No rest mesh: **Set** on the Rest row of the Attribute Editor's Inputs. |
| Brush does nothing | Select the groom first. Check **Group** at the top of the Groom Tools, frozen guides (blue: Ctrl+click where no guide is to unfreeze them all), and **Selected only** with an empty selection. |
| Cached Playback leaves a groom out | Its graph has a Simulate node without a bake over the playback range, or a Jiggle node. Bake the simulation to have it cached. |
| A texture map does nothing | The scalp needs UVs, and the path must resolve: `groomlab.resolve_path("sourceimages/map.png")`. |
| A map says "no map named" or "does not fit this scalp" | The Map node names a map the Paint tab doesn't list (make or rename it there), or a vertex map from a scalp with another vertex count (convert it to a guide layer or a texture on the old scalp first). |
| A map or file is missing after a copy or Save Scene As | Save again: every save copies the files the grooms name into the scene's `groomlab/<scene name>` folder. Or use **GroomLab > Utilities > Find Missing Files...** to relink everything from a folder you pick; for one map, right-click it > **Relink Image...**. |
| Painting a texture starts from blank | Its PNG is not beside the scene, so the brush started a new one. Undo the stroke, restore the file, paint again. |
| A texture map or bake is now `..._2` | Its file was taken by another groom or scene, so GroomLab gave this groom its own copy instead of writing over it. |
| A map ignores edits made in Photoshop | Right-click it > **Reload Image** (`groomlab.reload_map(groom, name)`). |
| Farm render has no hair | Is `groomlab_procedural` on `ARNOLD_PLUGIN_PATH`, and are the `.groom` file and its `.scalp.abc` reachable from the farm? |
| Import Groom File says the mesh "is not the scalp of" the file | The selected mesh has other vertices or faces than the groom was made on. Deselect it to have the scalp made from the file, then **Utilities > Transfer Groom to Scalp...** onto your mesh. |
| A scene from a newer GroomLab won't load | The error names the version; update GroomLab. Older scenes load. |
| Maya crashed | Read `%TEMP%\GroomLabCrash_<date>_<time>.log` for the reason and the stack, and please send it with what you were doing. |
