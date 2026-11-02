# SPDX-License-Identifier: Apache-2.0
"""Arnold translator test: export a groom to .ass through MtoA and inspect it.

Usage: mayapy test_arnold.py <GroomLab.mll> <maya/scripts dir> <extensions dir> [out.ass] [procedurals dir]
The extensions dir must hold groomLabTranslator.dll/.so. It is put on
MTOA_EXTENSIONS_PATH before MtoA loads, and the procedurals dir (holding
groomlab_procedural) on ARNOLD_PLUGIN_PATH.
"""

import json
import math
import os
import re
import subprocess
import sys
import tempfile
import unittest

PLUGIN_PATH = os.path.abspath(sys.argv[1])
SCRIPTS_DIR = os.path.abspath(sys.argv[2])
EXTENSIONS_DIR = os.path.abspath(sys.argv[3])
ASS_PATH = os.path.abspath(sys.argv[4]) if len(sys.argv) > 4 else os.path.join(tempfile.gettempdir(), "groomlab_test.ass")
PROCEDURALS_DIR = os.path.abspath(sys.argv[5]) if len(sys.argv) > 5 and sys.argv[5] else None
sys.argv = sys.argv[:1]
sys.path.insert(0, SCRIPTS_DIR)
os.environ["MTOA_EXTENSIONS_PATH"] = os.pathsep.join(
    p for p in [EXTENSIONS_DIR, os.environ.get("MTOA_EXTENSIONS_PATH", "")] if p)
if PROCEDURALS_DIR:
    os.environ["ARNOLD_PLUGIN_PATH"] = os.pathsep.join(
        p for p in [PROCEDURALS_DIR, os.environ.get("ARNOLD_PLUGIN_PATH", "")] if p)

import maya.standalone  # noqa: E402

maya.standalone.initialize(name="python")

from maya import cmds  # noqa: E402

import groomlab  # noqa: E402
import groomlab_core  # noqa: E402

DENSITY = 8000  # ~100k strands on a unit sphere (the M1 gate)


def read_curves_node(path):
    """Return (num_points values, points count, radius count, basis) of the first curves node."""
    with open(path) as f:
        text = f.read()
    match = re.search(r"\ncurves\s*\n\{(.*?)\n\}", text, re.S)
    if not match:
        return None
    body = match.group(1)

    def array_len(name):
        m = re.search(r"\n\s*" + name + r"\s+(\d+)\s+(\d+)\s+(\w+)", body)
        return int(m.group(1)) if m else 0

    basis = re.search(r"\n\s*basis\s+\"?(\w+)", body)
    shader = re.search(r"\n\s*shader\s+\"?([^\s\"]+)", body)
    return {
        "curves": array_len("num_points"),
        "points": array_len("points"),
        "radius": array_len("radius"),
        "basis": basis.group(1) if basis else None,
        "shader": shader.group(1) if shader else None,
        "text": text,
    }


class ArnoldTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin("mtoa")
        cmds.loadPlugin(PLUGIN_PATH)

    def setUp(self):
        cmds.file(new=True, force=True)
        self.scalp = cmds.polySphere(radius=1, subdivisionsX=80, subdivisionsY=40)[0]
        guides = [
            cmds.curve(degree=2, point=[(0, 1, 0), (0.6, 1.5, 0.2), (1.4, 1.2, 0.3)]),
            cmds.curve(degree=2, point=[(0, -1, 0), (-0.5, -1.5, 0.3), (-1.3, -1.6, 0.4)]),
            cmds.curve(degree=2, point=[(1, 0, 0), (1.6, -0.3, 0.4), (1.9, -1.0, 0.6)]),
            cmds.curve(degree=2, point=[(-1, 0, 0), (-1.6, 0.4, 0.2), (-2.0, 1.0, 0.3)]),
        ]
        self.groom = groomlab.create_groom(self.scalp, guides)
        graph = {
            "nodes": [
                {"name": "scalp", "type": "Scalp"},
                {"name": "guides", "type": "Guides"},
                {"name": "scatter", "type": "Scatter", "params": {"density": DENSITY}},
                {"name": "interpolate", "type": "Interpolate"},
                {"name": "width", "type": "Width", "params": {"width": 0.006, "profile": [[0, 1], [1, 0.1667]]}},
                {"name": "resample", "type": "Resample", "params": {"segments": 15, "mode": "even"}},
                {"name": "output", "type": "Output"},
            ],
            "connections": [
                {"from": "scalp", "to": "scatter", "input": "scalp"},
                {"from": "scatter", "to": "interpolate", "input": "strands"},
                {"from": "guides", "to": "interpolate", "input": "guides"},
                {"from": "interpolate", "to": "width", "input": "strands"},
                {"from": "width", "to": "resample", "input": "strands"},
                {"from": "resample", "to": "output", "input": "strands"},
            ],
            "output": "output",
        }
        groomlab.set_graph(self.groom, graph)
        # Viewport preview stays low; the render must ignore it.
        cmds.setAttr(self.groom + ".previewDensity", 0.05)

        cmds.shadingNode("aiSkyDomeLight", asLight=True)
        cmds.viewFit("persp", allObjects=True)

    def test_translator_adds_arnold_attributes(self):
        for attr in ("aiMode", "aiMinPixelWidth", "aiSelfShadows"):
            self.assertTrue(cmds.attributeQuery(attr, node=self.groom, exists=True), attr)

    def test_match_render_reads_mtoa_subdivision(self):
        # MtoA's own attributes on the scalp: the Scalp node (match render) refines as Arnold
        # does, and the scalp Arnold renders is that subdivision with UVs pinned at their corners
        # (subdiv_uv_smoothing pin_corners, the rule our UVs follow: subdivision.h).
        shape = cmds.listRelatives(self.scalp, shapes=True)[0]
        stats = json.loads(cmds.groomLabGraphStats(self.groom))
        self.assertNotIn("scalp", stats.get("notes", {}))
        cmds.setAttr(shape + ".aiSubdivType", 1)
        cmds.setAttr(shape + ".aiSubdivIterations", 2)
        note = json.loads(cmds.groomLabGraphStats(self.groom)).get("notes", {}).get("scalp", "")
        self.assertIn("Catmull-Clark x2", note)
        self.assertIn("Arnold catclark, 2 iterations", note)
        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True)
        with open(ASS_PATH) as f:
            text = f.read()
        mesh = re.search(r"\npolymesh\s*\n\{(.*?)\n\}", text, re.S)
        self.assertIsNotNone(mesh, "no polymesh in " + ASS_PATH)
        body = mesh.group(1)
        self.assertRegex(body, r"\n\s*subdiv_type\s+\"?catclark")
        self.assertRegex(body, r"\n\s*subdiv_iterations\s+2\b")
        uv_smoothing = re.search(r"\n\s*subdiv_uv_smoothing\s+\"?(\w+)", body)
        self.assertEqual(uv_smoothing.group(1) if uv_smoothing else "pin_corners", "pin_corners")

    def test_export_full_density_curves(self):
        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True)
        node = read_curves_node(ASS_PATH)
        self.assertIsNotNone(node, "no curves node in " + ASS_PATH)

        preview = cmds.getAttr(self.groom + ".outStrandCount")
        self.assertGreater(node["curves"], 95000)
        self.assertGreater(node["curves"], 10 * preview)
        self.assertEqual(node["points"], node["curves"] * 16)
        self.assertEqual(node["radius"], node["points"])
        self.assertEqual(node["basis"], "linear")
        self.assertIsNotNone(node["shader"])
        self.assertIn("standard_hair", node["text"])

    def test_render_overrides_reach_arnold_and_leave_the_viewport(self):
        # renderDensity, renderWidthScale, renderLengthScale: the render gets them (curves mode:
        # the translator; groom file mode: the procedural's density, width_scale, length_scale),
        # the viewport stays as groomed.
        def export(name):
            path = os.path.splitext(ASS_PATH)[0] + "_%s.ass" % name
            cmds.arnoldExportAss(filename=path, asciiAss=True)
            with open(path) as f:
                return f.read()

        def reach(body):
            points = array_values(body, "points", 3)[2]
            return max(math.sqrt(points[i] ** 2 + points[i + 1] ** 2 + points[i + 2] ** 2)
                       for i in range(0, len(points), 3))

        plain = block(export("overrides_plain"), "curves")
        curves = array_values(plain, "num_points")[0]
        radius = max(array_values(plain, "radius")[2])
        preview = cmds.getAttr(self.groom + ".outStrandCount")
        cmds.setAttr(self.groom + ".renderDensity", 0.5)
        cmds.setAttr(self.groom + ".renderWidthScale", 2.0)
        cmds.setAttr(self.groom + ".renderLengthScale", 0.5)
        self.assertEqual(cmds.getAttr(self.groom + ".outStrandCount"), preview)  # the viewport as groomed
        over = block(export("overrides"), "curves")
        self.assertAlmostEqual(array_values(over, "num_points")[0] / float(curves), 0.5, delta=0.05)
        self.assertAlmostEqual(max(array_values(over, "radius")[2]) / radius, 2.0, places=3)
        # The hair reaches out half as far from the unit sphere's surface.
        self.assertAlmostEqual((reach(over) - 1.0) / (reach(plain) - 1.0), 0.5, delta=0.05)

        groom_file = os.path.splitext(ASS_PATH)[0] + "_overrides.groom"
        groomlab.render_from_groom_file(self.groom, groom_file, scalp_cache=False)
        proc = block(export("overrides_procedural"), "groomlab_procedural")
        self.assertIsNotNone(proc)
        self.assertRegex(proc, r"\n\s*density 0\.5\b")
        self.assertRegex(proc, r"\n\s*width_scale 2\b")
        self.assertRegex(proc, r"\n\s*length_scale 0\.5\b")
        groomlab.render_curves(self.groom)

    def test_several_scalps_and_a_reference_render(self):
        # Another scalp of the groom and a Reference node's library: both render in Arnold.
        from groomlab import graph as gm
        cmds.setAttr(self.groom + ".previewDensity", 0.01)
        library = os.path.splitext(ASS_PATH)[0] + "_library.groom"
        groomlab.export_groom(self.groom, library)

        def curves():
            path = os.path.splitext(ASS_PATH)[0] + "_scalps.ass"
            cmds.arnoldExportAss(filename=path, asciiAss=True)
            with open(path) as f:
                return array_values(block(f.read(), "curves"), "num_points")[0]

        alone = curves()
        ear = cmds.polyPlane(width=0.6, height=0.6, subdivisionsX=4, subdivisionsY=4, name="ear")[0]
        cmds.xform(ear, translation=(1.4, 0.0, 0.0), rotation=(0, 0, 90))
        groomlab.add_scalp(self.groom, ear)
        self.assertEqual(groomlab.errors(self.groom), [])
        with_ear = curves()
        self.assertGreater(with_ear, alone + 1000)

        model = gm.GraphModel.from_groom(self.groom)
        ref = model.add_node("Reference", params={"path": library.replace("\\", "/"), "bind": "world"})
        merge = model.add_node("Merge")
        model.insert_before("output", merge)
        model.connect(ref, merge, "b")
        model.save(self.groom)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(curves(), with_ear + alone)  # the library is this groom as it was

    def test_instances_render_through_an_instancer(self):
        flake = cmds.polySphere(name="snowflake", radius=1, subdivisionsX=6, subdivisionsY=4)[0]
        cmds.setAttr(flake + ".visibility", False)
        groomlab.add_instances(self.groom, flake, probability=0.02, scale=0.01, orient="up")
        cmds.refresh(force=True)  # the viewport draws them too
        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True)
        with open(ASS_PATH) as f:
            text = f.read()
        instancer = block(text, "instancer")
        self.assertIsNotNone(instancer, "no instancer in the .ass")
        self.assertRegex(instancer, r"instance_matrix \d+ 1 MATRIX")
        count = int(re.search(r"node_idxs (\d+) 1 UINT", instancer).group(1))
        self.assertGreater(count, 1000)  # 2% of ~100k strands
        self.assertIn("prototype0", text)
        self.assertIsNotNone(read_curves_node(ASS_PATH))

    def test_instances_bent_along_their_strands_render_as_meshes(self):
        # deform alongStrand: each leaf (a strip up +Y, 8 rows) bent along its strand, rendered as
        # one polymesh with the scales' data (not by the instancer), in the leaf's shading group.
        groomlab.set_param(self.groom, "scatter", "density", 400.0)
        leaf = cmds.polyPlane(name="leaf", width=1, height=1, subdivisionsX=1, subdivisionsY=8, axis=(0, 0, 1))[0]
        cmds.move(0, 0.5, 0, leaf)
        cmds.makeIdentity(leaf, apply=True, translate=True)
        cmds.setAttr(leaf + ".visibility", False)
        leaf_shader = cmds.shadingNode("lambert", asShader=True, name="leafGreen")
        cmds.setAttr(leaf_shader + ".color", 0.1, 0.6, 0.1, type="double3")
        leaf_sg = cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name="leafGreenSG")
        cmds.connectAttr(leaf_shader + ".outColor", leaf_sg + ".surfaceShader")
        cmds.sets(leaf, edit=True, forceElement=leaf_sg)
        node = groomlab.add_instances(self.groom, leaf, probability=0.5, t_range=(0.0, 0.0), size_mode="fitStrand",
                                      fit_scale=0.9, aspect=0.15, spin_random=0.0)
        self.assertEqual(groomlab.errors(self.groom), [])
        for sky in cmds.ls(type="aiSkyDomeLight"):
            cmds.setAttr(sky + ".camera", 0)  # a black background
        cmds.setAttr("persp.translate", 0, 0, 7)
        cmds.setAttr("persp.rotate", 0, 0, 0)
        # Rigid (deform none): through the instancer, the prototype in the leaf's shading group.
        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True)
        with open(ASS_PATH) as f:
            text = f.read()
        self.assertIsNotNone(block(text, "instancer"))
        prototype = re.search(r"\npolymesh\s*\n\{(\s*\n\s*name\s+\S*prototype0\b.*?)\n\}", text, re.S).group(1)
        self.assertRegex(prototype, r"\n\s*shader \"?leafGreen", "leafGreen in the .ass: %s" % ("leafGreen" in text))
        rigid = sum(1 for p in self.render("rigid_leaves", kind="instances") if max(p) > 0.002)
        print("rigid instances rendered:", os.path.splitext(ASS_PATH)[0] + "_instances_rigid_leaves.png")
        self.assertGreater(rigid, 2000)
        groomlab.set_param(self.groom, node, "deform", "alongStrand")
        cmds.refresh(force=True)  # the viewport draws them too
        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True)
        with open(ASS_PATH) as f:
            text = f.read()
        self.assertIsNone(block(text, "instancer"), "bent instances are not placed by the instancer")
        match = re.search(r"\npolymesh\s*\n\{(\s*\n\s*name\s+\S*scales\b.*?)\n\}", text, re.S)
        self.assertIsNotNone(match, "no polymesh of the bent leaves in the .ass")
        mesh = match.group(1)
        faces = int(re.search(r"\n\s*nsides (\d+) 1 UINT", mesh).group(1))
        self.assertEqual(faces % 16, 0)  # 16 triangles a leaf
        self.assertGreater(faces // 16, 100)  # half of a few hundred strands
        self.assertRegex(mesh, r"declare scaleColor varying RGB")
        bent = sum(1 for p in self.render("bent_leaves", kind="instances") if max(p) > 0.002)
        print("bent instances rendered:", os.path.splitext(ASS_PATH)[0] + "_instances_bent_leaves.png")
        self.assertGreater(bent, 2000)
        self.assertRegex(mesh[:2000], r"\n\s*shader \"?leafGreen")

    def test_scales_render_as_one_mesh_with_their_data(self):
        from groomlab import presets
        presets.apply_preset(self.groom, "dragon_scales")
        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True)
        with open(ASS_PATH) as f:
            text = f.read()
        match = re.search(r"\npolymesh\s*\n\{(\s*\n\s*name\s+\S*scales\b.*?)\n\}", text, re.S)
        self.assertIsNotNone(match, "no scales polymesh in the .ass")
        mesh = match.group(1)
        faces = int(re.search(r"\n\s*nsides (\d+) 1 UINT", mesh).group(1))
        self.assertGreater(faces, 10000)  # a few hundred plates of ~100 triangles
        for name, kind in (("scaleColor", "varying RGB"), ("scaleRootColor", "uniform RGB"), ("scaleId", "uniform FLOAT"),
                           ("scaleUV", "uniform VECTOR2"), ("scaleVariant", "uniform INT")):
            self.assertRegex(mesh, r"declare %s %s" % (name, kind))
        variants = re.search(r"\n\s*scaleVariant (\d+) 1 (\w+)\s+([^\n]*)", mesh)
        self.assertEqual(int(variants.group(1)), faces)
        if variants.group(2) == "INT":  # (written as text, not base85)
            # Keeled plates on the back, shields on the belly.
            self.assertEqual(set(array_values(mesh, "scaleVariant")[2]), {0.0, 1.0})
        # The built-in plates' own shader reads scaleColor; the strands render as the scales alone.
        self.assertRegex(text, r"user_data_rgb\s*\n\{[^}]*attribute \"scaleColor\"")
        self.assertRegex(block(text, "curves"), r"\n\s*visibility 0\b")
        # A material named on the node: any surface shader, reading the user data.
        shader = cmds.shadingNode("aiStandardSurface", asShader=True, name="scaleSkin")
        data = cmds.shadingNode("aiUserDataColor", asUtility=True, name="scaleSkinColor")
        cmds.setAttr(data + ".attribute", "scaleColor", type="string")
        cmds.connectAttr(data + ".outColor", shader + ".baseColor")
        sg = cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name="scaleSkinSG")
        cmds.connectAttr(shader + ".outColor", sg + ".surfaceShader")
        groomlab.set_param(self.groom, "scales", "material", sg)
        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True)
        with open(ASS_PATH) as f:
            text = f.read()
        mesh = re.search(r"\npolymesh\s*\n\{(\s*\n\s*name\s+\S*scales\b.*?)\n\}", text, re.S).group(1)
        self.assertRegex(mesh, r"\n\s*shader \"?scaleSkin")
        self.assertRegex(text, r"attribute \"scaleColor\"")

    def test_scales_presets_render_and_spikes_follow_their_strands(self):
        # The scales presets driven by guides and flow, rendered by kick (the pictures' paths
        # printed); Swept spikes' plates bend along the guides out past the sphere (follow strand),
        # covering more of the frame than the same plates flat on the skin.
        from groomlab import presets
        for sky in cmds.ls(type="aiSkyDomeLight"):
            cmds.setAttr(sky + ".camera", 0)  # a black background
        cmds.setAttr("persp.translate", 0, 0, 7)
        cmds.setAttr("persp.rotate", 0, 0, 0)
        covered = {}
        for name in ("scales_from_guides", "scales_from_flow", "swept_spikes"):
            presets.apply_preset(self.groom, name)
            self.assertEqual(groomlab.errors(self.groom), [], name)
            pixels = self.render(name, kind="scales")
            covered[name] = sum(1 for p in pixels if max(p) > 0.002)
            print("scales preset rendered:", os.path.splitext(ASS_PATH)[0] + "_scales_%s.png" % name)
        with open(os.path.splitext(ASS_PATH)[0] + "_scales_swept_spikes.ass") as f:
            self.assertRegex(f.read(), r"\npolymesh\s*\n\{\s*\n\s*name\s+\S*scales\b")
        groomlab.set_param(self.groom, "spikes", "follow", "skin")
        flat = sum(1 for p in self.render("swept_spikes_flat", kind="scales") if max(p) > 0.002)
        self.assertGreater(covered["swept_spikes"], 1.15 * flat, covered)
        self.assertGreater(min(covered.values()), 3000, covered)

    def test_alembic_guides_render(self):
        # The translator DLL has its own operator registry; ImportCurves must be in it.
        guides_path = os.path.splitext(ASS_PATH)[0] + "_guides.abc"
        cmds.loadPlugin("AbcExport", quiet=True)
        curve = cmds.curve(degree=1, point=[(0, 1, 0), (0.6, 1.5, 0.2)], name="abcGuide")
        cmds.AbcExport(j="-frameRange 1 1 -root |abcGuide -file " + guides_path.replace("\\", "/"))

        graph = groomlab.get_graph(self.groom)
        graph["nodes"] = [n for n in graph["nodes"] if n["name"] != "guides"]
        graph["nodes"].append({"name": "guides", "type": "ImportCurves", "params": {"path": guides_path}})
        graph["connections"].append({"from": "scalp", "to": "guides", "input": "scalp"})
        groomlab.set_graph(self.groom, graph)

        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True)
        node = read_curves_node(ASS_PATH)
        self.assertGreater(node["curves"], 95000)

    def test_shading_group_assignment_wins(self):
        shader = cmds.shadingNode("aiStandardHair", asShader=True, name="testHair")
        sg = cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name="testHairSG")
        cmds.connectAttr(shader + ".outColor", sg + ".surfaceShader")
        groomlab.assign_shading_group(self.groom, sg)

        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True)
        node = read_curves_node(ASS_PATH)
        self.assertIn("testHair", node["shader"])

    def test_without_a_shader_the_groom_renders_its_own_colors(self):
        # No shading group: the translator's default standard_hair is made from the groom's look,
        # the pigment (melanin user data) for natural hair, the RGB (hair_color) for a picked color,
        # not a fixed brown.
        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True)
        with open(ASS_PATH) as f:
            text = f.read()
        self.assertIn("standard_hair", text)
        self.assertRegex(text, r'user_data_float\s*\{[^}]*attribute "melanin"')
        groomlab.set_graph(self.groom, with_hair_color(groomlab.get_graph(self.groom), "color", {}))
        groomlab.set_param(self.groom, "color", "mode", "color")
        groomlab.set_param(self.groom, "color", "color", [1.0, 1.0, 1.0])
        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True)
        with open(ASS_PATH) as f:
            text = f.read()
        self.assertRegex(text, r'user_data_rgb\s*\{[^}]*attribute "hair_color"')
        self.assertNotRegex(text, r'user_data_float\s*\{[^}]*attribute "melanin"')

    def test_a_made_hair_shader_follows_the_look(self):
        # create_hair_shader's network is made for the look of its time: after HairColor's highlight
        # changed it is made again (sync_hair_shader, which the Graph Editor and the Attribute
        # Editor's Hair Color section call after an edit). Its color reads HairColor's RGB in
        # either mode, so a mode change alone needs no new network.
        groomlab.set_graph(self.groom, with_hair_color(groomlab.get_graph(self.groom), "color", {}))
        shader = groomlab.create_hair_shader(self.groom, name="followHair")
        source = base_color_sources(shader)
        self.assertTrue(source and cmds.getAttr(source[0] + ".attribute") == "hair_color", source)
        self.assertIsNone(groomlab.sync_hair_shader(self.groom), "the look as made: nothing to do")
        groomlab.set_param(self.groom, "color", "mode", "color")
        self.assertIsNone(groomlab.sync_hair_shader(self.groom), "the same RGB network")
        groomlab.set_param(self.groom, "color", "specular", 0.15)
        made = groomlab.sync_hair_shader(self.groom)
        self.assertTrue(made)
        self.assertAlmostEqual(cmds.getAttr(made + ".specularTint")[0][0], 0.5, places=5)
        self.assertEqual(cmds.getAttr(made + ".melanin"), 0.0)
        self.assertEqual(len(cmds.ls(type="aiStandardHair")), 1, "the old shader went")
        # A shader of the user's own is left alone.
        own = cmds.shadingNode("aiStandardHair", asShader=True, name="ownHair")
        sg = cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name="ownHairSG")
        cmds.connectAttr(own + ".outColor", sg + ".surfaceShader")
        groomlab.assign_shading_group(self.groom, sg)
        groomlab.set_param(self.groom, "color", "mode", "melanin")
        self.assertIsNone(groomlab.sync_hair_shader(self.groom))

    def test_lod_and_camera_culling(self):
        cmds.setAttr("persp.translate", 0, 0, 4)
        cmds.setAttr("persp.rotate", 0, 0, 0)
        cmds.setAttr("perspShape.focalLength", 150)  # a close-up: part of the groom in view
        cmds.setAttr(self.groom + ".cameraCull", True)
        cmds.setAttr(self.groom + ".cullMargin", 0.0)
        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True, camera="perspShape")
        culled = read_curves_node(ASS_PATH)["curves"]
        self.assertGreater(culled, 100)
        self.assertLess(culled, 40000)

        cmds.setAttr(self.groom + ".cameraCull", False)
        cmds.setAttr(self.groom + ".lodDistance", True)
        cmds.setAttr(self.groom + ".lodNear", 2.5)
        cmds.setAttr(self.groom + ".lodFar", 5.0)
        cmds.setAttr(self.groom + ".lodMinDensity", 0.25)
        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True, camera="perspShape")
        thinned = read_curves_node(ASS_PATH)
        self.assertLess(thinned["curves"], 70000)  # the far side thinned
        self.assertGreater(thinned["curves"], 30000)
        self.assertEqual(thinned["points"], thinned["curves"] * 16)

    def test_backface_culling_drops_the_far_side(self):
        # Strands rooted facing away from the camera (past side-on by more than the padding) are
        # dropped; more padding keeps more of them.
        cmds.setAttr("persp.translate", 0, 0, 6)
        cmds.setAttr("persp.rotate", 0, 0, 0)
        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True, camera="perspShape")
        full = read_curves_node(ASS_PATH)["curves"]
        cmds.setAttr(self.groom + ".backfaceCull", True)
        cmds.setAttr(self.groom + ".backfaceAngle", 0.0)
        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True, camera="perspShape")
        culled = read_curves_node(ASS_PATH)
        self.assertLess(culled["curves"], full * 0.65)
        self.assertGreater(culled["curves"], full * 0.35)
        self.assertEqual(culled["points"], culled["curves"] * 16)
        cmds.setAttr(self.groom + ".backfaceAngle", 45.0)
        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True, camera="perspShape")
        padded = read_curves_node(ASS_PATH)["curves"]
        self.assertGreater(padded, culled["curves"])
        self.assertLess(padded, full)

    def test_render_data_reaches_arnold_and_the_hair_shader(self):
        # RenderData's opacity per point (varying user data, the curves no longer opaque) and
        # HairColor's highlight: its specular, roughness and tilt per strand (uniform user data:
        # RenderMan's procedural reads them too); create_hair_shader reads the specular and
        # roughness where they vary per strand (here both: their randomness), the node's own values
        # for the rest, and gives the standard_hair the tilt as its shift.
        graph = with_hair_color(self.use_density(150), "look", {
            "specular": 0.45, "specularRandom": 0.2, "roughness": 0.7, "roughnessRandom": 0.3, "tilt": 6.0})
        graph = with_hair_color(graph, "opacity", {"opacityProfile": [[0, 1], [1, 0.25]]}, node_type="RenderData")
        groomlab.set_graph(self.groom, graph)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(groomlab.render_data_node(self.groom), "opacity")
        self.assertEqual(groomlab.hair_look(self.groom), {"specular": 0.45, "roughness": 0.7, "tilt": 6.0})
        hair = groomlab.create_hair_shader(self.groom, name="dataHair")
        self.assertAlmostEqual(cmds.getAttr(hair + ".shift"), 6.0, places=5)
        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True)
        node = read_curves_node(ASS_PATH)
        self.assertIn(hair, node["shader"])
        self.assertRegex(block(node["text"], "standard_hair"), r"\n\s*shift\s+6\b")
        body = block(node["text"], "curves")
        self.assertRegex(body, r"declare opacity varying FLOAT")
        for channel in ("specular", "roughness", "tilt"):
            self.assertRegex(body, r"declare %s uniform FLOAT" % channel)
        _, _, tilts = array_values(body, "tilt")
        self.assertTrue(all(abs(v - 6.0) < 1e-5 for v in tilts), tilts[:5])
        count, _, values = array_values(body, "opacity")
        self.assertEqual(count, node["points"])
        self.assertAlmostEqual(min(values), 0.25, places=4)
        self.assertAlmostEqual(max(values), 1.0, places=4)
        self.assertRegex(body, r"\n\s*opaque\s+off")
        _, _, specular = array_values(body, "specular")
        self.assertTrue(all(0.45 * 0.8 - 1e-5 <= v <= 0.45 * 1.2 + 1e-5 for v in specular), specular[:5])
        self.assertGreater(len(set(specular)), 10)
        # The shader network: user data for each channel that varies per strand.
        readers = re.findall(r"\nuser_data_float\s*\n\{(.*?)\n\}", node["text"], re.S)
        attributes = {re.search(r'attribute "(\w+)"', r).group(1) for r in readers}
        self.assertTrue({"opacity", "specular", "roughness"} <= attributes, attributes)
        self.assertNotIn("tilt", attributes)  # one shift per shader
        self.assertNotIn("hueShift", node["text"])  # (the one color_correct: Arnold's gamma)
        opacity = cmds.listConnections(hair + ".opacityR", source=True, destination=False)
        self.assertEqual(cmds.getAttr(opacity[0] + ".attribute"), "opacity")
        self.assertEqual(cmds.nodeType(cmds.listConnections(hair + ".specularTint", source=True, destination=False)[0]),
                         "clamp")
        self.assertEqual(cmds.nodeType(cmds.listConnections(hair + ".roughness", source=True, destination=False)[0]),
                         "plusMinusAverage")
        # Without randomness or a map, the node's values on the shader itself.
        groomlab.set_param(self.groom, "look", "specularRandom", 0.0)
        groomlab.set_param(self.groom, "look", "roughnessRandom", 0.0)
        plain = groomlab.create_hair_shader(self.groom, name="plainDataHair")
        self.assertFalse(cmds.listConnections(plain + ".specularTint", source=True, destination=False))
        self.assertAlmostEqual(cmds.getAttr(plain + ".specularTint")[0][0], 1.0, places=5)  # 0.45 / 0.3, up to 1
        self.assertAlmostEqual(cmds.getAttr(plain + ".roughness"), 0.1 + 0.4 * 0.7, places=5)
        self.assertAlmostEqual(cmds.getAttr(plain + ".shift"), 6.0, places=5)
        # Another HairColor making the output that sets the highlight apart (merged grooms: each
        # strand the value of the HairColor that colored it last): the shader reads it per strand.
        graph = with_hair_color(groomlab.get_graph(self.groom), "dull", {"specular": 0.1, "roughness": 0.7}, before="look")
        groomlab.set_graph(self.groom, graph)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(groomlab.shading.highlight_channels(groomlab.shading.hair_look_json(self.groom)), ["specular"])
        both = groomlab.create_hair_shader(self.groom, name="bothDataHair")
        self.assertEqual(cmds.nodeType(cmds.listConnections(both + ".specularTint", source=True, destination=False)[0]),
                         "clamp")
        self.assertFalse(cmds.listConnections(both + ".roughness", source=True, destination=False))

    def test_wet_hair_reaches_arnold_and_the_hair_shader(self):
        # Wet's wetness: varying user data per point, which create_hair_shader's wet network reads
        # (core's wet_shading: the base color to a power, melanin up, the specular tint up, the
        # roughness down), in front of what drove each of them; dry grooms get none.
        graph = with_hair_color(self.use_density(150), "color", {"melanin": 0.3})
        graph = with_hair_color(graph, "wet", {"wetness": 0.6}, node_type="Wet")
        groomlab.set_graph(self.groom, graph)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertTrue(groomlab.shading.hair_look_json(self.groom).get("wet"))
        hair = groomlab.create_hair_shader(self.groom, name="wetHair")
        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True)
        node = read_curves_node(ASS_PATH)
        self.assertIn(hair, node["shader"])
        body = block(node["text"], "curves")
        self.assertRegex(body, r"declare wetness varying FLOAT")
        count, _, values = array_values(body, "wetness")
        self.assertEqual(count, node["points"])
        self.assertTrue(all(abs(v - 0.6) < 1e-5 for v in values), values[:5])
        readers = re.findall(r"\nuser_data_float\s*\n\{(.*?)\n\}", node["text"], re.S)
        self.assertIn("wetness", {re.search(r'attribute "(\w+)"', r).group(1) for r in readers})
        for attr, wet in (("baseColor", "wetHair_wetBase"), ("specularTint", "wetHair_wetSpecular"),
                          ("roughness", "wetHair_wetRoughness")):
            self.assertEqual(cmds.listConnections(hair + "." + attr, source=True, destination=False), [wet], attr)
        # The dry look behind them, as it was: HairColor's RGB (over no melanin), the lobes' tint
        # and roughness.
        base = cmds.listConnections("wetHair_wetBase.input1", source=True, destination=False)
        self.assertEqual(cmds.nodeType(base[0]), "aiColorCorrect")  # Arnold's gamma
        base = cmds.listConnections(base[0] + ".input", source=True, destination=False)
        self.assertEqual(cmds.getAttr(base[0] + ".attribute"), "hair_color")
        self.assertAlmostEqual(cmds.getAttr("wetHair_wetRoughness.input1X"), 0.3, places=5)
        self.assertAlmostEqual(cmds.getAttr("wetHair_wetSpecular.input1")[0][0], 1.0, places=5)
        shares = groomlab_core.wet_shading(1.0)
        factors = cmds.getAttr("wetHair_wetFactors.input2")[0]
        for got, want in zip(factors, (shares["absorption"], shares["specular"], shares["roughness"])):
            self.assertAlmostEqual(got, want - 1.0, places=5)
        self.assertEqual(cmds.getAttr("wetHair_wetBase.operation"), 3)  # the base color to a power
        for shader in ("\nuser_data_float", "\nmultiply", "\npow", "\nlayer_rgba"):  # (MtoA's for the Maya nodes)
            self.assertIn(shader, node["text"])
        # Dry: no wetness user data, no wet network.
        groomlab.set_graph(self.groom, with_hair_color(self.use_density(150), "color", {"melanin": 0.3}))
        self.assertFalse(groomlab.shading.hair_look_json(self.groom).get("wet"))
        dry = groomlab.create_hair_shader(self.groom, name="dryHair")
        self.assertFalse(cmds.objExists(dry + "_wetFactors"))
        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True)
        self.assertNotRegex(block(read_curves_node(ASS_PATH)["text"], "curves"), r"declare wetness")

    def test_hair_color_channels_drive_the_hair_shader(self):
        graph = groomlab.get_graph(self.groom)
        graph["nodes"].insert(-1, {"name": "color", "type": "HairColor",
                                   "params": {"melanin": 0.6, "melaninRandom": 0.2, "specular": 0.15, "roughness": 1.0}})
        graph["connections"] = [c for c in graph["connections"] if c["to"] != "output"]
        graph["connections"] += [{"from": "resample", "to": "color", "input": "strands"},
                                 {"from": "color", "to": "output", "input": "strands"}]
        groomlab.set_graph(self.groom, graph)
        hair = groomlab.create_hair_shader(self.groom, name="shotHair")
        self.assertEqual(groomlab.errors(self.groom), [])
        # HairColor's highlight: half the default strength is half Arnold's specular; the
        # broadest spread its roughness range's top; the default tilt Arnold's shift.
        self.assertEqual(groomlab.hair_look(self.groom), {"specular": 0.15, "roughness": 1.0, "tilt": 3.0})
        self.assertAlmostEqual(cmds.getAttr(hair + ".specularTint")[0][0], 0.5, places=5)
        self.assertAlmostEqual(cmds.getAttr(hair + ".roughness"), 0.5, places=5)
        self.assertAlmostEqual(cmds.getAttr(hair + ".shift"), 3.0, places=5)

        # The color: HairColor's RGB as the base color over no melanin, in mode melanin too (the
        # color whose absorption is the melanin's, with each strand's randomness in it).
        self.assertEqual(cmds.getAttr(hair + ".melanin"), 0.0)
        source = base_color_sources(hair)
        self.assertEqual([cmds.nodeType(n) for n in source], ["aiUserDataColor"])
        self.assertEqual(cmds.getAttr(source[0] + ".attribute"), "hair_color")
        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True)
        self.assertRegex(read_curves_node(ASS_PATH)["text"], r"\ncolor_correct\s*\n\{[^}]*gamma 1\.08")
        node = read_curves_node(ASS_PATH)
        self.assertIn(hair, node["shader"])
        body = block(node["text"], "curves")
        self.assertRegex(body, r"declare hair_color uniform RGB")
        self.assertRegex(body, r"declare melanin uniform FLOAT")  # as data
        count, _, colors = array_values(body, "hair_color", 3)
        greens = set(colors[1::3])
        self.assertGreater(len(greens), 1000)  # melanin randomness: strands apart

        # Hue and saturation randomness reach the render: each strand's RGB turned and scaled, the
        # same shader network (no rebuild needed for the look's kind).
        graph = groomlab.get_graph(self.groom)
        next(n for n in graph["nodes"] if n["name"] == "color")["params"].update(
            {"melaninRandom": 0.0, "hueRandom": 0.6, "saturationRandom": 0.5})
        groomlab.set_graph(self.groom, graph)
        rgb = groomlab.create_hair_shader(self.groom, name="rgbHair")
        self.assertEqual(cmds.getAttr(rgb + ".melanin"), 0.0)
        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True)
        body = block(read_curves_node(ASS_PATH)["text"], "curves")
        count, _, colors = array_values(body, "hair_color", 3)
        ratios = set(round(colors[3 * i] / max(colors[3 * i + 2], 1e-6), 1) for i in range(count))
        self.assertGreater(len(ratios), 10)  # hues apart, not only brighter or darker
        for gone in ("hueShift", "saturationScale", "valueScale", "dyeAmount"):
            self.assertNotRegex(body, r"declare %s " % gone)


    def use_density(self, density):
        graph = groomlab.get_graph(self.groom)
        next(n for n in graph["nodes"] if n["name"] == "scatter")["params"]["density"] = density
        return graph

    def test_a_tip_color_renders_per_point(self):
        # HairColor blending toward a tip color: hair_color is varying, a color per point of the
        # curves (after LOD), from the strand's own color at the root to the tip color.
        graph = with_hair_color(self.use_density(150), "color", {
            "melanin": 0.95, "tipColor": [0.9, 0.9, 0.9], "tipStrength": 1.0})
        groomlab.set_graph(self.groom, graph)
        self.assertEqual(groomlab.errors(self.groom), [])
        cmds.setAttr(self.groom + ".lodDistance", True)  # thinned: the colors follow the kept strands
        cmds.setAttr(self.groom + ".lodNear", 0.0)
        cmds.setAttr(self.groom + ".lodFar", 0.01)
        cmds.setAttr(self.groom + ".lodMinDensity", 0.5)
        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True)
        node = read_curves_node(ASS_PATH)
        color = hair_color(node["text"])
        self.assertEqual(color["category"], "varying")
        self.assertGreater(len(color["counts"]), 300)
        self.assertEqual(color["count"], node["points"])
        self.assertEqual(color["count"], sum(color["counts"]))
        self.assertEqual(len(color["strand"]), len(color["counts"]))
        at = 0
        for n, own in zip(color["counts"], color["strand"]):
            root, tip = color["colors"][at], color["colors"][at + n - 1]
            for k in range(3):
                self.assertAlmostEqual(root[k], own[k], places=4)  # the strand's color at the root
                self.assertAlmostEqual(tip[k], 0.9, places=4)      # the tip color at the tip
            at += n

        # Without a tip blend: one color per strand, as before.
        groomlab.set_param(self.groom, "color", "tipStrength", 0.0)
        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True)
        node = read_curves_node(ASS_PATH)
        color = hair_color(node["text"])
        self.assertEqual(color["category"], "uniform")
        self.assertEqual(color["count"], node["curves"])
        for got, own in zip(color["colors"], color["strand"]):
            for k in range(3):
                self.assertAlmostEqual(got[k], own[k], places=4)

    def test_bands_and_iridescence_render(self):
        # Bands after HairColor: hair_color per point. FeatherColor's iridescence: the iridescence
        # user data per point and each curve's root_normal, which create_hair_shader's network reads
        # (the angle to the camera ray picks the structural color off a ramp, the iridescence mixes
        # it in).
        graph = with_hair_color(self.use_density(150), "color", {"melanin": 0.8})
        graph = with_hair_color(graph, "bands", {"count": 2}, node_type="Bands")
        graph = with_hair_color(graph, "sheen", {"pattern": "none", "iridescence": 0.75,
                                                 "iridescenceColor": [0.1, 0.2, 0.9], "iridescenceShift": 0.25},
                                node_type="FeatherColor")
        groomlab.set_graph(self.groom, graph)
        self.assertEqual(groomlab.errors(self.groom), [])
        hair = groomlab.create_hair_shader(self.groom, name="iridescentHair")
        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True)
        node = read_curves_node(ASS_PATH)
        color = hair_color(node["text"])
        self.assertEqual(color["category"], "varying")  # banded: a color a point
        self.assertEqual(color["count"], node["points"])
        self.assertGreater(len(set(color["colors"])), len(color["counts"]))  # more colors than curves
        body = block(node["text"], "curves")
        self.assertRegex(body, r"declare iridescence varying FLOAT")
        self.assertRegex(body, r"declare root_normal uniform RGB")
        count, _, values = array_values(body, "iridescence")
        self.assertEqual(count, node["points"])
        self.assertTrue(all(abs(v - 0.75) < 1e-5 for v in values))
        self.assertEqual(array_values(body, "root_normal", 3)[0], node["curves"])
        # The network: the base color and the specular tint mixed by the iridescence.
        base = base_color_sources(hair)
        self.assertEqual(cmds.nodeType(base[0]), "blendColors")
        blender = cmds.listConnections(base[0] + ".blender", source=True, destination=False)
        self.assertEqual(cmds.getAttr(blender[0] + ".attribute"), "iridescence")
        ramp = cmds.listConnections(base[0] + ".color1", source=True, destination=False)[0]
        self.assertEqual(cmds.getAttr(ramp + ".ramp", size=True), 5)
        facing = cmds.getAttr(ramp + ".ramp[4].ramp_Color")[0]  # facing the camera: the color itself
        for got, want in zip(facing, (0.1, 0.2, 0.9)):
            self.assertAlmostEqual(got, want, places=4)
        edge = cmds.getAttr(ramp + ".ramp[0].ramp_Color")[0]  # edge-on: its hue a quarter turn on
        for got, want in zip(edge, groomlab.iridescent_tint((0.1, 0.2, 0.9), 0.25, 1.0)):
            self.assertAlmostEqual(got, want, places=4)
        self.assertTrue(cmds.listConnections(hair + ".specularTint", source=True, destination=False))
        # Rendered: the shading network evaluates (no Arnold errors, the hair shows).
        text = open(ASS_PATH).read()
        for shader in ("ramp_rgb", "state_vector", "user_data_float", "dot", "abs"):
            self.assertIn(shader, text)

    def test_merged_plain_strands_keep_their_own_color_per_point(self):
        # A Merge of a plain look (first: the main HairColor) and a tipped one: the plain strands'
        # points carry their own color, not HairColor's default, and the shader reads the RGB.
        graph = self.use_density(150)
        graph["nodes"].insert(-1, {"name": "plain", "type": "HairColor", "params": {"melanin": 0.05}})
        graph["nodes"].insert(-1, {"name": "tipped", "type": "HairColor", "params": {
            "melanin": 0.95, "tipColor": [0.9, 0.9, 0.9], "tipStrength": 1.0}})
        graph["nodes"].insert(-1, {"name": "merge", "type": "Merge"})
        graph["connections"] = [c for c in graph["connections"] if c["to"] != "output"]
        graph["connections"] += [{"from": "resample", "to": "plain", "input": "strands"},
                                 {"from": "resample", "to": "tipped", "input": "strands"},
                                 {"from": "plain", "to": "merge", "input": "a"},
                                 {"from": "tipped", "to": "merge", "input": "b"},
                                 {"from": "merge", "to": "output", "input": "strands"}]
        groomlab.set_graph(self.groom, graph)
        self.assertEqual(groomlab.errors(self.groom), [])
        stats = json.loads(cmds.groomLabGraphStats(self.groom))
        self.assertEqual(stats["hairColor"], "plain")
        self.assertEqual(stats["colorNodes"], ["plain", "tipped"])
        hair = groomlab.create_hair_shader(self.groom, name="mergedHair")
        source = base_color_sources(hair)
        self.assertEqual([cmds.nodeType(n) for n in source], ["aiUserDataColor"])  # the tipped branch's RGB

        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True)
        node = read_curves_node(ASS_PATH)
        color = hair_color(node["text"])
        self.assertEqual(color["category"], "varying")
        self.assertEqual(color["count"], node["points"])
        flat = tipped = 0
        at = 0
        for n, own in zip(color["counts"], color["strand"]):
            points = color["colors"][at:at + n]
            at += n
            for k in range(3):
                self.assertAlmostEqual(points[0][k], own[k], places=4)  # every root: its strand's color
            if all(abs(p[k] - points[0][k]) < 1e-4 for p in points for k in range(3)):
                flat += 1
                self.assertGreater(own[0], 3.0 * groomlab_core.DEFAULT_HAIR_COLOR[0])  # blond, not the default brown
            else:
                tipped += 1
        self.assertGreater(flat, 300)
        self.assertEqual(flat, tipped)  # the plain half is one color along each strand

    def test_a_blend_of_two_colors_renders_its_rgb(self):
        # Two plain looks (pigment only, each alone renders by melanin) mixed by a Blend toward the
        # ends: the shader reads the RGB, per point, dark at the roots and light at the tips.
        graph = self.use_density(150)
        plain = {"melaninRandom": 0.0}
        graph["nodes"].insert(-1, {"name": "dark", "type": "HairColor", "params": dict(plain, melanin=0.95)})
        graph["nodes"].insert(-1, {"name": "light", "type": "HairColor", "params": dict(plain, melanin=0.05)})
        graph["nodes"].insert(-1, {"name": "blend", "type": "Blend", "params": {"mix": 1.0, "profile": [[0, 0], [1, 1]]}})
        graph["connections"] = [c for c in graph["connections"] if c["to"] != "output"]
        graph["connections"] += [{"from": "resample", "to": "dark", "input": "strands"},
                                 {"from": "resample", "to": "light", "input": "strands"},
                                 {"from": "dark", "to": "blend", "input": "a"},
                                 {"from": "light", "to": "blend", "input": "b"},
                                 {"from": "blend", "to": "output", "input": "strands"}]
        groomlab.set_graph(self.groom, graph)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(json.loads(cmds.groomLabGraphStats(self.groom))["colorBlends"], ["blend"])
        hair = groomlab.create_hair_shader(self.groom, name="blendHair")
        source = base_color_sources(hair)
        self.assertEqual([cmds.nodeType(n) for n in source], ["aiUserDataColor"])

        cmds.arnoldExportAss(filename=ASS_PATH, asciiAss=True)
        node = read_curves_node(ASS_PATH)
        color = hair_color(node["text"])
        self.assertEqual(color["category"], "varying")
        self.assertEqual(color["count"], node["points"])
        root, tip = color["colors"][0], color["colors"][color["counts"][0] - 1]
        self.assertGreater(tip[0], root[0] + 0.2)

        # Shape only: the look is the dark branch's alone, rendered by its HairColor's RGB.
        groomlab.set_param(self.groom, "blend", "include", "shape")
        self.assertEqual(json.loads(cmds.groomLabGraphStats(self.groom))["colorBlends"], [])
        shape = groomlab.create_hair_shader(self.groom, name="shapeHair")
        source = base_color_sources(shape)
        self.assertEqual([cmds.getAttr(n + ".attribute") for n in source], ["hair_color"])

    def render(self, name, kind):
        """The scene rendered by kick (MtoA's .ass, a PNG beside ASS_PATH: <stem>_<kind>_<name>.png):
        its pixels as [(r, g, b)], linear."""
        import mtoa.core
        mtoa.core.createOptions()
        cmds.setAttr("defaultArnoldDriver.ai_translator", "png", type="string")
        cmds.setAttr("defaultArnoldRenderOptions.AASamples", 4)
        scene = os.path.splitext(ASS_PATH)[0] + "_%s_%s.ass" % (kind, name)
        image = os.path.splitext(ASS_PATH)[0] + "_%s_%s.png" % (kind, name)
        for path in (scene, image):
            if os.path.exists(path):
                os.remove(path)
        cmds.arnoldExportAss(filename=scene, asciiAss=True)
        mtoa = os.path.dirname(os.path.dirname(cmds.pluginInfo("mtoa", query=True, path=True)))
        command = [os.path.join(mtoa, "bin", "kick"), "-i", scene, "-l", os.path.join(mtoa, "shaders"),
                   "-o", image, "-r", "320", "240", "-dw", "-dp", "-sl", "-v", "1"]
        for attempt in range(2):  # kick now and then stalls at startup (see test_groom_file_mode_renders_with_kick)
            try:
                result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, universal_newlines=True, timeout=120)
                break
            except subprocess.TimeoutExpired:
                if attempt == 1:
                    self.fail("kick stalled twice rendering " + name)
        self.assertEqual(result.returncode, 0, result.stdout[-3000:])
        from groomlab.ui.qt import QtGui
        picture = QtGui.QImage(image)
        self.assertEqual((picture.width(), picture.height()), (320, 240), image)
        linear = [c / 255.0 / 12.92 if c <= 10 else ((c / 255.0 + 0.055) / 1.055) ** 2.4 for c in range(256)]
        pixels = []
        for y in range(picture.height()):
            for x in range(picture.width()):
                p = picture.pixel(x, y)
                pixels.append((linear[(p >> 16) & 255], linear[(p >> 8) & 255], linear[p & 255]))
        return pixels


