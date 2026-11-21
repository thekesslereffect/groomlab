# SPDX-License-Identifier: Apache-2.0
"""Importing XGen grooms (groomlab.import_xgen): both flavors, on synthetic scenes made with XGen's
own API (xgen_scenes.py), compared with XGen's own strands.

Usage: mayapy test_xgen_import.py <GroomLab.mll> <maya/scripts dir> <work dir>

The comparison numbers (strand counts, lengths, straightness, how far the tips gather) are printed,
and pictures of XGen's strands beside GroomLab's are written to the work dir (xgen_*.png).
"""

import os
import shutil
import sys
import unittest

PLUGIN_PATH = os.path.abspath(sys.argv[1])
SCRIPTS_DIR = os.path.abspath(sys.argv[2])
WORK_DIR = os.path.abspath(sys.argv[3])
sys.argv = sys.argv[:1]
sys.path.insert(0, SCRIPTS_DIR)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import maya.standalone  # noqa: E402

maya.standalone.initialize(name="python")

from maya import cmds  # noqa: E402

import groomlab  # noqa: E402
import groomlab_core  # noqa: E402
import xgen_scenes  # noqa: E402
from groomlab.importers import paths as ipaths  # noqa: E402
from groomlab.importers import seexpr  # noqa: E402
from groomlab.importers import xgen as gx  # noqa: E402

SCENES = {}


def setUpModule():
    cmds.loadPlugin(PLUGIN_PATH)
    if not gx.xgen_available():
        raise unittest.SkipTest("XGen's plugin is not installed")
    shutil.rmtree(WORK_DIR, ignore_errors=True)
    os.makedirs(WORK_DIR)
    textures = os.path.join(WORK_DIR, "studio_textures")
    os.makedirs(textures)
    os.environ[xgen_scenes.TEXTURES_VARIABLE] = textures
    os.environ.pop(xgen_scenes.MISSING_VARIABLE, None)
    SCENES["textures"] = textures
    SCENES["igs"] = xgen_scenes.igs_scene(os.path.join(WORK_DIR, "igs"), textures)
    SCENES["core"] = xgen_scenes.core_scene(os.path.join(WORK_DIR, "core"))


def compare(test, description, groom, label):
    """XGen's strands and GroomLab's: their stats (printed) and a picture of each side by side."""
    xgen_abc = os.path.join(WORK_DIR, "%s_xgen.abc" % label)
    gx.export_xgen_strands(description, xgen_abc)
    ours_abc = os.path.join(WORK_DIR, "%s_groomlab.abc" % label)
    groomlab.export_alembic(groom, ours_abc, scales="none")
    theirs = groomlab_core.read_alembic_curves_at(xgen_abc)
    ours = groomlab_core.read_alembic_curves_at(ours_abc)
    a, b = xgen_scenes.strand_stats(theirs), xgen_scenes.strand_stats(ours)
    print("\n%s: XGen vs GroomLab" % label)
    for key in ("count", "length", "straightness", "tip_spread", "bbox"):
        print("  %-12s %s\n  %-12s %s" % (key, a[key], "", b[key]))
    for view in ("front", "side"):
        xgen_scenes.strand_picture(os.path.join(WORK_DIR, "xgen_%s_%s.png" % (label, view)),
                                   [("XGen", theirs), ("GroomLab", ours)], view=view)
    return a, b


def graph_types(groom):
    return [n["type"] for n in groomlab.get_graph(groom)["nodes"]]


def node_of(groom, type_):
    return next(n for n in groomlab.get_graph(groom)["nodes"] if n["type"] == type_)


