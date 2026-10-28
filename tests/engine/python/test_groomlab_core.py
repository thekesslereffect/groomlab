# SPDX-License-Identifier: Apache-2.0
"""Binding tests. Uses unittest so it runs under mayapy without extra packages."""

import array
import json
import math
import os
import shutil
import tempfile
import unittest

import groomlab_core as gl


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def make_sphere(rings=12, segments=24, radius=1.0):
    """UV sphere with outward normals (pole rows are degenerate, fine for queries)."""
    points = []
    tris = []
    for r in range(rings + 1):
        theta = math.pi * r / rings
        for s in range(segments + 1):
            phi = 2.0 * math.pi * s / segments
            # + 0.0 turns -0.0 into +0.0: the same value, but JSON writes it as 0,
            # which would change the content hash across a file round trip.
            points.append((radius * math.sin(theta) * math.cos(phi) + 0.0,
                           radius * math.cos(theta) + 0.0,
                           radius * math.sin(theta) * math.sin(phi) + 0.0))

    def at(r, s):
        return r * (segments + 1) + s

    for r in range(rings):
        for s in range(segments):
            tris.append((at(r, s), at(r, s + 1), at(r + 1, s)))
            tris.append((at(r, s + 1), at(r + 1, s + 1), at(r + 1, s)))
    return gl.ScalpMesh(points, tris)


def straight(root, direction, segments=6):
    return [tuple(root[k] + direction[k] * i / segments for k in range(3)) for i in range(segments + 1)]


def make_hierarchy(scalp, guides=8, children=4):
    """Primary guides on the upper half of a unit sphere, with generated children."""
    h = gl.GuideHierarchy()
    primary = h.add_group("primary")
    tube = gl.TubeProfile([0.15, 0.1])
    for i in range(guides):
        a = 2.0 * math.pi * i / guides
        n = gl.Vec3(math.cos(a) * 0.6, 0.8, math.sin(a) * 0.6).normalized()
        h.add_guide(primary, straight(n, n * 0.8 + gl.Vec3(0, 0, 0.2)), tube)
    if children:
        params = gl.ChildGenerationParams(count=children)
        h.generate_children(primary, "children", params)
    h.evaluate(scalp)
    return h


LOOK_GRAPH = {
    "version": 1,
    "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "guides", "type": "Guides"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 300.0}},
        {"name": "interpolate", "type": "Interpolate"},
        {"name": "length", "type": "Length"},
        {"name": "width", "type": "Width"},
        {"name": "output", "type": "Output"},
    ],
    "connections": [
        {"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "interpolate", "input": "strands"},
        {"from": "guides", "to": "interpolate", "input": "guides"},
        {"from": "scalp", "to": "interpolate", "input": "scalp"},
        {"from": "interpolate", "to": "length", "input": "strands"},
        {"from": "length", "to": "width", "input": "strands"},
        {"from": "width", "to": "output", "input": "strands"},
    ],
    "output": "output",
}


class SceneMixin:
    @classmethod
    def setUpClass(cls):
        cls.scalp = make_sphere()
        cls.hierarchy = make_hierarchy(cls.scalp)


# ---------------------------------------------------------------------------
# Basics
# ---------------------------------------------------------------------------


class BasicsTest(unittest.TestCase):
    def test_hair_lobes_are_cores(self):
        lobes = gl.hair_lobes(0.15, 0.5)
        self.assertAlmostEqual(lobes["specular_tint"], 0.5, places=6)
        self.assertAlmostEqual(lobes["roughness"], 0.3, places=6)
        self.assertAlmostEqual(lobes["ior"], 1.55, places=6)
        self.assertEqual(gl.hair_lobes(0.9, 0.0)["specular_tint"], 1.0)
        self.assertAlmostEqual(gl.HAIR_FULL_SPECULAR, 0.3, places=6)
        self.assertAlmostEqual(gl.HAIR_ROUGHNESS_MIN + gl.HAIR_ROUGHNESS_RANGE, 0.5, places=6)

    def test_version(self):
        self.assertRegex(gl.__version__, r"^\d+\.\d+\.\d+$")
        self.assertIsInstance(gl.has_alembic, bool)
        self.assertIsInstance(gl.has_tbb, bool)

    def test_vec3(self):
        v = gl.Vec3(1, 2, 3)
        self.assertEqual(v, (1.0, 2.0, 3.0))
        self.assertEqual(tuple(v), (1.0, 2.0, 3.0))
        self.assertEqual(v[2], 3.0)
        self.assertEqual(gl.Vec3((1, 2, 3)), v)
        self.assertEqual((v + (1, 1, 1)).to_tuple(), (2.0, 3.0, 4.0))
        self.assertAlmostEqual(gl.Vec3(3, 4, 0).length(), 5.0)
        self.assertEqual(v.dot((1, 0, 0)), 1.0)
        self.assertNotEqual(v, (0, 0, 0))
        self.assertNotEqual(v, "abc")
        self.assertEqual(len(v), 3)

    def test_ramp(self):
        r = gl.Ramp([(0.0, 0.0), (1.0, 1.0)])
        self.assertAlmostEqual(r(0.5), 0.5, places=5)
        self.assertEqual(gl.Ramp.constant(2.0)(0.3), 2.0)
        self.assertEqual(r.keys, [(0.0, 0.0), (1.0, 1.0)])
        r.straight = True
        self.assertTrue(r.straight)
        self.assertEqual(gl.Ramp.linear(0, 1), gl.Ramp([(1.0, 1.0), (0.0, 0.0)]))

    def test_map_ref(self):
        m = gl.MapRef.texture("C:/maps/clump.png", gl.MapComponent.G, 0.2, 0.8)
        self.assertEqual(m.source, gl.MapSource.Texture)
        self.assertEqual(m.name, "C:/maps/clump.png")
        self.assertEqual(m.component_name, "g")
        self.assertEqual(m.source_key, "texture")
        self.assertEqual(gl.MapRef.attribute("melanin").source_key, "attribute")
        self.assertEqual(gl.MapRef.expression("0.5").source, gl.MapSource.Expression)

    def test_strand_root(self):
        r = gl.StrandRoot(3, 0.25, 0.5)
        self.assertEqual((r.face, r.u, r.v), (3, 0.25, 0.5))
        self.assertTrue(r.rooted)
        self.assertFalse(gl.StrandRoot().rooted)

    def test_hash_and_density(self):
        self.assertEqual(gl.hash64(1), gl.hash64(1))
        self.assertNotEqual(gl.hash64(1), gl.hash64(2))
        self.assertTrue(all(gl.keep_at_density(i, 1.0) for i in range(100)))
        kept = sum(1 for i in range(10000) if gl.keep_at_density(i, 0.1))
        self.assertTrue(500 < kept < 1500)

    def test_tube_profile(self):
        # Tube guides: a shape and sections (width, aspect, twist), each a list root to tip.
        t = gl.TubeProfile([0.2, 0.1], aspect=[0.5], twist=[0.0, 1.0], shape="ribbon")
        self.assertEqual(t.shape, "ribbon")
        self.assertAlmostEqual(t.width_at(0.5), 0.15, places=6)
        self.assertAlmostEqual(t.aspect_at(1.0), 0.5, places=6)
        self.assertAlmostEqual(t.twist_at(0.5), 0.5, places=6)
        t.shape = "blade"
        self.assertEqual(t.shape, "blade")
        with self.assertRaises(ValueError):
            t.shape = "star"
        self.assertEqual(gl.TubeProfile(), gl.TubeProfile([0.1, 0.1]))
        self.assertEqual(list(gl.tube_shapes()), ["round", "ribbon", "teardrop", "blade"])
        outline = gl.tube_outline("teardrop")
        self.assertGreater(len(outline), 32)
        self.assertAlmostEqual(max(v for _, v in outline), 1.0, places=2)
        # A hierarchy keeps it, and children follow its aspect.
        scalp = make_sphere()
        h = make_hierarchy(scalp, guides=2, children=0)
        primary = h.guides_in_group(h.find_group("primary"))[0]
        h.set_tube(primary, gl.TubeProfile([0.2], aspect=[0.1]))
        self.assertAlmostEqual(h.guide(primary).tube.aspect[0], 0.1, places=6)
        self.assertEqual(gl.GuideHierarchy.from_json(h.to_json()).guide(primary).tube, h.guide(primary).tube)


# ---------------------------------------------------------------------------
# StrandSet
# ---------------------------------------------------------------------------


