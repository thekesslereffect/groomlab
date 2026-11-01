# SPDX-License-Identifier: GPL-3.0-or-later
"""A small drawn UI for the 3D view: what the GroomLab window (window.py) is made of, in the app's
and Maya's look (the theme's colors and sizes, hosts/maya/themes/charcoal.json; Inter; our icons)
rather than Blender's panels, which take no styling.

Immediate mode: a window's content is laid out again (Layout) for each draw and each event, from
Blender's data as it is then, so nothing here holds a stale reference across undo. Widgets read and
write RNA properties through Prop (their range, steps, labels and tooltips are the property's own).

Drawing: rounded rectangles by one signed-distance shader (fill, border, soft edge for shadows),
icons as textures (the add-on's PNGs, read here without bpy.data, so they load while drawing), text
with blf. Coordinates are the region's pixels, bottom-up as Blender's; sizes are the theme's times
the preferences' UI scale.

Input: press() on a widget returns an Interaction that the window's modal operator feeds the
events to until it says it is done (a slider's drag or typing, a menu's pick, the title bar's drag)."""

import ast
import json
import math
import operator
import os
import struct
import time
import zlib

import blf
import bpy
import gpu
from gpu_extras.batch import batch_for_shader

HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.normpath(os.path.join(HERE, os.pardir, os.pardir, os.pardir))  # a source checkout's root


def _first(*paths):
    for p in paths:
        if os.path.isfile(p):
            return p
    return paths[0]


FONT_FILE = _first(os.path.join(HERE, "fonts", "Inter[opsz,wght].ttf"),
                   os.path.join(_REPO, "app", "fonts", "Inter[opsz,wght].ttf"))
THEME_FILE = _first(os.path.join(HERE, "themes", "charcoal.json"),
                    os.path.join(_REPO, "maya", "themes", "charcoal.json"))
ICON_DIR = os.path.join(HERE, "icons")
CAP_HEIGHT = 0.727  # Inter's cap height, in ems: centers text on a row
TOOLTIP_DELAY = 0.6  # seconds the pointer rests on a widget before its tooltip shows


# --- Theme -------------------------------------------------------------------------------------

def linear(c):
    """An sRGB channel (0-1) as linear: the 3D view's overlay takes linear colors and shows them in
    display space, so a theme's #1C1C1C is drawn as linear 0.011 to come out as #1C1C1C."""
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


_LINEAR_BYTES = [linear(i / 255.0) for i in range(256)]


def _rgba(value, palette, alpha=1.0):
    value = palette.get(value, value)
    if not isinstance(value, str) or not value.startswith("#"):
        return (1.0, 0.0, 1.0, 1.0)
    h = value.lstrip("#")
    return (_LINEAR_BYTES[int(h[0:2], 16)], _LINEAR_BYTES[int(h[2:4], 16)], _LINEAR_BYTES[int(h[4:6], 16)], alpha)


class Theme:
    """The theme file's ui colors (as RGBA) and sizes (the default density, unscaled)."""

    def __init__(self, path=THEME_FILE):
        try:
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError):
            data = {}
        ui, palette = data.get("ui", {}), data.get("palette", {})
        fallback = {"background": "#141414", "panel": "#1C1C1C", "raised": "#262626", "hover": "#303030",
                    "border": "#2A2A2A", "text": "#EDEDED", "textDim": "#A3A3A3", "textMuted": "#737373",
                    "accent": "#5B8DEF", "accentText": "#FFFFFF", "selection": "#35435E", "warning": "yellow",
                    "error": "red"}
        for key, default in fallback.items():
            setattr(self, key, _rgba(ui.get(key, default), palette))
        self.track = _rgba("#2E2E2E", palette)
        self.radius = float(ui.get("radius", 6))
        self.font_size = float(ui.get("fontSize", 12))
        self.row = float(ui.get("rowHeight", 26))


def with_alpha(color, alpha):
    return (color[0], color[1], color[2], alpha)


# --- Drawing -----------------------------------------------------------------------------------

_RECT_VERT = """
void main() {
    float pad = shape.w;
    pos = rect.xy - pad + position * (rect.zw + 2.0 * pad);
    gl_Position = ModelViewProjectionMatrix * vec4(pos, 0.0, 1.0);
}
"""
_RECT_FRAG = """
float roundedBox(vec2 p, vec2 half_size, float r) {
    vec2 q = abs(p) - half_size + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}
void main() {
    float r = shape.x, border = shape.y, soft = max(shape.z, 1.0);
    vec2 half_size = rect.zw * 0.5;
    float d = roundedBox(pos - rect.xy - half_size, half_size, min(r, min(half_size.x, half_size.y)));
    float coverage = clamp(0.5 - d / soft, 0.0, 1.0);
    vec4 c = fill;
    if (border > 0.0) {
        float t = clamp(d + border + 0.5, 0.0, 1.0);
        c = mix(fill, stroke, t);
    }
    FragColor = vec4(c.rgb, c.a * coverage);
}
"""
_ICON_VERT = """
void main() {
    uv = position;
    gl_Position = ModelViewProjectionMatrix * vec4(rect.xy + position * rect.zw, 0.0, 1.0);
}
"""
_ICON_FRAG = """
void main() {
    vec4 t = texture(image, uv);
    FragColor = vec4(mix(t.rgb, tint.rgb, tint.a), t.a * alpha.x);
}
"""

_shaders = {}
_textures = {}
_font = [None]


def _rect_shader():
    if "rect" not in _shaders:
        iface = gpu.types.GPUStageInterfaceInfo("groomlab_ui_rect")
        iface.smooth("VEC2", "pos")
        info = gpu.types.GPUShaderCreateInfo()
        info.push_constant("MAT4", "ModelViewProjectionMatrix")
        info.push_constant("VEC4", "rect")
        info.push_constant("VEC4", "fill")
        info.push_constant("VEC4", "stroke")
        info.push_constant("VEC4", "shape")  # radius, border width, edge softness, the quad's margin
        info.vertex_in(0, "VEC2", "position")
        info.vertex_out(iface)
        info.fragment_out(0, "VEC4", "FragColor")
        info.vertex_source(_RECT_VERT)
        info.fragment_source(_RECT_FRAG)
        shader = gpu.shader.create_from_info(info)
        quad = batch_for_shader(shader, "TRI_FAN", {"position": ((0, 0), (1, 0), (1, 1), (0, 1))})
        _shaders["rect"] = (shader, quad)
    return _shaders["rect"]


