# SPDX-License-Identifier: Apache-2.0
"""GroomLab's V-Ray scene export (.vrscene, engine/core vrscene_writer.h) rendered in V-Ray
Standalone: ctest vray.render (label vray; skipped without vray.exe).

    python test_vray.py <vray.exe> <groomlab-cli> <out dir> [--kick kick.exe --procedural dir]

The lookdev head groom (tests/data/app/head_graph.json on head_scalp.obj, a few tens of thousands of
strands) written by `groomlab-cli eval -o x.vrscene` and rendered by tests/data/app/vray_compare.py
(arnold_compare.py's camera, lights and scalp around an #include of it), small and with few
samples: the image is not black and the hair covers what its points project to; the pigment
renders brown and an RGB look (HairColor's color mode) blue, so each strand's color reaches the
shader; a moving groom (a Wind) over two frames is written as keys and renders blurred with motion
blur on. With kick and GroomLab's Arnold procedural (--kick, --procedural), the brown groom is
rendered in both (Arnold in linear sRGB, as V-Ray renders) and the hair's mean color compared;
the side-by-side is written to <out dir>/vray_vs_arnold.png. Standard library only.
"""

import base64
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
APP = os.path.join(ROOT, "tests", "data", "app")
SCALP = os.path.join(APP, "head_scalp.obj")
CAMERA = "camera eye 32 4 42 target 0 -3 0 fov 32"
VRAY = CLI = OUT = KICK = PROCEDURAL = ""


# ---- Grooms -----------------------------------------------------------------------------------

def read_obj_uv(path):
    """An OBJ's points, triangles (fans) and per-corner uvs, as a groom file's scalp holds them."""
    verts, uvs, tris, corner_uvs = [], [], [], []
    with open(path) as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            if p[0] == "v":
                verts.append([float(x) for x in p[1:4]])
            elif p[0] == "vt":
                uvs.append([float(x) for x in p[1:3]])
            elif p[0] == "f":
                corners = [q.split("/") for q in p[1:]]
                for k in range(1, len(corners) - 1):
                    for c in (corners[0], corners[k], corners[k + 1]):
                        tris.append(int(c[0]) - 1)
                        corner_uvs += uvs[int(c[1]) - 1] if len(c) > 1 and c[1] else [0.0, 0.0]
    return verts, tris, corner_uvs


def head_graph(density=60.0, color=None, wind=False, preset=None):
    """head_graph.json (strands along the scalp, combed down, clumped, brown), at `density` strands a
    cm2; `color`: HairColor's params replaced; `wind`: a Wind before the output (it moves);
    `preset`: a shipped preset's look (engine/assets/presets/<preset>.json: its nodes after its
    Interpolate) after the head's combing in place of the head's own."""
    with open(os.path.join(APP, "head_graph.json")) as f:
        graph = json.load(f)
    graph = {k: graph[k] for k in ("nodes", "connections", "output")}
    if preset:
        with open(os.path.join(ROOT, "engine", "assets", "presets", preset + ".json")) as f:
            look = json.load(f)
        keep = ["scalp", "guides", "scatter", "interpolate", "combAlong", "combDown", "collide"]
        nodes = [n for n in graph["nodes"] if n["name"] in keep]
        nodes += [n for n in look["nodes"] if n["type"] not in ("Scalp", "Guides", "Scatter", "Interpolate")]
        conns = [c for c in graph["connections"] if c["from"] in keep and c["to"] in keep]
        first = next(c["to"] for c in look["connections"] if c["from"] == "interpolate" and c["input"] == "strands")
        conns.append({"from": "collide", "to": first, "input": "strands"})
        conns += [c for c in look["connections"] if c["from"] not in ("scalp", "guides", "scatter", "interpolate")]
        graph = {"nodes": nodes, "connections": conns, "output": look["output"]}
    for node in graph["nodes"]:
        node.pop("ui", None)
        if node["type"] == "Scatter":
            node["params"]["density"] = density
        if node["type"] == "HairColor" and color is not None:
            node["params"] = dict(color)
    if wind:
        graph["nodes"].append({"name": "wind", "type": "Wind", "params": {"strength": 4.0, "speed": 120.0}})
        last = next(c for c in graph["connections"] if c["to"] == "output")
        last["to"] = "wind"
        graph["connections"].append({"from": "wind", "to": "output", "input": "strands"})
    graph["version"] = 1
    return graph


def write_groom(path, graph):
    verts, tris, uvs = read_obj_uv(SCALP)
    groom = {"format": "groomlab.groom", "version": 1, "framesPerSecond": 24.0, "source": "test_vray", "graph": graph,
             "scalp": {"points": [c for v in verts for c in v], "triangles": tris, "uvs": uvs, "colorSets": {}},
             "guides": [], "hierarchy": {"version": 1, "groups": [], "guides": []}, "guideCache": None, "scalpCache": None}
    with open(path, "w") as f:
        json.dump(groom, f)


# ---- Images -----------------------------------------------------------------------------------

