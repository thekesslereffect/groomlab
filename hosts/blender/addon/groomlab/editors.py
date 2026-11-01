# SPDX-License-Identifier: GPL-3.0-or-later
"""The GroomLab window's editors (uikit widgets drawn over the 3D view):

- ProfileEditor: a tube shape's cross-section (Maya's tube_shape_editor.py): the outline through its
  control points (the core's centripetal Catmull-Rom, denseCurve in tube.cpp), points dragged,
  added on the outline, removed with a right-click; Mirror keeps the sides alike. Built-in shapes
  show read-only.
- CurveEditor: a ramp (a Blender CurveMapping, what the sidebar's template_curve_mapping edits):
  points dragged, added with a click, removed with a right-click.
- IconPicker: an enum with icons (the brush alphas' grid): the current item's thumbnail and name; a
  press opens a grid of the items' thumbnails.

Thumbnails are the add-on's previews (bpy.utils.previews), read as textures from their pixels."""

import math
import time

import gpu

from . import uikit

# ---- The tube outline (tube.cpp's denseCurve) ----------------------------------------------------

AXIS = 0.02    # points this near the v axis are on it (mirror)
EXTENT = 1.25  # the square shows -EXTENT..EXTENT (outlines are fitted to -1..1)


def samples_per_span(n):
    return min(16, max(4, 512 // max(n, 1)))


def dense_curve(pts, smooth):
    """The closed curve through the points as the core samples it."""
    n = len(pts)
    if n < 3:
        return list(pts)
    per = samples_per_span(n)
    out = []

    def knot(a, b):
        return max(math.sqrt(math.hypot(b[0] - a[0], b[1] - a[1])), 1e-4)

    def mix(a, b, ka, kb, at):
        f = (at - ka) / (kb - ka)
        return (a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f)

    for i in range(n):
        p0, p1, p2, p3 = pts[(i - 1) % n], pts[i], pts[(i + 1) % n], pts[(i + 2) % n]
        for k in range(per):
            t = k / float(per)
            if not smooth:
                out.append((p1[0] + (p2[0] - p1[0]) * t, p1[1] + (p2[1] - p1[1]) * t))
                continue
            k0 = 0.0
            k1 = k0 + knot(p0, p1)
            k2 = k1 + knot(p1, p2)
            k3 = k2 + knot(p2, p3)
            x = k1 + (k2 - k1) * t
            a1, a2, a3 = mix(p0, p1, k0, k1, x), mix(p1, p2, k1, k2, x), mix(p2, p3, k2, k3, x)
            b1, b2 = mix(a1, a2, k0, k2, x), mix(a2, a3, k1, k3, x)
            out.append(mix(b1, b2, k1, k2, x))
    return out


def partners(points):
    """Each point's mirror image among the points (its own index on the axis; None when it has none)."""
    out = []
    for i, (u, v) in enumerate(points):
        if abs(u) <= AXIS:
            out.append(i)
            continue
        best, dist = None, 1e9
        for j, (a, b) in enumerate(points):
            d = abs(a + u) + abs(b - v)
            if j != i and d < dist:
                best, dist = j, d
        out.append(best if dist < 1e-3 else None)
    return out


class ProfileEditor(uikit.Widget):
    """A tube shape's cross-section in a square. shape: {"name", "points", "smooth", "builtin"};
    preview(points) while a point moves, commit(points) when an edit settles; mirror() says whether
    the sides move together."""

    HIT = 8.0

    def __init__(self, wid, rect, shape, preview, commit, mirror):
        super().__init__(wid, rect)
        self.shape, self.preview, self.commit, self.mirror = shape, preview, commit, mirror
        self.points = [tuple(p) for p in (shape or {}).get("points") or []]
        self.editable = bool(shape) and not shape.get("builtin")
        self.tooltip = ("Drag a point to move it; click the outline to add one; right-click a point to remove it"
                        if self.editable else "A built-in shape: Duplicate it (the library's glyph) to edit a copy")

    def to_screen(self, u, v):
        x, y, w, h = self.rect
        return x + (u / EXTENT * 0.5 + 0.5) * w, y + (v / EXTENT * 0.5 + 0.5) * h

    def from_screen(self, sx, sy):
        x, y, w, h = self.rect
        u = ((sx - x) / w - 0.5) * 2.0 * EXTENT
        v = ((sy - y) / h - 0.5) * 2.0 * EXTENT
        return max(-EXTENT, min(EXTENT, u)), max(-EXTENT, min(EXTENT, v))

    def point_at(self, sx, sy, scale):
        for i, (u, v) in enumerate(self.points):
            px, py = self.to_screen(u, v)
            if math.hypot(px - sx, py - sy) <= self.HIT * scale:
                return i
        return None

    def edge_at(self, sx, sy, scale):
        """(span, (u, v)): the outline's nearest point to the pointer, after control point span."""
        n = len(self.points)
        if n < 3:
            return None
        curve = dense_curve(self.points, self.shape.get("smooth", True))
        per = samples_per_span(n)
        best, where = None, None
        for k, (u, v) in enumerate(curve):
            px, py = self.to_screen(u, v)
            d = math.hypot(px - sx, py - sy)
            if d <= self.HIT * scale and (best is None or d < best):
                best, where = d, (k // per, (u, v))
        return where

    def draw(self, p, win):
        t, s = p.theme, p.scale
        x, y, w, h = self.rect
        p.rect(self.rect, t.background, t.radius * s, t.border, 1.0 * s)
        cx, cy = self.to_screen(0.0, 0.0)
        p.rect((x + 4 * s, cy, w - 8 * s, max(1.0, s)), t.border)  # the guide through the center
        p.rect((cx, y + 4 * s, max(1.0, s), h - 8 * s), t.border)
        if len(self.points) >= 3:
            curve = dense_curve(self.points, self.shape.get("smooth", True))
            pts = [self.to_screen(u, v) for u, v in curve]
            p.lines(pts + [pts[0]], t.accent if self.editable else t.textDim, 1.8)
        drag = getattr(win.interaction, "index", None) if isinstance(win.interaction, _ProfileDrag) else None
        mates = partners(self.points) if self.mirror() else [None] * len(self.points)
        for i, (u, v) in enumerate(self.points):
            px, py = self.to_screen(u, v)
            r = (5 if i == drag else 4) * s
            follower = self.mirror() and mates[i] not in (None, i) and u < 0
            color = t.text if self.editable else t.textMuted
            p.rect((px - r, py - r, 2 * r, 2 * r), uikit.with_alpha(color, 0.55 if follower else 1.0), r)
        if not self.editable:
            p.text("Built-in: Duplicate to edit", (x + 6 * s, y + 4 * s, w - 12 * s, 16 * s), t.textMuted,
                   t.font_size - 2)

    # Editing (Maya's move_point, add_point, remove_point)
    def move_point(self, i, u, v):
        mates = partners(self.points) if self.mirror() else None
        if mates is not None and mates[i] is not None:
            j = mates[i]
            if j == i:
                u = 0.0
            else:
                side = 1.0 if self.points[i][0] > 0 else -1.0
                u = side * max(side * u, AXIS * 2)
                self.points[j] = (-u, v)
        self.points[i] = (u, v)

    def add_point(self, span, at):
        n = len(self.points)
        a, b = span, (span + 1) % n
        mates = partners(self.points) if self.mirror() else None
        inserts = [(b if b != 0 else n, tuple(at))]
        if mates is not None and mates[a] is not None and mates[b] is not None:
            pa, pb = mates[a], mates[b]
            if pa == b and pb == a:
                inserts = [(b if b != 0 else n, (0.0, at[1]))]
            elif not (pa == a and pb == b):
                inserts.append((pa if pa != 0 else n, (-at[0], at[1])))
        for index, q in sorted(inserts, key=lambda e: -e[0]):
            self.points.insert(index, q)

    def remove_point(self, i):
        mates = partners(self.points) if self.mirror() else None
        gone = {i}
        if mates is not None and mates[i] is not None:
            gone.add(mates[i])
        if len(self.points) - len(gone) < 3:
            return False
        self.points = [q for k, q in enumerate(self.points) if k not in gone]
        return True

    def press(self, win, context, event):
        if not self.editable:
            return None
        sx, sy = event.mouse_region_x, event.mouse_region_y
        i = self.point_at(sx, sy, win.scale)
        if i is not None:
            return _ProfileDrag(self, i)
        hit = self.edge_at(sx, sy, win.scale)
        if hit is not None:
            self.add_point(*hit)
            self.commit(list(self.points))
        return None

    def right_press(self, win, context, event):
        if not self.editable:
            return
        i = self.point_at(event.mouse_region_x, event.mouse_region_y, win.scale)
        if i is not None and self.remove_point(i):
            self.commit(list(self.points))


class _ProfileDrag(uikit.Interaction):
    def __init__(self, editor, index):
        self.editor, self.index = editor, index
        self.sent = 0.0

    def event(self, win, context, event):
        ed = self.editor
        if event.type == "MOUSEMOVE":
            ed.move_point(self.index, *ed.from_screen(event.mouse_region_x, event.mouse_region_y))
            if time.time() - self.sent > 0.04:  # a preview at most every 40 ms: each is an evaluation
                self.sent = time.time()
                ed.preview(list(ed.points))
            return "RUNNING"
        if event.type == "LEFTMOUSE" and event.value == "RELEASE":
            ed.commit(list(ed.points))
            return "FINISHED"
        if event.type in ("ESC", "RIGHTMOUSE") and event.value == "PRESS":
            ed.commit(None)  # (None: put the shape back)
            return "CANCELLED"
        return "RUNNING"

    def draw(self, p, win):
        pass


# ---- Ramps (a CurveMapping) ---------------------------------------------------------------------

class CurveEditor(uikit.Widget):
    """A CurveMapping's first curve in a box: x along, y up, in its clip range."""

    HIT = 7.0

    def __init__(self, wid, rect, owner, prop):
        super().__init__(wid, rect)
        self.owner, self.prop = owner, prop
        self.tooltip = "Drag a point to move it; click to add one; right-click a point to remove it"

    @property
    def mapping(self):
        return getattr(self.owner, self.prop)

    def bounds(self):
        m = self.mapping
        if getattr(m, "use_clip", True):
            return m.clip_min_x, m.clip_max_x, m.clip_min_y, m.clip_max_y
        return 0.0, 1.0, 0.0, 1.0

    def inner(self, s):
        x, y, w, h = self.rect
        pad = 8 * s
        return x + pad, y + pad, w - 2 * pad, h - 2 * pad

    def to_screen(self, cx, cy, s):
        x0, x1, y0, y1 = self.bounds()
        ix, iy, iw, ih = self.inner(s)
        return ix + (cx - x0) / ((x1 - x0) or 1.0) * iw, iy + (cy - y0) / ((y1 - y0) or 1.0) * ih

    def from_screen(self, sx, sy, s):
        x0, x1, y0, y1 = self.bounds()
        ix, iy, iw, ih = self.inner(s)
        fx = min(1.0, max(0.0, (sx - ix) / (iw or 1.0)))
        fy = min(1.0, max(0.0, (sy - iy) / (ih or 1.0)))
        return x0 + fx * (x1 - x0), y0 + fy * (y1 - y0)

    def point_at(self, sx, sy, s):
        for i, pt in enumerate(self.mapping.curves[0].points):
            px, py = self.to_screen(pt.location[0], pt.location[1], s)
            if math.hypot(px - sx, py - sy) <= self.HIT * s:
                return i
        return None

    def draw(self, p, win):
        t, s = p.theme, p.scale
        x, y, w, h = self.rect
        p.rect(self.rect, t.background, t.radius * s, t.border, 1.0 * s)
        ix, iy, iw, ih = self.inner(s)
        for k in range(1, 4):  # a quarter grid
            p.rect((ix + iw * k / 4.0, iy, max(1.0, s), ih), uikit.with_alpha(t.border, 0.8))
            p.rect((ix, iy + ih * k / 4.0, iw, max(1.0, s)), uikit.with_alpha(t.border, 0.8))
        m = self.mapping
        curve = m.curves[0]
        try:
            m.initialize()
            x0, x1, _, _ = self.bounds()
            pts = []
            for k in range(65):
                cx = x0 + (x1 - x0) * k / 64.0
                pts.append(self.to_screen(cx, m.evaluate(curve, cx), s))
            p.lines(pts, t.accent, 1.8)
        except (RuntimeError, AttributeError, ValueError):
            pass
        drag = self.drag_index(win)
        for i, pt in enumerate(curve.points):
            px, py = self.to_screen(pt.location[0], pt.location[1], s)
            r = (5 if i == drag else 4) * s
            p.rect((px - r, py - r, 2 * r, 2 * r), t.text, r)

    def drag_index(self, win):
        inter = win.interaction
        return inter.index if isinstance(inter, _CurveDrag) and inter.editor.id == self.id else None

    def changed(self):
        m = self.mapping
        m.update()
        try:
            self.owner.update_tag()  # (a node: its tree notices)
        except AttributeError:
            pass
        uikit.changed.append("Curve")

    def press(self, win, context, event):
        s = win.scale
        sx, sy = event.mouse_region_x, event.mouse_region_y
        i = self.point_at(sx, sy, s)
        if i is None:
            cx, cy = self.from_screen(sx, sy, s)
            self.mapping.curves[0].points.new(cx, cy)
            self.changed()
            i = self.point_at(sx, sy, s)
            if i is None:
                return None
        return _CurveDrag(self, i)

    def right_press(self, win, context, event):
        points = self.mapping.curves[0].points
        i = self.point_at(event.mouse_region_x, event.mouse_region_y, win.scale)
        if i is not None and len(points) > 2:
            points.remove(points[i])
            self.changed()


class _CurveDrag(uikit.Interaction):
    def __init__(self, editor, index):
        self.editor, self.index = editor, index
        pt = editor.mapping.curves[0].points[index]
        self.start = tuple(pt.location)

    def event(self, win, context, event):
        ed = self.editor
        points = ed.mapping.curves[0].points
        if self.index >= len(points):
            return "CANCELLED"
        if event.type == "MOUSEMOVE":
            points[self.index].location = ed.from_screen(event.mouse_region_x, event.mouse_region_y, win.scale)
            ed.mapping.update()
            # Blender keeps the points in x order: follow the one dragged.
            here = ed.point_at(event.mouse_region_x, event.mouse_region_y, win.scale)
            if here is not None:
                self.index = here
            return "RUNNING"
        if event.type == "LEFTMOUSE" and event.value == "RELEASE":
            ed.changed()
            return "FINISHED"
        if event.type in ("ESC", "RIGHTMOUSE") and event.value == "PRESS":
            points[self.index].location = self.start
            ed.mapping.update()
            return "CANCELLED"
        return "RUNNING"


# ---- Thumbnails ---------------------------------------------------------------------------------

_thumbs = {}  # icon_value -> GPUTexture (or None: no pixels yet)


def thumbnail(preview):
    """A preview's image as a texture (None while Blender has not made its pixels yet)."""
    if preview is None:
        return None
    key = preview.icon_id
    tex = _thumbs.get(key)
    if tex is not None:
        return tex
    w, h = preview.image_size
    if not w or not h:
        w, h = preview.icon_size
        source = "icon_pixels_float"
    else:
        source = "image_pixels_float"
    if not w or not h:
        return None
    import array
    pixels = array.array("f", [0.0]) * (w * h * 4)
    getattr(preview, source).foreach_get(pixels)
    for i in range(0, len(pixels), 4):  # (sRGB thumbnails, the overlay takes linear)
        pixels[i] = uikit.linear(pixels[i])
        pixels[i + 1] = uikit.linear(pixels[i + 1])
        pixels[i + 2] = uikit.linear(pixels[i + 2])
    buf = gpu.types.Buffer("FLOAT", len(pixels), pixels)
    for fmt in ("SFLOAT_16_16_16_16", "RGBA16F"):
        try:
            tex = gpu.types.GPUTexture((w, h), format=fmt, data=buf)
            break
        except (TypeError, ValueError):
            continue
    _thumbs[key] = tex
    return tex


def release():
    _thumbs.clear()


def draw_thumbnail(p, tex, r):
    """A thumbnail texture in rect r (uikit's icon shader, no tint)."""
    shader, quad = uikit._icon_shader()
    gpu.state.blend_set("ALPHA")
    shader.bind()
    shader.uniform_float("ModelViewProjectionMatrix", p.mvp)
    shader.uniform_float("rect", r)
    shader.uniform_float("tint", (0.0, 0.0, 0.0, 0.0))
    shader.uniform_float("alpha", (p.fade, 0.0, 0.0, 0.0))
    shader.uniform_sampler("image", tex)
    quad.draw(shader)


class IconPicker(uikit.Widget):
    """An enum whose items have icons (the alphas): the current one's thumbnail and name; a press
    opens a grid of them. items: [(id, name, tip, icon_value)]; preview_of(icon_value)."""

    def __init__(self, wid, rect, prop, items, preview_of):
        super().__init__(wid, rect)
        self.prop, self.items, self.preview_of = prop, items, preview_of
        self.tooltip = prop.tooltip

    def current(self):
        value = self.prop.get()
        return next((it for it in self.items if it[0] == value), self.items[0] if self.items else None)

    def draw(self, p, win):
        t, s = p.theme, p.scale
        hover = win.hover == self.id or (isinstance(win.interaction, _GridInteraction) and win.interaction.picker.id == self.id)
        p.rect(self.rect, t.hover if hover else t.raised, t.radius * s)
        x, y, w, h = self.rect
        cur = self.current()
        side = h - 6 * s
        tex = thumbnail(self.preview_of(cur[3])) if cur and cur[3] else None
        if tex is not None:
            draw_thumbnail(p, tex, (x + 3 * s, y + 3 * s, side, side))
        p.text(cur[1] if cur else "-", (x + side + 10 * s, y, w - side - 30 * s, h), t.text)
        cx, cy, k = x + w - 13 * s, y + h * 0.5, 3.5 * s
        p.lines([(cx - k, cy + k * 0.5), (cx, cy - k * 0.5), (cx + k, cy + k * 0.5)], t.textDim)

    def press(self, win, context, event):
        return _GridInteraction(self, win)


class _GridInteraction(uikit.Interaction):
    """The picker's open grid: four thumbnails to a row with their names, under (or over) the field."""

    COLS = 4

    def __init__(self, picker, win):
        self.picker = picker
        s = win.scale
        x, y, w, h = picker.rect
        w = max(w, 280 * s)  # thumbnails big enough to tell apart, whatever the field's width
        x = max(4 * s, min(x, win.region_w - w - 4 * s))
        self.cell = (w - 8 * s) / self.COLS
        rows = max(1, (len(picker.items) + self.COLS - 1) // self.COLS)
        height = min(rows, 5) * (self.cell + 14 * s) + 8 * s
        top = y - 4 * s
        if top - height < 4 * s:
            top = y + h + 4 * s + height
        self.rect = (x, top - height, w, height)
        self.hover = -1
        self.armed = False

    def _index(self, win, mx, my):
        if not uikit.contains(self.rect, mx, my):
            return -1
        s = win.scale
        x, y, w, h = self.rect
        col = int((mx - x - 4 * s) // self.cell)
        row = int((y + h - 4 * s - my) // (self.cell + 14 * s))
        i = row * self.COLS + col
        return i if 0 <= col < self.COLS and 0 <= i < len(self.picker.items) else -1

    def event(self, win, context, event):
        mx, my = event.mouse_region_x, event.mouse_region_y
        if event.type == "MOUSEMOVE":
            self.hover = self._index(win, mx, my)
            return "RUNNING"
        if event.type == "LEFTMOUSE" and event.value == "RELEASE":
            i = self._index(win, mx, my)
            if i >= 0 and self.armed:
                self.picker.prop.set(self.picker.items[i][0])
                return "FINISHED"
            self.armed = True
            return "RUNNING"
        if event.type == "LEFTMOUSE" and event.value == "PRESS":
            i = self._index(win, mx, my)
            if i >= 0:
                self.picker.prop.set(self.picker.items[i][0])
                return "FINISHED"
            return "CANCELLED" if uikit.contains(self.picker.rect, mx, my) else "CANCELLED_PASS"
        if event.type in ("ESC", "RIGHTMOUSE") and event.value == "PRESS":
            return "CANCELLED"
        return "RUNNING"

    def draw(self, p, win):
        t, s = p.theme, p.scale
        p.rect(uikit.inset(self.rect, -2 * s), (0, 0, 0, 0.35), 8 * s, soft=10 * s)
        p.rect(self.rect, t.panel, t.radius * s, t.border, 1.0 * s)
        x, y, w, h = self.rect
        current = self.picker.prop.get()
        for i, (ident, name, _tip, icon) in enumerate(self.picker.items[:self.COLS * 5]):
            col, row = i % self.COLS, i // self.COLS
            cx = x + 4 * s + col * self.cell
            cy = y + h - 4 * s - (row + 1) * (self.cell + 14 * s)
            cell = (cx + 2 * s, cy, self.cell - 4 * s, self.cell + 12 * s)
            if ident == current:
                p.rect(cell, t.selection, t.radius * s, t.accent, 1.0 * s)
            elif i == self.hover:
                p.rect(cell, t.hover, t.radius * s)
            tex = thumbnail(self.picker.preview_of(icon)) if icon else None
            side = self.cell - 12 * s
            if tex is not None:
                draw_thumbnail(p, tex, (cx + 6 * s, cy + 14 * s, side, side))
            p.text(name, (cx + 2 * s, cy + 1 * s, self.cell - 4 * s, 12 * s), t.textDim, t.font_size - 3, "center")


# ---- Menus --------------------------------------------------------------------------------------

class PopupMenu(uikit.Interaction):
    """A menu drawn in the window's look: entries (drawn.menu_entries) in a box at `anchor` (under
    a button, or at the pointer), submenus to the side on hover. An entry's action runs when it is
    picked; a press elsewhere or Esc closes it. Entries: {"kind": op | check | sub | label | sep,
    "text", "icon", "run", "checked", "enabled", "sub" (a menu's entries, built when it opens)}."""

    def __init__(self, win, entries, anchor, armed=False):
        self.win_scale, self.region_w = win.scale, win.region_w
        self.levels = []  # [(entries, rect, hover)]
        self.armed = armed  # a press-and-release from the button first
        self.open_level(entries, anchor, below=True)

    def sizes(self, entries):
        s = self.win_scale
        import blf
        fid = uikit.font()
        blf.size(fid, uikit._theme().font_size * s)
        width = max([blf.dimensions(fid, e.get("text", ""))[0] for e in entries] + [80 * s]) + 64 * s
        heights = [(7 if e["kind"] == "sep" else 20 if e["kind"] == "label" else 24) * s for e in entries]
        return width, heights

    def open_level(self, entries, anchor, below):
        s = self.win_scale
        width, heights = self.sizes(entries)
        height = sum(heights) + 8 * s
        ax, ay, aw, ah = anchor
        if below:  # under the anchor (a button), else beside it (a submenu's row)
            x, top = ax, ay
            if top - height < 4 * s:
                top = ay + ah + height
        else:
            x, top = ax + aw + 2 * s, ay + ah + 4 * s
            if x + width > self.region_w - 4 * s:
                x = ax - width - 2 * s
            top = max(top, height + 4 * s)
        x = max(4 * s, min(x, self.region_w - width - 4 * s))
        self.levels.append([entries, (x, top - height, width, height), -1, heights])

    def entry_rects(self, level):
        entries, rect, _hover, heights = level
        x, y, w, h = rect
        s = self.win_scale
        top = y + h - 4 * s
        out = []
        for e, eh in zip(entries, heights):
            out.append((x + 4 * s, top - eh, w - 8 * s, eh))
            top -= eh
        return out

    def at(self, mx, my):
        """(level index, entry index) under the pointer, or (level, -1) over a box's edge, or None."""
        for li in range(len(self.levels) - 1, -1, -1):
            level = self.levels[li]
            if uikit.contains(level[1], mx, my):
                for ei, r in enumerate(self.entry_rects(level)):
                    if uikit.contains(r, mx, my):
                        return li, ei
                return li, -1
        return None

    def event(self, win, context, event):
        mx, my = event.mouse_region_x, event.mouse_region_y
        hit = self.at(mx, my)
        if event.type == "MOUSEMOVE":
            if hit is not None:
                li, ei = hit
                level = self.levels[li]
                if level[2] != ei:
                    level[2] = ei
                    del self.levels[li + 1:]
                    e = level[0][ei] if ei >= 0 else None
                    if e is not None and e["kind"] == "sub" and e.get("enabled", True):
                        sub = e["sub"]() if callable(e["sub"]) else e["sub"]
                        if sub:
                            self.open_level(sub, self.entry_rects(level)[ei], below=False)
            return "RUNNING"
        if event.type == "LEFTMOUSE" and event.value in ("PRESS", "RELEASE"):
            if hit is None:
                if event.value == "PRESS":
                    return "CANCELLED"
                self.armed = True
                return "RUNNING"
            li, ei = hit
            if ei < 0:
                return "RUNNING"
            e = self.levels[li][0][ei]
            if e["kind"] in ("op", "check") and e.get("enabled", True):
                if event.value == "RELEASE" and not self.armed:
                    self.armed = True  # the release of the press that opened it
                    return "RUNNING"
                self.levels = []
                e["run"](context)
                return "FINISHED"
            self.armed = True
            return "RUNNING"
        if event.type in ("ESC", "RIGHTMOUSE") and event.value == "PRESS":
            if event.type == "ESC" and len(self.levels) > 1:
                self.levels.pop()
                return "RUNNING"
            return "CANCELLED"
        return "RUNNING"

    def draw(self, p, win):
        t, s = p.theme, p.scale
        for level in self.levels:
            entries, rect, hover, _heights = level
            p.rect(uikit.inset(rect, -2 * s), (0, 0, 0, 0.35), 8 * s, soft=10 * s)
            p.rect(rect, t.panel, t.radius * s, t.border, 1.0 * s)
            for i, (e, r) in enumerate(zip(entries, self.entry_rects(level))):
                kind = e["kind"]
                if kind == "sep":
                    p.rect((r[0] + 4 * s, r[1] + r[3] * 0.5, r[2] - 8 * s, max(1.0, s)), t.border)
                    continue
                if kind == "label":
                    p.text(e.get("text", ""), uikit.inset(r, 8 * s, 0), t.textMuted, t.font_size - 1)
                    continue
                enabled = e.get("enabled", True)
                p.fade = 1.0 if enabled else 0.4
                if i == hover and enabled:
                    p.rect(r, t.hover, (t.radius - 2) * s)
                if e.get("checked"):
                    cx, cy, k = r[0] + 12 * s, r[1] + r[3] * 0.5, 3.5 * s
                    p.lines([(cx - k, cy), (cx - k * 0.3, cy - k * 0.8), (cx + k, cy + k * 0.8)], t.accent, 1.8)
                elif e.get("icon"):
                    g = 14 * s
                    p.glyph(e["icon"], (r[0] + 5 * s, r[1] + (r[3] - g) * 0.5, g, g), t.textDim)
                p.text(e.get("text", ""), (r[0] + 26 * s, r[1], r[2] - 44 * s, r[3]), t.text)
                if kind == "sub":
                    cx, cy, k = r[0] + r[2] - 10 * s, r[1] + r[3] * 0.5, 3.0 * s
                    p.lines([(cx - k * 0.5, cy + k), (cx + k * 0.5, cy), (cx - k * 0.5, cy - k)], t.textDim)
                p.fade = 1.0
