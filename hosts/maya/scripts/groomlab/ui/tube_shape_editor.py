# SPDX-License-Identifier: Apache-2.0
"""The tube shape editor (Groom Tools > Shape > Tubes): a square showing a tube's cross-section
outline, its control points draggable (Qt: groomlab.ui.qt).

The square is the section's unit domain: u across (left to right), v up, the guide through its
center (the cross). The outline is the closed curve through the points as the core draws it
(groomlab_core.tube_shape_curve: smooth, or straight edges). Drag a point to move it; click on the
outline between two points to add one there; right-click a point to remove it (3 stay at least).
With `mirror` (on by default) the outline stays symmetric about the v axis: a point and its mirror
image move, come and go together, points on the axis stay on it. Built-in shapes show read-only.

Signals: dragged(points) while a point moves, committed(points) when an edit settles (a release,
an added or removed point). The core fits the points into the square when they are saved
(groomlab.set_tube_shape_points), so a shape drawn small comes back filling it.
"""

import groomlab_core
from groomlab.ui import qt, theme
from groomlab.ui.qt import QtCore, QtGui, QtWidgets

EXTENT = 1.25  # the square shows -EXTENT..EXTENT on both axes (the outline is fitted to -1..1)
AXIS = 0.02    # points this near the v axis are on it (mirror)
HIT = 7.0      # pixels: how near a press must be to a point or the outline


def _on_axis(p):
    return abs(p[0]) <= AXIS


def symmetric(points):
    """The points made symmetric about the v axis from their right side (u > 0): the right chain
    in order, the points on the axis at its ends set on it, its mirror image closing the outline.
    Points with nothing right of the axis come back mirrored from the left. Fewer than 3 after:
    the points as they were."""
    pts = [tuple(p) for p in points]
    n = len(pts)
    if n < 3:
        return pts
    side = 1.0 if any(p[0] > AXIS for p in pts) else -1.0
    right = [side * p[0] > AXIS for p in pts]
    if all(right) or not any(right):
        return pts
    # The right chain: from the first right point after a point that is not, round the outline.
    start = next(i for i in range(n) if right[i] and not right[i - 1])
    chain = []
    i = start
    while right[i]:
        chain.append(pts[i])
        i = (i + 1) % n
    before, after = pts[start - 1], pts[i]
    out = []
    if _on_axis(before):
        out.append((0.0, before[1]))
    out.extend(chain)
    if _on_axis(after) and (not out or after != before):
        out.append((0.0, after[1]))
    out.extend((-u, v) for u, v in reversed(chain))
    if side < 0:  # (the chain was the left one: the same outline, flipped back)
        out = [(-u, v) for u, v in reversed(out)]
    return out if len(out) >= 3 else pts


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


