# Shipped assets

The files every GroomLab product ships, kept here once: the Maya module, the Blender extension,
the standalone app, the CLI and the core's tests all read them from this folder (each installs
them into its own layout: `<module>/presets`, the app's data folder, the extension's `bin/`).

| Folder | What it holds |
| --- | --- |
| `presets/` | Graph presets (`*.json`), read through the core's `presets.h`. |
| `nodes/` | Compound nodes shipped with every host (`*.glnode`). |
| `meshes/` | Sample meshes (CC BY 4.0, see `REUSE.toml`). |
| `alphas/` | Brush alphas (256 x 256 gray PNGs, `tools/dev/make_alphas.py`). |
| `themes/` | UI themes (`*.json`): every host's colors and sizes. |
| `expressions/` | The expression library's examples. |
