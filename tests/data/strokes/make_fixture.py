# SPDX-License-Identifier: Apache-2.0
"""Writes the scripted-stroke fixture test_strokes.py replays: a groom file, and what Maya's
groomlab.brush_stroke stored after each of a sequence of strokes on that groom (the md5 of each
attribute's text).

    mayapy make_fixture.py <GroomLab.mll> <hosts/maya/scripts>

(with MAYA_APP_DIR pointing at a prefs folder that does not auto-load an installed GroomLab).
Run it again only when a stroke's output is meant to change.
"""

import hashlib
import json
import math
import os
import sys

PLUGIN, SCRIPTS = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
HERE = os.path.dirname(os.path.abspath(__file__))
sys.argv = sys.argv[:1]
sys.path.insert(0, SCRIPTS)

import maya.standalone  # noqa: E402

maya.standalone.initialize(name="python")

from maya import cmds  # noqa: E402

import groomlab  # noqa: E402

# (mode, points, keyword arguments, the attribute it writes)
STROKES = [
    ("comb", [(-0.3, 0.95, 0.4), (0.0, 1.0, 0.45), (0.3, 0.95, 0.4)], {"radius": 0.4, "strength": 0.8, "stretch": False}, "guideData"),
    ("smooth", [(0.2, 0.9, 0.3), (0.4, 0.8, 0.4)], {"radius": 0.5, "strength": 0.6, "symmetry": "x"}, "guideData"),
    ("lift", [(0.3, 0.9, 0.2)], {"radius": 0.6, "strength": 0.5, "level": -1}, "guideData"),
    ("twist", [(0.3, 0.9, 0.2), (0.5, 0.7, 0.3)], {"radius": 0.5, "strength": 0.4, "group": "children"}, "guideData"),
    ("clump", [(0.4, 0.85, 0.2)], {"radius": 0.5, "value": 0.7}, "guideData"),
    ("place", [(0.2, 0.2, 0.95), (-0.5, 0.5, 0.7)], {"symmetry": "x", "length": 0.6}, "guideData"),
    ("draw", [(0.0, 0.7, 0.72), (0.0, 0.9, 1.0), (0.0, 1.1, 1.2)], {"segments": 8}, "guideData"),
    ("part", [(-0.3, 0.95, 0.3), (0.0, 0.97, 0.25), (0.3, 0.95, 0.3)], {"part_name": "crown", "symmetry": "x"}, "partData"),
    ("flow", [(-0.4, 0.9, 0.2), (-0.1, 0.95, 0.3), (0.2, 0.9, 0.35)], {"radius": 0.3}, "flowData"),
    ("comb", [(-0.3, 0.95, 0.3), (0.3, 0.95, 0.3)], {"radius": 0.3, "stretch": False, "targets": "guides,parts,flow"}, "guideData,partData,flowData"),
    ("erase", [(0.0, 0.6, 0.8)], {"radius": 0.3}, "guideData"),
]


def main():
    cmds.loadPlugin(PLUGIN)
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
    groom = groomlab.create_groom(scalp)
    curves = []
    for angle in (0.35, 0.8, 1.2):
        n = (math.sin(angle), math.cos(angle), 0.2)
        curves.append(cmds.curve(degree=1, point=[n, tuple(1.6 * c for c in n), tuple(2.1 * c for c in n)]))
    groomlab.guides_from_curves(groom, curves)
    groomlab.mirror_group(groom, "primary", "x")
    groomlab.generate_children(groom, "primary", name="children", count=3)
    path = os.path.join(HERE, "strokes.groom")
    groomlab.export_groom(groom, path)
    with open(path) as f:
        data = json.load(f)
    data["source"] = "make_fixture.py"  # no local paths in the fixture (the values read back the same)
    data["searchPaths"] = []
    with open(path, "w", newline="\n") as f:
        json.dump(data, f, separators=(",", ":"))
    expected = []
    for mode, points, kwargs, attributes in STROKES:
        groomlab.brush_stroke(groom, mode, points, **kwargs)
        after = {a: hashlib.md5((cmds.getAttr(groom + "." + a) or "").encode("utf-8")).hexdigest()
                 for a in attributes.split(",")}
        expected.append({"mode": mode, "points": points, "kwargs": kwargs, "after": after})
    with open(os.path.join(HERE, "expected.json"), "w", newline="\n") as f:
        json.dump(expected, f, indent=1, sort_keys=True)
        f.write("\n")
    print("wrote %d strokes" % len(expected))


main()
