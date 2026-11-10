# Hair and fur appearance research (2022-2026): shading, rendering, color biology

Scope note: researched 2026-11-08 with about 17 search/fetch calls. Older work is marked [foundational]. Every fact below has a source. Inferences are labeled. Where I could not reach a primary source, I say so.

## 1. Hair BSDF research since 2022 and renderer status

### Takeaway
The main new BSDF is Huang, Hullin and Hanika's 2022 microfacet hair model, which handles elliptical cross-sections. Blender Cycles ships it next to Chiang 2016. The other changes are in renderers: MaterialX gained a Chiang hair BSDF in v1.39.2 (Jan 2025), Arnold 7.4.4 (Nov 2025) added an "accurate"/"adaptive" cylindrical scattering mode and an OSL `chiang_hair_bsdf`, and OpenPBR still has no hair model. So the portable baseline across hosts is Chiang plus melanin. Ellipticity (aspect ratio plus orientation) is the one new per-strand input a groom tool can author.

### Cited Findings
- Huang W., Hullin M.B., Hanika J. (2022), "A Microfacet-based Hair Scattering Model", Computer Graphics Forum 41: 79-91, doi:10.1111/cgf.14588. Blender describes it as microfacet-based reflection and transmission that supports elliptically shaped hair. — [Blender manual, Principled Hair BSDF](https://docs.blender.org/manual/de/dev/render/shader_nodes/shader/hair_principled.html)
- Blender's Principled Hair BSDF (Cycles only) implements both Chiang et al. 2016 [CBTB16] and Huang et al. 2022 [HHH22]. Common inputs: Color, Melanin, Melanin Redness, Tint, Absorption Coefficient, IOR, Offset (cuticle tilt), Random Color, Random Roughness, Random. Chiang-only: Roughness, Radial Roughness, Coat. Huang-only: Aspect Ratio, Roughness, Reflection, Transmission, Secondary Reflection. — [Blender manual (latest)](https://docs.blender.org/manual/en/latest/render/shader_nodes/shader/hair_principled.html)
- Blender defines Aspect Ratio as "the ratio of the minor axis to the major axis of an elliptical cross-section". It suggests 0.8-1 for Asian hair, 0.65-0.9 for Caucasian hair and 0.5-0.65 for African hair. The major axis is aligned with the curve normal, which geometry nodes can create, and legacy particle hair does not support it. — [Blender manual (de/dev)](https://docs.blender.org/manual/de/dev/render/shader_nodes/shader/hair_principled.html)
- Huang lobes in Blender. Reflection modulates the first bounce ("Keep this 1.0 for physical correctness"). Transmission "picks up the color of the pigment inside the hair". Secondary Reflection is light transmitted in, reflected off the back of the fiber and transmitted out (TRT). Random Color varies melanin per strand by 0-100%. Random Roughness does the same for roughness. — [Blender manual (latest)](https://docs.blender.org/manual/en/latest/render/shader_nodes/shader/hair_principled.html)
- [foundational] Khungurn and Marschner, "Azimuthal Scattering from Elliptical Hair Fibers" (TOG 2017): even small deviations from circularity cause important changes in the scattering distribution. — [ACM DL](https://dl.acm.org/doi/10.1145/2998578)
- [foundational] Marschner et al. 2003 model the fiber as a transparent elliptical cylinder with an absorbing interior and tilted cuticle scales (the R/TT/TRT lobes). — [Stanford page](https://graphics.stanford.edu/papers/hair/)
- [foundational] Yan et al. 2015, "Physically-accurate fur reflectance": a double-cylinder fiber model with an outer cortex/cuticle and an inner medulla. This is the basis for fur-specific shaders. — [ACM DL](https://dl.acm.org/doi/10.1145/2816795.2818080)
- MaterialX v1.39.2 (January 2025) added a Chiang Hair BSDF, contributed by NVIDIA. It enables "cross-platform, customizable hair shading models in MaterialX and OpenUSD". — [ASWF blog](https://www.aswf.io/blog/materialx-v1-39-2-now-publicly-available/); [CGWorld](https://cgworld.jp/flashnews/202501-MaterialX1392.html)
- OpenPBR does not cover hair. Its technical companion says renderer-specific shaders or bespoke networks may be needed for specialized cases such as hair. OpenPBR 1.1 is the current version as of August 2025. — [OpenPBR: Novel Features and Implementation Details (arXiv 2512.23696)](https://arxiv.org/html/2512.23696v2)
- Houdini VEX has a `chiang` BSDF (since 19.0) and a `chiang_fur` BSDF (since 20.0). — [SideFX chiang](https://www.sidefx.com/docs/houdini/vex/functions/chiang.html); [SideFX chiang_fur](https://www.sidefx.com/docs/houdini/vex/functions/chiang_fur.html)
- Arnold 7.4.4.0 (12 Nov 2025) added `scattering_mode` to standard_hair:
  - approximate: the previous behavior, which treats each hair as a flat surface.
  - accurate: models each hair as a cylinder, which looks more realistic in close-ups but is noisier and roughly 10-30% slower.
  - adaptive: accurate for close hairs, approximate for distant ones.
  — [Arnold what's new card](https://help.autodesk.com/cloudhelp/ENU/AR-Core/files/ac-whats-new-cards/ac_wn_7440_hair_scatter_mode_html.html)
- Arnold 7.4.4.0 also added an OSL closure `chiang_hair_bsdf` that matches standard_hair in accurate mode. — [Arnold 7.4.4.0 release notes](https://help.autodesk.com/cloudhelp/ENU/AR-Core/files/ac-release-notes/ac-rn-7x/arnold_core_7440_html.html)
- Arnold for Maya 5.4.2.1 added GPU support for standard_hair `extra_depth`, which renders blond or bright hair accurately without raising GI_specular_depth. — [Maya 2025 What's New](https://help.autodesk.com/cloudhelp/2025/ENU/Maya-WhatsNew/files/GUID-BB2F51CF-DDF0-4D0C-9683-B5B4D62D8BFF.htm)

### Inferences
- An elliptical cross-section needs two per-strand channels, an aspect ratio and a major-axis orientation (a per-point normal/frame). GroomLab already computes strand frames for braids and cards, so it could export a "curve normal" that Blender Huang can use directly. Other renderers would ignore it, but it costs little. A "Cross-section" look node could set aspect ratio from an ethnicity-style preset (Blender's 0.5-1 ranges), with per-strand random variation and twist along length, since a rotating ellipse gives the glinting of kinky hair.
- Huang's Reflection/Transmission/Secondary Reflection weights correspond roughly to the R/TT/TRT tints that aiStandardHair and V-Ray already expose. A single GroomLab "lobe weights" abstraction could map to all hosts.
- Arnold's adaptive mode suggests a preview tier in GroomLab's viewport: a cheap flat-ribbon approximation far away and a cylindrical evaluation close up, chosen by projected strand width.
- MaterialX Chiang hair is now the most portable way to carry one hair look across USD/Hydra hosts. It fits the "universal .groom / USD plugin" roadmap: GroomLab could write a MaterialX `chiang_bsdf` material next to curves in USD.

### Gaps
- I did not confirm from a primary source how the Huang 2022 paper treats near-field vs far-field, or whether Cycles uses only the far-field form. The paper PDF was not fetched.
- I found no 2022-2026 release-note detail on V-Ray (VRayHairNextMtl) or RenderMan hair shader changes (PxrMarschnerHair / PxrHairColor / Interactive hair). The searches returned only Arnold.
- I found no "energy-conserving hair BSDF" paper from 2022-2026 in these searches. The SIGGRAPH 2023-2025 hair papers list was not reachable through search. Check the ACM DL tables of contents.
- I could not confirm the Unreal Engine 5.x strand shading changes or OpenPBR plans for a hair lobe.

## 2. Real hair color science: melanin, greying, bleaching, fur banding

### Takeaway
Measured data exists for greying prevalence by age and ancestry and for where greying starts on the scalp (temples first, then vertex, occiput last). Fur banding follows a known genetic mechanism: an eumelanin to pheomelanin to eumelanin switch over time in the hair cycle, so it is a function of length position. Sun bleaching is documented qualitatively: pigmented hair lightens, brown hair turns reddish, and moisture speeds it up. Its spatial profile (tips vs roots) is not well measured. These findings map onto per-strand probabilities and along-length melanin curves rather than flat colors.

### Cited Findings
- Panhard, Lozano and Loussouarn (L'Oreal), "Greying of the human hair: a worldwide survey, revisiting the '50' rule of thumb", Br J Dermatol 2012;167(4):865-873. About 4,192 volunteers, with natural color assessed by expert visual evaluation. At age 50, 6-23% of people have at least 50% grey coverage, not the 50% the rule claims: about 6% for Sub-Saharan Africans and about 23% for Northern Europeans. Onset and speed depend on ethnic or geographic origin, and men had significantly more grey than women. I relied on secondary summaries and an industry press report because I did not reach the paper itself. — [Cosmetics Design Europe](https://www.cosmeticsdesign-europe.com/Article/2012/10/02/50-shades-of-grey-Not-quite-as-L-Oreal-study-finds-grey-hair-less-common-than-previously-thought); [OUCI record](https://ouci.dntb.gov.ua/works/4Ngpz8Kl); [Donovan Medical summary](https://donovanmedical.com/hair-blog/50-50-50-rule-revisited)
- Regional greying, from a secondary report: temples lead, then the vertex, and the occiput lags by several years, especially in women. In men over 45, grey clusters at the temples regardless of ethnicity. In women of that age, grey appears on the crown and temples equally. — [Vice](https://www.vice.com/en/article/why-do-men-go-gray-at-the-temples-first/)
- Premature greying clinical series: temporal scalp was most often involved (35.2%), then frontal (18.3%). Another 52-person study found frontal and vertex worst affected. In a Nigerian population, greying was diffuse in 81.5% of cases, and localized cases were frontal in 55.5%. — [PMC5448266](https://pmc.ncbi.nlm.nih.gov/articles/PMC5448266/); [PMC4886586](https://pmc.ncbi.nlm.nih.gov/articles/PMC4886586); [IJRMS](https://www.msjonline.org/index.php/ijrms/article/download/12315/8075)
- Photobleaching:
  - Pigmented hair lightens progressively with solar exposure, and high moisture regain speeds this up. Brown hair develops a reddish tone under sunlight. — [J. Cosmet. Sci. (SCC library)](https://library.scconline.org/v038n03/37); [SCC Hair Melanin Content Photodamage](https://library.scconline.org/is-cacheable/1708708695958/Hair-Melanin-Content-Photodamage.pdf)
  - The sources disagree on which melanin is more UV-sensitive. One study found eumelanin more photoprotective than pheomelanin, and a red-hair study found pheomelanin "much more sensitive" to UV. An older study claimed the opposite. — [SCC library v046n04](https://library.scconline.org/v046n04/2); [SCC Photochemical Alterations II](https://library.scconline.org/is-cacheable/Photochemical-Alterations-Human-Hair-II.pdf)
  - The red-hair study suggests the hair protein absorbs UV before it reaches the melanin granules. — [SCC library](https://library.scconline.org/v046n04/9)
- Agouti banding:
  - The classic agouti hair is black-yellow-black: a yellow subapical band on a dark hair. It comes from a rapid switch from eumelanin to pheomelanin deposition and back within the shaft. — [MGI/JAX, Silvers, Coat Colors of Mice, ch. 2](https://informatics.jax.org/wksilvers/chapters/chapter2-1.shtml)
  - In dogs, the hair-cycle promoter of the ASIP gene drives the banding. — [Coats and Colors](https://coatsandcolors.com/?p=141)
- Wild canid coats:
  - The dorsal coat mixes guard hairs and undercoat that are usually banded. A typical agouti guard hair has a dark tip, a light section, one or more black sections alternating with red, and then a pale base.
  - Banded hairs concentrate on the dorsal torso. The ventral region, cheeks and eyebrows grow unbanded.
  — [The Color of Wild Canines](https://batw.net/borzois.com/coat.color/wild.canines.html); [Coats and Colors](https://coatsandcolors.com/?p=23680)
- [foundational] Melanin parametrization: Blender's Melanin = quantity and Melanin Redness = eumelanin/pheomelanin ratio, with Tint applied afterward as a dye. This is the Chiang 2016 mapping GroomLab already uses. — [Blender manual](https://docs.blender.org/manual/en/latest/render/shader_nodes/shader/hair_principled.html)

### Inferences
- **Greying preset from data.** "Grey fraction" could be a per-strand probability, as the Blend chance mode now uses. It would be driven by a scalp-region weight map: temples > crown/vertex > front > occiput, with a sex toggle (men temple-dominant, women crown and temples). An age slider would map to coverage using the Panhard ranges (6-23% of 50-year-olds above 50% grey). Greys are per strand and whole-strand (the follicle stops pigmenting), apart from the regrow zone at the root. That supports the chance-mode design.
- **Along-length melanin curve.** One "melanin vs length" ramp node could cover several looks:
  - sun bleaching: lighter and redder toward the tips, since the oldest hair has the most exposure
  - root regrowth after dye: natural color for the first N mm, where N = days since dye x about 0.35 mm/day. That growth rate is a common trichology figure but I did not verify it here.
  - balayage/ombre: blended over length
  - grey regrowth under dye: grey roots on strands flagged grey
  - fur agouti bands: a eumelanin to pheomelanin to eumelanin profile with a dark tip
- **Fur zones.** Since banding is dorsal and absent ventrally, a "fur band mask" by body region (dorsal/ventral/face) is a realistic default. Guard vs undercoat could be two strand populations with different band profiles and medulla settings (medulla per Yan 2015).

### Gaps
- No measured statistics turned up for natural per-strand melanin variance within one head, for example the standard deviation of eumelanin across strands. GroomLab's "random hue/value" amount has no data-backed default yet.
- I found no quantitative bleaching profile along the shaft (tip vs root melanin loss vs exposure time).
- No source in these searches gave hair growth rate, dye regrowth timing, or measured balayage/highlight conventions.
- I did not verify the date or origin of the original "50 rule".

## 3. Hair geometry statistics and curl classification

### Takeaway
Real data is available for scalp density and curl type. Density is roughly 115-145 hairs/cm² in East Asian studies, higher (median 330/cm²) in one 2023 African phototrichoscopy study, and lowest at the temples in most cohorts. The L'Oreal / De La Mettrie 2007 study defines a worldwide 8-type curl scale from three measurable descriptors: curve diameter, curl index and number of waves. These numbers can drive "realism" defaults: density maps, curl-type presets and cross-section ellipticity.

### Cited Findings
- De La Mettrie, Saint-Leger, Loussouarn et al. (2007), "Shape Variability and Classification of Human Hair: A Worldwide Approach", Human Biology 79(3). It measured hairs from 1,442 subjects in 18 countries. Three descriptors, curve diameter (CD), curl index (i) and number of waves (w), give eight hair-shape categories, independent of ethnicity. — [BioOne](https://bioone.org/journals/human-biology/volume-79/issue-3/hub.2007.0045/Shape-Variability-and-Classification-of-Human-Hair--A-Worldwide/10.1353/hub.2007.0045.full); [Wayne State Digital Commons](https://digitalcommons.wayne.edu/humbiol/vol79/iss3/1/)
  - Conflict: one secondary source cites 2,449 participants from 22 regions. This may be a later extension, so check the paper before quoting a number. — [UAL research online](https://ualresearchonline.arts.ac.uk/id/eprint/19335/1/ICS-Submission%20file.pdf)
- Ellipticity ranges: aspect ratio 0.8-1 Asian, 0.65-0.9 Caucasian, 0.5-0.65 African. — [Blender manual](https://docs.blender.org/manual/de/dev/render/shader_nodes/shader/hair_principled.html)
- Scalp density by region:

  | Population and method | Finding | Source |
  |---|---|---|
  | Korean men, phototrichogram | 117±13 to 140±16 hairs/cm². Occiput significantly denser than temple; no significant vertex vs occiput difference. | [Ann Dermatol](https://synapse.koreamed.org/articles/1044842) |
  | Han Chinese, image analysis, n=146 | Mean about 143/cm². Vertex and occiput high, temporal low. | [PMC8787532](https://pmc.ncbi.nlm.nih.gov/articles/PMC8787532) |
  | Thai cadaver biopsies | Temporoparietal lowest in hair count, follicular units and terminal:vellus ratio. Authors recommend per-region norms. | [OAE Publishing](https://www.oaepublish.com/articles/2347-9264.2017.52) |
  | US cohorts | Hispanic densest at the vertex; African-descent and Caucasian subjects densest frontally. | [Keratin.com](https://www.keratin.com/hair-biology/comparative-analysis-of-hair-density-in-africans-african-americans) |
  | African population, phototrichoscopy, n=909 (2023) | Range 113-660/cm², median 330 (IQR 301-368), highest frontally. | [Ghana Med J (AJOL)](https://www.ajol.info/index.php/gmj/article/view/329006) |

  Methods differ (hairs vs follicular units, biopsy vs photo), so the numbers do not compare directly.
- Clinical scales exist for regional density grading, such as Olsen's Regional Scalp Hair Density Scale. — [MedicalAlgorithms](https://www.medicalalgorithms.com/regional-scalp-hair-density-scale-rshds-of-olsen-et-al-for-a-patient-with-male-pattern-hair-loss)

### Inferences
- **"Measured head" preset.** It would combine:
  - a density map: temporal lowest, occiput and vertex high; frontal high for African and Caucasian presets
  - a curl type I-VIII picked from the De La Mettrie scale, mapped to GroomLab curl and coil node parameters (curve diameter to coil radius, waves to frequency)
  - a cross-section aspect ratio range
  - a terminal:vellus ratio, where vellus strands are short, thin and light, near the hairline and temples
- A "realism" control could be framed as how far a groom's measured statistics are from a reference population. GroomLab can compute density per cm² over the scalp UV, curvature distributions and length histograms from the groom itself and show them against these ranges, as an analysis overlay rather than a generator.

### Gaps
- I did not reach the exact curl-type cutoffs (CD, i, w per type I-VIII), the Andre Walker system definitions, or Loussouarn's curvature papers.
- Strand diameter variation statistics by region and ethnicity were not found in these searches.
- I found no frizz or flyaway statistics, such as the fraction of strands deviating from the clump or measured frizz indices.

## 4. Rendering efficiency, LOD, ray-traced curves, Gaussian/neural hair

### Takeaway
Real-time hair moved toward (a) hardware ray-traced strands, using NVIDIA's Linear Swept Spheres on RTX 50 / Blackwell with OptiX software fallback, first shipped in Indiana Jones DLC in 2025; (b) LOD that merges strands into thick elliptical "cluster hairs" with a matching BCSDF that includes in-cluster multiple scattering (Huang et al. 2024); and (c) neural reconstruction of undersampled hair G-buffers (SIGGRAPH Asia 2026). Gaussian-splatting hair (2024-2025) is mainly for capture and reconstruction, not authoring. For a groom tool, the useful points are LOD with appearance-preserving width and BCSDF compensation, and cluster-level elliptical shading.

### Cited Findings
- **NVIDIA RTX Hair / Linear Swept Spheres (LSS).**
  - RTX 50-series RT cores add hardware support for a new LSS primitive.
  - OptiX's LSS uses a software fallback on pre-Blackwell GPUs with no code changes.
  - Strands become spheres and tubes rather than triangles.
  — [NVIDIA developer blog](https://developer.nvidia.com/blog/render-path-traced-hair-in-real-time-with-nvidia-geforce-rtx-50-series-gpus); [TechPowerUp deep dive](https://techpowerup.com/review/nvidia-geforce-rtx-50-technical-deep-dive/5.html)
  - Press reports LSS needs about two spheres per segment and about 1/3 the data storage of a triangle representation. — [Tom's Hardware](https://www.tomshardware.com/pc-components/gpus/nvidia-neural-rendering-deep-dive-full-details-on-dlss-4-reflex-2-mega-geometry-and-more)
  - It debuted in Indiana Jones and the Great Circle (2025 DLC), Blackwell-only, with other GPUs keeping the old hair. — [Wccftech](https://wccftech.com/rtx-hair-coming-september-indiana-jones-and-the-great-circle/); [Guru3D](https://www.guru3d.com/story/nvidia-rtx-hair-debuts-in-indiana-jones-and-the-great-circle/)
- **Huang, Zhou, Lin, Zhu, Yan, Wu, "Real-time Level-of-Detail Strand-based Hair Rendering"** (arXiv 2405.10565, May 2024, rev. Oct 2024):
  - It targets the appearance and dynamics discontinuities of card LOD.
  - Each LOD uses thick elliptical hairs that contain multiple strands, with geometric fitting.
  - An elliptical BCSDF accounts for single and multiple scattering within a cluster, from sparse to dense.
  - Up to 3x speedup with appearance similar to full geometry at various distances. Tested on dynamic hair and knits.
  — [arXiv](https://arxiv.org/abs/2405.10565); [PDF](https://sites.cs.ucsb.edu/~lingqi/publications/paper_rthairlod.pdf)
  - A search summary says it extends dual scattering with a local scattering term and notes that the dual-scattering assumption breaks for curly or very light hair. That came from a search snippet, not a full-paper read. — [Moonlight review](https://www.themoonlight.io/review/real-time-level-of-detail-strand-based-hair-rendering)
- **Wu, Shen, Huang, Yan, Montazeri, Wu, "Real-Time Neural Hair G-Buffer Anti-Aliasing"** (arXiv 2605.17557, SIGGRAPH Asia 2026):
  - It reconstructs hair coverage and tangent from heavily undersampled rasterization via neural spatial reconstruction and temporal accumulation, then completes position guided by tangent.
  - The buffers feed physically based deferred hair shading.
  - It beats DLSS and FSR on straight, wavy, afro and ponytail styles.
  — [arXiv](https://arxiv.org/abs/2605.17557)
- [foundational] Dual scattering (Zinke et al. 2008) is still the real-time multiple-scattering baseline. — [Yuksel project page](https://cemyuksel.com/research/dualscattering/)
- [foundational] Moon, Walter, Marschner 2008 store multiple scattering in a spherical-harmonics volume grid, more than 10x faster than earlier accurate methods. — [Cornell](https://www.cs.cornell.edu/~srm/publications/SG08-shhair.html)
- [foundational] Yan et al. 2017 used a fur BSSRDF (dipole diffusion) for global illumination in fur. — [UCSB PDF](https://sites.cs.ucsb.edu/~lingqi/publications/paper_furbssrdf.pdf)
- A recent paper, "Efficient Fur and Hair Multiple Scattering Using Volumetric Approximation", treats dense fur as a highly anisotropic participating medium. It keeps explicit fibers for direct lighting and reports about 7-10x speedups over path tracing. Venue and year were not confirmed: the ACM page returned 403. — [ACM DL 10.1145/3811343](https://dl.acm.org/doi/10.1145/3811343)
- **Gaussian hair:**
  - GaussianHair (Luo et al. 2024) models strands as chains of cylindrical 3D Gaussians, light-aware and relightable. — [Project page](https://haiminluo.github.io/gaussianhair); [arXiv 2402.10483](https://arxiv.org/html/2402.10483v1)
  - Gaussian Haircut (ECCV 2024) uses strand-aligned Gaussians for strand reconstruction from multi-view data. — [MPI](https://ps.is.mpg.de/publications/gaussianhaircut-2024)
  - 3DGStrands (Computers & Graphics 2025) personalizes 3DGS hair from unordered photos. — [URJC](https://burjcdigital.urjc.es/items/cc79a329-1c94-4a88-a9df-aa8eaa966c1d)
  - HairGS (BMVC 2025) merges unconstrained Gaussians into strands. — [BMVC PDF](https://bmva-archive.org.uk/bmvc/2025/assets/papers/Paper_1220/paper.pdf)

### Inferences
- **Viewport.**
  - GroomLab's hair BSDF could add a cluster or thick-strand LOD mode: draw 1 of N strands as an elliptical ribbon fitted to its clump's cross-section and shade it with a "cluster" term, a darker, softer TT/TRT standing in for the multiple scattering of the strands it replaces.
  - The groom already knows clump membership, so the fitting is nearly free.
  - This matches Huang 2024 and fits the game-cards goal (seamless strand to card LOD).
- **Width compensation.** When decimating, widen the kept strands so total projected coverage is preserved (width scale about sqrt(N) or N, depending on whether you preserve area or opacity). Lowering opacity instead and keeping thin strands gives a stochastic-transparency look. Neither formula is sourced here; see Gaps.
- **Hand-off data.** Export clump IDs and a per-strand "LOD priority" (random rank) so renderers and engines can drop strands consistently across distance without popping. It could also feed Arnold-style adaptive accuracy.
- **Gaussian hair.** Mostly relevant as an import path, photo to strands. Possibly also as a web preview representation (splats from guides), but there is no evidence yet that it is better than rasterized strands for authoring.

### Gaps
- I found no source on stochastic transparency / order-independent transparency for hair in 2022-2026, or on a sourced width-compensation formula.
- Unreal Engine 5 Groom LOD and shading details, the Strands2Cards (SIGGRAPH Asia 2025) details, and the Huang 2025 follow-ups (precomputed dual-scattering corrections for textured strands, a seamless LOD framework) were cited in another paper but not confirmed.
- Not checked: Pixar, Weta or Disney tech memos on hair LOD or density.

## 5. Wet, oily, dusty hair and product (gel/hairspray) appearance

### Takeaway
The appearance literature on wet hair is mostly older (2004-2010). A water film makes fibers smooth and mirror-like, total internal reflection at the water-hair interface darkens outer hair, and increased opacity strengthens self-shadowing inside the volume. Wetness also drives clumping, which is the dominant visual cue. I found no 2022-2026 appearance models for oily hair, dust or styling products. This area is open for a groom tool to define its own "wetness" channel that drives both shape (clump) and shading (darker, sharper specular).

### Cited Findings
- [foundational] Ward, Galoppo and Lin (UNC, 2004), "Simulating and Rendering Wet Hair":
  - A thin water film around wet fibers gives a smooth, mirror-like surface, and total internal reflection in the film contributes to darkening.
  - Rendering parameters vary with water amount.
  - Water also changes simulation weight and stiffness and causes clumping.
  — [UNC sketch PDF](https://gamma.cs.unc.edu/HAIRWS/WGL_sketch04.pdf); [UNC paper](https://gamma.cs.unc.edu/HAIRWS/WGL04.pdf)
- [foundational] Gupta and Magnenat-Thalmann (2007), "Interactive rendering of optical effects in wet hair":
  - For outer hair, total internal reflection at the water-hair interface is dominant, handled probabilistically for absorption.
  - For inner hair, water raises opacity and strengthens self-shadow.
  - A parameterized Gaussian-based model handles the increased specularity.
  — [UNIGE archive](https://archive-ouverte.unige.ch/unige:120569); [PDF](https://typeset.io/pdf/interactive-rendering-of-optical-effects-in-wet-hair-29mhknp0zw.pdf)
- [foundational] Silva et al., "Real-Time Rendering of Wet Fur": prior real-time work ignored the clumping seen when fur wets and dries. A wetness texture controls fur shape and color. — [NTU PDF](https://www.cmlab.csie.ntu.edu.tw/~robin/docs/vc10_silva.pdf)
- A multi-scale liquid-hair interaction simulation exists (press coverage). — [80.lv](https://80.lv/articles/a-multi-scale-model-for-simulating-liquid-hair-interactions/)

### Inferences
- **"Wetness" look and shape node.** A per-strand/per-point scalar (from a map, gravity-low regions, or a "drip line" height) would drive four things:
  - clump strength and clump count
  - darkening (more absorption or lower TT, plus a melanin-independent multiplier)
  - lower R roughness and higher R weight (sharper highlight)
  - higher shadow density in the viewport
  A "drying" slider interpolates, with tips drying first as an assumption. This is one channel shared by the shape and look sides, a natural fit for a node-based tool.
- **Product presets as clump + shading pairs.** Gel would be strong, stiff clumps with a wet-ish specular. Hairspray would be mild clump plus slight specular gain. Oily would come from the root down: roots clump and lower roughness, gradient by length. Dust/dirt would be a per-point diffuse/gray coat that raises roughness and lowers saturation, concentrated on the upper (sky-facing) surface of a fur coat. None of these models has a measured source; they are design proposals.

### Gaps
- I found no 2022-2026 papers or measurements on oily/sebum-coated hair appearance, dust or dirt on fur, or hair-product (gel, hairspray, pomade) optical effects.
- I found no measured data on wet hair reflectance (e.g., how much darker in albedo terms) in this pass.
