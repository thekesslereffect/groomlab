# SPDX-License-Identifier: Apache-2.0
"""GroomSession from Python: a groom file's session matches the CLI's output, inputs from buffers,
setters that change nothing for the same input, previews, versions and stats."""

import array
import json
import math
import os
import shutil
import subprocess
import tempfile
import unittest

import groomlab_core as gl

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.path.join(HERE, "..", "..", "data", "compat", "v1", "groom.groom")
CLI = os.environ.get("GROOMLAB_CLI", "")

LOOK_GRAPH = {
    "version": 1,
    "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "guides", "type": "Guides"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 300.0}},
        {"name": "interpolate", "type": "Interpolate"},
        {"name": "cut", "type": "Length", "params": {"keep": {"colorSet": "tint", "min": 0.2, "max": 1.0}}},
        {"name": "width", "type": "Width"},
        {"name": "output", "type": "Output"},
    ],
    "connections": [
        {"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "interpolate", "input": "strands"},
        {"from": "guides", "to": "interpolate", "input": "guides"},
        {"from": "scalp", "to": "interpolate", "input": "scalp"},
        {"from": "interpolate", "to": "cut", "input": "strands"},
        {"from": "cut", "to": "width", "input": "strands"},
        {"from": "width", "to": "output", "input": "strands"},
    ],
    "output": "output",
}


def sphere_buffers(rings=12, segments=24):
    """A unit UV sphere as flat float32 points and int32 triangles (array.array: buffers, like numpy's)."""
    points = array.array("f")
    for r in range(rings + 1):
        theta = math.pi * r / rings
        for s in range(segments + 1):
            phi = 2.0 * math.pi * s / segments
            points.extend((math.sin(theta) * math.cos(phi) + 0.0, math.cos(theta) + 0.0,
                           math.sin(theta) * math.sin(phi) + 0.0))
    tris = array.array("i")
    at = lambda r, s: r * (segments + 1) + s
    for r in range(rings):
        for s in range(segments):
            tris.extend((at(r, s), at(r, s + 1), at(r + 1, s)))
            tris.extend((at(r, s + 1), at(r + 1, s + 1), at(r + 1, s)))
    return points, tris


def hierarchy_on(scalp):
    h = gl.GuideHierarchy()
    primary = h.add_group("primary")
    tube = gl.TubeProfile([0.15, 0.1])
    for i in range(8):
        a = 2.0 * math.pi * i / 8
        n = gl.Vec3(math.cos(a) * 0.6, 0.8, math.sin(a) * 0.6).normalized()
        tip = n * 0.8 + gl.Vec3(0, 0, 0.2)
        h.add_guide(primary, [tuple(n[k] + tip[k] * j / 6 for k in range(3)) for j in range(7)], tube)
    h.evaluate(scalp)
    return h


def same_strands(test, a, b):
    test.assertEqual(a.strand_count, b.strand_count)
    test.assertEqual(bytes(memoryview(a.ids_view())), bytes(memoryview(b.ids_view())))
    test.assertEqual(bytes(memoryview(a.offsets_view())), bytes(memoryview(b.offsets_view())))
    test.assertEqual(bytes(memoryview(a.positions_view())), bytes(memoryview(b.positions_view())))
    test.assertEqual(bytes(memoryview(a.widths_view())), bytes(memoryview(b.widths_view())))


def cli_hash(path):
    out = subprocess.run([CLI, "hash", path], check=True, capture_output=True, text=True).stdout
    return [line for line in out.splitlines() if line.startswith("all=")][0]


class GroomFileSessionTest(unittest.TestCase):
    def test_a_groom_files_session_is_what_groomscene_and_the_cli_evaluate(self):
        session = gl.GroomSession.from_groom_file(FIXTURE)
        fps = gl.GroomFile.read(FIXTURE).frames_per_second
        view = session.view(density=1.0, time=1.0 / fps)
        self.assertEqual(view.errors, [])
        strands = view.strands
        self.assertGreater(strands.strand_count, 1000)
        if hasattr(gl, "GroomScene"):  # the file as the CLI and the procedural evaluate it
            same_strands(self, strands, gl.GroomScene(FIXTURE).evaluate(1.0 / fps, 1.0))
        # The buffers are the session's own arrays (no copy): (points, 3) float32.
        positions = memoryview(strands.positions_view())
        self.assertEqual(positions.shape, (strands.point_count, 3))
        self.assertEqual(positions.format, "f")
        stats = session.stats()
        self.assertEqual(stats["strands"], strands.strand_count)
        self.assertEqual(stats["guides"], 15)
        self.assertIn("interpolate", stats["times"])

        if not CLI or not hasattr(gl, "AlembicCurvesWriter"):
            self.skipTest("groomlab-cli or Alembic not available")
        folder = tempfile.mkdtemp(prefix="groomlab_session_")
        try:
            ours = os.path.join(folder, "session.abc")
            with gl.AlembicCurvesWriter(ours, "groom", 1.0 / fps, 1.0 / fps) as w:
                w.write_frame(strands, view.scalp)
            theirs = os.path.join(folder, "cli.abc")
            subprocess.run([CLI, "eval", FIXTURE, "-o", theirs, "-f", "1"], check=True, capture_output=True)
            self.assertEqual(cli_hash(ours), cli_hash(theirs))
        finally:
            shutil.rmtree(folder, ignore_errors=True)