def _icon_shader():
    if "icon" not in _shaders:
        iface = gpu.types.GPUStageInterfaceInfo("groomlab_ui_icon")
        iface.smooth("VEC2", "uv")
        info = gpu.types.GPUShaderCreateInfo()
        info.push_constant("MAT4", "ModelViewProjectionMatrix")
        info.push_constant("VEC4", "rect")
        info.push_constant("VEC4", "tint")   # rgb, how much of it replaces the icon's own color
        info.push_constant("VEC4", "alpha")  # x: opacity
        info.sampler(0, "FLOAT_2D", "image")
        info.vertex_in(0, "VEC2", "position")
        info.vertex_out(iface)
        info.fragment_out(0, "VEC4", "FragColor")
        info.vertex_source(_ICON_VERT)
        info.fragment_source(_ICON_FRAG)
        shader = gpu.shader.create_from_info(info)
        quad = batch_for_shader(shader, "TRI_FAN", {"position": ((0, 0), (1, 0), (1, 1), (0, 1))})
        _shaders["icon"] = (shader, quad)
    return _shaders["icon"]


def read_png(path):
    """(width, height, rows of RGBA bytes bottom row first) of an 8-bit, non-interlaced PNG."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG: %s" % path)
    pos, idat, header = 8, [], None
    while pos < len(data):
        (n,) = struct.unpack(">I", data[pos:pos + 4])
        kind, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    width, height, depth, ctype, _, _, interlace = header
    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(ctype)
    if depth != 8 or interlace or channels is None:
        raise ValueError("unsupported PNG (8-bit gray/RGB/RGBA, not interlaced): %s" % path)
    raw, stride, n = zlib.decompress(b"".join(idat)), width * channels, channels
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(height):
        kind, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        if kind == 1:
            for x in range(n, stride):
                line[x] = (line[x] + line[x - n]) & 255
        elif kind == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 255
        elif kind == 3:
            for x in range(stride):
                line[x] = (line[x] + ((line[x - n] if x >= n else 0) + prev[x]) // 2) & 255
        elif kind == 4:
            for x in range(stride):
                a = line[x - n] if x >= n else 0
                b, c = prev[x], (prev[x - n] if x >= n else 0)
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    rgba = []
    for line in reversed(rows):
        out = bytearray(width * 4)
        for x in range(width):
            px = line[x * n:x * n + n]
            if n == 4:
                out[x * 4:x * 4 + 4] = px
            elif n == 3:
                out[x * 4:x * 4 + 4] = bytes((px[0], px[1], px[2], 255))
            elif n == 2:
                out[x * 4:x * 4 + 4] = bytes((px[0], px[0], px[0], px[1]))
            else:
                out[x * 4:x * 4 + 4] = bytes((px[0], px[0], px[0], 255))
        rgba.append(out)
    return width, height, rgba


def _texture(name):
    if name not in _textures:
        tex = None
        path = os.path.join(ICON_DIR, name + ".png")
        if os.path.isfile(path):
            w, h, rows = read_png(path)
            floats = [(_LINEAR_BYTES[b] if i % 4 != 3 else b / 255.0) for row in rows for i, b in enumerate(row)]
            buf = gpu.types.Buffer("FLOAT", len(floats), floats)
            for fmt in ("SFLOAT_16_16_16_16", "RGBA16F"):  # Blender 5's name, then 4's
                try:
                    tex = gpu.types.GPUTexture((w, h), format=fmt, data=buf)
                    break
                except (TypeError, ValueError):
                    continue
        _textures[name] = tex
    return _textures[name]


def font():
    if _font[0] is None:
        fid = blf.load(FONT_FILE) if os.path.isfile(FONT_FILE) else -1
        _font[0] = fid if fid != -1 else 0  # 0: Blender's own UI font
    return _font[0]


def forget_font():
    """The font loaded again on its next use: Blender drops the fonts add-ons loaded when it opens a
    file (the window then drew no text at all)."""
    _font[0] = None


def release():
    """Drops the GPU resources and the font (the add-on's unregister)."""
    _shaders.clear()
    _textures.clear()
    if _font[0]:
        blf.unload(FONT_FILE)
    _font[0] = None


class Painter:
    """Draws in a region's pixels (POST_PIXEL), sizes already scaled."""

    def __init__(self, theme, scale):
        self.theme, self.scale = theme, scale
        self.mvp = gpu.matrix.get_projection_matrix() @ gpu.matrix.get_model_view_matrix()
        self.fade = 1.0  # what the widget being drawn multiplies its alpha by (a disabled one's dim)
        gpu.state.blend_set("ALPHA")

    def _faded(self, color):
        return color if self.fade >= 1.0 else (color[0], color[1], color[2], color[3] * self.fade)

    def rect(self, r, fill, radius=0.0, stroke=None, border=0.0, soft=0.0):
        shader, quad = _rect_shader()
        gpu.state.blend_set("ALPHA")  # blf leaves blending off
        shader.bind()
        shader.uniform_float("ModelViewProjectionMatrix", self.mvp)
        shader.uniform_float("rect", r)
        shader.uniform_float("fill", self._faded(fill))
        shader.uniform_float("stroke", self._faded(stroke or fill))
        shader.uniform_float("shape", (radius, border if stroke else 0.0, soft, 1.0 + soft))
        quad.draw(shader)

    def icon(self, name, r, tint=None, alpha=1.0):
        tex = _texture(name)
        if tex is None:
            return
        shader, quad = _icon_shader()
        gpu.state.blend_set("ALPHA")
        shader.bind()
        shader.uniform_float("ModelViewProjectionMatrix", self.mvp)
        shader.uniform_float("rect", r)
        shader.uniform_float("tint", (tint[0], tint[1], tint[2], 1.0) if tint else (0.0, 0.0, 0.0, 0.0))
        shader.uniform_float("alpha", (alpha * self.fade, 0.0, 0.0, 0.0))
        shader.uniform_sampler("image", tex)
        quad.draw(shader)

    def glyph(self, name, r, color):
        """An icon: the window's mono glyphs (ui_*) in `color`, the tools' own in theirs."""
        self.icon(name, r, tint=color if name.startswith("ui_") else None)

    def lines(self, points, color, width=1.5):
        """A polyline through `points` (pairs), `width` unscaled pixels wide."""
        shader = gpu.shader.from_builtin("POLYLINE_UNIFORM_COLOR")
        batch = batch_for_shader(shader, "LINE_STRIP", {"pos": [(x, y, 0.0) for x, y in points]})
        shader.bind()
        shader.uniform_float("viewportSize", gpu.state.viewport_get()[2:])
        shader.uniform_float("lineWidth", width * self.scale)
        shader.uniform_float("color", self._faded(color))
        batch.draw(shader)

    def text_size(self, text, size=None):
        fid = font()
        blf.size(fid, (size or self.theme.font_size) * self.scale)
        return blf.dimensions(fid, text)[0]

    def text(self, text, r, color, size=None, align="left", clip=True):
        """`text` in rect r (x, y, w, h), centered vertically; cut with an ellipsis to fit."""
        fid = font()
        px = (size or self.theme.font_size) * self.scale
        blf.size(fid, px)
        width = blf.dimensions(fid, text)[0]
        if clip and width > r[2] > 0:
            while text and blf.dimensions(fid, text + "…")[0] > r[2]:
                text = text[:-1]
            text += "…"
            width = blf.dimensions(fid, text)[0]
        x = r[0] + (r[2] - width) * {"left": 0.0, "center": 0.5, "right": 1.0}[align]
        y = r[1] + r[3] * 0.5 - px * CAP_HEIGHT * 0.5
        blf.position(fid, round(x), round(y), 0)
        blf.color(fid, *self._faded(color))
        blf.draw(fid, text)


def inset(r, dx, dy=None):
    dy = dx if dy is None else dy
    return (r[0] + dx, r[1] + dy, r[2] - 2 * dx, r[3] - 2 * dy)


def contains(r, x, y):
    return r[0] <= x < r[0] + r[2] and r[1] <= y < r[1] + r[3]


# --- Values --------------------------------------------------------------------------------------

_OPS = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul, ast.Div: operator.truediv,
        ast.FloorDiv: operator.floordiv, ast.Mod: operator.mod, ast.Pow: operator.pow}


