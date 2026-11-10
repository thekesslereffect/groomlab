# Hair grooming tool landscape 2024-2026 (commercial and free)

Scope note: research done 2026-11-08 with about 18 web searches/fetches. Vendor pages for Yeti, Ornatrix, Houdini 20.5 and SideFX docs were reachable. MetaHuman release notes sit behind an Epic login, Blender 5.0 official notes and Houdini 21 hair what's-new pages were not retrieved, and forum/Reddit complaint threads did not surface in search. Those holes are listed under Gaps per question.

## Per-tool recent releases and headline features (Yeti, Ornatrix, XGen, Houdini, Blender, Unreal/MetaHuman, cards tools, others)

### Takeaway
The two commercial leaders shipped incremental but pointed releases: Yeti 5.x focused on viewport shading fidelity, XGen-IGS import, intersection resolve (Plume/Resolve) and an evaluation SDK; Ornatrix Maya 5 focused on direct manipulation (clump manipulators, curved comb sinks, texture paint on hair), operator-stack branching and strand-to-mesh/texture. XGen and Unreal saw little grooming investment; Epic instead pushed grooming into Houdini (MetaHuman Groom Tools, a slider-driven Parametric Hairstyle Generator) and a joint-based strand shaping workflow in the UE Groom plugin. Houdini added APEX-rigged "master hair" controls and guide utilities.

### Cited Findings

