# SPDX-License-Identifier: Apache-2.0
"""Write engine/assets/meshes/buddy_rig.fbx (for Blender) from the artist's Buddy scene (mayapy 2022).

Usage: mayapy export_fbx.py <source buddy.ma> <out buddy_rig.fbx>

The scene cleaned as for buddy_rig.ma (buddy_scene.open_clean), then the joints and the five
skinned meshes at the bind pose, without the controls or constraints (binary FBX 7.5, centimetres,
y up). The absolute paths the exporter embeds are rewritten to bare file names (fbx_paths.py).
"""

import os
import sys

SOURCE, OUT = (os.path.abspath(a) for a in sys.argv[1:3])
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import fbx_paths  # noqa: E402

import maya.standalone  # noqa: E402

maya.standalone.initialize(name="python")

from maya import cmds, mel  # noqa: E402

import buddy_scene  # noqa: E402

cmds.loadPlugin("fbxmaya", quiet=True)
buddy_scene.open_clean(SOURCE)  # checks the controls hold the bind pose

# The joints and meshes free of the rig: no constraints, no controls or groups.
cmds.delete(cmds.ls(type="constraint"))
cmds.parent("root", world=True)
# geometry_GRP doesn't inherit: the meshes' own matrices already are their world ones.
cmds.parent(buddy_scene.MESHES, world=True, relative=True)
cmds.delete("buddy_rig")

mel.eval("FBXResetExport")
for command in ("FBXExportSkins -v true", "FBXExportShapes -v true", "FBXExportSmoothingGroups -v true",
                "FBXExportSmoothMesh -v false", "FBXExportConstraints -v false",
                "FBXExportCameras -v false", "FBXExportLights -v false",
                "FBXExportInputConnections -v false", "FBXExportBakeComplexAnimation -v false",
                "FBXExportEmbeddedTextures -v false", "FBXExportUpAxis y", "FBXExportInAscii -v false",
                "FBXExportFileVersion -v FBX201800"):
    mel.eval(command)
cmds.select(["root"] + buddy_scene.MESHES, replace=True)
mel.eval('FBXExport -f "%s" -s' % OUT.replace("\\", "/"))

for old, new in fbx_paths.rewrite(OUT):
    print("rewrote", old.decode("utf-8", "replace"), "->", new.decode("utf-8", "replace"), flush=True)
print("wrote", OUT, flush=True)
buddy_scene.finish()