class TurntableRenderTest(unittest.TestCase):
    """Render Turntable: Arnold renders the saved scene in the background, into groomlab/renders."""

    def test_renders_the_frames_in_the_background(self):
        # The render process finds the build's plugin and scripts, as an installed module would give it.
        os.environ["MAYA_PLUG_IN_PATH"] = os.pathsep.join(
            p for p in [os.path.dirname(PLUGIN_PATH), os.environ.get("MAYA_PLUG_IN_PATH", "")] if p)
        os.environ["PYTHONPATH"] = os.pathsep.join(p for p in [SCRIPTS_DIR, os.environ.get("PYTHONPATH", "")] if p)
        if not cmds.pluginInfo(PLUGIN_PATH, query=True, loaded=True):
            cmds.loadPlugin(PLUGIN_PATH)
        cmds.loadPlugin("mtoa")
        cmds.file(new=True, force=True)
        home = tempfile.mkdtemp(prefix="groomlab_turntable_")
        cmds.file(rename=os.path.join(home, "shot.ma"))
        cmds.file(save=True, type="mayaAscii")
        groom = groomlab.create_groom(cmds.polySphere(radius=1)[0])
        job = groomlab.render_turntable(groom, frames=2, width=64, height=64)
        written, total, done = job.wait(timeout=600)
        with open(job.log) as f:
            log = f.read()
        self.assertTrue(job.ok(), log[-3000:])
        self.assertEqual((written, total), (2, 2))
        self.assertIn("/groomlab/shot/renders/turntable_", job.folder)  # the scene's own folder
        self.assertTrue(job.folder.startswith(home.replace("\\", "/")), job.folder)
        self.assertTrue(cmds.ls("groomTurntable_cam*"))  # the rig, in the scene
        from groomlab.ui.qt import QtGui
        image = QtGui.QImage(job.rendered()[0])
        self.assertEqual((image.width(), image.height()), (64, 64))
        brightest = max(QtGui.QColor(image.pixel(x, y)).value() for x in range(0, 64, 4) for y in range(0, 64, 4))
        self.assertGreater(brightest, 10, "the frame is black")

        # Stop: the render and what it started end; no more frames arrive.
        import time
        long_job = groomlab.render_turntable(groom, frames=200, width=64, height=64)
        time.sleep(8)
        long_job.cancel()
        self.assertTrue(long_job.poll()[2])
        time.sleep(1)
        count = len(long_job.rendered())
        time.sleep(6)
        self.assertEqual(len(long_job.rendered()), count, "frames kept coming after Stop")
        self.assertIsNotNone(long_job.process.poll())