def evaluate(text, current=0.0):
    """A typed number: arithmetic (+ - * / // % ** and parentheses); a leading * or / applies to the
    current value ("*2" doubles it). Raises ValueError for anything else."""
    text = text.strip().replace(",", ".")
    if text[:1] in ("*", "/"):
        text = repr(float(current)) + text

    def walk(node):
        if isinstance(node, ast.Expression):
            return walk(node.body)
        if isinstance(node, ast.Constant) and isinstance(node.value, (int, float)):
            return float(node.value)
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.UAdd, ast.USub)):
            v = walk(node.operand)
            return v if isinstance(node.op, ast.UAdd) else -v
        if isinstance(node, ast.BinOp) and type(node.op) in _OPS:
            return _OPS[type(node.op)](walk(node.left), walk(node.right))
        raise ValueError("not a number: %s" % text)

    try:
        value = walk(ast.parse(text, mode="eval"))
    except (SyntaxError, ZeroDivisionError, OverflowError) as e:
        raise ValueError(str(e))
    if not math.isfinite(value):
        raise ValueError("not a number: %s" % text)
    return value


class Prop:
    """An RNA property of `owner` by name: its value and what the widgets need of its definition."""

    def __init__(self, owner, name, label=None):
        self.owner, self.name = owner, name
        self.rna = owner.bl_rna.properties[name]
        self.label = label if label is not None else self.rna.name
        self.tooltip = self.rna.description

    @property
    def kind(self):
        return self.rna.type

    def get(self):
        return getattr(self.owner, self.name)

    def set(self, value):
        if self.kind == "INT":
            value = int(round(value))
        if self.kind in ("INT", "FLOAT"):
            value = min(max(value, self.rna.hard_min), self.rna.hard_max)
        if self.get() != value:
            setattr(self.owner, self.name, value)
            changed.append(self.label or self.name)  # the window's operator pushes one undo step for it

    def span(self):
        lo, hi = float(self.rna.soft_min), float(self.rna.soft_max)
        return (lo, hi) if hi > lo else (0.0, 1.0)

    def items(self, icons=False):
        """[(id, name, tip)], with each item's icon_value after (icons=True)."""
        items = [(i.identifier, i.name, i.description) + ((i.icon,) if icons else ()) for i in self.rna.enum_items]
        if items:
            return items
        # A Python enum whose items come from a function: its definition's, called as Blender does.
        deferred = getattr(type(self.owner), "__annotations__", {}).get(self.name)
        source = getattr(deferred, "keywords", {}).get("items")
        if callable(source):
            try:
                source = source(self.owner, bpy.context)
            except Exception:  # noqa: BLE001 - a menu that cannot list shows none
                source = []
        out = []
        for i in source or []:
            if not i:
                continue
            item = (i[0], i[1], i[2] if len(i) > 2 else "")
            if icons:
                icon = i[3] if len(i) > 3 else 0
                item += (icon if isinstance(icon, int) else 0,)
            out.append(item)
        return out

    @property
    def length_unit(self):
        return self.kind == "FLOAT" and self.rna.unit == "LENGTH"

    def text(self, value=None):
        value = self.get() if value is None else value
        if self.kind == "STRING":
            return str(value)
        if self.kind == "INT":
            text = str(int(value))
            return text + " px" if self.rna.subtype == "PIXEL" else text
        if self.length_unit:
            units = bpy.context.scene.unit_settings
            return bpy.utils.units.to_string(units.system, "LENGTH", value * units.scale_length, precision=3)
        if self.rna.subtype == "PERCENTAGE":
            return "%d%%" % round(value)
        digits = max(0, min(int(self.rna.precision), 4)) if self.rna.precision else 2
        return "%.*f" % (digits, value)

    def edit_text(self):
        """What a click opens for typing: the number alone (no unit), a length with its unit, or the text."""
        value = self.get()
        if self.kind == "STRING":
            return str(value)
        if self.kind == "INT":
            return str(int(value))
        if self.length_unit:
            return self.text()
        return ("%.4f" % value).rstrip("0").rstrip(".") or "0"

    def parse(self, text):
        """A typed value: text as it is; a length with its unit (2cm, 1.5 mm) or plain arithmetic."""
        if self.kind == "STRING":
            return text
        if self.length_unit:
            units = bpy.context.scene.unit_settings
            try:
                return bpy.utils.units.to_value(units.system, "LENGTH", text) / (units.scale_length or 1.0)
            except ValueError:
                pass
        return evaluate(text, self.get())


changed = []  # labels of the properties the widgets set since the window's operator last looked


class Choice:
    """A list of (id, name, tooltip) with get / set callables: a dropdown over something that is not
    an enum property (e.g. the grooms in the scene)."""

    kind = "ENUM"

    def __init__(self, label, items, get, set_, tooltip=""):
        self.label, self._items, self._get, self._set, self.tooltip = label, items, get, set_, tooltip

    def items(self):
        return self._items

    def get(self):
        return self._get()

    def set(self, value):
        self._set(value)


# --- Widgets --------------------------------------------------------------------------------------

class Widget:
    cursor = "DEFAULT"
    tooltip = ""
    disabled = False  # drawn dim, takes no press (Blender's enabled = False)
    dim = False       # drawn dim, still works (Blender's active = False)
    alert = False

    def __init__(self, wid, rect):
        self.id, self.rect = wid, rect

    def tooltip_text(self):
        tip = self.tooltip
        return tip() if callable(tip) else (tip or "")

    def draw(self, p, win):
        pass

    def press(self, win, context, event):
        return None


class Interaction:
    """What a press started: fed every event until it returns FINISHED or CANCELLED."""

    def event(self, win, context, event):
        return "FINISHED"

    def draw(self, p, win):
        pass


