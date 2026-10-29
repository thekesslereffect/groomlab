# Groom library and grafting

Research note, 2026-10-29. Design study, no prototype yet.

The prompt: Ornatrix added "grafting", an asset browser where an artist picks a groom asset and
applies it to a mesh through an editable wrap projection. For production studios the goal is
wider: artists publish groom assets once, every groom artist in the studio browses them, and an
asset lands on a new character with its placement adjustable before it is baked. It does not have
to work the way Ornatrix does.

## Verdict

| Idea | Verdict | Why |
| --- | --- | --- |
| **Groom library** (asset package, studio/user/shipped roots, browser, Publish) | **Go: phase 1** | The search-path pattern exists (`paths.library_dirs`, used by presets, alphas, library nodes). `.groom` already moves between Maya, Blender and the app, so one library serves all three hosts. |
| **A. Landmark retarget** (landmarks saved with the asset, clicked on the target) | **Go: phase 1** | It is the existing `markersWarp` transfer with the asset's side of the pairs filled in at publish. Only the target side is clicked, guided, with symmetry. |
| **B. Same-topology match** (studio base head) | **Go: phase 1** | The `uv` transfer method already maps exactly. The library only needs to know which assets share the target's layout, then placement needs no clicks. |
| **C. Decal patch grafting** (regional assets placed with a surface gizmo) | **Go: phase 2** | Needs a local surface frame around a dropped point, a gizmo, edge fading and a merge into the current groom as a guide group. New work, but it reuses mapping, map projection and groups. |
| **D. Editable wrap** (markers become live handles) | **Go: phase 2** | `TransferMarkerEditor` and the warp already exist. What is missing is a live, lower-density preview while a handle is dragged. |
| **E. Live referenced graft** (a Graft node pointing at an asset version) | **Later: phase 3** | The most production value (an asset update reaches every character), but it brings versions, pins and missing assets. The File Path Editor's dependency API covers part of it. |
| **F. Regrow from flow** (flow field and maps projected, guides grown fresh) | **Parked** | Survives very different heads (human to ogre, a muzzle) where point mapping folds. Research-level. |

## 1. What GroomLab has today

- **Transfer Groom** (`engine/core/include/groomlab/transfer.h`): five mapping methods: closest,
  bounds, markersAlign (least-squares similarity), markersWarp (polyharmonic spline warp) and uv.
  Guides, parts and flow arrows are re-rooted; guide shapes turn and scale with the mapping's local
  rotation and scale (never shear); painted maps are projected (`projectVertexMap`,
  `projectTexture`); `fitSize` and `keepLengths` handle head size.
- **Marker editor** (`transfer_markers_edit.h`): the Wrap-style two-view model behind every host's
  Transfer window: ordered pairs, symmetry groups, undo. Markers are stored as JSON on the groom
  (`transferMarkers`).
- **Libraries**: `library_dirs(path_env, user_dir, shipped)` looks up an env path, the user's
  `Documents/GroomLab/...` folder and the shipped folder, earlier entries hiding later ones.
  Presets, brush alphas and library nodes use it.
- **Groups**, **Collide**, **studio lighting presets** (for thumbnails) and the **File Path Editor**
  dependency API (`file_dependencies`, Collect Files).

So the missing parts are the asset package, the browser, publishing, and placement that feels
direct. The mapping math is in place.

## 2. Where assets live

Not in the install folder: updates replace it, and each artist's copy would differ. Three roots,
the same lookup as presets:

| Root | Location | Use |
| --- | --- | --- |
| Studio | every folder on `GROOMLAB_LIBRARY_PATH` (network share, version control) | Shared by every artist; read-only for most |
| User | `Documents/GroomLab/grooms` | Personal assets and drafts |
| Shipped | the install folder | Samples, later content packs |

An asset is a folder, `Name.groomasset/`:

```
Name.groomasset/
  asset.json        name, tags, region, author, version, units, up-axis, GroomLab version,
                    landmarks, base topology id (if any), strand count, length range
  groom.groom       the groom: guides, maps, graph, groups
  scalp.abc         the reference scalp it was groomed on (the transfer's "old" mesh)
  thumb.png         rendered at publish with a studio lighting preset
  turntable.webp    optional
  textures/         its file dependencies, collected at publish
```

A folder diffs and versions well in version control and is easy to read on a share. Content packs
can ship the same folder zipped. Versions sit side by side (`Name.groomasset/v003/`) or follow the
studio's version control; the browser shows whichever the root holds.

