# SPDX-License-Identifier: Apache-2.0
"""Card fidelity: how close a groom's game cards look to its strands in Maya's viewport.

Draws test grooms' strands and their cards (hero, mid, low) in Viewport 2.0 from five cameras
(front, three-quarter, side, back, top: fidelity_maya.py in a Maya window of its own, DirectX 11
by default, as artists run it), then measures each view's pair of pictures:

    area         silhouette size: the cards' covered pixels over the strands' (each covered over 0.5);
                 1 is the same size, and every level of detail should stay within 5% of it
    outside      ballooning: card pixels outside the strands' filled silhouette (their envelope),
                 as a share of the strands' covered pixels
    iou          silhouette: strand coverage vs card coverage (each over 0.5), intersection / union
    coverage     mean |strand coverage - card coverage| over the pixels either covers
    seeThrough   share of the strands' covered pixels the cards leave uncovered (skin, background)
    excess       card-covered pixels the strands don't cover, as a share of the strands' covered ones
    dE           mean CIE76 color difference where both cover; dE_roots, dE_mid, dE_tips by where
                 along the strands the pixel lies (the strands' own points projected: thirds of
                 their length); dL the cards' mean lightness minus the strands' (+: lighter);
                 dE_lo the same of both pictures blurred over their covered pixels (4 px): color
                 and shading at a few strands' scale, the strand-by-strand pattern left out
    fine         strand-scale detail: the cards' mean |L - blur(L, 1 px)| over the strands' (1 is equal)
    clump        clump-scale detail: the same of the band blur(L, 2) - blur(L, 7)

and writes metrics.json and a picture per groom (grid_<groom>.png: a row per view; the strands,
then each level of detail, over gray).

    python tools/bench/card_fidelity/run_fidelity.py run --out <folder> [--build build/win-maya2022]
        [--grooms long,fur,curly] [--lods hero,mid,low] [--opengl] [--size 720] [--no-cap]
    python tools/bench/card_fidelity/run_fidelity.py metrics <folder>
    python tools/bench/card_fidelity/run_fidelity.py compare <before folder> <after folder> [--md out.md]

Needs Python 3 with numpy and Pillow (not mayapy: Maya 2022's has no numpy), Maya 2022 and a
build (the plugin, the Python module). The Maya session uses a prefs folder of its own beside the
GUI tests' (build/.../tests/card_fidelity_app_dir, made before Maya starts, the plugin folder
trusted in it as maya.gui_trust does): no dialog ever opens, nothing it sets (the backgrounds)
reaches the GUI tests' prefs, and nothing changes Maya's SafeMode settings at runtime.
"""

import argparse
import json
import os
import subprocess
import sys
import time

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
MAYA = os.environ.get("MAYA_LOCATION", r"C:\Program Files\Autodesk\Maya2022")


# -- Running Maya -------------------------------------------------------------------------------

