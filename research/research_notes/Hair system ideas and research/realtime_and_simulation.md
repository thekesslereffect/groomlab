# Real-time hair (games and engines) and hair simulation, 2023-2026

Scope: shipped engine hair systems, hair cards, real-time and offline strand simulation, and interchange metadata. Each section ends with what a groom authoring tool (GroomLab) could do to serve them better. Research date: 2026-11-08. Older work is marked [foundational].

## 1. Strand hair in shipped games and engines: what do they need from authoring tools?

### Takeaway
Strand hair has shipped at AAA scale. Frostbite ran about 50k strands per character in Dragon Age: The Veilguard, Unreal ships its Groom system, and NVIDIA RTX Hair (LSS) shipped in Indiana Jones. All of them still need a **card fallback**, **guide/strand separation**, **per-strand width**, **group IDs** and **LOD/decimation-friendly data**. Unreal's direction for 2025-26 is Dataflow-driven card generation, USD import gated by a `GroomAPI` schema, and art-directable hair through joints and animation blending (MetaHuman 5.7). That is where an authoring tool can plug in.

### Cited Findings
**Unreal Engine Groom (5.x)**
- Groom LODs are set up in the Groom Asset Editor's LOD panel. Card and mesh geometry are managed in separate Cards and Meshes panels — [Epic: Setting Up Cards and Meshes](https://dev.epicgames.com/documentation/unreal-engine/setting-up-cards-and-meshes-for-grooms-in-unreal-engine)
- Unreal can project strand textures onto meshes to make card and "helmet" meshes. Helmets are the lowest geometric quality and are used at the most distant LODs — [Epic: Generating Groom Textures](https://dev.epicgames.com/documentation/en-us/unreal-engine/generating-groom-textures-in-unreal-engine)
- UE 5.5 release notes list hair items including "Improve noise on hair transmittance and shadow", a fix for a race condition in the hair cards builder, a fix for a crash when building large RT acceleration structures (many groom instances), a groom-cache fix (radius overriding position), and a hair voxel tracing fix — [UE 5.5 Release Notes](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-5-release-notes)
- Physics: simulation must be enabled on the groom asset (Groom Asset Editor) before a Groom component can override it. The XPBD solver has Solve Collision (inside the XPBD loop) and Project Collision (after the loop), plus friction and a collision radius used against Physics Assets. Doc versions also list Sub Steps, Iteration Count, a per-strand Radius Scale curve, Strand Viscosity (self-collision) and Grid Dimension. The component exposes Stretch Damping/Stiffness, Static Friction, a Local Bone for local-space simulation, and a Teleport Distance. Niagara solver selection allows a Custom Solver — [Epic: Enabling Physics Simulation on Grooms](https://dev.epicgames.com/documentation/en-us/unreal-engine/enabling-physics-simulation-on-grooms-in-unreal-engine). Note: the doc versions disagree on which panel holds which property.
- MetaHuman 5.7 (late 2025) made several hair changes:
  - A guide-driven workflow in the MetaHuman for Houdini plugin that builds hairstyles from pre-authored data, with strand interpolation and styling tools such as clumpiness.
  - A "MetaHuman Groom Advanced Kit for Houdini" on Fab, a learning resource for ponytails and braids.
  - An updated Groom plugin with a **joint-based workflow** for shaping and animating strands, art-directed motion through skeletal-mesh animation, and blending of keyframed animation with rigid-body simulation.
  - An **experimental** option to blend keyed animation with hair physics.
  - Grooms for MetaHuman can also be authored in Maya with XGen.
  - Source: [MetaHuman 5.7 release page (via search snippet; the page needs a login)](https://www.metahuman.com/releases/metahuman-5-7-is-now-available)
- Community reports (not official): users report flicker at strand/card LOD transitions and recommend locking LOD0 for close-ups — [yelzkizi MetaHuman hair fixes](https://yelzkizi.org/metahuman-hair-fixes-groom-lod-binding-wind-ue5/). A single forum report says Alembic groom strands are invisible on an M1 Mac in UE 5.6/5.7 — [Epic forum](https://forums.unrealengine.com/t/hair-strands-not-visible-in-ue-5-7-5-6/2679436)

**EA Frostbite strand hair (Dragon Age: The Veilguard, EA Sports FC, Madden)**
- [foundational] SIGGRAPH 2019 Advances course "Strand-based Hair Rendering in Frostbite" (Tafuri). Strands are simulated as points with constraints, using a mix of grid-based and point-based simulation — [slides](https://www.advances.realtimerendering.com/s2019/hair_presentation_final.pdf); blog series [Part 1](https://www.ea.com/frostbite/news/frostbite-hair-rendering-and-simulation) (shading, multiple scattering, antialiasing) and [Part 2](https://www.ea.com/frostbite/news/frostbite-hair-rendering-and-simulation-2)
- Veilguard (2024) asset facts:
  - About 50,000 strands per character across more than 100 hairstyles.
  - Maximum points per strand raised from **63 to 255** to allow waist-length hair.
  - A new system for **braids** and complex structures.
  - Simulation is decoupled from render tessellation.
  - **Card Hair** assets replace strands on low PC settings and Xbox Series S.
  - Source: [EA Tech: Strand Hair in Dragon Age: The Veilguard](https://www.ea.com/technology/news/strand-hair-dragon-age-the-veilguard)
- Veilguard simulation:
  - Runs as GPU compute, typically about 2 ms with spikes near 5 ms (asset loads, teleports).
  - Disabled for off-screen or distant hair, and for hair that doesn't affect visible shadows.
  - Rendering uses a **bespoke compute software rasterizer** that composites into the frame. Transparency is split at an alpha cutoff into opaque and transparent passes, with a transparent-hair depth texture and a stencil mask.
  - Source: [EA Tech](https://www.ea.com/technology/news/strand-hair-dragon-age-the-veilguard)
- Veilguard rendering and budgets:
  - Per-asset, per-important-light **hero shadow maps** with frustums auto-fitted to the hair bounds.
  - **Strand decimation for shadows**, so shadows render with fewer strands.
  - Console budget for 8 hair assets: 6.5 ms at 30 fps, 3 ms at 60 fps.
  - Hair does not go through DLSS/FSR/XeSS upscaling.
  - Memory: about 128 MB flat for 8 assets, plus 300-600 MB that scales with quality and resolution.
  - Source: [EA Tech](https://www.ea.com/technology/news/strand-hair-dragon-age-the-veilguard); also [Microsoft Game Dev article](https://developer.microsoft.com/en-us/games/articles/2024/12/bioware-dragon-age-the-veilguard-strand-hair-technology/)

**NVIDIA RTX Hair / Linear Swept Spheres (2025)**
- LSS is "a thick, round 3D line with varying radii". Chained segments share vertices. It is hardware-accelerated on RTX 50 (Blackwell), and OptiX falls back to software on older GPUs. Compared with DOTS (disjoint orthogonal triangle strips), LSS is about **2x faster and needs about 5x less geometry VRAM** for animated human hair. It averages about 1 vertex per segment, versus 2 or more for camera-facing quads, tubes or DOTS. A "successive implicit" index mode halves index memory. APIs: OptiX (default linear curve type), DirectX through NVAPI R570+, and Vulkan `VK_NV_ray_tracing_linear_swept_spheres`. LSS ships in the RTX Character Rendering SDK (part of RTX Kit) — [NVIDIA dev blog](https://developer.nvidia.com/blog/render-path-traced-hair-in-real-time-with-nvidia-geforce-rtx-50-series-gpus)
- First shipped in Indiana Jones and the Great Circle (2025 update), Blackwell only. Other GPUs keep the old hair path — [Wccftech](https://wccftech.com/rtx-hair-coming-september-indiana-jones-and-the-great-circle/); [Tom's Hardware](https://www.tomshardware.com/pc-components/gpus/nvidia-neural-rendering-deep-dive-full-details-on-dlss-4-reflex-2-mega-geometry-and-more). Release timing differs between outlets: earlier coverage said February, a later headline says September.
- GDC session "Manicured Characters: Path-Traced Hair and Skin in AAA Games" (NVIDIA) — [GDC schedule](https://schedule.gdconf.com/session/manicured-characters-path-traced-hair-and-skin-in-aaa-games-presented-by-nvidia/917525)

**AMD TressFX 5.0**
- TressFX 5.0 is an open-source (MIT) Unreal Engine patch for UE 4.26/4.27 and later UE 5.4. It has an improved simulation editor and a new SDF editor. Access is gated through Epic's GitHub — [GPUOpen TressFX5](https://gpuopen.com/learn/tressfx5/); [TressFX 5.0 for UE5](https://gpuopen.com/learn/tressfx-5-0-unreal-engine-5-update/). I found no 2025-2026 TressFX update.

**Unity**
- `com.unity.demoteam.hair` (from the Enemies demo) is open source on GitHub. It covers authoring, import, simulation and rendering. The solver handles tens of thousands of strands. It renders view-facing triangle strips, plus an HDRP compute line renderer that needs Unity 2023.1 or later. It depends on the digital-human package — [GitHub mirror/README](https://github.com/congobato/com.unity.demoteam.hair); [OpenUPM](https://openupm.com/packages/com.unity.demoteam.hair/). The docs are known to be thin — [third-party blog](https://scthe-github-io.vercel.app/blog/unity-hair)

### Inferences
- **Point-count caps matter.** Frostbite capped strands at 63, later 255 points. Unreal and LSS cost scales with segments. GroomLab's exporters could offer a "target engine profile": max points per strand, segment length resampling, curvature-adaptive resampling, and a strand budget per LOD.
- **Shadow decimation and sim decoupling** mean engines want a deterministic strand subset ordering. If strands are exported in a stable, randomized-but-stratified order (or with an `lod_rank` attribute), an engine can keep the first N% for shadows or LODs without visible clumping. GroomLab could compute and write that ordering.
- **Card fallback is mandatory**, even in strand-first engines (Veilguard on Series S). GroomLab's strand groom plus matching cards from one source is a key selling point. Keep root positions, color and silhouette consistent between the two.
- MetaHuman 5.7 moved toward **guide-driven Houdini authoring and joint-based hair animation**. GroomLab could export guides as a joint chain or skin-weight rig (guide -> bone chain), which serves joint-driven hair in Unreal and also the card rigs games use.
- LSS takes varying radii per vertex. Per-CV width (taper) export is therefore worth keeping exact, not quantized to a per-curve value.

### Gaps
- I could not read the UE 5.6 and 5.7 release notes directly (search didn't surface the groom sections). Changes to the Groom deformer, Groom binding and Niagara in 5.6/5.7 are unverified.
- I found no first-party talk on Frostbite's simulation algorithm after 2019. The Veilguard article gives budgets, not the solver.
- Meta's in-house real-time hair: the only items found were Meta Reality Labs neural-sim papers (section 3), no shipped-engine documentation.
- I found no reliable details on how Indiana Jones authors RTX Hair assets (source format, strand counts).

## 2. Hair cards: automatic generation, atlases, card LOD, transparency, hair-mesh LODs

### Takeaway
Card generation became an active research area in 2025. Strands2Cards and "Auto Hair Card Extraction" both use differentiable rendering and share textures across cards and LODs. Unreal's own generator is still Experimental, runs on Dataflow, and builds 3-card clumps. Hair meshes (Yuksel lab) are emerging as a third representation between strands and cards, with a 2026 Real-Time Live demo pitched as "game-ready hair in minutes".

### Cited Findings
- **Strands2Cards** (Tojo, Hu, Umetani, Hao Li; SIGGRAPH Asia 2025):
  - Clusters strands into wisps.
  - Builds a hairstyle-preserving texture per wisp, using skinning-based alignment into a normalized UV pose.
  - Shares textures between similar wisps.
  - Fits polygon strips with a differentiable renderer that optimizes semi-transparent, cluster-colored coverage masks.
  - Claims to beat prior methods on curly and wavy hair.
  - All 342 USC-HairSalon models render in real time (30 fps on an M3 Pro).
  - Code: [GitHub](https://github.com/kenji-tojo/strns2cards). Paper: [project page](https://kenji-tojo.github.io/publications/strns2cards/); [ACM DOI](https://dl.acm.org/doi/10.1145/3757377.3763864).
  - Short-hair textures and real-time simulation of the generated cards are listed as future work — [search summary of the paper](https://dl.acm.org/doi/10.1145/3757377.3763864)
- **Auto Hair Card Extraction for Smooth Hair with Differentiable Rendering** (Zheng, Huang, ..., Kui Wu; several authors from LightSpeed Studios; listed in the SIGGRAPH Asia 2025 program):
  - Each strand is encoded as a **projected 2D spline in texture space**.
  - Clusters strands into initial cards.
  - Two-stage optimization: per-card texture and geometry first, then joint fine-tuning after texture reduction.
  - Supports a **hair cap and cross-cards** for short and coily hair.
  - Gives **seamless LOD transitions through texture sharing**.
  - Says the UE5 auto card tool is not good enough for production in some cases.
  - Source: [arXiv 2505.18805](https://arxiv.org/abs/2505.18805)
- **Unreal Hair Card Generator**:
  - Experimental ("use caution when shipping").
  - A Dataflow graph: GetGroomAsset -> GroomAssetToCollection -> BuildCardSettings -> GenerateCardsClumps -> GenerateCardsGeometry -> GenerateCardsTextures -> CardAssetTerminal.
  - Per-LOD settings: Num Clumps (each a **3-card multi-card clump**), Num Flyaways (one card per flyaway strand), Num Triangles, Num Textures (shared texture islands), Texture Atlas Size, Adaptive Subdivision (by curvature) or Max Vertical Segments, Strand Width multiplier, Texture Compression along the strand direction, and an option to reduce cards from the previous LOD.
  - Card groups and groom groups must be **set up in the DCC and exported in the Alembic**.
  - Source: [Epic: Hair Card Generator](https://dev.epicgames.com/documentation/en-us/unreal-engine/hair-card-generator-for-grooms-in-unreal-engine)
- **Real-Time Hair Rendering with Hair Meshes** (SIGGRAPH 2024): hair meshes drive on-the-fly GPU strand generation, rendering hundreds of animated hair models in real time with on-the-fly hairstyle adjustment — [ACM](https://dl.acm.org/doi/10.1145/3641519.3657521); [SIGGRAPH history](https://history.siggraph.org/learning/real-time-hair-rendering-with-hair-meshes/)
- **"Game-Ready Hair in Minutes! Hair Mesh Modeling with Physics"** (Cem Yuksel), SIGGRAPH 2026 Real-Time Live. Hair meshes are described as orders of magnitude cheaper to render in real time than other strand representations — [ACM](https://dl.acm.org/doi/10.1145/3799821.3820913) (search snippet only; the page returned 403)
- **High-Performance Real-Time Implicit Strand-Based Hair Rendering via Software Rasterization** (Lipp, Jarabo, Wimmer, Bode; PACMCGIT 9(4), 2026). A deferred software-rasterization pipeline for strands from hair meshes, at 1 spp with filtering and reconstruction. Its LOD adapts both representation and shading by distance and screen coverage. It notes bottlenecks in mesh-shader implementations — [arXiv 2607.04230](https://arxiv.org/abs/2607.04230)
- **Real-Time Neural Hair G-Buffer Anti-Aliasing** (Wu, Shen, Huang, Yan, Montazeri, Kui Wu; SIGGRAPH Asia 2026). A neural spatial and temporal method reconstructs hair coverage and tangent from undersampled rasterization, then completes position for deferred hair shading. It claims better quality than DLSS/FSR on straight, wavy, afro and ponytail hair — [arXiv 2605.17557](https://arxiv.org/abs/2605.17557)
- Related 2026 preprints (titles only verified): "HairCS: Reconstructing Strand-Based Hair from Hair Cards" — [arXiv 2609.16465](https://arxiv.org/pdf/2609.16465); "HairDeck: Animation-Ready Stylized Hair Cards from a Single Image" — [arXiv 2610.40599](https://arxiv.org/html/2610.40599v1); "Inverse Rendering for Modeling with Line Primitives" — [arXiv 2609.00625](https://arxiv.org/pdf/2609.00625)
- Frostbite's transparency approach for strands (alpha-cutoff split into opaque and transparent passes, a transparent depth texture, stencil) — [EA Tech](https://www.ea.com/technology/news/strand-hair-dragon-age-the-veilguard)

### Inferences
- GroomLab's FiberShop-style card pipeline could adopt the ideas the 2025 papers share:
  - **Texture sharing across similar clumps and across LODs**, so LOD transitions don't pop, because LOD1 cards sample the same atlas islands as LOD0.
  - **Differentiable fitting of card shape and width against the strand render**: optimize card placement so card coverage matches the strand silhouette from several views.
  - **Cross-cards and a hair cap** for short and coily hair.
  - A side-by-side "strand vs card coverage error" view in the viewport, a feature no DCC tool advertises.
- Unreal's generator needs card groups and group IDs from the DCC. Even users who generate cards in Unreal benefit if GroomLab writes `groom_group_id`, group names and card-group tags well.
- The hair-mesh representation (Yuksel) is a candidate **new export target**: a coarse volumetric shell plus strand params. GroomLab already has guides and clumps and could emit a hair mesh as an LOD or a "procedural strand" export.
- Strand ordering for decimation, and a "card helmet" (lowest LOD) bake, close the LOD chain: strands, then decimated strands, then cards, then helmet mesh with a projected texture, all from one node graph.

### Gaps
- I found no 2023-2026 source specifically on alpha-to-coverage vs OIT for hair cards in shipped titles, beyond the Frostbite strand passes. This needs a dedicated search (e.g. GDC talks on card sorting and dithered TAA).
- I found no technical write-ups on commercial card tools (GS CurveTools, Haircard generators, FiberShop updates) in this pass.
- Strands2Cards atlas packing details and Auto-Card runtime numbers weren't on the abstract pages.

## 3. Real-time simulation: GPU strand sim, hair-hair interaction, body collision, sim LOD

### Takeaway
The field is splitting three ways:
- Faster physically based rod solvers: Stable Cosserat Rods (SIGGRAPH 2025, 1.4M vertices in 7 ms), the Augmented Mass-Spring model, and [foundational] ADMM DER on the GPU (2023).
- Neural real-time simulation from Meta Reality Labs: Quaffure for quasi-static drape, Neuralocks for dynamics.
- Production systems that simulate a guide subset and decimate: Unreal XPBD/Niagara and Frostbite.

### Cited Findings
- **Stable Cosserat Rods** (Hsu, Wang, Kui Wu, Yuksel; SIGGRAPH 2025). It greatly improves Cosserat rod stability, allowing larger time steps. It simulates every strand of an Afro hairstyle, **over 1.4 million vertices at 7.0 ms per frame**. The code repository is named "YarnBall" — [ACM](https://dl.acm.org/doi/10.1145/3721238.3730618); [kesen SIGGRAPH 2025 list](https://www.realtimerendering.com/kesen/sig2025.html)
- **Augmented Mass-Spring (AMS)** (Amador Herrera, Zhou, Sun, Shu, He, Pirk, Michels; arXiv Dec 2024, revised Oct 2025). Standard edge, bend and torsion springs plus a one-way biphasic coupling with a **ghost rest-shape configuration**, solved with a heptadiagonal decomposition. Claims stability, non-Hookean effects, and real-time **generation, interactivity and editing of simulation-ready dense hair** — [arXiv 2412.17144](https://arxiv.org/abs/2412.17144)
- **[foundational, 2023] Interactive Hair Simulation on the GPU using ADMM** (Daviet, NVIDIA; SIGGRAPH 2023). A local-global DER solver with Coulomb friction that handles thousands of contacting rods in real time. Demo: an interactive physics-based groom edit of about 86k curves, simulating 10% as guides — [NVIDIA research](https://research.nvidia.com/labs/prl/admm_hair/); [3dvf write-up](https://3dvf.com/en/nvidia-unveils-an-interactive-hair-simulation-technique-powered-by-ai/). Affiliation is NVIDIA, not Weta.
- **Quaffure** (Stuyck, Lin, Larionov, Chen, Bozic, Sarafianos, Roble; CVPR 2025). Self-supervised neural quasi-static drape that generalizes across pose, body shape and hairstyle. Inference takes a few ms, and 1,000 grooms drape in 0.3 s — [arXiv 2412.10061](https://arxiv.org/abs/2412.10061)
- **Neuralocks** (Lin, Larionov, Chen, Roble, Stuyck; Computer Graphics Forum). Compact, self-supervised neural networks for **dynamic** strand-level motion (bounce, sway) — [arXiv 2507.05191](https://arxiv.org/abs/2507.05191); [CGF/Wiley](https://onlinelibrary.wiley.com/doi/10.1111/cgf.70407)
- Unreal Groom physics: an XPBD loop with collision solve and projection, friction, a radius against Physics Assets, Strand Viscosity, and a grid for self-collision. Niagara solver selection allows custom solvers — [Epic docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/enabling-physics-simulation-on-grooms-in-unreal-engine)
- Frostbite: [foundational 2019] a mix of grid-based and point-based simulation — [SIGGRAPH 2019 slides](https://www.advances.realtimerendering.com/s2019/hair_presentation_final.pdf). In Veilguard (2024), GPU sim costs about 2 ms and is culled off-screen, at distance and when it doesn't affect shadows — [EA Tech](https://www.ea.com/technology/news/strand-hair-dragon-age-the-veilguard)
- Related rod papers (SIGGRAPH 2025, titles and authors only): "ANIME-Rod: Adjustable Nonlinear Isotropic Materials for Elastic Rods" (Chen, Wen, Barbic); "Fast Physics-Based Modeling of Knots and Ties using Templates" (Guo et al.); "Real-Time Knit Deformation and Rendering" (Huang et al.) — [kesen SIGGRAPH 2025](https://www.realtimerendering.com/kesen/sig2025.html)
- Hair-mesh physics: the Yuksel Real-Time Live 2026 includes "with Physics" in its title — [ACM](https://dl.acm.org/doi/10.1145/3799821.3820913)

### Inferences
- **Sim LOD** is now standard in production: simulate guides, deform strands, and cull by visibility or shadow relevance. GroomLab could export **simulation-ready guide sets** for engines: a chosen guide subset, per-guide stiffness and damping as attributes, collision radius per CV, and the guide-to-strand weights. Unreal accepts the weights as `groom_closest_guides` and `groom_guide_weights`. Unreal's guides then match GroomLab's preview instead of Unreal re-decimating them.
- Stable Cosserat Rods and AMS suggest GroomLab's PBD/rod solver could gain:
  - Larger stable time steps (fewer substeps, which matters in the Maya and Blender viewports).
  - A "ghost rest shape" coupling to keep curls and global shape under motion, which AMS emphasizes. This fits the existing gravity preload work.
  - A "sim-all-strands" preview mode for short and afro grooms, where guide interpolation looks worst.
- Neural quasi-static drape (Quaffure) points to a feature: **pose-dependent groom correction**. Given a body pose, predict drape or settle without a full sim, useful for avatar and character-creator pipelines. A non-neural alternative is to precompute a few settled poses and blend them (a "pose-space settle").
- Unreal's 5.7 joint-based animation and blend-with-physics suggest exporting a **bone-chain rig plus skin weights** from GroomLab guides, a cheap game-sim path that also drives card rigs.

### Gaps
- I couldn't confirm a 2024-2026 SIGGRAPH paper on hair-hair interaction approximations specifically. The Daviet 2023 ADMM work is the most recent strong reference found, and an ADMM note already exists in GroomLab memory.
- Frame times and strand counts for AMS and Neuralocks weren't on the abstract pages.
- I found no source on Unreal's Chaos-based or Niagara solver changes after 5.4.

## 4. Offline simulation advances 2023-2026 (DER, ADMM, contact-rich, wet hair, braids, Houdini, Weta)

### Takeaway
Offline groom pipelines are converging on **sparse guide sim + fast accurate guide deform**: Houdini 22 added Guide Reduce, Guide Deform and a render-time Hydra guide-deform procedural. Research rod solvers (Stable Cosserat, ADMM DER) are now fast enough to blur the line between offline and interactive. I found little new public material on wet hair, braid simulation, or Weta's Loki for hair in 2023-2026.

### Cited Findings
- Houdini 22 grooming:
  - **Guide Deform SOP**: faster and more accurate deformation of static grooms by simulated guides.
  - **Configure Guide Deform LOP**: a Hydra procedural that deforms grooms at **render time** in Karma.
  - **Guide Reduce SOP**: picks a sparse representative guide set by spatial capture up to a Target Guide Count.
  - **Guide Utility SOP**: snaps guides to the skin, makes a roots group, builds Vellum-compatible `orient`, and creates `curveu`.
  - **Shot Sculpt**: now sculpts guides and skin together.
  - **Hairgen "Blend in Skin Space"**: reorients curves relative to the skin normal at the root, so short fur avoids bald spots with sparser guides.
  - Source: [SideFX Houdini 22 What's New: Feathers/Grooming](https://www.sidefx.com/docs/houdini/news/22/feathers.html)
- Houdini USD grooms carry a **HoudiniHairDeformAPI** schema on BasisCurves, and guide deformation runs inside Karma at render time — [SideFX Configure Guide Deform LOP](https://www.sidefx.com/docs/houdini/nodes/lop/configureguidedeform.html)
- [foundational 2023] ADMM DER with Coulomb friction on the GPU — [NVIDIA](https://research.nvidia.com/labs/prl/admm_hair/)
- Stable Cosserat Rods (2025), see section 3 — [ACM](https://dl.acm.org/doi/10.1145/3721238.3730618)
- Braids in games: Frostbite added a dedicated braid system for Veilguard — [EA Tech](https://www.ea.com/technology/news/strand-hair-dragon-age-the-veilguard). MetaHuman ships a Houdini learning kit for ponytails and braids — [MetaHuman 5.7](https://www.metahuman.com/releases/metahuman-5-7-is-now-available)

### Inferences
- GroomLab's Guide Cache node (Houdini guide sims) lines up with Houdini 22's sparse-guide workflow. Two possible additions:
  - A **Guide Reduce**-style "pick N representative guides" node, so a dense groom gets a minimal sim set automatically.
  - **Skin-space blending** in interpolation, matching Hairgen's new mode, for short fur.
- A render-time guide deform (Houdini's Hydra procedural) suggests that **.groom + sim guides -> procedural expansion at render time** is the professional pattern. A USD procedural for .groom would fit the universal-format roadmap.

### Gaps
- I found no 2023-2026 primary sources on wet hair simulation, Weta's Loki or Synapse hair, or Pixar hair sim in this pass (time-boxed). These need a dedicated search of SIGGRAPH Talks and DigiPro.
- The full list of Houdini 22 Vellum hair solver changes wasn't covered on the grooming page.
- The second half of the SIGGRAPH 2025 and SIGGRAPH Asia 2025 kesen lists wasn't read (truncated), so some hair papers may be missing.

## 5. Interchange: metadata engines expect (IDs, root UVs, clump IDs, color, Alembic groom schema, USD BasisCurves)

### Takeaway
Unreal's Alembic groom schema (v1.5) is the de facto game interchange. Its key attributes are `groom_guide`, `groom_group_id`, `groom_root_uv`, `groom_id`, per-CV `groom_color`, `groom_width` (constant, per-curve or per-CV), and optional external interpolation `groom_closest_guides`/`groom_guide_weights` (int[3]/float[3]). For USD, Unreal needs an opt-in `GroomAPI` schema on BasisCurves (and `GroomBindingAPI` for binding). No vendor-neutral OpenUSD hair schema exists.

### Cited Findings
- Alembic groom schema v1.5 (`groom_version_major` 1, `groom_version_minor` 5):
  - `groom_tool` and `groom_properties` (strings on the top node).
  - `groom_guide` (int, constant or uniform; 1 = guide).
  - `groom_group_id` (int32, constant or uniform).
  - `groom_root_uv` (float2, uniform). If absent, a spherical mapping is auto-generated.
  - `groom_id` (int32 uniform, for debugging).
  - `groom_color` (float3 per vertex; falls back to black).
  - `groom_closest_guides` (int32[3] uniform) and `groom_guide_weights` (float32[3] uniform), for interpolation computed outside UE.
  - `groom_width` (constant, uniform or vertex; Maya curve width is converted; falls back to 1 cm; uniform width is "untested").
  - Names must be lowercase with a `groom_` prefix.
  - Source: [Epic: Using Alembic for Grooms](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-alembic-for-grooms-in-unreal-engine)
- The page I fetched marks most attributes as required. The search snippet calls `groom_closest_guides`/`groom_guide_weights` optional and niche. I treat the documentation table as ambiguous: [Epic doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-alembic-for-grooms-in-unreal-engine). Other attributes such as roughness, AO, clump ID, group name and cards ID are not on the page I fetched (see Gaps).
- USD import into Unreal (forum thread, Apr-Aug 2026; Epic staff reply 2026-08-18):
  - Grooms (static and baked animation) import from **BasisCurves prims with the custom `GroomAPI` schema** (`prepend apiSchemas = ["GroomAPI"]`).
  - **`GroomBindingAPI`** binds a groom to a skeletal mesh or geometry cache.
  - Groom Caches and LevelSequence groom tracks are supported.
  - Groom building is editor-only.
  - Epic staff: the schema is an opt-in so non-hair curves aren't imported as grooms, and `groom_*` attributes on the prim are read.
  - Source: [Epic forum](https://forums.unrealengine.com/t/how-does-the-usd-stage-need-to-be-structured-to-get-groom-assets-to-import/2794442)
- Unreal's Hair Card Generator needs card groups and groom groups authored in the DCC and carried in Alembic — [Epic](https://dev.epicgames.com/documentation/en-us/unreal-engine/hair-card-generator-for-grooms-in-unreal-engine)
- Houdini tags groom BasisCurves with `HoudiniHairDeformAPI` for render-time deformation — [SideFX](https://www.sidefx.com/docs/houdini/nodes/lop/configureguidedeform.html)
- LSS ray tracing wants **per-vertex radius** and linear segments, about 1 vertex per segment — [NVIDIA](https://developer.nvidia.com/blog/render-path-traced-hair-in-real-time-with-nvidia-geforce-rtx-50-series-gpus)

### Inferences
- Export checklist for GroomLab -> Unreal (Alembic and USD):
  - Always write `groom_version_*`, `groom_tool`, `groom_properties`, `groom_group_id` (one per GroomLab groom or part), a real `groom_root_uv` from the scalp UVs (don't let UE fall back to spherical), a stable `groom_id`, per-CV `groom_color` (baked from the look), and per-CV `groom_width` (taper).
  - Write guides with `groom_guide=1`. Optionally write `groom_closest_guides`/`groom_guide_weights`, so UE uses GroomLab's interpolation and the sim drives strands exactly as previewed.
  - For USD, apply `GroomAPI` and optionally `GroomBindingAPI`.
- **Stable IDs across edits** (`groom_id`) would let engines keep per-strand data (e.g. masks, LOD ranks) when a groom is re-exported, which makes iteration non-destructive.
- No neutral USD hair schema exists, so GroomLab could publish its own documented schema (a GroomLab API on BasisCurves carrying guide and clump relationships) alongside `GroomAPI`. This fits the "universal .groom" roadmap.

### Gaps
- I couldn't verify whether the current schema includes `groom_roughness`, `groom_ao`, `groom_clumpid`, `groom_group_name`, `groom_group_cards_id` or `groom_basis_type`/`groom_curve_type`. My training memory suggests some of these exist in newer UE versions, but the fetched page didn't show them, so they are unconfirmed.
- I found no documentation of the `GroomAPI` attribute list itself, and the forum thread gives none.
- Unit and up-axis conversion rules for Unreal groom import weren't on the page.