class SeExprAndPaths(unittest.TestCase):
    def test_simplify(self):
        self.assertEqual(seexpr.simplify("$a=1.5;#0.05,5.0\\n$a"), ("const", 1.5))
        self.assertEqual(seexpr.simplify("1.0 # map('${DESC}/density/')"), ("const", 1.0))
        self.assertEqual(seexpr.simplify("$a=map('${DESC}/paintmaps/len/');#3dpaint,10\\n$a"),
                         ("map", "${DESC}/paintmaps/len/", 1.0))
        self.assertEqual(seexpr.simplify("0.5*map('${DESC}/paintmaps/d/')"), ("map", "${DESC}/paintmaps/d/", 0.5))
        kind, text = seexpr.simplify("$a=rand(0.5,1.0);#0,1\\n$a*2")
        self.assertEqual(kind, "expr")
        self.assertIn("$a = rand(0.5,1.0);", text)
        self.assertEqual(seexpr.map_paths("map('x/') + cmap(\"y.ptx\")"), ["x/", "y.ptx"])
        self.assertEqual(seexpr.ramp("rampUI(0,0.5,3:1,0.0,3)"), ([(0.0, 0.5), (1.0, 0.0)], False))
        self.assertEqual(seexpr.compose("$a=2;\\n$a*3", "max(0, 1 - ({}) / $length)"),
                         "$a = 2;\nmax(0, 1 - ($a*3) / $length)")

    def test_paths(self):
        folder = os.path.join(WORK_DIR, "paths_case")
        os.makedirs(os.path.join(folder, "maps"), exist_ok=True)
        with open(os.path.join(folder, "maps", "a.png"), "wb") as f:
            f.write(b"x")
        resolver = ipaths.PathResolver(tokens={"DESC": folder}, environ={"HOMEDIR": folder})
        self.assertEqual(resolver.find("${DESC}/maps/a.png")[0], os.path.normpath(folder + "/maps/a.png").replace("\\", "/"))
        self.assertEqual(resolver.find("%HOMEDIR%/maps/a.png")[0].lower(),
                         os.path.normpath(folder + "/maps/a.png").replace("\\", "/").lower())
        found, tried, unset = resolver.find("${STUDIO_ROOT}/maps/a.png")
        self.assertIsNone(found)
        self.assertEqual(unset, ["STUDIO_ROOT"])
        roots, dirs = ipaths.parse_roots("STUDIO_ROOT=%s\n${OTHER}=C:/x\n%s" % (folder, folder))
        self.assertEqual(roots, {"STUDIO_ROOT": folder, "OTHER": "C:/x"})
        self.assertEqual(dirs, [folder])
        self.assertTrue(ipaths.PathResolver(roots=roots, environ={}).find("${STUDIO_ROOT}/maps/a.png")[0])
        # A relative path, and a file moved beside the scene: found in the search folders.
        self.assertTrue(ipaths.PathResolver(search_dirs=[folder], environ={}).find("maps/a.png")[0])
        self.assertTrue(ipaths.PathResolver(search_dirs=[os.path.join(folder, "maps")], environ={})
                        .find("C:/gone/elsewhere/a.png")[0])


class PtexAgainstXGen(unittest.TestCase):
    def test_xgen_baked_ptex_reads_back_as_the_vertex_colors(self):
        """XGen baked the Core scene's density Ptex from vertex colors: read back at each vertex's
        corners it gives those colors (the reader's conventions are XGen's)."""
        cmds.file(SCENES["core"]["scene"], open=True, force=True)
        ptex = SCENES["core"]["ptex"]
        info = groomlab_core.ptex_info(ptex)
        mesh = cmds.listRelatives("head", shapes=True)[0]
        from groomlab.importers.textures import scalp_topology
        topo = scalp_topology(mesh)
        self.assertEqual(info["faces"], len(topo["counts"]))
        values = groomlab_core.ptex_vertex_values(ptex, topo["counts"], topo["vertices"], topo["vertex_count"], 1)
        rgb = cmds.polyColorPerVertex(mesh + ".vtx[*]", query=True, rgb=True)
        colors = rgb[0::3]
        worst = max(abs(a - b) for a, b in zip(values, colors))
        self.assertLess(worst, 0.02)
        png = os.path.join(WORK_DIR, "dens_baked.png")
        result = groomlab_core.ptex_to_png(ptex, topo["counts"], topo["uv_ids"], topo["us"], topo["vs"], png, 256, 256, 1)
        self.assertGreater(result["covered"], 0.3)
        self.assertTrue(os.path.isfile(png))