class Label(Widget):
    passive = True  # text: a press or a hover goes to what is under it (a list's row)

    def __init__(self, wid, rect, text, dim=True, size=None, align="left", icon=None, bright=False):
        super().__init__(wid, rect)
        self.text, self.size, self.align, self.icon, self.bright = text, size, align, icon, bright or not dim

    def draw(self, p, win):
        t, s = p.theme, p.scale
        caps = len(self.text) > 2 and self.text.upper() == self.text and any(c.isalpha() for c in self.text)
        color = t.error if self.alert else t.textMuted if caps else t.text if self.bright else t.textDim
        size = self.size or (t.font_size - 1 if caps else None)
        x, y, w, h = self.rect
        if self.icon:
            g = 16 * s
            block = g + (6 * s + p.text_size(self.text) if self.text else 0.0)
            gx = x + (w - block) * {"left": 0.0, "center": 0.5, "right": 1.0}[self.align]
            gx = max(x, gx)
            p.glyph(self.icon, (gx, y + (h - g) * 0.5, g, g), color)
            if self.text:
                left = gx + g + 6 * s
                p.text(self.text, (left, y, x + w - left, h), color, size)
            return
        p.text(self.text, self.rect, color, size, self.align)


class Heading(Widget):
    """A block's title: small caps over a faint rule."""

    def __init__(self, wid, rect, text):
        super().__init__(wid, rect)
        self.text = text

    def draw(self, p, win):
        t, s = p.theme, p.scale
        x, y, w, h = self.rect
        p.text(self.text.upper(), (x, y + 2 * s, w, h - 2 * s), t.textMuted, t.font_size - 1)
        p.rect((x, y, w, max(1.0, s)), t.border)


class Button(Widget):
    def __init__(self, wid, rect, text, action, icon=None, primary=False, tooltip="", flat=False, depress=False,
                 align="center"):
        super().__init__(wid, rect)
        self.text, self.action, self.icon, self.primary, self.tooltip = text, action, icon, primary, tooltip
        self.flat, self.depress, self.align = flat, depress, align

    def on(self):
        return self.depress

    def draw(self, p, win):
        t, s = p.theme, p.scale
        hover = win.hover == self.id and not self.disabled
        on = self.on()
        if self.primary:
            fill = tuple(min(1.0, c * 1.12) for c in t.accent[:3]) + (1.0,) if hover else t.accent
        elif on:
            fill = t.selection
        elif self.flat:
            fill = t.hover if hover else None
        else:
            fill = t.hover if hover else t.raised
        if fill is not None:
            p.rect(self.rect, fill, t.radius * s)
        x, y, w, h = self.rect
        if self.primary:
            color = t.accentText
        elif self.alert:
            color = t.error
        else:
            color = t.text if (on or hover or not self.flat) else t.textDim
        tw = p.text_size(self.text) if self.text else 0.0
        if self.icon:
            size = 16 * s
            block = size + (6 * s + tw if self.text else 0.0)
            ix = x + 8 * s if self.align == "left" and self.text else x + (w - block) * 0.5
            ix = max(x + 2 * s, ix)
            p.glyph(self.icon, (ix, y + (h - size) * 0.5, size, size), color)
            if self.text:
                left = ix + size + 6 * s
                p.text(self.text, (left, y, x + w - left - 4 * s, h), color)
        else:  # (a narrow segment keeps more of its text)
            p.text(self.text, inset(self.rect, min(8 * s, max(2 * s, (w - tw) * 0.5)), 0), color, align=self.align)

    def press(self, win, context, event):
        return ClickInteraction(self)


class ClickInteraction(Interaction):
    """A button's press: runs its action on release over it."""

    def __init__(self, widget):
        self.widget = widget

    def event(self, win, context, event):
        if event.type == "LEFTMOUSE" and event.value == "RELEASE":
            if contains(self.widget.rect, event.mouse_region_x, event.mouse_region_y):
                self.widget.action(context)
            return "FINISHED"
        if event.type in ("ESC", "RIGHTMOUSE"):
            return "CANCELLED"
        return "RUNNING"


class Glyph(Button):
    """A small square button drawn with lines: "close", "chevron_down", "chevron_right"."""

    def draw(self, p, win):
        t, s = p.theme, p.scale
        if win.hover == self.id:
            p.rect(self.rect, t.hover, t.radius * s)
        x, y, w, h = self.rect
        cx, cy, k = x + w * 0.5, y + h * 0.5, 4.0 * s
        color = t.text if win.hover == self.id else t.textDim
        if self.text == "close":
            p.lines([(cx - k, cy - k), (cx + k, cy + k)], color)
            p.lines([(cx - k, cy + k), (cx + k, cy - k)], color)
        elif self.text == "chevron_down":
            p.lines([(cx - k, cy + k * 0.5), (cx, cy - k * 0.5), (cx + k, cy + k * 0.5)], color)
        elif self.text == "chevron_right":
            p.lines([(cx - k * 0.5, cy + k), (cx + k * 0.5, cy), (cx - k * 0.5, cy - k)], color)


class Tile(Button):
    """A tool's tile: its icon over its name; the active one in the selection tone."""

    def __init__(self, wid, rect, text, action, icon, active, tooltip=""):
        super().__init__(wid, rect, text, action, icon, tooltip=tooltip)
        self.active = active

    def draw(self, p, win):
        t, s = p.theme, p.scale
        hover = win.hover == self.id
        fill = t.selection if self.active else t.hover if hover else t.raised
        p.rect(self.rect, fill, t.radius * s, t.accent if self.active else None, 1.0 * s)
        x, y, w, h = self.rect
        size = 24 * s
        p.icon(self.icon, (x + (w - size) * 0.5, y + h - size - 7 * s, size, size))
        p.text(self.text, (x + 2 * s, y + 4 * s, w - 4 * s, 14 * s), t.text if self.active or hover else t.textDim,
               t.font_size - 2, "center")


class Tabs(Widget):
    """An enum as underline tabs."""

    def __init__(self, wid, rect, prop):
        super().__init__(wid, rect)
        self.prop = prop
        self.items = prop.items()

    def _cells(self):
        x, y, w, h = self.rect
        n = max(1, len(self.items))
        return [((x + w * i / n, y, w / n, h), item) for i, item in enumerate(self.items)]

    def draw(self, p, win):
        t, s = p.theme, p.scale
        current = self.prop.get()
        p.rect((self.rect[0], self.rect[1], self.rect[2], max(1.0, s)), t.border)
        for r, (ident, name, _) in self._cells():
            on = ident == current
            hover = win.hover == self.id and contains(r, *win.mouse)
            p.text(name, r, t.text if on or hover else t.textDim, align="center")
            if on:
                p.rect((r[0] + 6 * s, r[1], r[2] - 12 * s, 2 * s), t.accent, 1 * s)

    def press(self, win, context, event):
        for r, (ident, _, _) in self._cells():
            if contains(r, event.mouse_region_x, event.mouse_region_y):
                self.prop.set(ident)
        return None


