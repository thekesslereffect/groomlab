# GroomLab in the browser (WebAssembly)

A proof of concept of the web runtime: the real core (`engine/core`), built to WebAssembly with
Emscripten, evaluating a `.groom` in the page and feeding the website's three.js viewer. The page
is `/view` (`web/app/(home)/view/page.tsx`): a sample groom, its sliders, a density slider,
**Download .groom**, and drag and drop of your own `.groom` (with its `.obj`, or a preset `.json`
with an `.obj`) together with the texture maps it reads, or its whole folder (see "Maps").

What runs is the desktop engine, not a port: the same sources, the same portable math. The
module's strands are **bit for bit** the desktop's (see "Checking it").

## Pieces

| Where | What |
| --- | --- |
| `engine/web/src/web_groom.{h,cpp}` | `WebGroom`: a .groom (or a preset + OBJ) loaded into a `GroomSession`, its sliders, edits, evaluation into flat buffers, saving. Plain C++, no Emscripten. |
| `engine/web/src/bind.cpp` | Its embind binding (the browser build). |
| `engine/web/src/web_probe.cpp` | `groomlab_web_probe`: the same `WebGroom` built natively, for comparisons. |
| `engine/web/tools/compare.mjs` | The module in Node, printing what the probe prints. `diff_positions.mjs` compares raw dumps. |
| `engine/web/tools/make_sample.mjs` | Writes `web/public/samples/manny_hair.groom` and its texture map `masks/manny_hair/highlights.png` with the module. |
| `engine/web/tools/make_home_samples.mjs` | Writes the Try section's groom (`manny_curls.groom`) with the module. (The hero's, `webhero.groom`, is groomed in Maya and exported.) |
| `engine/web/tools/make_hair_bsdf.mjs` | Copies `engine/gpu/shaders/hair_bsdf.glsl` into `web/components/home/hair-bsdf.ts` (the viewer shades with it, as the app's lookdev and Maya's viewport do); `--check` says whether the copy is in step. Run it when the shading changes. |
| `engine/web/tools/test_maps.mjs` | Checks the module's maps (vertex maps, texture maps by path, by name, UDIM, `..`, missing, re-read). |
| `engine/web/tools/png.mjs` | A small gray PNG writer for those two. |
| `web/public/wasm/groomlab.{js,wasm}` | The build's output (copied there by the build). Committed, so the site builds without Emscripten. |
| `web/public/wasm/groomlab-worker.js` | A classic worker that loads the module and answers the page. |
| `web/vercel.json` | The site's cache headers: `/samples/*` and `/wasm/*` kept by the browser for an hour, then used while revalidated in the background (a week): a page loaded again starts the engine without a round trip. A change to those files reaches a returning visitor within the hour. |
| `web/lib/groomlab-wasm/index.ts` | The page's typed client of that worker. |
| `web/components/view/groom-view.tsx` | The `/view` page: viewer, sliders, open, download, drop. `?groom=<name>` opens `/samples/<name>.groom`. |
| `web/components/home/home-engine.ts` | The home page's grooms grown by the engine (see "The home page"). |

## Building it

Emscripten lives outside the repository, in `%USERPROFILE%\groomlab-deps\emsdk` (any folder works):

```
git clone https://github.com/emscripten-core/emsdk %USERPROFILE%\groomlab-deps\emsdk
cd %USERPROFILE%\groomlab-deps\emsdk
emsdk install latest
emsdk activate latest
```

Then, in a shell where `emsdk_env` has run (it sets `EMSDK` and puts `emcc` on `PATH`) and `ninja` is
on `PATH` (on Windows, Visual Studio's: `tools/dev/vsenv.ps1`, or
`Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja`):

```
cmake --preset wasm
cmake --build --preset wasm
```

The `wasm` preset builds the core and `engine/web` only (no CLI, tests, Python, C API, Alembic,
Maya, renderers, TBB) into `build/wasm` and copies `groomlab.js` and `groomlab.wasm` to
`web/public/wasm/` (`GROOMLAB_WEB_PUBLIC_DIR`; empty: no copy). A full build takes about a minute.
Then `node engine/web/tools/make_sample.mjs` writes the sample groom again, if wanted.