def points_keys(text):
    """(key count, [first key values, second key values]) of the first curves node's points."""
    body = block(text, "curves")
    m = re.search(r"\n\s*points\s+(\d+)\s+(\d+)\s+VECTOR\s", body)
    count, keys = int(m.group(1)), int(m.group(2))
    values = body[m.end():].split(None, 2 * 3 * count + 1)
    floats = [float(v) for v in values[:2 * 3 * count]] if keys > 1 else []
    return keys, floats[:3 * count], floats[3 * count:]


def instance_matrix_keys(instancer):
    """(instances, keys, every key's matrices as one flat list of floats) of an instancer block."""
    m = re.search(r"\n\s*instance_matrix\s+(\d+)\s+(\d+)\s+MATRIX\s", instancer)
    count, keys = int(m.group(1)), int(m.group(2))
    values = instancer[m.end():].split(None, 16 * count * keys)
    return count, keys, [float(v) for v in values[:16 * count * keys]]


def base_color_sources(hair):
    """The nodes driving `hair`.baseColor, past create_hair_shader's aiColorCorrect that hands
    Arnold the RGB to the power 1 / ARNOLD_BASE_COLOR_GAMMA (whose gamma is checked here)."""
    source = cmds.listConnections(hair + ".baseColor", source=True, destination=False) or []
    if source and cmds.nodeType(source[0]) == "aiColorCorrect" and source[0].endswith("_arnoldColor"):
        assert abs(cmds.getAttr(source[0] + ".gamma") - groomlab_core.ARNOLD_BASE_COLOR_GAMMA) < 1e-6
        source = cmds.listConnections(source[0] + ".input", source=True, destination=False) or []
    return source


