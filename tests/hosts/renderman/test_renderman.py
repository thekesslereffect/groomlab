# SPDX-License-Identifier: Apache-2.0
"""The RenderMan procedural (hosts/renderman) in prman: ctest renderman.procedural (label renderman).

    python test_renderman.py <prman> <procedural dir> <groomlab-cli> <out dir>

Renders tests/data/compat/v1's groom, given a HairColor (and a Wind, so it moves), through
groomlab_renderman: the strand and point counts against groomlab-cli's evaluation, the hair's
coverage and its primvars as PxrConstant renders them (the RGB and PxrHairColor's melanin, against
renderman_look.h's mapping), deformation blur keys, and the shipped example scene
(hosts/renderman/examples/groom.rib) rendering hair. The older Procedural "DynamicLoad" call too,
the one RenderMan for Maya's RenderManProcedural node makes (a data string of -key value pairs):
counts, primvars, motion keys, paths with spaces, unknown keys, and the data string as the Maya
helper (hosts/maya/scripts/groomlab/renderman.py, loaded without Maya) writes it. Small images and
low density: seconds. Standard library only.
"""

import json
import math
import os
import re
import shutil
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
COMPAT = os.path.join(ROOT, "tests", "data", "compat", "v1")
EXAMPLE = os.path.join(ROOT, "hosts", "renderman", "examples", "groom.rib")
PRMAN = PROCEDURALS = CLI = OUT = ""

# The test groom's HairColor: one pigment, no randomness.
MELANIN, REDNESS, DYE = 0.5, 0.3, (1.0, 0.85, 0.7)


def rib_path(path):
    return os.path.abspath(path).replace("\\", "/")


def melanin_color(melanin, redness, dye):
    """HairColor's RGB for a pigment: hair_color.h melaninColor (the color whose Chiang absorption,
    at aiStandardHair's azimuthal roughness 0.2, is 6.6 x melanin^2 x the pigments' spectra) times
    the dye."""
    eu, pheo = (0.419, 0.697, 1.37), (0.187, 0.4, 1.05)
    b = 0.2
    divisor = 5.969 - 0.215 * b + 2.532 * b ** 2 - 10.73 * b ** 3 + 5.574 * b ** 4 + 0.245 * b ** 5
    sigma = [6.6 * melanin * melanin * ((1 - redness) * e + redness * p) for e, p in zip(eu, pheo)]
    return [d * math.exp(-divisor * math.sqrt(s)) for d, s in zip(dye, sigma)]


def pxr_melanin(melanin, redness):
    """renderman_look.h pxrHairColorPigment's melanin (dry)."""
    return min(max((0.68 + 0.12 * redness) * melanin, 0.0), 1.0)


def srgb_to_linear(v):
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def read_tga(path):
    """A Targa's pixels (uncompressed or RLE, 24 or 32 bits): (width, height, rows of RGBA 0-255, top down)."""
    with open(path, "rb") as f:
        data = f.read()
    id_len, kind, bits, desc = data[0], data[2], data[16], data[17]
    width, height = data[12] | data[13] << 8, data[14] | data[15] << 8
    if kind not in (2, 10) or bits not in (24, 32):
        raise ValueError("unsupported Targa: type %d, %d bits" % (kind, bits))
    size, at, pixels = bits // 8, 18 + id_len, []
    while len(pixels) < width * height:
        if kind == 2:
            pixels.append(data[at:at + size])
            at += size
            continue
        head = data[at]
        at += 1
        count = (head & 0x7F) + 1
        if head & 0x80:
            pixels.extend([data[at:at + size]] * count)
            at += size
        else:
            pixels.extend(data[at + k * size:at + (k + 1) * size] for k in range(count))
            at += count * size
    rgba = [(p[2], p[1], p[0], p[3] if size == 4 else 255) for p in pixels[:width * height]]
    rows = [rgba[y * width:(y + 1) * width] for y in range(height)]
    if not desc & 0x20:  # stored bottom up
        rows.reverse()
    return width, height, rows


