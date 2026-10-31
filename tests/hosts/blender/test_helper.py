# SPDX-License-Identifier: Apache-2.0
"""groomlab-helper (hosts/blender/helper, docs/BLENDER.md) driven as the Blender add-on drives it, with
no Blender: the protocol, arrays through shared memory, a preset groom on the test head evaluated to
the same bits as groomlab-cli evaluates the .groom it saved, guides from the host (flat and into a
guide hierarchy) and their incremental evaluation, params and presets, Blender's space (Z up,
meters) against the groom's (Y up, centimeters), every export with no key (GroomLab is free: an old
file's "license" ignored), and bad requests.

Environment (ctest blender.helper sets it): GROOMLAB_HELPER, GROOMLAB_HELPER_TESTHOOKS, GROOMLAB_CLI,
GROOMLAB_TEST_DATA (tests/data).
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from helper_client import HelperClient, HelperError  # noqa: E402

HELPER = os.environ.get("GROOMLAB_HELPER", "")
HELPER_TESTHOOKS = os.environ.get("GROOMLAB_HELPER_TESTHOOKS", "")
CLI = os.environ.get("GROOMLAB_CLI", "")
DATA = os.environ.get("GROOMLAB_TEST_DATA", os.path.join(HERE, "..", "..", "data"))

HEAD_OBJ = os.path.join(DATA, "app", "head_scalp.obj")
COMPAT_GROOM = os.path.join(DATA, "compat", "v1", "groom.groom")

M64 = (1 << 64) - 1
GOLDEN = 0x9E3779B97F4A7C15


# ---- The core's hash (random.h), as groomlab-cli hash prints it -----------------------------------

def hash64(x):
    x = (x + GOLDEN) & M64
    x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & M64
    x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & M64
    return x ^ (x >> 31)


def combine_all(h, values):
    for v in values:
        x = (h ^ ((v + GOLDEN + (h << 6) + (h >> 2)) & M64)) & M64
        x = (x + GOLDEN) & M64
        x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & M64
        x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & M64
        h = x ^ (x >> 31)
    return h


def strands_hash(ids, offsets, positions, widths, root_uvs):
    """groomlab-cli hash's frame hash of strands (ids, offsets, positions, widths, root UVs: bits)."""
    h = hash64(len(ids))
    h = combine_all(h, ids.astype(np.uint64).tolist())
    h = combine_all(h, offsets.astype(np.uint64).tolist())
    h = combine_all(h, np.ascontiguousarray(positions, dtype=np.float32).view(np.uint32).ravel().tolist())
    h = combine_all(h, np.ascontiguousarray(widths, dtype=np.float32).view(np.uint32).tolist())
    h = combine_all(h, np.ascontiguousarray(root_uvs, dtype=np.float32).view(np.uint32).ravel().tolist())
    return h


def file_hash(frame_hashes):
    return combine_all(hash64(len(frame_hashes)), frame_hashes)


def cli_hash(path):
    out = subprocess.run([CLI, "hash", path], check=True, capture_output=True, text=True).stdout
    return int(re.search(r"^all=([0-9a-f]+)", out, re.M).group(1), 16)


def cli_eval(groom, out, density=None, frame=1):
    args = [CLI, "eval", groom, "-o", out, "-f", str(frame)]
    if density is not None:
        args += ["--density", repr(density)]
    run = subprocess.run(args, capture_output=True, text=True)
    if run.returncode != 0:
        raise AssertionError("groomlab-cli eval failed: " + run.stdout + run.stderr)


# ---- The test head --------------------------------------------------------------------------------

def read_obj(path):
    """Points (Y up, cm), triangles and per-corner UVs of an OBJ of triangles."""
    points, uvs, faces, face_uvs = [], [], [], []
    with open(path) as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "v":
                points.append([float(v) for v in parts[1:4]])
            elif parts[0] == "vt":
                uvs.append([float(v) for v in parts[1:3]])
            elif parts[0] == "f":
                corners = [c.split("/") for c in parts[1:]]
                faces.append([int(c[0]) - 1 for c in corners])
                face_uvs.append([uvs[int(c[1]) - 1] for c in corners])
    counts = np.array([len(f) for f in faces], dtype=np.int32)
    indices = np.array([i for f in faces for i in f], dtype=np.int32)
    corner_uvs = np.array([uv for f in face_uvs for uv in f], dtype=np.float32)
    return np.array(points, dtype=np.float32), counts, indices, corner_uvs


def to_blender(points_cm_yup):
    """Y up centimeters to Blender's Z up meters: (x, y, z) -> (x, -z, y) / 100."""
    p = np.asarray(points_cm_yup, dtype=np.float64)
    return (np.stack([p[:, 0], -p[:, 2], p[:, 1]], axis=1) / 100.0).astype(np.float32)


def to_groom(points_m_zup):
    p = np.asarray(points_m_zup, dtype=np.float64)
    return (np.stack([p[:, 0], p[:, 2], -p[:, 1]], axis=1) * 100.0).astype(np.float32)


HEAD = read_obj(HEAD_OBJ) if os.path.exists(HEAD_OBJ) else None


def env_for(**extra):
    env = dict(os.environ)
    env.pop("GROOMLAB_PRESET_PATH", None)
    env["GROOMLAB_USER_PRESETS"] = extra.pop("user_presets", os.path.join(tempfile.gettempdir(), "groomlab_no_user_presets"))
    env.pop("GROOMLAB_NODE_PATH", None)
    env["GROOMLAB_USER_NODES"] = extra.pop("user_nodes", os.path.join(tempfile.gettempdir(), "groomlab_no_user_nodes"))
    env.pop("GROOMLAB_ALPHA_PATH", None)
    env["GROOMLAB_USER_ALPHAS"] = extra.pop("user_alphas", os.path.join(tempfile.gettempdir(), "groomlab_no_user_alphas"))
    env.pop("GROOMLAB_CARD_PRESET_PATH", None)
    env["GROOMLAB_USER_CARD_PRESETS"] = extra.pop("user_card_presets",
                                                  os.path.join(tempfile.gettempdir(), "groomlab_helper_card_presets"))
    env.update(extra)
    return env


class HelperTestCase(unittest.TestCase):
    def setUp(self):
        if not HELPER or not os.path.exists(HELPER):
            self.skipTest("GROOMLAB_HELPER not built")
        self.tmp = tempfile.mkdtemp(prefix="groomlab_helper_")
        self.h = HelperClient(HELPER, env=env_for())

    def tearDown(self):
        if hasattr(self, "h"):
            code = self.h.close()
            self.assertEqual(code, 0, "the helper did not end cleanly")
        if hasattr(self, "tmp"):
            shutil.rmtree(self.tmp, ignore_errors=True)

    def head_session(self, preset="hair_basics", space=None):
        s = self.h.call("new_session", space=space or {"upAxis": "Z", "metersPerUnit": 1.0})["session"]
        points, counts, indices, uvs = HEAD
        r = self.h.call("set_scalp", session=s, **self.h.put(points=to_blender(points), faceCounts=counts,
                                                              faceIndices=indices, uvs=uvs))
        self.assertEqual(r["points"], len(points))
        if preset:
            self.h.call("apply_preset", session=s, name=preset)
        self.h.call("set_time", session=s, frame=1)
        return s

    def evaluate_hash(self, s, density):
        r = self.h.call("evaluate", session=s, density=density, space="groom", attributes=["rootUV"])
        self.assertEqual(r["errors"], [])
        a = r["arrays"]
        uvs = a["attributes"]["rootUV"]
        frame = strands_hash(self.h.array(a["ids"]), self.h.array(a["offsets"]), self.h.array(a["positions"]),
                             self.h.array(a["widths"]), self.h.array(uvs))
        return file_hash([frame]), r

    def assert_matches_cli(self, s, density, name):
        """The session's strands (groom space, from shared memory) are the bits groomlab-cli evaluates
        from the .groom the helper saves of it."""
        ours, r = self.evaluate_hash(s, density)
        groom = os.path.join(self.tmp, name + ".groom")
        self.h.call("save_groom", session=s, path=groom)
        abc = os.path.join(self.tmp, name + "_cli.abc")
        cli_eval(groom, abc, density=density)
        self.assertEqual("%016x" % ours, "%016x" % cli_hash(abc))
        return r


class ProtocolTest(HelperTestCase):
    def test_hello_names_the_protocol_and_the_version(self):
        r = self.h.call("hello", protocol=1)
        self.assertEqual(r["protocol"], 1)
        self.assertEqual(r["groomSpace"], {"upAxis": "Y", "units": "cm"})
        self.assertEqual(r["features"], {"tools": 1, "state": 1, "cardMaps": 1, "authoring": 1, "pipeline": 1, "photos": 1})
        if CLI:
            out = subprocess.run([CLI, "--version"], capture_output=True, text=True).stdout
            self.assertIn(r["version"], out)
        with self.assertRaises(HelperError) as e:
            self.h.call("hello", protocol=99)
        self.assertEqual(e.exception.code, "protocol")
        out = subprocess.run([HELPER, "--version"], capture_output=True, text=True).stdout
        self.assertIn("protocol 1", out)

    def test_bad_requests_are_answered_and_the_helper_carries_on(self):
        r = self.h.send_line("this is not json")
        self.assertFalse(r["ok"])
        self.assertIsNone(r["id"])
        self.assertEqual(r["error"]["code"], "bad_request")
        self.assertEqual(self.h.send_line("[1, 2, 3]")["error"]["code"], "bad_request")
        self.assertEqual(self.h.send_line('{"id": 7}')["error"]["code"], "bad_request")
        self.assertEqual(self.h.send_line('{"id": 8, "cmd": 5}')["id"], 8)
        self.assertEqual(self.h.request("no_such_command")["error"]["code"], "unknown_command")
        self.assertEqual(self.h.request("evaluate", session=99)["error"]["code"], "no_session")
        s = self.h.call("new_session")["session"]
        self.assertEqual(self.h.request("set_scalp", session=s, points=[0, 0, 0], faceCounts=[3],
                                        faceIndices=[0, 1, 2])["error"]["code"], "bad_request")
        self.assertEqual(self.h.request("set_scalp", session=s, points={"shm": "groomlab_no_such_block", "shape": [3, 3]},
                                        faceCounts=[3], faceIndices=[0, 1, 2])["error"]["code"], "shared_memory")
        self.assertEqual(self.h.request("set_scalp", session=s, points={"shm": "bad name!", "shape": [3, 3]},
                                        faceCounts=[3], faceIndices=[0, 1, 2])["error"]["code"], "shared_memory")
        self.assertEqual(self.h.request("set_scalp", session=s, points=[0, 0, 0, 1, 0, 0, 0, 1, 0], faceCounts=[3],
                                        faceIndices=[0, 1, 2], uvs=[0, 0])["error"]["code"], "bad_request")
        self.assertEqual(self.h.request("set_graph", session=s, graph={"nodes": [{"name": "a", "type": "NoSuchOp"}]})
                         ["error"]["code"], "graph")
        self.assertEqual(self.h.request("load_groom", session=s, path=os.path.join(self.tmp, "none.groom"))
                         ["error"]["code"], "io")
        bad = os.path.join(self.tmp, "bad.groom")
        with open(bad, "w") as f:
            f.write("{not a groom")
        self.assertEqual(self.h.request("load_groom", session=s, path=bad)["error"]["code"], "parse")
        self.assertEqual(self.h.request("set_param", session=s, node="nope", param="x", value=1)["error"]["code"],
                         "not_found")
        # An array too short for its shape in a real block.
        refs = self.h.put(points=np.zeros((2, 3), np.float32))
        refs["points"]["shape"] = [1000, 3]
        self.assertEqual(self.h.request("set_scalp", session=s, faceCounts=[3], faceIndices=[0, 1, 2], **refs)
                         ["error"]["code"], "bad_request")
        self.assertTrue(self.h.call("ping")["pong"])
        self.assertIsNone(self.h.proc.poll())

    def test_the_helper_ends_with_its_input(self):
        self.h.proc.stdin.close()
        self.assertEqual(self.h.proc.wait(30), 0)