class StrandSetTest(unittest.TestCase):
    def test_add_and_read_strand(self):
        s = gl.StrandSet()
        s.add_strand(7, [(0, 0, 0), (0, 1, 0), (0, 2, 0)])
        self.assertEqual(s.strand_count, 1)
        self.assertEqual(s.point_count, 3)
        self.assertEqual(s.ids, [7])
        self.assertEqual(s.strand_points(0)[2], (0.0, 2.0, 0.0))
        self.assertEqual(s.offsets, [0, 3])
        self.assertEqual(s.widths, [1.0, 1.0, 1.0])
        s.validate()
        self.assertTrue(s.is_valid())

    def test_bad_widths_raise(self):
        s = gl.StrandSet()
        with self.assertRaises(ValueError):
            s.add_strand(1, [(0, 0, 0), (0, 1, 0)], widths=[1.0])

    def test_add_strand_from_buffer(self):
        s = gl.StrandSet()
        pts = array.array("f", [0, 0, 0, 1, 0, 0, 2, 0, 0])
        s.add_strand(1, pts, widths=array.array("d", [0.5, 0.5, 0.5]), root=gl.StrandRoot(2, 0.1, 0.2))
        self.assertEqual(s.point_count, 3)
        self.assertEqual(s.strand_points(0)[2], (2.0, 0.0, 0.0))
        self.assertEqual(s.widths, [0.5, 0.5, 0.5])
        self.assertEqual(s.roots[0].face, 2)

    def test_buffer_views_have_the_right_shapes(self):
        s = gl.StrandSet()
        for i in range(4):
            s.add_strand(i, [(i, 0, 0), (i, 1, 0), (i, 2, 0)], widths=[0.1, 0.2, 0.3])
        pos = s.positions_view()
        self.assertEqual(pos.shape, (12, 3))
        self.assertEqual(pos.format, "f")
        self.assertTrue(pos.readonly)
        self.assertEqual(len(pos), 12)
        self.assertEqual(pos[4], (1.0, 1.0, 0.0))
        self.assertEqual(pos[-1], (3.0, 2.0, 0.0))
        mv = memoryview(pos)
        self.assertEqual(mv.shape, (12, 3))
        self.assertEqual(mv.format, "f")
        self.assertTrue(mv.readonly)
        self.assertEqual(mv.tolist()[4], [1.0, 1.0, 0.0])

        self.assertEqual(s.widths_view().shape, (12,))
        for got, want in zip(memoryview(s.widths_view()).tolist()[:3], [0.1, 0.2, 0.3]):
            self.assertAlmostEqual(got, want, places=6)
        self.assertAlmostEqual(s.widths_view()[1], 0.2, places=6)
        ids = s.ids_view()
        self.assertEqual(ids.shape, (4,))
        self.assertEqual(memoryview(ids).format, "Q")
        self.assertEqual(ids.tolist(), [0, 1, 2, 3])
        offs = s.offsets_view()
        self.assertEqual(offs.shape, (5,))
        self.assertEqual(offs.tolist(), [0, 3, 6, 9, 12])
        roots = s.roots_view()
        self.assertEqual(roots.shape, (4,))
        self.assertEqual(memoryview(roots).itemsize, 12)
        self.assertEqual(roots[0].face, -1)

        with self.assertRaises(IndexError):
            pos[12]
        with self.assertRaises(TypeError):
            pos[0] = (1, 1, 1)

    def test_mutable_views_write_through(self):
        s = gl.StrandSet()
        s.add_strand(1, [(0, 0, 0), (0, 1, 0)])
        copy = s.copy()  # shares the arrays until written
        pos = s.mutable_positions()
        self.assertFalse(pos.readonly)
        pos[1] = (5, 6, 7)
        self.assertEqual(s.strand_points(0)[1], (5.0, 6.0, 7.0))
        self.assertEqual(copy.strand_points(0)[1], (0.0, 1.0, 0.0))  # copy-on-write kept the copy
        mv = memoryview(s.mutable_widths())
        mv[0] = 0.25
        self.assertAlmostEqual(s.widths[0], 0.25)
        s.mutable_ids()[0] = 99
        self.assertEqual(s.ids, [99])
        s.mutable_roots()[0] = gl.StrandRoot(4, 0.5, 0.25)
        self.assertEqual(s.roots[0].face, 4)
        s.set_strand_points(0, [(1, 1, 1), (2, 2, 2)])
        self.assertEqual(s.strand_points(0)[0], (1.0, 1.0, 1.0))
        s.set_positions([(0, 0, 0), (0, 0, 1)])
        self.assertEqual(s.positions[1], (0.0, 0.0, 1.0))
        with self.assertRaises(ValueError):
            s.set_positions([(0, 0, 0)])

    def test_channels(self):
        s = gl.StrandSet()
        s.add_strand(1, [(0, 0, 0), (0, 1, 0)])
        s.add_strand(2, [(1, 0, 0), (1, 1, 0)])
        s.add_strand_channel("clumpId", 3.0)
        s.add_point_channel("wet")
        self.assertEqual(s.strand_channel_names(), ["clumpId"])
        self.assertEqual(s.point_channel_names(), ["wet"])
        self.assertEqual(s.strand_channel("clumpId").tolist(), [3.0, 3.0])
        self.assertEqual(s.strand_channel("missing"), None)
        s.mutable_point_channel("wet")[3] = 1.0
        self.assertEqual(s.point_channel("wet").tolist(), [0.0, 0.0, 0.0, 1.0])
        s.set_strand_channel("clumpId", [1, 2])
        self.assertEqual(s.strand_channel("clumpId").tolist(), [1.0, 2.0])
        s.set_point_channel("new", array.array("f", [1, 2, 3, 4]))
        self.assertEqual(s.point_channel("new").shape, (4,))
        with self.assertRaises(ValueError):
            s.set_strand_channel("clumpId", [1, 2, 3])
        s.remove_strand_channel("clumpId")
        self.assertEqual(s.strand_channel_names(), [])
        sub = s.subset([1])
        self.assertEqual(sub.ids, [2])
        self.assertEqual(sub.point_channel("wet").tolist(), [0.0, 1.0])

    def test_density_subset_is_stable(self):
        s = gl.StrandSet()
        for i in range(1000):
            s.add_strand(i, [(i, 0, 0), (i, 1, 0)])
        low = s.with_density(0.1)
        high = s.with_density(0.5)
        self.assertTrue(set(low.ids) <= set(high.ids))
        self.assertTrue(all(gl.keep_at_density(i, 0.1) for i in low.ids))
        self.assertEqual(low.content_hash(), s.with_density(0.1).content_hash())

    def test_append_resize_and_instances(self):
        a = gl.StrandSet()
        a.add_strand(1, [(0, 0, 0), (0, 1, 0)])
        b = gl.StrandSet()
        b.add_strand(2, [(1, 0, 0), (1, 1, 0), (1, 2, 0)])
        a.append(b)
        self.assertEqual(a.ids, [1, 2])
        self.assertEqual(a.uniform_point_count(), 0)
        self.assertEqual(a.max_point_count(), 3)
        a.resize_uniform(3, 4)
        self.assertEqual((a.strand_count, a.point_count, a.uniform_point_count()), (3, 12, 4))
        self.assertIsNone(a.instances)
        inst = gl.InstanceSet()
        self.assertEqual(inst.prototype_index("snow"), 0)
        one = gl.InstanceSet.Instance()
        one.strand = 0
        one.id = 11
        inst.instances = [one]
        a.instances = inst
        self.assertEqual(len(a.instances), 1)
        placed = a.instances.resolve(a)
        self.assertEqual(len(placed), 1)
        self.assertEqual(len(placed[0].matrix()), 16)
        self.assertEqual(placed[0].index, 0)
        # Pinned to a rest distance: that far along the strand (up to the tip).
        self.assertFalse(one.scale_by_width)
        self.assertFalse(one.pin_to_rest)
        self.assertLess(one.rest_distance, 0.0)
        one.pin_to_rest = True
        one.rest_distance = 0.25
        inst.instances = [one]
        c = gl.StrandSet()
        c.add_strand(0, [(0, 0, 0), (0, 1, 0)])
        c.instances = inst
        self.assertAlmostEqual(c.instances.resolve(c)[0].origin.y, 0.25, places=5)
        # Fitted to the strand: scale is a fraction of its rest length along +Y, aspect of that across.
        self.assertFalse(one.fit_strand)
        self.assertLess(one.rest_length, 0.0)
        one.fit_strand = True
        one.scale = 0.5
        one.aspect = 0.2
        one.rest_length = 4.0
        inst.instances = [one]
        c.instances = inst
        p = c.instances.resolve(c)[0]
        self.assertAlmostEqual(p.y.length(), 2.0, places=5)
        self.assertAlmostEqual(p.x.length(), 0.4, places=5)
        self.assertAlmostEqual(p.z.length(), 0.4, places=5)


# ---------------------------------------------------------------------------
# ScalpMesh
# ---------------------------------------------------------------------------


class ScalpMeshTest(unittest.TestCase):
    def test_sphere_queries(self):
        scalp = make_sphere()
        self.assertEqual(scalp.point_count, 13 * 25)
        self.assertEqual(scalp.triangle_count, 12 * 24 * 2)
        self.assertAlmostEqual(scalp.total_area, 4 * math.pi, delta=0.6)
        hit = scalp.closest_point((0, 2, 0))
        self.assertTrue(hit.valid)
        self.assertAlmostEqual(hit.position.y, 1.0, places=2)
        self.assertAlmostEqual(scalp.normal(hit.root).y, 1.0, places=1)
        self.assertEqual(scalp.position(hit.root), hit.position)
        self.assertTrue(scalp.contains(hit.root))
        self.assertFalse(scalp.contains(gl.StrandRoot()))
        self.assertFalse(scalp.closest_point((0, 5, 0), max_distance=1.0).valid)
        ray = scalp.intersect_ray((0, 3, 0), (0, -1, 0))
        self.assertIsNotNone(ray)
        point, t = ray
        self.assertAlmostEqual(t, 2.0, places=2)
        self.assertAlmostEqual(point.position.y, 1.0, places=2)
        self.assertIsNone(scalp.intersect_ray((0, 3, 0), (0, 1, 0)))

    def test_transfer_marker_editor(self):
        # The Transfer Groom windows' model (Maya's window drives this one).
        e = gl.TransferMarkerEditor()
        e.add("left", (1, 0, 0))
        e.add("old", gl.MarkerPoint(point=(2, 0, 0)))
        e.add("right", (10, 0, 0))
        self.assertEqual((e.pair_count, e.pending_count), (1, 1))
        self.assertEqual(e.summary(), "1 pair, 1 unmatched")
        self.assertFalse(e.matched("left", 1))
        e.add_mirrored("right", (3, 1, 0), (-3, 1, 0), 1e-4)
        self.assertEqual(e.count("right"), 3)
        self.assertEqual(e.remove("right", 2), [1, 2])
        self.assertEqual([p.point.x for p in e.points("left")], [1.0])
        self.assertTrue(e.undo())
        self.assertEqual(e.count("left"), 2)
        self.assertEqual(len(e.pairs()), 2)
        self.assertEqual(e.pairs()[1].new_point.x, 3.0)
        self.assertTrue(e.redo())
        with self.assertRaises(ValueError):
            e.add("middle", (0, 0, 0))
        self.assertIn("3 or more", gl.marker_requirement("markersAlign", 2))
        self.assertEqual(gl.marker_requirement("uv", 0), "")
        hit = gl.pick_marker_point(make_sphere(), (0.2, 3, 0.1), (0, -1, 0))
        self.assertGreaterEqual(hit.root.face, 0)
        self.assertAlmostEqual(hit.point.y, 0.97, delta=0.03)
        self.assertEqual(gl.mirror_x((1, 2, 3), 0.5).x, 0.0)

    def test_views_and_buffers(self):
        scalp = make_sphere(4, 8)
        pts = scalp.points_view()
        self.assertEqual(pts.shape, (5 * 9, 3))
        tris = scalp.triangles_view()
        self.assertEqual(tris.shape, (4 * 8 * 2, 3))
        self.assertEqual(memoryview(tris).format, "i")
        self.assertEqual(tuple(scalp.triangles[0]), tuple(tris[0]))
        # Build again from the buffers and from flat index lists.
        again = gl.ScalpMesh(memoryview(pts), memoryview(tris))
        self.assertEqual(again.content_hash(), scalp.content_hash())
        flat = [i for tri in scalp.triangles for i in tri]
        self.assertEqual(gl.ScalpMesh(scalp.points, flat).topology_hash(), scalp.topology_hash())
        moved = scalp.with_points([(p.x * 2, p.y * 2, p.z * 2) for p in scalp.points])
        self.assertEqual(moved.topology_hash(), scalp.topology_hash())
        self.assertNotEqual(moved.content_hash(), scalp.content_hash())

    def test_uvs_colors_rest(self):
        scalp = make_sphere(4, 8)
        n = scalp.triangle_count * 3
        with self.assertRaises(ValueError):
            scalp.set_corner_uvs([(0, 0)] * (n - 1))
        scalp.set_corner_uvs([(0.25, 0.75)] * n)
        self.assertTrue(scalp.has_uvs)
        self.assertEqual(scalp.corner_uvs_view().shape, (n, 2))
        hit = scalp.closest_point((0, 2, 0))
        self.assertEqual(scalp.uv_at(hit.root), [0.25, 0.75])
        scalp.set_corner_colors("mask", array.array("f", [1, 0, 0, 1] * n))
        self.assertEqual(scalp.color_set_names(), ["mask"])
        self.assertEqual(scalp.color_at("mask", hit.root), [1.0, 0.0, 0.0, 1.0])
        self.assertIsNone(scalp.corner_colors("other"))
        self.assertFalse(scalp.has_rest)
        scalp.set_rest_points(scalp.points)
        self.assertTrue(scalp.has_rest)
        with self.assertRaises(ValueError):
            scalp.set_rest_points([(0, 0, 0)])


