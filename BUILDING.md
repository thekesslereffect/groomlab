# Building GroomLab from source

GroomLab builds with CMake 3.25+ and Visual Studio 2022 on Windows, or GCC 9+ on Linux. The
engine (core, Python module, CLI, C API, Blender helper) needs nothing proprietary. The Maya
plugin, the Arnold parts and the RenderMan procedural build against **your own** installs of
those products. Their SDKs are not redistributable, so they are not in this repository and must
never be committed.

[docs/DEVELOPING.md](docs/DEVELOPING.md) is the full handbook: every preset, the test tiers,
faster builds, debugging Maya, releasing. Setting up a whole development machine (every tool,
SDK and sign-in): section 7 below. macOS (Apple silicon) is being ported: [docs/MACOS.md](docs/MACOS.md).

## 1. The engine only (no proprietary SDK)

```bash
cmake --preset win-core      # or linux-core
cmake --build --preset win-core
ctest --preset win-core
```

This builds the core library, the `groomlab_core` Python module, `groomlab-cli`, the C API and
`groomlab-helper`. The first configure needs network access: FetchContent pulls pybind11,
GoogleTest, nlohmann/json, Imath, Alembic and (on Windows) the Slang compiler. Each is pinned by
tag or hash ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). On Linux, install TBB
(`libtbb-dev`) for parallel loops. Without it the loops run serially.

## 2. The Maya plugin, with your Maya