def run_maya(out, build, grooms, lods, opengl, size, cap=True):
    build = os.path.abspath(build)
    plugin = os.path.join(build, "hosts", "maya", "RelWithDebInfo", "GroomLab.mll")
    module = os.path.join(build, "engine", "python", "RelWithDebInfo")
    # A prefs folder of its own: Maya saves its prefs as it quits, and the backgrounds set here must
    # never reach the GUI tests' (build/.../tests/maya_app_dir, whose suites read playblasts).
    app_dir = os.path.join(build, "tests", "card_fidelity_app_dir")
    for path in (plugin, module):
        if not os.path.exists(path):
            sys.exit("not built: %s" % path)
    # Maya asks (a modal dialog) when MAYA_APP_DIR is not a folder: made here, its prefs file
    # (the plugin folder trusted) written before Maya starts.
    os.makedirs(app_dir, exist_ok=True)
    subprocess.check_call(["cmake", "-DAPP_DIR=" + app_dir.replace("\\", "/"),
                           "-DDIR=" + os.path.dirname(plugin).replace("\\", "/"),
                           "-P", os.path.join(REPO, "tests", "hosts", "maya", "trust_plugin_folder.cmake")])
    if not os.path.isfile(os.path.join(app_dir, "2022", "prefs", "userPrefs.mel")):
        sys.exit("no prefs file under %s: not starting Maya" % app_dir)
    os.makedirs(out, exist_ok=True)
    mel = os.path.join(out, "fidelity.mel")
    with open(mel, "w") as f:
        f.write("python(\"exec(open(r'%s').read())\");\n" % os.path.join(HERE, "fidelity_maya.py").replace("\\", "/"))
    env = dict(os.environ)
    env.update({"MAYA_APP_DIR": app_dir, "PYTHONPATH": module, "GROOMLAB_PLUGIN": plugin,
                "GROOMLAB_SCRIPTS": os.path.join(REPO, "hosts", "maya", "scripts"),
                "FIDELITY_OUT": os.path.abspath(out), "FIDELITY_GROOMS": ",".join(grooms),
                "FIDELITY_LODS": ",".join(lods), "FIDELITY_SIZE": str(size), "FIDELITY_CAP": "1" if cap else "0",
                "FIDELITY_SCALP": os.path.join(REPO, "tests", "data", "app", "head_scalp.obj").replace("\\", "/")})
    if not opengl:
        env["MAYA_VP2_DEVICE_OVERRIDE"] = "VirtualDeviceDx11"
    started = time.time()
    with open(os.path.join(out, "maya.log"), "w") as log:
        code = subprocess.call([os.path.join(MAYA, "bin", "maya.exe"), "-script", mel], env=env, stdout=log,
                               stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, timeout=3600)
    meta = json.load(open(os.path.join(out, "meta.json")))
    if code != 0 or "error" in meta:
        sys.exit("Maya failed (%s): %s" % (code, meta.get("error")))
    print("Maya: %.0f s (%s)" % (time.time() - started, meta.get("drawAPI")))


# -- Pictures -----------------------------------------------------------------------------------

def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def coverage_and_color(folder, view, what):
    """Coverage (0..1) and color (display values) of a pass from its black and white pictures:
    hair draws the same over both, the background (and the holdout head) takes each one's."""
    k = load(os.path.join(folder, "%s_%s_k.png" % (view, what)))
    w = load(os.path.join(folder, "%s_%s_w.png" % (view, what)))
    # The backgrounds as drawn (a view transform may not show white as 1): the corners'.
    corners = [(0, 0), (0, -1), (-1, 0), (-1, -1)]
    span = np.mean([w[y, x] - k[y, x] for y, x in corners], axis=0)
    span = np.where(span > 0.05, span, 1.0)
    alpha = np.clip(1.0 - np.mean((w - k) / span, axis=2), 0.0, 1.0)
    base = np.mean([k[y, x] for y, x in corners], axis=0)
    color = np.clip((k - base * (1.0 - alpha)[..., None]) / np.maximum(alpha, 1e-3)[..., None], 0.0, 1.0)
    return alpha, color


def to_lab(rgb):
    lin = np.where(rgb <= 0.04045, rgb / 12.92, ((rgb + 0.055) / 1.055) ** 2.4)
    m = np.array([[0.4124, 0.3576, 0.1805], [0.2126, 0.7152, 0.0722], [0.0193, 0.1192, 0.9505]], dtype=np.float32)
    xyz = lin @ m.T / np.array([0.95047, 1.0, 1.08883], dtype=np.float32)
    f = np.where(xyz > 0.008856, np.cbrt(xyz), 7.787 * xyz + 16.0 / 116.0)
    return np.stack([116.0 * f[..., 1] - 16.0, 500.0 * (f[..., 0] - f[..., 1]), 200.0 * (f[..., 1] - f[..., 2])], axis=-1)


def blur(image, radius):
    """Three box blurs of `radius` pixels each way: close to a Gaussian (sigma about radius)."""
    out = image.astype(np.float32)
    r = max(int(round(radius)), 1)
    for _ in range(3):
        for axis in (0, 1):
            pad = [(0, 0), (0, 0)]
            pad[axis] = (r + 1, r)
            c = np.cumsum(np.pad(out, pad, mode="edge"), axis=axis)
            out = (np.take(c, range(2 * r + 1, c.shape[axis]), axis=axis) -
                   np.take(c, range(0, c.shape[axis] - 2 * r - 1), axis=axis)) / (2 * r + 1)
    return out