# ---------------------------------------------------------------------------
# GuideSet and GuideHierarchy
# ---------------------------------------------------------------------------


class GuideTest(SceneMixin, unittest.TestCase):
    def test_guide_set(self):
        g = gl.GuideSet()
        g.add(5, [(0, 1.5, 0), (0, 2, 0)], {"clump": 0.5})
        g.add(6, [(0, 2, 0.1), (0, 1.5, 0.1)])  # tip first
        self.assertEqual(len(g), 2)
        g.orient_and_root(self.scalp)
        guides = g.guides
        self.assertEqual(guides[0].id, 5)
        self.assertEqual(guides[0].attributes, {"clump": 0.5})
        self.assertTrue(guides[0].root.face >= 0)
        self.assertAlmostEqual(guides[1].points[0].y, 1.0, places=2)  # flipped, rooted
        g.set_attribute(1, "clump", 0.25)
        self.assertEqual(g.guide(1).attributes["clump"], 0.25)
        with self.assertRaises(IndexError):
            g.guide(5)

    def test_hierarchy_with_children(self):
        h = self.hierarchy
        primary = h.find_group("primary")
        children = h.find_group("children")
        self.assertNotEqual(primary, gl.NO_GROUP)
        self.assertNotEqual(children, gl.NO_GROUP)
        self.assertEqual(h.find_group("nope"), gl.NO_GROUP)
        self.assertEqual(len(h.guides_in_group(primary)), 8)
        self.assertEqual(len(h.guides_in_group(children)), 32)
        self.assertEqual(h.max_depth(), 1)
        self.assertEqual(h.group_depth(children), 1)
        self.assertEqual(h.group(children).parent, primary)
        self.assertEqual(len(h), 40)
        pid = h.guides_in_group(primary)[0]
        kids = h.children_of(pid)
        self.assertEqual(len(kids), 4)
        child = h.guide(kids[0])
        self.assertTrue(child.is_child)
        self.assertTrue(child.follows_parent)
        self.assertEqual(child.parent, pid)
        self.assertEqual(len(child.offsets), len(child.points))
        self.assertIsNone(h.guide(123456))
        # Every root sits on the (triangulated) sphere.
        for g in h.guides:
            self.assertAlmostEqual(gl.Vec3(g.points[0]).length(), 1.0, delta=0.02)
        flat = h.flatten()
        self.assertEqual(len(flat), 32)  # deepest level
        self.assertEqual(len(h.flatten(0)), 8)
        self.assertEqual(len(h.flatten_group(primary)), 8)

    def test_hierarchy_edits_and_json(self):
        h = self.hierarchy.copy()
        primary = h.find_group("primary")
        pid = h.guides_in_group(primary)[0]
        before = h.guide_points(h.children_of(pid)[0])
        pts = h.guide_points(pid)
        pts[-1] = pts[-1] + gl.Vec3(0, 0.5, 0)
        h.set_points(pid, pts)
        h.evaluate(self.scalp)
        after = h.guide_points(h.children_of(pid)[0])
        self.assertNotEqual(before[-1], after[-1])  # children follow
        h.set_attribute(pid, "clump", 0.7)
        self.assertAlmostEqual(h.effective_attribute(h.children_of(pid)[0], "clump"), 0.7, places=6)
        self.assertIsNone(h.effective_attribute(pid, "nope"))
        self.assertEqual(h.attribute_names(), ["clump"])
        text = h.to_json()
        back = gl.GuideHierarchy.from_json(text)
        back.evaluate(self.scalp)  # following children are rebuilt from their parents
        self.assertEqual(back.bound_topology(), h.bound_topology())
        self.assertEqual(len(back), len(h))
        self.assertEqual(back.guide(pid).attributes, h.guide(pid).attributes)
        for a, b in zip(h.guides, back.guides):
            self.assertEqual(a.id, b.id)
            self.assertLess(max((p - q).length() for p, q in zip(a.points, b.points)), 1e-5)
        h.lock(h.children_of(pid)[0])
        self.assertTrue(h.guide(h.children_of(pid)[0]).locked)
        h.remove_guides([pid])
        self.assertEqual(len(h.guides_in_group(primary)), 7)
        self.assertEqual(len(h.guides_in_group(h.find_group("children"))), 28)
        with self.assertRaises(ValueError):
            h.rename_group(primary, "")
        with self.assertRaises(RuntimeError):
            gl.GuideHierarchy.from_json("not json")
        with self.assertRaises(KeyError):
            h.guide_points(999999)

    def test_mirror_and_transfer(self):
        h = gl.GuideHierarchy()
        g = h.add_group("primary")
        h.add_guide(g, straight((0.5, 0.85, 0), (0, 0.5, 0)))
        h.evaluate(self.scalp)
        new = h.mirror_group(g, 0, 0.0, True)
        self.assertEqual(len(new), 1)
        self.assertLess(h.guide_points(new[0])[0].x, 0.0)
        big = self.scalp.with_points([(p.x * 2, p.y * 2, p.z * 2) for p in self.scalp.points])
        h.transfer(self.scalp, big, gl.ScalpFit.bounding_boxes(self.scalp, big))
        for guide in h.guides:
            self.assertAlmostEqual(gl.Vec3(guide.points[0]).length(), 2.0, places=1)


# ---------------------------------------------------------------------------
# Brushes
# ---------------------------------------------------------------------------


class BrushTest(SceneMixin, unittest.TestCase):
    def test_brush_stroke_moves_guides(self):
        h = self.hierarchy.copy()
        pid = h.guides_in_group(h.find_group("primary"))[0]
        before = h.guide_points(pid)
        tip = before[-1]
        settings = gl.BrushSettings(gl.BrushMode.Comb, radius=0.5, strength=1.0)
        path = [tip, tip + gl.Vec3(0.3, 0, 0), tip + gl.Vec3(0.6, 0, 0)]
        changed = gl.apply_brush_stroke(h, self.scalp, settings, path)
        self.assertGreaterEqual(changed, 1)
        after = h.guide_points(pid)
        self.assertEqual(after[0], before[0])  # root pinned
        self.assertGreater(after[-1].x, before[-1].x)  # combed toward +x
        # Lengths kept.
        length = lambda pts: sum((pts[i + 1] - pts[i]).length() for i in range(len(pts) - 1))
        self.assertAlmostEqual(length(after), length(before), places=3)
        # Nothing far away changed.
        settings.radius = 0.05
        self.assertEqual(gl.apply_brush_stroke(h, self.scalp, settings, [(0, -3, 0), (0, -3.1, 0)]), 0)

    def test_brush_modes_and_mask(self):
        h = self.hierarchy.copy()
        ids = h.guides_in_group(h.find_group("primary"))
        settings = gl.BrushSettings()
        settings.mode = gl.BrushMode.Attribute
        settings.attribute = "clump"
        settings.value = 1.0
        settings.radius = 5.0
        settings.mask = {ids[0]: 1.0}
        self.assertEqual(settings.mask, {ids[0]: 1.0})
        changed = gl.apply_brush_stamp(h, self.scalp, settings, (0, 1, 0))
        self.assertEqual(changed, 1)
        self.assertIn("clump", h.guide(ids[0]).attributes)
        self.assertNotIn("clump", h.guide(ids[1]).attributes)
        settings.mask = None
        self.assertIsNone(settings.mask)
        self.assertEqual(gl.brush_mode_from_string("comb"), gl.BrushMode.Comb)
        self.assertEqual(gl.brush_mode_name(gl.BrushMode.Slide), "slide")
        with self.assertRaises(ValueError):
            gl.brush_mode_from_string("nope")
        # A custom weight callable.
        weighted = gl.apply_brush_stamp(h, self.scalp, gl.BrushSettings(gl.BrushMode.Lift, 1.0, 0.5), (0, 1, 0),
                                        weight=lambda p: 1.0 if p.y > 0.9 else 0.0)
        self.assertGreater(weighted, 0)

    def test_place_and_draw(self):
        h = self.hierarchy.copy()
        place = gl.PlaceSettings()
        place.group = "primary"
        at = self.scalp.closest_point((0, 0, 1.5))
        new = gl.place_guide(h, self.scalp, place, at)
        self.assertNotEqual(new, gl.NO_GUIDE)
        self.assertEqual(h.guide(new).group, h.find_group("primary"))
        drawn = gl.draw_guide(h, self.scalp, place, [(0, 0, -1), (0, 0.2, -1.2), (0, 0.4, -1.4), (0, 0.6, -1.6)])
        self.assertAlmostEqual(gl.Vec3(h.guide_points(drawn)[0]).length(), 1.0, places=1)
        many = gl.place_guides(h, self.scalp, place, [(1, 0, 0), (1, 0.01, 0), (0, 0, 1)])
        self.assertEqual(len(many), 1)  # the others are too close to existing roots
        with self.assertRaises(ValueError):
            gl.draw_guide(h, self.scalp, place, [(0, 0, -1)])

    def test_selection_helpers(self):
        h = self.hierarchy
        primary = gl.guides_at(h, 0)
        self.assertEqual(len(primary), 8)
        self.assertEqual(len(gl.guides_at(h, -1)), 40)
        self.assertEqual(len(gl.guides_at(h, 0, "children")), 32)
        soft = gl.soft_selection(h, [primary[0]], 0.8)
        self.assertEqual(soft[primary[0]], 1.0)
        self.assertGreater(len(soft), 1)
        mirrored = gl.mirror_selection(h, [primary[0]], 2, 0.0, 0.5)
        self.assertEqual(len(mirrored), 1)
        lasso = gl.guides_in_lasso(h, primary, lambda p: (p.x, p.z), [(-2, -2), (2, -2), (2, 2), (-2, 2)])
        self.assertEqual(len(lasso), 8)


