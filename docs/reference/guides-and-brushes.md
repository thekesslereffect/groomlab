# Guides and brushes

The guide hierarchy, parting, guide selection, the brushes, grooming from reference photos, flow arrows for guideless short hair, and correctives.

User guide: [Guides & brushes](https://www.getgroomlab.com/docs/guides-and-brushes), [Reference photos](https://www.getgroomlab.com/docs/reference-photos), [Fur, feathers & scales](https://www.getgroomlab.com/docs/fur-and-feathers).

## Guide hierarchy

The Tonic-style part: primary guides, then children generated inside each parent's tube,
then grandchildren inside the children's tubes, to any depth. Children store their shape
relative to their parent, so editing a parent carries every descendant.

```python
curves = cmds.ls("guideCurve*", type="transform")
groomlab.guides_from_curves(groom, curves, group="primary", tube_radius=0.15)   # copies the curves
groomlab.generate_children(groom, "primary", name="children", count=8, twist=0.5)
groomlab.generate_children(groom, "children", name="grandchildren", count=4)
groomlab.list_guide_groups(groom)   # [{'name': 'primary', 'depth': 0, 'count': 6, 'color': '#...', ...}, ...]
cmds.setAttr(groom + ".displayTubes", True)
```

**Groups.** Guides live in named groups: `primary` (placed, drawn or from curves), child groups
from `generate_children`, `braids` for braid paths. The **Guides** tab of the Groom Tools shows
them as a tree with guide counts, levels and the graph nodes naming them; it selects, renames, deletes
and adds a Guides node for a group. Every group, child groups too, is drawn in a hue of its own
(a parent and its children, and siblings, well apart), from a hue of the groom's own; a group's
swatch shows it and picks another (`set_guide_group_color`). A `Guides` node reads every group at a `level` (-1: each family's deepest groups, a group
without children its own guides), or the
groups named in `group` (one or several, their guides together), and leaves the groups in `exclude`
out; in the Graph Editor both have a **+** menu of the groom's groups. Other text params offer what
exists too: a dropdown for fixed values (Braid `style`, HairColor `mode`, Instance
`orient`, ...) and suggestions for names (color sets, parts, meshes, controls). A param naming an
attribute (a strand channel) says **Writes** or **Reads**: a writing one (Clump's `clumpChannel`,
Attribute's `name`) is a plain field with a warning when the name overwrites an upstream node's; a
reading one (Curl's `clumpChannel`, Clump's `regionChannel`) lists only the attributes reaching the
node, each with the node that writes it, and notes a name that does not reach
(`groomlab_core.graph_channels(graph_json)`, the same in the app and Blender).

```python
groomlab.group_users(groom, "braids")               # [('guides', 'exclude'), ('braidPaths', 'group')]
groomlab.rename_guide_group(groom, "braids", "rows")  # graph params follow; groomLabGuides -renameGroup
groomlab.set_guide_group_color(groom, "rows", "#ff8800")  # None: its automatic color again
```

**Choosing who has children, and placing them by hand.** Parents don't have to be whole groups:

```python
groomlab.generate_children(groom, count=6, selected=True)          # only the selected guides get children
groomlab.generate_children(groom, count=4, guides=[child_id])      # a child picked to have children
groomlab.brush_stroke(groom, "place", [point], level=1)            # a child placed by hand
```

- **Growing a child group:** the child group (`<parent group>_children` unless named) grows
  with each call. A parent that already has children there gets a fresh set, so you decide
  parent by parent who has children, at any level.
- **Placing by hand:** Place and Draw at a child level add a child to the nearest guide one
  level up. The first ones copy their parents' shape, and later ones copy nearby children. In
  the Groom Tools, pick a child group in **Group** (New Group... under Guide groups & edits
  starts a deeper level) and use Place or Draw.
- **Individual edits:** brush at a child level to shape children one by one. They keep that
  shape relative to their parent, so moving the parent still carries them.

- **Storage:** the hierarchy is stored as JSON in the groom's hidden `guideData` attribute and
  saves with `.ma` and `.mb` scenes. Edit it only through `groomLabGuides` (or the Python
  wrappers: `guides_from_curves`, `generate_children`, `lock_guides`, `unlock_guides`,
  `remove_guide_group`, `clear_guides`). Every edit is undoable.
- **Scalp binding:** primary and locked guides are bound to the scalp by face and barycentric
  coordinates, with their shape stored in the root's surface frame, so they move and turn with a
  moving or deforming scalp.
- **Tip-first curves:** curves drawn tip-first are flipped when rooted.
- **Which level is used:** the default graph's `Guides` node uses each family's deepest groups
  (`level: -1`): a group's children where it has some, else its own guides.
  Set `level` or `group` to use another one, and connected `guideCurves` are merged in as extra
  flat guides.
- **Viewport:** primaries draw in `guideColor` (a vibrant blue by default), each level down
  lighter and further round the hue wheel, `guideWidth` pixels wide (4 by default; selected
  guides one more), drawn as a smooth curve through the guide's points (`guideSmoothing`
  pieces per segment, 4 by default; 1 draws straight segments).
  Guides toggle with `displayGuides`; tubes with `displayTubes`: a solid, shaded tube per guide
  (Lambert-like, in the theme's tube color) following its radius profile. The hair toggles with
  `displayHair` (the Groom Tools' first eye: strands, instances, scales, cards and strand meshes;
  the guides stay; renders ignore it).
- **Over the hair:** guides, their dots, parts and flow arrows draw over the strands, tubes and
  cards (`displayOverlaysOnTop`, on by default): the viewport pulls them toward the camera by
  the length of the longest strand before the depth test, so anything within the hair shows
  through it. The pull stops short of every visible mesh in the scene (the scalp, a body, a
  collar; a ray per point, redone as the camera moves), so those still hide what is behind
  them: no X-ray through the head. Off, they are depth-tested like everything else.

**Hierarchy tools.** Every tool keeps each guide's current shape; what changes is what it
follows. Each is one undo step:

```python
groomlab.mirror_group(groom, "primary", "x")               # copy +X guides to -X, replacing what is there
groomlab.symmetry_sync(groom, "x")                         # the whole groom +X to -X, children and edits too
groomlab.reparent_guides(groom, parent_id, guides=[...])   # follow another parent (default: the selection)
groomlab.promote_group(groom, "children")                  # one level up; at the top they become primaries
groomlab.demote_group(groom, "extra", under="primary")     # one level down; each follows the nearest guide
groomlab.set_guide_attribute(groom, "clump", 0.5, group="primary")
```

- **Mirroring:** `mirror_group` mirrors one root group; generate children after it.
  `symmetry_sync` (**Sync +X → -X** / **Sync -X → +X** under Guide groups & edits in the Groom Tools)
  mirrors every root group and copies each guide's children, grandchildren and so on with it,
  hand edits, locks and painted attributes included. Guides on the plane are left alone.
- **Per-guide attributes:** children inherit them from their ancestors unless they set their
  own. `Interpolate` blends them into strand channels of the same name, which attribute maps
  read, for example `attribute_map("clump")` on a Clump's strength.

## Parting

**Drawing parts:** pick **Part** in the Groom Tools (Tools tab) and draw freehand on the scalp.
- **While drawing:** the stroke follows the surface under the cursor and shows in red.
- **On release:** the stroke becomes a part, in one undo step. With symmetry on, its mirror
  image is added too.
- **Part node:** the groom gets a Part node if it has none, so the part opens.
- **Parts box:** with Part picked, the Groom Tools' Parts box names new parts and lists,
  removes (**Remove**) or clears (**Clear All**) them.
- **From curves:** parts can still be made from existing curves (the Parts box's **From
  Curves**, or `add_part`).
- **Reshaping:** with **Parts** ticked under Acts on in the brush settings, Comb slides a part's points
  along the scalp, Smooth and Straighten relax or straighten the line, Attract pulls it toward
  the cursor and Cut splits it in two (see Brushes). The parts preview live as you drag; each
  stroke is one undo step.

Parts are lines on the scalp that hair does not mix across, like the parting in an animal's
fur. Draw them with Maya's curve tool on the live scalp:

```python
groomlab.draw_part_tool(groom)                           # scalp made live, EP curve tool on
names = groomlab.add_part(groom, curves, name="crown")    # stored on the groom, bound to the scalp
groomlab.add_part_node(groom, width=0.25)                 # a Part node: opens the gap
groomlab.list_parts(groom); groomlab.remove_part(groom, "crown"); groomlab.clear_parts(groom)
```

- **No mixing:** `Interpolate` and `Clump` never take a guide or a clump center from the other
  side of a part. Their `respectParts` parameter is on by default. To keep whole areas apart
  rather than the two sides of a line, paint regions and name them in Interpolate's `regionMap`
  (see Interpolate keeps to painted regions in [Getting started](getting-started.md)).
- **Opening the gap:** the `Part` node makes strands within `width` of a part lean away from it,
  on their own side, keeping their lengths. Its `strength` parameter is mappable, `profile` sets
  the lean root to tip, and `part` limits it to one part.
- **Auto parts:** set the Part node's `partsFrom` to `auto` and it finds the parts
  itself, where the hair it takes flows away to both sides of a line (a parting combed into the
  guides), and opens them as it opens drawn ones (`width`, `strength`, `profile`). `splitAngle`
  (degrees, 90) is how sharply the hair must split, between its flow on the two sides; lower finds
  softer partings. `minLength` (a share of the scalp's size, 0.15) drops shorter splits, such as a
  whorl's center. The flow is the strands' own: each one's first segment along the scalp,
  smoothed over 5% of the scalp's size, so the parts move smoothly as you groom. Found parts are
  the node's alone: Interpolate's and Clump's `respectParts` keep to drawn parts (they run before
  Part). The GPU preview opens the parts an auto Part found at its last evaluation: while you
  drag or brush upstream of it they stay where they were, and the release finds them again
  (dragging `splitAngle` or `minLength` previews on the CPU). Scripted,
  `graph.part_lines(node_id, ctx)` gives the lines a Part node opens and
  `groomlab_core.auto_parts(strands, scalp, split_angle, min_length)` finds them.
- **Storage:** parts are stored bound to the scalp (face and barycentric coordinates). They
  follow a deforming scalp, draw red with the guides, and travel in groom files.
- **Sides:** sides are measured on the rest pose. Hair past either end of a part has no side, so
  it flows around the end.
- **Command:** `groomLabParts -fromCurves/-remove/-clear/-list` edits parts, and every edit is
  undoable.

## Guide selection

The selection is stored on the groom (`guideSelection`) and drawn on top of the other guides:
white for selected, orange for soft-selected. Every change is one undo step:

```python
groomlab.select_tool(level=0)                        # lasso or click in the viewport
groomlab.select_guides(groom, level=0)               # or by guides=[...], group="..."; mode="add"/"deselect"/"toggle"
groomlab.mirror_guide_selection(groom, "x")          # the mirror images of the selection
groomlab.set_soft_selection(groom, 0.5)              # weights fall off within 0.5 of the selection
groomlab.selected_guides(groom)
```

- **In the tool:** Shift toggles, Ctrl deselects and Ctrl+Shift adds.
- **Selection as a mask:** brushes take it with `selected_only=True`, with soft-selected guides
  weighted.
- **Hierarchy tools:** they act on the selection when no guides are given.
- **In the Groom Tools:** **Select** (Tools tab) lassoes, its Selection box sets the **Soft
  radius**, and Guide groups & edits has **Mirror Selection**, **Clear Selection** and the hierarchy
  tools.

## Brushes

Select the groom and pick a brush. Radius is in pixels, strokes are screen-space (hair at any
depth under the circle is affected), and each drag is one undo step:

```python
groomlab.brush_tool("comb", radius=80, strength=1.0, level=0)   # level 0 = primaries
groomlab.brush_tool("length", radius=60, strength=-1.0)         # negative = cut
groomlab.brush_tool("place", length=0.6)                        # click the scalp to add a guide
```

| Mode | Effect |
| --- | --- |
| `comb` | Drags guide points along the stroke. Roots stay pinned and segment lengths are kept, unless **Stretch** is on (`stretch=True`): then combing out past the tips makes guides longer (and pushing in, shorter), points respaced evenly. |
| `smooth` | Relaxes kinks, keeping lengths. |
| `straighten` | Relaxes guides toward the straight line along their root direction. |
| `lift` | Turns guides toward the scalp normal; negative strength lays them down. |
| `twist` | Swings guides around their root normal; the sign sets the direction. |
| `curl` | Bends guides progressively toward the tip, keeping length; the sign sets the direction. |
| `attract` | Pulls guide tips toward the brush center, for hand-made clumps; negative pushes them apart. |
| `direction` | Turns guides about their root normal toward the way you stroke, keeping their lean and length (Yeti's Direction brush); Ctrl turns them away. Half the turn per stamp at strength 1. |
| `noise` | Adds smooth random jitter along guides, across them, calm at the root, lengths kept. Each guide has its own pattern (seeded by its ID) and every stroke builds the same one up; negative strength (or Ctrl) smooths it out. |
| `length` | Grows guides (positive strength) or shortens them evenly (negative), 5% per stamp at strength 1. |
| `cut` | Trims guides where the brush touches them: the part past the brush is removed and the rest respaced. |
| `tube` | Grows or shrinks a guide's tube ring by ring where the brush touches it (the profile gets at least 8 rings), so a tube can bulge or taper along its length. The tube is where the guide's children are spread (Children..., and children placed by hand follow it). |
| `erase` | Removes what the brush touches, all in one undo step: guides (with their children), parts and flow arrows and sinks, each switchable (`erase_guides`, `erase_parts`, `erase_flow`; the Erases box in the Groom Tools). The Part tool with Ctrl erases parts too. Scripted, `-targets` picks what an erase stroke removes. |
| `paint` | Paints a per-guide `attribute` toward `value`, for any downstream attribute map (Ctrl: a gray map toward 0, or 1 when `value` is 0). A setting's map (density, width...) is painted with it: `groomlab.paint_param(groom, "scatter", "density")` (Groom Tools: Paint tab > + New Map > for a setting) makes, links and picks it, and Ctrl on the density map removes hair. The `width` and `density` brushes are gone: naming them raises an error that says so. |
| `clump` | Paint on the `clump` map (kept for scripts). Painted clumping is just a painted map: make one (say `clump`) and let the Clump's strength read it, or `groomlab.clump_paint(groom)` does both and starts the Paint brush. |
| `place` | Click to add a guide; drag to place many, no closer than `spacing` to any root of the group. New guides interpolate the 3 nearest guides of the group (`match_neighbors`, on by default): shape turned to the new root, length, point count, tube and painted maps (each map blended over the neighbors that have it). A group with no guides yet learns from the other groups at its level, so a new group starts from the hair around it. Without neighbors, or with `match_neighbors` off, guides stand `length` tall with `segments` segments. Draw keeps the drawn shape and takes the painted maps. |
| `slide` | Moves roots along the scalp with the drag; each guide keeps its shape, turned with the surface. Fixes a root in the wrong spot without redrawing. |
| `draw` | Press on the scalp and draw the guide's shape; it is resampled to `segments` segments. |

- **Brushes on parts and flow arrows:** the shaping brushes also work on part lines and on flow
  arrows and sinks. The **Brushes** box in the Groom Tools (Guides / Parts / Flow) picks what a
  brush acts on; scripted, `groomLabBrushStroke -targets guides,parts,flow` (default `guides`).
  Parts and arrows stay bound to the scalp: a moved point is re-bound to the closest scalp
  point, an arrow's direction carried onto the new tangent plane. What each brush does there:
  `comb` slides pins, sinks and part points with the stroke; a lone arrow is laid along the
  stroke, while the arrows of a drawn flow stroke point along the stroke as it now runs, so
  combing a flow curve reshapes it and its arrows follow the curve, not the brush; `smooth` averages an arrow with its nearest arrows and evens out the arrows of
  a stroke along it, and relaxes part lines; `twist` turns arrows around the normal and turns a
  sink's angle by the same; `length` grows or shrinks a sink's radius; `attract` pulls arrows,
  sinks and part points toward the brush center (negative pushes apart); `straighten` pulls a
  part toward the chord between its ends and turns arrows along their stroke's overall
  direction; `cut` splits a part where the brush touches it (the pieces keep the name, then
  `name1`, `name2`...); `erase` removes them. Lift, Curl, Tube, Paint and Slide leave them
  alone. Symmetry mirrors the stamps as for guides, and a stroke is one undo step with its
  guide edit. In C++: `applyBrushStrokeToFlow` / `applyBrushStrokeToParts` (`brush.h`); in
  Python, `apply_brush_stroke_to_flow` / `apply_brush_stroke_to_parts`.
- **Root to tip:** the shaping brushes (comb, smooth, straighten, lift, twist, curl, attract,
  direction, noise) take a curve of their strength along each guide, root to tip
  (`strength_profile=[[0, 0], [1, 1]]`: tips only; the **Root to tip** box in the brush
  settings). Flat (or none) is the brush as always, bit for bit.
- **Falloff curve:** the falloff drawn as a curve, the weight from the brush's center to its edge
  (`falloff_profile=[[0, 1], [1, 0]]`, the **Falloff curve** box), in place of the Falloff
  slider's smooth falloff; every brush, map paint included.
- **Symmetry:** `symmetry="x"` (or y, z) mirrors every stroke across a world plane at
  `symmetry_center`; `symmetry="topology"` mirrors it across the scalp's own surface instead:
  each vertex's partner is found once on the scalp's rest pose mirrored in x (its twin, or the
  nearest point of the surface on a mesh not quite symmetric), so a head turned, tilted or
  animated mirrors as it looks. Brushes, Place, Draw, Part, Flow and map paint all follow it.
- **Keeping off the scalp:** every brush keeps guides out of the scalp (`collide=True`): points
  pushed under the surface slide along it instead, with their lengths kept.
- **Pen pressure:** with a tablet, pen pressure scales strength (`pressure=True`). Maya's API has
  no pressure, so a Qt event filter reads it; with a mouse it is 1.
- **Resampling:** `groomlab.resample_guides(groom, segments, guides=None, group=None, level=None)`
  respaces guides (default: the selected ones, else every guide) to a new point count, keeping
  their shape; frozen guides keep theirs. Picked guides (`guides`, or the selection) get segments
  of their own, which a whole group, a level or every guide resampled later leaves as they are;
  picked at their group's count, they follow the group again
  (`groomlab.reset_group_segments(groom, group)`: every guide of the group back to its count,
  frozen ones kept; the group's right-click Reset Segments). A whole group, a level or every
  guide also sets the segments their groups' new guides get (drawn, placed, from curves, baked,
  generated children);
  `groomlab.set_group_segments(groom, group, segments)` sets it alone (0: the tool's own), and
  `list_guide_groups` gives each group's `segments`.
**Working comfortably:**
- **The current groom:** the top of the Groom Tools and the Graph Editor's groom list pick one
  current groom for both and for every tool (`groomlab.set_current_groom(groom)`); it stays
  current whatever else you select, until you pick or select another groom. The Graph Editor's pin
  keeps it on its groom. The eyes beside the list show or hide the groom and its guides
  (Shift+click: every groom; GroomLab > Display; Alt+Shift+G with the hotkey set).
- **Ctrl while stroking:** reverses the brush. Signed brushes flip, paint goes toward
  1 − value (unfreeze or erase a map), and Cut grows.
- **Hover:** the guides the brush would touch light up before you press.
- **While sculpting:** by default the strands stay as they are, every strand of the preview
  (`sculptDisplay` normal, `sculptDensity` 1: **Full density** on). Dim or hide them, or turn
  Full density off (the strands drop to 0.35 × the preview density) to keep heavy grooms fast.
- **Isolate** (beside the Group choice): the brushed group and its child groups stay at full
  color and the rest dim (`groomlab.isolate_group`).
- **Guides by map:** guides can be colored by a painted map, black 0 to white 1
  (`color_guides_by_map`, or **Show on guides** in Groom Tools > Paint).
- **Freeze mask:** **Freeze** (Groom Tools > Tools) paints the `freeze` map, and brushes leave
  frozen guides (and their children) alone; they draw blue. Ctrl unfreezes, **Unfreeze All**
  (the tile beside Freeze, the GroomLab Unfreeze All runtime command) frees every guide, and
  `freeze_guides` and `unfreeze_all` do it by script.
- **Solo:** Place and Draw's **Solo new guides** (`solo_new_guides=True`; off by default) keep
  the guides a stroke makes unfrozen and freeze every other, in the same undo step, so the next
  brush works on the new guide at once. **Shift+click** with a guide brush solos the
  guides in its ring the same way (Shift+drag paints them in); a Shift+click where the ring holds
  none unfreezes them all. The **Select**
  tool solos its selection (the guides not selected freeze, a pick of nothing unfreezes all;
  `select_tool(solo=False)` turns it off), one undo step with the selection. With symmetry on
  (world X or topology) the mirror guides stay live too: a guide placed with its mirror image, a
  Shift+clicked guide's mirror guide, the mirror guides of a selection (`select_tool(symmetry=...)`,
  picked as well). In C++: `soloGuides`, `unfreezeAll`, `freezeToSelection` and
  `withMirrorGuides` (`brush.h`).
- **Hotkeys:** GroomLab > Install GroomLab Hotkeys makes a GroomLab hotkey set, then shows a small
  window listing exactly which keys it changed and the hotkey set it switched from. **Help >
  Keyboard Shortcuts** lists every key (one table for Maya, the app and Blender) and marks the
  ones that need the hotkey set.
  - **Alt+Shift+X:** symmetry on or off.
  - **Y + left click:** the marking menu, with the main brushes around the ring and the rest
    below.
  - **Page Up / Page Down:** step the brushed level.
  - **B / M (hold) + drag:** the brush radius / strength, as in Maya's sculpt tools. Outside
    GroomLab brushes B and M do what they do in Maya. Middle-drag (Shift: strength) works
    without the hotkey set.
- **Shift and Ctrl in a brush:** Ctrl is the brush's opposite (`BrushTool::pressed`: Length
  shortens, Lift lays down, Twist, Curl, Attract, Direction, Noise and Shape flip, Cut grows,
  painting goes toward 0; Comb, Slide, Smooth, Straighten and Erase have none). With a guide
  brush Shift selects (`BrushToolSettings::shiftSelects`, the hosts'): Shift+click adds the
  guides in the ring (the rest freeze; on none: unfreeze all), Shift+drag paints them in; the
  lasso is the Select tool's. Painting maps or Freeze, and with Shape (its tube channel), Shift+drag
  smooths.
  - **Your own keys:** every tool also has a runtime command (category GroomLab) you can bind.

- **Which guides:** `level` (or `group`) picks the guides a brush edits. Children still follow
  edited parents, and `propagate=True` also brushes locked descendants, which otherwise ignore
  their parents.
- **Selected guides only:** with `selected_only=True`, strokes only touch the selected guides,
  scaled by the soft selection.
- **Symmetry:** `symmetry="x"` (or `"y"` or `"z"`, with `symmetry_center`) repeats every stamp
  mirrored across the plane, so a groom mirrored with `mirror_group` stays symmetric. A guide on
  the seam changes once. Place puts a guide on each side. The Groom Tools' brush settings have
  the Symmetry choice (off, world X, topology) and every other option; Maya's Tool Settings
  window only points there.
- **Scripted strokes:** strokes can be scripted in world space, which is how the tests drive
  them. Each point stamps a brush sphere of `radius` scene units:

```python
groomlab.brush_stroke(groom, "comb", [(-1, 1.6, 0), (1.2, 1.6, 0)], radius=1.2)
```

Interactive strokes are journaled as `groomLabBrushStroke -mode comb -interactive true` for undo.
They cannot be replayed, because their weights came from the view.

## Grooming from reference

Photos of the real hair, as cameras you groom to: **Groom Tools > Photos**.

```python
import groomlab.reference as ref
cam = ref.import_reference("C:/photos/front.jpg", groom=groom)   # a camera + image plane
ref.look_through(cam)                    # the active viewport shows the photo's view
ref.set_pair(cam, 0, image=(412, 230), mesh=(0, 9.5, 3.1))  # point pairs (below)
ref.solve(cam)                           # {"rms": 0.8, "focal": 52.3, "translate": ..., ...}
ref.lock_reference(cam)                  # tumbling through it does nothing now
ref.set_reference_opacity(cam, 0.4); ref.set_reference_depth(cam, "behind")
ref.look_back()                          # back to the camera the viewport showed before (persp)
```

- **Import Photo...** makes a camera under the `groomlab_references` group with the photo on an
  image plane: its film back takes the photo's aspect (36 mm wide) and the image plane is sized to
  the film gate, so the photo fills the camera's view exactly (the whole gate shows in the
  viewport, with a little margin). It starts in front of the groom's scalp (or the scene's
  meshes) at 35 mm, aimed at it. The photo is **copied** into the scene's
  `groomlab/<scene>/references` folder and stored relative to the scene, so it travels with the
  scene and Save Scene As takes it along (`copy=False` keeps the original file). The image plane
  shows only while looking through its camera.
- **Look Through** shows the photo's camera in the active viewport; again (**Back to
  Perspective**) returns to the camera it showed before. The list follows the viewport: looking
  through a reference any other way picks it in the tab.
