# Third-party notices (source tree)

GroomLab is licensed under the [Apache License 2.0](LICENSE). The Blender add-on in
`hosts/blender/addon/` is licensed under the GPL-3.0-or-later ([its LICENSE](hosts/blender/addon/LICENSE)).

This page lists what the source tree contains or pulls in from others, how each piece is used,
and what its license asks of you. The full license texts for everything that ends up inside
GroomLab's binaries are in [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt), which every
package installs beside the binaries.

## Vendored in this repository

| Component | Where | License | Notes |
| --- | --- | --- | --- |
| stb_image 2.30 (Sean Barrett) | `third_party/stb/stb_image.h` (commit in `COMMIT`) | MIT or public domain (Unlicense), at your choice | Texture maps. Keep the license block at the end of the header. |
| Figtree (font) | `hosts/app/fonts/Figtree[wght].ttf` | SIL OFL 1.1 (`OFL-Figtree.txt`) | The app's UI font. The OFL text must travel with the font; the font may not be sold on its own. |
| JetBrains Mono (font) | `hosts/app/fonts/JetBrainsMono[wght].ttf` | SIL OFL 1.1 (`OFL-JetBrainsMono.txt`) | The app's monospace font. Same terms. |
| License texts | `hosts/app/package/licenses/`, `hosts/app/package/spdx/` | the texts themselves | Copied into the app package for Qt and its third-party code. |

## Fetched at configure time (not in the repository)

CMake's FetchContent downloads these, pinned by tag or SHA-256 (`cmake/GroomLabDependencies.cmake`,
`engine/gpu/CMakeLists.txt`).

| Component | Version | License | Used for | Ships in binaries? |
| --- | --- | --- | --- | --- |
| Alembic | 1.8.8 | BSD-3-Clause | Alembic export and import (static) | yes: notice in THIRD_PARTY_NOTICES.txt |
| Imath | 3.1.12 | BSD-3-Clause | Alembic's math types (static) | yes: notice in THIRD_PARTY_NOTICES.txt |
| nlohmann/json | 3.11.3 | MIT | Graph and groom JSON (header-only) | yes: notice in THIRD_PARTY_NOTICES.txt |
| pybind11 | 2.13.6 | BSD-3-Clause | The `groomlab_core` Python module | yes: notice in THIRD_PARTY_NOTICES.txt |
| GoogleTest | 1.15.2 | BSD-3-Clause | Unit tests only | no |
| Slang (`slangc`) | 2026.19 | Apache-2.0 WITH LLVM-exception | Build tool: compiles the look kernels to DXBC/GLSL and C++ | no (only its output) |
| Slang C++ prelude (`third_party/slang`) | 2026.19 | Apache-2.0 WITH LLVM-exception | The headers the kernels' C++ (the CPU engine) compiles against | yes (compiled in): notice in THIRD_PARTY_NOTICES.txt |

## Found on your machine (never in the repository)

