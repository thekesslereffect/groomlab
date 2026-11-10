# ML / CV capture, reconstruction and generation of strand-based hair grooms (2023-2026)

Scope note: researched 2026-11-08. Where a fact could not be verified from a primary source (paper, project page, repo README) it is listed under Gaps rather than stated. "Code: research-only" means the README says so; exact LICENSE.txt wording was often not visible through the fetch tool.

## 1. Single-image and multi-view strand reconstruction: what is production-usable today?

### Takeaway
There are many strong research reconstructors (multi-view: NeuralHaircut, MonoHair, Dr.Hair, GaussianHaircut, GroomCap, HairGS; single-view: HairStep, DiffLocks, Im2Haircut, Perm, plus 2026 LRM-based pipelines), but none is turnkey for artists. They are Linux/CUDA research stacks that need a big GPU, take minutes (single-view) to hours or days (multi-view), and nearly all ship under research-only or non-commercial terms. The 2026 trend is toward "production-ready" outputs: scalp-rooted, evenly rooted strands made for grooming modifiers. Perm is the only one with an MIT code licence.

### Cited Findings

**Multi-view / video (per-subject optimisation)**
- NeuralHaircut (ICCV 2023, foundational for the 2024-25 Tübingen/ETH line) takes multi-view images or monocular video, fits FLAME coarse geometry, then outputs strand-based hair. It depends on NeuS, k-diffusion, MODNet, CDGNet, PIXIE and npbgpp. Code and model are "available for scientific research purposes" under LICENSE.txt, and pretrained models are on Google Drive. — [NeuralHaircut GitHub](https://github.com/Vanessik/NeuralHaircut)
- The MonoHair authors report that NeuralHaircut takes 3-4 days per subject on one RTX 3090, against 4-6 hours for MonoHair. They also report MonoHair at 60.8% precision and 17.8% F-score vs NeuralHaircut's 52.9% and 16.4% (synthetic, 2mm/20°). These are the authors' own numbers. — [MonoHair paper (arXiv 2403.18356)](https://arxiv.org/html/2403.18356v1), via [search summary](https://keyuwu-cs.github.io/MonoHair/)
- MonoHair (CVPR 2024) takes monocular video and needs COLMAP + Instant-NGP preprocessing, PyTorch 1.11/CUDA 11.3, tiny-cuda-nn, PyTorch3D, MODNet, CDGNet, DELTA and face-parsing. It was tested on an RTX 3090 Ti under Ubuntu 22.04 and outputs a `.hair` strand file shown in a Windows "VoxelHair" viewer. The licence is a LICENSE.txt whose terms were not visible. — [MonoHair GitHub](https://github.com/KeyuWu-CS/MonoHair)
- Dr.Hair (CVPR 2024) reconstructs scalp-connected strands without pre-training, via differentiable rendering of line segments. It reports that NeuralHaircut's estimated absolute strand orientation is "mostly the opposite" of the true direction, so it targets correct root-to-tip direction. — [Dr.Hair (arXiv 2403.17496)](https://arxiv.org/html/2403.17496v1); [CVPR paper](https://openaccess.thecvf.com/content/CVPR2024/papers/Takimoto_Dr.Hair_Reconstructing_Scalp-Connected_Hair_Strands_without_Pre-Training_via_Differentiable_Rendering_CVPR_2024_paper.pdf)
- Gaussian Haircut (ECCV 2024, ETH/MPI) represents hair as strand-aligned 3D Gaussians and outputs strand geometry that can be "rendered, edited, and animated using classical computer graphics techniques". The project page says it runs "in a few hours on a single GPU" from monocular video. — [Project page](https://eth-ait.github.io/GaussianHaircut); [arXiv 2409.14778](https://arxiv.org/abs/2409.14778v1)
- Gaussian Haircut's code is CC BY-NC-SA 4.0 (plus the 3DGS licence). It needs CUDA 11.8 and Blender 3.6, takes `raw.mp4` monocular video, and depends on NeuralHaircut (FLAME fitting, strand prior, diffusion prior), HAAR (upsampling), Matte-Anything, PIXIE, Face-Alignment and OpenPose. — [GaussianHaircut GitHub](https://github.com/eth-ait/GaussianHaircut)
- GroomCap (SIGGRAPH Asia 2024 Best Paper, ETH Zürich + Google; ACM TOG 43(6) art. 254) is "prior-free" multi-view capture. It builds an implicit volume of 3D orientation and occupancy using volumetric orientation rendering, traces explicit strands, then refines them with a "chained Gaussian" representation under photometric supervision. It is aimed at high-end capture rigs. No code release was found. — [arXiv 2409.00831](https://arxiv.org/pdf/2409.00831); [project page](https://syntec-research.github.io/GroomCap)
- GaussianHair (Luo et al., arXiv 2402.10483, Feb 2024; ShanghaiTech et al.) models each strand as a chain of cylindrical Gaussians with a light-aware Marschner-based (UE4 approximation) scattering model. It supports editing, relighting and dynamic rendering, and released a real-hair dataset. — [project page](https://haiminluo.github.io/gaussianhair); [arXiv](https://arxiv.org/abs/2402.10483)
- HairGS (BMVC 2025, TUM: Pan, Nießner, Kirschstein) works from multi-view images. It turns each 3DGS Gaussian into a two-joint mini strand, greedily merges these into strands, then refines and grows them. It "typically completes within one hour", proposes a strand-topology accuracy metric, and was evaluated on USC-HairSalon, Cem Yuksel's data and NeRSemble. — [arXiv 2509.07774](https://arxiv.org/abs/2509.07774)
- GeomHair (arXiv 2505.05376, 2025) reconstructs strands from colourless 3D scans using a diffusion prior. Only the title and abstract-level info were verified. — [arXiv PDF](https://arxiv.org/pdf/2505.05376)

**Single image**
- HairStep (CVPR 2023, foundational for single-view) uses a "strand map + depth" intermediate. The released HiSa/HiDa dataset (1,250 real 512² images with artist-drawn SVG hair strokes, strand maps, depth) and checkpoints are "available for non-commercial research purposes only". Hair matting is replaced by SAM, which "fails sometimes". Tested with torch 1.9/CUDA 11.1/Ubuntu 20.04. — [HairStep GitHub](https://github.com/GAP-LAB-CUHK-SZ/HairStep)
- DiffLocks (CVPR 2025, MPI Tübingen + Meshcapade) is an image-conditioned diffusion transformer. It predicts a scalp texture in which each texel holds the latent code of one strand, then decodes the latents to 3D strands. It is trained only on synthetic data, with a pretrained image backbone for in-the-wild generalisation, and takes a single frontal image. — [arXiv 2505.06166](https://arxiv.org/abs/2505.06166); [CVPR paper](https://openaccess.thecvf.com/content/CVPR2025/papers/Rosu_DiffLocks_Generating_3D_Hair_from_a_Single_Image_using_Diffusion_CVPR_2025_paper.pdf); [project page](https://difflocks.is.tue.mpg.de/)
- DiffLocks repo: inference and training code, StrandVAE, checkpoints and a 40K-sample synthetic Blender dataset (~100K strands per sample, with RGB render). It can export `.blend` and Alembic but needs exactly Blender 4.1.1, plus NATTEN and FlashAttention-2 CUDA kernels and torch 2.5. Downloading data or checkpoints needs registration. Commercial licensing goes through sales@meshcapade.com. — [DiffLocks GitHub](https://github.com/summ2020/difflocks); [project page](https://difflocks.is.tue.mpg.de/)
- Im2Haircut (ICCV 2025, MPI/TUM/ETH: Sklyarova, Zakharov, Prinzler, Becherini, Black, Thies) uses a transformer hairstyle prior trained on synthetic data for internal geometry plus real data for the outer shell. At inference it fine-tunes on the input image(s) through Gaussian-splatting rendering losses. It claims better orientation, silhouette and back-side consistency than HairStep, NeuralHDHair and Perm. Code is linked at github.com/Vanessik/Im2Haircut, licence not stated. — [arXiv 2509.01469](https://arxiv.org/abs/2509.01469); [project page](https://im2haircut.is.tue.mpg.de)
- HairLRM (SIGGRAPH 2026; Shen, Dong, Huang, Zheng, Zheng, Kui Wu) uses a Large Reconstruction Model mesh as a structural anchor for occluded parts (ponytails). A "Dual Orientation AutoEncoder" lifts it to strands, then vector-field singularities (curls) are fixed via latent optimisation and surface-guided refinement. No code link was found. — [arXiv 2606.15238](https://arxiv.org/abs/2606.15238)
- "Strand-based Hairstyle Generation via Large Reconstruction and Multimodal Models" (arXiv 2608.13679, Aug 2026; Hao, Huang, Shen, Wang, Zheng, Kui Wu) needs no task-specific training. It combines an LRM, a multimodal LLM and classical geometry processing to produce strands from one image "within only a few minutes", covering curly hair, ponytails and buns. It explicitly targets production-ready geometry for editing and simulation. No code link was found. — [arXiv 2608.13679](https://arxiv.org/abs/2608.13679)
- HairCS (arXiv 2609.16465, Sep 2026; Utah / LIGHTSPEED / UCLA) automatically converts hair-card models (textured strips) into strand hair. Strands start at the scalp, roots are evenly distributed and the volume is filled, so the output suits strand rendering, simulation and "grooming modifiers such as clumping, curling, and noise". It was validated on short, long, curly, bun and ponytail styles. A dataset is on Hugging Face (HairCS2027/HairCS); no code link was found. — [arXiv 2609.16465](https://arxiv.org/abs/2609.16465)
- PanoHair (arXiv 2508.18944, 2025) is "detailed hair strand synthesis on volumetric heads" and compares against HairStep. Only the title and snippet were verified. — [arXiv](https://arxiv.org/html/2508.18944)

**Industry**
- Epic: no ML photo-to-groom feature was found. Epic's 2025 "MetaHuman for Houdini" is free procedural grooming HDAs (ponytails, braids, buns) that export Alembic to Unreal, and Epic's grooms ship with a source grooming head mesh. — [Digital Production, 2025-09](https://digitalproduction.com/2025/09/01/metahuman-meets-houdini/); [Epic Bantu Knots groom](https://forums.unrealengine.com/t/2538891)

### Inferences
- **Nothing is "production-usable" as a bundled feature.** Every reconstructor except Perm carries research-only or NC terms (NeuralHaircut, HAAR, GaussianHaircut CC BY-NC-SA, HairStep data, DiffLocks via Meshcapade). They need Linux, CUDA, specific old PyTorch versions and 24-40 GB class GPUs, and they run for minutes (single-view) to hours or days (video/multi-view).
- **The most realistic integration path is a "bring your own reconstruction" importer** rather than shipping models. GroomLab would read the strands these tools already export (DiffLocks Alembic/.blend, MonoHair `.hair`, Perm `.data`/`.abc`) and then convert dense strands to editable GroomLab guides plus node parameters. That keeps licence liability with the user, who downloads the weights. The value GroomLab adds is the guide-extraction and fitting step, which no paper offers.
- **Gap that matches GroomLab's existing feature:** "guides from photos" today means calibrated photos plus hand-drawn guide lines. Orientation-map extraction (Gabor/2D orientation fields, used by every method above) could overlay a per-photo flow field to snap or auto-suggest guide strokes. That would use no learned weights, so there is no licence problem.
- **Card-to-strand is a natural reverse of GroomLab's game-card export.** HairCS shows the field now values root-correct, evenly rooted strand output ready for clump/curl/noise. GroomLab could do a classical (non-ML) card→guides importer for existing game assets.
- **2026 single-view work is converging on LRM + MLLM + geometry processing** (HairLRM, 2608.13679) instead of hair-specific training data. That lowers the dataset-licence problem but depends on large general models with their own licences.

### Gaps
- Exact LICENSE.txt text for NeuralHaircut, HAAR, MonoHair, HairStep code, DiffLocks code and Im2Haircut was not visible. Dr.Hair code availability and licence were not found.
- No paper reported VRAM or inference seconds for DiffLocks, Im2Haircut or HairStep single-image runs. I found no reliable per-method runtime table beyond MonoHair's comparison and HairGS's "within one hour".
- No public code was found for GroomCap, HairLRM, 2608.13679 or HairCS.
- No industry talks (Weta, ILM, Disney, Meta) on production hair capture pipelines were found in this pass.

## 2. Generative strand models (text / image / sketch / latent): can they output editable guides?

### Takeaway
Yes. Several generative models are explicitly hierarchical with a guide level: HAAR saves "guiding strands", GroomGen has a sparse-guide level, and Perm has a 32×32 guide texture. That makes guide-level output feasible. Text-to-hair in practice is either diffusion over guide latents (HAAR), retrieval from a captioned database (Digital Salon), or SDS distillation (StrandHead). The 2025-26 frontier adds sketch/lineart conditioning (TANGLED, StrandDesigner), autoregressive "strand-as-token" models (HairGPT), and braid-aware post-processing.

### Cited Findings
- **HAAR** (CVPR 2024; Sklyarova, Zakharov, Hilliges, Black, Thies): a text-conditioned diffusion model that generates *guiding strands* in a latent space, then latent upsampling to dense hair. Training data was artist hairstyles annotated by VQA systems. — [project page](https://haar.is.tue.mpg.de/); [MPI](https://is.mpg.de/ps/projects/text-conditioned-generative-model-of-3d-strand-based-human-hairstyles)
- HAAR repo: the flags `--save_guiding_strands`, `--save_upsampled_hairstyle` and `--save_latent_textures`, and an upsample resolution of 64/128, so guides can be exported separately. Tested on an A100 40 GB under Linux. Research-use licence. It supports interpolating between two text prompts. — [HAAR GitHub](https://github.com/Vanessik/HAAR)
- One secondary review reports HAAR at ~4.3 s per hairstyle vs hours for SDS-based TECA. This is a third-party figure. — [Liner review](https://liner.com/review/haar-textconditioned-generative-model-3d-strandbased-human-hairstyles)
- **GroomGen** (SIGGRAPH Asia 2023; Google + ETH: Zhou, Chai, Pepe, Gross, Beeler): a three-level hierarchy (single strand, sparse guide hairs, dense hair), with a strand-level and a hairstyle-level autoencoder and a densification step from guides to dense hair. It supports sampling, interpolation and interactive editing. No code was found. — [project page](https://syntec-research.github.io/GroomGen); [SIGGRAPH Asia listing](https://asia.siggraph.org/2023/index.html%3Fpost_type=page&p=14494&id=papers_130&sess=sess137.html)
- **Perm** (ICLR 2025; Yale/Adobe, C. He et al.): a SMPL-like parametric hair model. It uses PCA strand blend shapes and fitted geometry textures, with guide strands from a 32×32 texture (compressed from 256×256): a StyleGAN2 backbone for guide textures, U-Net super-resolution, and a VAE for residual textures. It supports random generation, fitting to existing hair, interpolation (θ, β or full) and single-view reconstruction (the single-view pipeline is not released). "Curly/kinky hairstyles are currently not well reconstructed." **MIT licence**, with pretrained `.pkl` networks plus fitted parameters for all 343 USC-HairSalon styles on OneDrive. ~64 GB RAM recommended for PCA fitting. — [Perm GitHub](https://github.com/c-he/perm); [arXiv 2407.19451](https://arxiv.org/abs/2407.19451)
- **DiffLocks** generates dense strands from an image via a per-texel strand-latent scalp map (see section 1). — [arXiv](https://arxiv.org/abs/2505.06166)
- **TANGLED** (arXiv 2502.06392, Feb 2025; ShanghaiTech group incl. Lan Xu, Jingyi Yu): a latent diffusion model conditioned on multi-view *lineart* via cross-attention, picking up strand density and parting lines. A parametric post-process enforces braid-specific constraints. The new MultiHair dataset has 457 hairstyles with 74 attributes, emphasising complex and culturally significant styles. The abstract mentions sketch-based strand editing. Code was not confirmed. — [arXiv 2502.06392](https://arxiv.org/abs/2502.06392)
- **StrandDesigner** (ACM MM 2025): "the first sketch-based strand generation model". It is a transformer with diffusion heads, a learnable multi-scale strand upsampling, and multi-scale adaptive conditioning. Code was "will be released" at github.com/fighting-Zhang/StrandDesigner, live status unverified. — [arXiv 2508.01650](https://arxiv.org/abs/2508.01650)
- **HairGPT** (SIGGRAPH 2026 journal; Luo, Ouyang, Xu, Yu): treats strands as tokens with a geometric tokenizer. It generates autoregressively, scalp-region by scalp-region, through a hierarchy from global layout to fine style, with region-aware semantic annotations. It claims compositional editing, rare and complex styles, and stylised-domain adaptation vs diffusion. Paper under CC BY 4.0; no code link. — [arXiv 2605.08824](https://arxiv.org/abs/2605.08824)
- **StrandHead** (arXiv 2412.11586): text-to-3D head avatars with disentangled strand hair. It distills 2D generative models pretrained on human data instead of using hair-text paired data. — [arXiv 2412.11586](https://arxiv.org/abs/2412.11586)
- **Digital Salon** (SIGGRAPH Asia 2024 Real-Time Live!, arXiv 2507.07387; Yale, Adobe, KAUST et al.): text-guided *retrieval* from a curated 3D hair database, then real-time simulation (10K-80K strands), interactive refinement and hair-conditioned image generation. User studies show it beats traditional workflows for rapid prototyping. — [arXiv 2507.07387](https://arxiv.org/html/2507.07387v1); [Yale page](https://graphics.cs.yale.edu/node/272)

### Inferences
- **Guide-level generation is a solved representation question.** HAAR, GroomGen and Perm all generate a sparse guide layer first and densify second, which is exactly GroomLab's guides → interpolation → modifiers stack. A GroomLab "generate guides" node could, in principle, take HAAR/Perm guide output and let GroomLab's own interpolation and clump/curl nodes replace the learned upsampler. That keeps everything editable.
- **Only Perm is licence-clean for code.** Its weights are trained on Hair20k, which is derived from USC-HairSalon, whose licence is unverified (see section 5). So even Perm weights carry dataset-provenance risk for a commercial-use tool.
- **Digital Salon's retrieval design is the cheapest "text-to-groom" GroomLab could do legally.** Caption GroomLab's own presets and community grooms and retrieve by text or embedding. No generative model and no dataset licence issue.
- **Sketch/lineart conditioning (TANGLED, StrandDesigner)** is the closest research match to GroomLab's "draw guide lines over photos". That suggests a "complete the groom from strokes" feature is research-validated, but no licence-clean model exists.
- **Braids:** TANGLED handles braids with a parametric post-process, not learned geometry. That supports GroomLab's choice of a procedural braid engine.

### Gaps
- Guide counts and points per guide for HAAR output were not stated in the README.
- I could not confirm whether code or weights exist for TANGLED, GroomGen, HairGPT or StrandHead.
- No 2023-2026 method was found that outputs a *node graph* or procedural parameters (clump/curl settings) rather than raw strands. This looks like an open niche (see section 3).

## 3. Learned priors useful inside a groom tool

### Takeaway
Research covers retargeting/transfer (HairCUP, for Gaussian avatars), style transfer onto grooms (Disney 2025), neural real-time strand simulation (Neuralocks), parametric fitting and interpolation (Perm, GroomGen), sketch completion (TANGLED, StrandDesigner) and braid constraints (TANGLED). Nothing was found specifically on learned auto-guide placement, auto-parting or 3D hairline detection for artist tools. Those remain open and are probably better served by classical methods in GroomLab.

### Cited Findings
- **Style transfer onto grooms:** Disney Research / ETH, "Shaping Strands with Neural Style Transfer" (SIGGRAPH Asia 2025), is "the first stylization pipeline to support hair and fur". It takes a style image and produces 3D-consistent, temporally coherent stylised grooms. No code was stated. — [Disney Research Studios](https://studios.disneyresearch.com/2025/12/14/shaping-strands-with-neural-style-transfer/)
- **Hairstyle transfer between heads (avatars):** HairCUP (ICCV 2025) models hair and face separately for 3D Gaussian avatars, "enabling seamless hairstyle transfer between avatars without requiring per-subject optimization". — [HairCUP CVF paper](https://openaccess.thecvf.com/content/ICCV2025/papers/Kim_HairCUP_Hair_Compositional_Universal_Prior_for_3D_Gaussian_Avatars_ICCV_2025_paper.pdf)
- **2D-only transfer:** HairFast (NeurIPS 2024) is a fast encoder for transferring a hairstyle between photos at near real-time. It is image-space only, but useful for making reference images. — [NeurIPS paper](https://proceedings.neurips.cc/paper_files/paper/2024/file/50f1dd93c57ba57fe246fba9e64f5b20-Paper-Conference.pdf)
- **Interpolation and fitting:** Perm fits parameters to existing 3D hairstyles and interpolates θ (guides), β (residual detail) or both. GroomGen supports interpolation and interactive editing. — [Perm GitHub](https://github.com/c-he/perm); [GroomGen](https://syntec-research.github.io/GroomGen)
- **Neural simulation:** Neuralocks (arXiv 2507.05191, 2025; Lin, Larionov, Chen, Roble, Stuyck; Meta Reality Labs authors) is a self-supervised, compact, strand-level real-time dynamic hair simulator meant to pair with reconstruction for automatic avatars. No code. GroomGen also includes a neural simulator that moves hair with head pose. — [arXiv 2507.05191](https://arxiv.org/abs/2507.05191); [GroomGen search summary](https://syntec-research.github.io/GroomGen)
- **Dynamic explicit strands on avatars:** HADES (ICCV 2025) anchors hair strand points to Gaussian components so hair animates with the avatar, beyond prior static or rigid hair. "Head Avatars with Dynamic Explicit Hair" (arXiv 2607.23861) and PhysHead (arXiv 2604.06467, "simulation-ready Gaussian head avatars") continue the line; only titles verified for the latter two. — [HADES CVF](https://openaccess.thecvf.com/content/ICCV2025/papers/Liao_HADES_Human_Avatar_with_Dynamic_Explicit_Hair_Strands_ICCV_2025_paper.pdf); [2607.23861](https://arxiv.org/pdf/2607.23861); [PhysHead](https://arxiv.org/pdf/2604.06467)
- **Parting lines** are a conditioning cue that TANGLED's lineart encoder picks up. — [arXiv 2502.06392](https://arxiv.org/abs/2502.06392)
- **Orientation correctness:** Dr.Hair's finding that NeuralHaircut flips strand direction shows root-to-tip direction is a key failure mode for learned reconstruction. — [Dr.Hair](https://arxiv.org/html/2403.17496v1)

### Inferences
- **"Inverse procedural" fitting is the most novel and GroomLab-specific opportunity.** Perm's split of low-frequency guides and high-frequency residuals is analogous to GroomLab guides plus clump/curl/noise nodes. Fitting GroomLab node parameters (clump count/profile, curl frequency/radius, noise) to a dense groom from any reconstructor would turn non-editable ML output into an editable graph. No paper was found that does this; it can be done with classical optimisation or differentiable evaluation and needs no licensed weights.
- **Retargeting:** GroomLab's marker-based transfer is already the practical answer. Research transfer (HairCUP) works on Gaussian avatars, not artist strands. A cheap upgrade suggested by Perm/FLAME-style methods is fitting a template head (FLAME/SMPL-X-like topology) to both heads to auto-place correspondence markers. Note that FLAME/SMPL-X carry their own licences (not verified here).
- **Style transfer** (Disney) suggests a "match look from reference image" node is a valid research direction. A practical GroomLab version would use the photo only to tune node parameters, not to deform strands neurally.
- **Neural sim** (Neuralocks) is for real-time avatars. GroomLab's guide sim already covers the authoring need; a learned sim would mostly matter for game export.

### Gaps
- No 2023-2026 paper found specifically on learned auto-guide placement, auto-parting or hairline detection on 3D scalps for authoring tools. Hairline and parting appear only as implicit cues in generative models.
- No learned curl-synthesis model beyond the curl handling inside HairLRM (vector-field singularities) and Perm's noted curly weakness.
- Weta/ILM/Pixar production talks on ML grooming were not found in this pass.

## 4. Neural / Gaussian-splat hair rendering for previews or real time

### Takeaway
Strand-aligned Gaussians (GaussianHair, Gaussian Haircut, GroomCap's chained Gaussians, HairGS, HADES) are the dominant 2024-26 representation. They are used mainly as a *differentiable renderer for reconstruction*, not as a real-time preview format for authored grooms. For real-time preview, classical strand rasterisation with LOD (2025-26 papers) is more relevant to GroomLab than splats.

### Cited Findings
- GaussianHair: each strand is a chain of cylindrical Gaussians with a light-aware Marschner (UE4 approximation) scattering split into primary and local components, plus a transmittance term. It supports relighting and editing; one benchmark lists PSNR 27.22 on multi-view OLAT. — [project page](https://haiminluo.github.io/gaussianhair); [ar5iv](https://ar5iv.labs.arxiv.org/html/2402.10483); [Wizwand listing](https://www.wizwand.com/paper/68fa72361e209131cea54f9a)
- Gaussian Haircut uses strand-aligned 3D Gaussians and its output is editable and animatable with classical CG. — [project page](https://eth-ait.github.io/GaussianHaircut)
- GroomCap refines traced strands with a "chained Gaussian" representation under photometric supervision. — [arXiv 2409.00831](https://arxiv.org/pdf/2409.00831)
- Im2Haircut uses Gaussian splatting to render the prior's output for 2D supervision. — [arXiv 2509.01469](https://arxiv.org/abs/2509.01469)
- "High-Performance Real-Time Implicit Strand-Based Hair Rendering via Software Rasterization" (Lipp, Jarabo, Wimmer, Bode; PACMCGIT 9(4), July 2026): a deferred software-rasterisation pipeline generating strands from hair meshes, with strand filtering and reconstruction at 1 spp, plus distance and coverage LOD with "minimal hardware support". No fps or code given. — [arXiv 2607.04230](https://arxiv.org/abs/2607.04230)
- A 2025 CGF paper on real-time LOD strand rendering (Huang et al.) is cited by HairLRM; only the citation was seen. — [HairLRM PDF](https://arxiv.org/pdf/2606.15238)

### Inferences
- For GroomLab previews, splats add little. GroomLab already has explicit strands, and Gaussians are mostly a bridge from photos to strands. A chained-Gaussian or strand-to-splat *export* could matter if users want to drop grooms into 3DGS scenes or web viewers, which is speculative.
- The "implicit strands from hair meshes + software raster + LOD" line (2607.04230) is directly relevant to GroomLab's web app, game cards and LOD goals, as an alternative to cards for mid- and far-range LODs.

### Gaps
- No verified fps, strand-count or GPU benchmarks were found for any Gaussian hair renderer or for 2607.04230.
- I did not check whether any Gaussian hair renderer has a permissive licence. The base 3DGS licence is non-commercial (inferred from GaussianHaircut shipping a separate LICENSE_3DGS; terms not read).

## 5. Datasets and licences a free tool could legally use; open code/weights and runtime cost

### Takeaway
Almost every hair dataset and pretrained model is research-only or non-commercial (CT2Hair CC BY-NC 4.0, HairStep data NC, HAAR/NeuralHaircut research-only, GaussianHaircut CC BY-NC-SA, DiffLocks research with commercial via Meshcapade). Perm's code is MIT but its weights and data derive from USC-HairSalon, whose licence I could not verify. GroomLab should treat all of these as "user-installed, user-licensed" optional bridges, or build its own synthetic data from its own procedural grooms, which is the strategy DiffLocks used with Blender.

### Cited Findings
| Asset | Content | Licence (as verified) | Source |
|---|---|---|---|
| CT2Hair (SIGGRAPH 2023, Meta RL + ZJU + CMU) | 10 CT-scanned hairstyles: 7.2 GB CT volumes, 248 MB strand models, template heads, scalp points | CC BY-NC 4.0 | [GitHub](https://github.com/facebookresearch/CT2Hair) |
| Hair20k (Perm) | ~21,054 augmented hairstyles from USC-HairSalon; blend shapes, neural textures | Repo MIT; data provenance USC-HairSalon (licence unverified) | [Perm GitHub](https://github.com/c-he/perm) |
| DiffLocks dataset | 40K synthetic Blender hairstyles, ~100K strands each, plus RGB | Research; registration; commercial via Meshcapade | [GitHub](https://github.com/summ2020/difflocks), [project](https://difflocks.is.tue.mpg.de/) |
| HiSa/HiDa (HairStep) | 1,250 real images, SVG strokes, strand maps, depth | Non-commercial research only | [GitHub](https://github.com/GAP-LAB-CUHK-SZ/HairStep) |
| MultiHair (TANGLED) | 457 styles, 74 attributes, culturally diverse | Not verified | [arXiv](https://arxiv.org/abs/2502.06392) |
| HairCS dataset | Card→strand pairs (HF: HairCS2027/HairCS) | Not verified | [arXiv](https://arxiv.org/abs/2609.16465) |
| GaussianHair real-hair dataset | Real hair with strand geometry | Not verified | [project](https://haiminluo.github.io/gaussianhair) |
| USC-HairSalon (Hu et al. 2015, foundational) | 343 hairstyles | Not verified | used by Perm, HairGS ([HairGS](https://arxiv.org/abs/2509.07774)) |

**Code/weights and runtime summary (verified fields only)**
| Method | Code | Weights | Licence | HW / runtime (as stated) |
|---|---|---|---|---|
| NeuralHaircut | yes | yes (GDrive) | research-only | 3-4 days/subject on RTX 3090 (per MonoHair paper) |
| MonoHair | yes | prereq models | LICENSE.txt (unread) | RTX 3090 Ti; 4-6 h/subject (authors) |
| Gaussian Haircut | yes | uses NH/HAAR priors | CC BY-NC-SA 4.0 + 3DGS | CUDA 11.8; "few hours" single GPU |
| HairGS | not verified | - | - | "typically within one hour" |
| GroomCap | not found | - | - | - |
| HairStep | yes | yes | data/ckpt NC research | torch 1.9, CUDA 11.1 |
| DiffLocks | yes | yes (registration) | research; commercial via Meshcapade | torch 2.5, NATTEN, FlashAttn2; time n/a |
| Im2Haircut | linked repo | n/a | not stated | n/a |
| HAAR | yes | yes | research-only | tested A100 40 GB; ~4.3 s/style (secondary) |
| Perm | yes | yes (OneDrive, status unclear in TODO) | MIT | CUDA ≥11.3, gcc ≤10; 64 GB RAM for PCA fitting |
| StrandDesigner | promised | - | - | - |
| 2608.13679 (LRM+MLLM) | not found | - | - | "a few minutes" |

### Inferences
- **Legal-use ranking for a free tool used commercially:** (1) the user's own data and GroomLab's procedural presets (safe); (2) Perm code (MIT) trained on GroomLab-generated synthetic data (safe if GroomLab trains it); (3) Perm released weights (MIT code but dataset provenance unclear); (4) everything else is research-only or NC and cannot be bundled.
- **The DiffLocks recipe (an automated synthetic Blender dataset of 40K styles) is reproducible with GroomLab itself.** GroomLab's node graph can generate large licence-clean synthetic datasets with ground-truth guides *and* node parameters. That is a strategic asset: a community or third party could train a GroomLab-native prior (image → GroomLab graph parameters) without NC data.
- **Runtime reality:** single-image methods take seconds to minutes on A100 or 3090-class GPUs; multi-view/video methods take hours or days. None runs on CPU or Mac, which conflicts with GroomLab's macOS and laptop targets, so it fits only as an optional cloud or local-CUDA helper.

### Gaps
- USC-HairSalon, MultiHair, HairCS and GaussianHair dataset licences could not be verified.
- Perm weights: the README links OneDrive `.pkl` networks but the TODO still lists "Release pre-trained checkpoints" as unchecked, so the status conflicts.
- No VRAM numbers were published for any method checked.
- FLAME / SMPL-X licences (dependencies of NeuralHaircut and GaussianHaircut) were not checked here.
