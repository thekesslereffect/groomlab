# Setting up a new dev machine (Windows)

A checklist for getting a second Windows machine to the point where it builds, tests, installs and
releases GroomLab the way the main one does. The details behind each step are in
[DEVELOPING.md](DEVELOPING.md) ("Build, test, install", "Other Maya versions", "Linux builds") and
[BUILDING.md](../BUILDING.md).

## The one thing to decide first: which Maya runs the tests

The everyday preset, `ninja-maya2022`, builds against the **installed Maya 2022** and runs every
test tier with it (the mayapy suites, the GUI session, Arnold). The `ninja-maya2023` to
`ninja-maya2027` presets build from Autodesk's devkits with **no tests** (they ship as build-checked
betas). So on a machine with only Maya 2026 or 2027:

- **Simplest:** install Maya 2022 too (and its MtoA). Everything below then works as it does on the
  main machine, and the newer Maya is for trying the plugin by hand.
- **Moving dev to a newer Maya:** the `ninja-maya2026-tests` and `ninja-maya2027-tests` presets
  build the plugin from that version's devkit with every test on, run in the installed Maya and its
  MtoA (`C:/Program Files/Autodesk/Maya2026` or `Maya2027`, `Arnold/maya2026` or `maya2027`). The
  tests already take Qt through `groomlab.ui.qt`, so they can run under PySide6; what is left is
  getting the mayapy and GUI suites to pass there (DEVELOPING.md, "Testing in a newer Maya"). Keep
  releasing from a Maya 2022 machine until they do.

## 1. Accounts and sign-ins

- [ ] **GitHub:** Git for Windows (Git Bash), then the GitHub CLI (`winget install GitHub.cli`) and
      `gh auth login`.
- [ ] **git identity:** `git config --global user.name` / `user.email`, the same as on the main
      machine.
- [ ] **Claude Code** (the desktop app), signed in.
- [ ] **Autodesk** (Maya, Arnold) and, if you use them, Chaos (V-Ray) and Pixar (RenderMan).
- [ ] **Releases only:** the R2 upload token as *User* environment variables: `R2_ACCOUNT_ID`,
      `R2_ACCESS_KEY_ID`, `R2_SECRET_ACCESS_KEY` (optionally `R2_BUCKET`). Copy them from the main
      machine's environment variables; never put them in a file in the repository.

## 2. Tools

- [ ] **Visual Studio 2022** (Community is fine) with the **Desktop development with C++** workload
      (MSVC v143, a Windows 10/11 SDK). It brings CMake (3.25+) and Ninja; `tools/dev/vsenv.ps1`
      finds them, so no separate install.
- [ ] **Python 3** (3.12 or later) on PATH, for the tools and the commit guard.
- [ ] **Node.js 24** for the website (`web/`): `cd web && npm ci`.
- [ ] **WSL 2 with Ubuntu 22.04**, for the Linux build (`tools/dev/linux_build.ps1`; the first run
      with `-Bootstrap` if you have no sudo there).
- [ ] Optional: **sccache** for faster rebuilds: `tools/dev/get_sccache.ps1`.

## 3. Maya, renderers and Blender

- [ ] **Maya 2022** (see above) at `C:/Program Files/Autodesk/Maya2022`, with **MtoA** (Arnold) at
      `C:/Program Files/Autodesk/Arnold/maya2022`: the paths `CMakePresets.json` names. Other paths
      work with `-DMAYA_LOCATION=... -DMTOA_LOCATION=...`.
- [ ] The newer Maya you want (2026, 2027) and its MtoA, for trying the plugin by hand.
- [ ] **Blender** 4.2 or later under `C:/Program Files/Blender Foundation/` (the add-on tests use the
      newest there; `GROOMLAB_BLENDER_EXE` picks another).
- [ ] Optional: **V-Ray** (the `vray` tests) and **RenderMan Pro Server** (the `renderman` tier).

## 4. The deps folder (outside the repository)

Everything the build needs that is not in the repository lives in `%USERPROFILE%\groomlab-deps`.
**The fastest way is to copy that whole folder from the main machine** (several GB). What is in it,
and how to rebuild each piece instead:

| Folder | What | Rebuild it |
| --- | --- | --- |
| `sdks/maya-devkit-2023` ... `2027` | Autodesk's Maya devkits, for the 2023-2027 plugins | DEVELOPING.md, "Other Maya versions" (download links) |
| `sdks/python-3.x.y` | CPython for each devkit's module | the python.org NuGet packages, same section |
| `sdks/mtoa-*` | MtoA SDKs (headers, libraries) for the 2023-2027 translators | extracted from each MtoA installer |
| `Qt/6.8.3/msvc2022_64` | Qt for the standalone app (`ninja-app`) | the Qt online installer or `aqtinstall`, Qt 6.8.3 MSVC 2022 64-bit |
| `fetch` | pybind11, GoogleTest, json, Imath, Alembic, Slang sources | fetched by the first configure |
| `kernels` | the compiled GPU kernel cache | rebuilt by the first build (slow once) |
| `venvs` | PySide venvs for `qt_compat_check.py` (one per Python) | DEVELOPING.md, "Other Maya versions" |
| `emsdk` | Emscripten, for the website's WebAssembly engine | [WEB.md](WEB.md) |
| `private` | the commit guard and its hook installer (not public) | copy it: it cannot be rebuilt from the repository |
| `assets` | test and showcase assets (the scan head) | copy it |

## 5. The repository

- [ ] Clone into `%USERPROFILE%\Documents\GitHub\groomlab` (the same path as the main machine keeps
      Claude Code's project memory and the scripts' defaults matching) and check out `dev`:
      `gh repo clone thekesslereffect/groomlab` then `git checkout dev`.
- [ ] Install the commit guard: `python "$USERPROFILE/groomlab-deps/private/install_hooks.py"` (Git Bash). It
      blocks private terms, user paths and secrets in commits and pushes; never skip it with
      `--no-verify`.
- [ ] Never commit `tests/maya_app_dir/` or `research/ui-redesign/.vite/` (local test prefs and a
      build cache).

## 6. First build, tests, install

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

## 7. Releasing from this machine (optional)

Only once this machine has everything above, including every preset:

- [ ] Configure and build `ninja-maya2022` to `ninja-maya2027` and `ninja-app` (the release zip holds
      all six Maya versions).
- [ ] The Linux build (`tools/dev/linux_build.ps1 -Tests all`).
- [ ] The R2 variables from step 1, then the publish scripts (`tools/publish_release.py`,
      `publish_app.py`, `publish_blender.py`) as in DEVELOPING.md.

## 8. Claude Code

- [ ] Copy Claude Code's project memory for this repository from the main machine:
      `%USERPROFILE%\.claude\projects\<the repository path, dashed>\memory\`. It holds the working
      notes (release steps, test tiers, conventions). With the repository at the same path, the
      folder name is the same.
- [ ] Optional: `%USERPROFILE%\.claude\settings.json` (plugins, permissions) the same way.
