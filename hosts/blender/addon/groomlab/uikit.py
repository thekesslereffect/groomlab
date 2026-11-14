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


def _srgb(value, palette):
    """A theme color ("#RRGGBB" or a palette name) as sRGB (0-1), or None."""
    value = palette.get(value, value)
    if not isinstance(value, str) or not value.startswith("#") or len(value) < 7:
        return None
    h = value.lstrip("#")
    return tuple(int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4))


def _mix(a, b, t):
    """mix(a, b, t): a toward b by t, in sRGB (as the app's theme.cpp mixes)."""
    return tuple(x + (y - x) * t for x, y in zip(a, b))


def _linear_rgba(c, alpha=1.0):
    return (linear(c[0]), linear(c[1]), linear(c[2]), alpha)


class Theme:
    """The theme file's ui colors (as RGBA, linear) and sizes (the default density, unscaled), with
    the tokens a theme may leave out made by its "derived" formulas (the app's theme.cpp: card,
    sunken, accentSoft, accentInk, cardRule, accentEdge, segmentOn...) and the components' sizes and
    roles (Charcoal's "components" block: every theme draws the components as it says)."""

    def __init__(self, path=None, data=None):
        data = data if data is not None else (self._read(path) if path else theme_data())
        components = data.get("components")
        if not isinstance(components, dict):  # (Charcoal's block, whatever the theme)
            components = self._read(THEME_FILE).get("components") or {}
        self.components = components
        ui, palette = data.get("ui", {}), data.get("palette", {})
        fallback = {"background": "#141414", "panel": "#1C1C1C", "raised": "#262626", "hover": "#303030",
                    "border": "#2A2A2A", "text": "#EDEDED", "textDim": "#A3A3A3",
                    "accent": "#5B8DEF", "accentText": "#FFFFFF", "selection": "#35435E", "warning": "#D4B26A",
                    "error": "#D9736C"}
        c = {}
        for key, default in fallback.items():
            c[key] = _srgb(ui.get(key, default), palette) or _srgb(default, palette)
        own = lambda key: _srgb(ui.get(key), palette) if ui.get(key) else None  # noqa: E731
        p = c["panel"]
        self.light = 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2] > 0.5  # derived.about
        text, dim = c["text"], c["textDim"]
        # The app's text steps (theme.cpp text(), textSoft(), textDim(): ui.textDim is the far end).
        c["textSoft"] = _mix(text, dim, 0.33)
        c["textDim"] = _mix(text, dim, 0.66)
        c["textMuted"] = own("textMuted") or dim
        c["textDisabled"] = own("textDisabled") or c["textMuted"]
        c["card"] = own("card") or _mix(c["panel"], c["raised"], 0.45)
        c["sunken"] = own("sunken") or (_mix(c["panel"], text, 0.07) if self.light
                                        else _mix(c["background"], c["panel"], 0.3))
        c["accentSoft"] = own("accentSoft") or _mix(c["card"], c["accent"], 0.16 if self.light else 0.22)
        c["accentInk"] = own("accentInk") or _mix(c["accent"], text, 0.45)
        c["accentEdge"] = _mix(c["accentSoft"], c["accent"], 0.45)
        c["accentFill"] = own("accentFill") or c["accent"]
        c["cardRule"] = own("cardRule") or _mix(c["card"], c["hover"], 0.6)
        c["segmentOn"] = own("segmentOn") or ((1.0, 1.0, 1.0) if self.light else _mix(c["hover"], text, 0.08))
        c["outline"] = own("outline") or c["border"]
        c["selectionText"] = own("selectionText") or c["accentText"]
        sliders = ui.get("sliders")
        one = _srgb(sliders[0], dict(palette, accent=ui.get("accent", "#5B8DEF"))) \
            if isinstance(sliders, list) and len(sliders) == 1 and isinstance(sliders[0], str) else None
        c["sliderFill"] = own("sliderFill") or one or c["selection"]
        c["checkEdge"] = _mix(c["hover"], c["textMuted"], 0.8)  # (the app's @checkEdge@)
        c["popupEdge"] = own("popupEdge") or _mix(c["raised"], text, 0.18)
        c["swatchEdge"] = own("checkEdge" if self.light else "swatchEdge") or _mix(c["raised"], text,
                                                                                  0.45 if self.light else 0.22)
        c["scroll"] = _mix(c["hover"], text, 0.25)
        c["scrollHover"] = _mix(c["hover"], text, 0.45)
        icons = data.get("icons", {})
        c["toolIcon"] = _srgb(icons.get("shape", ""), palette) or c["textDim"]  # a solid theme's tiles: one tone
        self._srgb = c
        for key, value in c.items():
            setattr(self, key, _linear_rgba(value))
        self.track = self.hover  # (a switch's off track)
        self.radius = float(ui.get("radius", 6))
        self.card_radius = float(ui.get("cardRadius", self.radius + 2))
        self.check_radius = float(ui.get("checkRadius", max(2.0, self.radius - 2)))
        self.font_size = float(ui.get("fontSize", 12))
        self.small_size = self.font_size - 1
        self.tiny_size = max(11.0, self.font_size - 2)
        self.row = float(ui.get("rowHeight", 26))

    @staticmethod
    def _read(path):
        try:
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError):
            data = {}
        return data if isinstance(data, dict) else {}

    def size(self, component, key, fallback, index=None):
        """A component's size (components.<component>.<key>; index: one of a list's)."""
        v = (self.components.get(component) or {}).get(key)
        if index is not None and isinstance(v, list):
            v = v[index] if index < len(v) else None
        return float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else float(fallback)

    def role(self, component, key, fallback):
        """A component's color: the token it names (components.<component>.<key>), else fallback's."""
        name = (self.components.get(component) or {}).get(key)
        if isinstance(name, str):
            if name.startswith("#"):
                value = _srgb(name, {})
                if value is not None:
                    return _linear_rgba(value)
            elif name in self._srgb:
                return getattr(self, name)
        return getattr(self, fallback) if isinstance(fallback, str) else fallback


