<!-- Pull requests go against `dev`. See CONTRIBUTING.md. -->

## What and why

<!-- What changes for the user (or the code), and why. Link the issue: "Closes #123". -->

## Hosts

- [ ] Engine (core)
- [ ] Maya
- [ ] Blender
- [ ] Standalone app

## Tested

<!-- The presets you built and the tests you ran, e.g. `ctest --preset win-app -L "quick|app"`.
     For UI changes, a screenshot helps. -->

- [ ] The feature's logic is in `engine/core` (hosts only call it: UI and viewport) and what it
      saves is in the groom's data (`.groom`), not a host's scene (CONTRIBUTING.md, "Making a change")
- [ ] `ctest -L quick` passed, the whole label (Python and golden tests included)
- [ ] New or changed behavior has a test
- [ ] Docs updated (`web/content/docs`, `docs/`) where users would look
- [ ] New source files start with an SPDX line
