# Rendering grooms in RenderMan XPU through XGen

Research note, 2026-11-02. Shelved: nothing here is built. A spike proved the route works, but the
conversion as tested is too slow for XPU, whose point is fast feedback. Revisit if XPU users ask
again or the conversion can be made close to free.

## The problem

RenderMan XPU renders no procedurals. Every `Ri:Procedural` is rejected ("Geometry ... is of
unsupported type"), including RenderMan for Maya's own RenderManArchive. GroomLab's grooms reach
RenderMan through a procedural, so XPU leaves them out. Since 0f6c507 GroomLab warns about it and
points users to RIS (`hosts/maya/scripts/groomlab/renderman.py`, `warn_if_xpu`).

XPU does render plain `Curves`, and with GroomLab's PxrMarschnerHair network. It also renders
XGen Interactive Groom, because RenderMan for Maya translates `xgmSplineDescription` itself (its
`MayaTransXGenGroom` translator reads the spline data through XGen's spline API and emits curves).

## The idea

Make GroomLab's strands look like an Interactive Groom: write them in the Alembic layout of XGen's
spline cache, then load that into an `xgmSplineDescription` with `xgmSplineCache -import`.
RenderMan then renders XGen's own node, which XPU accepts.

Writing XGen's in-memory spline data (`xgmSplineData`) directly is not an option. Maya ships
`plug-ins/xgen/include/xgen/src/xgsculptcore/api/XgSplineAPI.h`, but it only reads that data
(`XgFnSpline::load`, `XgItSpline`) and has no way to create it.

## Spike result (Maya 2026, RenderMan 27.3)

| Step | Result |
| --- | --- |
| Native groom (`xgmCreateSplineDescription`), cache export, import into a new scene | 1219 splines out, 1219 back |
| GroomLab's own Alembic (`export_alembic`), import | 0 splines (as in the earlier attempts) |
| GroomLab strands rewritten in XGen's layout, 2000 strands | 2000 splines; renders in XPU |
| Same, 100,028 strands in one curve object | only 40,000 splines; the rest dropped silently |
| Same, 100,028 strands in groups of 20,000 | 100,028 splines; renders in XPU |

Unsolved in the spike: color (the render used XGen's default `hairPhysicalShader`, grey), and
keeping the cache in sync with edits, interactive renders and animation.

## XGen's spline cache layout

What `xgmSplineCache -export -j "-file <abc> -df ogawa -fr 1 1 -step 1 -obj <transform>"` writes
(Ogawa, written by "XGen Spline Abc Writer"):

- An Xform per description (`/groomXgen`), with user properties:
  - `Width`, `WidthTaper`, `WidthTaperStart`: float.
  - `WidthRampPositions`, `WidthRampValues`: float arrays `[0, 1]` and `[1, 1]`.
  - `WidthRampInterps`: int array `[1, 1]`.
  - The three arrays have two samples.
- One or more curve objects under it, `SplineGrp0`, `SplineGrp1` and so on:
  - Curve type cubic, basis B-spline, non-periodic.
  - Clamped knots per strand, `n + 4` of them: `0 0 0 0 1 2 .. n-4 n-3 n-3 n-3 n-3`.
  - `uv`, per vertex: the strand's root UV repeated.
  - `width`: the native file has one constant value.
- Properties beside `.geom` on each curve object:
  - `ItemName`: string `.boundMesh[0]`.
  - `guid`: string, `{UUID}`.
  - `InWidthPerCV`: float per vertex.
  - `InDirectionPerCV`: V3f per vertex, the ribbon's width direction.
  - `densityMask`: uint32 per strand, 0.
  - `InWidthPerCV` and `InDirectionPerCV` have two samples, at 1/24 and 2/24.
- **One curve object holds at most 40,000 splines.** Import drops the rest without a warning.
  Write groups of 20,000.

GroomLab's Alembic differs in all of these: linear curves, no knots, per-strand UVs, colors and
melanin in `.arbGeomParams`, and none of the XGen-only properties. That is why the import loads 0
splines.

## Why it is shelved: cost

| Step (100,028 strands, 1.7M points) | Time |
| --- | --- |
| Convert GroomLab's Alembic to XGen's layout (Python, element by element) | 15 s |
| Start mayapy, import the cache, save the scene | 12 s (mostly mayapy startup; import alone not measured) |
| XPU batch render, 480x360 | about 15 s, including Maya startup |

Fifteen seconds of conversion before every XPU render defeats the purpose of XPU. The 15 s is the
spike's Python loop over 1.7M points. A C++ writer in the plugin, writing straight from the
evaluated strands, would not need that loop; how fast it would be has not been measured. Before
any rebuild, measure:

1. A C++ writer: time to write the cache from evaluated strands.
2. `xgmSplineCache` loading the file: time per frame on a large groom.
3. Whether an edit can rewrite one group instead of the whole file.

If those are not well under a second for a production groom, the route is not worth building.

## Measured again: fast enough (2026-11-16, issue #82)

`writeXGenSplineCache` (engine/io, C++) writes the layout above from a `StrandSet`. The same
100,000 strands of 17 points, Maya 2026 and RenderMan 27.3 on the dev laptop:

| Step | Time |
| --- | --- |
| Write the cache (C++, 44.6 MB, 5 groups of 20,000) | 51-69 ms |
| `xgmSplineCache -import` | 0.03 s |
| First query of the description (XGen reads the splines) | 0.23 s |
| The cache node's file set again (a rewritten cache read again) | 0.23 s |
| XPU render, 480x360, 128 samples | 2.9 s |

About 0.3 s from evaluated strands to an Interactive Groom XPU renders, against the 15 s that
shelved it. The test `XGenSplineCache.AHundredThousandStrandsWriteFast` reports the write time;
the Maya probe is build/probe/xgen82/load.py.

### Color: one look per description

RfM's XGen groom translator (`MayaTransXGenGroom`, which reads XGen's `XgItSpline`) writes only
`vertex point P`, `varying float width` and `varying normal N` for Interactive Groom curves
(RIB written with `rmanRender -ribFile`):

- Arbitrary params in the cache (`Cd`, a custom color, per strand) are dropped.
- The cache's per-vertex UVs are dropped.
- A groom bound to a scalp mesh (`xgmCreateSplineDescription` on a mesh) writes no UV primvar
  either. The `facevarying st` in its RIB is the mesh's own.

So GroomLab's shading network, which reads every color from per-strand `rman_*` primvars, can't
run as it is. A Pxr material assigned to the description does render in XPU
(PxrHairColor + PxrMarschnerHair: brown hair, 3.5 s at 480x360). But `PxrHairColor`'s
`randomMelanin` gives no strand-to-strand variation on these curves: the same scene rendered twice
differs more (mean 0.0021) than random melanin 0 vs 0.4 (mean 0.0014), since it needs a per-strand
index primvar XGen doesn't write.

Shape: RfM draws the points as a uniform cubic B-spline with a point mirrored past each end, so
GroomLab's points used as control points drew curls a third smaller. The writer solves for control
points whose curve passes through the points, and writes no width direction (RfM's `N`, which
made the curves ribbons). On a curly groom, XPU then matches RIS's curls (smooth where RIS's linear
curves are angular).

Root UVs can't reach the shader through XGen either (RfM writes no UV primvar for Interactive
Groom curves). Maya's Hydra viewport carries them, but needs an env change: hydra_xpu_viewport.md.