What it took to compile: nothing beyond the existing guards. The SSE2 paths fall back to their
scalar code (Emscripten does not define `__SSE2__` without `-msse2`), `engine/gpu` leaves itself
out off Windows, TBB is off (the serial backend, `parallel_serial.cpp`), and one cast in
`node_library.cpp` (Emscripten's libc++ counts file times in `__int128`). C++ exceptions are
WebAssembly exceptions (`-fwasm-exceptions`, top-level `CMakeLists.txt`, under `if(EMSCRIPTEN)`). Since the look stages run
from `look.slang`'s generated C++ (0.33.0), Slang's C++ prelude needs one guard: it knows no
WebAssembly target, so `look_kernels_cpu_support.h` names the platform Linux's for Emscripten and
reads `<float.h>` first, keeping the prelude's `half` a struct (wasm32 has no `_Float16`).

## The JavaScript API

The worker client (`web/lib/groomlab-wasm`), all calls async:

```ts
import { createGroomEngine } from '@/lib/groomlab-wasm';

const engine = createGroomEngine();            // starts public/wasm/groomlab-worker.js
const { controls, scalp, version, missing } = await engine.loadGroom(groomText, objText?, files?);
if (missing.length) await engine.addFiles({ 'masks/hair/density.png': bytes });  // or addFile(path, bytes)
await engine.setParam('control:frizz', 2.5);   // or setParams([[id, value], ...])
const { strands, guides, evaluateMs, errors } = await engine.evaluate({ density: 0.25 });
viewer.setStrands(strands);                    // StrandData: count, offsets, positions, colors (sRGB)
const bytes = await engine.saveGroom();        // the edited .groom
```

- `loadGroom(groom, mesh?, files?)`: a `.groom`'s text (its scalp is inside it), or a preset's graph JSON
  with an OBJ to grow on (fitted to the mesh as hosts fit presets). An OBJ given with a `.groom`
  replaces its scalp. `files`: the files it reads (`{path: bytes}` or `[path, bytes][]`, as
  `addFiles`); the previous groom's files are dropped. The result also carries `files`
  (`referencedFiles()`), `missing` and `failed`.
- `addFile(path, bytes)`, `addFiles(files)`: texture maps (any file the groom reads) into the
  module's memory file system, where the engine's own loaders read them (see "Maps"). Returns
  `{files, missing, failed}`; evaluate again to see them.
- `missingFiles()`: the stored paths of the files the groom reads and nobody gave, for the page to ask
  for. `referencedFiles()`: all of them, `{path, resolved, found, kind, udim, users}`.
- `fetchGroomFiles(missing, groomUrl)` (a plain function): the missing files fetched relative to a
  groom loaded by URL, ready for `addFiles` (what `/view` does for the bundled sample).
- `listControls()`: the sliders, `{id, kind, label, group, node, param, min, max, step, default,
  value, unit, description}`: first the **Control nodes** (`control:<name>`, their own min, max and
  default), then the **exposed params** (`param:<node>.<param>`, ranges and defaults from the
  engine's param specs): each Compound's exposed params; else the graph's `"exposed"` list (a
  preset's, kept when a preset is loaded); else a default pick of the look's key params (one plain
  number param per look node, Clump, Curl, Noise... first).
- `setParam(id, value)`: a Control value goes to `GroomSession::setControls`; a node param is written
  into the graph and set with `setGraphJson`, which keeps every node whose inputs did not change.
  Values sent while an evaluation runs are coalesced (the latest per slider).
- `evaluate({density})`: the preview density (default 0.25). Strands come back as typed arrays,
  transferred from the worker.
- `setBypass([[node, bypassed], ...])`: the graph's own `bypass` flag on nodes, set with `setGraphJson`
  (the nodes before them stay cached).
- `slot(name)`: another groom in the same worker, with the same calls (one engine, several grooms: the
  home page's viewers). Disposing a slot unloads its groom; disposing the engine ends the worker. The
  slots share the module's memory file system (`/groom`), so their files do too.
- `scalpMesh()`, `saveGroom()`, `dispose()`.

The embind class (`WebGroom`, in the worker or in Node: `createGroomLab()` from `groomlab.js`) has
the same calls synchronously (`load`, `controlsJson`, `setParam`, `evaluate`, `offsets`,
`positions`, `colors`, `guideOffsets`, `guidePositions`, `scalpPositions`, `scalpNormals`,
`scalpIndices`, `statsJson`, `saveGroom`, `graphJson`, `setGraphJson`, `bakeGuides`, `filesJson`,
`missingFilesJson`, `addFile(path, Uint8Array)`, `clearFiles`, `setDirectory`, `directory`); its
buffers are views into the module's memory, valid until the next call.

## Maps

Every map a `.groom` can use works as on the desktop, read by the same code:

- **Painted vertex maps** (and color sets): inside the `.groom` (the scalp's `colorSets`, the
  `maps` catalog's `"kind": "vertices"`), so they need nothing more.
- **Texture maps**: image files the groom names by path: a map param's `{"texture": path}`, a
  painted texture map (`maps`, `"kind": "texture"`; the standalone app keeps them in
  `masks/<groom name>/` beside the groom), a Map node or HairColor's `map` naming an image, FeatherColor's
  textures. Formats: the core's loader (`texture.h`): PNG (8 and 16 bit), JPEG, TGA, BMP, PSD, GIF,
  HDR, PIC, PNM. UDIM tile sets (`<UDIM>`, `<udim>`, `%(UDIM)d`, or 1001-style tile names) too.
- **Referenced grooms** (a Reference node's `.groom`) are listed and read the same way.

The module is linked with Emscripten's in-memory file system (MEMFS, its default; no NODEFS, nothing
of the host's): the folder `/groom` stands for the `.groom`'s folder, and the session resolves
relative paths from it, then the groom's `searchPaths`, exactly as `GroomScene` resolves a `.groom`
on disk. `addFile(path, bytes)` writes a file there: `path` relative to the groom's folder (`..` allowed;
an absolute path as it is), or just a name. After each file, every referenced file still missing
whose name is the file's (any case; for a tile set, the pattern with a tile number in it) gets a copy
where the engine reads it, so maps given without their folders are found. The images are then
decoded by stb_image in the module, as on the desktop: strands and colors come out bit for bit the
same (see "Checking it"). A file rewritten later is read again (the texture cache keys on the file's
stamp).

`/view` takes several files at once (Open, a drop, or the folder button / a dropped folder, walked
with `webkitGetAsEntry`): the shallowest `.groom` (else a preset `.json`), the first `.obj`, and every
image (and `.groom`) under it, as paths relative to the groom's folder. Images dropped without a
groom go to the groom open. A groom loaded by URL (the sample) fetches what it misses relative to its
URL. What is still missing shows as "Missing: <files>" with a picker; the groom evaluates again once
they are added (a missing texture map is an error of its node, as on the desktop).

The bundled sample uses both kinds: `cut`, a vertex map (shorter on the head's right, through a
Length node's keep), and `highlights`, a texture map
(`web/public/samples/masks/manny_hair/highlights.png`, drawn by `make_sample.mjs`, no third-party
image) giving HairColor's greys light streaks and face-framing pieces.

## Size and speed (2026-10-21, Emscripten 6.0.13, -O3)

| | raw | gzip -9 | brotli 11 |
| --- | --- | --- | --- |
| `groomlab.wasm` | 3.98 MB | 1.49 MB | 1.04 MB |
| `groomlab.js` | 98 KB | 26 KB | 23 KB |
| `-Oz` instead (not used) | 2.35 MB | 0.95 MB | 0.72 MB |

The file system (maps) added 27 KB to the `.wasm` (10 KB gzip) and 51 KB to the `.js` (13 KB
gzip: Emscripten's FS, MEMFS and TTY code): 23 KB more to download gzipped, 18 KB with brotli.

`-Oz` is 1.4 to 1.7 times slower to evaluate. The sample groom (Hair basics on the Manny head,
25,843 strands at full density), single-threaded, in Node 24 on the development machine:

| density | strands | first evaluation (fresh page / warm) | melanin changed | clump strength changed | frizz changed |
| --- | --- | --- | --- | --- | --- |
| 0.25 | 6,442 | 250 ms / 117 ms | 3 ms | 13 ms | 12 ms |
| 1.0 | 25,843 | 560 ms / 282 ms | 10 ms | 54 ms | 47 ms |

Headless Chrome (SwiftShader) showed the same: 252 ms for the page's first evaluation, 19 ms after
four sliders moved. The native probe (MSVC, also single-threaded) takes 237 ms and 563 ms for the
fresh evaluations: WebAssembly runs here at about native single-thread speed. Edits after the first evaluation re-run only
the nodes after the one changed, so sliders stay live at full density.

## The home page

Every strand on the home page is the engine's: two viewers, one worker, no stand-in. `strand-3d.ts`
draws the bare Manny head at once, then, once the page is idle, imports `home-engine.ts`, which
starts the page's one engine (`createGroomEngine`, so `groomlab.wasm` is only fetched then) and
loads each viewer's groom into its own slot. The engine's strands, scalp and guides come into the
same canvas. Without WebAssembly or workers the head stays bare; without WebGL2 the viewport stays
empty.

| Viewer | Groom | Strands drawn | What the page sets |
| --- | --- | --- | --- |
| Hero | `webhero.groom`: a ball of curls on a 5 cm sphere, groomed in Maya and exported (its HairColor at melanin 0.6, redness 1); its own space (`space` in `engine-grooms.ts`), no bare head while it loads | ~22,000 (phones ~12,000) | nothing |
| "Try the node graph" | `manny_curls.groom`: Curly, tuned, on guides standing off the head | ~7,000, more with Scatter's density, up to 16,000 | each card's switch: `setBypass` on `clump`, `curl`, `noise`, `color`; each slider: the groom's `"exposed"` params, ranges and labels from `listControls()`; the seed |

`manny_curls.groom`'s chain is the Try section's, node for node: Scalp, Scatter, Interpolate (with
the Guides node), Transform (`length`, scale 1.8), Clump, Curl, Noise, HairColor, Output. Its Scatter
density is a plain number (a slider) and keeps to the hair's part of the head by `guideRadius`
around the guides' roots.

The viewer shades the strands with GroomLab's hair shading (`hair_bsdf.glsl`, as the app and Maya's
viewport do), the groom's highlight from the engine (`evaluate`'s `look`: core's `hairLobes` of its
HairColor), the strands' appearance color turned back into their base color, the hair volume's depth
standing in for Maya's density grid, lit by a key and a camera light and shown through ACES (Maya's
default view).

`node engine/web/tools/make_home_samples.mjs` writes the Try groom (the Curly preset on the Manny head,
guides baked from a small combing graph) and prints its controls and full strand count:
`web/components/home/engine-grooms.ts` mirrors them (the page draws its sliders before the engine
answers), so change both together. The preview density is a share of the full count.

Edits go to the engine as they come (only what changed); evaluations run one at a time, at most one a
frame, with the latest edits. Each result is moved into the viewer (the scalp painted with the hair's
root color under the hair when the roots or their colors changed). `window.__groomlabHome` holds the
page's timings per viewer: milestones (ms since the page started) and, per evaluation, the engine's
time, the round trip and the page thread's share.

Measured 2026-10-21 (headless Chrome, SwiftShader, the static build served locally, 1440 x 900):

- First load: the viewers' 3D chunks load at ~0.75 s, the engine is asked for at ~1.2 s (idle),
  the module is ready ~0.15 to 0.26 s later (cold cache: fetch and compile), the hero's first
  evaluation takes ~300 ms in the engine (~370 ms round trip) and its strands are on screen at ~2.0 s;
  the play bust's (~160 to 200 ms) follow at ~2.2 s, the Try section's (~90 to 100 ms) when it nears
  the screen.
- Edits: Try's sliders re-evaluate in 5 ms (melanin) to 20 to 95 ms (Curl's frequency, by how many
  points Curl adds), 50 to 180 ms round trip with the copies; the page thread spends 2 to 17 ms a
  result (and 5 to 40 ms when the scalp is painted again).
- Dragging a slider (60 values, one a frame): no long task on the page's thread (the old procedural stand-in:
  9 long tasks, 604 ms, for Curl's radius); median frame 16.5 ms. The slow frames left (p90 ~100 to
  160 ms) are SwiftShader drawing the new strands in software.
- The home page's initial JS: +3.8 KB raw, +1.3 KB gzip (the grooms' values for the sliders);
  three.js, the engine client and `home-engine.ts` load lazily, `groomlab.wasm` from the worker.

