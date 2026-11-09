# GroomLab on macOS: the porting guide

For the Claude Code session on the MacBook (Apple silicon) that finishes the macOS port. GroomLab
has never been built on macOS. The groundwork was laid on Windows on 2026-10-20 (presets, platform
guards, this guide) and checked there only so far as Windows and Linux allow: nothing below the
"Prepared on Windows" list has run on a Mac yet. Work top to bottom, tick the checklist at the end,
and fix what breaks in small, separate commits.

**What the Mac has (2026-11-09):** Maya 2024, 2025, 2026 and 2027, Arnold (MtoA) for each, and
V-Ray for Maya. Not RenderMan: its procedural is left out by itself (no `RMANTREE`, the configure
says "No RenderMan SDK ... left out") and its tests with it. The Maya devkits for macOS still have to
be downloaded (section 2). The Windows session that wrote this guide has notes the Mac cannot see:
everything the Mac needs is here and in `docs/DEVELOPING.md`.

## Rules that still apply on the Mac

- **The user's work files are off limits.** On Windows they are on the D: drive; on the Mac, any
  folder the user names as work (studio projects, client files). Never read, copy, commit or
  publish their names, data or pictures.
- **No publishing, no pushing, no CI** until the user says so: commit on a branch, locally.
  No R2 uploads, no `tools/publish_*.py`, no `git push` (Vercel deploys from main).
- **Proprietary SDKs are never committed** (Maya devkit, MtoA, RenderMan, V-Ray): they live in
  `~/groomlab-deps/sdks/` or their default install folders. `BUILDING.md`, "Keeping SDKs out".
- **Small, focused changes.** One topic per commit; keep Windows and Linux byte-identical: new
  platform code goes behind `#ifdef __APPLE__` / `if(APPLE)`, never into the shared paths.
- **Tests by label**, not everything every time: `ctest --preset macos-core -L quick` after a
  change (section 12); the GUI tiers (`-L gui`) are Windows-only for now.
- **No format migrations.** Formats change directly; never add back-compat readers.
- Scripted Maya sessions must never open a modal dialog the user has to click.

## Prepared on Windows (what to expect in the tree)