@unittest.skipIf(HEAD is None, "no test head")
class GroomTest(HelperTestCase):
    def test_a_preset_groom_is_what_the_cli_evaluates_from_its_saved_file(self):
        s = self.head_session()
        r = self.assert_matches_cli(s, 0.1, "preset")
        self.assertGreater(r["strandCount"], 1000)
        self.assertNotIn("learning", r)
        self.assertNotIn("watermark", r)
        # The saved file is Maya's own space, with no license tag.
        with open(os.path.join(self.tmp, "preset.groom")) as f:
            saved = json.load(f)
        self.assertEqual(saved.get("upAxis", "Y"), "Y")
        self.assertEqual(saved.get("units", "cm"), "cm")
        self.assertNotIn("license", saved)

        # The host's space: the same strands turned into Z up meters, widths too.
        groom = self.h.call("evaluate", session=s, space="groom")
        g_pos = self.h.array(groom["arrays"]["positions"])
        g_wid = self.h.array(groom["arrays"]["widths"])
        host = self.h.call("evaluate", session=s)
        self.assertTrue(host["sameTopology"])
        h_pos = self.h.array(host["arrays"]["positions"])
        np.testing.assert_allclose(h_pos, to_blender(g_pos), rtol=1e-6, atol=1e-9)
        np.testing.assert_allclose(self.h.array(host["arrays"]["widths"]), g_wid / 100.0, rtol=1e-6)
        counts = self.h.array(host["arrays"]["pointCounts"])
        offsets = self.h.array(host["arrays"]["offsets"])
        self.assertEqual(offsets.dtype, np.int32)
        self.assertEqual(int(counts.sum()), host["pointCount"])
        np.testing.assert_array_equal(np.diff(offsets), counts)
        names = {a["name"] for a in host["attributesAvailable"]}
        self.assertTrue({"rootUV", "color", "melanin"} <= names, names)
        colors = self.h.call("evaluate", session=s, attributes=["color", "melanin", "nope"])
        self.assertEqual(colors["arrays"]["attributes"]["color"]["shape"], [colors["strandCount"], 3])
        self.assertEqual(colors["missingAttributes"], ["nope"])

        # A render evaluation (every strand) exported by the helper is the CLI's export of the file.
        ours = os.path.join(self.tmp, "helper.abc")
        self.h.call("export", session=s, format="alembic", path=ours, frames={"first": 1, "last": 1})
        theirs = os.path.join(self.tmp, "cli_full.abc")
        cli_eval(os.path.join(self.tmp, "preset.groom"), theirs)
        self.assertEqual(cli_hash(ours), cli_hash(theirs))
        usd = os.path.join(self.tmp, "helper.usda")
        self.h.call("export", session=s, format="usd", path=usd)
        with open(usd) as f:
            self.assertIn("metersPerUnit = 0.01", f.read(4000))
        # A V-Ray scene: the hair, its shader from the look, the CLI's strands.
        vrscene = os.path.join(self.tmp, "helper.vrscene")
        self.h.call("export", session=s, format="vrscene", path=vrscene, frames={"first": 1, "last": 1})
        with open(vrscene) as f:
            text = f.read()
        for plugin in ("GeomMayaHair groom@hair", "BRDFHair4 groom@hairBrdf", "Node groom@node"):
            self.assertIn(plugin, text)
        cli_vrscene = os.path.join(self.tmp, "cli.vrscene")
        cli_eval(os.path.join(self.tmp, "preset.groom"), cli_vrscene)
        with open(cli_vrscene) as f:
            theirs = f.read()
        strip = lambda t: t[t.index("SettingsUnitsInfo"):]  # past the header comment (its source and counts)
        self.assertEqual(strip(text), strip(theirs))
        # In the scene's unit (Blender's V-Ray Scene object: vray.py): meters, still Y up.
        self.h.call("export", session=s, format="vrscene", path=vrscene, frames={"first": 1, "last": 1}, units="scene")
        with open(vrscene) as f:
            scene_text = f.read()
        self.assertIn("meters_scale=1;", scene_text)
        self.assertIn("scene_upDir=Vector(0, 1, 0);", scene_text)
        with self.assertRaises(HelperError):
            self.h.call("export", session=s, format="vrscene", path=vrscene, units="inches")

    def test_the_hair_look_and_the_attributes_arnold_shades_by(self):
        """evaluate's look (what Maya's create_hair_shader builds the Arnold shader from) and the
        attributes the add-on's material reads: per-point color, iridescence, rootNormal. A HairColor
        colors by its RGB in either mode."""
        s = self.head_session()
        wanted = ["color", "pointColor", "melanin", "melaninRedness", "iridescence", "rootNormal"]
        r = self.h.call("evaluate", session=s, density=0.05, attributes=wanted)
        look = r["look"]
        self.assertEqual(look["node"], "color")
        self.assertTrue(look["colorsByRgb"])
        self.assertFalse(look["colorBlends"] or look["iridescent"])
        self.assertNotIn("bySource", look)
        self.assertEqual(look["dye"], [1.0, 1.0, 1.0])
        self.assertAlmostEqual(look["roughness"], 0.5)
        self.assertEqual(set(r["arrays"]["attributes"]), {"color", "melanin", "melaninRedness"})
        self.assertEqual(sorted(r["missingAttributes"]), ["iridescence", "pointColor", "rootNormal"])

        for param, value in (("tipStrength", 0.6), ("dye", [0.5, 0.6, 0.7])):
            self.h.call("set_param", session=s, node="color", param=param, value=value)
        # Iridescence: a FeatherColor after the HairColor.
        after = next(c for c in self.h.call("get_graph", session=s)["graph"]["connections"] if c["from"] == "color")
        sheen = self.h.call("edit_graph", session=s, op="add_node", type="FeatherColor")["name"]
        self.h.call("edit_graph", session=s, op="connect", **{"from": "color", "to": sheen, "input": "strands"})
        self.h.call("edit_graph", session=s, op="connect", **{"from": sheen, "to": after["to"], "input": after["input"]})
        for param, value in (("pattern", "none"), ("iridescence", 0.5)):
            self.h.call("set_param", session=s, node=sheen, param=param, value=value)
        r = self.h.call("evaluate", session=s, density=0.05, attributes=wanted)
        look = r["look"]
        self.assertTrue(look["colorsByRgb"] and look["iridescent"])
        self.assertAlmostEqual(look["dye"][2], 0.7, places=6)
        S, P = r["strandCount"], r["pointCount"]
        at = r["arrays"]["attributes"]
        self.assertEqual((at["pointColor"]["scope"], at["pointColor"]["shape"]), ("point", [P, 3]))
        self.assertEqual((at["iridescence"]["scope"], at["iridescence"]["shape"][0]), ("point", P))
        self.assertEqual((at["rootNormal"]["scope"], at["rootNormal"]["shape"]), ("strand", [S, 3]))
        host = self.h.array(at["rootNormal"]).copy()
        np.testing.assert_allclose(np.linalg.norm(host, axis=1), 1.0, atol=1e-4)
        # A direction: turned into the host's axes, not scaled.
        g = self.h.call("evaluate", session=s, density=0.05, space="groom", attributes=["rootNormal"])
        groom_normals = self.h.array(g["arrays"]["attributes"]["rootNormal"])
        np.testing.assert_allclose(host, np.stack([groom_normals[:, 0], -groom_normals[:, 2], groom_normals[:, 1]], 1),
                                   atol=1e-6)

    def test_guides_from_the_host_drive_the_groom_and_edit_incrementally(self):
        s = self.head_session()
        points = to_blender(HEAD[0])
        center = points.mean(axis=0)
        picked = [i for i in range(len(points)) if points[i, 2] > center[2] + 0.02][::40][:24]
        self.assertGreater(len(picked), 10)
        curves = []
        for i in picked:
            n = points[i] - center
            n /= np.linalg.norm(n)
            curves.append([points[i] + n * (0.12 * k / 4.0) + np.array([0, 0, -0.01 * k]) for k in range(5)])
        flat = np.array(curves, dtype=np.float32).reshape(-1, 3)
        counts = np.full(len(curves), 5, np.int32)
        r = self.h.call("set_guides", session=s, **self.h.put(points=flat, counts=counts))
        self.assertEqual(r["target"], "flat")
        self.assertEqual([a["id"] for a in r["added"]], list(range(1, len(curves) + 1)))
        self.assert_matches_cli(s, 0.1, "guides")

        # The guides come back with their IDs, rooted on the scalp (their roots were on it).
        g = self.h.call("get_guides", session=s)
        self.assertEqual(g["source"], "flat")
        ids = self.h.array(g["arrays"]["ids"])
        back = self.h.array(g["arrays"]["points"])
        np.testing.assert_array_equal(ids, np.arange(1, len(curves) + 1, dtype=np.uint64))
        np.testing.assert_allclose(back, flat, atol=1e-4)
        # Sent back unchanged (the host's round trip): nothing changes, the cache stays.
        same = self.h.call("set_guides", session=s, **self.h.put(points=back, counts=counts, ids=ids))
        self.assertFalse(same["updated"])

        # One guide sculpted: only it changes, the groom re-evaluates incrementally.
        self.h.call("evaluate", session=s, density=0.1)
        moved = back.copy()
        moved[5 * 3 + 1:5 * 3 + 5] += np.array([0.02, 0.01, 0.0], np.float32)
        r = self.h.call("set_guides", session=s, **self.h.put(points=moved, counts=counts, ids=ids))
        self.assertEqual((r["changed"], r["added"], r["removed"]), (1, [], 0))
        e = self.h.call("evaluate", session=s, density=0.1)
        self.assertTrue(e["sameTopology"])
        self.assertGreater(e["nodesIncremental"], 0, e)
        self.assert_matches_cli(s, 0.1, "guides_edited")

        # A guide removed and a new one drawn: IDs stay stable, the new one gets the next.
        keep = np.ones(len(curves), bool)
        keep[0] = False
        pts = np.concatenate([moved.reshape(len(curves), 5, 3)[keep].reshape(-1, 3), flat[:5] + 0.001])
        r = self.h.call("set_guides", session=s, **self.h.put(points=pts, counts=np.full(len(curves), 5, np.int32),
                                                               ids=np.concatenate([ids[keep], [0]]).astype(np.uint64)))
        self.assertEqual(r["removed"], 1)
        self.assertEqual(r["added"], [{"index": len(curves) - 1, "id": len(curves) + 1}])
        self.assert_matches_cli(s, 0.1, "guides_redrawn")

        # The scalp remeshed (its triangles in another order): the guides are rooted on it again.
        p, c, i, uv = HEAD
        tris = i.reshape(-1, 3)[::-1].ravel().copy()
        tri_uvs = uv.reshape(-1, 3, 2)[::-1].reshape(-1, 2).copy()
        r = self.h.call("set_scalp", session=s, **self.h.put(points=to_blender(p), faceCounts=c, faceIndices=tris,
                                                              uvs=tri_uvs))
        self.assertEqual(r["guidesRerooted"], len(curves))
        self.assert_matches_cli(s, 0.1, "guides_remeshed")

    def test_guides_kept_at_rest_follow_a_deforming_scalp(self):
        """Blender's guide curves stay on the scalp's rest pose (set_guides pose "rest"): the session
        is handed them carried to the scalp as it deforms (update_scalp), and following the scalp
        carries the strands grown at rest."""
        points, counts, indices, uvs = HEAD
        rest = to_blender(points)
        s = self.h.call("new_session")["session"]
        self.h.call("set_scalp", session=s, **self.h.put(points=rest, restPoints=rest, faceCounts=counts,
                                                          faceIndices=indices, uvs=uvs))
        self.h.call("apply_preset", session=s, name="hair_basics")
        self.h.call("set_time", session=s, frame=1)
        center = rest.mean(axis=0)
        picked = [i for i in range(len(rest)) if rest[i, 2] > center[2] + 0.02][::60][:16]
        curves = []
        for i in picked:
            n = rest[i] - center
            n /= np.linalg.norm(n)
            curves.append([rest[i] + n * (0.1 * k / 4.0) for k in range(5)])
        flat = np.array(curves, dtype=np.float32).reshape(-1, 3)
        gcounts = np.full(len(curves), 5, np.int32)
        r = self.h.call("set_guides", session=s, pose="rest", **self.h.put(points=flat, counts=gcounts))
        self.assertEqual(r["pose"], "rest")
        self.assertEqual(self.h.call("set_options", session=s, followScalp=True)["followScalp"], True)
        self.assertTrue(self.h.call("groom_info", session=s)["followScalp"])
        at_rest = self.h.call("evaluate", session=s, density=0.05)
        self.assertFalse(at_rest["following"])
        base = self.h.array(at_rest["arrays"]["positions"])
        base_ids = self.h.array(at_rest["arrays"]["ids"])
        rested = self.h.array(self.h.call("get_guides", session=s, pose="rest")["arrays"]["points"])

        # The head turned 30 degrees about Z and moved: a rigid motion of the whole scalp.
        a = np.radians(30.0)
        rot = np.array([[np.cos(a), -np.sin(a), 0], [np.sin(a), np.cos(a), 0], [0, 0, 1]])
        move = np.array([0.05, -0.02, 0.1])
        turned = (rest.astype(np.float64) @ rot.T + move).astype(np.float32)
        self.h.call("update_scalp", session=s, **self.h.put(points=turned, restPoints=rest))
        g = self.h.call("get_guides", session=s)
        self.assertEqual(g["pose"], "now")
        np.testing.assert_allclose(self.h.array(g["arrays"]["points"]), rested @ rot.T + move, atol=2e-5)
        g = self.h.call("get_guides", session=s, pose="rest")
        self.assertEqual(g["pose"], "rest")
        np.testing.assert_array_equal(self.h.array(g["arrays"]["points"]), rested)
        followed = self.h.call("evaluate", session=s, density=0.05)
        self.assertTrue(followed["following"])
        np.testing.assert_array_equal(self.h.array(followed["arrays"]["ids"]), base_ids)
        # (Within the scalp's distance field's own accuracy: Interpolate's clearance lifts a point by
        # the field's blend or, where the field cannot tell, the mesh's answer, which a rounding of
        # the point may switch between.)
        np.testing.assert_allclose(self.h.array(followed["arrays"]["positions"]), base @ rot.T + move, atol=6e-5)
        # Not following: grown on the turned scalp from the carried guides; the roots are turned.
        self.h.call("set_options", session=s, followScalp=False)
        grown = self.h.call("evaluate", session=s, density=0.05)
        offsets = self.h.array(grown["arrays"]["offsets"])[:-1]
        np.testing.assert_allclose(self.h.array(grown["arrays"]["positions"])[offsets],
                                   base[offsets] @ rot.T + move, atol=2e-5)
        # Back at rest: the same strands as before, bit for bit.
        self.h.call("set_options", session=s, followScalp=True)
        self.h.call("update_scalp", session=s, **self.h.put(points=rest, restPoints=rest))
        back = self.h.call("evaluate", session=s, density=0.05)
        np.testing.assert_array_equal(self.h.array(back["arrays"]["positions"]), base)
        # A sculpt at rest while the scalp is turned: stored at rest, handed turned.
        edited = rested.copy()
        edited[4] += np.array([0.0, 0.0, -0.02], np.float32)
        self.h.call("update_scalp", session=s, **self.h.put(points=turned, restPoints=rest))
        ids = self.h.array(g["arrays"]["ids"])
        r = self.h.call("set_guides", session=s, pose="rest", **self.h.put(points=edited, counts=gcounts, ids=ids))
        self.assertEqual(r["changed"], 1)
        now = self.h.array(self.h.call("get_guides", session=s)["arrays"]["points"])
        np.testing.assert_allclose(now[4], edited[4] @ rot.T + move, atol=2e-5)
        self.assertEqual(self.h.request("set_guides", session=s, pose="later", points=[0, 0, 0, 0, 0, 1], counts=[2])
                         ["error"]["code"], "bad_request")

    def test_a_groom_files_hierarchy_guides_are_edited_by_id(self):
        s = self.h.call("new_session")["session"]
        info = self.h.call("load_groom", session=s, path=COMPAT_GROOM)
        self.assertEqual(info["fileUpAxis"], "Y")
        self.assertEqual(info["hierarchyGuides"], 15)
        self.h.call("set_time", session=s, frame=1)
        # Loaded as it is: the CLI's strands of the same file.
        ours, _ = self.evaluate_hash(s, 1.0)
        abc = os.path.join(self.tmp, "compat_cli.abc")
        cli_eval(COMPAT_GROOM, abc)
        self.assertEqual(ours, cli_hash(abc))

        g = self.h.call("get_guides", session=s, source="hierarchy")
        self.assertEqual(g["guides"], 5)  # the 3 primaries and 2 locked children: the others follow them
        ids = self.h.array(g["arrays"]["ids"])
        counts = self.h.array(g["arrays"]["counts"])
        pts = self.h.array(g["arrays"]["points"])
        r = self.h.call("set_guides", session=s, target="hierarchy", **self.h.put(points=pts, counts=counts, ids=ids))
        self.assertFalse(r["updated"])
        tip = int(counts[0]) - 1
        pts[tip] += np.array([0.003, 0.0, 0.002], np.float32)
        r = self.h.call("set_guides", session=s, target="hierarchy", **self.h.put(points=pts, counts=counts, ids=ids))
        self.assertEqual((r["changed"], r["removed"]), (1, 0))
        info = self.h.call("groom_info", session=s)
        self.assertEqual(info["hierarchyGuides"], 15)  # the children are still there
        self.assert_matches_cli(s, 1.0, "compat_edited")

    def test_a_groom_file_with_other_scalps_and_a_reference(self):
        # A groom file with another scalp (its own Scalp and Scatter merged in) and a Reference node
        # bringing in another groom file: the helper's strands are the CLI's, and its save keeps the
        # other scalp.
        with open(COMPAT_GROOM) as f:
            groom = json.load(f)
        ear = {"name": "ear", "points": [], "triangles": []}
        for z in range(5):
            for x in range(5):
                ear["points"] += [30.0 + 2.0 * x, 0.0, 2.0 * z]
        for z in range(4):
            for x in range(4):
                a = z * 5 + x
                ear["triangles"] += [a, a + 5, a + 1, a + 5, a + 6, a + 1]
        groom["scalps"] = [ear]
        groom["searchPaths"] = [os.path.dirname(os.path.abspath(COMPAT_GROOM)).replace("\\", "/")]  # its maps
        graph = groom["graph"]
        output = graph.get("output", "output")
        feed = next(c for c in graph["connections"] if c["to"] == output and c["input"] == "strands")
        library = os.path.abspath(COMPAT_GROOM).replace("\\", "/")
        graph["nodes"] += [{"name": "earScalp", "type": "Scalp", "params": {"input": "ear"}},
                           {"name": "earScatter", "type": "Scatter", "params": {"density": 2.0, "seed": 5}},
                           {"name": "library", "type": "Reference", "params": {"path": library, "bind": "world"}},
                           {"name": "earMerge", "type": "Merge"}, {"name": "libraryMerge", "type": "Merge"}]
        graph["connections"].remove(feed)
        graph["connections"] += [{"from": "earScalp", "to": "earScatter", "input": "scalp"},
                                 {"from": feed["from"], "to": "earMerge", "input": "a"},
                                 {"from": "earScatter", "to": "earMerge", "input": "b"},
                                 {"from": "earMerge", "to": "libraryMerge", "input": "a"},
                                 {"from": "library", "to": "libraryMerge", "input": "b"},
                                 {"from": "libraryMerge", "to": output, "input": "strands"}]
        path = os.path.join(self.tmp, "scalps.groom")
        with open(path, "w") as f:
            json.dump(groom, f)
        s = self.h.call("new_session")["session"]
        self.h.call("load_groom", session=s, path=path)
        self.h.call("set_time", session=s, frame=1)
        ours, r = self.evaluate_hash(s, 1.0)
        abc = os.path.join(self.tmp, "scalps_cli.abc")
        cli_eval(path, abc)
        self.assertEqual(ours, cli_hash(abc))
        plain, _ = self.evaluate_hash(self.compat_session(), 1.0)
        self.assertNotEqual(ours, plain)
        saved = os.path.join(self.tmp, "scalps_saved.groom")
        self.h.call("save_groom", session=s, path=saved)
        with open(saved) as f:
            self.assertEqual([x["name"] for x in json.load(f).get("scalps", [])], ["ear"])

    def compat_session(self):
        s = self.h.call("new_session")["session"]
        self.h.call("load_groom", session=s, path=COMPAT_GROOM)
        self.h.call("set_time", session=s, frame=1)
        return s

    def test_card_maps_bake_as_a_preset_packs_them(self):
        s = self.head_session()
        # A Cards node before the output.
        graph = self.h.call("get_graph", session=s)["graph"]
        into = next(c for c in graph["connections"] if c["to"] == graph["output"] and c["input"] == "strands")
        graph["nodes"].append({"name": "cards", "type": "Cards", "params": {"maxCards": 30}})
        graph["connections"].append({"from": into["from"], "to": "cards", "input": "strands"})
        into["from"] = "cards"
        self.h.call("set_graph", session=s, graph=graph)
        listed = self.h.call("card_map_presets")
        names = [p["name"] for p in listed["presets"]]
        self.assertEqual(names[:4], ["Unreal Hair Cards", "Unreal (packed)", "Unity HDRP Hair", "Separate maps (all)"])
        self.assertIn("normal.y", listed["sources"]["channels"])
        r = self.h.call("bake_card_maps", session=s, directory=self.tmp, name="head", resolution=64)
        self.assertEqual([os.path.basename(p) for p in r["paths"]],
                         ["head_%s.tga" % m for m in ("Depth", "Coverage", "Tangent", "Attribute", "Color", "AO", "Normal")])
        self.assertEqual(r["size"], 64)
        self.assertGreater(r["cards"], 0)
        mine = {"name": "Helper Packed", "files": [{"map": "Pack", "channels": ["depth", "root", "id", "coverage"]}]}
        r = self.h.call("bake_card_maps", session=s, directory=self.tmp, name="mine", resolution=32, preset=mine,
                        format="png", bitDepth=16)
        self.assertEqual([os.path.basename(p) for p in r["paths"]], ["mine_Pack.png"])
        with open(r["paths"][0], "rb") as f:
            self.assertEqual(f.read(26)[24], 16)  # 16 bits a channel
        saved = self.h.call("save_card_map_preset", preset=mine)["path"]
        self.assertTrue(os.path.isfile(saved))
        r = self.h.call("bake_card_maps", session=s, directory=self.tmp, name="again", resolution=32,
                        preset="Helper Packed", format="exr")
        self.assertEqual([os.path.basename(p) for p in r["paths"]], ["again_Pack.exr"])
        self.assertTrue(self.h.call("delete_card_map_preset", name="Helper Packed")["deleted"])
        self.assertEqual(self.h.request("bake_card_maps", session=s, directory=self.tmp, preset="Nothing")["error"]["code"],
                         "not_found")

    def test_params_graph_edits_and_presets(self):
        s = self.head_session()
        nodes = {n["name"]: n for n in self.h.call("list_nodes", session=s)["nodes"]}
        self.assertEqual(nodes["noise"]["type"], "Noise")
        spec = self.h.call("node_spec", session=s, node="noise")["spec"]
        amplitude = [p for p in spec["params"] if p["name"] == "amplitude"][0]
        self.assertIn("unit", amplitude)
        self.assertTrue(any("group" in p for p in spec["params"]))
        self.assertTrue(any(p.get("advanced") for p in spec["params"]))
        before = self.h.call("get_param", session=s, node="noise", param="amplitude")["value"]
        self.h.call("set_param", session=s, node="noise", param="amplitude", value=before * 2)
        self.assertEqual(self.h.call("get_param", session=s, node="noise", param="amplitude")["value"], before * 2)
        with self.assertRaises(HelperError) as e:
            self.h.call("set_param", session=s, node="noise", param="amplitude", value="loud")
        self.assertEqual(e.exception.code, "type_mismatch")
        self.assert_matches_cli(s, 0.1, "param")

        # Graph edits as an editor makes them, and a preset inserted as a compound.
        made = self.h.call("edit_graph", session=s, op="add_node", type="Gravity")["name"]
        self.assertEqual(self.h.call("edit_graph", session=s, op="connect", **{"from": "noise", "to": made, "input": "strands"})
                         ["graphError"], "")
        # A link into a picking Clump's centers sets it to take them (GraphEdit::connectByHand).
        clump = self.h.call("edit_graph", session=s, op="add_node", type="Clump")["name"]
        spots = self.h.call("edit_graph", session=s, op="add_node", type="ToGuides")["name"]
        self.h.call("edit_graph", session=s, op="connect", **{"from": spots, "to": clump, "input": "centers"})
        self.assertEqual(self.h.call("get_param", session=s, node=clump, param="centersFrom")["value"], "inputPoints")
        for n in (clump, spots):
            self.h.call("edit_graph", session=s, op="remove_node", name=n)
        inserted = self.h.call("insert_preset", session=s, name="frizz", after="noise")
        self.assertTrue(inserted["node"])
        names = [n["name"] for n in self.h.call("list_nodes", session=s)["nodes"]]
        self.assertIn(inserted["node"], names)
        graph = self.h.call("get_graph", session=s)["graph"]
        self.h.call("set_graph", session=s, graph=graph)
        self.assert_matches_cli(s, 0.1, "inserted")
        presets = {p["name"] for p in self.h.call("list_presets")["presets"]}
        self.assertTrue({"hair_basics", "frizz", "curly"} <= presets)

        # The node library (Maya's and the app's): the shipped compound nodes listed, one added as
        # the Compound holding its copy, evaluated by the core as the CLI evaluates the saved groom.
        library = {n["id"]: n for n in self.h.call("library_nodes")["nodes"]}
        self.assertTrue({"groomlab.soft_frizz", "groomlab.grey_streaks", "groomlab.windswept_ends"} <= set(library))
        self.assertEqual(library["groomlab.soft_frizz"]["type"], "node:groomlab.soft_frizz")
        frizz = self.h.call("edit_graph", session=s, op="add_node", type="node:groomlab.soft_frizz")["name"]
        self.assertEqual(frizz, "softFrizz")
        self.h.call("edit_graph", session=s, op="connect", **{"from": inserted["node"], "to": frizz, "input": "strands"})
        graph = self.h.call("get_graph", session=s)["graph"]
        node = next(n for n in graph["nodes"] if n["name"] == frizz)
        self.assertEqual(node["type"], "Compound")
        self.assertEqual(node["params"]["definition"]["id"], "groomlab.soft_frizz")
        self.assertFalse(self.h.call("edit_graph", session=s, op="update_library_node", name=frizz)["updated"])
        self.assert_matches_cli(s, 0.1, "library node")
        # A shipped script node: listed as one, added as the Script node holding its file, its
        # params the header's, evaluated by the core as the CLI evaluates the saved groom.
        self.assertEqual(library["groomlab.wave"]["kind"], "script")
        wave = self.h.call("edit_graph", session=s, op="add_node", type="node:groomlab.wave")["name"]
        self.assertEqual(wave, "wave")
        self.h.call("edit_graph", session=s, op="connect", **{"from": frizz, "to": wave, "input": "strands"})
        graph = self.h.call("get_graph", session=s)["graph"]
        node = next(n for n in graph["nodes"] if n["name"] == wave)
        self.assertEqual(node["type"], "Script")
        self.assertIn("per point", node["params"]["definition"]["script"])
        spec = self.h.call("node_spec", session=s, node=wave)["spec"]
        self.assertIn("amplitude", [p["name"] for p in spec["params"]])
        self.assert_matches_cli(s, 0.1, "script node")

        # A preset fits the scalp: the same on a head ten times smaller, lengths scaled.
        big = self.h.call("get_param", session=s, node="scatter", param="length")["value"]
        small = self.h.call("new_session", space={"upAxis": "Z", "metersPerUnit": 0.1})["session"]
        p, c, i, uv = HEAD
        self.h.call("set_scalp", session=small, **self.h.put(points=to_blender(p), faceCounts=c, faceIndices=i))
        fit = self.h.call("apply_preset", session=small, name="hair_basics")
        self.assertAlmostEqual(fit["fitFactor"], 0.1 * self.scalp_size() / 20.0, 4)  # hair_basics was made on a 20 cm head
        self.assertAlmostEqual(self.h.call("get_param", session=small, node="scatter", param="length")["value"], big * 0.1, 4)

    def scalp_size(self):
        p = HEAD[0]
        return float((p.max(axis=0) - p.min(axis=0)).max())