## Checking it

`groomlab_web_probe` is `WebGroom` built natively: configure any native preset with
`-DGROOMLAB_BUILD_WEB=ON` and build that target. Then compare:

```
groomlab_web_probe web/public/samples/manny_hair.groom --density 0.25 --dump native.f32
node engine/web/tools/compare.mjs web/public/samples/manny_hair.groom --density 0.25 --dump wasm.f32
node engine/web/tools/diff_positions.mjs native.f32 wasm.f32
```

On 2026-10-21 every case matched bit for bit (strand counts, point counts, a hash of every
position's bits): the sample at densities 0.25, 0.5 (with three sliders moved) and 1, the strokes
test groom, and Creature fur, Curly, Dragon scales and the head lookdev graph on their meshes. The
shipped `groomlab-cli` 0.24.0 (TBB on, multi-threaded) exporting the sample to USD
(`eval ... -o sample.usda -f 0 --density 0.25`, and 1) gives the same points to the bit: 328,542 and
1,317,993 floats.

Both tools also print a hash of the colors (sRGB per point, converted with `pm::pow` so every C
runtime gives the same bits) and take `--dump-colors f`. The probe reads the groom's texture maps
from disk relative to its folder; `compare.mjs` reads the same files and hands them to the module
with `addFile` at the paths the groom stores (`--no-files` leaves them out). With maps (2026-10-21,
after the sample gained its two maps): the sample at 0.25, 0.5 with three sliders moved, and 1
(25,843 strands), and `tests/data/compat/v1/groom.groom` (its scatter density read from
`../../textures/density.pgm`) matched bit for bit in strands, points, positions and colors.
`node engine/web/tools/test_maps.mjs` checks the rest in the module: missing files listed, files
found by plain name, UDIM tiles by name in any case, `..` paths, a rewritten texture read again, and
that both sample maps change the strands.

## Limits

- **Single-threaded.** Threads need `-pthread` (Emscripten's pthreads on Web Workers, with the
  core's own pool backend, `parallel_pool.cpp`, instead of the serial one) and a page that is
  cross-origin isolated: served with `Cross-Origin-Opener-Policy: same-origin` and
  `Cross-Origin-Embedder-Policy: require-corp` (or `credentialless`), for `SharedArrayBuffer`. The
  static export would need those headers from its host (Vercel: `vercel.json` headers), and every
  cross-origin asset on the page then needs CORP or CORS.
- **Files: maps only.** Texture maps and referenced grooms are read (see "Maps"); Alembic caches
  (scalp, guides, simulation bakes) are not (no Alembic in the module), nor `ImportCurves`, Ptex
  (`.ptx`: only XGen import reads it) or EXR and TIFF (the desktop's loader does not read them
  either). A referenced groom's own texture maps are not listed in `missingFiles()` (they are read if
  given at the paths it resolves). A UDIM set cannot be fetched by URL (no listing over HTTP): a
  groom loaded by URL asks for its tiles. Files live in the module's memory, as many copies as were
  given (plus a copy per name match), until the next groom loads.
- **No GPU look preview** (`engine/gpu` is Windows only); evaluation is the CPU engine.
- **Instances with prototypes of their own** (`"prototypes"` in a `.groom`) are not handed to the
  session yet; cards, feathers and scales draw as their strands only.
- **Memory**: up to 2 GB (`-sMAXIMUM_MEMORY`); grows as needed from 64 MB.
- The module is a build product committed to the repository (about 4 MB a build): rebuilding it
  rewrites the file, so commit it only when the engine changed.