def with_alpha(color, alpha):
    return (color[0], color[1], color[2], alpha)


# --- Drawing -----------------------------------------------------------------------------------

_RECT_VERT = """
void main() {
    float pad = 1.0 + shape.z;
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
    coverage *= clamp(shape.w - pos.x + 0.5, 0.0, 1.0);  /* clipped at x = shape.w (a slider's fill) */
    FragColor = vec4(c.rgb, c.a * coverage);
}
"""
# A rounded rect filled with a vertical gradient (bottom color at band.x, top color at band.y) and
# kept to that band (a node's header: the card's shape, its top part only).
_BAND_FRAG = """
float roundedBox(vec2 p, vec2 half_size, float r) {
    vec2 q = abs(p) - half_size + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}
void main() {
    float r = shape.x, soft = max(shape.z, 1.0);
    vec2 half_size = rect.zw * 0.5;
    float d = roundedBox(pos - rect.xy - half_size, half_size, min(r, min(half_size.x, half_size.y)));
    float coverage = clamp(0.5 - d / soft, 0.0, 1.0);
    coverage *= clamp(pos.y - band.x + 0.5, 0.0, 1.0) * clamp(band.y - pos.y + 0.5, 0.0, 1.0);
    float t = clamp((pos.y - band.x) / max(band.y - band.x, 1.0), 0.0, 1.0);
    vec4 c = mix(fill, stroke, t);
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
        info.push_constant("VEC4", "shape")  # radius, border width, edge softness, the x it is clipped at
        info.vertex_in(0, "VEC2", "position")
        info.vertex_out(iface)
        info.fragment_out(0, "VEC4", "FragColor")
        info.vertex_source(_RECT_VERT)
        info.fragment_source(_RECT_FRAG)
        shader = gpu.shader.create_from_info(info)
        quad = batch_for_shader(shader, "TRI_FAN", {"position": ((0, 0), (1, 0), (1, 1), (0, 1))})
        _shaders["rect"] = (shader, quad)
    return _shaders["rect"]


def _band_shader():
    if "band" not in _shaders:
        iface = gpu.types.GPUStageInterfaceInfo("groomlab_ui_band")
        iface.smooth("VEC2", "pos")
        info = gpu.types.GPUShaderCreateInfo()
        info.push_constant("MAT4", "ModelViewProjectionMatrix")
        info.push_constant("VEC4", "rect")
        info.push_constant("VEC4", "fill")    # the band's bottom color
        info.push_constant("VEC4", "stroke")  # its top color
        info.push_constant("VEC4", "shape")   # radius, -, edge softness, -
        info.push_constant("VEC4", "band")    # x: the band's bottom y, y: its top y
        info.vertex_in(0, "VEC2", "position")
        info.vertex_out(iface)
        info.fragment_out(0, "VEC4", "FragColor")
        info.vertex_source(_RECT_VERT)
        info.fragment_source(_BAND_FRAG)
        shader = gpu.shader.create_from_info(info)
        quad = batch_for_shader(shader, "TRI_FAN", {"position": ((0, 0), (1, 0), (1, 1), (0, 1))})
        _shaders["band"] = (shader, quad)
    return _shaders["band"]


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

    def rect(self, r, fill, radius=0.0, stroke=None, border=0.0, soft=0.0, clip_x=None):
        """A rounded rect; clip_x: only its part left of that x (the shape's corners kept)."""
        shader, quad = _rect_shader()
        gpu.state.blend_set("ALPHA")  # blf leaves blending off
        shader.bind()
        shader.uniform_float("ModelViewProjectionMatrix", self.mvp)
        shader.uniform_float("rect", r)
        shader.uniform_float("fill", self._faded(fill))
        shader.uniform_float("stroke", self._faded(stroke or fill))
        shader.uniform_float("shape", (radius, border if stroke else 0.0, soft, 1e9 if clip_x is None else clip_x))
        quad.draw(shader)

    def band(self, r, bottom, top, radius, y0, y1):
        """The part of rounded rect r between y0 and y1, filled from `bottom` (at y0) to `top` (at y1):
        a card's header, its top corners rounded and its bottom edge straight."""
        shader, quad = _band_shader()
        gpu.state.blend_set("ALPHA")
        shader.bind()
        shader.uniform_float("ModelViewProjectionMatrix", self.mvp)
        shader.uniform_float("rect", r)
        shader.uniform_float("fill", self._faded(bottom))
        shader.uniform_float("stroke", self._faded(top))
        shader.uniform_float("shape", (radius, 0.0, 0.0, 0.0))
        shader.uniform_float("band", (y0, y1, 0.0, 0.0))
        quad.draw(shader)

    def dots(self, points, radius, color):
        """Small squares (a canvas' dot grid) centered on `points`, in one batch."""
        if not points:
            return
        verts, tris = [], []
        for x, y in points:
            i = len(verts)
            verts += [(x - radius, y - radius), (x + radius, y - radius), (x + radius, y + radius), (x - radius, y + radius)]
            tris += [(i, i + 1, i + 2), (i, i + 2, i + 3)]
        shader = gpu.shader.from_builtin("UNIFORM_COLOR")
        batch = batch_for_shader(shader, "TRIS", {"pos": verts}, indices=tris)
        gpu.state.blend_set("ALPHA")
        shader.bind()
        shader.uniform_float("color", self._faded(color))
        batch.draw(shader)

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

    def glyph(self, name, r, color, mono=False):
        """An icon: the window's mono glyphs (ui_*) in `color`, the tools' own in theirs (mono: in
        `color` too, as a solid theme's tiles draw them: one quiet tone)."""
        self.icon(name, r, tint=color if mono or name.startswith("ui_") else None)

    def chevron(self, cx, cy, k, color, down=True, width=1.5):
        """A small chevron (a menu's, a fold's) centered at (cx, cy), k pixels from the middle."""
        if down:
            self.lines([(cx - k, cy + k * 0.5), (cx, cy - k * 0.5), (cx + k, cy + k * 0.5)], color, width)
        else:
            self.lines([(cx - k * 0.5, cy + k), (cx + k * 0.5, cy), (cx - k * 0.5, cy - k)], color, width)

    def lines(self, points, color, width=1.5):
        """A polyline through `points` (pairs), `width` unscaled pixels wide."""
        shader = gpu.shader.from_builtin("POLYLINE_UNIFORM_COLOR")
        batch = batch_for_shader(shader, "LINE_STRIP", {"pos": [(x, y, 0.0) for x, y in points]})
        shader.bind()
        shader.uniform_float("viewportSize", gpu.state.viewport_get()[2:])
        shader.uniform_float("lineWidth", width * self.scale)
        shader.uniform_float("color", self._faded(color))
        batch.draw(shader)

    def text_size(self, text, size=None, bold=False):
        fid = font()
        blf.size(fid, (size or self.theme.font_size) * self.scale)
        return blf.dimensions(fid, text)[0] + (self._embolden() if bold else 0.0)

    def _embolden(self):
        """How far a bold text's second pass is drawn to the right (blf has one weight: Inter's regular)."""
        return max(0.5, 0.5 * self.scale)

    def text(self, text, r, color, size=None, align="left", clip=True, bold=False):
        """`text` in rect r (x, y, w, h), centered vertically; cut with an ellipsis to fit. bold: the
        semibold titles of the app (drawn twice, a fraction of a pixel apart)."""
        fid = font()
        px = (size or self.theme.font_size) * self.scale
        blf.size(fid, px)
        extra = self._embolden() if bold else 0.0
        width = blf.dimensions(fid, text)[0] + extra
        if clip and width > r[2] > 0:
            while text and blf.dimensions(fid, text + "…")[0] + extra > r[2]:
                text = text[:-1]
            text += "…"
            width = blf.dimensions(fid, text)[0] + extra
        x = r[0] + (r[2] - width) * {"left": 0.0, "center": 0.5, "right": 1.0}[align]
        y = r[1] + r[3] * 0.5 - px * CAP_HEIGHT * 0.5
        blf.color(fid, *self._faded(color))
        blf.position(fid, round(x), round(y), 0)
        blf.draw(fid, text)
        if bold:
            blf.position(fid, round(x) + extra, round(y), 0)
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

    def __init__(self, label, items, get, set_, tooltip="", placeholder=""):
        self.label, self._items, self._get, self._set, self.tooltip = label, items, get, set_, tooltip
        self.placeholder = placeholder  # what shows while none is chosen

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
    own_disabled = False  # draws its disabled look itself (the window fades the others)
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
    """Text, in one of the app's roles (style):
    "body" the dim text of a row; "bright" a list row's name; "title" a card's or a list's title
    (text, semibold); "about" a card's line of text (dim); "tray" a tray's or a block's small title (dim, small, semibold); "hint" a
    muted small line; "count" a title's count (muted, small); "tool" the picked tool's name with
    its icon (semibold, the icon in the accent's ink); "emptyTitle" / "emptyBody" an empty state's
    title and line, centered; "status" a warning line. A label in capitals reads as a small
    muted heading (the sidebar's)."""

    passive = True  # text: a press or a hover goes to what is under it (a list's row)

    def __init__(self, wid, rect, text, dim=True, size=None, align="left", icon=None, bright=False, style=None):
        super().__init__(wid, rect)
        self.text, self.size, self.align, self.icon = text, size, align, icon
        self.style = style or ("bright" if bright or not dim else "body")

    def look(self, t):
        """(color, size, bold) of this label's role."""
        caps = len(self.text) > 2 and self.text.upper() == self.text and any(c.isalpha() for c in self.text)
        style = self.style
        if self.alert:
            return t.error, None, False
        if style == "title":
            return t.text, t.font_size, True
        if style == "tray":
            return t.textDim, t.small_size, True
        if style == "hint":
            return t.textMuted, t.small_size, False
        if style == "about":
            return t.textDim, None, False
        if style == "count":
            return t.textMuted, t.small_size, False
        if style == "tool":
            return t.text, t.font_size + 1, True
        if style == "emptyTitle":
            return t.text, t.font_size + 1, True
        if style == "emptyBody":
            return t.textDim, t.small_size, False
        if style == "status":
            return t.warning, None, False
        if caps:
            return t.textMuted, t.small_size, False
        return (t.text if style == "bright" else t.textDim), None, False

    def draw(self, p, win):
        t, s = p.theme, p.scale
        color, size, bold = self.look(t)
        size = self.size or size
        x, y, w, h = self.rect
        if self.icon:
            g = (18 if self.style == "tool" else 16) * s
            gap = (8 if self.style == "tool" else 6) * s
            block = g + (gap + p.text_size(self.text, size, bold) if self.text else 0.0)
            gx = x + (w - block) * {"left": 0.0, "center": 0.5, "right": 1.0}[self.align]
            gx = max(x, gx)
            if self.style == "tool":
                p.glyph(self.icon, (gx, y + (h - g) * 0.5, g, g), t.accentInk, mono=True)
            else:
                p.glyph(self.icon, (gx, y + (h - g) * 0.5, g, g), color)
            if self.text:
                left = gx + g + gap
                p.text(self.text, (left, y, x + w - left, h), color, size, bold=bold)
            return
        p.text(self.text, self.rect, color, size, self.align, bold=bold)


class EmptyIcon(Widget):
    """An empty state's disc: the soft accent, the glyph in the accent's ink (components.emptyState)."""

    passive = True

    def __init__(self, wid, rect, icon):
        super().__init__(wid, rect)
        self.icon = icon

    def draw(self, p, win):
        t, s = p.theme, p.scale
        d = t.size("emptyState", "disc", 40) * s
        x, y, w, h = self.rect
        cx, cy = x + w * 0.5, y + h * 0.5
        p.rect((cx - d * 0.5, cy - d * 0.5, d, d), t.role("emptyState", "fill", "accentSoft"), d * 0.5)
        g = d * 0.5
        if self.icon:
            p.glyph(self.icon, (cx - g * 0.5, cy - g * 0.5, g, g), t.role("emptyState", "icon", "accentInk"), mono=True)


class Button(Widget):
    """An operator's button, in the app's kinds: a field button (the raised tone), primary (the
    accent's fill), flat (a ghost: no fill until hovered), a glyph (an icon alone: no fill; pressed,
    the selection), a menu (outlined, a small chevron after its name: it opens something), depress
    (on: the soft accent, as a chip that is on)."""

    own_disabled = True

    def __init__(self, wid, rect, text, action, icon=None, primary=False, tooltip="", flat=False, depress=False,
                 align="center", menu=False):
        super().__init__(wid, rect)
        self.text, self.action, self.icon, self.primary, self.tooltip = text, action, icon, primary, tooltip
        self.flat, self.depress, self.align, self.menu = flat, depress, align, menu
        self.small = False  # a chip's smaller text (the toggles and segments)

    def on(self):
        return self.depress

    def colors(self, t, hover, on):
        """(fill or None, edge or None, text color, bold) for this button's kind and state."""
        glyph = self.icon and not self.text
        if self.primary:
            return (t.accent if hover else t.accentFill), None, t.accentText, True
        if glyph:
            fill = t.selection if on else t.hover if hover else None
            return fill, None, (t.selectionText if on else t.text if hover else t.textDim), False
        if on:
            return t.role("chip", "on", "accentSoft"), None, t.role("chip", "onText", "accentInk"), True
        if self.menu:
            return (t.raised if hover else None), t.role("disclosure", "edge", "hover"),                 (t.text if hover else t.role("disclosure", "text", "textDim")), False
        if self.flat:
            return (t.hover if hover else None), None, (t.text if hover else t.textSoft), False
        return (t.hover if hover else t.role("chip", "fill", "raised")), None, (t.text if hover else t.textSoft), False

    def draw(self, p, win):
        t, s = p.theme, p.scale
        hover = win.hover == self.id and not self.disabled
        on = self.on()
        fill, edge, color, bold = self.colors(t, hover, on)
        if self.alert:
            color = t.error
        if self.disabled:
            color = t.textDisabled
            if self.primary:  # (the app's disabled primary: the field's tone)
                fill = t.raised
        if fill is not None or edge is not None:
            p.rect(self.rect, fill or (0, 0, 0, 0), t.radius * s, edge, 1.0 * s)
        x, y, w, h = self.rect
        size = t.small_size if self.small else None
        tw = p.text_size(self.text, size, bold) if self.text else 0.0
        if self.icon:
            g = 16 * s
            block = g + (6 * s + tw if self.text else 0.0)
            ix = x + 8 * s if self.align == "left" and self.text else x + (w - block) * 0.5
            ix = max(x + 2 * s, ix)
            ink = t.accentInk if on and not glyph_only(self) else color
            p.glyph(self.icon, (ix, y + (h - g) * 0.5, g, g), ink, mono=True)  # (the app's glyphs: one tone)
            if self.text:
                left = ix + g + 6 * s
                p.text(self.text, (left, y, x + w - left - 4 * s, h), color, size, bold=bold)
            return
        chevron = 10 * s if self.menu else 0.0
        pad = min(8 * s, max(2 * s, (w - tw - chevron) * 0.5))
        r = (x + pad, y, w - 2 * pad - chevron, h)
        p.text(self.text, r, color, size, align=self.align, bold=bold)
        if self.menu:
            shown = min(tw, r[2])
            right = r[0] + (shown if self.align == "left" else (r[2] + shown) * 0.5)
            p.chevron(right + 7 * s, y + h * 0.5, 2.5 * s, color, width=1.3)

    def press(self, win, context, event):
        return ClickInteraction(self)


def glyph_only(button):
    return bool(button.icon) and not button.text


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
        color = t.text if win.hover == self.id else t.textMuted
        if self.text == "close":
            p.lines([(cx - k, cy - k), (cx + k, cy + k)], color)
            p.lines([(cx - k, cy + k), (cx + k, cy - k)], color)
        elif self.text == "chevron_down":
            p.chevron(cx, cy, k, color)
        elif self.text == "chevron_right":
            p.chevron(cx, cy, k, color, down=False)


class Tile(Button):
    """A tool's tile, flat (a tray's): its icon over its name, in one quiet tone; hovered, the
    hover; the picked one on the soft accent with its edge, the icon in the accent's ink."""

    def __init__(self, wid, rect, text, action, icon, active, tooltip=""):
        super().__init__(wid, rect, text, action, icon, tooltip=tooltip)
        self.active = active

    def draw(self, p, win):
        t, s = p.theme, p.scale
        hover = win.hover == self.id and not self.disabled
        if self.active:
            p.rect(self.rect, t.accentSoft, t.radius * s, t.accentEdge, 1.0 * s)
        elif hover:
            p.rect(self.rect, t.hover, t.radius * s)
        x, y, w, h = self.rect
        g = 24 * s
        ink = t.textMuted if self.disabled else t.accentInk if self.active else t.toolIcon
        p.glyph(self.icon, (x + (w - g) * 0.5, y + h - g - 7 * s, g, g), ink, mono=True)
        color = t.textDisabled if self.disabled else t.text if self.active or hover else t.textDim
        p.text(self.text, (x + 2 * s, y + 4 * s, w - 4 * s, 15 * s), color, t.tiny_size, "center")


class RailButton(Widget):
    """A page's icon on the window's rail (components.iconRail): muted at rest, the text's tone
    hovered, the accent's ink on the soft accent while its page is open. A press opens the page
    (in Blender the rail only switches pages: folding is the app's)."""

    cursor = "HAND"

    def __init__(self, wid, rect, ident, label, tip, icon, get, set_):
        super().__init__(wid, rect)
        self.ident, self.label, self.icon, self.get, self.set = ident, label, icon, get, set_
        self.tooltip = label + ("\n" + tip if tip else "")  # (the page's name, its line under it)

    def draw(self, p, win):
        t, s = p.theme, p.scale
        on = self.get() == self.ident
        hover = win.hover == self.id
        radius = t.size("iconRail", "radius", 8) * s
        if on:
            p.rect(self.rect, t.role("iconRail", "on", "accentSoft"), radius)
        elif hover:
            p.rect(self.rect, t.role("iconRail", "hover", "hover"), radius)
        ink = t.role("iconRail", "inkOn", "accentInk") if on else             t.role("iconRail", "inkHover", "text") if hover else t.role("iconRail", "ink", "textMuted")
        g = t.size("iconRail", "icon", 20) * s
        x, y, w, h = self.rect
        p.glyph(self.icon, (x + (w - g) * 0.5, y + (h - g) * 0.5, g, g), ink, mono=True)

    def press(self, win, context, event):
        if self.get() != self.ident:
            self.set(self.ident)
            win.state.scroll = 0.0  # a page opens at its top
        return None


class Slider(Widget):
    """A number as a fill field: its name inside on the left, its value on the right, filled by the
    value's place in the soft range. Drag scrubs (Shift: 1/10, Ctrl: steps of 10x); a click types."""

    own_disabled = True

    cursor = "SCROLL_X"

    def __init__(self, wid, rect, prop):
        super().__init__(wid, rect)
        self.prop, self.tooltip = prop, prop.tooltip

    def draw(self, p, win):
        t, s = p.theme, p.scale
        hover = win.hover == self.id and not self.disabled
        r = self.rect
        radius = t.size("field", "radius", t.radius) * s
        editing = isinstance(win.interaction, TypingInteraction) and win.interaction.widget.id == self.id
        if editing:
            win.interaction.draw_field(p, win)
            return
        p.rect(r, t.hover if hover else t.role("field", "fill", "raised"), radius)
        lo, hi = self.prop.span()
        value = float(self.prop.get())
        frac = min(max((value - lo) / (hi - lo), 0.0), 1.0)
        filled = round(r[2] * frac)
        if filled > 0.0 and not self.disabled:
            # The fill clipped to the field's corners, the value as a 2 px bar along its bottom
            # (inset from the rounded ends): the app's ValueSlider.
            right = r[0] + filled
            p.rect(r, t.role("slider", "fill", "sliderFill"), radius, clip_x=right)
            bar = t.role("slider", "bar", "accent")
            if hover:
                bar = tuple(min(1.0, c * 1.25) for c in bar[:3]) + (1.0,)
            inset_ = min(radius, 4 * s) + 1 * s
            bar_h = t.size("slider", "barHeight", 2) * s
            if filled > inset_:
                p.rect((r[0] + inset_, r[1], min(right, r[0] + r[2] - inset_) - r[0] - inset_, bar_h), bar)
        pad = 9 * s
        value_text = self.prop.text()
        vw = p.text_size(value_text)
        label_color = t.textDisabled if self.disabled else t.textDim
        value_color = t.textDisabled if self.disabled else t.text
        if self.prop.label:
            p.text(self.prop.label, (r[0] + pad, r[1], r[2] - vw - 3 * pad, r[3]), label_color)
        else:  # no name: the value alone, centered
            p.text(value_text, r, value_color, align="center", clip=False)
            return
        p.text(value_text, (r[0], r[1], r[2] - pad, r[3]), value_color, align="right", clip=False)

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
        p.rect(r, t.background, t.size("field", "radius", t.radius) * s, t.role("field", "focus", "accent"), 2.0 * s)
        pad = 9 * s
        inner = (r[0] + pad, r[1], r[2] - 2 * pad, r[3])
        if self.all and self.text:
            tw = p.text_size(self.text)
            p.rect((inner[0] - 2 * s, r[1] + 5 * s, tw + 4 * s, r[3] - 10 * s), t.accentSoft, 2 * s)
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

    own_disabled = True

    def __init__(self, wid, rect, prop, show_label=True):
        super().__init__(wid, rect)
        self.prop, self.show_label, self.tooltip = prop, show_label, prop.tooltip

    def current_name(self):
        current = self.prop.get()
        for ident, name, _ in self.prop.items():
            if ident == current:
                return name
        return None  # (none: the choice's placeholder)

    def draw(self, p, win):
        t, s = p.theme, p.scale
        open_ = isinstance(win.interaction, MenuInteraction) and win.interaction.widget.id == self.id
        hover = (win.hover == self.id and not self.disabled) or open_
        r = self.rect
        p.rect(r, t.hover if hover else t.role("field", "fill", "raised"), t.size("field", "radius", t.radius) * s,
               t.role("field", "focus", "accent") if open_ else None, 1.0 * s)
        pad = 9 * s
        name = self.current_name()
        value = t.textDisabled if self.disabled else t.text
        if name is None:
            name, value = getattr(self.prop, "placeholder", "") or "-", t.textMuted
        if self.show_label and self.prop.label:
            # A field row: the name in the dim tone, the value after the label column (components.field).
            col = min(t.size("field", "labelWidth", 104) * s, r[2] * 0.45)
            p.text(self.prop.label, (r[0] + pad, r[1], col - pad - 4 * s, r[3]),
                   t.textDisabled if self.disabled else t.textDim)
            p.text(name, (r[0] + col, r[1], r[2] - col - pad - 14 * s, r[3]), value)
        else:
            p.text(name, (r[0] + pad, r[1], r[2] - 2 * pad - 14 * s, r[3]), value)
        p.chevron(r[0] + r[2] - pad - 5 * s, r[1] + r[3] * 0.5, 4.0 * s, t.text if hover else t.textDim, width=1.8)

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
        draw_popup(p, self.rect)
        x, y, w, h = self.rect
        current = self.widget.prop.get()
        for i, (ident, name, _) in enumerate(self.items):
            r = (x + 4 * s, y + h - self.pad - (i + 1) * self.row, w - 8 * s, self.row)
            if ident == current:
                p.rect(r, t.role("listBand", "selected", "accentSoft"), (t.radius - 2) * s)
            elif i == self.hover:
                p.rect(r, t.hover, (t.radius - 2) * s)
            p.text(name, inset(r, 8 * s, 0), t.accentInk if ident == current else t.text if i == self.hover else t.textDim,
                   bold=ident == current)


class Check(Widget):
    """A boolean as a field row with a switch at its right (the app's switch rows, components.switchRow
    and components.switch): its name in the dim tone; the switch a rounded track, its knob at the
    left (off) or, on the accent's fill, at the right (on)."""

    own_disabled = True

    def __init__(self, wid, rect, prop):
        super().__init__(wid, rect)
        self.prop, self.tooltip = prop, prop.tooltip

    def draw(self, p, win):
        t, s = p.theme, p.scale
        x, y, w, h = self.rect
        on = bool(self.prop.get())
        hover = win.hover == self.id and not self.disabled
        p.rect(self.rect, t.hover if hover else t.role("switchRow", "fill", "raised"),
               t.size("field", "radius", t.radius) * s)
        pad = 9 * s
        sw, sh = t.size("switch", "width", 28) * s, t.size("switch", "height", 16) * s
        track = (x + w - pad - sw, y + (h - sh) * 0.5, sw, sh)
        fill = t.role("switch", "trackOn", "accentFill") if on else t.role("switch", "track", "textDisabled")
        knob_color = t.role("switch", "knobOn" if on else "knob", "accentText")
        p.rect(track, fill, sh * 0.5)
        k = sh - 5 * s  # the knob's diameter: 2.5 px of track round it
        kx = track[0] + (sw - sh if on else 0.0) + 2.5 * s
        p.rect((kx, track[1] + 2.5 * s, k, k), knob_color, k * 0.5)
        p.text(self.prop.label, (x + pad, y, w - sw - 3 * pad, h), t.textDisabled if self.disabled else t.textDim)

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
        p.icon(self.icon, (x + 14 * s, y + (h - size) * 0.5, size, size))
        p.text(self.text, (x + 14 * s + size + 8 * s, y, w, h), t.text, t.font_size + 1, clip=False, bold=True)

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
        self.small = bool(text)  # a chip (components.chip)

    def on(self):
        return bool(self.prop.get())

    def press(self, win, context, event):
        self.prop.set(not bool(self.prop.get()))
        return None


class Segment(Button):
    """One choice of an enum shown expanded: on a sunken track (tracked: components.segTrack, the
    chosen one raised in a neutral tone), else a chip; pressed while it is the value."""

    def __init__(self, wid, rect, prop, ident, text, icon=None, tooltip="", tracked=False):
        super().__init__(wid, rect, text, None, icon, tooltip=tooltip or prop.tooltip)
        self.prop, self.ident, self.tracked = prop, ident, tracked
        self.small = True

    def colors(self, t, hover, on):
        if not self.tracked:
            return Button.colors(self, t, hover, on)
        if on:
            return t.role("segTrack", "on", "segmentOn"), None, t.text, True
        return None, None, (t.text if hover else t.textDim), False

    def draw(self, p, win):
        if not self.tracked:
            return Button.draw(self, p, win)
        t, s = p.theme, p.scale
        hover = win.hover == self.id and not self.disabled
        on = self.on()
        fill, _, color, bold = self.colors(t, hover, on)
        if self.disabled:
            color = t.textDisabled
        if fill is not None:
            p.rect(self.rect, fill, t.size("segTrack", "segmentRadius", 6) * s)
        x, y, w, h = self.rect
        if self.icon and not self.text:
            g = 16 * s
            p.glyph(self.icon, (x + (w - g) * 0.5, y + (h - g) * 0.5, g, g), color)
            return
        p.text(self.text, inset(self.rect, 2 * s, 0), color, t.small_size, "center", bold=bold)

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
        if editing:
            win.interaction.draw_field(p, win)
            return
        p.rect(r, t.hover if win.hover == self.id else t.role("field", "fill", "raised"),
               t.size("field", "radius", t.radius) * s)
        pad = 9 * s
        text = str(self.prop.get())
        if self.prop.label:
            lw = min(r[2] * 0.4, p.text_size(self.prop.label) + pad)
            p.text(self.prop.label, (r[0] + pad, r[1], lw, r[3]), t.textDim)
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
        if getattr(self, "dot", False):  # a list row's color
            p.rect(self.rect, color, self.rect[3] * 0.5)
            return
        p.rect(self.rect, color, t.check_radius * s, t.text if win.hover == self.id else t.swatchEdge, 1.0 * s)

    def press(self, win, context, event):
        if self.open_picker is not None:
            self.open_picker(context, self.prop)
        return None


class Backdrop(Widget):
    """What is drawn under rows, never pressed: "card" a section's card (components.card), "box" a
    box (a card of its own; inside a card, "rule": a rule over it, the app's item box), "track" a
    segmented choice's sunken track, "line" a hairline, "list" a list's ground (none: its rows
    on the card)."""

    passive = True

    def __init__(self, wid, rect, kind="box"):
        super().__init__(wid, rect)
        self.kind = kind

    def draw(self, p, win):
        t, s = p.theme, p.scale
        x, y, w, h = self.rect
        if self.kind in ("card", "box"):
            p.rect(self.rect, t.role("card", "fill", "card"), t.size("card", "radius", t.card_radius) * s)
        elif self.kind == "rule":
            p.rect((x, y + h - max(1.0, s), w, max(1.0, s)), t.cardRule)
        elif self.kind == "track":
            p.rect(self.rect, t.role("segTrack", "track", "sunken"), t.size("segTrack", "radius", 8) * s)
        elif self.kind == "line":
            p.rect((x, y + h * 0.5, w, max(1.0, s)), t.cardRule)


class ListRow(Widget):
    """A list's row: picked on a press (its item made the active one); the picked row in the soft
    accent, a hovered one a step up (components.listBand)."""

    def __init__(self, wid, rect, active, pick):
        super().__init__(wid, rect)
        self.active, self.pick = active, pick
        self.context_menu = None  # a right-click: (context) -> opens the row's menu

    def draw(self, p, win):
        t, s = p.theme, p.scale
        radius = max(2.0, t.radius - 2) * s
        if self.active:
            p.rect(self.rect, t.role("listBand", "selected", "accentSoft"), radius)
        elif win.hover == self.id:
            p.rect(self.rect, t.cardRule, radius)

    def press(self, win, context, event):
        self.pick(context)
        return None


class PanelHeader(Widget):
    """A folding section's header (Blender's layout.panel and sub-panels): a chevron and its title,
    semibold (the app's sections); a press opens or folds it."""

    def __init__(self, wid, rect, text, open_, toggle):
        super().__init__(wid, rect)
        self.text, self.open, self.toggle = text, open_, toggle

    def draw(self, p, win):
        t, s = p.theme, p.scale
        x, y, w, h = self.rect
        hover = win.hover == self.id
        cx, cy, k = x + 5 * s, y + h * 0.5, 3.0 * s
        p.chevron(cx, cy, k, t.accentInk if hover else t.textDim, down=self.open, width=1.4)
        p.text(self.text, (x + 16 * s, y, w - 16 * s, h), t.accentInk if hover else t.text, bold=True)

    def press(self, win, context, event):
        self.toggle()
        return None


def draw_popup(p, rect):
    """A menu's or a tooltip's ground: a soft shadow, the raised tone, a hairline edge."""
    t, s = p.theme, p.scale
    p.rect(inset(rect, -2 * s), (0, 0, 0, 0.35), 8 * s, soft=10 * s)
    p.rect(rect, t.raised, t.radius * s, t.popupEdge, 1.0 * s)


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
    draw_popup(p, (x, y, width, height))
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

    def slider(self, owner, name, label=None):
        prop = Prop(owner, name, label)
        return self.add(Slider(self._id("slider", name), self._take(self.row), prop))

    def dropdown(self, prop, key, show_label=True):
        return self.add(Dropdown(self._id("dropdown", key), self._take(self.row), prop, show_label))

    def check(self, owner, name, label=None):
        return self.add(Check(self._id("check", name), self._take(self.row), Prop(owner, name, label)))

    def button(self, text, action, icon=None, primary=False, key=None, tooltip=""):
        return self.add(Button(self._id("button", key or text), self._take(self.row), text, action, icon, primary,
                               tooltip))


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


class Rail:
    """A window's pages as icons in a row under its title bar (components.iconRail; a side column
    would widen the window): pages of (id, label, tip, icon), the open one get(), a press set(id)."""

    def __init__(self, pages, get, set_):
        self.pages, self.get, self.set = pages, get, set_


class Window:
    """One frame of a window: its rect in `region`, its widgets laid out by content(layout, context);
    with a rail, the pages' icons in a row under the title bar."""

    width = 300  # (the app's dock column)

    def __init__(self, state, context, region, title, icon, content, theme=None, rail=None):
        self.state, self.theme = state, theme or _theme()
        t = self.theme
        self.scale = context.preferences.system.ui_scale
        self.interaction = state.interaction
        self.hover, self.mouse = state.hover, state.mouse
        self.region_w = region.width
        s = self.scale
        self.rail_h = t.size("iconRail", "width", 44) * s if rail is not None else 0.0  # (its thickness)
        w = self.width * s
        right = region.width - _sidebar_width(context, region) - _gizmo_width(context, s) - (state.right + 12) * s
        self.title_h = 40 * s
        top = region.height - _header_height(context, region) - (state.top + 8) * s
        top = max(top, self.title_h + 4 * s)  # the title bar stays in the view, to drag it back
        x = max(4 * s, right - w)
        pad = 10 * s
        head = self.title_h + self.rail_h  # (the title bar and the rail: they don't scroll)
        # Under the rail, the content's own gap between its rows (an even step to the header row);
        # under a bare title bar, 2 px.
        lead = (t.size("card", "gap", 8) if rail is not None else 2) * s
        layout = Layout(self, x + pad, top - head - lead + state.scroll * s, w - 2 * pad)
        content(layout, context)
        content_h = (top - head - lead + state.scroll * s) - layout.y + pad
        max_h = max(head + 60 * s, top - 8 * s)
        h = min(head + content_h, max_h)
        self.rect = (x, top - h, w, h)
        self.body = (x, top - h, w, h - self.title_h)
        self.content = (x, top - h, w, h - head)  # (what scrolls)
        self.overflow = max(0.0, (head + content_h - h) / s)
        self.widgets = [TitleBar("title", (x, top - self.title_h, w - 40 * s, self.title_h), title, icon),
                        Glyph("close", (x + w - 34 * s, top - self.title_h + 8 * s, 24 * s, 24 * s), "close",
                              lambda ctx: close(state))]
        self.chrome = {"title", "close"}  # (drawn unclipped by the scroll)
        if rail is not None:
            button = t.size("iconRail", "button", 36) * s
            gap = t.size("iconRail", "gap", 4) * s
            bx = x + pad  # (the first tile on the cards' left edge, as the header row under it)
            by = top - self.title_h - (self.rail_h - button) * 0.5 - button
            for ident, label, tip, glyph in rail.pages:
                wid = "rail:" + ident
                self.widgets.append(RailButton(wid, (bx, by, button, button), ident, label, tip, glyph,
                                               rail.get, rail.set))
                self.chrome.add(wid)
                bx += button + gap
        c = self.content
        self.widgets += [wd for wd in layout.widgets if wd.rect[1] + wd.rect[3] > c[1] and wd.rect[1] < c[1] + c[3]]

    def widget_at(self, x, y):
        if not contains(self.rect, x, y):
            return None
        for wd in reversed(self.widgets):
            if getattr(wd, "passive", False):
                continue
            if contains(wd.rect, x, y) and (wd.id in self.chrome or contains(self.content, x, y)):
                return wd
        return None

    def draw(self):
        t, s = self.theme, self.scale
        p = Painter(t, s)
        x, y, w, h = self.rect
        radius = t.card_radius * s
        p.rect(inset(self.rect, -3 * s), (0, 0, 0, 0.35), 12 * s, soft=14 * s)
        p.rect(self.rect, t.panel, radius, t.outline, 1.0 * s)
        rule = max(1.0, s)
        p.rect((x + 1 * s, y + h - self.title_h, w - 2 * s, rule), t.cardRule)
        if self.rail_h:
            p.rect((x + 1 * s, y + h - self.title_h - self.rail_h, w - 2 * s, rule), t.role("iconRail", "edge", "cardRule"))
        cx, cy, cw, ch = self.content
        gpu.state.scissor_test_set(True)
        try:
            for wd in self.widgets:
                if wd.id in self.chrome:
                    gpu.state.scissor_set(int(x), int(y), int(w), int(h))
                else:
                    gpu.state.scissor_set(int(cx), int(cy + 2 * s), int(cw), int(ch - 3 * s))
                p.fade = 0.5 if wd.dim or (wd.disabled and not wd.own_disabled) else 1.0
                wd.draw(p, self)
            p.fade = 1.0
        finally:
            gpu.state.scissor_test_set(False)
        if self.overflow > 0:  # a thin bar at the right edge (the app's scroll bar)
            track_h = ch - 12 * s
            frac = ch / (ch + self.overflow * s)
            pos = self.state.scroll / self.overflow if self.overflow else 0.0
            th = max(24 * s, track_h * frac)
            ty = cy + 6 * s + (track_h - th) * (1.0 - pos)
            p.rect((x + w - 6 * s, ty, 4 * s, th), t.scroll, 2 * s)
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
_theme_data = [None]  # the active theme's data (themes.py: the one picked, merged with what it extends)


def theme_data():
    """The active theme's data (themes.apply sets it), else Charcoal's file as it ships."""
    if _theme_data[0] is None:
        _theme_data[0] = Theme._read(THEME_FILE)
    return _theme_data[0]


def set_theme_data(data):
    _theme_data[0] = data
    forget_theme()


def forget_theme():
    """The theme read again on the next draw (another one picked, its file edited)."""
    _theme_cache[0] = None


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
