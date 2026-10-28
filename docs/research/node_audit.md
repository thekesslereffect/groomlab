# Node audit: params and math (2026-10-28)

## Contents
- [Scope and method](#scope-and-method)
- [Verdicts](#verdicts)
- [A. No look change: hide, fix wiring, defaults kept by version](#a-no-look-change-hide-fix-wiring-defaults-kept-by-version)
- [B. Math fixes](#b-math-fixes)
- [C. Removals and renames](#c-removals-and-renames)
- [D. Default changes](#d-default-changes)
- [Nodes with no change](#nodes-with-no-change)
- [Not covered yet](#not-covered-yet)

## Scope and method

**Nodes covered.** The 26 nodes grooms use, ordered by how many of the 38 shipped presets use them:
- Scatter, Interpolate, HairColor, Width, Resample, Clump, Noise, Length, Flow, Transform
- Curl, Bend, Blend, Cards, Flyaway, Collide, Cull, Direction, Smooth, Gravity
- Displace, Twist, Tangle, Wet, BlendNeighbors, Braid

**How.** Two read-only passes read each node's ParamSpecs and kernel (look.slang and the operators). They counted which params the presets set, including inside compounds, and grepped the docs for each param. The main claims were then checked against the code:
- Interpolate's `preserveLength` is skipped under `blend` directions (look.slang:1569).
- Bend's vertical-strand jump (look.slang:6383).
- Clump's lock target is a random spot in the disc, whatever side the strand starts on (look_engine.cpp:495; look.slang:2467).
- Clump's repel push is not monotonic (look.slang:2369).
- Tangle's `sameClump` reads a hard-coded `clumpId` (cinematic_ops.cpp:196).

**Preferences that frame the proposals:**
- Params as light as the looks allow.
- Saved scenes keep their look unless upgraded: new defaults go through node versions and `formerDefault`.
- No format migrations: a removed param simply drops from old files, with the owner's OK.

## Verdicts

**No node should go.** Each has a look the others can't make:
- Twist, Curl's clump coherence and Braid's rope style are three different ropes.
- Tangle makes pairs stuck over a span.
- Wet is a deliberate one-slider macro.
- BlendNeighbors smooths at strand scale.

The weakest nodes:
- **Direction:** no preset uses it, and Bend and Flow cover most of it.
- **Displace:** no preset uses it, and Bend keeps length where Displace stretches.

**Model nodes, the size to aim for:** Width (3 params) and Flow (3).

**Real complexity sits inside a few nodes:**
- Clump: 26 params, 9 that no preset sets.
- Cards: 35 params, all but one basic.
- Interpolate: compatibility toggles shown as basic.
- A few mis-named or duplicate controls.

## A. No look change: hide, fix wiring, defaults kept by version

Hidden params move under Advanced. Every item here leaves saved scenes and presets looking the same.

1. **Interpolate:** hide `followSurface` and `respectParts`. Their docs say "leave on" and no preset sets them.
2. **Interpolate:** show `preserveLength` only with `blend` offsets. Under directions, the default, it does nothing.
3. **Cards:** hide about 15 tuning params. Presets set 11 of 35.
   - Hide: `margin`, `minWidth`, `minStrands`, `segmentsByLength`, `minSegments`, `mirror`, `volumeNormals`, `thinnestStrand`, `fitTwist`, `braids`, `crossCount`, `separationIterations`, `featherSpine`, `featherFlatten`, `featherMinLength`.
4. **Clump:** hide `repelRadius` (it only acts with a negative strength), `centerWeight` and `lockRadiusRandom`. No preset sets them.
5. **HairColor:** hide `specularRandom`, `roughnessRandom` and `tilt`. Only the parity test sets them.
6. **Curl:** hide `phase`, `phaseRandom` and `weightDirection`.
   - With `phaseRandom` at its default of 1, `phase` only shows when animated.
7. **Braid:** hide `resolution`, `phase`, `sectionTwist`, `lobeFill` and `alongScalp`.
8. **Tangle:** replace `sameClump`, a bool reading a hard-coded `clumpId`, with a `clumpChannel` param, where empty means across clumps.
   - This is a bug: cinematic_hair renames the clump channel to `lockId`, and Tangle ignores it.
9. **Direction:** default `along` to `direction` for new nodes; saved nodes keep `tangent` through `formerDefault`.
   - `tangent` is the scalp's UV U, so every UV seam draws a line through the hair.
10. **Scatter:** never thin a scatter that makes fewer than about 5k strands in the preview.
    - 14 presets set `previewAll` by hand for exactly that.
    - Renders are unchanged.
11. **Yeti importer:** map `weightRandom` to Clump's `strandStrengthRandom` instead of folding it into `strengthRandom` (yeti.py:978). Document `strandStrengthRandom`.
12. **Interpolate `guideWeight`:** the audit suggests hiding it, because a number alone does nothing. Since it was put up front to be painted, the better fix is a tooltip and docs that say "paint it". Decide.

## B. Math fixes

These change looks only in the edge cases they fix. No shipped preset relies on any of them, except where noted.

1. **Clump lock (`lockRadius`): strands cross inside the lock.**
   - Each strand's target is a random angle in the disc, unrelated to where it sits around the axis. Wherever the pull is partial, strands from opposite sides swap and cross.
   - Fix: keep the strand's own angle from its root offset, in the center's frame, with the radius drawn as now so the disc fills evenly.
   - Affects dreads and ropes.
2. **Clump repel (negative strength): inner strands overtake outer ones.**
   - The push gives r'/R = 1 - x² + x³. A strand starting at 0.67R ends at 0.85R, inside one that started at the center. So strands cross at parts and aren't pushed out to `repelRadius` as the docs say.
   - Fix: a monotonic remap, such as [0, 2R] → [R, 2R] scaled by the amount.
3. **Clump flatness: the ribbon spins where the axis turns toward the thin direction.**
   - The thin direction is fixed at the root and projected onto each point's plane (look.slang:2375-2420). Fur standing up, or curly axes, flip it.
   - Fix: carry the root angle in the center's rotation-minimizing frames, which exist already, the way the random mode draws it.
4. **Clump copy (`copyAmount`): pops and swaps.**
   - It copies the plain mean of the k nearest centers, k = 1 by default. Two mutual nearest centers swap shapes at 1, and the nearest flips as centers move.
   - Fix: inverse-distance weights over 4 neighbours (BlendNeighbors' method), and remove `copyNeighbors`.
   - cinematic_hair (copyAmount 0.3) shifts slightly.
5. **Bend: a jump for strands standing exactly on the normal** (look.slang:6383).
   - A positive angle is capped at the strand's current lean, so a strand 0.001° off the normal barely bends. A strand exactly on it is forced into a full downward bend.
   - Guide-less Scatter grows strands exactly along the normal, so Scatter → Bend bends everything, but Scatter → Noise → Bend bends nothing.
   - Fix: a strand at the target does nothing at a positive angle. At a negative angle, the bend axis eases from the strand's lean to the root frame's axis inside a small cone.
6. **Displace: the profile is read by point index, not arc length** (look.slang:6524).
   - The result depends on point spacing. Curl adds points, and Resample's curvature mode spaces them unevenly.
   - Fix: read the profile by arc length, as Width and Gravity do.
7. **Blend (optional): a plain point lerp shortens strands** when mixing a curly look with a straight one.
   - Fix: lerp segment directions and lengths from the root.
   - Other tools lerp points too, so this is a choice, not a bug.

## C. Removals and renames

Saved scenes that set these params lose the value.

1. **Gravity:** remove `directionColorSet`.
   - `direction` already takes an expression, and `vmap("name")` reads a color set; the code normalises it anyway.
   - No preset or doc uses it. Three host special cases go too.
2. **Interpolate:** remove `smoothGuides` and `clearanceEveryPoint`.
   - Both are compatibility toggles.
   - Removing `clearanceEveryPoint` also changes version-1 nodes, which had it off. That is a fix, since fewer points dip into the skin, but it breaks the saved-scene rule.
3. **Interpolate:** remove `reach` and `power`, which `smoothness` already scales; keep `neighbors` advanced. Low confidence.
4. **Length:** remove `trimMin` and rename `trimMax` to `randomness`.
   - No preset sets `trimMin`. With `fraction` 1 it only duplicates lowering `keep`.
   - The random trim is the node's one spread, so by the naming rules it is `randomness`.
   - 5 presets are updated in the repo.
5. **Clump:** rename `flatten` to `flatAlong` and `flattenDirection` to `flatDirection`.
   - `flatness` (an amount) and `flatten` (an orientation) read as the same word.
6. **Bands:** rename `randomness` to `melaninRandom`, since Bands has three spreads; this matches HairColor.
7. **Scatter:** cap `randomness` at 1.
   - Above 1 it clumps roots past white noise, which is Clump's or a density map's job. No preset uses it.

## D. Default changes

New nodes only; saved nodes keep the old default through `formerDefault`.

1. **Clump `clumpBy` = size.**
   - 18 of 18 shipped presets pick `size`.
   - With `count`, any change of more than 1% in scalp area moves every clump.
   - The trade-off: `size` depends on scale.
2. **Flyaway `fraction` = 0.01.** All 3 presets use 0.01 to 0.012; the default is 0.03.
3. **Noise `octaves` = 1.** 9 of 27 uses set 1 explicitly, and none goes above 1. Check visually first.

## Nodes with no change

- **Width, Flow, Resample, Smooth, Cull, Collide:** lean and right.
  - Collide's `softness` and `smoothing` apply to the collider only; the tooltips could say so.
- **Transform:** in strand space, `rotate` follows the scalp's UV U, so it jumps across UV seams. Low priority. Reusing Direction's `along` would fix it.
- **Noise:** lean.
- **Twist, Wet, BlendNeighbors:** distinct and lean.

## Not covered yet

Simulate, Propagate, Plume, Scales, Instance and the field and utility nodes.