class SessionTest(unittest.TestCase):
    def make(self):
        points, tris = sphere_buffers()
        session = gl.GroomSession()
        session.set_scalp_mesh(points, tris)
        session.set_hierarchy(hierarchy_on(session.scalp))
        session.set_graph_json(LOOK_GRAPH)
        values = [1.0 if points[3 * i] > 0.0 else 0.0 for i in range(len(points) // 3)]
        session.set_scalp_maps_json({"tint": {"kind": "vertices", "values": values}})
        return session, points, values

    def test_show_tubes_names_mesh_and_display_item(self):
        # Show Tubes (a Braid node's button in every host's graph editor) through the bindings: the
        # core's rules for the names, the session's mesh of the nodes shown, the builder's item.
        session, _points, _values = self.make()
        graph = json.loads(json.dumps(LOOK_GRAPH))
        last = [c for c in graph["connections"] if c["to"] == graph["output"]][0]
        graph["nodes"].append({"name": "side braid", "type": "Braid", "params": {"region": 0.6, "width": 0.15}})
        graph["connections"].remove(last)
        graph["connections"] += [{"from": last["from"], "to": "side braid", "input": "strands"},
                                 {"from": "side braid", "to": graph["output"], "input": last["input"]}]
        session.set_graph_json(graph)
        edit = gl.GraphEdit(json.dumps(graph))
        self.assertTrue(edit.braid_tubes_offered("side braid"))
        self.assertFalse(edit.braid_tubes_offered("scatter"))
        shown = edit.braid_tubes_toggled("side braid", "")
        self.assertEqual(shown, "side braid")
        self.assertEqual(edit.braid_tubes_toggled("scatter", shown), shown)
        self.assertTrue(edit.braid_tubes_shown("side braid", shown))
        self.assertEqual(edit.braid_tubes_toggled("side braid", shown), "")
        self.assertEqual(gl.braid_tube_names("a\n\nb\na"), ["a", "b"])
        self.assertEqual(gl.braid_tubes_text(["a", "b"]), "a\nb")
        self.assertEqual(gl.renamed_braid_tubes(shown, "side braid", "plait"), "plait")
        self.assertEqual(edit.braid_tubes_kept("gone\nscatter\nside braid"), "side braid")
        self.assertNotEqual(gl.braid_section_color(0), gl.braid_section_color(1))

        view = session.view()
        self.assertEqual(view.errors, [])
        self.assertIsNone(session.braid_tube_mesh(""))
        self.assertIsNone(session.braid_tube_mesh("scatter"))
        mesh = session.braid_tube_mesh(shown)
        self.assertIsNotNone(mesh)
        self.assertGreater(mesh.tubes, 0)
        points = memoryview(mesh.points)
        self.assertEqual(points.shape[1], 3)
        self.assertEqual(points.shape[0] % 24, 0)  # (a version 2 node's pressed sections: 24 points a ring)
        self.assertEqual(memoryview(mesh.normals).shape, points.shape)
        self.assertEqual(memoryview(mesh.colors).shape, points.shape)
        self.assertEqual(memoryview(mesh.uvs).shape, (points.shape[0], 2))
        quads = memoryview(mesh.quads)
        self.assertEqual(quads.shape, (points.shape[0] - 24 * mesh.tubes, 4))
        self.assertEqual(len(mesh.triangles()), 6 * quads.shape[0])
        builds = session.braid_tube_builds
        self.assertEqual(session.braid_tube_mesh(shown).key, mesh.key)
        self.assertEqual(session.braid_tube_builds, builds)  # kept: nothing built again

        builder = gl.DisplayBuilder()
        settings = gl.DisplaySettings()
        items = builder.update(view, settings, session=session)
        self.assertEqual(items["braidTubes"].vertex_count, 0)
        strands = items["strands"].geometry.digest
        settings.braid_tubes = shown
        items = builder.update(view, settings, session=session)
        tubes = items["braidTubes"]
        self.assertTrue(tubes.rebuilt)
        self.assertEqual(tubes.vertex_count, points.shape[0])
        self.assertEqual(memoryview(tubes.geometry.indices).shape, (2 * quads.shape[0], 3))
        self.assertEqual(memoryview(tubes.geometry.colors).shape, (points.shape[0], 4))
        self.assertEqual(items["strands"].geometry.digest, strands)  # the strands as they were
        self.assertNotIn("strands", builder.rebuilt_parts)
        self.assertIn("braidTubes", builder.rebuilt_parts)

    def test_inputs_from_buffers_and_setters_that_change_nothing(self):
        session, points, values = self.make()
        first = session.view()
        self.assertEqual(first.errors, [])
        self.assertTrue(first.scalp.has_color_set("tint"))
        self.assertGreater(first.strands.strand_count, 100)
        runs = session.evaluation_count
        # The same inputs again: nothing evaluates, no version moves.
        session.set_graph_json(json.dumps(LOOK_GRAPH))
        session.set_scalp_maps_json(session.scalp_maps_json)
        session.update_scalp_points(points)
        again = session.view()
        self.assertEqual(session.evaluation_count, runs)
        self.assertEqual(again.versions, first.versions)
        # Moved points: the same topology, other strands.
        moved = array.array("f", points)
        for i in range(1, len(moved), 3):
            moved[i] += 0.25
        session.update_scalp_points(moved)
        after = session.view()
        self.assertGreater(after.versions["strands"], first.versions["strands"])
        lo, hi = after.bounds
        self.assertGreater(hi[1], first.bounds[1][1])
        with self.assertRaises(ValueError):
            session.update_scalp_points([(0, 0, 0)])

    def test_follow_scalp_and_reads_time(self):
        points, tris = sphere_buffers()
        session = gl.GroomSession()
        session.set_scalp_mesh(points, tris, rest_points=points)
        session.set_hierarchy(hierarchy_on(session.scalp))
        session.set_graph_json(LOOK_GRAPH)
        values = [1.0 if points[3 * i] > 0.0 else 0.0 for i in range(len(points) // 3)]
        session.set_scalp_maps_json({"tint": {"kind": "vertices", "values": values}})
        self.assertFalse(session.follow_scalp)
        self.assertFalse(session.reads_time)
        first = session.view()
        self.assertEqual(first.errors, [])
        rest = first.strands
        # The scalp moved up (its rest points kept): followed, the groom at rest moved along.
        moved = array.array("f", points)
        for i in range(1, len(moved), 3):
            moved[i] += 0.25
        session.update_scalp_points(moved)
        grown = session.view().strands
        session.follow_scalp = True
        followed = session.view()
        self.assertEqual(followed.errors, [])
        self.assertTrue(session.following)
        self.assertEqual(followed.strands.strand_count, rest.strand_count)
        a, b = followed.strands.positions, rest.positions
        self.assertTrue(all(abs(a[i].y - b[i].y - 0.25) < 1e-4 and abs(a[i].x - b[i].x) < 1e-4
                            for i in range(0, len(b), 7)))
        session.follow_scalp = False
        same_strands(self, session.view().strands, grown)
        self.assertFalse(session.following)
        # New rest points with the points: the rest pose moves too.
        session.update_scalp_points(moved, rest_points=moved)
        first = session.scalp.rest_points[0]
        self.assertEqual((first.x, first.y, first.z), tuple(moved[0:3]))

    def test_previews_views_and_stats(self):
        session, points, values = self.make()
        before = session.view(density=0.5)
        # A vertex map stroke: the map's readers re-run at the sculpt density, and the stroke asks
        # for another view at the full density when it ends.
        session.set_live_stroke(True)
        session.set_preview_vertex_map("tint", array.array("f", [1.0] * len(values)))
        during = session.view(density=0.5, sculpt_density=0.5)
        self.assertLess(during.strands.strand_count, before.strands.strand_count)
        self.assertTrue(session.set_live_stroke(False))
        session.set_preview_vertex_map("tint", None)
        same_strands(self, session.view(density=0.5).strands, before.strands)
        # A node viewed instead of the output, a scratch evaluation of another.
        scatter = session.view(view_node="scatter")
        self.assertEqual(scatter.viewed_node, "scatter")
        self.assertTrue(scatter.viewed_roots)
        self.assertEqual(session.evaluate_node("interpolate").strand_count, scatter.strands.strand_count)
        # Stats as groomLabGraphStats reports them, and the one-call evaluate of the alias Groom.
        stats = session.stats()
        for key in ("view", "strands", "points", "guides", "cards", "colorNodes", "times", "channels", "notes",
                    "scalpHistory", "instances", "restEvaluations"):
            self.assertIn(key, stats)
        self.assertIs(gl.Groom, gl.GroomSession)
        self.assertGreater(session.evaluate(1.0, 0.0).strand_count, 0)
        # Bad texts are said, not raised; a bad graph raises and keeps the old one.
        session.set_parts_json("{nope")
        self.assertTrue(session.view().errors[0].startswith("partData: "))
        self.assertEqual(len(session.input_errors), 1)
        with self.assertRaises(gl.GraphError):
            session.set_graph_json("{bad")
        session.set_parts_json("")
        self.assertEqual(session.view().errors, [])

    def test_simulation(self):
        """A Simulate node between the guides and Interpolate: the head's motion through a feed,
        Simulate Range, the cache's status, a bake written and read back."""
        points, tris = sphere_buffers()
        graph = json.loads(json.dumps(LOOK_GRAPH))
        graph["nodes"].insert(2, {"name": "sim", "type": "Simulate",
                                  "params": {"gravity": 9.8, "collisionOffset": 0.02, "cacheMode": "cached"}})
        graph["connections"] = [c for c in graph["connections"] if not (c["from"] == "guides" and c["to"] == "interpolate")]
        graph["connections"] += [{"from": "guides", "to": "sim", "input": "guides"},
                                 {"from": "sim", "to": "interpolate", "input": "guides"}]
        session = gl.GroomSession()
        session.set_scalp_mesh(points, tris)
        session.set_hierarchy(hierarchy_on(session.scalp))
        session.set_graph_json(graph)
        tint = {"tint": {"kind": "vertices", "values": [1.0] * (len(points) // 3)}}
        session.set_scalp_maps_json(tint)
        self.assertEqual(session.frames_per_second, 24.0)
        self.assertEqual(session.simulate_nodes(), ["sim"])
        self.assertTrue(session.reads_time)

        def feed(seconds):  # the head slides +x over the first 8 frames
            moved = array.array("f", points)
            for i in range(0, len(moved), 3):
                moved[i] += 0.05 * min(max(seconds * 24.0 - 1.0, 0.0), 8.0)
            session.update_scalp_points(moved)
        session.set_simulation_feed(feed)
        feed(1 / 24.0)
        self.assertEqual(session.view(time=1 / 24.0).errors, [])
        seen = []
        run = session.simulate_to(16, progress=lambda done, count: seen.append((done, count)))
        self.assertEqual(run["errors"], [])
        self.assertEqual(run["frames"], 16)
        self.assertFalse(run["cancelled"])
        self.assertEqual(seen[-1], (16, 16))
        status = session.simulation_status()[0]
        self.assertEqual(status["node"], "sim")
        self.assertEqual(status["ranges"], [(1, 16)])
        self.assertFalse(status["live"])
        self.assertEqual(len(status["stamp"]), 16)
        frame = session.simulated_frame("sim", 8)
        self.assertEqual(len(frame), 8)
        self.assertIsNone(session.simulated_frame("sim", 30))
        feed(8 / 24.0)
        moving = session.view(time=8 / 24.0)
        self.assertEqual(moving.errors, [])
        if gl.has_alembic:
            folder = tempfile.mkdtemp()
            try:
                path = os.path.join(folder, "sim.abc")
                self.assertEqual(gl.write_simulation_cache(session, "sim", path, 1, 16), 16)
                baked = gl.GroomSession()
                baked.set_scalp_mesh(points, tris)
                baked.set_hierarchy(hierarchy_on(baked.scalp))
                baked.set_graph_json(graph)
                baked.set_scalp_maps_json(tint)
                gl.attach_simulation_cache(baked, "sim", path, stamp=session.simulation_stamp("sim"))
                moved = array.array("f", points)
                for i in range(0, len(moved), 3):
                    moved[i] += 0.05 * 7.0
                baked.update_scalp_points(moved)
                read = baked.view(time=8 / 24.0)
                a, b = read.strands.positions, moving.strands.positions
                self.assertTrue(all(abs(a[i].x - b[i].x) < 1e-5 and abs(a[i].y - b[i].y) < 1e-5 for i in range(0, len(b), 5)))
                self.assertTrue(baked.simulation_status()[0]["baked"])
                del baked
            finally:
                shutil.rmtree(folder, ignore_errors=True)
        session.invalidate_simulation()
        self.assertEqual(session.simulation_status()[0]["frames"], 0)
        session.set_simulation_feed(None)


if __name__ == "__main__":
    unittest.main()