class Slider(Widget):
    """A number as a fill field: its name inside on the left, its value on the right, filled by the
    value's place in the soft range. Drag scrubs (Shift: 1/10, Ctrl: steps of 10x); a click types."""

    cursor = "SCROLL_X"

    def __init__(self, wid, rect, prop):
        super().__init__(wid, rect)
        self.prop, self.tooltip = prop, prop.tooltip

    def draw(self, p, win):
        t, s = p.theme, p.scale
        hover = win.hover == self.id
        r = self.rect
        p.rect(r, t.hover if hover else t.raised, t.radius * s)
        editing = isinstance(win.interaction, TypingInteraction) and win.interaction.widget.id == self.id
        if editing:
            win.interaction.draw_field(p, win)
            return
        lo, hi = self.prop.span()
        value = float(self.prop.get())
        frac = min(max((value - lo) / (hi - lo), 0.0), 1.0)
        if frac > 0.0:
            p.rect((r[0], r[1], max(r[2] * frac, 2 * t.radius * s * min(1.0, frac * 8)), r[3]), t.selection,
                   t.radius * s)
        pad = 9 * s
        value_text = self.prop.text()
        vw = p.text_size(value_text)
        if self.prop.label:
            p.text(self.prop.label, (r[0] + pad, r[1], r[2] - vw - 3 * pad, r[3]), t.text)
        else:  # no name: the value alone, centered
            p.text(value_text, r, t.text, align="center", clip=False)
            return
        p.text(value_text, (r[0], r[1], r[2] - pad, r[3]), t.text, align="right", clip=False)

    def press(self, win, context, event):
        if self.prop.kind == "STRING":
            return TypingInteraction(self)
        return ScrubInteraction(self, event)


class ScrubInteraction(Interaction):
    def __init__(self, widget, event):
        self.widget, self.x0 = widget, event.mouse_region_x
        self.start = float(widget.prop.get())
        self.moved = False

    def event(self, win, context, event):
        w = self.widget
        if event.type == "MOUSEMOVE":
            dx = event.mouse_region_x - self.x0
            if not self.moved and abs(dx) < 3 * win.scale:
                return "RUNNING"
            if not self.moved:
                self.moved = True
                context.window.cursor_modal_set("SCROLL_X")
            lo, hi = w.prop.span()
            per_px = (hi - lo) / max(w.rect[2], 1.0)
            if event.shift:
                per_px *= 0.1
            value = self.start + dx * per_px
            if event.ctrl:
                step = (hi - lo) / 10.0
                value = round(value / step) * step
            w.prop.set(value)
            return "RUNNING"
        if event.type == "LEFTMOUSE" and event.value == "RELEASE":
            context.window.cursor_modal_restore()
            if not self.moved:
                win.interaction = TypingInteraction(w)
                return "RUNNING"  # the window's operator keeps running for the typing
            return "FINISHED"
        if event.type in ("ESC", "RIGHTMOUSE") and event.value == "PRESS":
            context.window.cursor_modal_restore()
            w.prop.set(self.start)
            return "CANCELLED"
        return "RUNNING"


class TypingInteraction(Interaction):
    """A value typed in the field: Enter, Tab or a click away commits, Esc puts the old value back."""

    def __init__(self, widget):
        self.widget = widget
        self.old = widget.prop.get()
        self.text = widget.prop.edit_text()
        self.cursor = len(self.text)
        self.all = True  # the whole text selected: the first key replaces it
        self.error = False

    def draw_field(self, p, win):
        t, s = p.theme, p.scale
        r = self.widget.rect
        p.rect(r, t.panel, t.radius * s, t.accent, 1.0 * s)
        pad = 9 * s
        inner = (r[0] + pad, r[1], r[2] - 2 * pad, r[3])
        if self.all and self.text:
            tw = p.text_size(self.text)
            p.rect((inner[0] - 2 * s, r[1] + 5 * s, tw + 4 * s, r[3] - 10 * s), t.selection, 2 * s)
        p.text(self.text, inner, t.error if self.error else t.text, clip=False)
        if not self.all:
            cx = inner[0] + p.text_size(self.text[:self.cursor])
            p.rect((cx, r[1] + 6 * s, max(1.0, s), r[3] - 12 * s), t.text)

    def commit(self):
        try:
            self.widget.prop.set(self.widget.prop.parse(self.text))
        except ValueError:
            self.error = True
            return False
        return True

    def event(self, win, context, event):
        if event.value != "PRESS":
            return "RUNNING"
        kind = event.type
        if kind in ("RET", "NUMPAD_ENTER", "TAB"):
            return "FINISHED" if self.commit() else "RUNNING"
        if kind == "ESC":
            self.widget.prop.set(self.old)
            return "CANCELLED"
        if kind in ("LEFTMOUSE", "RIGHTMOUSE"):
            if contains(self.widget.rect, event.mouse_region_x, event.mouse_region_y):
                self.all = False
                return "RUNNING"
            self.commit()
            return "FINISHED_PASS"  # the click goes on to whatever it was over
        if event.ctrl and kind == "A":
            self.all = True
        elif event.ctrl and kind == "V":
            self._insert(context.window_manager.clipboard.strip())
        elif kind == "BACK_SPACE":
            if self.all:
                self.text, self.cursor, self.all = "", 0, False
            elif self.cursor:
                self.text = self.text[:self.cursor - 1] + self.text[self.cursor:]
                self.cursor -= 1
        elif kind == "DEL":
            if self.all:
                self.text, self.cursor, self.all = "", 0, False
            else:
                self.text = self.text[:self.cursor] + self.text[self.cursor + 1:]
        elif kind in ("LEFT_ARROW", "RIGHT_ARROW", "HOME", "END"):
            if self.all:
                self.cursor = 0 if kind in ("LEFT_ARROW", "HOME") else len(self.text)
                self.all = False
            else:
                self.cursor = {"LEFT_ARROW": max(0, self.cursor - 1), "RIGHT_ARROW": min(len(self.text), self.cursor + 1),
                               "HOME": 0, "END": len(self.text)}[kind]
        elif event.unicode and event.unicode.isprintable():
            self._insert(event.unicode)
        self.error = False
        return "RUNNING"

    def _insert(self, chars):
        if self.all:
            self.text, self.cursor, self.all = "", 0, False
        self.text = self.text[:self.cursor] + chars + self.text[self.cursor:]
        self.cursor += len(chars)


class Dropdown(Widget):
    """An enum (Prop or Choice) as a field showing its current item; a press opens its menu."""

    def __init__(self, wid, rect, prop, show_label=True):
        super().__init__(wid, rect)
        self.prop, self.show_label, self.tooltip = prop, show_label, prop.tooltip

    def current_name(self):
        current = self.prop.get()
        for ident, name, _ in self.prop.items():
            if ident == current:
                return name
        return "-"

    def draw(self, p, win):
        t, s = p.theme, p.scale
        open_ = isinstance(win.interaction, MenuInteraction) and win.interaction.widget.id == self.id
        hover = win.hover == self.id or open_
        r = self.rect
        p.rect(r, t.hover if hover else t.raised, t.radius * s)
        pad = 9 * s
        name = self.current_name()
        right = (r[0] + pad, r[1], r[2] - 2 * pad - 14 * s, r[3])
        if self.show_label and self.prop.label:
            p.text(self.prop.label, (r[0] + pad, r[1], r[2] * 0.45, r[3]), t.text)
            p.text(name, right, t.text, align="right")
        else:
            p.text(name, right, t.text)
        cx, cy, k = r[0] + r[2] - pad - 4 * s, r[1] + r[3] * 0.5, 3.5 * s
        p.lines([(cx - k, cy + k * 0.5), (cx, cy - k * 0.5), (cx + k, cy + k * 0.5)], t.textDim)

    def press(self, win, context, event):
        return MenuInteraction(self, win)


