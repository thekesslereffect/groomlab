# Motion and simulation

Motion without a cache (Wind, Jiggle), simulating the guides, guides from Alembic and the simulation round trip.

User guide: [Motion & simulation](https://www.getgroomlab.com/docs/motion-and-simulation).

## Motion without a cache

Two nodes move the groom with no simulation and no cache: each frame is worked out on its own,
from the time and the scalp's last few frames. So a shot scrubs (any frame, in any order, looks
the same as when played to), renders on a farm frame by frame and caches the same every time.
Both are non-destructive: bypass or delete them and the groom is exactly what it was, and at
rest (no wind, a still scalp) they give their strands back to the bit. They keep roots and
segment lengths (segments turn, never stretch), and work on strands or guides. On feathers they
move the barbs alone: the shafts (`isShaft` 1) hold where Propagate laid them, as their rachis
tubes do (see [Feathers](feathers-braids-scales.md#feathers): to move whole feathers, put the
motion before Propagate).

| Operator | What it does | Main parameters |
| --- | --- | --- |
| `Wind` | Bends strands along a wind blowing toward `direction`. Its strength varies over space and time through a noise field that travels with the wind: `frequency` sets the gusts' size (features per scene unit), `speed` how fast they pass (scene units per second), `gust` how much the wind varies (and sways its direction). `turbulence` adds a fast flutter that grows toward the tips. Each segment turns toward the wind by strength x gust x `profile` (root to tip; stiff root, free tip by default). With `loop` the wind repeats every `loopEnd` - `loopStart` frames, seamlessly (see **Looping animation** below). A function of the strand, the time and the params: strength 0 is no wind at all | `direction`, `strength` (mappable), `gust` (mappable), `frequency`, `speed`, `turbulence` (mappable), `profile`, `loop`, `loopStart`, `loopEnd`, `seed` |
| `Jiggle` | Secondary motion from the scalp's motion: each strand hangs from its root's frame on the scalp by a damped spring, and where that frame was over the last `history` frames gives the spring's lag behind it now. Strands trail when the head moves, overshoot when it stops and settle; a turning head drags them too. The lag turns each strand about its root, by `profile` along it (tips lag more). With `loop`, the frames before `loopStart` are read from the end of the range, so a scalp animation that loops over it jiggles seamlessly. A scalp that did not move over the history changes nothing | `strength` (mappable), `frequency` (the swing's frequency, cycles per second), `damping` (0 swings on, 1 settles without overshoot), `history` (frames), `profile`, `loop`, `loopStart`, `loopEnd` |

- **Looping animation:** turn on `loop` and set `loopStart` and `loopEnd` (frames) to make a
  cycle (a game idle, a looping clip): the groom at `loopEnd` is the groom at `loopStart`, and
  the motion runs on across the seam without a jump or a kink. Wind repeats its time over the
  range (before and after it too): its gusts walk a closed path through a four-dimensional noise
  (the field travels `speed` x the loop's length along the wind each cycle, so the gusts repeat
  that far apart along it) and the flutter turns a whole number of cycles a loop. Jiggle reads a
  frame before `loopStart` that many frames before `loopEnd` (the host samples those frames of
  the scalp too), so give it a scalp animation that loops over the same range (its pose at
  `loopEnd` its pose at `loopStart`). Render `loopStart` to `loopEnd` - 1 and play them on
  repeat. While Maya plays the first time through, frames near `loopEnd` not seen yet read as the
  scalp now (still), as for any history frame; stopped on a frame, or from the second time
  round, they are read. `Simulate` has no loop (a simulation steps from its start frame).
- **Adding them:** Add Node > Motion in the Graph Editor, or `groomlab.add_wind(groom,
  direction=(1, 0, 0), strength=0.3, ...)` and `groomlab.add_jiggle(groom, strength=1.0, ...)`.
  The helpers put the node just before the output, before a Collide or Cards node that ends the
  chain (`groomlab.motion_anchor`): the look is finished when it moves, strands collide where they
  moved to, and cards, exports and renders all take the motion.
