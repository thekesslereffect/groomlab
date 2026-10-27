# Interpolation methods: choosing how strands follow guides

Ideas note, 2026-10-27. Nothing here is built yet. It started from Animal Logic's
[Smoothly Interpolating Sparse Curves on a Surface](https://animallogic.com/wp-content/uploads/2024/04/Sparse-Curve-Interpolation-1.pdf)
(Manuel Ruivo de Oliveira, UBC and Animal Logic, 2024) and a question: could Interpolate offer
several methods, improve its own default, and leave room for people to bring their own, without
breaking the nodes after it?

## Verdict

Yes, if the choice is made in the right place. Interpolate is two steps, **weights** and
**blend**, and the per-strand record between them (the binding) is the one interface the rest of
GroomLab sees. A new weighting method that writes the same records changes nothing anywhere else:
the GPU preview, incremental dabs, guide attributes, the influence channel and every node after
Interpolate keep working. A new blend is one more mode in `look.slang`'s InterpolateCS, which the
CPU engine and the GPU preview share.

The paper's full method is different in kind (strands rebuilt from fields on the scalp, with no
per-strand guide weights). It would be a node of its own, not a mode.

Recommended order:

1. **Direction blending** (a blend mode): the likeliest everyday gain. It stops strands shrinking
   where neighboring guides point different ways.