def make_groom(path, wind=False, search_paths=False):
    """The compat groom with a HairColor (one pigment, a dye) before its output, and a Wind after
    it; with `search_paths`, the compat folder its search path (its density map is relative to it)."""
    with open(os.path.join(COMPAT, "groom.groom")) as f:
        groom = json.load(f)
    if search_paths:
        groom["searchPaths"] = [rib_path(COMPAT)]
    graph = groom["graph"]
    graph["nodes"].insert(-1, {"name": "color", "type": "HairColor", "params": {
        "melanin": MELANIN, "redness": REDNESS, "dye": list(DYE), "melaninRandom": 0.0}})
    chain = ["width", "color"]
    if wind:
        graph["nodes"].insert(-1, {"name": "wind", "type": "Wind", "params": {"strength": 0.6, "speed": 40.0}})
        chain.append("wind")
    chain.append("output")
    graph["connections"] = [c for c in graph["connections"] if c["to"] != "output"]
    graph["connections"] += [{"from": a, "to": b, "input": "strands"} for a, b in zip(chain, chain[1:])]
    with open(path, "w") as f:
        json.dump(groom, f)


def old_style(data, bound=5):
    """The older Procedural "DynamicLoad" call (RenderMan for Maya's RenderManProcedural node): the
    data string's `-key value` pairs (written as a RIB string: quotes inside escaped) and a bound."""
    return 'Procedural "DynamicLoad" ["groomlab_renderman" "%s"] [%s]\n' % (
        data.replace("\\", "\\\\").replace('"', '\\"'), " ".join(str(v) for v in (-bound, bound) * 3))


def scene(display, body, procedural, size=96, samples=16):
    """A RIB: the camera on the compat groom (a head-sized sphere of hair at the origin). The
    procedural: Procedural2's typed parameters, or a whole call (old_style)."""
    if not procedural.startswith("Procedural "):
        procedural = 'Procedural2 "DynamicLoad" "DSOBound" "string dsoname" ["groomlab_renderman"] %s\n' % procedural
    return "".join([
        'Option "searchpath" "string procedural" ["%s:@"]\n' % rib_path(PROCEDURALS),
        'Format %d %d 1\n' % (size, size),
        display,
        'Hider "raytrace" "int maxsamples" [%d] "int minsamples" [%d]\n' % (samples, samples),
        'Integrator "PxrPathTracer" "pt"\n',
        'Projection "PxrPerspective" "float fov" [40]\n',
        'Translate 0 0.3 6.5\n',
        'WorldBegin\n', body, procedural,
        'WorldEnd\n'])


def prman(rib, name):
    path = os.path.join(OUT, name + ".rib")
    with open(path, "w") as f:
        f.write(rib)
    env = dict(os.environ, RMANTREE=os.path.dirname(os.path.dirname(os.path.abspath(PRMAN))))
    result = subprocess.run([PRMAN, path], cwd=OUT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            universal_newlines=True, timeout=300)
    return result