| Component | License | How the build finds it | Notes |
| --- | --- | --- | --- |
| Autodesk Maya (install or devkit) | Autodesk proprietary (devkit license) | `MAYA_LOCATION` | Headers and libraries are **not redistributable**; never commit them. See [BUILDING.md](BUILDING.md). |
| Arnold SDK / MtoA | Autodesk proprietary | `MTOA_LOCATION` | Same: use your own install. |
| RenderMan Pro Server | Pixar proprietary | `GROOMLAB_RMANTREE` (or `RMANTREE`) | Same: use your own install (the free non-commercial edition works). |
| Intel TBB 2020 / oneTBB | Apache-2.0 | From the Maya install (or a system package) | Packages ship Maya's own `tbb.dll`/`tbb12.dll` beside the plugin when needed; its notice is in THIRD_PARTY_NOTICES.txt. |
| Qt 6.8 | LGPL-3.0 (GPL-3.0 for some tools) | `GROOMLAB_QT_DIR` | The standalone app only, **linked dynamically**. When you distribute the app: ship Qt's license texts, say where Qt's source is (or offer it), let users swap in their own Qt build (dynamic linking does this), and don't forbid the reverse engineering needed to debug that. `hosts/app/package/licenses/QT_NOTICE.txt` covers this for the official package. |
| Python, PySide2/PySide6 | PSF / LGPL-3.0 | The host's own | Used from Maya; nothing is shipped. |
| Blender | GPL-2.0-or-later (Blender as a whole: GPL-3.0) | The host | The add-on imports `bpy`, so the add-on is GPL-3.0-or-later. `groomlab-helper` is a separate program (Apache-2.0) that the add-on starts and talks to over pipes and shared memory. Apache-2.0 code may also be combined into GPLv3 works. |
| Microsoft Visual C++ runtime | Microsoft redistributable terms | Visual Studio | The app package ships the runtime DLLs app-local. |
| Unreal Engine, Unity | Epic / Unity terms | not used by the build | GroomLab only writes files those engines import. Nothing of either is in the repository. |

## The website (`web/`)

The site's npm dependencies (Next.js, React, Fumadocs, Tailwind CSS, lucide-react, TypeScript,
ESLint and their dependencies) are under permissive licenses (mainly MIT, ISC, Apache-2.0 and
BSD). `web/package-lock.json` lists the exact versions. Run `npx license-checker --summary` in
`web/` to list every license. The Google Sans Flex font comes from Google Fonts through
`next/font` (SIL OFL 1.1); no font file is in the repository. The press kit's wordmark
(`web/public/press/`) is that font's letters turned into outlines.

The groom viewer's engine (`web/public/wasm/groomlab.js` and `groomlab.wasm`, built from
`engine/web` with Emscripten: [docs/WEB.md](docs/WEB.md)) is GroomLab's core with nlohmann/json
(MIT) and stb_image (MIT) compiled in, and Emscripten's runtime: its JavaScript glue and musl libc
(MIT) and libc++/libc++abi (Apache-2.0 WITH LLVM-exception). The sample groom beside it
(`web/public/samples/manny_hair.groom`) holds the Manny head, CC BY 4.0.

The renderer logos in `web/public/renderers/` (Arnold, RenderMan) are their owners' trademarks.
They are there only to say that GroomLab works with those renderers. They are **not** covered by
GroomLab's license. Use them only within Autodesk's and Pixar's brand guidelines.

## Assets and test data

- `tests/data/app/head_scalp.obj` is generated by `tests/data/app/make_head_scalp.py` (an
  ellipsoid). Apache-2.0 like the rest.
- The scanned head used by `tests/data/app/scan_head/*.py` is **not** in the repository. It is
  Lee Perry-Smith's head scan (Infinite Realities, CC BY 3.0, via three.js), downloaded into
  `%USERPROFILE%/groomlab-deps/assets`. Anything published that is rendered from it must credit
  "Lee Perry-Smith / Infinite Realities, CC BY 3.0".
- Icons (`hosts/*/icons`, `hosts/blender/addon/groomlab/icons`) are generated by
  `tools/make_icons.py` and `tools/make_blender_icons.py`. Apache-2.0.
- Renders, contact sheets and benchmark results under `design/`, `tools/bench/` and
  `tools/research/` are GroomLab's own output. Apache-2.0 unless the file says otherwise.
- The wolf model and its groom are by Nick Rutlinh. They will be published as a free sample groom
  under CC BY 4.0 ([samples/wolf/README.md](samples/wolf/README.md)). The model files are not in
  the repository yet. Credit: "Wolf model and groom by Nick Rutlinh, CC BY 4.0".
- The website's hero image (`web/public/landing.webp` in the private repository) is a
  placeholder of unknown origin. It is **not** part of the public repository or covered by any
  license here. It will be replaced by a render of the wolf sample.