- **Previewing:** play the timeline. Only the motion nodes and what follows them re-evaluate from
  frame to frame (Wind's cache key holds the time, Jiggle's the scalp's history); a still groom
  with a bypassed or zero-strength Wind stays cached. Wind costs about 2 ms per 50k preview
  strands.
- **Where Jiggle's history comes from:** the graph reads the scalp's earlier frames as the
  external `scalpHistory` (its points at t - 1 frame ... t - `history` frames). In Maya the
  groom reads them only while a Jiggle node makes the output, by evaluating its `inMesh` at those
  times, and keeps them by time: playing or scrubbing forward reads no new frame (each earlier
  frame was the current one before), and a frame visited again is not read twice; stopped on a
  frame of a moving scalp (a script setting the time, an edit to the animation), every frame of
  the history is read again. Exports and the Arnold translator read the same frames, so renders
  and caches match the viewport. The CLI and the procedural sample a groom file's animated scalp
  cache (`scalpCache`); without one nothing moves, and Jiggle passes the strands through with a
  note (`groomlab-cli eval` prints it; the Graph Editor shows a node's note as its warning badge).
  `groomLabGraphStats` reports `scalpHistory` (`frames`, and `reads`: inMesh evaluations at other
  times so far) and each node's `notes`.
- **Longer memory:** the spring starts settled at the oldest frame of the history, so a swing
  older than `history` frames is forgotten. Give slow, loosely damped hair (a low `frequency`, a
  low `damping`) more history, about 1 / (damping x stiffness) seconds' worth.
- **Not yet:** Unreal card exports carry no wind weights; cards take the motion as geometry.

## Simulating the guides

`Simulate` (Guides, an optional collider Mesh -> Guides) is GroomLab's own hair simulation, in the
core, so Maya, the standalone app, groom files, the CLI and the Arnold procedural all run it the
same way. Between the guides and Interpolate, it moves the guides as a position-based rod solver
does over the frames since `startFrame` (from rest in the groomed shape): segments keep their
length (`stretchStiffness`), the groomed curves and twist pull back (`bend`, `twist`), points are held to
the groomed shape moving with the root by `rootStiffness` (mappable) times `rootStiffnessProfile` along the length, roots ride the scalp;
gravity, air `drag` toward a gusting wind (`windSpeed`, `windDirection`, `windGust`,
`windFrequency`), `damping` of the motion relative to the root, `inertia` (the share of the head's
motion the hair feels), collisions with the scalp and the collider (`collisionOffset`, `friction`)
and an optional guide-guide `repulsion`; `substeps` and `iterations` trade speed for accuracy.
The same inputs give the same frames, bit for bit, on any thread count.

- **Frames over time:** the host's session keeps every frame. Playing (or scrubbing past the last
  frame) steps on from the last frame (`cacheMode` `live`); scrubbing back reads the frames kept; a
  changed setting, or other guides or animation at a frame shown, simulates again from the start
  frame. `cached` steps only in Simulate Range.
- **Simulate, bake, render:** Simulate Range fills a range; Bake writes each Simulate node's
  frames as an Alembic guide cache (`strandId` the guide's ID) that the groom (Maya's `simCaches`,
  a groom file's `"simCaches"`) names with the stamp of the settings it was made with. Renders,
  exports and farms read the bake; a node whose settings changed since ignores it and simulates.
- **Maya:** GroomLab > Simulate > Add Simulate Node, Simulate Range, Bake Simulation, Clear
  Simulation; `groomlab.sim.add_simulate`, `simulate`, `bake_simulation`, `invalidate_simulation`;
  `groomLabSimulate`. The time is the scene's; the scalp is read at each frame on the way. nHair
  (Simulate Guides) is still there beside it.
- **The standalone app:** the timeline's Simulate, Bake and Clear, and a strip of the frames
  simulated and baked; `--simulate N`, `--bake-simulation N` from the command line.
- **Bypass** gives the groom exactly as it was.
- **Cost:** about 11 ms a frame for 1k guides of 20 points, 31 for 5k and 42-46 for 10k (4
  substeps, 6 iterations, scalp collisions; 24 threads): at 5k each substep takes about 8 ms,
  a fifth of a frame's time at 24 fps.
- **Memory:** every frame is kept (about 2 MB a frame at 5k guides of 20 points;
  `simulation_status` reports it): bake long shots and clear the frames.
- **Not yet:** guides inside a compound are not simulated (they pass through); colliders and guide
  curves at the frames on the way to a jump are read as they are now (the scalp is read at each
  frame).

## Guides from Alembic

```python
groomlab.import_guides_from_alembic(groom, "C:/cache/guides.abc", object_name=None, samples=24)
```

This swaps the graph's `guides` node for an `ImportCurves` node and roots the curves on the
groom's scalp, the same way connected curves are rooted:
- **Which curves:** every curve in the file, or only those under `object_name`. Parent
  transforms in the file are applied.
- **Animation:** animated files are read at the scene time. Between samples positions blend
  when the curves match; otherwise the nearest sample is used. To pose a hierarchy with
  simulated guides instead, see Simulation below.
- **Curve types:** linear curves come in exactly. Cubic curves, such as those from `AbcExport` of
  degree-3 Maya curves, are read as their control points.
- **Caching:** rewriting the file invalidates the cache, because the file's timestamp is part of
  the cache key.

XGen guides work through Maya curves: convert them with XGen's guides-to-curves tool and
connect them, or `AbcExport` them.

Guide curves connect to `guideCurves`. Whichever curve end is nearer the scalp
becomes the root. The scalp and guides are read in world space, including their
transforms. Keep the groom's own transform at identity.

## Simulation

Simulated guides pose the groom's hierarchy guides by ID. Children re-follow their posed parents,
so simulating the primary guides moves the whole groom. The authored guides (`guideData`) never
change; remove the simulation and the groom is as groomed.

In Maya, with nHair or any curve deformer:

```python
from groomlab import sim

made = sim.create_sim_curves(groom, "primary")   # a curve per guide, made dynamic (nHair), connected
# ... play the scene from the start frame ...
sim.remove_sim(groom, made)                      # back to the authored groom, nodes deleted

made = sim.create_sim_curves(groom, "primary", preset="long", collide=True, preroll=20,
                             wind=((1, 0, 0), 3.0, 0.2))
sim.bake_sim(groom, "shot010.guides.abc", made)  # cache the playback range, drop the live sim
```

- **Presets:** `fur`, `short`, `long`, `bouncy` and `wet` set the hair system's stiffness,
  damping, drag, mass and start-curve attraction, with attraction falling along the length
  (stiff roots, free tips). `apply_sim_preset(hair_system, name, **overrides)` and
  `configure_sim(sim, ...)` change them later.
- **Collision:** `collide=True` makes the scalp a passive collider (nRigid); `self_collide`
  turns on hair-to-hair collision.
- **Pre-roll:** the nucleus starts `preroll` frames before the playback range, so the hair has
  settled by the first frame. **Wind:** direction, speed and noise on the nucleus.
- **Baking:** `bake_sim` plays the sim from its start (pre-roll included), writes the posed
  guides over the playback range to Alembic, reads the groom from that cache and deletes the
  live sim: playback becomes fast and scrubbable, and renders and groom files use the cache.
  Shot sculpts stay on top of it (they are not baked into the cache).

- **Curves:** `create_sim_curves` builds the curves from the guides at the current frame.
- **nHair:** `makeCurvesDynamic` attaches the follicles to the scalp. The simulated output
  curves connect to the groom's `simGuides`.
- **Other deformers:** `dynamic=False` connects the plain curves for any other deformer.
- **Custom curves:** `connect_sim_curves(groom, {guide_id: curve})` connects curves you already
  have.

With another package (Houdini, ...):

```python
sim.export_guides(groom, "guides.abc", "primary", start=1, end=100)  # strandId = guide ID
# ... simulate, keeping the strandId attribute ...
sim.set_guide_cache(groom, "sim.abc")
```

- **Guide caches:** a cache blends between samples, so motion-blur subframes are smooth.
- **Groom files:** `export_groom` writes the poses to `<name>.guides.abc` and references it, so
  `groomlab-cli` and the Arnold procedural render the simulated groom too. The pipeline test
  runs an nHair simulation and checks that the CLI matches Maya on the simulated frame.
- **Menu:** Simulate Guides (nHair, with a group and a preset), Bake Simulation to Cache, Remove
  Simulation, Export Guides for Simulation and Load Guide Cache are under GroomLab > Simulate > Legacy.
- **Commands:** `groomLabExportAlembic` takes `-guideGroup <name>` (posed guides of a group) or
  `-guidePoses` (the poses alone). `groomLabGuides -guidePoints <id>` returns a guide's points.

Every saved groom writes all of its params and each node's version, so later changes of defaults
or of an operator's behaviour never change it (an outdated node offers an Upgrade); see
[DEVELOPING.md](../DEVELOPING.md), "Param specs, versions and saved grooms".
