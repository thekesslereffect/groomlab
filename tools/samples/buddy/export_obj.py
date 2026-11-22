# SPDX-License-Identifier: Apache-2.0
"""Write engine/assets/meshes/buddy.obj (for the app and the MCP server) from the artist's Buddy scene
(run with mayapy 2022).

Usage: mayapy export_obj.py <source buddy.ma> <out buddy.obj>

The body alone at the bind pose (buddy_scene.open_clean checks the controls hold it), written by
Maya's OBJ exporter without groups or materials (normals, smoothing), with the SPDX lines and the
credit in place of the exporter's comment.
"""

import os
import sys

SOURCE, OUT = (os.path.abspath(a) for a in sys.argv[1:3])
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

HEADER = """# SPDX-FileCopyrightText: 2026 Nick Rutlinh
# SPDX-License-Identifier: CC-BY-4.0
# Buddy: a cartoon character's body to start a groom on (centimetres, y up, facing +z; the Maya and
# Blender samples ship it rigged: buddy_rig.ma, buddy_rig.fbx).
# Credit: "Buddy by Nick Rutlinh (GroomLab), CC BY 4.0".
# centimetres
"""

import maya.standalone  # noqa: E402

maya.standalone.initialize(name="python")

from maya import cmds  # noqa: E402

import buddy_scene  # noqa: E402

cmds.loadPlugin("objExport", quiet=True)
buddy_scene.open_clean(SOURCE)
cmds.select("buddy_body", replace=True)
cmds.file(OUT, exportSelected=True, type="OBJexport", force=True, constructionHistory=False,
          options="groups=0;ptgroups=0;materials=0;smoothing=1;normals=1")

lines = open(OUT, encoding="ascii").read().split("\n")
if not lines[0].startswith("# This file uses centimeters"):
    raise RuntimeError("unexpected OBJ header: %r" % lines[0])
with open(OUT, "w", encoding="ascii", newline="\n") as f:
    f.write(HEADER + "\n".join(lines[1:]))
print("wrote", OUT, flush=True)
buddy_scene.finish()
