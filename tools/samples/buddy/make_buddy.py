# SPDX-License-Identifier: Apache-2.0
"""Regenerate the shipped Buddy files from the artist's scene: one command per rig update.

Usage: python tools/samples/buddy/make_buddy.py <source buddy.ma> [out dir, default engine/assets/meshes]

Runs clean_rig.py, export_fbx.py and export_obj.py with Maya 2022's mayapy (MAYAPY overrides the
path) and a throwaway MAYA_APP_DIR under build/, then checks no absolute path or user/folder name
is left in the FBX. See README.md.
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, HERE)

import fbx_paths  # noqa: E402

MAYAPY = os.environ.get("MAYAPY", r"C:\Program Files\Autodesk\Maya2022\bin\mayapy.exe")
STEPS = [("clean_rig.py", "buddy_rig.ma"), ("export_fbx.py", "buddy_rig.fbx"), ("export_obj.py", "buddy.obj")]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    source = os.path.abspath(sys.argv[1])
    out_dir = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "engine", "assets", "meshes"))
    env = dict(os.environ, MAYA_APP_DIR=os.path.join(ROOT, "build", "buddy_maya_app"))
    os.makedirs(env["MAYA_APP_DIR"], exist_ok=True)
    for script, name in STEPS:
        out = os.path.join(out_dir, name)
        print("%s -> %s" % (script, out), flush=True)
        # mayapy crashes in Arnold's shutdown after the script's work: judge by the file written.
        before = os.path.getmtime(out) if os.path.exists(out) else None
        subprocess.call([MAYAPY, os.path.join(HERE, script), source, out], env=env)
        if not os.path.exists(out) or os.path.getmtime(out) == before:
            sys.exit("%s didn't write %s" % (script, out))

    fbx = os.path.join(out_dir, "buddy_rig.fbx")
    left = fbx_paths.scan(fbx)
    raw = open(fbx, "rb").read()
    words = [w for w in (b"NickRutlinh", b"Users", b"worktree", b".claude", os.path.expanduser("~").encode())
             if w in raw]
    # "D:" also turns up inside compressed arrays: only a readable path counts.
    drives = re.findall(rb"[\x20-\x7e]*[A-Za-z]:[/\\][\x20-\x7e]{3,}", raw)
    if left or words or drives:
        sys.exit("paths left in %s: %r %r %r" % (fbx, left, words, drives))
    print("no paths left in", fbx)


if __name__ == "__main__":
    main()