def erode(mask, steps):
    m = mask.copy()
    for _ in range(steps):
        n = m.copy()
        n[1:, :] &= m[:-1, :]
        n[:-1, :] &= m[1:, :]
        n[:, 1:] &= m[:, :-1]
        n[:, :-1] &= m[:, 1:]
        m = n
    return m


def along_map(folder, meta, groom, view, size):
    """Where along the strands each pixel lies (0 root .. 1 tip; NaN: no strand near): the
    viewport's strand points (every strand SEGMENTS + 1 points) projected, the nearest kept."""
    g = meta["grooms"][groom]
    per = g["segments"] + 1
    pts = np.fromfile(os.path.join(folder, "points.bin"), dtype=np.float32).reshape(-1, 3)
    if len(pts) % per:
        return None
    strands = pts.reshape(-1, per, 3)
    if len(strands) > 40000:  # a share of them is plenty for the regions
        strands = strands[np.linspace(0, len(strands) - 1, 40000).astype(int)]
    sub = 6  # points along each segment
    t = np.linspace(0.0, 1.0, sub, endpoint=False, dtype=np.float32)
    a, b = strands[:, :-1, None, :], strands[:, 1:, None, :]
    dense = (a + (b - a) * t[None, None, :, None]).reshape(len(strands), -1, 3)
    dense = np.concatenate([dense, strands[:, -1:, :]], axis=1)
    u = np.linspace(0.0, 1.0, dense.shape[1], dtype=np.float32)[None, :].repeat(len(strands), axis=0)
    cam = g["views"][view]
    eye, target, up = (np.array(cam[k], dtype=np.float32) for k in ("eye", "target", "up"))
    forward = target - eye
    forward /= np.linalg.norm(forward)
    right = np.cross(forward, up)
    right /= np.linalg.norm(right)
    true_up = np.cross(right, forward)
    d = dense.reshape(-1, 3) - eye
    z = d @ forward
    x = (d @ right) / (z * cam["tanHalf"])
    y = (d @ true_up) / (z * cam["tanHalf"])
    px = np.floor((x * 0.5 + 0.5) * size).astype(np.int64)
    py = np.floor((0.5 - y * 0.5) * size).astype(np.int64)
    ok = (px >= 0) & (px < size) & (py >= 0) & (py < size) & (z > 0)
    pixel = (py * size + px)[ok]
    depth, uu = z[ok], u.reshape(-1)[ok]
    order = np.lexsort((depth, pixel))  # by pixel, nearest first
    pixel, uu = pixel[order], uu[order]
    first = np.unique(pixel, return_index=True)[1]
    out = np.full(size * size, np.nan, dtype=np.float32)
    out[pixel[first]] = uu[first]
    out = out.reshape(size, size)
    for _ in range(12):  # into the gaps between the projected points
        empty = np.isnan(out)
        if not empty.any():
            break
        fill = out.copy()
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            moved = np.roll(np.roll(out, dy, axis=0), dx, axis=1)
            take = np.isnan(fill) & ~np.isnan(moved)
            fill[take] = moved[take]
        out = fill
    return out


def measure_view(folder, meta, groom, view, size):
    alpha_s, color_s = coverage_and_color(folder, view, "strands")
    lab_s = to_lab(color_s)
    along = along_map(folder, meta, groom, view, size)
    covered_s = alpha_s > 0.5
    fine_s, clump_s = detail(lab_s[..., 0], covered_s)
    out = {}
    for lod in meta["lods"]:
        alpha_c, color_c = coverage_and_color(folder, view, lod)
        lab_c = to_lab(color_c)
        covered_c = alpha_c > 0.5
        union = covered_s | covered_c
        either = (alpha_s > 0.02) | (alpha_c > 0.02)
        both = covered_s & covered_c
        de = np.linalg.norm(lab_s - lab_c, axis=-1)
        m = {
            "iou": float((covered_s & covered_c).sum() / max(union.sum(), 1)),
            "area": float(covered_c.sum() / max(covered_s.sum(), 1)),
            "outside": outside_share(covered_s, covered_c),
            "coverage": float(np.abs(alpha_s - alpha_c)[either].mean()) if either.any() else 0.0,
            "seeThrough": float((covered_s & ~covered_c).sum() / max(covered_s.sum(), 1)),
            "excess": float((covered_c & ~covered_s).sum() / max(covered_s.sum(), 1)),
            "dE": float(de[both].mean()) if both.any() else None,
            "dL": float((lab_c[..., 0][both] - lab_s[..., 0][both]).mean()) if both.any() else None,
            "dE_lo": float(np.linalg.norm(smooth_lab(lab_s, alpha_s) - smooth_lab(lab_c, alpha_c), axis=-1)[both].mean())
            if both.any() else None,
        }
        if along is not None:
            for name, lo, hi in (("roots", 0.0, 1 / 3.0), ("mid", 1 / 3.0, 2 / 3.0), ("tips", 2 / 3.0, 1.01)):
                region = both & (along >= lo) & (along < hi)
                m["dE_" + name] = float(de[region].mean()) if region.sum() > 50 else None
        fine_c, clump_c = detail(lab_c[..., 0], covered_c)
        m["fine"] = fine_c / fine_s if fine_s > 0 else None
        m["clump"] = clump_c / clump_s if clump_s > 0 else None
        out[lod] = m
    return out


