# Project working rules

- The assistant owns all configure, build, install, and test commands. Never ask the user to run a build or installation command.
- Reuse the existing build caches under `E:\GroomLab_build` whenever possible; rebuild only the targets affected by the current change.
- For Maya changes, build the appropriate Maya target before handoff, verify the produced binaries, and give the user only the Maya-side reload/hot-swap or testing code they need.
- Maya hot-swap code must refresh the complete development stack: close GroomLab UI instances, unload the plug-in, load the rebuilt MLL, replace the loaded `groomlab_core` extension when its API changed, clear stale `groomlab` Python modules or catalogs, and reopen the interface when practical. Use a uniquely named MLL copy for each native rebuild so Windows cannot reuse a still-mapped DLL image with the same module name. A plug-in-only reload is not a complete hot-swap when operator specs or UI behavior changed.
- Preserve unrelated work and existing build outputs when switching issues or branches.
- When working on a GitHub issue, rename the chat to `Issue #<number> - <short issue title>` as soon as the issue number and scope are known. Do this automatically without waiting for the user to request it.