| What you have | Point the build at it |
| --- | --- |
| Maya 2022 installed | `MAYA_LOCATION` (default in the presets: `C:/Program Files/Autodesk/Maya2022`) |
| Maya 2023-2027 devkit (free from the [Autodesk Platform Services](https://aps.autodesk.com/developer/overview/maya) site) | unzip it anywhere and set `MAYA_LOCATION` to it |

```bash
cmake --preset win-maya2022                                   # installed Maya 2022
cmake --preset ninja-maya2025 -DMAYA_LOCATION=C:/sdks/maya-devkit-2025   # a devkit (example path)
```

The `ninja-maya2023` to `ninja-maya2027` presets expect each devkit under
`%USERPROFILE%/groomlab-deps/sdks/maya-devkit-<year>`, and the matching Python under
`%USERPROFILE%/groomlab-deps/sdks/python-<version>/tools` (`GROOMLAB_PYTHON_ROOT`). Override any
of these on the command line or in a `CMakeUserPresets.json`, which git ignores. TBB comes from
the same Maya or devkit.

## 3. Arnold, with your MtoA

Set `MTOA_LOCATION` to an MtoA install (it ships the Arnold and MtoA SDK headers and libraries),
for example `C:/Program Files/Autodesk/Arnold/maya2022`. With it, the build adds the MtoA
translator (`GROOMLAB_BUILD_ARNOLD`) and the `groomlab_procedural` Arnold plugin
(`GROOMLAB_BUILD_PROCEDURAL`). Without it, both are skipped. Rendering tests use `kick` from the
same install. Without an Arnold license, renders carry Arnold's watermark, which is fine for the
tests.

## 4. RenderMan, with your RenderMan Pro Server

Set `GROOMLAB_RMANTREE` (or the `RMANTREE` environment variable) to a RenderMan Pro Server
install. The non-commercial edition works. The build adds the `groomlab_renderman` procedural
(`GROOMLAB_BUILD_RENDERMAN`). Without it, the procedural is skipped.

## 5. The standalone app, with your Qt

`-DGROOMLAB_BUILD_APP=ON -DGROOMLAB_QT_DIR=<Qt 6.8 kit>` (for example
`.../Qt/6.8.3/msvc2022_64`, from the Qt online installer under the open-source LGPL terms), or
use the `ninja-app` preset. The app links Qt dynamically. If you distribute it, follow the LGPL
obligations in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## 6. The Blender add-on

The add-on (`hosts/blender/addon`, GPL-3.0-or-later) is pure Python. It starts
`groomlab-helper`, which section 1 builds. `python tools/package_blender.py` assembles the
extension zip. See [docs/BLENDER.md](docs/BLENDER.md).

## Keeping SDKs out of the repository

- Keep SDKs and devkits outside the source tree (the presets use `%USERPROFILE%/groomlab-deps`).
  Put machine-specific paths in `CMakeUserPresets.json` or environment variables, never in
  committed files.
- Never commit anything from a Maya devkit, MtoA/Arnold, RenderMan, Unreal or Unity
  (headers, `.lib`/`.dll`/`.so`, sample scenes), and no assets you don't have the right to
  redistribute (scans, purchased models, MetaHuman/Megascans content).

## 7. A full development machine (Windows)

A checklist for getting a Windows machine to the point where it builds, tests, installs and
releases GroomLab the way the maintainer's does (sections 1 to 6 above are the minimum to build
one part; this is everything). The details behind each step are in
[docs/DEVELOPING.md](docs/DEVELOPING.md) ("Build, test, install", "Other Maya versions", "Linux
builds", "Releasing").

### The one thing to decide first: which Maya runs the tests

The everyday preset, `ninja-maya2022`, builds against the **installed Maya 2022** and runs every
test tier with it (the mayapy suites, the GUI session, Arnold). The `ninja-maya2023` to
`ninja-maya2027` presets build from Autodesk's devkits with **no tests** (they ship as build-checked
betas). So on a machine with only Maya 2024 to 2027:

- **Simplest:** install Maya 2022 too (and its MtoA). Everything below then works as it does on the
  main machine, and the newer Maya is for trying the plugin by hand.
- **Moving dev to a newer Maya:** the `ninja-maya2024-tests` to `ninja-maya2027-tests` presets
  build the plugin from that version's devkit with every test on, run in the installed Maya and its
  MtoA (`C:/Program Files/Autodesk/Maya<year>`, `Arnold/maya<year>`): the devkit and the CPython
  of step 4, no MtoA SDK. The
  tests already take Qt through `groomlab.ui.qt`, so they can run under PySide6; what is left is
  getting the mayapy and GUI suites to pass there (docs/DEVELOPING.md, "Testing in a newer Maya"). Keep
  releasing from a Maya 2022 machine until they do.

### 1. Accounts and sign-ins

- [ ] **GitHub:** Git for Windows (Git Bash), then the GitHub CLI (`winget install GitHub.cli`) and
      `gh auth login`.
- [ ] **git identity:** `git config --global user.name` and `user.email`, as on the main machine;
      in the GroomLab clone, `git config user.email` set to your GitHub no-reply address
      (`<id>+<login>@users.noreply.github.com`) so the public history carries no personal email.
- [ ] **Claude Code** (the desktop app), signed in.
- [ ] **Autodesk** (Maya, Arnold) and, if you use them, Chaos (V-Ray) and Pixar (RenderMan).
- [ ] **Releases only:** the R2 upload token as *User* environment variables: `R2_ACCOUNT_ID`,
      `R2_ACCESS_KEY_ID`, `R2_SECRET_ACCESS_KEY` (optionally `R2_BUCKET`). Copy them from the main
      machine's environment variables; never put them in a file in the repository.

### 2. Tools

- [ ] **Visual Studio 2022** (Community is fine) with the **Desktop development with C++** workload
      (MSVC v143, a Windows 10/11 SDK) and **C++ Clang tools for Windows** (the build compiles the
      CPU kernels with its clang-cl, else they fall back to MSVC and run slower). It brings CMake
      (3.25+) and Ninja; `tools/dev/vsenv.ps1` finds them, so no separate install. It looks for
      Visual Studio 2022 only: next to a newer Visual Studio, add the 2022 Build Tools:
      `winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.26100 --add Microsoft.VisualStudio.Component.VC.CMake.Project --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset"`.
- [ ] **Python 3** (3.12 or later) on PATH, for the tools and the commit guard.
- [ ] **Node.js 24** for the website (`web/`): `cd web && npm ci`.
- [ ] **WSL 2 with Ubuntu 22.04**, for the Linux build (`tools/dev/linux_build.ps1`; the first run
      with `-Bootstrap` if you have no sudo there).
- [ ] Optional: **sccache** for faster rebuilds: `tools/dev/get_sccache.ps1`.

### 3. Maya, renderers and Blender

- [ ] **Maya 2022** (see above) at `C:/Program Files/Autodesk/Maya2022`, with **MtoA** (Arnold) at
      `C:/Program Files/Autodesk/Arnold/maya2022`: the paths `CMakePresets.json` names. Other paths
      work with `-DMAYA_LOCATION=... -DMTOA_LOCATION=...`.
- [ ] The newer Maya you want (2026, 2027) and its MtoA, for trying the plugin by hand.
- [ ] **Blender** 4.2 or later under `C:/Program Files/Blender Foundation/` (the add-on tests use the
      newest there; `GROOMLAB_BLENDER_EXE` picks another).
- [ ] **V-Ray for Maya** (V-Ray 7) for every Maya version you build, and V-Ray Standalone: the V-Ray
      for Maya install holds the SDK GroomLab's V-Ray translator builds against (without it that
      Maya's build leaves the translator out, and the release zip has none for it); Standalone runs
      the `vray` tests. Its licensing needs the Chaos login and VRLService (the installer's), no
      license server of your own.
- [ ] Optional: **RenderMan Pro Server** and **RenderMan for Maya** (the `renderman` tier; RfM
      renders grooms through GroomLab's procedural).

### 4. The deps folder (outside the repository)

Everything the build needs that is not in the repository lives in `%USERPROFILE%\groomlab-deps`.
**The fastest way is to copy that whole folder from the main machine** (several GB). What is in it,
and how to rebuild each piece instead:

| Folder | What | Rebuild it |
| --- | --- | --- |
| `sdks/maya-devkit-2023` ... `2027` | Autodesk's Maya devkits, for the 2023-2027 plugins | docs/DEVELOPING.md, "Other Maya versions" (download links) |
| `sdks/python-3.x.y` | CPython for each devkit's module | the python.org NuGet packages, same section |
| `sdks/mtoa-*` | MtoA SDKs (headers, libraries) for the 2023-2027 translators | extracted from each MtoA installer |
| `Qt/6.8.3/msvc2022_64` | Qt for the standalone app (`ninja-app`) | the Qt online installer or `aqtinstall`, Qt 6.8.3 MSVC 2022 64-bit |
| `cmake` | only beside VS 2022 Build Tools, whose bundled CMake and Ninja are 32-bit: a 64-bit CMake and `ninja.exe` in `bin/`, which `tools/dev/vsenv.ps1` puts first | CMake's `cmake-3.31.6-windows-x86_64.zip` unzipped, plus Ninja's `ninja-win.zip` |
| `fetch` | pybind11, GoogleTest, json, Imath, Alembic, Slang sources | fetched by the first configure |
| `kernels` | the compiled GPU kernel cache | rebuilt by the first build (slow once) |
| `venvs` | PySide venvs for `qt_compat_check.py` (one per Python) | docs/DEVELOPING.md, "Other Maya versions" |
| `emsdk` | Emscripten, for the website's WebAssembly engine | [docs/WEB.md](docs/WEB.md) |
| `private` | the commit guard and its hook installer (not public) | copy it: it cannot be rebuilt from the repository |
| `public-snapshot-terms.txt` | the guard's list of private terms (beside `private`, not in it): without it the guard refuses every commit | copy it |
| `assets` | test and showcase assets (the scan head) | copy it |

### 5. The repository

- [ ] Clone into `%USERPROFILE%\Documents\GitHub\groomlab` (the same path as the main machine keeps
      Claude Code's project memory and the scripts' defaults matching) and check out `dev`:
      `gh repo clone thekesslereffect/groomlab` then `git checkout dev`.
- [ ] Install the commit guard: `python "$USERPROFILE/groomlab-deps/private/install_hooks.py"` (Git Bash). It
      blocks private terms, user paths and secrets in commits and pushes; never skip it with
      `--no-verify`.
- [ ] Never commit `tests/maya_app_dir/` or `research/ui-redesign/.vite/` (local test prefs and a
      build cache).

### 6. First build, tests, install

From the repository root, in Git Bash or PowerShell:

```bash
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev/vsenv.ps1 cmake --preset ninja-maya2022
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev/vsenv.ps1 cmake --build --preset ninja-maya2022
```

```bash
ctest --preset ninja-maya2022 -L quick
```

```bash
cmake --install build/ninja-maya2022 --config RelWithDebInfo --component GroomLab --prefix "$USERPROFILE/Documents/maya/modules"
```

- [ ] The first configure fetches the third-party sources and the first build compiles every GPU
      kernel: expect it to take a while. A build folder grows to 30+ GB with the tests; keep space.
- [ ] Trust the plugin folder for the GUI test session once (CMake does it in `maya.gui_trust`), and
      run the `gui` tier (`ctest --preset ninja-maya2022 -L gui`) to check the viewport on this GPU.
      The `maya.gui_dx11` test runs it on DirectX 11.
- [ ] The app: `cmake --preset ninja-app`, then `cmake --build --preset ninja-app` (needs the Qt
      above), and `ctest --preset ninja-app -L quick`.
- [ ] Install into Maya only while Maya is closed (the `.mll` is locked while it runs).

### 7. Releasing from this machine (optional)

Only once this machine has everything above, including every preset:

- [ ] Configure and build `ninja-maya2022` to `ninja-maya2027` and `ninja-app` (the release zip holds
      all six Maya versions).
- [ ] The Linux build (`tools/dev/linux_build.ps1 -Tests all`).
- [ ] The R2 variables from step 1, then the publish scripts (`tools/publish_release.py`,
      `publish_app.py`, `publish_blender.py`) as in docs/DEVELOPING.md, "Releasing".

### 8. Claude Code

- [ ] Copy Claude Code's project memory for this repository from the main machine:
      `%USERPROFILE%\.claude\projects\<the repository path, dashed>\memory\`. It holds the working
      notes (release steps, test tiers, conventions). With the repository at the same path, the
      folder name is the same.
- [ ] Optional: `%USERPROFILE%\.claude\settings.json` (plugins, permissions) the same way.
