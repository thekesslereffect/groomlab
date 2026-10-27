# SPDX-License-Identifier: Apache-2.0
"""Viewport buffers from Python (DisplayBuilder, core display.h): a groom file's session drawn as
Maya's viewport draws it, the buffers read through memoryviews, built again only for what changed."""

import array
import os
import unittest

import groomlab_core as gl

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.path.join(HERE, "..", "..", "data", "compat", "v1", "groom.groom")
# The fixture draws strands and guides only: (vertices, lines or triangles) of the other items.
EXPECTED_SIZES = {name: (0, 0) for name in (
    "dimStrands", "coloredStrands", "ribbons", "instanceMarkers", "instanceMeshes", "scales", "tubes", "selectedGuides",
    "softSelectedGuides", "parts", "flowArrows", "flowField", "cards", "scalpMap", "strandMesh")}


def digest_of(geometry):
    """The geometry's digest from its streams as Python reads them (the core hashes the same bytes)."""
    streams = [geometry.positions, geometry.indices, geometry.colors, geometry.normals, geometry.tangents,
               geometry.bitangents, geometry.uvs]
    return [gl.buffer_digest(memoryview(s)) for s in streams]


class DisplayBuffersTest(unittest.TestCase):
    def setUp(self):
        self.session = gl.GroomSession.from_groom_file(FIXTURE)
        fps = gl.GroomFile.read(FIXTURE).frames_per_second
        self.view = self.session.view(density=1.0, time=1.0 / fps)
        self.assertEqual(self.view.errors, [])

    def test_a_groom_files_buffers(self):
        strands = self.view.strands
        settings = gl.DisplaySettings()
        builder = gl.DisplayBuilder()
        items = builder.update(self.view, settings)
        self.assertEqual(sorted(builder.rebuilt_parts),
                         sorted(["strands", "instances", "guides", "selection", "parts", "flow", "cards", "scalpMap", "strandMesh"]))
        # Lit strand lines: every strand point, a segment between each two along a strand, a
        # shading frame per point.
        lines = items["strands"].geometry
        self.assertTrue(lines.lit)
        self.assertFalse(lines.triangles)
        positions = memoryview(lines.positions)
        self.assertEqual(positions.format, "f")
        self.assertEqual(positions.shape, (strands.point_count, 3))
        self.assertEqual(positions.tobytes(), memoryview(strands.positions_view()).tobytes())
        self.assertEqual(memoryview(lines.indices).shape, (strands.point_count - strands.strand_count, 2))
        self.assertEqual(memoryview(lines.indices).format, "I")
        for stream in (lines.normals, lines.tangents):
            self.assertEqual(memoryview(stream).shape, (strands.point_count, 3))
        self.assertEqual(memoryview(lines.bitangents).shape, (0, 3))  # the host's cross product
        self.assertEqual(lines.uv_floats, 1)  # u root to tip alone
        self.assertEqual(memoryview(lines.uvs).shape, (strands.point_count, 1))
        self.assertEqual(memoryview(lines.colors).shape, (0, 4))  # the item's color
        self.assertEqual(list(items["strands"].color[:3]), list(settings.hair_color))
        # The memoryviews are the core's buffers: the same bytes the digest hashes.
        again = gl.DisplayBuilder().update(self.view, settings)["strands"].geometry
        self.assertEqual(again.digest, lines.digest)
        self.assertEqual(digest_of(again), digest_of(lines))
        # Every item's size, and the buffers as recorded (the core's floating point, the same bits on
        # every platform as tests/data/golden/display_buffers.json; record again after an intended change).
        sizes = {name: (item.vertex_count, memoryview(item.geometry.indices).shape[0]) for name, item in items.items()}
        self.assertEqual(sizes, dict(EXPECTED_SIZES, strands=(22100, 19890), guides=(1395, 1380)))
        self.assertEqual("%016x" % lines.digest, "d1b2ace3b459e113")  # its Collide by the distance field
        self.assertEqual("%016x" % items["guides"].geometry.digest, "2483fbee9bf9139d")
        # The guides: colored lines over the hair, with dots.
        guides = items["guides"].geometry
        self.assertTrue(guides.overlay)
        self.assertEqual(memoryview(guides.colors).shape[0], guides.vertex_count)
        self.assertGreater(guides.vertex_count, 0)
        dots = memoryview(builder.guide_dots)
        self.assertEqual(dots.shape[1], 6)
        self.assertGreater(dots.shape[0], 0)
        self.assertEqual(builder.point_size, settings.theme.point_size + settings.guide_width)
        self.assertGreater(builder.overlay_pull, 0.0)
        self.assertEqual(items["strands"].layer, "dormantWire")
        self.assertEqual(items["guides"].layer, "activeWire")

    def test_only_what_changed_builds_again(self):
        settings = gl.DisplaySettings()
        settings.color_mode = 2  # by a strand channel
        settings.color_source = "len"
        builder = gl.DisplayBuilder()
        first = builder.update(self.view, settings)
        colored = first["coloredStrands"]
        self.assertTrue(colored.rebuilt and colored.shown)
        self.assertEqual(memoryview(colored.geometry.colors).shape, (colored.vertex_count, 4))
        self.assertEqual(builder.update(self.view, settings)["coloredStrands"].rebuilt, False)
        self.assertEqual(builder.rebuilt_parts, [])
        # A new range: the colors alone.
        settings.color_max = 0.5
        recolored = builder.update(self.view, settings)["coloredStrands"]
        self.assertTrue(recolored.recolor)
        self.assertEqual(recolored.geometry_version, colored.geometry_version)
        self.assertGreater(recolored.color_version, colored.color_version)
        self.assertEqual(memoryview(recolored.recolored).tobytes(),
                         memoryview(gl.DisplayBuilder().update(self.view, settings)["coloredStrands"].geometry.colors).tobytes())
        # Ribbons: triangles, two vertices per point.
        settings.ribbons = True
        ribbons = builder.update(self.view, settings)["ribbons"].geometry
        self.assertTrue(ribbons.triangles)
        self.assertEqual(ribbons.vertex_count, 2 * self.view.strands.point_count)
        builder.invalidate("flow")
        builder.update(self.view, settings)
        self.assertEqual(builder.rebuilt_parts, ["flow"])
        with self.assertRaises(ValueError):
            builder.invalidate("nope")

    def test_selection_prototypes_and_helpers(self):
        hierarchy = self.view.hierarchy
        ids = [g.id for g in hierarchy.guides][:2]
        builder = gl.DisplayBuilder()
        items = builder.update(self.view, gl.DisplaySettings(), selection={ids[0]: 1.0, ids[1]: 0.5}, selection_hash=1)
        self.assertGreater(items["selectedGuides"].vertex_count, 0)
        self.assertGreater(items["softSelectedGuides"].vertex_count, 0)
        r, power = gl.hair_highlight(0.3, 0.5)
        self.assertAlmostEqual(power, 4.0 + 120.0 * 0.25, places=4)
        self.assertEqual(len(gl.root_to_tip_ramp()), 32 * 4)
        pixels = bytes(range(256)) * 4  # 32 x 8 RGBA8
        card = gl.combine_card_maps(pixels, pixels, None, 32, 8)
        self.assertEqual(card[:3], pixels[:3])
        self.assertEqual(card[3], pixels[0])  # the coverage's red in the alpha
        limits = gl.overlay_pull_limits(array.array("f", [0, 0, 5, 0, 0, -5]), (0, 0, 10), 1.0, [self.view.scalp])
        self.assertEqual(len(limits), 2)


if __name__ == "__main__":
    unittest.main()