What lookdev in XPU can have through XGen:
- One material per description: melanin, redness, dye, specular, cones and roughness, set as
  constants (from HairColor), edited live in XPU.
- Variation only by splitting the groom into several descriptions (clusters of similar color),
  each with its own constants.

## Routes without a conversion: both closed

Checked 2026-11-02 against RenderMan for Maya 27.3 on Maya 2026.

**A translator of our own for `groomLabShape`, as V-Ray for Maya allows.** Not possible:

- The geometry translators are a fixed set compiled into `rfm.mll`: Archive, Camera, ClippingPlane,
  CoordSys, Curve, Dag, Fluid, Geo, Instancer, Light, LightFilter, Mesh, Nurbs, OpenVDB, Particles,
  Pfx, Procedural, RunProgram, USD, Volume, XGen and XGenGroom.
- No SDK, plugin path or registration call adds one.
- `config/mayaTranslation.json` looks like a node table, but it only maps shading nodes to Pxr
  patterns (`lambert` to `PxrMayaLambert`, `hairSystem` to `PxrMayaHair`).
- The only devkit shipped is `openvdb_maya`.
- A `nurbsCurve` renders as curves with `rman_renderCurve`, but that means one Maya node per strand.

**A USD stage in a `mayaUsdProxyShape`, then possibly a Hydra generative procedural.** Not
rendered:

- A proxy shape holding all 100,028 strands as one `BasisCurves` prim was left out of batch renders
  in both XPU and RIS. The log doesn't mention it, and `rfm.mll` links no USD library.
- The hdPrman plugins RenderMan for Maya ships (`hdPrmanMaya`, the "RenderMan XPU" Hydra renderers)
  are for Maya's Hydra viewport, not final renders.
- Writing the USD file from Python took 5 s.

So an Interactive Groom is the only XPU route found.

## What a full version would need

- **Where it hooks in:** a hidden `xgmSplineCache` and `xgmSplineDescription` per groom, created
  only when `rmanGlobals.renderVariant` is XPU, beside the RenderManProcedural that
  `sync_renderman` already manages. Hide the GroomLab procedural from XPU renders so the hair is
  not doubled when switching back to RIS.
- **Writer:** in C++, from the same strands the procedural renders: render overrides, density and
  width scale applied, groups of 20,000.
- **Shape:** XGen draws a B-spline near GroomLab's points, not through them. Check tight curls;
  refit control points if needed.
- **Color:** check whether RenderMan for Maya passes per-vertex or per-strand primvars from the
  description, so GroomLab's color and melanin reach PxrMarschnerHair. Otherwise only a flat
  material works.
- **Updates:** rewrite the cache on groom changes (the `outVrayPluginData` counter and the RfM
  dirty watches already say when), and refresh the cache node so interactive renders follow.
- **Animation:** a cache per frame, or one file with time samples; deformation blur needs at
  least two samples.
- **Material:** carry the groom's RenderMan shading group over to the description.

## Probe files

The spike scripts were left uncommitted in `build/probe/render_hooks/xgen_spike/` (the `build`
folder is not in git):

- `to_xgen_cache.py`: GroomLab Alembic to XGen layout (Maya's bundled `alembic` and `imath`
  Python modules).
- `import_render.py`: `xgmSplineCache -import`, count the splines, save a RenderMan XPU scene.
- `../dump_abc.py`: print an Alembic file's objects, properties and first values.
- `../probe_xgen_native_cache.py`: make a native groom, export its cache, import it back.

- `../usd_xpu/make_usd_scene.py` and `run.ps1`: the USD proxy shape test.

Run them with Maya 2026's `mayapy`. To load RenderMan for Maya, set `MAYA_MODULE_PATH` to the
shared Autodesk modules folder, and set `RMANTREE`, `RFMTREE` and `MAYA_RENDER_DESC_PATH`.