2. **Smooth surface weights** (a weighting mode): the paper's scalar idea as weights. It fixes
   sparse guides on concave or overlapping surfaces (the paper's snout).
3. **An extension point** for user weights, once two built-in methods have shaped the interface.
4. **Field interpolation** as its own node, only if 1 and 2 leave gaps.

The default stays as it is until a new method has proven itself on real grooms. Changing it
changes every saved groom's look: that needs a release note, or files saved before keeping the old
method.

## 1. What Interpolate does today

`engine/core/src/operators/interpolate.cpp`, with the blend in `engine/gpu/shaders/look.slang`
(InterpolateCS):

- **Weights (the binding).** For each strand, from its root on the rest scalp: up to `neighbors`
  guides (16 at most in a record), weighed by a modified Shepard (inverse distance to `power`, each
  guide's `reach` from the local guide spacing, a fringe band where reaches fade). Candidates come
  from a grid of guide roots; `followSurface` measures along the scalp (surface paths) instead of
  straight through it; `respectParts` keeps strands from guides across a part; `regionMap` keeps
  regions apart; `limitDistance` and `falloff` fade the guides' influence with distance. The
  binding is kept while the strands', the guides' roots, the parts and the params stay: a comb dab
  moves shapes, not roots, so it never binds again (since 0.33.0 an edited guide also keeps its
  exact root).
- **Blend.** Each guide's shape is resampled (smoothed with `smoothGuides`) into offsets from its
  root. A strand's shape is the weighted sum of its guides' offsets, turned from each guide's root
  normal to the strand's, placed at the strand's root. Then `take` (shape or length),
  `preserveLength`, `keepClearance` (stay out of the scalp), tubes (`fillTubes`), and the guides'
  attributes and per-point layers blended by the same weights.
- **Who reads the binding:** InterpolateCS (CPU engine and GPU preview); incremental dabs (the
  strands bound to a guide that changed are the ones made again); the influence channel; attribute
  and layer blending; `rebind` (operator_state.h) when guide roots move during a drag.

Nothing after Interpolate reads the binding. Clump, Curl, Noise and the rest see strands and
channels.

## 2. The paper

**The problem.** The common method (Houdini's, Animal Logic's Alfro) builds an interpolation mesh
from the guide roots and finds each strand's guides by 3D closest point. With sparse guides that
mesh is a poor stand-in for the surface. On concavities or self-overlaps it picks wrong guides:
their snout example has strands below the snout pulled by guides above it.

**The method.** It works on the scalp mesh itself.

- Each guide is cut into segments. Each segment vector is split into a part **along the surface**
  and a **height** off it (a length along the normal). The split loses nothing: the guide can be
  rebuilt from it.
- **Heights: a scalar field.** One field per segment index, solved over the mesh with the guides'
  values held at their root vertices. Harmonic interpolation (cotangent Laplacian) was too local:
  abrupt transitions, near-constant away from guides. **Biharmonic** (∆²u = 0) gave smooth fields,
  at the cost of slight overshoot past the prescribed values.
- **Along-surface parts: a tangent vector field.** Encoded as a discrete 1-form on the mesh's
  edges, following Fisher et al.'s
  [Design of tangent vector fields](https://doi.org/10.1145/1276377.1276447) (2007), asking for zero
  divergence and curl, with a weight (10³) on matching the guides. Singularities (whorls, partings)
  can be placed and typed by the user. Whitney 1-forms read the field inside a triangle.
- **Strands:** at each scattered root, segment by segment: the field's tangent vector plus the
  normal times the height, followed from the root.

**Its limits, as the authors give them:**

- The smoothness is **global**: every guide reaches everywhere. Parts, regions and grooming one area
  alone are future work (local edge coefficients, distance decay).
- Guides must sit on **mesh vertices**.
- The tangent solve is about O(E²) in edges. In practice the matrix is sparse and can be
  **pre-factored**, so iterative grooming stays fast.
- **Face normals** at scattered roots can make discontinuities at sharp features.

## 3. Options

### A. Weighting methods (a `weighting` param)

| Weighting | How | Good at | Cost |
| --- | --- | --- | --- |
| **shepard** (today) | inverse distance with reach, through the scalp or along it | dense guides, fast, local | none: it exists |
| **surface barycentric** | the guide roots triangulated over the scalp; a strand takes its triangle's three guides | the Houdini and Alfro look, predictable | the paper's sparse-guide and concavity problems |
| **smooth (biharmonic)** | one weight field per guide, solved over the scalp (1 at its root, 0 at the others); a strand takes its root's top weights | sparse guides, concavities, overlaps (distance runs over the surface, never across a gap) | a sparse solve per guide-root layout |

Notes for **smooth**:

- **Bounded biharmonic weights** (Jacobson et al. 2011,
  [Bounded biharmonic weights for real-time deformation](https://doi.org/10.1145/2010324.1964973))
  keep each weight between 0 and 1 and summing to 1. That removes the overshoot the paper notes.
  Plain biharmonic with clamping and renormalizing is the cheap first try.
- **Records:** the top `neighbors` weights at the strand's root (barycentric from the root
  triangle's vertices), renormalized, so the binding stays the size it is.
- **When it is solved:** once per scalp rest surface and guide-root layout, with the matrix
  factored once per scalp. Comb dabs never move roots, so they stay incremental. Adding or moving a
  guide solves again: one back-substitution per guide on the factored matrix.
- **Parts and regions:** cut the mesh along parts, and solve per region, so weights cannot cross
  them (the paper's open item, which our parts and region maps already define).
- **Guides off vertices:** spread each guide's constraint over its root triangle's three vertices
  by barycentrics. The paper's vertex-only limit goes away.
- **The scalp's resolution:** solve on the cage of a subdivided scalp (CageBinding), so the cost
  does not grow with subdivision.

Everything after the weights (blend, GPU preview, incremental dabs, attributes, influence) is
unchanged. `limitDistance` and `falloff` need a distance: for smooth, use the surface distance to
the nearest guide (the surface paths already measure it), or fade by the weight field itself.

### B. Blend methods (a `blend` param)

| Blend | How | Fixes |
| --- | --- | --- |
| **offsets** (today) | the weighted sum of the guides' offsets from their roots | (it is the reference) |
| **directions** | the weighted blend of each segment's direction in the root frame (normalized), the strand rebuilt segment by segment at the blended segment length; or the blend of each segment's turn from the one before | strands shrinking where guides point different ways (averaging positions pulls them in); tips flattening between curly guides |
| **tangent + height** | each segment split as in the paper: the along-surface part carried from the guide's root frame to the strand's over the surface (parallel transport), the height kept | strands keeping the surface's lie on curved areas (around an ear, down a neck) with sparse guides |

Notes:

- **directions** is cheap: the same records, one more mode in InterpolateCS. Lengths are kept by
  construction (the blended segment length), so `preserveLength` becomes a no-op for it.
- Guide attributes and per-point layers blend as now, by weight.
- Clearance, tubes and `take` length work on the blended shape as they do now.

### C. Field interpolation (the paper's whole method, a node of its own)

Strands rebuilt from a tangent field and height fields solved on the scalp, with no guide weights.

- **What it brings:** the smoothest result with very few guides, and placed singularities
  (whorls and partings as field features, not as guides).
- **Why not a mode of Interpolate:** there is no per-strand record, so guide attributes and
  layers, the influence channel, incremental dabs by guide, tubes and the GPU preview's
  InterpolateCS would all need re-deriving. As its own node (a **Field Interpolate**, guides in,
  strands out), everything after it still works.
- **Effort:** a sparse solver in the core (factor once, back-substitute per edit), 1-form
  construction, Whitney interpolation, and an editor for singularities. The largest of the three
  by far.

### D. Bring your own

Once A and B have two methods each, the binding is the natural interface:

- **Weights from a node:** Interpolate's `weighting` set to `input` reads a binding from an input
  port. Any node that produces one (a Script node, a future Slang module per the custom-nodes plan,
  a weight map per guide painted by hand) supplies the weights. The blend, GPU preview and
  incremental dabs come for free.
- **A blend from a module:** harder. It runs inside InterpolateCS on both CPU and GPU, so a user
  blend means user Slang compiled for both. Leave it until the custom-nodes plan has a story for
  user kernels.
- **The contract to write down first:** a record is up to 16 guide indices and weights summing to
  1, the guides' order as Interpolate keeps them, and a key (what the weights were made from) so
  incremental dabs know when they still hold.

## 4. Would anything break

- **Nodes after Interpolate:** no. They read strands and channels.
- **The GPU preview:** no for A (records as now). B needs its modes in InterpolateCS, written once
  in `look.slang` for both engines.
- **Incremental dabs:** no for A and B: comb dabs do not move roots, and the dirty strands are
  still those bound to the changed guides.
- **Saved grooms:** no, while the defaults stay `shepard` and `offsets`. A new default needs a
  release note, or old files keeping their method (a param saved with the node keeps it; only new
  nodes would take the new default).
- **Rebind during drags (operator_state.h `rebind`):** has to call the chosen method. For smooth
  weights, a root dragged across the scalp re-solves on the factored matrix (milliseconds for a
  few hundred guides; to measure).
- **Budgets:** smooth weights add a solve when guide roots change. The InterpolateHalfMillion
  budget's bind times would get a line for it.

## 5. What other tools and studios do (2026-10-27 survey)

No single method is "the best"; productions pair a weighting with a blend and artist controls.

| Who | Weights | Blend and controls |
| --- | --- | --- |
| XGen | each guide's area from boundaries between guides, weights normalized | a weighted average of the guides' points ("relative interpolation"); a Blend control from the nearest wire alone to smooth; the docs ask for dense guides and little curvature between them ([XGen guides interpolation](https://help.autodesk.com/cloudhelp/2017/ENU/Maya/files/GUID-49B8C299-71DD-4341-8638-97437D9A328F.htm)) |
| DreamWorks, Houdini | barycentric over a triangle of guides | DreamWorks lets artists change the weights afterwards against clumping and interpenetration ([Somasundaram 2015](https://history.siggraph.org/wp-content/uploads/2022/10/2015-Talks-Somasundaram_Dynamically-Controlling-Hair-Interpolation.pdf)); Houdini 18.5 added barycentric weights ([notes](https://www.sidefx.com/docs/houdini/news/18_5/groom.html)) |
| Pixar, Turning Red | kernel interpolation from nearby guides | coarse "style guides" artists sculpt, driving the thousands of guides ([Hair Emoting](https://graphics.pixar.com/library/HairEmoting)) |
| Animal Logic (2024) | smooth fields solved on the scalp mesh | section 2 |
| "Real-Time Physically Guided Hair Interpolation" (SIGGRAPH 2024) | the simulated guides' forces interpolated, not their positions | the render hair keeps its own physics: no collapse or interpenetration in simulation ([project](https://graphics.cs.utah.edu/research/projects/physically-guided-hair-interpolation/)) |
| "Groom Styles Interpolation with Features Preservation" (2022) | | blends between groom shapes keeping curvature and segment lengths: what blend directions starts, taken further |

GroomLab already has most of this: smooth reach-based weights with no popping, distances and
shapes carried along the skin, parts, regions, clearance, tubes, a guide hierarchy (Pixar's style
guides) and now the directions blend. The gaps: smooth surface weights for sparse guides, keeping
curl where guides disagree, and per-guide artist control of the weights.

## 6. A simpler, better Interpolate (the plan, 2026-10-27)

The node's main controls become `weights` (later), `smoothness` and `blend`; limit distance,
parts, regions, clearance and tubes stay.

1. **Directions the default for new nodes** (done on branch interp-simpler): Interpolate's
   version 2 (`ParamSpec::formerDefault`: nodes saved at 1 keep `offsets`, loaded or completed by
   an editor; Upgrade moves a param still at its former default to the new one). Found on the way:
   - guides cancelling out flipped directions' strands (neighbours' tips 2 apart): the segment now
     eases to nothing below half of the guides' step (guides about 120 degrees apart), so the flip
     happens at no length; directions' gradient there stays steeper than offsets' (0.6-0.9 against
     0.5-0.8 in the continuity tests), inherent to turning rather than shrinking;
   - the clearance walk asked every few points and let a point between two clear ones dip into the
     skin (the ear test: 431 points inside with directions): `clearanceEveryPoint`, on for version
     2 nodes, asks every point (the ear: none inside, and closer to the shaped path below it).
   Still to do before it ships: judge the presets' contact sheets with both blends (the GPU), then
   record the goldens, preset digests and display buffers again.
2. **`smoothness`** (done on the branch): one 0-1 slider scaling `reach` (2/3 to 2) and `power`
   (3 to 1/2) about 0.5, where they are as set; `neighbors`, `reach` and `power` advanced. No
   version needed: 0.5 changes nothing, and scripts and importers setting the three still work.
3. **A `curvature` blend:** each segment's turn from the one before blended, so curly guides
   with their curls out of phase do not cancel into straight hair (both offsets and directions
   flatten them today). A few days in the same kernel.
4. **A `weights` choice:** `smooth` (today's), `triangle` (barycentric over the guide roots, the
   Houdini and DreamWorks look) and `surface` (section 3 A's smooth weights).
5. **Per-guide influence:** a guide attribute or painted map scaling its weight (DreamWorks'
   artist control): cheap, and the fix studios reach for against stray blending.
6. **Later, for Simulate:** the 2024 force interpolation for simulated render hair.

## 7. Order and size

1. **directions blend**: a few days (InterpolateCS mode, params, tests: bits on CPU and GPU, a
   golden of two diverging guides showing no shrinkage).
2. **smooth weights**: one to two weeks (a sparse Cholesky in the core or a small vendored solver,
   cotangent Laplacian on the cage, per-guide fields, records, parts as cuts, regions, rebind).
   Judge on the wolf, a sparse-guide head and the paper's kind of case (a snout, ears).
3. **surface barycentric**: a few days, mostly for comparison and for artists used to Houdini.
4. **weights from an input**: a few days once the record contract is written down.
5. **Field Interpolate node**: weeks; only if the above leave real gaps.

## 8. Tests

- Each method at rest: guides alone (no strands between them) give back the guides' shapes
  exactly at their roots.
- Sparse guides on a concave mesh (a snout or a U shape): with smooth weights, no strand takes
  guides from across the gap; compare against shepard and barycentric in a contact sheet.
- Two guides pointing apart: directions keeps the strands between them at full length; offsets
  shows the known shrinkage (the test documents it).
- Incremental: comb dabs with each method match a fresh evaluation bit for bit (the incremental
  tests' pattern), and bind no more than today.
- GPU preview matches the CPU engine for every mode (the bits tests).

## Sources

- M. Ruivo de Oliveira, [Smoothly Interpolating Sparse Curves on a Surface](https://animallogic.com/wp-content/uploads/2024/04/Sparse-Curve-Interpolation-1.pdf), Animal Logic, 2024.
- M. Fisher, P. Schröder, M. Desbrun, H. Hoppe, [Design of tangent vector fields](https://doi.org/10.1145/1276377.1276447), ACM TOG 26(3), 2007.
- A. Jacobson, I. Baran, J. Popović, O. Sorkine, [Bounded biharmonic weights for real-time deformation](https://doi.org/10.1145/2010324.1964973), ACM TOG 30(4), 2011.