def block(text, node_type):
    m = re.search(r"\n" + node_type + r"\s*\n\{(.*?)\n\}", text, re.S)
    return m.group(1) if m else None


def array_values(body, name, width=1):
    """(count, keys, values as floats) of an array parameter in a node's block, None when absent."""
    m = re.search(r"\n\s*" + name + r"\s+(\d+)\s+(\d+)\s+(\w+)\s", body)
    if not m:
        return None
    count, keys = int(m.group(1)), int(m.group(2))
    total = count * keys * width
    return count, keys, [float(v) for v in body[m.end():].split(None, total)[:total]]


def hair_color(text):
    """The first curves node's num_points, hair_color's declaration ("uniform" or "varying"), its
    count and colors as (r, g, b), and the strand channels colorR/G/B per curve as (r, g, b)."""
    body = block(text, "curves")
    declared = re.search(r"declare hair_color (\w+) RGB", body)
    colors = array_values(body, "hair_color", 3)
    rgb = colors[2] if colors else []
    strand = [array_values(body, c)[2] for c in ("colorR", "colorG", "colorB")]
    return {"counts": [int(n) for n in array_values(body, "num_points")[2]],
            "category": declared.group(1) if declared else None,
            "count": colors[0] if colors else 0,
            "colors": [tuple(rgb[i:i + 3]) for i in range(0, len(rgb), 3)],
            "strand": list(zip(*strand))}


