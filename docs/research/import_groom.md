# Importing an Alembic groom into GroomLab

Ideas note, 2026-10-27. Nothing here is built yet. It started from a tester who exported a groom
from Unreal as Alembic, brought it into Blender, and wanted to combine it with a GroomLab braid.
They could not select the braid: GroomLab's hair object is the add-on's live output, locked on
purpose (`hosts/blender/addon/groomlab/groom.py`, `ensure_hair_object`).

The question behind it: **how does a finished groom from somewhere else (Unreal, MetaHuman, XGen,
Houdini, Ornatrix, an Alembic from a client) become something GroomLab can combine with, restyle or
rebuild?**

## Verdict

One **Import Groom** window, laid out like Transfer Groom to Scalp: the file on the left, the
result on the right, and a **Method** dropdown. Three methods, from exact to procedural:

| Method | The original look | What you can do after | Effort |
| --- | --- | --- | --- |
| **Keep as imported** | exact | combine (Merge), cut, recolor, widths, Collide, cards, export | small |
| **Make editable** | exact at rest | all of the above, plus comb and sculpt it through guides | medium |
| **Rebuild as GroomLab** | changes | a full procedural groom: density, lengths, clumps, every look node | larger, quality uncertain |

Build them in that order. Keep as imported answers the tester and anyone combining with Unreal or
MetaHuman hair. Make editable is how Unreal and Houdini already let you restyle imported hair.
Rebuild is a starting point, never a copy.

## 1. What is in an Alembic groom

