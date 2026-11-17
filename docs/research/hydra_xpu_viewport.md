# RenderMan XPU lookdev in Maya's viewport through Hydra

Issue #82: a user wants to do lookdev in RenderMan XPU because it's faster. XPU renders no
procedurals, so GroomLab now gets its grooms into XPU through an XGen Interactive Groom
(xgen_xpu_bridge.md). That route works in batch renders, but XGen's curves carry no per-strand data:
no root UVs (so maps on the scalp can't drive the hair) and no per-strand color, so a groom renders
with one look.

This note covers the other route: Maya's Hydra viewport with RenderMan's own Hydra renderer
(hdPrman), drawing GroomLab's strands as USD. It works, with maps and the full per-strand look, but
it needs every user to change Maya's environment, so it was built, tested and then taken out
(2026-11-16). The prototype is in `build/probe/hydra_prototype/` on the dev laptop (not tracked):
`hydra_lookdev.py`, the USD writer and menu patch, the docs as written.

## What works

Tested on the dev laptop with Maya 2026.3, RenderMan for Maya 27.3 and MayaUSD 0.34.5:

- **The pieces exist.** RenderMan for Maya ships hdPrman for Maya Hydra
  (`<RFMTREE>/lib/<year>/usd_plugins_<usd>`, with XPU GPU and CPU loaders). MayaUSD ships Maya
  Hydra itself (`mayaHydra.mll`, in `MayaUSD_<usd>/lib/maya`, not on Maya's plug-in path).
- **GroomLab's USD draws.** `groomlab.export_usd` already writes `BasisCurves` with `primvars:st`
  (each strand's root UV), `displayColor` and the strand channels. In a MayaUSD proxy shape, with
  the viewport's renderer set to `mayaHydraRenderOverride_HdPrmanXpuGpuLoaderRendererPlugin`, the
  hair renders in RenderMan XPU in the viewport.
- **Maps by root UV work.** A USD material (`PxrMarschnerHair` whose color is a `PxrTexture` read
  through `st`) showed a checker painted across the hair by root UV.
- **GroomLab's full look works.** The prototype added the RenderMan procedural's per-strand
  primvars (`rman_*`, `hair_index`, from `renderman::curvePrimvars`) to the USD export, and
  translated the groom's RenderMan shading network into a UsdShade material node for node (a Pxr
  node's type is RenderMan's shader id; non-default params become inputs; connections become
  connections). GroomLab's own hair network then rendered each strand's melanin and redness as RIS
  does.
- **Live edits.** A groom edit rewrote the curves and the viewport followed. A Hypershade value edit
  updated that shader's inputs in place (not confirmed in a capture: see below).

## Why it was taken out: Maya's environment

Two settings must be in place before Maya starts, so GroomLab can't make them once it loads:

1. **`MULTI_USD_VERSION=0.25.5`.** `maya.exe` starts MayaUSD 0.34.5 on its USD 0.24.11 build
   (hard-coded, unless that variable is set). RenderMan 27.3's Hydra plugins for Maya 2026 are built
   for USD 25.05 (`usd_plugins_2505`) and don't load into 24.11.
2. **`PXR_PLUGINPATH_NAME=<RFMTREE>/lib/2026/usd_plugins_2505`.** RenderMan for Maya doesn't put its
   Hydra plugins on USD's plugin path. GroomLab can register them at runtime
   (`Plug.Registry().RegisterPlugins`), and that is enough for the renderer to run. But USD's shader
   registry (Sdr) reads RenderMan's shaders from its discovery plugins once, as USD starts:
   registered later, hdPrman drops every material and the hair renders grey.

Side effects that rule out GroomLab setting them for everyone (from its module file, say):

- The USD switch changes MayaUSD for all of the user's Maya 2026 work.
- With V-Ray for Maya installed, USD 25.05 makes Windows show "Entry Point Not Found" for
  `hdVRay.dll` (V-Ray's Hydra plugin, built for 24.11) at every start-up. It's a native message
  box: clicking OK goes on without V-Ray's viewport renderer, but scripted sessions wait on it.
- The plugin path without the USD switch would load RenderMan's 25.05 plugins into 24.11: the same
  kind of message box, about RenderMan's DLLs.

## Other things learned

- Maya Hydra makes one viewport renderer per Hydra plugin it finds as it loads: RenderMan's must be
  registered first, or Maya Hydra loaded again.
- Maya Hydra also draws GroomLab's own viewport lines, as thick black hair: the groom has to be
  hidden from the viewport (`lodVisibility`) while its USD copy shows.
- The XPU renderer saved in prefs or a scene crashes RenderMan (`rfm.mll`, from hdPrman) when a
  session starts drawing with it before RenderMan for Maya loads; quitting with it on crashed too.
  A real feature has to switch panels back before saving and on exit.
- Maya scans its standard module folder whatever `MAYA_MODULE_PATH` says, so V-Ray's module can't
  be left out that way.
- Captures: screen grabs come back blank with the screen off; playblast captures the Hydra
  viewport. After turning the mode off and on in one session, playblasts came back as Viewport 2.0
  frames, which is why the live Hypershade edit wasn't confirmed in a capture.

## Without env changes on Maya 2024 and 2025 (checked 2026-11-16)

Each Maya's default MayaUSD, against RenderMan 27.3's Hydra plugins (mayapy, no env set):

| Maya | Default MayaUSD | Its USD | RenderMan's Hydra plugins | Match |
| --- | --- | --- | --- | --- |
| 2024 | 0.25.0 | 22.11 | `usd_plugins_2211` | yes |
| 2025 | 0.32.0 | 23.11 | `usd_plugins_2311` | yes |
| 2026 | 0.34.5 (0.24.11 build) | 24.11 | `usd_plugins_2505` | **no** |
| 2027 | 0.37.0 | 25.11 | (no RenderMan for Maya) | — |

So `MULTI_USD_VERSION` is a Maya 2026 problem only. And the shader registry learns RenderMan's
shaders from a runtime registration (`Plug.Registry().RegisterPlugins(<RFMTREE>/lib/<year>/
usd_plugins_<usd>)`) as long as nothing has queried it yet, before or after MayaUSD loads (mayapy,
2024 and 2025). Once something has queried it, it never rescans (the GUI failure on 2026: the
registration came when lookdev was turned on, too late). On 2024 the first query raises a
`Tf.ErrorException` carrying ~1,700 warnings about RenderMan's `uiHints` metadata (USD 22.11
predates it); asked again, the registry answers.

In a Maya 2025 GUI session with fresh prefs (nothing USD loaded at start), with no env set at all:
GroomLab's USD export in a proxy shape, the checker material by root UV, RenderMan XPU in the
viewport through Maya Hydra: rendered, on the CPU and on the GPU. Switching the viewport from one
Hydra renderer to another in the same session crashed Maya 2025 (a USD fatal error in Maya Hydra's
library: Storm to XPU GPU, XPU CPU to RIS); switching from Viewport 2.0 to XPU and back was clean.

Built for Maya 2025 the same day (hydra_lookdev.py, no settings). Maya 2024 left out: with
RenderMan's plugins registered, the first Python call reading USD's shader registry raises the
`uiHints` warnings as an exception in anyone's code (GroomLab's own USD export test did).

What's left for 2024 and 2025:
- Register RenderMan's plugins before anything queries USD's shader registry. With fresh prefs
  nothing USD loads at start; a user whose MayaUSD or LookdevX auto-load may have it queried before
  GroomLab loads. Find what queries it and when (LookdevX's node library is the likely one), and
  register from the earliest point GroomLab gets (its plugin's initialize, or a userSetup.py in its
  module). Detect the late case (the registry lacks `PxrMarschnerHair` after registering) and say
  to restart Maya.
- Never switch between two Hydra renderers in one session on 2025: always go back to Viewport 2.0
  first.
- Maya 2024 in the GUI is not run yet.

## Better ways: what to look into

None of these is checked yet.

1. **Maya 2024 and 2025 need no USD switch** (confirmed above; runtime registration works there). Each ships a single MayaUSD build (MayaUSD 0.25.0
   and 0.32.0), and RenderMan 27.3 has `usd_plugins_2211` and `usd_plugins_2311` for them. If those
   match the builds' USD, only the plugin path is missing. That could then come from GroomLab's own
   module file, in the 2024 and 2025 sections only, with no message boxes. It still has to point at
   RenderMan's folder, so GroomLab's installer would write it, or the module could hold a path that
   only exists when RenderMan is installed. Check: `pxr.Usd.GetVersion()` in each Maya's mayapy
   against the folder name, and whether a plugin path entry that doesn't exist is harmless.
2. **Wait for the versions to line up.** The env change exists only because RenderMan 27.3's Maya
   2026 plugins are built for a USD that isn't MayaUSD's default. A later RenderMan for Maya built
   for MayaUSD's default USD, or a Maya whose default MayaUSD build is 25.05, removes setting 1.
   RenderMan for Maya has no Maya 2027 build yet; check what it targets when it ships.
3. **Ask Pixar for primvars on Interactive Grooms.** RenderMan for Maya's XGen translator already
   reads XGen's `texcoords` and `patchUVs` (they're imported in `rfm.mll`), but writes only `P`,
   `width` and `N`. If it wrote the root UVs as a primvar, the XGen route (no env change, batch
   renders too) would get maps. A feature request to Pixar.
4. **Ask Pixar for procedurals in XPU.** GroomLab's RenderMan procedural already writes every
   primvar. XPU support for `Procedural2` would make all of this unnecessary.
5. **Shader discovery without the env.** If USD's shader registry could be refreshed after RenderMan's
   plugins are registered at runtime, setting 2 would go away. Look for a supported way in USD 25.05
   (Sdr has no public reload). Alternatively, a USD material that doesn't need Sdr to know the
   shaders (hdPrman reading `info:id` without discovery).
6. **Approximate variation in the XGen route.** Split a groom into several XGen descriptions by
   color (clusters of similar strand colors, a material each), so XPU shows some of GroomLab's
   variation and maps' effect, baked at the time of the split. No env change; batch renders too.

## Probe files

On the dev laptop, in `build/probe/render_hooks`: `run_hydra.ps1` and `hydra_gui.py` (the first
spike: USD, a checker material, Storm and XPU captures), `run_lookdev.ps1` and `lookdev_gui.py`
(the built module in a GUI session), `mods_novray/` (a module folder without V-Ray; it didn't keep
V-Ray out).