class RenderManProcedural(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.makedirs(OUT, exist_ok=True)
        cls.groom = os.path.join(OUT, "colored.groom")
        make_groom(cls.groom)
        cls.counted = os.path.join(OUT, "counted.groom")
        make_groom(cls.counted, search_paths=True)
        cls.moving = os.path.join(OUT, "moving.groom")
        make_groom(cls.moving, wind=True)
        # Written away from the compat folder: its density map ("../../textures/density.pgm")
        # resolves through the procedural's searchPaths.
        cls.params = '"string filename" ["%s"] "string searchPaths" ["%s"]' % (rib_path(cls.groom), rib_path(COMPAT))

    def check(self, result):
        self.assertEqual(result.returncode, 0, result.stdout[-3000:])
        self.assertNotRegex(result.stdout, r"(?i)\{(ERROR|SEVERE)\}|R1600\d", result.stdout[-3000:])
        return result.stdout

    _stats = None

    def cli_stats(self):
        """groomlab-cli's evaluation of the counted groom at density 0.5: its strand and point counts.
        (It reads the groom with the compat folder as its search path: the CLI takes no other.)"""
        if RenderManProcedural._stats is None:
            cli = subprocess.run([CLI, "eval", self.counted, "-o", os.path.join(OUT, "counts.abc"), "--density",
                                  "0.5", "--stats"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 universal_newlines=True, timeout=120)
            self.assertEqual(cli.returncode, 0, cli.stdout[-2000:])
            RenderManProcedural._stats = json.loads(re.search(r"^stats: (\{.*\})$", cli.stdout, re.M).group(1))
        return RenderManProcedural._stats

    def check_counts(self, out, stats):
        """The debug output's counts are the CLI's, and every primvar has a value per strand
        (uniform), per point (vertex) or one (constant)."""
        got = re.search(r"\[groomlab\] \S+: (\d+) strands, (\d+) points, 1 motion key", out)
        self.assertIsNotNone(got, out[-3000:])
        self.assertEqual((int(got.group(1)), int(got.group(2))), (stats["strands"], stats["points"]))
        primvars = dict((m.group(2), (m.group(1), int(m.group(3)))) for m in
                        re.finditer(r"primvar (\w+) [\w\[\]0-9]+ (\w+): (\d+) values", out))
        for name in ("strand_id", "hair_index", "st", "rman_melanin", "rman_dye", "rman_color", "rman_rgbShare"):
            self.assertIn(name, primvars, out[-3000:])
        for name, (detail, count) in primvars.items():
            want = {"uniform": stats["strands"], "vertex": stats["points"], "constant": 1}[detail]
            self.assertEqual(count, want, name)

    def test_counts_match_the_core(self):
        # groomlab-cli's evaluation at the same density: the same strands and points.
        stats = self.cli_stats()
        out = self.check(prman(scene('Display "counts" "null" "rgba"\n', 'Bxdf "PxrDiffuse" "d"\n',
                                     '"string filename" ["%s"] "float density" [0.5] "int debug" [1]' %
                                     rib_path(self.counted), size=16, samples=1), "counts"))
        got = re.search(r"\[groomlab\] \S+: (\d+) strands, (\d+) points, 1 motion key", out)
        self.assertIsNotNone(got, out[-3000:])
        self.assertEqual((int(got.group(1)), int(got.group(2))), (stats["strands"], stats["points"]))
        self.assertGreater(stats["strands"], 500)
        # Every primvar with a value per strand (uniform), per point (vertex) or one (constant).
        primvars = dict((m.group(2), (m.group(1), int(m.group(3)))) for m in
                        re.finditer(r"primvar (\w+) [\w\[\]0-9]+ (\w+): (\d+) values", out))
        for name in ("strand_id", "hair_index", "st", "scalp_st", "melanin", "melaninRedness", "clumpId", "colorR",
                     "hair_color", "rman_melanin", "rman_redness", "rman_dye", "rman_color", "rman_rgbShare",
                     "rman_specularGainR", "rman_coneAngleR", "rman_coneAngleTT", "rman_coneAngleTRT"):
            self.assertIn(name, primvars, out[-3000:])
        for name, (detail, count) in primvars.items():
            want = {"uniform": stats["strands"], "vertex": stats["points"], "constant": 1}[detail]
            self.assertEqual(count, want, name)

    def render_primvar(self, name, emit):
        """The hair's color in a render whose PxrConstant emits `emit`: mean RGB of covered pixels
        (unpremultiplied), and the share of the image covered."""
        body = emit + 'Bxdf "PxrConstant" "c" "reference color emitColor" ["value:resultRGB"]\n'
        out = self.check(prman(scene('Display "%s.tga" "targa" "rgba"\n' % name, body, self.params), name))
        width, height, rows = read_tga(os.path.join(OUT, name + ".tga"))
        covered = [p for row in rows for p in row if p[3] >= 250]
        anywhere = sum(1 for row in rows for p in row if p[3] > 0)
        self.assertGreater(len(covered), 50, "no hair in %s.tga" % name)
        # Coverage around the middle, where the groom is (the sphere and its hair), little in the corners.
        middle = sum(1 for y in range(height // 4, 3 * height // 4) for x in range(width // 4, 3 * width // 4)
                     if rows[y][x][3] > 0)
        self.assertGreater(middle, (width * height // 4) * 0.5, name)
        corners = sum(1 for y in (0, height - 1) for x in (0, width - 1) if rows[y][x][3] > 0)
        self.assertLessEqual(corners, 1, name)
        # (prman's 8-bit drivers write sRGB: back to linear.)
        mean = [srgb_to_linear(sum(p[k] for p in covered) / (255.0 * len(covered))) for k in range(3)]
        return mean, anywhere / float(width * height), out

    def test_hair_color_arrives(self):
        mean, coverage, _ = self.render_primvar(
            "hair_color", 'Pattern "PxrPrimvar" "value" "string varname" ["hair_color"] "string type" ["color"]\n')
        want = melanin_color(MELANIN, REDNESS, DYE)
        for k in range(3):
            self.assertAlmostEqual(mean[k], want[k], delta=0.02, msg="hair_color %s, want %s" % (mean, want))
        self.assertGreater(coverage, 0.1)

    def test_pigment_primvars_arrive(self):
        # rman_melanin as gray (a PxrMix of black and white by it), and the dye as constant color.
        mean, _, _ = self.render_primvar(
            "rman_melanin", 'Pattern "PxrPrimvar" "pv" "string varname" ["rman_melanin"] "string type" ["float"]\n'
                            'Pattern "PxrMix" "value" "color color1" [0 0 0] "color color2" [1 1 1]'
                            ' "reference float mixer" ["pv:resultF"]\n')
        want = pxr_melanin(MELANIN, REDNESS)
        for k in range(3):
            self.assertAlmostEqual(mean[k], want, delta=0.02, msg="rman_melanin %s, want %s" % (mean, want))
        mean, _, _ = self.render_primvar(
            "rman_dye", 'Pattern "PxrPrimvar" "value" "string varname" ["rman_dye"] "string type" ["color"]\n')
        for k in range(3):
            self.assertAlmostEqual(mean[k], DYE[k], delta=0.02, msg="rman_dye %s" % mean)

    def test_motion_keys(self):
        # A groom the wind moves: three keys over the shutter, in one MotionBegin block.
        params = '"string filename" ["%s"] "string searchPaths" ["%s"]' % (rib_path(self.moving), rib_path(COMPAT))
        out = self.check(prman(scene('Shutter 0 0.5\nDisplay "moving.tga" "targa" "rgba"\n', 'Bxdf "PxrDiffuse" "d"\n'
                                     'Light "PxrDomeLight" "dome"\n',
                                     params + ' "float density" [0.3] "float motionStart" [0] "float motionEnd" [0.5]'
                                     ' "int motionKeys" [3] "int debug" [1]'), "moving"))
        self.assertRegex(out, r"\[groomlab\] \S+: \d+ strands, \d+ points, 3 motion key")
        # A still groom keeps one key whatever it asks for.
        out = self.check(prman(scene('Shutter 0 0.5\nDisplay "still" "null" "rgba"\n', 'Bxdf "PxrDiffuse" "d"\n',
                                     self.params + ' "float density" [0.3] "float motionEnd" [0.5] "int motionKeys" [3]'
                                     ' "int debug" [1]', size=16, samples=1), "still"))
        self.assertRegex(out, r"\[groomlab\] \S+: \d+ strands, \d+ points, 1 motion key")

    def test_overrides(self):
        # widthScale scales the widths, lengthScale the strands about their roots: the bound shrinks.
        out = self.check(prman(scene('Display "short" "null" "rgba"\n', 'Bxdf "PxrDiffuse" "d"\n',
                                     self.params + ' "float density" [0.2] "float lengthScale" [0.5] "int debug" [1]',
                                     size=16, samples=1), "short"))
        self.assertRegex(out, r"\[groomlab\] \S+: \d+ strands")
        # No file: an error, prman goes on.
        result = prman(scene('Display "missing" "null" "rgba"\n', 'Bxdf "PxrDiffuse" "d"\n',
                             '"string filename" ["%s"]' % rib_path(os.path.join(OUT, "missing.groom")), size=16,
                             samples=1), "missing")
        self.assertIn("[groomlab]", result.stdout)
        self.assertIn("missing.groom", result.stdout)

    # The older Procedural "DynamicLoad" API (ConvertParameters, Subdivide, Free): the call RenderMan
    # for Maya's RenderManProcedural node makes, its data string the parameters.

    def test_data_string_counts_match_the_core(self):
        stats = self.cli_stats()
        out = self.check(prman(scene('Display "old_counts" "null" "rgba"\n', 'Bxdf "PxrDiffuse" "d"\n',
                                     old_style("-file %s -frame 1 -density 0.5 -debug" % rib_path(self.counted)),
                                     size=16, samples=1), "old_counts"))
        self.check_counts(out, stats)
        self.assertRegex(out, r"\[groomlab\] \S+: \d+ strands, \d+ points, 1 motion key\(s\) at frame 1 ")
        # A Procedural2 call with a "data" string (and RenderMan for Maya's typed frame) reads it the same.
        out = self.check(prman(scene('Display "data_counts" "null" "rgba"\n', 'Bxdf "PxrDiffuse" "d"\n',
                                     '"string data" ["-file %s -density 0.5 -debug 1"] "float frame" [3]' %
                                     rib_path(self.counted), size=16, samples=1), "data_counts"))
        self.check_counts(out, stats)
        self.assertRegex(out, r"motion key\(s\) at frame 3 ")

    def test_data_string_primvars_arrive(self):
        # hair_color through the old API, the groom's texture folder in -searchPaths.
        emit = 'Pattern "PxrPrimvar" "value" "string varname" ["hair_color"] "string type" ["color"]\n'
        body = emit + 'Bxdf "PxrConstant" "c" "reference color emitColor" ["value:resultRGB"]\n'
        out = self.check(prman(scene('Display "old_color.tga" "targa" "rgba"\n', body, old_style(
            "-file %s -frame 1 -searchPaths %s" % (rib_path(self.groom), rib_path(COMPAT)))), "old_color"))
        self.assertNotIn("unknown key", out)
        _, _, rows = read_tga(os.path.join(OUT, "old_color.tga"))
        covered = [p for row in rows for p in row if p[3] >= 250]
        self.assertGreater(len(covered), 50, "no hair in old_color.tga")
        mean = [srgb_to_linear(sum(p[k] for p in covered) / (255.0 * len(covered))) for k in range(3)]
        want = melanin_color(MELANIN, REDNESS, DYE)
        for k in range(3):
            self.assertAlmostEqual(mean[k], want[k], delta=0.02, msg="hair_color %s, want %s" % (mean, want))

    def test_data_string_motion_keys(self):
        out = self.check(prman(scene('Shutter 0 0.5\nDisplay "old_moving" "null" "rgba"\n', 'Bxdf "PxrDiffuse" "d"\n',
                                     old_style("-file %s -searchPaths %s -frame 12 -density 0.3 -motionStart 0 "
                                               "-motionEnd 0.5 -motionKeys 3 -debug" %
                                               (rib_path(self.moving), rib_path(COMPAT))), size=16, samples=1),
                               "old_moving"))
        self.assertRegex(out, r"\[groomlab\] \S+: \d+ strands, \d+ points, 3 motion key\(s\) at frame 12 ")
        # A shutter centred on the frame: negative values.
        out = self.check(prman(scene('Shutter -0.25 0.25\nDisplay "old_centred" "null" "rgba"\n',
                                     'Bxdf "PxrDiffuse" "d"\n',
                                     old_style("-file %s -searchPaths %s -frame 12 -density 0.3 -motionStart -0.25 "
                                               "-motionEnd 0.25 -motionKeys 2 -debug" %
                                               (rib_path(self.moving), rib_path(COMPAT))), size=16, samples=1),
                               "old_centred"))
        self.assertRegex(out, r"\[groomlab\] \S+: \d+ strands, \d+ points, 2 motion key\(s\)")
        self.assertNotIn("is not a number", out)

    def test_data_string_path_with_spaces(self):
        folder = os.path.join(OUT, "a folder with spaces")
        os.makedirs(folder, exist_ok=True)
        groom = os.path.join(folder, "colored groom.groom")
        shutil.copyfile(self.groom, groom)
        for quote in ('"', "'"):
            data = "-file {q}{f}{q} -searchPaths {q}{c}{q} -density 0.3 -debug".format(
                q=quote, f=rib_path(groom), c=rib_path(COMPAT))
            name = "spaces_%s" % ("double" if quote == '"' else "single")
            out = self.check(prman(scene('Display "%s" "null" "rgba"\n' % name, 'Bxdf "PxrDiffuse" "d"\n',
                                         old_style(data), size=16, samples=1), name))
            self.assertRegex(out, r"\[groomlab\] .*colored groom\.groom: [1-9]\d* strands", out[-3000:])

    def test_data_string_unknown_key_warns(self):
        out = self.check(prman(scene('Display "unknown" "null" "rgba"\n', 'Bxdf "PxrDiffuse" "d"\n',
                                     old_style("-file %s -searchPaths %s -bogus 3 -frame 2 -density 0.3 -alsoBogus "
                                               "-debug" % (rib_path(self.groom), rib_path(COMPAT))),
                                     size=16, samples=1), "unknown"))
        self.assertIn("unknown key -bogus", out)
        self.assertIn("unknown key -alsoBogus", out)
        # The keys after them still read: frame 2, strands rendered.
        self.assertRegex(out, r"\[groomlab\] \S+: [1-9]\d* strands, \d+ points, 1 motion key\(s\) at frame 2 ")
        # No file: an error, prman goes on.
        result = prman(scene('Display "old_missing" "null" "rgba"\n', 'Bxdf "PxrDiffuse" "d"\n',
                             old_style("-frame 1"), size=16, samples=1), "old_missing")
        self.assertEqual(result.returncode, 0, result.stdout[-3000:])
        self.assertIn("no filename", result.stdout)

    def test_data_string_from_the_maya_helper(self):
        # The data string as groomlab.renderman (RenderMan for Maya's pre-shape script) writes it,
        # loaded without Maya: a path with spaces and quotes, a centred shutter, the procedural reads it all.
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "groomlab_renderman_data", os.path.join(ROOT, "hosts", "maya", "scripts", "groomlab", "renderman.py"))
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        folder = os.path.join(OUT, "it's a folder")
        os.makedirs(folder, exist_ok=True)
        groom = os.path.join(folder, "moving groom.groom")
        shutil.copyfile(self.moving, groom)
        data = helper.update_data("-foo bar -density 0.3", file=groom, frame=7.5, search_paths=[COMPAT, OUT],
                                  motion_start=-0.25, motion_end=0.25, motion_keys=2, name="hair", debug=True,
                                  revision=12)
        self.assertIn("-revision 12", data)
        out = self.check(prman(scene('Shutter -0.25 0.25\nDisplay "helper" "null" "rgba"\n', 'Bxdf "PxrDiffuse" "d"\n',
                                     old_style(data), size=16, samples=1), "helper"))
        self.assertRegex(out, r"\[groomlab\] .*moving groom\.groom: [1-9]\d* strands, \d+ points, 2 motion key\(s\) "
                              r"at frame 7\.5 ")
        self.assertIn("unknown key -foo", out)
        self.assertNotIn("unknown key -revision", out)  # the host's change counter: read and ignored
        self.assertNotIn("not a number", out)

    def test_example_scene_renders_hair(self):
        # The shipped example, on the test groom: PxrMarschnerHair and PxrHairColor from the primvars.
        with open(EXAMPLE) as f:
            rib = f.read()
        rib = rib.replace('["hair.groom"]', '["%s"] "string searchPaths" ["%s"] "int debug" [1]' %
                          (rib_path(self.groom), rib_path(COMPAT)))
        rib = rib.replace('[".:@"]', '["%s:@"]' % rib_path(PROCEDURALS))
        rib = rib.replace('Format 960 720 1', 'Format 120 90 1').replace('"int maxsamples" [64] "int minsamples" [16]',
                                                                        '"int maxsamples" [16] "int minsamples" [16]')
        rib = rib.replace('Display "groom.exr" "openexr"', 'Display "example.tga" "targa"')
        # (The compat groom is a sphere of radius 1: the camera ten times closer.)
        rib = re.sub(r"Transform \[(.*)\]", lambda m: "Transform [%s]" % " ".join(
            "%.6f" % (float(v) / 10.0 if i in (12, 13, 14) else float(v)) for i, v in enumerate(m.group(1).split())), rib)
        out = self.check(prman(rib, "example"))
        self.assertRegex(out, r"\[groomlab\] \S+: \d+ strands")
        _, _, rows = read_tga(os.path.join(OUT, "example.tga"))
        lit = [p for row in rows for p in row if p[3] >= 250 and max(p[:3]) > 8]
        self.assertGreater(len(lit), 100, "the example rendered no lit hair")
        # Brown: more red than blue.
        self.assertGreater(sum(p[0] for p in lit), sum(p[2] for p in lit))


if __name__ == "__main__":
    PRMAN, PROCEDURALS, CLI, OUT = sys.argv[1:5]
    if os.path.isdir(OUT):
        shutil.rmtree(OUT, ignore_errors=True)
    unittest.main(argv=sys.argv[:1] + sys.argv[5:], verbosity=2)