# ---------------------------------------------------------------------------
# Graph
# ---------------------------------------------------------------------------


class GraphTest(SceneMixin, unittest.TestCase):
    def test_strand_resolution_is_the_branchs(self):
        # The default graph's Resample, after the Scatter, sets its resolution; the graph editors' note says so.
        doc = gl.load_graph_json(gl.default_graph_json())
        scatter = [n for n in doc.names if doc.op(n).type_name == "Scatter"][0]
        resample = [n for n in doc.names if doc.op(n).type_name == "Resample"][0]
        segments, source = doc.strand_resolution(scatter)
        self.assertEqual(source, resample)
        self.assertGreater(segments, 0)
        self.assertIn("set by Resample '%s'" % resample, doc.strand_resolution_note(scatter))
        self.assertIn("Scatter '%s'" % scatter, doc.strand_resolution_note(resample))

    def test_default_graph_and_specs(self):
        text = gl.default_graph_json()
        doc = json.loads(text)
        self.assertEqual(doc["version"], 1)
        self.assertEqual(gl.default_graph(), doc)
        names = {n["type"] for n in doc["nodes"]}
        self.assertTrue({"Scalp", "Scatter", "Interpolate", "Output"} <= names)
        specs = gl.operator_specs()
        self.assertEqual(json.loads(gl.operator_specs_json()), specs)
        types = {s["type"] for s in specs}
        self.assertTrue({"Clump", "Noise", "Cards"} <= types)
        self.assertEqual(set(types), set(gl.operator_type_names()))
        self.assertIn("Scatter", gl.OPERATORS)
        reg = gl.OperatorRegistry.instance()
        self.assertTrue(reg.contains("Scatter"))
        op = reg.create("Clump")
        self.assertEqual(op.type_name, "Clump")
        self.assertEqual(op.category, "look")
        kinds = {s.name: s.kind for s in op.param_specs}
        self.assertEqual(kinds["strength"], "float")
        self.assertEqual(op.get_param("strength"), 0.7)
        self.assertIsInstance(op.params.get_ramp("profile"), gl.Ramp)

    def test_new_nodes_and_params_in_the_specs(self):
        # Gravity, Displace and Mirror (shape nodes) and the new params, every spec described.
        specs = {s["type"]: s for s in gl.operator_specs()}
        for t in ("Gravity", "Displace", "Mirror"):
            self.assertEqual(specs[t]["role"], "shape")  # the operator's own; the menu's is the catalog's
            self.assertEqual(specs[t]["output"]["type"], "Curves")  # strands or guides, as fed
        self.assertEqual(specs["Gravity"]["category"], "deform")
        self.assertEqual(specs["Mirror"]["category"], "shape")
        self.assertEqual(specs["Reference"]["label"], "Linked Groom")
        for t, spec in specs.items():
            self.assertTrue(spec["description"], t)
        params = {t: {p["name"]: p for p in s["params"]} for t, s in specs.items()}
        self.assertEqual(params["Curl"]["shape"]["choices"], ["helix", "wave", "zigzag", "coil"])
        self.assertEqual(params["Curl"]["shape"]["default"], "helix")
        self.assertEqual(params["Curl"]["phaseRandom"]["default"], 1.0)
        self.assertEqual(params["Curl"]["coherence"]["default"], "strand")
        self.assertEqual(params["Clump"]["clumpBy"]["default"], "count")
        self.assertEqual(params["Clump"]["count"]["showWhen"], {"param": "clumpBy", "in": ["count"]})
        self.assertEqual(params["Clump"]["clumpBy"]["showWhen"], {"param": "centersFrom", "in": ["pick"]})
        self.assertEqual(params["Clump"]["centersFrom"]["choices"], ["pick", "inputPoints", "inputCurves"])
        # A link drawn by hand into a picking Clump's centers sets it to take them; plain connect does not.
        edit = gl.GraphEdit(json.dumps({"version": 1, "nodes": [
            {"name": "s", "type": "Scatter"}, {"name": "c", "type": "Clump"}, {"name": "g", "type": "ToGuides"}],
            "connections": [{"from": "s", "to": "c", "input": "strands"}, {"from": "s", "to": "g", "input": "strands"}]}))
        edit.connect("g", "c", "centers")
        self.assertNotEqual(json.loads(edit.to_json())["nodes"][1].get("params", {}).get("centersFrom"), "inputPoints")
        self.assertTrue(edit.connect_by_hand("g", "c", "centers"))
        self.assertEqual(json.loads(edit.to_json())["nodes"][1]["params"]["centersFrom"], "inputPoints")
        self.assertFalse(edit.connect_by_hand("g", "c", "centers"))  # already taking them
        # From a Guides node (through a curve node after it too): each clump along its guide.
        for source in ("guides", "wave"):
            edit = gl.GraphEdit(json.dumps({"version": 1, "nodes": [
                {"name": "s", "type": "Scatter"}, {"name": "c", "type": "Clump"}, {"name": "guides", "type": "Guides"},
                {"name": "wave", "type": "Noise"}],
                "connections": [{"from": "s", "to": "c", "input": "strands"}, {"from": "guides", "to": "wave", "input": "strands"}]}))
            self.assertTrue(edit.connect_by_hand(source, "c", "centers"), source)
            self.assertEqual(json.loads(edit.to_json())["nodes"][1]["params"]["centersFrom"], "inputCurves", source)
        self.assertEqual(params["Clump"]["flatness"]["default"], 0.0)
        self.assertEqual(params["Noise"]["mode"]["default"], "smooth")
        self.assertEqual(params["Width"]["randomness"]["default"], 0.0)
        self.assertEqual(params["Collide"]["method"]["default"], "field")
        self.assertEqual(params["Mirror"]["mode"]["choices"], ["replace", "add"])
        self.assertTrue(params["Gravity"]["strength"]["mappable"])
        # A graph with them evaluates through the bindings: Gravity, a kinky Noise, a coil (after
        # Width), then Mirror doubling the strands from one side.
        graph = {"version": 1, "output": "mirror", "nodes": [
            {"name": "scalp", "type": "Scalp"}, {"name": "scatter", "type": "Scatter", "params": {"density": 200.0}},
            {"name": "gravity", "type": "Gravity"}, {"name": "kinks", "type": "Noise", "params": {"mode": "kinky"}},
            {"name": "width", "type": "Width"}, {"name": "coil", "type": "Curl", "params": {"shape": "coil"}},
            {"name": "mirror", "type": "Mirror", "params": {"mode": "add"}}],
            "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                            {"from": "scatter", "to": "gravity", "input": "strands"},
                            {"from": "gravity", "to": "kinks", "input": "strands"},
                            {"from": "kinks", "to": "width", "input": "strands"},
                            {"from": "width", "to": "coil", "input": "strands"},
                            {"from": "coil", "to": "mirror", "input": "strands"}]}
        doc = gl.load_graph_json(graph)
        externals = gl.ExternalInputs()
        externals.set_groom(scalp=self.scalp, hierarchy=self.hierarchy)
        ctx = gl.EvalContext(externals=externals)
        out = doc.evaluate(ctx)
        grown = doc.graph.evaluate(doc.node("scatter"), ctx)
        self.assertGreater(out.strand_count, grown.strand_count)
        self.assertLessEqual(out.strand_count, 2 * grown.strand_count)
        self.assertTrue(out.is_valid())

    def test_document_from_json_evaluates(self):
        doc = gl.load_graph_json(LOOK_GRAPH)  # a dict is accepted too
        self.assertEqual(set(doc.names), {n["name"] for n in LOOK_GRAPH["nodes"]})
        self.assertEqual(doc.output, doc.node("output"))
        self.assertEqual(doc.name_of(doc.node("scatter")), "scatter")
        externals = gl.ExternalInputs()
        externals.set_groom(scalp=self.scalp, hierarchy=self.hierarchy)
        self.assertIsInstance(externals.find("scalp"), gl.ScalpMesh)
        self.assertIsInstance(externals.find("hierarchy"), gl.GuideHierarchy)
        self.assertIsNone(externals.find("missing"))
        full = gl.EvalContext(preview_density=1.0, externals=externals)
        strands = doc.evaluate(full)
        self.assertGreater(strands.strand_count, 500)
        self.assertTrue(strands.is_valid())
        preview = gl.EvalContext(preview_density=0.1, externals=externals)
        low = doc.evaluate(preview)
        self.assertLess(low.strand_count, strands.strand_count * 0.25)
        self.assertTrue(set(low.ids) <= set(strands.ids))
        # Guides node yields a GuideSet value; Scalp a mesh.
        self.assertIsInstance(doc.graph.evaluate(doc.node("guides"), full), gl.GuideSet)
        self.assertIsInstance(doc.graph.evaluate(doc.node("scalp"), full), gl.ScalpMesh)
        times = dict(doc.node_times())
        self.assertIn("scatter", times)
        self.assertEqual(doc.graph.output_type(doc.node("scatter")), gl.PortType.Strands)
        self.assertEqual(json.loads(doc.to_json())["output"], "output")
        self.assertEqual(json.loads(gl.save_graph_json(doc))["output"], "output")

    def test_param_tweak_reevaluates_only_downstream(self):
        doc = gl.load_graph_json(LOOK_GRAPH)
        externals = gl.ExternalInputs()
        externals.set_groom(scalp=self.scalp, hierarchy=self.hierarchy)
        ctx = gl.EvalContext(externals=externals)
        g = doc.graph
        doc.evaluate(ctx)
        first = g.evaluation_count
        self.assertEqual(first, len(doc.names))  # every node ran once
        doc.evaluate(ctx)
        self.assertEqual(g.evaluation_count, first)  # all cached
        # Width is the last node before Output: only Width and Output re-run.
        width = doc.op("width")
        self.assertIn("width", [s.name for s in width.param_specs])
        width.set_param("width", 0.02)
        self.assertEqual(width.get_param("width"), 0.02)
        doc.evaluate(ctx)
        self.assertEqual(g.evaluation_count, first + 2)
        # Length is upstream of Width: Length, Width, Output re-run; Scatter and Interpolate do not.
        doc.op("length").params["scale"] = 0.5
        doc.evaluate(ctx)
        self.assertEqual(g.evaluation_count, first + 5)
        # Coercion: an int for a float param stays a float; a number for a ramp becomes a constant ramp.
        width.set_param("width", 1)
        self.assertIsInstance(width.get_param("width"), float)
        clump = gl.OperatorRegistry.instance().create("Clump")
        clump.set_param("profile", 0.5)
        self.assertIsInstance(clump.get_param("profile"), gl.Ramp)
        self.assertEqual(clump.get_param("profile")(0.3), 0.5)

    def test_building_a_graph_by_hand(self):
        doc = gl.GraphDocument()  # the default graph
        self.assertIn("scatter", doc.names)
        g = gl.Graph()
        scalp = g.add_node("Scalp")
        scatter = g.add_node("Scatter")
        out = g.add_node("Output")
        g.connect(scalp, scatter, "scalp")
        g.connect(scatter, out, 0)
        g.node(scatter).set_param("density", 200.0)
        g.set_label(scatter, "scatter")
        self.assertEqual(g.label(scatter), "scatter")
        self.assertEqual(g.input_source(out, 0), scatter)
        self.assertEqual(g.input_source(scatter, "scalp"), scalp)
        noise = g.add_node("Noise")
        self.assertIsNone(g.input_source(noise, "strands"))
        g.remove_node(noise)
        self.assertEqual(len(g), 3)
        self.assertEqual([p.name for p in g.input_specs(scatter)][0], "scalp")
        externals = gl.ExternalInputs()
        externals.set_groom(scalp=self.scalp)
        ctx = gl.EvalContext(externals=externals)
        strands = g.evaluate_strands(out, ctx)
        self.assertGreater(strands.strand_count, 100)
        self.assertEqual(sorted(g.last_evaluation_times()), sorted([scalp, scatter, out]))
        self.assertIsNotNone(g.cached_channels(scatter))
        self.assertIsNone(g.cached_channels(scalp))
        # Exposed params: a Field node driving Scatter's density.
        field = g.add_node("Expression")
        g.node(field).set_param("expression", "0.5")
        g.set_param_inputs(scatter, ["density"])
        self.assertEqual(g.param_inputs(scatter), ["density"])
        self.assertEqual(g.input_specs(scatter)[-1].type, gl.PortType.Field)
        g.connect(field, scatter, "density")
        fewer = g.evaluate_strands(out, ctx)
        self.assertLess(fewer.strand_count, strands.strand_count)
        self.assertIsInstance(g.evaluate(field, ctx), gl.FieldValue)
        g.set_bypass(scatter, True)
        self.assertTrue(g.bypassed(scatter))
        g.disconnect(scatter, "density")
        g.remove_node(field)
        self.assertFalse(g.has_node(field))
        g.set_bypass(scatter, False)
        # A Value node sets any other param for the whole node (a Value port): Scatter's seed.
        value = g.add_node("Value")
        g.node(value).set_param("kind", "integer")
        g.node(value).set_param("integer", 3)
        self.assertEqual(g.output_specs(value)[0].type, gl.PortType.Value)
        self.assertEqual(g.output_specs(value)[0].value_kind, gl.ValueKind.Integer)
        self.assertEqual(g.evaluate(value, ctx), 3)
        g.set_param_inputs(scatter, ["seed"])
        self.assertEqual(g.input_specs(scatter)[-1].type, gl.PortType.Value)
        self.assertEqual(g.input_specs(scatter)[-1].value_kind, gl.ValueKind.Integer)
        g.connect(value, scatter, "seed")
        self.assertTrue(g.param_driven(scatter, "seed"))
        g.evaluate_strands(out, ctx)
        self.assertEqual(g.node(scatter).get_param("seed"), 3)
        noise = g.add_node("FieldNoise")
        with self.assertRaises(gl.GraphError):
            g.connect(noise, scatter, "seed")  # a value per strand cannot set one value
        g.disconnect(scatter, "seed")
        self.assertEqual(g.node(scatter).get_param("seed"), 0)  # its own value back
        g.set_param_inputs(scatter, [])
        g.remove_node(value)
        g.remove_node(noise)
        # Named helpers on a document.
        doc2 = gl.load_graph_json("")
        nid = doc2.add_node("wave", "Noise")
        doc2.connect("width", "wave", "strands")
        doc2.disconnect("output", "strands")
        doc2.connect("wave", "output", "strands")
        self.assertEqual(doc2.node("wave"), nid)
        doc2.rename_node("wave", "wobble")
        self.assertTrue(doc2.has("wobble"))
        self.assertEqual(json.loads(doc2.to_json())["nodes"][-1]["name"], "wobble")

    def test_links_pick_an_output(self):
        # A Map node has five outputs; a link reads the one it names (index or name).
        g = gl.Graph()
        mask = g.add_node("Map")
        self.assertEqual([p.name for p in g.node(mask).outputs], ["luminance", "r", "g", "b", "a", "rgb"])
        self.assertEqual(g.node(mask).output.name, "luminance")
        self.assertEqual([p.name for p in g.output_specs(mask)][2], "g")
        self.assertEqual(g.output_index(mask, "b"), 3)
        self.assertEqual(g.output_type(mask, 4), gl.PortType.Field)
        length = g.add_node("Length")
        g.set_param_inputs(length, ["keep"])
        g.connect(mask, length, "keep", "g")
        self.assertEqual(g.input_source(length, "keep"), mask)
        self.assertEqual(g.input_output(length, "keep"), 2)
        g.connect(mask, length, "keep")  # the first
        self.assertEqual(g.input_output(length, "keep"), 0)
        with self.assertRaises(gl.GraphError):
            g.connect(mask, length, "keep", "q")
        with self.assertRaises(gl.GraphError):
            g.connect(mask, length, "strands", "r")  # a Field into strands
        self.assertIsNone(g.input_output(length, "strands"))
        doc = gl.load_graph_json("")
        doc.add_node("mask", "Map")
        doc.graph.set_param_inputs(doc.node("width"), ["width"])
        doc.connect("mask", "width", "width", output="a")
        self.assertIn({"from": "mask", "to": "width", "input": "width", "output": "a"},
                      json.loads(doc.to_json())["connections"])
        with self.assertRaises(gl.GraphError):
            doc.graph.evaluate_output(doc.node("mask"), 7)

    def test_exceptions_map(self):
        with self.assertRaises(gl.GraphError):
            gl.load_graph_json("{not json")
        with self.assertRaises(gl.GraphError):
            gl.load_graph_json({"version": 1, "nodes": [{"name": "x", "type": "NoSuchOperator"}], "connections": [], "output": "x"})
        self.assertTrue(issubclass(gl.GraphError, RuntimeError))
        g = gl.Graph()
        with self.assertRaises(gl.GraphError):
            g.add_node("NoSuchOperator")
        a = g.add_node("Scalp")
        b = g.add_node("Scatter")
        with self.assertRaises(gl.GraphError):
            g.connect(b, a, 0)  # Scalp has no inputs
        with self.assertRaises(gl.GraphError):
            g.connect(a, b, "nope")
        with self.assertRaises(gl.GraphError):
            g.node(b).params.get("nope")
        with self.assertRaises(TypeError):
            g.node(b).set_param("density", object())
        doc = gl.load_graph_json(LOOK_GRAPH)
        with self.assertRaises(gl.GraphError):
            doc.node("nope")
        with self.assertRaises(gl.GraphError):
            doc.graph.node(999999)
        with self.assertRaises(gl.GraphError):
            doc.graph.evaluate(999999)
        with self.assertRaises(gl.GroomFileError):
            gl.GroomFile.from_json("{not json")
        with self.assertRaises(gl.GroomFileError):
            gl.GroomFile.read(os.path.join(tempfile.gettempdir(), "does-not-exist.groom"))
        with self.assertRaises(gl.TextureError):
            gl.load_texture(os.path.join(tempfile.gettempdir(), "does-not-exist.png"))
        self.assertTrue(issubclass(gl.TextureError, RuntimeError))
        with self.assertRaises(ValueError):
            gl.flow_sink_angle("nope")
        with self.assertRaises(ValueError):
            gl.PartSet().add("part", [(0, 1, 0)], self.scalp)
        if gl.has_alembic:
            with self.assertRaises(gl.AlembicError):
                gl.read_alembic_curves(os.path.join(tempfile.gettempdir(), "does-not-exist.abc"))

    def test_param_values_round_trip(self):
        op = gl.OperatorRegistry.instance().create("Scatter")
        p = op.params
        p.set("flag", True)
        p.set("count", 3)
        p.set("ratio", 0.5)
        p.set("name", "abc")
        p.set("dir", (1, 2, 3))
        p.set("ramp", gl.Ramp.linear(0, 1))
        p.set("map", gl.MapRef.attribute("melanin"))
        self.assertIs(p.get("flag"), True)
        self.assertEqual(p.get("count"), 3)
        self.assertIsInstance(p.get("count"), int)
        self.assertEqual(p.get("ratio"), 0.5)
        self.assertEqual(p.get("name"), "abc")
        self.assertEqual(p.get("dir"), (1.0, 2.0, 3.0))
        self.assertIsInstance(p.get("dir"), gl.Vec3)
        self.assertIsInstance(p["ramp"], gl.Ramp)
        self.assertEqual(p["map"].name, "melanin")
        self.assertIn("count", p)
        self.assertEqual(p.get_float("count"), 3.0)
        self.assertEqual(set(p.keys()) & {"flag", "count", "density"}, {"flag", "count", "density"})
        self.assertEqual(p.values()["name"], "abc")
        h1 = p.hash()
        p["count"] = 4
        self.assertNotEqual(p.hash(), h1)