class SpaceTest(HelperTestCase):
    @unittest.skipIf(HEAD is None, "no test head")
    def test_blender_space_round_trips_through_a_maya_groom(self):
        points, counts, indices, uvs = HEAD
        blender = to_blender(points)
        s = self.h.call("new_session", space={"upAxis": "Z", "metersPerUnit": 1.0})["session"]
        self.h.call("set_scalp", session=s, **self.h.put(points=blender, faceCounts=counts, faceIndices=indices, uvs=uvs))
        got = self.h.call("get_scalp", session=s)
        self.assertTrue(got["scalp"]["facesExact"])
        np.testing.assert_allclose(self.h.array(got["arrays"]["points"]), blender, rtol=1e-6, atol=1e-9)
        np.testing.assert_array_equal(self.h.array(got["arrays"]["faceCounts"]), counts)
        np.testing.assert_array_equal(self.h.array(got["arrays"]["faceIndices"]), indices)
        np.testing.assert_array_equal(self.h.array(got["arrays"]["uvs"]), uvs.reshape(-1, 2))
        groom_points = self.h.array(self.h.call("get_scalp", session=s, space="groom")["arrays"]["points"])
        np.testing.assert_array_equal(groom_points, to_groom(blender))

        # Saved as Maya keeps grooms: Y up, centimeters.
        path = os.path.join(self.tmp, "space.groom")
        self.assertEqual(self.h.call("save_groom", session=s, path=path)["upAxis"], "Y")
        with open(path) as f:
            saved = json.load(f)
        self.assertEqual(saved.get("upAxis", "Y"), "Y")
        self.assertEqual(saved.get("units", "cm"), "cm")
        np.testing.assert_array_equal(np.array(saved["scalp"]["points"], np.float32).reshape(-1, 3), groom_points)
        np.testing.assert_allclose(groom_points, points, rtol=1e-6, atol=1e-5)

        # Opened again in Blender's space: the same points, the same groom-space bits.
        t = self.h.call("new_session")["session"]
        self.h.call("load_groom", session=t, path=path)
        np.testing.assert_allclose(self.h.array(self.h.call("get_scalp", session=t)["arrays"]["points"]), blender, rtol=1e-6)
        np.testing.assert_array_equal(
            self.h.array(self.h.call("get_scalp", session=t, space="groom")["arrays"]["points"]), groom_points)

        # Saved Z up (a Houdini or Blender pipeline's choice) and read back: the quarter turn is exact.
        zpath = os.path.join(self.tmp, "space_z.groom")
        self.h.call("save_groom", session=t, path=zpath, upAxis="Z")
        with open(zpath) as f:
            zsaved = json.load(f)
        self.assertEqual(zsaved["upAxis"], "Z")
        np.testing.assert_array_equal(np.array(zsaved["scalp"]["points"], np.float32).reshape(-1, 3),
                                      np.stack([groom_points[:, 0], -groom_points[:, 2], groom_points[:, 1]], axis=1))
        u = self.h.call("new_session")["session"]
        self.assertEqual(self.h.call("load_groom", session=u, path=zpath)["fileUpAxis"], "Z")
        np.testing.assert_array_equal(
            self.h.array(self.h.call("get_scalp", session=u, space="groom")["arrays"]["points"]), groom_points)

        # Y up centimeters is no conversion at all: the bits as given.
        v = self.h.call("new_session", space={"upAxis": "Y", "metersPerUnit": 0.01})["session"]
        self.h.call("set_scalp", session=v, **self.h.put(points=points, faceCounts=counts, faceIndices=indices))
        np.testing.assert_array_equal(self.h.array(self.h.call("get_scalp", session=v)["arrays"]["points"]), points)


@unittest.skipIf(HEAD is None, "no test head")
class FreeTest(HelperTestCase):
    def test_every_export_with_no_key(self):
        # GroomLab is free for any use: no license in the protocol, every export writes its file,
        # and an old groom saved in the former Learning mode ("license": "learning") is one like any.
        hello = self.h.call("hello")
        self.assertNotIn("license", hello)
        for gone in ("license_status", "license_reload", "license_activate", "license_check", "license_deactivate"):
            self.assertEqual(self.h.request(gone)["error"]["code"], "unknown_command", gone)
        s = self.head_session()
        r = self.h.call("evaluate", session=s, density=0.05)
        self.assertNotIn("learning", r)
        self.assertNotIn("watermark", r)
        for fmt in ("alembic", "usd"):
            out = os.path.join(self.tmp, "x." + ("abc" if fmt == "alembic" else "usda"))
            self.h.call("export", session=s, format=fmt, path=out, density=0.05)
            self.assertGreater(os.path.getsize(out), 0, fmt)
        path = os.path.join(self.tmp, "free.groom")
        saved = self.h.call("save_groom", session=s, path=path)
        self.assertNotIn("license", saved)
        self.assertNotIn("learning", saved)
        with open(path) as f:
            text = json.load(f)
        self.assertNotIn("license", text)
        self.assertNotIn("license", self.h.call("get_state", session=s))
        text["license"] = "learning"
        old = os.path.join(self.tmp, "old_learning.groom")
        with open(old, "w") as f:
            json.dump(text, f)
        t = self.h.call("new_session")["session"]
        info = self.h.call("load_groom", session=t, path=old)
        self.assertNotIn("learningFile", info)
        self.assertNotIn("license", info)
        self.h.call("export", session=t, format="alembic", path=os.path.join(self.tmp, "old.abc"), density=0.05)
        self.assertGreater(os.path.getsize(os.path.join(self.tmp, "old.abc")), 0)
        resaved = os.path.join(self.tmp, "resaved.groom")
        self.h.call("save_groom", session=t, path=resaved)
        with open(resaved) as f:
            self.assertNotIn("license", json.load(f))
        # groomlab-cli exports the old file too.
        if CLI:
            cli_eval(old, os.path.join(self.tmp, "cli.abc"), density=0.05)
            self.assertGreater(os.path.getsize(os.path.join(self.tmp, "cli.abc")), 0)


# ---- GroomLab's tools in the host (docs/BLENDER.md, "Tools") ----------------------------------------

CORE_PYTHON = os.environ.get("GROOMLAB_CORE_PYTHON", "")
CORE_PYTHONPATH = os.environ.get("GROOMLAB_CORE_PYTHONPATH", "")
FROZEN = [0.45, 0.6, 0.85]  # display.h DisplayTheme::frozen


class Camera(object):
    """A Blender viewport's matrices (Z up, meters; column vectors, rows first as bpy prints them):
    a perspective camera at `eye` looking at `target`, over a region of width x height pixels."""

    def __init__(self, eye=(0.0, -0.30, 0.30), target=(0.0, 0.0, 0.02), width=800, height=600, fov=40.0):
        eye, target = np.asarray(eye, np.float64), np.asarray(target, np.float64)
        up = np.array([0.0, 0.0, 1.0])
        fw = target - eye
        fw /= np.linalg.norm(fw)
        r = np.cross(fw, up)
        r /= np.linalg.norm(r)
        u = np.cross(r, fw)
        self.V = np.array([[r[0], r[1], r[2], -r.dot(eye)], [u[0], u[1], u[2], -u.dot(eye)],
                           [-fw[0], -fw[1], -fw[2], fw.dot(eye)], [0, 0, 0, 1.0]])
        t = 1.0 / np.tan(np.radians(fov) / 2.0)
        n, f = 0.01, 100.0
        self.P = np.array([[t * height / width, 0, 0, 0], [0, t, 0, 0], [0, 0, -(f + n) / (f - n), -2 * f * n / (f - n)], [0, 0, -1.0, 0]])
        self.width, self.height = width, height
        self.eye, self.target = eye, target

    def view(self, target=True):
        v = {"viewMatrix": self.V.tolist(), "projectionMatrix": self.P.tolist(), "width": self.width, "height": self.height}
        if target:
            v["target"] = self.target.tolist()
        return v

    def groom_matrix(self):
        """The world-to-clip matrix of groom space (Y up cm) in the core's convention (row vectors, rows first)."""
        k = 0.01
        C = np.array([[k, 0, 0, 0], [0, 0, -k, 0], [0, k, 0, 0], [0, 0, 0, 1.0]])
        return (self.P @ self.V @ C).T.ravel()

    def pixel(self, p):
        clip = self.P @ self.V @ np.append(np.asarray(p, np.float64), 1.0)
        ndc = clip[:3] / clip[3]
        return [float((ndc[0] + 1.0) * 0.5 * self.width), float((ndc[1] + 1.0) * 0.5 * self.height)]

    def pixel_size(self, p):
        """Meters per pixel at point p (along the view)."""
        d = np.dot(np.asarray(p) - self.eye, (self.target - self.eye) / np.linalg.norm(self.target - self.eye))
        return 2.0 * d * (1.0 / self.P[1][1]) / self.height


def crown_points(count):
    """Scalp vertices on the top of the head toward the camera (Blender space), left to right."""
    pts = to_blender(HEAD[0]).astype(np.float64)
    candidates = pts[(pts[:, 2] > 0.055) & (pts[:, 1] < -0.01)]
    candidates = candidates[np.argsort(candidates[:, 0])]
    picks = np.linspace(0, len(candidates) - 1, count).round().astype(int)
    return [candidates[i] for i in picks]


def line(a, b, count, pressure=1.0):
    return [[a[0] + (b[0] - a[0]) * k / (count - 1), a[1] + (b[1] - a[1]) * k / (count - 1), pressure] for k in range(count)]


# The settings every compared stroke sets, in the helper's names (host units) and the bindings'
# (groom units: centimeters, Y up). Meters to centimeters are exact for these values.
BASE = {"radius": 50.0, "strength": 1.0, "falloff": 1.0, "symmetry": "none", "symmetryCenter": 0.0, "selectedOnly": False,
        "stretch": False, "collide": True, "frontFacingOnly": True, "pressure": True, "level": 0, "group": "",
        "length": 0.04, "segments": 8, "matchNeighbors": False, "spacing": 0.01, "targets": "guides",
        "partName": "part", "flowSink": "", "smoothStrength": 1.0, "strengthProfile": None, "falloffProfile": None}
BASE_REF = {"screen_radius": 50.0, "strength": 1.0, "hardness": 0.0, "symmetry": "none", "symmetry_center": 0.0,
            "selected_only": False, "stretch": False, "collide": True, "mask_by_scalp": True, "pressure": True, "level": 0,
            "group": "", "length": 4.0, "segments": 8, "match_neighbors": False, "spacing": 1.0, "targets": "guides",
            "part_name": "part", "flow_sink": "", "smooth_strength": 1.0}


