# Developing GroomLab

The handbook for anyone (or any model) picking the code up cold: how it is put together, how
to build, test and install it, how to find out why Maya crashed or hung, and the rules the code
follows. [reference/](reference/README.md) is the feature reference; the [user guide](USER_GUIDE.md)
is the artist's walkthrough; [SPEC.md](SPEC.md) and [PILOT.md](PILOT.md) hold the decisions and
the 1.0 gate.

## 1. What this is, in one screen

GroomLab is a hair and fur system for Maya 2022 to 2027 (2022 tested, the others built from their
devkits: "Other Maya versions" in section 2): hierarchical guides (brushed, generated as
children inside their parents' tubes) feeding a procedural node graph (scatter, interpolate,
clump, noise, curl, braids, cards...). Everything that computes hair lives in a Maya-free C++17
core; Maya, Python, the CLI and Arnold are thin hosts over it.

The repository's root follows that split: `engine/` is the system (`core`, `gpu`, `io` for
Alembic and groom scenes, `python`, `cli`), `hosts/` the products built on it (`maya` with its
Arnold translator, `arnold` for the procedural, `app`, `blender` for the Blender extension: the
helper process and the GPL add-on, docs/BLENDER.md), `tests/` mirrors the two
(`tests/engine/...`, `tests/hosts/...`, with the shared `tests/data`), `tools/` packages and
benchmarks, `web/` is the website, `docs/` these handbooks; `cmake/` and `third_party/` support
the build. The build folders mirror the source (`hosts/maya/RelWithDebInfo/GroomLab.mll`).