# ---------------------------------------------------------------------------
# Groom and groom files
# ---------------------------------------------------------------------------


class GroomTest(SceneMixin, unittest.TestCase):
    def groom_data(self, scalp=None):
        return gl.GroomData(LOOK_GRAPH, self.hierarchy.to_json(), scalp or self.scalp)

    def test_evaluate_at_densities(self):
        groom = gl.Groom(self.groom_data())
        full = groom.evaluate(1.0, 0.0)
        low = groom.evaluate(0.1, 0.0)
        self.assertGreater(full.strand_count, 500)
        self.assertLess(low.strand_count, full.strand_count * 0.25)
        self.assertGreater(low.strand_count, 0)
        self.assertTrue(set(low.ids) <= set(full.ids))
        self.assertEqual(full.uniform_point_count(), 9)  # 8 segments by default
        times = dict(groom.node_times())
        self.assertIn("interpolate", times)
        h = groom.hierarchy()
        self.assertIsInstance(h, gl.GuideHierarchy)
        self.assertEqual(len(h), 40)
        self.assertEqual(groom.data.graph_json, json.dumps(LOOK_GRAPH))

    def test_scalp_and_poses(self):
        groom = gl.Groom(self.groom_data())
        before = groom.evaluate(1.0, 0.0)
        poses = gl.GuideSet()
        pid = self.hierarchy.guides_in_group(self.hierarchy.find_group("primary"))[0]
        pts = self.hierarchy.guide_points(pid)
        poses.add(pid, [p + gl.Vec3(0, 0.3 * i / (len(pts) - 1), 0) for i, p in enumerate(pts)])
        groom.set_guide_poses(poses)
        after = groom.evaluate(1.0, 0.0)
        self.assertEqual(before.strand_count, after.strand_count)
        self.assertNotEqual(before.content_hash(), after.content_hash())
        moved = self.scalp.with_points([(p.x * 1.2, p.y, p.z) for p in self.scalp.points])
        moved.set_rest_points(self.scalp.points)
        groom.set_scalp(moved)
        deformed = groom.evaluate(1.0, 0.0)
        self.assertEqual(deformed.ids, after.ids)  # the rest pose keeps the same strands
        groom.set_controls({"amount": 0.5})
        groom.set_search_paths([tempfile.gettempdir()])
        groom.set_cache_intermediates(False)
        self.assertEqual(groom.evaluate(1.0, 0.0).strand_count, deformed.strand_count)

    def test_groom_file_round_trip(self):
        data = self.groom_data()
        data.parts_json = gl.PartSet().to_json()
        file = gl.GroomFile(data)
        file.frames_per_second = 30.0
        file.source = "test"
        file.search_paths = ["$PROJECT/maps"]
        file.scalp_cache = gl.CacheRef("scalp.abc", "headShape")
        text = file.to_json()
        back = gl.GroomFile.from_json(text)
        self.assertEqual(back.frames_per_second, 30.0)
        self.assertEqual(back.source, "test")
        self.assertEqual(back.search_paths, ["$PROJECT/maps"])
        self.assertEqual(back.scalp_cache.file, "scalp.abc")
        self.assertEqual(back.data.scalp.content_hash(), self.scalp.content_hash())
        self.assertEqual(back.data.hierarchy_json, self.hierarchy.to_json())
        original = gl.Groom(data).evaluate(1.0, 0.0)
        reread = gl.Groom(back.data).evaluate(1.0, 0.0)
        self.assertEqual(original.content_hash(), reread.content_hash())
        # The up axis and the card bake travel too; a Z-up file turns Y-up.
        self.assertEqual((back.units, back.up_axis), ("cm", "Y"))
        self.assertTrue(back.card_bake.empty)
        file.up_axis = "Z"
        file.card_bake = gl.CardBakeRef("test_cards/test_", "cards:1")
        zup = gl.GroomFile.from_json(file.to_json())
        self.assertEqual(zup.up_axis, "Z")
        self.assertEqual((zup.card_bake.prefix, zup.card_bake.strands), ("test_cards/test_", "cards:1"))
        gl.convert_up_axis(zup, "Y")
        self.assertEqual(zup.up_axis, "Y")
        p, q = self.scalp.points[0], zup.data.scalp.points[0]
        self.assertEqual((q.x, q.y, q.z), (p.x, p.z, -p.y))
        with self.assertRaises(gl.GroomFileError):
            gl.convert_up_axis(zup, "up")
        file.up_axis = "Y"
        file.card_bake = gl.CardBakeRef()
        folder = tempfile.mkdtemp(prefix="groomlab_py_")
        try:
            path = os.path.join(folder, "test.groom")
            file.write(path)
            disk = gl.GroomFile.read(path)
            self.assertEqual(os.path.normcase(disk.directory), os.path.normcase(folder))
            self.assertEqual(disk.scalp_cache.object, "headShape")
            self.assertEqual(gl.Groom(disk.data).evaluate(1.0, 0.0).content_hash(), original.content_hash())
            self.assertEqual(gl.read_groom_file(path).frames_per_second, 30.0)
            # A session of the file runs at its frame rate, as its scene and the C API's do.
            self.assertEqual(gl.GroomSession.from_groom_file(path).frames_per_second, 30.0)
            if gl.has_alembic:
                with self.assertRaises(gl.AlembicError):
                    gl.GroomScene(path)  # the scalp cache it names does not exist
                file.scalp_cache = gl.CacheRef()
                file.write(path)
                scene = gl.GroomScene(path)
                self.assertEqual(scene.frames_per_second, 30.0)
                self.assertEqual(scene.evaluate(0.0, 1.0).content_hash(), original.content_hash())
                self.assertEqual(scene.scalp_at(0.0).content_hash(), self.scalp.content_hash())
                self.assertEqual(scene.node_notes(), [])
                # A Jiggle with no scalp cache to take motion from: the strands as they were, and a note.
                graph = json.loads(file.data.graph_json)
                graph["nodes"].append({"name": "jiggle", "type": "Jiggle"})
                last = next(c for c in graph["connections"] if c["to"] == graph["output"])
                graph["connections"].append({"from": last["from"], "to": "jiggle", "input": "strands"})
                last["from"] = "jiggle"
                file.data.graph_json = json.dumps(graph)
                file.write(path)
                scene = gl.GroomScene(path)
                self.assertEqual(scene.evaluate(0.0, 1.0).content_hash(), original.content_hash())
                self.assertEqual([n for n, _ in scene.node_notes()], ["jiggle"])
                self.assertIn("no scalp motion history", scene.node_notes()[0][1])
        finally:
            shutil.rmtree(folder, ignore_errors=True)