An Alembic groom is curves: the final strands, sometimes the guides too, and a few attributes.
Unreal's groom schema
([Using Alembic for grooms](https://dev.epicgames.com/documentation/unreal-engine/using-alembic-for-grooms-in-unreal-engine))
is the common one:

| Attribute | Scope | Meaning | What GroomLab does with it |
| --- | --- | --- | --- |
| `groom_guide` | constant or uniform, int | 1: these curves are guides (Unreal simulates them), 0: strands | split the file into guides and strands |
| `groom_group_id` | uniform, int | the strand's group (Unreal allows up to 15 for MetaHumans) | a strand attribute, and a group filter in the window |
| `groom_root_uv` | uniform, float2 | the root's UV on the scalp | bind roots by UV when the scalp has UVs |
| `widths` / `groom_width` | per point or per curve | the width | the strands' widths |
| `groom_color`, `groom_roughness` | per curve or point | color, roughness | HairColor's inputs, or strand attributes |
| `groom_guide_weights` | uniform | interpolation weights made outside Unreal | Make editable's binding, when present |

**Guides are in the file when the artist tagged them.** XGen and Houdini exports often have them,
and our own Unreal export writes the primary guides with `groom_guide` 1
(`engine/io/src/alembic_io.cpp`). When none are tagged, Unreal makes its own by picking a share of
the strands. A file that does not follow the schema still imports as plain curves, with no
attributes.

Other things a file can hold: several curve objects (one per group or per LOD), animation (a
simulation cache), and roots that float a little off the scalp it was groomed on.

## 2. What other tools do

- **Unreal and Houdini never rebuild an imported groom.** They keep the strands and **deform them by
  guides**: each strand is bound to nearby guides with weights, and moving a guide moves its strands
  by that change. Houdini has Guide Deform and Hair Generate's "Create Guide & Weight Attribs"
  ([Hair Generate](https://www.sidefx.com/docs/houdini/nodes/obj/hairgen.html),
  [Configure Guide Deform](https://www.sidefx.com/docs/houdini/nodes/lop/configureguidedeform.html)).
  Unreal deforms the strands by its simulation guides.
- **The known pitfall:** guides shorter than the hair they drive leave the tips behaving badly.
  Houdini users fixed it by making every guide as long as the longest strand it drives
  ([SideFX forum](https://www.sidefx.com/forum/topic/74463/?page=1)).
- **XGen, Yeti and Ornatrix** import curves as guides for a new groom. None of them turns finished
  strands back into a procedural setup. (This was not exhaustively checked for every package.)

## 3. What GroomLab already has

- **ImportCurves** (`engine/io/src/import_curves_op.cpp`): an Alembic file's curves as **guides**,
  resampled, flipped when drawn tip-first and rooted on the scalp. It reads every curve as a guide
  and ignores `groom_guide`.
- **Reference** (`engine/core/src/operators/reference_op.cpp`): another `.groom` file's strands or
  guides, bound onto this scalp. The binding code is what Keep as imported needs, for Alembic
  instead of `.groom`.
- **ToGuides**: guides picked from strands. **Interpolate**: strands from guides with a binding,
  incremental under brushes. **Merge**, **Split**, the look nodes' `affect` masks.
- **Transfer Groom to Scalp**: the window to copy, in Maya and the app
  (`docs/reference/pipeline.md`).
- **The Unreal export** writes the schema above, so a GroomLab groom round-trips its guides.

## 4. The Import Groom window

Maya: GroomLab > Utilities > **Import Groom...**; Blender: the GroomLab menu; the app: File >
**Import Groom...**. Not modal, like Transfer.

- **Left view:** the file. Strands in their color, guides (when the file has them) drawn over them,
  the scalp mesh it is bound to. A summary line: `48,120 strands, 312 guides, 4 groups, widths,
  root UVs, 1 frame`.
- **Right view:** the result with the method's settings, updated as they change (a preview density
  for Rebuild).
- **File:** the `.abc`, and its **Objects** and **Groups** checklists (`groom_group_id`), to import
  some of it.
- **Scalp:** the groom's scalp (the active groom), or a new groom on a picked mesh. **Bind roots
  by:** UV (when the file has `groom_root_uv` and the mesh has UVs), closest point, or as is (no
  scalp: the hair stays where it is in space).
- **Space:** up axis and units, guessed from the file (Unreal's Alembic is in Unreal's space; ours
  says so), with Y up / Z up and a scale to override. A **Frame** button and a frame field for an
  animated file.
- **Method:** the three below, each with its own options.
- **Status line:** what will happen and why Import is disabled (`no scalp`, `the roots are 3 cm off
  the scalp: check the space`).
- **Import:** one undo step. It adds the nodes to the graph (never hidden: the user can see and
  change what Import made), and a report of what came along and what did not.

## 5. Method: Keep as imported

The strands exactly as in the file, as a branch of the graph.

- ImportCurves grows into an **Import Groom** node with two outputs, **strands** and **guides**:
  the curves with `groom_guide` 1 go to guides, the rest to strands. Without the flag, everything is
  strands (a param: `curvesAre` strands / guides / split by flag).
- Strands keep their exact points: no resampling by default (`samples` 0). Widths, group IDs,
  colors, roughness and root UVs become strand attributes.
- Roots bind to the scalp by UV or closest point, so the hair follows a deforming scalp, and Scatter
  density masks, Cull and maps read it like any strand.
- **The graph Import makes:** Import Groom (strands) → **Merge** ← the groom's own hair. The user's
  look nodes can go before the Merge on either side, or after it for both.
- Look nodes on imported hair work as they do on any strands. `affect` masks let them touch only
  part of it (a little noise and frizz on the tips, recolored roots).
- Animated files: the strands at the evaluation time (ImportCurves already reads at the time).

This alone solves the tester's case. In Blender they would do it all inside GroomLab: import the
Unreal groom, Merge with the braid, export both to Unreal.

## 6. Method: Make editable

The industry's answer. At rest the result is exactly the file; comb a guide and the strands bound
to it follow with all their detail.

- **Guides:** the file's (`groom_guide`), when it has them. Otherwise generated:
  - group the strands by where their roots are on the scalp (roots spread evenly: a target count
    or spacing, like Clump's own pick), and make each group's guide its **average shape**: the
    strands resampled by arc length to the same point count, summed relative to their roots. An
    average combs better than one picked strand, which carries its own noise and curl.
  - extend each guide to the longest strand it drives (the Houdini lesson), along its last
    direction.
  - an option to pick real strands instead (ToGuides), for sparse fur where averages would shrink.
- **Binding:** Interpolate's, from the strands' roots to the guides' (respectParts, followSurface,
  regions all apply). `groom_guide_weights` when the file has them.
- **Offsets:** each strand stores where it is relative to its guides' blended shape, sample by
  sample in the guides' frames (rotation-minimizing, from the root normal), as a fraction of the
  guide's length. A new Interpolate mode, `take` **offsets** (beside shape and length): the strand
  is the blended guide shape plus its own offsets. At rest that gives back the file exactly; the
  test is a bit-for-bit compare. If it cannot, the offsets carry the remainder.
- Everything else comes along: guide brushes, symmetry, masks, the GPU preview (the offsets are a
  per-point channel, so the Interpolate stage reads them like a layer), incremental dabs.
- **Limits:** large restyles stretch the original's detail (a curl combed straight stays a curl
  laid along the new shape); guides that cross parts or partings need regions.

## 7. Method: Rebuild as GroomLab

A new procedural groom that matches the original's coverage, lengths and flow. It is a starting
point, not a copy: Scatter and the look nodes make new strands.

- **Measured into maps on the scalp:**
  - **density:** roots per area, splatted onto the scalp's vertices or a texture, normalized, and
    Scatter's count set to the file's strand count.
  - **length:** each strand's length at its root, as a Length map.
  - **width:** the file's root width (and taper: the width along the strand's length, averaged into
    Width's profile).
  - **color:** `groom_color` when present, as a color map for HairColor.
  - **groups:** a region map from `groom_group_id`, so Interpolate keeps groups apart.
- **Guides:** as in Make editable (the file's, or group averages).
- **A starter graph:** Scatter (density map) → Interpolate (the guides, regions) → Length (map) →
  Clump → Width (map, profile) → HairColor (map).
- **What can be measured and what cannot:**
  - clump size: where strand tips gather. Group strands whose tips are close together but whose
    roots are not, and set Clump's size and count from the groups' spread. Reasonable.
  - curl and noise: frequency and amplitude from each strand's deviation from its smoothed
    self. A rough first value at best.
  - the original's exact look: no. Many setups make similar hair, so the fit is a guess.
- The window shows the rebuild next to the original, so the user sees what changed before
  importing. Useful for MetaHuman hair: it becomes a groom GroomLab's density, clump and look
  nodes act on.

## 8. Related: Bake Hair to Curves

A separate, small fix for the same tester. A button (Blender first, then Maya and the app) that
leaves an unlocked, static snapshot of the groom's hair as a plain curves object, beside the live
one, for anything outside GroomLab (joining with other hair, sculpting by hand, other exporters).
Until then the workaround is GroomLab > Export Alembic, then File > Import > Alembic.

## 9. Order and size

1. **Import Groom node + Keep as imported** (ImportCurves extended: guides by flag, strands
   unresampled, attributes, UV binding; the window with this one method). A few days, plus the
   window in each host.
2. **Bake Hair to Curves** in Blender, Maya and the app. A day.
3. **Make editable** (generated guides, Interpolate `take` offsets, its GPU stage). One to two
   weeks; the risk is the rest-pose exactness and long guides.
4. **Rebuild as GroomLab** (the measurements, the starter graph, the side-by-side). One to two
   weeks; quality has to be judged on real grooms (MetaHuman, an XGen export, our own exports).

## 10. Tests

- Round trip: a GroomLab groom exported to Unreal Alembic, imported with Keep as imported: the
  same strands bit for bit, the guides from the `groom_guide` flag.
- Make editable at rest gives back the file exactly; a comb dab moves only the strands bound to
  the combed guides, and matches a fresh evaluation (the incremental tests' pattern).
- Binding by UV vs closest point on a scalp with UVs, and the "roots far off the scalp" warning.
- Rebuild: the density map's total matches the strand count; lengths within a tolerance of the
  file's.

## Open questions

- Does the imported file live beside the groom (a path, re-read when it changes, like
  ImportCurves now) or get copied into the groom file? A path keeps groom files small; a copy
  keeps them self-contained (Collect Files could cover it).
- Very large files (MetaHuman grooms near a million strands): Keep as imported should stream at a
  preview density like Scatter's.
- Several LODs in one file: import one, or keep them as LOD sources for the card and LOD exports.