class TubeShapeEditor(QtWidgets.QWidget):
    dragged = QtCore.Signal(object)
    committed = QtCore.Signal(object)

    def __init__(self, parent=None):
        super(TubeShapeEditor, self).__init__(parent)
        self.setObjectName("tubeShapeEditor")
        self.points = []
        self.smooth = True
        self.mirror = True
        self.editable = False
        self.problem = ""  # what is wrong with the outline as it is (drawn red)
        self.note = ""     # said in the corner (a built-in: read-only)
        self._drag = None
        self._hover = None
        self._curve = []
        self.setMinimumSize(theme.px(150), theme.px(150))
        self.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        self.setFixedHeight(theme.px(190))
        self.setMouseTracking(True)
        self.setFocusPolicy(QtCore.Qt.ClickFocus)
        self.setToolTip("The tube's cross-section: the guide runs through the cross, u across, v up (off the "
                        "scalp). Drag a point to move it, click the outline to add a point, right-click a point to "
                        "remove it. Mirror keeps the left and right sides alike.")

    # -- State ---------------------------------------------------------------------

    def set_shape(self, points, smooth=True, editable=True, note=""):
        """Show an outline (control points (u, v)); not while a point is dragged."""
        if self._drag is not None:
            return
        points = [tuple(float(c) for c in p) for p in points]
        if (points == self.points and bool(smooth) == self.smooth and bool(editable) == self.editable
                and note == self.note and not self.problem):
            return  # (the panel shows it again at every poll: nothing to sample or draw again)
        self.points = points
        self.smooth = bool(smooth)
        self.editable = bool(editable)
        self.note = note
        self.problem = ""
        self._update_curve()
        self.update()

    def busy(self):
        return self._drag is not None

    def set_mirror(self, on):
        """Mirror on: the outline made symmetric (from its right side) when it is not. Returns
        whether the points changed."""
        self.mirror = bool(on)
        if not on or not self.editable:
            return False
        made = symmetric(self.points)
        if made == self.points:
            return False
        self.points = made
        self._update_curve()
        self.update()
        return True

    def _update_curve(self):
        try:
            self._curve = [tuple(p) for p in groomlab_core.tube_shape_curve(self.points, self.smooth)] if len(self.points) >= 3 else []
        except (RuntimeError, ValueError, TypeError):
            self._curve = []
        try:
            groomlab_core.make_tube_shape("check", self.points, self.smooth)
            self.problem = ""
        except ValueError as e:
            self.problem = str(e).split("': ", 1)[-1]

    # -- Geometry ------------------------------------------------------------------

    def square(self):
        side = min(self.width(), self.height()) - 12.0
        return QtCore.QRectF((self.width() - side) / 2.0, (self.height() - side) / 2.0, side, side)

    def to_screen(self, u, v):
        r = self.square()
        return QtCore.QPointF(r.center().x() + u / EXTENT * r.width() / 2.0, r.center().y() - v / EXTENT * r.height() / 2.0)

    def from_screen(self, point):
        r = self.square()
        u = (point.x() - r.center().x()) / (r.width() / 2.0) * EXTENT
        v = -(point.y() - r.center().y()) / (r.height() / 2.0) * EXTENT
        return min(max(u, -EXTENT), EXTENT), min(max(v, -EXTENT), EXTENT)

    def point_at(self, pos):
        for i, (u, v) in enumerate(self.points):
            if QtCore.QLineF(self.to_screen(u, v), pos).length() <= HIT:
                return i
        return None

    def edge_at(self, pos):
        """(span, (u, v)): the outline's point nearest `pos` within HIT pixels, and the span (from
        control point `span` to the next) it lies on; None when the press is off the outline."""
        n = len(self.points)
        if n < 3 or not self._curve:
            return None
        per = max(len(self._curve) // n, 1)
        best, dist = None, HIT
        for k, (u, v) in enumerate(self._curve):
            d = QtCore.QLineF(self.to_screen(u, v), pos).length()
            if d <= dist:
                best, dist = k, d
        if best is None:
            return None
        return min(best // per, n - 1), self._curve[best]

    # -- Drawing ---------------------------------------------------------------------

    def paintEvent(self, event):
        painter = QtGui.QPainter(self)
        painter.setRenderHint(QtGui.QPainter.Antialiasing)
        t = theme.current()
        accent = t.qcolor(t.get("ui.sliders", ["yellow"])[0] if t.get("ui.sliders") else "yellow")
        bad = t.qcolor("ui.error")
        painter.fillRect(self.rect(), t.qcolor("ui.panel"))
        r = self.square()
        painter.fillRect(r, t.qcolor("ui.background"))
        # The grid: the unit square (where the outline is fitted) and its halves; the axes stronger.
        painter.setPen(QtGui.QPen(t.qcolor("ui.raised"), 1))
        for k in (-1.0, -0.5, 0.5, 1.0):
            painter.drawLine(self.to_screen(k, -EXTENT), self.to_screen(k, EXTENT))
            painter.drawLine(self.to_screen(-EXTENT, k), self.to_screen(EXTENT, k))
        painter.setPen(QtGui.QPen(t.qcolor("ui.textDim"), 1, QtCore.Qt.DashLine if self.mirror else QtCore.Qt.SolidLine))
        painter.drawLine(self.to_screen(0.0, -EXTENT), self.to_screen(0.0, EXTENT))
        painter.setPen(QtGui.QPen(t.qcolor("ui.raised"), 1))
        painter.drawLine(self.to_screen(-EXTENT, 0.0), self.to_screen(EXTENT, 0.0))
        line = bad if self.problem else accent
        if not self.editable:
            line = QtGui.QColor(line)
            line.setAlpha(150)
        if self._curve:
            path = QtGui.QPainterPath(self.to_screen(*self._curve[0]))
            for u, v in self._curve[1:]:
                path.lineTo(self.to_screen(u, v))
            path.closeSubpath()
            wash = QtGui.QColor(line)
            wash.setAlpha(45)
            painter.fillPath(path, wash)
            painter.setPen(QtGui.QPen(line, 2))
            painter.drawPath(path)
        # The guide: a small cross at the center.
        painter.setPen(QtGui.QPen(t.qcolor("ui.text"), 1.5))
        c = self.to_screen(0.0, 0.0)
        painter.drawLine(c + QtCore.QPointF(-5, 0), c + QtCore.QPointF(5, 0))
        painter.drawLine(c + QtCore.QPointF(0, -5), c + QtCore.QPointF(0, 5))
        if self.editable:
            mates = partners(self.points) if self.mirror else [None] * len(self.points)
            for i, (u, v) in enumerate(self.points):
                hot = i in (self._drag, self._hover)
                painter.setPen(QtGui.QPen(t.qcolor("ui.border"), 1))
                dot = t.qcolor("ui.text") if hot else line
                if self.mirror and mates[i] is not None and mates[i] != i and u < 0:
                    dot = QtGui.QColor(dot)
                    dot.setAlpha(150)  # the mirrored side: it follows the other
                painter.setBrush(dot)
                painter.drawEllipse(self.to_screen(u, v), 5 if hot else 4, 5 if hot else 4)
        painter.setPen(bad if self.problem else t.qcolor("ui.textDim"))
        text = self.problem or self.note
        if not text and (self._drag is not None or self._hover is not None):
            u, v = self.points[self._drag if self._drag is not None else self._hover]
            text = "u %.2f  v %.2f" % (u, v)
        if text:
            painter.drawText(QtCore.QRectF(r.left() + 4, r.bottom() - 32, r.width() - 8, 30),
                             QtCore.Qt.AlignLeft | QtCore.Qt.AlignBottom | QtCore.Qt.TextWordWrap, text)

    # -- Editing ---------------------------------------------------------------------

    def move_point(self, i, u, v):
        """Point i to (u, v), its mirror image with it (mirror): a point on the axis stays on it,
        one off it stays on its side."""
        mates = partners(self.points) if self.mirror else None
        if mates is not None and mates[i] is not None:
            j = mates[i]
            if j == i:
                u = 0.0
            else:
                side = 1.0 if self.points[i][0] > 0 else -1.0
                u = side * max(side * u, AXIS * 2)
                self.points[j] = (-u, v)
        self.points[i] = (u, v)
        self._update_curve()
        self.update()

    def add_point(self, span, at):
        """A point at `at` on the outline after control point `span` (mirror: its mirror image on
        the mirrored span too, or one point on the axis for a span crossing it)."""
        n = len(self.points)
        a, b = span, (span + 1) % n
        mates = partners(self.points) if self.mirror else None
        inserts = [(b if b != 0 else n, tuple(at))]
        if mates is not None and mates[a] is not None and mates[b] is not None:
            pa, pb = mates[a], mates[b]
            if pa == b and pb == a:  # the span crosses the axis: one point, on it
                inserts = [(b if b != 0 else n, (0.0, at[1]))]
            elif not (pa == a and pb == b):
                # The mirrored span runs from pb to pa (the outline goes round the other way there).
                inserts.append((pa if pa != 0 else n, (-at[0], at[1])))
        for index, p in sorted(inserts, key=lambda x: -x[0]):
            self.points.insert(index, p)
        self._update_curve()
        self.update()

    def remove_point(self, i):
        """Point i removed (its mirror image with it); False when fewer than 3 would stay."""
        mates = partners(self.points) if self.mirror else None
        gone = {i}
        if mates is not None and mates[i] is not None:
            gone.add(mates[i])
        if len(self.points) - len(gone) < 3:
            return False
        self.points = [p for k, p in enumerate(self.points) if k not in gone]
        self._update_curve()
        self.update()
        return True

    def mousePressEvent(self, event):
        if not self.editable:
            return
        pos = qt.event_pos(event)
        i = self.point_at(pos)
        if event.button() == QtCore.Qt.RightButton:
            if i is not None and self.remove_point(i):
                self.committed.emit(list(self.points))
            return
        if event.button() != QtCore.Qt.LeftButton:
            return
        if i is not None:
            self._drag = i
            self.update()
            return
        hit = self.edge_at(pos)
        if hit is not None:
            self.add_point(*hit)
            self.committed.emit(list(self.points))

    def mouseMoveEvent(self, event):
        pos = qt.event_pos(event)
        if self._drag is None:
            hover = self.point_at(pos) if self.editable else None
            if hover != self._hover:
                self._hover = hover
                self.update()
            return
        self.move_point(self._drag, *self.from_screen(pos))
        self.dragged.emit(list(self.points))

    def mouseReleaseEvent(self, event):
        if self._drag is not None:
            self._drag = None
            self.update()
            self.committed.emit(list(self.points))
