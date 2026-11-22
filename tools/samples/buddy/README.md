# Buddy sample pipeline

Regenerates the shipped Buddy files in `engine/assets/meshes/` (CC BY 4.0, "Buddy by Nick Rutlinh
(GroomLab), CC BY 4.0") from the artist's Maya 2022 scene of the rig. One command per rig update:

```
python tools/samples/buddy/make_buddy.py <path to the artist's buddy.ma>
```

It runs Maya 2022's mayapy (`MAYAPY` overrides `C:\Program Files\Autodesk\Maya2022\bin\mayapy.exe`)
with a throwaway `MAYA_APP_DIR` under `build/`. mayapy crashes in Arnold's shutdown once a script
is done (the scene loads mtoa through its requires); the files are written by then, and
make_buddy.py checks each one was.

| Script | Writes | What |
| --- | --- | --- |
| `clean_rig.py <source> <out>` | `buddy_rig.ma` | The rig for Maya: `buddy_rig` exported with its history, so no cameras or default nodes; no requires but Maya's, no time unit, no `select -ne :shared` blocks (time, render and color management settings a user's scene would get); the SPDX lines and the credit as comments after the first line. |
| `export_fbx.py <source> <out>` | `buddy_rig.fbx` | The rig for Blender: the joints and the five skinned meshes at the bind pose, no controls or constraints (binary FBX 7.5, cm, y up); the absolute paths the exporter embeds rewritten to bare file names (`fbx_paths.py`). |
| `export_obj.py <source> <out>` | `buddy.obj` | The body alone at the bind pose for the app and the MCP server (Maya's OBJ exporter, no groups or materials), with the SPDX lines and the credit. |

All three start from `buddy_scene.open_clean`: the scene without the unskinned reference mesh
`ref`, the Arnold nodes (and so mtoa's requires) and the script nodes, with `geometry_GRP` not
inheriting its parents' transforms (the joints alone carry the skinned meshes, so scaling or
turning `buddy_rig` doesn't move them twice). It checks the five meshes are there and that the
controls at rest hold every joint at its bind pose with each skin matching its Orig shape (what a
groom's rest mesh is).

After an update: diff `buddy_rig.ma` against the previous one (only the artist's changes should
show, plus fresh UUIDs for `layerManager` and `lightLinker1`), and recheck what depends on the rig:
the extent `add_sample` sizes it by (`SAMPLES` in `hosts/maya/scripts/groomlab/groom.py`, `96.65` in
`hosts/blender/helper/src/pipeline.cpp`: the meshes' largest extent in cm), the joint and control
counts and control names in `tests/hosts/maya/test_grooming.py` and
`tests/hosts/blender/test_addon_pipeline.py`, and the height in `engine/cli/mcp_tools.cpp`.