class XGenImport(unittest.TestCase):
    def test_lists_both_flavors(self):
        cmds.file(SCENES["igs"]["scene"], open=True, force=True)
        self.assertEqual([(d["name"], d["flavor"]) for d in groomlab.xgen_descriptions()], [("igsHair", "igs")])
        cmds.file(SCENES["core"]["scene"], open=True, force=True)
        self.assertEqual([(d["name"], d["flavor"], d["palette"]) for d in groomlab.xgen_descriptions()],
                         [("coreHair", "core", "hairCollection")])
        with self.assertRaises(ValueError):
            groomlab.import_xgen(descriptions=["noSuchDescription"])

    def test_interactive_groom(self):
        cmds.file(SCENES["igs"]["scene"], open=True, force=True)
        report = groomlab.import_xgen()
        self.assertEqual(len(report.grooms), 1)
        entry = report.grooms[0]
        groom = entry["groom"]
        self.assertTrue(groom and cmds.objExists(groom))
        self.assertEqual(groomlab.scalp_mesh(groom).split("|")[-1], "headShape")
        types = graph_types(groom)
        for t in ("Scatter", "Interpolate", "Clump", "Noise", "Length", "Width", "Map", "Resample", "Output"):
            self.assertIn(t, types)
        self.assertLess(types.index("Clump"), types.index("Noise"))
        width = node_of(groom, "Width")["params"]
        self.assertAlmostEqual(width["width"], 0.03, places=3)  # XGen's own widths
        self.assertLess(width["profile"]["keys"][-1][1], 0.6)   # widthTaper 0.6
        # The density map: found under ${GROOMLAB_XGEN_TEST_TEXTURES}, copied into the masks folder.
        self.assertEqual(len(entry["files"]), 1)
        density = groomlab.map_texture(groom, "density")
        self.assertTrue(density and os.path.isfile(density))
        masks = groomlab.asset_folder("masks", create=False)
        self.assertTrue(os.path.normcase(density).startswith(os.path.normcase(masks)))
        self.assertIn("density", [c["from"] for c in groomlab.get_graph(groom)["connections"]
                                  if c["to"] == "scatter" and c["input"] == "density"])
        # The mask under a variable nothing sets: reported with what to set, the rest imported.
        self.assertEqual(len(entry["missing"]), 1)
        self.assertEqual(entry["missing"][0]["unset"], [xgen_scenes.MISSING_VARIABLE])
        self.assertEqual(report.unset_variables, [xgen_scenes.MISSING_VARIABLE])
        self.assertIn("set nowhere", report.text())
        # Muted modifiers come back unmuted.
        self.assertFalse(any(cmds.getAttr(n + ".mute") for n in ("clumpA", "noiseA", "cutA")))

        theirs, ours = compare(self, entry["name"], groom, "igs")
        self.assertAlmostEqual(ours["count"] / float(theirs["count"]), 1.0, delta=0.03)
        self.assertAlmostEqual(ours["length"][1] / theirs["length"][1], 1.0, delta=0.15)
        self.assertLess(abs(ours["straightness"] - theirs["straightness"]), 0.08)

        # Pointing a root at the missing variable's folder brings the mask in too.
        root = os.path.join(WORK_DIR, "missing_root")
        os.makedirs(root, exist_ok=True)
        xgen_scenes.write_gradient_png(os.path.join(root, "noise_mask.png"))
        cmds.delete(cmds.listRelatives(groom, parent=True))
        report = groomlab.import_xgen(roots="%s=%s" % (xgen_scenes.MISSING_VARIABLE, root))
        entry = report.grooms[0]
        self.assertEqual(entry["missing"], [])
        self.assertEqual(len(entry["files"]), 2)
        groom = entry["groom"]
        noise = node_of(groom, "Noise")
        self.assertIn("affect", noise.get("paramInputs", []))
        # The copies keep the groom whole when the sources go.
        cmds.file(rename=os.path.join(WORK_DIR, "igs", "igs_imported.ma"))
        cmds.file(save=True, type="mayaAscii")
        shutil.move(SCENES["textures"], SCENES["textures"] + "_moved")
        shutil.rmtree(root)
        try:
            for name in ("density", "mask"):
                path = groomlab.map_texture(groom, name)
                self.assertTrue(os.path.isfile(path), path)
            self.assertEqual(groomlab.missing_files(groom)["maps"], {})
            self.assertGreater(cmds.getAttr(groom + ".outStrandCount"), 0)
        finally:
            shutil.move(SCENES["textures"] + "_moved", SCENES["textures"])

    def test_core_description(self):
        cmds.file(SCENES["core"]["scene"], open=True, force=True)
        report = groomlab.import_xgen(descriptions="coreHair")
        entry = report.grooms[0]
        groom = entry["groom"]
        self.assertTrue(groom and cmds.objExists(groom), report.text())
        types = graph_types(groom)
        for t in ("Transform", "Clump", "Noise", "Curl", "Length", "Width", "Map"):
            self.assertIn(t, types)
        self.assertEqual(types.index("Clump") < types.index("Noise") < types.index("Curl") < types.index("Length"), True)
        # The Ptex density mask (map('${DESC}/paintmaps/dens/')) as the map "dens", a PNG in the masks folder.
        self.assertEqual(groomlab.map_kind(groom, "dens"), "texture")
        self.assertTrue(os.path.isfile(groomlab.map_texture(groom, "dens")))
        self.assertEqual([f["what"] for f in entry["files"]], ["dens"])
        # XGen's clumps at the guides: Clump's centers are the guides.
        clump = node_of(groom, "Clump")
        self.assertEqual(clump["params"]["centersFrom"], "inputPoints")
        graph = groomlab.get_graph(groom)
        self.assertIn({"from": "guides", "to": clump["name"], "input": "centers"}, graph["connections"])
        self.assertAlmostEqual(node_of(groom, "Transform")["params"]["scale"], 1.1)
        self.assertIn("1 - 0.3 / $length", node_of(groom, "Length")["params"]["keep"]["expression"])
        self.assertEqual(entry["missing"], [])
        self.assertEqual(len(groomlab.list_guide_groups(groom)), 1)

        theirs, ours = compare(self, entry["name"], groom, "core")
        self.assertAlmostEqual(ours["count"] / float(theirs["count"]), 1.0, delta=0.2)
        self.assertAlmostEqual(ours["length"][1] / theirs["length"][1], 1.0, delta=0.15)
        self.assertLess(abs(ours["straightness"] - theirs["straightness"]), 0.08)
        self.assertLess(theirs["tip_spread"], 0.6)  # XGen's clumps
        self.assertLess(ours["tip_spread"], 0.8)    # and GroomLab's


