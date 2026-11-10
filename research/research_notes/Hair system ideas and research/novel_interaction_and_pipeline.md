# Novel interaction, authoring and pipeline ideas for GroomLab (adjacent fields, 2022-2026)

Scope note: research done 2026-11-08 with about 19 web searches and fetches. Foundational (pre-2022) work is marked **[foundational]**. Difficulty estimates are inferences about GroomLab (node-based core, Maya/Blender/app/web hosts, MCP server, .groom files), not facts from the sources.

## Sketch-based, example-based and direction-field authoring for hair (what research has shown, what artists prefer)

### Takeaway
Hair research since 2023 has moved from "sketch -> vector field -> grow strands" to learned strand generators driven by sketch, text or image: GroomGen 2023, HAAR (CVPR 2024), DiffLocks (CVPR 2025), StrandDesigner (ACM MM 2025), HairLRM-style reconstruction (2026). None of them publishes an artist study showing pros prefer them to node grooming. The only hair-authoring user study found (Digital Salon 2025) says experts treat AI and retrieval as a fast starting point and keep a node tool (Blender) for the real work. Direction fields stay the backbone under the hood, but several sources say artists find raw vector fields unintuitive to edit.

### Cited Findings
- **HAAR (Sklyarova et al., CVPR 2024)**: a text-conditioned latent diffusion model makes *guide strands* in latent space, then latent upsampling yields dense grooms of up to ~100k strands, in seconds rather than the hours SDS methods need. Its latent space allows semantic edits such as text-prompt editing and strand-count upsampling. — [HAAR arXiv](https://arxiv.org/pdf/2312.11666); [HAAR project](https://haar.is.tue.mpg.de/); [CVPR paper](https://openaccess.thecvf.com/content/CVPR2024/papers/Sklyarova_Text-Conditioned_Generative_Model_of_3D_Strand-based_Human_Hairstyles_CVPR_2024_paper.pdf)
- **GroomGen (2023)** calls itself the first generative model for dense, detailed strand hair, using hierarchical latent representations. — [GroomGen (ResearchGate)](https://www.researchgate.net/publication/376258132_GroomGen_A_High-Quality_Generative_Hair_Model_Using_Hierarchical_Latent_Representations)
- **DiffLocks (CVPR 2025)** predicts a 256x256 scalp texture in which each texel holds one strand's latent code, then decodes strands directly from a single image. That makes it a "scalp-texture of strand codes" representation. — [DiffLocks CVPR 2025](https://openaccess.thecvf.com/content/CVPR2025/papers/Rosu_DiffLocks_Generating_3D_Hair_from_a_Single_Image_using_Diffusion_CVPR_2025_paper.pdf)
- **StrandDesigner (Zhang et al., accepted ACM Multimedia 2025)** claims to be the first sketch-conditioned strand generator. It uses learnable multi-scale strand upsampling and a transformer with diffusion heads for "multi-scale adaptive conditioning" across diverse sketch patterns. No user study is reported, and code was not yet released when checked. — [StrandDesigner arXiv](https://arxiv.org/abs/2508.01650)
- Recent strand pipelines also include reconstruction from a single image (Im2Haircut 2025), reconstruction models (HairLRM 2026), hair cards turned back into strands (HairCS 2026), and auto hair-card extraction with differentiable rendering (2025). — [Im2Haircut](https://arxiv.org/pdf/2509.01469); [HairLRM](https://arxiv.org/html/2606.15238); [HairCS](https://arxiv.org/pdf/2609.16465); [Auto hair card extraction](https://arxiv.org/pdf/2505.18805)
- **Digital Salon (He, Amador Herrera, Shu, Sun, Wang, Zhou et al., Adobe Research and others; arXiv July 2025)** is the most complete hair-authoring loop found:
  - CLIP text retrieval over **1,320 artist-groomed hairstyles** returns the top 3 matches.
  - Real-time Augmented Mass-Spring simulation runs under 20 ms per frame on an RTX 3090; users drag the head or ask for wind.
  - Simulation-driven "grab and trim" plus procedural beard growth refine the result.
  - A ControlNet render produces a final image.
  - Study: 7 participants plus 1 stylist, with modeling times of about 3.5 to 10 minutes.
  - Experts said Blender stays stronger for complex hair node work and framed the tool as complementary.
  - One novice found the parameter panel distracting.
  - Listed gaps: no digital comb, curling iron or wax; no image-guided retrieval; CUDA only; about 25 GB RAM and 9 GB VRAM.
  - [Digital Salon arXiv](https://arxiv.org/html/2507.07387v1)
- **[foundational] DeepSketchHair (2019)** does deep sketch-based 3D hair modeling. **[foundational] Fu et al. 2007** and Wang & Yang built sketching interfaces on fast vector-field solvers. — [DeepSketchHair](https://ar5iv.labs.arxiv.org/html/1908.07198); [Fu et al. SBIM 2007](https://hongbofu.people.ust.hk/doc/sbim07_hairstyle_sketching.pdf); [Sketching interface for hairstyles](https://typeset.io/pdf/a-sketching-interface-for-modeling-and-editing-hairstyles-4poxh3e0iv.pdf)
- **[foundational] Fisher et al. 2007, "Design of Tangent Vector Fields"**: constraints solved by weighted least squares update fast enough for *interactive* direction-field design on surfaces. This remains the reference for paint-a-flow tools. — [Hoppe project page](https://hhoppe.com/proj/vfdesign/)
- **DreamWorks** describes styling hair by building a sparse *volumetric* vector field from the tangents of input curves and then editing the field. — [DreamWorks Research](https://research.dreamworks.com/?p=446)
- **[foundational] Olsen et al. 2020, "Intuitive, Interactive Beard and Hair Synthesis with Generative Models"**: in its user study no user had prior experience with the interface, and users relied mainly on *color and vector-field brushes*. Several sketch-hair papers say raw vector fields are "not very intuitive" to edit directly. — [arXiv 2004.06848](https://arxiv.org/pdf/2004.06848)
- **"Curvature Space Editing of Highly-Coiled Hair"** (ACM, 2026 DOI) is a grooming tool for coily hair that edits in curvature space. Per the search snippet, it treats sag-free initialization as separate prior work. The full text returned 403, so details are unverified. — [ACM DL](https://dl.acm.org/doi/10.1145/3799902.3811115)

### Inferences
**Idea: "Strand-code scalp maps" as a node type** (DiffLocks-style)
- Store a per-texel latent or style code on the scalp UV and decode it to curves.
- Hair gain: one paintable texture holds the whole groom's style, so you can blend styles with a brush and it travels through .groom and the web viewer as a small image.
- Difficulty: high, since it needs a trained decoder. A non-learned version is medium: per-texel indices into a library of guide shapes ("style atlas").

**Idea: Text/retrieval to graph, not to strands**
- Digital Salon shows that *retrieval over an artist-made library* is good enough as a starting point, and experts then refine.
- For GroomLab, retrieval would return a **node graph preset** (fully editable) instead of baked strands. That fits the 21 core looks and preset sheets already in the project.
- Hair gain: a novice-friendly entry point that does not trap users in a black box.
- Difficulty: low to medium. Caption the presets, embed the captions (CLIP or text embeddings), and expose the search in the UI and over MCP.

**Idea: Sketch-a-silhouette to guides**
- The user draws front and side silhouette strokes and a part line; the system fits guide curves and a flow field (classic sketch-to-field, as in Fu 2007 and Wang & Yang), with no ML.
- Hair gain: a fast blockout before procedural detail.
- Difficulty: medium.

**Idea: Direction-field brush with singularity handles**
- Expose crowns and whorls as draggable singularities, with flow paint as soft constraints solved Fisher-style (weighted least squares on the scalp mesh).
- This answers the "raw fields are unintuitive" complaint: artists move whorls and part lines, not vectors.
- Difficulty: medium (a sparse solve on the scalp mesh).

**Idea: Coil/curl editing in curvature space** (per the 2026 coily-hair paper)
- Expose curvature and torsion profiles along the strand as editable curves, instead of xyz noise.
- Hair gain: Type 3-4 textured hair is a known weak spot of noise-based curl tools.
- Difficulty: medium. It has to fit the existing Curl/Coil nodes.

### Gaps
- No published controlled study compares artist preference between sketch, field painting, procedural nodes and text for hair. Digital Salon is a 7-person qualitative study.
- Not found: Polyvector-field (Diamanti et al. 2014) applications to hair, and 2022-2026 example-based curve synthesis papers (e.g. strand texture by example). These were not searched for lack of budget.
- The Perm paper (parametric hair, ICLR 2025?) did not appear in results, so it is unverified.

## VR/XR and tablet/pen grooming

### Takeaway
No hair-specific production XR grooming tool was found. The research reference is still HairBrush (UIST 2019): VR guide strokes plus a learned hairstyle prior. Mainstream VR modelers (Substance 3D Modeler, Gravity Sketch) have converged on "switch between desktop and VR at any time, with stylus support". There is hobby WebXR hair-card work (2024). Haptic hair research exists but as feel/feedback, not authoring.

### Cited Findings
- **[foundational] HairBrush (Xing, Hao Li et al., UIST 2019)**:
  - Users sketch rough guide strokes in VR, and a network trained on a curated hairstyle dataset proposes complete hairstyles.
  - Output is textured polygon strips (game hair cards) that can be converted to strands.
  - Its user study reports less manual work and better quality for hard head and facial styles.
  - [HairBrush PDF](https://vgl.ict.usc.edu/Research/HairBrush/HairBrush%20for%20Immersive%20Data-Driven%20Hair%20Modeling.pdf); [ACM](https://dl.acm.org/doi/10.1145/3332165.3347876)
- **Hair Creator VR (WebXR)** is a browser-VR prototype for low-poly hair cards; its first version shipped in August 2024. — [itch.io](https://yuripourre.itch.io/hair-creator)
- **HairTouch (2021)** is a haptic controller attachment that uses brush bristles to simulate the feel of hair and fur. — [Geeky Gadgets](https://www.geeky-gadgets.com/hairtouch-13-05-2021/)
- **[foundational] UNC Interactive Virtual Hair Salon (Ward, Galoppo, Lin)**: a 3D haptic interface lets users position strands and apply cutting and blow-drying, with multi-resolution simulation. **Magnenat-Thalmann et al.** did haptic cutting, brushing, curling and grasping. — [ISALON](http://gamma.cs.unc.edu/ISALON/); [ResearchGate](https://www.researchgate.net/publication/220090188_Interactive_Virtual_Hair_Salon)
- **Adobe Substance 3D Modeler** is built on Oculus Medium (acquired 2019). Artists can switch between flat screen and VR at any point, and it supports stylus input for precision surface sculpting. It is on Steam for Rift, Quest via Link and Valve Index. — [Road to VR](https://roadtovr.com/adobe-substance-3d-modeler-medium-vr-modeling-pro-workflows/); [Adobe](https://www.adobe.com/products/substance3d-modeler.html); [Steam](https://store.steampowered.com/app/1745780)
- **Gravity Sketch** is reviewed as intuitive for fast ideation; its "volume" tool is praised for laying down volume quickly. — [Parametric Architecture](https://parametric-architecture.com/vr-sculpting-gravity-sketch-substance-modeler/)

### Inferences
**Idea: WebXR mode in the web viewer**
- GroomLab already has a WASM web viewer. A WebXR session to view, comb and trim guides on a headset (Quest browser, Vision Pro Safari) would be a cheap way into XR without a native app.
- Hair gain: judging silhouette and volume in stereo at real scale matters for hair. Combing and cutting are 6-DoF hand actions by nature.
- Difficulty: medium for view only, high for editing that round-trips to the graph (edits must become node layers, see collaboration below).

**Idea: Desktop/VR switching like Substance Modeler**
- Treat XR as a *mode of the same document*, not a separate tool. The .groom file is the shared state.

**Idea: Pen pressure and tilt mapped to groom brushes**
- Pressure maps to comb strength, tilt to the lift angle from the scalp. This is a cheap win on tablets.
- Difficulty: low to medium per host.

**Idea: Learned prior behind VR strokes** (HairBrush pattern)
- A few 3D strokes retrieve and deform the nearest library hairstyle.
- Can reuse the retrieval idea from section 1.

### Gaps
- Not found: an Apple Vision Pro sculpting or grooming app; Gravity Sketch or Quill hair-specific workflows; any 2022-2026 haptic *grooming* (as opposed to feel) research. Searches returned nothing.
- No evidence on whether pro groomers would adopt VR; the HairBrush study is 2019 and small.

## AI assistants and natural-language driven 3D authoring (MCP, text-to-procedural-graph)

### Takeaway
LLM-to-DCC bridges (BlenderMCP) are popular and simple: a socket server inside the host plus an MCP server. But benchmarks show VLM agents do worst exactly on procedural node-graph editing. What works better is (a) the model writes code against a high-level API, (b) iterative render, critique and refine loops, and (c) "process traces" from expert workflows used as in-context examples. These lessons map straight onto GroomLab's MCP server and node graph.

### Cited Findings
- **BlenderMCP (ahujasid, 2025)**:
  - Architecture: a Blender add-on opens a socket server inside Blender, and a separate MCP server connects to it.
  - Works with Claude Desktop, Claude Code, Cursor, VS Code and others; open source and free.
  - Commentary says it "cannot replace a 3D artist" and advises artists to audit such plugins for privacy and licensing.
  - [mdskills](https://www.mdskills.ai/mcp-servers/blender-mcp); [ecosyste.ms](https://awesome.ecosyste.ms/projects/github.com%2Fahujasid%2Fblender-mcp); [AI Architects blog](https://theaiarchitects.com/blog/blender-mcp)
- **BlenderGym (CVPR 2025)**: a benchmark of 245 handcrafted start/goal scene pairs over five editing tasks. VLM systems did **particularly poorly on procedural material and geometry editing**, because that needs wiring nodes through the Blender Python API. — [BlenderGym CVPR](https://openaccess.thecvf.com/content/CVPR2025/papers/Gu_BlenderGym_Benchmarking_Foundational_Model_Systems_for_Graphics_Editing_CVPR_2025_paper.pdf); [arXiv](https://arxiv.org/html/2504.01786v1)
- **BlenderAlchemy (2024)** samples and refines node graphs iteratively, using VLM judgement of the rendered output. — [ResearchGate](https://www.researchgate.net/publication/386105899_BlenderAlchemy_Editing_3D_Graphics_with_Vision-Language_Models)
- **VLMaterial (ICLR 2025)** fine-tunes a VLM (LLaVA-NeXT / LLaMA 3) to output Blender procedural materials as Python, and releases about **550K image-program pairs**. — [VLMaterial paper](https://proceedings.iclr.cc/paper_files/paper/2025/file/5393eaf0b723278738788520ec54e4b4-Paper-Conference.pdf); [arXiv](https://arxiv.org/pdf/2501.18623)
- **MaterialApprentice (arXiv 2607.13318, 2026)**:
  - LLM "process synthesis" over expert material workflows; a compiler turns the trace into a Blender shader node graph.
  - Training-free, using in-context examples.
  - Reports beating VLMaterial, BlenderAlchemy and Blender-MCP in metrics and perceptual studies. It is a preprint, not peer-reviewed.
  - [arXiv](https://arxiv.org/html/2607.13318)
- **Digital Salon** uses natural language only for *retrieval* and *wind* commands, not for direct parameter editing. Experts still refine by hand. — [Digital Salon](https://arxiv.org/html/2507.07387v1)
- **HAAR** supports text-prompt edits in latent space, which is another way to edit by language. — [HAAR](https://arxiv.org/pdf/2312.11666)

### Inferences
**Idea: MCP tools at the "intent" level, not raw wiring**
- BlenderGym says raw node wiring is where agents fail. GroomLab's MCP should expose high-level, validated ops ("add clump layer to region X", "make curls tighter at the tips") that compile to graph edits. That is MaterialApprentice's "process trace -> compiler" idea, applied to grooms.
- Difficulty: medium. It is mostly API design plus a curated library of example traces (e.g. how each of the 21 core looks was built).

**Idea: Closed render-critique loop**
- The agent edits, renders a contact sheet (GroomLab already has `ctest -L sheet` contact sheets), the VLM compares it with the reference image or text, and the loop repeats.
- Hair gain: hair quality is mostly judged visually, and parameters are entangled.
- Difficulty: low to medium, since the pieces exist.

**Idea: Groom "process trace" dataset**
- Record the node-edit history of the shipped presets as step-by-step traces and use them as in-context examples for agents. Cheap, and it compounds over time.

**Idea: Language-driven edits must land as named, undoable node layers**
- Every AI edit becomes a visible node or override, never a hidden bake, so artists can audit it. This addresses the "black box" and privacy/trust concerns from commentary.

**Idea: Reference photo to graph**
- Image to preset retrieval, then agent refinement. Digital Salon lists image retrieval as unsolved for in-the-wild photos. DiffLocks and Im2Haircut-style reconstruction could provide guides that the graph then fits.

### Gaps
- No sources found on Houdini MCP, Maya MCP or Autodesk's in-product assistants (search returned nothing).
- No published failure taxonomy specific to hair or groom agents.
- No quantitative BlenderGym scores were collected per model.

## Procedural asset ecosystems, libraries and a "hairstyle library / style grammar"

### Takeaway
The hair-specific precedents are:
- Blender's bundled hair node-group assets (3.5) are *operators*, not hairstyles.
- MetaHuman ships grooms plus an experimental *parametric hairstyle generator* (sliders and procedural regions in Houdini) and a Groom Advanced Kit for braids and ponytails (MetaHuman 5.7, late 2025).
- Fab hosts community MetaHuman grooms.

An editable, remixable, versioned **hairstyle library** with previews (the Substance Source/Share model applied to grooms) seems open ground.

### Cited Findings
- **Blender 3.5** shipped a set of procedural hair node-group assets alongside the new curves grooming tools. The aim was "a solid foundation of procedural hair tools for an out-of-the-box experience". — [Blender devtalk](https://devtalk.blender.org/t/procedural-hair-nodes-nodegroup-assets-for-blender-3-5/27601)
- A tutorial site says Blender 3.5 includes **26 hair node-group assets** (generate, clump, curl and so on, but not full hairstyles) in the Essentials library's Hair category. This is a single low-authority source. — [yelzkizi](https://yelzkizi.org/realistic-hair-new-hair-geometry-nodes-hair-curves/)
- Third-party Geometry Nodes hair asset packs are sold on Gumroad and Blendatlas. — [Blendatlas Hair Nodes](https://blendatlas.com/products/geometry-node-asset-hair-nodes); [Gumroad](https://johnnygizmo.gumroad.com/l/habif)
- **Epic's Parametric Hairstyle Generator** (experimental, Houdini 20.5+) lets artists use *sliders and procedural regions instead of manual guide grooming*, which makes broad changes such as overall shape or hairline position easy. — [Epic docs](https://dev.epicgames.com/documentation/en-us/metahuman/parametric-hairstyle-generator); [MH Groom Tools](https://dev.epicgames.com/documentation/metahuman/mh-groom-tools)
- **MetaHuman 5.7 (Dec 2025)** added a MetaHuman Groom Advanced Kit for Houdini to teach ponytail and braid styles, plus a joint-based workflow in the Unreal Groom plugin for shaping and animating strands. — [MetaHuman 5.7 release](https://www.metahuman.com/releases/metahuman-5-7-is-now-available)
- **Fab** sells and gives away community MetaHuman grooms (braid ponytails, buzzcut/skin fade, short and curly grooms) that are editable in MetaHuman Creator. — [Fab listing](https://www.fab.com/listings/d042865f-0d8d-4668-9061-5a663c9da4f3?lang=en); [Free buzzcut skin fade](https://forums.unrealengine.com/t/pinkninjasstudio-free-metahuman-groom-buzzcut-skin-fade/2638683); [Braid ponytail](https://forums.unrealengine.com/t/td-studio-metahuman-braid-ponytail-groom-hair/2682580)
- Digital Salon's 1,320 artist-groomed hairstyle database with text captions shows a *captioned library* is enough for useful retrieval. — [Digital Salon](https://arxiv.org/html/2507.07387v1)

### Inferences
**Idea: Hairstyle library = .groom files of named grooms + a parametric "style grammar"**
- Each entry is a graph with a few exposed "style knobs" (Epic's sliders-and-regions model), plus a contact-sheet thumbnail, text caption, tags (hair type 1A-4C, length, cut technique) and semantic version.
- Hair gain: discovery, reuse and remixing. Captions double as the retrieval and agent corpus.
- Difficulty: medium. A web gallery can reuse the WASM viewer for live turntable previews instead of static thumbnails, which few asset stores can do.

**Idea: Style grammar**
- Decompose a style into regions (fringe, crown, nape, sides) times cut technique times texture times part. Users or agents compose "undercut + textured crop + 3B curls" as a typed recipe that expands to nodes.
- Precedent: Epic's "procedural regions". The grammar itself is novel.
- Difficulty: medium to high (taxonomy design is the hard part).

**Idea: Remix lineage**
- Store a "derived from <library id@version>" field in .groom so community remixes keep attribution and can pull upstream updates.
- Precedent: a pattern seen in package managers and Fab/Substance-like stores, but not verified for any of them here.

**Idea: Free content packs** fit the business-model memory (free core, content packs later).

### Gaps
- Not researched due to budget: Substance Source/Share and community-graph numbers, Houdini HDA marketplaces (Orbolt status), Blender Extensions platform rules, and Blender Market's rename. No figures on how texture-tool communities grew.
- No evidence found of an existing versioned, remixable *hairstyle* library outside Fab/MetaHuman.

## Collaboration and review: USD layering, per-shot overrides, diffing, version control, web review

### Takeaway
OpenUSD's BasisCurves is the de facto curve container for hair, but searches found **no public groom-specific USD schema or studio talk on layered groom overrides**. Groom overrides in production remain DCC-side (Maya per-follicle overrides, Houdini attribute-override masks). A layered, diffable groom document with web review links looks like real white space for GroomLab, but the evidence is absence, not a confirmed gap.

### Cited Findings
- `UsdGeomBasisCurves` is a batched curve type modeled on RIB Basis/Curves and "typically used for dense geometry such as hair or grass". One prim holds many curves, counted by `curveVertexCounts`. Pinned cubic curves better record "the authors' intent for interchange". Hermite curves suit animation guides. — [OpenUSD BasisCurves](https://openusd.org/dev/api/class_usd_geom_basis_curves.html); [UsdGeomCurves](https://openusd.org/dev/api/class_usd_geom_curves.html); [Pixar example usda](https://github.com/PixarAnimationStudios/OpenUSD/blob/release/extras/usd/examples/usdGeomExamples/basisCurves.usda)
- A V-Sekai issue proposes saving pen strokes as USDA BasisCurves per stroke, which shows community use of BasisCurves for sketch data. — [GitHub issue](https://github.com/V-Sekai-fire/transport-meshing-pen/issues/6)
- Production override practice: Maya per-follicle overrides change density, curl and dynamics on individual clumps; Houdini uses attribute overrides plus painted masks. — [Mike Cauchi guide](https://www.mikecauchiart.com/single-post/2016/10/16/beginners-guide-to-grooming-maya-2016); [SideFX intro to grooming](https://www.sidefx.com/tutorials/introduction-to-grooming-in-houdini-19-hair-and-fur/)
- Unreal's groom handoff is still Alembic, imported as a Groom asset. — [StraySpark blog 2026](https://www.strayspark.studio/blog/blender-hair-grooming-to-unreal-engine-pipeline-2026)
- A patent covers a "computer generated hair groom transfer tool" (moving grooms between characters). — [USPTO 11557076](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/11557076)

### Inferences
**Idea: Override layers in .groom**
- The base groom graph sits under per-shot or per-variant layers that only hold parameter deltas and painted-map deltas, like USD opinions.
- Hair gain: shot-specific fixes (wet hair, wind-tossed, hat-hair) without forking the groom.
- Difficulty: medium to high. It must respect the "no format migrations" rule, so it is additive only.

**Idea: Semantic groom diff**
- Diff two .groom versions at the graph level (nodes added or changed, param deltas) *and* visually (render both and show a heat map of strand displacement or density change).
- Hair gain: review in pull requests or notes, and a dailies "what changed" view.
- Difficulty: medium (graph diff is easy; a visual diff needs stable strand IDs).

**Idea: Web review links**
- Publish a .groom to the WASM viewer as a shareable link with pinned comments anchored to 3D positions on the scalp.
- Hair gain: supervisors review grooms without Maya.
- Difficulty: medium. Hosting needs a decision, since the project is free and donations-only.

**Idea: USD export as BasisCurves with primvars + a "GroomLab graph" custom layer**
- Interchange uses plain curves; the procedural recipe rides along as metadata.
- Matches the universal-format roadmap in memory.

### Gaps
- No public sources found on Pixar, Weta or ILM groom-in-USD workflows, groom-specific USD schemas or AOUSD hair working groups. They likely exist in talks (SIGGRAPH/DigiPro) that the searches did not surface.
- Not researched: version control of procedural graphs (e.g. Houdini node diff tools, Substance graph diff) and procedural-graph merge research.

## Physics-aware / feasibility-aware styling and cosmetology-informed tools

### Takeaway
The key enabling research is **sag-free initialization**: solving an inverse problem for rest shapes so a groom holds its authored shape under gravity at sim start. That covers Hsu et al. SIGGRAPH 2023 (contact-aware, hybrid strand sim) and a 2024 follow-up for discrete elastic rods. Simulation-driven grooming (grab, trim, wind) is now interactive in research (Digital Salon, NVIDIA's ADMM work with cutting planes). Cosmetology techniques (layering, fades, texturizing) are *not* represented as dedicated tools in research found; cutting research is mostly old haptic-salon work.

### Cited Findings
- **Hsu, Wang, Pan, Gao, Yuksel et al., "Sag-Free Initialization for Strand-Based Hybrid Hair Simulation" (ACM TOG 42(4), SIGGRAPH 2023)**:
  - A four-stage, two global-local pair framework solves for stable quasistatic configurations.
  - It recovers rest states per segment so hair doesn't sag at sim start.
  - Billed as the first *contact-aware* sag-free initialization.
  - Modifies the Darboux vector to remove Cosserat rest-pose ambiguity.
  - Handles hundreds of strands in seconds to minutes, with minimal impact on motion.
  - Builds on Hsu et al. 2022.
  - [Utah project](https://graphics.cs.utah.edu/research/projects/sag-free-hair/); [PDF](https://graphics.cs.utah.edu/research/projects/sag-free-hair/sig23_sagfree-hair.pdf); [ACM](https://dl.acm.org/doi/10.1145/3592143); [Roblox publications](https://about.roblox.com/publications/sag-free-initialization-for-strand-based-hybrid-hair-simulation)
- **"Rest Shape Optimization for Sag-Free Discrete Elastic Rods" (arXiv 2024)** is a follow-up that extends sag-free rest shapes to DER. — [arXiv 2409.12362](https://arxiv.org/pdf/2409.12362)
- **NVIDIA / Daviet ADMM GPU hair sim**: edited a groom of about 86k rendered curves in real time by simulating 10% of strands; the demo supports trimming rods along a cutting plane. — [3DVF](https://3dvf.com/en/nvidia-unveils-an-interactive-hair-simulation-technique-powered-by-ai/)
- **Digital Salon** has simulation-driven grab and trim, but lists combs, curling irons and wax as missing. — [Digital Salon](https://arxiv.org/html/2507.07387v1)
- **[foundational] Hair Cut Simulator (Hayashimoto, Mitani, Igarashi 2010)**: comb-scoop, scissor cut and wax for local shapes. Tests suggested it lowered anxiety before real haircuts. — [JST ERATO](https://www.jst.go.jp/erato/igarashi/en/projects/HairCut/index.html)
- **[foundational] UNC Virtual Hair Salon**: haptic cutting and blow-drying. — [ISALON](http://gamma.cs.unc.edu/ISALON/)
- A patent describes predicting haircut outcomes by shortening fibers and running a steady-state simulation. — [USPTO 10849408](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/10849408)
- A 2023 paper covers contactless multi-user virtual hair design synthesis (Electronics). — [MDPI](https://doi.org/10.3390/electronics12173686)

### Inferences
**Idea: "Groomed under gravity" toggle**
- Author in a zero-g or styled pose; GroomLab solves rest shapes so the hair *looks exactly as authored* when gravity and sim are on (Hsu 2023 / DER 2024).
- Complements the existing 0.39 Simulate preload/weightless mode, which per memory uses an analytic model with known 14° and buckling limits. A proper inverse-statics solve could lift those limits.
- Difficulty: high (a solver), but it has a strong precedent.

**Idea: Feasibility heat map**
- Color strands by how much stiffness or rest-curvature change they would need to hold the shape (or by sag if released). This flags styles that are "impossible without product", e.g. gravity-defying anime spikes.
- Hair gain: realism checks before sim, and art-direction honesty.
- Difficulty: medium (a one-step static solve per strand gives an estimate).

**Idea: Cosmetology tool set as nodes and brushes** (no research precedent found, so novel)
- **Guide-length cut**: pick a guide section, then a "travelling guide" or elevation angle (0°, 45°, 90°) yields one-length, graduated or layered cuts. Barber elevation-angle vocabulary maps straight onto length-from-angle formulas.
- **Fade/taper**: a length ramp over a scalp region with clipper-guard steps (#0-#8 as mm lengths).
- **Undercut**: a region mask with a hard length step.
- **Texturize/point-cut/thinning shears**: randomize tip lengths and remove a percentage of strands in mid-lengths.
- Hair gain: speaks the language of hairdressers and real references, and pairs with the style grammar.
- Difficulty: low to medium (mostly length/density ops plus scalp regions).

**Idea: Salon tools in sim**
- Comb (sim-aware drag), curling iron (set rest curvature on contact), wax (raise local stiffness / damping), blow-dry (a directional wind field that changes rest shape).
- Digital Salon explicitly lists these as missing. Memory notes the user calls physics brushes a "gimmick", so treat this as an optional, sim-mode-only idea.
- Difficulty: medium to high.

### Gaps
- No 2022-2026 paper found on layered-cut, fade or texturizing geometry tools. "Virtual haircut" research is mostly pre-2015 haptics or image GAN editing.
- No details on the 2026 coily-hair curvature paper (403).
- Not checked: whether Hsu 2023 code is public.

## Accessibility to non-experts: avatar hair customization (MetaHuman, Roblox, VRChat, Ready Player Me)

### Takeaway
Consumer platforms keep hair as a pick-from-catalog asset. Roblox hair is mostly a rigid accessory (layered clothing fits garments, not hair), and its creator market sells styles by named category (afro, locs, braids, Y2K and so on). Epic's MetaHuman goes furthest toward parametric non-expert hair (sliders, regions, editable grooms in Creator). Hard user-research data on what end users want from hair customization was not found.

### Cited Findings
- **Roblox layered clothing** stretches and fits over any body and existing clothing, unlike rigid accessories that attach to one point. Hair is generally the rigid kind. — [Roblox docs: layered clothing](https://create.roblox.com/docs/avatar/accessories/layered-clothing); [Roblox docs: layered accessories](https://create.roblox.com/docs/avatar/layered-accessories)
- A Roblox hair creator asked on the DevForum for officially supported **alpha hair** (cards) and PBR. This is anecdotal. — [Roblox DevForum](https://devforum.roblox.com/t/upcoming-launches-for-avatar-creators/3432370?page=2)
- Freelance listings for Roblox hair advertise categories: short, long, curly, afro, anime, braids, ponytails, ombre, Y2K, locs, wavy, spiky. This shows what sells, not what users ask for. — [Fiverr listing](https://www.fiverr.com/ambrebel/create-roblox-ugc-accessories-and-dynamic-head)
- **MetaHuman**: parametric hairstyle generator (sliders and regions), Fab grooms editable in MetaHuman Creator, braid and ponytail kit (5.7). — [Epic docs](https://dev.epicgames.com/documentation/en-us/metahuman/parametric-hairstyle-generator); [MetaHuman 5.7](https://www.metahuman.com/releases/metahuman-5-7-is-now-available)
- **Digital Salon** novices found the text workflow accessible but the parameter panel cluttered. Measured end-to-end times were about 3.5 to 10 minutes. — [Digital Salon](https://arxiv.org/html/2507.07387v1)
- **Hair Cut Simulator (2010)** framed a consumer use: preview a cut to reduce anxiety before a real haircut. — [JST ERATO](https://www.jst.go.jp/erato/igarashi/en/projects/HairCut/index.html)

### Inferences
**Idea: "Simple mode" over any graph**
- A preset exposes 5-8 named style knobs (length, volume, curl type 1A-4C, part side, fringe, fade height). Everything else hides.
- Usable on mobile and web through the WASM viewer, and doubles as the agent API surface.
- Directly answers the "cluttered panel" finding.
- Difficulty: low to medium (knob-promotion metadata in presets).

**Idea: Export to avatar platforms as cards**
- Cards with alpha are the format consumer platforms (Roblox UGC, VRChat, game engines) can ingest. GroomLab's cards pipeline plus a "platform target" preset (tri budget, texture size, rigid vs layered) would let a non-expert go from style knobs to a UGC-ready accessory.
- Difficulty: medium. Platform specs change.

**Idea: Representation and texture coverage**
- The sold categories (afro, locs, braids, coils) suggest textured hair is in high demand among creators. That is a place where strong curl and coil and braid tools (GroomLab has a braid engine) would stand out.

**Idea: Selfie to style suggestion**
- Image retrieval into the library, using reconstruction methods (Im2Haircut, DiffLocks) for guides. Consumer-grade but research-backed.
- Difficulty: high.

### Gaps
- Not found: VRChat hair practices, Ready Player Me (status after 2025) and any user surveys on avatar hair preferences. The searches returned no primary data.
- Not found: usage stats on how many players customize hair versus pick presets.