def smooth_lab(lab, alpha, radius=4):
    """Lab blurred over the covered pixels only (weighted by coverage): the color and shading at a
    few strands' scale, without the strand-by-strand pattern."""
    weight = blur(alpha, radius)[..., None]
    return np.stack([blur(lab[..., k] * alpha, radius) for k in range(3)], axis=-1) / np.maximum(weight, 1e-4)


def dilate(mask, steps):
    return ~erode(~mask, steps)


def envelope(covered):
    """The strands' silhouette filled: every pixel the background can't reach from the picture's
    edge without crossing them (thickened by 2 pixels, so gaps between strands don't count)."""
    wall = dilate(covered, 2)
    picture = Image.fromarray(np.where(wall, 255, 0).astype(np.uint8)).copy()  # its own pixels, to fill
    h, w = wall.shape
    for x, y in ((0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)):
        if picture.getpixel((x, y)) == 0:
            ImageDraw.floodfill(picture, (x, y), 128)
    return np.asarray(picture) != 128


def outside_share(covered_s, covered_c):
    """Card pixels outside the strands' filled silhouette, as a share of the strands' covered
    pixels: how far the cards balloon past the groom's envelope."""
    return float((covered_c & ~envelope(covered_s)).sum() / max(covered_s.sum(), 1))


def detail(lightness, covered):
    """Mean strand-scale and clump-scale contrast of the lightness inside the covered pixels
    (their silhouette's edge left out)."""
    inner = erode(covered, 3)
    if inner.sum() < 100:
        return 0.0, 0.0
    l1, l2, l7 = blur(lightness, 1), blur(lightness, 2), blur(lightness, 7)
    return float(np.abs(lightness - l1)[inner].mean()), float(np.abs(l2 - l7)[inner].mean())


def tile(folder, view, what, size):
    alpha, color = coverage_and_color(folder, view, what)
    gray = np.full_like(color, 0.32)
    rgb = color * alpha[..., None] + gray * (1.0 - alpha[..., None])
    return Image.fromarray((np.clip(rgb, 0, 1) * 255).astype(np.uint8)).resize((size, size), Image.LANCZOS)


def grid(folder, meta, groom, out_path, size=320):
    whats = ["strands"] + list(meta["lods"])
    views = meta["views"]
    label = 22
    picture = Image.new("RGB", (size * len(whats), label + size * len(views)), (40, 40, 40))
    draw = ImageDraw.Draw(picture)
    for c, what in enumerate(whats):
        text = what
        if what in meta["grooms"][groom]["lods"]:
            lod = meta["grooms"][groom]["lods"][what]
            text = "%s: %d cards, %d tris" % (what, lod["cards"], lod["triangles"])
        else:
            text = "strands: %d" % meta["grooms"][groom]["strands"]
        draw.text((c * size + 6, 5), text, fill=(230, 230, 230))
        for r, view in enumerate(views):
            picture.paste(tile(folder, view, what, size), (c * size, label + r * size))
    picture.save(out_path)