# ---------------------------------------------------------------------------
# Parts, flow, correctives, LOD, cards, USD, Alembic
# ---------------------------------------------------------------------------


class ExtrasTest(SceneMixin, unittest.TestCase):
    def test_parts(self):
        parts = gl.PartSet()
        name = parts.add("part", [(0, 1, -0.5), (0, 1, 0), (0, 1, 0.5)], self.scalp)
        self.assertEqual(name, "part")
        self.assertEqual(parts.add("part", [(0, 1, -0.5), (0, 1, 0.5)], self.scalp), "part1")
        self.assertEqual(len(parts), 2)
        self.assertTrue(parts.find("part").points[0].face >= 0)
        lines = gl.PartLines(parts, self.scalp)
        self.assertEqual(lines.names, ["part", "part1"])
        left = lines.side_of((-0.3, 0.95, 0))
        right = lines.side_of((0.3, 0.95, 0))
        self.assertTrue(gl.PartLines.separated(left, right))
        self.assertEqual(gl.PartSet.from_json(parts.to_json()).content_hash(), parts.content_hash())
        externals = gl.ExternalInputs()
        externals.set_groom(scalp=self.scalp, parts=lines)
        self.assertIsInstance(externals.find("parts"), gl.PartLines)

    def test_auto_parts(self):
        # A sphere welded at its poles (on the x axis), strands on its cap combed apart at x = 0.
        rings, segments = 24, 48
        points = [(-1.0, 0.0, 0.0)]
        for r in range(1, rings):
            th = math.pi * r / rings
            for s in range(segments):
                ph = 2 * math.pi * s / segments
                points.append((-math.cos(th), math.sin(th) * math.cos(ph), math.sin(th) * math.sin(ph)))
        points.append((1.0, 0.0, 0.0))

        def at(r, s):
            return 1 + (r - 1) * segments + s % segments

        tris = []
        for s in range(segments):
            tris += [(0, at(1, s + 1), at(1, s)), (len(points) - 1, at(rings - 1, s), at(rings - 1, s + 1))]
            for r in range(1, rings - 1):
                tris += [(at(r, s), at(r, s + 1), at(r + 1, s)), (at(r, s + 1), at(r + 1, s + 1), at(r + 1, s))]
        head = gl.ScalpMesh(points, tris)
        strands = gl.StrandSet()
        count = 3000
        for i in range(count):
            y = 1 - 2 * (i + 0.5) / count
            if y < 0.3:
                continue
            a = 2.39996323 * i
            sp = head.closest_point((math.sqrt(1 - y * y) * math.cos(a), y, math.sqrt(1 - y * y) * math.sin(a)))
            p = sp.position
            side = gl.Vec3(-1.0 if p.x < 0 else 1.0, 0.0, 0.3)
            d = (side - p * p.dot(side)).normalized() + p * 0.3
            strands.add_strand(i + 1, [p + d * (0.05 * k) for k in range(7)], None, sp.root)
        found = gl.auto_parts(strands, head, 90.0, 0.15)
        self.assertEqual(len(found), 1)
        lines = gl.PartLines(found, head)
        self.assertEqual(lines.names, ["auto1"])
        self.assertTrue(all(abs(q.x) < 0.06 for q in lines.lines[0]))
        # The strands as guides, interpolated, into a Part node in flow mode: part_lines gives the
        # lines it opens (what a host draws for it), found in the strands it takes.
        guides = gl.GuideSet()
        for s in range(0, strands.strand_count, 4):
            guides.add(s + 1, strands.strand_points(s))
        guides.root_on_mesh(head)
        doc = gl.load_graph_json({
            "version": 1,
            "nodes": [{"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
                      {"name": "scatter", "type": "Scatter", "params": {"density": 600.0}},
                      {"name": "interpolate", "type": "Interpolate"},
                      {"name": "part", "type": "Part", "params": {"partsFrom": "auto"}}, {"name": "output", "type": "Output"}],
            "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                            {"from": "scatter", "to": "interpolate", "input": "strands"},
                            {"from": "guides", "to": "interpolate", "input": "guides"},
                            {"from": "scalp", "to": "interpolate", "input": "scalp"},
                            {"from": "interpolate", "to": "part", "input": "strands"},
                            {"from": "part", "to": "output", "input": "strands"}],
            "output": "output"})
        ext = gl.ExternalInputs()
        ext.set_groom(scalp=head, guides=guides)
        ctx = gl.EvalContext(externals=ext)
        drawn = doc.graph.part_lines(doc.nodes_by_name["part"], ctx)
        self.assertIsInstance(drawn, gl.PartLines)
        interpolated = doc.graph.evaluate_strands(doc.nodes_by_name["interpolate"], ctx)
        self.assertEqual(drawn.content_hash(), gl.PartLines(gl.auto_parts(interpolated, head, 90.0, 0.15), head).content_hash())
        self.assertTrue(all(abs(q.x) < 0.1 for q in drawn.lines[0]))

    def test_flow(self):
        flow = gl.FlowSet()
        at = self.scalp.closest_point((0, 0.7, 0.7))  # off the pole, where triangles are degenerate
        flow.add_pin(self.scalp, at, (1, 0, 0))
        flow.add_sink(self.scalp.closest_point((0, 0, 1)).root, gl.flow_sink_angle("whorl"), 0.5)
        self.assertEqual(len(flow.pins), 1)
        self.assertEqual(len(flow.sinks), 1)
        field = gl.FlowField(flow, self.scalp)
        self.assertEqual(len(field.positions), 1)
        near = self.scalp.closest_point((0.1, 0.7, 0.7))  # beside the pin (exactly at it, the weights overflow)
        d = field.at(near.position, near.normal)
        self.assertGreater(d.x, 0.9)
        self.assertAlmostEqual(d.dot(near.normal), 0.0, places=4)  # tangent to the surface
        self.assertEqual(gl.FlowSet.from_json(flow.to_json()).content_hash(), flow.content_hash())
        x, y, z = gl.surface_frame(self.scalp, at.root)
        self.assertAlmostEqual(z.dot(at.normal), 1.0, places=3)
        with self.assertRaises(ValueError):
            gl.FlowSet.make_pin(self.scalp, at, at.normal)

    def test_correctives(self):
        posed = self.hierarchy.copy()
        sculpted = posed.copy()
        pid = posed.guides_in_group(posed.find_group("primary"))[0]
        pts = sculpted.guide_points(pid)
        pts[-1] = pts[-1] + gl.Vec3(0, 0.3, 0)
        sculpted.set_points(pid, pts)
        sculpted.evaluate(self.scalp)
        c = gl.Correctives()
        got = c.record("fix", posed, sculpted, 1.0, self.scalp)
        self.assertIn(pid, got)
        layer = c.layer("fix")
        self.assertEqual(layer.time, 1.0)
        self.assertAlmostEqual(layer.weight_at(1.0), 1.0)
        self.assertAlmostEqual(layer.weight_at(10.0), 0.0)
        applied = posed.copy()
        c.apply(applied, 1.0, self.scalp)
        self.assertAlmostEqual((applied.guide_points(pid)[-1] - pts[-1]).length(), 0.0, places=2)
        back = gl.Correctives.from_json(c.to_json())
        self.assertEqual(len(back.layers), 1)
        self.assertEqual(back.layers[0].name, "fix")
        layer.weight = 0.5
        self.assertEqual(c.layer("fix").weight, 0.5)  # edited in place

    def test_lod(self):
        s = gl.StrandSet()
        for i in range(400):
            x = (i % 20) * 2.0 - 20.0
            z = -(i // 20) * 5.0 - 1.0
            s.add_strand(i, [(x, 0, z), (x, 1, z)], widths=[0.1, 0.1])
        cam = gl.LodCamera()
        cam.tan_half_fov_x = cam.tan_half_fov_y = 0.5
        self.assertEqual(cam.matrix[3], [0.0, 0.0, 0.0, 1.0])
        settings = gl.LodSettings()
        settings.frustum = True
        settings.margin = 0.0
        plan = gl.plan_lod(s, cam, settings)
        self.assertGreater(plan.culled, 0)
        self.assertEqual(len(plan.kept) + plan.culled, 400)
        settings.distance = True
        settings.near_distance = 10.0
        settings.far_distance = 60.0
        settings.min_density = 0.2
        plan = gl.plan_lod(s, cam, settings)
        self.assertGreater(plan.thinned, 0)
        out = gl.apply_lod(s, plan)
        self.assertEqual(out.strand_count, len(plan.kept))
        self.assertGreater(max(out.widths), 0.1)  # far survivors widened
        cam.matrix = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 100, 1]
        self.assertEqual(cam.matrix[3][2], 100.0)

    def test_card_presets(self):
        # The levels of detail Maya's cards_preset and the app's Cards panel share (card_presets.h).
        presets = gl.card_lod_presets()
        self.assertEqual(list(presets), ["hero", "mid", "low"])
        self.assertEqual((presets["hero"]["segments"], presets["hero"]["variations"]), (10, 32))
        self.assertEqual(presets["low"]["segments"], 5)
        self.assertEqual(gl.feather_card_lod_presets()["mid"]["featherSegments"], 3)
        params = gl.card_preset_params("mid", "hair")
        self.assertEqual(params["maxCards"], 15000 // (2 * 8 * 2))
        self.assertIsInstance(params["widthScale"], float)
        self.assertEqual(gl.card_preset_params("low", "hair", 1000)["maxCards"], 100)
        self.assertIn("featherSpine", gl.card_preset_params("hero", "both"))
        with self.assertRaises(ValueError):
            gl.card_preset_params("ultra")
        graph = json.dumps({"version": 1, "output": "out", "nodes": [
            {"name": "scalp", "type": "Scalp"}, {"name": "scatter", "type": "Scatter"},
            {"name": "cards", "type": "Cards"}, {"name": "out", "type": "Output"}],
            "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                            {"from": "scatter", "to": "cards", "input": "strands"},
                            {"from": "cards", "to": "out", "input": "strands"}]})
        self.assertEqual(gl.card_node_kind(graph, "cards"), "hair")
        self.assertEqual(gl.strands_reaching(graph, "cards"), (False, True))
        with self.assertRaises(ValueError):
            gl.card_node_kind(graph, "nothing")

    def test_map_names(self):
        # The one map name rule and rename every host keeps (scalp_maps.h).
        self.assertTrue(gl.valid_map_name("density"))
        for bad in ("hair-mask", "tint.r", "2nd", ""):
            self.assertFalse(gl.valid_map_name(bad))
            self.assertTrue(gl.map_name_error(bad))
        self.assertEqual(gl.sanitize_map_name("hair-mask"), "hair_mask")
        graph = json.dumps({"version": 1, "output": "out", "nodes": [
            {"name": "m", "type": "Map", "params": {"name": "density"}}, {"name": "out", "type": "Output"}],
            "connections": []})
        text, changed = gl.retarget_map_readers(graph, "density", "thickness")
        self.assertTrue(changed)
        self.assertEqual(json.loads(text)["nodes"][0]["params"]["name"], "thickness")
        self.assertEqual(gl.retarget_map_readers(graph, "other", "x"), (graph, False))
        self.assertEqual(gl.renamed_map_texture("masks/a/density_2.png", "density", "thick"), "masks/a/thick.png")
        self.assertEqual(gl.renamed_map_texture("masks/a/other.png", "density", "thick"), "")

    def test_graph_edits(self):
        # The editors' edits every host makes (graph_edit.h): Maya's GraphModel and the app's
        # node graph name, seed, connect and check a node the same way.
        edit = gl.GraphEdit(gl.default_graph_json())
        edit.remove_node("noise")  # the default graph has one: start the names afresh
        self.assertEqual(edit.unique_name("Noise"), "noise")
        self.assertEqual(edit.add_node("Noise"), "noise")
        self.assertEqual(edit.add_node("Noise"), "noise1")
        spec = json.loads(edit.node_spec_json("noise"))
        self.assertEqual(spec["outputs"][0]["type"], "Curves")
        x0 = json.loads(edit.to_json())["nodes"][-1]["ui"]["x"]
        self.assertEqual(edit.add_node("Scatter"), "scatter1")
        nodes = {n["name"]: n for n in json.loads(edit.to_json())["nodes"]}
        self.assertNotIn("seed", nodes["scatter"].get("params", {}))
        self.assertIn("seed", nodes["scatter1"]["params"])  # a seed of its own: the default is the first's
        self.assertGreater(nodes["scatter1"]["ui"]["x"], x0)  # a free column right of the graph
        self.assertEqual(edit.seed_warnings(), [])
        edit.connect("scatter", "noise", "strands")
        self.assertEqual(edit.output_type("noise"), "Strands")  # Curves gives what feeds it
        with self.assertRaises(gl.GraphError):
            edit.connect("scalp", "noise1", "strands")  # a Mesh into strands
        with self.assertRaises(gl.GraphError):
            edit.connect("noise", "scatter", "scalp")  # no cycle (nor a fit)
        self.assertIn("scatter", edit.upstream("noise"))
        self.assertTrue(edit.disconnect("noise", "strands"))
        self.assertFalse(edit.disconnect("noise", "strands"))
        edit.remove_node("noise")
        self.assertFalse(edit.has_node("noise"))
        with self.assertRaises(gl.GraphError):
            edit.add_node("Teleport")

    def test_portable_path(self):
        # The one rule for stored paths (paths.h), Maya's Python's portable_path too.
        root = tempfile.mkdtemp(prefix="groomlab_py_paths_").replace("\\", "/")
        try:
            os.makedirs(root + "/shot/masks")
            self.assertEqual(gl.portable_path(root + "/shot/masks/a.png", [root + "/shot"]), "masks/a.png")
            self.assertEqual(gl.portable_path(root + "/shared/b.png", [root + "/shot"]), root + "/shared/b.png")
            self.assertEqual(gl.portable_path(root + "/shared/b.png", [root + "/shot"], True), "../shared/b.png")
            self.assertEqual(gl.path_from_groom_folder(root + "/shared/b.png", root + "/shot"), "../shared/b.png")
            self.assertEqual(gl.portable_path("$SHOW/b.png", [root]), "$SHOW/b.png")
        finally:
            shutil.rmtree(root, ignore_errors=True)

    def test_card_bake_files(self):
        # A card bake's files and record (card_files.h): what Maya's Python and the app share.
        self.assertEqual(gl.card_map_names(), ["Depth", "Coverage", "Tangent", "Attribute", "Color", "AO", "Normal"])
        self.assertEqual(gl.card_bake_suffixes()[-1], "Atlas.json")
        self.assertEqual(gl.card_bake_files("a/b_")[4], "a/b_Color.tga")
        self.assertEqual(gl.groom_file_card_bake_prefix("hair"), "hair_cards/hair_")
        text = gl.format_card_maps("a/b_", "cards:1", 2)
        self.assertEqual(gl.parse_card_maps(text)["Coverage"], "a/b_Coverage.tga")
        self.assertEqual(gl.card_maps_prefix(text), "a/b_")
        self.assertEqual(gl.card_maps_prefix(gl.with_card_maps_prefix(text, "c/d_")), "c/d_")

    def test_cards(self):
        groom = gl.Groom(gl.GroomData(LOOK_GRAPH, self.hierarchy.to_json(), self.scalp))
        strands = groom.evaluate(1.0, 0.0)
        settings = gl.CardSettings()
        settings.max_cards = 12
        settings.segments = 4
        cards = gl.build_cards(strands, self.scalp, settings)
        self.assertGreater(cards.card_count, 0)
        self.assertEqual(cards.positions_view().shape, (cards.vertex_count, 3))
        self.assertEqual(cards.quads_view().shape[1], 4)
        self.assertEqual(len(cards.u), cards.vertex_count)
        card = cards.cards[0]
        self.assertGreater(len(card.strands), 0)
        self.assertEqual(len(card.cell), 4)
        bake = gl.CardBakeSettings()
        bake.size = 64
        bake.scalp = self.scalp
        textures = gl.bake_card_textures(strands, cards, bake)
        self.assertEqual(textures.size, 64)
        self.assertEqual(textures.coverage_view().shape, (64, 64))
        self.assertEqual(textures.color_view().shape, (64, 64, 3))
        self.assertEqual(len(textures.attribute), 64 * 64 * 4)
        self.assertGreater(max(textures.coverage), 0)
        folder = tempfile.mkdtemp(prefix="groomlab_py_")
        try:
            path = os.path.join(folder, "coverage.tga")
            gl.write_tga(path, 64, 64, 1, bytes(textures.coverage))
            self.assertEqual(os.path.getsize(path), 18 + 64 * 64)
        finally:
            shutil.rmtree(folder, ignore_errors=True)
        with self.assertRaises(ValueError):
            gl.bake_scalp_textures(strands, self.scalp)  # no UVs
        # Cards through the graph: EvalContext.build_cards.
        doc = gl.load_graph_json(LOOK_GRAPH)
        doc.add_node("cards", "Cards")
        doc.disconnect("output", "strands")
        doc.connect("width", "cards", "strands")
        doc.connect("cards", "output", "strands")
        doc.op("cards").set_param("maxCards", 10)
        externals = gl.ExternalInputs()
        externals.set_groom(scalp=self.scalp, hierarchy=self.hierarchy)
        with_cards = doc.evaluate(gl.EvalContext(externals=externals, build_cards=True))
        self.assertEqual(len(with_cards.card_layers()), 1)
        self.assertGreater(with_cards.card_layers()[0].card_count, 0)
        self.assertEqual(len(doc.evaluate(gl.EvalContext(externals=externals)).card_layers()), 0)

    def test_usd_writer(self):
        s = gl.StrandSet()
        s.add_strand(1, [(0, 0, 0), (0, 1, 0)])
        s.add_strand_channel("clumpId", 2.0)
        folder = tempfile.mkdtemp(prefix="groomlab_py_")
        try:
            path = os.path.join(folder, "strands.usda")
            options = gl.UsdOptions()
            options.comment = "from python"
            with gl.UsdCurvesWriter(path, "hair groom", options) as w:
                w.write_frame(1.0, s)
                w.write_frame(2.0, s)
                self.assertEqual(w.frame_count(), 2)
            with open(path) as f:
                text = f.read()
            self.assertIn("BasisCurves", text)
            self.assertIn("hair_groom", text)
            self.assertIn("clumpId", text)
            self.assertEqual(gl.usd_identifier("hair groom"), "hair_groom")
        finally:
            shutil.rmtree(folder, ignore_errors=True)

    def test_vrscene_writer(self):
        s = gl.StrandSet()
        s.add_strand(1, [(0, 0, 0), (0, 1, 0)], widths=[0.02, 0.01])
        folder = tempfile.mkdtemp(prefix="groomlab_py_")
        try:
            path = os.path.join(folder, "hair.vrscene")
            options = gl.VrsceneOptions()
            options.comment = "from python"
            options.compress = False
            options.set_look(specular=0.3, roughness=1.0)
            with gl.VrsceneWriter(path, "hair groom", options) as w:
                w.write_frame(1.0, s, matrix=[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 3, 0, 0, 1])
                self.assertEqual(w.frame_count(), 1)
                with self.assertRaises(ValueError):
                    w.write_frame(2.0, s, matrix=[1, 2, 3])
            with open(path) as f:
                text = f.read()
            self.assertIn("GeomMayaHair hair_groom@hair {", text)
            self.assertIn("num_hair_vertices=ListIntHex(\"02000000\");", text)
            self.assertIn("Vector(3, 0, 0))", text)  # the matrix's translation
            self.assertRegex(text, r"glossiness=0\.3[45]\d*;")  # roughness 1: Arnold's 0.5
        finally:
            shutil.rmtree(folder, ignore_errors=True)

    @unittest.skipUnless(gl.has_alembic, "built without Alembic")
    def test_alembic_round_trip(self):
        s = gl.StrandSet()
        s.add_strand(7, [(0, 0, 0), (0, 1, 0), (0, 2, 0)], widths=[0.1, 0.2, 0.3])
        s.add_strand(9, [(1, 0, 0), (1, 1, 0)])
        folder = tempfile.mkdtemp(prefix="groomlab_py_")
        try:
            path = os.path.join(folder, "strands.abc")
            with gl.AlembicCurvesWriter(path, "groom", 0.0, 1.0 / 24.0) as w:
                w.write_frame(s)
                w.write_frame(s)
            frames = gl.read_alembic_curves(path, "groom")
            self.assertEqual(len(frames), 2)
            self.assertEqual(frames[0].strands.ids, [7, 9])
            self.assertAlmostEqual(frames[1].seconds, 1.0 / 24.0, places=5)
            self.assertEqual(frames[0].strands.strand_points(0)[2], (0.0, 2.0, 0.0))
            guides = gl.read_alembic_curves_at(path, 0.0)
            self.assertEqual(guides.strand_count, 2)
            unreal = os.path.join(folder, "unreal.abc")
            with gl.UnrealGroomWriter(unreal, "groom", "hair") as w:
                w.write_frame(s, None, None)
            self.assertGreater(os.path.getsize(unreal), 0)
            # In Unreal's space (Z up, left-handed, cm): Y up (x, y, z) -> (x, z, y).
            self.assertEqual(gl.read_alembic_curves(unreal)[0].strands.strand_points(0)[2], (0.0, 0.0, 2.0))
            with gl.UnrealGroomWriter(unreal, "groom", "hair", up_axis="Z", centimeters_per_unit=100.0) as w:
                w.write_frame(s, None, None)
            self.assertEqual(gl.read_alembic_curves(unreal)[0].strands.strand_points(0)[2], (0.0, -200.0, 0.0))
            self.assertAlmostEqual(gl.read_alembic_curves(unreal)[0].strands.widths[0], 10.0, places=4)
            with self.assertRaises(ValueError):
                gl.UnrealGroomWriter(os.path.join(folder, "bad.abc"), "groom", "hair", up_axis="X")
            self.assertIn("ImportCurves", gl.operator_type_names())
        finally:
            shutil.rmtree(folder, ignore_errors=True)