- **Opacity** (the image plane's alpha gain) and **In front / Behind**: in front, the photo draws
  over the mesh and you see the mesh through it; behind, the mesh draws over the photo. The image
  plane's depth is set between the camera and the subject or past it, and follows the camera.
  The eye hides the photo.
- **Lock** fixes the camera where it is: Maya's Lock Camera (`camera -lockTransform`: tumbling,
  tracking and dollying through it do nothing), plus its scale, focal length, film back and the
  photo's placement on the image plane, all locked attributes. Opacity and side still change. A
  locked camera is not re-aligned (unlock it first).
- **Focal Length** sets the camera's focal length by hand (mm); Keep focal length turns on, so the
  alignment keeps it.

**Align Photo...** Line the camera up with the mesh from point pairs, a feature on the photo
(the corner of an eye, the tip of the nose, an ear lobe) and the same feature on the mesh, in the
Photos window (`groomlab.ui.reference_window`, its Align stage; built like the Transfer Groom
window from `mesh_views.py`):
- The photo on the left (the wheel zooms at the cursor, the middle button pans, F fits), the
  scalp on the right in a Maya view of its own. A click on either side adds a point; the Nth
  photo point pairs with the Nth mesh point, numbered on both sides (photo yellow, mesh cyan).
  Drag a point to slide it; Ctrl+click deletes it and its partner. Ctrl+Z / Ctrl+Y undo and redo
  the photo's edits (the window's own stack).