def with_hair_color(graph, name, params, before="output", node_type="HairColor"):
    """`graph` with a HairColor node (or one of `node_type`) `name` between `before` and what fed its strands."""
    feed = next(c for c in graph["connections"] if c["to"] == before and c["input"] == "strands")
    graph["nodes"].insert(-1, {"name": name, "type": node_type, "params": params})
    graph["connections"].append({"from": feed["from"], "to": name, "input": "strands"})
    feed["from"] = name
    return graph


class MotionAndProceduralTest(unittest.TestCase):
    """A squashing scalp, so motion keys differ; low density keeps the .ass small."""

    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin("mtoa")
        cmds.loadPlugin(PLUGIN_PATH)

    def setUp(self):
        cmds.file(new=True, force=True)
        cmds.currentUnit(time="film")
        self.scalp = cmds.polySphere(radius=1, subdivisionsX=30, subdivisionsY=16)[0]
        squash = cmds.nonLinear(self.scalp, type="squash")[0]
        cmds.setKeyframe(squash, attribute="factor", time=1, value=0.0)
        cmds.setKeyframe(squash, attribute="factor", time=10, value=0.8)
        self.groom = groomlab.create_groom(self.scalp)  # with the Orig shape as rest mesh
        cmds.currentTime(5)
        cmds.shadingNode("aiSkyDomeLight", asLight=True)
        cmds.viewFit("persp", allObjects=True)

    def enable_motion_blur(self, steps=3):
        import mtoa.core
        mtoa.core.createOptions()  # a new scene has no render options yet
        options = "defaultArnoldRenderOptions"
        blur = [a for a in cmds.listAttr(options) if "mb_" in a or "motion" in a]
        self.assertIn("motion_blur_enable", blur, blur)
        cmds.setAttr(options + ".motion_blur_enable", True)
        cmds.setAttr(options + ".mb_object_deform_enable", True)
        cmds.setAttr(options + ".motion_steps", steps)

    def export(self):
        path = os.path.splitext(ASS_PATH)[0] + "_motion.ass"
        if os.path.exists(path):
            os.remove(path)
        cmds.arnoldExportAss(filename=path, asciiAss=True)  # at the current frame (5)
        with open(path) as f:
            return f.read()

    def test_deformation_blur_keys(self):
        self.enable_motion_blur(3)
        text = self.export()
        keys, first, second = points_keys(text)
        self.assertEqual(keys, 3)
        self.assertEqual(len(first), len(second))
        self.assertNotEqual(first, second)  # the scalp moved during the shutter
        body = block(text, "curves")
        self.assertRegex(body, r"motion_start -0\.\d+")
        self.assertIn("declare strand_id uniform UINT", body)
        self.assertIn("uvs ", body)

    def test_follow_scalp_blurs_the_groom_carried_from_rest(self):
        # Following the scalp: every key carries the one evaluation at rest (the squash moves the
        # roots), the same strands on every key, and no warning about strands changing.
        self.enable_motion_blur(2)
        unfollowed = points_keys(self.export())
        groomlab.set_follow_scalp(self.groom, True)
        text = self.export()
        keys, first, second = points_keys(text)
        self.assertEqual(keys, 2)
        self.assertEqual(len(first), len(unfollowed[1]))
        self.assertNotEqual(first, second)  # carried with the scalp during the shutter
        self.assertNotEqual(first, unfollowed[1])  # the groom at rest, carried (not grown on the squashed scalp)

    def test_no_blur_without_it(self):
        keys, _, _ = points_keys(self.export())
        self.assertEqual(keys, 1)
        # Instances too: one key of their matrices.
        flake = cmds.polySphere(name="flake", radius=1, subdivisionsX=6, subdivisionsY=4)[0]
        groomlab.add_instances(self.groom, flake, probability=0.3, scale=0.02)
        count, keys, _ = instance_matrix_keys(block(self.export(), "instancer"))
        self.assertGreater(count, 10)
        self.assertEqual(keys, 1)

    def test_instances_blur_with_the_hair(self):
        # The squashing scalp moves the strands during the shutter: the instances on them get a
        # key of instance_matrix per motion step, as the curves get points keys.
        flake = cmds.polySphere(name="flake", radius=1, subdivisionsX=6, subdivisionsY=4)[0]
        cmds.setAttr(flake + ".visibility", False)
        groomlab.add_instances(self.groom, flake, probability=0.3, scale=0.02)
        self.enable_motion_blur(3)
        text = self.export()
        self.assertEqual(points_keys(text)[0], 3)
        instancer = block(text, "instancer")
        self.assertIsNotNone(instancer, "no instancer in the .ass")
        count, keys, matrices = instance_matrix_keys(instancer)
        self.assertGreater(count, 10)
        self.assertEqual(keys, 3)
        first, last = matrices[:16 * count], matrices[-16 * count:]
        self.assertNotEqual(first, last)  # they moved with the scalp
        origins = lambda m: [m[16 * i + 12:16 * i + 15] for i in range(count)]
        worst = max(max(abs(a - b) for a, b in zip(p, q)) for p, q in zip(origins(first), origins(last)))
        self.assertGreater(worst, 1e-3)
        self.assertRegex(instancer, r"motion_start -0\.\d+")
        self.assertIn("instance_visibility", instancer)

    def test_fitted_instances_keep_their_per_axis_scale_in_every_key(self):
        # Fitted to their strands (sizeMode fitStrand): each matrix's y row is fitScale times the
        # strand's rest length, its x and z rows aspect of that, in every motion key: the scalp
        # squashes during the shutter, the rest length does not.
        leaf = cmds.polyPlane(name="leaf", width=1, height=1, subdivisionsX=1, subdivisionsY=1)[0]
        cmds.setAttr(leaf + ".visibility", False)
        groomlab.add_instances(self.groom, leaf, probability=0.3, t_range=(0.0, 0.0), scale_random=0.0,
                               scale_by_width=False, size_mode="fitStrand", fit_scale=0.5, aspect=0.25)
        self.enable_motion_blur(3)
        instancer = block(self.export(), "instancer")
        self.assertIsNotNone(instancer, "no instancer in the .ass")
        count, keys, matrices = instance_matrix_keys(instancer)
        self.assertGreater(count, 10)
        self.assertEqual(keys, 3)
        norm = lambda v: math.sqrt(sum(c * c for c in v))
        lengths = []
        for k in range(keys):
            key = []
            for i in range(count):
                m = matrices[16 * (k * count + i):16 * (k * count + i + 1)]
                x, y, z = norm(m[0:3]), norm(m[4:7]), norm(m[8:11])
                self.assertGreater(y, 1e-3)
                self.assertAlmostEqual(x / y, 0.25, places=4)
                self.assertAlmostEqual(z / y, 0.25, places=4)
                key.append(y)
            lengths.append(key)
        for key in lengths[1:]:  # the same rest length in every key
            self.assertLess(max(abs(a - b) for a, b in zip(key, lengths[0])), 1e-4)
        first, last = matrices[:16 * count], matrices[-16 * count:]
        self.assertNotEqual(first, last)  # while they moved with the scalp

    def scales_graph(self, prototype="groomlab:scale"):
        """A few hundred scales on the squashing sphere, laid out by the strands."""
        groomlab.set_graph(self.groom, {
            "nodes": [
                {"name": "scalp", "type": "Scalp"},
                {"name": "scatter", "type": "Scatter", "params": {"density": 30.0, "length": 0.25}},
                {"name": "width", "type": "Width", "params": {"width": 0.12}},
                {"name": "scales", "type": "Scales", "params": {"prototype": prototype}},
                {"name": "output", "type": "Output"},
            ],
            "connections": [
                {"from": "scalp", "to": "scatter", "input": "scalp"},
                {"from": "scatter", "to": "width", "input": "strands"},
                {"from": "width", "to": "scales", "input": "strands"},
                {"from": "scales", "to": "output", "input": "strands"},
            ],
            "output": "output",
        })

    def assert_scales_blur(self, text, keys):
        """The scales' polymesh has `keys` keys of points and normals, moving with the skin."""
        mesh = re.search(r"\npolymesh\s*\n\{(\s*\n\s*name\s+\S*scales\b.*?)\n\}", text, re.S)
        self.assertIsNotNone(mesh, "no scales polymesh")
        body = mesh.group(1)
        count, got, values = array_values(body, "vlist", 3)
        self.assertGreater(count, 1000)
        self.assertEqual(got, keys)
        self.assertEqual(array_values(body, "nlist", 3)[1], keys)
        first, last = values[:3 * count], values[-3 * count:]
        self.assertGreater(max(abs(a - b) for a, b in zip(first, last)), 1e-3)  # the skin moved under them
        self.assertRegex(body, r"motion_start -0\.\d+")
        return count

    def test_scales_blur_with_the_skin(self):
        self.scales_graph()
        self.enable_motion_blur(3)
        text = self.export()
        self.assertEqual(points_keys(text)[0], 3)
        self.assert_scales_blur(text, 3)
        cmds.setAttr("defaultArnoldRenderOptions.motion_blur_enable", False)
        mesh = re.search(r"\npolymesh\s*\n\{(\s*\n\s*name\s+\S*scales\b.*?)\n\}", self.export(), re.S).group(1)
        self.assertEqual(array_values(mesh, "vlist", 3)[1], 1)

    @unittest.skipUnless(PROCEDURALS_DIR, "groomlab_procedural not built")
    def test_procedural_blurs_scales_of_the_groom_files_own_plates(self):
        # A plate of the artist's own: the groom file holds it, so kick builds the scales without
        # the scene's mesh (deleted before the render), with a key per motion key.
        plate = cmds.polyPlane(name="myPlate", width=0.5, height=1, subdivisionsX=2, subdivisionsY=3)[0]
        cmds.rotate(90, 0, 0, plate + ".vtx[*]", pivot=(0, 0, 0), relative=True)
        cmds.move(0, 0.5, 0, plate + ".vtx[*]", relative=True)
        self.scales_graph("myPlate")
        groom_file = os.path.splitext(ASS_PATH)[0] + "_plates.groom"
        groomlab.render_from_groom_file(self.groom, groom_file, start=1, end=10)
        cmds.delete(plate)
        self.enable_motion_blur(3)
        self.export()
        mtoa = os.path.dirname(os.path.dirname(cmds.pluginInfo("mtoa", query=True, path=True)))
        expanded = os.path.splitext(ASS_PATH)[0] + "_plates_expanded.ass"
        command = [os.path.join(mtoa, "bin", "kick"), "-i", os.path.splitext(ASS_PATH)[0] + "_motion.ass",
                   "-l", PROCEDURALS_DIR, "-forceexpand", "-resave", expanded, "-db", "-dw", "-dp", "-sl", "-v", "2"]
        for attempt in range(2):
            try:
                result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, universal_newlines=True, timeout=90)
                break
            except subprocess.TimeoutExpired:
                if attempt == 1:
                    self.fail("kick -resave stalled twice")
        self.assertEqual(result.returncode, 0, result.stdout[-3000:])
        self.assertNotIn("changes between motion keys", result.stdout)
        with open(expanded) as f:
            text = f.read()
        count = self.assert_scales_blur(text, 3)
        import json
        with open(groom_file) as f:
            plate_points = len(json.load(f)["prototypes"]["myPlate"]["points"]) // 3
        self.assertEqual(count % plate_points, 0)  # the plate's own vertices, not quads
        self.assertNotEqual(plate_points, 4)

    @unittest.skipUnless(PROCEDURALS_DIR, "groomlab_procedural not built")
    def test_groom_file_mode_renders_with_kick(self):
        groom_file = os.path.splitext(ASS_PATH)[0] + ".groom"
        # Instances too: the procedural finds the prototype in the scene by name (a visible mesh
        # here, so the .ass holds it).
        cmds.polySphere(name="flake", radius=0.02, subdivisionsX=6, subdivisionsY=4)
        groomlab.add_instances(self.groom, "flake", probability=0.3, scale=1.0)
        # A tip color: per-point hair_color from the procedural too (checked in the expanded scene).
        groomlab.set_graph(self.groom, with_hair_color(groomlab.get_graph(self.groom, expand_default=True), "tipped", {
            "tipColor": [0.9, 0.9, 0.9], "tipStrength": 1.0}))
        written = groomlab.render_from_groom_file(self.groom, groom_file, start=1, end=10)
        self.assertTrue(all(os.path.exists(p) for p in written))
        self.enable_motion_blur(3)
        # Render LOD travels to the procedural: everything past the camera's nose at half density.
        cmds.setAttr(self.groom + ".lodDistance", True)
        cmds.setAttr(self.groom + ".lodNear", 0.0)
        cmds.setAttr(self.groom + ".lodFar", 0.01)
        cmds.setAttr(self.groom + ".lodMinDensity", 0.5)
        text = self.export()
        self.assertTrue(block(text, "curves") is None, "the .ass holds curves")  # no strands in the .ass
        proc = block(text, "groomlab_procedural")
        self.assertIsNotNone(proc)
        self.assertIn(groom_file.replace("\\", "/"), proc.replace("\\", "/"))
        self.assertRegex(proc, r"\n\s*frame 5\b")
        self.assertRegex(proc, r"\n\s*motion_keys 3\b")

        # The farm half: kick renders the .ass without Maya.
        mtoa = os.path.dirname(os.path.dirname(cmds.pluginInfo("mtoa", query=True, path=True)))
        image = os.path.splitext(ASS_PATH)[0] + "_kick.exr"  # kick keeps the driver's format
        command = [os.path.join(mtoa, "bin", "kick"), "-i", os.path.splitext(ASS_PATH)[0] + "_motion.ass",
                   "-l", PROCEDURALS_DIR, "-l", os.path.join(mtoa, "shaders"),
                   "-o", image, "-r", "160", "120", "-as", "1", "-dw", "-dp", "-sl", "-v", "2"]
        # kick renders this in seconds, but now and then stalls at startup (licensing
        # services on this machine); a stalled run is retried once instead of failing.
        for attempt in range(2):
            try:
                result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, universal_newlines=True, timeout=90)
                break
            except subprocess.TimeoutExpired as e:
                stalled = (e.output or "")[-3000:] if isinstance(e.output, str) else repr(e.output)[-3000:]
                if attempt == 1:
                    self.fail("kick stalled twice; its log so far:\n" + stalled)
        self.assertEqual(result.returncode, 0, result.stdout[-3000:])
        self.assertRegex(result.stdout, r"\[groomlab\] \S+: \d{3,} strands, \d+ points, 3 motion key")
        kept = re.search(r"LOD keeps (\d+) of (\d+) strands", result.stdout)
        self.assertIsNotNone(kept, result.stdout[-3000:])
        self.assertAlmostEqual(int(kept.group(1)) / float(kept.group(2)), 0.5, delta=0.1)
        self.assertTrue(os.path.exists(image))

        # What the procedural made, expanded and saved (as text): its instancer, expanded into a
        # ginstance per instance, has a matrix key per motion key, and they move with the scalp.
        expanded = os.path.splitext(ASS_PATH)[0] + "_expanded.ass"
        resave = command[:3] + ["-l", PROCEDURALS_DIR, "-forceexpand", "-resave", expanded, "-db",
                                "-dw", "-dp", "-sl", "-v", "1"]
        for attempt in range(2):
            try:
                result = subprocess.run(resave, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, universal_newlines=True, timeout=90)
                break
            except subprocess.TimeoutExpired as e:
                if attempt == 1:
                    self.fail("kick -resave stalled twice")
        self.assertEqual(result.returncode, 0, result.stdout[-3000:])
        with open(expanded) as f:
            text = f.read()
        color = hair_color(text)
        self.assertEqual(color["category"], "varying")
        self.assertEqual(color["count"], sum(color["counts"]))  # every kept strand's points, after LOD
        self.assertEqual(color["count"], array_values(block(text, "curves"), "points")[0])
        at = 0
        for n in color["counts"]:
            self.assertAlmostEqual(color["colors"][at + n - 1][0], 0.9, places=4)
            at += n
        copies = [b for b in re.findall(r"\nginstance\s*\n\{(.*?)\n\}", text, re.S) if "/instances_instance_" in b]
        self.assertGreater(len(copies), 10, "no instances in the expanded scene")
        moving = 0
        for body in copies:
            m = re.search(r"\n\s*matrix\s+1\s+(\d+)\s+MATRIX\s", body)
            self.assertEqual(int(m.group(1)), 3, body[:500])
            values = [float(v) for v in body[m.end():].split(None, 48)[:48]]
            moving += max(abs(a - b) for a, b in zip(values[12:15], values[32 + 12:32 + 15])) > 1e-4
        self.assertGreater(moving, len(copies) // 2)  # the origins move from the first key to the last

        groomlab.render_curves(self.groom)
        self.assertIsNotNone(block(self.export(), "curves"))


def curves_body(path):
    """The first curves node's block of an .ass, its name lines left out (what the groom made)."""
    with open(path) as f:
        body = block(f.read(), "curves")
    return re.sub(r"\n\s*name .*", "", body) if body else None


class KeptEvaluatorTest(unittest.TestCase):
    """The translator keeps each groom's evaluator from one export to the next (a sequence's frames,
    an interactive render's updates): what it writes must be what a fresh evaluator writes."""

    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin("mtoa")
        cmds.loadPlugin(PLUGIN_PATH, quiet=True)

    def setUp(self):
        cmds.file(new=True, force=True)
        cmds.currentUnit(time="film")
        self.folder = tempfile.mkdtemp(prefix="groomlab_kept_")
        scalp = cmds.polySphere(radius=1, subdivisionsX=30, subdivisionsY=16)[0]
        squash = cmds.nonLinear(scalp, type="squash")[0]
        cmds.setKeyframe(squash, attribute="factor", time=1, value=0.0)
        cmds.setKeyframe(squash, attribute="factor", time=10, value=0.8)
        self.curves = [cmds.curve(degree=2, point=[(0, 1, 0), (0.6, 1.5, 0.2), (1.4, 1.2, 0.3)]),
                       cmds.curve(degree=2, point=[(1, 0, 0), (1.6, -0.3, 0.4), (1.9, -1.0, 0.6)])]
        self.groom = groomlab.create_groom(scalp, self.curves)
        groomlab.set_param(self.groom, "scatter", "density", 300.0)
        groomlab.add_wind(self.groom, strength=0.4)  # moves with time on top of the squash
        cmds.shadingNode("aiSkyDomeLight", asLight=True)
        import mtoa.core
        mtoa.core.createOptions()
        options = "defaultArnoldRenderOptions"
        cmds.setAttr(options + ".motion_blur_enable", True)
        cmds.setAttr(options + ".mb_object_deform_enable", True)
        cmds.setAttr(options + ".motion_steps", 2)
        cmds.currentTime(1)

    def save(self, name):
        path = os.path.join(self.folder, name + ".ma").replace("\\", "/")
        cmds.file(rename=path)
        cmds.file(save=True, type="mayaAscii", force=True)
        return path

    def fresh(self, scene, frame, name):
        """The groom's curves at `frame` from a freshly opened scene (no kept evaluator)."""
        cmds.file(scene, open=True, force=True)
        cmds.currentTime(frame)
        path = os.path.join(self.folder, name + ".ass").replace("\\", "/")
        cmds.arnoldExportAss(filename=path, asciiAss=True)
        return curves_body(path)

    def test_a_sequence_writes_what_each_frame_alone_writes(self):
        scene = self.save("sequence")
        base = os.path.join(self.folder, "seq.ass").replace("\\", "/")
        cmds.arnoldExportAss(filename=base, startFrame=1, endFrame=4, asciiAss=True)
        written = sorted(f for f in os.listdir(self.folder) if f.startswith("seq") and f.endswith(".ass"))
        self.assertEqual(len(written), 4, written)
        sequence = [curves_body(os.path.join(self.folder, f)) for f in written]
        self.assertEqual(len(set(sequence)), 4)  # every frame differs (the squash, the wind)
        for frame, body in zip(range(1, 5), sequence):
            self.assertEqual(body, self.fresh(scene, frame, "alone%d" % frame), "frame %d" % frame)

    def test_edits_between_exports_are_picked_up(self):
        path = os.path.join(self.folder, "first.ass").replace("\\", "/")
        cmds.arnoldExportAss(filename=path, asciiAss=True)
        first = curves_body(path)
        # The graph edited (an interactive render's update, or the next export): swapped in.
        groomlab.set_param(self.groom, "scatter", "density", 500.0)
        cmds.arnoldExportAss(filename=path, asciiAss=True)
        denser = curves_body(path)
        self.assertNotEqual(denser, first)
        self.assertEqual(denser, self.fresh(self.save("denser"), 1, "denserFresh"))
        # The guides made a hierarchy (guideData) and a guide moved: the groom is read afresh.
        groomlab.guides_from_curves(self.groom, self.curves)
        cmds.arnoldExportAss(filename=path, asciiAss=True)
        guided = curves_body(path)
        self.assertNotEqual(guided, denser)
        self.assertEqual(guided, self.fresh(self.save("guided"), 1, "guidedFresh"))
        # Another frame after the edits.
        cmds.currentTime(3)
        cmds.arnoldExportAss(filename=path, asciiAss=True)
        self.assertEqual(curves_body(path), self.fresh(self.save("frame3"), 3, "frame3Fresh"))


if __name__ == "__main__":
    result = unittest.main(exit=False).result
    maya.standalone.uninitialize()
    sys.exit(0 if result.wasSuccessful() else 1)
