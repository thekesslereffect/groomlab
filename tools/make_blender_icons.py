# SPDX-License-Identifier: Apache-2.0
"""Renders GroomLab's icons (hosts/maya/icons/groomLab_*.svg) as the PNGs the Blender add-on shows
(hosts/blender/addon/groomlab/icons/<name>.png, 64 px), in the Maya palette's default theme colors
(hosts/maya/themes/playful.json, as groomlab.ui.widgets.icon colors them):

    python tools/make_blender_icons.py        (Python 3 with Pillow)

Run it after tools/make_icons.py changes the SVGs; the PNGs are committed (Blender's previews read
PNGs, and the extension build needs no SVG renderer). The SVGs use a small subset (path with M L H V
C Q A Z, rect, circle, ellipse, a rotated g; strokes with round caps and joins, dashes, opacity),
which this draws supersampled and scales down."""

import json
import math
import os
import re
import xml.etree.ElementTree as ET

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, os.pardir))
SVG_DIR = os.path.join(ROOT, "hosts", "maya", "icons")
OUT = os.path.join(ROOT, "hosts", "blender", "addon", "groomlab", "icons")
THEME = os.path.join(ROOT, "hosts", "maya", "themes", "playful.json")
SIZE = 64
SS = 8                       # supersampling
K = SIZE * SS / 32.0         # SVG units (32 x 32) to canvas pixels

# groomlab/ui/widgets.py: which theme color each icon's main color is, and the placeholders.
ICON_GROUPS = {
    "create": ("place", "draw", "part", "flow", "select"),
    "shape": ("comb", "slide", "smooth", "lift", "twist", "attract"),
    "trim": ("cut", "length", "lengths", "shape", "erase"),
    "paint": ("paint", "freeze", "unfreeze"),
}
PLACEHOLDERS = {"main": "#3D7EFF", "shade": "#1F4FB3", "tint": "#A8C7FF", "ink": "#F2F2F5", "shine": "#FFFFFF",
                "red": "#FF4D50", "yellow": "#FFC53E", "blue": "#2F7BF7", "emerald": "#2ECC90"}


def _hex(c):
    c = c.lstrip("#")
    return tuple(int(c[i:i + 2], 16) for i in (0, 2, 4))


def _mix(a, b, f):
    ca, cb = _hex(a), _hex(b)
    return "#%02X%02X%02X" % tuple(int(round(x + (y - x) * f)) for x, y in zip(ca, cb))


def colors_for(name, theme):
    palette = theme["palette"]

    def resolve(v):
        if v == "accent":
            v = theme["ui"]["accent"]
        return palette.get(v, v)

    group = next((g for g, names in ICON_GROUPS.items() if name in names), "app")
    main = resolve(theme["icons"].get(group, "accent"))
    out = {"main": main, "shade": _mix(main, "#000000", 0.38), "tint": _mix(main, "#FFFFFF", 0.5),
           "ink": resolve(theme["icons"].get("ink", "white")), "shine": "#FFFFFF"}
    for n in ("red", "yellow", "blue", "emerald"):
        out[n] = resolve(n)
    return out


# ---- Geometry ---------------------------------------------------------------------------------

def _cubic(p0, p1, p2, p3, n=24):
    return [tuple((1 - t) ** 3 * a + 3 * (1 - t) ** 2 * t * b + 3 * (1 - t) * t * t * c + t ** 3 * d
                  for a, b, c, d in zip(p0, p1, p2, p3)) for t in (i / n for i in range(1, n + 1))]


def _quad(p0, p1, p2, n=16):
    return [tuple((1 - t) ** 2 * a + 2 * (1 - t) * t * b + t * t * c for a, b, c in zip(p0, p1, p2))
            for t in (i / n for i in range(1, n + 1))]