**Yeti (Peregrine Labs)**
- Yeti 5.0 (announced 5 March 2024): new Maya Viewport 2.0 hair shader that "aims to match commonly used production hair shading techniques", with melanin and IOR controls; XGen Interactive Grooms can be imported into the Yeti graph; all Yeti Maya nodes support soft selection and weighted transforms; new Groom Sampler node lets any node parameter be driven by a groom's paintable attribute (and length) — [CG Channel](https://www.cgchannel.com/2025/03/peregrine-labs-ships-yeti-5/); [Peregrine Labs: What's new in Yeti 5.0](https://support.peregrinelabs.com/support/solutions/articles/66000522678-what-s-new-in-yeti-5-0); [Yeti 5.0 blog](https://peregrinelabs.com/blogs/news/yeti-5)
- Yeti 5.0.x patch dates are in 2024 (e.g. 5.0.5 dated 24/06/24) — [Yeti 5.0.x release notes](https://docs.peregrinelabs.com/release-notes/5-0-x)
- Yeti 5.1 was mainly bugfixes and host/renderer version support — [CG Channel](https://www.cgchannel.com/2025/03/peregrine-labs-ships-yeti-5/)
- Yeti 5.2 (24 March 2025): Plume node resolves feather-feather and feather-geometry intersections; Resolve node resolves fiber intersections; cross-platform file path mapping (Arnold-like convention); new C++ "Yeti Evaluation SDK" for evaluating Yeti graphs outside Maya — [CG Channel](https://www.cgchannel.com/2025/03/peregrine-labs-ships-yeti-5/); [Yeti 5.2.x release notes](https://docs.peregrinelabs.com/release-notes/5-2-x)
- 5.2.x patches: V-Ray 7.0 support; `alembicSubdivision` argument to pgYetiCommand to toggle subdivision on Alembic export; 5.2.6 dated 04/12/25 — [Yeti 5.2.x release notes](https://docs.peregrinelabs.com/release-notes/5-2-x)
- Yeti is Windows and Linux only; macOS dropped since Yeti 4.2. Requires Maya 2024+; renderers Arnold, Redshift, RenderMan, Unreal Engine, V-Ray — [CG Channel](https://www.cgchannel.com/2025/03/peregrine-labs-ships-yeti-5/)
- No public information found on a Yeti 6 or a published roadmap — [CG Channel](https://www.cgchannel.com/2025/03/peregrine-labs-ships-yeti-5/) (article silent on it)

**Ornatrix (Ephere)**
- Ornatrix Maya 5: Ephere forum announcement 30 Aug 2025, CG Channel says officially announced 2 Sept 2025 after shipping earlier in 2025; MotionMedia gives 28 April 2025 premiere (sources disagree on date) — [CG Channel](https://www.cgchannel.com/2025/09/ephere-releases-ornatrix-maya-5/); [Ephere forum](https://ephere.com/plugins/autodesk/maya/ornatrix/forums/news/thread_17988.html); [MotionMedia](https://www.motionmedia.com/mm-blog/ornatrix-maya-v5-the-ultimate-grooming-upgrade-is-here/)
- Ornatrix Maya 5 features: Warp Roots operator (bunch/spread root positions by map or painting; aimed at feathers and sparse fur); Surface Comb "curved sinks" drawn on the surface to direct/repel/attract hair; Length operator can extend strands along curves; Clump operator manipulators to move/scale/rotate clumps at any point along their length; procedural braids generated at any point on a clump; Mesh from Strands meshes hair groups into flat or tubular clumps and procedurally generates textures from the source hair; texture paint brush paints textures/decals onto hair in the viewport; Operator Stack branching (multiple shapes/base objects share one groom history); Merge operator blending of sub-grooms and overlap resolution; SIMD speed work — [CG Channel](https://www.cgchannel.com/2025/09/ephere-releases-ornatrix-maya-5/); [Ephere forum](https://ephere.com/plugins/autodesk/maya/ornatrix/forums/news/thread_17988.html); [80.lv beta preview of clump manipulators](https://80.lv/articles/ornatrix-maya-v5-will-get-new-intuitive-clump-manipulators)
- Warp Roots also came to the 3ds Max plugin (2 Sept 2025 update) — [CG Channel tag page](https://www.cgchannel.com/tag/ornatrix-3ds-max/)
- Ornatrix 3 for Cinema 4D (15 Nov 2024, C4D R25+); Ornatrix 4 for C4D (12 Aug 2026) brings Max/Maya feature set to C4D plus a Create Groom dialog — [CG Channel 2024](https://www.cgchannel.com/2024/11/ephere-releases-ornatrix-3-for-cinema-4d); [CG Channel 2026](https://www.cgchannel.com/2026/08/ephere-releases-ornatrix-4-for-cinema-4d/); [Digital Production](https://digitalproduction.com/2026/08/19/ornatrix-4-expands-c4d-grooming/)
- Ornatrix for Unreal: Alembic-based link; per-strand groom colour written to `groom_color`; supports UE 5.x and 4.27 — [Toolfarm](https://www.toolfarm.com/buy/ephere_ornatrix_for_unreal_engine/)
- Ornatrix made free to schools and students worldwide (28 Sept 2025) — [CG Channel](https://www.cgchannel.com/tag/free-license)
- Ornatrix Maya 5 runs on Maya 2022+ Windows/Linux; Maya 2024+ on macOS 13+ with partial feature support — [CG Channel](https://www.cgchannel.com/2025/09/ephere-releases-ornatrix-maya-5/)

**XGen (Autodesk Maya)**
- Maya 2027 released (March 2026); headline features are modelling (Smart Bevel), skinning, shot management, MotionMaker — no XGen items among highlights — [CG Channel](https://www.cgchannel.com/2026/03/autodesk-releases-maya-2027-and-maya-creative-2027/); [CGI Academy Hub](https://cgiacademyhub.com/blog/novedades-maya-2027)
- Third-party scripts keep filling XGen gaps: "Groomer's Tool" added a braid tool and Maya 2027 support (V15.04.01), plus IG-guides-to-curves and snapping XGen guides to curves — [Groomer's Tool updates](https://nobraintools.gumroad.com/p/groomer-s-tool-updates-v-13)
- Autodesk support article on XGen Interactive Grooming brushes not updating in the viewport, workaround = roll back graphics driver — [Autodesk support](https://www.autodesk.com/support/technical/article/caas/sfdcarticles/sfdcarticles/XGen-Interactive-Grooming-Brushes-are-not-visible-in-Maya-Viewport.html)
- Opinion: one artist's 2021 blog says XGen has had no official development since 2016 and claims (citing groomers) XGen is not used in production due to animation problems — [charlytuts blog](https://charlytuts.blogspot.com/2021/02/ornatrix-advantages-or-why-i-ditched.html) (single, old, opinion source)

**Houdini (SideFX)**
- Houdini 20.5 hair/fur/feathers: APEX Add Groom SOP creates controls and joints from a few selected master hairs/feathers that drive the whole groom; Feather Template Assign SOP (assign feather templates to curves by layers, probabilities, weights, masks); Transfer Velocity for motion blur to Solaris; Feather Surface barb segment reduction; Feather Deintersect gains barb reduction; Feather Surface Blend pins roots during relax; Guide Interpolation Mesh SOP "Compute Weights" toggle; Hair Clump SOP accepts an `orient` attribute — [SideFX: What's new 20.5 hair, fur, feathers](https://www.sidefx.com/docs/houdini/news/20_5/feathers.html)
- APEX Add Groom (21.0 docs): by default constrains hair to the animated skin; options "Straighten Guides" and "Add White Hairs" — [SideFX APEX Add Groom](https://www.sidefx.com/docs/houdini/fur/addgroom.html)
- Houdini 21 (released Aug 2025) sneak-peek lists mention Guide Clump Center (generate clump centre lines), Guide Surface, Guide Find Strays (identify/group strays), fast grooming with Parallel Transport Along Geometry — names from a pre-release sneak peek, not confirmed in final notes — [CG Channel sneak peek](https://www.cgchannel.com/2025/08/sneak-peek-houdini-21-see-300-features-listed-in-the-video/); [SideFX forum sneak peek](https://www.sidefx.com/forum/topic/101604/)
- Workflow: animators pose a few APEX master curves, extract animated guides via Scene Invoke, then feed classic Guide Deform for high-res hair — [Digital Production on APEX](https://digitalproduction.com/2026/07/01/houdini-rigging-how-i-learned-to-stop-worrying-and-start-loving-apex/)
- Karma XPU renders fur; hair procedurals generate hair from guides at render time — [SideFX Hair & Fur](https://www.sidefx.com/products/houdini/characters/hair-fur)
- Houdini 22 docs reference a new "Guide deforming workflow" (Capybara Sim and Deform shelf recipe) and a Guide Process tab with a grooming viewport state and radial menus — [SideFX Houdini 22 feathers](https://www.sidefx.com/docs/houdini/news/22/feathers.html); [Guide Groom docs](https://www.sidefx.com/docs/houdini/fur/groom.html)

**Blender**
- Hair curves system (since 3.5) uses Geometry Nodes hair node assets (e.g. Generate Hair Curves on a mesh surface + UV) and Sculpt Mode comb/smooth/puff — [Blender manual: Generate Hair Curves](https://docs.blender.org/manual/en/latest/modeling/geometry_nodes/hair/generation/generate_hair_curves.html); [Blender 3.5 release](https://www.blender.org/download/releases/3-5/)
- 4.3: sculpt mode entry ~5x faster; 4.4: stability focus, "new operations for Curves"; 4.5 LTS: data-access nodes, Manifold trim solver — [Blender 4.3](https://www.blender.org/download/releases/4-3/); [4.4](https://www.blender.org/download/releases/4-4/); [4.5 LTS](https://www.blender.org/download/releases/4-5/)
- Blender 5.0 (Nov 2025): new curve rendering algorithm making hair rendering up to 50% faster in Cycles; EEVEE hair higher quality/faster with self-shadowing fixed; new "Cylinder" viewport display for thick curves; Geometry Nodes bundles and closures; sculpt brush pressure curves — [Digital Production](https://digitalproduction.com/2025/11/20/blender-5-0-its-here/); [CGPress](https://cgpress.org/archives/blender-5-0-released.html); [Linuxiac](https://linuxiac.com/blender-5-0-released-with-a-ui-overhaul-smarter-nodes-and-hdr-support/)
- Large paid add-on ecosystem fills hair gaps (hair cards from curves, stylized hair, facial hair kits) — [80.lv custom GN hair system](https://80.lv/articles/developing-a-custom-hair-creation-system-with-blender-s-geometry-nodes); [Facial Hair Toolkit](https://blendermarket.com/products/facial-hair-toolkit)

**Unreal Engine / MetaHuman (Epic)**
- Epic docs: the groom system is a "minimal viable implementation" of import/edit/render/sim and "does not provide a hair grooming solution, like being able to shape hair" — hair is styled externally and imported as Alembic — [Epic: Groom Strands](https://dev.epicgames.com/documentation/en-us/unreal-engine/groom-strands-in-unreal-engine)
- Groom Asset Editor exists for LODs/cards/physics settings; Hair Card Generator (experimental) auto-generates cards from strands — [Epic Groom Asset Editor](https://dev.epicgames.com/documentation/en-us/unreal-engine/groom-asset-editor-user-guide-in-unreal-engine); [80.lv Hair Card Generator](https://80.lv/articles/learn-how-to-use-ue5-s-hair-card-generator)
- MetaHuman 5.7 (with UE 5.7): Groom plugin offers finer strand shaping through a joint-based workflow and an experimental option to blend keyed animation with hair physics; MetaHuman Groom Advanced Kit for Houdini (ponytails, braids) on Fab; grooms can be made in MetaHuman for Maya with XGen — [metahuman.com 5.7 release (login-gated; via search summary)](https://www.metahuman.com/releases/metahuman-5-7-is-now-available)
- MetaHuman Groom Tools for Houdini: MH Groom Builder for styling interpolated hair, MH Groom Bun, MH Groom Cornrow and other style nodes — [Epic: MH Groom Tools](https://dev.epicgames.com/documentation/metahuman/mh-groom-tools)
- Parametric Hairstyle Generator (experimental, Houdini): uses pre-built guide data and slider-based style parameters to adjust hairlines, follicles, guides and strands, replacing manual guide grooming — [Epic: Parametric Hairstyle Generator](https://dev.epicgames.com/documentation/metahuman/parametric-hairstyle-generator?lang=en-US)
- Third-party grooms on Fab are sold as MetaHuman-compatible, editable in the MetaHuman Creator plugin; full compatibility requires building against Epic's standard MetaHuman groom — [Fab listing](https://www.fab.com/listings/622c1a7e-f8c0-4761-bc7a-414175c7cae1?lang=en); [Viirtuals guide PDF](https://viirtuals.com/guide/metahumanGroomGuide-Viirtuals.pdf)
- Latest MetaHuman for Houdini targets UE 5.8 and supports characters made in 5.6-5.8 — [Fab: MetaHuman for Houdini](https://www.fab.com/listings/7bbdfbb5-5eaf-4aa6-b32b-b8b048ebea25)

**Hair-card tools and smaller vendors**
- GS CurveTools v2 (Maya): all-in-one cards/tubes/braids; new Mesh Generator node faster than v1.3 card warp; Fill Plus window creates curves/cards between selected curves; braid generator; UV Lattice; v2 NOT compatible with v1 scenes; Maya 2018-2027 Windows, macOS only on v1.3.19 — [ArtStation GS CurveTools v2](https://www.artstation.com/marketplace/p/zzR2/gs-curvetools-v2-maya-plug-in-braids-hair-card-sculpting-and-more); [GS CurveTools changelog](https://gs-curvetools.readthedocs.io/en/latest/changelog.html)
- HairTG (Eyosido): Substance Designer hair/fur texture generator + Blender card add-on; HairTG-Cards generates card geometry from Blender hair curves; free limited "HairTG - Strand"; HairTG-Cards 2 in early access 2024 — [Flippednormals HairTG](https://flippednormals.com/product/hairtg-hair-fur-287); [Eyosido Gumroad](https://eyosido.gumroad.com/l/hairtg-hairandfur); [HairTG-Strand 2.5](https://eyosido.gumroad.com/p/hairtg-strand-2-5-is-available)
- FiberShop: procedural hair-strip texture design outputting alpha, normal, ID and other maps; no 2025 changelog found — [yelzkizi (low-quality tutorial site)](https://yelzkizi.org/best-hair-grooming-tools-for-unreal-engine/)
- Character Creator 5 (28 Aug 2025): HD base characters, new Maya plugin; hair via "Smart Hair" Hair Builder packs of modular elements with morphs, physics and material presets (e.g. Hair Builder 3 - Edgy: 20 pre-assembled styles, 40 elements with "base connectors") — [CG Channel CC5](https://www.cgchannel.com/tag/smart-hair-shader); [Reallusion marketplace](https://marketplace.reallusion.com/tokomotion-hair-pack-vol-2)

### Inferences
- Both commercial leaders are converging on "direct manipulation of procedural results" (Yeti soft selection on every node, Ornatrix clump manipulators and curved sinks). Grooming by gizmo on top of procedural layers is the current frontier.
- Intersection/deintersection is a recurring 2024-2026 theme (Yeti Plume/Resolve, Houdini Feather Deintersect, Ornatrix Merge overlap resolve). A generic fiber deintersect node is a likely gap for GroomLab.
- A headless graph evaluator (Yeti Evaluation SDK) shows studios want groom graphs evaluated outside the DCC (farm, engine, USD). GroomLab's core library already matches this; publicising it as an SDK is a positioning win.
- Epic is betting on parametric, slider-driven hairstyles plus modular style nodes (bun, cornrow, ponytail, braid) rather than in-engine grooming. A free tool with in-engine/in-viewport styling or a parametric-hairstyle layer would fill the space Epic left open.
- Reallusion's modular "hair elements with base connectors" is a kit-bash model: pre-built locks snapped onto a base. A strand-based version (hairstyle parts as reusable sub-graphs) is a distinctive idea.

### Gaps
- No Yeti 6 information or Peregrine roadmap found.
- No official Ornatrix for Blender release found in search (Ephere has had a Blender beta in the past, unconfirmed here); Ornatrix UE plugin details beyond Alembic/groom_color not found.
- Houdini 21 final release notes for hair (vs sneak-peek names) not retrieved; Vellum hair changes in 20.5/21 not found.
- Blender 5.0 official release notes for curves/hair sculpt brushes not retrieved; whether 5.0 added new hair node-group assets is unconfirmed.
- MetaHuman Creator in-app hair editing specifics (5.6 vs 5.7) behind Epic login; only search summary used.
- ZBrush FiberMesh, Cinema 4D native hair, Hair Farm (3ds Max), Sisir, and dedicated AI-groom startups: no 2024-2026 sources surfaced.

## Distinctive workflow ideas GroomLab lacks or could adopt

### Takeaway
The distinctive ideas surfacing now are: branching procedural history (Ornatrix), viewport manipulators on procedural clumps, painting textures directly onto strands, procedural root density warping, slider-driven parametric hairstyles from pre-built guide data (MetaHuman), APEX "master hair" rigs that animators pose, modular hairstyle element kits, and image-to-strands generation from research.

### Cited Findings
- Operator stack branching: multiple shapes/bases share one groom history — [Ephere forum](https://ephere.com/plugins/autodesk/maya/ornatrix/forums/news/thread_17988.html); [CG Channel](https://www.cgchannel.com/2025/09/ephere-releases-ornatrix-maya-5/)
- Texture paint brush paints textures/decals onto hair in viewport; Mesh from Strands bakes textures from dense hair onto a mesh approximation — [CG Channel](https://www.cgchannel.com/2025/09/ephere-releases-ornatrix-maya-5/)
- Curved comb "sinks" drawn on the surface direct/repel/attract hair — [CG Channel](https://www.cgchannel.com/2025/09/ephere-releases-ornatrix-maya-5/)
- Groom Sampler: any node parameter driven by a groom's painted attribute or length — [Peregrine Labs](https://support.peregrinelabs.com/support/solutions/articles/66000522678-what-s-new-in-yeti-5-0)
- Viewport hair shader with melanin/IOR to match production render — [CG Channel](https://www.cgchannel.com/2025/03/peregrine-labs-ships-yeti-5/)
- Import competitor groom data into own graph (Yeti imports XGen IGS) — [CG Channel](https://www.cgchannel.com/2025/03/peregrine-labs-ships-yeti-5/)
- APEX master hairs: animators pose a few curves, the rest follows, constrained to skin; extracted as animated guides — [SideFX APEX Add Groom](https://www.sidefx.com/docs/houdini/fur/addgroom.html); [Digital Production](https://digitalproduction.com/2026/07/01/houdini-rigging-how-i-learned-to-stop-worrying-and-start-loving-apex/)
- Find Strays / Clump Center utility nodes (sneak-peek names) — [CG Channel sneak peek](https://www.cgchannel.com/2025/08/sneak-peek-houdini-21-see-300-features-listed-in-the-video/)
- Feather template assignment by layers/probabilities/weights/masks — [SideFX 20.5](https://www.sidefx.com/docs/houdini/news/20_5/feathers.html)
- Parametric Hairstyle Generator: sliders over pre-built guide data — [Epic](https://dev.epicgames.com/documentation/metahuman/parametric-hairstyle-generator?lang=en-US)
- Style-specific nodes: MH Groom Bun, MH Groom Cornrow — [Epic MH Groom Tools](https://dev.epicgames.com/documentation/metahuman/mh-groom-tools)
- Blend keyed hair animation with physics (experimental, UE 5.7) — [metahuman.com 5.7 (search summary)](https://www.metahuman.com/releases/metahuman-5-7-is-now-available)
- Modular hair elements with base connectors, morphs and physics presets — [Reallusion marketplace](https://marketplace.reallusion.com/tokomotion-hair-pack-vol-2)
- GS CurveTools "Fill Plus": create curves/cards between selected curves — [ArtStation](https://www.artstation.com/marketplace/p/zzR2/gs-curvetools-v2-maya-plug-in-braids-hair-card-sculpting-and-more)
- Image-to-strand research: TANGLED (diffusion; 457-style dataset; polyline output, arbitrary viewpoint/style), DiffLocks (image-conditioned diffusion transformer, large synthetic strand dataset), HairGPT (strand-as-language autoregressive), 2026 LRM+multimodal paper (buns, ponytails, curly) — [TANGLED arXiv](https://arxiv.org/abs/2502.06392); [HairGPT](https://haiminluo.github.io/hairgpt/); [2026 arXiv](https://arxiv.org/html/2608.13679v1)
- HairDeck: animation-ready hair cards from one character image, GLB/VRM output — [note.com overview](https://note.com/npaka/n/n06fcb7653436?hl=en)

### Inferences
- High-value, novel-for-GroomLab ideas (to verify against GroomLab's current node set): (1) branching groom history / variants sharing a base; (2) generic Deintersect/Resolve node for strands and feathers; (3) paint-onto-strands decal/texture brush (stylised and animal patterns); (4) Warp Roots density warping; (5) "master hair" rig output for animators (few posable controls driving guides) — pairs with GroomLab's existing simulate/correctives; (6) parametric hairstyle templates (hairline, part, volume sliders over a stored guide set) built on GroomLab's .groom package; (7) style-specific macro nodes (bun, cornrow, ponytail) as sub-graphs; (8) import of XGen IGS/Yeti/Ornatrix data into the graph (already on GroomLab's universal-format roadmap); (9) a production-matched viewport hair shader with melanin/IOR; (10) keyed-pose-plus-physics blend.
- Image-to-strands research (TANGLED, DiffLocks, HairGPT) could extend GroomLab's existing "guides from photos" toward full style priors; licensing of those models/datasets would need checking.

### Gaps
- No commercial "AI groom" startup with a shipped product was confirmed; Pinscreen DeepHair came only from a low-reliability blog and was not included.
- No hairstyle preset marketplace for procedural (graph) grooms found beyond Fab/Reallusion asset sales.

## Common artist complaints and wishes

### Takeaway
Search did not surface current Reddit/Polycount threads, so complaint evidence is thin and partly old. What is citable: XGen stagnation and viewport bugs, interpolation-quality differences between tools, real-time parting/interpolation limits, slow fur texture/card baking, plugin-evaluation failures on render farms, scene-incompatibility on major version upgrades (GS CurveTools v2), and Unreal lacking any in-engine shaping.

### Cited Findings
- XGen IGS brushes not updating in viewport (driver-dependent) — [Autodesk support](https://www.autodesk.com/support/technical/article/caas/sfdcarticles/sfdcarticles/XGen-Interactive-Grooming-Brushes-are-not-visible-in-Maya-Viewport.html)
- Opinion: XGen undeveloped since 2016, problematic for animation; Ornatrix faster to results and faster parting since v3 — [charlytuts blog, 2021](https://charlytuts.blogspot.com/2021/02/ornatrix-advantages-or-why-i-ditched.html)
- Users asked Ornatrix for XGen-style interpolation, saying Ornatrix interpolation looks less natural; Ephere replied clustering/parting can approximate it (old request) — [Ephere feature request](https://ephere.com/plugins/autodesk/max/ornatrix/request_4867.html)
- Real-time: "you can't control the interpolation between the hair and parting" — [80.lv CGI vs real-time](https://80.lv/articles/004adk-hair-production-cgi-vs-real-time-workflows)
- Baking fur textures from FiberMesh/groom geometry described as slow guess-and-wait; replies suggest exporting tubes from XGen/Ornatrix — [Polycount (old thread)](https://polycount.com/discussion/205158/looking-for-fur-texture-workflow-ideas-possibilities-with-xgen-and-arnold)
- Render farms: remote renders come out bald when Yeti/Ornatrix/XGen plugin isn't evaluated (vendor marketing; timing figures unverified) — [iRendering](https://irendering.net/redshift-render-farm-guide-hair-grooming-instancing/)
- Version break: GS CurveTools v2 "NOT compatible with v1 scenes" — [ArtStation](https://www.artstation.com/marketplace/p/zzR2/gs-curvetools-v2-maya-plug-in-braids-hair-card-sculpting-and-more)
- Platform gaps: Yeti dropped macOS; Ornatrix macOS only partial — [CG Channel Yeti](https://www.cgchannel.com/2025/03/peregrine-labs-ships-yeti-5/); [CG Channel Ornatrix](https://www.cgchannel.com/2025/09/ephere-releases-ornatrix-maya-5/)
- Unreal: no shaping tools; all grooming external then Alembic import — [Epic docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/groom-strands-in-unreal-engine)
- Hair Card Generator limited to simpler styles; more complex styles a stated future aim — [80.lv](https://80.lv/articles/learn-how-to-use-ue5-s-hair-card-generator)

### Inferences
- Where GroomLab can win on complaints: stable file format across versions (GroomLab already has a no-format-migration rule), macOS support (Yeti absent, Ornatrix partial), self-contained farm evaluation via the core library/USD procedural, a single groom driving both strands and cards, and viewport-to-render fidelity.
- The "bald on the farm" problem argues for baking to plain curves/USD as a first-class, one-click output.

### Gaps
- No 2024-2026 Reddit (r/vfx, r/blender, r/Maya) or recent Polycount threads retrieved; no YouTube groomer interview quotes. A targeted follow-up on specific threads is needed for current sentiment.
- Praise data (what users love) per tool was not found in citable form beyond vendor/feature claims.

## Pricing and licensing landscape

### Takeaway
Yeti and Ornatrix cost roughly $200-650 per seat with rental options; XGen is bundled with Maya; Houdini grooming comes with Houdini licences; Blender hair is free but thin on grooming; Epic gives tooling away to drive Unreal/MetaHuman; card tools are cheap one-off purchases. Ornatrix went free for education in 2025, closing one door for a free tool among students.

### Cited Findings
- Yeti Indie (revenue under $100k/yr): 1 node-locked workstation + 1 render licence, $289 perpetual or $209/yr. Studio: 1 floating workstation + 5 render licences, $699 perpetual or $499/yr; extra 5 render licences $399 (as of 11 Dec 2025) — [CG Channel](https://www.cgchannel.com/2025/03/peregrine-labs-ships-yeti-5/)
- Ornatrix Maya 5: $649 perpetual workstation; rental $59/month or $504/year — [CG Channel](https://www.cgchannel.com/2025/09/ephere-releases-ornatrix-maya-5/)
- Ornatrix free to schools and students worldwide from 28 Sept 2025 — [CG Channel](https://www.cgchannel.com/tag/free-license)
- Epic: MetaHuman Groom tools/Houdini kit distributed via Fab and Epic docs; Unreal Groom plugin included with engine — [Fab](https://www.fab.com/listings/7bbdfbb5-5eaf-4aa6-b32b-b8b048ebea25); [Epic docs](https://dev.epicgames.com/documentation/metahuman/mh-groom-tools)
- HairTG has a free limited version (HairTG-Strand) — [Eyosido](https://eyosido.gumroad.com/p/hairtg-strand-2-5-is-available)
- Cracked copies of GS CurveTools and Yeti circulate (evidence of price pressure among hobbyists) — [cgpersia listing](https://cgpersia.com/2025/02/peregrine-labs-yeti-5-1-8-maya-2023-2025-win-x64-198414.html); [vfxmed](https://www.vfxmed.com/2025/01/gs-curvetools-v1-3-cracked-maya-2025-download/)

### Inferences
- A free tool wins most clearly with indie/freelance artists who need cross-host (Maya + Blender + Unreal) portability that no single paid licence gives; Yeti and Ornatrix licences are per host.
- Render licences (Yeti charges separately) are a hidden cost GroomLab avoids by being free and by baking to plain curves/USD.
- Students now get Ornatrix free, so GroomLab's student pitch must rest on cross-host and standalone-app reach, not price alone.

### Gaps
- Houdini licence tiers, Maya pricing, GS CurveTools / FiberShop / HairTG prices, Character Creator pricing not retrieved.
- Ornatrix Blender/C4D/UE pricing not retrieved.