class ReferenceGuidesTest(unittest.TestCase):
    """Guides from reference photos through Python: a camera solved from point pairs, and guides
    grown from a picture of vertical strands over a ball on a plain backdrop (the mask automatic)."""

    def camera(self, size):
        # In front of the ball (+z), looking at it, 40 degrees across.
        return {"rotation": [[1, 0, 0], [0, 1, 0], [0, 0, 1]], "position": [0.0, 0.0, 6.0],
                "focal_px": 0.5 * size / math.tan(math.radians(20.0)), "width": size, "height": size}

    def project(self, cam, p):
        z = cam["position"][2] - p[2]
        return (0.5 * cam["width"] + cam["focal_px"] * p[0] / z, 0.5 * cam["height"] - cam["focal_px"] * p[1] / z)

    def test_solve_camera_finds_the_camera_of_its_points(self):
        cam = self.camera(400)
        world = [(0.3, 0.2, 0.1), (-0.5, 0.4, 0.6), (0.6, -0.5, -0.2), (-0.4, -0.3, 0.5), (0.1, 0.6, -0.6),
                 (0.5, 0.1, 0.7), (-0.2, -0.6, -0.4)]
        image = [self.project(cam, p) for p in world]
        r = gl.solve_camera(image, world, 400, 400)
        self.assertTrue(r["ok"], r["error"])
        self.assertLess(r["rms"], 0.01)
        self.assertAlmostEqual(r["focal_px"], cam["focal_px"], delta=0.5)
        for a, b in zip(r["position"], cam["position"]):
            self.assertAlmostEqual(a, b, delta=1e-3)

    def test_guides_follow_the_strands_in_the_picture(self):
        size = 160
        cam = self.camera(size)
        pixels = bytearray([255] * (size * size * 3))
        # Dark vertical strands over the ball's silhouette and below it, 3 pixels apart.
        r = cam["focal_px"] * 1.1 / 6.0
        for y in range(size):
            for x in range(0, size, 3):
                dx, dy = x - size / 2.0, y - size / 2.0
                if abs(dx) < r and -r < dy < 1.6 * r:
                    i = 3 * (y * size + x)
                    pixels[i:i + 3] = b"\x40\x30\x20"
        folder = tempfile.mkdtemp(prefix="groomlab_reference_")
        try:
            path = os.path.join(folder, "front.tga")
            gl.write_tga(path, size, size, 3, bytes(pixels))
            scalp = make_sphere(16, 32)
            out = gl.guides_from_reference(scalp, [{"image": path, "camera": cam}], guide_count=80, symmetry="none",
                                           max_length=1.2)
            guides = out["guides"]
            self.assertGreater(len(guides), 5)
            self.assertEqual(out["symmetry"], "none")
            down = 0
            for g in guides:
                root, tip = g["points"][0], g["points"][-1]
                if tip[1] < root[1] - 0.3:
                    down += 1
            self.assertGreater(down, 0.6 * len(guides))
            self.assertIn("melanin", out["look"])
        finally:
            shutil.rmtree(folder, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
