# Art directing Asha's braids (Disney's "Wish")

Research note, 2026-11-17. Source: A. Kaur, J. Stratton, N. Mull, D. Hutchins, *Art Directing
Asha's Braids in Disney's "Wish"*, SIGGRAPH Talks 2024 (2 pages),
<https://cdn.disneyanimation.com/uploads/publications/WishAshaHair_sig2024_talk.pdf>,
doi:10.1145/3641233.3664338. Summarized here in our own words; read the talk for the details.

## What the talk describes

Asha wears about 80 long, thin box braids swept over one shoulder, plus cornrows on one side. Disney's
earlier films had one or two braids per character, so the volume needed new tools and workflows.

### Grooming: tubes, then Houdini braids, then tubes again

The groom is a hierarchy of tubes in Tonic, Disney's in-house tube groomer.

1. **Level 1:** regions give the overall structure of the groom.
2. **Level 2:** each region is broken out into braids, with one tube per braid sculpted for its
   length and flow.
3. The braid tubes' center curves go to Houdini, where a SOP network builds the braided strands:
   - **Strand width:** each strand's width is a percentage of the braid's width.
   - **Knots:** a knot frequency, varied with the width.
   - **Width along the braid:** a ramp.
   - **Per-group settings:** the network is copied once per group of braids with its own settings,
     then the groups are merged back.
   - **Noise:** global noise with a per-braid offset.
   - **Strand shape:** strand curves are offset sine waves, which makes the knots.
   - **Preview:** a separate network shows the result while the artist edits.
   - **Collisions:** Vellum fixes strand-to-strand collisions inside a braid. It runs while the
     braid's width is animated over a few frames, which squeezes the strands apart.
   - **Volume:** a circle is swept along each strand to give a mesh for its volume.
4. **Level 3:** those sweep meshes come back into Tonic as tubes, one per braid strand. Artists can
   then shape a whole braid or any single strand of it.

The result had over 500K tube vertices, two orders of magnitude more than an average Tonic groom.
Tonic needed optimizations to cope:
- drawing and selection became 60 times faster, reaching interactive rates;
- loading and exporting got faster.

Final hair is made in Disney's XGen as usual, from region maps and guide curves sampled from the
tubes, with procedural modules adding detail.

### Simulation rig

- **One curve per braid.** Each of the 80 braids is simulated as a single center curve with a
  circular collision width, not strand by strand.
- **Start poses.** The rig offers four draped start poses: the characteristic loop round the back
  onto the shoulder, through to braids hanging down the back. Shot artists blend between them for
  any in-between start pose, to match surrounding shots.

### Shots

- **2D drawovers first.** Animation drew the hair motion they wanted over Asha's body motion, and
  Technical Animation worked to those drawings.
- **Physics alone loses the shape.** As soon as Asha moved, simulated braids fell out of their
  designed drape. The look also depended on the camera: a loop from the back, less hair piled on
  the shoulder from the front.
- **Fixes, in order:**
  1. Shot-specific shapes dialled in on the rig.
  2. Several simulation passes that took hand-sculpted shapes as input.
  3. Hierarchical sculpting after the simulation.

### Stylization (not GroomLab's area)

- **Compositing:** modifiers and mattes for hand-drawn linework and watercolor.
- **Grooming changes for the line look:**
  - braids shaped for a graphic read;
  - fewer strays, which keeps lines clean;
  - broken-up mattes, so lines look gestural;
  - Kuwahara-softened silhouettes;
  - thinner lines at the hairline.
- **Lighting:**
  - simplified geometry for the braids' shadow shapes;
  - screen-space watercolor and paper textures.

## How it maps onto GroomLab

| Talk | GroomLab today | Gap / idea |
| --- | --- | --- |
| Region → braid → braid-strand tube hierarchy (3 levels) | Tube hierarchy levels: **Tube hierarchy > Add** splits tubes a level deeper, in every host since dev c19c16d1; root-first tubes | A level made *from a braid*: one tube per braid section, so a single woven strand can be sculpted on its own. We go the other way today (guides in, Braid builds strands). |
| Houdini SOP braid generator (width %, knot frequency varying with width, width ramp, per-group settings, noise with per-braid offset) | Braid node (dev 2234009): crossing engine (flat/three to five/fishtail/rope/cornrow), `width`, `tightness`, feed, ties, ends, loops; per-guide-group settings through separate Braid nodes or maps | Already covered: `widthProfile` is the width ramp along the braid, and a crossing's step is 0.6 braid widths / `tightness`, so thinner braids knot tighter on their own. Missing: a per-braid **random variation** of tightness or width (the talk varies knot frequency per braid). |
| Vellum pass to stop strands inside a braid crossing (animate width over a few frames) | Crossings are built so sections stay apart, with low-pass tracks | If tight braids still cross, a cheap analytic fix (push sections apart by section radius, not a sim) is closer to our approach than a Vellum-style relaxation. |
| Sweep a circle per braid strand → tubes | Cards node `braidCards` sections: a tube per braid section, winding with the weave (5a2f041) | Same idea already used for cards; the gap is reusing those section tubes as **editable** tubes. |
| 500K tube vertices; 60x faster drawing and selection | GPU tube wires and dots (0.46.0); root tubes in core | A box-braid head (80 braids × 3 sections × length) is a good **tube-count stress test** for the Tube tool's drawing and picking. Add it to the viewport bench. |
| Simulate one center curve per braid with a collision radius | Simulate on guides with `repulsionRadius`; Braid builds strands from the guides after the sim, so a guide per braid already is the center curve | Per-guide **collision radius** (a braid's width) instead of one repulsion radius for all: braids of different thickness. |
| Blend between four draped start poses | Simulate preload (gravity), weightless/off; Correctives; Sculpt on Simulation; DriveGuides | **Start-pose blending** isn't there: a set of sculpted rest poses on the Simulate node with a keyable blend, used as the start state. Fits the Yeti-style correctives users prefer. |
| Sims steered by hand-sculpted shapes; hierarchical post-sim sculpting | Sculpt on Simulation (shot sculpt layers), correctives on groomed guides, root-tube hierarchy | Shape targets as **sim goals** (a soft pull toward a sculpted pose, by weight), on top of post-sim sculpting. |
| Fewer strays for a clean graphic read | Flyaway node, strays by fraction | None: a stylized preset with strays off is enough. |

## Worth doing, in order

1. **Braid variation:** a random per-braid spread of `tightness` and `width` (seeded per guide). This
   is cheap; the width ramp (`widthProfile`) and knots following the width already exist.
2. **Simulate start poses:** sculpted rest poses on the Simulate node with a keyable blend, used as
   the start state. Then shape goals as a weighted pull. Together they cover the talk's sim and shot
   workflow.
3. **Per-guide collision radius** in Simulate, from a map or the braid width.
4. **Box-braid tube stress scene** for the viewport bench (Tube tool drawing and picking at
   about 500K tube vertices).
5. Later: a tube level made from braid sections, so single braid strands can be sculpted.