def _arc(p0, rx, ry, phi, large, sweep, p1, n=32):
    """SVG's endpoint arc as points (the spec's center conversion)."""
    if rx == 0 or ry == 0:
        return [p1]
    phi = math.radians(phi)
    cp, sp = math.cos(phi), math.sin(phi)
    dx, dy = (p0[0] - p1[0]) / 2.0, (p0[1] - p1[1]) / 2.0
    x1, y1 = cp * dx + sp * dy, -sp * dx + cp * dy
    rx, ry = abs(rx), abs(ry)
    lam = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry)
    if lam > 1:
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1
    den = rx * rx * y1 * y1 + ry * ry * x1 * x1
    co = math.sqrt(max(0.0, num / den)) if den else 0.0
    if large == sweep:
        co = -co
    cx1, cy1 = co * rx * y1 / ry, -co * ry * x1 / rx
    cx = cp * cx1 - sp * cy1 + (p0[0] + p1[0]) / 2.0
    cy = sp * cx1 + cp * cy1 + (p0[1] + p1[1]) / 2.0

    def ang(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a

    t1 = ang(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry)
    dt = ang((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry)
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    elif sweep and dt < 0:
        dt += 2 * math.pi
    pts = []
    for i in range(1, n + 1):
        t = t1 + dt * i / n
        x, y = rx * math.cos(t), ry * math.sin(t)
        pts.append((cp * x - sp * y + cx, sp * x + cp * y + cy))
    return pts


def parse_path(d):
    """Subpaths [(points, closed)] of a path's d (absolute commands)."""
    tokens = re.findall(r"[MLHVCQAZmlhvcqaz]|-?\d*\.?\d+(?:e-?\d+)?", d)
    subs, pts, closed = [], [], False
    cur = (0.0, 0.0)
    i, cmd = 0, None

    def num():
        nonlocal i
        v = float(tokens[i])
        i += 1
        return v

    while i < len(tokens):
        if re.match(r"[A-Za-z]", tokens[i]):
            cmd = tokens[i]
            i += 1
            if cmd in "Zz":
                closed = True
                if pts:
                    subs.append((pts, True))
                    cur = pts[0]
                pts, closed = [], False
                continue
        if cmd == "M":
            if pts:
                subs.append((pts, False))
            cur = (num(), num())
            pts = [cur]
            cmd = "L"
        elif cmd == "L":
            cur = (num(), num())
            pts.append(cur)
        elif cmd == "H":
            cur = (num(), cur[1])
            pts.append(cur)
        elif cmd == "V":
            cur = (cur[0], num())
            pts.append(cur)
        elif cmd == "C":
            c1, c2, end = (num(), num()), (num(), num()), (num(), num())
            pts += _cubic(cur, c1, c2, end)
            cur = end
        elif cmd == "Q":
            c1, end = (num(), num()), (num(), num())
            pts += _quad(cur, c1, end)
            cur = end
        elif cmd == "A":
            rx, ry, phi, large, sweep = num(), num(), num(), int(num()), int(num())
            end = (num(), num())
            pts += _arc(cur, rx, ry, phi, large, sweep, end)
            cur = end
        else:
            raise ValueError("path command %r is not supported" % cmd)
    if pts:
        subs.append((pts, False))
    return subs


def rounded_rect(x, y, w, h, rx, ry=None):
    ry = rx if ry is None else ry
    rx, ry = min(rx, w / 2.0), min(ry, h / 2.0)
    if rx <= 0 or ry <= 0:
        return [(x, y), (x + w, y), (x + w, y + h), (x, y + h)]
    pts = []
    for cx, cy, a0 in ((x + w - rx, y + ry, -90), (x + w - rx, y + h - ry, 0), (x + rx, y + h - ry, 90),
                       (x + rx, y + ry, 180)):
        for k in range(9):
            a = math.radians(a0 + 90 * k / 8.0)
            pts.append((cx + rx * math.cos(a), cy + ry * math.sin(a)))
    return pts


def ellipse(cx, cy, rx, ry, n=72):
    return [(cx + rx * math.cos(2 * math.pi * k / n), cy + ry * math.sin(2 * math.pi * k / n)) for k in range(n)]


def dashes(points, closed, pattern):
    """The polyline cut into dashes (on, off, on, off...)."""
    if closed:
        points = points + [points[0]]
    out, cur, on, left, idx = [], [points[0]], True, pattern[0], 0
    for a, b in zip(points, points[1:]):
        seg = math.hypot(b[0] - a[0], b[1] - a[1])
        pos = 0.0
        while seg - pos > left:
            pos += left
            t = pos / seg
            p = (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)
            if on:
                cur.append(p)
                out.append(cur)
            cur = [p]
            on = not on
            idx = (idx + 1) % len(pattern)
            left = pattern[idx]
        left -= seg - pos
        if on:
            cur.append(b)
    if on and len(cur) > 1:
        out.append(cur)
    return out


# ---- Drawing ----------------------------------------------------------------------------------

def _style(el, inherited):
    s = dict(inherited)
    for k in ("fill", "stroke", "stroke-width", "opacity", "stroke-dasharray", "stroke-linecap", "transform"):
        if el.get(k) is not None:
            s[k] = el.get(k)
    return s


def _transform(s):
    t = s.get("transform")
    if not t:
        return lambda p: p
    m = re.match(r"rotate\(([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\)", t)
    if not m:
        raise ValueError("transform %r is not supported" % t)
    a, cx, cy = math.radians(float(m.group(1))), float(m.group(2)), float(m.group(3))
    ca, sa = math.cos(a), math.sin(a)
    return lambda p: (cx + (p[0] - cx) * ca - (p[1] - cy) * sa, cy + (p[0] - cx) * sa + (p[1] - cy) * ca)


def _shapes(el, s):
    """[(points, closed)] of an element in SVG units."""
    tag = el.tag.split("}")[-1]
    f = lambda key, default=0.0: float(el.get(key, default))
    if tag == "path":
        return parse_path(el.get("d"))
    if tag == "rect":
        return [(rounded_rect(f("x"), f("y"), f("width"), f("height"), f("rx"), f("ry", f("rx"))), True)]
    if tag == "circle":
        return [(ellipse(f("cx"), f("cy"), f("r"), f("r")), True)]
    if tag == "ellipse":
        return [(ellipse(f("cx"), f("cy"), f("rx"), f("ry")), True)]
    raise ValueError("element %r is not supported" % tag)


def draw_element(img, el, s, colors):
    shapes = _shapes(el, s)
    tf = _transform(s)
    opacity = float(s.get("opacity", 1.0))
    canvas = lambda pts: [(x * K, y * K) for x, y in (tf(p) for p in pts)]
    fill, stroke = s.get("fill", "none"), s.get("stroke", "none")
    if fill and fill != "none":
        mask = Image.new("L", img.size, 0)
        d = ImageDraw.Draw(mask)
        for pts, _closed in shapes:
            if len(pts) >= 3:
                d.polygon(canvas(pts), fill=255)
        _paint(img, mask, colors.get(fill.upper(), fill), opacity)
    if stroke and stroke != "none":
        width = float(s.get("stroke-width", 1.0)) * K
        cap = s.get("stroke-linecap", "round")
        dash = s.get("stroke-dasharray")
        mask = Image.new("L", img.size, 0)
        d = ImageDraw.Draw(mask)
        pieces = []
        for pts, closed in shapes:
            if dash:
                pieces += [(p, False) for p in dashes(pts, closed, [float(v) for v in dash.split()])]
            else:
                pieces.append((pts + [pts[0]] if closed else pts, closed))
        r = width / 2.0
        for pts, closed in pieces:
            c = canvas(pts)
            for a, b in zip(c, c[1:]):
                d.line([a, b], fill=255, width=max(1, int(round(width))))
            joints = c if cap == "round" or closed else c[1:-1]
            for x, y in joints:
                d.ellipse([x - r, y - r, x + r, y + r], fill=255)
        _paint(img, mask, colors.get(stroke.upper(), stroke), opacity)


def _paint(img, mask, color, opacity):
    rgb = _hex(color)
    if opacity < 1.0:
        mask = mask.point(lambda v: int(v * opacity))
    layer = Image.new("RGBA", img.size, rgb + (0,))
    layer.putalpha(mask)
    img.alpha_composite(layer)


def walk(img, el, s, colors):
    for child in el:
        cs = _style(child, s)
        if child.tag.split("}")[-1] == "g":
            walk(img, child, cs, colors)
        else:
            draw_element(img, child, cs, colors)


def render(path, colors_by_placeholder):
    root = ET.parse(path).getroot()
    img = Image.new("RGBA", (SIZE * SS, SIZE * SS), (0, 0, 0, 0))
    walk(img, root, _style(root, {}), colors_by_placeholder)
    return img.resize((SIZE, SIZE), Image.LANCZOS)


def main():
    with open(THEME, encoding="utf-8") as f:
        theme = json.load(f)
    os.makedirs(OUT, exist_ok=True)
    count = 0
    for file in sorted(os.listdir(SVG_DIR)):
        m = re.match(r"groomLab_(\w+)\.svg$", file)
        if not m:
            continue
        name = m.group(1)
        colors = colors_for(name, theme)
        by_placeholder = {PLACEHOLDERS[role].upper(): colors[role] for role in PLACEHOLDERS}
        render(os.path.join(SVG_DIR, file), by_placeholder).save(os.path.join(OUT, name + ".png"), optimize=True)
        count += 1
    print(count, "icons in", OUT)


if __name__ == "__main__":
    main()