`asset.json` records the GroomLab version that wrote it, so an asset from a newer GroomLab gets a
clear message instead of a failure (as the Graph Editor does since 0.35.2).

## 3. Placement methods

### A. Landmark retarget

At publish the artist clicks a standard landmark set on the reference scalp: ear tops, temples,
outer eye corners, crown, nape, chin (for facial hair). They are stored in `asset.json` as the old
side of the marker pairs.

On a new character the artist clicks the same landmarks in order. A small picture shows where the
next one goes, and symmetric landmarks are placed in pairs (`addMirrored`). Then the existing
`markersWarp` transfer runs (4+ pairs, or `markersAlign` with 3).

Fits whole-head grooms on heads of one species with different proportions.

### B. Same-topology match

Studios often wrap every character to one base head topology. An asset authored on that base maps
exactly through the `uv` method (or vertex ids). `asset.json` carries a base topology id; when the
selected mesh matches, placement skips markers.

### C. Decal patch grafting

For regional assets: eyebrows, beards, mustaches, sideburns, lashes, fur patches.

- The asset is stored about its own center point in a local surface frame (a geodesic polar map
  around the center, so it unrolls onto a new surface without depending on the source head's shape
  far away).
- The artist drops it on the mesh (the drop point is the anchor), then moves it along the surface,
  rotates it about the normal and scales it with a gizmo. Mirror places the other side.
- Edges fade with a mask and a density falloff, so the patch blends into hair already there.
- Bake adds the patch as a new guide group in the current groom (its own look through group
  parameters), or as a separate groom.

### D. Editable wrap

After any first fit (A, B, or bounds), the markers become handles in the viewport. Dragging one
re-solves the warp while a lower-density preview of the strands follows. Clicking adds a marker to
pin or bend a spot. Mostly UI on top of `markersWarp` and `TransferMarkerEditor`; the cost to watch
is re-solving the spline and re-rooting the preview strands at drag rates.

### E. Live referenced graft

A Graft node holds an asset path, a version (or "latest") and its markers, and merges the mapped
asset into the groom on evaluation. When the studio publishes eyebrows v003, every character on
"latest" picks it up; pinned ones stay. Bake freezes it into plain guides.

Like Maya references. It needs version resolution, a missing-asset state (the File Path Editor
shows the path) and a rule for edits made on top of a live graft (correctives layered above it,
kept when the asset updates).

### F. Regrow from flow

Instead of moving guides point by point, the asset stores a flow direction field plus length,
density and clump maps on its scalp. These are projected to the target and guides are grown fresh
from the field. Point mapping folds or stretches when shapes differ a lot (a human groom on an ogre
or a muzzle); regrowing does not. Needs a flow-field-to-guides generator good enough to match the
original look. Parked.

## 4. The browser

1. A dockable panel (Maya, Blender, app) with a thumbnail grid, search, and filters by tag,
   region, length and type. Favorites and a version dropdown.
2. Select a mesh, then double-click an asset or drag it onto the mesh (the drop point anchors a
   patch).
3. Auto-fit picks the best method available: B when the topology matches, else A when the asset
   has landmarks, else bounds.
4. A preview shows on the target. Adjust with the gizmo (C), marker handles (D), and a mask brush
   to trim or extend coverage.
5. Apply chooses what comes over: guides, maps, look (the graph), or all of it.
6. **Publish to Library** from the current groom renders the thumbnail, asks for landmarks and
   tags, collects textures and writes the package to the user or studio root.

## 5. Hard parts

- **Interpenetration after a warp**: run Collide against the new scalp after placement.
- **Patch edges**: blending a regional patch into existing hair (fade mask, density falloff,
  matching the neighbors' direction near the border).
- **Size**: `fitSize` keeps densities and widths right when the head is bigger or smaller.
- **Preview speed** while dragging handles: a strand budget for the preview, full density on
  release.
- **Version skew**: assets from newer GroomLab versions, and live grafts across version changes.

## 6. Plan

1. **Phase 1**: package, the three roots, the browser, Publish, placement through A and B on the
   existing transfer. Studios can share whole-head assets.
2. **Phase 2**: C and D: regional patches with the gizmo, live editable wrap, merge as a group.
3. **Phase 3**: E: live referenced grafts with versions and pins.
4. F stays parked.

Open questions for studios:

- Whole-head assets or regional patches first?
- Is a shared base head topology common?
- Baked copies, or live links that follow asset updates?