- From 4 complete pairs the camera lines up live (the focal length kept); 6 or more, spread in
  depth, solve the focal length too, unless **Keep focal length** (kept on the camera,
  `glRefKeepFocal`). The mesh's outline or wireframe draws over the photo through the camera,
  each pair shows its error in pixels, and pairs that disagree with the rest turn red.
- The window's **Focal Length**, **Keep focal length** and **Lock** are the camera's, as in the
  tab. A locked camera is not moved by the landmarks.
- `ref.solve(cam)` does the same from a script: 6 complete pairs (4 with Keep focal length, or
  `fix_focal=True`); it sets the camera's position, rotation and focal length (the film back
  stays) as one undo step and returns the average and per-pair errors.
- The solve is a pinhole camera (principal point at the photo's center, square pixels, no lens
  distortion) from a Direct Linear Transform estimate refined by Levenberg-Marquardt, with a
  spread of starting views so a camera far from right still converges (`engine/core/include/groomlab/camera_solve.h`).
  `groomLabCameraSolve` runs it on any points: `-imagePoint u v` and `-worldPoint x y z` (one of
  each per pair; pixels from the image's top-left), `-imageSize w h`, `-filmBack`, `-focal`,
  `-fixFocal`, `-camera` (start from that camera); it returns the camera's translate, rotate
  (xyz order, degrees), focal length, RMS error and each pair's error, changing nothing.

A reference keeps its settings on its camera (saved with the scene): `glRefImage`, `glRefWidth`,
`glRefHeight`, `glRefPairs` (JSON), `glRefDepth`, `glRefRms`, `glRefKeepFocal`. `ref.list_references()`,
`ref.reference_info(cam)`, `ref.pairs(cam)`, `ref.set_pair`, `ref.remove_pair`, `ref.set_keep_focal`,
`ref.clear_pairs`, `ref.rename_reference` and `ref.remove_reference(cam, delete_file=False)` do
the rest.

## Flow arrows (guideless short hair)

For vellus, short fur, brows and other short hair, groom a direction field instead of guides.
With the **Flow** tool (Groom Tools > Tools, or `groomlab.flow_tool(groom)`), drag on the scalp:
arrows go down along the stroke every half a brush radius, and arrows the brush passes over turn
toward the stroke; Ctrl+drag erases. Between arrows the field blends smoothly (the nearest
arrows, inverse-distance weighted, carried onto the surface), so a few strokes make partings
and sweeps.

**Sinks** are points hair turns around: set the Flow box's **Place** to Whorl, Whorl
(clockwise), Gather or Part out and click on the scalp. Each sink is the size of the brush;
within it hair spirals out (a crown whorl), gathers in, or points away (a cowlick), fully near the
center and fading into the arrows at its edge. Ctrl-click removes sinks; symmetry places a
mirrored twin (a whorl's mirror turns the other way). Sinks draw as a ring with streamlines, and
alone (no arrows) they make a field too.

The **Flow** node (Shape) lays strands along the field: each leaves its root `lift` degrees off
the skin, heads the way the field points there and bends toward the skin along its length
(`bend`), turned at random up to `rollRandom` degrees around the normal; the common `affect` blends from the incoming shape. Without
arrows, strands lie along their own heading. `Scatter -> Length -> Flow -> Width` grooms fur with
no guides at all: the `creature_fur` preset is a starting point.

**Guides in a flow groom.** The `flow_with_guides` preset grooms the coat along the flow and lets
the guides take over where you paint: two looks of Scatter's strands joined by a **Blend**, `a`
the fur (`Scatter -> Length -> Flow`), `b` the guides' shape and length (`Interpolate -> Clump`),
its `mix` driven by a **Map** node reading the vertex map `guideAreas`. Applying the preset makes
the map black, so the groom starts as fur everywhere (a guide drawn changes nothing yet); paint it
white in Groom Tools > Paint where a tuft, a ruff, a mane or longer hair should follow the guides
(grays blend the two). Place or Draw those guides in their own group if you like (e.g. `accents`).

The other way is a radius: put an `Interpolate` after Flow and give it a `limitDistance` (its
Limit group, with `falloff` and `influenceChannel`, which do nothing at 0). Each guide then only
takes the strands within that distance of its root, blending by the `falloff` curve (full at the
guide, none at the edge; edit it in the Graph Editor), and everything farther keeps the flow.
`take` "length" keeps the flow's direction and only brings nearby strands to the guides' length.
The influence (0-1) is written to the `guideInfluence` channel (`influenceChannel`), so later
nodes can use it through an attribute map (a Clump whose `strength` reads it clumps only near the
guides). With `limitDistance` 0, Interpolate shapes every strand, as usual.

**Presets fit the scalp.** A preset records the scalp size it was made on (`"size"`: the
largest extent of the scalp's bounding box). `apply_preset` (and the Graph Editor's Presets menu)
fits it to the groom's scalp: params in length and width units (strand length, radii, widths,
noise amplitudes, the min and max of their maps) scale with the size, densities with its square,
and rates per unit of length (Curl's turns, Twist's turns, Braid's crossings, Wind's and
FieldNoise's frequency: their spec's unit `perLength`) with its inverse, so curls keep their count
per strand. Noise's `frequency` is per strand length by default and does not change.
A preset made on a head works on a 1 cm test sphere. `save_preset` records the size;
`apply_preset(groom, name, fit=False)` applies the numbers as they are.

- Each stroke draws as one curve along its arrows with an arrowhead at its end (it breaks where
  arrows were erased); the field draws as a pale preview (`displayFlow`). Arrows are bound to the
  scalp and follow it when it deforms, and travel with groom files (`"flow"`: each pin records its
  `line` and `seq`).
- With **Flow** ticked under Acts on in the brush settings, the shaping brushes work on the arrows and sinks
  too: Comb slides them with the stroke and lays arrows along it, Smooth
  averages arrows with their neighbours and evens out a stroke's arrows, Twist turns arrows
  and a whorl's angle, Length sizes a sink's radius, Attract gathers them, Straighten lines a
  stroke's arrows up (see Brushes). The field previews live as you drag.
- Symmetry mirrors strokes and sinks. Each stroke or click is one undo step. The Flow box's
  **Clear All** removes every arrow and sink; the Erase brush removes both (`erase_flow`). Sinks
  travel in `flowData` (`"sinks"`: face, u, v, `angle` in degrees, 0 out, 180 in, positive
  counter-clockwise, and `radius`).

```python
groomlab.flow_stroke(groom, points, radius=0.2)          # scripted; erase=True removes
groomlab.add_flow_node(groom, after="interpolate", lift=10, bend=0.6)
groomlab.flow_sink(groom, point, kind="whorl", radius=0.3)  # whorlcw, converge, part; erase=True
groomlab.flow_arrows(groom); groomlab.flow_sinks(groom); groomlab.clear_flow(groom)
```

## Correctives (sculpt layers and shot fixes)

The groom is a stack, as with Yeti's layers: the **Base groom** (its guides) at the bottom and
**correctives** on top. In Groom Tools > Correctives, click the row brushes should edit:
the Base groom edits the guides; a corrective records each stroke as a correction and leaves the
guides alone.

- **Weights are keyable.** Each corrective's weight is an attribute on the groom's transform
  named after it (in the Channel Box with the groom selected), driving the shape; its keys show in
  the timeline. Unkeyed, a corrective applies on every frame at its weight (**+**: a new one; use
  them to try looks side by side and blend them). For a shot fix, key it: **Fix Frame** (the ◆+
  button) makes a corrective that is fully on at the current frame and blends in over the 6
  frames before and out over the 6 after (keyed 0, 1, 0), so the fix doesn't pop. Drag or add
  keys in the timeline or Graph Editor like any animation (more keys, holds, eases). The **Weight** slider sets it on this frame; once it is
  keyed, changing it keys it there (◆ keys it by hand). Above 1 exaggerates.
- **The list:** each corrective's weight on the current frame and its keys (◆ 3, or "all
  frames"); tick to show or hide, double-click to rename, ↑ / ↓ to reorder, **S** to solo
  (again for all), ✕ to delete it and its keys. Right-click: Go to Its Frame (its strongest
  key), Key Weight Here, Clear Keys, Rename.
- Offsets live in each guide's own frames, so a fix made on frame 1012 follows the hair as it moves
  (a simulation or a guide cache). Children follow corrected parents, and only guides you actually
  sculpted get offsets.
- **Renders** include them: the MtoA translator, groom files (`"correctives"`, keyed weights
  sampled per frame into a `weightCurve`) and so the procedural and groomlab-cli.
- **Tools:** shaping brushes only (not Place, Draw, Erase, Paint or Part). Each stroke is one
  undo step.
- Older shot layers (with fade-in/out frames) turn into keys the first time the Correctives list
  shows them (`shots.upgrade(groom)`).

```python
from groomlab import shots
shots.add_layer(groom)                              # a corrective on every frame; brushes edit it
shots.add_layer(groom, frame=1012, fade=6)          # a shot fix keyed 0 -> 1 -> 0 around 1012
shots.set_layer(groom, "sculpt1", weight=0.5, enabled=False, rename="look")
shots.key_weight(groom, "shot1012", 0.7, frame=1015); shots.clear_keys(groom, "look")
shots.weight_plug(groom, "shot1012")                # "groom.correctiveWeight[i]": key it like anything
shots.set_target(groom, None)                       # back to editing the base groom
shots.layers(groom)                                 # name, enabled, weight, keys, frame, guides
shots.solo_layer(groom, "look"); shots.move_layer(groom, "look", 1); shots.remove_layer(groom, "look")
```

`groomLabGuides -guidePoints <id> -evaluated` returns a guide as shown (posed and corrected).