def metrics(out):
    meta = json.load(open(os.path.join(out, "meta.json")))
    result = {"drawAPI": meta.get("drawAPI"), "grooms": {}}
    for groom, g in meta["grooms"].items():
        folder = os.path.join(out, groom)
        views = {view: measure_view(folder, meta, groom, view, meta["size"]) for view in meta["views"]}
        mean = {}
        for lod in meta["lods"]:
            keys = views[meta["views"][0]][lod].keys()
            mean[lod] = {}
            for key in keys:
                values = [views[v][lod][key] for v in meta["views"] if views[v][lod][key] is not None]
                mean[lod][key] = float(np.mean(values)) if values else None
        result["grooms"][groom] = {"strands": g["strands"], "lods": g["lods"], "views": views, "mean": mean}
        grid(folder, meta, groom, os.path.join(out, "grid_%s.png" % groom))
    with open(os.path.join(out, "metrics.json"), "w") as f:
        json.dump(result, f, indent=1)
    print(table(result))
    return result


KEYS = ("area", "outside", "iou", "seeThrough", "excess", "coverage", "dE", "dE_lo", "dE_roots", "dE_mid", "dE_tips", "dL", "fine", "clump")


def fmt(v):
    return "-" if v is None else "%.3f" % v if abs(v) < 10 else "%.1f" % v


def table(result):
    lines = ["| groom | lod | view | " + " | ".join(KEYS) + " | cards | tris |",
             "|---|---|---|" + "---|" * (len(KEYS) + 2)]
    for groom, g in result["grooms"].items():
        for lod, mean in g["mean"].items():
            stats = g["lods"][lod]
            rows = [("mean", mean)] + [(view, v[lod]) for view, v in g["views"].items()]
            for view, m in rows:
                lines.append("| %s | %s | %s | %s | %d | %d |" % (groom, lod, view, " | ".join(fmt(m.get(k)) for k in KEYS),
                                                                   stats["cards"], stats["triangles"]))
    return "\n".join(lines)


def compare(before, after, md=None):
    a = json.load(open(os.path.join(before, "metrics.json")))
    b = json.load(open(os.path.join(after, "metrics.json")))
    lines = ["| groom | lod | view | " + " | ".join(KEYS) + " | tris | textures | atlas |",
             "|---|---|---|" + "---|" * (len(KEYS) + 3)]
    for groom in b["grooms"]:
        if groom not in a["grooms"]:
            continue
        for lod in b["grooms"][groom]["mean"]:
            sa, sb = a["grooms"][groom]["lods"][lod], b["grooms"][groom]["lods"][lod]
            views = ["mean"] + list(b["grooms"][groom]["views"])
            for view in views:
                ma = a["grooms"][groom]["mean"][lod] if view == "mean" else a["grooms"][groom]["views"][view][lod]
                mb = b["grooms"][groom]["mean"][lod] if view == "mean" else b["grooms"][groom]["views"][view][lod]
                cells = ["%s → %s" % (fmt(ma.get(k)), fmt(mb.get(k))) for k in KEYS]
                lines.append("| %s | %s | %s | %s | %d → %d | %d → %d | %d → %d |" % (
                    groom, lod, view, " | ".join(cells), sa["triangles"], sb["triangles"], sa["textures"], sb["textures"],
                    sa.get("atlas", 0), sb.get("atlas", 0)))
    text = "\n".join(lines)
    if md:
        with open(md, "w", encoding="utf-8") as f:
            f.write(text + "\n")
    print(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    r = sub.add_parser("run")
    r.add_argument("--out", required=True)
    r.add_argument("--build", default=os.path.join(REPO, "build", "win-maya2022"))
    r.add_argument("--grooms", default="long,fur,curly")
    r.add_argument("--lods", default="hero,mid,low")
    r.add_argument("--opengl", action="store_true", help="Viewport 2.0 on OpenGL instead of DirectX 11")
    r.add_argument("--size", type=int, default=720)
    r.add_argument("--no-cap", action="store_true", help="no hair cap (bake_scalp) under the cards")
    m = sub.add_parser("metrics")
    m.add_argument("out")
    c = sub.add_parser("compare")
    c.add_argument("before")
    c.add_argument("after")
    c.add_argument("--md")
    a = parser.parse_args()
    if a.command == "run":
        run_maya(a.out, a.build, a.grooms.split(","), a.lods.split(","), a.opengl, a.size, not a.no_cap)
        metrics(a.out)
    elif a.command == "metrics":
        metrics(a.out)
    else:
        compare(a.before, a.after, a.md)


if __name__ == "__main__":
    main()
