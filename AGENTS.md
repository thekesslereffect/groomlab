# Notes for AI coding assistants

Read this before changing GroomLab. The full rules are in [CONTRIBUTING.md](CONTRIBUTING.md) and
[docs/DEVELOPING.md](docs/DEVELOPING.md) section 6.

## One engine, three hosts

GroomLab runs in **Maya** (`hosts/maya`), **Blender** (`hosts/blender`: a Python add-on and a C++
helper) and the **standalone app** (`hosts/app`, Qt), all on one C++ engine (`engine/core`). Most
tools, menus and panels exist in all three. A change to one host's tool is not finished until you
have checked the other two.

- **Logic goes in `engine/core`, in C++, with core tests.** A host holds only its UI and viewport
  plumbing (events in, drawing out) and calls the core. Never put a feature's math or rules in
  Maya's Python, the Blender add-on or the app's widgets: the other hosts cannot use them there.
- **Saved data goes in the groom's own data** (the graph, the guide hierarchy, scalp maps, the
  `.groom` file), never only in a host's scene (a Maya attribute, a Blender property).
- **UI shared by the hosts is defined once** in `engine/ui/*.json`
  ([docs/UI_DEFINITIONS.md](docs/UI_DEFINITIONS.md)); bind new action ids in every host.
- **Say which hosts a change covers.** If you only did one host's UI, say so plainly in the pull
  request ("Maya UI only; Blender and the app still need their UI for it").

## Before calling it done

- Build and run the tests ([BUILDING.md](BUILDING.md)): `ctest --preset <preset> -L quick`, the
  whole label (it includes the Python bindings' tests and the golden files, not only
  `groomlab_tests`), plus `-L maya`, `-L blender` and `-L app` for the hosts you touched.
- Golden files and fixtures change only for an intended change, re-recorded with the script that
  made them.
- No format migrations (stored formats change in place), scale invariance (no fixed scene-unit
  lengths), a param's default only in its spec: DEVELOPING.md section 6.
- Name tools, files and UI text for what they do, not after another product.
