# SPDX-License-Identifier: Apache-2.0
"""The Redshift for Maya hook's pure helpers (issue 81), without Maya or Redshift: ctest
redshift.hook (labels quick, redshift).

    python test_redshift_hook.py <hosts/maya/scripts>

groomlab.redshift_hook: Redshift's Pre Render Frame MEL line added once and taken out (the user's
lines kept), the deformation blur's shutter frames for each Frame Position, the cache's file
names, and the Principled Hair settings and user data from a look's "principledHair" (by RGB and
by melanin). Standard library only.
"""

import os
import sys
import types
import unittest

SCRIPTS = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "hosts", "maya", "scripts")
sys.argv = sys.argv[:1]
sys.path.insert(0, SCRIPTS)

# The groomlab package's __init__ needs Maya; the hook's helpers do not: a bare package to hold it.
_package = types.ModuleType("groomlab")
_package.__path__ = [os.path.join(SCRIPTS, "groomlab")]
sys.modules["groomlab"] = _package

from groomlab import redshift_hook as rs  # noqa: E402


class ScriptLine(unittest.TestCase):
    def test_added_once_and_removed_keeping_the_users_lines(self):
        user = 'print("mine");'
        added = rs.add_line(user)
        self.assertEqual(added.splitlines(), [user, rs.SCRIPT_LINE])
        self.assertEqual(rs.add_line(added), added)
        self.assertEqual(rs.add_line(None), rs.SCRIPT_LINE)
        self.assertEqual(rs.remove_line(added), user)
        self.assertEqual(rs.remove_line(""), "")
        self.assertIn("pre_render_frame", rs.SCRIPT_LINE)
        self.assertTrue(rs.SCRIPT_LINE.startswith("python(") and rs.SCRIPT_LINE.endswith(";"))  # MEL


class Shutter(unittest.TestCase):
    def test_no_blur_writes_the_frame_alone(self):
        self.assertEqual(rs.shutter_frames(12), (12.0, 12.0, 1.0))
        self.assertEqual(rs.shutter_frames(12, False, 1.0, 0.0, 1.0, 0, 5), (12.0, 12.0, 1.0))
        self.assertEqual(rs.shutter_frames(12, True, 1.0, 0.5, 0.5, 0, 2), (12.0, 12.0, 1.0))  # a closed shutter

    def test_each_frame_position(self):
        # Redshift's defaults: a frame long, open from 0.25 to 0.75 of it, two deformation steps.
        self.assertEqual(rs.shutter_frames(12, True, 1.0, 0.25, 0.75, 0, 2), (11.75, 12.25, 0.5))  # centered
        self.assertEqual(rs.shutter_frames(12, True, 1.0, 0.25, 0.75, 1, 2), (12.25, 12.75, 0.5))  # starting on it
        self.assertEqual(rs.shutter_frames(12, True, 1.0, 0.25, 0.75, 2, 2), (11.25, 11.75, 0.5))  # ending on it
        self.assertEqual(rs.shutter_frames(10, True, 2.0, 0.0, 1.0, 1, 5), (10.0, 12.0, 0.5))  # 5 steps
        self.assertEqual(rs.shutter_frames(10, True, 1.0, 0.0, 1.0, 1, 1), (10.0, 11.0, 1.0))  # 2 at least


class Files(unittest.TestCase):
    def test_cache_names(self):
        self.assertEqual(rs.frame_tag(12), "0012")
        self.assertEqual(rs.frame_tag(12.5), "0012_5")
        self.assertEqual(rs.frame_tag(-3), "n0003")
        self.assertEqual(rs.cache_path(os.sep.join(["C:", "a", "b"]), "charA_hair", 7), "C:/a/b/charA_hair.0007.abc")

    def test_every_write_a_name_of_its_own(self):
        # Redshift keeps a proxy's file as first read: each write of a frame is a new version.
        self.assertEqual(rs.cache_path("C:/c", "hair", 7, 3), "C:/c/hair.0007.3.abc")
        names = ["hair.0007.1.abc", "hair.0007.12.abc", "hair.0008.4.abc", "hair.0007.abc", "fur.0007.2.abc",
                 "hair.0007.x.abc", "hair.0007.2.abc.tmp"]
        self.assertEqual(rs.cache_versions(names, "hair", 7), {1: "hair.0007.1.abc", 12: "hair.0007.12.abc"})
        self.assertEqual(rs.next_version(names, "hair", 7), 13)
        self.assertEqual(rs.next_version(names, "hair", 9), 1)
        self.assertEqual(rs.next_version(names, "fur", 7), 3)


class RenderViewWindow(unittest.TestCase):
    def test_its_title(self):
        self.assertTrue(rs.is_render_view_title("Redshift Render View"))
        self.assertTrue(rs.is_render_view_title("REDSHIFT  render view - frame 12"))
        self.assertTrue(rs.is_render_view_title("Redshift RenderView"))
        self.assertFalse(rs.is_render_view_title("Render View"))  # Maya's own
        self.assertFalse(rs.is_render_view_title("Redshift Render Settings"))
        self.assertFalse(rs.is_render_view_title(None))


class RenderViewWrap(unittest.TestCase):
    REDSHIFT_2026 = """global proc redshiftRvRender(string $action, string $camera)
{
	rsRender -render -stopIpr;
	if($action == "ipr")
	{
		rsRender -render -ipr -rv -cam "<renderview>";
	}
	else
	{
		rsRender -render -rv  -cam "<renderview>";
	}
}"""

    def test_only_the_known_procedure_is_wrapped(self):
        self.assertTrue(rs.rv_render_known(self.REDSHIFT_2026))
        self.assertTrue(rs.rv_render_known(self.REDSHIFT_2026.replace("\t", "    ")))  # spacing aside
        self.assertFalse(rs.rv_render_known(self.REDSHIFT_2026.replace("-stopIpr", "-stopIpr -wait")))
        self.assertFalse(rs.rv_render_known(""))


class Shader(unittest.TestCase):
    LOOK = {"roughness": 0.3, "azimuthalRoughness": 0.2, "ior": 1.55, "angleShift": 3.0, "tint": [0.9, 0.5, 0.4],
            "melanin": 0.35, "melaninRedness": 0.2, "colorData": "Cd", "melaninData": "melanin",
            "rednessData": "melaninRedness"}

    def test_by_melanin_the_channels_and_the_dye(self):
        attrs, data = rs.shader_settings(dict(self.LOOK, byColor=False))
        self.assertEqual(attrs["tint"], (0.9, 0.5, 0.4))
        self.assertEqual(attrs["albedoMix"], 0.0)
        self.assertEqual(attrs["roughness_l"], 0.3)
        self.assertEqual(attrs["alpha"], 3.0)
        self.assertEqual(attrs["melaninVar"], 0.0)
        self.assertEqual(data, [("RedshiftUserDataScalar", "melanin", "melanin"),
                                ("RedshiftUserDataScalar", "melaninRedness", "pheomelanin")])

    def test_by_color_the_rgb_as_albedo(self):
        attrs, data = rs.shader_settings(dict(self.LOOK, byColor=True, tint=[1, 1, 1]))
        self.assertEqual(attrs["albedoMix"], 1.0)
        self.assertEqual(attrs["melanin"], 0.0)
        self.assertEqual(data, [("RedshiftUserDataColor", "Cd", "albedo")])

    def test_no_look_still_gives_a_shader(self):
        attrs, data = rs.shader_settings(None)
        self.assertEqual(attrs["tint"], (1, 1, 1))
        self.assertEqual(len(data), 2)


if __name__ == "__main__":
    unittest.main(verbosity=1)
