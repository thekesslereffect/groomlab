# Skinned bust sample

A small test scene for hair on an animated character: a head and shoulders skinned to a joint
chain, with a GroomLab bob on a skinned scalp. Made for checking exports, renderers and live viewers
against hair that moves. `make_scene.py` builds it on a stylized bust made from primitives, or on a
bust you give it (`--head your_bust.obj`: cm, y up, facing +z, one closed mesh down to the
shoulders); the hairline, joints and hair lengths are measured on the mesh.

| File | What it is |
|---|---|
| `skinned_bust_hair.ma` | The groom follows the scalp (Follow Scalp on): roots ride the skinned head, no simulation. |
| `skinned_bust_sim.ma` | The same groom with a Simulate node: the hair swings and settles as the head moves. |
| `make_scene.py` | Builds both scenes (run it with `mayapy`). |

**The animation (156 frames, 24 fps, cm, y up), paced like a person:**

| Frames | Motion |
|---|---|
| 1-12 | Holds the bind pose, so a simulation starts at rest. |
| 12-132 | Moves forward 60 cm with a bob and a sway. |
| 12-96 | The head turns left, then right (30 degrees, a second each way). |
| 96-120 | A nod. |
| 120-140 | A head shake (about four swings a second). |
| 140-156 | Settles. |

The simulated scene's hair is held near the root, keeps its shape with a firm bend and settles by
air drag (natural hair, not a spring): `groomedUnderGravity` preload, bend 0.78, damping 0.35, drag 0.4,
inertia 0.75. The guides start clear of the head mesh, so nothing moves before the head does.

About 50,000 strands of 17 points each (Hair basics preset, preview density 1). The scalp mesh
(`scalp_geo`, cut from the head and given its skin weights) is hidden; the head mesh renders.

## Use

1. Install GroomLab 0.14.0 or later for Maya.
2. Open a scene. For the simulated one, play from frame 1 or use **GroomLab > Simulate > Simulate
   Range** (scrubbing back reads the frames already simulated).
3. Export, render or sync as you would any groom. For example, a USD file per frame:

   ```python
   from groomlab import exports
   exports.export_usd("hairShape", "C:/tmp/hair.usda", start=1, end=156)
   ```

## Build it again

```bash
mayapy samples/skinned_bust/make_scene.py --out build/samples/skinned_bust
mayapy samples/skinned_bust/make_scene.py --out build/samples/my_bust --head my_bust.obj
```

It also writes `preview_f001.usda` and `preview_f060.usda`, two single-frame exports for checking
that the hair moves with the head.

## License

The script, and the scenes it writes from primitives, are Apache-2.0 like the rest of GroomLab: no
third-party assets. A scene built with `--head` carries that mesh under its own terms.