class ConvertToInteractiveGroom(unittest.TestCase):
    """groomlab.convert_to_xgen_interactive: a groom's strands as a new Interactive Groom on its
    scalp, each strand a spline where it is, kept with the scene (not the Maya curves)."""

    def test_strands_become_splines_where_they_are(self):
        import math
        cmds.file(new=True, force=True)
        # (16 sides: XGen roots no spline at a pole of more than 20 edges)
        scalp = cmds.polySphere(radius=1, subdivisionsX=16, subdivisionsY=16)[0]
        cmds.move(0, 2, 0, scalp)
        groom = groomlab.create_groom(scalp)
        curves = []
        for angle in (0.35, 0.8, 1.2):
            n = (math.sin(angle), math.cos(angle) + 2.0, 0.2)
            curves.append(cmds.curve(degree=1, point=[n, (n[0] * 1.6, n[1] + 0.6, 0.3), (n[0] * 2.1, n[1] + 0.9, 0.4)]))
        groomlab.guides_from_curves(groom, curves)
        groomlab.mirror_group(groom, "primary", "x")
        path = os.path.join(WORK_DIR, "convert", "groom_xgi.abc")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        cmds.select(groom)
        made = groomlab.convert_to_xgen_interactive(groom, path=path, density=0.05)
        self.assertTrue(os.path.isfile(made["file"]))
        self.assertEqual(cmds.nodeType(made["shape"]), "xgmSplineDescription")
        self.assertEqual(cmds.getAttr(made["curves"] + ".fileName"), path.replace("\\", "/"))
        self.assertEqual(cmds.ls(selection=True), [groom], "the selection kept")
        ours = groomlab_core.read_alembic_curves_at(made["file"])
        self.assertGreater(ours.strand_count, 20)
        self.assertEqual(made["strands"], ours.strand_count)

        # XGen's splines: every strand, root and tip where GroomLab's are (XGen roots them on the
        # faceted scalp, GroomLab on the smoothed one: a few thousandths apart).
        def ends(strands):
            out = []
            for i in range(strands.strand_count):
                pts = strands.strand_points(i)
                out.append(((pts[0].x, pts[0].y, pts[0].z), (pts[-1].x, pts[-1].y, pts[-1].z)))
            return out

        def far(a, b):
            return math.sqrt(sum((a[k] - b[k]) ** 2 for k in range(3)))

        theirs_abc = os.path.join(WORK_DIR, "convert", "xgen_splines.abc")
        gx.export_splines(made["shape"], theirs_abc)
        theirs = ends(groomlab_core.read_alembic_curves_at(theirs_abc))
        self.assertEqual(len(theirs), ours.strand_count)
        worst = 0.0
        for root, tip in ends(ours):
            other = min(theirs, key=lambda e: far(e[0], root))
            worst = max(worst, far(other[0], root), far(other[1], tip))
        self.assertLess(worst, 0.03)

        # Nothing links back to the groom: deleted, the Interactive Groom keeps its splines, saved
        # and opened again too.
        cmds.delete(groom)
        self.assertEqual(cmds.xgmSplineQuery(made["shape"], splineCount=True), ours.strand_count)
        scene = os.path.join(WORK_DIR, "convert", "converted.ma")
        cmds.file(rename=scene)
        cmds.file(save=True, type="mayaAscii")
        cmds.file(new=True, force=True)
        cmds.file(scene, open=True, force=True)
        shapes = cmds.ls(type="xgmSplineDescription")
        self.assertEqual(len(shapes), 1)
        self.assertEqual(cmds.xgmSplineQuery(shapes[0], splineCount=True), ours.strand_count)


if __name__ == "__main__":
    result = unittest.main(exit=False, verbosity=2).result
    maya.standalone.uninitialize()
    sys.exit(0 if result.wasSuccessful() else 1)