def read_png(path):
    """An 8-bit PNG's (width, height, channels, rows of values), any filter; RGB or RGBA."""
    with open(path, "rb") as f:
        data = f.read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    pos, idat = 8, b""
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and color in (2, 6), "an 8-bit RGB or RGBA PNG"
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    n = 4 if color == 6 else 3
    raw = zlib.decompress(idat)
    stride = width * n
    rows, prev = [], bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - n] if i >= n else 0
            b = prev[i]
            c = prev[i - n] if i >= n else 0
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif kind == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return width, height, n, rows


def write_png(path, width, height, rows):
    """RGB rows (bytes, width x 3 each) as a PNG."""
    raw = b"".join(b"\0" + bytes(r) for r in rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def linear(v):
    v /= 255.0
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def pixels(path):
    w, h, n, rows = read_png(path)
    return w, h, [[tuple(row[x * n:x * n + n]) for x in range(w)] for row in rows]


def mean_linear(px, mask):
    """The mean linear RGB of the pixels where `mask` (rows of bools) is set."""
    tot, count = [0.0, 0.0, 0.0], 0
    for prow, mrow in zip(px, mask):
        for p, m in zip(prow, mrow):
            if m:
                for k in range(3):
                    tot[k] += linear(p[k])
                count += 1
    return [t / max(count, 1) for t in tot], count


# ---- The .vrscene -----------------------------------------------------------------------------

def unpack_floats(value):
    """A V-Ray packed float list (List...Hex("..."), plain hex or ZIPC) as floats."""
    data = re.search(r'Hex\("([^"]*)"\)', value).group(1)
    if data.startswith("ZIPC"):
        raw = zlib.decompress(base64.b64decode(data[20:]))
    else:
        raw = bytes.fromhex(data)
    return struct.unpack("<%df" % (len(raw) // 4), raw)


def project(points, width, height):
    """The camera's pixel bounds of `points` (the app's camera words, the vertical fov)."""
    words = CAMERA.split()
    eye, target, fov = [float(w) for w in words[2:5]], [float(w) for w in words[6:9]], float(words[10])
    f = [t - e for t, e in zip(target, eye)]
    n = math.sqrt(sum(c * c for c in f))
    f = [c / n for c in f]
    r = [f[1] * 0 - f[2] * 1, f[2] * 0 - f[0] * 0, f[0] * 1 - f[1] * 0]
    n = math.sqrt(sum(c * c for c in r))
    r = [c / n for c in r]
    u = [r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]]
    t = math.tan(math.radians(fov) * 0.5)
    xs, ys = [], []
    for i in range(0, len(points), 3):
        d = [points[i] - eye[0], points[i + 1] - eye[1], points[i + 2] - eye[2]]
        z = sum(a * b for a, b in zip(d, f))
        x = sum(a * b for a, b in zip(d, r)) / (z * t)
        y = sum(a * b for a, b in zip(d, u)) / (z * t)
        xs.append((x * height / width * 0.5 + 0.5) * width)
        ys.append((0.5 - y * 0.5) * height)
    return min(xs), min(ys), max(xs), max(ys)


def render(groom, name, size="200x112", lights="studio", extra=(), cli=True, samples=4):
    """vray_compare.py's render of `groom` (a .groom, written as .vrscene by groomlab-cli) to
    <OUT>/<name>.png; the result and the image's path."""
    png = os.path.join(OUT, name + ".png")
    cmd = [sys.executable, os.path.join(APP, "vray_compare.py"), "--groom", groom, "--scalp", SCALP, "--camera", CAMERA,
           "--out", png, "--size", size, "--vray", VRAY, "--lights", lights, "--samples", str(samples)]
    if cli:
        cmd += ["--cli", CLI]
    result = subprocess.run(cmd + list(extra), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True,
                            timeout=600)
    return result, png


class VRayRender(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.makedirs(OUT, exist_ok=True)
        cls.brown = os.path.join(OUT, "brown.groom")
        write_groom(cls.brown, head_graph())
        cls.blue = os.path.join(OUT, "blue.groom")
        write_groom(cls.blue, head_graph(color={"mode": "color", "color": [0.04, 0.08, 0.5]}))
        cls.moving = os.path.join(OUT, "moving.groom")
        write_groom(cls.moving, head_graph(density=20.0, wind=True))

    def check(self, result, png):
        self.assertEqual(result.returncode, 0, result.stdout[-3000:])
        self.assertNotRegex(result.stdout, r"(?i)\berror\b", result.stdout[-3000:])
        self.assertTrue(os.path.isfile(png), result.stdout[-2000:])

    def test_renders_hair_where_it_is(self):
        result, png = render(self.brown, "brown", extra=["--hide-scalp"])
        self.check(result, png)
        w, h, px = pixels(png)
        self.assertEqual(len(px[0][0]), 4, "V-Ray writes alpha")
        covered = [[p[3] > 128 for p in row] for row in px]
        lit, count = mean_linear(px, covered)
        self.assertGreater(count, 0.05 * w * h, "the hair covers part of the image")
        self.assertGreater(sum(lit), 0.01, "lit, not black: %s" % lit)
        self.assertGreater(lit[0], lit[2] * 1.3, "brown hair: more red than blue (%s)" % lit)
        # Where the strands' points project (the camera's bounds of the .vrscene's own points).
        with open(os.path.join(OUT, "brown_groom.vrscene")) as f:
            text = f.read()
        points = unpack_floats(re.search(r"\n  hair_vertices=([^;]*);", text).group(1))
        x0, y0, x1, y1 = project(points, w, h)
        touched = [[p[3] > 0 for p in row] for row in px]  # the thinnest tips too
        xs = [x for row in touched for x, c in enumerate(row) if c]
        ys = [y for y, row in enumerate(touched) if any(row)]
        for got, want in ((min(xs), x0), (max(xs), x1), (min(ys), y0), (max(ys), y1)):
            self.assertLess(abs(got - want), 4.0, "the hair's bounds in the image: %s against the points' %s" %
                            ((min(xs), min(ys), max(xs), max(ys)), (x0, y0, x1, y1)))

    def test_each_strands_color_reaches_the_shader(self):
        result, png = render(self.blue, "blue", extra=["--hide-scalp"])
        self.check(result, png)
        _, _, px = pixels(png)
        lit, _ = mean_linear(px, [[p[3] > 128 for p in row] for row in px])
        self.assertGreater(lit[2], lit[0] * 1.5, "an RGB look of blue renders blue (%s)" % lit)

    def test_motion_blur_from_keys(self):
        still, png_still = render(self.moving, "still", lights="key", extra=["--hide-scalp"])
        self.check(still, png_still)
        blurred, png_blurred = render(self.moving, "blurred", lights="key", extra=["--hide-scalp", "--motion-blur"])
        self.check(blurred, png_blurred)
        with open(os.path.join(OUT, "blurred_groom.vrscene")) as f:
            text = f.read()
        self.assertRegex(text, r"hair_vertices=interpolate\(\s*\(1, ", "two frames: the points as keys")
        self.assertRegex(text, r"\n  widths=List", "the widths written once")
        _, _, a = pixels(png_still)
        _, _, b = pixels(png_blurred)
        # (V-Ray's sampling is deterministic: the still groom renders the same pixels with motion blur on.)
        covered = [(p[3], q[3]) for prow, qrow in zip(a, b) for p, q in zip(prow, qrow) if p[3] > 8 or q[3] > 8]
        moved = sum(1 for p, q in covered if abs(p - q) > 32)
        self.assertGreater(moved, 0.1 * len(covered), "motion blur smears the moving hair (%d of %d pixels)" %
                           (moved, len(covered)))

    def test_close_to_arnold(self):
        if not (KICK and PROCEDURAL):
            self.skipTest("kick and GroomLab's Arnold procedural (--kick, --procedural)")
        size = "320x180"
        result, vpng = render(self.brown, "brown_key", size=size, lights="key", extra=["--hide-scalp"], samples=8)
        self.check(result, vpng)
        apng = os.path.join(OUT, "brown_key_arnold.png")
        code = subprocess.call([sys.executable, os.path.join(APP, "arnold_compare.py"), "--groom", self.brown, "--scalp",
                                SCALP, "--camera", CAMERA, "--out", apng, "--size", size, "--lights", "key",
                                "--linear-srgb", "--kick", KICK, "--procedural", PROCEDURAL, "--aa", "4", "--hide-scalp"],
                               timeout=600)
        self.assertEqual(code, 0)
        w, h, v = pixels(vpng)
        _, _, a = pixels(apng)
        mask = [[p[3] > 200 for p in row] for row in v]
        vm, _ = mean_linear(v, mask)
        am, _ = mean_linear(a, mask)
        write_png(os.path.join(OUT, "vray_vs_arnold.png"), 2 * w, h,
                  [bytes(c for p in vr for c in p[:3]) + bytes(c for p in ar for c in p[:3]) for vr, ar in zip(v, a)])
        for k in range(3):
            self.assertLess(abs(math.log(vm[k] / am[k])), math.log(1.25),
                            "V-Ray's hair within 25%% of Arnold's, channel by channel: %s against %s" % (vm, am))


if __name__ == "__main__":
    args = sys.argv[1:]
    for flag in ("--kick", "--procedural"):
        if flag in args:
            i = args.index(flag)
            if flag == "--kick":
                KICK = args[i + 1]
            else:
                PROCEDURAL = args[i + 1]
            del args[i:i + 2]
    VRAY, CLI, OUT = args[:3]
    if not os.path.isfile(VRAY):
        print("vray.exe not found (%s): skipped" % VRAY)
        sys.exit(0)
    if os.path.isdir(OUT):
        shutil.rmtree(OUT, ignore_errors=True)
    unittest.main(argv=sys.argv[:1] + args[3:], verbosity=2)