| Area | What was done | Files |
|---|---|---|
| Presets | `macos-core`, `macos-app`, `macos-maya2024` to `macos-maya2027` (Ninja, Apple Clang, arm64, deployment target 13.3; 2024 and 2027 added 2026-11-09 from 2025's, their paths guesses to check) | `CMakePresets.json` |
| GPU engine | Built without a backend off Windows when a host needs it: planning + run algorithm + a null backend whose factories throw, so hosts preview on the CPU | `engine/gpu/CMakeLists.txt`, `engine/gpu/src/look_engine_none.cpp`, `engine/gpu/src/line_frames.cpp`, `engine/gpu/src/look_chain.cpp` |
| Maya plugin | DirectX 11 / WGL code behind `_WIN32`: no device is found, the GPU preview and GPU frames say why and the CPU draws. Maya classes declared through `<maya/MApiNamespace.h>`, not forward declarations (from Maya 2023 `MHWRender::X` is a using-declaration of a versioned namespace's class: GCC and Clang reject a plain `class X;` beside it, MSVC let it pass) | `hosts/maya/src/gpu_preview.cpp`, `gpu_lines.cpp`, `groom_draw_override_gpu.cpp` (`<OpenGL/gl.h>`), the plugin's headers, `hosts/maya/CMakeLists.txt` |
| Maya find | `MAC_PLUGIN`; libraries from the devkit's `lib/` or `Maya.app/Contents/MacOS` (`GROOMLAB_MAYA_APP`), else `-undefined dynamic_lookup`; the plugin is a `MODULE` named `GroomLab.bundle` | `cmake/FindMaya.cmake`, `CMakeLists.txt` |
| TBB | Maya's TBB on macOS (`libtbb.dylib` / `libtbb.12.dylib` in Maya.app), else headers + dynamic lookup; installs set `@loader_path` and copy Homebrew's `libtbb.12.dylib` beside the binaries | `cmake/GroomLabTBB.cmake` |
| Python module | Maya devkit on macOS: no libpython linked; the interpreter is `GROOMLAB_MAYA_APP/bin/mayapy` or `GROOMLAB_PYTHON_ROOT/bin/python3.x` | `engine/python/CMakeLists.txt` |
| Core | `scalp_mesh.cpp` SSE box distance has a scalar twin (arm64); `.groom` writer reads its own float text with `strtod_l` in the C locale (Apple libc++ has no floating `std::from_chars`) | `engine/core/src/scalp_mesh.cpp`, `engine/core/src/groom_file.cpp` |
| C API | `-exported_symbol _gl_*` (Linux's version script); the worker threads pin the dylib with `dlopen(RTLD_NODELETE)` (Linux: `-z nodelete`) | `engine/capi/CMakeLists.txt`, `engine/core/src/parallel_pool.cpp` |
| Executable path | `_NSGetExecutablePath` where Linux reads `/proc/self/exe` | `engine/cli/mcp_server.cpp`, `hosts/blender/helper/src/main.cpp` |
| Blender helper | Shared memory names `gl_<pid>_...` on macOS (31-byte `PSHMNAMLEN`); the rest was POSIX already | `hosts/blender/helper/src/shared_memory.cpp` |
| Arnold | Procedural named `groomlab_procedural.dylib` (what Arnold loads on macOS); `~/groomlab-deps/sdks/arnold-macos` as the default Arnold SDK | `hosts/arnold/CMakeLists.txt`, `CMakeLists.txt` |
| RenderMan | `/Applications/Pixar/RenderManProServer-27.3` as the default `RMANTREE` | `CMakeLists.txt` |
| App | Offscreen renders on Metal by default (they fell to the Null backend before) | `hosts/app/src/offscreen.cpp` |

Checked from Windows: `ninja-maya2022` and `ninja-app` build and pass `-L quick`; `linux-core`
builds and passes `-L quick` in WSL; and, as a one-off check (not a supported build), the whole
Maya plugin with the backend-less GPU engine compiled and linked with GCC on Linux against the
Maya 2025 devkit's headers, which exercises every non-Windows branch above except the
`__APPLE__` ones (those are small; Apple Clang sees them first).

Already portable before (no change needed): `engine/core` (POSIX branches for Linux; SSE2 paths
with scalar fallbacks: `frames.cpp`, `noise.cpp`, `interpolate.cpp`, `simulation.cpp`),
`presets.cpp` (`~/Library/Preferences/Autodesk/maya`), the app's `Q_OS_WIN` guards and QRhi
backend choice (Metal on macOS: `viewport.cpp`), `GroomLabSDKConfig.cmake` (`libgroomlab.dylib`),
the Blender add-on's platform tag (`macos-arm64`), the helper's `"macos"` platform name.

## 1. Prerequisites

1. **Xcode** 16 or later from the App Store (Apple Clang, the macOS SDK, `xcrun metal` for the
   Metal backend later), then `sudo xcode-select -s /Applications/Xcode.app` and
   `xcodebuild -runFirstLaunch`. The Command Line Tools alone build the engine but have no Metal
   compiler.
2. **Homebrew** (`/opt/homebrew`, Apple silicon), then:
   ```sh
   brew install cmake ninja tbb python@3.12 git
   ```
   CMake 3.25 or later (presets version 6). `tbb` is oneTBB (TBBConfig.cmake, found through the
   presets' `CMAKE_PREFIX_PATH=/opt/homebrew`). `python@3.12` is for the engine's Python module and
   the tests (pybind11 2.13.6 supports 3.7 to 3.13); `python3 -m pip install --user numpy` for the
   `blender.helper` test when Blender is not installed.
3. **Qt 6.8.3** for the app (the Windows build's version), not Homebrew's newer Qt:
   ```sh
   python3.12 -m venv ~/groomlab-deps/tools/aqt-venv
   ~/groomlab-deps/tools/aqt-venv/bin/pip install aqtinstall
   ~/groomlab-deps/tools/aqt-venv/bin/python -m aqt install-qt mac desktop 6.8.3 clang_64 -m qtshadertools -O ~/Qt
   ```
   Homebrew's Python refuses `pip install --user` (PEP 668: externally managed), hence the venv.
   It lands in `~/Qt/6.8.3/macos`, 4.9 GB (the `macos-app` preset's `GROOMLAB_QT_DIR`). The Qt online
   installer works too (same folder).
4. **Slang** (optional at first): only the `gpu.*kernel_targets` tests and the Metal backend need
   `slangc`. Download `slang-2026.19-macos-aarch64.zip` from
   github.com/shader-slang/slang/releases (the version Windows pins), unpack it into
   `~/groomlab-deps/sdks/slang-2026.19-macos`, `xattr -dr com.apple.quarantine` that folder, and
   pass `-DGROOMLAB_SLANGC=~/groomlab-deps/sdks/slang-2026.19-macos/bin/slangc`. When the backend
   needs it at build time, pin the zip by SHA-256 in `engine/gpu/CMakeLists.txt` like the Windows
   one (ask the user before downloading).

Dependencies fetched at configure time (Alembic 1.8.8, Imath 3.1.12, pybind11 2.13.6, GoogleTest
1.15.2, nlohmann/json 3.11.3) are plain CMake projects that build on macOS; their sources are cached
in `~/.cache/groomlab-deps/fetch` (`GROOMLAB_FETCH_CACHE`).

## 2. Proprietary SDKs (download on the Mac, never commit)

| SDK | Where | Used by |
|---|---|---|
| Maya 2024 to 2027 install (installed) | `/Applications/Autodesk/maya<year>` (`Maya.app/Contents/MacOS`: libraries; `Maya.app/Contents/bin/mayapy`) | running the plugin; `GROOMLAB_MAYA_APP` |
| Maya devkit, macOS, per version (to download) | Autodesk Platform Services, "Maya devkit" for macOS: unpack `devkitBase` to `~/groomlab-deps/sdks/maya-devkit-<year>-macos` (it holds `include/maya`, `include/Python3xx`, `include/tbb`) | `macos-maya<year>` `MAYA_LOCATION` |
| MtoA for macOS (installed) | installed with Arnold for Maya: `/Applications/Autodesk/Arnold/mtoa/<year>` (`include/mtoa`, `bin/libmtoa_api.dylib`, `bin/libai.dylib`) | the MtoA translator |
| V-Ray for Maya (installed) | find it (`ls -d /Applications/Chaos*/V-Ray/*` or `/Applications/ChaosGroup/...`): the folder per Maya version holding `vray/include/vray_maya_sdk/translator_interface.h`; pass it as `-DVRAY_FOR_MAYA_LOCATION=...` | the V-Ray for Maya translator (section 4, item 11: its build is Windows-only today) |
| Arnold SDK for macOS (optional) | `Arnold-7.x-darwin.tgz` unpacked to `~/groomlab-deps/sdks/arnold-macos` | the Arnold procedural without MtoA |
| RenderMan Pro Server 27.3 (not installed) | `/Applications/Pixar/RenderManProServer-27.3` or `RMANTREE` | the RenderMan procedural: left out without it |
| V-Ray Standalone (optional) | as installed; `GROOMLAB_VRAY` / the app's Render > V-Ray Standalone Location | `vray` tests only |

Each preset's paths can be overridden on the command line (`cmake --preset macos-maya2025
-DMAYA_LOCATION=...`) or in a `CMakeUserPresets.json` (ignored by git).

## 3. Build order (commands)

From the repository root. Each step stands alone; fix it before the next.

```sh
# 1. Engine, C API, CLI, Python module, Blender helper, tests (no proprietary SDK)
cmake --preset macos-core
cmake --build --preset macos-core
ctest --preset macos-core -L quick -j 8

# 2. The standalone app (Qt 6.8.3, Metal)
cmake --preset macos-app
cmake --build --preset macos-app
ctest --preset macos-app -L "quick|app" -j 8
./build/macos-app/hosts/app/GroomLab          # a plain executable for now (see 6)

# 3. The Maya plugin (with Maya and its macOS devkit installed): 2025 first, then 2026, 2027, 2024
cmake --preset macos-maya2025 -DVRAY_FOR_MAYA_LOCATION=<V-Ray for Maya 2025's folder>
cmake --build --preset macos-maya2025
cmake --install build/macos-maya2025 --component GroomLab --prefix ~/groomlab-modules
```

Then point Maya at the module: `MAYA_MODULE_PATH=~/groomlab-modules` in
`~/Library/Preferences/Autodesk/maya/2025/Maya.env`, start Maya, load `GroomLab.bundle` in the
Plug-in Manager. Each version installs into the same module (`maya<year>/plug-ins`), as on Windows.
Start with 2025 (the presets' first, Qt 6 and Python 3.11); 2024 is the odd one out (Qt 5 and
PySide2, Python 3.10, TBB 2020: section 7).

## 4. Expected first failures (not verifiable from Windows)

In rough order of likelihood. Fix each behind `__APPLE__`/`APPLE` unless the fix is plainly
portable (a cast that is correct everywhere).

1. **`size_t` is not `uint64_t` on macOS.** On Windows and Linux both are the same type; on macOS
   `size_t` is `unsigned long` and `uint64_t` is `unsigned long long` (and `int64_t` is `long long`,
   not Linux's `long`). Expect ambiguous overloads (a `size_t` passed to functions overloaded on
   `uint64_t` and `uint32_t`, e.g. hashing helpers), `std::min`/`std::max`/`std::clamp` failing to
   deduce one type from a `size_t` and a `uint64_t`, and `uint64_t*` vs `size_t*` mismatches. Fix
   with an explicit cast or template argument (`std::min<std::uint64_t>(...)`): portable, no guard
   needed. Maya's `MInt64` is `long long` on macOS too.
2. **Apple libc++ gaps.** Floating-point `std::to_chars` needs macOS 13.3 (the deployment
   target); floating `std::from_chars` is missing (already handled in `groom_file.cpp`; look for
   others with `grep -rn from_chars`). Anything else libc++ lacks shows as "unavailable" or
   "no member": guard it the same way.
3. **New warnings.** `groomlab::warnings` is `-Wall -Wextra -Wpedantic` without `-Werror`, but the
   SDK sample `groom_counts` builds with `-Werror`; Apple Clang deprecates `sprintf` and warns on
   things GCC does not. Fix the code, don't silence globally.
4. **Determinism on arm64.** The goldens (`core.golden`, `test_determinism`, `test_compat`) are
   recorded on x64. The SSE2 paths have scalar twins meant to give the same bits, and
   `-ffp-contract=off` stops Clang fusing multiply-adds (it does on arm64 by default), but arm64
   has never been compared. A golden mismatch is a real finding: find the operation that differs
   (scalar fallback vs SSE lane, libm's double functions under `portable_math.h`) before touching
   any golden. Never re-record goldens to make the Mac pass.
5. **Maya devkit layout.** `FindMaya.cmake` assumes `include/maya` in the devkit and libraries in
   its `lib/` or `Maya.app/Contents/MacOS`. If the configure log says it links with
   `-undefined dynamic_lookup`, the plugin still loads in Maya, but prefer setting
   `GROOMLAB_MAYA_APP` right. The devkit's own `cmake/devkit.cmake` is the reference.
6. **Maya's Python.** `engine/python/CMakeLists.txt` reads the devkit's `include/Python3*/Python`
   and runs `GROOMLAB_MAYA_APP/bin/mayapy`. If `mayapy` cannot run under CMake (quarantine,
   wrapper script), set `GROOMLAB_PYTHON_ROOT` to a python.org Python of the same minor version
   (3.10 for 2024, 3.11 for 2025 and 2026, 3.13 for 2027).
7. **Maya's TBB.** Maya 2024 and 2025 ship TBB 2020 (`libtbb.dylib`), 2026 and 2027 oneTBB (`libtbb.12.dylib`);
   `GroomLabTBB.cmake` looks in `Maya.app/Contents/MacOS`. Check `otool -L GroomLab.bundle` names
   `@rpath/libtbb...` the way Maya's own plugins do (compare `otool -L` on one of Maya's
   `.bundle`s in `Maya.app/Contents/plug-ins`).
8. **MtoA on macOS.** The translator is a `SHARED` library named `groomLabTranslator.dylib`;
   check MtoA loads `.dylib` extensions from `MTOA_EXTENSIONS_PATH` and that `find_library(ai)`
   found `libai.dylib`.
9. **The app.** `QRhiMetalInitParams` in `offscreen.cpp` assumes Qt 6.8's public `rhi/qrhi.h`
   declares it on macOS; the app's `.qsb` shaders already carry MSL. Watch the QRhi backend in
   the window title or `GROOMLAB_APP_RHI=metal`.
10. **Blender helper.** POSIX shared memory works on macOS with two quirks: names are capped at
    31 bytes (handled) and an object's `st_size` is rounded up to a page (harmless here). The
    extension zip must keep the helper's executable bit; a downloaded zip is quarantined.
11. **The V-Ray for Maya translator** (`hosts/maya/vray/groom_translator.cpp`,
    `hosts/maya/CMakeLists.txt`, "V-Ray"): its build is written for Windows. The default
    `VRAY_FOR_MAYA_LOCATION` is set on `WIN32` only (pass it on the Mac), it links
    `vutils_s.lib`, `plugman_s.lib` and `vray.lib` (on the Mac look in V-Ray's `vray/lib` for their
    `.a`/`.dylib` names), defines `NOMINMAX` and the x86 `__SSE*__` macros (wrong on arm64: check
    what V-Ray's own macOS SDK samples define) and is named `vraytr_groomlab` with no prefix (check
    the file name and extension V-Ray for Maya loads translators by on macOS:
    `VRAY_FOR_MAYA<year>_TRANSLATORS_PATH`). Make those `if(APPLE)` branches; without them the
    translator is simply not built (the configure says why) and the rest builds. The plugin's
    V-Ray hook (`groomlab/vray_hook.py`, `vray_render.py`) is Python and should run as it is.
12. **Maya 2024's Qt 5.** The plugin's Qt bits (`groomlab_maya_qt`, Windows-only today) and the
    module's Python UI (`groomlab/ui/qt.py`: PySide2 or PySide6) already handle Qt 5 for Maya 2022
    and 2023 on Windows; on the Mac, 2024 is the only Qt 5 one.

Things that will be skipped, not fail: the GPU device tests (`groomlab_gpu_tests`: Windows only),
the `gpu.*kernel_targets` tests without `GROOMLAB_SLANGC`, the Maya tests (the `macos-maya*`
presets build no tests; section 12), RenderMan (not installed) and the Arnold/V-Ray tests without
their SDKs, `app.package_manifest` (Windows only).

## 5. What is guarded and why

- **GPU look preview** (`engine/gpu`): its backends are DirectX 11 and OpenGL 4.5 compute through
  WGL. macOS's OpenGL stops at 4.1 (no compute) and is deprecated, so the Mac gets the CPU path
  until a Metal backend exists. `look_engine_none.cpp` makes every factory throw; the hosts
  already catch that (a device the engine cannot use) and say why in
  `groomLabGpuPreview -stats`.
- **Maya** (`gpu_preview.cpp`, `gpu_lines.cpp`): `mayaDevice()` / `viewportDevice()` return no
  device off Windows, so drags, strokes, GPU frames and GPU ribbons are the CPU's; the
  `groomLabGpuPreview` command still answers (`-isEnabled`, `-stats`, `-drawApi`). Viewport 2.0
  drawing itself (`groom_draw_override*.cpp`: Maya's shader fragments) is API-neutral and should
  draw on Metal: untested.
- **Pen pressure** (`pen_pressure.cpp`): Maya's Qt headers are wired up on Windows only, so the
  pressure is 1 on macOS. The macOS devkit ships Qt too (`Qt/`): wire `groomlab_maya_qt` for
  `APPLE` in `hosts/maya/CMakeLists.txt` when needed.
- **Crash log** (`crash_log.cpp`): Windows only (dbghelp); a no-op elsewhere.
- **Symbols**: `groomlab_hide_static_symbols` is a no-op on macOS (two-level namespaces do not
  interpose like ELF); the C API exports `gl_*` only.

## 6. GPU on the Mac

- **Now**: CPU everywhere for compute. The app's viewport draws with QRhi on Metal (its shaders are
  `.qsb` with SPIR-V, HLSL, MSL and GLSL: `hosts/app/CMakeLists.txt`). Maya's viewport draws with
  Viewport 2.0's own device (Metal on macOS).
- **Metal compute backend** (the next big step once everything builds):
  1. Slang already compiles every kernel to Metal (`engine/gpu/cmake/check_kernel_targets.cmake`,
     `-target metal -DGROOMLAB_SLANG_LAYOUT -DGROOMLAB_SLANG_METAL`). Add a `metal` format to
     `embed_kernel.cmake` and embed the MSL text (compiled at runtime with
     `newLibraryWithSource`, or offline with `xcrun metal` into a `.metallib`).
  2. Write `engine/gpu/src/look_engine_metal.mm` mirroring `look_engine_d3d11.cpp`: the engine's
     few primitives (buffers by number, a dispatch, a copy, timestamps) on an `MTLDevice` and a
     command queue. Add `GpuApi::Metal` and `makeMetalLookEngine(void* device)` to `look_engine.h`;
     likewise a Metal `LineFrames` in `line_frames.cpp`'s pattern. Build it only `if(APPLE)`
     (`enable_language(OBJCXX)`, link `-framework Metal -framework Foundation`), and drop
     `look_engine_none.cpp`'s factories for the ones it replaces.
  3. Run `groomlab_gpu_tests` on macOS (enable them for `APPLE` in `tests/CMakeLists.txt`): they
     compare every kernel with the CPU operators.
  4. Hosts: the app gets the `MTLDevice` from `QRhiMetalNativeHandles` (`hosts/app/src/gpu_preview.cpp`,
     `supported()`); Maya from `MRenderer::GPUDeviceHandle()` when `drawAPI()` is Metal (check
     what Maya 2025/2026 return there) in `gpu_preview.cpp`'s `mayaDevice()`.
- `sse2neon` (MIT, header-only) could give the SSE2 paths NEON speed instead of the scalar
  fallbacks; measure first (`tools/bench`, the `Budget.*` tests), and check the determinism tests
  stay green.

## 7. Maya plugin specifics

- Plugin: `GroomLab.bundle` (a Mach-O bundle), installed into the module's `plug-ins/` (Maya 2022
  layout) or `maya<year>/plug-ins/` (`hosts/maya/CMakeLists.txt`). Maya on macOS is 2024 or later
  for Apple silicon; the presets cover 2024 to 2027 (2024's and 2027's paths copied from 2025's:
  check them against the installs and the devkits).
- Python: Maya 2024 is Python 3.10 with PySide2 (Qt 5), 2025 and 2026 are Python 3.11, 2027 is
  3.13; PySide6 (Qt 6) from 2025. The
  module's Python (`hosts/maya/scripts`) is shared with Windows; look for Windows-only bits there:
  `os.startfile` (use `open` on macOS: `scripts/groomlab/ui/menu.py`), `GroomLab.mll` in
  `scripts/groomlab/updates.py`, `Render` under `MAYA_LOCATION/bin` (`review.py`: on macOS it is
  `Maya.app/Contents/bin/Render`), `renderman.py`'s `.dll`/`.so` names (`.so` is right on macOS).
- Module file: `module/GroomLab.mod.in` writes `MAYAVERSION` sections only. A multi-platform
  module needs `PLATFORM:mac` (and `win64`, `linux`) in each line, joined by
  `tools/package_release.py` (Windows-only today, `-maya-win64.zip`).
- Arnold: `ARNOLD_PLUGIN_PATH+:=procedurals` holds `groomlab_procedural.dylib` (built by a preset
  with `GROOMLAB_BUILD_PROCEDURAL=ON` and an Arnold SDK; the `macos-maya*` presets leave it out).

## 8. The app

- Built as a plain executable (`build/macos-app/hosts/app/GroomLab`); it finds `presets/` and
  `nodes/` beside itself (`QCoreApplication::applicationDirPath()`: `graph_presets.cpp`), which the
  build copies there.
- A distributable `.app`: set `MACOSX_BUNDLE TRUE` on `groomlab_app` for `APPLE`, install with
  `BUNDLE DESTINATION`, move `presets/`, `nodes/`, `docs/` and `procedurals/` to
  `Contents/Resources` (and teach `graph_presets.cpp`, `main_window.cpp` and `prman_render.cpp`
  to look there on macOS), then `macdeployqt GroomLab.app` (Qt's `bin/macdeployqt`) and the TBB
  dylib into `Contents/Frameworks`. `tools/package_app.py` is Windows-only (windeployqt, DLL
  checks): give it a macOS branch rather than a second script.

## 9. The Blender add-on

- The extension zip is per platform (`hosts/blender/addon/CMakeLists.txt`:
  `GroomLab-Blender-<version>-macos-arm64.zip`, `blender_manifest.toml`'s `platforms`). It holds
  `bin/groomlab-helper` (+ `libtbb.12.dylib` beside it, `@loader_path`) and the add-on.
- Tests: `blender.helper` runs with Blender's Python
  (`/Applications/Blender.app/Contents/Resources/*/python/bin/python3*`, globbed already). The
  tests that start Blender itself need `-DGROOMLAB_BLENDER_EXE=/Applications/Blender.app/Contents/MacOS/Blender`.
- `tools/package_blender.py` handles the helper's name per platform already.

## 10. Unsigned builds, signing, notarization

- Local builds run unsigned (Apple silicon requires a signature, and the linker adds an ad-hoc one).
- Anything downloaded (a zip from the website, Slang) is quarantined: `xattr -dr
  com.apple.quarantine <folder>`. Maya refuses to load a quarantined `.bundle` silently.
- After editing a binary (`install_name_tool`), re-sign it ad hoc: `codesign --force -s - <file>`.
- Shipping to users needs a Developer ID certificate, `codesign --options runtime --timestamp`,
  `xcrun notarytool submit` and `xcrun stapler staple`: ask the user before anything that needs
  an Apple account.

## 11. Packaging and publishing (later, with the user)

`tools/package_release.py`, `package_app.py`, `publish_*.py` are Windows-only (`win64` zips,
`.mll`, windeployqt, DLL manifests). A macOS release needs: the Maya module zip with
`PLATFORM:mac` sections and `GroomLab.bundle` per Maya version, the app as a signed `.app` in a
zip or `.dmg`, and the Blender extension zip. Nothing is published without the user's go-ahead.

## 12. Test tiers on the Mac

The tiers changed on 2026-11-09 (`docs/DEVELOPING.md` section 2): every test is in exactly one, and
`quick` holds GoogleTest only. Each GoogleTest executable runs as a few ctest shards
(`core.shard0` to `core.shard11`, `app.shard0`...: `groomlab_gtest_shards` in
`tests/CMakeLists.txt`, GoogleTest's sharding), one process each. A failure prints its test's name;
one test alone is the executable with `--gtest_filter=Suite.Name`
(`build/macos-core/tests/groomlab_tests --gtest_filter=...`), not `ctest -R`.

- `ctest --preset macos-core -L quick -j 8`: the core's, the C API's and the bindings' GoogleTest
  (the timings, `Budget.*` and `Performance.*`, are `-L budget`, one at a time: they were tuned on
  Windows, so treat a miss as a number to look at, not a failure to fix blindly).
- `ctest --preset macos-core -L blender -LE vray -j 4`: the Blender helper and add-on.
- `ctest --preset macos-app -L app -j 8`: the app's tests (offscreen platform).
- `-L core`, `-L io`, `-L python`: one area.
- `-L maya` (the mayapy suites: `tests/hosts/maya/test_*.py`) needs a Maya preset that builds the
  tests: the `macos-maya*` presets set `GROOMLAB_BUILD_TESTS` off. Add one (as Windows'
  `ninja-maya2025-tests`) once the plugin loads, and run `ctest -L maya -LE "gui|arnold|vray|renderman"`;
  then `-L arnold` with MtoA. No `gui` tier on macOS yet (the Maya GUI suites drive Windows' Maya);
  no `renderman` (not installed).
- `python3 tools/dev/test_changed.py` picks the tests for what you changed (check it runs on macOS).

## 13. Checklist (update the boxes as you go)

- [ ] Xcode, Homebrew packages, Qt 6.8.3 installed
- [x] `macos-core` configures
- [x] `macos-core` builds (core, C API + samples, CLI, Python module, Blender helper, tests)
- [x] `ctest -L quick` green on `macos-core` (goldens and determinism included)
- [ ] `macos-app` builds; the app opens, draws a groom on Metal, previews on the CPU
- [ ] `ctest -L "quick|app"` green on `macos-app`
- [ ] Offscreen renders (thumbnails, turntables) correct on Metal
- [ ] Maya 2025 devkit + install in place; `macos-maya2025` builds `GroomLab.bundle`
- [ ] The plugin loads in Maya 2025; a groom draws in Viewport 2.0 (Metal); brushes and drags work (CPU)
- [ ] The Python module imports in mayapy; the GroomLab menu works (fix Windows-only Python bits)
- [ ] MtoA translator builds and renders a groom in Arnold
- [ ] A Maya preset with tests; `ctest -L maya` green on the Mac
- [ ] Maya 2026 (`macos-maya2026`)
- [ ] Maya 2027 (`macos-maya2027`: Python 3.13)
- [ ] Maya 2024 (`macos-maya2024`: Qt 5, PySide2, Python 3.10)
- [ ] V-Ray for Maya translator builds (`if(APPLE)` branches, section 4 item 11) and renders a groom
- [ ] Arnold procedural (`groomlab_procedural.dylib`) renders in kick
- [ ] RenderMan procedural renders in prman (when RenderMan is installed)
- [ ] Blender extension zip installs in Blender for macOS; the helper runs
- [ ] Slang for macOS pinned; `gpu.*kernel_targets` green
- [ ] Metal compute backend (`look_engine_metal.mm`), `groomlab_gpu_tests` green on macOS
- [ ] App and Maya use the Metal backend for drags and strokes
- [ ] Pen pressure in Maya on macOS (Maya's Qt)
- [ ] `.app` bundle with macdeployqt; packaging scripts' macOS branches
- [ ] Signing and notarization (with the user's Apple account)
