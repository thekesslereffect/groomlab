# Spec summary

The full PRD and spec are kept outside the repository. This file keeps what code changes depend on.

## Decisions

- Our own core instead of XGen or Bifrost. XGen guides are flat and its modifier stack is closed; Bifrost is weak at interactive guide authoring.
- One `groomLabShape` shape node holds the whole graph (Yeti-style), not one DG node per operator.
- Target: Maya 2022 (C++17, Python 3.7, PySide2, TBB 2020.2), the tested version; Maya 2023 to 2027 built from their devkits (PySide6 and oneTBB from 2025/2026), their tests being ported. Renderer: Arnold (also V-Ray and RenderMan exports).
- Output: USD `BasisCurves` and Alembic, plus an Arnold procedural later.
- Groom files (M4) are JSON (`.groom`): graph, hierarchy, flat guides and the scalp, with caches (animated scalp, guide poses) in Alembic beside them. USD was considered for the groom file itself; JSON keeps the core free of a USD dependency, and USD stays an output format.
- Host strategy (2026-09-25):
  - GroomLab stays a Maya plugin through M4.
  - A standalone GroomLab app with a live link is kept possible, not built.
  - To keep that cheap, all grooming logic stays in the core, and M4's groom file doubles as the
    live-link payload.
  - Revisit at the M4 gate with a small standalone editor prototype, or sooner if Maya becomes
    the bottleneck.
  - Guide selection (GT-5) moved out of M2; it returns before 1.0.
- Revisited at the M4 gate (2026-09-25): stay in Maya through 1.0. A standalone app or a Blender plugin may come later on the same core and groom file; neither is scheduled.

## Milestones

| Milestone | Scope | Gate |
| --- | --- | --- |
| M0 Foundations (done) | CMake, CI, core types, bindings, test harness | CI green on Windows and Linux |
| M1 Strands on screen (done) | `groomLabShape` evaluation, VP2 draw, Guides/Import Curves/Scatter/Interpolate/Length/Width/Output, Alembic export | 100k strands render in Arnold |
| M2 Guide hierarchy (done; GT-5 deferred) | Groups, tubes, frames, Generate Children, lock, Place/Comb/Cut brushes, undo | 3-level groom authored by hand |
| M3 Procedural look (done) | Clump levels, Noise, Curl, frizz (now Noise's `fraction`), maps, Qt graph editor, Maya Python API | Evaluation budgets met |
| M4 Pipeline (done) | USD export, Arnold procedural, CLI, motion blur, sim round-trip | Farm render from groom file |
| M5 Hardening, 1.0 (built; pilot pending) | Symmetry, clump paint, referencing, migrations, docs, pilot | All P0 done, pilot sign-off |
| M5+ Grooming depth (done) | Map node and paint in Maya, parting curves and Part node, guide attribute layers (Layers panel, density from layers), transfer to a new scalp | Tests and docs per feature |

## Core invariants

- Preview density keeps a strand when `hash(id) < density` (`groomlab::keepAtDensity`), so lowering density never moves survivors, and every strand a preview shows is the render's strand of that ID, bit for bit: nodes reading other strands read only the preview core (`preview_core.h`: the strands every preview holds, under a source's core share), which a preview below that share evaluates whole and hides (a strand channel the graph strips from what it hands on).
- A graph node's cache key hashes its type, params, the eval context (density, point budget, segments; never the time), the externals it reads and its input keys. Only changed nodes and their downstream nodes re-evaluate; a static groom is a cache hit on every frame. An operator that reads other context fields salts its key (`Operator::contextSalt`).
- `StrandSet` arrays are copy-on-write: a node's output shares every array it did not write with its input.
- Interpolate weights are modified Shepard: each guide reaches `reach` times the distance to its `neighbors`-th nearest guide, and its weight is zero at that edge, so the blend is continuous across the scalp.
- A hierarchy remembers the scalp topology it was bound on (`scalpTopology`); evaluated on a re-meshed scalp, bound guides keep their world shape and re-root instead of being rebuilt on the wrong face.
- Child guides store their shape as offsets in the parent's rotation-minimizing frame, scaled by the parent's tube radius (M2).
- With a rest pose on the scalp, Scatter counts strands from rest triangle areas, and Interpolate and Clump search neighbors at rest root positions. A deforming scalp then keeps the same strands and assignments on every frame (M4).
- Every host evaluates through `groomlab::Groom` (`GroomData` in, strands out), so Maya, `groomlab-cli` and the procedural agree: bit-exactly on one platform and build (the pipeline test compares `groomlab-cli hash` of a Maya export and a CLI export), and within a small tolerance across platforms or compilers, where `pow`, `sin` and the like may differ in the last bits. A groom file stores floats exactly (shortest round-trip form) for the same reason.
- Guide poses (e.g. simulated guides) replace hierarchy guides by ID for one evaluation; children re-follow their posed parents, and the authored shapes are untouched.
- Every stored document (graph, guide data, groom file) carries a schema version. Loading checks it (`schema.cpp`): a newer, older or malformed version is refused with a clear error; there are no migrations (see below). A format change that would not read back bumps the version and adds a `tests/data/compat/vN` fixture (M5).
- Stored paths are never required to be absolute: relative paths resolve through `EvalContext::paths` (Maya: project root, then scene folder; groom files: their folder, then `searchPaths`), and `$VAR` is expanded (M5).
- Operators only throw `GraphError`; the Maya node reports failures in `lastError` and never lets an exception reach Maya (M5).
- An operator's ports may depend on its params (a Compound's definition): `Graph::connect` grows the node's slots, and evaluation pads unconnected ones (M5).
- Things painted or drawn for the groom that must survive a scalp change (layers, parts) are bound to guides or to scalp points, never to UVs or vertex indices; `GuideHierarchy::transfer` and `PartSet::transferred` re-bind them by closest point (M5+).
- Guide IDs are 64-bit. Wherever Maya stores them (selection, sim curves) they are decimal strings (M5).
- Symmetry is across a world axis; topological symmetry (GT-4's other half) is after 1.0.
- Formats are changed in place (2026-09-27): no migrations for documents written by earlier builds; a version bump refuses them with a clear error.