class MenuInteraction(Interaction):
    """A dropdown's open list, under (or over, when there is no room) its field."""

    def __init__(self, widget, win):
        self.widget, self.items = widget, widget.prop.items()
        s = win.scale
        x, y, w, h = widget.rect
        self.row, self.pad = 24 * s, 4 * s
        height = self.row * len(self.items) + 2 * self.pad
        top = y - 4 * s
        if top - height < 4 * s:  # no room below: open upward
            top = y + h + 4 * s + height
        self.rect = (x, top - height, w, height)
        self.hover = -1
        self.armed = False  # the press that opened it is released first

    def _index(self, mx, my):
        x, y, w, h = self.rect
        if not contains(self.rect, mx, my):
            return -1
        i = int((y + h - self.pad - my) // self.row)
        return i if 0 <= i < len(self.items) else -1

    def event(self, win, context, event):
        mx, my = event.mouse_region_x, event.mouse_region_y
        if event.type == "MOUSEMOVE":
            self.hover = self._index(mx, my)
            return "RUNNING"
        if event.type == "LEFTMOUSE" and event.value == "RELEASE":
            i = self._index(mx, my)
            if i >= 0 and self.armed:
                self.widget.prop.set(self.items[i][0])
                return "FINISHED"
            self.armed = True
            return "RUNNING"
        if event.type == "LEFTMOUSE" and event.value == "PRESS":
            i = self._index(mx, my)
            if i >= 0:
                self.widget.prop.set(self.items[i][0])
                return "FINISHED"
            return "CANCELLED" if contains(self.widget.rect, mx, my) else "CANCELLED_PASS"
        if event.type in ("ESC", "RIGHTMOUSE") and event.value == "PRESS":
            return "CANCELLED"
        return "RUNNING"

    def draw(self, p, win):
        t, s = p.theme, p.scale
        p.rect(inset(self.rect, -2 * s), (0, 0, 0, 0.35), 8 * s, soft=10 * s)
        p.rect(self.rect, t.panel, t.radius * s, t.border, 1.0 * s)
        x, y, w, h = self.rect
        current = self.widget.prop.get()
        for i, (ident, name, _) in enumerate(self.items):
            r = (x + 4 * s, y + h - self.pad - (i + 1) * self.row, w - 8 * s, self.row)
            if i == self.hover:
                p.rect(r, t.hover, t.radius * s)
            p.text(name, inset(r, 8 * s, 0), t.text if ident == current or i == self.hover else t.textDim)
            if ident == current:
                p.rect((r[0], r[1] + 6 * s, 2 * s, r[3] - 12 * s), t.accent, 1 * s)


class Check(Widget):
    """A boolean as a switch with its name."""

    def __init__(self, wid, rect, prop):
        super().__init__(wid, rect)
        self.prop, self.tooltip = prop, prop.tooltip

    def draw(self, p, win):
        t, s = p.theme, p.scale
        x, y, w, h = self.rect
        on = bool(self.prop.get())
        hover = win.hover == self.id
        sw, sh = 28 * s, 16 * s
        track = (x + w - sw, y + (h - sh) * 0.5, sw, sh)
        p.rect(track, t.accent if on else (t.hover if hover else t.track), sh * 0.5)
        knob = sh - 4 * s
        kx = track[0] + (sw - knob - 2 * s if on else 2 * s)
        p.rect((kx, track[1] + 2 * s, knob, knob), t.accentText if on else t.textDim, knob * 0.5)
        p.text(self.prop.label, (x, y, w - sw - 8 * s, h), t.text)

    def press(self, win, context, event):
        self.prop.set(not bool(self.prop.get()))
        return None


class TitleBar(Widget):
    """The window's top bar: its icon and name; dragging it moves the window."""

    cursor = "HAND"

    def __init__(self, wid, rect, text, icon):
        super().__init__(wid, rect)
        self.text, self.icon = text, icon

    def draw(self, p, win):
        t, s = p.theme, p.scale
        x, y, w, h = self.rect
        size = 16 * s
        p.icon(self.icon, (x + 10 * s, y + (h - size) * 0.5, size, size))
        p.text(self.text, (x + 10 * s + size + 8 * s, y, w, h), t.text, clip=False)

    def press(self, win, context, event):
        return MoveInteraction(win, event)


class MoveInteraction(Interaction):
    def __init__(self, win, event):
        self.win, self.mx, self.my = win, event.mouse_region_x, event.mouse_region_y
        self.right0, self.top0 = win.state.right, win.state.top

    def event(self, win, context, event):
        if event.type == "MOUSEMOVE":
            # The offsets are unscaled pixels (Window multiplies them by the UI scale): the pointer's
            # move divided by it, so the window stays under the pointer at any display scale.
            win.state.right = max(0.0, self.right0 - (event.mouse_region_x - self.mx) / win.scale)
            win.state.top = max(0.0, self.top0 - (event.mouse_region_y - self.my) / win.scale)
            return "RUNNING"
        if event.type == "LEFTMOUSE" and event.value == "RELEASE":
            return "FINISHED"
        if event.type in ("ESC", "RIGHTMOUSE") and event.value == "PRESS":
            win.state.right, win.state.top = self.right0, self.top0
            return "CANCELLED"
        return "RUNNING"


class ToggleButton(Button):
    """A boolean as a button that stays pressed while on (Blender's toggle=True, or an icon)."""

    def __init__(self, wid, rect, prop, text, icon=None, flat=False):
        super().__init__(wid, rect, text, None, icon, tooltip=prop.tooltip, flat=flat)
        self.prop = prop

    def on(self):
        return bool(self.prop.get())

    def press(self, win, context, event):
        self.prop.set(not bool(self.prop.get()))
        return None


class Segment(Button):
    """One choice of an enum shown expanded (a segmented row): pressed while it is the value."""

    def __init__(self, wid, rect, prop, ident, text, icon=None, tooltip=""):
        super().__init__(wid, rect, text, None, icon, tooltip=tooltip or prop.tooltip)
        self.prop, self.ident = prop, ident

    def on(self):
        value = self.prop.get()
        return self.ident in value if isinstance(value, (set, frozenset)) else value == self.ident

    def press(self, win, context, event):
        value = self.prop.get()
        if isinstance(value, (set, frozenset)):  # a flag enum: each choice toggles
            self.prop.set(set(value) ^ {self.ident})
        else:
            self.prop.set(self.ident)
        return None


class TextField(Slider):
    """A string as a field: its text; a click types (Enter commits, Esc puts it back)."""

    cursor = "TEXT"

    def __init__(self, wid, rect, prop, placeholder=""):
        super().__init__(wid, rect, prop)
        self.placeholder = placeholder

    def draw(self, p, win):
        t, s = p.theme, p.scale
        editing = isinstance(win.interaction, TypingInteraction) and win.interaction.widget.id == self.id
        r = self.rect
        p.rect(r, t.hover if win.hover == self.id else t.raised, t.radius * s)
        if editing:
            win.interaction.draw_field(p, win)
            return
        pad = 9 * s
        text = str(self.prop.get())
        if self.prop.label:
            lw = min(r[2] * 0.4, p.text_size(self.prop.label) + pad)
            p.text(self.prop.label, (r[0] + pad, r[1], lw, r[3]), t.text)
            r = (r[0] + lw + pad, r[1], r[2] - lw - pad, r[3])
        else:
            r = (r[0] + pad, r[1], r[2] - 2 * pad, r[3])
        p.text(text or self.placeholder, r, t.text if text else t.textMuted)

    def press(self, win, context, event):
        return TypingInteraction(self)


class ColorSwatch(Widget):
    """A color property as a swatch; a press opens Blender's color picker for it (open_picker)."""

    def __init__(self, wid, rect, prop, open_picker=None):
        super().__init__(wid, rect)
        self.prop, self.open_picker, self.tooltip = prop, open_picker, prop.tooltip

    def draw(self, p, win):
        t, s = p.theme, p.scale
        c = tuple(self.prop.get())
        color = (c[0], c[1], c[2], c[3] if len(c) > 3 else 1.0)  # (scene linear: the overlay takes linear)
        p.rect(self.rect, color, t.radius * s, t.hover if win.hover == self.id else t.border, 1.0 * s)

    def press(self, win, context, event):
        if self.open_picker is not None:
            self.open_picker(context, self.prop)
        return None


class Backdrop(Widget):
    """A box's or a list's background: drawn under its rows, never pressed."""

    passive = True

    def __init__(self, wid, rect, kind="box"):
        super().__init__(wid, rect)
        self.kind = kind

    def draw(self, p, win):
        t, s = p.theme, p.scale
        if self.kind == "list":
            p.rect(self.rect, t.background, t.radius * s, t.border, 1.0 * s)
        elif self.kind == "line":
            x, y, w, h = self.rect
            p.rect((x, y + h * 0.5, w, max(1.0, s)), t.border)
        else:
            p.rect(self.rect, with_alpha(t.raised, 0.45), t.radius * s, t.border, 1.0 * s)


class ListRow(Widget):
    """A list's row: picked on a press (its item made the active one); the picked row highlighted."""

    def __init__(self, wid, rect, active, pick):
        super().__init__(wid, rect)
        self.active, self.pick = active, pick
        self.context_menu = None  # a right-click: (context) -> opens the row's menu

    def draw(self, p, win):
        t, s = p.theme, p.scale
        if self.active:
            p.rect(self.rect, t.selection, (t.radius - 2) * s)
        elif win.hover == self.id:
            p.rect(self.rect, t.hover, (t.radius - 2) * s)

    def press(self, win, context, event):
        self.pick(context)
        return None


class PanelHeader(Widget):
    """A folding section's header (Blender's layout.panel and sub-panels): a chevron and its title;
    a press opens or folds it."""

    def __init__(self, wid, rect, text, open_, toggle):
        super().__init__(wid, rect)
        self.text, self.open, self.toggle = text, open_, toggle

    def draw(self, p, win):
        t, s = p.theme, p.scale
        x, y, w, h = self.rect
        hover = win.hover == self.id
        p.rect((x, y + h - max(1.0, s), w, max(1.0, s)), t.border)
        cx, cy, k = x + 6 * s, y + h * 0.5, 3.5 * s
        color = t.text if hover else t.textDim
        if self.open:
            p.lines([(cx - k, cy + k * 0.5), (cx, cy - k * 0.5), (cx + k, cy + k * 0.5)], color)
        else:
            p.lines([(cx - k * 0.5, cy + k), (cx + k * 0.5, cy), (cx - k * 0.5, cy - k)], color)
        p.text(self.text.upper(), (x + 18 * s, y, w - 18 * s, h), color, t.font_size - 1)

    def press(self, win, context, event):
        self.toggle()
        return None


def draw_tooltip(p, win, text):
    """A tooltip under the pointer, its lines wrapped, kept inside the region."""
    import textwrap
    t, s = p.theme, p.scale
    lines = []
    for paragraph in text.splitlines():
        lines += textwrap.wrap(paragraph, 56) or [""]
    lines = lines[:14]
    size = t.font_size - 1
    line_h = 17 * s
    width = max(p.text_size(line, size) for line in lines) + 20 * s
    height = line_h * len(lines) + 12 * s
    mx, my = win.mouse
    x = min(mx + 14 * s, win.region_w - width - 4 * s)
    y = max(4 * s, my - 22 * s - height)
    p.rect((x, y, width, height), (0, 0, 0, 0.3), 6 * s, soft=8 * s)
    p.rect((x, y, width, height), t.background, 6 * s, t.border, 1.0 * s)
    for i, line in enumerate(lines):
        p.text(line, (x + 10 * s, y + height - 6 * s - (i + 1) * line_h, width - 20 * s, line_h),
               t.text if i == 0 else t.textDim, size, clip=False)


# --- Layout ------------------------------------------------------------------------------------

class Layout:
    """Top-down placement in a column of width `w` starting at the region y `top` (bottom-up): each
    call takes a row and adds its widget."""

    def __init__(self, win, x, top, w):
        self.win, self.x, self.y, self.w = win, x, top, w
        self.s = win.scale
        self.row = win.theme.row * self.s
        self.gap = 6 * self.s
        self.widgets = []

    def _take(self, h, gap=True):
        r = (self.x, self.y - h, self.w, h)
        self.y -= h + (self.gap if gap else 0.0)
        return r

    def _id(self, kind, key):
        return "%s:%s" % (kind, key)

    def space(self, h):
        self.y -= h * self.s

    def add(self, widget):
        self.widgets.append(widget)
        return widget

    def label(self, text, dim=True):
        return self.add(Label(self._id("label", len(self.widgets)), self._take(self.row * 0.8), text, dim))

    def heading(self, text):
        self.space(4)
        return self.add(Heading(self._id("heading", text), self._take(self.row * 0.85), text))

    def slider(self, owner, name, label=None):
        prop = Prop(owner, name, label)
        return self.add(Slider(self._id("slider", name), self._take(self.row), prop))

    def dropdown(self, prop, key, show_label=True):
        return self.add(Dropdown(self._id("dropdown", key), self._take(self.row), prop, show_label))

    def check(self, owner, name, label=None):
        return self.add(Check(self._id("check", name), self._take(self.row), Prop(owner, name, label)))

    def tabs(self, owner, name):
        return self.add(Tabs(self._id("tabs", name), self._take(self.row + 4 * self.s), Prop(owner, name)))

    def button(self, text, action, icon=None, primary=False, key=None, tooltip=""):
        return self.add(Button(self._id("button", key or text), self._take(self.row), text, action, icon, primary,
                               tooltip))

    def tiles(self, items, tile_w=56, tile_h=54):
        """Tool tiles: items of (id, icon, label, active, action, tooltip); as many columns as fit
        (at most 4), the row's width split evenly between them."""
        s, gap = self.s, self.gap
        cols = max(1, min(4, int((self.w + gap) // (tile_w * s + gap))))
        w = (self.w - gap * (cols - 1)) / cols
        h = tile_h * s
        for i, (ident, icon, label, active, action, tooltip) in enumerate(items):
            c, rrow = i % cols, i // cols
            r = (self.x + c * (w + gap), self.y - (rrow + 1) * h - rrow * gap, w, h)
            self.add(Tile(self._id("tile", ident), r, label, action, icon, active, tooltip))
        rows = (len(items) + cols - 1) // cols
        self.y -= rows * h + max(0, rows - 1) * gap + gap


# --- Window ------------------------------------------------------------------------------------

class WindowState:
    """What a window keeps between draws: whether it shows and in which 3D view, where (its offset
    from the region's top right), its scroll, and what the pointer is over."""

    def __init__(self):
        self.shown = False
        self.area = 0       # the 3D view's area (as_pointer) it shows in
        self.right = 0.0    # unscaled pixels from the region's right edge (past the sidebar)
        self.top = 0.0      # and from its top
        self.scroll = 0.0   # unscaled pixels the content is scrolled up
        self.hover = None
        self.hover_since = 0.0  # when the pointer came over what it is over (tooltips wait a moment)
        self.mouse = (-1, -1)
        self.interaction = None
        self.folds = {}     # folding sections open (True) or folded, by id
        self.lists = {}     # lists' scroll (first row shown), by id


class Window:
    """One frame of a window: its rect in `region`, its widgets laid out by content(layout, context)."""

    width = 320

    def __init__(self, state, context, region, title, icon, content, theme=None):
        self.state, self.theme = state, theme or _theme()
        self.scale = context.preferences.system.ui_scale
        self.interaction = state.interaction
        self.hover, self.mouse = state.hover, state.mouse
        self.region_w = region.width
        s = self.scale
        w = self.width * s
        right = region.width - _sidebar_width(context, region) - _gizmo_width(context, s) - (state.right + 12) * s
        self.title_h = 34 * s
        top = region.height - _header_height(context, region) - (state.top + 8) * s
        top = max(top, self.title_h + 4 * s)  # the title bar stays in the view, to drag it back
        x = max(4 * s, right - w)
        pad = 12 * s
        layout = Layout(self, x + pad, top - self.title_h + state.scroll * s, w - 2 * pad)
        content(layout, context)
        content_h = (top - self.title_h + state.scroll * s) - layout.y + pad
        max_h = max(self.title_h + 60 * s, top - 8 * s)
        h = min(self.title_h + content_h, max_h)
        self.rect = (x, top - h, w, h)
        self.body = (x, top - h, w, h - self.title_h)
        self.overflow = max(0.0, (self.title_h + content_h - h) / s)
        self.widgets = [TitleBar("title", (x, top - self.title_h, w - 34 * s, self.title_h), title, icon),
                        Glyph("close", (x + w - 30 * s, top - self.title_h + 6 * s, 24 * s, 22 * s), "close",
                              lambda ctx: close(state))]
        self.widgets += [wd for wd in layout.widgets if wd.rect[1] + wd.rect[3] > self.body[1] and wd.rect[1] < top - self.title_h]

    def widget_at(self, x, y):
        if not contains(self.rect, x, y):
            return None
        for wd in reversed(self.widgets):
            if getattr(wd, "passive", False):
                continue
            if contains(wd.rect, x, y) and (wd.id in ("title", "close") or contains(self.body, x, y)):
                return wd
        return None

    def draw(self):
        t, s = self.theme, self.scale
        p = Painter(t, s)
        x, y, w, h = self.rect
        p.rect(inset(self.rect, -3 * s), (0, 0, 0, 0.35), 10 * s, soft=14 * s)
        p.rect(self.rect, t.panel, (t.radius + 2) * s, t.border, 1.0 * s)
        p.rect((x + 1 * s, y + h - self.title_h, w - 2 * s, max(1.0, s)), t.border)
        bx, by, bw, bh = self.body
        gpu.state.scissor_test_set(True)
        try:
            for wd in self.widgets:
                if wd.id in ("title", "close"):
                    gpu.state.scissor_set(int(x), int(y), int(w), int(h))
                else:
                    gpu.state.scissor_set(int(bx), int(by + 2 * s), int(bw), int(bh - 3 * s))
                p.fade = 0.4 if wd.disabled or wd.dim else 1.0
                wd.draw(p, self)
            p.fade = 1.0
        finally:
            gpu.state.scissor_test_set(False)
        if self.overflow > 0:
            track_h = bh - 12 * s
            frac = bh / (bh + self.overflow * s)
            pos = self.state.scroll / self.overflow if self.overflow else 0.0
            th = max(20 * s, track_h * frac)
            ty = by + 6 * s + (track_h - th) * (1.0 - pos)
            p.rect((x + w - 6 * s, ty, 3 * s, th), with_alpha(t.textMuted, 0.6), 1.5 * s)
        if self.interaction is not None:
            self.interaction.draw(p, self)
        elif self.hover and self.state.hover_since and time.time() - self.state.hover_since > TOOLTIP_DELAY:
            hovered = next((wd for wd in self.widgets if wd.id == self.hover), None)
            text = hovered.tooltip_text() if hovered is not None else ""
            if text:
                draw_tooltip(p, self, text)


def _sidebar_width(context, region):
    """How much of the region's right side the sidebar covers (region overlap)."""
    if not context.preferences.system.use_region_overlap:
        return 0
    for r in context.area.regions:
        if r.type == "UI" and r.width > 1 and r.alignment == "RIGHT":
            return r.width
    return 0


def _header_height(context, region):
    """How much of the region's top the header and tool settings cover (region overlap)."""
    top = region.y + region.height
    covered = 0
    for r in context.area.regions:
        if r.type in ("HEADER", "TOOL_HEADER") and r.height > 1 and region.y + region.height * 0.5 < r.y < top:
            covered = max(covered, top - r.y)
    return covered


def _gizmo_width(context, s):
    """Room for the 3D view's navigation gizmo column on the right, when it shows."""
    space = context.space_data
    if space is not None and getattr(space, "show_gizmo", False) and getattr(space, "show_gizmo_navigate", False):
        return 64 * s
    return 0


_theme_cache = [None]


def _theme():
    if _theme_cache[0] is None:
        _theme_cache[0] = Theme()
    return _theme_cache[0]


def close(state):
    state.shown = False
    state.interaction = None
    state.hover = None
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == "VIEW_3D":
                area.tag_redraw()
