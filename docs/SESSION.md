# GroomSession: one evaluated groom, for any host

GroomLab is an SDK first: a host (Maya, Houdini, Blender, a custom app, the CLI, Python) feeds a
`GroomSession` its inputs and asks it for views; the session owns everything that keeps a groom
evaluated (parsed inputs, rooted guides, bound parts and flow, the graph and its caches, the rest
pose, the cards, previews, versions, stats). Hosts only convert their own data and draw.
`Groom` is the same class (`using Groom = GroomSession`): exports, the Arnold procedural and the
tests keep their one-call `evaluate(density, seconds, buildCards)`.

Header: `engine/core/include/groomlab/groom_session.h`. Scalp maps: `engine/core/include/groomlab/scalp_maps.h`.

## API

**Inputs** (each setter no-ops on the same pointer, the same text or the same content hash, so a
host may call them on every evaluation; a change bumps what depends on it):

| Setter | Input | No-op when |
| --- | --- | --- |
| `setGraphJson(text)` | the graph (`{}`: default). Parsed once; the card, rachis, rest, scratch and stroke documents are clones; each adopts its old cache. Throws `GraphError` and keeps the old graph; `dropGraph(error)` empties it instead (Maya: the error shows on the node). | same text |
| `setHierarchyJson(text)` / `setHierarchy(ptr)` | guides as stored (not rooted). | same text / pointer |
| `setPartsJson`, `setFlowJson`, `setCorrectivesJson` | parts, flow arrows, shot sculpts. Parse errors are kept (`inputErrors()`), the input is then empty. | same text |
| `setCorrectiveWeights(map<slot, w>)` | live layer weights (Maya's keyable `correctiveWeight`): layers with a slot take them, not their envelope (`useLiveWeights`). | equal map |
| `setScalp(mesh)` / `updateScalpPoints(points, rest?)` | the scalp (raw, or already carrying its color sets); points only reuse the topology, UVs and colors (`withPoints`: the closest-point tree is refitted, answers as a fresh mesh's), `rest` replaces the rest pose (a rest mesh moving with its transform). The session refines it by `scalpSubdivision()` (the Scalp node's `subdivision`, `matchRender` the mesh's `renderSubdivision`; `subdivision.h`): `scalp()` is the refined mesh everything binds to, `cageScalp()` the mesh as given with its vertex maps (what they are painted on) and the mesh stored parts and flow are bound to, whatever the level: they are carried onto `scalp()` each to the same place of the surface (`PartSet::onRefined`, `FlowSet::onRefined`). | same pointer or content hash |
| `setScalpMapsJson(text)` / `setScalpMaps(maps)` | painted maps: vertex maps laid on the scalp as color sets (`withVertexMaps`), the map catalog. | same text / hash |
| `setGuides`, `setGuidePoses`, `setMeshes`/`setMesh`, `setControls`, `setScalpHistory`, `setPathResolver` | flat guides, posed guides by ID, colliders, Control values, Jiggle's past scalp, file resolution. | same pointer / hash / equal |
| `setCardMaps(text)` | the last card bake (`Atlas=`, `Strands=` stamps): rebuilt cards keep their baked cells; stats flag stale nodes. | same text |
| `setFollowScalp(on)` | follow the scalp (`follow_scalp.h`, `GroomData::followScalp`, groom files' `followScalp`): while the scalp is away from its rest pose the graph evaluates on the rest pose (stored guides, parts and flow bound there; flat guides, guide poses and a stroke's guides taken back through their roots; the scalp history turned into rest space) and each strand (instance directions, cards, rachis rings) is carried to the scalp now by its root's rigid frame. `following()` says whether the last view did. A host that carries the strands itself (`ViewRequest::hostCarries`: Maya's viewport, on the GPU) is handed them at rest instead, where carrying would move only their points and the instances placed on them (`carriesPointsAndInstances`: no cards or rachis rings): `ViewResult::follow` (`FollowCarry`: the rest pose and the scalp now) carries them, `placed()` (`FollowCarry::carried`, made once on demand) is carryStrands' result, and the bounds hold the carried strands (`carriedBounds`: each strand's box at rest, `strandBoxes`, turned and moved with its root; a little more than the points' extent, never less). | the flag only |
| `setScaleSource(source)`, `setScaleReference(area)`, `setScaleTransform(s)`, `setScaleTransformReference(s)` | scale the groom with its scalp (`GroomData::scaleSource`, `scaleReference`, `scaleTransformReference`; groom files' keys of those names). `ScaleSource::Transform`: k = the host's transform scale over its reference (a host without transforms: the area's k on the first measure, then kept); `Off`: k = 1; `Area`: `groomScale()` k = sqrt(area / reference), the area the scalp's surface on its rest pose (`restSurfaceArea`; the scalp as it is without one), the reference the one set or the first area measured (`scaleReference()`). Applied at evaluation, what is stored stays: the graph parsed from `scaleGraphJson` (params by their units, the presets' table: `unitScaleFactor`), keyed controls by `controlScaleFactors`, the guides by `GuideHierarchy::setScale` (shot sculpt offsets with them), `EvalContext::scale` anchoring the 1% lattices. k changes under 1e-4 are left out. A host whose rest pose does not take the scalp's scale sizes it (`withRestArea`: Maya, to the scalp's area at the start frame). Under `Area`, k changing with the area while the transform scale (handed in before the scalp) stands sets `scaleNotice()` / `scaleNoticeSerial()`, a line a host says once. | k changes: the graph again (caches adopted), the guides rooted again |

**Previews** (brush strokes; the tool mutates the object between evaluations):
`setPreviewHierarchy(h, final)`, `setPreviewOverlays(parts, flow)` (bound to `scalp()`, as the tools
edit them; `OverlayBinding` takes stored ones there and back), `setPreviewVertexMap(name,
values, channels)`, `setPreviewTextureMap(name, image)`, `setLiveStroke(on)` (returns true when
an evaluation took the stroke's density and must run again at the full one). `hasPreview()`.
While a preview is set and `sculptDensity` changes the density, views evaluate a document of their
own (a clone, at the stroke's density): the stroke's first dab leaves the view's cache alone, so
the release runs again only what the stroke changed, and the next stroke finds this one's results.
`warmStroke(request)` evaluates that document ahead with the last evaluation's inputs and no
preview (a host's idle time while its brush tool is up: Maya's `GroomShape::setBrushTarget`), so
a stroke's first dab makes again only what it changed (at 500k strands, a first press of ~100 ms
becomes ~20; after a release it takes the commit in, ~20 ms, and the next press is a few ms); its
evaluations are not counted. `releaseStroke()` drops the document (the brush put away; at 500k
strands and sculptDensity 0.35 it holds ~220 MB), `stats().strokeDocument` says whether one is kept.

**Strokes** (`engine/core/include/groomlab/stroke.h`): the interactive tools for any host. A host
implements `StrokeHost` (what a stroke reads: the scalp, the authored and shown guides, the stored
parts, flow, correctives and maps, the selection and hidden groups, shot sculpting, a texture
map's `TexturePaint`; where it previews; `commit(StrokeEdit)` and `warn`); `SessionStrokeHost`
is that host for a `GroomSession` alone (edits applied to it and kept for `undo`/`redo`).
`BrushTool` takes mouse events in a `ToolView`'s pixels (`view_projection.h`: a world-to-clip
matrix, or a host's own projection and rays): `press`/`drag`/`release` (Ctrl inverts; Shift: a
click picks a level, a drag smooths), `stroke` for a whole one, `pickLevel`, `affectedLines`
(hover). `scriptedStroke` runs one along world points. Each stroke commits one `StrokeEdit`:
the stored texts it set (`guideData`, `partData`, `flowData`, `correctiveData`, `scalpMaps`,
`shotLayer`; befores recorded by the host) and a texture map's changed tiles. The tools as
functions of data (`placeWithSymmetry`, `drawWithSymmetry`, `addPartWithSymmetry`,
`flowAlongStroke`, `flowSinksAt`) and the map dab (`MapDab`, `paintMapDab`, `paintMapStroke`:
vertex and texture maps, interactive and scripted) are public too. Picking is in
`guide_selection.h`: `nearestGuidePoint`, `pickGuides`, `combineSelection`, `mirrorSelection`.
Python: `ToolView`, `StrokeSettings`, `BrushTool`, `StrokeHost`, `scripted_stroke`,
`brush_stroke` (groomlab.brush_stroke's arguments), `pick_guides`, `combine_selection`.

```python
session = gl.GroomSession.from_groom_file("hero.groom")
host = gl.StrokeHost(session)
view = gl.ToolView.look_at((0, 1.4, 5), (0, 0.8, 0), fov=40, width=1000, height=800)
gl.BrushTool(mode="comb", screen_radius=150).stroke(host, view, [(400, 500), (480, 505), (560, 510)])
gl.brush_stroke(host, "smooth", [(0.2, 0.9, 0.3), (0.4, 0.8, 0.4)], radius=0.5, symmetry="x")
host.undo()
```

**Simulation** (`engine/core/include/groomlab/simulation.h`): a Simulate node's guides are what a
position-based rod solver made of them over the frames since its start frame. The node itself is
pure: it reads its frame from the external `sim:<node>`, which the session fills on every
evaluation; the session keeps each node's frames (`SimCache`: every frame from the start frame to
the last stepped, and the solver's state at the last) by node name. With water or rain its frames carry each
point's wetness as the guides' `wetness` point layer (`kWetnessChannel`), which frames between two
blend, bakes keep (per vertex in the Alembic guide cache) and Interpolate makes a point channel of;
`SimInputs::water` is the mesh on the node's `water` input (null: the plane at `waterLevel`).

| Call | Does |
| --- | --- |
| `setFramesPerSecond(fps)` | Frame f is at f / fps seconds (24 by default; another rate drops every frame). |
| `setSimulationFeed(feed)` | `feed(seconds)` sets the host's moving inputs at another time through the setters (the scalp's points, guide poses, colliders), as before showing a frame, and returns true: what the frames on the way to the one asked for read. The session puts its inputs back after. Without a feed, or when it returns false (it cannot read that time now: Maya while it plays), those frames are stepped in one go toward the inputs as they are (the roots moving evenly from the last frame's) and stamped unchecked, so a later view of one keeps them. GroomScene feeds its caches' frames, Maya the inMesh plug, the other scalps and the colliders at that time. |
| `evaluate(request)` | Steps each node in `cacheMode` "live" on to the request's frame when it is not cached (from its last frame; a fresh node from rest at its start frame), then shows it; a time between two frames blends them; before the start frame, while a brush stroke is live or bypassed, the guides pass through. At a whole frame it keeps, a node checks what it is fed against what that frame was stepped toward: a groom edit or another animation drops its frames. |
| `simulateTo(frame, progress)` | Simulate Range: every node (either `cacheMode`) stepped on to `frame`; `SimRun` (frames, cancelled, ms, errors). |
| `invalidateSimulation(node)` | Drops a node's frames ("": all). Settings changes do it by themselves (`SimSettings::hash`). |
| `simulationStatus()`, `simulateNodes()`, `simulatedFrame(node, f)`, `simulationStamp(node)` | Each node's cached ranges, bake, last step's ms and bytes; the nodes making the output (upstream first); a frame's guides (what a bake writes); the settings' stamp. |
| `setBakedSimulation(node, baked)` | A bake (`BakedSimulation`: a reader of guides by time, its span and stamp; engine/io `readSimulationCache` reads an Alembic guide cache, `writeSimulationCache` writes one): while its stamp is the node's settings', frames within its span come from it and nothing steps. |

The workflow every host documents: simulate (play, or Simulate Range), bake to a cache, render
(groom files name each node's bake, `simCaches`; GroomScene attaches them, so the CLI and the
Arnold procedural read baked frames). A groom with Simulate nodes `readsTime()`. Following the
scalp, the guides are carried to the scalp now for the solver and its frames back to the rest
pose for the graph. `GroomStats::simulations` (`stats().toJson()`'s "simulations") reports the
caches. Python: `GroomSession.frames_per_second`, `set_simulation_feed`, `simulate_to`,
`simulation_status`, `simulated_frame`, `invalidate_simulation`, `simulation_stamp`;
`write_simulation_cache`, `attach_simulation_cache`, `GroomScene.session`.

```python
scene = gl.GroomScene("hero.groom")            # fps, feed and bakes from the file
scene.set_time(1 / 24.0)
scene.session.simulate_to(120)
gl.write_simulation_cache(scene.session, "simulate", "hero_sim.abc", 1, 120)
```

**Views**: `const ViewResult& evaluate(const ViewRequest&)` never throws (errors are in the
result); `evaluate(density, seconds, buildCards)` returns the strands and rethrows a failure.

```cpp
struct ViewRequest {
    float density = 1, sculptDensity = 1;  // sculptDensity multiplies while hasPreview()
    double pointBudget = 0;                // EvalContext::maxPoints
    std::string viewNode;                  // a Strands node instead of the output (a Clump: its centers as dots too)
    double time = 0;                       // seconds
    bool buildCards = false;               // the evaluation builds cards (exports)
    bool wantCards = false;                // + the Cards nodes' cards at cardDensity, laid out
    float cardDensity = 1;
    bool wantBounds = true;                // off: bounds left for a later view (exports)
    bool hostCarries = false;              // following the scalp, the host carries the strands (ViewResult::follow)
};
struct ViewResult {
    StrandsPtr strands; std::string viewedNode; bool viewedRoots;
    FollowCarryPtr follow;                   // hostCarries: `strands` at rest, these carry them; placed() the strands where they are
    std::shared_ptr<const std::vector<Vec3f>> clumpCenters;  // a viewed Clump's centers' roots (DisplayInputs: dots)
    std::uint64_t strandsSerial, changesBase;  // which strands; and strandsChanges against those of changesBase
    ChangesPtr strandsChanges;                 // (StrandChanges: the strands that differ; null: not known)
    MeshPtr scalp;                           // with its vertex maps
    HierarchyPtr hierarchy, authoredHierarchy; PartsPtr parts; FlowPtr flow;
    std::shared_ptr<const CardMesh> cards; StrandsPtr cardStrands; std::vector<CardNodeSummary> cardNodes;
    Bounds bounds;                           // strands and active guides
    ViewVersions versions;                   // evaluation, strands, hierarchy, cards
    std::vector<std::string> errors;         // lastError lines
    std::vector<SessionMessage> messages;    // say once per change (Script Editor)
};
```

**Evaluating off the UI thread** (`engine/core/include/groomlab/async_evaluation.h`): a host whose UI
must not wait for an evaluation hands it to an `AsyncEvaluation` on the session: `start(request,
prepare)` evaluates on a worker thread (`prepare`, optional, runs there first: setters whose parsing
should not hold up the host), `ready()` says when `wait()` will not block, `wait()` hands the session
back with its result (`last()`) and rethrows, once, what escaped the worker. From `start` to `wait`
the session is the worker's: the host calls nothing on it. A second `start` before the first is taken
waits for it (its result dropped): evaluations chain in order, each reusing the one before's work.
`setThreads(n)` runs the worker's loops on at most n threads (`parallel.h` `runWithThreads`: a TBB
arena), leaving cores to the host's drawing. The host decides what its queries answer meanwhile.

A host that must go on with the session meanwhile (Maya: a brush stroke pressed while the last
stroke's commit evaluates) detaches the evaluation instead: `detach(request, after)` takes, on the
session's thread, what `evaluate(request)` would take now (the inputs bound, the node shown) and a copy
of the viewed document whose cached results it shares (`Graph::clone`), and returns a
`DetachedEvaluation` that owns all it reads; `run()` evaluates the copy on any thread while the session
stays the host's (it may evaluate meanwhile, on its own document as it was: a stroke's dabs). `after`, a
detached evaluation not adopted yet: the new one copies the document that one evaluates, once it ran,
so a chain of commits goes on each from the last. `adopt(run)` makes the evaluated copy the session's
document (false, nothing changed, when the session took another graph since or the run failed), and
the `evaluate(request)` that follows finds every result made: bit for bit what the session evaluating
in place makes (`DetachedEvaluation` tests, `Graph::setVerifyIncremental` on). Null from `detach` for
what only the session's own evaluation does: a live preview, Simulate nodes, cards wanted, a `stopAt`.
`DetachedRunner` (`async_evaluation.h`) runs them in order, each on a worker that first waits for the
one before: `start` never waits, `wait` waits for them all and hands back the last. The incremental
operators cannot take the arrays of results the session's document still holds (they copy them, from
`detail::ArrayPool` when it has them): the price of the session staying usable, a few percent. Maya
keeps the frame from before (a commit the GPU preview already draws), lets a new GPU stroke chain on it
and takes the commit in wherever the session's results are needed (docs/DEVELOPING.md, "Commits off
the UI thread").

`warmStroke` (the stroke document evaluated ahead) detaches the same way: `detachWarm(request)` copies
the stroke document with the inputs warmStroke would take now, its `run()` evaluates the copy on any
thread, and `adoptWarm(run)` makes it the stroke document unless that document was evaluated,
released or made again since (a stroke pressed meanwhile keeps its own). A warm is no commit:
`detach` never chains on one, `adopt` refuses it (the app runs warms on the commits' runner).

A live stroke's dab detaches too: `detachStroke(request)` binds the inputs evaluate(request) would
bind now with a copy of the preview guides (the tool goes on editing its own) and lends the stroke
document to the run (no copy: until `adoptStroke(run)` nothing may evaluate it, `strokeLent()`).
`adoptStroke` gives the document back (unless the graph was set again meanwhile) and makes the copy
the preview, so the `evaluate(request)` that follows evaluates no node and shows what a dab evaluated
here would show for those guides; the host then puts its tool's preview back
(`setPreviewHierarchy(live, previewFinal())`). Guide strokes only (null for map, part and flow
previews, Simulate nodes, cards, `stopAt`, a stroke at the view's density). The app runs one at a
time, the dabs between going as one (docs/DEVELOPING.md, "The app's frame budget").

Also: `evaluateNode(name, density, seconds)` (scratch clone), `evaluateFullDensity(seconds)` /
`releaseFullDensity()` (rachis), `posedHierarchy()`, `authoredHierarchy()` (stored, rooted: what
strokes edit), `storedHierarchy()`, `scalpMapView(name,
channel)`, `hairLook()`, `hasCardsNode()`, `scalpHistoryFrames(viewNode)`, `stats()` →
`GroomStats` with `toJson()` (what `groomLabGraphStats` prints), `nodeTimes()`, `nodeNotes()`,
`restEvaluations()`, `evaluationCount()`, `readsTime()` (the result depends on the time through what the session
knows: `graphReadsTime` (Wind, ImportCurves, Jiggle, Simulate, any node salting its key with the time), fading correctives, a
scalp history; a host adds a deforming scalp, guide caches, keyed controls: with none, other frames give the same
strands and the host may skip them).

## Display

`engine/core/include/groomlab/display.h`: what Maya's viewport draws, as flat buffers, for any host's
viewport (an OpenGL/Vulkan viewer, a Houdini or Blender drawer, Python). A `DisplayBuilder` takes
`DisplayInputs` (`DisplayInputs::fromView(viewResult, stroke)`, plus what only the host knows:
the guide selection, the instances' prototype meshes, the map shown on the scalp and whether the
host holds its image as a texture) and `DisplaySettings` (the display attributes: strand color
mode, range and source, ribbons, lighting, sculpt display, cards view, guides and their colors,
points, smoothing, hidden groups, flow, the `DisplayTheme`), and returns a `DisplayFrame`: one
`DisplayItem` per render item (strands, dimmed strands, colored strands, ribbons, instance
markers and meshes, guides, tubes, selected and soft-selected guides, parts, flow arrows and
field, cards, scalp map, braid tubes), the guide dots, the dots' size and the overlays' pull.

Show Tubes (a Braid node's button in every host's Graph Editor): `DisplaySettings::braidTubes`
names the Braid nodes whose section tubes draw, and `fillBraidTubes(settings, inputs, session)`
hands their mesh on before the build (`GroomSession::braidTubeMesh`: read off the last view's
results, kept while the nodes' params and inputs stay, so a camera move or an edit elsewhere
costs nothing). The builder draws it as the item `braidTubes`: a lit surface, each section in
its own color.

| Item field | Is |
| --- | --- |
| `geometry` | `DisplayGeometry`: positions (xyz), uint32 indices (lines or triangles), per-vertex RGBA colors (empty: the item's color), normals, tangents, uvs (`uvFloats` floats a vertex: 1, u root to tip, for lit strands and ribbons; 2 for cards, scales and the scalp map), bitangents (the cards' and scales' shading normal; lit strands carry none: a host's shader takes tangent x normal where it wants one); flags say how it shades: `lit()` (hair), `surface`, `cardTextures`, `overlay` (drawn over the hair, pulled toward the camera by `overlayPull`, stopped short of occluders by `overlayPullLimits`), `mapTexture`. |
| `color`, `lineWidth`, `layer` | The style, set on every update (restyle without rebuilding). |
| `rebuilt` / `geometryVersion` | Built this update (upload all of it; empty: remove the item). |
| `recolor` / `recolored` / `colorVersion` | Only the colored strand lines' colors changed (a new range or source): `StrandColors` writes the new per-point colors (upload the colors stream alone). |
| `geometry->lines` | With `setWriteLines(true)`, the strand lines (strands, dimmed and colored strands) come as a `StrandLines` instead of filled vectors: the host writes each stream straight into its own buffers (`writePositions`, `writeIndices`, `writeFrames`, `writeUvs`, `writeColors`: parallel, the very bytes the vectors would hold, `DisplayGeometry::materialized`), and keeps the indices and the one-float uvs while `layoutKey()` (the strands' point counts) stays, the two-float uvs (u plus twice the strand's number, `lineUvId`, and the width: what width as coverage draws, `lineCoverage`, hashed per strand, `lineHashThreshold`) while `widthsKey()` (the widths and the IDs) stays too: a tweak or a frame of playback rewrites the points and frames alone. |
| `updated` | Strand lines rebuilt holding the strands and point counts the host holds for the item (it uploaded them): only these strands differ (`DisplayInputs::strandsSerial`, `changesBase`, `strandsChanges`, from the `ViewResult`), so the host writes just their vertex ranges (`strandRuns`, `writePositions` / `writeFrames` over a range of strands) and keeps the rest; empty: the very strands it holds. Null: write it all. A brush dab rewrites the strands it changed, nothing else. |
| `keep` | Rebuilt empty but kept: the host hides the item and keeps its buffers. The lit strands step back this way for a stroke (and the dimmed ones after it), so the release writes again only the strands the stroke changed, and the next stroke's press only those that changed since the last one. A host that drops the item instead writes it whole when it comes back; one that lets a kept item's buffers go later (Maya: the dimmed strands when the brush is put away) says so with `forgetKept(id)`. |

Each part keys on what it draws (the strands' version and their settings, the hierarchy's
version and the guide settings and theme version, the selection hash, parts' and flow's content
hashes, the cards' version, the map's version or the host's texture generation), so a brush stamp
rebuilds the guides and then the strands, never the parts; `invalidate(part)` rebuilds one (a host
that lost its items). `setRetain(false)` keeps no geometry past the update that built it (Maya: the
buffers live on the GPU, written in place: `setWriteLines(true)`). Strand builders run in parallel into precomputed offsets; their output
equals a serial run. Also pure helpers: strand colors per mode, the lit color without a material
(`strandsLookColor`, the strands' mean; `strandLookColors`, each strand's own as the lit lines'
colors stream where they vary, keyed so a host keeps them while they stay), the hair highlight, the root-to-tip ramp, the card map combine, the scalp
map's pixels, group colors and hidden groups parsing. Maya's `GroomSubSceneOverride` reads its
plugs into the settings, uploads the items and keeps shaders, textures, occluders and prototypes;
Python's `DisplayBuilder.update(view, settings, ...)` gives the streams as buffer views.

## Moves to core

- `hosts/maya/src/scalp_maps.{h,cpp}` → `engine/core/.../scalp_maps.{h,cpp}` (parse, serialize, hash, catalog,
  `withVertexMaps`, `hashText`).
- From `groom_shape.cpp`: graph documents and adoption (`updateGraph`), guide parsing and rooting,
  poses + correctives (and `useLiveWeights`), parts/flow binding and previews, mapped scalp and
  map previews, the viewed node, the rest pose for the view, cards at cardDensity with the live
  budget (`kLiveCardBuildBudgetMs`), atlas lock (`refreshCardAtlas`, `parseCardMaps`, the bake
  stamp), versions, bounds, `graphStatsJson`, `hairLook`, `evaluateNode`, the rachis evaluation,
  the scalp map view's sampling.
- `Groom` (engine/core/src/groom.cpp) becomes `GroomSession`; `RestPose` and the free helpers stay in groom.h.

## Stays in the host (Maya)

Plug reading and dirty tracking (`textDirty_`, mesh/curve/collider fingerprints, control
attribute lists), Maya-to-core conversion (`toScalpMesh`, `toGuides`, `toGuidePoses`, colliders,
`sampleScalpHistory` over the core's `ScalpHistorySampler`, `withWeightCurves`), path resolution per node (handed over as a resolver),
reporting, drawing and GPU upload (the draw override reads the versions from the shape, which
copies them from the `ViewResult`), `MPx` glue (`compute`, `setDependentsDirty`, out meshes for
`outCards`/`outRachis`), bakes to files, the scalp map view's memo (Maya attributes, file writes),
selection weights. `NodeEvaluator` keeps one session per node and feeds it only what changed
(`NodeInputCache`: the raw scalp, the maps as text for the session to lay); the Arnold translator
keeps its evaluator registry.

## Rules

- **Ownership.** Inputs are shared immutable pointers (`MeshPtr`, `GuidesPtr`, ...); the session
  keeps them. Preview objects stay the host's; the host must not mutate one during `evaluate`.
  Results are shared pointers valid until replaced; hold them, never mutate them.
- **Threads.** A session is single-threaded: one caller at a time (Maya: the main thread,
  `kUntrusted`; or an `AsyncEvaluation`'s worker between its `start` and `wait`). A
  `DetachedEvaluation` runs on any thread beside it: it reads nothing of the session's. Evaluation
  runs parallel inside; parallel == serial.
- **Cache keys** follow DEVELOPING.md §4. The graph keys on externals' hashes: `scalp` by
  `surfaceHash` (color sets salt their readers), so painting re-runs only map readers. Rooted
  guides key on the parse and the raw scalp's `surfaceHash`; the active hierarchy on that, the
  preview (always new while set), poses' content hash, the correctives' version, live weights,
  the time (with correctives) and the scalp's surface. Parts and flow bind on their content hash
  and the scalp's `surfaceHash`. The mapped scalp keys on the raw scalp's `contentHash`, the maps'
  hash and the preview's version. Cards rebuild when the card document returns other strands, or
  the baked atlas file's stamp changed. Versions only move when a result pointer changed.
- No format migrations; output is identical to the host code it replaces.
