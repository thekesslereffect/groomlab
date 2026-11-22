# SPDX-License-Identifier: Apache-2.0
"""Write engine/assets/meshes/buddy_rig.ma from the artist's Buddy scene (run with mayapy 2022).

Usage: mayapy clean_rig.py <source buddy.ma> <out buddy_rig.ma>

The scene cleaned by buddy_scene.open_clean, buddy_rig exported with its history (that leaves out
the cameras and the scene's default nodes), then in the text: no requires but Maya's, no time unit,
no `select -ne :shared` blocks (time, render globals, color management, which would otherwise be
pushed into the user's scene on import), and the SPDX lines and the credit as comments after the
first line.
"""

import os
import re
import sys

SOURCE, OUT = (os.path.abspath(a) for a in sys.argv[1:3])
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

HEADER = """// SPDX-FileCopyrightText: 2026 Nick Rutlinh
// SPDX-License-Identifier: CC-BY-4.0
// Buddy: a posable cartoon character rig to groom (centimetres, y up, facing +z; body, eyes, teeth,
// tongue skinned to one joint hierarchy, posed with the *_CTL controls).
// Credit: "Buddy by Nick Rutlinh (GroomLab), CC BY 4.0".
"""

import maya.standalone  # noqa: E402

maya.standalone.initialize(name="python")

from maya import cmds  # noqa: E402

import buddy_scene  # noqa: E402

buddy_scene.open_clean(SOURCE)
cmds.select("buddy_rig", replace=True)
cmds.file(OUT, exportSelected=True, type="mayaAscii", force=True, constructionHistory=True,
          channels=True, constraints=True, expressions=True, shader=True)

out = []
in_select = False
for line in open(OUT, encoding="latin-1").read().split("\n"):
    if in_select and line.startswith(("\t", " ")):
        continue  # the block's setAttr lines
    in_select = line.startswith("select -ne :")
    if in_select:
        continue
    if line.startswith("requires ") and not line.startswith('requires maya "'):
        continue
    if line.startswith("currentUnit "):
        line = re.sub(r" -t \w+", "", line)
    out.append(line)
with open(OUT, "w", encoding="latin-1", newline="\n") as f:
    f.write(out[0] + "\n" + HEADER + "\n".join(out[1:]))
print("wrote", OUT, flush=True)
buddy_scene.finish()