| Layer | Where | What it holds |
| --- | --- | --- |
| Core | `engine/core/` (`groomlab_core` static lib) | `GroomSession` (`groom_session.cpp`, also called `Groom`: one groom kept evaluated for any host, see [SESSION.md](SESSION.md)), painted scalp maps (`scalp_maps.cpp`), `StrandSet` (copy-on-write arrays), `ScalpMesh`, `GuideHierarchy`/`GuideSet`, the graph evaluator and cache (`graph.cpp`), the editors' edits of a graph's JSON every host makes (`graph_edit.cpp`: names, seeds, connections and their checks, a node's ports), operators (`engine/core/src/operators/*`), brushes (`brush.cpp`), the interactive tools and strokes for any host (`stroke.cpp`, `stroke_tools.cpp`: see [SESSION.md](SESSION.md)), guide picking and selection (`guide_selection.cpp`), painted maps on the scalp (`scalp_paint.cpp`), Catmull-Clark subdivision of the scalp and colliders (`subdivision.cpp`: the plans per cage topology, what match render refines by), HairColor's color patterns for every host (`color_patterns.cpp`: bands along a strand, vane space and the feather patterns drawn in it, the vane normals, the iridescent tint the shaders match), cards (`cards.cpp`; feathers' cards `card_feathers.cpp`, shared textures, layers and the atlas `card_atlas.cpp`, the bakes `card_bake.cpp`, the levels of detail every host offers and what a Cards node cards `card_presets.cpp`, a bake in memory, its files' names and layout and the `cardMaps` record `card_files.cpp`), rachis meshes and feather groups for engines (`feathers.cpp`), strands as meshes and budgets by strand ID for the utilities (`strand_geometry.cpp`), correctives, flow, parts, the viewport's buffers for any host (`display.cpp`, `DisplayBuilder`, see [SESSION.md](SESSION.md)), meshes' sparse signed distance fields on the world's grid, shared across evaluations (`distance_field.cpp`: Collide's, the guide simulation's, Interpolate's keepClearance: coarser cells, a wider band, the field's answers alone, and as tables for the GPU's walk), scales (`scales.cpp`: the built-in plates, `ScaleMeshBuilder` bending the Instance node's scales onto the skin through those fields, kept per scale, with their per-scale shader data: what the viewport, Arnold and the exports read), scales baked into skin maps (`scale_maps.cpp`), the guide simulation (`simulation.cpp`: the position-based rod solver, its water (buoyancy, water drag, a current) and per-point wetness, and each Simulate node's frames, which the session steps and keeps: see [SESSION.md](SESSION.md) Simulation; the node `operators/simulate_op.cpp`), following the scalp (`follow_scalp.cpp`: rest evaluation carried root by root), paths along the scalp (`surface_paths.cpp`: the rest pose welded into a graph, shared by its rest surface hash, and fronts from guide roots: distances along the skin and the turns a direction takes round folds, Interpolate's frame transfer and `followSurface`), groom files, USD writer, glTF writer (`gltf_writer.cpp`) and compressed PNGs (`png.cpp`) for game exports, path resolution (`paths.cpp`). |
| GPU | `engine/gpu/` (`groomlab_gpu` static lib, Windows) | The GPU look preview's engine, host-free: which run of nodes ending a graph the GPU can take (Interpolate, Length, Clump, Noise, Curl, Width, Part, Gravity, Flyaway, Collide...; a Cull, Split or Mirror as a run's first stage, the nodes up its input a run of their own copied into its layout) and with what params, mapped values and kept operator work (`look_chain.h`), the compute engine on a device the host gives (`look_engine.h`: one run algorithm, DirectX 11 and OpenGL backends), what every host's preview shares (`look_preview.h`: planning a drag's or a stroke's run, arming the stroke, the base the engine holds), the kernels (`shaders/look.slang`, compiled by Slang at build time). |
| Maya plugin | `hosts/maya/src/` (`GroomLab.mll`) | `groomLabShape` (`groom_shape.cpp`: plugs and dirty tracking into its `GroomSession`), Viewport 2.0 drawing (`groom_draw_override*.cpp`: uploads the core's display buffers, keeps shaders and textures), the brush and select tools (`brush_tool.cpp`, `select_tool.cpp`: adapters of the core's tools, Maya's views read by `tool_view.cpp`), the camera solve and the Reference Flow tool (`reference_tool.cpp`), commands (`*_cmd.cpp`; `sim_cmd.cpp`: `groomLabSimulate`), Maya-to-core conversion (`maya_inputs.cpp`, also linked by the Arnold translator), crash logging (`crash_log.cpp`). |
| Python | `hosts/maya/scripts/groomlab/` | The artist API, `groomlab.<name>` (`__init__.py` imports it from its modules and lists them: `groom.py`, `guides.py`, `painting.py` (maps), `params.py` (graph, controls, keys), `cards.py`, `game.py`, `files.py` (.groom), `paths.py` and more; a module imports from the ones listed before it, any later one inside the function that needs it), reference photos as cameras (`reference.py`), the graph model (`graph.py`: its edits the core's `GraphEdit`), sims, and the UI (`ui/`: Groom Tools palette and its Tools, Paint, Sculpt and Reference tabs, Graph Editor, Attribute Editor template, menu, hotkeys, themes: their `ui.font`, Inter, is loaded from the module's `fonts/` (the source tree's `hosts/app/fonts` from a checkout) and named only in the style sheet of GroomLab's own windows and the Graph Editor's painters, so Maya's windows keep Maya's font). The UI reaches Qt only through `ui/qt.py` (`from groomlab.ui.qt import QtCore, QtGui, QtWidgets`): PySide2 in Maya 2022 to 2024, PySide6 in 2025 and later, and the few names that differ (`qt.QAction`, `qt.QActionGroup`, `qt.QShortcut`, `qt.exec_(dialog)`, `qt.event_pos(event)`, `qt.to_int(flags)`, `qt.shiboken`, `qt.QtSvg()`); never import PySide2 or PySide6 directly. |
| Bindings | `engine/python/` | `groomlab_core` pybind11 module for each Maya's Python (3.7 in Maya 2022, 3.9 in 2023, 3.10 in 2024, 3.11 in 2025 and 2026, 3.13 in 2027): tests and pipelines, and the plugin's own Python (`hosts/maya/scripts/groomlab` imports it for what every host shares, so nothing the app also needs is written twice; it installs beside the plugin, on the module's `PYTHONPATH`, and the mayapy and GUI tests put the build's on theirs); `GroomSession` (`bind_session.cpp`) keeps a groom evaluated from buffers; strokes on it from screen or world points (`bind_stroke.cpp`); `DisplayBuilder` (`bind_display.cpp`) gives the viewport's buffers as buffer views. |
| UI definitions | `engine/ui/*.json` | The UI structure every host reads, plain JSON: the menus, the Groom Tools' tabs, sections and Output strip (the app: Render and Export in the menu bar corner), the lists' bars and right-click menus, option lists, the marking menu, the Graph Editor's menus and colors, as action ids with labels, tips, order, shortcut ids, conditions and host masks. Each host binds the ids (Maya: `ui/definitions.py` and the windows' `ACTIONS` tables); the layout stays in code. Installed beside the Maya module's `themes/`, embedded in the app, copied into the Blender extension. See [UI_DEFINITIONS.md](UI_DEFINITIONS.md). |
| Arnold | `hosts/maya/arnold/`, `hosts/arnold/` | MtoA translator (renders the shape as curves) and the `groomlab_procedural` that renders groom files without Maya. |
| RenderMan | `hosts/renderman/` | `groomlab_renderman`, a Procedural2 and old-style Procedural DSO that renders groom files in prman without Maya (RenderMan procedural, section 2); the look's primvars from `renderman_look.h`. `hosts/maya/scripts/groomlab/renderman.py`: RenderMan for Maya's RenderManProcedural node for it (experimental). |
| Alembic | `engine/io/` | Curves/mesh readers and writers, `ImportCurves`, groom scenes with caches. |
| CLI | `engine/cli/` | `info`, `eval`, `abc-info` on groom files, headless; `mcp`: the MCP server for AI agents (`mcp_server.cpp` the protocol, `mcp_tools.cpp` the tools, `preview_render.cpp` the CPU preview; tested by `cli.mcp_session`). |
| App | `hosts/app/` (`GroomLab.exe`, `GROOMLAB_BUILD_APP`) | The standalone viewer without Maya: Qt 6 Widgets, the viewport on Qt's RHI (`renderer.cpp`), `GroomDocument` over a `GroomScene` (edits as whole graphs, undo, save, export). See "Standalone app" in section 2. |
| Tests | `tests/` | GoogleTest core suites (`tests/engine/core`, goldens in `tests/data/golden`), the GPU kernels (`tests/engine/gpu`), Alembic (`tests/engine/io`), Python binding tests (`tests/engine/python`), mayapy suites and GUI-session suites (`tests/hosts/maya`), the app's tests (`tests/hosts/app`, built with the app). |

Data a groom keeps on the `groomLabShape` node (all stored strings, all read by compute):

| Attribute | Content |
| --- | --- |
| `graph` | The node graph as JSON (`{}` = the default graph). Schema: `engine/core/include/groomlab/graph_io.h`. It holds the Graph Editor's layout too (each node's `"ui": {"x", "y"}`, the graph's `"backdrops"`), so positions travel with the groom file to the app and back; moving a node rewrites only that, outside undo (`GraphModel.save_layout`). `graphUi`, where older scenes kept the layout, is read when the graph has none. A connection is `{"from", "to", "input"}` plus `"output"` when it reads another output of `from` than its first (a Map node's `r`, `g`, `b`, `a`). |
| `guideData` | The guide hierarchy as JSON: groups, guides, per-guide layers (`attributes`), per-point layers. |
| `partData`, `flowData`, `correctiveData` | Parts, flow arrows, corrective (shot sculpt) layers. |
| `scalpMaps` | Maps painted on the scalp that are not guide layers: `{"name": {"kind": "vertices", "values": [...]}, "name2": {"kind": "texture", "texture": "groomlab/shot/masks/hair/name2.png"}}` (`engine/core/include/groomlab/scalp_maps.h`). A color map adds `"channels": 4`: a vertex map's `values` hold r g b a per vertex, a texture map's PNG is RGBA; without `channels` a map is gray. |
| `cardMaps` | The last card bake: `Color=<path>;Coverage=<path>;AO=<path>;Atlas=<path>;Strands=<node>:<hash>,<node>:<hash>;Bake=<n>`, paths relative to the scene (made and read by `engine/core/include/groomlab/card_files.h`, as the app's session record is: `formatCardMaps`, `cardMapsPrefix`, `CardBakePaths`); `Strands` stamps what each Cards node's cards were made from (the stale check, per node). The atlas JSON records each card's `node`, `strands`, `cell`, `mirrored`, `textureWidth`, `flyaway`, `feather` and `colors` (the mean color, root to tip, of the strands the cell was baked from: what a card taking the cell is tinted from). |
| `scalpMapDisplay`, `scalpMapChannel`, `strandColor*`, `guideColor*`, `display*` | Viewport-only settings (no re-evaluation); `scalpMapChannel` is what of a color map the scalp shows: `rgb` or one channel. |
| `brushTick` | An integer the brush and map tools bump to force a re-evaluation (previews, rewritten files). |
| `groomFile`, `renderMode` | The groom file the groom is linked to (imported from, saved to, rendered from), stored portable; `renderMode` `groomFile` has Arnold render that file instead of the node. |
| `ctl_*` | Dynamic keyable attributes: one per Control node, per keyable Expression variable and per keyed param (a node's `paramControls`: a double, a long or a bool; a vector's double3 with X Y Z children, a color's float3 with R G B). Compute reads every numeric one by name (`readControls`, a vector's children one by one). |

## 2. Build, test, install

Windows, Maya 2022, Visual Studio 2022, CMake 3.25+. The presets live in `CMakePresets.json`:
the Ninja ones for everyday work (`ninja-maya2022`: the plugin, the core, every test and the GUI
sessions; `ninja-app`: the same and the standalone app; `ninja-core`; `ninja-maya2023` to
`ninja-maya2027`: another Maya version's plugin from its devkit, see "Other Maya versions" below), the Visual Studio ones
(`win-maya2022`, `win-core`: the IDE and CI) and Linux's (`linux-core`, `linux-asan`,
`linux-coverage`, `linux-maya2022`: built from Windows in WSL with one command, see "Linux builds
(WSL)" below). Ninja builds several times faster (see "Faster builds" below for the numbers
and the rest of the setup); it needs the Visual Studio x64 developer environment, which
`tools/dev/vsenv.ps1` brings to any shell:

```bash
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev/vsenv.ps1 cmake --preset ninja-maya2022
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev/vsenv.ps1 cmake --build --preset ninja-maya2022
```

(In PowerShell, `. tools/dev/vsenv.ps1` once brings it into the session; then `cmake --build
--preset ninja-maya2022` alone.) The build folder is `build/ninja-maya2022`, laid out as the
Visual Studio one (`hosts/maya/RelWithDebInfo/GroomLab.mll`, `tests/RelWithDebInfo/...`), so every
command below takes either folder. To try a change in Maya or the app without the tests, the Python
module and the Blender helper, build only what the install needs, in the same folders:
`--build --preset ninja-maya2022-plugin` (then the install below) or `--build --preset ninja-app-exe`.
Build the whole preset before running ctest. The Visual Studio presets work as before:

```bash
cmake --preset win-maya2022
cmake --build build/win-maya2022 --config RelWithDebInfo -- -m
```

`-- -m` lets MSBuild build targets in parallel (and `/MP`, which the build sets, a target's files).
A single target: `--target GroomLab` (the
plugin), `--target groomlab_tests` (core tests). The first configure fetches pybind11,
GoogleTest, nlohmann/json, Imath and Alembic (a few minutes), and Slang 2026.19's release
(`engine/gpu/CMakeLists.txt`, pinned by hash): a build tool only, whose `slangc` compiles the GPU
preview's kernels (`engine/gpu/shaders/look.slang`, one source) for every GPU API; nothing of it ships or
runs in Maya (set `GROOMLAB_SLANGC` to a `slangc` of your own to configure offline). Each is
fetched once per machine: the sources land in `%USERPROFILE%/groomlab-deps/fetch/<name>-<version>`
(`GROOMLAB_FETCH_CACHE`; empty fetches into each build folder) and every later configure, in any
checkout or worktree, builds from there (each build folder still builds them under its own `_deps`).

**GPU kernels (Slang).** `look.slang` is written in Slang's HLSL-like language. The build runs
`slangc` per kernel and embeds what it writes (`engine/gpu/cmake/embed_kernel.cmake`): DXBC (`cs_5_0`)
for DirectX 11, which slangc gets from Windows' own `d3dcompiler_47` (no Windows SDK tool), and
GLSL 4.50 compute text for OpenGL, which the driver compiles. d3dcompiler is slow (minutes over
`ClearanceCS`; slangc's HLSL for it, a second), so each DXBC kernel is kept in
`%USERPROFILE%/groomlab-deps/kernels` keyed by the HLSL slangc writes for it (`GROOMLAB_KERNEL_CACHE`;
empty for none): an edit to `look.slang` compiles again only the kernels whose code it changed, every
build folder and worktree shares them, and a kernel's header is rewritten only when its bytes change.
The hosts start the driver's compile as a groom first
draws (`prepareGLLookKernels`), in the background where the driver can (`KHR_parallel_shader_compile`),
and draw a drag or stroke on the CPU until its kernels are ready (`LookEngine::kernelsReady`,
`glLookKernelsReady`) rather than wait. Each compiled program is kept (`glGetProgramBinary`) under
`%LOCALAPPDATA%/GroomLab/glcache`, a file a kernel keyed by its source and the driver
(`GROOMLAB_GL_CACHE`: another folder, `0` for none), so a later session loads them in milliseconds
even where the driver's own cache misses (seconds), and an update compiles again only the kernels it
changed. `GROOMLAB_GL_SALT` (any text) makes every kernel's source differ, so the driver's cache
misses: a cold compile measured. A kernel's compile time grows with each copy of a large function
the driver inlines (the field's push in Collide and Interpolate's clearance walk: one call site
each) and with loops around them. The ctest `gpu.kernel_targets` compiles every
kernel to SPIR-V (Vulkan) and Metal as well, unused for now, so the source keeps serving the hosts
to come. Each resource names its DirectX register and its `vk::binding` (OpenGL's shader storage
and uniform block bindings); Metal has one buffer table, so its check lets Slang lay it out
(`GROOMLAB_SLANG_LAYOUT`). Loops are unrolled by Slang (`[ForceUnroll]`: OpenGL drivers take no
unroll attribute), and fxc behind the DXBC fails on a UAV store Slang's loop form lets it flatten
(`[branch]` on it). Check a change with `groomlab_gpu_tests`, every case on both APIs.

**Tests run in three tiers**, ctest labels; every test is in one of them. Run them from the
build folder (`cd build/win-maya2022`):

| Tier | When | Command |
| --- | --- | --- |
| `quick`: core GoogleTest, the Python bindings, the mayapy suites (no windows, about a minute) | after every change | `ctest -C RelWithDebInfo -L quick -j 8 --output-on-failure` |
| `gui`: one Maya window running every GUI suite (about a minute) | when the UI (`hosts/maya/scripts/groomlab/ui`), the viewport or draw override, the brush, select or reference tools, or selection changed; before packaging or pushing | `ctest -C RelWithDebInfo -L gui --output-on-failure` |
| `arnold`: MtoA export and the procedural (about a minute) | when the Arnold translator, procedural or export, or the color nodes, RenderData and shading changed; before packaging or pushing | `ctest -C RelWithDebInfo -L arnold --output-on-failure` |
| `renderman`: the RenderMan procedural in prman (about 15 s; built only with RenderMan Pro Server) | when the RenderMan procedural, `renderman_look`, or the color nodes changed; before packaging or pushing | `ctest -C RelWithDebInfo -L renderman --output-on-failure` |
| all (about 2.5 minutes) | before packaging (`tools/package_release.py`) and before pushing | `ctest -C RelWithDebInfo -j 8 --output-on-failure` |

`-j 8` runs the windowless tests side by side; the GUI session, Arnold and the `Budget.*` and
`Performance.*` timings always run alone (`RUN_SERIAL`), and the long mayapy suites start first
(`COST`). The tier takes about two minutes whatever the `-j`: its timing tests run one at a time
(about 70 s) and `maya.grooming` alone takes 30 to 45 s, which is why the area labels below exist.
With the Ninja presets, `ctest --preset ninja-maya2022 -L quick` runs from the checkout's
root with the right configuration, `--output-on-failure` and `-j 12` already set.

**Area labels.** Every test also carries the label of what it tests: `core` (the core's
GoogleTest), `io` (Alembic, groom scenes, simulation caches), `gpu` (the GPU kernels), `python`
(the bindings), `maya` (the mayapy suites, and the GUI and Arnold sessions), `app` (the standalone
app's tests), `blender` (groomlab-helper driven as the Blender add-on drives it, under Blender's own
Python: `blender.helper`; the extension validated and the add-on in a headless Blender with a
throwaway profile: `blender.extension`, `blender.addon`, `blender.addon_free`,
`blender.addon_render`). While working on a change run the areas it touched, and the tiers once before
merging:

```bash
python tools/dev/test_changed.py            # the labels of every file changed since main
ctest --preset ninja-maya2022 -L core       # or name the area yourself
```

`test_changed.py` maps each changed path to its labels (the core's sources run `core` and
`python`, its headers every `quick` test, the Maya UI and tools `maya` and the `gui` session, the
Arnold translator and procedural `arnold`...) and runs ctest on them; `--print` shows the commands
only, `--build` names another build folder.

**Core tests** (fast, no Maya) are also one executable:

```bash
build/win-maya2022/tests/RelWithDebInfo/groomlab_tests.exe
```

`--gtest_filter="ScalpPaint.*:Maps.*"` narrows them. After an intended change of any
operator's output, re-record goldens with `GROOMLAB_UPDATE_GOLDEN=1` in the environment and
review the diff of `tests/data/golden/*.json`; every operator type must have a golden case
(`test_golden.cpp` checks that). Strand counts and IDs must match exactly; sampled points, widths
and channels within 1e-4, so the files hold on every compiler. Stored formats change in place
(no migrations, section 6): the fixtures under `tests/data/compat/v1` (written by
`tools/make_compat_fixtures.py`) are loaded by the compatibility tests to catch a format change
nobody meant to make; update them with an intended one.

**What the full run covers:** the core unit tests; the `groomlab_core` Python tests under
`mayapy`; `mayapy` standalone tests that load `GroomLab.mll` and groom a sphere; an Arnold test
that exports a 100k-strand groom through MtoA to `.ass` and checks the curves, deformation blur
keys, and a groom-file `.ass` rendered by `kick` through the procedural; Alembic round trips,
through the core reader and through Maya's `AbcImport`; groom files evaluated by `groomlab-cli`
against Maya's own export, and USD checked with MayaUSD's `pxr`; the Python graph API and presets;
and the 1.0 hardening (golden output for every operator, the same output with one thread and with
many, the v1 compatibility fixtures and bad-input sweeps, errors on the node, referenced grooms in
a moved project, and the performance budgets below). The Arnold translator builds when
`MTOA_LOCATION` points at an MtoA install (the Windows presets set it); turn it off with
`-DGROOMLAB_BUILD_ARNOLD=OFF`.

**Performance budgets** are measured by the `Budget.*` tests, `maya.hardening` and the GUI
session's `viewport` suite (`maya.gui`) on a 24-core machine. CI reports them without failing.

| Budget | Target | Measured |
| --- | --- | --- |
| Full graph, 100k strands x 16 points | < 100 ms | ~80 ms |
| One parameter tweak at preview density | < 33 ms | ~5 ms |
| Brush dab on 2k guides + interpolation preview | < 16 ms | ~14 ms (~20 ms with a full look graph) |
| Viewport orbit, 200k preview strands | >= 24 fps | ~120 fps |
| Render expansion, 1M strands x 24 points | < 5 s, < 1.25 GB | ~0.9 s, ~1.1 GB |
| Groom file, graph + 5k guides | < 10 MB | ~6 MB |
| Scene open with 3 grooms | < 3 s added | ~20 ms |

**Coverage.** The `linux-coverage` preset builds the core with gcov. CI's coverage job reports
line coverage of `engine/core/` in its summary; the target is 80%.

**Maya tests** run through ctest, which gives them a `MAYA_APP_DIR` of their own under the build
folder (one per mayapy suite, `tests/maya_app_dirs/<test>`, so the suites running side by side never
write the same prefs; the GUI sessions share `tests/maya_app_dir`, where the plugin folder is
trusted) and points them at the build's plugin. Running `mayapy tests/hosts/maya/test_grooming.py` by hand
loads whatever GroomLab module the user's prefs auto-load (usually the installed, older one)
and tests the wrong build. Suites: `python.groomlab_core`, `maya.plugin`, `maya.graph_editor`,
`maya.pipeline`, `maya.groom_io` (groom files into Maya and the round trips, the app's included
when it is built), `maya.grooming` (most features), `maya.subdivision` (the subdivided scalp: match render, against Maya's smooth mesh, parts and flow across levels), `maya.hardening`, `maya.studio` (studio lighting's rig, with MtoA) (all `quick`), `maya.arnold`
and `maya.studio_render` (`arnold`) and `maya.gui` (`gui`); `ctest -R maya\.grooming` runs one.

**The GUI session** (`maya.gui`, configure with `-DGROOMLAB_GUI_TESTS=ON`) starts one real Maya
window and runs the GUI suites in it one after another (`tests/hosts/maya/gui_runner.py`, harness and
shared helpers in `gui_support.py`): `viewport` (the draw budget), `brush`, `select`, `reference`
and `graph_editor` (`editor_cases.py`, the Graph Editor and Groom Tools widgets), `cached_playback`
(grooms played with Maya's Cached Playback: see Evaluation Manager and Cached Playback), `updates`,
`render` (Render > Maya Hardware 2.0: Nika's groom in the Studio Portrait, the render density, the
anti-aliasing, everything put back; `GROOMLAB_HW2_COMPARE=1` with `GROOMLAB_HW2_OUT=<folder>` makes
a side by side with Arnold instead), then `quit`
(the panels open, what Maya's quit runs, the panels open again for the session's own quit:
see Quitting Maya below). Between suites
the session is put back as it started (new scene, docks and windows closed, scriptJobs killed,
tools, panel, playback and project restored, empty undo queue). It never moves the mouse and does
not need the window to have the focus (events go to the widgets: `hover`, `drag`, `activate`);
tests that count undo steps first run Maya's deferred idle work (`settle_undo`). A suite that has not finished
after five minutes (`GROOMLAB_GUI_SUITE_TIMEOUT`, seconds) fails with the Python stacks in the log and
the session moves on, so a step that never comes does not leave Maya idle until ctest's timeout; the
session also fails when cached playback entered its safe mode, and the suite running fails on a
GroomLab warning that one of its shaders did not compile (`FAILING_WARNINGS`: a stand-in then
draws, which a picture's check may not notice). Its log,
`build/win-maya2022/tests/gui.log`, marks every line with its suite and ends with a summary naming
each failure by suite and test; read it when the session fails (ctest's own output is Maya's
startup chatter). The log's first line names Viewport 2.0's device: fresh prefs give OpenGL Core
Profile, so `maya.gui_dx11` runs the `viewport`, `brush`, `cached_playback`, `render` and `quit` suites again on DirectX 11 (forced with
`MAYA_VP2_DEVICE_OVERRIDE=VirtualDeviceDx11`, log `gui_dx11.log`), which many artists use and
where buffers behave differently (GPU-only vertex buffers cannot be written there). One or a few
suites alone:

```bash
GROOMLAB_GUI_SUITES=brush,viewport ctest -C RelWithDebInfo -R maya.gui --output-on-failure
```

(PowerShell: `$env:GROOMLAB_GUI_SUITES="brush"` first, and remove it after.) `GROOMLAB_GUI_CASES=ctrl_z,set_key`
runs only the editor cases whose names contain those words.

**Preset contact sheets** (how every shipped preset looks in the viewport, for any change of a
preset, an operator's look or the drawing): `ctest --preset ninja-maya2022 -L sheet` runs
`maya.preset_sheet` (OpenGL) and `maya.preset_sheet_dx11` (DirectX 11), the opt-in GUI suite
`tests/hosts/maya/gui_presets.py`. In one Maya window each preset is applied as the Presets menu
applies it (fitted, maps made), with guides groomed first when it reads guides (on the head a
shoulder-length style from a center part, hanging under gravity off the head and neck), and
playblasted from a framed perspective view at the groom's default preview density, on a head
(`head_scalp.obj`), a wolf-sized creature (one mesh, ~170 units) and the head at a tenth of the
size (`small`: the fit must give the same picture); then a labelled sheet per scene.
`GROOMLAB_PRESET_SHEET_DIR` (the folder), `_SCENES` (head,creature,small), `_NAMES` (some
presets), `_TAG` and `_SIZE` (pixels per picture) steer it.

**Card fidelity** (how close a groom's game cards look to its strands in the viewport, for any
change of card placement, bakes or card shading): `tools/bench/card_fidelity/run_fidelity.py run
--out <folder>` (system Python with numpy and Pillow) starts a Maya window of its own on DirectX 11
(`--opengl` for the Core Profile) with a prefs folder of its own (`tests/card_fidelity_app_dir`,
made and trusted before Maya starts: the GUI tests' prefs never see its backgrounds), draws three
test grooms on `tests/data/app/head_scalp.obj` (the long hair of the usual graph, short fur,
ringlets; `--grooms banded,barred` the color patterns' two, agouti fur and barred iridescent
feathers, `results/2026-10-04_color_patterns.md`) from five cameras, strands and hero, mid and low cards with the hair cap, over black
and white (the head a holdout), and writes per view and level of detail the silhouette's size
(`area`, cards over strands; within 5% at every level is the aim), how far cards balloon past the
strands' envelope (`outside`), IoU, see-through, coverage, CIE76 color difference (overall,
blurred, roots/mid/tips) and strand- and clump-scale detail, with a picture grid per groom.
`compare <before> <after>` prints the before/after table. About a minute for all of it.

**Install** into a module folder on `MAYA_MODULE_PATH` (the plugin, scripts, presets, expressions,
themes, icons, the Python module, the Arnold extension and procedural, the CLI):

```bash
cmake --install build/win-maya2022 --config RelWithDebInfo --component GroomLab --prefix "C:/Users/<you>/Documents/maya/modules"
```

Always pass `--component GroomLab`; without it the install also copies Alembic and Imath headers
and libraries. Maya then loads the installed build; a Maya that is open keeps the old plugin until
it restarts. It writes `GroomLab.mod` and, under `GroomLab/`: `plug-ins/GroomLab.mll`,
`scripts/groomlab`, `python/groomlab_core.cp37-win_amd64.pyd`, `presets`, `expressions`, `themes`,
`icons`, `extensions/groomLabTranslator.dll`, `procedurals/groomlab_procedural.dll` and
`bin/groomlab-cli.exe`. Another Maya version's build (`build/ninja-maya2025`...) installs its
binaries under `GroomLab/maya<year>/` (`plug-ins`, `python`, `extensions`) and its own section of
`GroomLab.mod`; the packaged release holds them all. Put the long form of the path on
`MAYA_MODULE_PATH`: Maya's plugin lookup fails on 8.3 short paths like `C:/Users/ABCDEF~1/...`.
TBB comes from the Maya's own (TBB 2020 in Maya 2022 to 2025, oneTBB `tbb12` in 2026 and 2027);
without Maya a system TBB package is used if found, otherwise loops run serially.

**The SDK** (the C API for third-party bridges: `groomlab.dll` with its TBB, the import library,
`groomlab_c.h` / `groomlab.hpp`, a CMake package and the MIT samples; layout in
engine/capi/CMakeLists.txt) is the `SDK` component, into a `GroomLabSDK` folder:

```bash
cmake --install build/ninja-app --config RelWithDebInfo --component SDK --prefix C:/scratch/sdk
cmake -S C:/scratch/sdk/GroomLabSDK/samples -B C:/scratch/sdk-samples && cmake --build C:/scratch/sdk-samples --config Release
```

Its docs are the website's Developers section (web/content/docs: developers, c-api, host-bridge,
groom-format). The function reference in c-api.mdx is generated from groomlab_c.h's comments:
after changing the header run `python tools/dev/capi_reference.py` (ctest `capi.reference_docs`
fails until you do). A change to the .groom format updates groom-format.mdx too.

**Package for someone else** (Maya 2022 to 2027, Windows): one zip with the module, the user
guide and a drag-and-drop installer (`tools/package/`: `install.mel` runs `install_groomlab.py`,
which writes a `GroomLab.mod` pointing at the unzipped folder into the user's modules folder, loads
the running Maya's plugin and sets it to auto-load):

```bash
python tools/package_release.py
```

It writes `dist/GroomLab-<version>-maya-win64.zip` (about 90 MB with all six versions;
`--symbols` adds each plugin's ~150 MB `.pdb` for symbolized crash logs). Build first: it installs
`build/ninja-maya2022` and every `build/ninja-maya<year>` there is (`--maya 2022,2025` picks;
Maya 2022 is always in, as the tested version and the only build with the procedural and the CLI).
The module holds the shared parts once (scripts, presets, icons, themes, expressions,
`procedurals/`, `bin/`, docs), Maya 2022's binaries at its root (`plug-ins/`, `python/`,
`extensions/`: the layout every earlier release had, which their updaters check), each other
version's in `maya<year>/` (`plug-ins/`, `python/`, `extensions/`), and a `GroomLab.mod` with a
`MAYAVERSION` section per version (each build's `hosts/maya/GroomLab.mod`, from
`hosts/maya/module/GroomLab.mod.in`: `plug-ins: maya<year>/plug-ins`, its `PYTHONPATH` and
`MTOA_EXTENSIONS_PATH`). `GroomLab/maya_versions.json` names each version's build, whether it is
tested or beta and whether it has the Arnold translator; the installer reads it (and says when the
running Maya's build is a beta) and so does `publish_release.py`. One zip rather than one per
version: one download and install for every Maya on a machine, one `latest.json`.

**Publish a release** to the downloads bucket (Cloudflare R2, served from
https://downloads.getgroomlab.com): `python tools/publish_release.py` packages and uploads
`maya/GroomLab-<version>-Maya-Windows.zip`, the `maya/GroomLab-Maya-Windows-latest.zip` copy and
the same under `maya/GroomLab-Maya2022-Windows-latest.zip` (what the website's "Download free"
links to, `web/site.config.ts`), and `maya/latest.json` (version, url, sha256, size, date, notes,
maya, mayaVersions, mayaBeta, platform: `mayaVersions` every Maya the zip has a build for,
`mayaBeta` those not yet tested in their Maya, `maya` the tested one, 2022, which releases before
`mayaVersions` read alone).
`--notes "..."` or `--notes-file notes.txt` sets the release notes (a few lines; empty by default)
that GroomLab > Help > Check for Updates shows. `--dry-run` shows what it would upload.
It reads R2_ACCOUNT_ID, R2_ACCESS_KEY_ID and R2_SECRET_ACCESS_KEY (an R2 token for the bucket only;
R2_BUCKET defaults to groomlab-downloads) from the environment: never commit them.

**Updates in Maya** (`hosts/maya/scripts/groomlab/updates.py`, the window `ui/updates_window.py`):
Check for Updates reads `latest.json` (one plain GET, User-Agent `GroomLab/<version>`), compares
semantic versions with the install's `VERSION.txt`, offers the release when the running Maya is in
its `mayaVersions` (saying so when that Maya's build is a beta), and Update Now downloads the versioned zip,
refuses it unless its sha256 matches, unpacks it as a sibling `GroomLab-<version>` folder and
rewrites the user's `GroomLab.mod` to point at it (atomically, the old one kept as `GroomLab.mod.bak`).
The running module's files are never touched (Windows locks the `.mll`); the previous folder goes
on the first start of the new version (`%APPDATA%/GroomLab/updates.json` remembers it, along with
the last startup check: at most one a day, on a thread, a quiet note in the Groom Tools, never a
dialog; optionVar `groomLabCheckForUpdates` 0 turns it off).

**Developer installs are never updated:** only `tools/package_release.py` writes `VERSION.txt`
into the module, so `cmake --install` (into `Documents/maya/modules` or anywhere), the build tree
and the source tree have none, and updates are off there ("Updates are off for this install
(developer build)", no startup check). A module with a `CMakeLists.txt` or `CMakeCache.txt` in a
folder above it counts as a developer build too, and `GROOMLAB_NO_UPDATES=1` turns updates off for
any install. The tests (`maya.updates`, the GUI suite `updates`) run a fake downloads site on
localhost (`tests/hosts/maya/fake_downloads.py`) and never reach the real domain.

**The standalone app's beta** (Windows): `python tools/package_app.py` (after building `ninja-app`)
installs the `App` component (`hosts/app/CMakeLists.txt`: the exe, windeployqt's Qt without
translations, DXC or the TUIO plugin, the offscreen platform, `tbb.dll`, the MSVC runtime app-local
through InstallRequiredSystemLibraries, the presets, `hosts/app/package/`'s README_BETA.txt and
Qt's LGPL notice and texts), checks it against the manifest in the script (required and forbidden
files, every PE import beside it or in Windows, the Qt files identical to the Qt install's), writes
`licenses/QT_THIRD_PARTY.txt` from Qt's SBOM, zips `dist/GroomLab-App-<version>-beta-windows-x64.zip`
and smoke-runs it from a temp folder (PATH of Windows' folders only: open, save, render offscreen,
then the window for a moment; every loaded module must come from the unzipped folder or Windows;
`--no-window` skips the window). ctest's `app.package_manifest` (quick) runs the check alone (`--stage-only`).
`python tools/publish_app.py [--notes ...] [--dry-run]` uploads `app/GroomLab-App-<version>-beta-windows-x64.zip`,
`app/GroomLab-App-windows-latest.zip`, `app/latest.json` (what the app's Help > Check for Updates
reads: `hosts/app/src/update_check.h`) and `app/releases.json`. The three publish scripts share
`tools/downloads_bucket.py` (the signed upload, the release index). The website lists the app and
Blender as Beta once their `latest.json` is published (it reads them when it is built: redeploy after).

**The loop for a change:** build, run the `quick` tier, run `gui` and `arnold` when the change
touches them (the table above), install, commit; the full run before packaging and pushing. Commit messages say what changed and why.

### Open source: the private and the public repository

GroomLab is open source (Apache-2.0; the Blender add-on GPL-3.0-or-later: `LICENSE`, `NOTICE`,
`THIRD_PARTY_NOTICES.md`, `REUSE.toml`). Every package ships `LICENSE`, `NOTICE` and
`THIRD_PARTY_NOTICES.txt` (`groomlab_install_legal` in the top `CMakeLists.txt`; `package_app.py`,
`package_blender.py`, `package_release.py` and `assemble_extension.cmake` check they are there).
New source files start with an `SPDX-License-Identifier` line (`CONTRIBUTING.md`).

Two repositories:

- **maya-hair (private)** is where work happens: every branch, worktree, research harness, probe
  output and internal note. Its history is never published.
- **groomlab (public, github.com/thekesslereffect/groomlab)** gets one commit per release, made
  from a snapshot of the private tree. Its local clone sits beside the private one
  (`Documents/GitHub/groomlab`, `publish_public_repo.py`'s default).

The snapshot: `python tools/dev/make_public_snapshot.py` writes `build/public-snapshot` from
`git archive HEAD`, leaves out what is not public (the website's placeholder hero image, the wolf
renders until the wolf sample ships, the research harness `tools/research/reference_guides`,
`.claude/`, agent tooling, the bench and preset pictures, the publish scripts), strips internal
notes (section 7's AI notes, the commit-trailer note above, the CI comment), and scans the
result: private terms (read from `%USERPROFILE%/groomlab-deps/public-snapshot-terms.txt`, never
committed), local user paths, machine names, secrets, private links, SPDX lines. Any finding
fails it. Add to its `EXCLUDE` list whatever new must stay private.

The release, after the downloads are published: `python tools/dev/publish_public_repo.py`
regenerates the snapshot (refusing on any finding), mirrors it into the clone (files gone from the
snapshot are deleted; the clone's `.git` stays), commits "GroomLab <version>" with that version's
section of What's new as the message, tags `v<version>`, and stops. Review the commit in the
clone, then run it again with `--push`. `--notes-file` replaces the notes; `--version` the version.

**An outside pull request** on the public repository: review and merge it there (or not), then
bring it into the private repository, where the next release will include it again:

```bash
# in the public clone, after the PR is merged (or fetched as a branch)
git format-patch -o ../pr-123 v0.11.0..main          # one .patch per commit since the release
# in the private repository, on a branch from main
git am -3 ../pr-123/*.patch                           # keeps the author and message
```

Paths are the same in both trees, so the patches apply; fix up anything that touched a file the
snapshot left out or changed (rare). After the next release the public `main` and the private tree
agree again.

### Faster builds

What the build does to build fast, none of which changes what it builds (the same sources, flags
that only move debug information, the same install):

- **Ninja** (`ninja-*` presets, Ninja Multi-Config with MSVC). Visual Studio's MSBuild compiles a
  project's files one after another unless told otherwise; Ninja keeps every core busy across all
  targets at once. Its no-op build is under a second. Visual Studio builds now compile with `/MP`.
- **`/Z7` debug information** for the Ninja builds: each object carries its own instead of every
  `cl.exe` writing one shared compiler PDB per target through `mspdbsrv` (the contention behind the
  LNK1201 PDB write errors under a high `-j`). The linker still writes the full `.pdb` beside each
  binary (`hosts/maya/RelWithDebInfo/GroomLab.pdb`), so debuggers, `crash_log`'s symbols and
  `package_release.py --symbols` read what they did. Visual Studio keeps `/Zi`.
- **Precompiled headers** (`GROOMLAB_PCH`, on with MSVC): per target, the standard library and
  the third-party headers its files use most: nlohmann/json, GoogleTest, pybind11, the Maya API, Qt
  (`cmake/GroomLabBuildSpeed.cmake`, `groomlab_precompile`). Never GroomLab's own headers, which
  change all the time. Files that define macros before their includes (`_CRT_SECURE_NO_WARNINGS`,
  `windows.h`'s) skip the PCH (`SKIP_PRECOMPILE_HEADERS`, `tests/CMakeLists.txt`).
- **The fetched dependencies, once per machine** (`GROOMLAB_FETCH_CACHE`,
  `%USERPROFILE%/groomlab-deps/fetch`): a new build folder configures in about 17 s instead of
  100 s, offline.
- **A compiler cache, opt-in** (`GROOMLAB_COMPILER_CACHE=AUTO`, sccache 0.18 through
  `tools/dev/sccache_launcher.py`; `tools/dev/get_sccache.ps1` fetches the pinned, checksummed
  release into `%USERPROFILE%/groomlab-deps/sccache`, or `winget install Mozilla.sccache`). Off by
  default because measured it loses: with MSVC sccache preprocesses every file before looking it
  up, so every file an edit touches compiles about 30% slower, a hit still costs the preprocessing,
  and it cannot cache a file built with a precompiled header (the cache turns them off). It pays
  for builds that are nearly all hits: a fresh worktree seeded from the main checkout's cache
  builds in 70 s instead of 110 s, every later header edit then costing 30 to 40 s more. Each tree
  has its own cache (`build/.sccache`) and server; its root is stripped from the keys, the include
  lists a hit replays are mapped to the tree's own files (so Ninja's dependencies stay right), and
  the paths compiled into the tests and the app (`GROOMLAB_TEST_DATA`, `GROOMLAB_SHIPPED_PRESETS`)
  carry a token of their tree, so a hit never hands one tree's paths to another. Objects that came
  from another tree's cache name that tree's sources in their debug information: rebuild without
  the cache before stepping through a worktree build in a debugger.

Measured on the 12-core Threadripper PRO 5945WX (24 threads), the Maya configuration
(`ninja-maya2022` against `win-maya2022`, both with the GUI tests), medians of two or three runs, the
machine otherwise mostly idle (10 to 25% load from other work):

| | Visual Studio (before) | Ninja + PCH (now) |
| --- | --- | --- |
| configure, new build folder | 100 s | 17 s |
| build from scratch | 421 s | 94 s |
| nothing changed | 3.0 s | 0.8 s |
| a core `.cpp` edited (`flow.cpp`) | 10.0 s | 7.7 s |
| a core header everything includes (`strand_set.h`) | 390 s | 65 s |
| a plugin `.cpp` (`brush_tool.cpp`) | 6.7 s | 3.1 s |
| an app `.cpp` (`node_graph.cpp`, `ninja-app` against the app configuration) | 28.8 s | 8.7 s |
| build from scratch, the app configuration | 489 s | 149 s |
| `ctest -L quick` (862 tests before, 891 after) | 135 s (`-j 8`) | 125 s (`-j 12`) |

The Visual Studio preset gains too (`/MP` and the precompiled headers): 149 s from scratch instead
of 421 s. A sccache build with an empty cache took 130 s, the same edits 30 to 40% longer.

**The loop** with a Ninja preset, in the Visual Studio developer environment (`tools/dev/vsenv.ps1`
runs a command in it; configure first with `... vsenv.ps1 cmake --preset ninja-maya2022`):

```bash
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev/vsenv.ps1 cmake --build --preset ninja-maya2022
python tools/dev/test_changed.py             # the areas the branch touched
ctest --preset ninja-maya2022 -L quick       # the tiers, once, before merging
ctest --preset ninja-maya2022 -L gui
ctest --preset ninja-maya2022 -L arnold
```

Machines are shared: building takes every core (`cmake --build --preset ninja-maya2022 -j 12` takes half). A
core header edit rebuilds nearly everything (about 190 of 210 files include `strand_set.h` and
`math.h`), so the plugin, the module and the tests rebuild together; Ninja's parallelism is what
makes that a minute. Unity builds are left out: three dozen file-local helpers share names across
the test files, and with every core busy a few large unity files would lengthen a rebuild's
critical path rather than shorten it. Cutting `graph.h`'s includes is left out too: its `Value`
variant needs every alternative's type complete wherever a `Value` is made.

### Other Maya versions

A Maya plugin is binary compatible within one Maya version only, so every version gets its own
plugin, `groomlab_core` module and MtoA translator. Maya 2022 builds against the installed Maya
and is the tested version. Maya 2023 to 2027 build from Autodesk's public devkits, with no Maya
installed, and ship as a beta (build-verified) until someone tests them inside that Maya:

| Preset | Devkit | Python (module) | Qt (pen pressure; the UI's PySide) | TBB | MtoA SDK (translator) |
| --- | --- | --- | --- | --- | --- |
| `ninja-maya2022` | the Maya 2022 install | 3.7 (mayapy) | Qt 5.15, PySide2 | TBB 2020 | MtoA 5.0 / Arnold 7.0 (install) |
| `ninja-maya2023` | `maya-devkit-2023` | 3.9 | Qt 5.15, PySide2 | TBB 2020 | MtoA 5.2.1 / Arnold 7.1.3 |
| `ninja-maya2024` | `maya-devkit-2024` | 3.10 | Qt 5.15, PySide2 | TBB 2020 | MtoA 5.3.0 / Arnold 7.2.0 |
| `ninja-maya2025` | `maya-devkit-2025` | 3.11 | Qt 6.5, PySide6 | TBB 2020 | MtoA 5.4.0 / Arnold 7.3.0 |
| `ninja-maya2026` | `maya-devkit-2026` | 3.11 | Qt 6.5, PySide6 | oneTBB (`tbb12`) | MtoA 5.6.1.1 / Arnold 7.5.1 |
| `ninja-maya2027` | `maya-devkit-2027` | 3.13 | Qt 6.8, PySide6 | oneTBB (`tbb12`) | MtoA 5.6.1.1 / Arnold 7.5.1 |

Every devkit asks for C++17; Visual Studio 2022 builds them all. Each translator builds against
the oldest MtoA SDK for its Maya at hand, so it loads in that MtoA and every later one. Check an
SDK's `mtoa_api.lib` names that Maya (its imports are in the `OpenMaya2024xxxx`-style namespace of the
Maya it was built for: an installer named for one Maya can hold another's: "MtoA-5.2.1-2024" is
Maya 2023's). Maya 2024's is MtoA 5.3.0, the one Maya 2024 shipped with. The Arnold procedural needs Arnold alone: built once, by `ninja-maya2022` against
Arnold 7.0, it loads in every Arnold 7 and ships with every version (`GROOMLAB_BUILD_PROCEDURAL`
is off in the devkit presets, as are the tests, the CLI, the C API and the Blender helper, which
do not depend on the Maya version).

**What a devkit build needs**, all outside the repository, in `%USERPROFILE%/groomlab-deps/sdks`:

- **The devkit**, unzipped into `maya-devkit-<year>/` (its `devkitBase` folder's contents: `include`,
  `lib`, `cmake`...), from
  `https://autodesk-adn-transfer.s3-us-west-2.amazonaws.com/ADN+Extranet/M%26E/Maya/devkit+<year>/Autodesk_Maya_<year>_DEVKIT_Windows.zip`
  (no login; 90 MB for 2023, up to 600 MB for 2027). Its `devkit` folder (the samples) can go.
  Maya 2025 and later keep Qt in a `Qt.zip`: the build unpacks the headers and import libraries
  it needs from it, or reads them from a `Qt` folder unpacked beside it (`Qt/include/QtCore`,
  `QtGui`, `QtWidgets`, `Qt/lib/Qt6Core.lib`, `Qt6Gui.lib`, `Qt6Widgets.lib`), after which the zip can go.
- **A CPython of the devkit's Python version**: a devkit has Python's headers (`include/Python3x`)
  but no import library and no interpreter. `GROOMLAB_PYTHON_ROOT` names a CPython of the same
  minor version: the python.org NuGet package's `tools` folder, unzipped into `python-<version>/`
  (`https://api.nuget.org/v3-flatcontainer/python/<version>/python.<version>.nupkg`: 3.9.7,
  3.10.8, 3.11.4, 3.11.9, 3.13.9, each the devkit's own), or an installed CPython. The module
  compiles against the devkit's headers and links `python3x.dll` by name, Maya's own.
- **The MtoA SDK** (headers, `ai.lib`, `mtoa_api.lib`) in `mtoa-<version>-maya<year>/`, extracted
  from that MtoA's installer (`include/arnold`, `include/mtoa`, `lib`).

Then build as any preset (the presets name those folders; `-D` overrides any of them):

```bash
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev/vsenv.ps1 cmake --preset ninja-maya2025
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev/vsenv.ps1 cmake --build --preset ninja-maya2025
```

A build folder holds about 3 GB after a build (most of it objects, precompiled headers and
`.pdb`); `tools/package_release.py` needs only the `.mll`, the `.pyd` and the translator. The
CMake side: `cmake/FindMaya.cmake` reads a devkit as an install (`include`, `lib`), the module's
year comes from `MAYA_API_VERSION` (`GROOMLAB_MAYA_YEAR`, `hosts/maya/CMakeLists.txt`, which also
picks the install folders and the `.mod` section), `engine/python/CMakeLists.txt` finds the
devkit's Python headers and `GROOMLAB_PYTHON_ROOT`, and `cmake/GroomLabTBB.cmake` links the
devkit's `tbb.lib` or `tbb12.lib` (a devkit has no TBB DLL: Maya loads its own).

**Checking a build without its Maya.** What can be checked without Maya, and what the beta label
stands on:

- It builds and links: the `.mll`'s and the translator's Maya imports are all in that version's
  API namespace (`dumpbin /imports`: `OpenMaya20250000`...), and `dumpbin /dependents` shows only
  that Maya's DLLs (`Foundation`, `OpenMaya*`, `Qt5*` or `Qt6*`, `tbb` or `tbb12`, `python3x`,
  `ai`, `mtoa_api`) besides Windows and the MSVC runtime.
- The `groomlab_core` module under a CPython of that version: `tests/engine/python` (put the
  module's folder on `sys.path` and that Maya's TBB DLL on `os.add_dll_directory`: TBB 2020's
  `tbb.dll` from a Maya 2022 install, oneTBB's `tbb12.dll` from `pip install tbb`).
- The UI under that Maya's PySide, offscreen: `tests/hosts/maya/qt_compat_check.py` imports every
  module of the `groomlab` package and builds, shows, paints and drives the Graph Editor (the
  default graph, every node's parameters, keys, pan and zoom), the Groom Tools, the updates
  and expression windows, the ramp, sliders, dialogs and menus, with a stand-in `maya`
  (every command answers empty). The sliders are fill fields (`widgets.SoftSlider`):
  `fill_field_check.py` (run by this check and by `maya.ui_scaling`) paints, types, drags and
  nudges them and checks the names inside them in the Groom Tools and the Graph Editor; `--shots
  <folder>` saves PNGs of them. Under a venv of that Python with PySide from pip (PySide2
  5.15.2.1 for 3.9 and 3.10; PySide6-Essentials 6.5.3 for 3.11, 6.8.3 for 3.13: the versions
  Maya 2025 to 2027 ship):

```bash
python tests/hosts/maya/qt_compat_check.py --api PySide6 --core build/ninja-maya2025/engine/python/RelWithDebInfo --dll-dir "C:/Program Files/Autodesk/Maya2022/bin"
```

What only that Maya can show: the plugin loading and its nodes, commands, Viewport 2.0 drawing
and tools working in it; the UI docked in Maya's own windows; the `.mod`'s per-version sections
picked as written; the translator inside that MtoA. Those wait for a tester (or a machine) with
that Maya.

### RenderMan procedural

`hosts/renderman/` builds `groomlab_renderman.dll`, the counterpart of the Arnold procedural: prman
evaluates a groom file at render time, without Maya. It builds when the configure finds RenderMan
Pro Server's SDK: `GROOMLAB_RMANTREE` (default: the `RMANTREE` environment variable, else
`C:/Program Files/Pixar/RenderManProServer-27.3` when it is there; empty: none),
`-DGROOMLAB_BUILD_RENDERMAN=OFF` to leave it out. Like the Arnold procedural it links the core
statically and only `ninja-maya2022` builds it (the other versions' presets turn it off); it links
libprman (the Ri calls), and nothing of Pixar's is in the repository or the package, so nothing
goes into THIRD_PARTY_NOTICES. It installs with the module in `GroomLab/procedurals/renderman/`
(with `tbb.dll`, delay-loaded as the Arnold procedural's, and `groom.rib`, the example scene);
`tools/package_release.py` says when the package has it. Tested with RenderMan 27.3 (the
non-commercial license): `ctest --preset ninja-maya2022 -L renderman` (`renderman.procedural`,
`tests/hosts/renderman/test_renderman.py`, about 15 s), and the core's mapping in
`RenderManLook.*` (quick).

**The API.** A RenderMan Procedural2 `DynamicLoad` DSO, RenderMan's supported way (24 to 27) to
make geometry at render time from a plugin: prman calls its `Subdivide2` with the RIB's typed
parameters, and its `Bound` when the bounder is `DSOBound` (the groom is evaluated once: Bound keeps
the curves for the Subdivide2 of the same parameters). It emits one `RiCurves`: linear,
nonperiodic, `P` and `width` (diameters) per point, as Arnold's curves; with motion keys a
`MotionBegin` block of them. (RixSceneGraph, the alternative in the headers, is a preview API
Pixar says will change.) The tokens are kept for the process's life: prman can read a parameter
list's tokens after `RiCurvesV` returns (freed ones crashed it now and then at the end of a render).

```
Option "searchpath" "string procedural" ["C:/.../GroomLab/procedurals/renderman:@"]
Procedural2 "DynamicLoad" "DSOBound"
    "string dsoname" ["groomlab_renderman"]
    "string filename" ["C:/shots/010/hair.groom"]
    "float frame" [12]
    "float motionStart" [0] "float motionEnd" [0.5] "int motionKeys" [2]
```

**The older API too.** The DSO also exports `ConvertParameters`, `Subdivide` and `Free`, RenderMan's
older `Procedural "DynamicLoad"` API. RenderMan 27 still runs it: RixSceneGraph's `Procedural::Define`
takes `DynamicLoad` with no bounder, and Pixar's own AlembicProcPrim uses it. RenderMan for Maya's
generic RenderManProcedural node drives this API. `ConvertParameters` gets one data string of
`-key value` pairs and returns the parsed `Params`. `Subdivide` makes and emits the same curves and
primvars as `Subdivide2`, motion keys included. `Free` deletes the `Params`. The bound comes from the
call (there is no `Bound` call).

```
Procedural "DynamicLoad" ["groomlab_renderman" "-file 'C:/shots/010/hair.groom' -frame 12 -density 0.5"]
    [-100 100 -100 100 -100 100]
```

- **Keys:** the Procedural2 parameters' names, with `-file` (or `-filename`) for `filename`.
- **Case:** keys match in any case.
- **Quoting:** a value with spaces goes in double or single quotes. Nothing is escaped inside
  quotes, so Windows paths keep their backslashes. In a RIB string, a double quote is `\"`.
- **`-debug`:** alone it means 1, or it takes 0 or 1.
- **Unknown keys:** a warning, and the key is skipped with its value. A value doesn't count when
  it's the next key.
- **Bad values:** a value that isn't a number, or a key with no value, is warned about and ignored.
- **Procedural2:** a Procedural2 call's `string data` parameter is parsed the same way. The typed
  parameters are read after it, so they win.

Parameters (`groomlab_renderman.cpp`'s header): `filename`, `frame`, `fps` (0: the file's),
`density` (0-1, by stable strand ID), `widthScale`, `lengthScale` (`render_overrides.h`),
`motionStart`/`motionEnd` (frames relative to `frame`, the MotionBegin times: the scene's `Shutter`
in frames) and `motionKeys` (a still groom keeps one key), `searchPaths` (folders separated by
`;` where the groom's relative paths resolve after its own folder and search paths: RenderMan hands
a DSO one string), `name` (the curves' identifier) and `debug` (1: the counts and every primvar
with its count, printed). Not yet: instances and scales (a warning), LOD and camera culling (they
need the render camera), the per-strand iridescence normal.

**Primvars** (`engine/core/include/groomlab/renderman_look.h`, `renderman::curvePrimvars`, named for
PxrPrimvar): `strand_id` (uniform int), `hair_index` (uniform float, PxrHairColor's
`hairIndexPrimvar`: PxrHairColor reads the index as a float and needs it non-zero), `st` and
`scalp_st` (uniform float[2], the root's UV), every strand channel as a uniform float of its name
(`melanin`, `melaninRedness`, `clumpId`, `colorR/G/B`, `specular`, `roughness`, `tilt`...,
as Arnold's user data), `hair_color` (uniform color, vertex with per-point colors), `opacity`,
`wetness`, `iridescence` (vertex float), and the look in RenderMan's terms: `rman_melanin`,
`rman_redness` (PxrHairColor's pigment), `rman_dye` (constant color), `rman_color` (the RGB look),
`rman_rgbShare` (0 the pigment's look, 1 the RGB's, per strand), `rman_specularGainR`,
`rman_coneAngleR`, `rman_coneAngleTT`, `rman_coneAngleTRT` (PxrMarschnerHair's lobes, per strand,
per point where wet), `rman_specularOffset` (constant: minus HairColor's tilt; the shader param
takes no connection, so the network sets it).

**The shading network** (the procedural makes no shaders: `hosts/renderman/examples/groom.rib`):
PxrPrimvar nodes read `rman_melanin`, `rman_redness` and `rman_dye` into a PxrHairColor in Physical
mode (its own randomness and strays off: GroomLab varied each strand already, and its greys are in
each strand's melanin), `rman_color` into a second PxrHairColor in Artistic mode (the RGB look:
maps, bands, tips, feather patterns, what Arnold renders as `aiStandardHair`'s base color), three
PxrMix by `rman_rgbShare` into PxrMarschnerHair's `diffuseColor`, `specularColorTT` and
`specularColorTRT`; `rman_specularGainR` and the cone angles into its gains and cones,
`specularGainTT` 2, `specularGainTRT` 1.2, `specularOffset` -3 (minus the tilt), `specularIor`
1.55; `opacity` (default 1) into `presence`. The mapping (`pxrHairColorPigment`,
`pxrMarschnerLobes`): PxrHairColor colors a fiber exp(-33 m^2 x eumelanin) mixed toward
pheomelanin's by redness (the colors mixed; GroomLab's 6.6 m^2 mixes the absorptions), so its
melanin is GroomLab's times 0.68 + 0.12 x redness and its redness the mix nearest GroomLab's color;
the R gain is 1.75 x the specular tint; the cones are `hairLongitudinalWidth` of Arnold's roughness
in degrees (TT half, TRT twice). Fitted against `aiStandardHair` on the head groom at five pigments
with `tests/data/app/renderman_compare.py` beside `arnold_compare.py` (the same camera, lights and
groom; prman's rendering space is linear Rec.709, the comparison's `--linear-srgb`): within about
10% under a light from the camera's side; under the key light blonde renders 15-30% darker and a
light RGB look 35-45%, PxrMarschnerHair carrying less light through many hairs than
`aiStandardHair`. Standalone prman applies no OCIO transform to PxrHairColor (checked: its output
is the formula's to five digits); prman's 8-bit drivers write sRGB.

```bash
python tests/data/app/renderman_compare.py --groom head.groom --scalp tests/data/app/head_scalp.obj \
    --camera "camera eye 32 4 42 target 0 -3 0 fov 32" --out renderman.png \
    --procedural build/ninja-maya2022/hosts/renderman/procedurals/renderman/RelWithDebInfo
```

PxrDistantLight's intensity is its disk's radiance (an Arnold distant light's is irradiance: the
script divides by the disk's solid angle) and it shines down its +Z; PxrDomeLight reads `.tex`
(`txmake -envlatl`) and its lat-long is Z-up.

#### RenderMan for Maya (experimental)

**Status:** needs RenderMan for Maya 27 on Maya 2024 to 2026, and is **not yet verified in Maya**
(this machine has only Maya 2022). `hosts/maya/scripts/groomlab/renderman.py` renders a groom
through RfM's own generic `RenderManProcedural` node, so no GroomLab translator is needed in RfM.

**What RfM has** (read from `RenderManForMaya-27.3`):
- **The node's params** (`nodes/RenderManProcedural.json`): `__dsoname` (the plugin), `data` (the
  string for `ConvertParameters`), `frame` (float, Maya time), and the `preShapeScript` (MEL) and
  `preShapePythonScript` scripts. Each runs before RfM translates the node and can edit `data`.
- **Translation:** it happens in `rfm.mll`, not in Python. `__dsoname`, `data` and `frame` are
  RixSceneGraph's predefined `k_dsoname`, `k_data` and `k_frame`. RenderMan for Blender's procedural
  likewise sets `__dsoname`, `data` and `__bound` on a `DynamicLoad` scene-graph procedural, which
  is the older API. That API passes the DSO only the data string, so **the `frame` param never
  reaches the plugin**.
- **Where the frame comes from:** the frame goes in `data` as `-frame`, as with Pixar's
  AlembicProcPrim. If RfM ever makes a Procedural2 call instead, `Subdivide2` would read `data` and
  the typed `frame` too.
- **What the pre-shape script sees:** `rfm.mll` sets `rfm2.api.strings` vars before its pre-shape
  scripts: `node`, `shape` and `shapepath`, readable with `get_var`. RfM also expands `<tokens>` in
  string params (`<frame>`, `<f4>`, `<shape.frame>`; `rfm2/utils/string_expr.py`). Whether it
  expands `data` isn't verified, so the script writes literal values.

**The helper:**
- **`add_renderman_procedural(groom, density, width_scale, length_scale, debug, shader)`** makes
  `<groom>_rman|<groom>_rmanShape`, sets `__dsoname` to `dso_path()`, and connects `time1.outTime`
  to `frame` (RfM's UI). `link_procedural` then:
  - connects the groom's `message` to `groomLabGroom`;
  - adds `groomLabShutterFromGlobals` (on);
  - writes the data string's settings (`-name`, `-density`, the scales, `-debug`);
  - sets the pre-shape Python script to `groomlab.renderman.pre_shape("<the node's UUID>")`, so a
    rename doesn't break it.
  It runs `pre_shape` once so `data` is complete. Without RfM loaded, or without the DSO installed,
  it raises `RuntimeError`.
- **`dso_path()`** looks in this order:
  1. `$GROOMLAB_RENDERMAN_PROCEDURAL` (the file or its folder);
  2. `<module>/procedurals/renderman/groomlab_renderman.dll` (the module these scripts are in);
  3. the module from Maya's `getModulePath`.
  In a source checkout, set the environment variable to
  `build/ninja-maya2022/hosts/renderman/procedurals/renderman/RelWithDebInfo`.
- **`pre_shape(uuid)`** never raises; a problem is a `cmds.warning`. It finds the node from RfM's
  current shape var (when that node has `groomLabGroom`), else from the UUID. Then:
  - **Groom file:** the groom's own groom file when `renderMode` is 1 (Render from Groom File, which
    caches the scalp). Otherwise it calls `export_groom` to
    `asset_folder("renders")/renderman/<asset_name>.<frame_tag>.groom`, one file a frame, so RIB
    written ahead for a farm keeps each frame's.
  - **Data string:** `update_data` sets `-file` and `-frame` (the current time). It also sets the
    shutter from `rmanGlobals` with `shutter()`: `motionBlur` 1, `shutterAngle`/360 frames, opening
    at, centred on or closing at the frame by `shutterTiming`, keys from `motionSamples`.
    `shutter()` assumes RfM's motion times are frame offsets as in RIB (`Shutter 0 0.5`), which
    isn't verified. All other keys stay as the user wrote them.
- **Data-string helpers** (`split_words`, `parse_data`, `quote`, `build_data`, `update_data`)
  mirror the DSO's parser and need no Maya. `renderman.procedural` loads them by path and renders
  their output.
- **`create_hair_network(name, tilt)`** builds `groom.rib`'s network from RfM nodes:
  - PxrPrimvar readers;
  - PxrHairColor in Physical mode and in Artistic mode;
  - three PxrMix by `rman_rgbShare`;
  - PxrMarschnerHair with `specularOffset` = -tilt, from `hair_look`.
  Params are set through `_set`, which handles RfM's string, enum and number attributes. The
  shading group takes `outColor` on `surfaceShader`. If a step fails, it's a warning that points
  here, and the network above (this section's "The shading network") is the reference.
- **Menu:** **GroomLab > Render > RenderMan Procedural (experimental)**. The Render submenu's
  `postMenuCommand` adds the item while `cmds.pluginInfo("RenderMan_for_Maya", loaded=True)` and
  removes it otherwise, so RfM can load after GroomLab. Without RfM, as in Maya 2022, nothing shows
  and nothing changes.

**Tested:**
- **In prman** (`renderman.procedural`): the exact call RfM's node makes, the older API with a
  data string. Counts against `groomlab-cli eval`, primvars and `hair_color`'s rendered color,
  motion keys (a centred shutter's negative values too), paths with spaces in double and single
  quotes, unknown keys, a missing `-file`, `data` on a Procedural2 call, and a data string built by
  the Maya helper (a path with an apostrophe).
- **In Maya 2022 mayapy** (`maya.pipeline`, `RenderManForMayaTest`):
  - the data-string builder and updater, and the DSO's path resolution;
  - nothing made and nothing raised without RfM;
  - the pre-shape script exec'd as RfM runs it, on a network node standing in for
    RenderManProcedural (renamed, so it finds the node by UUID): the per-frame groom file in a
    folder with a space, which `groomlab-cli` evaluates; the user's keys kept; the shutter from a
    stand-in `rmanGlobals`; the groom's own file when it renders from one.

**Not yet verified** (needs Maya 2024+ with RfM 27):
- that RfM runs the pre-shape Python script each frame and in batch;
- that RfM passes `data` as the older API's string;
- the bound RfM gives the procedural (a too-small bound would cull the hair);
- shutter units;
- the RfM node attributes `create_hair_network` sets, and shading-group assignment on a
  RenderManProcedural.

Not done either: LOD and culling (they need the camera), and instances and scales.

### V-Ray scene export

`engine/core vrscene_writer.h` writes a groom as a `.vrscene` (V-Ray's text scene format; no V-Ray
library): `GeomMayaHair` (points, radii, per-point colors, root UVs, opacity as transparency), a
`TexHairSampler` whose `hair_color` feeds `BRDFHair4`'s (VRayHairNextMtl's) dye over melanin 0,
`MtlSingleBRDF` and a `Node`; long lists as `List*Hex("ZIPC...")` (zlib + base64, as V-Ray's
exporters write them); a frame range as `interpolate()` keys. Callers: `groomlab-cli eval -o
x.vrscene` (and the MCP export), `groomLabExportVrscene`/`groomlab.export_vrscene` (the groom's world
matrix on the Node), the Blender helper's `export` format `vrscene`, `groomlab_core.VrsceneWriter`.

Measured with V-Ray Standalone 7 (`plgparams.exe <plugin>` lists any plugin's parameters):
`GeomMayaHair`'s `widths` are radii; `Node` `nsamples=1` turns geometry motion blur off (left out:
V-Ray's default); `LightDirect`'s intensity times pi is its irradiance and it shines down its -Z;
`LightDome`'s "spherical" lat-long is Z-up; V-Ray's license (the trial's VRLService) checks out
headless, `vray -display=0 -autoClose=1`. The shader mapping (`vray::vrayDyeColor`,
`vrayHairBrdf`) was fitted against `aiStandardHair` with `tests/data/app/vray_compare.py` beside
`arnold_compare.py --linear-srgb --lights key` on the head groom at four pigments and three
roughnesses (the numbers in `vrscene_writer.h`). Tests: `VrsceneWriter.*` (groomlab_tests, the
file read back) and `ctest -L vray` (`tests/hosts/vray/test_vray.py`: coverage, per-strand color,
motion blur, and against Arnold in the Maya build). A contact sheet against Arnold:

```bash
python tools/bench/vray/vray_sheet.py --cli build/ninja-app/engine/cli/RelWithDebInfo/groomlab-cli.exe \
    --procedural build/ninja-maya2022/hosts/arnold/procedurals/RelWithDebInfo --out build/probe/vray
```

**Blender (V-Ray for Blender) and the app.** `hosts/blender/addon/groomlab/vray.py`: V-Ray for
Blender renders a mesh object whose `obj.vray.VRayAsset.assetType` is "2" (Scene) as a `VRayScene`
plugin loading `obj.data.vray.VRayScene["filepath"]` at the object's world matrix; the add-on keeps
one such object per groom (role `VRAY`, an empty mesh under the scalp, its local matrix the Y-up to
Z-up turn) and, in `render_pre` when the engine is `VRAY_RENDER_RT`, has the helper write the groom
(`export` format `vrscene`, `units: "scene"`: the points and radii in the scene's unit, since V-Ray
does not scale hair widths by a Node's or VRayScene's transform: measured, the strands render that
many times thicker; `scale`: the scalp's world scale, which the object's transform cancels) for
the frames V-Ray needs (V-Ray animation mode: the range; its motion blur:
a frame more each side), shows that object and hides the hair Curves for the render job
(`render_complete`/`render_cancel` restore both). Test: `blender.addon_vray` (labels blender, vray;
skipped without V-Ray for Blender: the throwaway profile gets the user's `scripts` folder as a
script directory, read only). The app: `hosts/app/src/vray_render.h` (the stage scene around
`GroomDocument::exportVrscene`, vray.exe as a QProcess, its console progress parsed, the V-Ray
Render window), `--render-vray`/`--vray-turntable`; tests `AppVray.*`,
`AppPanels.VrayRenderWindowShowsTheFrame` and `app.vray_render` (labels app, vray).

**V-Ray Standalone from Maya (any version):** `hosts/maya/scripts/groomlab/vray_scene.py` (no Maya:
the scene text around the grooms' files, RenderView, LightRectangle/LightDome/LightDirect,
GeomStaticMesh + BRDFVRayMtl, SettingsLightLinker; vray.exe's command line, image names, progress
and the background `VRayJob`) and `vray_render.py` (reads the Maya scene into it: grooms, meshes,
camera, lights, per-frame keys; `groomlab.render_vray`). Measured: a `LightRectangle` (units 0)
renders its color x intensity as radiance, Arnold's non-normalized quad light's, and shines down its
-Z; V-Ray's `directional` keeps the light's power, so Arnold's spread maps to
`0.78 (1 - spread)^2` (`spread_to_directional`, fitted on a lit floor with kick and vray); vray
writes `<stem>.<frame>.png` for a frame range and `<stem>.png` for one frame; it resolves a relative
`-imgFile` against the scene file's folder (pass absolute paths). The studio portrait renders within
about 15% of Arnold in linear sRGB (MtoA's `.ass` with an OCIO linear-sRGB color manager and the
PNG driver's `color_space` "sRGB": its "" writes linear values). Tests: `vray.scene` (quick: the
scene text, command line and the hook against a stand-in `vray.utils`) and `maya.vray` (label vray:
the studio's scene from Maya, and with vray.exe a small render whose hair is centered, and a
two-frame sequence).

**V-Ray for Maya hook (Maya 2024+, experimental, never run here):** `vray_hook.py`. V-Ray for Maya
supports Maya 2024+ only, so it is written from Chaos's documented API alone (the doc pages are cited
in the module) and every entry point raises `VRayRequired` ("V-Ray for Maya 2024+ required") in Maya
2022. `enable_hook` appends one line to `vraySettings.postTranslatePython` (V-Ray execs that script
after translating); `post_translate` exports each visible groom over V-Ray's motion-blur shutter
(`cam_mbOn`, `cam_mbDuration`, `cam_mbIntervalCenter`, `cam_mbGeomSamples`) and makes a `VRayScene`
plugin (`vray.utils.create`/`findByName`, `filepath`, `use_transform` 0: the file's Node has the
world matrix) and deletes Node plugins V-Ray may have made from the groom shape. `load_vrayscene`
makes a Maya VRayScene node (`FilePath`, `inputTime`, `outMesh`). Read V-Ray for Maya's shipped
scripts (`maya_vray/scripts/vray`) to learn the public API; never copy Chaos code.

### Linux builds (WSL): farms and pipelines

What render farms and pipelines run builds on Linux: the core, the C API (`libgroomlab.so`) and its
samples, `groomlab-cli`, the Python module (`groomlab_core`), the Blender helper, every core, C API,
Alembic and Python test, and the Arnold and RenderMan procedurals when their Linux SDKs are there
(below). Not on Linux yet: the GPU look preview (`engine/gpu`: DirectX 11 and WGL OpenGL, Windows
only, so its tests are not built), the Maya plugin (the `linux-maya2022` preset is there, untried
without a Linux Maya) and the app.

**One command from Windows** (WSL 2 with Ubuntu 22.04):

```bash
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev/linux_build.ps1             # quick tier, dist/linux
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev/linux_build.ps1 -Tests all  # every test
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev/linux_build.ps1 -Tests none -KeepGoing  # build, every error at once
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev/linux_build.ps1 -Preset linux-asan      # AddressSanitizer + UBSan
```

It lists the tree's files with Windows git (tracked and untracked, `.gitignore` respected, so
uncommitted edits go too), copies them with rsync into the WSL filesystem
(`~/groomlab-src/<tree folder>`: building on `/mnt/c` is several times slower; each worktree has its
own copy), and runs `tools/dev/linux_build.sh` there: configure the preset, build with Ninja, ctest
(`-Tests`: a label, `all` or `none`; `-Clean` starts the build folder over). Then it stages the
artifacts, stripped, in `dist/linux/`: `GroomLab/bin/groomlab-cli`, `GroomLab/procedurals/` (when
built), `GroomLabSDK/` (`lib/libgroomlab.so`, headers, CMake package, samples), `python/`
(`groomlab_core.cpython-3xx-x86_64-linux-gnu.so`), `GroomLabBlender/`, each binary with
`libtbb.so.12` beside it and `$ORIGIN` in its RUNPATH (a render node needs nothing installed), the
same as `groomlab-<version>-linux-x86_64.tar.gz` (the archive keeps the executable bits),
`BUILDINFO.txt` (the newest glibc and libstdc++ symbol versions each binary needs) and ctest's
JUnit report. On a Linux machine, `tools/dev/linux_build.sh` does the same without the copy.

**Tools.** CMake 3.25+, Ninja, GCC 9+, TBB (oneTBB) and the Python headers. With root:
`sudo apt install -y ninja-build python3-dev libtbb-dev` and `python3 -m pip install --user
"cmake>=3.24"`. Without: `-Bootstrap` (once) runs `tools/dev/linux_bootstrap.sh`, which puts the
CMake and Ninja release binaries (checksummed) under `~/opt` and unpacks Ubuntu's own `-dev`
packages (`apt-get download`, no install) into `~/opt/sysroot`; `linux_build.sh` finds both. Without
Python headers the bindings are left out. The dependencies' sources are fetched once per machine
(`GROOMLAB_FETCH_CACHE`, on Linux `$GROOMLAB_DEPS/fetch`, else `$XDG_CACHE_HOME/groomlab-deps/fetch`,
else `~/.cache/groomlab-deps/fetch`).

**Presets:** `linux-core` (everything above, RelWithDebInfo), `linux-asan` (the core and the tests
under AddressSanitizer and UBSan, the timings left out: it finds the use-after-free MSVC's heap
hides), `linux-coverage` (gcov), `linux-maya2022`. The `Budget.*` timings report rather than gate in
`linux_build.sh` (`GROOMLAB_ENFORCE_BUDGETS=0`: they are set for the Windows reference machine; set
it to 1 to gate).

**The procedurals' SDKs** are not downloaded by the build (Autodesk's and Pixar's accounts):
- Arnold: the Arnold SDK for Linux of the Arnold the Windows procedural builds against (Maya 2022's
  MtoA 5.0.0, Arnold 7.0.0.3: `Arnold-7.0.0.3-linux.tgz`; one build loads in every Arnold 7),
  unpacked into `~/groomlab-deps/sdks/arnold-linux` in WSL (`include/ai.h`, `bin/libai.so`), or
  anywhere named by `ARNOLD_SDK` / `-DGROOMLAB_ARNOLD_SDK=`. Builds `groomlab_procedural.so` and
  the `arnold.curves` test.
- RenderMan: RenderMan Pro Server 27.3 for Linux, installed in `/opt/pixar/RenderManProServer-27.3`
  or unpacked into `~/groomlab-deps/sdks/renderman-linux` (`include/ri.h`, `lib/libprman.so`,
  `bin/prman`), or named by `RMANTREE`. Builds `groomlab_renderman.so`; the `renderman.procedural`
  test needs prman's license on that machine.

Both export their entry points only (`node_loader`; RenderMan's `PRMANEXPORT` functions): the core,
Alembic and Imath they link statically stay hidden, so they never meet a renderer's own copies.

**Farm compatibility.** Built on Ubuntu 22.04 the binaries need glibc 2.35 and GCC 11's libstdc++
(`BUILDINFO.txt`). For farms on Rocky or Alma Linux 8/9 (the VFX Reference Platform's glibc 2.28),
build in a WSL distribution of that family (e.g. AlmaLinux 8 from the Microsoft Store, with
`gcc-toolset-11`, rsync and python3; not tried yet): `linux_build.ps1 -Distro AlmaLinux-8`.

**Same bits on every machine.** GroomLab's evaluation is deterministic within a platform (thread
counts, order, a file written and read back: `test_determinism.cpp`) and across them: Windows (MSVC)
and Linux (GCC, glibc) make the same strands bit for bit. The float math is compiled as written (no
FMA contraction or fast math, `CMakeLists.txt`), and the transcendental functions, whose float
versions the C runtimes round differently in the last bit (MSVC's and glibc's `sinf`, `expf`,
`powf`...), are the core's own: every call goes through `groomlab/portable_math.h`, which evaluates
them in double precision and rounds (`GROOMLAB_PORTABLE_MATH`, on in every preset; on Windows the
timing tests measured no cost beyond noise). With the C runtime's float functions, 9 of the 39
presets made Windows' bits on Linux. `Determinism.EveryPresetMakesTheRecordedBitsOnEveryPlatform`
hashes every preset's strands part by part against `tests/data/golden/PresetDigests.json` (recorded
by `tools/dev/record_preset_looks.py`, with PresetLooks) and gates on both platforms, as do the
display buffers' digests, the simulation's frame hashes and the card goldens (`test::kRecordedMath`).
New core code calls `pm::sin`, `pm::exp`... rather than `std::`'s, and test fixtures feeding a
bit-exact check do too.

### Standalone app

`hosts/app/` is GroomLab without Maya: a lookdev viewer (and, later, the game-cards tool and the hub
that opens any scalp and exports to every package) over the same core. It is off by default
(`GROOMLAB_BUILD_APP`), so the plugin, the CLI and the tests build without Qt.

**Stack.** Qt 6.8 (LTS; `hosts/app/CMakeLists.txt` asks for 6.8 or later), linked dynamically (LGPL:
Qt's DLLs ship beside the executable, its notices with them), C++17, CMake. The viewport is a
`QRhiWidget` on Qt's RHI (`QRhi`, public since Qt 6.6): Direct3D 11 on Windows by default
(`GROOMLAB_APP_RHI=d3d12|vulkan|opengl|metal|null` picks another), Metal on macOS, OpenGL or
Vulkan on Linux, one renderer for all of them (`hosts/app/src/renderer.cpp`). The shaders are written
once in Qt's RHI GLSL dialect (`hosts/app/shaders/*.vert|frag`) and `qt_add_shaders` compiles them with
`qsb` at build time to SPIR-V, HLSL 5.0, MSL and GLSL, embedded as resources. Nothing
platform-specific sits outside Qt; the app is built and tested on Windows only for now.

**Qt.** Get the kit with [aqtinstall](https://github.com/miurahr/aqtinstall) into a cache
outside the repository (never commit Qt), the pinned version 6.8.3 with the shader tools:

```bash
pip install aqtinstall
aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -m qtshadertools --outputdir "%USERPROFILE%/groomlab-deps/Qt"
```

(macOS: `mac desktop 6.8.3 clang_64`; Linux: `linux desktop 6.8.3 linux_gcc_64`.)

**Configure, build, run.** Point `GROOMLAB_QT_DIR` at the kit (or put it on `CMAKE_PREFIX_PATH`):

```bash
cmake --preset win-maya2022 -B build/wt -DGROOMLAB_BUILD_APP=ON -DGROOMLAB_QT_DIR=%USERPROFILE%/groomlab-deps/Qt/6.8.3/msvc2022_64
cmake --build build/wt --config RelWithDebInfo --target groomlab_app groomlab_app_tests -- -m
build/wt/hosts/app/RelWithDebInfo/GroomLab.exe tests/data/strokes/strokes.groom
```

`windeployqt` runs after the build and puts Qt's DLLs and platform plugins beside
`GroomLab.exe`, so it runs from the build folder.

**The window.** The whole window is the viewport, edge to edge; everything else floats over it
as translucent glass (the owner's mockup, "like Plasticity but Pixar": the Playful theme's
charcoal and primaries, round and friendly). `hosts/app/src/theme.h` holds the colors (the palette,
the UI tones, the graph's and the icons' read from the Maya host's theme files themselves,
`hosts/maya/themes/*.json`, every one embedded as a resource at build time, so a theme edit reaches
both hosts; Playful by default, View > Theme saves another (QSettings `ui/theme`) for the next start,
`--theme` picks one for a run; a glass theme keeps the app's own glass and text tones, a theme with
`"glass": false` (Charcoal) gets solid `ui.panel` panels with a thin `ui.outline` edge and its `ui` text
tones), the fonts (Inter for words and, with tabular digits, numbers: the theme's `ui.font`;
JetBrains Mono for hex colors, expressions and paths; bundled
as resources from google/fonts under the SIL OFL, `THIRD_PARTY_NOTICES.txt`), the brush rail's
order and categories, the layout of the floating pieces over a view of any size (pure geometry,
tested without a window) and the style sheet the ordinary widgets inside the panels wear.
`glass.h` paints what a style sheet cannot: glass panels, round and pill buttons with tinted
icons (`hosts/app/icons/ui_*.svg`, white strokes tinted at paint time), the sliders, the segmented
mode switcher, text with a shadow straight on the viewport, and the floating panel with a title
row. The number field (`ValueSlider`) is every panel's number control, one fill field (the UI
redesign's FillField, Blender's number fields): a rounded rect a row tall (`ui.rowHeight`, the
raised tone, a 1 px edge, `ui.radius`), the caption inside at the left in the dim text and the
number at the right in Inter's tabular digits (`theme::numberFont`), filled from the left by the
value's place in the range with `ui.selection`. No separate label: the Properties rows, the brush
card's form rows, the node panel and the other panels put the name in the field. A drag past 3 px
scrubs relative to the press (the field's width is the range; Shift a tenth as fast, Ctrl steps
of ten; the drag stays in the range); a click opens the inline edit with the number selected
(Enter, Tab or clicking away sets, Escape restores; it takes arithmetic, `parseNumberExpression`:
`0.5*2`, `1/3`, `,` as the decimal point); focused, Enter or F2 types and the arrows nudge a step
(Shift ten), and the wheel steps only a focused field (it never steals a panel's scrolling). A
number typed past the range widens it within the limits (a Maya soft range: the node panel's
ranges fit each param's default). A driven param's field takes `ui.driven` (`setStateColor`). `overlay.h`'s
`OverlayHost` is the central widget: the
viewport fills it, the pieces are its children placed on every resize, and where no piece is the
mouse reaches the viewport. Top-left the menu button (every File, Edit, View, Tools and Help
action, with the shortcuts Ctrl+O/N/S/Shift+S/R/E, Ctrl+Z/Y, F, Tab working from anywhere; Help >
Docs opens the docs folder beside the executable or in the source tree, else the web docs, and
Keyboard Shortcuts and About are floating pages, `Panel::Help`, never dialogs), the logo and
the file name (a yellow dot when edited); with no groom open an empty-state card sits in the
middle (Open groom, New groom on mesh, Start from a preset, up to five recent files from
`QSettings`, an ini in the user's config folder, which a scripted run, a ctest run and the test suite
never touch: `RunSettings` in `main_window.h` gives them a throwaway folder or `GROOMLAB_SETTINGS_DIR`'s,
and every ctest that runs the app sets that under the build folder; a .groom, .obj or .abc dropped anywhere on the
window opens, `fileDropped`) and the brush rail and bar are dimmed and disabled until one is;
top-center the modes (Groom, Look, Cards, Export);
top-right undo, redo and Render (refine mode from a clean slate: the viewport accumulates
frames until the picture converges, the line at the bottom right counting them). Groom mode:
the brush rail at the left (the core's brush catalog in its order, `engine/core/include/groomlab/brush_catalog.h`:
emerald create, blue shape, red trim, yellow paint, then Select among the guide tools, the
same list, names and hints Maya's Groom Tools tiles are built from, so a tool added there shows
in both hosts; the active one filled and named beside it; the buttons shrink from
44 px to 30 so every brush fits a short window; under the mouse the rail widens to the right over
150 ms and names every tool beside its icon, floating over the viewport, which does not move), the
active brush's main settings as one pill
at the bottom (radius, strength, falloff, Sym X, pen pressure, and "..." for the full brush
panel), the stats at the bottom left (strands, the time per dab, the GPU badge), and at the
right the round buttons that open the guide groups, nodes (the list with the parameters), maps
(the painted maps: made, painted, shown), correctives (the shot-sculpt layers), look (the full
material, surfaces and lighting panel) and display panels on demand, one at a time, plus frame
all; under the mouse each button grows its name in a glass pill to its left (`PanelRail`, the same
150 ms; the tooltips stay). Look mode: the look card
at the top right with three tabs (Material: the hair color presets as swatches and the
melanin, redness and roughness sliders, which edit the groom's last HairColor node
(`LookCard::setGroomColor`, `groomColorEdited`, `groomSwatchChosen`: a slider's drag previews
each step and records one undo step at its end, a swatch sets the pigment as one `editGraph`
step, so the color is saved with the groom and undo reaches the card; without a HairColor node
they edit the viewport material and the note under them says "viewport only"), and behind
"More settings" the viewport-only override (tagged so), every aiStandardHair parameter with the
dye and the lobes' tints as color pickers, then the surfaces: the scalp, the props and the
eyes with a preset, a kind and its parameters, the skin's scattering mode, radius, texturing
and mean free paths among them; Lighting: the presets, exposure, each light's azimuth,
elevation, intensity, color and shadows, the environment, its intensity and rotation, the
tone map, the background and its color; Render: the quality, the refine sample count, the
resolution scale and the interactive quality, Adaptive or Full), the refine line at the bottom
right, and the guides hidden (they belong to Groom mode). The Graph and Presets pills sit at the
bottom left in every mode, the Graph pill counting the nodes (after a preset too,
`setNodeCount`). The layout is collision-aware (`theme::layoutOverlay`): the stats sit above
the pills; the brush rail's buttons shrink to fit between the top row and the stats, then its
gaps tighten, and in a short window (1280x720 with the graph up) it takes the whole height
and the stats and pills move to its right; the brush bar moves right of the pills, or above
them, rather than meet them; the floating panels sit left of the right rail, the node panel as
tall as its content within the view.
Cards and Export modes are below. Glass is translucency alone: widgets over a `QRhiWidget`
cannot sample its frame, so there is no background blur. Errors go to stderr and a toast over the viewport, never a dialog; file
dialogs open only on a click (Open, Save as, the export panel's "...").
`--ui-screenshot out.png [--ui-mode groom|look|cards|export] [--ui-graph] [--ui-panel name]
[--ui-look-tab material|lighting|render[,more]] [--ui-node name] [--ui-hover rail|panels|all]
[--ui-preset name] [--ui-menu presets] [--ui-paint-param node.param]` shows the window at `--size`
(high-DPI scaling off, so the image is exactly that big), with the node graph, a panel (`guides`,
`nodes`, `maps`, `correctives`, `look`, `display`, `brush`), a look tab or a node's floating
parameters open, the rails held in their hover state (the names shown), a graph preset applied,
the Presets menu popped up with its Add-ons submenu and the first add-on's Apply / Insert (each
grabbed at once and drawn into the grab, since a popup closes when the window is not the active
one), a param painted
(`MainWindow::paintParam`: the map, the wiring, the Paint brush),
grabs it with its UI after the first frame
(`MainWindow::grabUi`: the viewport's framebuffer with the pieces rendered over it) and quits;
`--display name=value` sets the display options first (`colorMode`, `colorSource`, `scalpMap`,
`guides`, `cardsView`, `widthCoverage`...). The preview starts with no point budget (the Display
panel's: past one every Scatter thins to fit); `--density` and the panel change it.

**Graph presets** are the Maya host's files (`hosts/maya/presets/*.json`), read through the core's
`presets.h` by both hosts: where they are looked up (`GROOMLAB_PRESET_PATH`, the user's folder,
`GROOMLAB_USER_PRESETS` else Maya's `Documents/maya/groomlab/presets`, where both hosts save, then
the shipped folder: beside `GroomLab.exe`, where the build copies them, else the source tree's in a
development build), what the menu shows of each (`about`), how one is fitted to a scalp's size
(lengths and widths with the size, densities with its inverse square, per-length rates with its
inverse, inside compounds and the keyed controls too: `fitPresetJson`, which Maya's `presets.py`
calls through the Python module), the painted maps it requires and the preset a graph makes
(`makePresetJson`). The app's glue is `hosts/app/src/graph_presets.h` (`applyPresetFile`: the maps
made, the graph fitted and set as one undo step, "Undo preset Wet hair"; `savePresetFile`;
`graphAsText` and `pasteGraphText` for Copy Graph and Paste Graph). The Presets pill beside the
Graph pill (and View > Graph Presets) pops the menu: a submenu per category, the user's own after,
Save as Preset (a glass name prompt in the graph, no dialog), Copy Graph, Paste Graph.

**Correctives** (`correctives_panel.h`, the right rail's wave button): the shot-sculpt layers the
groom holds (`correctives.h`), each with its weight and switch, new layers on every frame or fixed
to the timeline's frame with a six-frame fade, and "Sculpt into": with a layer picked the brush
session runs the core's shot sculpting (`BrushSession::setShotSculpt`: strokes reshape the groom as
shown at the frame and record the difference into the layer), so a correction on one frame rides
along with the motion around it. Every edit of the layers is one undo step through
`GroomDocument::editStoredText` (the correctives text, as a stroke's); the parts and flow the
brushes laid are edited the same way from the guides panel (a part removed, every part or the flow
cleared), which also mirrors a root group across X, makes the whole groom symmetric (Maya's Sync
+X -> -X), promotes a group a level and resamples the selected (else every) guide to a segment
count. Keyed weights (Maya's animCurves) stay Maya's. Tools > Transfer Groom to Mesh moves the
groom onto another mesh (`GroomDocument::transferToScalp`: the core's transfer of the guides, parts
and flow, the maps projected by `projectScalpMaps`, which Maya's `groomLabTransfer` projects with
too; the history is dropped with the scalp), Tools > Turntable turns the camera, File > Reload
reads the groom again from its file, every prop loaded beside the groom is a collider by its name
(a `ColliderMesh` node naming it), Page Up and Page Down step the brush level.

**The timeline** (`timeline.h`) is one pill at the bottom (above the brush bar in Groom mode)
shown when the groom animates (`GroomDocument::animated`: a scalp or guide cache, or a graph
that reads the time): to the start, play or pause, to the end, a scrubber, the frame, the
range's first and last frames (the caches' span, `cacheFrameRange`; a hundred frames for a
graph that reads the time alone), the fps, Loop and, when the scalp has a rest pose, Follow
(followScalp, kept in the file). The document holds the time (`setFrame`; `evaluate` is always
at it), the window evaluates and draws each frame the timeline names. With a Simulate node in the
graph (`GroomDocument::simulateNodes`) the pill grows the sim controls: Simulate (Simulate Range
over the timeline's range, `GroomDocument::simulateTo`, on the UI thread with the events between
frames, as an export), Bake (`bakeSimulation`: each node's frames to `<stem>_sim/<stem>_<node>.abc`
beside the groom, read from there on and named in the file when saved, `simCaches`) and Clear
(`invalidateSimulation`: the frames and the bakes dropped); a strip under the scrubber's track
shows the frames cached in memory (emerald) and baked (blue), refreshed after every evaluation.
Playing steps a live node frame by frame as it is shown (GroomScene feeds the scalp cache's
frames on the way to a frame jumped to). Playback runs on the wall
clock (`playback.h`): the clock is read after every drawn frame, so a groom slower than the fps
skips frames rather than slowing down (the stderr line after a play says how many were dropped).
Opening a mesh `.abc` whose points animate makes a new groom with the file as its scalp cache
(`GroomDocument::newOnAlembic`), so the timeline spans its samples. Saved, the file keeps the
cache's path as given.

**Cards mode** (`cards_panel.h`): the Cards node to edit (Add Cards node wires one between the
groom and its output), Strands or Cards in the viewport (with the baked maps, "Maps", after a
bake: the cards drawn as a surface beside the props, `card_preview.h`; flat in the theme's card
color before one), a level of detail (Hero 6 x 2 cards a clump with flyaways, Mid 2 x 1, Low one
flat card a clump: the core's presets, `card_presets.h`, which Maya's `cards_preset` sets too, by
what the node cards, `cardNodeKind`, one undo step; Custom when the sliders moved), the
main settings as sliders (cards per clump, layers, width scale, segments, variations,
max cards, the atlas resolution; each drag one undo step), the stats of the last view (cards,
triangles, textures; "changed since the bake" when stale), Bake maps (resolution, occlusion) and
the baked atlas as a picture. The bake (`GroomDocument::bakeCards`) writes `<stem>_cards/<stem>_
Depth|Coverage|Tangent|Attribute|Color|AO|Normal.tga` beside the groom in Unreal's hair card layout (the
Maya host's names), `_Atlas.json` (the cells rebuilt cards keep: the session takes it through
`setCardMaps`) and `_Preview.png` (color x occlusion, the coverage in alpha: the viewport's map).
Saving names the last bake in the file (`"cardBake"`: the prefix relative to the groom, the
stamp), so Maya's import takes it; opening adopts the bake the file names, with its stamp (a bake
Maya exported has no preview: `writeCardBakePreview` makes one from its maps), else one found at
the default place (no stamp: stale until a bake). Nothing is written over another groom's files
(the folder is the groom's own). A Z-up groom file opens turned Y-up (`convertUpAxis`).

**Export mode** (`export_panel.h`): the targets (Alembic curves, USD curves, Unreal groom with
the primary guides and groom groups, cards as glTF `.glb` or `.gltf` with the maps embedded and
the hair cap, Unity strands with per-point root UVs, positive root widths and a point count), the
frames (the current one, or a range for an animated groom with a step), the density, the target's
own options (Unreal's and Unity's the Maya host's export's, `engine_export.h`: groom groups, the
feathers' shafts, a lighter groom of barbs, feather guides, barb points), the output file (`<stem>_<target>.<ext>` beside the groom by default) and Export,
which runs on the UI thread with the bar and the toast kept up between frames (`GroomDocument`'s
export methods take a progress callback; Cancel stops after the frame being written, leaving a
shorter cache). Every export evaluates each frame through the scene's view (the cached scalp, its
history, the guide poses, followScalp), at the export's density, so a frame exports as the
viewport shows it. FBX is not written: the Maya host writes it through Maya's own exporter, and
a writer of its own would be a dependency; engines take the glTF.

**Parameters and maps.** The node panel (`param_panel.h`, floating beside a selected node or
in the Nodes panel) shows the node's name, its operator's description (as Maya's Attribute
Editor does; nothing without a node) and a Bypass box, then makes an editor per param kind
from the node's spec (`GroomDocument::nodeSpec`: the type's, or for a Compound or an Input the
core's per-node spec, `graph_edit.h`'s `nodeSpecJson`, so a Compound shows the params its
definition exposes, a feather design's; the rows never widen the panel): a check box, a
spin box, a slider with a spin box, a dropdown of the choices, three numbers for a vector, and
(`param_editors.h`) a swatch with a picker for a color (shown in sRGB, stored linear as the
core's "color" vec3 params are), a curve editor for a ramp (the keys dragged, added with a
double click and removed with Delete, smooth or straight segments, presets, drawn as
`ramp.h` evaluates it, a Curve port the graph can wire) and, for a mappable number, the
source the value comes from: a value, a painted map by name (a vertex map as the color set of
its name, a guide layer, a texture map's file), a texture file, a color set of the scalp mesh,
an expression, or a field port on the node (the param in its "paramInputs", what feeds it
shown). A map set on a param is never lost by editing: the value's editor shows only while
the source is a value, and the map set before comes back when its source is chosen again. The
maps panel (`maps_panel.h`) lists the groom's painted maps (`GroomDocument::maps`: the
session's vertex and texture maps, which the file keeps as the scalp's color sets and its
map catalog, and the hierarchy's guide layers; a one-line hint while there are none), makes
(filled with 1.0 by default: a mask that lets everything through until painted), renames and removes them (one undo
step each, the graph's readers following a rename), shows one on the scalp
(`setScalpMapDisplay`: a vertex map's values, a guide layer blended between the guides, a
texture map's file or the brush's live image sampled at the scalp's vertices, since the
viewport draws the map per vertex) and paints the picked one: the Paint brush set on it (the
kind, the value or color and channels painted toward) and the viewport in brush mode. A
texture map's image lives beside the groom as the Maya host keeps them, `masks/<groom
name>/<map>.png` relative to the groom file (a free `<map>_2.png` when that is taken), written
blank when the map is made and again after every stroke, undo and redo
(`GroomDocument::texturePaint` opens it for the core's `StrokeHost`, `applyTextureEdit` puts a
stroke's tiles back); a new texture map needs the groom saved first. The display panel adds
the strand color modes (solid or lit, a color set, an attribute over a range, random, by
clump, by density: the colored modes draw lines, the lookdev ribbons coming back with the
solid one), the guide dots and width, the map shown on the scalp and its channel, and the
card view when the graph has a Cards node.

**The node graph.** `node_graph.h` draws the graph straight over the viewport (Tab or the Graph
pill; Escape hides it): nodes as rounded cards with a header in their category's color
(`playful.json`'s graph section), their ports down the sides in the port type's color, wires as
curves, no canvas behind them, so the groom shows between the nodes. The view pans and zooms on
its own; nodes without a position are laid out by their depth from the sources (a long chain
wraps into blocks of seven columns) and positions the user drags to are kept in the node's JSON
(`"ui": {"x", "y"}`, which the core's load and save carry, `graph_io.h`, and Maya's Graph Editor
reads and writes the same: positions survive Maya, the groom file and the app; a move is not an
undo step). While the graph is shown it owns the mouse (a click on empty space deselects, a
drag there pans); Alt with any button or the wheel still goes to the camera (the events are
forwarded to the viewport). Clicking a node selects it and its parameters open in a floating
panel; dragging from an output port makes a wire (dropped on an input it connects, replacing
what fed it), dragging a fed input picks its wire up (dropped on empty space it disconnects),
a click on a wire selects it, Delete removes the selected wire or node (the output node stays),
B bypasses the selected node, Tab opens the add-node search at the cursor (Enter adds the type,
fed by the selected node when their ports match), F fits the graph, a double click on a node's
header (or F2) renames it (a glass prompt), Ctrl+D duplicates the selected node beside it (its params
copied, its own seed), Ctrl+L tidies the layout (every position dropped and laid out afresh), and
the right button's menu offers the same on a node (Rename, Duplicate, Set as Output, Bypass,
Delete, each with its key) or the canvas (Add Node, Tidy Layout, Frame All). The graph's backdrops (a preset's, Maya's
Graph Editor's: the graph JSON's `"backdrops"`) draw as tinted frames behind the nodes, each
node's header shows the time of its last run (the session's `nodeTimes`), and while the graph is
up the Graph pill shows in every mode with the Presets pill and a round close button beside it
(Escape, the close button or a click on any mode closes it; Tab and the pill toggle it; the stats
move above the pills). Every edit goes through
`GroomDocument::editGraph` (`addNode`, `removeNode`, `connectNodes`, `disconnectInput`,
`renameNode`, `duplicateNode`, `setOutputNode`, `tidyLayout`): the
whole graph before and after as one undo step, the edits themselves the core's (GraphEdit,
`engine/core/include/groomlab/graph_edit.h`: a new node's name, seed and place, the type and cycle checks
of a connection, a node's ports, a rename with its connections and the output; Maya's GraphModel
edits through it too), the session refusing what does not evaluate.

`GroomLab [file.groom | mesh.obj | mesh.abc]
[--graph graph.json] [--density d] [--set node.param=value]... [--save out.groom] [--material
preset] [--lighting preset] [--quality interactive|refine] [--tonemap none|aces|agx]
[--interactive-quality adaptive|full] [--scalp-material preset] [--hide-scalp] [--prop mesh.obj
[--prop-material preset] [--prop-set name=value]...]... [--eye cx,cy,cz,fx,fy,fz,r
[--eye-material preset] [--prop-set
name=value]...]... [--occlusion-rays N] [--orbit yaw pitch] [--dolly f] [--look-at x,y,z]
[--distance d] [--time frame] [--follow-scalp on|off] [--show-cards] [--simulate frame] [--bake-simulation frame]
[--export-alembic out.abc] [--export-usd out.usda] [--export-unreal out.abc] [--export-unity out.abc]
[--export-cards out.glb|out.gltf] [--export-frames a-b[:step]] [--export-density d]
[--points-per-strand N] [--export-scale s] [--cards-resolution N] [--bake-cards dir [--bake-name name]]
[--screenshot out.png [--size WxH] [--frames N]] [--benchmark N]
[--gpu-preview on|off] [--benchmark-drag node.param=from,to[,steps]]... [--benchmark-orbit N]
[--draw-dabs] [--live-benchmark N [--live-mode orbit|hover|stroke|param|param-commit]]`:
a `.groom` opens with its scalp and caches (`GroomScene`), a mesh makes a new groom on it with
the default graph (`--graph` swaps in another, say `tests/data/app/head_graph.json`; `--set`
edits params); `--prop` loads a mesh beside the groom (a scanned head the scalp was cut from:
`--hide-scalp` then, so the two do not fight) shaded by a surface preset (`--prop-material`)
and edited by name (`--prop-set roughness=0.3`, `setSurfaceParam`'s names); `--eye` makes an
eyeball at a center, looking along a direction, of a radius (cm: 1.2); `--look-at` and
`--distance` put the camera on a point (a close-up); a skin prop's scattering is set the
same way (`--prop-set sssMode=screen|preintegrated`, `sssRadius=`, `sssStrength=`,
`sssTexturing=`, `sssMeanFreePath.r=`); `--screenshot` renders the scene
without a window (an offscreen Direct3D 11 device, `offscreen.h`) and quits, a check without
anyone at the window, `--frames N` accumulating N refined frames first; `--benchmark N`
renders N frames the same way and prints the time per frame, with the creation (the groom
opened and evaluated) and the first frame. `--time` goes to a frame,
`--follow-scalp` sets followScalp, `--show-cards` shows the cards, `--simulate N` runs Simulate
Range to frame N and `--bake-simulation N` bakes beside the groom (before the frame is shown); `--export-*` and
`--bake-cards` write the groom without a window (at `--time`, or over `--export-frames`, at
`--export-density`) and quit, the scripted exports the tests run (`--bake-cards dir` writes the
six TGA maps, the atlas and the preview as `<dir>/<name>_*`, `--bake-name` the groom's stem by
default; `--cards-resolution` the atlas size of a bake or a cards export).
`--benchmark-drag node.param=from,to[,steps]` drags
a param as a slider does, each step previewed (the GPU's when `--gpu-preview` is on and takes
it, else the CPU's) and drawn offscreen, then the release committed and drawn, printing the
times to each drawn frame (the GPU waited for); `--draw-dabs` draws every `--stroke` (or
`--stroke-world "x,y,z ..."`: scene points projected with the camera) dab the same way and
prints the press, the dabs and the release to their drawn frames; `--benchmark-orbit N` turns
the camera N frames for the frames per second; `--bypass node` bypasses a node first (the
benchmarks without Collide). `--live-benchmark N` does the same in the real window (one step a
frame: an orbit, the brush hovering, a stroke of the Maya bench's comb on the guides it places
if the groom has none, a param drag or a commit a frame) and prints the time between frames on
screen, the input's handling and the render's CPU, the evaluation and the dab, the GPU's time
(`QSG_RHI_PROFILE=1` turns the window's timestamps on; they slow the frames), the strands shown,
why a stroke stays on the CPU, and after the release what the UI thread spent on it, when a commit
off the UI thread landed and the longest the UI thread went without turning. `GROOMLAB_APP_NO_VSYNC=1`
presents without waiting for the display (a frame's cost past the refresh rate).

**The app's frame budget.** The floating widgets (`glass.h`) sit over the viewport's texture: while
only the viewport changes Qt composes it under them as they were, but once any widget changes Qt
paints all of them again and uploads the window's widgets whole (about 10 ms a frame at 1440p).
So their setters change nothing when given what they show, the overlay lays itself out again only
when a size changed, the stats line follows the frames at most twice a second and once they stop
(`MainWindow::scheduleStats`), and tinted icons are drawn from their SVGs once (`QPixmapCache`).
The shaders are `PRECOMPILE`d (DXBC in the .qsb: no HLSL compiled when the first frame makes its
pipelines). Commits go off the UI thread in the window someone sits at (`MainWindow::setAsyncCommits`,
`GROOMLAB_APP_ASYNC_COMMITS=0` keeps them on it): a stroke's release, a drag's end, a typed value
or a graph edit with the same view as the last evaluation is detached (`GroomSession::detach`,
docs/SESSION.md) onto a `DetachedRunner` and lands once evaluated (adopt, then the evaluation that
finds every result made; the GPU preview plans again, `GpuLookPreview::sessionDocumentSwapped`);
the stroke document's warm goes there too (`detachWarm`). A CPU stroke's dabs after its press go
off the UI thread the same way (`MainWindow::setAsyncDabs`, `GROOMLAB_APP_ASYNC_DABS=0` keeps them
on it): `GroomSession::detachStroke` lends the stroke document to a run with a copy of the tool's
preview guides, the window keeps drawing (the ring, the camera) and the tool keeps taking the mouse;
dabs that come while one evaluates are not queued, the next run takes the stroke as it is when the
last one lands (`adoptStroke`, then the evaluation here that finds every result made, then the
tool's guides back as the preview); the release does not wait for a run (the commit evaluates the
view's document), and what the tool commits is what synchronous dabs commit
(`AppPanels.DabsOffTheUiThreadCommitWhatDabsHereCommit`). A landing commit's hair buffers are built
off the UI thread too (`LookdevRenderer::setBuildAhead`, with the async commits): until built the
frames keep the hair the textures hold, so the landing frame costs the texture writes alone. A live
drag's steps, a GPU stroke's dabs, map, part and flow strokes, playback, another view, a GPU drag or
stroke in flight, and headless or scripted runs evaluate on the UI thread as before; an evaluation
there, a slider's drag starting, exports, card bakes and simulations take a pending dab or commit in
first. On the wolf groom (13.9k strands, 2400x1350, Direct3D 11, `GROOMLAB_APP_NO_VSYNC=1`, the live
benchmark's `stroke`) a CPU stroke's frames went from 33 ms (p95 46) to 1.2 ms (p95 2.8), the hair
updating every 42 ms (a dab's 39 ms run on the worker, its landing 1.4 ms here); 2026-10-11. The
core-only preset works the same
(`win-core`, `GROOMLAB_BUILD_ALEMBIC` must stay on: scalps from caches and the Alembic export
need it).

**Lookdev.** The viewport predicts the Arnold render (`hosts/app/src/lookdev_renderer.cpp`,
`hosts/app/shaders/hair*.frag`, `look.h`). The strands draw as camera-facing ribbons pulled from float
textures in the vertex shader (`hair.vert`: two texels a point, a triangle strip a strand, a
restart between strands; thinner than a pixel they are widened to one and their coverage fades
them, so thin hair neither aliases nor vanishes). The shading is Marschner's R, TT and TRT lobes
in Karis 2016's closed forms, their widths from aiStandardHair's roughness through d'Eon's fits,
the fiber's color from melanin and redness through the eumelanin and pheomelanin spectra exactly
as the core's `melaninColor` has the render show them (the groom's `melanin` / `melaninRedness`
channels per strand, as the Arnold translator reads them; its `colorR/G/B` as Arnold's base
color, per point where the color nodes wrote per-point colors: a third texture, a texel a point, r g b
and the iridescence, made only for strands that carry them, else one texel telling `hair.vert`
to take each strand's; a groom colored by more than its pigment draws its RGB,
`HairLook::colorsByRgb`, as `create_hair_shader` picks; or the material's own values), the
structural color (FeatherColor's iridescence: the fiber's color and the R lobe's tint mixed toward
`iridescentTint` of the node's color, its hue turned by the view's angle to the strand's normal,
a feather's vane's, on both shading paths), Zinke's dual scattering for the light that comes through
the other hairs (its back-scattering amount from the forward attenuation's luminance and its
tint from one pass through the fiber's color, which is what Arnold renders of brown hair
show), Arnold's lobe weights and tints, and Marschner's cosine of the light's angle to the
fiber. Hair-on-hair shadows and the hair's shadow on the scalp come from a deep opacity map per
light (Yuksel and Keyser: the first hair's distance from the light, then four layers of hair
counts behind it, additive; the scalp counts as opaque in them); a map from above gives
ambient occlusion, and the scalp's normal at each root says what the head hides from the
lights that have no map (the environment's). The environment is generated (a studio, a sky, overcast, a sunset:
nothing to license) and prefiltered as spherical harmonics plus eight directional lights by
median cut; key, fill and rim lights come as presets. Everything renders into a linear HDR
multisampled target, then exposure and ACES or AgX. Transparency: the interactive frame draws
the opaque strands with depth, then the thin ones blended, each weighed by the transmittance of
the hair in front of it from a third deep opacity map, the camera's (no sorting, no noise);
in refine mode, once nothing moves, the viewport keeps accumulating frames with a sub-pixel
jitter, stochastic coverage (the exact order-independent transparency in the limit), jittered
shadow taps and all eight environment lights until the image converges (256 frames). The maps
are drawn again only when the hair, the lights or the material change, not when the camera
moves (the camera's own map follows it). On a device without float render targets or texel
fetch in the vertex stage the renderer falls back to the line path. `GROOMLAB_APP_STOCHASTIC=1`
makes every frame stochastic (a check of that estimator alone).

The interactive frame is shaded once per point, not per fragment: a pass over a texture one
texel a point (`hair_lookdev.frag` under `HAIR_POINT_SHADE`, `points.vert`) shades each point
as the ribbon's center for the frame's camera, lights and maps, and the ribbons (`hair.vert`
under `HAIR_GOURAUD`, `hair_gouraud.frag`) fetch that color at their two points and do the
coverage alone. A thin strand's fragments overlap a hundred deep at 1080p and each of them ran
the full Marschner shading before: the frame was 46 ms at 100k strands (186 at 500k, all of it
the thin pass), and is 16 / 35 with the shading bounded by the points. The picture differs
from the per-fragment one by a mean of 1 / 255 (the shading interpolated along a centimeter
of strand instead of evaluated at the fragment); the refine frames are untouched, so the idle
image is exactly what it was. `GROOMLAB_APP_HAIR_SHADING=fragment` draws the interactive frame
per fragment as before (the reference the tests compare against).

**The visibility path** (`LookdevRenderer::HairPath::Visibility`, the default where the device
computes and has 3D textures; everything above is the legacy path, kept for comparison:
`GROOMLAB_APP_HAIR_PATH=legacy`). The hair is rasterized in compute shaders into a visibility
buffer, Unreal style, with 32-bit atomics only (Direct3D 11 has no 64-bit ones):
`hair_raster.comp` runs a thread a segment, projects it and walks it a pixel column at a time
along its major axis (pixel centers along it, half open, so a strand's segments share no
column); in each column the fiber covers the interval of its true projected width times the
secant of its slope, and each pixel takes its share of it: analytic box-filtered coverage at
any width, no MSAA, no floor. The same walk runs twice: first every fragment adds `-ln(1 - c)`
to its pixel's optical depth (fixed point, `atomicAdd`: the pixel's alpha is `1 - exp(-tau)`,
order independent) and offers its key, its view depth with a top bit set unless its coverage
reaches the threshold hashed from its strand and the stretch the pixel lies on (`display.h`'s
`lineHashThreshold`, the line path's rule), to the pixel's `atomicMin`; then the fragment
holding the minimum writes its segment and where on it. The visible fragment is the nearest
kept one, strand i with the weight `c_i x prod(1 - c_j)` of the ones in front, as their
ribbons would; refine frames hash with a new seed each (and jitter), so their mean is the exact
transparency. Hair behind the surfaces (their view depth, drawn first through
`surface_depth.frag`) is not drawn. `hair_shade.comp` shades the visible fragment once a pixel
with the same lobes (`hair_shading.glsl` around `engine/gpu/shaders/hair_bsdf.glsl`, shared with
`hair_lookdev.frag` and Maya's viewport: see "One hair shading" below) and writes it
premultiplied by the alpha; `hair_composite.frag` lays it over the resolved scene. The hair's
shadows come from voxel volumes instead of the deep opacity maps: `hair_voxelize.comp` drops each
segment's fiber area into a grid over the hair (128 voxels along its longest side),
`hair_density.comp` smooths it into a 3D texture and `hair_volume.comp` marches each voxel
toward each casting light and the sky into a count of hairs, so shading reads a light's count in
one trilinear look-up (two voxels toward the light); they are made again when the hair, the
material or the lights change, never for the camera, and the surfaces read them for the hair's
shadow on them. A volume counts the hairs a ray crosses (pi / 4 of the fiber area per voxel face
crossed, toward the sky 0.405), as Maya's density grid does; the shading turns that into its
optical depth (`hair_bsdf.glsl`'s `hairShadowCount`; `GROOMLAB_APP_VOXEL_SCALE` overrides the
scales, the surfaces' shadows from the hair take 2.55 times the count, as they were fitted). The clumps' level of detail (`strand_lod.h`) thins what the view cannot
resolve: a strand of class k is widened to `1 - (1 - c)^e`, the rest skipped. At 1920 x 1080 on
the bench scene (Direct3D 11, the GPU's time) a still frame takes 0.9 ms at 100k strands, 2.9 at
500k and 4.5 at 1M (the legacy path 14.7, 52.6 and 98.5).

**One hair shading** (`engine/gpu/shaders/hair_bsdf.glsl`). The app's lookdev (both paths) and
Maya's Viewport 2.0 hair effect shade with one source: Marschner's lobes in Karis's closed forms,
the fiber's absorption, Zinke's dual scattering, the shadows' optical depth, as pure functions
in the words GLSL and HLSL share (GLSL's vector names, no `vec3(x)` splats, no `fract`/`mod`/
`inversesqrt`, no global constants). The app includes it from the shaders that include
`hair_shading.glsl` (qsb's dependency file lists only a shader's own includes); Maya embeds its
text at build time (`hosts/maya/cmake/embed_text.cmake`, `hair_bsdf_text.h`) and renames the
vector types for DirectX 11 (`hlslOf`). Each host hands in its material, lights and the hairs
crossed toward each light (the app's volumes or deep opacity maps, those counted 2.55 times the
hairs; Maya's grid). Calibrated against Arnold's `aiStandardHair` on the head groom
(`tests/data/app/head_graph.json`) at five pigments (melanin/redness 0.35/0.2, 0.12/0.45,
0.6/0.25, 0.85/0.1, 0.4/0.95), in scene-linear Rec.709-sRGB (`arnold_compare.py --linear-srgb`),
the key light alone and the environment alone, in two steps:
- *The lobes, single scattering:* a sparse groom (Scatter at 3 strands a cm2, no strand shadows
  another) against `arnold_compare.py --single-scattering` (GI depths and `standard_hair`'s
  `extra_depth` 0). Arnold's lobes are d'Eon's width (in theta_h a Gaussian of variance v / 4:
  `longitudinalWidth`/`hairLongitudinalWidth` 0.5 sqrt(v), they were 0.71), the cuticle tilt the
  other way (the R highlight at theta_h = +shift for tangents root to tip), the azimuthal spread
  1.24 times d'Eon's, the R, TT and TRT lobes 0.89, 0.52 and 0.82 of Karis's, and melanin's
  absorption 6.6 x melanin^2 x (eumelanin, pheomelanin by redness) per fiber radius (a fitted
  exponent came out 2.00; the core's `melaninColor` is the hair color with that absorption, so
  Maya, which gets the pigment as that color, shades it with `hairColorSigma` like any other).
  A hair color (HairColor's RGB in either mode, a tip color, Bands, FeatherColor: Arnold's base
  color over no melanin) is absorbed as aiStandardHair absorbs a base color,
  Chiang et al. 2016's (ln c / (5.969 - 0.215 b + 2.532 b^2 - 10.73 b^3 + 5.574 b^4 + 0.245 b^5))^2
  for the azimuthal roughness b (`hairColorSigma`, core's `hairColorAbsorption`): the color the
  groom's scatterings settle on, not one pass's transmittance (handed to the lobes as it was, TT,
  TRT and the multiple scattering took it to the second, fourth and higher powers: a skin tone
  drew dark red, a mid grey a tenth of Arnold's brightness). Against Arnold on the head groom
  under the key light (the pixels Arnold lights, linear): a picked 0.5 grey 7.0 times the melanin
  0.35 brown's luminance, the viewport (DirectX 11) 11.2 and the app 11.6 (0.7 and 0.7 before);
  a skin tone with white tips 1.06 times the grey's red over blue, the viewport 1.05 and the app
  1.05 (2.6 and 2.7 before); a saturated red's chromaticity 0.80 against Arnold's 0.74 (0.67
  before). Light colors read brighter than Arnold's, as light melanin does (`gui_hair.py`
  `picked_color_check`, `AppRender.PickedColorsRenderAsArnoldsBaseColor`).
- *Through the other hairs:* the dense groom. Arnold's shadow rays darken more gently than one
  hair a crossing: optical depth 1.21 a hair crossed plus 0.33 (1 - e^(-hairs / 0.18)), the
  strand's own clump; the dual scattering's forward attenuation a_f = 0.04 + 0.86 c^1.95, back
  a_b = 0.02 + 0.34 c^0.85, Zinke's A_b channel by channel times 0.22, the forward-scattered
  light 0.053 a_f^n; in the app the environment's irradiance 0.063 / pi of the albedo, occluded
  by e^(-0.29 sky count).
- *The multiple scattering from every way:* Arnold's light hair is as bright for a strand along
  the light or the view as across it (a furball under a light along the camera, its strands
  binned by the angle to the view: blonde 0.09 to 0.13 in every bin), where the lobes and
  Zinke's terms, all through the longitudinal angles and times cos theta_i, fade to a fifth (the
  viewport's furball went black between the clumps and at the rims, self-shadowing on or off).
  The share they lose comes back as 0.17 (1 - cos theta_i cos theta_o)^2 A_b a_f^(n/2), not
  through the lobes (fitted on the furball and the head together: A_b, nothing for dark hair,
  keeps red, brown and black as they were). The blonde furball's mean against Arnold went from
  0.55 to 0.90 (OpenGL, self-shadowing on; 0.58 to 0.98 on DirectX 11), its strands toward the
  camera from 0.13 to 0.96, brown 0.97 both; the head's luminance |log| from 0.16 to 0.09
  (OpenGL) while its lower third, the light pigments' tips already too bright, went from 0.36 to
  0.43.
`GROOMLAB_APP_TUNE="a,b,..."` overrides the constants in the app for a tuning loop (0 keeps one):
1 the melanin scale, 2 the back scattering, 3 the shadow's slope, 4 the melanin exponent, 5 the
forward-scattered light, 6 a_f's exponent, 7 the R lobe, 8 a_b, 9 the multiple scattering from
every way, 10 the azimuthal spread, 11 TT,
12 TRT, 13 and 14 the environment's irradiance and its occlusion. Against Arnold, over the five
pigments (mean absolute error of the 8-bit sRGB pixels / |log| of the luminance ratio /
|log| of the saturation ratio / |log| of the lower third's luminance ratio): the app
12.2 / 0.37 / 0.25 / 0.85 before, 8.4 / 0.14 / 0.15 / 0.40 after (key and environment);
Maya's viewport (key) on DirectX 11 17.1 / 0.74 / 0.23 / 1.87 before, 9.7 / 0.09 / 0.12 / 0.37
after, on OpenGL 15.6 / 0.66 / 0.23 / 1.71 before, 10.7 / 0.13 / 0.17 / 0.33 after.
- *The back scattering's width:* Zinke's sqrt(beta_b^2 + 2 beta_f^2) with the R lobe's B and TT's
  B/2 (1.22 B), where it had been the TRT lobe's 2B with one forward spread (2.06 B). The wide lobe
  lit strands far off the specular cone with back scattering: light hair's tips, hanging under the
  head's curve, read brighter than Arnold's. Against Arnold on the head under the key light, scalp
  shown (`arnold_compare.py --lights key --linear-srgb`, the app's `--lighting key --tonemap none`,
  8 frames; luminance ratio app / Arnold, the hair's lower third / all): blonde 0.12/0.45 1.23 / 0.94
  before, 1.01 / 1.03 after; red 0.4/0.95 1.28 / 1.01, 1.07 / 1.07; 0.6/0.25 0.73 / 1.03, 0.74 / 1.04;
  black 0.85/0.1 1.20 / 1.15 both; brown 0.35/0.2 0.69 / 0.76, 0.57 / 0.79 (brown is darker than
  Arnold's throughout, its own gap). The environment alone moves little (blonde all 1.04 to 1.11).

**Wet hair** (`hair_color.h` `wetShading`, no param of its own). The `wetness` channel (Wet's per
point; a Simulate's with water or rain; an Attribute node's per strand named `wetness`; 0 dry .. 1
soaked) drives the look wherever it is: the fiber's absorption times 1 + w (`kWetAbsorption`: water
around the fiber turns less light back at its surface, so more goes the long way through it;
darker and more saturated, white stays white), the R lobe's tint times 1 + 0.25 w and Arnold's
roughness times 1 - 0.5 w (the water-filled cuticle: a narrower, stronger highlight). One mapping,
three hosts: `hair_bsdf.glsl`'s `hairWet` takes `wetShadingUniform`'s numbers (Maya's `hairWetLook`,
the app's Frame `wet`; the width ratio for the look's roughness, blended linearly); the strands'
wetness rides in the lit colors' fourth float packed with the iridescence (`packWetLook`, 4 and
over; `hairLookAlpha` unpacks it per vertex, so a dry groom's colors are the very bytes they were),
and where the strands carry no colors of their own the item's color goes with it
(`wetLookColors`); Arnold gets `wetness` user data (varying, or uniform for a strand channel) and
`create_hair_shader`'s wet network (`shading.py` `_wet_network`: melanin x sqrt(1 + w), the base
color to the power 1 + w, the tint and roughness scaled, in front of what drove them; the viewport
reads the dry look through its `<shader>_wet...` nodes). The game cards do not take it; a GPU preview
drag of the wetness shades the new wetness once the drag ends (the colors are the CPU strands'). Checks:
`HairLook.WetHairIsDarkerMoreSaturatedAndGlossier`, `Display.LitLinesCarryTheWetness`, `gui_hair.py`
`wet_check`, `test_arnold.py` `test_wet_hair_reaches_arnold_and_the_hair_shader`.

**Adaptive interactive quality** (the Render tab's "Interactive quality", `LightingSetup::
interactiveQuality`, `--interactive-quality adaptive|full`): while the camera, a slider or a
stroke moves, the frames draw into the corner of the same targets at a resolution scaled down
to fit a display's refresh and the display transform scales the picture up (`tonemap.frag`
samples the corner); once nothing moved for 150 ms the full picture draws and the refine
accumulation starts. The renderer sees a stream in two frames back to back each after a change
(or the host holding a drag: the viewport's mouse drags and the param panels' slider drags say
so, `setInteracting`, so a drag's first frame is reduced too). It measures the full frame from
the frames that came back to back at full size (the refine frames, a stream's first) and takes
the scale that brings it to 12 ms; a reduced frame over 24 ms lowers the stream's scale further;
before any measurement the point count stands in. A reduced frame is never accumulated. On the
bench scene the orbit goes from 23 fps to 85 at full size and 159 adaptive at 100k strands
(5.7 to 27 and 45 at 500k); live in the window an orbit or a GPU-preview drag on the lookdev
path is one refresh (16.6 ms between frames, 33 before). `tests/data/app/arnold_compare.py` renders a
saved groom in Arnold (kick, the GroomLab procedural) with the studio preset's lights for a
side-by-side; `--linear-srgb` renders in scene-linear Rec.709-sRGB, the space the app's constants
are in (kick's default is ACEScg), to compare with the app's `--tonemap none` (`--lights key` /
`env` and the app's `--lighting key` / `env` put both over black). `--benchmark` prints the GPU's
own time per frame beside the wall clock (timestamps), `--benchmark-orbit` the GPU's mean.
`GROOMLAB_APP_RHI=d3d12` runs the offscreen device on Direct3D 12 (`GROOMLAB_APP_RHI_DEBUG=1` with
its debug layer, the messages on stderr; 2 adds GPU-based validation). On Direct3D 12 QRhi sets the
scissor from the viewport only while a pipeline is bound: the renderers set each pass's viewport
again after every pipeline (before, every draw there was clipped away and only the clears showed).

**Surfaces.** The scalp, the props loaded beside the groom and the eyeballs made in code are
`Surface`s (`hosts/app/src/surface.h`: a `ScalpMesh`, normals per corner from the file, occlusion
baked per vertex by rays against the props, a `SurfaceMaterial` from `look.h`), uploaded as
interleaved vertices with tangents (Lengyel 2001) and a curvature per vertex, drawn in the
HDR pass by `surface_lookdev.frag`. The hair's deep opacity maps shadow them; they shadow
the hair and each other through a 2048 float distance map per light fit to the surfaces'
own bounds (the hair's maps fit the hair's, and hold the hair alone), read with a normal
offset and a slope-scaled bias and filtered over a rotated 12-tap disk (percentage-closer
filtering; the rotation changes per pixel and per accumulated frame, so refine mode
converges to a soft disk), the map's say fading out over the last degrees before the
terminator; the hair shader reads the same maps for the head's shadow on the hair. A
material is one of three kinds, with presets (`surfacePreset`: clay,
scalp, plastic, metal; skin, skin light, skin dark; eye brown, eye blue, eye green) and the
maps an OBJ's material library names (`map_Kd` albedo, `map_Bump` normal, `map_Ks` specular,
`map_Pr` roughness, `map_Pm` metallic, `map_Ka` occlusion; the gray maps are packed into
one texture): *Default* is metal/roughness PBR (GGX with Smith's height-correlated
visibility, Schlick's Fresnel, the multiple scattering a rough lobe loses put back after
Turquin 2019 for the lights and Fdez-Aguera 2019 for the environment, the environment through
Karis 2013's split sum: the map prefiltered per roughness into the mip levels of one texture
and an environment BRDF table, both computed on the CPU, Lambert's diffuse weighed by one
minus the specular lobe's directional albedo from the same table (Fdez-Aguera's energy
split: Schlick's Fresnel of the macro normal alone goes to 1 at grazing angles and blacked
the rim out), specular occlusion from the occlusion after Lagarde 2014; an interpolated
normal leaning past the horizon at a silhouette is bent back toward the viewer rather than
flipped, which lit the rim as if it faced away). *Skin* is not Lambertian: light enters,
scatters and leaves elsewhere, by Burley's normalized diffusion profile (Christensen and
Burley 2015) with a diffuse mean free path per channel (`sssMeanFreePath`, cm: Jensen
2001's skin, 3.67 / 1.37 / 0.68 mm, red the furthest) times a scale (`sssRadius`, as
Arnold's `subsurface_radius` x `subsurface_scale`). Two ways to apply it (`sssMode`):
*screen* (the default; Jimenez et al. 2015, "Separable Subsurface Scattering") draws the
skin surfaces once more before the HDR pass, the same shader under `SSS_PREPASS`, into an
irradiance target (the lights' and the environment's diffuse through the shadows plus the
light transmitted through thin parts, times the albedo's pre-scatter share, `sssTexturing`,
the blur's width in alpha) and a view depth target; `sss_blur.frag` convolves it along x
then y with the profile's one-dimensional kernel (`separableSssKernel`: 31 taps crowded
toward the center over five mean free paths, each the mass of the profile over its cell from
a strip table that integrates the profile's 1 / r singularity in polar coordinates), the
offsets scaled per pixel by the material's radius over the depth, taps across a depth edge
or off the skin taking the center's value, jittered within their cells in the accumulated
frames; the HDR pass reads the result at its pixel, applies the rest of the albedo
(post-scatter texturing: the pores stay in the specular while the diffuse goes soft) and
adds the specular. *Pre-integrated* (Penner 2011) looks the same profile up by n.l and the
curvature from a table integrated around spheres, the diffuse normal blurred toward the
geometry's per channel: the fallback, no extra passes. Both: two GGX lobes (a tight and a
wide one), F0 0.028, the cavity map occluding the specular, light through thin parts (ears)
from the thickness the shadow map gives (read bilinearly, lifted by the slope's span in a
texel) through diffusion theory's decay e^(-t/d) after Jimenez 2010 (which also softens a
thin shadow's edge), a generated tileable micro-detail normal for pores.
`GROOMLAB_APP_DEBUG_VIEW=1..11` shows one term of a surface's shading (diffuse, specular,
the diffuse weight, occlusion, the first light's shadow, the normals, the transmittance, the
blurred irradiance, the albedo, the blur's width). *Eye* follows Jimenez 2012's
"Next-Generation Character Rendering": the view ray
refracted at the cornea (IOR 1.376) onto the iris plane behind it (parallax), a procedural
iris of fibers with a collarette, a limbal ring and a pupil, lit as a shallow bowl by the
refracted light (the cornea's focusing brightens the side away from the light), a sclera with
veins, the lids' shadow from the baked occlusion, a wet tear line where the lids meet the eye,
the tear film's specular over everything; `makeEyeSurface` builds the eyeball (a sphere with
the cornea's bulge inside the limbus). The Look dock's Surfaces group edits every surface's
material. The tables (`environmentBrdfLUT`, `burleyProfile`, `profileStripTable`,
`separableSssKernel`, `preintegratedSkinLUT`, `skinTransmittance`, `microNormalTexture`,
`prefilterEnvironment`) are plain functions the tests check. `surface_lookdev.frag` reads
its 19 textures through four separate sampler objects (Direct3D 11 has 16 sampler slots;
QRhi pairs them on every backend).

`tests/data/app/scan_head/arnold_skin_compare.py --build build/wt --view all` renders the
head in the app and in Arnold (kick, `standard_surface` with randomwalk subsurface at the
same albedo, mean free paths, F0 and roughness, the scan's maps, the same camera and lights,
in linear sRGB) and writes side-by-sides with the difference and the mean error over the
head to the renders folder; `--sss-type diffusion` compares against Arnold's diffusion
profile instead, `--no-maps` the flat skin color (the shading alone). The app lands within
about 10 / 255 of Arnold's diffusion mode on the flat head and within 12 of its randomwalk;
what remains is Arnold's bounce light on the dark side (the app has no indirect diffuse
between surfaces) and randomwalk's extra red on the lit face (a volumetric effect no
diffusion profile gives).

**The head scan.** The showcase renders use the Lee Perry-Smith head scan by Infinite
Realities (CC BY 3.0; the copy in three.js's examples, its 4k albedo from keijiro's
InfiniteScan package), kept outside the repository in `%USERPROFILE%/groomlab-deps/assets/`
(never committed; credit "Head scan: Lee Perry-Smith / Infinite Realities, CC BY 3.0" with
any picture of it): `head.obj` in cm with its material library (the scan as it is, lids
closed: no eyeballs), and `head_scalp.obj` the faces above the hairline for the groom
(`tests/data/app/scan_head_graph.json` combs it back). The eye material is tested on generated
eyeballs (`makeEyeSurface`), not on the scan.
A run with all of it: `GroomLab head_scalp.obj --graph tests/data/app/scan_head_graph.json
--hide-scalp --prop head.obj --prop-material skin --lighting studio
--screenshot out.png --size 1920x1080 --frames 64`.

**How it is put together** (`hosts/app/src`): `GroomDocument` (no Qt) holds the open groom: the
`GroomScene` and its `GroomSession`, the graph as JSON every param edit changes and hands back
to the session whole (`setGraphJson` adopts the old cache, so only the edited node and what
follows re-run), an undo history of those edits (a slider's drag is one step), the
`DisplayBuilder`, save and the Alembic export (`evaluateFullDensity` on the session's own
document, so the view's cache stays). `GroomRenderer` keeps one GPU copy per display item and
uploads it again only when the builder rebuilt it (`DisplayItem::rebuilt`), draws lit hair lines
(Kajiya-Kay from the core's shading frames, the highlight from `hairHighlight`), flat and
colored lines, overlays over the hair (depth test off), shaded surfaces, and (on the line
path) the scalp and the props as Lambert surfaces; on the lookdev path the scalp and the props
are the `LookdevRenderer`'s surfaces. `Viewport` is the `QRhiWidget`: it hands its frames to
the renderer and turns the mouse into the `OrbitCamera` (left orbits, middle pans, Alt+right
or the wheel dollies, F frames all); it draws only on `update()`. `MainWindow` has the node
list, the parameter panel (`ParamPanel`: editors from the operator specs, live edits), the
maps panel, the display panel (preview density, point budget, lookdev, the GPU preview, strand
colors, guides, lit hair, scalp, the map on the scalp, the card view) and the props it loads
(`addProp`, `addEye`). `mesh_io` reads OBJ (as a scalp, or as a surface with its normals and
material library) and, through `AlembicMeshReader::meshAt`, Alembic meshes with their faces
and UVs. Errors go to the status bar and stderr, never a dialog.

**Partial uploads.** The builder hands the strand lines over as `StrandLines`
(`DisplayBuilder::setWriteLines`: the vectors stay empty) and the renderer writes them
itself, in parallel, into scratch arrays it keeps and vertex buffers it keeps (`Static`, the
same size written again in place: a tweak that rebuilds every strand allocates nothing, where
making and freeing tens of MB a frame cost the driver more than the write and showed as
spikes). When only some strands changed (a brush dab, a stroke's release: `DisplayItem::updated`,
the view's `strandsChanges` against the serial the buffers hold) only their vertex ranges go
up (`uploadStaticBuffer` at an offset per run of `strandRuns`; the indices and uvs kept, the
lit frames and colors with the points), and the lookdev path writes only their texels (two a
point, three a strand, as sub-region texture uploads: a partial first row, whole rows, a
partial last row), keeping the index buffers; whole uploads when the layout changed (another
point count: a stroke's press, its release). `GROOMLAB_APP_PARTIAL_UPLOADS=0` writes
everything whole (`setPartialUploads`: the tests compare both, pixel for pixel). A whole write
of a buffer that already exists goes straight through the device where it can
(`writeBufferNative`: `UpdateSubresource` on Direct3D 11, the positions from the strands' own
array, no copy on the way), where QRhi's upload batch would first copy the bytes into itself;
a CPU tweak of Noise went from 37 ms to 28 at 100k strands (181 to 142 at 500k) on the line
path. The stroke document is evaluated ahead between strokes in brush mode and before a
scripted stroke that draws its dabs (`MainWindow::warmStroke`), so a stroke's press makes
again only what it changes: 2.8 ms instead of 17 at 100k (14 instead of 63 at 500k).

**GPU look preview.** The same engine as Maya's (`engine/gpu/`, see the GPU look preview section
above) on QRhi's device: `hosts/app/src/gpu_preview.h` is the host glue, reading the device from
`QRhi::nativeHandles` (the Direct3D 11 device; the OpenGL context current while the viewport
renders; a Vulkan or Metal engine would plug in the same way once `engine/gpu/` has one). A slider's
drag of a node the GPU runs (Interpolate's radius, Length, Clump, Noise, Curl, Width, Part, Gravity, Flyaway) does not
touch the graph: `GroomDocument::livePreview` holds the value (`param` shows it, nothing
evaluates) and the renderer's next frame runs that node and every one after it, from the
strands entering it (the last evaluation's cached result, uploaded once, only the changed
strands after a stroke), straight into what it draws: on the lookdev path the point texture
(a kernel of look.slang writes the app's RGBA32F layout, `LookTargets::pointTexture`, through
an unordered access view on Direct3D 11 and an image binding on OpenGL; the maps are refit to
the box the run can reach, `lookBounds`), on the line path the strands item's buffers (made
with the storage usage so the kernels write them in place on Direct3D 11 too, else copied into
on the GPU). The run happens inside `GroomRenderer::render` after the frame's uploads are
recorded and before anything draws; a frame that uploads the strands puts the run off to the
next one (QRhi records uploads and draws, the kernels run at once). The release records the
edit and commits the held value on the CPU, the reference. A stroke: the press is the CPU's
(the stroke document at its density, shown); then `armStroke` plans the run from Interpolate
and each dab's evaluation stops there (`ViewRequest::stopAt`: the guides alone, the view keeps
its strands) while `strokeHolds` says the dab's guides fit what the GPU holds (else the dab,
and the rest of the stroke, is the CPU's); the GPU writes the item drawing the stroke's strands
(the dimmed one, `sculptDisplay` 1). The Display panel's "GPU preview" toggle (default on
where the device has an engine; `GROOMLAB_APP_GPU_PREVIEW=0|1` forces it, `--gpu-preview`
the CLI) turns it off: drags and dabs preview on the CPU as before. The stats line's time per
dab and `--benchmark-drag` say what ran where. The numbers against Maya's viewport (the same
scene, `tools/bench/xgen_compare/make_app_scene.py`, the same drags and strokes, both APIs) go
in `tools/bench/xgen_compare/results/2026-10-02_app_vs_maya.md`, which holds the method and the
commands and the findings, and its "Interactivity pass" section the numbers after the per-point
shading, the adaptive interactive quality, the native buffer writes and the Collide query work.
In short (RTX 3090, 1920x1080, 100k / 500k strands): a GPU-preview
drag of Noise draws in 2.5 / 10.2 ms against Maya's 2.9-3.5 / 10-12, a GPU dab in 3.0 / 8.4
against 3.4-4.8 / 4.6-8.4, a CPU dab in 9.2 / 25 against 13 / 35 (the partial writes), a CPU
tweak of Noise in 28 / 142 against 26-32 / 108-136 (the whole write, straight through the
device); live in the window a param drag went from 75 ms between frames to
50 on the CPU path and to one vsync period (16.5 ms) with the GPU preview; the lookdev frame
is 16 / 35 ms (46 / 186 before the per-point shading).

**Brushes.** The tools are the core's (`stroke.h`: the same `BrushTool`, settings and strokes
as the Maya context, nothing of the brush math in the app). `tool_view.h` turns the orbit
camera into a `ToolView` (Qt's column-vector matrix transposed into the core's row convention
over the widget's size, pixels counted from the bottom; `toPort` flips a widget position), so
projection, rays, Mask by scalp and the parallel screen weights all come from the matrix.
`BrushSession` (no widgets: the tests and the CLI drive it) runs the tool over
`DocumentStrokeHost`, a `SessionStrokeHost` whose selection and hidden groups are the
document's and whose `commit` hands the `StrokeEdit` to `GroomDocument::commitStroke`: the
stored texts before are recorded, the edit applied to the session and kept as one step in the
same history as the parameter edits (undo puts the texts back bit for bit, redo the stroke's;
`undoText` says "Undo comb stroke"). Each dab previews through the session (`setPreviewHierarchy`,
the stroke document at `sculptDensity`, the dimmed strands of `sculptDisplay`), and the window
evaluates and redisplays after it (`strokeDabbed`), the release after the commit. Hover asks
the tool for the guides under the brush (`affectedLines`) and puts them, with the ring (on the
scalp under the cursor, or at the camera target's depth off it, as wide in the world as the
screen radius there: `worldRadiusAt`), into overlay lines the renderer draws last
(`setToolOverlay`), in the colors Maya's context draws them in (stroke.h's `brushRingColor`,
`brushPathColor`, `brushHoverColor`); guide point dots are camera-facing diamonds (`dots.vert`: the point's
position plus a pixel offset per vertex, so they need no rebuild when the camera moves).
`GroomDocument::editGuides` runs any edit of the authored hierarchy (groups, children,
deleting guides) as an undoable step the same way. The `Viewport` has camera, brush and select
modes (B, S, Escape; the Tools menu and tool bar): Alt with any button is always the camera;
in brush mode a left drag strokes (Shift smooths; with a guide brush Ctrl+drag paints the
selection, Ctrl+Shift+drag lassos it, Ctrl+click adds the guides in the ring, on none unfreezes
them all; painting, Ctrl is the opposite), B held or the middle button drags the radius (Shift: the strength), the right orbits,
a pen's pressure comes from `QTabletEvent`; in select mode a click or box picks guides and the
rest freeze (Shift toggles, Ctrl deselects, Ctrl+Shift adds; nothing picked unfreezes all; Delete removes them). The brush rail and
the brush bar (`overlay.h`) pick the brush and its main settings; the full brush panel
(`BrushPanel`, behind the bar's "...") lists the kinds with GroomLab's icons (`hosts/app/icons`, the
plugin's own) and every setting (Falloff as the catalog names it, 0 hard to 1 soft: the core's
`hardness` is 1 - falloff; Front facing only); the guides panel (`GuidesPanel`, the right rail's first
button) the groups (hide, rename, add root or child groups, remove, generate children, select a
group's guides). Saving writes the session's guides,
parts, flow and correctives with the graph. The CLI grooms before its screenshot: `--brush
name=value` (mode, radius, strength, hardness, symmetry, level, group...) and `--stroke "x,y
x,y ..."` (pixels of `--size`'s view, y down; `ctrl:` or `shift:` first) run in order and
print each stroke's dab times.

**Tests.** `groomlab_app_tests` (label `quick`, only with the app on) runs under
`QT_QPA_PLATFORM=offscreen`: the document (open, evaluate, edit, undo, redo, save, export), the
OBJ reader (scalps and surfaces with their material libraries), the camera, the surfaces'
geometry (tangents, curvature, the eyeball, baked occlusion), the surface tables, presets and
params, one frame of `tests/data/strokes/strokes.groom` rendered through `QRhi` (Direct3D 11
when there is a device, else the Null backend, where the pixels are not checked) into a
texture, read back and checked for drawn pixels, and a sphere in every surface material (with
the sample groom over it); the tools: the adapter's projection, rays and radius against the
camera, strokes through the session (guides move, one undo step each, undo bit-identical,
redo, the history shared with parameter edits, saved and read back), hover, pressure,
symmetry, place, draw, erase, select and delete, groups and children, and the dots and the
ring drawn offscreen; the graph's edits (add, connect, disconnect, remove, positions saved);
the theme (the fonts register, the brushes are grouped, the overlay layout at the mockup's
1440x900 and at tall and short views); partial uploads against whole ones (a stroke's dabs on
both paths: the same pixels); the GPU preview (skipped without a device with an engine): a
Noise drag's frame (its amplitude, its profile's ramp, its seed) against the same value
committed on the CPU (the lines to the pixel; the
lookdev path within a few percent of the pixels, its maps fit to the run's box), three stroke
dabs blended by the GPU against the CPU's evaluation of the dab (to the pixel), and the
document holding a drag's value until its end; the panels, driven without a window (`test_panels.cpp`,
a `QApplication` on the offscreen platform): the param editors round-trip every kind the
specs have without losing a map (a ramp's keys and interpolation, a map's source, component
and range, a port, a color stored linear), the look card and panel apply every setting (the
render settings survive a lighting preset, the skin's scattering controls edit the surface),
and the maps panel makes a vertex map, a color map, a guide layer and a texture map (its
image beside the groom), paints the vertex and texture maps through the brush session (one
undo step each, the file written again on undo), shows one on the scalp, renames and removes
with undo, and reads them back from a saved file; a saved groom names its bake and opens with
it (stamp kept, a missing preview made the same as the app's), and a Z-up file opens turned
Y-up, exactly; a Simulate node steps live, scrubs back to the same frames, fills the range, bakes
and is read from its bake after a save; the timeline's sim controls ask for their runs and its
strip draws the cached and baked frames (`app.cli_exports` bakes from the command line too). It leaves `%TEMP%/groomlab_app_smoke*.png`,
`%TEMP%/groomlab_app_surface_*.png`, `%TEMP%/groomlab_app_gpu_*.png` and
`%TEMP%/groomlab_app_brush_ring.png` to look at.

**Parity with Maya.** [APP_PARITY.md](APP_PARITY.md) lists every artist-facing capability of the
Maya plugin by area with its Maya entry point and whether the app has it (present, partial,
missing, or Maya only by design: Maya's own nodes, animCurves, its viewport camera, nHair, FBX,
MtoA), and what is still missing in order of value.

**Next.** A texture map drawn on the scalp at its own resolution (a
textured scalp item in the renderer; it is sampled per vertex today), converting maps between
kinds and relinking a texture map's file, parts and flow overlays with
their own panels, a lasso, soft selection, cards and the Unreal export (the bake and the glTF writer are in the core),
Vulkan and Metal engines for the GPU preview (`engine/gpu/` has Direct3D 11 and OpenGL; the glue
takes the device from `QRhi::nativeHandles` the same way), a timeline for caches, macOS
and Linux builds and the Qt notices in the package. For the surfaces: a scene file for props and
eyes (the CLI holds them for now), indirect diffuse between the surfaces (the chin's bounce onto
the neck, which Arnold shows and the app does not), a kernel per skin material (every skin
surface shares the first's profile shape; each has its own radius scale) and cascades for the
surfaces' shadow maps when a scene holds more than a head.

## 3. When Maya crashes or hangs

**Crash logs.** The plugin installs handlers (`hosts/maya/src/crash_log.cpp`) for `abort()`,
`std::terminate`, pure virtual calls, invalid CRT parameters and, on Windows, the top-level
unhandled exception filter (access violations and the like). Each writes one file
`%TEMP%\GroomLabCrash_<date>_<time>.log` with the reason, the exception detail, the **faulting
module** on its own line (`faulting module: ai.dll+0x78d60c (...)`: a fault in `ai.dll` is inside
Arnold, whatever filter wrote the log), the GroomLab, Maya, MtoA and Arnold versions, and the
faulting thread's stack unwound from the fault (frame 0 is the faulting instruction). Only
GroomLab's own frames are symbolized, through Dbghelp from the `.pdb` beside each binary (the build
folder's `hosts/maya/RelWithDebInfo/GroomLab.pdb`; copy it beside the installed `.mll` for symbols
from an installed module); other modules show as module+offset, so no symbol server is ever
reached while Maya waits to save. The filter then hands on to the one before it (Maya's, which
saves the scene to `%TEMP%`), even when writing the log faults. Unloading the plugin takes the
filter out only if it is still the top one; otherwise (Arnold sets its own filter after it when
MtoA starts, and calls it) the filter stays, only handing on, and `GroomLab.mll` stays loaded
(pinned) until Maya exits. Look there first after "Maya has stopped working". Maya's own report
goes to `%TEMP%\<user>.<date>.ma` and the Windows Event Log; the GroomLab log is the useful one.

**Hangs.** Maya frozen (100% of one core, or waiting) means a loop or a deadlock, not an
exception, so no crash log is written. Get the thread stacks of the live process:
`procdump -ma <pid>` (Sysinternals) then `windbg`/`cdb -z dump.dmp` with `~*k`, or a small
Dbghelp tool that opens the process, suspends each thread and walks its stack (StackWalk64 with
the PDB path set through `SymInitialize`). The stack that sits in `GroomLab.mll` names the loop.
Past hangs were: a null texture assignment inside Maya's renderer (fixed by never assigning
null textures), and a freed vertex buffer (fixed by keeping the overlay colors buffer alive).

**Quitting Maya.** Maya does not delete its docks' widgets when it quits, nor finalize Python:
every PySide2 wrapper is still alive when the process exits, and shiboken2's static cleanup
(`BindingManager`'s destructor, run as `shiboken2.cp37-win_amd64.dll` unloads) walks them, their
children and the objects they keep references to. It used to crash there now and then
(`shiboken2...dll+0x38380`, a null read under `_execute_onexit_table`, after every GUI suite had
passed): `widget.style()` on a widget under a style sheet returns Qt's own `QStyleSheetStyle`,
and PySide2 keeps its wrapper alive from the widget's (`"__style__"`); when the cleanup's walk,
in hash order, freed that wrapper before reaching the widget through its parents, it read the
freed pointers again. So: repolish through `widgets.repolish(w)` (the style sheet set again),
never `w.style().unpolish/polish`; and each panel registers a step with `widgets.at_exit`
(kMayaExiting) that stops its callbacks, timers and scriptJobs and deletes its widgets
(`widgets.dispose`) while Qt and Python are whole, so nothing of GroomLab's is left for that walk
or fires while Maya shuts down. The `quit` GUI suite checks both (no `style()` called while the
panels open and repolish; the step deletes them and kills their jobs) and ends the session with
the panels open; `maya.hardening` ends a mayapy process as Maya does (`os._exit`, no Python
finalization) with fifty repolished widgets alive, which crashed every time with `style()`. A minidump from Maya's crash handler (`%TEMP%\MayaCrashLog*.dmp`) can be read with
`dumpbin /disasm` of the faulting module and a scan of the faulting thread's stack for return
addresses when no debugger is installed.

**Errors that are not crashes.** Evaluation never throws into Maya: `groomlab.errors(groom)`
(the `lastError` attribute) lists what went wrong, node by node, and the Graph Editor's status
bar shows it. Warnings in the Script Editor are prefixed `GroomLab:`. `cmds.groomLabPaths()`
prints the folders relative paths resolve against; `cmds.groomLabPaths(resolve=path)` resolves
one. `groomlab.card_stats(groom)` says whether the cards' bake is stale.

**Viewport oddities.** Drawing goes through `GroomSubSceneOverride`, which uploads what the core's `DisplayBuilder` built (`display.h`; the strand lines are written straight into the items' GPU-only buffers, kept and written again in place, the indices and uvs kept while the point counts stay; when only some strands changed (a brush dab, a stroke's release: `DisplayItem::updated`) only their vertex ranges are written (`MVertexBuffer::update` per run of strands under OpenGL; under DirectX 11, where the buffers keep a system-memory copy and every `update` sends the whole buffer again, the copy is acquired as it is, the runs written into it and each stream committed once; `-drawTimes` says `partialStrands`), and the lit strands are kept hidden through a stroke (`DisplayItem::keep`); `groomLabGraphStats -drawTimes` times the last upload; `GROOMLAB_VIEWPORT_SNAPSHOTS=<folder>` makes the viewport GUI suite write pictures of every kind of item, to compare a change of the draw code pixel for pixel); an exception there clears
the drawing and warns instead of ending the session. The card effect (`groomLabCardSurface`: Maya's hair
tube graph with its color fragment swapped for `groomLabCardColor`, the baked map times the card's
tint from the fcolor stream, cut out by the coverage, kept as the texture shrinks and hashed per
pixel past a texel a pixel, the structural color mixed in where the bake's iridescence says, a
second texture `irMap` made from the Color map's alpha (one texel of none when the map is RGB),
and its geometry terms for `groomLabCardShaderGeom`, which reads the card's normal from the
bitangent stream: two-sided lighting turns Nw round on a back face, a strand's lines have no
back) carries Cg, HLSL and GLSL implementations, so it draws on DirectX 11
and the OpenGL Core Profile alike (the GUI sessions' warning gate fails on a fragment that did not
register). The textured cards and Viewport 2.0's passes (Maya 2022, found with draw callbacks
logging each pass and captures of the live view, issue #11): the color pass draws the item's own
shader; the shadow pass calls the item's callbacks and draws with the item's shader too (an
effect's single pass, whatever its output: GroomLab's hair's `HairCard` technique takes `cardPass`
from `cardPassPreDraw` and writes the shadow map's depth there, or a point light's distance over
`LightRange`; Maya's hair graph keeps its alpha test, and a parameter updated from a shadow pass's
callback on the graph left the scene without shadows, so `cardCoveragePreDraw` sets the graph's
`alphaToCoverage`/`alphaTest` in color draws only and puts the alpha test back after each); the
SSAO normal-depth pass calls nothing of the item's and draws its own `SSAO_NormalDepth` effect
(on DirectX 11 it skips GroomLab's hair's card effect, on OpenGL and for Maya's graph it draws the
whole quads), so the textured cards are excluded from the post effects (`setExcludedFromPostEffects`).
Effect pass annotations (`drawContext = "shadowPass"`, the dx11Shader/glslShader plug-ins'
convention) are not honored for an `MShaderInstance`: every pass draws, in order. With the
viewport's multisampling the card effect writes a coverage mask (`cardMask`: a ramp a pixel wide
across the cutoff, the coverage below a texel, `lineSampleMask`'s run turned by a hash of the
pixel's place on the card); Maya's graph, whose fragments cannot write the mask, gets an alpha to
coverage blend state and `groomLabCardCoverage` after its last fragment sets the alpha.
`groomLabGraphStats -drawTimes` counts the cards' color draws (`cardDraws`: `sampled`,
`tested`). Anything else that must draw everywhere uses stock shaders: the map shown on the scalp draws through `k3dSolidTextureShader` (kind 8)
and the flat strand lines with width as coverage off through `k3dSolidShader` and `k3dCPVSolidShader`
(kinds 0 and 1). Render item kinds in `ItemBuffers::shaderKind`: 0 solid,
1 per-vertex colors, 2 lit hair (`groomLabHairTubeSurface`: Maya's hair tube graph as it is),
3/4 thick lines, 5 shaded surface, 6 textured cards (the card effect), 7 overlay lines (guides,
parts, flow drawn over the hair through their own effect), 8 an item's own texture, 9 lit hair in
the strands' own colors (`groomLabHairSurface`). Both strand graphs take `groomLabHairTubeSpecular`,
Maya's specular fragment patched at run time without its bitangent varying (it read the stream
only as the tube's direction when `tubeDirection` is 1; GroomLab's tubes run along the strand), so
the lit strands upload three streams a point (positions, normals, tangents: 36 bytes) and their
`uvCoord` as (u, width) (`StrandLines::writeUvs(out, 2)`: the core's u root to tip and the point's
width, `kNoLineWidth` for strands without widths), kept while the point counts and the widths
(`StrandLines::widthsKey`) stay; cards and scales keep the bitangent stream for their shading
normal.

**Width as coverage** (`displayWidthCoverage`, on by default; display.h `lineCoverage` and the
hashed coverage's functions, the reference the tests check): the strand lines stay a pixel wide and
each point covers the share of the pixel its width covers, the width times `ribbonWidthScale`
projected to pixels (half the viewport's height times the projection's [1][1], over the clip w:
the view depth, or 1 orthographic), clamped to `kLineCoverageFloor` and 1 (0 since 2026-10-09: the
0.05 it was drew a tip of 0.008 pixels six times too dark, 3 to 7% more ink far away and twice
the color error; docs/research/clump_lod.md), then taken to
the render's density with `displayCoverageDensity` (`lineCoverageAtDensity`: 1 - (1 - c)^(1 / d)
at the preview density d the view was evaluated at, `ViewResult::density`, the shader's
`lineDensity`; 1 off). The coverage is drawn as hashed alpha testing (Wyman and McGuire 2017),
never blended across strands: each fragment is kept, opaque, where its coverage reaches a
threshold (`lineHashThreshold`) hashed (pcg3d) from its strand and the stretch of the strand it
lies on. The strand rides in the lines' uvs: `StrandLines::writeUvs` with two floats writes u plus
twice a number from the strand's ID (`lineIdOf`, 1024 of them: u keeps 13 bits below the point;
`lineUvU`/`lineUvId` take them apart; `widthsKey` covers the IDs), so the pattern belongs to the
strand at any preview density and from evaluation to evaluation. The stretches are a pixel long
along the strand (u's change from pixel to pixel, the derivatives), the hashes of the two powers of
two around that blended and the blend's distribution made uniform again, so nothing pops or swims
as the view zooms; nothing of the screen goes in, so a still view draws the same pixels frame after
frame and an orbit carries the pattern with the strands (the GUI session's checks: the same frame
twice identical, a 0.2 degree orbit changing under 1% of the pixels where blending changed 15%).
n strands crossing a pixel then cover 1 - (1 - c)^n of it in any order, depth written: a dense
groom seen from afar keeps 93 to 98% of the ink opaque lines give it (flat; lit 75 to 86%) where
blending with depth writes kept about half (the front strand hid the rest). On a multisampled
target the flat lines' fragment writes a coverage mask (`SV_Coverage`, `gl_SampleMask`;
`lineSampleMask`: floor(c x samples + 1 - threshold) samples, a run turned by the strand's hash, so
strands crossing a pixel fill different samples), which reads smooth. With one sample a pixel the
top of the range blends instead (`lineKeepShare`: kept with share c up to `kLineHashedBelow` 0.1,
rising smoothly to every fragment at `kLineBlendedAbove` 0.4, drawn with alpha c / share): a strand
covering much of a pixel is one seen close, where strands seldom share a pixel and an opaque
stipple reads as noise, so it draws smooth; where they do overlap the blend's depth order costs a
little (a dense groom seen close keeps 91% of its ink, flat). With one sample a pixel the threshold's
stretches are about four pixels long (`lineHashThreshold` with `samples` 1), so a thin strand shows
as short dashes instead of scattered dots (not one threshold a whole strand: the 1024 strand
numbers made strands drawn over each other come and go together); the lit lines' graph fragment,
kept or not as a whole, learns the target's samples from its draw callback (`lineSamples`). The shaders'
side of all this is one source, `engine/gpu/shaders/line_coverage.glsl` (in GLSL's words, renamed
for HLSL as `hair_bsdf.glsl` is): the app's line shaders and visibility rasterizer `#include` it,
Maya embeds it at build time (`kLineCoverageText`) into the lines and hair effects and the lit
lines' coverage fragment (Cg's float hash aside). The flat and colored strand lines
(kinds 0 and 1) draw with the lines effect (`kLinesEffectHlsl`: `technique11`, `ps_5_0` for the
coverage mask and `GetRenderTargetSampleCount`; `kLinesEffectGlsl`: OGSFX at `#version 430` for
`gl_SampleMask` and `gl_NumSamples`; techniques `Plain` and `Colored`), which makes the coverage in
its vertex stage and hashes it per pixel. The lit lines' graphs (kinds 2 and 9) take
`groomLabStrandCoverage` after `mayaComputeSurfaceFinal`: the coverage per pixel from `Pw` and the
`viewprojection`, `projection` and `viewportPixelSize` semantics, the fragment discarded below the
threshold of its share and its alpha times c / share; a graph fragment cannot write a coverage
mask (DirectX 11's pixel shader output is Maya's; the GLSL Maya 2022 composes is older than 4.00),
so the lit lines are kept or not per pixel on multisampled targets too. `lineCoverage` 0 leaves
the fragment alone, which the ribbons, drawn with the same graph, keep. The uvs reach it and the
color fragment through `groomLabUvPassThrough` (a pass-through whose own input carries the
`uvCoord` semantic: Maya's float2 pass-through, whose input has none, left the stream out of the
shader), the color fragment through `groomLabLineUv` (u alone, the strand's number taken off: the
root-to-tip ramp). The Cg implementation hashes with floats (Cg has no unsigned integers). The
items stay in the opaque pass, depth written (the head, the guides and the hair's shadows and
ambient occlusion are as before), and their shaders are acquired with draw callbacks
(`coveragePreDraw`/`coveragePostDraw`) that, in a color pass, set an alpha blending state of their
own around the draw (for the blended top of the range; the hashed fragments are opaque) and put back
the one they found. `groomLabGraphStats -drawTimes` counts the draws of each kind
(`coverageDraws`: `hashed`, one sample a pixel; `sampled`, multisampled). The hashed test's
discard costs early depth, which the viewport suite's orbit of 200k lit strands does not show
(on 2026-10-08: 333 fps with it on, 338 off on OpenGL; 552 and 514 on DirectX 11). Turning it on or off writes the strand lines again (the flat
lines take a uvs stream then); partial writes, recolors and the GPU preview's bindings carry the
uvs (`addStreams`); a Width the GPU preview runs writes its widths into the uvs' second float
(`LookTargets::uvs`, look.slang's `UvWidthsCS`, through `kShown` for a preview core; in place:
the item's own uvs on OpenGL, the preview's own uvs buffer on DirectX 11, its u and the strand's
number written from the CPU when made). The app's line path does the same in its shaders
(`strands.vert`, `strands_colored.vert`, `lines_lit.vert`: the pixel scale and the widths' scale in
the uniforms' `viewport.zw`, the density and the target's samples in `lines.xy`; `strands.frag`,
`lines_lit.frag`: the hash, compiled again under `LINES_SAMPLE_MASK` as `strands_mask.frag` and
`lines_lit_mask.frag` for the viewport's multisampled target, all of them at GLSL 150 on OpenGL for
the unsigned integers, `GL_ARB_sample_shading` for the masks), with blending for the top of the
range; `OffscreenView` takes a sample count so the app tests draw as the viewport does.

**Screen-space level of detail** (`displayClumpLod`, on by default, and `clumpLodError`; the app's
Display panel's Clump LOD, `DisplaySettings::clumpLod`/`clumpLodError`, `--display clumpLod=off`;
core `strand_lod.h`, the research and its numbers in docs/research/clump_lod.md). The viewport
only: renders and exports never see it. With width as coverage on, the strand lines (the lit or
flat strands' item, or the colored strands') draw each clump's strands by class: a group (the
strands' `clumpId`; without one, cells of their roots holding about 256 strands) of n strands keeps
its first m by the hash of their IDs (the preview's own order, `keepAtDensity`: fewer is always a
subset of more), each standing for n/m, its coverage widened in the shaders to 1 - (1 - c)^(n/m)
from its own c (`lineLod`, the app's `lines.z`, divided into the preview's `lineDensity`; no
floor). m follows the research's two rules: the widened coverage stays under c* = 2 x
`clumpLodError` (0.175: widening's bias, about half c* relative, stays within it; m >= n ln(1 - c)
/ ln(1 - c*), c the members' mean true coverage) and about one line a pixel across the group's
spread (4 sigma). n/m is rounded down to a class a quarter octave apart (`lodClassExponent`, 32 of
them) and m taken as round(n / exponent), so a class is a draw with one exponent. A group whose
members spread under 0.75 pixel about their axis past the root draws as one line instead (an
impostor: `StrandLod::impostors`, the members' mean point by point, as wide as the ink of a
Gaussian band of Beer-Lambert fibers, `lodBandInk`, at the render's density, in the members' mean
color, `impostorColors`). Each axis point is taken at the pixels a unit spans where it comes
closest as the camera orbits its pivot at its distance (`StrandLodView`: the level, eighth octaves
with hysteresis, of the scale at the nearest depth the strands' bounding sphere's center comes to,
the pivot's depth less its distance to the center, `farLevel` at the farthest, plus that distance,
and the projection's pixel scale). The pivot is the point the host's camera turns about: in Maya
the tumble tool's (`tumbleCtx -localTumble`, asked once a second: 1, the default, the camera's
center of interest; 0, "tumble on recent selection", its tumble pivot; 2 its axes offset by the
tumble pivot), in the app the orbit camera's target; without one the sphere's center. So a tumble
about any pivot changes nothing and a dolly changes it in steps (a pivot the host is not told of,
`orbit -pivotPoint`, still can); a pivot the center can swing to the eye's plane about (one close
in front of the camera, the strands behind it) gives no level: every strand drawn. At the
same view a group keeps its class within a quarter class (a dab, a tweak), at another view the
rules alone choose (the same picture whichever views came before). Thinned strands are hashed
alone (`lineLod` > 0: `lineKeepShare` with own 0: they stand for several, no blended top). The
light streaks of blended fragments writing depth (a strand of coverage 0.1 to 0.4 hid the strands
drawn after it behind it: 2.6% of the coverage at mid distance) are gone where strands thin, and
for class 0 the segments that can blend at any orbit at this distance (`splitBlended`,
`kLodBlended`) draw twice: their opaque fragments with the rest, depth written (`lineBlend` 1),
and after everything their blended fragments alone, depth tested and not written (`lineBlend` 2:
Maya's transparent pass, its depth state set by `coveragePreDraw`; the app's `noDepthWrite`
pipelines). The other segments draw once, every fragment, so a coarser view drawing the same lists
(a playblast smaller than its panel) never loses a fragment. On a multisampled target the flat
lines write masks and nothing blends (no list). In Maya each class is an item of its own
(`groomLabStrandsLod<k>`, `groomLabStrandsLodBlendOpaque`, `groomLabStrandsLodBlend`,
`groomLabStrandsLodImpostors`) binding the streams the strands' item was last given (recorded by
`bindGeometry`: a render item's `geometry()` answers what it drew last), bound again only when
they change (a staged buffer bound again is sent up over what a GPU preview wrote into it), its
shader made as the strands' was and styled as theirs; the strands' item is off meanwhile. The
view is each camera's (and viewport size's) from `requiresUpdate`, the finest of those seen in the
last two seconds (several panels draw the same items); another level asks for an update, and a
class's indices (and its item's binding) are written again only when that class's segments change
(`Selection::slotKeys`), counted and written in parallel chunks (`slotSegments`, `writeSegments`). The impostors are the CPU's strands' axes:
off while the GPU carries the strands (Follow Scalp) or a GPU preview moves them (the optionVar
`groomLabLodImpostors` 0 turns them off for a look). Ribbons are not thinned (their width is
geometry, and at the distances the level of detail pays they are under a pixel). `groomLabGraphStats
-drawTimes` says what the last update drew (`lod`: on, level, drawnStrands, totalStrands, groups,
clumped, thinnedGroups, impostors, blendSegments, items, ms, indicesMs, megabytes, updates,
rebuilds, selectMs, slotsWritten, farLevel; `coverageDraws.blendItem`). Checks: `StrandLod.*` (the
view, orbits about off-center pivots, dollies, the rules, nested subsets, determinism, hysteresis,
segments and their slots' keys, impostors), the opt-in picture test `core.lod_pictures`
(`ctest -L pictures`: the research harness in `--check` mode at 1M strands, the hosts' drawing
within 2% of the physical reference's ink and its error no worse than every strand's at the four
framings), `AppRender.ClumpLodDrawsAShareOfTheStrandsWithTheirInk`, and the viewport suite's
`lod_check` (`GROOMLAB_VIEWPORT_CASE=lod`; near to very far, a still view, an orbit writing no
indices, full tumbles about four pivots writing them at most once, a dolly in steps, colored
lines, MSAA, the groom selected, a GPU drag, ribbons, off) on both devices; the viewport
benchmark's part `lod` (`GROOMLAB_BENCH_PARTS=lod`, `GROOMLAB_BENCH_LOD_MODES`,
`GROOMLAB_BENCH_LOD_SCENE` and `_DENSITY` for a scene of one's own) times it at near, mid, far and
very far, and its part `lodorbit` (`GROOMLAB_BENCH_ORBIT_SCENES`, `_PIVOTS`, `_FRAMINGS`,
`_FRAMES`) counts the rebuilds and times the frames of full tumbles about each pivot.

A thread cap for the core's loops (`GROOMLAB_THREADS`, `setParallelThreads`: parallel.h)
caps GroomLab's TBB loops in an arena of their own, never Maya's.
The overlay effect is written once per language Viewport 2.0 takes effects in, picked by
`MRenderer::drawAPI()` (`overlayEffectText`): HLSL for DirectX 11, OGSFX (GLSL stages in an
effect frame, as Maya's `bin/OGSFX/*.ogsfx` and `presets/GLSL/examples`) for the OpenGL Core
Profile, which parses nothing else (CgFX failed there from the start, so guides drew
depth-tested on OpenGL until it got its own), and CgFX for legacy OpenGL (`VirtualDeviceGL`,
which compiles it). The HLSL and OGSFX ones widen the lines to `gLineWidth` in a geometry
shader (the CgFX one draws them a pixel wide), and draw the same pictures pixel for pixel; the
viewport suite's `overlay_check` counts guide pixels over dense strands on each device. Effects are compiled from text in
memory (`getEffectsBufferShader`), fragments registered from XML (`MFragmentManager`): a
fragment needs an implementation per language Maya composes graphs in (Cg, HLSL and GLSL; see
below). A shader that does not compile warns `GroomLab: the ... shader did not compile`, and the
GUI sessions fail on that warning on either device.

**The lit strands' colors.** In the solid lit look each strand shows its own color (the core's
`strandLookColors`: the color nodes' per-point `colorR/G/B` (a tip, Bands, FeatherColor), else
the per-strand ones, else the melanin's color; `DisplaySettings::strandLookColors`, off when a material or the
display color colors every strand, except an `aiStandardHair` made from the groom by
`create_hair_shader`, which reads the same channels: see "One hair color for every host"). Where they vary, the lit lines (and lit
ribbons) carry a colors stream (`StrandLines::colors`) and draw as kind 9: `groomLabHairSurface`,
Maya's hair tube graph with its color fragment swapped for `groomLabHairColor` (Maya's color
times the root-to-tip ramp, times the vertex color: a varying input of semantic `fcolor`, which
Viewport 2.0 feeds from the item's `kColor` stream), so lights, highlights, translucence, shadows
and post effects stay Maya's. The fragment carries Cg, HLSL and GLSL implementations (OpenGL Core
Profile composes GLSL; so does the card fragment). The colors' alpha is 1 plus the point's
iridescence (FeatherColor's channel; 1 without: a reader of it as opacity sees what it always did),
and the fragment mixes in the structural color by it, its hue turned by the angle between the
view and the strand's normal: the graph's `Vw` and `Nw` reach the color fragment through
`mayafloat3PassThrough`s that also feed `mayaShaderGeom` (`registerHairGraph`). A property of
its own with a stream's semantic (a second `tangent`, say) made Viewport 2.0 keep a copy of that
stream which partial writes missed (the lit strands' highlights went stale after a stroke), and
a pass-through of the tangent drew nothing on DirectX 11 and hung OpenGL: the normal it is (a
feather's vane normal, else the root's, the strand frames' `up`). The stream is RGBA floats, as the
fragment's input asks (`requiredVertexBuffers`: kFloat x 4): tried as RGB floats or RGBA bytes,
Maya 2022 drew nothing or hung. So it costs 16 bytes a point (500k strands x 16 points: 128 MB,
and the system-memory copy under DirectX 11). Uniform colors (and grooms without color nodes) carry no
stream and draw as kind 2 in that one color. `StrandColors::key` hashes the colors, so a whole
write whose colors did not change (a tweak moving the strands) keeps the stream, and a partial one
writes the changed strands' colors with their points only when the key moved (a dab moving strands
whose colors depend on their points). A GPU preview's drag binds the item's colors stream beside
its own points (the color nodes pass the points on: the colors are the CPU's last evaluation's).

**GroomLab's hair shading** (`displayHairShading` 1, the default; `hair_shading.h/.cpp`). The lit
strand lines and ribbons (kinds 2 and 9, `ItemBuffers::hair`) draw with an effect of GroomLab's
instead of Maya's hair tube graph: HLSL (`technique11 Hair`, `HairColored`) on DirectX 11, OGSFX at
`#version 430` on the OpenGL Core Profile, the shading written once in the words both share
(`kHairShared`, the vector types renamed for GLSL by `glslOf`). The lobes are the app's, one
source (`engine/gpu/shaders/hair_bsdf.glsl`, embedded: see "One hair shading"). The fiber's color
is the strands' hair color (the item's, or the colors stream: HairColor's RGB, a material's base
color, an aiStandardHair's melanin as `melaninColor`) absorbed as aiStandardHair absorbs its base color
(`hairColorSigma`, the `hairAzimuthalRoughness` uniform); HairColor's specular, roughness and tilt as `create_hair_shader`
maps them (core's `hairLobes`: the `hairWeights` and `hairLobes` uniforms). The lights are Viewport 2.0's, read from the
draw context in the shader's pre-draw callback (`hairPreDraw`: once a frame and pass, up to 8, sent
only when they changed, then `updateParameters`): kinds by `lightType()`, ambient lights summed,
light linking from the lightLinker nodes' ignore and link entries naming the groom or its shading
group, and lights outside `defaultLightSet` (`ignoredLights`, looked at again once a second on
an update; `MLightLinks` resolves neither for a plugin shape, and `lightlink` takes only the
shading group). Output is linear, times pi (`hairExposure`: a VP2 Lambert shows color x light),
Maya's color management turning it to the view. The coverage is the lines effect's (hash, keep
share, a coverage mask on multisampled targets, `lineLod`, `lineBlend`; the level of detail copies
the shader as the strands' and so splits blends as the flat lines do). Per strand: two hashes of
the strand's number in the uvs (`rnd`, `rnd2`; DirectX 11's effects compiler gave 0 for fractions
of sums of hashed floats; the lit ribbons' uvs carry the number too, `RibbonLayoutCS` copying the
lines' u, `strandRibbons` on the CPU) vary the highlight's tilt, width and strength and the pigment
(`displayStrandVariation`, plus HairColor's `specularRandom`/`roughnessRandom`, `HairLook`).
`groomLabHairDebug` (optionVar) 1 shows the tangents, 2 each strand's random numbers, 3 the
structural color's hue turned by each strand's random number (`hairHue`'s fractions, every hue on
either API: `gui_hair.py`).
Self-shadowing (`displaySelfShadowing`; B8.2): after each update that moved the drawn strands
(written, carried for Follow Scalp, a GPU preview's), `updateHairGrid` has the line engine splat the
points as drawn (the items' own GPU buffers, a preview's, the ribbons' lines) into a grid over the
groom's bounds, cells three strand spacings at the render's density (16 to 192 a side, 4M cells at
most), `line_frames.h runDensity` (`shaders/density.slang`: clear, a trilinear splat of (pi/4) x
width x the mean segment a point at the render's density, gathered per thread group in a
groupshared table of the cells it touches before the grid's atomics, once a cell: those were the
grid's cost, 2.9 ms on the GPU at 1M strands on DirectX 11, 1.1 now, 3.6 and 1.6 on OpenGL,
`test_line_frames.cpp`'s `DISABLED_DensityGridTimes`; the same integers, `DensityGridIsTheCpus`),
and a resolve writing (density, six-axis occlusion) into a texture the effect samples: on DirectX 11 a texture of the engine's copied into
Maya's `MTexture`, whose resource handle is its shader resource view; on OpenGL Maya's texture
written as an image). The shader marches toward each light in 1.5-cell steps (at most 96) from half
a step plus a per-strand jitter out, for `direct = exp(-count)`, the dual scattering's forward
spread and the ambient occlusion of ambient lights. Where the drawn buffers are not ones compute
can read (DirectX 11 with the GPU frames off) there is no grid (`groomLabGraphStats -hairShading`:
`grid.why`). `groomLabGraphStats -hairShading` gives the last draw's lights and the grid. Checks:
`gui_hair.py` (in the viewport suite on both devices; an opt-in suite `hair` on its own;
`GROOMLAB_HAIR_COMPARE=1` draws the app's head under the app's key light with the app's camera for
side-by-sides with `tests/data/app/arnold_compare.py --lights key --ocio-linear` and the app's
`--lighting key --tonemap none`). The cards and scales keep Maya's hair tube graph.

**GPU look preview (on by default).** With Viewport 2.0 on DirectX 11 or OpenGL, a drag of a
number of a node the GPU runs (Interpolate's radius, Length, Clump's strength, flyaways, lock
radius, repel radius, volumize, flatness and per-clump randomness, Noise, Curl (a coil's
coilWidths too), Width, Part's strength and width, Gravity's strength, stiffness and stiffness
profile, Flyaway's numbers and seed, Curl's weightDirection: the Graph Editor's `preview_param`, or
`groomLabGpuPreview -node n -param p -value v <groom>`, `-ramp` a profile's keys, `-vector x y z` a
vec3 param; the app's livePreview the same, `gpuOverrideFromJson`) evaluates nothing, and neither do a brush stroke's dabs on guides
after its press: the draw override runs the graph from the first node that changes to the output,
and the lit lines' frames, as compute kernels (`engine/gpu/shaders/look.slang`, see GPU kernels above;
the host-free engine is the `groomlab_gpu` library in `engine/gpu/`, `look_chain.h` and `look_engine.h`,
one run algorithm over a backend per API, and `look_preview.h`, what every host's preview shares: the
plans and why a run cannot happen, a stroke armed and its dabs held, the base uploaded when it changed;
Maya glue in `hosts/maya/src/gpu_preview.cpp`, the standalone app's in `hosts/app/src/gpu_preview.cpp`,
which also takes the app's point texture as a target: `LookTargets::pointTexture`, written by
look.slang's `PointsTextureCS`) on Viewport
2.0's own device, from the strands entering that node (the last evaluation's cached result,
`Graph::cachedValue`, uploaded when it changes), straight into what the strands item draws.
`findLookChain` walks up from the output through the nodes the GPU runs, in any order, and those
handing their input on as it came (Output, a bypass, a Switch: the input it picks, an even
Resample with nothing to do, a Clump without centers), to the first node it does not run; the run starts at the first node a drag's
values change (`firstOverridden`, `chainFrom`: the nodes before it keep their cached results), or
at Interpolate for a stroke. An Interpolate keeping clearance (`keepClearance` with a scalp, its
default) runs on the GPU too: its walk takes the scalp's distance field's answers alone
(`MeshDistanceField::Probe::fieldOnly`: where the field does not blend, the closest point among the
triangles around the closest points it holds there), which the GPU reads from the field's tables
(`MeshDistanceField::tables`, made in full and uploaded once per scalp: tens of MB for a head,
about half a second to make the first time), in a kernel of its own after the blend (look.slang
`ClearanceCS`: the walk, a thread a strand, lifts points just onto the skin and eases them back to
their shaped path past a bulge, the clearance by the strand's influence; it takes its group's
points into groupshared memory a window at a time, side by side; where
the field does not blend, the closest point is found from the mesh, `answerNear`, which skips the
triangles at corners it already scanned, on the CPU as on the GPU: about 2.4 times faster on a
creature's fur walk; and those whose sphere, `MeshDistanceField::TriangleBound` (in the tables:
8 words a triangle), is farther than the nearest so far, half of those it weighed on the wolf: the
walk's `answerNear` part about a quarter faster on the CPU (the closest-point test inline too), the GPU's
clearance kernel 13-15% on both APIs, every answer the same to the bit). The engine keeps every strand's walked points; a run that changes only
guide shapes (a brush dab: the base, binding, field and params as they were) walks only the strands
bound to a guide it changed, the rest taken as kept (`LookEngine::walkOnlyChanged`, flags beside the
guides' rows; `GROOMLAB_GPU_WALK_ALL=1` walks them all, for comparisons). Over
a scalp deformed away from its rest pose the walk asks the rest pose's field vertex by vertex
(`RestPoseCollision`), on the GPU too: the rest field's tables, then the walk's words after them
(`RestPoseCollision::walkWords`: the deformed vertices, their rings, each vertex's motion to rest
and the vertices the tree holds), each strand's cursor at its root's vertex in Interpolate's
per-strand numbers; look.slang `walkPush` walks to the point's nearest vertex over the edges (64
steps, else the nearest of every vertex, as the tree finds it), takes the point to rest by that
vertex's frame, pushes it there and turns the push back, as the CPU rounds it. A degenerate face's
vertices have no frame (the CPU asks the mesh there): where the strands' walks can reach one
(`LookInputs`: within four times the longest guide, three longest edges and the search of a root),
the stroke is the CPU's. Interpolate and Clump run from what the CPU's operators keep for their
cached results (`engine/core/include/groomlab/operator_state.h`, read by `LookInputs`): Interpolate's
binding (each strand's guides, weights and twists, the turns its paths add round a fold, 16 guides
a strand at most on the GPU; the weights 16-bit shares of 65536 that the CPU blends too, so the GPU
takes variable-length records, a word a guide, two words of header a strand: about 37 bytes a
strand against 208 before; where a dab moved guide
roots, children following their bent parents, the strands are bound again on the CPU as
Interpolate binds them, `interpolateRebind`, and only the binding's chunks that changed go up,
where they were when they fit, else into the room the buffer keeps after them),
the guides' shapes as Interpolate makes them (`interpolateGuideRows`), Clump's center choice and
resampled centers, and the strands its curves are (the centers it chose, or with `centersFrom`
`inputPoints` each clump's axis strand, `ClumpState::axes`; an input mode with nothing linked
is the CPU's): centers made from strands the run moves are
resampled (and given lock frames) on the GPU, as is `pastTip` `extend` (`ClumpCS`, flag 1).
With `centersFrom` `inputCurves` the centers may come from a branch of their own that a stroke
moves too (a creature's medium and big clumps: a sparse Scatter's strands through another
Interpolate of the same guides, then a Noise): that branch is a chain of its own
(`findLookChainTo` the node feeding the centers, from its first node the run changes; its base,
the Scatter, held), read by a `LookInputs` of its own into `ClumpInputs::centersRun`, and run
first by a sibling engine on the same device sharing the kernels (`LookEngine::sibling`); the
Clump's `CentersCS` reads that run's points and offsets (`LookEngine::borrow`, `Binding::offsets`)
for the strands the CPU's centers are (the preview core of the centers input, by
`ClumpState::centerGuides`). On the wolf groom (120k strands) a comb dab went from 229 ms on the
CPU to 34 ms in Maya (DirectX 11) and from 164 to 46 ms (lookdev) in the app.
`GROOMLAB_GPU_CENTERS_BRANCH=0` leaves such centers to the CPU, for comparisons. Operators hand that work on with their results when
a graph text is set again or cloned (`Operator::adoptKeptWork`), so an edit elsewhere in the graph
does not drop it. Maya 2022 gives no vertex buffer compute may write under DirectX 11 (they are
system-memory staged there), so the preview makes its own (`createD3D11DisplayBuffer`: vertex
buffers with unordered access), hands them to Viewport 2.0 as custom resource handles
(`MVertexBuffer::resourceHandle(handle, vertexCount)`; Viewport 2.0 draws them and takes no
reference) and binds them to the item for the drag; nothing is copied. When the drag (or stroke)
ends the item binds its own buffers again (the CPU's evaluation writes them) and the preview's go
(`GROOMLAB_GPU_PREVIEW_COPY=1` copies into the item's own buffers instead, for comparisons).
Under OpenGL the item's own buffers are GPU-only plain GL buffers (`resourceHandle()` points at
their names): the kernels bind them as shader storage and write them in place, with Viewport
2.0's context current on the main thread (the engine saves and puts back every binding it touches,
and checks it can still reach its objects from the context current, else it is made again); when
it stops, the item is written whole from the CPU's (`DisplayBuilder::forgetHeld` for a stroke's
dimmed strands). The release ends the drag (`-endDrag`) and commits on the CPU, which stays the
reference for everything else. Profiles go as the CPU evaluates them (the keys with Ramp's
tangents, and RampAlong's own table by point index where every strand has the same count; a
plugged Curve too: plugged fields and curves are cleared from a node's params after each
evaluation, so `look_chain.cpp` takes them from their sources' cached results,
`Graph::cachedValue(id, output)`). Mapped params (Length's keep, Clump's strength and volumize,
Noise's amplitude and fraction, Curl's radius, Width's width, Part's strength: a texture, a painted
vertex map, a channel, an expression, a plugged field) go as their own `ScalarField`'s values on the
strands each node takes, per strand or per point, read once per evaluation (`readLookFields`) and
multiplied by the dragged number where a field is plugged (as the CPU multiplies it by the
param's). What each reads is weighed against what the run changes before its node (`whatItReads`:
a channel a running stage writes is one its cached result holds in another array than its cached
input): values of the roots, IDs, rand, index and channels nothing before it writes (a texture, a
color set, `$ny`, a Clump's clump numbers) hold whatever moves (`FieldValues::fixed`); channels a
running Interpolate blends from its binding (guide attributes) hold while it is not bound again for
moved guide roots (`binding`, checked by `LookInputs::read`, which knows). A field of the strands'
shapes (`$length / $longest`, reach, straightness, lift, tip: `ScalarField::readsPositions`) or of
the guides' influence a running Interpolate makes (its `influenceChannel`) is made again on the GPU,
per strand, from the points the run hands its node (`FieldProgram`: the expression's reverse
Polish ops, `Expression::program`, with what does not move per strand read by the CPU,
`Expression::strandVariablesInto`; `FieldCS` runs them into the mapped values before the node's
kernel, `LongestCS` first for `longest`, the longest of the core strands; `InterpolateCS` writes
each strand's influence for them, 0 where unbound, as the CPU's channel): numbers alone (no vectors,
noise, curves, picks or maps), at most 16 deep. Otherwise a value read from the points holds only
on the run's first node, and for Clump, Noise and Curl not per point (they read each point after the
ones before it moved): `lookFieldsHold` answers false. Width varies each strand by its draw (every stage
has a block of per-strand numbers now). Noise runs its kinky mode (two knots' noise per point;
in world space by each strand's arc length, so a thread a strand). Interpolate runs 16 threads a
strand (`InterpolateCS`), a guide sample each: the strand's guides' turns made once into groupshared
memory, the arc lengths summed in order by the strand's first thread, each thread then resampling
its points. Its arithmetic, and the clearance walk's, is rounded as the CPU's (no fused
multiply-adds; square roots and divisions correctly rounded, `divExact`, `sqrtExact`): the walk
decides on its points (a push or not, which cell, the field's blend or the mesh's answer), and a
rounding either side of one moves a strand by the field's own error. Noise (but that), the lines'
frames and the app's point texture run a thread a point, strands of one point count or not (each
point's strand from a table the engine makes on the GPU once per layout); a dispatch past 65535
groups (16.78M points) runs as rows of them (`threadIndex`). Curl runs its helix, wave and zigzag (the phase in the
constants, the plane a fourth draw per strand) and its coil (a helix coilWidths strand widths
across: the widths the strands carry go as the radius's values per point, or where a Width before
it runs in the same run, the widths that Width's kernel made, bound in their place; a coil by clump
on widths the run makes is the CPU's). Curls coherent by clump (`coherence` `clump`, ringlets) take the clumps as the CPU
finds them on the strands Curl takes (the clump channel, each clump's core strands, their root
normals summed and each clump's draw: `CurlInputs`, fixed through a run), and `CurlAxesCS` makes
each clump's middle from the points the run hands it (the mean of its core strands at 32 shares of
their points, its rotation-minimizing frames, its arc lengths), which `CurlCS` curls each member
with. Weighted curls (`weight`) take the stretch at each of the axis's samples, as `Axis::weigh`
makes it: `CurlCS` walks a strand's own axis to its tip once before curling (its hang and its
unweighted turns at each sample, a local array of 32), `CurlAxesCS` a clump's after its frames
(the stretches a row of the clump's table); the profiled arc is then weighted by it on the same
walk, and the radius narrowed (`curlStretchedShare`). A Clump varies its clumps on the GPU too: each center's draws (strength, lock radius, a random
flatten's turn) from its ID and the seed (`ClumpState::centerIds`, drawn in `LookInputs::read`, a
seed dragged draws again), volumize along its own profile (a second ramp, `gRamp2`), flatness in
the centers' frames (made for a lock radius or a flatness; the thin way the scalp's normal at the
center's root, a random turn or across a direction), and copyAmount: centers the CPU made come
shared, centers made on the GPU are resampled into a buffer of their own and shared by `ShareCS`
(each center's nearest centers by the roots it was assigned by, found once on the CPU in its order,
`ClumpInputs::neighbors`). A Clump picking its own centers by a centerWeight that varies per strand
(`ClumpState::picked`, not by the roots alone) keeps that choice while the weight's values hold
(fixed, or the influence while the binding, radius and falloff are its cached result's: a drag of
Interpolate's radius then is the CPU's). A flat clump lying along the scalp on strands straight out
of it is a rounding's choice on either side (its thin way is the normal less its part along the
strand), as it is between two CPU builds. Part runs on the GPU (`PartCS`, `turnToward`), each
strand's side of the parts from its root on the rest scalp read once on the CPU (`PartInputs`: the
way away and the distance; the falloff by the dragged width on the GPU), so brush dabs on a parted
groom go through it. A Clump's negative strength (a repel: `look_ops.cpp` `repelled`, fur parting around
whiskers) runs in `ClumpCS` too, so a drag from a pull across 0 into a push stays on the GPU: the
kernel checks the strand's points for a negative weight first, then makes each point and keeps
the segment lengths as it goes (follow the leader, a segment nothing moved left bit for bit, as the
CPU leaves it); the skin's normal is the root normals' block, its `w` saying whether the root is
on the scalp. The way out of a point on (or a rounding from) the center is blended in over a
hundredth of the repel radius rather than chosen by a test, so the GPU and the CPU never part
ways on a rounding (centers a Clump picks are strands: their points lie on their centers).
Gravity runs a thread a strand (`GravityCS`: its turns carried down the strand as a unit
quaternion, as `GravityOp` carries them; its direction a world vector, normalized on the CPU: one
per strand, an expression or a painted color set, refuses), as does Flyaway (`FlyawayCS`: each
strand's gate, noise offset and lengthening drawn on the CPU from its ID and the seed, two blocks
of per-strand numbers; the strays' noise the Noise kernel's `vectorNoise` with one octave; the
`flyaway` channel it writes is not made on the GPU, so a mapped param after it reading anything but
fixed values refuses). Collide (`CollideCS`) walks each strand as `CollideOp` does, pass by pass,
asking its surfaces' fields through their tables as the clearance walk asks the scalp's
(`MeshDistanceField::tables` of the fields the CPU asks at Collide's offset, made for every offset
those fields serve, the most a cell size takes plus the softness, at the field's reach: a dragged
offset is the kernel's number, the tables made again only where it asks another field; the scalp's
then a collider's in one buffer, `sTableBase` in look.slang naming whose; `CollideInputs`):
the field's answers alone (`Probe::fieldOnly`), every step rounded as the CPU rounds it, so it is
the CPU's `method` `field` walk (which takes the field's answers alone too) to the bit (DirectX 11;
OpenGL within an ulp or two; CollideKeepsStrandsOutAsTheCpuDoes). Where the field cannot blend
(deep under a surface, across a crease) that answer is the closest point among the triangles around
the closest faces the field holds near the point (a fine cell's corners', or a far brick's middle's
and those of the 7 bricks beside it on the point's side), then down the mesh while a triangle around
the nearest one's corners is nearer (`MeshDistanceField::kAnswerSteps`, look.slang `ANSWER_STEPS`):
the mesh's own answer but for near ties. After a stage the GPU rounds otherwise than the CPU
(Gravity's sine, cosine and exp, by about 2e-5), a point on the edge of one of the walk's decisions
(a push or not, the field's blend or its answer from closest faces) falls the other way and its
strand after it: no more than the CPU's own Collide moves for its input nudged that much, which the
test measures and bounds the GPU by. A
Collide whose surface is deformed away from its rest pose (asked through each vertex's frame) is
the CPU's: the GPU runs from the node after it; `method` `exact` refuses. Its offset and depth
make its tables: dragging them is the CPU's. A Jiggle handing its input back as it came (no
history of the scalp's motion, or a still scalp: the very strands it took) is handed on like a
bypass; one that moved them ends the run there; a Jiggle with motion stays the CPU's. Wind runs a
thread a strand (`WindCS`) at the time its cached result was made at (`windTime`, operator_state.h:
the run itself carries no time): how far its gusts travelled and its flutter's phase worked out on
the CPU in double as `WindOp` does, its gusts, sway and flutter the Noise kernel's lattice noise
(`latticeNoise`) with their own octave seeds, each strand's flutter phase drawn on the CPU from
its ID. A Wind blowing nowhere at its own strength (a plain 0) stays cached whatever the time, so a
drag of its strength is the CPU's. A Blend runs where its looks a and b are each a run of the GPU's
nodes up to where they part (the first node both come from: a stage, or the base) with no Width in
either (their widths alike, so the Blend leaves them): `findLookChain` lists look a's stages, then
look b's, then the Blend, each stage naming the one whose result it takes (`LookStage::from`, a
Blend's look b `from2`), and the engine keeps a third points buffer (`kPointsC`) for where the looks
part while look a runs, then look a's result while look b runs. `chainFrom` inside the looks starts
from where they part (both are made again). `BlendCS` / `BlendPointsCS` mix the points by the mix
(a number, a map, a Control) times the profile; its mix channel and the look channels it mixes stay
as the CPU last wrote them (a mapped param after it reading them refuses, as after a Flyaway).
SelfCollide and Static run as the hair grid in compute (look.slang's hair grid section): on the
strands the node took (its cached input) `LookInputs::read` reads the strands it splats (the core,
thinned by ID), their segments, the cell, what a sample carries and the size of the grid the CPU
lays on them (`hairGridLayout`, operator_state.h `HairGridLayout`), the scalp near the world's grid
nodes for that cell (`hairGridWalls`, once per scalp and cell: wallsOf's closest points) and the
floor's field as tables. Each run lays the grid again on the GPU (the samples' bounds by atomic min
and max, blocks marked where their cells' corners fall and numbered by an atomic counter: the CPU's
sparse layout), splats into 64-bit fixed-point sums added by 32-bit atomics carrying into the high
word (the CPU's integers, in any order), solves red-black SOR a dispatch a colour over a list of the
unknowns (`dispatchAlternating`, bound once), and pushes each strand (`SelfCollidePushCS`,
`StaticPushCS` with its cantilever bend), walked by one kernel for both (`GridWalkCS`: the floor's
push compiled once). The samples, their cells (the floor of the CPU's double quotient, decided by
the remainder) and the sums are the CPU's to the bit; the solve, the slabs' eigenvectors and the
pushes are float where the CPU's are double. From the node itself the GPU is within a few 1e-6 of
the CPU. From a node before it (the GPU's rounding of what it takes), and in SelfCollide's later
passes (which take what its first moved), a decision of the CPU's (a sample's cell, a node
overpacked or not, a slab or not, the grid's origin) flips with a rounding: the CPU's own result
moves as far for what it takes shaken by 1e-5 (up to 3e-3 at 8k strands, 2e-2 for SelfCollide at
100k), and the tests bound the GPU by twice that (`cpuShake`). The grid's buffers hold room for the
CPU's last grid half as wide again and 4 blocks more each way, twice its blocks and 256 more; a
drag or stroke spreading the hair past that loses the grid's far side until the release. A scalp
deformed away from its rest pose (the walk asks the rest pose's field vertex by vertex) refuses, as
does a node without a scalp whose strands the run moves (its cell goes by their size). Their params
(strength, packing, randomness, profile) change values on the grid alone: each drags on the GPU.
A stage masked by its affect (a map, a plugged field, a number between 0 and 1, a dragged affect) runs
as ever, then `AffectCS` mixes its result back toward what the node's first stage took, per strand
or per point, as `applyAffect` does (`StageParams::masked`; the engine keeps the node's input alive
till then, a Wet's past its levels; a Width's widths, those it took copied aside after another
Width). Its affect is the node's first stage's third field (`StageFields::affect`, read on what it
takes like the mapped params: fixed, or made per strand by `FieldCS`). This is the one way every
stage is mixed, the shapers reading their neighbours included.
Operators a host reads state from by their result (Clump, Wet, Wind, an auto Part, BlendNeighbors,
Tangle, Flow, Jiggle) follow the graph's mixed result (`Operator::resultMixed`). Noise masked by a Group costs about 0.15 ms more at 100k.
Jiggle with motion runs too: `JiggleOp` keeps the scalp, history and frame its result read
(`jiggleState`), and `jiggleLags` (its own arithmetic) makes each vertex's lag and face's turn for the
run's frequency, damping and history; `LookInputs` blends each strand's root lag as the CPU does and
`JiggleCS` turns the strand about its root (Cephes' atan2: fxc's put DirectX 11 ten times further from
the CPU); a history dragged past the frames sampled for it refuses. About 0.12 ms at 100k. MeshCut
keeps point counts, so it runs: `ScalpMesh::rayTree` hands the mesh's own tree (the answers a fresh
build gives) to `MeshCutCS`, which casts each segment down it (`rayOnTree`'s Moller-Trumbore, its
products rounded as the CPU's), finds the root's side by its closest point on the tree, and resamples
the kept part (`resampleByArcLengthInto`); about 0.45 ms (DirectX 11) and 0.75 ms (OpenGL) at 100k.
HairColor, Bands and FeatherColor hand the points on with their colors as the CPU last wrote them, except
a Bands picking its guard hairs by length (`guardsBy` `longest`): `findLookChain` refuses then,
since a run before it changes the lengths. Anything else (a Resample that changes the strands,
strands of more than 64 points into Interpolate, Interpolate taking the guides' length alone, a
followed scalp, a stroke of maps, parts or flow) answers false and the CPU previews. A preview below
its core share (preview_core.h) holds hidden core strands: the engine runs every strand of the base
(a Clump's centers may be hidden ones) into buffers of its own and gathers the shown strands' points
and frames into the viewport's buffers (`GatherCS`, the shown points' places in `kShown`), as many
points as the viewport draws; the app's point texture takes no such run (the CPU's then). The base
uploads in part by the graph's changes of the base as made (`changesSince(..., asMade)`).

A live stroke on the GPU: the stroke's press is the CPU's (it evaluates the stroke's document at
`sculptDensity`); once it is drawn the override arms the GPU (`armStroke`: the chain from
Interpolate, what the operators keep for that document, the item drawing the strands) and the
shape's evaluation of each later dab stops at Interpolate (`GroomShape::setGpuStroke`,
`ViewRequest::stopAt`: only the guides are evaluated, the view keeps its strands); right after it
`gpuStrokeHolds` checks the dab's guides fit what the GPU holds (as many, the same IDs, the parts
as at the press), else the dab is evaluated in full on the CPU and so is the rest of the stroke.
The dab is drawn by the GPU from the stroke document's base (Scatter's strands, uploaded once),
its binding and the dab's guide shapes; the release commits on the CPU, incrementally, as ever,
off the UI thread (Commits off the UI thread, below).

A stroke on a Sculpt node's guides (the brushes' sculpt target, `setPreviewSculpt`) arms the same
way (`gpu::strokeShapesGuides`: a guide preview, or a sculpt preview without parts or flow): each
dab's evaluation up to Interpolate evaluates the Sculpt node alone (the stroke's edits laid on its
cached input, the ToGuides above it untouched), and the GPU runs Interpolate and what follows from
the guides it hands on. Its edits turn segments and scale them, never moving roots, so
Interpolate's binding holds. A drag of a Sculpt node's `strength` makes its guides on the CPU at
the dragged value (`gpu::sculptedGuides`, its stored edits on its cached input, nothing of the graph
evaluated) and runs from the first stage they make (`LookInputs::read`'s `standIn`); guides that
reach Interpolate through another node (a Noise of the guides) make that drag the CPU's.

It is a preference: the Graph Editor's Preview Drags on the GPU (more menu),
`groomlab.gpu_preview(on)` (the optionVar `groomLabGpuPreview`, read when the plugin loads),
`groomLabGpuPreview -enable off` for the session; `GROOMLAB_GPU_PREVIEW=0` or `1` in the
environment forces it. During a drag the strands item draws with bounds grown by the farthest the
run can move a point (`lookBounds`, stage by stage: Interpolate within twice its guides' longest
offset of the roots, Clump toward its centers and their lock (and out by the repel radius
where a strength is negative), Length (it only trims), Noise's sum under
`kNoiseReach` per axis, Curl's varied radius (a coil's by the widths), Part and Gravity within
their strands' length of the roots, Flyaway past the tips by lengthen and out by its stray under
the noise's reach, Collide within that of its surfaces' boxes grown by the offset), so hair it pushes out of the CPU's last bounds keeps
its shadows, clip planes and culling right. The base goes up again only where it changed: the
graph's `changesSince` for the base node (a brush stroke's incremental evaluation) lists the
strands, and only theirs are uploaded. A keyed param drags on the GPU too (the node's params hold
the control's value; the release keys the attribute on the CPU). A drag changes nothing in the
scene, so nothing would redraw the views: each value (and the drag's end) marks the groom's drawing
dirty and schedules a redraw (`setGeometryDrawDirty`, `M3dView::scheduleRefreshAllViews`), so every
step draws live, whoever sends it (the viewport GUI suite's live drag case: every step of a Graph
Editor drag, keyed or not, and of a keyed attribute draws before the release, on both APIs, the
default graph and hair_basics). A drag's request goes with the
groom's draw override (a new scene, the node deleted), the OpenGL engine is made again when the
context current cannot reach its objects (a signature buffer it reads back) and deletes them only
from a context that can, and the plugin unloads and loads again cleanly (the viewport GUI check
does each of these on both APIs, with four views drawing). `-stats` (the stages run, the stroke's
dabs and GPU runs, why a drag was refused), `-check [-from node]` (the GPU's results against the
CPU's, read back), `-sync` (wait for the GPU: timing) and `-drawApi` help measure it, `-isEnabled`
says whether it is on; `groomlab_gpu_tests` (`tests/engine/gpu`) checks every kernel against its operator
on a device of their own (skipped without one), on both APIs, and the `viewport` GUI suite checks
the Maya side (drags, a stroke's dabs) on both APIs (`maya.gui`, `maya.gui_dx11`).
**Commits off the UI thread.** A GPU drag's release (`-endDrag`, then `set_param`) and the release
of a brush stroke whose dabs the GPU drew commit what the viewport already shows; the commit's CPU
evaluation (a full one for a drag, the incremental one of a stroke) took ~70 ms and ~230 ms at 500k
strands, with the UI waiting. Now the tool says so before it commits (`GroomShape::expectCommit`:
`-endDrag` when the viewport drew the drag's last values, `gpuPreviewDrawn`; the brush when the
stroke's dabs were the GPU's, not shot sculpting) and the live evaluation that follows, when only the
committed text changed (or a keyed param's control), with the same view as the last evaluation (time,
densities, node viewed), no stroke live, no Simulate node (it steps through Maya), no scalp history
(Jiggle) and no cards wanted, reads its inputs as ever, then detaches the evaluation (docs/SESSION.md,
"Evaluating off the UI thread": `GroomSession::detach` binds the inputs and copies the session's
document, its cached results shared) and runs it on a worker thread (`DetachedRunner`, its loops on
three quarters of the cores, `runWithThreads`: on every core Maya's own drawing waited ~50 ms for TBB;
`GROOMLAB_ASYNC_THREADS`) and returns at once; the session stays the main thread's. Until it lands `outFrame` holds the very frame from before the commit (its
versions, so the draw override rebuilds nothing), `outStrandCount` and `lastError` theirs: a change
the GPU previews keeps every strand, so the count is already the commit's. The draw override holds
while it is pending (`needsUpdate` false, `update` leaves the items as they are): the strands go on
drawing the GPU preview's results (the drag's values, the stroke's last dab). The commit lands on
Maya's idle once evaluated (one idle callback for every groom with a commit pending, there only while
one is: `landCommits`; never an `MTimerMessage`, whose removal while a timer event was queued crashed
Maya in Qt), its frame written into the data block (`forceCache`, only while `outFrame` is still
clean: an input dirtied since means the next pull evaluates, from the session as the commit left it)
and the views redrawn: the update then is the one a synchronous release made (partial writes for a
stroke, the preview's buffers let go), so the pictures after it are today's (the viewport GUI suite's
`async` case: held, committed and fully redrawn pictures, both APIs). Landing, the session adopts the
evaluated copy as its document (`GroomSession::adopt`) and evaluates the request, which finds every
result made. Everything that reads the live session's results or changes its inputs takes a pending
commit in first (`settleCommit`): a compute of the live lane (any input dirtied meanwhile: an edit, a
time change or playback, undo and redo, whose evaluation then goes on from the commit's work),
`session()`, `ensureEvaluated` (the tools' and commands' entry), the previews' setters (setting one),
`storedHierarchy`, `scalpMaps`, `hairLook`, `graphStatsJson`, `readsEarlierFrames` (cached playback's
setup), a connected `outCards`/`outRachis`, the brush target changing, `groomLabGraphStats` (and its
`-points`, `-instances`, `-overlays`); the node's destructor (a new scene, file open, delete) waits for
the worker. What does not wait: the viewport, plug reads of the clean outputs, `frame()` and what
reads the frame (`evaluatedStrands`, the hierarchy, the scalp: the brush's hover, picking,
`-frameInfo`, `-evaluations`: the counts move when the commit lands, so the Graph Editor's status asks
once it has). `hasPreview()` is false while pending (a commit starts only once the stroke ended; a
preview set waits for it), but for a chained stroke. A scene saved while one is pending saves the
committed texts (they are stored at once; the frame is not saved). A run that failed adopts nothing:
the landing's evaluation makes the results itself and says why, as in compute. `-frameInfo` says
`pending`, `commitMs` (the last commit on the worker), `commits` (landed), `chained` and `settledBy`
(what landed the last one: `idle`, `compute`, `ensureEvaluated`...). The swap at landing is the
CPU's strands written as a synchronous release writes them (no read-back to skip it when they match
the GPU's: a read-back of the drawn points costs about what writing the changed ones does); held and
landed pictures match (0 of the pixels differ in the GUI suite). With `sculptDisplay` dimmed or
`sculptDensity` below 1 (the defaults) the stroke's GPU look is not the final one: the release is as
quick, the lit full-density strands appear when the commit lands. GPU preview off, a change it did
not draw, or `GROOMLAB_ASYNC_COMMIT=0`: the commit evaluates in compute as before.
`maya.viewport_bench`'s `commit` and `stroke` parts measure the release (2026-10-09,
tools/bench/gpu_audit/results/2026-10-09_async_commit.md): a drag's release at 500k strands took 134
ms (OpenGL) and 143 (DirectX 11) and takes 7 and 4.5 (1M: 216 and 201 to 8 and 4), a stroke's 253
and 278 to 6 (1M: 517 and 506 to 6); the commit lands 65-470 ms later, its update 15-85 ms.

**Strokes chained on a pending commit.** The commit of a stroke on a creature's fur (the user's wolf:
32 guides, Interpolate keeping clearance, 246k strands at Scatter density 20, 615k at 50) is
incremental (Interpolate blends again only the strands bound to a guide the stroke changed, and the
nodes after it follow those, `groomLabGraphStats` `runs` says how each node ran) but a stroke's guides
reach most strands (a comb over ~9 of 32 guides: 61% of them at density 20, 68% at 50), and the
clearance walk of each costs microseconds, so it takes 0.55 and 1.5 s, which the next stroke's press
waited for (0.5 and 0.75 s). Adopting the GPU's points instead is not exact (the GPU's Interpolate is the
CPU's within a tolerance, not to the bit: renders and exports would differ from a fresh evaluation). So a
stroke of the guides pressed while a stroke's commit evaluates chains on it and waits for nothing: the
brush host's `prepareStroke` asks `GroomShape::chainStroke` (a brush shaping guides alone: no map, no
erase, no shot sculpt) instead of `ensureEvaluated`, and when the stroke sets its guide preview the GPU
preview arms at once on the session's document as it was (the frame held is that document's output),
so even the press's evaluation stops at the GPU's node (`Chain::Armed`); the authored guides it starts
from are the committed ones (`GroomSession::authoredHierarchy`, bound when the commit was detached).
Its dabs draw as any GPU stroke's; `settleCommit` takes nothing in under it and `landCommits` waits for
it. Its release detaches its commit after the pending one (`detach(request, last)`), which copies what
that one made once it ran and makes again only what the second stroke changed; only the last lands. A
stroke the GPU does not take (it refuses the press, a dab's guides do not fit, parts or flow shaped, a
map painted) takes the commit in first and goes on as before. Exact: the chained commits land, to the
bit, what a copy of the groom evaluated afresh makes, and a chained stroke undone while both commits
are pending goes one step back (the viewport GUI suite's `async` case, both APIs; the core's
`DetachedEvaluation` tests with every incremental result checked against a full evaluation).
`maya.viewport_bench` part `chain` (`GROOMLAB_BENCH_CHAIN`: `hair_basics:<strands>` or
`<scene>@<Scatter density>`) measures it: the commit, and the press of a stroke made right after a
release (2026-10-10, tools/bench/gpu_audit/results/2026-10-10_chained_strokes.md, DirectX 11): on the
wolf at density 20 and 50 and hair_basics at 500k strands that press took 515, 750 and 222 ms (the
commit's rest) and takes 9, 15 and 13; the commits take what they took (544, 1385 and 171 ms).
**GPU frames after a CPU evaluation.** A param's commit, a brush release or a playback frame
evaluates on the CPU, and the lit strand lines then send their points alone: the GPU writes their
normals and tangents from them with the preview's own lines arithmetic (`engine/gpu`
`line_frames.h`: `shaders/lines.slang` includes look.slang for `lineFrame`; a dispatcher of its own,
apart from the look engine), new widths go up alone into the uvs' second float, the uvs' u only
when the point counts or IDs change (`StrandLines::idsKey`), the root normals and layout only when
they change (`writeRootNormals`, `layoutKey`): 12 bytes a point (16 with new widths) instead of 36
(44). Maya glue in `hosts/maya/src/gpu_lines.cpp` and `GroomSubSceneOverride::writeGpuLines`: under
DirectX 11 the lit lines draw compute-made buffers of their own as custom resource handles (as the
preview's; the size given to `resourceHandle` is in floats), their points sent through mapped
staging buffers filled in parallel (UpdateSubresource copies on one thread: ~18 ms for 100 MB);
under OpenGL Maya's own GPU-only buffers, the points written through `acquire`/`commit` (a
`glBufferSubData` of our own into a buffer the last frame drew stalled) and the frames in place.
Partial writes (a stroke's release) send the changed strands' points and uvs, the frames all
written again. Where the kernels cannot run (no device, another API, a failure) the CPU writes
every stream as before; `GROOMLAB_GPU_FRAMES=0` or `groomLabGpuPreview -frames off` forces it,
`-checkFrames` compares the drawn buffers with the CPU's. The opt-in GUI suite `pictures`
(`gui_pictures.py`, `GROOMLAB_PICTURES_OUT`, `GROOMLAB_PICTURES_BEFORE`) playblasts every display
mode with the GPU's frames and the CPU's, and against another build's pictures.
**Ribbons made by the GPU.** With `displayRibbons` the builder hands the Maya override the ribbons
as lines (`DisplayBuilder::setRibbonLines`: `DisplayGeometry::ribbons`, a `StrandLines` and the
width scale; `materialized()` builds the triangles `strandRibbons` builds, for where the GPU
cannot), and the override sends the lines alone, as the lit lines' (`writeGpuRibbons`): their points
(12 bytes a point), their uvs' widths when they change (4), the layout and root normals when they
change, the colors when their key moves (lit, the strands' own one a point; unlit, a color mode's
one a strand). The kernels make the ribbons from them (`line_frames.h` ribbons: `lines.slang`
`RibbonsCS`, `RibbonLayoutCS`, `RibbonColorsCS`): two vertices a point, the point less and plus its
frame's side (look.slang `lineFrame`, the CPU's `strandFrame`) times half its width
(`kRibbonDefaultWidth` where the strands carry none) times `ribbonWidthScale`, the point's normal and
tangent on both (lit) or the strand's color shaded by `ribbonKeyLight` (unlit); the triangles (six
indices a point, nothing past a strand's last point) and the lit uvs (u, 0.5) once per layout, the
colors onto both vertices once per key. They draw with the same shaders (kinds 1, 2 and 9: Maya's
hair tube graph, its lights, shadows, selection and post effects), to float rounding the CPU's
vertices (`groomlab_gpu_tests` `RibbonsAreTheCpus`; `groomLabGpuPreview -checkFrames` says
`ribbons`). In the viewport they face the camera (the strands at their true width: `RibbonFacing`,
`FacingRibbonsFaceTheViewer`): across cross(tangent, toward the eye), at least a pixel wide, made
again for each view (`refaceRibbons`, the strands not built again); each view also splits them
(`writeFacingSplit`): a segment a pixel or more wide keeps its triangles, drawn opaque, a thinner
one goes into the line indices, drawn by `groomLabRibbonsThin` from the ribbons' own buffers with
width as coverage (`updateThinRibbons`), since lines draw sub-pixel strands far faster than
pixel-wide triangles. Unlit, they keep the strands' colors (`RibbonFacing::unshaded`). Where the GPU
cannot make them, the CPU's `strandRibbons` (along the scalp) draw instead. Strands of one point make
vertices no triangle uses (the CPU's leave them out). Every buffer the ribbons item draws (and the
lines it is made from) is the override's own (`gpu_lines.h` `GpuRibbonStreams`), handed to Viewport
2.0 as custom resource handles: under DirectX 11 made for compute (the indices
`createD3D11IndexBuffer`), under OpenGL buffer objects made with no data (Viewport 2.0 takes a
pointer to the name), since Maya's own GPU-only buffers, made through `acquire` and `commit`, sent
their whole size up (half a second of a first draw at a million strands). At a million
strands (17M points, lit in their own colors) a first draw sent 2.4 GB built on the CPU and took
0.7 s (DirectX 11) to 1.1 s (OpenGL); now 0.54 GB (the colors once) in 0.14 to 0.27 s, and a CPU
tweak 0.27 GB in 0.16 to 0.2 s where it took 1 s (2026-10-09, `maya.viewport_bench`,
`GROOMLAB_BENCH_MODES=lit_coverage,ribbons_lit`, `GROOMLAB_BENCH_REPEAT=3`). The GPU look
preview drives ribbons too: the run writes the item's lines (the preview's own buffers under DirectX
11, the item's lines in place under OpenGL) and `expandGpuRibbons` makes the ribbons from them, so
drags and brush dabs with ribbons on stay on the GPU. Without a device the kernels run on (or
`-frames off`, `GROOMLAB_GPU_FRAMES=0`), the CPU builds the ribbons at upload as before and the GPU
preview refuses them ("the ribbons are built on the CPU").
**Following the scalp on the GPU.** With Follow Scalp on and the scalp away from its rest pose, a
playback frame changes the scalp alone: the strands are the rest evaluation, carried root by root.
The Maya override says when it can carry them itself (`GroomShape::setViewportCarries`: lit lines or
ribbons, in one color or the strands' own, no stroke, root dots, cards or feathers' vanes, the GPU
frames on and a device); evaluations then ask for them at rest (`ViewRequest::hostCarries`), and a
frame holds the rest strands with what carries them (`ViewResult::follow`, `FollowCarry`: the rest
pose and the scalp now; `placed()` carries them on the CPU, once, for whoever needs the strands
where they are: `evaluatedStrands`, `-points`, `-checkFrames`). The builder is handed the strands
at rest under a version of their own (built and uploaded once, as any rebuild), and every update
the GPU carries them (`line_frames.h` follow: `shaders/follow.slang` `FollowCS`, a thread per point:
`rootFrameOn` on both scalps from their points and vertex normals, `RootMotion::point`, the same
arithmetic in the same order as `carryStrands`, a root whose frame did not move keeping its points
to the bit) straight into the lit lines' points or the ribbons' lines, writing each strand's root
normal on the scalp now for `FramesCS` or `RibbonsCS` after it (`GroomSubSceneOverride::carryOnGpu`,
`carryFollowed`). What goes up per frame is the scalp's points and normals (24 bytes a scalp vertex);
the rest points (12 bytes a point) and each strand's root (`writeFollowRoots`: its face's corners,
the edge its frame takes, its barycentrics; 32 bytes) only when the rest evaluation changes (a graph
reading the time, Simulate, re-evaluates at rest every frame: then the points go up every frame, the
roots kept while the strands share them). The session's bounds are the carried strands' boxes
(`carriedBounds`), not a pass over their points. Cached playback keeps such frames as they are (the
one rest evaluation shared by every frame, the scalp per frame) and draws them the same way; frames
the viewport cannot carry (another display mode since, no GPU) draw `placed()`. The GPU look preview
refuses followed strands ("the viewport shows other strands than the graph's output"). Checked by
`groomlab_gpu_tests` `FollowIsTheCpusCarry` (points to 2e-5, still roots to the bit, frames and
ribbons), `FollowScalp.AHostCarryingTheStrandsGetsThemAtRestAndTheSameBounds`, the `viewport` GUI
suite's `follow_check` (`GROOMLAB_VIEWPORT_CASE=follow`: the scalp alone sent, the drawn buffers
against the CPU's carried strands, the pictures against the CPU's) and the `pictures` suite's
`follow_*` cases; `maya.viewport_bench`'s `follow` part times it (`GROOMLAB_BENCH_FOLLOW_EVAL`
live and cached, `GROOMLAB_BENCH_FOLLOW_MODES`): at 500k strands (8.5M points), lit lines, a frame evaluated live took 54 ms
(DirectX 11) and 72 ms (OpenGL), 110 MB sent, and takes 18 ms on both, 0.17 MB sent (a million
strands: 103 and 115 ms to 28; cached playback 24 and 34 ms to 11 and 13; 2026-10-09,
tools/bench/gpu_audit/results/2026-10-09_follow_gpu.md). The rest of a live frame is Maya's, the
scalp's conversion and the session's bounds.
**Running an experiment in a real Maya session.** The GUI tests show the pattern: a `.mel` file
that `python("exec(open(...).read())")`s a script, launched with `maya.exe -script that.mel`,
with `MAYA_APP_DIR` pointed at a scratch prefs folder so the installed module is not auto-loaded
and the build's plugin is loaded explicitly. The script logs to a file, does its work inside
`cmds.evalDeferred(..., lowestPriority=True)` (after Maya's own idle work, including the
plugin's deferred menu install), and quits with `cmds.evalDeferred(lambda: cmds.quit(force=True))`.
`groomLabBrushCtx -edit -debugStroke "x y x y ..."` (or `"ctrl x y ..."`, `"shift x y ..."`)
runs a stroke through the interactive press/drag/release code from view coordinates
(`GROOMLAB_DEBUG_STROKE_SYNC=1`: each step timed until the GPU has drawn it, its CPU part and the
wait given apart in `-debugStrokeDraws`; `groomLabGraphStats -drawTimes` splits an update into the
evaluation pulled, the GPU preview's run and the hair grid);
`omui.M3dView.active3dView().worldToView` gives the coordinates of a world point.
`cmds.playblast(frame=1, format="image", completeFilename=...)` captures the viewport to look at.

## 4. How things flow

**Evaluation.** Every host evaluates through a `GroomSession` (`groom_session.h`,
[SESSION.md](SESSION.md)): it hands the session its inputs through setters that do nothing for
the same pointer, text or hash, and asks for a view. The session lays the vertex maps from
`scalpMaps` onto the scalp as color sets of the maps' names (`withVertexMaps`; the brush's
preview map replaces the stored one while a stroke is live), roots the guide hierarchy on it,
applies guide poses and correctives, binds parts and flow, then fills the graph's external
inputs (`fillExternals`: scalp, guides, hierarchy, parts, flow, collider meshes, controls and the
map catalog `maps` = where each painted map lives) and evaluates the graph at the view's density.
`GroomShape::computeStrands` is the viewport's host: it converts the scalp mesh once per change
(`toScalpMesh`, cached by a fingerprint) and reads only the plugs that were dirtied. Exports and
the Arnold translator build the same inputs from the node with `groomDataFromNode`
(`maya_inputs.cpp`) into a session of their own, so they see what the viewport sees.

**Caching.** Every node's cache key is its params, its inputs' keys and the hashes of the
externals it declares (`Operator::externalInputs`), plus `cacheSalt`/`contextSalt` for things
outside the params (a texture file's stamp, the map catalog). The context's own hash leaves the
time out: what changes with time arrives as a hashed external, and the few operators that read
the time itself salt their own keys with it (`contextSalt`: ImportCurves, Wind), so only they and
what follows them re-evaluate from frame to frame. A bypassed node that passes its input through
keys on that input alone (its params, salts and externals do not count). A Compound's inner graph
keeps a cache of its own as well (`compound.cpp`): each compound input reaches it as an external
hashed by the value's identity (the compound holds the last evaluation's inputs, so an address
names one object), so an exposed param tweak or an upstream change re-runs only the inner nodes
it reaches; a clone or an adopted cache carries the inner cache along (`adoptKeptWork`). The
session, though, holds most compounds as their nodes inline (`GraphEdit::flattenCompounds`, as
Unreal's collapsed graphs and Blender's node groups run): each inner node named by its path
("compound/inner", the key Cards, Sculpt and Simulate already use), so the CPU, the caches and the
GPU's preview see one graph and a compound's Noise drags on the GPU like any Noise. What hosts ask
by a compound's name maps through `GroomSession::flatCompounds` (a view or evaluation of it: its
output node; a drag of an exposed param: the inner node's param; its time and notes: its nodes'
summed and joined). A compound that repeats, reads its pass (`iteration`), is bypassed, runs
viewport- or render-only or has a control keyed on a param stays one Compound, evaluated as above.
`Graph::lastEvaluationTimes` gives each node's milliseconds in the last evaluation (0 for a cache
hit: what the graph stats show per evaluation), `lastRunTimes` what each cost when it last ran. A brush stamp bumps
`brushTick`, which dirties the shape; nodes whose inputs did not change are cache hits.

**The preview core.** A preview at density p must show exactly the render's strands of the IDs it
keeps (`PreviewInvariance.*` checks every preset and every node reading other strands, bit for
bit). A thinned source (Scatter) marks each strand's role in the strand channel `_preview`
(`preview_core.h`): shown, core (shown) or hidden; its core is the strands under its core share
(`coreShare`: 3%, or a power of two more so that it holds 5,000 to 10,000, all of a smaller
scatter; none thinned with `previewAll`), the same at every density, the render included. Below
the core share a preview holds the core strands it does not show, marked hidden. Every node runs on
every strand it is handed; a node whose result for a strand reads other strands (center picks and
axes, clump middles, neighbours, partners, paths, longest strands and length quantiles, ToGuides)
reads only the core (`PreviewCore`), so its inputs are the same at every density. Copies, subsets
and joins carry the roles like any channel (default core: strands joined from a source never
thinned are core). The graph hands its callers the shown strands without the roles
(`Graph::evaluate*`, `shownStrands`, kept per node so a cache hit hands the same pointer), and
`changesSince` maps a result's changes onto the shown strands (unknown when what is hidden
changed; `asMade` for the result as made); `cachedValue` is as the nodes made it (a Compound's inner graph evaluates with
`EvalContext::keepPreviewCore`), and kept operator state answers for the shown copy too
(`sameOrShown`). Scatter relaxes every root whatever the preview (its kept placement is the
render's), so a preview's roots sit where the render's do. Left out: Propagate's thinning of
barbs under the viewport's point budget (memory, not the density), and an expression's `index`.

**Incremental evaluation.** A brush dab changes a few guides, so only a few strands change. Each
node's result carries a serial (unique in the process; a result a clone or an adoption takes
keeps its serial and the serials of what it was made from, so one held from another document
names that very result or none, and a stroke right after a graph edit is as incremental as
without it) and how it differs from the result before it (`StrandChanges`: a
sorted list of strands, or all); a node whose own key (everything but its inputs' keys) is its
last result's, and whose inputs changed in some strands, asks its operator to make only those
again (`Operator::evaluateIncremental`, opt in; default: a full evaluation). The result is the
full evaluation's, bit for bit, and is cached under the same content key, so adoption and clones
work as ever. An operator copies each array it writes from its last result (never writing into a
value someone may be holding; the graph lets go of the last result while the operator runs
(`IncrementalInputs::sole`), so when nothing else holds it or shares its points or widths, the
operator takes that very array instead: `positionsOfLast`, `widthsOfLast` in
`operators/incremental.h`, `StrandSet::takePositionsFrom`; at 500k strands a 96 MB copy saved per
node), makes the changed strands again from its inputs exactly as a full
run makes them, and falls back (returns false) when anything its strands depend on beyond
themselves differs: a whole-set pass (an expression of `longest`, or a field node plugged in
reading one: each `FieldValue` says whether it is `strandLocal`, true for Map, Constant,
Control and FieldNoise, for FieldRemap and FieldCombine when what they read is, false for an
Expression of `longest`), a kept binding or center choice, the layout. Nodes that hand their
first input on as it came (Output, a
bypass, a Resample with nothing to do) pass its changes on. Incremental now: Interpolate (each
guide told from the last result's to the bit: the strands bound to changed guides, and those
whose binding changed when roots moved; with fillTubes in full), Clump (the moved strands and the members of moved
centers; centers it picks stay the same while no root moves, the moved ones taking their
strands' points), Noise, Curl, Length, Width, HairColor, Bands (guard hairs not by length),
FeatherColor, RenderData, Resample (even, or by curvature while
the changed strands keep their counts), Collide (while its meshes' inputs are unchanged: each
strand is pushed against the meshes alone), Smooth, Bend, Part, Flow, Flyaway, Attribute, Wind,
Gravity, Displace,
Jiggle (a made-again strand works out its own face's lag), Twist (around axes that did not
change; around its clumps' middles it is a whole-clump pass), RestoreLength (evenly, or with a
reference unchanged or changed in the same strands, the same IDs in the same order), Instance
(constant maps: the last result's instances), Switch (its picked input's changes), Merge (without
a seam to blend: a's changed strands, and b's after a's count, merged again on their own and
written over their places, while a's changed strands keep their clump, braid and feather ids),
Blend (the strands either side changed in), Cull (its kept strands, kept by the operator with the
result they cut, stay while the changed strands' verdicts do: those kept are cut again over their
places) and Braid (without feed and without an earlier Braid's ids, while its paths stay: each
strand is braided by its own root) and Split (`Operator::evaluateOutputsIncremental`: changes are
kept per output, so each output's readers get the changed strands it holds, in its own indices;
an output a changed strand left or joined, its select read from what changed, is cut again and
says `all`, as Cull falls back when its kept strands change). Everything else evaluates in full:
Tangle, BlendNeighbors, Wet, SelfCollide and Static read other strands, Mirror changes the layout (on guides, the
Interpolate after it diffs them itself), Curl by clump reads whole clumps. A node with several
outputs that evaluates in full diffs each output against its last. A node that evaluates in full while its own key stayed and its inputs changed in
only some strands (one of those, or an incremental operator that fell back) then compares its
result with its last one strand by strand, in parallel blocks, skipping the arrays the two share
(`strandChangesBetween`, graph.h: 2.5 ms at 500k strands of 16 points with 1% moved, 3.6 ms with the widths and a channel written anew too), and hands the strands that
differ on as its changes when the two hold the same strands, so what follows stays incremental
(`Graph::setDiffFullResults(false)` turns it off for timings). Collide's own share of a dab is its walk of the moved strands: every point a lookup in each
mesh's signed distance field (`MeshDistanceField`, made where the hair goes on the first
evaluation and kept by the mesh's surface hash, `sharedDistanceField`), answered by the field alone
(`Probe::fieldOnly`: where its blend cannot tell, the closest point among the triangles around the
closest faces it holds there), as the GPU's Collide answers; `Budget.BrushStrokeLatencyWithCollide`
times it beside `method` `exact`, which asks the tree at every point. A mesh deformed away from its
rest pose (an animated scalp, a collider given a rest pose) is asked through its rest pose's field,
made once for every frame (`RestPoseCollision`: each point taken to rest by its nearest vertex's
follow-the-scalp frame, found by a walk along the strand); `Collide.RestFieldIsMadeOnceForEveryFrame`
times a frame against `exact` and against a field made for each frame. Gravity's surface and the guide
simulation's colliders (and its scalp, for guides not rooted on it) ask the same way
(`CollisionSurface`; `Gravity.RestField*`, `Simulation.RestField*` in test_distance_field.cpp). Maya
gives a collider its rest pose through `colliderMeshes[i].colliderRestMesh`: `add_collider` connects a
deformed mesh's original shape (`outMesh`, object space, still while the transform animates). `Graph::setVerifyIncremental` (tests)
checks every incremental result against a full evaluation and the strands it says it left alone
against its last result; `Graph::setIncrementalEvaluation(false)` turns it off.

The `scalp` external is keyed by the surface only (`ScalpMesh::surfaceHash`: points, triangles,
UVs, rest pose; not the color sets), so painting a vertex map leaves Scatter, Interpolate and the
rest cached. Every reader of a color set (a map param with a colorSet source, HairColor's
map, the Map node) adds that set's hash itself
(`colorSetSalt` in operators/internal.h); a new color-set reader must do the same, or it goes
stale when the set is repainted. The Scalp node hands out the current mesh even on a cache hit
(`Operator::refreshesOnCacheHit`), so its colors are never stale. File stamps (textures, UDIM
tiles) are taken once per evaluation (`ScopedFileStamps`, texture.h).

Some operators keep their own work between evaluations (a mutex-guarded memo of 2 entries):
Scatter its relaxed placement and Interpolate each strand's guides and weights, keyed on
everything that work comes from except the current pose (`ScalpMesh::restSurfaceHash`,
`PartLines::sideHash`, seeds, densities, guide roots). Any new input such work reads must join
its key. A randomness is never in such a key: Scatter keeps each root's even and random spots and
mixes them (its last mixes kept too), Clump keeps the plan of its own centers
(`planClumpCenters`: the even centers and each one's way to its random spot) and moves them
(`placeClumpCenters`), so a randomness tweak redoes only the mix, and a small change moves the
same strands or centers a little (`Continuity.*`), never picks others. With a `regionMap`, Interpolate's key holds the map's source (`contextSalt`) and every
strand's color on it (kept with the strands), and the regions come from the guide roots: a
binding is made per region (each region's guides searched and weighed on their own, `bind`'s
subsets), binding again after roots moved keeps strands only while every guide and strand keeps
its region, and `interpolateRebind` answers false (the CPU evaluates) when a strand would change
region. Clump keeps each strand's center with the query it was searched from (the root on the
rest scalp, else the first point), the centers' roots and the parts' sides: a full run with the
same (a strength or profile tweak) takes the assignment instead of searching again. With
`centersFrom` `inputPoints` the strands are assigned first and each center's axis is the strand it
gathers nearest its root; a dab keeps those axes (no root moves) and clumps again the strands that
moved and the members of an axis that moved, and a strand changing its center runs it in full; Cull
keeps the strands it kept with the result it cut. Clump, Noise and Curl (not by clump) take their
points for writing anew (`StrandSet::overwritePositions`) and copy each strand in just before
shaping it, rather than cloning every point first. Interpolate's binding also holds the paths along the scalp from each guide root
(`SurfaceFronts`, surface_paths.h: a front per guide out to its reach, rounded up to an eighth of
an octave so a reshaped guide's rounding of its root keeps it, and per triangle how many fronts
do not reach it straight); binding again after roots moved makes only the moved guides' fronts
and those whose reach changed, and every front made is also kept on the scalp's `SurfaceGraph`
by root and reach, so another binding of the same guides (the stroke document's, a density
changed) makes none. A strand on a triangle every front reaches straight (almost all of a
smooth scalp) looks nothing up: its paths are the straight line, its weights and turns are the
plain ones to the bit. Such work goes with the results it serves to another copy of the graph (a graph text set
again, a clone: `Operator::adoptKeptWork`; Interpolate's binding and Clump's
center choice) and hosts read it through `operator_state.h` (the GPU preview). `GuideHierarchy` keeps its content hash until it changes: a new non-const member must
call `invalidateHash()`. To evaluate several graphs on one groom (LODs), `GroomSession::setGraphJson`
swaps the graph and adopts the old one's cache, so only what differs re-runs.

Exports and the Arnold translator evaluate through `NodeEvaluator`, whose `NodeInputCache`
converts the scalp, guide curves and colliders again only when their data reads differently
(a deforming scalp: only its points, see A deforming scalp below), and keeps a guide cache's
Alembic reader open across frames. It hands its session the raw scalp and the `scalpMaps` text
(`groomDataFromNode` with a text out, `rawScalpFromNode`): the session lays the painted maps, as
the viewport's does, and is the only place that does for evaluation (a bad maps text is left out
of the evaluator's errors, as before). After its first
evaluation it reads only what changes with time (`groomMotionFromNode`: scalp, curves, poses,
controls, colliders; no JSON) into its `GroomSession`'s setters, `setGraphJson` swaps another
graph into the same session (the glTF export's levels of detail and hair cap share one
evaluator, caching on), and `refreshStoredInputs` hands the session the stored texts again (each
parsed only when it changed; what an edit does not reach stays cached). The Arnold translator keeps one evaluator per groom node (UUID and instance)
across frames (MtoA makes a translator per frame) and interactive updates, dropped on node
deletion, a new or opened scene, the first idle after its last translator went, and past the four
unused ones used last. `encodePngs` / `writeCompressedPngs` (png.h) encode several images in
parallel, byte for byte as one at a time.

**What compute reads.** The shape copies its stored texts (`graph`, `guideData`, `scalpMaps`,
`correctiveData`, `partData`, `flowData`) out of the node, into its session, only after their plug was dirtied
(`textDirty_`: `setDependentsDirty`, `connectionMade`/`Broken`, and the Evaluation Manager's dirty
plugs in `preEvaluation`, which plays without dirty propagation), or on every compute while one is
connected; guide curves' CVs are fingerprinted only when dirtied; the control attributes (`ctl_*`)
are listed once per added, removed or renamed attribute (an attribute-changed callback) and their
values read on every compute. A new stored input read by compute must join `textDirty_` and get
a session setter. The session's parsed graph (`parsed_`) is cloned for the card, rachis, rest,
stroke (sculptDensity) and scratch documents (`GraphDocument::clone`), never parsed again. `outRachis` evaluates only while connected (the
Evaluation Manager computes every dirtied output). A set's bounds and longest strand are
`StrandSet::extent()` (one parallel pass, kept until the set is written).

**Evaluation Manager and Cached Playback.** Maya's Cached Playback (the `cacheEvaluator`; its
safe mode is the Evaluation Toolkit's, unrelated to script security's SafeMode) keeps the outputs
of the nodes it caches frame by frame and plays them back without evaluating. The groom
participates as a node whose state is its outputs:
- *The frame.* `outFrame` (data type `groomLabFrameData`, `GroomFrameData`) holds a `GroomFrame`:
  what the viewport draws of one evaluation (the session's `ViewResult`: strands, scalp,
  hierarchies, parts, flow, cards; the bounds, the errors) and its versions. Every evaluation sets
  it with `outStrandCount` and `lastError`; `configCache` adds the three to the meshes (outCards,
  outRachis) Maya caches by default. The draw override and everything reading what is drawn
  (`evaluatedStrands`, `evaluatedScalp`, `evaluatedHierarchy`, the versions, `boundingBox`,
  `-points`) read `GroomShape::frame()`, the plug's value: a frame cached playback restored is drawn
  as it is, nothing evaluated. The versions come from one counter for the process (`nextVersion`),
  so a cached frame drawn after a live one never passes for it. Tools and commands that work on
  the live session (a stroke's press, the GPU preview, Simulate Range, `evaluateNode`, the bakes)
  call `ensureEvaluated()`, which evaluates live again when the frame shown is not the live
  session's last one.
- *Two lanes.* Compute in the normal context is the live lane (`session_`, `live_`: the viewport's,
  the tools'). Cached playback also fills frames in the background when Maya is idle: compute
  comes in another context (`MDGContext` at that frame, `isNormal()` false) on its own threads, in
  any order (outward from the current frame). That is the background lane (`BackgroundLane`: a
  `GroomSession` of its own, a recursive mutex: one thread at a time), fed from the data block in
  that context, which leaves the live session alone. Off the main thread it reads the data block
  alone: what only plugs say (connections, a collider's transform name, the control attributes,
  how the node resolves paths) it takes from the live lane's last evaluation (`NodeLinks`).
  Controls are read from the data block in both lanes (a key at another frame), never with
  `MPlug::asFloat` inside compute. The background lane reads every text and fingerprints every
  mesh (its data block holds another time's values) and never steps a simulation
  (`GroomSession::setSimulationStepping(false)`).
- *What is not cached.* A frame that depends on the frames before it cannot be made out of order:
  `getCacheSetup` sets `kWantToCacheByDefault` only when `GroomSession::readsEarlierFrames` (over
  the playback range) is false: no Jiggle node, and every Simulate node making the output has a
  bake with its settings covering the range. Otherwise the groom is left out of the cache and
  evaluated live as Maya plays (the Simulate nodes step frame after frame, Jiggle keeps its
  history), while the rest of the scene stays cached. `monitoredAttributes` (graph, simCaches,
  viewNode) has Maya ask again when they change (a Simulate node added, a bake made). Never
  `MNodeCacheDisablingInfo::setCacheDisabled`: that stops cached playback for the whole scene, its
  safe mode, which a groom triggered in every scene before (the reason Maya printed was the
  groom's own).
- *No evaluation at other times while playing.* The live lane reads the scalp at other times
  (the Simulate nodes' frames on the way, Jiggle's history: `scalpPointsAt`, an `MDGContextGuard`
  inside compute) only while Maya neither plays nor scrubs; playing, those reads give nothing (the
  feed returns false: the frames stepped over, those real-time playback drops, are stepped in one
  go toward the inputs as shown, stamped unchecked so a later view of one keeps them; a history
  frame not seen yet reads as still), so
  the Evaluation Manager's playback never evaluates the graph at another time from inside compute.
  Stopped (a jump, Simulate Range, a bake), the frames are read exactly: the scalp, the other
  scalps and the colliders at that time (guide curves, guide caches and controls as shown). The groom stays
  `kUntrusted` for that reason: never evaluated alongside other nodes.
- *The same groom however a frame is reached.* Maya triangulates a polygon by its shape: a bent
  quad splits across the other diagonal than at rest, and strands are placed by triangle. With a
  rest mesh the scalp takes the rest mesh's triangles (`toScalpMesh`), so a frame first read on its
  own (the background lane, a jump, a scene opened mid-shot) gives the frame played to. Without one
  the strands scatter on the deformed scalp anyway (the user guide asks for a rest mesh).
- *Checks.* `groomLabGraphStats -frameInfo` says what the frame shown is (its time, live or made
  in the background, its version) and how many evaluations each lane made, evaluating nothing that
  is clean. The `cached_playback` GUI suite plays a static scalp, a followed scalp cache, a deforming
  scalp, a background fill and the Simulate node live and baked twice each: no safe mode, the second
  pass evaluates nothing (the live Simulate node: every frame, not cached), a param edit evaluates
  every frame again, and the frames drawn from the cache are an uncached evaluation's, strand for
  strand and pixel for pixel. A deforming 50k-strand groom plays at about 80 fps evaluated and
  320 from the cache (OpenGL; 75 and 280 on DirectX 11).

**Motion without a cache.** Wind reads `ctx.time`; Jiggle reads the external `scalpHistory`
(`ScalpHistory`: the scalp's points at the frames before the evaluated time, hashed), which hosts
fill only when `scalpHistoryFrames(graph, output)` (groom.h: the most `history` of the active
Jiggle nodes) is above 0. Every host keeps the earlier frames in the core's
`ScalpHistorySampler` (scalp_history.h: frames by time, each read once, the current frame
remembered, an edited animation noticed, a window of frames kept), feeding it how to read a
frame: the shape in compute (`sampleScalpHistory`, maya_inputs.h: the `inMesh` plug evaluated
under an `MDGContextGuard` at each earlier time, which works inside compute in DG and parallel
evaluation, never while Maya plays or scrubs (see Evaluation Manager and Cached Playback); read
again only when the scalp was dirtied while not playing or scrubbing),
`NodeEvaluator` the same for exports and the Arnold translator, `GroomScene` the groom file's
scalp cache. Operators may report a `note` (not an error), collected by `Graph::lastNotes` and shown in
the graph stats (`notes`) and by the CLI.

**Simulation** (`simulation.h`, [SESSION.md](SESSION.md) Simulation). The Simulate node is pure:
it lays the frame the session made (the external `sim:<node>`) on the guides it is fed, by ID, and
gives them through without one. The session steps each live node to the frame a view asks for
(before the graph evaluates: `stepSimulations`), from its last frame, the frames on the way fed
by the host's `SimFeed` (the inputs put back after; frames it cannot feed are stepped in one go
toward the next inputs known, the roots moving evenly, and stamped 0), and keeps every frame; a node's settings
(`SimSettings::hash`) or its input at a frame it shows changing drops them. The solver
(`GuideSolver`) is Kugelstadt and Schoemer's position and orientation based rods: per segment a
quaternion, stretch-shear coupling it to its points, bend-twist (XPBD) toward the groomed
shape's Darboux vector swept from the root with the parent held (a long chain would otherwise
sag however stiff), then follow the leader for exact lengths (with its velocity correction), then
collisions: the scalp through the distance field of its rest pose (or of the start frame's scalp),
each point taken there by its root's rigid motion, so the field is made once for a moving head;
colliders through their own, or a deforming one through its rest pose's (`CollisionSurface`).
Each guide's root frame (the follow-the-scalp frame on its face) is interpolated over the substeps (origin linearly, rotation spherically) and the groomed shape rides
it. Guides are independent (`parallelFor` over batches of up to four guides of a point count, their
rods solved side by side in the lanes of a four-wide float, each lane the arithmetic of its guide
alone, to the bit); repulsion is a Jacobi pass over a
hash grid built in index order. Parallel and serial give the same bits (a test checks). Bakes are
Alembic guide caches (`sim_cache.h`); GroomScene attaches a groom file's (`simCaches`), Maya's
sessions the node's `simCaches` attribute (`prepareSimulation` in maya_inputs.h, which also sets
the scene's fps and the inMesh feed). Maya keeps its nHair route as well (`groomlab.sim`'s
`create_sim_curves`, guide caches posing the hierarchy): both exist.

**A deforming scalp.** A host that has the same mesh at another frame hands only its points
(`GroomSession::updateScalpPoints`, `ScalpMesh::withPoints`): the triangles, UVs, colors and rest
pose are kept with their hashes, and the closest-point tree is refitted (the source's tree, boxes
again bottom-up) on the first query. A refitted tree answers only when its answer cannot depend
on the order triangles are met (ties within a rounding allowance, a degenerate triangle's
answer): those queries, and every later one on that mesh, go to a tree built on its own points,
so answers are a fresh mesh's to the bit. `groomLabShape` takes the points only when the mesh's
`meshTopologyKey` (counts, face-vertices, UV set and count, color-set names; the rest mesh's
too) matches its last full conversion and the points moved (the same points with the plug
dirtied, an edit of UVs or colors, or a new connection: converted in full); `NodeEvaluator`
does the same and checks UVs and colors (`meshAttributesKey`) on its first read after
`refreshStoredInputs`.

**A subdivided scalp** (`subdivision.h`). Hosts hand the scalp as triangles plus the polygons they
came from (`ScalpMesh::setPolygons`: triangles per polygon, Maya's `getTriangles` counts, the
app's OBJ faces; outside every existing hash, so a mesh roots, scatters and caches the same with or
without them) and the level the renderer refines it by (`setRenderSubdivision`, in `contentHash`
when above 0: Maya's `renderSubdivisionOf`, Arnold's catclark iterations else the smooth mesh
preview level, read live on the main thread and kept for background lanes; a groom file's
`scalp.subdivision`). `GroomSession::bindScalp` lays the vertex maps on the scalp as given
(`cageScalp_`, what vertex maps are stored and painted by: `StrokeHost::paintScalp`,
`ViewResult::cageScalp`, `groomLabScalpMaps`) and refines that by `scalpSubdivision()` (the graph's
Scalp nodes' `subdivision`, the most of several; `matchRender` the scalp's render level) into
`scalp_`, which everything after binds to: the rooted guides, flat guides rooted again
(`bindRefined`), parts, flow, the graph's `scalp` external, following, Simulate's inputs, Jiggle's
history (each frame refined) and the view (`scalpMapView` shows a vertex map refined linearly). At
level 0 `scalp_` is `cageScalp_`, the old path to the bit. Parts and flow are stored bound to the
cage whatever the level (a part drawn at level 2 stays put when the level changes): `bindOverlays`
carries the stored ones onto `scalp_` (`PartSet::onRefined`, `FlowSet::onRefined`, kept per content
and cage topology; the flow's directions turned on the rest pose, the same every frame), and the
tools edit them on the scalp shown and store them back (`OverlayBinding` in `stroke.h`: interactive
and scripted strokes, both ways; a preview is on `scalp_`). The carrying is exact in the surface's
parametrization (`SubdivisionPlan::refinedRoot`, `cageRoot`): each cage face laid out as a regular
polygon its triangles cover, split into a quad per corner as level 1 splits it, each later level
into quarters, the refined triangles halves of their quads; piecewise linear, continuous across
faces, each way the other's inverse (to rounding), so stored data does not drift through strokes.
A cage point lands where its place refines to, a little off it in space where Catmull-Clark draws
the cage in or evens its spacing out (`SubdivisionBinding.*`). The level never changes the
groom's layout, only where it sits: a refined mesh keeps its cage (`ScalpMesh::cage`, set by
`subdivideScalp`), and `CageBinding` (`subdivision.h`) gives what must not change with it. Scatter
places its roots on the cage (random, blue noise, uvGrid; density, guide maps, `guideRadius` and
the placement memo there) and carries each onto the scalp (`toScalp`); `ScalarField` reads
texture, color set and guide maps on the cage (`atRoot` through `toCage`, `atCageRoot` for
Scatter); the centers Clump picks (`planClumpCenters`: the targets sampled on the cage,
roots at `stablePosition`, the cage's rest position) and Clump's nearest-center search key on the
cage (part sides still on the scalp, where the parts are). Strands, IDs, centers and each
strand's clump are the same at every level (`SubdivisionBinding.TheLevelKeepsTheStrandsAndTheirClumps`,
`maya.subdivision`); on a cage `CageBinding` answers as the scalp does, to the bit. Shapes that
read positions (Interpolate's weights, BlendNeighbors, noise in world space) follow the surface.
Guides need none of this: the
hierarchy keeps its world shape and re-roots on a scalp of another topology, flat guides are rooted
again by position (`bindRefined`); vertex maps are stored per cage vertex. ColliderMesh refines its own mesh in
`evaluate`. The Catmull-Clark: a `SubdivisionPlan` per cage topology, polygons, UVs and level (a
cache of 8, `SubdivisionPlan::of`) holds each level's faces, edges and vertex rules (smooth,
crease: two sharp edges, corner: a boundary vertex with one face or more than two sharp edges;
boundary and non-manifold edges sharp), and the UV layout's own plan (a value per distinct
vertex and UV, so seams are its borders: Arnold's `pin_corners`, Maya's OpenSubdiv "sharp edges
and corners" UV boundary); color sets refine linearly per face. A frame of a deforming scalp runs
the plan's weights over the new points and moves the last refined mesh (`withPoints`: its tree
refits): on a 10,000-face head about 3, 10 and 40 ms at levels 1, 2 and 3, the plan 12, 35 and
150 ms once (`Budget.SubdivisionRefine`). `maya.subdivision` checks the surface against
`MFnMesh::generateSmoothMesh` (vertices within 1e-6 of the size, every UV the same); quads split
`(a, b, c) (a, c, d)` on every frame (stable bindings), where Maya splits across the shorter
diagonal, so a root sits within a refined quad's bend of Maya's triangulation. Edge creases and
Arnold's other UV smoothing modes are not read.

**Following the scalp** (`followScalp`, `follow_scalp.h`): `GroomSession::prepareFollow` makes
the inputs at rest (the mapped scalp's `restPose()`, guides rooted on it, parts and flow bound
to it, flat guides and poses taken back through their roots, the history turned by each
vertex's frame) and `fillExternals` hands those to the graph; `carry` moves the result to the
scalp now (memo per view, cards, full density and scratch node; or by the host, on the GPU: Following the scalp on the GPU). None of the rest inputs moves
with the scalp, so the graph is a cache hit from frame to frame and motion key to key. At rest,
or without a rest pose, nothing changes: the normal path, to the bit.

**The rest pose.** An Instance node with `pinToRest` keeps each instance at the distance from the
root its fraction gives on the strand at rest; one with `sizeMode` `fitStrand` sizes each from
its strand's rest length (`fitScale` of it along the prototype's +Y, `aspect` of that across, a
non-uniform scale every host's matrix carries). When some instance needs it
(`InstanceSet::needsRestPose`), the host evaluates the graph a second time, at
rest (`RestPose` in groom.h: the scalp's rest pose, the hierarchy as authored on it, no poses,
correctives, colliders or scalp history, time 0), in a document of its own, and
`pinRestDistances` writes each pinned instance's `restDistance` and each fitted one's (and each
scale's) `restLength` into a copy of the strands (sharing every array), which `InstanceSet::resolve`
then uses. None of the rest inputs change
with time, so from frame to frame the rest evaluation is a cache hit (with caching off, as in
renders, the rest inputs' hash skips it). `GroomSession::evaluate` does this for every host
(`restPose_`; a guide preview is the authored hierarchy while a stroke is live).

**Scales.** A Scales node (an `InstanceOp` with `scales_` set) makes instances flagged `onSkin`: one per strand,
placed by `InstanceSet::resolve` at the root, flat on the skin (the scalp's normal at the root when
the host passes the scalp, else the one the node saw), sized from the strand's rest length and root
width. `ScaleMeshBuilder` (scales.h) makes their geometry for every host: the prototype (a built-in
plate, `builtinPrototype`, or the host's scene mesh through a lookup) tilted (lift, tile), raised
(offset, stack) and bent onto the skin through `collisionField` (each vertex's height above the
root's tangent plane laid onto the surface under it; `pushOutOf` for what a fold leaves inside),
with per-vertex colors (the color nodes') along the strand and per-scale data (`ScaleMesh::Scale`: frame,
random ID, root uv, variant, material). It keeps each scale's bent vertices by ID with a key of its
frame, prototype signature, offset and the skin's surface hash, so a rebuild bends only what moved.
The frame's +Y is the strand's chord laid onto the skin, or its direction near the root
(`Instance::aimRoot`, the node's `aim` `root`, and always with `followStrand`). A scale with
`followStrand` (the node's `follow` `strand`) bends along its strand: each vertex is also placed
about the strand's curve at its distance along the plate (`StrandSpine`: the curve by arc length,
the width axis parallel-transported from the scale's +X, straight on past the tip) and blended in
from the bent plate at the base over `kStrandBase` of its length, then pushed out of the skin as
the bend does; its key then hashes the strand's points too.
`DisplayBuilder` holds one (the `scales` item, `DisplayGeometry::scaleSurface`; flat quads past
`kInstanceTriangleBudget` or with `preview` quads); the Arnold translator and the procedural build
one per export (`setScaleMesh`: one polymesh, user data, a shader per material and prototype).
`InstanceSet::hidesStrands` says the strands only lay scales out: the viewport and both Arnold
hosts leave them out. Maya's draw override shades the item with `groomLabScaleSurface`: the hair
graph with the hair color fragment and the cards' geometry fragment (normals in the bitangent
stream). An Instance node's instances hash and place as before (`Instance::onSkin` false).

**Scales out of the groom.** Every export takes a `ScaleMesh` the host builds per frame with a
kept `ScaleMeshBuilder` (only moved scales bend again) and its prototype lookup (Maya's scene
meshes, `readInstancePrototype`; a groom file's `GroomFile::prototypes`, which
`groomLabExportGroom` fills from `graphMeshPrototypes` of the graph): `UsdCurvesWriter::writeFrame`
(a `Mesh` and/or a `PointInstancer` whose orientations come from `usdOrientation` of each
`ScaleMesh::Scale` frame, `Options::scales`), `AlembicCurvesWriter::writeFrame` (a polymesh under
the root), `groomLabScaleMesh` (a Maya mesh for FBX) and `scale_maps.h` (`bakeScaleMaps`: the
scales' triangles as a `ScalpMesh` for its ray tree, each skin texel cast down its normal; tiles
per UDIM; `encodePng16` for the height). In Arnold, `setScaleMesh` takes the motion keys and
`setScaleMeshKey` fills each from that step's strands and skin (translator `ExportScalesKey`,
procedural `Scales`); keys whose scales differ (IDs, vertex counts) fall back to the first.

**Keyed params.** A node's `paramControls` links params to the host's controls (`Graph::setParamControls`):
on every evaluation `Graph::applyControls` sets each linked param from the externals
`control:<name>` (a vec3 from three), else back to its own value, so the value keys the cache and
walks after an evaluation (a keyed Switch, the hair look) see it; `storedParams` and the JSON keep
the own value. A compound's key includes its inner links (`controlInputs`). A spec with
`animatable` false (counts, segments, card topology, seeds: `notAnimatable`) refuses a link.

**Fields and maps.** A mappable param is a number, a map (`texture_map`, `color_set_map`,
`attribute_map`, `expression_map`, `guide_map`) or, when exposed as an input (`paramInputs`), a
Field port fed by a node. A plugged field is multiplied by the param's own number
(`ScalarField::scale_`), so a 0-1 mask on a density of 100 gives 0-100. The **Map** node reads a
painted map by name: the host's catalog says whether it is a guide layer (found on the
hierarchy), a vertex map (a color set on the scalp of that name) or a texture (its file);
without a catalog entry it looks on the scalp, then the guides, then reads the name as an image
file's path at the scalp's UVs. Its `min` and `max` are what the map's 0 and 1 become (min above
max inverts it). A strand channel as a node is an Expression (`$name`).

**Several outputs.** An operator's `outputs()` lists its output ports (default: `{output()}`, the
primary); one that has several (MapOp: `luminance`, `r`, `g`, `b`, `a`) overrides `outputs()`
and `evaluateOutputs()`, which returns one Value per port from one run. The graph caches a
node's values together (`Node::cachedValues`) and each input slot records which output of its
source it reads (`Node::inputOutputs`; the primary adds nothing to the cache key, another
output its index), so every consumer shares one evaluation. `Graph::evaluate` returns the
primary, `evaluateOutput(id, i)` and `evaluateOutputs(id)` the rest; connection type checks use
the chosen output's type. Compounds give one output (their inner output node's first).

**Painted maps** (Groom Tools > Paint) are one concept with three storages that convert into
each other (`scalp_paint.h`: stamps, smoothing, the texel-to-scalp table, PNG writing,
resampling): a guide layer lives in `guideData`, a vertex map in `scalpMaps`, a texture map as
a PNG in the scene's masks folder (`masks/<asset>/<map>.png`). Vertex and texture maps are gray
(1 channel) or color (4, r g b a: stamps, smoothing and fills take a channel mask and a target
color per channel, `kGrayMap`/`kColorMap`, `convertMapChannels`); a color vertex map is laid on
the scalp as a color set with its real colors, so the Map node's `r` `g` `b` `a` outputs and
HairColor's `map` (which names a painted map, else a color set of the scalp, else an image
file) read it as they read a texture's. `groomLabScalpMaps` makes, fills, renames,
removes and converts them; the brush tool paints them where the cursor hits the scalp
(`BrushToolSettings::paintKind`), previewing a vertex map through evaluation and a texture in
the viewport, and commits each stroke as one undoable `groomLabBrushStroke` (a texture stroke
keeps only its changed 32x32 tiles, before and after; undo and redo patch them into the paint
and write the file again, and the texture cache is primed with what was written
(`primeTextureCache`), so nothing decodes it again). While painting, `TexturePaint` tracks the
changed rectangle and `TexturePreview` (scalp_paint.h) converts only that region for the
viewport, which uploads only that region. Screen-space brush math (projection, scalp
visibility, screen weights, the hover grid) lives in core `view_projection.h` for any host. Moving a groom to another scalp projects
vertex and texture maps (`projectVertexMap`, `projectTexture`: each point of the new scalp taken
back through the `ScalpFit`'s inverse, the old scalp's closest point, its value or UV there; both
meshes' rest poses when both have one, else their points, as the guides' transfer reads them).

**Brushes.** The tools are the core's (`stroke.h`, [SESSION.md](SESSION.md) Strokes): `BrushTool`
takes mouse events in a `ToolView`'s pixels, starts on hair or scalp under the cursor, stamps
working copies of the hierarchy (or parts, flow, a scalp map) with a screen-space weight,
previews them through its `StrokeHost` and commits one `StrokeEdit` when the stroke ends.
Shift+drag smooths, Ctrl inverts. Maya's `BrushContext` (an `MPxContext`) is an adapter: it
turns events into `BrushTool` calls with the active view (`tool_view.cpp`: the matrix where it
agrees with `worldToView`, `viewToWorld` rays), draws the circle, the hover hint and the part
line, handles B/M-drag (radius/strength), and its host reads the groom's plugs, previews through
the shape (`setPreviewHierarchy`, `setPreviewVertexMap`, `setPreviewTextureMap`), keeps texture
maps' files and commits each edit through `BrushStrokeCmd` (an `MPxToolCommand` holding an
`MDGModifier` of the edit's texts, a texture stroke's tiles), so a stroke is one undo step. The
groom the tool is on (`GroomShape::setBrushTarget`, from `findGroom`) has its stroke document
evaluated ahead on the first idle after each of its evaluations (`GroomSession::warmStroke`, a
one-off `idle` callback), so a stroke's first dab makes again only what it changed; putting the
tool away drops the document and the viewport's dimmed strands (`releaseStroke`, `forgetKept`). The
scripted `groomLabBrushStroke -point x y z ...` runs `scriptedStroke` (world space) for tests and
pipelines. The select tool picks through `pickGuides` (lasso with a bounds reject, clicks: whole
pixels as `worldToView` gives them) and `groomLabGuides -select` combines with `combineSelection`.

**Where a scene's files live.** Everything GroomLab writes goes into
`<scene folder>/groomlab/<scene name>/<kind>` (`masks`, `cards`, `exports`, `caches`,
`renders`, `references`; an untitled scene's into `<project>/groomlab/untitled`, and the UI asks to save the
scene first, `menu.ensure_scene_saved`), stored relative to the scene. `groomLabPaths
-assetFolder kind` / `groomlab.asset_folder` and C++ `assetFolder` (`asset_files.h`) say where.
A groom's files are named by its asset name (`groomlab.asset_name`, `groomLabPaths -assetName`,
C++ `assetName`: the transform's full DAG path, every character outside `[A-Za-z0-9_.-]` made
`_`), never its short name: `masks/<asset>/<map>.png`, `cards/<asset>/<asset>_<map>.tga`.
Every host stores a path by one rule (`portablePath`, `engine/core/include/groomlab/paths.h`;
Maya's `portableMayaPath` and `groomlab.portable_path`, the `.groom` export's texture maps,
`save_groom`'s scalp cache, the app's texture maps and bake prefix): relative to the first folder
holding it (Maya: the scene's folder, then the project; a groom file or the app: the groom's
folder); else, relative to a groom file or a groom's folder (`pathFromGroomFolder`), with `..`
when it is on the same drive (a groom moves between machines as a folder tree, and a shared
texture folder beside it comes along); else absolute (a Maya scene: what lies outside its folder
and its project is the machine's, as Maya stores it); variables and relative paths are kept,
slashes are forward. Relative paths resolve against the scene's folder, then the project (`refreshMayaPaths`, kept
current by scene and reference callbacks and the `workspaceChanged` job); a node from a
referenced file resolves against that file's folder first (`resolveNodePath`/`nodePathResolver`:
everything that reads or writes a groom's files passes the node). Old scenes' paths
(`groomlab/masks/x.png`) resolve as they are. Writers never clobber: a new file takes a free
name when one is in the way (`uniqueFilePath`), a texture map file another groom names is copied
for the writing groom first (`fileUsedByOtherGroom`, `forkTextureFile`: `groomLabScalpMaps
-fill/-rename` and both brush commit paths, repointing `scalpMaps` in the same undo step), and a
bake never takes another groom's bake prefix (`bakePrefixUsedByOtherGroom`); `-noStore` bakes an
export copy without touching `cardMaps`. Before every save the plugin's `kBeforeSave` callback
calls `groomlab.follow_scene_files`: when the scene's name or folder changed, the files its
(non-referenced) grooms name are copied into the new scene's folder and the grooms repointed
before the file is written; a destination with other content is never adopted.

**Groom files between hosts.** A `.groom` (`groom_file.h`) is the interchange: Maya exports and
imports it, the app opens and saves it, the CLI and the procedural evaluate it. Import is the
inverse of export, key by key: `graph`, `hierarchy`, `parts`, `flow` and `correctives` are the
node's texts (a slot layer's `weightCurve`, which export samples from the weight's keys, goes
back to keys on the groom's transform: `shots`' weight attribute), `controls` the `ctl_*`
values (after `sync_controls` makes the attributes), `maps` plus the scalp's color sets of the
vertex maps' names are `scalpMaps` (`scalpMapsFromGroomData`; the scalp's other color sets are
the mesh's own), `cardBake` is `cardMaps` (files copied into the groom's cards folder, the stamp
as `Strands=`), `followScalp`, `guideCache` (`guideCache`, `guideCacheObject`), `scalpCache` the
scalp's animation (AbcImport: its mesh is the scalp when it is the file's, else it drives the
made scalp through a world-space blendShape; AbcImport does not undo, so it runs outside the
undo queue), `scalp` the mesh (made by `-makeScalp`, or a given mesh checked against it: the
guides are rooted on its triangles, so the same vertices and triangles; Transfer Groom to Scalp
covers the rest), its `restPoints` a hidden rest mesh, `guides` curves on `guideCurves[id]`.
Every reader reads `upAxis` and `convertUpAxis` turns a file into the host's axis (a quarter turn
about X of the scalp, rest pose, flat guides and the hierarchy's world shapes and frame
references, and the colliders; what is stored on the surface or in the guides' frames turns as it
is; graph params flagged ParamSpec::worldDirection turn too: directions as vectors, Mirror's axis as
the axis it becomes). The C API's gl_session_set_up_axis is the same function. Display settings
are not in the file. A `"license"` field (written by versions before GroomLab was free) is ignored
on load and never written. The round trips are `maya.groom_io`'s: an exported groom imported into
a new scene evaluates bit for bit as it did (the CLI's `hash` of an export), the cache's frames
within float rounding, and exports again to an equivalent file; an app-saved groom evaluates in Maya exactly as the app does and a
Maya groom in the app exactly as in Maya (the app run headless with `--save` and
`--export-alembic`).

**Feathers for engines.** A Propagate node with a `shaftWidth` records its shafts as rachis
rings and a `groupId` its group's name, in a `FeatherLayer` (`feathers.h`) on its output strands;
like card layers they ride copies, subsets, Resample and Merge to the output. `buildRachisMesh`
turns them into the mesh `outRachis` gives (`GroomShape::computeRachis`: the graph at full
density on its own `rachisDoc_`, only while the output is connected), and the Unreal and Unity
exports read them for group names and whether the shafts are geometry (`splitGroomGroups`,
`withoutShafts`, `thinBarbs` in `export_view.h`; `UnrealGroomWriter::writeGroups`). Both engines'
exports, Maya's `groomLabExportAlembic -unreal|-unity` and the app's Export panel, write each
frame through one writer with one options struct (`EngineGroomWriter`, `EngineExportOptions` in
`engine/io/include/groomlab/engine_export.h`: groups, shafts, barb density and width, feather
guides, barb points, Unity's scale, points and strand limit). Unity's
(`groomLabExportAlembic -unity -groups auto`) writes a curves object per group through
`AlembicCurvesWriter::writeGroups` (Unity's Hair Asset makes a strand group of each), groups past
Unity's 64,000 strands in parts (`limitGroupSize`), every root width above 0
(`keepRootWidthsPositive`); `-report` returns the objects written as JSON, which
`export_unity_feathers` turns into its manifest.

**Cards.** A Cards node builds cards from the strands (`cards.cpp`), named by its node path
(`Operator::nodePath`: the node's name, `compound/inner` inside a compound; it salts the node's
cache key). Every Cards node making the output (`Graph::forEachActiveUpstream`: not bypassed,
on a Switch's picked input, inside compounds) adds a card layer; the host combines them into one
atlas (`combineCardLayers`, each card keeping its node). A card rooted on the scalp is seated
on it (`seatCard`: `Card::base`, its root row on the skin under it, which `buildCardVertices`
bends the mesh to; follow-the-scalp carries it). Cards keep to the groom's envelope at every level
of detail: a card runs down its strands' median curve, fits nine in ten of them across and along
(`kFitShare`: the farthest bake at its edge), lies on their outer side (`kOuterShell`) and shades
with their own lit drawing's normals (`volumeNormals`: the members' root normal leaned along the
card, as `display.cpp`'s `strandFrame` leans a strand's). Each card keeps its strands' mean color
root to tip (`Card::colors`, the color nodes' per-point colors included) and its texture's
(`textureColors`: its own, the shared texture's owner's, or the baked atlas entry's); their ratio is
the per-vertex tint (`CardMesh::tints`) the viewport multiplies the map by and the exports carry (the
card mesh's color set `groomLabTint`, half the tint; glTF `_TINT`), and its strands' iridescence per
vane (`Card::iridescence`, left and right; `CardMesh::iridescence` per vertex across, which the card
effect gates the bake's per-texel iridescence with). The bake writes Unreal's map layout plus
an atlas record that locks each card's cell to its node and strands (the Color map RGBA when the
strands carry FeatherColor's iridescence: its alpha), and stamps each node's
cards (`summarizeCardNodes`) in `cardMaps`; the viewport draws cards through the card effect
with the baked maps, flagging the nodes whose bake is stale. Feathers (strands with a
`featherId`) are carded per feather (`makeFeatherCards` in `card_feathers.cpp`: a strip along the shaft in
the vane plane, with `featherSpine` a vertex column on the shaft and each half folded to its
vane (`Card::spine`: edges lifted along the normal so the flat bake's UVs hold; the bake's depth
and tangent in each half's frame), down feathers crossed, variations clustered apart from the
hair's and mirrored by vane asymmetry); a Cards node set to carding only feathers or only hair (`featherMode`
`perFeather`/`off`) makes its cards from a subset of its input holding just those, so the other
kind never makes its bake stale. The viewport's hair look comes
from the nodes the same walk meets (`GroomShape::hairLook`, from `GroomSession::hairLook`): the
dye and the highlight (specular, roughness, tilt) from the first HairColor (0.3, 0.5 and 3 degrees
without one), the iridescence's color and shift from the first iridescent FeatherColor.

**One hair color for every host.** HairColor (modeled on RenderMan's PxrHairColor) writes one
linear RGB per strand (per point with a tip), a hair color in aiStandardHair's base-color sense
(`hair_color.h`): Arnold renders it as the base color over no melanin (`create_hair_shader`'s
`hair_color` user data; the translator's default shader likewise), Maya's viewport and the app
absorb it by Chiang et al. 2016's mapping (`hairColorSigma`, core's `hairColorAbsorption`),
RenderMan takes it as `rman_color` (`rman_rgbShare` 1), Blender as its RGB look.
`HairLook::colorsByRgb` is true whenever a color node makes the output. In mode melanin the RGB is
`melaninColor(m, r) x dye`, where `melaninColor` is `hairAbsorptionColor(hairMelaninAbsorption(m,
r))`: the color whose Chiang absorption is the pigment's (6.6 x melanin^2 x the eumelanin and
pheomelanin spectra by redness), so the viewport shades it exactly as it shaded the melanin before
and Arnold renders it as close to its melanin as Chiang's mapping is to Arnold's (the Blender fit,
docs/BLENDER.md "Rendering", has Arnold's base color absorbing about 1.18x Chiang's). Its randomness
(melanin or value, hue, saturation: `varyHairColor`, HSL, the lightness kept) and the per-clump
share (`clumpShare`: the strand's and its clump's draws mixed with weights whose squares add to 1)
are in the RGB, so every host shows them. Arnold gets the RGB through a `color_correct` of gamma
`kArnoldBaseColorGamma` (1.084, the square root of the 1.175 the Blender fit measured: Arnold absorbs
a base color that much more than Chiang's mapping), in `create_hair_shader`, the translator's default
shader and Blender's material alike; kick then renders a melanin's color within 5% of
aiStandardHair's own melanin from 0.1 to 0.7 (redness 0.2), where it was 13-26% darker without it
(`build/probe/haircolor/kick_melanin_vs_base.py`, the diffuse part beside the white highlight;
at redness 0.8 the core's pheomelanin absorbs less red than Arnold's, in the viewport too). The dye
adds its absorption to the pigment's (`dyedHairColor`), as aiStandardHair combines melanin and base
color. The colorR/G/B channels hold that BASE color; what stores or draws a plain color takes
the APPEARANCE instead (`hairAppearanceColor`: s + (1 - s) exp(-k a^q) of the base color's
absorption a, fitted to kick renders of a few-strand-deep lock under a white dome,
`build/probe/haircolor/kick_appearance_sparse.py`; `hairBaseOfAppearance` its inverse):
`describeExport(..., kExportAppearanceColors)` for USD displayColor, Alembic Cd and Unreal's
groom_color, `strandAppearanceColors` for the cards' bake and tints, `strandLookColors(...,
appearance)` (DisplaySettings::appearanceColors) for Maya's hair tube shading and the app's plain
modes. Maya's hair-shaded cards turn their baked appearance back into a base color in the shader
(hair_bsdf.glsl hairBaseOfAppearance); the bake's ambient occlusion, multiplied into the color map,
is amplified by that inversion (cards about 15-25% darker than their strands there; tube shading
matches within a few percent). Before, mode melanin rendered by the `melanin` user
data while hue and saturation randomness lived only in the RGB: the viewport's pigment path
(`hairLookSigma`, a projection of the color onto the melanin family) and Arnold's melanin shader
dropped them, and turning them on flipped the whole groom to an RGB look in a different color
space (the display `melaninColor`, 5 x melanin with its saturation halved), lighter than the
melanin. The `melanin` and `melaninRedness` channels are still written, as data.

### Param specs, versions and saved grooms

**Param metadata.** A `ParamSpec` (`graph.h`) carries, besides its name, default, description,
unit and choices: `min`/`max` (hard limits: `ParamSet::getInt` and `getDouble` clamp the stored
value to them, the one place a param's range is enforced, so operators and the GPU preview's
`look_chain.cpp` read the same clamped value; a field or map plugged in is per strand and not
clamped there), `softMin`/`softMax` (the slider's range; the limits when unset), `group` ("Base",
"Variation", "Tip", "Bands", "Feather pattern", "Iridescence", "Render", "Advanced"...),
`advanced` (a tuning knob: seeds, channels, iterations) and `showWhen` (`ParamCondition`: shown
while another param holds one of some values, Bend's `direction` only `along` `tangent` or `direction`,
and while that param shows itself: Clump's `count` shows with `clumpBy` `count`, and `clumpBy` only
with `centersFrom` `pick`). Never by whether an input is linked: a param stays where the user
finds it, and where an input changes what a node does a dropdown says so (Clump's `centersFrom`;
`GraphEdit::connectByHand`, every editor's link drawn by hand, sets it to `inputPoints` when
centers are linked into a Clump that picks; plain `connect`, pastes and scripts never change a
param). A param that is unused while an input is linked (FieldCombine's `b`, Simulate's
`waterLevel`) shows always and says so in its description. Specs are
written with chainable helpers: `ParamSpec{"strength", 0.7, "..."}.range(-1, 1).in("Base")`,
`.soft(lo, hi)`, `.adv()`, `.when("mode", {"scales"})`. The
catalog writes `"showWhen": {"param", "in"}`; Maya's
Graph Editor, the app's param panel and both of Blender's panels hide a row whose condition does
not hold, or whose condition's param is hidden. A param shows only where it does something: `ParamUse.EveryDeclaredParamIsReadWhereItShows`
(`test_param_use.cpp`) evaluates every operator on the shipped presets' inputs in many states (each
choice, each bool, each optional input open and linked, numbers at 0 set) with a `ParamReadLog`
on its params and fails for a declared param never read where its showWhen holds; params only
hosts or the session read (FeatherColor's iridescence look, Control's slider range) are allowlisted
there with who reads them. Units: `length`, `width`, `density`, `perLength` (fitted to a scalp's
size by presets), `color`, `angle` (degrees), `turns`, `fraction` (0-1, shown as a percentage),
`seconds`, `pixels`. `operatorSpecsJson` emits all of it (limits and slider ranges only where set);
Maya's Graph Editor and the app's param panel take softMin/softMax as the slider's track and
min/max as what may be typed. `ParamSpecs.EveryOperatorsRangesAreSound` checks every spec.

**Every param saved.** A saved graph writes every param of every node, set or not
(`saveGraphJson`; `GraphEdit::addNode` and `completeParams`; `completeGraphJson` for groom files,
which `groomFileToJson` applies; Maya's `GraphModel.save`), so a later change of a default never
changes a saved groom. Loading stays tolerant: a param left out takes its default, an unknown one is
kept and ignored. Presets stay sparse (they are meant to follow the defaults).

**Node versions.** Each operator has a `version()` (1 for all of them now) and each node the
behaviour it was saved with (`Operator::nodeVersion`, the node's `"version"` in the graph's JSON,
written on save; none: the newest). When a change of what an operator makes would alter saved
grooms, bump its `version()` and keep the old behaviour behind `nodeVersion() < N`: old grooms
evaluate as before, new nodes take the new behaviour. A node's spec (`GraphEdit::nodeSpecJson`)
says `"nodeVersion"` and `"outdated"`, so a host can offer Upgrade (`GraphEdit::upgradeNode`,
`GraphModel.upgrade_node`); a node newer than the build's operator is refused with a clear error.
The node version joins the cache key. `NodeVersions.OldNodesKeepTheirBehaviourUntilUpgraded`
shows the mechanism with a test operator at version 2. The groom file has a format version of its
own (`schema.h`): a newer file than the build reads is refused with "update GroomLab".

**Preset looks.** `PresetLooks.EveryShippedPresetMakesWhatItMadeBefore` evaluates every shipped
preset on the strokes fixture (`tests/data/strokes/strokes.groom`, the preset fitted to its size,
the maps its `requires` lists painted on the scalp as varying vertex maps by
`test::paintRequiredMaps`, density 0.05) and compares a digest (strand and point counts, IDs, point sums and bounds, widths,
channel sums) with `tests/data/golden/PresetLooks.json`, so a change that alters a preset's look is
flagged. After an intended one, `python tools/dev/record_preset_looks.py` re-records it; review the
JSON diff. A preset that cannot evaluate there records its error. Determinism's PresetDigests
paints the required maps the same way (basic arithmetic only, so the same bits on every platform).

**Notes.** `ctx.note(text)` (`EvalContext::note`) tells the user something about the node being
evaluated that is not an error: it does nothing as set. The graph keeps each node's notes from its
last run (`Graph::lastNotes`; an incremental run that notes nothing keeps them), the session's
stats carry them (`"notes"`, `GroomSession::nodeNotes`), and both hosts show a grey badge on the
node and a grey line in its param panel. Clump without centers and count 0, Part without parts,
Flow without arrows, HairColor with a tipColor and tipStrength 0, Cards without their clump channel
and Interpolate without guides note it; an Instance naming a mesh the host lacks is noted by the
hosts (`missingInstancePrototypes`, `scales.h`).

**Param naming** (every node follows these; a new param does too):

- A node's one random spread is `randomness`; the random spread of a param `x` is `xRandom`.
  Their group is `Random`.
- How much of a node's effect is `strength`. A param that would only blend the result with the
  input is left out: `affect` does that on every node.
- A choice of way is `along` (the scalp's U direction is `tangent`); a world vector is
  `direction`; `space` is only strand or world.
- A node's one ramp along the strand is `profile`; the ramp of a param `x` is `xProfile`.
- A param naming the clump channel is `clumpChannel`; other channel params say what they hold.
- A field's output range is `min`/`max` (set in reverse, it inverts); Control's `min`/`max` are
  only its slider's range.
- `lift` and `angle` are degrees; a 0-1 share never takes their names.

## 5. Commands and Python entry points

| Maya command | Does |
| --- | --- |
| `groomLabGuides` | Guides and groups: from curves, generate children, lock, mirror, selection, layers (`-listAttributes`, `-setAttribute`, `-removeAttribute`, `-renameAttribute`), resample, `-reshape <id> <curve>` (guides take edited curves' edit points in place: Curves to Guides), tube guides (`-setTube <json>` on the targets, `editedTube` in `tube.h`; `-tube <id>` returns one as JSON), the tube shape library (`-tubeShapes`, `-editTubeShape <json>`, `-previewTubeShape <json>`: `GuideHierarchy::setTubeShape` and friends, `CustomTubeShape` in `tube.h`), and `-tubeTargets` (the Tubes panel's targets: the selection, else the soloed guides). |
| `groomLabConvert` | GroomLab > Utilities' geometry (`convert_cmd.h`): `-guideCurves` (a curve per guide, tagged `glGuideId`/`glGuideGroup`, a transform per group), `-strandCurves` (a node's strands, capped by `-maxCount` through `thinStrandsById`, `-attributes` for channels), `-strandMesh` (`-make` auto, strands, scales, feathers, cards or instances: tubes or ribbons, scale plates, barbs and rachis, cards, instance meshes; `strand_geometry.h`). Made through the API in one go; undo deletes them (the `delete` command, outside the queue), redo makes them again from the data the command keeps. |
| `groomLabScalpMaps` | Painted maps of every kind: `-list` (`name\|kind\|texture\|channels`), `-kindOf`, `-channelsOf`, `-make <name> -kind <k> [-color [-fillColor r g b a]]`, `-remove`, `-rename`, `-fill (-value v \| -fillColor r g b a [-fillChannels rgba])`, `-convert`, `-setChannels <name> 1\|4`, `-values`, `-relink <name> -texture <png> [-copyFrom <image>]` (a map reads another image: a PNG in place, or any image written as a PNG copy by `writeImagePng`, removed on undo). A texture file it rewrites keeps its bytes for undo; it only ever writes a `.png` (painting writes PNG, so the brush refuses a map naming another format). |
| `groomLabBrushCtx` / `groomLabBrushStroke` | The interactive brush context and its undoable stroke command; the stroke command also runs scripted strokes (`-point`). Paint flags: `-attribute`, `-value`, `-paintKind`, `-paintTexture`, `-paintResolution`, `-hardness`, `-smoothStrength`, `-paintColor r g b a` and `-paintChannels rgba` (a color map; the tool's query of `-paintColor` returns `"r g b a"`). The Shape brush (`-mode shape`) paints tubes: `-tubeChannel width\|flatness\|twist`; while it is on, the groom draws the tubes of every guide it can shape as wires (`GroomShape::setTubeWires`, `activeTubeGuides`, `appendTubeWire`; the selected ones brighter), and Shift+drag smooths its channel (`BrushSettings::smoothTube`, `useShiftSmoothing`). Place and Draw take `-placeTube <json>` (`setPlacementTube`: the new guides' shape, and their sections where no guides are near). |
| `groomLabParts` | Parts (part lines on the scalp). |
| `groomLabPaths` | Path resolution: the search folders, `-resolve <path>` (`-node n`: as n resolves it), `-assetName <groom>`, `-assetFolder <kind>`. |
| `groomLabBakeCards` | Bakes the cards' maps and atlas (`-scalp`: the scalp bake; `-noStore`: an export copy, `cardMaps` untouched; `-layout unity`: Unity's PNG maps instead of Unreal's TGAs, never stored). |
| `groomLabTransfer` | Moves a groom onto another scalp by a transfer method (`-method` closest, bounds, markersAlign, markersWarp or uv, `-markers <json>`, `-keepLengths`: the core's `ScalpMapping`, transfer.h; the markers used are stored snapped in the groom's `transferMarkers`; `-snapMarkers` only returns them snapped) (guides, parts, flow; with `-maps` the scalp maps projected through the same mapping: `projectVertexMap`/`projectTexture` in `scalp_paint.h`, a texture map into a new file of the groom's masks folder, removed on undo and written again on redo). `-mapsFrom <groom> [-mapName n ...]` only projects another groom's maps onto this one's scalp (Copy Graph across scalps). |
| `groomLabCopyColorSets` | `<mesh> -source <mesh> -colorSet <name> ...`: color sets of a mesh of the same topology made on `<mesh>` as its own data, color for color (the Transfer's color set copy onto a scalp without history, where a baked `transferAttributes` does not undo cleanly). Undo deletes them. |
| `groomLabMarkerSet` (node) | A locator the Transfer Groom window draws one side's marker points with (`marker_set.h`): `points`, `normals`, `matched`, `hover`, `color`, `size` (pixels), `camera`; in screen space, x-ray, unselectable, unbounded. |
| `groomLabImportGroom` | A groom file into Maya (`import_cmd.h`), the inverse of `groomLabExportGroom`: `-file f -info` (what it holds, JSON, in the scene's up axis), `-checkScalp mesh` (why a mesh is not the file's scalp: its vertices and triangles), `-makeScalp [-rest] [-name]` (a mesh of the file's scalp: a face per triangle in the file's order, its UVs and own color sets; nodes from a modifier, so undo and redo keep them the same nodes for the commands after it), and with a groom the file applied to it in one undo step (graph, guideData, partData, flowData, correctiveData without slot layers' weight curves, scalpMaps from the scalp's color sets and the catalog, texture images and the card bake copied into the groom's folders and removed on undo, cardMaps with the file's stamp, followScalp, guideCache with `-caches`, groomFile). See Groom files between hosts below. |
| `groomLabExportAlembic`, `groomLabExportUsd`, `groomLabExportGroom` | Exports (guides come in from Alembic through the graph's `ImportCurves` node: `groomlab.import_guides_from_alembic`). `-scales` writes the scales (Alembic: `mesh` or `none`; USD: `mesh`, `instancer`, `both`, `none`). `groomLabExportGroom` writes the scene's up axis, the texture maps' images relative to the file, the Instance nodes' scene meshes as `"prototypes"`, and the card bake copied beside it (`<stem>_cards/<stem>_*` with a preview) as `"cardBake"`. The Alembic export's `-scale`, `-pointsPerStrand` and `-vertexUVs` fit it to an engine's importer; `-unity` is Unity's hair package (UVs per point, root widths above 0, and with `-groups` an object per group, `-barbPoints`, `-maxStrandsPerGroup`, `-report`); with `-unreal`, `-groups` (`auto`: an object per `groupId`), `-shafts`, `-featherGuides`, `-barbDensity` and `-barbWidthScale` shape Unreal's groom (feathers the Meerkat way; all but `-featherGuides` go with `-unity` too). |
| `groomLabScaleMesh`, `groomLabBakeScaleMaps` | The scales at the current time as a Maya mesh (`-name`; undoable: uv sets map1, scaleUV, scaleData, color sets scaleColor, scaleRootColor, locked normals; `export_scales` sends it to FBX), and baked onto the scalp's uvs (`-directory`, `-name`, `-resolution`, `-maxHeight`, `-padding`; returns the files, then the max height). |
| `groomLabExportGltf` | The cards as glTF 2.0 (`.glb`/`.gltf`, `engine/core/src/gltf_writer.cpp`): baked in memory, a mesh per `-levelOfDetail`/`-lodGraph` pair (else the cards as they are), `-scalpCap` (the core's `addScalpCapToGltf`, the app's too: `-capResolution`, else `-resolution`, else 2048), `-scale`, `-alphaMode`; the groom is untouched. |
| `groomLabSelectCtx` | The guide selection tool (lasso, soft, mirrored). |
| `groomLabCameraSolve` | A camera from 2D-3D point pairs (`engine/core/include/groomlab/camera_solve.h`): `-imagePoint u v` / `-worldPoint x y z` (multi-use, pixels from the image's top-left), `-imageSize w h`, `-filmBack h v`, `-focal mm`, `-fixFocal`, `-camera c` (start pose). Returns `tx ty tz rx ry rz focalMm rmsPx e1..eN` (Maya's camera, xyz rotation order); changes nothing. |
| `groomLabSimulate` | The Simulate nodes in the groom's own session (`sim_cmd.h`): `-toFrame N` (Simulate Range), `-invalidate [-node n]`, `-status` (JSON per node: ranges, frames, bake, step ms, stamp), `-nodes`, `-stamp -node n`, `-bake file -node n [-startFrame a] [-endFrame b]` (writes the frames as an Alembic guide cache and returns the stamp; `groomlab.sim.bake_simulation` stores both in `simCaches`). Not undoable. |
| `groomLabGraphStats` | Evaluation stats as JSON (node times, notes, channels, cards, `hairColor` (the HairColor the look comes from), `colorNodes` (every HairColor, Bands and FeatherColor making the output), `renderData` (the RenderData node the opacity comes from), `colorBlends` (the Blend nodes mixing two colors), `scalpHistory`, `instances`, `restEvaluations`, `strokeDocument`); `-evaluations` returns the shape's live evaluation count (cheap, and nothing evaluated for a frame cached playback restored: the Graph Editor refreshes its status only when it changes); `-frameInfo` the drawn frame (JSON: its time, live or background, version, strands, both lanes' evaluation counts, and a commit off the UI thread: `pending`, `commitMs`, `commits`, `settledBy`; nothing waits for it); `-points` returns the viewport's strand points instead, `-overlays` the drawn frame's parts and flow (JSON: `parts` (each line's points), `pins`, `directions`, `sinks`, world space, on the scalp as evaluated), `-instances` its instances' matrices (16 values each, rows x, y, z, origin); `-drawTimes` (report-only) the last evaluation's time in compute and the last viewport update's breakdown (`buildMs`, `uploadMs`, per item `acquireMs`/`writeMs`/`commitMs`/`bindMs` and `megabytes` written). |
| `groomLabDefaultGraph`, `groomLabOperators`, `groomLabVersion`, `groomLabDisplayTheme` | The default graph JSON, operator specs (what editors list), version, viewport theme. |

Python (`import groomlab`): grooms and inputs (`create_groom`, `set_scalp`, `add_collider`),
guides (`guides_from_curves`, `generate_children`, selection, groups, `set_tube`, `get_tube`, `scale_tube`,
`fill_tubes`, `set_fill_tubes`), brushes (`brush_tool`,
`brush_stroke`, `clump_paint`), maps (`add_map`, `paint_map`, `fill_map`, `use_map`, `paint_param`,
`convert_map`, `map_channels`, `set_map_channels`, `export_map`, `map_users`, `show_map_on_scalp`;
a map's image: `set_map_texture`, `reload_map`, `missing_files`, `find_missing_files`), the graph (`get_graph`, `set_graph`,
`set_param`, the `graph.GraphModel` behind the editor), groom files (`import_groom`,
`reload_groom`, `save_groom`, `groom_file`, `groom_file_info`), motion (`add_wind`, `add_jiggle`,
`viewport_points`), looks as text (`copy_graph_text`, `paste_graph_text`: Copy Graph and Paste
Graph, a preset with `"controls"`, checked by `presets.parse_text` before anything changes),
cards (`create_cards`, `bake_cards`,
`export_cards_unreal`, `export_cards_unity`, `export_cards_gltf`, `create_feather_cards`,
`card_node_kind`; card map exports, `engine/core/include/groomlab/card_maps.h`: `bake_card_maps`,
`card_map_presets`, `card_map_sources`, `save_card_map_preset`, `delete_card_map_preset`, the
Export Card Maps window `ui/card_maps_window.py`), feathers (`add_feathers`,
`feather_designs`: Propagate designs from the `feathers_*` presets; `feather_groups`, `set_rachis_width`,
`create_rachis_mesh`, `export_rachis`: the Meerkat workflow for Unreal; `export_unity_feathers`: the
same for Unity), exports (`export_alembic`,
`export_unreal_groom`, `export_unity_strands`, `export_usd`, `export_groom`), reference photos
(`groomlab.reference`: `import_reference`, `look_through`, `set_pair`, `solve`, `lock_reference`), paths (`asset_folder`, `asset_name`, `portable_path`,
`resolve_path`, `follow_scene_files`), utilities (`groomlab.utils`, GroomLab > Utilities: `guides_to_curves`,
`curves_to_guides`, `strands_to_curves`, `to_mesh`, `bake_guides`, `transfer_to_scalp`, `copy_graph`,
`graph_maps`; the menu's actions in `ui/menu.py` take the same choices as arguments and ask only
when one is None), sims (`groomlab.sim`: the Simulate node's `add_simulate`, `simulate`,
`bake_simulation`, `invalidate_simulation`, `simulation_status`; nHair's `create_sim_curves`, `bake_sim`), correctives (`groomlab.shots`), scalp/card bakes
(`groomlab.bake`), presets (`groomlab.presets`), review snapshots (`groomlab.review`), studio lighting
(`groomlab.studio`: `add_studio`, `remove_studio`, the presets as data in `studio.PRESETS`) and unit
handling (`groomlab.units`). The UI modules under `groomlab.ui` open with `palette.show()`,
`graph_editor.show()` and `menu.install()` (the plugin calls the last on load).

## 6. Rules

- The core has no Maya headers; operators are pure functions of params, inputs and context.
- **No format migrations.** Stored formats (graph JSON, guide data, scalp maps, groom files)
  change in place; unknown params and fields are ignored on load. There are no schema upgrade
  steps and no Python upgrade shims, by decision (the formats still move, and tech debt would
  pile up); `engine/core/src/schema.cpp` only refuses documents from a newer GroomLab. When a format
  changes, update the tests, goldens, fixtures and presets with it. Since unknown params are
  ignored, `maya.graph_editor`'s `PresetsTest` runs `presets.check` on every shipped preset: a
  renamed or removed param or node type fails there. A preset is a graph plus its own optional
  `size`, `about`, `requires`, `backdrops` and `controls` (see `presets.py`); a param measuring something
  gives its spec a `unit` (`length`, `width`, `density`, `perLength`) so presets fit other scalps:
  a vec3 position, move or size too (every component scales; positions about the origin), and a
  param whose meaning depends on another says when its unit holds (`unitsWhen`: Noise's frequency
  is per scene unit only in world space). Fitting and every host's UI read `unitWhen`. A field
  node plugged into a param needs none: the param's number times the field is what it takes, so
  its range is a factor. An Expression's own variable says its unit in its JSON
  (`{"value": 2, "unit": "length"}`).
- **Scale invariance.** A groom fitted to a scene k times as big makes the same strands, k times
  as far apart (`ScaleInvariance.*`: every shipped preset at k = 0.01 to 100). So an operator keeps
  no length of its own: a threshold, epsilon, offset or step is a share of a strand's length, a
  segment, a spacing or the scalp's size, never a number of scene units (a `1e-6f` that decides a
  direction compares to the length it is a direction of). A value rounded for edit stability (one
  taken from the whole scalp or all strands) rounds on a decimal lattice (`roundToDecadeStep`,
  math.h) so cm, mm and m round alike; the distance fields sit on world grids of power-of-two
  cells (Interpolate keepClearance's distances by the scalp's size in eighths of an octave), so
  at other scales they differ within their interpolation error. Widths of 1 on every point are
  unset (no Width node), not a scene unit.
- **A param's default is written once, in its spec.** `ParamSet`'s getters fall back to the spec
  default (`Operator::applyDefaults` hands it over), so operators read `params().getDouble("x")`
  and `ScalarField(params(), "x", scalp)` with no fallback of their own; `ParamSpecs.*` checks
  every param and that no operator source passes one again. Defaults that measure something are
  authored at a 20-unit scalp (`kDefaultsScalpSize`, a head in cm, as the hair presets): a node
  added with the groom's size (`GraphEdit::addNode`'s `groomSize`, Maya's `GraphModel.groom_size`,
  the app's document) takes them fitted, as presets fit their params; a new groom on a scalp
  stores the default graph fitted the same way (`defaultGraphJsonFor`, Maya's
  `create_groom(fit=True)` from the menu). Maya measures the scalp in centimeters, its internal
  unit, whatever the UI's linear unit (`presets.scalp_size`).
- Goldens must match exactly in counts and IDs, within 1e-4 in values; re-record only for an
  intended change.
- **A new feature lands on the CPU first.** The CPU operators are the reference, and the GPU
  preview (`look_chain.h`) hands any node it cannot run to the CPU. So a new node, or a new param
  on a node the GPU runs, first ships CPU-only: the GPU refuses it (`findLookChain` for a node,
  `readLookParams` for a param away from the value the kernel already matches), and brushing or
  dragging over it evaluates on the CPU. Porting it to `look.slang` is its own later change, with
  the GPU tests comparing the two. Never let the GPU draw a param it ignores.
- Randomness is seeded from strand IDs, never from thread order or time; parallel and serial
  runs give the same output (a test checks).
- Maya attribute short names start with `gl`; the shape draws through a subscene override
  (`drawdb/subscene/...`); node type IDs sit in Autodesk's local range.
- Never assign a null texture to a Maya shader parameter (it crashes inside Maya); never free a
  vertex buffer an item still references.
- Every change ships with a test: a core test for core behavior, a `test_grooming` case for
  Maya behavior, an editor case or a GUI check for UI behavior.
- Adding, removing or upgrading a dependency that ships inside the binaries (or a DLL beside
  them) updates `THIRD_PARTY_NOTICES.txt` at the repository root: its version, what uses it and its
  license text in full. It installs with the module and goes in every package. Build- and
  test-only tools (GoogleTest) and what the host provides (Maya, Arnold, Qt, Python) stay out.

**Node types: one line each.** `engine/core/include/groomlab/builtin_operators.inc` lists every
built-in type in the add-node menu's order (`op::k*`, the register functions and their calls come
from it), and each operator registers with its menu info (`NodeInfo`: category, description, search
words) next to its class; `node_catalog.cpp` keeps only the categories. The GPU preview has the same
for its kernels (`look_kernels.inc`: the enum, both backends' tables, the CMake build) and stages
(`look_stages.inc`: kind, name, mapped fields, the params a drag may set, the pass-through types).
`tools/dev/new_node.py` adds a node; [EXTENDING.md](EXTENDING.md) is the walk-through.

**The brush catalog and presets inserted.** `brush_catalog.h` is the one list of tools both
palettes are built from (id, label, category in order, icon, default strength, signed, a hint,
the settings each card shows with `brushSettingSpecs` giving every setting's label and
direction); Maya's `palette.py` reads it through `groomlab_core.brush_catalog()` (its SECTIONS,
NAMES, HINTS and which boxes a tool shows all come from it) and the app's `theme::brushes()`
and the rail from the C++ side. A tool's card in Maya and the app shows what the catalog's
`settings` name; a new tool is a catalog entry, an icon in both hosts' icon folders and the
host's start of it. `presets.h`'s `insertPresetJson` adds a preset to a graph instead of
replacing it: the look between the preset's sources and its Output as one Compound after a
node (GraphEdit checks the wiring; the preset's Scalp or Guides become Compound inputs wired
to the graph's), used by Maya's `GraphEditor.insert_preset` and the app's `insertPresetFile`;
the Add-ons presets (`hosts/maya/presets/wet.json`...) are small complete graphs made for it,
and `Presets.EveryAddOnInsertsIntoEveryBasePreset` inserts each into every shipped preset.

## 7. Maya API notes

Notes that saved time before:
- MSyntax refuses some short flags (`-a`/`-add`, `-n`/`-new`) and any short flag longer than
  three characters; the plugin loads, but the flag silently does not exist
  (`cmds.help("groomLabX")` lists what registered).
- `scriptJob` events coalesce (two `SceneSaved` in one idle become one); the plugin uses
  `MSceneMessage` callbacks for anything that must see every event.
- Textures made from raw pixels (`MTextureManager::acquireTexture`) sample bottom row first,
  as `MImage` stores files; a top-down buffer draws flipped.
- `MFileIO::currentFile()` names an untitled scene under the project; `file -q -sceneName` is
  empty for it.
- Nodes a command makes with function sets (`MFnTransform::create`, `MFnNurbsCurve::create`)
  must not be deleted by an `MDGModifier` in `undoIt`: a modifier kept to hold them for redo
  brings Maya down when the undo queue is flushed (a new scene) while they are undone, and a
  local one leaves stale names that crash the next `setName`. `groomLabConvert` deletes them with
  the `delete` command outside the queue and makes them again on redo.