@unittest.skipIf(HEAD is None, "no test head")
class ToolsTest(HelperTestCase):
    """The helper's tools against the core's own BrushTool (stroke_reference.py) doing the same strokes."""

    def setUp(self):
        HelperTestCase.setUp(self)
        self.cam = Camera()
        self.s = self.h.call("new_session")["session"]
        points, counts, indices, uvs = HEAD
        self.h.call("set_scalp", session=self.s, **self.h.put(points=to_blender(points), faceCounts=counts,
                                                               faceIndices=indices, uvs=uvs))
        info = self.h.call("new_groom", session=self.s)
        self.assertEqual(info["guides"], "hierarchy")
        self.assertEqual(info["groom"]["hierarchyGuides"], 0)
        self.assertEqual(info["groom"]["flatGuides"], 0)
        self.steps = []      # what the reference replays
        self.after = []      # the helper's texts after each step
        self.start = None    # the groom file and texts the steps start from

    # ---- Driving the helper as the add-on will ----

    def call(self, cmd, **args):
        return self.h.call(cmd, session=self.s, **args)

    def texts(self):
        return self.call("get_state", texts=True, textures=False)["texts"]

    def begin(self):
        """The groom the compared steps start from, saved for the reference."""
        self.start = os.path.join(self.tmp, "start%d.groom" % len(self.after))
        self.call("save_groom", path=self.start)
        self.start_maps = self.texts()["scalpMaps"]
        self.steps, self.after = [], []

    def stroke(self, tool, events, settings=None, ref=None, ctrl=False, shift=False, view=None):
        """A stroke through tool_set, stroke_begin, stroke_drag and stroke_end; recorded for the reference."""
        host_settings = dict(BASE)
        host_settings.update(settings or {})
        ref_settings = dict(BASE_REF)
        ref_settings.update(ref or {})
        if tool == "freeze":
            for k in ("attribute", "value", "paintKind"):
                host_settings.pop(k, None)
            ref_settings.update(mode="paint", attribute="freeze", value=1.0, paint_kind="guides")
        else:
            ref_settings["mode"] = tool
        self.call("tool_set", tool=tool, settings=host_settings)
        x, y, p = events[0]
        r = self.call("stroke_begin", view=view or self.cam.view(), x=x, y=y, pressure=p, ctrl=ctrl, shift=shift)
        self.assertEqual(r["view"]["width"], self.cam.width)
        if len(events) > 1:
            self.call("stroke_drag", points=events[1:])
        end = self.call("stroke_end")
        self.steps.append({"op": "stroke", "settings": ref_settings, "events": events, "ctrl": ctrl, "shift": shift})
        self.after.append(self.texts())
        return end

    def step(self, op, **extra):
        d = {"op": op}
        d.update(extra)
        self.steps.append(d)
        self.after.append(self.texts())

    def assert_matches_reference(self, fields=("guideData", "partData", "flowData", "scalpMaps")):
        """The helper's texts after each step are the core's own strokes' (the bindings' BrushTool)."""
        if not CORE_PYTHON or not os.path.exists(CORE_PYTHON):
            return  # no bindings built: the helper's own checks stand
        matrix = self.call("hover", view=self.cam.view(), x=1.0, y=1.0, overlay=False)["view"]["groomMatrix"]
        spec = {"groom": self.start, "matrix": matrix, "width": self.cam.width, "height": self.cam.height,
                "scalpMaps": self.start_maps, "steps": self.steps}
        spec_path = os.path.join(self.tmp, "reference_spec.json")
        out_path = os.path.join(self.tmp, "reference_out.json")
        with open(spec_path, "w") as f:
            json.dump(spec, f)
        env = dict(os.environ)
        env["PYTHONPATH"] = CORE_PYTHONPATH
        env.pop("PYTHONHOME", None)
        run = subprocess.run([CORE_PYTHON, os.path.join(HERE, "stroke_reference.py"), spec_path, out_path],
                             capture_output=True, text=True, env=env)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        with open(out_path) as f:
            ref = json.load(f)
        self.assertEqual(len(ref), len(self.after))
        for i, (ours, theirs) in enumerate(zip(self.after, ref)):
            for field in fields:
                self.assertEqual(ours[field], theirs[field], "step %d (%s): %s differs from the core's" %
                                 (i, self.steps[i]["op"], field))

    def guides(self):
        r = self.call("get_guides", source="hierarchy")
        a = r["arrays"]
        ids, counts, pts = self.h.array(a["ids"]), self.h.array(a["counts"]), self.h.array(a["points"])
        out, at = {}, 0
        for gid, n in zip(ids.tolist(), counts.tolist()):
            out[gid] = pts[at:at + n]
            at += n
        return out

    def place_guides(self, count=5):
        pixels = [self.cam.pixel(p) for p in crown_points(count)]
        for px in pixels:
            self.stroke("place", [[px[0], px[1], 1.0]])
        return pixels

    def comb_events(self, dx=120.0, count=12):
        g = self.guides()
        mids = [self.cam.pixel(pts[len(pts) // 2]) for pts in g.values()]
        xs = [m[0] for m in mids]
        y = float(np.mean([m[1] for m in mids]))
        return line((min(xs) - 30.0, y), (max(xs) + dx, y + 25.0), count)

    # ---- Tests ----

    def test_tool_catalog_and_settings_in_host_units(self):
        cat = self.h.call("tool_catalog")
        ids = [t["id"] for t in cat["tools"]]
        for tool in ("place", "draw", "part", "flow", "comb", "slide", "smooth", "lift", "twist",
                     "attract", "shape", "cut", "length", "erase", "paint", "select", "freeze"):
            self.assertIn(tool, ids)
        self.assertFalse({"curl", "direction", "noise", "straighten"} & set(ids))  # removed (Straighten: Smooth's chip)
        corrective = set(t["id"] for t in cat["tools"] if t.get("corrective"))
        self.assertEqual(corrective, {"comb", "slide", "smooth", "lift", "twist", "attract", "cut", "length", "select"})
        self.assertIn("falloff", [s["id"] for s in cat["settings"]])
        r = self.call("tool_set", tool="comb", settings={"radius": 42, "falloff": 0.25, "symmetry": "y", "symmetryCenter": 0.01,
                                                        "length": 0.03, "frontFacingOnly": False, "targets": ["guides", "parts"]})
        st = r["settings"]
        self.assertEqual(r["tool"], "comb")
        self.assertEqual(st["radius"], 42)
        self.assertAlmostEqual(st["hardness"], 0.75)
        self.assertEqual(st["symmetry"], "y")  # the host's axis back, through the groom's z
        self.assertAlmostEqual(st["symmetryCenter"], 0.01, places=7)
        self.assertAlmostEqual(st["length"], 0.03, places=7)
        self.assertFalse(st["frontFacingOnly"])
        self.assertEqual(st["targets"], "guides,parts")
        self.assertIn("radius", r["shows"])
        self.assertEqual(self.call("tool_set", tool="freeze")["settings"]["attribute"], "freeze")
        self.assertEqual(self.call("tool_set", tool="paint")["settings"]["attribute"], "clump")  # Paint's map back
        self.assertEqual(self.call("tool_set", tool="select")["tool"], "select")
        for bad in ({"tool": "nope"}, {"settings": {"nope": 1}}, {"settings": {"symmetry": "w"}}, {"settings": {"radius": "x"}}):
            with self.assertRaises(HelperError) as e:
                self.call("tool_set", **bad)
            self.assertEqual(e.exception.code, "bad_request")
        with self.assertRaises(HelperError) as e:
            self.call("stroke_begin", view=self.cam.view(), x=1, y=1)  # the Select tool does not stroke
        self.assertEqual(e.exception.code, "state")
        self.call("tool_set", tool="comb")
        with self.assertRaises(HelperError) as e:
            self.h.call("stroke_begin", session=self.h.call("new_session")["session"], x=1, y=1)
        self.assertEqual(e.exception.code, "state")  # no view yet

    def test_the_view_is_the_hosts_turned_into_the_grooms(self):
        top = crown_points(3)[1]
        px = self.cam.pixel(top)
        r = self.call("hover", view=self.cam.view(), x=px[0], y=px[1])
        np.testing.assert_allclose(r["view"]["groomMatrix"], self.cam.groom_matrix(), rtol=1e-9, atol=1e-12)
        self.assertFalse(r["view"]["orthographic"])
        np.testing.assert_allclose(r["view"]["eye"], self.cam.eye, atol=1e-5)
        ring = r["ring"]
        self.assertTrue(ring["valid"])
        self.assertTrue(ring["onScalp"])
        # The ring sits on the scalp under the cursor, as wide as 50 px there (the radius set).
        self.assertLess(np.linalg.norm(np.asarray(ring["center"]) - top), 0.004)
        self.call("tool_set", settings={"radius": 50})
        r = self.call("hover", x=px[0], y=px[1])
        expected = 50.0 * self.cam.pixel_size(top)
        self.assertAlmostEqual(r["ring"]["radius"] / expected, 1.0, delta=0.08)
        o = r["overlay"]
        self.assertGreater(o["vertexCount"], 40)
        ring_pts = self.h.array(o["arrays"]["positions"])
        self.assertLess(abs(np.linalg.norm(ring_pts - np.asarray(r["ring"]["center"]), axis=1).mean() - r["ring"]["radius"]), 1e-5)
        colors = self.h.array(o["arrays"]["colors"])
        self.assertEqual(colors.shape[1], 4)
        # Off the scalp: at the view's target depth.
        r = self.call("hover", x=5.0, y=5.0)
        self.assertTrue(r["ring"]["valid"])
        self.assertFalse(r["ring"]["onScalp"])
        self.assertEqual(self.call("hover_clear")["overlay"]["version"], r["overlay"]["version"] + 1)

    def test_place_comb_and_stretch_match_the_core(self):
        self.begin()
        self.place_guides(5)
        info = self.call("guides_info")
        self.assertEqual(info["guides"], 5)
        self.assertEqual(info["groups"][0]["name"], "primary")
        before = self.guides()
        # Standing 4 cm along the normal (length 0.04 m).
        for pts in before.values():
            self.assertAlmostEqual(float(np.linalg.norm(pts[-1] - pts[0])), 0.04, delta=0.002)
        self.stroke("comb", self.comb_events())
        combed = self.guides()
        moved = [gid for gid in before if np.abs(before[gid] - combed[gid]).max() > 1e-4]
        self.assertGreaterEqual(len(moved), 3)
        lengths = [np.linalg.norm(np.diff(combed[g], axis=0), axis=1).sum() for g in moved]
        np.testing.assert_allclose(lengths, 0.04, rtol=0.02)  # comb keeps lengths
        self.stroke("comb", self.comb_events(dx=200.0), settings={"stretch": True}, ref={"stretch": True})
        stretched = self.guides()
        lengths = [np.linalg.norm(np.diff(stretched[g], axis=0), axis=1).sum() for g in moved]
        self.assertGreater(max(lengths), 0.0405)  # stretch: longer with the drag
        # The Smooth brush relaxes the stroked guides (Shift with a guide brush selects: the add-on's).
        end = self.stroke("smooth", self.comb_events())
        self.assertEqual(end["released"], "stroke")
        self.assertTrue(end["committed"])
        smoothed = self.guides()
        self.assertTrue(any(np.abs(smoothed[g] - stretched[g]).max() > 1e-5 for g in moved))
        # Draw: a guide shaped like the path from a scalp point.
        a = self.cam.pixel(crown_points(7)[3])
        self.stroke("draw", line(a, (a[0] + 40.0, a[1] + 60.0), 8))
        self.assertEqual(self.call("guides_info")["guides"], 6)
        self.assert_matches_reference()
        u = self.call("undo_state")
        self.assertEqual(u["undoDepth"], 9)
        self.assertEqual(u["undoLabel"], "draw")

    def test_brush_pack_curves_symmetry_and_param_brushes(self):
        """The brush pack through the helper, as the core strokes it: the curves, Direction (with
        topological symmetry) and Noise, and the Density brush wiring its map on its first stroke."""
        self.place_guides(5)
        self.begin()
        curves = {"strengthProfile": [[0, 0], [1, 1]], "falloffProfile": [[0, 1], [1, 0]]}
        r = self.call("tool_set", tool="comb", settings=curves)
        self.assertEqual(r["settings"]["strengthProfile"], [[0, 0], [1, 1]])
        self.assertIn("strengthProfile", r["shows"])
        self.stroke("comb", self.comb_events(), settings=curves,
                    ref={"strength_profile": [[0, 0], [1, 1]], "falloff_profile": [[0, 1], [1, 0]]})
        before = self.guides()
        self.stroke("twist", self.comb_events(dx=160.0), settings={"symmetry": "topology"}, ref={"symmetry": "topology"})
        self.assertEqual(self.call("tool_get")["settings"]["symmetry"], "topology")
        self.stroke("smooth", self.comb_events(), settings={"strength": 0.8}, ref={"strength": 0.8})
        after = self.guides()
        moved = [g for g in before if np.abs(after[g] - before[g]).max() > 1e-4]
        self.assertTrue(moved)
        for g in moved:  # turned and smoothed, lengths kept
            np.testing.assert_allclose(np.linalg.norm(np.diff(after[g], axis=0), axis=1).sum(),
                                       np.linalg.norm(np.diff(before[g], axis=0), axis=1).sum(), rtol=0.02)
        before = after
        # Density: no brush any more (the error names the new way); New Map > For a setting
        # (map_for_param) makes a white map, links it to Scatter's density (the add-on's node tree
        # follows: graphChanged) and puts the Paint tool on it toward 1, one undo step; Ctrl paints
        # hair away. Asked again, the same map.
        with self.assertRaises(HelperError) as e:
            self.call("tool_set", tool="density")
        self.assertIn("+ New Map > for a setting", str(e.exception))
        self.assertEqual([p["id"] for p in self.call("tool_catalog")["paintable"]][:2], ["density", "width"])
        r = self.call("map_for_param", node="scatter", param="density", kind="vertices")
        self.assertEqual((r["name"], r["kind"]), ("scatter_density", "vertices"))
        self.assertTrue(r["made"])
        self.assertTrue(r["graphChanged"])
        self.assertEqual(r["tool"]["mode"], "paint")
        self.assertEqual(r["tool"]["settings"]["attribute"], "scatter_density")
        self.assertEqual(r["tool"]["settings"]["value"], 1.0)
        self.step("map_for_param", node="scatter", param="density")
        again = self.call("map_for_param", node="scatter", param="density", kind="texture")
        self.assertEqual(again["name"], "scatter_density")
        self.assertFalse(again["made"])
        self.assertEqual(again["undo"]["undoDepth"], r["undo"]["undoDepth"])
        paint = dict(BASE, attribute="scatter_density", paintKind="vertices", value=1.0)
        self.call("tool_set", tool="paint", settings=paint)
        top = self.cam.pixel(crown_points(3)[1])
        events = line((top[0] - 40.0, top[1]), (top[0] + 40.0, top[1]), 9)
        r = self.call("stroke_begin", view=self.cam.view(), x=events[0][0], y=events[0][1], ctrl=True)
        self.assertFalse(r["graphChanged"])
        self.assertEqual(r["mode"], "paint")
        self.call("stroke_drag", points=events[1:])
        self.call("stroke_end")
        self.steps.append({"op": "stroke", "settings": dict(BASE_REF, mode="paint", attribute="scatter_density",
                                                            paint_kind="vertices", value=1.0),
                           "events": events, "ctrl": True, "shift": False})
        self.after.append(self.texts())
        maps = {m["name"]: m for m in self.call("maps_list")["maps"]}
        self.assertIn("scatter_density", maps)
        self.assertEqual(maps["scatter_density"]["kind"], "vertices")
        scatter = [n for n in self.call("get_graph")["graph"]["nodes"] if n["type"] == "Scatter"][0]
        self.assertEqual(scatter["params"]["density"]["colorSet"], "scatter_density")
        values = json.loads(self.texts()["scalpMaps"])["scatter_density"]["values"]
        self.assertLess(min(values), 0.6)
        self.assertEqual(max(values), 1.0)
        for gid, pts in self.guides().items():
            np.testing.assert_array_equal(pts, before[gid])  # a map stroke leaves the guides
        self.assert_matches_reference()
        # Undo: the paint, then the map and its link in one step (graphChanged again).
        self.call("undo")
        self.step("undo")
        r = self.call("undo")
        self.step("undo")
        self.assertTrue(r["graphChanged"])
        self.assertNotIn("scatter_density", [m["name"] for m in self.call("maps_list")["maps"]])
        # Solo: Place with soloNewGuides freezes every other guide in its step; a guide brush handed
        # Shift strokes nothing (the add-on selects instead); a brush's Shift click (a solo pick that
        # adds) selects a guide and freezes the rest; a pick of nothing unfreezes all; a painted selection's end
        # (selection_set solo) freezes the rest.
        spot = self.cam.pixel(crown_points(7)[5])
        self.stroke("place", [[spot[0], spot[1], 1.0]], settings={"soloNewGuides": True}, ref={"solo_new_guides": True})
        info = self.call("guides_info")
        self.assertEqual(info["frozen"], info["guides"] - 1)
        g = self.guides()
        first = sorted(g)[0]
        tip = self.cam.pixel(g[first][len(g[first]) // 2])
        self.call("tool_set", tool="comb", settings=dict(BASE))
        self.call("stroke_begin", view=self.cam.view(), x=tip[0], y=tip[1], shift=True)
        end = self.call("stroke_end")
        self.assertEqual(end["released"], "none")
        self.assertFalse(end["committed"])
        r = self.call("select", view=self.cam.view(), lasso=[tip], mode="add", solo=True)
        self.assertTrue(r["picked"])
        self.assertEqual(self.call("undo_state")["undoLabel"], "solo selection")
        self.assertEqual(self.call("guides_info")["frozen"], info["guides"] - len(r["selection"]))
        r = self.call("select", view=self.cam.view(), lasso=[[2.0, 2.0]], mode="replace", solo=True)
        self.assertTrue(r["froze"])
        self.assertEqual(self.call("guides_info")["frozen"], 0)
        r = self.call("select", view=self.cam.view(), lasso=[tip], mode="replace", solo=True)
        self.assertTrue(r["froze"])
        self.assertGreater(self.call("guides_info")["frozen"], 0)
        self.call("select", view=self.cam.view(), lasso=[[2.0, 2.0]], mode="replace", solo=True)
        self.call("selection_set", ids=[first], mode="add")
        r = self.call("selection_set", mode="add", solo=True)
        self.assertTrue(r["froze"])
        self.assertEqual(self.call("guides_info")["frozen"], info["guides"] - 1)
        self.assert_matches_reference()

    def test_symmetry_in_the_hosts_axes(self):
        self.place_guides(5)
        self.begin()
        # x in Blender is x in the groom; y is the groom's -z (its center negated).
        self.stroke("comb", self.comb_events(), settings={"symmetry": "x", "symmetryCenter": 0.0},
                    ref={"symmetry": "x", "symmetry_center": 0.0})
        center = float(np.float32(-(0.01 * 100.0)))
        self.stroke("smooth", self.comb_events(), settings={"symmetry": "y", "symmetryCenter": 0.01, "strength": 0.6},
                    ref={"symmetry": "z", "symmetry_center": center, "strength": 0.6})
        self.stroke("lift", self.comb_events(), settings={"symmetry": "x", "strength": -0.5},
                    ref={"symmetry": "x", "strength": -0.5})
        # Place with symmetry makes a mirrored pair; a comb on the left one alone moves its mirror
        # image across x = 0 the mirrored way.
        before = self.guides()
        spot = self.cam.pixel(to_blender(HEAD[0])[np.argmin(np.abs(to_blender(HEAD[0])[:, 0] + 0.045) +
                                                             np.abs(to_blender(HEAD[0])[:, 2] - 0.05) +
                                                             np.abs(to_blender(HEAD[0])[:, 1] + 0.04))])
        self.stroke("place", [[spot[0], spot[1], 1.0]], settings={"symmetry": "x"}, ref={"symmetry": "x"})
        g = self.guides()
        pair = [gid for gid in g if gid not in before]
        self.assertEqual(len(pair), 2)
        left, right = sorted(pair, key=lambda gid: g[gid][0][0])
        self.assertLess(abs(g[left][0][0] + g[right][0][0]), 1e-4)
        mid = self.cam.pixel(g[left][len(g[left]) // 2])
        self.stroke("comb", line(mid, (mid[0] - 10.0, mid[1] + 45.0), 8), settings={"symmetry": "x", "radius": 30.0},
                    ref={"symmetry": "x", "screen_radius": 30.0})
        after = self.guides()
        moved_left = after[left] - g[left]
        moved_right = after[right] - g[right]
        self.assertGreater(np.abs(moved_left).max(), 2e-3)
        np.testing.assert_allclose(moved_right[:, 0], -moved_left[:, 0], atol=1.5e-3)
        np.testing.assert_allclose(moved_right[:, 1:], moved_left[:, 1:], atol=1.5e-3)
        self.assert_matches_reference()

    def test_freeze_holds_guides_and_draws_them_frozen(self):
        pixels = self.place_guides(5)
        self.begin()
        # Freeze the two leftmost guides (a small hard brush along their middles).
        g = self.guides()
        order = sorted(g, key=lambda gid: g[gid][0][0])
        mids = [self.cam.pixel(g[gid][len(g[gid]) // 2]) for gid in order[:2]]
        self.stroke("freeze", line(mids[0], mids[1], 10), settings={"radius": 25.0, "falloff": 0.0},
                    ref={"screen_radius": 25.0, "hardness": 1.0})
        info = self.call("guides_info")
        self.assertGreaterEqual(info["frozen"], 2)
        self.assertIn("freeze", info["layers"])
        frozen_layer = [m for m in self.call("maps_list")["maps"] if m["name"] == "freeze"]
        self.assertTrue(frozen_layer[0]["freezeMask"])
        before = self.guides()
        self.stroke("comb", self.comb_events())
        after = self.guides()
        held = [gid for gid in before if np.array_equal(before[gid], after[gid])]
        self.assertGreaterEqual(len(held), 2)
        self.assertLess(len(held), len(before))
        self.assert_matches_reference()
        # Drawn in the theme's frozen blue, per vertex.
        d = self.call("display", settings={"guidePoints": "ends"})
        guides = d["items"]["guides"]
        self.assertTrue(guides["changed"])
        self.assertTrue(guides["perVertexColors"])
        colors = self.h.array(guides["arrays"]["colors"])
        frozen_rows = np.all(np.abs(colors[:, :3] - FROZEN) < 1e-6, axis=1)
        self.assertGreater(frozen_rows.sum(), 0)
        self.assertLess(frozen_rows.sum(), len(colors))
        self.assertGreater(d["items"]["dots"]["count"], 0)
        # Unfreeze all: one undo step, nothing frozen.
        self.call("guides_edit", op="unfreeze_all")
        self.assertEqual(self.call("guides_info")["frozen"], 0)
        self.call("undo")
        self.assertGreaterEqual(self.call("guides_info")["frozen"], 2)

    def test_lasso_select_and_selected_only(self):
        self.place_guides(6)
        self.begin()
        g = self.guides()
        tips = {gid: self.cam.pixel(pts[-1]) for gid, pts in g.items()}
        xs = sorted(p[0] for p in tips.values())
        split = (xs[2] + xs[3]) / 2.0
        ys = [p[1] for p in tips.values()]
        lasso = [[split, min(ys) - 80.0], [self.cam.width - 1.0, min(ys) - 80.0], [self.cam.width - 1.0, max(ys) + 80.0],
                 [split, max(ys) + 80.0]]
        r = self.call("select", view=self.cam.view(), lasso=lasso, mode="replace")
        picked = set(r["picked"])
        right = {gid for gid, p in tips.items() if p[0] > split}
        clearly_left = {gid for gid, pts in g.items() if max(self.cam.pixel(q)[0] for q in pts) < split - 3.0}
        self.assertTrue(right <= picked, (right, picked))
        self.assertFalse(picked & clearly_left)
        self.assertEqual(set(self.call("selection_get")["selection"]), picked)
        # A box (two corners) adds; a click toggles one off.
        one = sorted(right)[0]
        r = self.call("select", lasso=[tips[one]], mode="toggle")
        self.assertNotIn(one, r["selection"])
        r = self.call("select", lasso=[tips[one]], mode="add")
        self.assertIn(one, r["selection"])
        selection = r["selection"]
        self.step("select", ids=selection, soft_radius=0.0)
        before = self.guides()
        self.stroke("comb", self.comb_events(), settings={"selectedOnly": True}, ref={"selected_only": True})
        after = self.guides()
        changed = {gid for gid in before if not np.array_equal(before[gid], after[gid])}
        self.assertTrue(changed)
        self.assertTrue(changed <= set(selection), (changed, selection))
        self.assert_matches_reference()
        # The selection draws over the guides; a display with what the host holds sends nothing again.
        d = self.call("display")
        self.assertGreater(d["items"]["selectedGuides"]["vertexCount"], 0)
        self.assertEqual(d["items"]["selectedGuides"]["color"], [1.0, 1.0, 1.0, 1.0])
        have = {name: item["version"] for name, item in d["items"].items()}
        have["brush"] = d["brush"]["overlay"]["version"]
        again = self.call("display", generation=d["generation"], have=have)
        self.assertFalse(any(item["changed"] for item in again["items"].values()))
        self.assertNotIn("arrays", again["items"]["guides"])
        self.call("selection_set", ids=[])
        again = self.call("display", generation=d["generation"], have=have)
        self.assertTrue(again["items"]["selectedGuides"]["changed"])
        self.assertEqual(again["items"]["selectedGuides"]["vertexCount"], 0)
        self.assertFalse(again["items"]["guides"]["changed"])
        # Another generation (a new helper's builder): everything again.
        fresh = self.call("display", generation="another", have=have)
        self.assertTrue(fresh["items"]["guides"]["changed"])

    def test_part_and_flow_strokes(self):
        self.place_guides(3)
        self.begin()
        pts = crown_points(9)
        a, b = self.cam.pixel(pts[1]), self.cam.pixel(pts[7])
        self.stroke("part", line(a, b, 14), settings={"partName": "middle"}, ref={"part_name": "middle"})
        parts = json.loads(self.after[-1]["partData"])
        self.assertTrue(parts)
        self.assertIn("middle", json.dumps(parts))
        c = self.cam.pixel(pts[2])
        d = self.cam.pixel(pts[6])
        self.stroke("flow", line((c[0], c[1] - 30.0), (d[0], d[1] - 30.0), 14))
        self.assertTrue(json.loads(self.after[-1]["flowData"]))
        self.stroke("flow", [[c[0], c[1] - 30.0, 1.0]], settings={"flowSink": "whorl"}, ref={"flow_sink": "whorl"})
        # Combing parts and flow too.
        self.stroke("comb", self.comb_events(), settings={"targets": "guides,parts,flow"}, ref={"targets": "guides,parts,flow"})
        self.assert_matches_reference()
        # Bound to the cage (the scalp: faces and barycentrics), drawn over the guides.
        self.call("evaluate", density=0.05)
        disp = self.call("display")
        self.assertGreater(disp["items"]["parts"]["vertexCount"], 0)
        self.assertGreater(disp["items"]["flowArrows"]["vertexCount"], 0)
        saved = os.path.join(self.tmp, "parts.groom")
        self.call("save_groom", path=saved)
        with open(saved) as f:
            data = json.load(f)
        self.assertTrue(data.get("parts"))
        self.assertTrue(data.get("flow"))

    def test_paint_vertex_and_texture_maps(self):
        self.place_guides(3)
        made = self.call("map_create", name="mask", kind="vertices", value=0.0)
        self.assertEqual(made["undo"]["undoLabel"], "create map mask")
        tex = self.call("map_create", name="tint", kind="texture", color=True, fill=[0.2, 0.3, 0.4], resolution=64)
        layer = self.call("map_create", name="layer", kind="guides", value=0.5)
        self.assertEqual(layer["kind"], "guides")
        names = {m["name"]: m for m in self.call("maps_list")["maps"]}
        self.assertEqual(names["mask"]["kind"], "vertices")
        self.assertEqual(names["tint"]["channels"], 4)
        self.assertEqual(names["layer"]["kind"], "guides")
        with self.assertRaises(HelperError):
            self.call("map_create", name="mask")  # taken
        with self.assertRaises(HelperError):
            self.call("map_create", name="9bad")
        tint_file = names["tint"]["file"]
        with open(tint_file, "rb") as f:
            tint_before = f.read()
        self.begin()
        pts = crown_points(5)
        events = line(self.cam.pixel(pts[0]), self.cam.pixel(pts[4]), 12)
        self.stroke("paint", events, settings={"attribute": "mask", "paintKind": "vertices", "value": 1.0},
                    ref={"attribute": "mask", "paint_kind": "vertices", "value": 1.0})
        maps = json.loads(self.after[-1]["scalpMaps"])
        self.assertGreater(max(maps["mask"]["values"]), 0.5)
        self.stroke("paint", events, settings={"attribute": "layer", "paintKind": "guides", "value": 1.0},
                    ref={"attribute": "layer", "paint_kind": "guides", "value": 1.0})
        self.assert_matches_reference()
        # A texture map: its file painted, undo and redo writing its tiles back.
        self.call("tool_set", tool="paint", settings={"attribute": "tint", "paintKind": "texture", "paintColor": [1, 0, 0],
                                                      "paintChannels": "rgb"})
        self.call("stroke_begin", view=self.cam.view(), x=events[0][0], y=events[0][1])
        self.call("stroke_drag", points=events[1:])
        end = self.call("stroke_end")
        self.assertTrue(end["committed"])
        with open(tint_file, "rb") as f:
            tint_after = f.read()
        self.assertNotEqual(tint_after, tint_before)
        self.assertTrue(self.call("undo")["done"])
        with open(tint_file, "rb") as f:
            self.assertEqual(f.read(), tint_before)
        self.assertEqual(self.call("undo_state")["redoLabel"], "paint")
        self.call("redo")
        with open(tint_file, "rb") as f:
            self.assertEqual(f.read(), tint_after)
        # The map shown on the scalp, per vertex.
        disp = self.call("display", map={"name": "mask"})
        self.assertGreater(disp["items"]["scalpMap"]["vertexCount"], 0)
        disp = self.call("display", map={"name": "tint", "channel": "rgb"})
        self.assertGreater(disp["items"]["scalpMap"]["vertexCount"], 0)
        # Saved, a file the CLI evaluates with its maps; removed, one undo step.
        self.call("map_remove", name="mask")
        self.assertNotIn("mask", [m["name"] for m in self.call("maps_list")["maps"]])
        self.call("undo")
        self.assertIn("mask", [m["name"] for m in self.call("maps_list")["maps"]])

    def test_a_vertex_map_shows_again_after_undo_and_a_reload(self):
        # The map shown on the scalp follows its values through undo and redo, and a groom file
        # loaded again into the live session (Open Groom File on the same mesh) shows its map at
        # once: the session's tools are made again then, and their answers' shared memory must not
        # take a name the add-on still holds open (it did: "exists already", the old map left shown).
        self.place_guides(3)
        self.call("map_create", name="mask", kind="vertices", value=0.0)

        def shown(have=None):
            # (Every array read, so the client holds each block open as the add-on does.)
            self.call("evaluate", density=0.1)
            r = self.call("display", map={"name": "mask"}, have=have or {}, strandColors={"mode": "random"})
            item = r["items"]["scalpMap"]
            colors = self.h.array(item["arrays"]["colors"]) if item["changed"] else None
            if r["strandColors"]["changed"]:
                self.h.array(r["strandColors"]["colors"])
            return item["version"], colors, r

        blank_version, blank, _ = shown()
        pts = crown_points(5)
        events = line(self.cam.pixel(pts[0]), self.cam.pixel(pts[4]), 12)
        self.stroke("paint", events, settings={"attribute": "mask", "paintKind": "vertices", "value": 1.0})
        painted_version, painted, _ = shown({"scalpMap": blank_version})
        self.assertIsNotNone(painted)
        self.assertGreater(painted.sum(), blank.sum())
        self.call("undo")
        version, colors, _ = shown({"scalpMap": painted_version})
        np.testing.assert_array_equal(colors, blank)
        self.call("redo")
        version, colors, _ = shown({"scalpMap": version})
        np.testing.assert_array_equal(colors, painted)
        path = os.path.join(self.tmp, "mask.groom")
        self.call("save_groom", path=path)
        self.call("undo")  # the session's map blank again; the file's painted
        self.h.array(self.call("hover", view=self.cam.view(), x=events[0][0], y=events[0][1])["overlay"]["arrays"]["positions"])
        self.call("load_groom", path=path)
        version, colors, r = shown({"scalpMap": version})
        self.assertIsNotNone(colors)
        np.testing.assert_array_equal(colors, painted)
        self.assertTrue(r["strandColors"]["changed"])
        self.assertTrue(self.call("hover", view=self.cam.view(), x=events[0][0], y=events[0][1])["ring"]["valid"])
        # The state given back to the live session (set_state): the same.
        state = self.call("get_state", scalp="maps")
        self.call("set_state", groom=state["groom"], textures=state["textures"], tools=state["tools"], keepScalp=True)
        version, colors, _ = shown()
        np.testing.assert_array_equal(colors, painted)

    def test_undo_and_redo_restore_exactly(self):
        initial = self.texts()
        self.place_guides(4)
        self.stroke("comb", self.comb_events())
        self.stroke("twist", self.comb_events(), settings={"strength": 0.5})
        self.call("guides_edit", op="generate_children", count=3)
        info = self.call("guides_info")
        self.assertEqual(info["levels"], 2)
        self.assertEqual(info["guides"], 4 + 12)
        # Each group its own automatic color (the child group's not its parent's), from the groom's name.
        hair = [g["autoColor"] for g in self.call("guides_info", guideColorSeed="hair")["groups"]]
        self.assertEqual(len(hair), 2)
        self.assertNotEqual(hair[0], hair[1])
        brows = [g["autoColor"] for g in self.call("guides_info", guideColorSeed="brows")["groups"]]
        self.assertNotEqual(hair, brows)
        self.assertTrue(all(g["color"] is None for g in info["groups"]))
        self.after.append(self.texts())
        self.stroke("erase", self.comb_events(), settings={"radius": 25.0})
        history = [initial] + self.after
        state = self.call("undo_state")
        self.assertEqual(state["undoDepth"], len(history) - 1)
        self.assertEqual(state["undoLabel"], "erase")
        for i in range(len(history) - 1, 0, -1):
            self.assertTrue(self.call("undo")["done"])
            self.assertEqual(self.texts(), history[i - 1], "undo to step %d" % (i - 1))
        self.assertFalse(self.call("undo")["done"])
        self.assertFalse(self.call("undo_state")["canUndo"])
        for i in range(1, len(history)):
            self.assertTrue(self.call("redo")["done"])
            self.assertEqual(self.texts(), history[i], "redo to step %d" % i)
        self.assertFalse(self.call("redo")["done"])
        # A new edit drops what could be redone.
        self.call("undo")
        self.call("guides_edit", op="add_group", name="extra")
        self.assertFalse(self.call("undo_state")["canRedo"])
        # pick_level: the level of the guide under the cursor (a Shift+click alone does nothing).
        g = self.guides()
        px = self.cam.pixel(list(g.values())[0][-1])
        self.call("tool_set", tool="comb", settings={"level": 1})
        self.call("stroke_begin", view=self.cam.view(), x=px[0], y=px[1], shift=True)
        self.assertEqual(self.call("stroke_end")["released"], "none")
        picked = self.call("pick_level", view=self.cam.view(), x=px[0], y=px[1])
        self.assertIn(picked["level"], (0, 1))
        self.assertIn(picked["group"], ("primary", "primary_children"))

    def test_guide_edits_and_hidden_groups(self):
        self.place_guides(4)
        r = self.call("guides_edit", op="generate_children", count=2)
        self.assertEqual(r["group"], "primary_children")
        self.assertEqual(r["added"], 8)
        self.call("selection_set", group="primary_children")
        self.assertEqual(self.call("selection_get")["count"], 8)
        self.call("guides_edit", op="lock")
        self.call("guides_edit", op="freeze")
        self.assertEqual(self.call("guides_info")["frozen"], 8)
        r = self.call("guides_edit", op="rename_group", group="primary_children", name="kids")
        self.assertEqual([g["name"] for g in r["guides"]["groups"]], ["primary", "kids"])
        self.call("set_hidden_groups", groups=["kids"])
        self.assertTrue(self.call("guides_info")["groups"][1]["hidden"])
        disp = self.call("display")
        hidden_count = disp["items"]["guides"]["vertexCount"]
        self.call("set_hidden_groups", groups=[])
        self.assertGreater(self.call("display")["items"]["guides"]["vertexCount"], hidden_count)
        r = self.call("guides_edit", op="delete_guides")
        self.assertEqual(r["removed"], 8)
        self.assertEqual(self.call("selection_get")["count"], 0)
        self.assertEqual(self.call("guides_info")["guides"], 4)
        with self.assertRaises(HelperError):
            self.call("guides_edit", op="remove_group", group="nope")
        with self.assertRaises(HelperError):
            self.call("guides_edit", op="nope")

    def test_bake_node_to_guides_and_paint_a_param_on_the_guides(self):
        self.place_guides(4)
        # Bake Node to Guides: the output's strands as primaries of "baked" (made), one undo step; a
        # node's again adds to it; a node that is not there, or a child group, is refused.
        r = self.call("bake_to_guides", density=0.01)
        self.assertEqual(r["group"], "baked")
        self.assertGreater(r["guides"], 4)
        self.assertEqual(len(r["ids"]), r["guides"])
        info = self.call("guides_info")
        self.assertEqual([g["name"] for g in info["groups"]], ["primary", "baked"])
        self.assertEqual(info["guides"], 4 + r["guides"])
        scatter = [n["name"] for n in self.call("list_nodes")["nodes"] if n["type"] == "Scatter"][0]
        more = self.call("bake_to_guides", node=scatter, group="baked", density=0.01)
        self.assertEqual(more["node"], scatter)
        self.assertGreater(self.call("guides_info")["guides"], info["guides"])
        self.call("undo")
        self.call("undo")
        self.assertEqual(self.call("guides_info")["guides"], 4)
        with self.assertRaises(HelperError) as e:
            self.call("bake_to_guides", node="nope")
        self.assertEqual(e.exception.code, "not_found")
        # A param painted on the guides (the Graph Editor's Paint > On Guides): a guide layer at 1 on
        # every primary, linked to the param, the Paint tool on it; one undo step.
        noise = [n["name"] for n in self.call("list_nodes")["nodes"] if n["type"] == "Noise"][0]
        r = self.call("map_for_param", node=noise, param="amplitude", kind="guides")
        self.assertEqual((r["name"], r["kind"]), ("%s_amplitude" % noise, "guides"))
        self.assertTrue(r["made"] and r["graphChanged"])
        self.assertEqual((r["tool"]["mode"], r["tool"]["settings"]["paintKind"]), ("paint", "guides"))
        maps = {m["name"]: m for m in self.call("maps_list")["maps"]}
        self.assertEqual(maps[r["name"]]["kind"], "guides")
        self.assertEqual(self.call("map_users", name=r["name"])["users"], [[noise, "amplitude"]])
        self.assertEqual(self.call("evaluate", density=0.1)["errors"], [])
        self.call("undo")
        self.assertNotIn(r["name"], [m["name"] for m in self.call("maps_list")["maps"]])

    def test_strand_colors_and_display_while_stroking(self):
        self.place_guides(5)
        self.call("evaluate", density=0.1)
        r = self.call("evaluate", density=0.1)
        d = self.call("display", strandColors={"mode": "random"})
        c = d["strandColors"]
        self.assertTrue(c["changed"])
        colors = self.h.array(c["colors"])
        self.assertEqual(colors.shape, (r["strandCount"], 3))
        self.assertGreater(colors.std(), 0.01)
        again = self.call("display", generation=d["generation"], have={"strandColors": c["version"]},
                          strandColors={"mode": "random"})
        self.assertFalse(again["strandColors"]["changed"])
        per_point = self.call("display", strandColors={"mode": "density", "perPoint": True})["strandColors"]
        self.assertEqual(self.h.array(per_point["colors"]).shape, (r["pointCount"], 3))
        self.assertEqual(self.call("display", strandColors={"mode": "solid"})["strandColors"]["mode"], "solid")
        # During a stroke the guides show the live preview (no evaluation needed), the strands
        # evaluated at the stroke's density.
        d = self.call("display")
        version = d["items"]["guides"]["version"]
        ev = self.comb_events()
        self.call("tool_set", tool="comb", settings=BASE)
        self.call("stroke_begin", view=self.cam.view(), x=ev[0][0], y=ev[0][1])
        drag = self.call("stroke_drag", points=ev[1:])
        self.assertTrue(drag["changed"])
        self.assertTrue(drag["ring"]["valid"])
        live = self.call("display")
        self.assertTrue(live["stroking"])
        self.assertNotEqual(live["items"]["guides"]["version"], version)
        during = self.call("evaluate", density=0.1, sculptDensity=0.35)
        self.assertLess(during["strandCount"], r["strandCount"])
        self.call("stroke_end")
        after = self.call("evaluate", density=0.1)
        self.assertEqual(after["strandCount"], r["strandCount"])

    def test_state_round_trip(self):
        self.place_guides(4)
        self.stroke("comb", self.comb_events())
        self.call("map_create", name="tint", kind="texture", color=True, fill=[0.5, 0.1, 0.1], resolution=32)
        self.call("map_create", name="mask", kind="vertices", value=0.25)
        self.call("selection_set", all=True)
        self.call("set_hidden_groups", groups=["primary"])
        state = self.call("get_state")
        self.assertEqual(state["format"], "groomlab.groom")
        self.assertEqual([t["name"] for t in state["textures"]], ["tint"])
        groom = json.loads(state["groom"])
        self.assertEqual(groom["upAxis"], "Y")
        self.assertIn("mask", groom["scalp"]["colorSets"])
        ours = self.texts()
        h1, _ = self.evaluate_hash(self.s, 0.1)
        # Another session (another helper's, after a crash): the same groom from the state.
        t = self.h.call("new_session")["session"]
        points, counts, indices, uvs = HEAD
        self.h.call("set_scalp", session=t, **self.h.put(points=to_blender(points), faceCounts=counts, faceIndices=indices, uvs=uvs))
        info = self.h.call("set_state", session=t, groom=state["groom"], textures=state["textures"], tools=state["tools"])
        self.assertEqual(info["hierarchyGuides"], 4)
        theirs = self.h.call("get_state", session=t, texts=True, textures=True)
        for field in ("guideData", "partData", "flowData"):
            self.assertEqual(theirs["texts"][field], ours[field])
        # A vertex map travels as the scalp's color set: per face corner, so a point no face uses
        # (the test head has some) comes back 0, as in every host.
        used = np.unique(HEAD[2])
        theirs_mask = np.asarray(json.loads(theirs["texts"]["scalpMaps"])["mask"]["values"])
        ours_mask = np.asarray(json.loads(ours["scalpMaps"])["mask"]["values"])
        np.testing.assert_array_equal(theirs_mask[used], ours_mask[used])
        self.assertEqual(theirs["textures"][0]["png"], state["textures"][0]["png"])
        self.assertNotEqual(json.loads(theirs["texts"]["scalpMaps"])["tint"]["texture"], json.loads(ours["scalpMaps"])["tint"]["texture"])
        self.assertEqual(theirs["tools"]["hiddenGroups"], ["primary"])
        self.assertEqual(sorted(theirs["tools"]["selection"]), sorted(state["tools"]["selection"]))
        self.h.call("set_time", session=t, frame=1)
        self.call("set_time", frame=1)
        h2, _ = self.evaluate_hash(t, 0.1)
        h1, _ = self.evaluate_hash(self.s, 0.1)
        self.assertEqual(h1, h2)
        # The saved file evaluates to the same bits in groomlab-cli.
        self.assert_matches_cli(t, 0.1, "restored")

    def test_new_groom_fits_and_a_small_state(self):
        # new_groom on a scalp: the default graph in proportion to it (Maya's create_groom(fit=True)),
        # by the factor a preset is fitted with; without a scalp, the graph as authored.
        bare = self.h.call("new_session")["session"]
        self.h.call("new_groom", session=bare)
        scatter = [n["name"] for n in self.call("list_nodes")["nodes"] if n["type"] == "Scatter"][0]
        authored = self.h.call("get_param", session=bare, node=scatter, param="length")["value"]
        fitted = self.call("get_param", node=scatter, param="length")["value"]
        self.h.call("close_session", session=bare)
        factor = self.call("apply_preset", name="hair_basics")["fitFactor"]
        self.assertNotAlmostEqual(factor, 1.0, places=3)
        self.assertAlmostEqual(fitted / authored, factor, places=5)
        # The state the add-on keeps after each edit: no scalp (the host feeds its own) until a map
        # painted on its vertices lives in its color sets.
        self.place_guides(2)
        self.assertFalse(json.loads(self.call("get_state", scalp="maps")["groom"]).get("scalp"))
        self.assertTrue(json.loads(self.call("get_state")["groom"]).get("scalp"))
        self.call("map_create", name="mask", kind="vertices", value=0.25)
        self.assertIn("mask", json.loads(self.call("get_state", scalp="maps")["groom"])["scalp"]["colorSets"])

    # ---- Groom Tools' edits beyond strokes (Maya's groomLabGuides/-Parts/-ScalpMaps over the helper) ----

    def test_tube_settings_and_tube_commands(self):
        # The Shape brush's channel and the new guides' tube, as Maya's brush context takes them.
        self.assertEqual(self.call("tool_set", tool="shape", settings={"tubeChannel": "flatness"})["settings"]["tubeChannel"], "flatness")
        with self.assertRaises(HelperError):
            self.call("tool_set", settings={"tubeChannel": "nope"})
        tube = self.call("tool_set", tool="place", settings={"tube": {"shape": "ribbon", "width": 0.004, "aspect": 0.5}})["settings"]["tube"]
        self.assertEqual(tube["shape"], "ribbon")
        self.assertAlmostEqual(tube["width"][0], 0.004, places=6)  # host units (meters)
        self.place_guides(3)
        self.assertEqual(self.call("tube_targets")["source"], "none")  # nothing selected or soloed
        self.call("selection_set", all=True)
        info = self.call("tube_targets")
        self.assertEqual((info["source"], len(info["guides"]), info["shapes"]), ("selection", 3, ["ribbon"]))
        gid = info["guides"][0]
        placed = self.texts()["guideData"]
        r = self.call("tube_set", shape="blade", width=0.01, aspect=0.4, twist=0.5)
        self.assertEqual((r["changed"], r["undo"]["undoLabel"]), (3, "set tube"))
        got = self.call("tube_get", guide=gid)["tube"]
        self.assertEqual(got["shape"], "blade")
        self.assertAlmostEqual(got["width"][0], 0.01, places=5)
        self.assertAlmostEqual(got["twist"][0], 0.5, places=5)
        self.call("tube_scale", factor=2.0, ids=[gid])
        self.assertAlmostEqual(self.call("tube_get", guide=gid)["tube"]["width"][0], 0.02, places=5)
        self.assertEqual(self.call("tube_smooth", channel="flatness", passes=2)["changed"], 3)
        for bad in ({"channel": "nope"},):
            with self.assertRaises(HelperError):
                self.call("tube_smooth", **bad)
        with self.assertRaises(HelperError):
            self.call("tube_set", shape="nope")  # not built in, not in the library
        # The shape library: built-ins, then the groom's own; guides taking a shape follow its edits.
        shapes = self.call("tube_shapes")["shapes"]
        self.assertEqual([s["name"] for s in shapes if s["builtin"]], ["round", "ribbon", "teardrop", "blade"])
        square = [[-1, -1], [1, -1], [1, 1], [-1, 1]]
        self.assertEqual(self.call("tube_shape_add", name="square", points=square, smooth=False)["users"], 0)
        with self.assertRaises(HelperError):
            self.call("tube_shape_add", name="square", points=square)
        self.call("tube_set", shape="square")
        self.assertEqual(self.call("tube_targets")["shapes"], ["square"])
        slab = [[-1, -0.5], [1, -0.5], [1, 0.5], [-1, 0.5]]
        depth = self.call("undo_state")["undoDepth"]
        self.assertTrue(self.call("tube_shape_preview", name="square", points=slab, smooth=False)["previewing"])
        self.assertEqual(self.call("undo_state")["undoDepth"], depth)  # a preview: no undo step
        self.assertTrue(self.call("guides_preview_end")["ended"])
        self.assertEqual(self.call("tube_shape_set", name="square", points=slab)["users"], 3)
        self.call("tube_shape_rename", name="square", to="slab")
        self.assertEqual(self.call("tube_targets")["shapes"], ["slab"])
        self.assertEqual(self.call("tube_shape_remove", name="slab")["users"], 3)
        self.assertEqual(self.call("tube_targets")["shapes"], ["round"])
        # Each edit one undo step: back to the tubes as placed.
        for _ in range(8):
            self.assertTrue(self.call("undo")["done"])
        self.assertEqual(self.texts()["guideData"], placed)

    def test_resample_mirror_selection_and_group_colors(self):
        self.place_guides(4)
        # Segments: the slider's value, a live preview (no undo step), the commit.
        placed = self.call("guide_segments")["segments"]
        self.assertEqual(len(placed), 4)
        depth = self.call("undo_state")["undoDepth"]
        self.assertEqual(self.call("guides_preview", segments=3)["previewed"], 4)
        self.assertEqual(self.call("undo_state")["undoDepth"], depth)
        self.assertEqual(self.call("guide_segments")["segments"], placed)  # the stored guides unchanged
        self.call("guides_preview_end")
        r = self.call("guides_edit", op="resample", segments=5)
        self.assertEqual((r["resampled"], r["undo"]["undoLabel"]), (4, "resample guides"))
        self.assertEqual(self.call("guide_segments")["segments"], [5] * 4)
        self.call("undo")
        self.assertEqual(self.call("guide_segments")["segments"], placed)
        with self.assertRaises(HelperError):
            self.call("guides_edit", op="resample", segments=0)
        # Mirror Selection: the guide across the plane selected (a selection: no undo step).
        g = self.guides()
        left = min(g, key=lambda k: g[k][0][0])
        self.call("selection_set", ids=[left])
        r = self.call("guides_edit", op="mirror_selection", axis="x", add=True)
        self.assertEqual(len(r["picked"]), 1)
        self.assertGreater(g[r["picked"][0]][0][0], 0.0)
        self.assertEqual(sorted(r["selection"]["selection"]), sorted([left, r["picked"][0]]))
        self.assertEqual(self.call("undo_state")["undoDepth"], depth)
        # A group's own color: in guides_info and the display, kept with the state, one undo step.
        self.call("selection_set", ids=[])
        shown = self.call("display")["items"]["guides"]["version"]
        r = self.call("guides_edit", op="set_group_color", group="primary", color="#FF8000")
        self.assertEqual((r["color"], r["undo"]["undoLabel"]), ("#ff8000", "group color"))
        self.assertEqual(r["guides"]["groups"][0]["color"], "#ff8000")
        self.assertEqual(r["guides"]["guideGroupColors"], "primary:#ff8000")
        self.assertNotEqual(self.call("display", have={"guides": shown})["items"]["guides"]["version"], shown)
        r = self.call("guides_edit", op="rename_group", group="primary", name="main")
        self.assertEqual(r["guides"]["guideGroupColors"], "main:#ff8000")  # the color follows its group
        self.call("undo")
        self.assertEqual(self.call("guides_info")["guideGroupColors"], "primary:#ff8000")
        self.call("undo")
        self.assertEqual(self.call("guides_info")["guideGroupColors"], "")
        self.call("redo")
        self.assertEqual(self.call("get_state", textures=False)["tools"]["groupColors"], {"primary": "#ff8000"})
        self.call("guides_edit", op="set_group_color", group="primary", color=[0.1, 0.2, 0.3])
        self.assertEqual(self.call("guides_info")["groups"][0]["color"], "#1a334d")
        self.assertIsNone(self.call("guides_edit", op="set_group_color", group="primary", color=None)["color"])
        for bad in ({"group": "primary", "color": "orange"}, {"group": "nope", "color": "#000000"}):
            with self.assertRaises(HelperError):
                self.call("guides_edit", op="set_group_color", **bad)

    def test_parts_and_flow_commands(self):
        self.place_guides(3)
        pts = np.array(crown_points(9), dtype=np.float64)
        path = pts[1:8]
        self.assertEqual(self.call("part_add", points=path.tolist(), name="middle")["name"], "middle")
        r = self.call("part_add", points=path.tolist(), name="middle")
        self.assertEqual((r["name"], r["undo"]["undoLabel"]), ("middle1", "add part"))
        parts = self.call("parts_list")["parts"]
        self.assertEqual([p["name"] for p in parts], ["middle", "middle1"])
        self.assertGreater(parts[0]["points"], 1)
        self.call("part_remove", name="middle1")
        with self.assertRaises(HelperError):
            self.call("part_remove", name="middle1")
        self.call("part_add", name="side", **self.h.put(points=path.astype(np.float32)))  # from shared memory
        self.assertEqual([p["name"] for p in self.call("parts_list")["parts"]], ["middle", "side"])
        self.assertTrue(self.call("parts_clear")["cleared"])
        self.assertEqual(self.texts()["partData"], "")
        depth = self.call("undo_state")["undoDepth"]
        self.assertFalse(self.call("parts_clear")["cleared"])  # nothing to clear: no undo step
        self.assertEqual(self.call("undo_state")["undoDepth"], depth)
        self.call("undo")
        self.assertEqual(len(self.call("parts_list")["parts"]), 2)
        # Flow: a stroke lays arrows; flow_clear takes them all in one step.
        c, d = self.cam.pixel(pts[2]), self.cam.pixel(pts[6])
        self.stroke("flow", line((c[0], c[1] - 30.0), (d[0], d[1] - 30.0), 14))
        laid = self.texts()["flowData"]
        self.assertTrue(json.loads(laid))
        counted = self.call("parts_list")["flow"]  # the Flow card's count
        self.assertEqual(counted, {"arrows": len(json.loads(laid).get("pins", [])), "sinks": 0})
        self.assertGreater(counted["arrows"], 0)
        r = self.call("flow_clear")
        self.assertEqual((r["cleared"], r["undo"]["undoLabel"]), (True, "clear flow"))
        self.assertEqual(self.texts()["flowData"], "")
        self.assertEqual(self.call("parts_list")["flow"], {"arrows": 0, "sinks": 0})
        self.call("undo")
        self.assertEqual(self.texts()["flowData"], laid)

    def test_map_management(self):
        self.place_guides(3)
        folder = self.call("maps_list")["folder"]

        def maps():
            return {m["name"]: m for m in self.call("maps_list")["maps"]}

        def read(path):
            with open(path, "rb") as f:
                return f.read()

        self.call("map_create", name="tint", kind="texture", color=True, fill=[0.2, 0.3, 0.4], resolution=32)
        self.call("map_create", name="gray", kind="texture", value=0.5, resolution=32)
        self.call("map_create", name="layer", kind="guides", value=0.5)
        scatter = [n["name"] for n in self.call("list_nodes")["nodes"] if n["type"] == "Scatter"][0]
        made = self.call("map_for_param", node=scatter, param="density", kind="vertices")["name"]
        self.assertEqual(self.call("map_users", name=made)["users"], [[scatter, "density"]])
        # Rename: the readers follow (one undo step); a texture's file follows its name.
        r = self.call("map_rename", name=made, to="dens")
        self.assertTrue(r["graphChanged"])
        self.assertEqual(self.call("map_users", name="dens")["users"], [[scatter, "density"]])
        self.call("undo")
        self.assertEqual(self.call("map_users", name=made)["users"], [[scatter, "density"]])
        self.call("redo")
        tint_file = maps()["tint"]["file"]
        r = self.call("map_rename", name="tint", to="color")
        color_file = r["file"]
        self.assertIn("color", os.path.basename(color_file))
        self.assertFalse(os.path.exists(tint_file))
        self.assertTrue(os.path.exists(color_file))
        self.call("undo")
        self.assertTrue(os.path.exists(tint_file))
        self.assertFalse(os.path.exists(color_file))
        self.call("redo")
        self.assertEqual(maps()["color"]["file"], color_file)
        self.call("map_rename", name="layer", to="layer2")
        with self.assertRaises(HelperError):
            self.call("map_rename", name="layer2", to="dens")  # taken
        # Fill: a vertex map's values; a color texture's file (its bytes back on undo); selected guides.
        self.call("map_fill", name="dens", value=0.75)
        self.assertTrue(all(abs(v - 0.75) < 1e-3 for v in json.loads(self.texts()["scalpMaps"])["dens"]["values"]))
        self.call("undo")
        before = read(color_file)
        self.assertEqual(self.call("map_fill", name="color", color=[1, 0, 0], channels="rg")["undo"]["undoLabel"], "fill map color")
        after = read(color_file)
        self.assertNotEqual(after, before)
        self.call("undo")
        self.assertEqual(read(color_file), before)
        self.call("redo")
        self.assertEqual(read(color_file), after)
        self.call("selection_set", ids=[sorted(self.guides())[0]])
        self.call("map_fill", name="layer2", value=1.0, selected=True)
        for bad in ({"name": "dens", "color": [1, 0, 0]}, {"name": "dens", "value": 1.0, "selected": True}, {"name": "dens"}):
            with self.assertRaises(HelperError):
                self.call("map_fill", **bad)
        # Channels: gray <-> color, a texture's file rewritten.
        self.assertEqual(self.call("map_set_channels", name="dens", channels=4)["channels"], 4)
        self.assertEqual(len(json.loads(self.texts()["scalpMaps"])["dens"]["values"]) % 4, 0)
        self.call("map_set_channels", name="color", channels=1)
        self.assertEqual(maps()["color"]["channels"], 1)
        with self.assertRaises(HelperError):
            self.call("map_set_channels", name="layer2", channels=4)
        self.call("undo")
        self.call("undo")
        self.assertEqual((maps()["dens"]["channels"], maps()["color"]["channels"]), (1, 4))
        # Convert: vertices -> texture (a new file, gone again on undo); a guide layer and back.
        r = self.call("map_convert", name="dens", kind="texture", resolution=32)
        self.assertTrue(os.path.exists(r["file"]))
        self.assertEqual(maps()["dens"]["kind"], "texture")
        self.call("undo")
        self.assertFalse(os.path.exists(r["file"]))
        self.assertEqual(maps()["dens"]["kind"], "vertices")
        self.call("map_convert", name="layer2", kind="vertices")
        self.assertNotIn("layer2", self.call("guides_info")["layers"])
        self.call("map_convert", name="layer2", kind="guides")
        self.assertEqual(maps()["layer2"]["kind"], "guides")
        # Import: a PNG read in place (gray: a gray map), or copied into the maps folder; another
        # format copied as a PNG.
        src = os.path.join(self.tmp, "my image.png")
        shutil.copyfile(maps()["gray"]["file"], src)
        r = self.call("map_import", path=src)
        self.assertEqual((r["name"], r["channels"]), ("my_image", 1))
        self.assertEqual(os.path.normcase(os.path.abspath(r["file"])), os.path.normcase(src))
        r = self.call("map_import", path=src, name="copied", copy=True)
        copied = r["file"]
        self.assertTrue(os.path.exists(copied))
        self.assertNotEqual(os.path.normcase(os.path.abspath(copied)), os.path.normcase(src))
        tga = os.path.join(self.tmp, "red.tga")
        with open(tga, "wb") as f:  # 2 x 2, 24-bit, uncompressed: red
            f.write(bytes([0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 24, 0]) + bytes([0, 0, 255]) * 4)
        with self.assertRaises(HelperError):
            self.call("map_import", path=tga, copy=False)
        r = self.call("map_import", path=tga)
        self.assertEqual((r["name"], r["channels"]), ("red", 4))
        self.assertTrue(r["file"].endswith(".png"))
        self.call("undo")
        self.assertFalse(os.path.exists(r["file"]))
        # Relink: a guide layer becomes a texture map reading the image; reload lists the files.
        r = self.call("map_relink", name="layer2", path=src)
        self.assertEqual((maps()["layer2"]["kind"], maps()["layer2"]["channels"]), ("texture", 1))
        self.assertIn(os.path.normcase(src), [os.path.normcase(os.path.abspath(p)) for p in self.call("map_reload")["files"]])
        with self.assertRaises(HelperError):
            self.call("map_reload", name="dens")  # not a texture map
        # Remove: what reads the map lets go of it; a file is deleted only when GroomLab made it.
        r = self.call("map_remove", name="dens")
        self.assertEqual((r["unlinked"], r["graphChanged"]), ([[scatter, "density"]], True))
        density = self.call("get_param", node=scatter, param="density")["value"]
        self.assertNotIsInstance(density, dict)
        self.call("undo")
        self.assertEqual(self.call("map_users", name="dens")["users"], [[scatter, "density"]])
        r = self.call("map_remove", name="copied", deleteFile=True)
        self.assertTrue(r["fileDeleted"])
        self.assertFalse(os.path.exists(copied))
        self.call("undo")
        self.assertTrue(os.path.exists(copied))
        r = self.call("map_remove", name="my_image", deleteFile=True)
        self.assertFalse(r["fileDeleted"])  # yours, linked in place: it stays
        self.assertTrue(os.path.exists(src))
        self.assertTrue(folder)


    # ---- Correctives and Sculpt nodes (Maya's shots.py and sculpts.py over the helper) ----

    def test_correctives_shot_sculpt_into_layers(self):
        self.place_guides(4)
        self.call("set_time", frame=10)
        r = self.call("corrective_new")  # on every frame, made what brushes sculpt into
        self.assertEqual((r["name"], r["target"], r["undo"]["undoLabel"]), ("sculpt1", {"kind": "corrective", "name": "sculpt1"}, "add corrective"))
        self.assertTrue(r["layers"][0]["allFrames"])
        guide_data = self.texts()["guideData"]
        end = self.stroke("comb", self.comb_events())
        self.assertTrue(end["committed"])
        self.assertEqual(self.texts()["guideData"], guide_data)  # the stroke went into the layer
        self.assertGreater(self.call("correctives_list")["layers"][0]["guides"], 0)
        # A fix keyed around frame 20: no weight at frame 10, so a stroke there records nothing.
        r = self.call("corrective_new", frame=20, fade=4)
        fix = r["layers"][-1]
        self.assertEqual((fix["name"], fix["allFrames"], fix["frame"], fix["fadeIn"]), ("shot20", False, 20.0, 4.0))
        self.assertEqual(fix["weightNow"], 0.0)
        end = self.stroke("comb", self.comb_events())
        self.assertFalse(end["committed"])
        self.assertTrue(any("shot sculpt" in w for w in end["warnings"]))
        self.call("set_time", frame=20)
        self.assertTrue(self.stroke("comb", self.comb_events())["committed"])
        self.assertGreater(self.call("correctives_list")["layers"][1]["guides"], 0)
        # Weights: a value, keys from the host's curve ([frame, weight]), a key set where it is keyed.
        layer = self.call("corrective_set", name="shot20", weight=0.5)["layers"][1]
        self.assertEqual(layer["weight"], 0.5)
        layer = self.call("corrective_set", name="shot20", keys=[[15, 0.0], [20, 1.0], [25, 0.0]])["layers"][1]
        self.assertEqual((layer["keyed"], layer["keys"], layer["frame"]), (True, [[15.0, 0.0], [20.0, 1.0], [25.0, 0.0]], 20.0))
        layer = self.call("corrective_set", name="shot20", weight=0.25)["layers"][1]
        self.assertEqual(layer["keys"][1], [20.0, 0.25])
        layer = self.call("corrective_clear_keys", name="shot20")["layers"][1]
        self.assertEqual((layer["keyed"], layer["allFrames"], layer["weight"]), (False, True, 0.25))
        layer = self.call("corrective_set", name="shot20", frame=30, fade=2)["layers"][1]
        self.assertEqual((layer["allFrames"], layer["frame"], layer["fadeIn"], layer["fadeOut"]), (False, 30.0, 2.0, 2.0))
        with self.assertRaises(HelperError):
            self.call("corrective_set", name="nope", weight=1.0)
        # The stack: rename (the target follows), reorder, solo, the base groom, delete.
        r = self.call("corrective_rename", name="shot20", to="hero")
        self.assertEqual(r["target"], {"kind": "corrective", "name": "hero"})
        for bad in ("sculpt1", "1 x"):
            with self.assertRaises(HelperError):
                self.call("corrective_rename", name="hero", to=bad)
        names = [l["name"] for l in self.call("corrective_move", name="hero", step=-1)["layers"]]
        self.assertEqual(names, ["hero", "sculpt1"])
        self.assertEqual([l["enabled"] for l in self.call("corrective_solo", name="sculpt1")["layers"]], [False, True])
        self.assertEqual([l["enabled"] for l in self.call("corrective_solo", name="sculpt1")["layers"]], [True, True])
        self.assertEqual(self.call("sculpt_target", base=True)["target"]["kind"], "base")
        self.assertEqual(self.call("sculpt_target", corrective="hero")["target"]["name"], "hero")
        r = self.call("corrective_delete", name="hero")
        self.assertEqual(([l["name"] for l in r["layers"]], r["target"]["kind"]), (["sculpt1"], "base"))
        self.call("undo")
        self.assertEqual([l["name"] for l in self.call("correctives_list")["layers"]], ["hero", "sculpt1"])
        # Every edit one undo step, back to the guides as placed; the layers travel with the groom file.
        path = os.path.join(self.tmp, "shots.groom")
        self.call("save_groom", path=path)
        with open(path) as f:
            saved = json.load(f)["correctives"]["layers"]
        self.assertEqual([l["name"] for l in saved], ["hero", "sculpt1"])
        while self.call("undo_state")["undoLabel"] != "place":
            self.call("undo")
        self.assertEqual(self.texts()["correctiveData"], "")
        self.assertEqual(self.call("correctives_list")["target"]["kind"], "base")

    def test_sculpt_node_strokes_and_clear(self):
        self.place_guides(4)
        self.call("set_graph", graph=SCULPT_GRAPH)
        self.assertEqual(self.call("evaluate", density=0.1)["errors"], [])
        placed = self.call("display")["items"]["guides"]["vertexCount"]
        r = self.call("sculpt_target", node="sculpt1")
        self.assertGreater(self.call("display")["items"]["guides"]["vertexCount"], placed)  # the node's guides shown
        self.assertEqual((r["target"], r["sculptNodes"]), ({"kind": "node", "name": "sculpt1"}, [{"name": "sculpt1", "editedGuides": 0}]))
        for bad in ("nope", "hair"):
            with self.assertRaises(HelperError):
                self.call("sculpt_target", node=bad)
        guide_data = self.texts()["guideData"]
        self.assertTrue(self.stroke("comb", self.comb_events(), settings={"radius": 150.0})["committed"])
        self.assertEqual(self.texts()["guideData"], guide_data)  # the node's guides, not the stored ones
        self.assertTrue(self.texts()["sculptData"])
        self.assertGreater(self.call("correctives_list")["sculptNodes"][0]["editedGuides"], 0)
        sculpted = self.texts()["sculptData"]
        r = self.call("sculpt_clear", node="sculpt1")
        self.assertEqual((r["cleared"], r["undo"]["undoLabel"]), (True, "clear sculpt edits"))
        self.assertEqual(json.loads(self.texts()["sculptData"] or "{}"), {})
        self.assertFalse(self.call("sculpt_clear", node="sculpt1")["cleared"])
        self.call("undo")
        self.assertEqual(self.texts()["sculptData"], sculpted)
        # Grouped, the node's edits follow it into the compound in the same step; brushes go back to the base groom.
        r = self.call("edit_graph", op="group", names=["sculpt1"], name="look")
        self.assertEqual(r["moved"], [["sculpt1", "look/sculpt1"]])
        self.assertIn("look/sculpt1", json.loads(self.texts()["sculptData"]))
        self.assertEqual(self.call("sculpt_target")["target"]["kind"], "base")
        self.call("undo")
        self.assertEqual(self.texts()["sculptData"], sculpted)


# Sparse hand guides -> an even Scatter + Interpolate -> ToGuides -> Sculpt -> the strands'
# Interpolate (sculpt.h's workflow, Maya's test_grooming.py).
SCULPT_GRAPH = {
    "version": 1, "output": "output",
    "nodes": [{"name": "scalp", "type": "Scalp"}, {"name": "sparse", "type": "Guides"},
              {"name": "even", "type": "Scatter", "params": {"density": 0.5, "randomness": 0.0, "length": 4.0,
                                                             "previewAll": True}},
              {"name": "dense", "type": "Interpolate"}, {"name": "toGuides", "type": "ToGuides"},
              {"name": "sculpt1", "type": "Sculpt"}, {"name": "roots", "type": "Scatter", "params": {"density": 20.0}},
              {"name": "hair", "type": "Interpolate"}, {"name": "output", "type": "Output"}],
    "connections": [{"from": "scalp", "to": "even", "input": "scalp"},
                    {"from": "even", "to": "dense", "input": "strands"},
                    {"from": "sparse", "to": "dense", "input": "guides"},
                    {"from": "scalp", "to": "dense", "input": "scalp"},
                    {"from": "dense", "to": "toGuides", "input": "strands"},
                    {"from": "toGuides", "to": "sculpt1", "input": "guides"},
                    {"from": "scalp", "to": "sculpt1", "input": "scalp"},
                    {"from": "scalp", "to": "roots", "input": "scalp"},
                    {"from": "roots", "to": "hair", "input": "strands"},
                    {"from": "sculpt1", "to": "hair", "input": "guides"},
                    {"from": "scalp", "to": "hair", "input": "scalp"},
                    {"from": "hair", "to": "output", "input": "strands"}],
}


@unittest.skipIf(HEAD is None, "no test head")
class AuthoringTest(HelperTestCase):
    """The Graph Editor's authoring, the controls, the node library and presets over the helper (Maya's
    GraphModel, params.py, nodes.py and presets.py)."""

    def setUp(self):
        HelperTestCase.setUp(self)
        self.assertEqual(self.h.close(), 0)
        self.nodes_dir = os.path.join(self.tmp, "nodes")
        self.presets_dir = os.path.join(self.tmp, "presets")
        self.h = HelperClient(HELPER, env=env_for(user_nodes=self.nodes_dir, user_presets=self.presets_dir))
        self.s = self.head_session()

    def call(self, cmd, **args):
        return self.h.call(cmd, session=self.s, **args)

    def graph(self):
        return self.call("get_graph")["graph"]

    def node(self, name):
        return next((n for n in self.graph()["nodes"] if n["name"] == name), None)

    def links(self):
        return sorted((c["from"], c["to"], c["input"]) for c in self.graph()["connections"])

    def test_group_ungroup_expose_and_insert(self):
        before = self.graph()
        strands, _ = self.evaluate_hash(self.s, 0.05)
        r = self.call("edit_graph", op="group", names=["noise", "color"], name="look")
        self.assertEqual((r["name"], r["graphChanged"], r["undo"]["undoLabel"]), ("look", True, "group nodes"))
        self.assertEqual(self.node("look")["type"], "Compound")
        self.assertIsNone(self.node("noise"))
        self.assertEqual(self.evaluate_hash(self.s, 0.05)[0], strands)  # the same strands, grouped
        r = self.call("edit_graph", op="ungroup", name="look")
        self.assertEqual(sorted(r["names"]), ["color", "noise"])
        self.assertEqual(self.evaluate_hash(self.s, 0.05)[0], strands)
        self.call("undo")
        self.assertEqual(self.node("look")["type"], "Compound")
        self.call("undo")
        self.assertEqual(self.graph(), before)
        self.call("redo")
        depth = self.call("undo_state")["undoDepth"]
        for op, args in (("group", {"names": ["scatter", "clump"]}),  # two nodes feeding out
                         ("ungroup", {"name": "noise"}), ("ungroup", {"name": "nope"})):
            with self.assertRaises(HelperError):
                self.call("edit_graph", op=op, **args)
        self.assertEqual(self.call("undo_state")["undoDepth"], depth)  # a refused edit: no step
        # A compound's own panel: an inner param shown under a name of its own.
        self.call("edit_graph", op="compound_expose", name="look", param="amount", target="noise.amplitude")
        spec = self.call("node_spec", node="look")["spec"]
        self.assertIn("amount", [p["name"] for p in spec["params"]])
        with self.assertRaises(HelperError):
            self.call("edit_graph", op="compound_expose", name="look", param="x", target="noise.nope")
        self.call("edit_graph", op="compound_unexpose", name="look", target="noise.amplitude")
        self.assertNotIn("amount", [p["name"] for p in self.call("node_spec", node="look")["spec"]["params"]])
        self.call("edit_graph", op="ungroup", name="look")
        # An exposed param: an input port of its own; unexposed, what fed it goes.
        r = self.call("edit_graph", op="expose_param", name="width", param="width")
        self.assertEqual(r["undo"]["undoLabel"], "expose param")
        ports = {p["name"]: p for p in self.call("node_spec", node="width")["spec"]["inputs"]}
        self.assertTrue(ports["width"]["param"])
        self.assertFalse(self.call("edit_graph", op="expose_param", name="width", param="width")["graphChanged"])
        with self.assertRaises(HelperError):
            self.call("edit_graph", op="expose_param", name="width", param="nope")
        self.call("edit_graph", op="unexpose_param", name="width", param="width")
        self.assertNotIn("paramInputs", self.node("width"))
        # A node put on a link: from -> node -> to, one step (Maya's insert_on_link).
        extra = self.call("edit_graph", op="add_node", type="Noise", name="noise2")["name"]
        r = self.call("edit_graph", op="insert_on_link", **{"from": "width", "to": "resample", "input": "strands", "node": extra})
        self.assertEqual(r["undo"]["undoLabel"], "insert node")
        self.assertIn(("width", extra, "strands"), self.links())
        self.assertIn((extra, "resample", "strands"), self.links())
        self.assertNotIn(("width", "resample", "strands"), self.links())
        with self.assertRaises(HelperError):
            self.call("edit_graph", op="insert_on_link", **{"from": "width", "to": "resample", "input": "strands", "node": extra})
        self.call("undo")
        self.assertIn(("width", "resample", "strands"), self.links())

    def test_controls_keyed_params_and_maps(self):
        strands, _ = self.evaluate_hash(self.s, 0.05)
        amplitude = self.call("get_param", node="noise", param="amplitude")["value"]
        r = self.call("edit_graph", op="add_control", node="noise", param="amplitude", name="frizz", min=0.0, max=2.0)
        self.assertEqual((r["control"], r["node"], r["undo"]["undoLabel"]), ("frizz", "frizzControl", "control"))
        self.assertIn(("frizzControl", "noise", "amplitude"), self.links())
        controls = {c["name"]: c for c in self.call("controls_list")["controls"]}
        self.assertEqual((controls["frizz"]["kind"], controls["frizz"]["default"], controls["frizz"]["max"]), ("control", amplitude, 2.0))
        self.assertFalse(controls["frizz"]["set"])
        slider, _ = self.evaluate_hash(self.s, 0.05)  # the slider at its default
        self.assertTrue(self.call("set_controls", controls={"frizz": amplitude * 3.0})["changed"])
        self.assertEqual(self.call("undo_state")["undoLabel"], "control")  # a control's value: the host's, no step
        self.assertNotEqual(self.evaluate_hash(self.s, 0.05)[0], slider)
        self.call("set_controls", controls={"frizz": None})
        self.assertEqual(self.evaluate_hash(self.s, 0.05)[0], slider)
        with self.assertRaises(HelperError):
            self.call("edit_graph", op="add_control", node="noise", param="frequency", name="frizz")  # taken
        with self.assertRaises(HelperError):
            self.call("set_controls", controls={"frizz": "a lot"})
        self.call("undo")
        self.assertEqual(self.evaluate_hash(self.s, 0.05)[0], strands)
        # A keyed param: linked to a control of its own, which the host keys.
        width = self.call("get_param", node="width", param="width")["value"]
        r = self.call("edit_graph", op="link_control", node="width", param="width")
        self.assertEqual((r["control"], r["undo"]["undoLabel"]), ("width_width", "key param"))
        self.assertEqual(self.node("width")["paramControls"], {"width": "width_width"})
        controls = {c["name"]: c for c in self.call("controls_list")["controls"]}
        self.assertEqual((controls["width_width"]["kind"], controls["width_width"]["param"]), ("keyed", "width"))
        self.call("set_controls", controls={"width_width": width * 0.5})
        widths = self.h.array(self.call("evaluate", density=0.05)["arrays"]["widths"])
        self.call("set_controls", controls={"width_width": width})
        full = self.h.array(self.call("evaluate", density=0.05)["arrays"]["widths"])
        np.testing.assert_allclose(widths, full * 0.5, rtol=1e-4)
        for param in ("segments", "nope"):  # a count is never keyed
            with self.assertRaises(HelperError):
                self.call("edit_graph", op="link_control", node="resample", param=param)
        self.call("set_controls", controls={"width_width": width * 0.25})
        r = self.call("edit_graph", op="unlink_control", node="width", param="width")
        self.assertAlmostEqual(r["value"], width * 0.25, places=5)  # the value now goes back into the graph
        self.assertNotIn("paramControls", self.node("width"))
        # A param reading a map: checked before the graph takes it.
        self.call("set_param", node="noise", param="amplitude", value={"expression": "0.5 * u", "min": 0, "max": 1})
        self.assertEqual(self.call("evaluate", density=0.05)["errors"], [])
        self.call("set_param", node="width", param="width", value={"guide": "clumpiness", "component": "r", "min": 1, "max": 3})
        for value, code in (({"expression": "0.5 *"}, "expression"), ({"texture": "a.png", "colorSet": "b"}, "type_mismatch"),
                            ({"attribute": "melanin", "component": "q"}, "type_mismatch"),
                            ({"attribute": "melanin", "scale": 2}, "type_mismatch"), ({"colorSet": ""}, "type_mismatch")):
            with self.assertRaises(HelperError) as e:
                self.call("set_param", node="noise", param="amplitude", value=value)
            self.assertEqual(e.exception.code, code, value)
        with self.assertRaises(HelperError) as e:
            self.call("set_param", node="resample", param="segments", value={"expression": "4"})
        self.assertEqual(e.exception.code, "type_mismatch")
        # Backdrops ride with the positions (the layout): kept in the graph, not an undo step.
        depth = self.call("undo_state")["undoDepth"]
        box = {"title": "Look", "x": 10.0, "y": -20.0, "w": 300.0, "h": 200.0, "color": [200, 100, 50]}
        self.call("edit_graph", op="set_positions", backdrops=[box])
        self.assertEqual(self.graph()["backdrops"], [box])
        self.assertEqual(self.call("undo_state")["undoDepth"], depth)
        with self.assertRaises(HelperError):
            self.call("edit_graph", op="set_positions", backdrops=[dict(box, w=0)])
        self.call("edit_graph", op="set_positions", positions={"noise": [1.0, 2.0]}, backdrops=[])
        self.assertNotIn("backdrops", self.graph())
        self.assertEqual(self.node("noise")["ui"], {"x": 1.0, "y": 2.0})
        # So do the pins the links are drawn through (Blender's reroutes): as given, all of them.
        link = next(c for c in self.graph()["connections"] if c["to"] == "noise")
        route = {"from": link["from"], "to": "noise", "input": link["input"], "via": ["pin1", "pin2"]}
        pins = {"points": [{"id": "pin1", "x": 120.0, "y": 300.0}, {"id": "pin2", "x": 160.0, "y": 280.5}], "routes": [route]}
        self.assertEqual(self.call("edit_graph", op="set_positions", pins=pins)["graphError"], "")
        self.assertEqual(self.graph()["pins"], pins)
        self.assertEqual(self.call("undo_state")["undoDepth"], depth)
        for bad in ({"points": "pin1"}, {"points": [{"id": "pin1", "x": "a", "y": 0}], "routes": []},
                    {"points": [], "routes": [dict(route, via=[])]}, {"points": [], "routes": [dict(route, to=3)]}, []):
            with self.assertRaises(HelperError):
                self.call("edit_graph", op="set_positions", pins=bad)
        self.assertEqual(self.graph()["pins"], pins)
        self.call("edit_graph", op="set_positions", pins={"points": [], "routes": []})
        self.assertNotIn("pins", self.graph())

    def test_node_library_and_presets(self):
        folders = self.h.call("nodes_folder")
        self.assertEqual(os.path.normcase(os.path.abspath(folders["userDir"])), os.path.normcase(os.path.abspath(self.nodes_dir)))
        # Save as Node: a compound to the user's library; the compound then holds the saved copy.
        self.call("edit_graph", op="group", names=["noise", "color"], name="look")
        self.call("edit_graph", op="compound_expose", name="look", param="amount", target="noise.amplitude")
        preview = self.call("node_file", node="look", label="My Look", author="Tester", category="look")
        self.assertEqual((preview["file"]["label"], preview["file"]["version"]), ("My Look", 1))
        self.assertFalse(os.path.exists(self.nodes_dir))  # a preview writes nothing
        r = self.call("save_node", node="look", label="My Look", author="Tester", category="look")
        node_id = r["id"]
        self.assertTrue(os.path.isfile(r["path"]))
        self.assertEqual((r["version"], r["undo"]["undoLabel"]), (1, "save node"))
        self.assertIn(node_id, {n["id"] for n in self.h.call("library_nodes")["nodes"]})
        library = self.call("node_spec", node="look")["spec"]["library"]
        self.assertEqual((library["id"], library["updateAvailable"]), (node_id, False))
        self.call("undo")
        self.assertNotIn("library", self.call("node_spec", node="look")["spec"])
        self.assertTrue(os.path.isfile(r["path"]))  # the file stays, as Maya's does
        # Import: a node file checked and copied into a folder.
        other = os.path.join(self.tmp, "imported")
        r = self.h.call("import_node", path=r["path"], dir=other)
        self.assertEqual((r["id"], os.path.dirname(os.path.abspath(r["path"]))), (node_id, os.path.abspath(other)))
        bad = os.path.join(self.tmp, "bad.glnode")
        with open(bad, "w") as f:
            f.write("{\"glnode\": 1}")
        with self.assertRaises(HelperError):
            self.h.call("import_node", path=bad, dir=other)
        # Script nodes: a template, its errors, one added after a node, edited, saved to the library.
        template = self.h.call("script_template", label="Lift")["definition"]
        self.assertEqual(self.h.call("script_errors", definition=template)["errors"], [])
        broken = dict(template, script="per point { P += ; }")
        errors = self.h.call("script_errors", definition=broken)["errors"]
        self.assertEqual(errors[0]["part"], "script")
        r = self.call("new_script_node", definition=template, after="width")
        name = r["name"]
        self.assertEqual((name, r["undo"]["undoLabel"]), ("lift", "add script node"))
        self.assertIn(("width", name, "strands"), self.links())
        self.assertIn((name, "resample", "strands"), self.links())
        self.assertEqual(self.call("evaluate", density=0.05)["errors"], [])
        with self.assertRaises(HelperError) as e:
            self.call("set_script", node=name, definition=broken)
        self.assertEqual(e.exception.code, "script")
        edited = dict(template, label="Lift More")
        self.assertEqual(self.call("set_script", node=name, definition=edited)["undo"]["undoLabel"], "edit script node")
        r = self.call("save_script_node", node=name)
        self.assertEqual((r["version"], os.path.dirname(os.path.abspath(r["path"]))), (template["version"], os.path.abspath(self.nodes_dir)))
        self.assertEqual(self.call("save_script_node", node=name)["version"], template["version"] + 1)  # one up from the library's
        with self.assertRaises(HelperError):
            self.call("save_script_node", node=name, asNew=True)  # the library has its id
        # Save as Preset: the graph with its scalp's size, about and layout, in the user's preset folder.
        self.call("edit_graph", op="set_positions", backdrops=[{"title": "Look", "x": 0, "y": 0, "w": 100, "h": 80}])
        r = self.call("save_preset", name="my look", about={"title": "My look", "category": "hair"})
        with open(r["path"]) as f:
            preset = json.load(f)
        self.assertEqual(os.path.dirname(os.path.abspath(r["path"])), os.path.abspath(self.presets_dir))
        self.assertEqual((preset["about"]["title"], preset["backdrops"][0]["title"]), ("My look", "Look"))
        self.assertGreater(preset["size"], 0.0)
        self.assertIn("my look", {p["name"] for p in self.h.call("list_presets")["presets"]})
        with self.assertRaises(HelperError) as e:
            self.call("save_preset", name="my look")
        self.assertEqual(e.exception.code, "exists")
        self.call("save_preset", name="my look", overwrite=True)
        with self.assertRaises(HelperError):
            self.call("save_preset", name="a/b")


# ---- The pipeline (docs/BLENDER.md, "Pipeline") ---------------------------------------------------------

class PipelineTest(HelperTestCase):
    """Game exports, the scales, strands as a mesh, Simulate Range in steps with the host's scalp
    frames, bakes, colliders and other scalps, Transfer Groom to Scalp and the sample meshes."""

    def call(self, cmd, **args):
        return self.h.call(cmd, **args)

    def with_cards(self, s):
        graph = self.call("get_graph", session=s)["graph"]
        into = next(c for c in graph["connections"] if c["to"] == graph["output"] and c["input"] == "strands")
        graph["nodes"].append({"name": "cards", "type": "Cards", "params": {"maxCards": 30}})
        graph["connections"].append({"from": into["from"], "to": "cards", "input": "strands"})
        into["from"] = "cards"
        self.call("set_graph", session=s, graph=graph)

    def compat_session(self):
        s = self.call("new_session")["session"]
        self.call("load_groom", session=s, path=COMPAT_GROOM)
        self.call("set_time", session=s, frame=1)
        return s

    def mesh(self, ref):
        return {k: self.h.array(ref[k]) for k in ("points", "faceCounts", "faceIndices")}

    def test_engine_strand_exports(self):
        s = self.head_session()
        unreal = os.path.join(self.tmp, "head_unreal.abc")
        r = self.call("export", session=s, format="unrealGroom", path=unreal, density=0.1, name="head")
        self.assertGreater(os.path.getsize(unreal), 0)
        self.assertEqual(r["frames"], 1)
        self.assertTrue(r["objects"])
        unity = os.path.join(self.tmp, "head_unity.abc")
        r = self.call("export", session=s, format="unityStrands", path=unity, density=0.1, pointsPerStrand=16,
                      frames={"first": 1, "last": 3})
        self.assertEqual(r["frames"], 3)
        self.assertEqual({o["pointsPerStrand"] for o in r["objects"]}, {16})
        self.assertEqual({(o["minPoints"], o["maxPoints"]) for o in r["objects"]}, {(16, 16)})
        e = self.h.request("export", session=s, format="unityStrands", path=unity, pointsPerStrand=200)
        self.assertEqual(e["error"]["code"], "bad_request")
        e = self.h.request("export", session=s, format="unrealGroom", path=unreal, lod="tiny")
        self.assertEqual(e["error"]["code"], "bad_request")
        # Frame by frame, the engine's groom too.
        ex = self.call("export_open", session=s, format="unrealGroom", path=os.path.join(self.tmp, "frames.abc"), lod="mid")["export"]
        for f in (1, 2):
            self.call("export_frame", session=s, export=ex, frame=f, density=0.1)
        self.assertEqual(self.call("export_close", session=s, export=ex)["frames"], 2)
        self.assertEqual(self.h.request("export", session=s, format="fbx", path=unreal)["error"]["code"], "bad_request")

    def test_card_exports_gltf_unreal_and_unity(self):
        s = self.head_session()
        self.with_cards(s)
        graph = self.call("get_graph", session=s)["graph"]
        glb = os.path.join(self.tmp, "head_cards.glb")
        r = self.call("export", session=s, format="gltfCards", path=glb, name="head", resolution=64)
        self.assertEqual(len(r["files"]), 1)
        self.assertTrue(os.path.samefile(r["files"][0], glb))
        self.assertEqual(r["meshes"], ["head_cards", "head_scalpCap"])
        with open(glb, "rb") as f:
            self.assertEqual(f.read(4), b"glTF")
        r = self.call("export", session=s, format="gltfCards", path=os.path.join(self.tmp, "lods.gltf"), name="head",
                      resolution=32, lods=["mid", "low"], cap=False)
        self.assertEqual(r["meshes"], ["head_mid", "head_low"])
        self.assertEqual(len(r["files"]), 2 + 6)  # the .gltf, its .bin, three PNGs a mesh
        self.assertEqual(self.call("get_graph", session=s)["graph"], graph, "the levels of detail are baked from copies")
        self.assertEqual(self.h.request("export", session=s, format="gltfCards", path=glb, lods=["huge"])["error"]["code"],
                         "bad_request")

        out = os.path.join(self.tmp, "unreal")
        r = self.call("export", session=s, format="unrealCards", directory=out, name="head", resolution=64)
        self.assertEqual(sorted(os.path.basename(p) for p in r["maps"]),
                         sorted(["head_%s.tga" % m for m in ("Depth", "Coverage", "Tangent", "Attribute", "Color", "AO", "Normal",
                                                              "ScalpColor", "ScalpCoverage")]))
        with open(r["manifest"]) as f:
            manifest = json.load(f)
        self.assertEqual(manifest["mesh"], "head_cards.fbx")
        self.assertEqual(manifest["scalpCap"], "head_scalpCap.fbx")
        self.assertGreater(manifest["cards"], 0)
        self.assertEqual(manifest["maps"]["Color"], "head_Color.tga")
        cards = r["meshes"]["cards"]
        m = self.mesh(cards)
        self.assertTrue((m["faceCounts"] == 4).all())
        self.assertEqual(cards["corners"], len(m["faceIndices"]))
        self.assertEqual(self.h.array(cards["uvs"]).shape, (cards["corners"], 2))
        self.assertEqual(self.h.array(cards["normals"]).shape, (cards["corners"], 3))
        tint = self.h.array(cards["colors"]["groomLabTint"])
        self.assertEqual(tint.shape, (cards["corners"], 4))
        self.assertTrue(np.allclose(tint[:, 3], 1.0))
        self.assertEqual(cards["triangles"], manifest["triangles"])
        self.assertEqual(cards["fbx"], r["fbx"])
        cap = self.mesh(r["meshes"]["scalpCap"])
        self.assertEqual(len(cap["points"]), len(HEAD[0]))
        # The cap stands just off the skin (host space: Blender's meters).
        lift = np.linalg.norm(cap["points"] - to_blender(HEAD[0]), axis=1)
        self.assertTrue(np.median(lift) > 0 and (lift < 0.01).all())  # (a point no face uses stays)

        out = os.path.join(self.tmp, "unity")
        r = self.call("export", session=s, format="unityCards", directory=out, name="head", resolution=32)
        self.assertEqual(sorted(os.path.basename(p) for p in r["maps"]),
                         sorted(["head_%s.png" % m for m in ("BaseColor", "Mask", "AO", "Tangent", "Depth", "Attribute", "Normal",
                                                              "ScalpBaseColor")]))
        with open(r["manifest"]) as f:
            manifest = json.load(f)
        self.assertEqual(manifest["engine"], "Unity")
        self.assertEqual(manifest["materials"]["HDRP"]["Base Color Map"], "head_BaseColor.png")
        self.assertEqual(r["meshes"]["cards"]["material"], "head_cards_mat")
        self.assertEqual(r["fbxSettings"], {"upAxis": "Y", "units": "m"})

        # The groom's strands as one mesh: tubes of 4 sides, at most 40 strands (thinned by ID).
        r = self.call("strand_mesh", session=s, style="tube", sides=4, maxStrands=40, density=0.2, strandUVs=True)
        self.assertEqual(r["strands"], 40)
        self.assertTrue(r["capped"])
        m = r["meshes"]["strands"]
        self.assertEqual(sum(self.h.array(m["faceCounts"])), m["corners"])
        self.assertIn("strandUVs", m["uvSets"])
        self.assertEqual(self.call("strand_mesh", session=s, nodes=True)["nodes"], [])  # no StrandMesh node

    def test_scales_mesh_maps_and_instancer(self):
        s = self.head_session(preset="dragon_scales")
        r = self.call("scale_mesh", session=s)
        scales = r["meshes"]["scales"]
        self.assertGreater(scales["scales"], 0)
        m = self.mesh(scales)
        self.assertTrue((m["faceCounts"] == 3).all())
        self.assertEqual(set(scales["uvSets"]), {"scaleUV", "scaleData"})
        self.assertEqual(set(scales["colors"]), {"scaleColor", "scaleRootColor"})
        fbx = os.path.join(self.tmp, "head_scales.fbx")
        r = self.call("export", session=s, format="scalesFbx", path=fbx)
        self.assertEqual(r["fbx"], fbx)
        self.assertEqual(r["meshes"]["scales"]["fbx"], fbx)
        self.assertEqual(self.h.request("export", session=s, format="scalesFbx", path=fbx + ".abc")["error"]["code"], "bad_request")
        maps = self.call("bake_scale_maps", session=s, directory=self.tmp, name="head", resolution=64)
        self.assertEqual(sorted(os.path.basename(p) for p in maps["files"]),
                         sorted(["head_ScaleNormal.png", "head_ScaleHeight.png", "head_ScaleColor.png"]))
        self.assertGreater(maps["maxHeight"], 0.0)
        usd = os.path.join(self.tmp, "head_scales.usda")
        self.call("export", session=s, format="scalesUsd", path=usd, density=0.2)
        with open(usd) as f:
            self.assertIn("PointInstancer", f.read())
        plain = self.head_session()
        self.assertEqual(self.h.request("scale_mesh", session=plain)["error"]["code"], "state")

    def add_simulate(self, s):
        self.call("edit_graph", session=s, op="add_node", type="Simulate", name="sim")
        self.call("edit_graph", session=s, op="insert_on_link", **{"from": "guides"}, to="interpolate", input="guides", node="sim")
        self.assertEqual(self.call("simulate_status", session=s)["nodes"], ["sim"])

    def test_simulate_range_in_steps_fed_by_the_scalps_frames(self):
        moved, still = self.compat_session(), self.compat_session()
        for s in (moved, still):
            self.add_simulate(s)
        scalp = self.call("get_scalp", session=moved)
        rest = self.h.array(scalp["arrays"]["points"])
        frames = []
        for f in range(1, 11):  # the head swings 10 cm along X and comes back by frame 10
            p = rest.copy()
            p[:, 0] += 0.10 * np.sin(np.pi * (f - 1) / 9.0)
            frames.append(p)
        r = self.call("set_scalp_frames", session=moved, first=1, **self.h.put(points=np.concatenate(frames)))
        self.assertEqual((r["first"], r["last"], r["frames"]), (1, 10, 10))
        # Steps of at most 1 ms: the range runs over several requests, the protocol answering between.
        job = self.call("simulate_range", session=moved, last=10, budgetMs=1)
        polls = 0
        while job["running"]:
            self.assertTrue(self.call("ping")["pong"])
            job = self.call("simulate_poll", session=moved)
            polls += 1
            self.assertLess(polls, 200)
        self.assertTrue(job["done"])
        self.assertEqual(job["status"][0]["ranges"][-1][-1], 10)
        self.assertTrue(self.call("simulate_range", session=still, last=10, budgetMs=0)["done"])
        positions = []
        for s in (moved, still):
            self.call("set_time", session=s, frame=10)
            r = self.call("evaluate", session=s, density=0.2, space="groom")
            positions.append(self.h.array(r["arrays"]["positions"]))
        self.assertEqual(positions[0].shape, positions[1].shape)
        self.assertFalse(np.allclose(positions[0], positions[1], atol=1e-3), "the swing moved the simulated hair")
        # A job can be stopped; the frames stepped stay.
        self.call("simulate_clear", session=still)
        job = self.call("simulate_range", session=still, last=10, budgetMs=1)
        self.assertTrue(self.call("simulate_cancel", session=still)["cancelled"] or not job["running"])
        self.assertFalse(self.call("simulate_poll", session=still)["running"])

        # Bake: one undo step; undo reads no bake, redo the bake again.
        r = self.call("simulate_bake", session=moved, last=10, directory=self.tmp, name="compat")
        path = r["written"]["sim"]
        self.assertTrue(os.path.isfile(path))
        status = self.call("simulate_status", session=moved)
        self.assertTrue(status["status"][0]["baked"])
        self.assertIn("sim", status["bakes"])
        self.assertEqual(r["undo"]["undoLabel"], "bake simulation")
        saved = os.path.join(self.tmp, "baked.groom")
        self.call("save_groom", session=moved, path=saved)
        with open(saved) as f:
            self.assertIn("sim", json.load(f)["simCaches"])
        self.call("undo", session=moved)
        self.assertFalse(self.call("simulate_status", session=moved)["status"][0]["baked"])
        self.call("redo", session=moved)
        self.assertTrue(self.call("simulate_status", session=moved)["status"][0]["baked"])
        # Clear: the frames and the bakes go (the bakes as one undo step).
        r = self.call("simulate_clear", session=moved)
        self.assertEqual(r["clearedBakes"], ["sim"])
        self.assertEqual((r["status"][0]["frames"], r["status"][0]["baked"]), (0, False))
        self.call("undo", session=moved)
        self.assertTrue(self.call("simulate_status", session=moved)["status"][0]["baked"])
        self.assertEqual(self.call("set_scalp_frames", session=moved)["frames"], 0)
        plain = self.head_session()
        self.assertEqual(self.h.request("simulate_range", session=plain, last=5)["error"]["code"], "state")

    def test_colliders_prototypes_and_other_scalps(self):
        s = self.head_session()
        box = {"points": [[-0.1, -0.1, 0.0], [0.1, -0.1, 0.0], [0.1, 0.1, 0.0], [-0.1, 0.1, 0.0]],
               "faceCounts": [4], "faceIndices": [0, 1, 2, 3]}
        r = self.call("set_meshes", session=s, meshes={"collar": box})
        self.assertEqual([(c["name"], c["points"], c["triangles"]) for c in r["colliders"]], [("collar", 4, 2)])
        moved = {"points": [[x, y, z - 0.05] for x, y, z in box["points"]]}  # the same mesh, posed
        r = self.call("set_meshes", session=s, meshes={"collar": moved})
        self.assertEqual(r["colliders"][0]["points"], 4)
        self.assertEqual(self.h.request("set_meshes", session=s, meshes={"new": moved})["error"]["code"], "bad_request")
        self.assertEqual(self.h.request("set_meshes", session=s, meshes={"scalp": box})["error"]["code"], "bad_request")
        r = self.call("set_meshes", session=s, kind="prototype", meshes={"plate": box})
        self.assertEqual([(p["name"], p["points"], p["triangles"]) for p in r["prototypes"]], [("plate", 4, 2)])
        ear = {"name": "ear", "points": [[0.3 + 0.02 * (i % 3), 0.0, 0.02 * (i // 3)] for i in range(9)],
               "faceCounts": [4] * 4, "faceIndices": [0, 1, 4, 3, 1, 2, 5, 4, 3, 4, 7, 6, 4, 5, 8, 7]}
        r = self.call("set_scalps", session=s, scalps=[ear])
        self.assertEqual([(x["name"], x["points"], x["triangles"]) for x in r["scalps"]], [("ear", 9, 8)])
        self.assertEqual(self.h.request("set_scalps", session=s, scalps=[dict(ear, name="scalp")])["error"]["code"], "bad_request")
        saved = os.path.join(self.tmp, "inputs.groom")
        self.call("save_groom", session=s, path=saved)
        with open(saved) as f:
            groom = json.load(f)
        self.assertIn("collar", groom["colliders"])
        self.assertEqual([x["name"] for x in groom["scalps"]], ["ear"])
        self.assertIn("plate", groom["prototypes"])
        r = self.call("set_meshes", session=s, meshes={"collar": None})
        self.assertEqual(r["colliders"], [])
        self.assertEqual(self.call("set_scalps", session=s, scalps=[])["scalps"], [])

    def hierarchy_points(self, s):
        g = self.call("get_guides", session=s, source="hierarchy", space="groom")
        return self.h.array(g["arrays"]["points"])

    def test_transfer_to_another_scalp_is_one_undo_step(self):
        s = self.compat_session()
        scalp = self.call("get_scalp", session=s)
        a = scalp["arrays"]
        points = self.h.array(a["points"])
        counts, indices = self.h.array(a["faceCounts"]), self.h.array(a["faceIndices"])
        uvs = self.h.array(a["uvs"]) if "uvs" in a else None
        before = self.hierarchy_points(s)
        moved = points.copy()
        moved[:, 0] += 0.05  # 5 cm along X (host space: meters)
        arrays = dict(points=moved, faceCounts=counts, faceIndices=indices)
        if uvs is not None:
            arrays["uvs"] = uvs
        r = self.call("transfer", session=s, method="bounds", **self.h.put(**arrays))
        self.assertEqual(r["method"], "bounds")
        self.assertGreater(r["guides"], 0)
        self.assertEqual(r["undo"]["undoLabel"], "transfer")
        after = self.hierarchy_points(s)
        self.assertEqual(after.shape, before.shape)
        self.assertTrue(np.allclose(after[:, 0] - before[:, 0], 5.0, atol=0.05))
        self.assertTrue(np.allclose(after[:, 1:], before[:, 1:], atol=0.05))
        self.assertEqual(self.call("groom_info", session=s)["scalp"]["points"], len(points))
        self.assertTrue(np.allclose(self.h.array(self.call("get_scalp", session=s)["arrays"]["points"]), moved, atol=1e-6))
        u = self.call("undo", session=s)
        self.assertTrue(u["scalpChanged"])
        self.assertTrue(np.allclose(self.hierarchy_points(s), before, atol=1e-4))
        self.assertTrue(self.call("redo", session=s)["scalpChanged"])
        self.assertTrue(np.allclose(self.hierarchy_points(s), after, atol=1e-4))
        # Markers: given as host points, snapped onto the two meshes and stored; snapMarkers alone
        # changes nothing.
        picks = [0, 60, 120, 200]
        markers = [{"old": moved[i].tolist(), "new": (moved[i] + [0.0, 0.0, 0.02]).tolist()} for i in picks]
        lifted = moved.copy()
        lifted[:, 2] += 0.02
        arrays["points"] = lifted
        depth = self.call("undo_state", session=s)["undoDepth"]
        snapped = self.call("transfer", session=s, method="markersAlign", markers=markers, snapMarkers=True,
                            **self.h.put(**arrays))["markers"]
        self.assertEqual(len(snapped), 4)
        self.assertTrue(all(m["old"]["face"] >= 0 and m["new"]["face"] >= 0 for m in snapped))
        self.assertEqual(self.call("undo_state", session=s)["undoDepth"], depth)
        r = self.call("transfer", session=s, method="markersAlign", markers=markers, **self.h.put(**arrays))
        self.assertEqual(r["method"], "markersAlign")
        self.assertEqual(len(r["markers"]), 4)
        self.assertTrue(np.allclose(self.hierarchy_points(s)[:, 1] - after[:, 1], 2.0, atol=0.05))  # host Z is groom Y
        self.assertEqual(len(self.call("transfer_markers", session=s)["markers"]), 4)
        self.assertEqual(self.h.request("transfer", session=s, method="markersWarp", markers=markers[:2],
                                        **self.h.put(**arrays))["error"]["code"], "bad_request")
        self.assertEqual(self.h.request("transfer", session=s, method="nearest", **self.h.put(**arrays))["error"]["code"],
                         "bad_request")

    def test_sample_meshes(self):
        r = self.call("sample_meshes")
        self.assertEqual([m["id"] for m in r["samples"]], ["nika", "manny", "wolf", "buddy"])
        for m in r["samples"]:
            self.assertTrue(os.path.isfile(m["file"]), m["file"])
            self.assertEqual(m["license"], "CC BY 4.0")
        self.assertEqual(r["samples"][2]["part"], "wolf_body")
        self.assertEqual([m["kind"] for m in r["samples"]], ["mesh", "mesh", "mesh", "rig"])  # Buddy: an FBX rig
        self.assertTrue(r["samples"][3]["file"].endswith("buddy_rig.fbx"))
        self.assertEqual(r["samples"][3]["part"], "buddy_body")
        self.assertEqual(self.call("hello")["features"]["pipeline"], 1)

    def test_import_alembic_curves_as_guides(self):
        # Curves written by an Alembic export (GroomLab's: Y up, cm) become the guides an ImportCurves
        # node reads; the strands follow them. A file with no curves, or none, is refused.
        s = self.head_session()
        abc = os.path.join(self.tmp, "curves.abc")
        self.call("export", session=s, format="alembic", path=abc, frames={"first": 1, "last": 1}, density=0.02)
        t = self.head_session(preset=None)
        r = self.call("import_alembic_curves", session=t, path=abc, samples=12)
        self.assertGreater(r["curves"], 10)
        self.assertEqual(r["graphError"], "")
        graph = self.call("get_graph", session=t)["graph"]
        node = next(n for n in graph["nodes"] if n["name"] == "guides")
        self.assertEqual(node["type"], "ImportCurves")
        self.assertEqual((node["params"]["path"], node["params"]["samples"]), (abc, 12))
        self.assertIn({"from": "scalp", "to": "guides", "input": "scalp"}, graph["connections"])
        ev = self.call("evaluate", session=t, density=0.1)
        self.assertEqual(ev["errors"], [])
        self.assertGreater(len(self.h.array(ev["arrays"]["pointCounts"])), 0)
        with self.assertRaises(HelperError) as e:
            self.call("import_alembic_curves", session=t, path=os.path.join(self.tmp, "none.abc"))
        self.assertEqual(e.exception.code, "io")


def write_tga(path, width, height, pixels):
    """An uncompressed 24-bit TGA (top-left origin) of `pixels` (rows top to bottom, RGB bytes)."""
    header = bytes([0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, width & 255, width >> 8, height & 255, height >> 8, 24, 0x20])
    rgb = np.frombuffer(bytes(pixels), np.uint8).reshape(-1, 3)
    with open(path, "wb") as f:
        f.write(header + rgb[:, ::-1].tobytes())  # BGR


def make_sphere(rings=16, segments=32, radius=1.0):
    """A UV sphere (Y up), its triangles flat."""
    points, tris = [], []
    for r in range(rings + 1):
        theta = np.pi * r / rings
        for s in range(segments + 1):
            phi = 2.0 * np.pi * s / segments
            points.append((radius * np.sin(theta) * np.cos(phi), radius * np.cos(theta), radius * np.sin(theta) * np.sin(phi)))
    at = lambda r, s: r * (segments + 1) + s  # noqa: E731
    for r in range(rings):
        for s in range(segments):
            tris += [(at(r, s), at(r, s + 1), at(r + 1, s)), (at(r, s + 1), at(r + 1, s + 1), at(r + 1, s))]
    return np.array(points, np.float32), np.array(tris, np.int32)


def look_rotation(forward, up=(0.0, 0.0, 1.0)):
    """A camera's rotation (rows; columns its X right, Y up, Z back axes) looking along `forward`."""
    fw = np.asarray(forward, np.float64)
    fw /= np.linalg.norm(fw)
    x = np.cross(fw, up)
    x /= np.linalg.norm(x)
    y = np.cross(x, fw)
    return np.stack([x, y, -fw], axis=1)


def project(rotation, eye, focal, width, height, p):
    c = np.asarray(rotation).T @ (np.asarray(p, np.float64) - np.asarray(eye, np.float64))
    return [0.5 * width + focal * c[0] / -c[2], 0.5 * height - focal * c[1] / -c[2]]


class PhotosTest(HelperTestCase):
    """Groom Tools > Photos: the photos kept with the groom (its file's referencePhotos, groom space),
    the camera solved from point pairs in the host's space, and Guides from Photos."""

    def call(self, cmd, **args):
        return self.h.call(cmd, **args)

    @unittest.skipIf(HEAD is None, "no test head")
    def test_photos_kept_with_the_groom_and_the_camera_solve(self):
        s = self.head_session()
        pts = to_blender(HEAD[0]).astype(np.float64)
        center = 0.5 * (pts.min(axis=0) + pts.max(axis=0))
        width, height, focal = 800, 1000, 1400.0
        eye = center + np.array([0.12, -0.55, 0.05])
        rot = look_rotation(center - eye)
        picks = pts[np.linspace(0, len(pts) - 1, 9).round().astype(int)]
        pairs = [{"image": project(rot, eye, focal, width, height, p), "mesh": p.tolist()} for p in picks]
        # A first guess off by a few degrees and centimeters, the focal length unknown.
        guess = look_rotation(center - eye + np.array([0.03, 0.0, -0.02]))
        r = self.call("solve_camera", session=s, pairs=pairs, width=width, height=height, focalPx=1000.0,
                      initial={"rotation": guess.tolist(), "position": (eye + 0.03).tolist()})
        self.assertLess(r["rms"], 0.05)
        self.assertTrue(r["focalSolved"])
        self.assertAlmostEqual(r["focalPx"], focal, delta=1.0)
        np.testing.assert_allclose(r["position"], eye, atol=1e-4)
        np.testing.assert_allclose(r["rotation"], rot, atol=1e-4)
        self.assertEqual(len(r["errors"]), 9)
        # The focal length kept: 4 pairs line it up; a pending pair is left out (its error null).
        four = pairs[:4] + [{"image": [10.0, 10.0], "mesh": None}]
        r = self.call("solve_camera", session=s, pairs=four, width=width, height=height, focalPx=focal, keepFocal=True,
                      initial={"rotation": guess.tolist(), "position": (eye + 0.03).tolist()})
        self.assertFalse(r["focalSolved"])
        self.assertEqual(r["focalPx"], focal)
        np.testing.assert_allclose(r["position"], eye, atol=1e-3)
        self.assertIsNone(r["errors"][4])
        with self.assertRaises(HelperError) as e:  # 3 pairs: too few
            self.call("solve_camera", session=s, pairs=pairs[:3], width=width, height=height, focalPx=focal)
        self.assertEqual(e.exception.code, "state")
        # Kept with the groom in groom space (Y up, cm), as the app's Reference window keeps them; the
        # flow arrows the app drew stay when the host stores the photo again.
        photo = {"name": "front", "image": "C:/photos/front.png", "width": width, "height": height,
                 "camera": {"rotation": rot.tolist(), "position": eye.tolist(), "focalPx": focal},
                 "pairs": [dict(pairs[0], error=0.5), {"image": [3.0, 4.0], "mesh": None}], "rms": 0.5}
        self.assertEqual(self.call("set_reference_photos", session=s, photos=[photo])["photos"], 1)
        path = os.path.join(self.tmp, "photos.groom")
        self.call("save_groom", session=s, path=path)
        with open(path) as f:
            stored = json.load(f)["referencePhotos"]
        self.assertEqual(stored[0]["name"], "front")
        np.testing.assert_allclose(stored[0]["camera"]["position"], to_groom(eye[None])[0], atol=1e-3)
        # Its axes turned as directions are: host (x, y, z) is groom (x, z, -y).
        np.testing.assert_allclose(np.asarray(stored[0]["camera"]["rotation"])[:, 2], [rot[0, 2], rot[2, 2], -rot[1, 2]], atol=1e-9)
        self.assertEqual(stored[0]["camera"]["focal_px"], focal)
        self.assertEqual(stored[0]["state"]["landmarks"][1], {"image": [3.0, 4.0], "mesh": None})
        stored[0]["state"]["strokes"] = [{"points": [[1, 2], [3, 4]], "directed": True}]
        t = self.call("new_session")["session"]
        with open(path) as f:
            whole = json.load(f)
        whole["referencePhotos"] = stored
        with open(path, "w") as f:
            json.dump(whole, f)
        self.call("load_groom", session=t, path=path)
        back = self.call("reference_photos", session=t)["photos"]
        self.assertEqual(len(back), 1)
        np.testing.assert_allclose(back[0]["camera"]["position"], eye, atol=1e-6)
        np.testing.assert_allclose(back[0]["camera"]["rotation"], rot, atol=1e-9)
        np.testing.assert_allclose(back[0]["pairs"][0]["mesh"], pairs[0]["mesh"], atol=1e-6)
        self.assertEqual((back[0]["pairs"][0]["error"], back[0]["rms"], back[0]["strokes"]), (0.5, 0.5, 1))
        self.call("set_reference_photos", session=t, photos=[dict(back[0], pairs=[])])
        again = self.call("reference_photos", session=t)["photos"][0]
        self.assertEqual((again["pairs"], again["strokes"]), ([], 1))
        self.call("set_reference_photos", session=t, photos=[])
        self.assertEqual(self.call("reference_photos", session=t)["photos"], [])
        with self.assertRaises(HelperError):
            self.call("set_reference_photos", session=t, photos=[photo, photo])

    def test_guides_from_photos(self):
        # The core's own case (test_groomlab_core: vertical strands over a ball on a plain backdrop),
        # through the helper: the photo kept with the groom, the guides into fromReference, one undo step.
        s = self.call("new_session", space={"upAxis": "Y", "metersPerUnit": 0.01})["session"]
        points, tris = make_sphere()
        self.call("set_scalp", session=s, **self.h.put(points=points, faceCounts=np.full(len(tris), 3, np.int32),
                                                        faceIndices=tris.ravel()))
        self.call("new_groom", session=s)
        size = 160
        focal = 0.5 * size / np.tan(np.radians(20.0))
        pixels = bytearray([255] * (size * size * 3))
        r = focal * 1.1 / 6.0
        for y in range(size):
            for x in range(0, size, 3):
                dx, dy = x - size / 2.0, y - size / 2.0
                if abs(dx) < r and -r < dy < 1.6 * r:
                    i = 3 * (y * size + x)
                    pixels[i:i + 3] = b"\x40\x30\x20"
        image = os.path.join(self.tmp, "front.tga")
        write_tga(image, size, size, pixels)
        cam = {"rotation": np.eye(3).tolist(), "position": [0.0, 0.0, 6.0], "focalPx": focal}
        self.call("set_reference_photos", session=s, photos=[
            {"name": "front", "image": image, "width": size, "height": size, "camera": cam},
            {"name": "side", "image": image, "width": size, "height": size}])  # not lined up: left out
        r = self.call("guides_from_photos", session=s, guideCount=80, symmetry="none", maxLength=1.2)
        self.assertEqual((r["group"], r["views"], r["symmetry"]), ("fromReference", 1, "none"))
        self.assertGreater(r["guides"], 5)
        self.assertIn("melanin", r["look"])
        info = self.call("guides_info", session=s)
        group = next(g for g in info["groups"] if g["name"] == "fromReference")
        self.assertEqual(group["guides"], r["guides"])
        g = self.call("get_guides", session=s, source="hierarchy")
        a = g["arrays"]
        counts, pts = self.h.array(a["counts"]), self.h.array(a["points"])
        starts = np.concatenate([[0], np.cumsum(counts)[:-1]])
        down = sum(1 for st, n in zip(starts, counts) if pts[st + n - 1][1] < pts[st][1] - 0.3)
        self.assertGreater(down, 0.6 * len(counts))
        # Again: the group replaced, not added to; undo takes it back.
        again = self.call("guides_from_photos", session=s, guideCount=80, symmetry="none", maxLength=1.2)
        self.assertEqual(self.call("guides_info", session=s)["guides"], again["guides"])
        self.call("undo", session=s)
        self.assertEqual(self.call("guides_info", session=s)["guides"], r["guides"])
        with self.assertRaises(HelperError) as e:
            self.call("guides_from_photos", session=s, photos=["side"])
        self.assertEqual(e.exception.code, "state")


if __name__ == "__main__":
    unittest.main(verbosity=2)
