# SPDX-License-Identifier: Apache-2.0
"""GroomLab Groom Tools: everything you do to a groom's guides, in four tabs (Qt: groomlab.ui.qt).

    Tools      make a groom; the brushes (create, shape, trim, freeze);
               the guide group they work in; guide groups and edits on many guides
    Paint      painted maps (0-1 values on the guides any param can read)
    Sculpt     correctives (sculpt layers on top of the groom)
    Reference  photos as cameras to groom to (look through, align by points, lock)

Pick a tool and drag in the viewport over the groom (the pinned one, else the
selected one). The brush settings under the tabs follow the tool: radius,
strength (signed for tools that work both ways), which guides, symmetry, pen
pressure, keeping off the scalp; Place and Draw add the new guide's shape.
Radius and strength changed in the viewport (middle-drag) show up here. Each brush
keeps its own radius, strength, falloff and curves: picked again, it has them back.

    import groomlab.ui.palette as palette
    palette.show()               # or show(tab="paint"), show(tab="photos")
"""

from groomlab.ui import qt
from groomlab.ui.qt import QtCore, QtWidgets

import re
import groomlab
import groomlab_core
from groomlab.ui import definitions, length_keys_box, theme, tool_tabs, widgets

# The tools are the core's brush catalog (brush_catalog.h), the one list both hosts build their
# palettes from: id, label, category, default strength, whether the strength is signed, a hint
# and the settings its card shows. "select" is the lasso and "freeze" the mask; the rest are
# brush modes.
CATALOG = groomlab_core.brush_catalog()
CATEGORIES = groomlab_core.brush_categories()
SETTINGS = dict((s["id"], s) for s in groomlab_core.brush_settings())
# (section, [(tool, label, tooltip)]) per category, in the catalog's order.
SECTIONS = [(c, [(t["id"], t["label"], t["hint"]) for t in CATALOG if t["category"] == c]) for c in CATEGORIES]
# The settings each tool's card shows (SETTINGS' ids).
TOOL_SETTINGS = dict((t["id"], set(t["settings"])) for t in CATALOG)
SIGNED = set(t["id"] for t in CATALOG if t["signed"])
DEFAULT_STRENGTH = dict((t["id"], t["default_strength"]) for t in CATALOG)
# The tools that work on a corrective: the Correctives tab shows only these.
CORRECTIVE = set(t["id"] for t in CATALOG if t.get("corrective"))
PART = "part"
# The tabs, the texts and the option lists: the UI definitions every host shares (engine/ui:
# palette.json, choices.json; groomlab.ui.definitions).
TABS = [t["id"] for t in definitions.tabs()]
TAB_ALIASES = dict((a, t["id"]) for t in definitions.tabs() for a in t.get("aliases", []))  # old ids
# What each tool does, shown under its name while it is active (the tiles' tooltips say the same).
HINTS = dict((t["id"], t["hint"]) for t in CATALOG)
NAMES = dict((t["id"], t["label"]) for t in CATALOG)
GESTURES = definitions.text("gestures")
SYMMETRY_TIP = definitions.text("symmetryTip")
NO_TOOL = "Pick a tool, then drag over the groom in the viewport."
# The brush block's chips: a short name each (their tooltips: the catalog's hints).
CHIP_LABELS = {"pressure": "Pressure", "collide": "Keep off scalp"}
# A brush's radius (screen pixels) and falloff until it has its own (ToolPalette.recall_brush).
DEFAULT_RADIUS = int(SETTINGS["radius"]["default"])
DEFAULT_FALLOFF = SETTINGS["falloff"]["default"]


SMOOTHING = ("Shape", "Trim")  # the groups whose tools smooth on Shift+drag (Create's, Paint's and Guides' don't say)


def short_hint(hint, category=""):
    """A tool's line on its card, as the app's (groom_tools.cpp refreshCard): the hint's first
    clause without its asides in parentheses (cut at ". " or "; " past 24 characters), then, for
    the Shape and Trim tools, the Shift+drag smoothing they share."""
    short = re.sub(r"\s*\([^()]*\)", "", hint)  # (asides in parentheses: the tooltip's)
    for stop in (". ", "; ", " ("):
        at = short.find(stop)
        if at > 24:
            short = short[:at] + "."
    if not short.endswith("."):
        short += "."
    return short + (" Shift+drag smooths, Ctrl+Shift+drag selects." if category in SMOOTHING else "")


def rail_icon(name):
    """A page's rail icon (palette.json tabs' "icon": the app's file stems, ui_tools,
    groomLab_paint...) as widgets draws it: a line glyph (GLYPHS) or a GroomLab icon."""
    name = name or "tools"
    if name.startswith("groomLab_"):
        return name[len("groomLab_"):]
    if name.startswith("ui_"):
        words = name[3:].split("_")
        return words[0] + "".join(w.title() for w in words[1:])
    return name


# A tile's word where its name doesn't fit a tile (the name stays the tile's text: its tooltip and
# the card say it whole), as the app's tiles show them.
TILE_WORDS = {"lengths": "Keys", "unfreeze": "Unfreeze", "invert": "Invert"}


def help_tip(tool):
    """A tool tile's tooltip: the tool's name and its whole hint, then (for the brushes) the
    gestures as a small list of key and what it does."""
    import html
    parts = ["<b>%s</b>" % html.escape(NAMES.get(tool, tool)), html.escape(HINTS.get(tool, ""))]
    if "radius" in TOOL_SETTINGS.get(tool, set()):
        rows = []
        for entry in GESTURES.split(u" · "):
            key, sep, what = entry.partition(": ")
            rows.append(u"<tr><td style='padding-right:8px'><b>%s</b></td><td>%s</td></tr>"
                        % ((html.escape(key), html.escape(what)) if sep else ("", html.escape(entry))))
        parts.append(u"<table cellspacing='0' cellpadding='1'>%s</table>" % "".join(rows))
    return u"<p style='white-space:pre-wrap'>%s</p>" % u"<br>".join(parts[:2]) + (parts[2] if len(parts) > 2 else "")



# The block titles a core older than these definitions may not name yet.
SETTING_LABELS = {"lengthKeys": "Keys", "newGuides": "New guides", "tube": "Tube", "tubeChannel": "Visibility",
                  "selection": "Selection", "erases": "Erases", "flow": "Flow", "parts": "Parts"}


def _setting(setting, field):
    """A catalog setting's label or hint (brush_settings); "" (a label: SETTING_LABELS') when the
    core doesn't have it."""
    value = SETTINGS.get(setting, {}).get(field)
    if value is None:
        return SETTING_LABELS.get(setting, setting) if field == "label" else ""
    return value


def _default(setting):
    """What one of the catalog's values starts at (brush_settings: the core's BrushToolSettings')."""
    return SETTINGS[setting]["default"]


def _slider(setting, factor=1.0, **options):
    """A SoftSlider for one of the catalog's values: its default, its span and what a typed value
    may reach are the spec's, shown `factor` times it (a length in the shown unit, a fraction in
    percent). `options`: SoftSlider's own (integer, key, label, suffix)."""
    spec = SETTINGS[setting]
    integer = options.pop("integer", spec["kind"] == "int")
    shown = (lambda v: int(round(v * factor))) if integer else (lambda v: v * factor)
    if not integer:
        options.setdefault("shown_decimals", widgets.display_decimals(spec["decimals"], factor))
    return widgets.SoftSlider(shown(spec["default"]), shown(spec["soft_min"]), shown(spec["soft_max"]), integer=integer,
                              minimum=shown(spec["min"]), maximum=shown(spec["max"]), **options)


def _options(name):
    """A choices.json list as (value, label, tooltip)."""
    return [(c["value"], c["label"], c.get("tip", "")) for c in definitions.choices(name)]


# The Shape brush's channels: (name, label, tooltip); a negative strength does the opposite of each.
TUBE_CHANNELS = _options("tubeChannel")
TUBE_SHAPES = _options("tubeShape")


def flow_count_text(arrows, sinks):
    """The Flow card's count line, as every host words it: "34 arrows · 2 sinks", "No flow arrows yet"."""
    if not arrows and not sinks:
        return "No flow arrows yet"
    return "%d arrow%s · %d sink%s" % (arrows, "" if arrows == 1 else "s", sinks, "" if sinks == 1 else "s")


def section_shown(section, tool, tab):
    """Whether a Groom Tools section (palette.json's) shows: its "tools" (the active tool one of
    them), its "tabs" (the current tab one of them); a section with neither always shows."""
    if section.get("tools") and tool not in section["tools"]:
        return False
    return not section.get("tabs") or tab in section["tabs"]


def shape_icon(points, t, size=44):
    """A tube profile's outline as a small icon (the Tube card's Root / Mid / Tip tiles); none: a dash."""
    from groomlab.ui.qt import QtGui
    pixmap = QtGui.QPixmap(size, size)
    pixmap.fill(QtCore.Qt.transparent)
    painter = QtGui.QPainter(pixmap)
    painter.setRenderHint(QtGui.QPainter.Antialiasing, True)
    pen = QtGui.QPen(QtGui.QColor(t.color("ui.text")))
    pen.setWidthF(size / 16.0)
    painter.setPen(pen)
    if not points:
        painter.drawLine(int(size * 0.35), size // 2, int(size * 0.65), size // 2)
    else:
        reach = max(max(abs(float(u)), abs(float(v))) for u, v in points) or 1.0
        half = size * 0.42
        poly = QtGui.QPolygonF([QtCore.QPointF(size / 2.0 + float(u) / reach * half, size / 2.0 - float(v) / reach * half)
                                for u, v in points])
        fill = QtGui.QColor(t.color("ui.accent"))
        fill.setAlpha(70)
        painter.setBrush(fill)
        painter.drawPolygon(poly)
    painter.end()
    return QtGui.QIcon(pixmap)


class TubePanel(QtWidgets.QWidget):
    """The tubes in the tool card, as three blocks (the panel itself is never shown: it owns them):

    Profile          (Shape, Select) the Root, Mid and Tip shape pickers, each with its Apply (the
                     tube's outline changes smoothly root to mid to tip), the cross-section editor
                     on the profile picked last, Mirror and Smooth; the library's New / Rename /
                     Duplicate / Delete as glyphs in the block's header; with no targets, the
                     profiles the groom's guides take (edit one: every guide taking it follows)
    Selected guides  (Shape, Select) the targets' Width, Flatness and Twist, Smooth in the header
    Tube             (Place, Draw) the tube new guides get: a small preview, the shape picker and
                     Width / Flatness / Twist (the library is edited with the Tube brush)

    The shape picker and the three fields are one set of widgets that set_mode() moves between
    the blocks: "edit" (the targets' tubes: each change one undo step) or "new" (the placement
    tube the brush gives new guides). Follows the groom through refresh().

    The targets: the selected guides, else the soloed ones (those left unfrozen:
    groomlab.tube_targets). The editor shows the chosen shape: a built-in read-only (Duplicate
    copies it into the library), a library shape editable: dragging previews live on the groom (no
    undo step), the release commits (one undo step, every guide taking it following)."""

    PREVIEW_MS = 60  # a drag's live preview at most this often

    def __init__(self, owner):
        super(TubePanel, self).__init__()
        self.owner = owner
        self.setObjectName("tubePanel")
        self.setVisible(False)  # a holder: its blocks go in the tool card
        from groomlab.ui.tube_shape_editor import TubeShapeEditor
        from groomlab import units
        self.mode = "edit"
        self.shape = QtWidgets.QComboBox()
        self.shape.setObjectName("tubeShape")
        self.shape.setToolTip("The tube shape: the built-ins, then the groom's library of custom shapes")
        self.shape.activated.connect(lambda _i: self.pick(self.shape.currentData(), "root"))
        # The mid's and the tip's profiles (GH-59): their pickers start with "unset".
        self.mid_shape = QtWidgets.QComboBox()
        self.mid_shape.setObjectName("tubeMidShape")
        self.mid_shape.setToolTip("The profile at the middle of the tube: the outline changes smoothly from the root's "
                                  "to this one and on to the tip's. Between: halfway between the root's and the tip's")
        self.mid_shape.activated.connect(lambda _i: self.pick(self.mid_shape.currentData(), "mid"))
        self.tip_shape = QtWidgets.QComboBox()
        self.tip_shape.setObjectName("tubeTipShape")
        self.tip_shape.setToolTip("The profile at the tip of the tube: the outline changes smoothly to it from the "
                                  "mid's. As root: the root's shape")
        self.tip_shape.activated.connect(lambda _i: self.pick(self.tip_shape.currentData(), "tip"))
        self.profile_combos = {"root": self.shape, "mid": self.mid_shape, "tip": self.tip_shape}
        self.length_factor = units.factor("length")  # shown in real units, stored in scene units
        self.width = widgets.SoftSlider(0.1 * self.length_factor, 0.0, 1.0 * self.length_factor, minimum=0.0,
                                        key="palette.tubeWidth@" + units.unit("length")[0], label="Width",
                                        suffix=units.label("length"))
        self.width.setObjectName("tubeWidth")
        self.width.setToolTip("The widest section's half-width (a round tube's radius): the selected tubes scaled "
                              "to it, their taper and what the brush painted kept; new guides' where none are near")
        self.width.committed.connect(self.set_width)
        self.flatness = widgets.SoftSlider(0.0, 0.0, 1.0, minimum=0.0, maximum=0.99, key=None, label="Flatness")
        self.flatness.setObjectName("tubeFlatness")
        self.flatness.setToolTip("0: as the shape is drawn; toward 1 a flat ribbon (the sections' aspect 1 - this, "
                                 "the same root to tip)")
        self.flatness.committed.connect(lambda v: self.apply(aspect=groomlab_core.tube_aspect_of_flatness(v)))
        self.twist = widgets.SoftSlider(0.0, -180.0, 180.0, key=None, label="Twist", suffix=u"°")
        self.twist.setObjectName("tubeTwist")
        self.twist.setToolTip("Degrees: the sections turned about the guide from lying flat on the scalp, the same "
                              "root to tip (the brush's Twist paints it along the guide)")
        self.twist.committed.connect(lambda v: self.apply(twist=v * 3.14159265358979 / 180.0))
        self.fields = (self.width, self.flatness, self.twist)

        # Profile: the library's actions as glyphs in the header.
        self.new_shape = widgets.glyph_button("plus", "A new library shape (a circle to start from)",
                                              self.new_library_shape)
        self.new_shape.setObjectName("tubeShapeNew")
        self.rename_shape = widgets.glyph_button("rename", "Rename this library shape (its guides keep it)",
                                                 self.start_rename)
        self.rename_shape.setObjectName("tubeShapeRename")
        self.duplicate_shape = widgets.glyph_button("duplicate", "A copy of this shape in the groom's library, to "
                                                    "change on its own (the selected guides take it): how a built-in "
                                                    "shape is edited", self.duplicate_library_shape)
        self.duplicate_shape.setObjectName("tubeShapeDuplicate")
        self.delete_shape = widgets.glyph_button("delete", self.DELETE_TIP, self.delete_library_shape)
        self.delete_shape.setObjectName("tubeShapeDelete")
        # The library's actions: kept as buttons (scripts and tests click them), shown in the ⋯ menu.
        for glyph in (self.new_shape, self.rename_shape, self.duplicate_shape, self.delete_shape):
            glyph.setParent(self)
            glyph.setVisible(False)
        # The Tube card's Profile (the Simple layout): untitled; Root, Mid and Tip as tiles with
        # their outline (the one picked shows in the editor); the editor beside the picked profile's
        # shape, Apply, Hold and the outline's Mirror / Smooth; a hint and the library's ⋯ under them.
        self.profile_block = widgets.Block("Profile", name="tubeProfile")
        self.profile_block.set_titled(False)
        self.profile_block.setToolTip(_setting("tube", "hint"))
        self.profile_labels = {}
        self.apply_buttons = {}
        self.profile_rows = {}
        self.profile_pages = {}
        self.hold_sliders = {}
        self.profile_group = QtWidgets.QButtonGroup(self)  # exclusive: one profile in the editor
        tiles = QtWidgets.QHBoxLayout()
        tiles.setContentsMargins(0, 0, 0, 0)
        tiles.setSpacing(theme.px(4))
        self.profile_block.add(tiles)
        self.editor = TubeShapeEditor()
        self.editor.setFixedSize(theme.px(132), theme.px(132))
        self.editor.dragged.connect(self.shape_dragged)
        self.editor.committed.connect(self.shape_committed)
        body = QtWidgets.QHBoxLayout()
        body.setContentsMargins(0, 0, 0, 0)
        body.setSpacing(theme.px(8))
        body.addWidget(self.editor, 0, QtCore.Qt.AlignTop)
        side = QtWidgets.QVBoxLayout()
        side.setContentsMargins(0, 0, 0, 0)
        side.setSpacing(theme.px(4))
        body.addLayout(side, 1)
        self.profile_block.add(body)
        for key, label in (("root", "Root"), ("mid", "Mid"), ("tip", "Tip")):
            tile = QtWidgets.QToolButton()
            tile.setObjectName("tubeProfile_" + key)
            tile.setText(label)
            tile.setToolTip("Show the %s profile's shape in the editor" % label.lower())
            tile.setCheckable(True)
            tile.setChecked(key == "root")
            tile.setProperty("role", "pill")
            tile.setProperty("tile", True)
            tile.setToolButtonStyle(QtCore.Qt.ToolButtonTextUnderIcon)
            tile.setIconSize(QtCore.QSize(theme.px(22), theme.px(22)))
            tile.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
            tile.clicked.connect(lambda _=False, k=key: self.show_profile(k))
            self.profile_group.addButton(tile)
            tiles.addWidget(tile, 1)
            self.profile_labels[key] = tile
            # The picked profile's page beside the editor: its shape (the root's picker: self.shape,
            # moved here by set_mode), Apply and Hold.
            page = QtWidgets.QWidget()
            column = QtWidgets.QVBoxLayout(page)
            column.setContentsMargins(0, 0, 0, 0)
            column.setSpacing(theme.px(4))
            if key != "root":
                column.addWidget(self.profile_combos[key])
            button = QtWidgets.QPushButton("Apply")
            button.setObjectName({"root": "tubeShapeApply", "mid": "tubeMidShapeApply", "tip": "tubeTipShapeApply"}[key])
            button.setToolTip("Give the selected guides this %s profile (every one selected)" % label.lower())
            button.clicked.connect(lambda _=False, k=key: self.apply_profile(k))
            column.addWidget(button)
            slider = widgets.SoftSlider(0, 0, 100, integer=True, minimum=0, maximum=100, key=None, label="Hold",
                                        suffix="%")
            slider.setObjectName("tube%sLength" % label)
            slider.setToolTip("How much of the tube's length is exactly the %s profile's shape (%s); 0%%: only "
                              "at its own point. Between these stretches the shape changes smoothly"
                              % (label.lower(), {"root": "from the root", "mid": "centered on the middle",
                                                 "tip": "up to the tip"}[key]))
            slider.committed.connect(lambda v, k=key: self.set_hold(k, v))
            column.addWidget(slider)
            page.setVisible(key == "root")
            side.addWidget(page)
            self.profile_pages[key] = page
            self.apply_buttons[key] = button
            self.profile_rows[key] = column
            self.hold_sliders[key] = slider
        self.profile_row = self.profile_rows["root"]
        self.apply_shape = self.apply_buttons["root"]
        self.mirror = widgets.chip("Mirror", "Keep the outline's left and right sides alike (about the up axis): a "
                                   "point and its mirror image move, come and go together", True, "tubeShapeMirror")
        self.mirror.toggled.connect(self.set_mirror)
        self.smooth_shape = widgets.chip("Smooth", "A smooth curve through the points; off: straight edges and sharp "
                                         "corners", True, "tubeShapeSmooth")
        self.smooth_shape.toggled.connect(self.set_smooth)
        # The outline's options: a library shape's (a built-in is read-only).
        self.shape_options = widgets.pill_row((self.mirror, self.smooth_shape))
        side.addWidget(self.shape_options)
        side.addStretch(1)
        # What the targets keep (the core's tube targets): root tubes their drawn root, split levels
        # their own Mid and Tip.
        self.profile_note = widgets.small_label("", wrap=True)
        self.profile_note.setObjectName("tubeProfileNote")
        self.profile_note.setVisible(False)
        self.profile_block.add(self.profile_note)
        # No targets: the profiles the groom's guides take (picked, one is edited for all of them).
        self.in_use = widgets.small_label("", wrap=True)
        self.in_use.setObjectName("tubeInUse")
        self.profile_block.add(self.in_use)
        foot = QtWidgets.QHBoxLayout()
        foot.setContentsMargins(0, 0, 0, 0)
        foot.setSpacing(theme.px(4))
        self.shape_hint = widgets.small_label("", wrap=True)
        self.shape_hint.setObjectName("tubeShapeHint")
        foot.addWidget(self.shape_hint, 1)
        self.library_menu = widgets.menu_button("The shape library: rename, new, duplicate, delete",
                                                self.fill_library_menu)
        self.library_menu.setObjectName("tubeShapeLibrary")
        foot.addWidget(self.library_menu, 0, QtCore.Qt.AlignTop)
        self.profile_block.add(foot)
        self.rename_field = QtWidgets.QLineEdit()
        self.rename_field.setObjectName("tubeShapeName")
        self.rename_field.setPlaceholderText("New name, then Enter (Esc: keep it)")
        self.rename_field.returnPressed.connect(self.finish_rename)
        self.rename_field.installEventFilter(self)
        self.rename_field.hide()
        self.profile_block.add(self.rename_field)

        # Selected guides: their Width, Flatness and Twist; Smooth (the channel the brush paints, or
        # all three: its arrow's menu) in the header.
        self.smooth_what = QtWidgets.QComboBox(self)  # the choice Smooth's menu sets (not shown)
        self.smooth_what.setObjectName("tubeSmoothChannels")
        self.smooth_what.setVisible(False)
        self.smooth = QtWidgets.QToolButton()
        self.smooth.setText("Smooth")
        self.smooth.setObjectName("tubeSmooth")
        self.smooth.setProperty("role", "chip")
        self.smooth.setToolTip("Smooth the selected guides' tubes along each guide: what the brush painted evened "
                               "out, the root's and tip's sections kept. " + definitions.get("choices/tubeSmooth")["tip"])
        self.smooth.clicked.connect(self.smooth_selected)
        smooth_menu = QtWidgets.QMenu(self.smooth)
        for value, label, _tip in _options("tubeSmooth"):
            self.smooth_what.addItem(label, value)
            action = smooth_menu.addAction(label)
            action.setCheckable(True)
            action.setData(value)
            action.triggered.connect(lambda _=False, v=value: self.smooth_what.setCurrentIndex(self.smooth_what.findData(v)))
        self.smooth_what.currentIndexChanged.connect(
            lambda i: [a.setChecked(a.data() == self.smooth_what.itemData(i)) for a in smooth_menu.actions()])
        if smooth_menu.actions():
            smooth_menu.actions()[0].setChecked(True)
        self.smooth.setMenu(smooth_menu)
        self.smooth.setPopupMode(QtWidgets.QToolButton.MenuButtonPopup)
        self.smooth_menu = smooth_menu
        self.selected_block = widgets.Block("Selected tubes", (self.smooth,), name="tubeSelected")
        self.selected_block.set_foldable("shape/selectedTubes")  # (folded, as the mock: "Selected tubes · 12")
        self.selected_block.set_folded(True)
        self.targets = widgets.small_label("", wrap=True)
        self.selected_block.add(self.targets)

        # Tube (Place, Draw): what new guides get.
        self.tube_block = widgets.Block(_setting("tube", "label"), name="tubeNew")
        self.tube_block.setToolTip("The tube new guides get: the shape always, the sections where no guides are "
                                   "near to blend them from")
        self.preview = TubeShapeEditor()
        self.preview.setObjectName("tubePreview")
        self.preview.setMinimumSize(theme.px(56), theme.px(56))
        self.preview.setFixedSize(theme.px(56), theme.px(56))
        self.preview.setAttribute(QtCore.Qt.WA_TransparentForMouseEvents, True)
        self.preview.setToolTip("")
        top = QtWidgets.QHBoxLayout()
        top.setContentsMargins(0, 0, 0, 0)
        top.setSpacing(theme.px(8))
        top.addWidget(self.preview)
        self.tube_column = QtWidgets.QVBoxLayout()
        self.tube_column.setContentsMargins(0, 0, 0, 0)
        self.tube_column.setSpacing(theme.px(4))
        self.profile_hint = widgets.small_label(definitions.text("profileHint"), wrap=True)
        self.tube_column.addWidget(self.profile_hint)
        top.addLayout(self.tube_column, 1)
        self.tube_block.add(top)
        self.tube_fields = QtWidgets.QVBoxLayout()
        self.tube_fields.setContentsMargins(0, 0, 0, 0)
        self.tube_fields.setSpacing(theme.px(4))
        self.tube_block.add(self.tube_fields)

        self.blocks = (self.profile_block, self.selected_block, self.tube_block)
        self._disarm = QtCore.QTimer(self)
        self._disarm.setSingleShot(True)
        self._disarm.setInterval(4000)
        self._disarm.timeout.connect(self.disarm_delete)
        self._armed = False
        self._preview = QtCore.QTimer(self)
        self._preview.setSingleShot(True)
        self._preview.setInterval(self.PREVIEW_MS)
        self._preview.timeout.connect(self.send_preview)
        self._previewing = False
        # What needs targets (Selected guides' edits and Apply).
        self.edits = (self.smooth,) + tuple(self.apply_buttons.values()) + tuple(self.hold_sliders.values()) + self.fields
        self._selection = []
        self._source = "none"
        self._reference = 0.0  # the first target's widest section (scene units)
        # The profiles picked (root, mid, tip; "" an unset mid or tip): the targets', else the last
        # picked; the editor shows the active one's (_choice).
        self._choices = {"root": "round", "mid": "", "tip": ""}
        self._active = "root"
        self._in_use = []
        # The tube Place and Draw give new guides (the Tube block's: a shape, its width in scene
        # units, the sections' aspect and twist in radians).
        self._placement = {"shape": "round", "width": 0.1, "aspect": 1.0, "twist": 0.0}
        self._library = []      # the groom's custom shapes (groomlab.tube_shapes' non-built-ins)
        self._owned = {}        # the root tubes' drawn roots, by name (no picker lists them)
        self._root_targets = 0  # of the targets: root tubes, and tubes in split levels (tube_targets)
        self._split_targets = 0
        self._groom = None
        self._pushed = None     # the placement tube last sent to the brush
        self._targets_key = None  # the targets and their shapes as last read
        self.set_mode("edit")
        self.fill_shapes()
        self.show_choice()

    DELETE_TIP = "Remove this shape from the library: click twice (its guides become round)"

    def set_mode(self, mode):
        """"new" (Place, Draw: the Tube block) or "edit" (Shape, Select: Profile and Selected
        guides): the picker and the fields move to that block."""
        if mode == self.mode and self.shape.parent() is not None and self.shape.parent() is not self:
            return
        self.mode = mode
        if mode == "new":
            self.tube_column.insertWidget(0, self.shape)
            for field in self.fields:
                self.tube_fields.addWidget(field)
        else:
            self.profile_row.insertWidget(0, self.shape)
            for field in self.fields:
                self.selected_block.add(field)
        self._targets_key = None
        self.show_values()
        self.show_choice()

    # -- Reading the groom ---------------------------------------------------------

    def busy(self):
        """A slider held, a field being typed in or a point dragged: refresh() leaves the panel as it is."""
        for slider in self.fields + tuple(self.hold_sliders.values()):
            if slider.busy():
                return True
        return self.editor.busy() or self.rename_field.isVisible()

    def refresh(self, groom, full=True, force=False):
        """The shape library and, editing ("edit"), the targets' tubes (each guide has its own: the
        blocks edit the targets only): the shape shown is the targets', "Mixed" when they differ;
        width, flatness and twist the first target's. Left alone while the artist is busy in it
        (busy()); `force` (after the panel's own edit): only not mid-drag."""
        if self.editor.busy() if force else self.busy():
            return
        # (Polled: read again only when the guides or their selection changed, or for another
        # groom or mode; each read parses the guides, a hitch every tick on a big groom.)
        from groomlab.ui import groom_watch
        stamp = groom_watch.stamp(groom, ("guideData", "guideSelection"))
        key = (groom, self.mode, stamp)
        if not force and stamp is not None and key == getattr(self, "_read_key", None):
            return
        self._read_key = key
        info = {"source": "none", "guides": [], "tube": None, "shapes": [], "midShapes": [], "tipShapes": [], "inUse": []}
        if groom:
            try:
                if self.mode == "edit":
                    info = groomlab.tube_targets(groom)
                if full or force or groom != self._groom or stamp is not None:
                    shapes = [s for s in groomlab.tube_shapes(groom, owned=True) if not s["builtin"]]
                    self._library = [s for s in shapes if not s.get("owned")]
                    self._owned = dict((s["name"], s) for s in shapes if s.get("owned"))
            except (RuntimeError, ValueError):
                pass
        elif self._groom:
            self._library = []
            self._owned = {}
        self._groom = groom
        self._selection = list(info["guides"])
        self._source = info["source"]
        tube = info["tube"]
        shapes = info["shapes"]
        mids, tips = info.get("midShapes", []), info.get("tipShapes", [])
        self._in_use = info.get("inUse", [])
        self._root_targets = int(info.get("rootTubes", 0))
        self._split_targets = int(info.get("splitTubes", 0))
        self.fill_shapes()
        if self.mode == "new":
            self.show_values()
            self.show_choice()
            self.push_placement()
            return
        for w in self.edits:
            w.setEnabled(tube is not None)
        if tube is None:
            self.targets.setText("No guides selected or soloed: select some (Select, or Shift with a brush).")
            self.selected_block.title.setText("Selected tubes")
            for combo in self.profile_combos.values():
                self.show_mixed(combo, False)
        else:
            count = len(self._selection)
            how = "selected" if self._source == "selection" else "soloed"
            self.targets.setText("%d guide%s %s." % (count, "" if count == 1 else "s", how))
            self.selected_block.title.setText("Selected tubes · %d" % count)
            # Width, Flatness, Twist: the core's (tube_card_values, every host's).
            card = groomlab_core.tube_card_values(tube.get("width") or [], tube.get("aspect") or [], tube.get("twist") or [])
            self._reference = card["width"]
            for w in tuple(self.profile_combos.values()) + self.fields:
                w.blockSignals(True)
            # The choices follow the targets when they (or their profiles) change, and are left
            # alone otherwise (a shape made or picked in the editor while they stay selected).
            key = (tuple(self._selection), tuple(shapes), tuple(mids), tuple(tips))
            if key != self._targets_key:
                for profile, among in (("root", shapes), ("mid", mids), ("tip", tips)):
                    if len(among) == 1:
                        self._choices[profile] = among[0]
            elif len(shapes) == 1 and shapes[0] in self._owned:
                self._choices["root"] = shapes[0]  # (a Root profile refused for a root tube: its own again)
            self._targets_key = key
            for profile, slider in self.hold_sliders.items():
                if not slider.busy():
                    slider.blockSignals(True)
                    slider.setValue(round(float(tube.get(profile + "Hold", 0.0)) * 100.0))  # (shown in percent)
                    slider.blockSignals(False)
            self.width.setValue(self._reference * self.length_factor)
            self.flatness.setValue(card["flatness"])
            self.twist.setValue(card["twist"] * 180.0 / 3.14159265358979)
            for w in tuple(self.profile_combos.values()) + self.fields:
                w.blockSignals(False)
        mixed = {"root": len(shapes) > 1, "mid": len(mids) > 1, "tip": len(tips) > 1} if tube is not None else None
        self.show_choice(mixed=mixed)
        self.push_placement()

    def show_values(self):
        """New guides' tube ("new"): the fields show the placement tube."""
        if self.mode != "new":
            return
        p = self._placement
        for w in self.fields:
            w.blockSignals(True)
        self.width.setValue(p["width"] * self.length_factor)
        self.flatness.setValue(min(max(1.0 - p["aspect"], 0.0), 0.99))
        self.twist.setValue(p["twist"] * 180.0 / 3.14159265358979)
        for w in self.fields:
            w.blockSignals(False)
            w.setEnabled(True)
        self.show_mixed(self.shape, False)

    MIXED = "*"  # a picker's "Mixed" entry (no shape is named so)
    DRAWN = "@drawn"  # the root picker's "Drawn root" entry: a root tube's own (no shape is named so)
    UNSET = {"mid": ("Between", "Halfway between the root's profile and the tip's"),
             "tip": ("As root", "The root's profile again at the tip")}

    def fill_shapes(self):
        """The shape pickers: the mid's and the tip's "unset" entry first, the built-ins, then the
        library (each rebuilt only when that changes)."""
        names = [name for name, _, _ in TUBE_SHAPES] + [s["name"] for s in self._library]
        for profile, combo in self.profile_combos.items():
            unset = profile in self.UNSET
            have = [combo.itemData(i) for i in range(combo.count())
                    if combo.itemData(i) is not None and combo.itemData(i) not in (self.MIXED, self.DRAWN)]
            if have == ([""] if unset else []) + names:
                continue
            mixed = combo.count() > 0 and combo.itemData(0) == self.MIXED
            combo.blockSignals(True)
            combo.clear()  # (Drawn root: back with show_choice)
            if mixed:
                combo.addItem("Mixed", self.MIXED)
            if unset:
                label, tip = self.UNSET[profile]
                combo.addItem(label, "")
                combo.setItemData(combo.count() - 1, tip, QtCore.Qt.ToolTipRole)
            for name, label, tip in TUBE_SHAPES:
                combo.addItem(label, name)
                combo.setItemData(combo.count() - 1, tip, QtCore.Qt.ToolTipRole)
            if self._library:
                combo.insertSeparator(combo.count())
            for s in self._library:
                combo.addItem(s["name"], s["name"])
                combo.setItemData(combo.count() - 1, "A custom shape of this groom's library (%d guide%s)" %
                                  (s["users"], "" if s["users"] == 1 else "s"), QtCore.Qt.ToolTipRole)
            combo.blockSignals(False)

    @property
    def _choice(self):
        """The profile the editor shows: the active picker's (root, mid or tip)."""
        return self._choices[self._active]

    @_choice.setter
    def _choice(self, name):
        self._choices[self._active] = name

    def library_shape(self, name):
        for s in self._library:
            if s["name"] == name:
                return s
        return None

    def _known(self, name):
        return (self.library_shape(name) is not None or name in self._owned
                or name in [n for n, _, _ in TUBE_SHAPES])

    def _outline(self, name):
        """(points, smooth) of a shape: the library's (a root tube's drawn root too), else the built-in's."""
        custom = self.library_shape(name) or self._owned.get(name)
        if custom is not None:
            return custom["points"], custom["smooth"]
        import groomlab_core
        return groomlab_core.builtin_tube_shape_points(name)

    def show_choice(self, mixed=None):
        """The pickers, the editor (or the Tube block's preview) and the library glyphs on the
        chosen shapes; `mixed`: {profile: whether the targets' differ} ("Mixed" shown)."""
        mixed = mixed or {}
        for profile in self.profile_combos:
            name = self._choices[profile]
            if not self._known(name) and not (profile != "root" and name == ""):
                # (A library shape gone: renamed, deleted, another groom.)
                self._choices[profile] = "round" if profile == "root" else ""
        if not self._known(self._placement["shape"]):
            self._placement["shape"] = "round"
        for profile, combo in self.profile_combos.items():
            shown = self._placement["shape"] if self.mode == "new" and profile == "root" else self._choices[profile]
            # (A root tube's drawn root: no library shape, "Drawn root" its entry.)
            drawn = self.mode != "new" and profile == "root" and shown in self._owned and not mixed.get(profile)
            self.show_drawn(combo, drawn)
            self.show_mixed(combo, bool(mixed.get(profile)))
            combo.blockSignals(True)
            combo.setCurrentIndex(0 if mixed.get(profile) or drawn else max(0, combo.findData(shown)))
            combo.blockSignals(False)
        for profile, button in self.profile_labels.items():
            button.blockSignals(True)
            button.setChecked(profile == self._active)
            button.blockSignals(False)
            self.profile_pages[profile].setVisible(profile == self._active)
            name = self._choices[profile]
            if name and name != self.MIXED and self._known(name):
                points, smooth = self._outline(name)
                button.setIcon(shape_icon(points, theme.current()))
            else:
                button.setIcon(shape_icon(None, theme.current()))
        notes = []
        if self._selection and self._root_targets:
            n = self._root_targets
            notes.append("%d root tube%s keep%s %s drawn root: a Root profile goes to the other guides (Roots reshapes "
                         "a root)." % (n, "" if n == 1 else "s", "s" if n == 1 else "", "its" if n == 1 else "their"))
        if self._selection and self._split_targets:
            notes.append("Mid and tip don't carry into split levels.")
        self.profile_note.setText(" ".join(notes))
        self.profile_note.setVisible(bool(notes))
        if self._selection:
            self.in_use.setVisible(False)
        else:
            listed = ", ".join("%s (%d)" % (u["name"], u["guides"]) for u in self._in_use)
            self.in_use.setText("Profiles in this groom: %s. Pick one to edit it: every guide taking it follows."
                                % listed if listed else "No guides take a profile yet.")
            self.in_use.setVisible(bool(self._groom))
        points, smooth = self._outline(self._placement["shape"])
        self.preview.set_shape(points, smooth, editable=False)
        custom = self.library_shape(self._choice)
        if self._choice == "":
            # An unset mid or tip: the root's outline, read-only.
            points, smooth = self._outline(self._choices["root"])
            note = "Between the root's and the tip's" if self._active == "mid" else "The root's again"
            self.editor.set_shape(points, smooth, editable=False, note=note)
        elif self._choice in self._owned:
            # A root tube's drawn root: read-only here (the Roots brush reshapes it).
            points, smooth = self._outline(self._choice)
            self.editor.set_shape(points, smooth, editable=False, note="Drawn root: the Roots brush reshapes it")
        elif custom is not None:
            self.editor.set_shape(custom["points"], custom["smooth"], editable=True)
            # Mirror only for the edits to come: showing a shape never changes it. (Made symmetric
            # and saved here, at every tick of the panel's poll, it rewrote the shape, an
            # asymmetric one modeled or converted included, and evaluated the groom again and
            # again: the viewport lagged in the Tube tool.) Turning Mirror on is what mirrors it.
            self.editor.mirror = self.mirror.isChecked()
        else:
            points, smooth = self._outline(self._choice)
            self.editor.set_shape(points, smooth, editable=False, note="Built in: Duplicate it to edit a copy")
        self.smooth_shape.blockSignals(True)
        self.smooth_shape.setChecked(self.editor.smooth)
        self.smooth_shape.blockSignals(False)
        has_groom = bool(self._groom)
        self.shape_options.setVisible(custom is not None)
        self.shape_hint.setText("Drag a point to edit; ⋯ for the library" if custom is not None
                                else "⋯: duplicate it to edit a copy, or a new shape")
        self.new_shape.setEnabled(has_groom)
        self.duplicate_shape.setEnabled(has_groom)
        for b in (self.rename_shape, self.delete_shape):
            b.setEnabled(custom is not None)
        for b in self.apply_buttons.values():
            b.setEnabled(bool(self._selection))

    def fill_library_menu(self, menu_):
        """The shape library's ⋯ menu (Rename first, Delete last in red), as its buttons stand."""
        for text, button in (("Rename...", self.rename_shape), ("New Shape", self.new_shape),
                             ("Duplicate", self.duplicate_shape)):
            action = menu_.addAction(text, button.click)
            action.setEnabled(button.isEnabled())
            action.setToolTip(button.toolTip())
        menu_.addSeparator()
        widgets.danger_action(menu_, "Delete Shape", self.delete_from_menu, self.DELETE_TIP.split(":")[0],
                              self.delete_shape.isEnabled())

    def delete_from_menu(self):
        """Delete Shape from the menu: picking it is the confirmation (one undo step)."""
        self._armed = True
        self.delete_library_shape()

    def show_drawn(self, combo, on):
        """The root picker's "Drawn root" entry (first): there while the targets are root tubes, whose
        root is their own drawn outline (no library shape: the core's isOwnedTubeShape)."""
        at = combo.findData(self.DRAWN)
        if on and at < 0:
            combo.blockSignals(True)
            combo.insertItem(0, "Drawn root", self.DRAWN)
            combo.setItemData(0, "The root tube's own outline, drawn on the scalp: the Roots brush reshapes it; "
                                 "a Root profile picked goes to the other guides", QtCore.Qt.ToolTipRole)
            combo.blockSignals(False)
        elif not on and at >= 0:
            combo.blockSignals(True)
            combo.removeItem(at)
            combo.blockSignals(False)

    def show_mixed(self, combo, on):
        """A shape picker's "Mixed" entry (first, no shape of its own): there while the targets'
        shapes differ."""
        at = combo.findData(self.MIXED)
        if on and at < 0:
            combo.blockSignals(True)
            combo.insertItem(0, "Mixed", self.MIXED)
            combo.setItemData(0, "The selected guides have different shapes here: pick one to give them all",
                              QtCore.Qt.ToolTipRole)
            combo.blockSignals(False)
        elif not on and at >= 0:
            combo.blockSignals(True)
            combo.removeItem(at)
            combo.blockSignals(False)

    # -- The placement tube ------------------------------------------------------------

    def placement(self):
        """The tube Place and Draw give new guides: the Tube block's shape, Width, Flatness and Twist."""
        return dict(self._placement)

    def push_placement(self):
        """The placement tube sent to the brush (when it changed)."""
        tube = self.placement()
        if tube == self._pushed:
            return
        from maya import cmds
        import json
        if not cmds.contextInfo(groomlab.BRUSH_CONTEXT, exists=True):
            return
        try:
            cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, edit=True, placeTube=json.dumps(tube))
            self._pushed = tube
        except RuntimeError as e:
            self.owner.status.setText(str(e).strip())

    # -- Edits -----------------------------------------------------------------------

    def run(self, fn, *args, **kwargs):
        """fn(*args, **kwargs), its error said in the status line; the panel read again after.
        Returns whether it worked."""
        ok = True
        try:
            fn(*args, **kwargs)
        except (RuntimeError, ValueError) as e:
            self.owner.status.setText(str(e).strip())
            ok = False
        self.refresh(self.owner.groom(), force=True)
        return ok

    # The set_tube argument each profile's picker sets.
    PROFILE_ARGS = {"root": "shape", "mid": "mid_shape", "tip": "tip_shape"}

    def pick(self, name, profile="root"):
        """A shape picked for a profile (root, mid or tip; "" an unset mid or tip): new guides'
        ("new", the root's), else given to the targets (one undo step); the editor shows it."""
        if name is None or name in (self.MIXED, self.DRAWN):  # "Mixed", "Drawn root": nothing to give
            return
        if self.mode == "new":
            if profile == "root":
                self._placement["shape"] = name
            self.show_choice()
            self.push_placement()
            return
        self._active = profile
        self._choice = name
        if self._selection:
            self.apply(**{self.PROFILE_ARGS[profile]: name})
        self.show_choice()

    def show_profile(self, profile):
        """A profile's button: the editor shows that profile's shape (nothing given to the targets)."""
        self._active = profile
        self.show_choice()

    def set_hold(self, profile, value):
        """A Length slider let go (percent): the share of the targets' tubes keeping that profile's
        exact shape (set_tube's holds: one undo step)."""
        groom = self.owner.groom()
        if groom and self._selection:
            self.run(groomlab.set_tube, groom, guides=self._selection, **{profile + "_hold": float(value) / 100.0})

    def apply_profile(self, profile):
        """A profile's Apply: its picked shape and its Length given to every target (one undo step;
        while the targets' shapes there are Mixed, the Length alone)."""
        name = self._choices[profile]
        self._active = profile
        edit = {profile + "_hold": self.hold_sliders[profile].value() / 100.0}
        if name != self.MIXED and name not in self._owned and not (profile == "root" and name == ""):
            edit[self.PROFILE_ARGS[profile]] = name
        self.apply(**edit)

    def apply(self, **edit):
        """An edit of the targets' tubes (set_tube: one undo step); for new guides ("new"), the
        placement tube's."""
        if edit.get("shape") == "" or self.MIXED in edit.values():
            return
        if self.mode == "new":
            self._placement.update(edit)
            self.push_placement()
            return
        groom = self.owner.groom()
        if groom and self._selection:
            self.run(groomlab.set_tube, groom, guides=self._selection, **edit)

    def set_width(self, value):
        """The width field: the targets scaled so the first one's widest section is `value` (new
        guides: their width)."""
        width = value / self.length_factor
        if self.mode == "new":
            self._placement["width"] = width
            self.push_placement()
            return
        groom = self.owner.groom()
        if groom and self._selection:
            if self._reference > 0.0:
                self.run(groomlab.scale_tube, groom, width / self._reference, guides=self._selection)
            else:
                self.run(groomlab.set_tube, groom, guides=self._selection, width=width)

    def smooth_selected(self):
        """Smooth: the targets' channel the brush paints (or all three) evened out along each
        guide (smooth_tube: one undo step)."""
        groom = self.owner.groom()
        if not groom or not self._selection:
            return
        channel = self.owner.tube_channel_name() if self.smooth_what.currentData() == "current" else "all"
        self.run(groomlab.smooth_tube, groom, guides=self._selection, channel=channel)

    def unique_name(self, base):
        """"<base> 1", "<base> 2"...: no built-in or library shape's (tube.h freeTubeShapeName, every host's)."""
        import groomlab_core
        return groomlab_core.free_tube_shape_name(base, [s["name"] for s in self._library])

    def edit_as_custom(self):
        """The chosen built-in copied into the library as a new shape, chosen (the targets take it:
        one undo step)."""
        groom = self.owner.groom()
        if not groom:
            return
        base = self._choice or self._choices["root"]  # (an unset mid or tip: the root's)
        points, smooth = self._outline(base)
        # (A drawn root's copy: a library shape of its own, not named as root tubes' are.)
        name = self.unique_name("root" if base in self._owned else base)
        from groomlab.base import undo_chunk
        try:
            with undo_chunk("tube shape"):
                groomlab.add_tube_shape(groom, name, points, smooth=smooth)
                if self._selection:
                    groomlab.set_tube(groom, guides=self._selection, **{self.PROFILE_ARGS[self._active]: name})
        except (RuntimeError, ValueError) as e:
            self.owner.status.setText(str(e).strip())
            return
        self._choice = name
        self.refresh(groom, force=True)
        self.owner.status.setText("%s: an editable copy (drag its points)." % name)

    def new_library_shape(self):
        groom = self.owner.groom()
        if not groom:
            return
        import groomlab_core
        points, _ = groomlab_core.builtin_tube_shape_points("round")
        name = self.unique_name("shape")
        if self.run(groomlab.add_tube_shape, groom, name, points, smooth=True):
            self._choice = name
            self.refresh(groom, force=True)

    def duplicate_library_shape(self):
        """A copy of the shape in the library, chosen: a built-in's is how it is edited."""
        groom = self.owner.groom()
        custom = self.library_shape(self._choice)
        if groom and custom is None:
            self.edit_as_custom()
            return
        if not groom:
            return
        name = self.unique_name(custom["name"])
        if self.run(groomlab.add_tube_shape, groom, name, custom["points"], smooth=custom["smooth"]):
            self._choice = name
            self.refresh(groom, force=True)

    def start_rename(self):
        """Rename: a name field in place (Enter renames, Esc or leaving it keeps the name); no dialog."""
        if self.library_shape(self._choice) is None:
            return
        self.rename_field.setText(self._choice)
        self.rename_field.show()
        self.rename_field.setFocus()
        self.rename_field.selectAll()

    def finish_rename(self):
        groom = self.owner.groom()
        new = self.rename_field.text().strip()
        old = self._choice
        self.hide_rename()
        if not groom or not new or new == old:
            return
        # The profiles (and the placement) on the shape, taken before the rename: run's refresh
        # finds the old name gone and sets them back to round.
        using = [profile for profile, name in self._choices.items() if name == old]
        placed = self._placement["shape"] == old
        if self.run(groomlab.rename_tube_shape, groom, old, new):
            for profile in using:
                self._choices[profile] = new
            if placed:
                self._placement["shape"] = new
            self.refresh(groom, force=True)

    def eventFilter(self, obj, event):
        if obj is self.rename_field and event.type() == QtCore.QEvent.KeyPress and event.key() == QtCore.Qt.Key_Escape:
            self.hide_rename()
            return True
        if obj is self.rename_field and event.type() == QtCore.QEvent.FocusOut and self.rename_field.isVisible():
            self.rename_field.hide()
        return super(TubePanel, self).eventFilter(obj, event)

    def hide_rename(self):
        """The name field gone, the focus on the editor (not passed on to a slider's field, which
        would read as typing there)."""
        self.editor.setFocus()
        self.rename_field.hide()

    def delete_library_shape(self):
        """Delete: the first click arms the glyph (its tooltip, and the status line, say how many
        guides go round), a second one within a few seconds deletes (one undo step); no dialog."""
        groom = self.owner.groom()
        custom = self.library_shape(self._choice)
        if not groom or custom is None:
            return
        if not self._armed:
            self._armed = True
            users = custom["users"]
            text = "Delete? %d go%s round" % (users, "es" if users == 1 else "")
            self.delete_shape.setText(text)
            self.delete_shape.setToolTip(text + ": click again to delete")
            self.delete_shape.setProperty("role", "danger")
            widgets.repolish(self.delete_shape)
            self.owner.status.setText(text + ": click the delete glyph again.")
            self._disarm.start()
            return
        self.disarm_delete()
        if self.run(groomlab.remove_tube_shape, groom, custom["name"]):
            for profile in self._choices:
                if self._choices[profile] == custom["name"]:
                    self._choices[profile] = "round" if profile == "root" else ""
            self.refresh(groom, force=True)

    def disarm_delete(self):
        self._armed = False
        self._disarm.stop()
        self.delete_shape.setText("Delete")
        self.delete_shape.setToolTip(self.DELETE_TIP)
        self.delete_shape.setProperty("role", "glyph")
        widgets.repolish(self.delete_shape)

    # -- The editor ------------------------------------------------------------------

    def shape_dragged(self, points):
        """A point moving: the groom shows the shape so, at most every PREVIEW_MS (no undo step)."""
        if not self._preview.isActive():
            self._preview.start()

    def send_preview(self):
        groom = self.owner.groom()
        if not groom or self.editor.problem or self.library_shape(self._choice) is None:
            return
        try:
            groomlab.preview_tube_shape(groom, self._choice, self.editor.points, smooth=self.editor.smooth)
            self._previewing = True
        except (RuntimeError, ValueError):
            pass

    def shape_committed(self, points):
        """An edit settled: the library shape takes the points (one undo step; every guide taking it
        follows). Refused (crossing itself, leaving out its center): said, the shape as it was."""
        self._preview.stop()
        groom = self.owner.groom()
        custom = self.library_shape(self._choice)
        if not groom or custom is None:
            return
        try:
            groomlab.set_tube_shape_points(groom, custom["name"], points, smooth=self.editor.smooth)
            self.owner.status.setText("")
        except (RuntimeError, ValueError) as e:
            self.owner.status.setText(str(e).strip())
            if self._previewing:
                groomlab.end_guide_preview(groom)
        self._previewing = False
        self.refresh(groom, force=True)

    def set_mirror(self, on):
        if self.editor.set_mirror(on) and self.library_shape(self._choice) is not None:
            self.shape_committed(self.editor.points)
        self.editor.update()

    def set_smooth(self, on):
        groom = self.owner.groom()
        custom = self.library_shape(self._choice)
        if groom and custom is not None:
            self.run(groomlab.set_tube_shape_points, groom, custom["name"], smooth=on)


class GuideDisplay(widgets.PopupPanel):
    """What the viewport shows of the groom's guides, under the header's guides arrow: one row of
    what shows (Guides, Parts, Flow, Tubes, Instances), then how the guides draw (width, points,
    colors, smoothed, on top), then how the tubes draw (add_tubes: the look)."""

    SHOWN = (("displayGuides", "Guides", "The guides (with their part lines and flow; the eye beside the arrow)"),
             ("displayParts", "Parts", "The part lines the Part tool draws"),
             ("displayFlow", "Flow", "The flow arrows and sinks"),
             ("displayTubes", "Tubes", "Each guide's tube with the guides, drawn with every tool as set below (the "
                                       "Tube and Roots tools show them either way)"),
             ("displayInstances", "Instances", "Instanced meshes on the strands"))
    LOOK = (("guideSmoothing", "Smoothed", "Guides draw as a smooth curve through their points"),
            ("displayOverlaysOnTop", "On top", "Guides, parts and flow draw over the strands"))
    POINTS = (("Off", "No dots"), ("Ends", "A dot at each guide's root and tip"), ("All", "A dot at every point"))
    COLORS = (("Group", "Each guide group's own color"), ("Layer", "A painted guide layer's values, black to white"))

    def __init__(self, owner):
        super(GuideDisplay, self).__init__(owner, width=372)
        self.owner = owner
        self.setObjectName("guideDisplayPopup")
        self.chips = {}
        for attr, label, tip in self.SHOWN + self.LOOK:
            b = widgets.chip(label, tip, False, "display_" + attr)
            # (Smoothed: guideSmoothing's pieces per segment, 4 on, 1 straight.)
            b.toggled.connect(lambda on, a=attr: self.set_attr(a, (4 if on else 1) if a == "guideSmoothing" else bool(on)))
            self.chips[attr] = b
        self.add(widgets.pill_row([self.chips[a] for a, _l, _t in self.SHOWN]))
        self.guides_block = self.add(widgets.Block("Guides", name="guideLook"))
        self.width_field = widgets.SoftSlider(2.0, 1.0, 6.0, minimum=0.5, maximum=20.0, key=None, label="Width",
                                              suffix="px")
        self.width_field.setToolTip("The guides' width in pixels")
        self.width_field.committed.connect(lambda v: self.set_attr("guideWidth", float(v)))
        self.guides_block.add(self.width_field)
        self.points = self._segments("Points", "guidePoints", self.POINTS)
        self.colors = self._segments("Colors", "guideColorMode", self.COLORS)
        self.guides_block.add(widgets.pill_row([self.chips[a] for a, _l, _t in self.LOOK]))
        self.about_to_show.append(self.read)

    def _segments(self, title, attr, items):
        """A setting of a few choices as a track of segments (its index the attribute's value)."""
        group = QtWidgets.QButtonGroup(self)
        group.setExclusive(True)
        buttons = []
        for i, (label, tip) in enumerate(items):
            b = widgets.chip(label, tip, i == 0, "display_%s_%d" % (attr, i))
            group.addButton(b, i)
            buttons.append(b)
        group.buttonClicked.connect(lambda b, a=attr, g=group: self.set_attr(a, int(g.id(b))))
        row, _ = widgets.field_row(title, widgets.seg_row(buttons))
        self.guides_block.add(row)
        return group

    def add_tubes(self, block):
        """The tubes' display (Shaded, Wireframe, Colors, level colors, with every tool)."""
        block.setVisible(True)
        if hasattr(block, "title"):
            block.title.setText("Tubes")
        self.add(block)

    def groom(self):
        return groomlab.active_groom()

    def read(self):
        from maya import cmds
        groom = self.groom()
        if not groom:
            return
        for attr, b in self.chips.items():
            b.blockSignals(True)
            try:
                value = cmds.getAttr(groom + "." + attr)
                b.setChecked(int(value) > 1 if attr == "guideSmoothing" else bool(value))
            except (RuntimeError, ValueError, TypeError):
                pass
            b.blockSignals(False)
        for attr, group in (("guidePoints", self.points), ("guideColorMode", self.colors)):
            try:
                button = group.button(int(cmds.getAttr(groom + "." + attr)))
            except (RuntimeError, ValueError, TypeError):
                button = None
            if button is not None:
                button.setChecked(True)
        try:
            if not self.width_field.busy():
                self.width_field.setValue(float(cmds.getAttr(groom + ".guideWidth")))
        except (RuntimeError, ValueError, TypeError):
            pass

    def set_attr(self, attr, value):
        from maya import cmds
        groom = self.groom()
        if groom:
            try:
                cmds.setAttr(groom + "." + attr, value)
            except RuntimeError as e:
                self.owner.status.setText(str(e))


class _Work(QtWidgets.QWidget):
    """The Groom Tools' work area (the tab's list and the card): shown and hidden with the top's
    part that goes with it (the tabs and the tool grid)."""

    def __init__(self, partner):
        super(_Work, self).__init__()
        self.partner = partner

    def setVisible(self, on):
        super(_Work, self).setVisible(on)
        self.partner.setVisible(on)



class ToolPalette(QtWidgets.QWidget):
    def __init__(self, parent=None):
        super(ToolPalette, self).__init__(parent)
        self.setWindowTitle("GroomLab Groom Tools")
        self.setWindowIcon(widgets.icon("logo"))
        self.tool = None
        self.strengths = dict(DEFAULT_STRENGTH)
        # Each brush's radius, falloff and curves as last left (strength: self.strengths), and the
        # tool whose settings the Brush block shows now (remember_brush, recall_brush).
        self.brushes = {}
        self._brush_shown = None
        self.buttons = {}  # tool -> its tile (the first made; a tool with tiles on two tabs: tiles)
        self.tiles = {}    # tool -> every tile of it

        # One scrolling column, the whole panel: the top (the header, the tabs, the tool grid), then
        # the list the tab works on, then the tool's card; the window paints the theme's background.
        # The page rail along the top (the pages' icons, widgets.Pages: a side column would widen
        # the docked panel), the column under it.
        frame = QtWidgets.QVBoxLayout(self)
        frame.setContentsMargins(0, 0, 0, 0)
        frame.setSpacing(0)
        self.frame = frame
        scroll = QtWidgets.QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        scroll.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
        self.scroll = scroll
        frame.addWidget(scroll, 1)
        content = QtWidgets.QWidget()
        content.setObjectName("groomToolsContent")
        scroll.setWidget(content)
        outer = QtWidgets.QVBoxLayout(content)
        outer.setContentsMargins(theme.px(12), theme.px(10), theme.px(12), theme.px(12))
        outer.setSpacing(theme.px(10))
        self.top_part = QtWidgets.QWidget()
        self.top_part.setObjectName("groomToolsTop")
        top_column = QtWidgets.QVBoxLayout(self.top_part)
        top_column.setContentsMargins(0, 0, 0, 0)
        top_column.setSpacing(theme.px(8))
        outer.addWidget(self.top_part)

        # What the tools work on: the current groom (groomlab.active_groom: the Graph Editor's too
        # unless pinned). Its eyes, the Graph Editor and More go on the page rail, right of the
        # pages (issue 73: in view however far the panel scrolls); the groom's list stays here.
        top = QtWidgets.QHBoxLayout()
        top.setSpacing(theme.px(4))
        self.grooms = QtWidgets.QComboBox()
        header = definitions.get("palette/header")
        self.grooms.setToolTip(header["groom"]["tip"])
        self.grooms.setSizeAdjustPolicy(QtWidgets.QComboBox.AdjustToMinimumContentsLengthWithIcon)
        self.grooms.setMinimumContentsLength(8)
        self.grooms.activated.connect(self.groom_picked)
        top.addWidget(self.grooms, 1)
        # The eyes: the groom's hair (the strands alone), and its guides (every guide group);
        # Shift+click or right-click: every groom's.
        self.visible = self.header_button("eye", header["visible"]["tip"], checkable=True)
        self.visible.setObjectName("hairVisible")
        self.visible.toggled.connect(self.hair_visibility)
        self.eye_menu(self.visible, header["visible"].get("items", []))
        self.guides_eye = self.header_button("guides", header["guides"]["tip"], checkable=True)
        self.guides_eye.setObjectName("guidesVisible")
        self.guides_eye.toggled.connect(self.guides_visibility)
        self.eye_menu(self.guides_eye, header["guides"].get("items", []))
        self.header_icons = {}
        graph = self.header_button(None, header["graph"]["tip"])
        t = theme.current()
        graph.setIcon(widgets.tinted_icon("graphLine", t.token("textDim"), hover=t.color("ui.text"),
                                          disabled=t.token("textDisabled")))
        graph.clicked.connect(self.open_graph)
        self.header_icons["graph"] = graph
        # The guides' eye is a split button: the eye shows or hides them, the arrow beside it opens
        # how they draw (the Attribute Editor's Display > Guides, and the tubes' display).
        self.guides_display = self.header_button("chevronDown", "How the guides draw: width, points, colors, "
                                                 "flow, tubes, instances")
        self.guides_display.setObjectName("guidesDisplay")
        self.guides_display.setFixedWidth(theme.px(14))
        self.guides_display.setIconSize(QtCore.QSize(theme.px(10), theme.px(10)))
        self.display_popup = GuideDisplay(self)
        self.guides_display.clicked.connect(lambda: self.display_popup.show_under(self.guides_display))
        # Find Command: every GroomLab command, brush and graph preset by name.
        find = self.header_button("search", definitions.action("commands.find")["tip"])
        find.setObjectName("findCommand")
        find.clicked.connect(self.find_command)
        more = self.header_button("more", header["more"]["tip"])
        more.setPopupMode(QtWidgets.QToolButton.InstantPopup)
        self.more_menu = QtWidgets.QMenu(more)
        widgets.fill_menu(self.more_menu, header["more"]["items"], definitions.binder(self.ACTIONS, self),
                          generators=self.generators())
        more.setMenu(self.more_menu)
        self.header_buttons = [self.visible, self.guides_eye, self.guides_display, graph, find, more]
        for b in self.header_buttons:
            top.addWidget(b)
        self.header = QtWidgets.QWidget()
        self.header.setObjectName("groomToolsHeader")
        top.setContentsMargins(theme.px(8), theme.px(8), theme.px(6), theme.px(2))
        self.header.setLayout(top)
        # A newer GroomLab (the startup check's): a quiet note linking to GroomLab > Help > Check for Updates.
        from groomlab.ui import updates_window
        self.update_note = updates_window.UpdateNote()
        top_column.addWidget(self.update_note)

        # The group brushes edit and Place and Draw add to: picked in the Guide groups list (its
        # picked row; the viewport's level keys too). The choice itself, not shown: the groups as
        # the brushes take them ("All groups" first), what the list, the poll and Page Up / Down set.
        self.level = QtWidgets.QComboBox(self)
        self.level.setObjectName("groupChoice")
        self.level.setVisible(False)
        self.level.currentIndexChanged.connect(self.target_changed)

        # (Made early: the tabs report into it.) One line, always there, under the scrolled column
        # (the panel's foot, no bar while it has nothing to say): in view however far the panel
        # scrolls, and messages coming and going never move the tools (issue #114).
        self.status = widgets.StatusLabel(single_line=True)
        self.status.setContentsMargins(theme.px(12), theme.px(2), theme.px(12), theme.px(4))
        self.status.fit_height()
        frame.insertWidget(frame.indexOf(scroll) + 1, self.status)

        # No groom to work on: how to start one.
        self.empty = QtWidgets.QFrame()
        self.empty.setProperty("role", "empty")
        column = QtWidgets.QVBoxLayout(self.empty)
        pad = theme.px(14)
        column.setContentsMargins(pad, pad, pad, pad)
        column.setSpacing(theme.px(8))
        empty = definitions.get("palette/empty")
        title = QtWidgets.QLabel(empty["title"])
        title.setProperty("role", "title")
        column.addWidget(title)
        self.empty_text = QtWidgets.QLabel()
        self.empty_text.setWordWrap(True)
        self.empty_text.setProperty("role", "about")
        column.addWidget(self.empty_text)
        # What is selected now (Maya's way is select, then create): the button below waits for a mesh.
        self.empty_selection = QtWidgets.QLabel()
        self.empty_selection.setObjectName("emptySelection")
        self.empty_selection.setProperty("role", "itemBox")
        self.empty_selection.setWordWrap(True)
        column.addWidget(self.empty_selection)
        self.empty_create = None
        # Make one on the selected mesh, add a sample mesh, or bring one in from XGen, Yeti, Alembic
        # curves or a groom file (the chip: the GroomLab menu's Import list).
        bind = definitions.binder(self.ACTIONS, self)
        for item in definitions.resolve(empty["items"]):
            if item["kind"] == "action":
                make = QtWidgets.QPushButton(item["label"])
                make.setProperty("role", "primary")
                make.setToolTip(item["tip"])
                make.clicked.connect(lambda _=False, fn=bind(item): fn())
                column.addWidget(make)
                if item["id"] == "groom.create":
                    self.empty_create = make
            elif item["id"] == "import":
                self.empty_import = self.menu_chip(item)
                self.empty_import.setProperty("disclosure", True)  # (opens a menu: outlined, as the app's)
                column.addWidget(self.empty_import)
            else:
                sample = QtWidgets.QPushButton(item["label"])
                sample.setProperty("role", "disclosure")
                sample.setToolTip(item["tip"])
                samples = QtWidgets.QMenu(sample)
                widgets.fill_menu(samples, definitions.get(item["entry"]["ref"]), bind)
                sample.setMenu(samples)
                column.addWidget(sample)
        outer.addWidget(self.empty)

        # Everything that works on a groom (in the top: the tabs and the tool grid; under it: the
        # tab's list and the tool's card): hidden while there is none, so the empty state shows
        # only "Start a groom" (fill_grooms).
        self.top_work = QtWidgets.QWidget()
        self.top_work.setObjectName("groomToolsTopWork")
        top_work = QtWidgets.QVBoxLayout(self.top_work)
        top_work.setContentsMargins(0, 0, 0, 0)
        top_work.setSpacing(theme.px(6))
        top_column.addWidget(self.top_work)
        self.work = _Work(self.top_work)
        self.work.setObjectName("groomToolsWork")
        work = QtWidgets.QVBoxLayout(self.work)
        work.setContentsMargins(0, 0, 0, 0)
        work.setSpacing(theme.px(10))
        outer.addWidget(self.work)

        self.group = QtWidgets.QButtonGroup(self)
        self.group.setExclusive(False)  # the active tool's tiles are checked by hand (one tool, maybe two tiles)
        self.tabs = widgets.Pages(text=True)
        frame.insertWidget(0, self.header)    # the groom and its buttons, pinned on top
        frame.insertWidget(1, self.tabs.rail)  # then the pages, labeled (Tools · Paint · Photos)
        # The tool grid (Tools and Correctives): each category a tray, its name inside over its
        # tiles; the Paint tile alone in a tray on the Paint tab.
        self.tool_palette = QtWidgets.QWidget()
        self.tool_palette.setObjectName("toolGrid")
        grids = QtWidgets.QVBoxLayout(self.tool_palette)
        grids.setContentsMargins(0, 0, 0, 0)
        grids.setSpacing(theme.px(8))
        self.tool_grids = []  # (the category's label, its grid)
        self.tool_trays = []  # each category's tray (its card)
        for section, tools in SECTIONS:
            if section == "Paint":
                continue  # the painting brushes live in the Paint tab, beside the maps they paint
            tray = self.tray("tray_" + section.lower(), margins=(4, 4, 4, 4))
            tray.setToolTip(section)  # (the group's name: no label row, the tiles speak for it)
            label = widgets.small_label(section, role="category")
            label.setVisible(False)
            # Beside Freeze: Invert Frozen and Unfreeze All, buttons (not tools).
            extra = [self.invert_tile(), self.unfreeze_tile()] if any(t[0] == "freeze" for t in tools) else []
            grid = self.tool_grid(tools, extra=extra)
            tray.layout().addWidget(grid)
            grids.addWidget(tray)
            self.tool_grids.append((label, grid))
            self.tool_trays.append(tray)
        top_work.addWidget(self.tool_palette)
        self.paint_tiles = self.wide_tools(dict(SECTIONS).get("Paint", []))
        top_work.addWidget(self.paint_tiles)

        self.paint_tab = tool_tabs.PaintTab(self)
        self.reference_tab = tool_tabs.ReferenceTab(self)
        # Correctives: the groom as a stack (base groom + correctives); the picked row is what brushes edit.
        self.correctives = tool_tabs.CorrectivesBox(self)
        # Guide groups and edits on many guides (Tools tab).
        sections = dict((section["id"], section) for section in definitions.get("palette/sections"))
        self.section_rules = sections  # which tool and tab each section shows with (show_sections)
        self.guides_tab = tool_tabs.GuidesTab(self)
        self.guides_section = self.guides_tab.section
        self.isolate = self.guides_tab.isolate  # the eye in the list's header: the picked group at full color
        tools_tab = QtWidgets.QWidget()
        tools_layout = QtWidgets.QVBoxLayout(tools_tab)
        tools_layout.setContentsMargins(0, 0, 0, 0)
        tools_layout.setSpacing(theme.px(10))
        tools_layout.addWidget(self.guides_tab)  # (the card holding the Guide groups section)
        # Correctives: a section of its own under the guide groups (on the Tools page, its own card).
        self.correctives.setProperty("role", "tray")
        self.correctives.setAttribute(QtCore.Qt.WA_StyledBackground, True)
        self.correctives.layout().setContentsMargins(theme.px(10), theme.px(4), theme.px(10), theme.px(10))
        pages = {"tools": tools_tab, "paint": self.paint_tab, "photos": self.reference_tab}
        for page in (self.guides_tab, self.paint_tab, self.reference_tab):  # each list on a card (the app's trays)
            page.setProperty("role", "tray")
            page.setAttribute(QtCore.Qt.WA_StyledBackground, True)
            page.layout().setContentsMargins(theme.px(10), theme.px(4), theme.px(10), theme.px(10))
        tools_tab.layout().setContentsMargins(0, 0, 0, 0)
        tools_layout.addWidget(self.correctives)

        # With no tool, one line on how to start, on a compact card in the tool card's place.
        self.hint = widgets.small_label(NO_TOOL, role="noTool", wrap=True)
        self.hint.setObjectName("noTool")
        # The tool's card (under the tools; on Paint under the maps): its icon and name, what it
        # does, then its settings in blocks: Brush (untitled: the header names the tool), the
        # tool's own, Applies to. Paint: its own block first (what a stroke paints), then Brush.
        self.settings = QtWidgets.QFrame()
        self.settings.setObjectName("toolCard")
        self.settings.setProperty("role", "toolCard")
        card = QtWidgets.QVBoxLayout(self.settings)
        card.setContentsMargins(theme.px(10), theme.px(6), theme.px(10), theme.px(10))
        card.setSpacing(theme.px(4))
        head = QtWidgets.QHBoxLayout()
        head.setContentsMargins(0, theme.px(2), 0, theme.px(4))
        head.setSpacing(theme.px(8))
        self.card_icon = QtWidgets.QLabel()
        self.card_icon.setObjectName("toolCardIcon")
        self.card_icon.setFixedSize(theme.px(18), theme.px(18))
        head.addWidget(self.card_icon)
        self.card_title = QtWidgets.QLabel()
        self.card_title.setObjectName("toolCardTitle")
        self.card_title.setProperty("role", "toolTitle")
        head.addWidget(self.card_title)
        head.addStretch(1)
        # What the next stroke edits: the picked guide group, on the base groom or a corrective.
        self.card_editing = QtWidgets.QLabel()
        self.card_editing.setObjectName("toolCardEditing")
        self.card_editing.setProperty("role", "muted")
        self.card_editing.setTextFormat(QtCore.Qt.RichText)
        head.addWidget(self.card_editing)
        card.addLayout(head)
        self.card_hint = widgets.small_label("", role="toolHint", wrap=True)
        self.card_hint.setObjectName("toolCardHint")
        card.addWidget(self.card_hint)

        # Brush: how the stroke feels.
        self.brush_block = widgets.Block(definitions.text("brushBlock"), name="brushBlock")
        self.brush_block.rule.setVisible(False)  # (the card's header names the tool: no title over its first block)
        self.brush_block.title.setVisible(False)
        self.brush_form = QtWidgets.QWidget()
        self.brush_form.setProperty("role", "options")
        fields = QtWidgets.QVBoxLayout(self.brush_form)
        fields.setContentsMargins(0, 0, 0, 0)
        fields.setSpacing(theme.px(4))
        self.radius = _slider("radius", integer=True, key="palette.radius", label=_setting("radius", "label"), suffix="px")
        self.radius.unit_tip = "Screen pixels: the circle's radius on screen, whatever the zoom"
        self.radius.setToolTip(_setting("radius", "hint"))
        self.radius.committed.connect(lambda v: self.push(radius=v))
        self.radius.dragged.connect(lambda v: self.push(radius=v))
        self.strength = _slider("strength", key=None, label=_setting("strength", "label"))
        self.strength.setToolTip(_setting("strength", "hint"))
        self.strength.committed.connect(self.set_strength)
        self.strength.dragged.connect(self.set_strength)
        # Falloff as the catalog names it in both hosts: 0 hard (full strength to the edge), 1 soft
        # (fading from the center out); the brush's hardness is 1 - falloff.
        self.falloff = _slider("falloff", key="palette.falloff", label=_setting("falloff", "label"))
        self.falloff.setToolTip(_setting("falloff", "hint"))
        self.falloff.committed.connect(lambda v: self.push(hardness=1.0 - v))
        self.falloff.dragged.connect(lambda v: self.push(hardness=1.0 - v))
        self.smooth_strength = _slider("smoothStrength", key="palette.smoothStrength", label=_setting("smoothStrength", "label"))
        self.smooth_strength.setToolTip(_setting("smoothStrength", "hint"))
        self.smooth_strength.committed.connect(lambda v: self.push(smoothStrength=v))
        self.smooth_strength.dragged.connect(lambda v: self.push(smoothStrength=v))
        for slider in (self.radius, self.strength):
            fields.addWidget(slider)  # the name inside the field
        for slider in (self.radius, self.strength, self.falloff):  # the brush's numbers: the card's tallest fields
            slider.setFixedHeight(widgets.SoftSlider.row_height() + theme.px(4))
        self.brush_block.add(self.brush_form)
        # The stroke's toggles as switch rows, in a fixed order (a row never moves between tools):
        # Pressure, Keep off scalp, Stretch (Comb: lengths change with the pull), Straighten (Smooth: toward the
        # straight line); then the brush's curves as chips (each off until its chip is on, then
        # drawn in a curve editor under the chips; right-click: presets). Selected only and Front
        # only are not in the panel: the brushes keep their defaults (off and on).
        # The stroke's switches: one row of chips under the brush's numbers (Mirror first; the
        # card's toggles row, self.toggles, holds them below the tool's own block).
        self.pressure = self.toggle_chip(CHIP_LABELS["pressure"], _setting("pressure", "hint"), _default("pressure"),
                                         "chip_pressure")
        self.pressure.toggled.connect(lambda on: self.push(pressure=on))
        self.collide = self.toggle_chip(CHIP_LABELS["collide"], _setting("collide", "hint"), _default("collide"),
                                        "chip_collide")
        self.collide.toggled.connect(lambda on: self.push(collide=on))
        self.stretch = self.toggle_chip("Stretch", _setting("stretch", "hint"), _default("stretch"), "stretch")
        self.stretch.toggled.connect(lambda on: self.push(stretch=on))
        self.straighten = self.toggle_chip("Straighten", _setting("straighten", "hint"), _default("straighten"),
                                           "straighten")
        self.straighten.toggled.connect(lambda on: self.push(straighten=on))
        # More settings (folded, remembered): what most strokes never change.
        self.more_body = QtWidgets.QWidget()
        more_rows = QtWidgets.QVBoxLayout(self.more_body)
        more_rows.setContentsMargins(0, 0, 0, 0)
        more_rows.setSpacing(theme.px(4))
        self.more_rows = more_rows
        self.more_section = widgets.Section("More settings", self.more_body, expanded=False)
        self.more_section.setObjectName("moreSettings")
        self.falloff_curve_box, self.falloff_curve = self.curve_setting(
            "falloffProfile", [[0, 1], [0.5, 0.56], [1, 0]], ("center", "edge"))
        self.strength_curve_box, self.strength_curve = self.curve_setting(
            "strengthProfile", [[0, 0.25], [1, 1]], ("root", "tip"))
        self.brush_chips = widgets.chip_row((self.falloff_curve_box, self.strength_curve_box))
        self.brush_chips.setObjectName("brushChips")
        # The alpha stamp (Paint on a scalp map): in More, under the curves.
        self.alpha_box = self.alpha_setting()

        # -- The tool's own blocks, named for what they set --------------------------
        # Paint: the mode, the value (or color) and Fill (the map's: the Paint tab's). What a stroke
        # paints comes first, right under the card's header (untitled: the header names the tool);
        # Brush follows, titled (show_tool_settings).
        self.paint_block = widgets.Block(NAMES.get("paint", "Paint"), name="paintBlock")
        self.paint_block.rule.setVisible(False)
        self.paint_block.title.setVisible(False)
        self.paint_block.add(self.paint_tab.paint_controls)
        card.addWidget(self.paint_block)
        card.addWidget(self.brush_block)

        # Place and Draw: the new guides, and the tube they get.
        from groomlab import units
        self.create_box = widgets.Block(_setting("newGuides", "label"), name="newGuides")
        self.create_box.setToolTip(_setting("newGuides", "hint"))
        self.length_factor = units.factor("length")  # shown in real units, stored in scene units
        length_key = "palette.length@" + units.unit("length")[0]
        self.length = _slider("length", self.length_factor, key=length_key, label="Length", suffix=units.label("length"))
        self.length.committed.connect(lambda v: self.push(length=v / self.length_factor))
        self.segments = _slider("segments", key="palette.segments", label="Segments")
        self.segments.committed.connect(lambda v: self.push(segments=v))
        self.spacing = _slider("spacing", self.length_factor, key="palette.spacing@" + units.unit("length")[0], label="Spacing",
                               suffix=units.label("length"))
        self.spacing.setToolTip(_setting("spacing", "hint"))
        self.spacing.committed.connect(lambda v: self.push(spacing=v / self.length_factor))
        for field in (self.length, self.segments):
            self.create_box.add(field)  # the name inside the field (Spacing: in More)
        self.match = widgets.switch("Match nearby", "New guides take the shape, length and painted maps of the nearest "
                                    "guides: the group's own, or the groups around it while it has none. Draw takes the "
                                    "maps. Off: they stand straight up, with no maps", _default("matchNeighbors"), "matchNearby")
        self.match.toggled.connect(lambda on: self.push(matchNeighbors=on))
        self.solo_new = widgets.switch("Solo new", _setting("soloNewGuides", "hint"), _default("soloNewGuides"), "soloNewGuides")
        self.solo_new.toggled.connect(lambda on: self.push(soloNewGuides=on))  # (off: placing freezes nothing)
        self.create_box.add(self.match.row)  # (Solo new: in More)
        card.addWidget(self.create_box)
        self.tube_panel = TubePanel(self)  # its blocks: Tube (Place, Draw), Profile and Selected guides
        self.more_rows.addWidget(self.tube_panel.tube_block)  # (Place, Draw: the new guides' tube, in More)

        # Shape: what it paints on the tubes (Width, Flatness, Twist) at the top of Brush; then
        # Visibility (how the tubes are drawn), the profile and the selected guides' tubes.
        self.tube_channel = QtWidgets.QButtonGroup(self)  # exclusive: one channel at a time
        self.tube_channel_buttons = {}
        for name, label, tip in TUBE_CHANNELS:
            b = widgets.chip(label, tip, name == "width", "tubeChannel_" + name)
            self.tube_channel.addButton(b)
            self.tube_channel_buttons[name] = b
        self.tube_channel.buttonClicked.connect(lambda _b: self.push(tubeChannel=self.tube_channel_name()))
        self.tube_channel_row = widgets.seg_row(self.tube_channel_buttons.values())  # one of them: a track
        self.tube_channel_row.setToolTip(_setting("tubeChannel", "hint"))
        self.brush_block.body.insertWidget(0, self.tube_channel_row)
        # The tubes' look (the header's guides menu, add_tubes); whether they show with every tool
        # is its Tubes pill (the groom's displayTubes: tube_show), which level's a level row's tube.
        self.tube_channel_box = widgets.Block("Tubes", name="paintsBlock")
        self.tube_channel_box.setToolTip("How the tubes are drawn")
        self.tube_show = self.display_popup.chips["displayTubes"]
        # The look, each on its own: shaded surfaces (off by default) to judge the profiles' shapes,
        # see-through wires (on), each tube in its root tube's color, each split tube a shade of its own.
        self.tube_shaded = widgets.chip("Shaded", "Draw the tubes as shaded surfaces, to judge the profiles' shapes",
                                        False, "tubeShaded")
        self.tube_wireframe = widgets.chip("Wireframe", "Draw the tubes as see-through wires (over the shaded "
                                           "surfaces when Shaded is on)", True, "tubeWireframe")
        self.tube_colors = widgets.chip("Colors", "Color each tube as the root tube it comes from (its split tubes and "
                                        "children in its color), or a color of its own: the shaded surfaces with "
                                        "Shaded on (the wires stay gray), else the wires", True, "tubeColors")
        self.tube_level_colors = widgets.chip("Level colors", "Each tube of a tube hierarchy's levels (a split tube) in "
                                              "a shade of its own on top of its root tube's color, so the tubes of a "
                                              "level tell apart. Tubes with no levels stay as they are", False,
                                              "tubeLevelColors")
        for chip_ in (self.tube_shaded, self.tube_wireframe, self.tube_colors, self.tube_level_colors):
            chip_.toggled.connect(lambda on: self.push_tube_display())
        self.tube_look = widgets.pill_row((self.tube_shaded, self.tube_wireframe, self.tube_colors,
                                           self.tube_level_colors))
        self.tube_channel_box.add(self.tube_look)
        self.display_popup.add_tubes(self.tube_channel_box)  # (the tubes' display: the header's guides menu)

        # Select: the soft radius (its selection's), then the profile and the selected guides' tubes.
        self.select_box = widgets.Block(_setting("selection", "label"), name="selectionBlock")
        self.soft_factor = units.factor("length")  # shown in real units (View > Units), kept in scene units
        self.soft = widgets.SoftSlider(0.0, 0.0, 1.0 * self.soft_factor, minimum=0.0, key="palette.soft",
                                       shown_decimals=widgets.display_decimals(3, self.soft_factor),
                                       label="Soft radius", suffix=units.label("length"))
        self.soft.setToolTip("Soft selection radius: guides near the selection are partly selected; "
                             "brushes fade out over it on the selected guides")
        self.soft.committed.connect(lambda v: self.set_soft_selection(v / self.soft_factor))
        self.select_box.add(self.soft)
        card.addWidget(self.select_box)
        # The Roots brush's block (tubes drawn root first), and the tube hierarchy's levels, which
        # the Roots and Tube brushes share: one row right under the card's header.
        from groomlab.ui import root_tube_card
        self.root_card = root_tube_card.RootTubeCard(self)
        card.insertWidget(card.indexOf(self.card_hint), self.root_card.hierarchy_block)
        card.addWidget(self.root_card.root_block)

        # Erase: what it removes.
        self.erase_box = widgets.Block(_setting("erases", "label"), name="erasesBlock")
        self.erase_box.setToolTip("What the Erase brush removes where it passes (one undo step)")
        self.erase_guides = widgets.chip("Guides", "Guides, with their children", True, "eraseGuides")
        self.erase_parts = widgets.chip("Parts", "Part lines", True, "eraseParts")
        self.erase_flow = widgets.chip("Flow", "Flow arrows and sinks", True, "eraseFlow")
        for box, flag in ((self.erase_guides, "eraseGuides"), (self.erase_parts, "eraseParts"),
                          (self.erase_flow, "eraseFlow")):
            box.toggled.connect(lambda on, f=flag: self.push(**{f: on}))
        self.erase_box.add(widgets.chip_row((self.erase_guides, self.erase_parts, self.erase_flow)))
        card.addWidget(self.erase_box)

        # Length Keys: the selected key, Apply and Cancel (Clear in the block's header).
        self.length_keys = length_keys_box.LengthKeysBox()
        self.keys_block = widgets.Block(_setting("lengthKeys", "label"), (self.length_keys.clear,), name="keysBlock")
        self.keys_block.setToolTip(_setting("lengthKeys", "hint"))
        self.keys_block.add(self.length_keys)
        card.addWidget(self.keys_block)

        # Flow arrows: what a click lays, how many the groom keeps, Clear all (in the header).
        clear_flow = QtWidgets.QToolButton()
        clear_flow.setText("Clear all")
        clear_flow.setObjectName("clearFlow")
        clear_flow.setAutoRaise(True)
        clear_flow.setProperty("role", "ghost")
        clear_flow.setToolTip(definitions.get("actions/flow.deleteAll")["tip"])
        clear_flow.clicked.connect(self.clear_flow)
        self.clear_flow_button = clear_flow
        # Mirror to Other Side (issue 105): one side's arrows and sinks over the other's.
        self.flow_mirror = self.mirror_menu_button("lists/flow/mirror", "flowMirror",
                                                  "Copy one side's flow arrows and sinks over the other (across the Mirror "
                                                  "below; X when it is off)")
        self.flow_box = widgets.Block(_setting("flow", "label"), (self.flow_mirror, clear_flow), name="flowBlock")
        self.flow_sink = QtWidgets.QComboBox()
        for kind, label, tip in _options("flowSink"):
            self.flow_sink.addItem(label, kind)
            self.flow_sink.setItemData(self.flow_sink.count() - 1, tip, QtCore.Qt.ToolTipRole)
        self.flow_sink.currentIndexChanged.connect(self.flow_sink_changed)
        self.flow_box.labeled(definitions.get("choices/flowSink")["label"], self.flow_sink)
        # How many arrows and sinks the groom keeps (GroomLab > Utilities > Delete All Flow Arrows
        # clears them too).
        self.flow_count = widgets.small_label("")
        self.flow_count.setObjectName("flowCount")
        self.flow_count.setToolTip("The flow arrows and sinks kept on the groom (the Flow node lays the strands "
                                   "along them)")
        self.flow_box.add(self.flow_count)
        self.flow_sink_changed()
        card.addWidget(self.flow_box)

        # Clone: Pick Source (the next click copies, as Ctrl+click does), what the source holds,
        # and whether copies replace the guides where they land.
        self.clone_box = widgets.Block(_setting("clone", "label"), name="cloneBlock")
        self.clone_box.setToolTip(_setting("clone", "hint"))
        self.clone_pick = QtWidgets.QPushButton("Pick Source")
        self.clone_pick.setObjectName("clonePickSource")
        self.clone_pick.setCheckable(True)
        self.clone_pick.setToolTip("The next click in the viewport copies the guides rooted in the brush circle "
                                   "(Ctrl+click does the same any time)")
        self.clone_pick.toggled.connect(self.pick_clone_source)
        self.clone_count = widgets.small_label("")
        self.clone_count.setObjectName("cloneCount")
        self.clone_count.setToolTip("The guides the source holds (their children come along)")
        self.clone_reset = QtWidgets.QPushButton("Reset")
        self.clone_reset.setObjectName("cloneReset")
        self.clone_reset.setToolTip("Let the source go, ready for a new pick (a Ctrl+click off the model, or "
                                    "picking another tool, does the same)")
        self.clone_reset.clicked.connect(self.reset_clone_source)
        self.clone_box.add(self.clone_pick)
        self.clone_box.add(self.clone_reset)
        self.clone_box.add(self.clone_count)
        self.clone_replace = widgets.switch("Replace", _setting("cloneReplace", "hint"), _default("cloneReplace"),
                                            "cloneReplace")
        self.clone_replace.toggled.connect(lambda on: self.push(cloneReplace=on))
        self.clone_box.add(self.clone_replace.row)
        card.addWidget(self.clone_box)

        # Parts: the next part's name, the parts drawn so far (Parts from Curves in the header).
        bind = definitions.binder(self.ACTIONS, self)
        part_items = dict((item["id"], item) for item in definitions.resolve(definitions.get("lists/parts/bar"), None))
        from_curves = part_items["parts.fromCurves"]
        self.part_box = widgets.Block(_setting("parts", "label"), (widgets.glyph_button(
            "curves", from_curves["label"] + ": " + from_curves["tip"], bind(from_curves)),), name="partsBlock")
        self.part_name = QtWidgets.QLineEdit("part")
        self.part_name.setToolTip("Name for the next part (made unique)")
        self.part_name.editingFinished.connect(lambda: self.push(partName=self.part_name.text() or "part"))
        self.part_box.labeled("Name", self.part_name)
        self.part_list = QtWidgets.QListWidget()
        self.part_list.setMaximumHeight(theme.px(90))
        self.part_box.add(self.part_list)
        part_buttons = QtWidgets.QHBoxLayout()
        part_buttons.setSpacing(theme.px(4))
        for key in ("parts.remove", "parts.clear"):
            item = part_items[key]
            b = widgets.chip(item["label"], item["tip"])
            b.clicked.connect(lambda _=False, fn=bind(item): fn())
            part_buttons.addWidget(b)
        # Mirror to Other Side (issue 105): one side's parts over the other's.
        self.part_mirror = self.mirror_menu_button("lists/parts/mirror", "partsMirror",
                                                   "Copy one side's parts over the other (across the Mirror below; X when "
                                                   "it is off)")
        part_buttons.addWidget(self.part_mirror)
        part_buttons.addStretch(1)
        self.part_box.add(part_buttons)
        card.addWidget(self.part_box)

        # Applies to: where the stroke lands. What it acts on (the shaping brushes that also shape
        # part lines and flow arrows), and Mirror.
        self.applies_block = widgets.Block(definitions.text("appliesBlock"), name="appliesBlock")
        self.target_guides = widgets.chip("Guides", "Guides", True, "targetGuides")
        self.target_parts = widgets.chip("Parts", "Part lines", False, "targetParts")
        self.target_flow = widgets.chip("Flow", "Flow arrows and sinks", False, "targetFlow")
        for box in (self.target_guides, self.target_parts, self.target_flow):
            box.toggled.connect(self.push_targets)
        self.target_box = QtWidgets.QWidget()
        self.target_box.setProperty("role", "options")
        targets = QtWidgets.QVBoxLayout(self.target_box)
        targets.setContentsMargins(0, 0, 0, 0)
        targets.setSpacing(theme.px(4))
        targets.addWidget(widgets.seg_row((self.target_guides, self.target_parts, self.target_flow)))
        self.target_box.setToolTip("What the brush shapes where it passes: guides, part lines, flow arrows and sinks "
                                   "(Comb slides and lays arrows along the stroke; Swing turns arrows and whorls; "
                                   "Length sizes whorls; Smooth and Attract shape both; Cut splits parts)")
        # (the targets: a row in More, below)
        # Mirror: Symmetry, and across what: world X, or the scalp's own topology (a head turned,
        # or not quite symmetric).
        self.symmetry = self.toggle_chip("\u21c6 Mirror", _setting("symmetry", "hint"), False, "symmetry")
        self.symmetry_mode = QtWidgets.QComboBox()
        self.symmetry_mode.setObjectName("symmetryMode")
        for name, label, tip in _options("symmetry"):
            self.symmetry_mode.addItem(label, name)
            self.symmetry_mode.setItemData(self.symmetry_mode.count() - 1, tip, QtCore.Qt.ToolTipRole)
        self.symmetry_mode.setToolTip(_setting("symmetry", "hint"))
        self.symmetry_mode.setEnabled(False)
        self.symmetry.toggled.connect(lambda on: (self.symmetry_mode.setEnabled(on), self.push(symmetry=self.symmetry_name())))
        self.symmetry_mode.currentIndexChanged.connect(lambda _i: self.push(symmetry=self.symmetry_name()))
        self.symmetry.setToolTip(_setting("symmetry", "hint") + ". " + SYMMETRY_TIP)
        # Mirror across what: in More (the switch itself is the toggles row's Mirror chip).
        self.symmetry_row, mirror = widgets.field_row("Mirror across", self.symmetry_mode)
        self.symmetry_row.setObjectName("symmetryRow")
        mirror.setToolTip(self.symmetry.toolTip())
        self.targets_row, targets_label = widgets.field_row("Applies to", self.target_box)
        targets_label.setToolTip(self.target_box.toolTip())
        # The toggles row, then More settings: Falloff, Smooth, Mirror across, Applies to, the
        # curves, the stamp, Spacing, Solo new.
        self.toggles = widgets.pill_row((self.symmetry, self.pressure, self.collide, self.stretch, self.straighten,
                                         self.paint_tab.whole_faces))
        self.toggles.setObjectName("brushToggles")
        card.addWidget(self.toggles)
        for widget in (self.falloff, self.smooth_strength, self.symmetry_row, self.targets_row, self.brush_chips,
                       self.falloff_curve, self.strength_curve, self.alpha_box, self.spacing, self.solo_new.row):
            more_rows.addWidget(widget)
        card.addWidget(self.more_section)
        self.applies_block.setVisible(False)  # (its rows moved: the toggles row and More)
        # The Tube brush's Profile (root, mid and tip outlines, the library) under its brush
        # settings, then the selected tubes last (the Tube tool has no Applies to: its Mirror is in Brush).
        card.addWidget(self.tube_panel.profile_block)
        card.addWidget(self.tube_panel.selected_block)

        # Right under the grid: the list the tab works on (Guide groups, Maps, Layers, Photos);
        # the tool's card under it.
        for tab in definitions.tabs():
            self.tabs.addTab(pages[tab["id"]], tab["label"], icon=rail_icon(tab.get("icon")), tip=tab.get("tip", ""))
        self.tabs.currentChanged.connect(self.tab_changed)
        work.addWidget(self.hint)
        work.addWidget(self.settings)
        work.addWidget(self.tabs)
        self.work_layout = work
        outer.addStretch(1)

        theme.style(self, surface="panel")  # the panel's tone, the sections on cards over it
        theme.listen(self, "apply_theme")
        self.timer = QtCore.QTimer(self)
        self.timer.setInterval(self.POLL_IDLE_MS)
        self.timer.timeout.connect(self.poll)
        self.timer.start()
        self._scene_jobs = []
        self._watch_scene()
        groomlab.on_current_groom(self.current_groom_changed)  # (a pick in the Graph Editor: at once)
        self.poll()
        self.tab_changed(self.tabs.currentIndex())
        self.show_tool_settings()

    def header_button(self, glyph, tip, checkable=False):
        b = QtWidgets.QToolButton()
        if glyph:
            b.setIcon(widgets.glyph(glyph))
            b.glyph = glyph
        b.setIconSize(QtCore.QSize(theme.px(20), theme.px(20)))
        b.setToolTip(tip)
        b.setAutoRaise(True)
        b.setCheckable(checkable)
        b.setProperty("role", "glyph")
        return b

    # What the Groom Tools' own buttons and menus run (engine/ui ids; the Output strip runs the GroomLab
    # menu's: menu.ACTIONS): method names, or functions of the palette.
    ACTIONS = {
        "groom.create": "create_groom",
        "sample.nika": lambda self: self.add_sample("nika"),
        "sample.manny": lambda self: self.add_sample("manny"),
        "sample.wolf": lambda self: self.add_sample("wolf"),
        "sample.buddy": lambda self: self.add_sample("buddy"),
        "parts.fromCurves": "parts_from_curves",
        "parts.remove": "remove_part",
        "parts.clear": "clear_parts",
        "parts.mirrorNegative": lambda self: self.mirror_parts("negative"),
        "parts.mirrorPositive": lambda self: self.mirror_parts("positive"),
        "flow.mirrorNegative": lambda self: self.mirror_flow("negative"),
        "flow.mirrorPositive": lambda self: self.mirror_flow("positive"),
    }

    def mirror_menu_button(self, path, name, tip):
        """A "Mirror" button opening `path`'s actions (Mirror -X to +X, +X to -X)."""
        button = QtWidgets.QToolButton()
        button.setText("Mirror  ▾")
        button.setObjectName(name)
        button.setAutoRaise(True)
        button.setProperty("role", "ghost")
        button.setToolTip(tip)
        button.setPopupMode(QtWidgets.QToolButton.InstantPopup)
        popup = QtWidgets.QMenu(button)
        popup.setToolTipsVisible(True)
        bind = definitions.binder(self.ACTIONS, self)
        for item in definitions.resolve(definitions.get(path), None):
            if item.get("kind") != "action":
                continue
            action = popup.addAction(item["label"])
            action.setToolTip(item["tip"])
            action.triggered.connect(lambda _=False, fn=bind(item): fn())
        button.setMenu(popup)
        return button

    @staticmethod
    def eye_menu(button, items):
        """An eye's right-click menu: the GroomLab menu's Show / Hide All (palette.json's header items)."""
        from groomlab.ui import menu
        popup = QtWidgets.QMenu(button)
        widgets.fill_menu(popup, items, definitions.binder(menu.ACTIONS))
        popup.setToolTipsVisible(True)
        button.setContextMenuPolicy(QtCore.Qt.CustomContextMenu)
        button.customContextMenuRequested.connect(lambda pos: popup.exec_(button.mapToGlobal(pos)))
        button.eye_menu = popup  # kept (an unowned QMenu is garbage-collected)
        return popup

    def generators(self):
        """The menus' entries the Groom Tools fill themselves: the Theme submenu (menus/theme)."""
        return {"themeMenu": widgets.menu_filler(lambda menu, _item: menu.addMenu(widgets.theme_menu(self)))}

    @staticmethod
    def menu_chip(item):
        """A chip opening a submenu item's list of the GroomLab menu's actions (the empty state's
        Import: menus/output.import; menu.shown_items, one table for both)."""
        from groomlab.ui import menu
        kind = item["id"]
        chip = QtWidgets.QToolButton()
        chip.setText(item["label"] + "  ▾")
        chip.setToolTip(item["tip"])
        chip.setProperty("role", "chip")
        chip.setObjectName("output_" + kind)
        chip.setPopupMode(QtWidgets.QToolButton.InstantPopup)
        chip.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        popup = QtWidgets.QMenu(chip)
        for entry in menu.shown_items(kind, menu.TOOLS_ONLY):
            if entry is None:
                popup.addSeparator()
                continue
            text, fn, annotation = entry[:3]
            action = popup.addAction(text)
            action.setToolTip(annotation)
            action.triggered.connect(menu._guarded(fn, text))
        popup.setToolTipsVisible(True)
        chip.setMenu(popup)
        return chip

    @staticmethod
    def setting_box(setting):
        """A checkbox for one of the catalog's settings: its label and hint."""
        box = QtWidgets.QCheckBox(SETTINGS[setting]["label"])
        box.setToolTip(SETTINGS[setting]["hint"])
        return box

    @staticmethod
    def setting_switch(setting, on=False):
        """A switch row for one of the catalog's settings: its short name (CHIP_LABELS) and hint."""
        return widgets.switch(CHIP_LABELS.get(setting, SETTINGS[setting]["label"]), SETTINGS[setting]["hint"], on,
                              "chip_" + setting)

    @staticmethod
    def toggle_chip(text, tip, on, name):
        """A stroke switch as a chip in the card's toggles row (its `.row` is itself: shown and hidden
        as the switch rows were)."""
        b = widgets.chip(text, tip, bool(on), name)
        b.row = b
        return b

    # Maya's Help Line while a brush is up: what it does, then the keys every brush shares (the
    # brushes with a circle; Roots clicks CVs).
    HELP = {"shape": "Drag: paint the tubes' channel (Width, Flatness, Twist); Ctrl: the opposite; Shift: smooth. "
                     "The Profile's editor: drag a point to edit a library shape",
            "roots": "Draw: click the root's outline CV by CV, the first CV again closes it, then draw its guide; "
                     "Edit: drag a CV (Ctrl+click an edge adds one); Select: click a root's middle, drag to move it"}

    def help_text(self, tool):
        name = NAMES.get(tool, (tool or "").title())
        what = self.HELP.get(tool) or HINTS.get(tool, "")
        if "radius" not in TOOL_SETTINGS.get(tool, set()):
            return "%s: %s" % (name, what)
        return "%s: %s   ·   B / M + drag (or middle-drag): radius / strength   ·   Alt+Shift+X: mirror" % (
            name, what)

    def push_help(self, tool):
        """The brush's keys in Maya's Help Line (the context's -helpText)."""
        try:
            self.push(helpText=self.help_text(tool))
        except Exception:  # (a plug-in older than the flag: the general text stays)
            pass

    def find_command(self):
        """Find Command: a search over every GroomLab command, brush and graph preset."""
        from groomlab.ui import find_command
        find_command.show(self)

    def show_editing(self):
        """The card's header: what the next stroke edits (the picked group, and the layer: the base
        groom or a corrective, in the accent while it is one)."""
        group = self.current_group() or "All groups"
        color = "#888888"
        groom = self.groom() or getattr(self.guides_tab, "groom", None)  # (the list's groom: its dots' colors)
        if groom and self.current_group():
            try:
                color = groomlab.guide_group_colors(groom).get(self.current_group()) or next(
                    (g["color"] for g in groomlab.list_guide_groups(groom) if g["name"] == self.current_group()), color)
            except RuntimeError:
                pass
        layer = self.editing_layer()
        t = theme.current()
        layer_html = ("<span style='color:%s'>%s</span>" % (t.color("ui.accent"), layer)) if layer else "Base groom"
        if self.card_tool() == "paint":  # Paint: the map it paints
            name = self.paint_tab.current()
            self.card_editing.setText("on  <b>%s</b>" % name if name else "no map yet: + under Maps")
            return
        # A level of a tube hierarchy kept in view while another's guides are picked: said here, so
        # the brush is seen to comb these guides under those tubes.
        viewed = self.tube_view(groom) if groom else ""
        tubes = ""
        if viewed and viewed != self.current_group():
            level = self.tube_level_of(viewed, groom)
            tubes = "  \u00b7  tubes: %s" % ("Level %d" % level if level else viewed)
        self.card_editing.setText("<span style='color:%s'>\u25cf</span> %s  on  %s%s" % (color, group, layer_html, tubes))

    def editing_layer(self):
        """The corrective (or Sculpt node) brushes edit now, or "" for the base groom."""
        box = getattr(self, "correctives", None)
        if box is None:
            return ""
        item = box.tree.currentItem()
        name = item.data(0, QtCore.Qt.UserRole) if item else ""
        if not name or name == tool_tabs.BASE:
            return ""
        return name[len(tool_tabs.NODE):] if name.startswith(tool_tabs.NODE) else name

    def layer_changed(self, force=False):
        """A corrective (or Sculpt node) picked or let go: the tiles that can't edit one greyed or back,
        the card's header saying what the next stroke edits. At once from the list (`force`), else on
        the poll when it sees a change."""
        layer = self.editing_layer()
        if force or layer != getattr(self, "_layer_shown", None):
            self._layer_shown = layer
            self.show_layer_tools()
            if self.tool:
                self.show_editing()

    def show_layer_tools(self):
        """On a corrective or Sculpt node, the tools that can't edit one are greyed (not hidden: the
        tiles never move); the active one, if it can't, stops."""
        layer = bool(self.editing_layer())
        for tool, tiles in self.tiles.items():
            for b in tiles:
                b.setEnabled(not layer or tool in CORRECTIVE or tool == "paint")
        if layer and self.tool and self.tool not in CORRECTIVE and self.tool != "paint":
            self.stop_tool()

    @staticmethod
    def setting_chip(setting, on=False):
        """A toggle chip for one of the catalog's settings: its short name (CHIP_LABELS) and hint."""
        b = widgets.chip(CHIP_LABELS.get(setting, SETTINGS[setting]["label"]), SETTINGS[setting]["hint"], on,
                         "chip_" + setting)
        return b

    def curve_setting(self, setting, keys, ends):
        """A brush curve of the catalog's: a chip (its label and hint) and a curve editor shown
        under the chips while the chip is on; the curve goes to the tool as it settles ("" when off).
        Returns (chip, editor)."""
        check = self.setting_chip(setting, False)
        check.setProperty("disclosure", True)  # (it opens an editor below: outlined, never a toggle's fill)
        editor = widgets.RampEditor(keys, ends=ends)
        editor.default_keys = [list(k) for k in keys]  # (a brush with no curve of its own yet)
        editor.setToolTip(SETTINGS[setting]["hint"] + " (right-click: presets)")
        editor.setVisible(False)
        flag = setting  # the tool's flag has the setting's name
        check.toggled.connect(lambda on: (editor.setVisible(on), self.push(**{flag: self.curve_of(check, editor)})))
        editor.committed.connect(lambda _keys: self.push(**{flag: self.curve_of(check, editor)}))
        editor.check = check
        return check, editor

    def alpha_setting(self):
        """Paint's stamp on a scalp map (the catalog's "alpha"): the picker, and while one is
        picked its rotation, angle, spacing and invert."""
        box = QtWidgets.QWidget()
        box.setProperty("role", "options")
        column = QtWidgets.QVBoxLayout(box)
        column.setContentsMargins(0, 0, 0, 0)
        column.setSpacing(theme.px(4))
        self.alpha = widgets.AlphaPicker()
        self.alpha.setToolTip(_setting("alpha", "hint"))
        self.alpha.changed.connect(self.alpha_changed)
        top, label = widgets.field_row("Stamp", self.alpha)
        label.setToolTip(_setting("alpha", "hint"))
        column.addWidget(top)
        self.alpha_options = QtWidgets.QWidget()
        options = QtWidgets.QVBoxLayout(self.alpha_options)
        options.setContentsMargins(0, 0, 0, 0)
        options.setSpacing(theme.px(4))
        self.alpha_rotation = QtWidgets.QComboBox()
        self.alpha_rotation.setObjectName("alphaRotation")
        for name, label_text, tip in (("fixed", "Fixed", "Every stamp at the angle"),
                                      ("stroke", "Follow Stroke", "Each stamp turned the way the stroke goes, plus the angle"),
                                      ("random", "Random", "Each stamp at a random angle")):
            self.alpha_rotation.addItem(label_text, name)
            self.alpha_rotation.setItemData(self.alpha_rotation.count() - 1, tip, QtCore.Qt.ToolTipRole)
        self.alpha_rotation.currentIndexChanged.connect(self.alpha_rotation_changed)
        rotation_row, _rotation = widgets.field_row("Rotation", self.alpha_rotation)
        options.addWidget(rotation_row)
        self.alpha_angle = _slider("alphaAngle", integer=True, label="Stamp angle", suffix=u"°")
        self.alpha_angle.setToolTip("The stamp's angle in degrees, counterclockwise (Follow Stroke: added to the stroke's way)")
        self.alpha_angle.committed.connect(lambda v: self.push(alphaAngle=v))
        self.alpha_angle.dragged.connect(lambda v: self.push(alphaAngle=v))
        options.addWidget(self.alpha_angle)
        # (the spec's is a share of the brush's diameter: shown in percent)
        self.alpha_spacing = _slider("alphaSpacing", 100.0, integer=True, key="palette.alphaSpacing", label="Stamp spacing",
                                     suffix="%")
        self.alpha_spacing.setToolTip("How far apart the stamps fall along a stroke, in percent of the brush's "
                                      "diameter: low for a smooth trail, 100 and up for separate stamps")
        self.alpha_spacing.committed.connect(lambda v: self.push(alphaSpacing=v / 100.0))
        options.addWidget(self.alpha_spacing)
        self.alpha_invert = widgets.switch("Invert", "Paint where the stamp is black instead", _default("alphaInvert"),
                                           "alphaInvert")
        self.alpha_invert.toggled.connect(lambda on: self.push(alphaInvert=on))
        options.addWidget(self.alpha_invert.row)
        self.alpha_options.setVisible(False)
        column.addWidget(self.alpha_options)
        return box

    def alpha_changed(self, path):
        self.alpha_options.setVisible(bool(path))
        self.push(alpha=path)

    def alpha_rotation_changed(self, _index):
        name = self.alpha_rotation.currentData()
        self.alpha_angle.setEnabled(name != "random")
        self.push(alphaRotation=name)

    def alpha_flags(self):
        """The alpha settings as brush_tool takes them (an alpha whose file is gone: none)."""
        self.alpha.set_path(self.alpha.path, emit=False)
        self.alpha_options.setVisible(bool(self.alpha.path))
        return dict(alpha=self.alpha.path, alpha_rotation=self.alpha_rotation.currentData(),
                    alpha_angle=self.alpha_angle.value(), alpha_spacing=self.alpha_spacing.value() / 100.0,
                    alpha_invert=self.alpha_invert.isChecked())

    def sync_alpha(self):
        """The stamp shows for Paint on a scalp map (vertices or texture), not on a guide map."""
        if not hasattr(self, "alpha_box"):
            return  # the Paint tab picks its map while the card is still being built
        on = "alpha" in TOOL_SETTINGS.get(self.card_tool(), set()) and self.paint_tab.current_kind() in ("vertices", "texture")
        self.alpha_box.setVisible(on)
        self.alpha_options.setVisible(on and bool(self.alpha.path))

    @staticmethod
    def curve_of(check, editor):
        """A curve setting as the tool takes it: the editor's keys as JSON, or "" while it is off."""
        import json
        return json.dumps(editor.value()) if check.isChecked() else ""

    def closeEvent(self, event):
        self.stop_following()
        super(ToolPalette, self).closeEvent(event)

    def stop_following(self):
        """The tabs stop following their groom (their Maya callbacks go), and the poll stops:
        closing, or Maya quitting."""
        self.timer.stop()
        self._unwatch_scene()
        groomlab.remove_current_groom_listener(self.current_groom_changed)
        for tab in (self.guides_tab, self.paint_tab, self.correctives, self.reference_tab):
            tab.unwatch()

    def _watch_scene(self):
        """File > Open: the opened scene's groom is the one worked on (groomlab.adopt_scene_groom)."""
        from maya.api import OpenMaya as om
        self._unwatch_scene()

        def opened(*_):
            if widgets.alive(self):
                QtCore.QTimer.singleShot(0, lambda: widgets.alive(self) and self.scene_opened())
            else:  # deleted without closing
                self._unwatch_scene()
        try:
            self._scene_jobs = [om.MSceneMessage.addCallback(om.MSceneMessage.kAfterOpen, opened)]
        except RuntimeError:
            self._scene_jobs = []

    def _unwatch_scene(self):
        from maya.api import OpenMaya as om
        for job in getattr(self, "_scene_jobs", []):
            try:
                om.MMessage.removeCallback(job)
            except RuntimeError:
                pass
        self._scene_jobs = []

    def scene_opened(self):
        groomlab.adopt_scene_groom()
        self._groom_rows = None
        self.poll()

    def showEvent(self, event):
        super(ToolPalette, self).showEvent(event)
        if not self.timer.isActive():  # closed and shown again: follow the groom once more
            for tab in (self.guides_tab, self.paint_tab, self.correctives, self.reference_tab):
                tab._watch()
                tab.refresh()
            self._watch_scene()
            groomlab.on_current_groom(self.current_groom_changed)
            self.timer.start()
        if not getattr(self, "_polled", False):  # the first show: the groom (or the empty state) at once
            QtCore.QTimer.singleShot(0, lambda: widgets.alive(self) and not getattr(self, "_polled", False) and self.poll())

    @staticmethod
    def heading(text):
        label = QtWidgets.QLabel(text.upper())
        label.setProperty("role", "heading")
        return label

    @staticmethod
    def tray(name, margins=(8, 8, 8, 8), spacing=4):
        """A card holding a section, its title inside (QFrame[role="tray"], components.tray)."""
        tray = QtWidgets.QFrame()
        tray.setObjectName(name)
        tray.setProperty("role", "tray")
        column = QtWidgets.QVBoxLayout(tray)
        column.setContentsMargins(*[theme.px(m) for m in margins])
        column.setSpacing(theme.px(spacing))
        return tray

    def apply_theme(self, t):
        """A new theme: the icons in its colors (the style sheet follows by itself)."""
        for tool, tiles in list(self.tiles.items()) + [(k, [self.buttons[k]]) for k in ("invert", "unfreeze")]:
            for button in tiles:
                self.paint_tile(button, tool, t)
        for name, button in self.header_icons.items():
            button.setIcon(widgets.tinted_icon("graphLine", t.token("textDim"), hover=t.color("ui.text"),
                                               disabled=t.token("textDisabled")))
        for button in self.findChildren(QtWidgets.QToolButton):
            if getattr(button, "glyph", None):
                button.setIcon(widgets.glyph(button.glyph))
        if self.tool:
            self.show_card_header(self.card_tool())

    @staticmethod
    def paint_tile(button, tool, t=None):
        """A tile's two-tone artwork in its group's color (the theme's icons.<group>), greyed while
        disabled; a theme whose ui.toolOn is "category" (Playful): the chosen tile a soft wash of
        its group's color, else the accent's ink."""
        t = t or theme.current()
        button.setIcon(widgets.tool_icon(tool, t))
        if t.get("ui.toolOn") == "category":
            group = next((g for g, names in widgets.ICON_GROUPS.items() if tool in names), "app")
            cat = t.hex(t.get("icons." + group, "accent"))
            card = t.token("card")
            button.setStyleSheet("QToolButton:checked { background: %s; border: %dpx solid %s; color: %s; }"
                                 % (theme._blend(card, cat, 0.22), max(1, theme.px(1)), theme._blend(card, cat, 0.55),
                                    t.color("ui.text")))
        else:
            button.setStyleSheet("")

    TILE_ICON = 28       # px at 100%: the tool tiles' icons
    TILE = (50, 56)      # room for a 28 px icon and its label (the grid stretches the width)
    MAX_COLUMNS = 5      # at most this many tiles to a row, however wide the panel

    def _tile_size(self, b):
        b.setIconSize(QtCore.QSize(theme.px(self.TILE_ICON), theme.px(self.TILE_ICON)))
        b.setToolButtonStyle(QtCore.Qt.ToolButtonTextUnderIcon)  # the icon over the name (elided: widgets.Tile)
        b.setAutoRaise(True)
        b.setMinimumSize(theme.px(self.TILE[0]), theme.px(self.TILE[1]))
        b.setMaximumHeight(theme.px(self.TILE[1]))
        b.setProperty("role", "tool")  # a rounded tile: the theme colors it (checked: the selection, an accent border)

    def unfreeze_tile(self):
        """Unfreeze All as a tile beside the Freeze tool's (a button, not a tool: never checked):
        every guide brushable again."""
        b = widgets.Tile()
        b.setObjectName("unfreezeAllTile")
        b.setText("Unfreeze All")
        b.short = TILE_WORDS.get("unfreeze")
        self.paint_tile(b, "unfreeze")
        self._tile_size(b)
        b.setToolTip("<b>Unfreeze All</b><br>Clear the freeze mask: every guide can be brushed again "
                     "(a click with the Select tool where no guide is does the same)")
        b.clicked.connect(lambda: self.paint_tab.unfreeze_all())
        self.buttons.setdefault("unfreeze", b)  # its icon follows the theme
        return b

    def invert_tile(self):
        """Invert Frozen as a tile between Freeze's and Unfreeze All's (a button, not a tool): the
        frozen guides and the free ones swap."""
        b = widgets.Tile()
        b.setObjectName("invertFrozenTile")
        b.setText("Invert Frozen")
        b.short = TILE_WORDS.get("invert")
        self.paint_tile(b, "invert")
        self._tile_size(b)
        b.setToolTip("<b>Invert Frozen</b><br>Swap the frozen guides and the free ones: the frozen can be brushed, "
                     "the rest freeze")
        b.clicked.connect(lambda: self.paint_tab.invert_frozen())
        self.buttons.setdefault("invert", b)  # its icon follows the theme
        return b

    def wide_tools(self, tools):
        """The Paint tab's tools as full-width buttons, the name beside the icon (Paint, over the
        maps), in a tray as the Tools tab's; one tool is active across every tab."""
        holder = self.tray("paintGrid")
        column = holder.layout()
        for tool, label, tip in tools:
            b = widgets.Tile()
            b.setText(label)
            self.paint_tile(b, tool)
            b.setCheckable(True)
            b.setIconSize(QtCore.QSize(theme.px(self.TILE_ICON), theme.px(self.TILE_ICON)))
            b.setToolButtonStyle(QtCore.Qt.ToolButtonTextBesideIcon)
            b.setAutoRaise(True)
            b.setFixedHeight(theme.px(38))
            b.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
            b.setProperty("role", "tool")
            b.setProperty("wide", True)
            b.setToolTip(help_tip(tool) if tool in HINTS else "<b>%s</b><br>%s" % (NAMES.get(tool, label), tip))
            b.clicked.connect(lambda _=False, t=tool: self.activate(t))
            self.group.addButton(b)
            self.buttons.setdefault(tool, b)
            self.tiles.setdefault(tool, []).append(b)
            column.addWidget(b)
        return holder

    def tool_grid(self, tools, extra=()):
        """Labeled tiles (tool, label, tooltip: the icon over the name, the name and the hint in the
        tooltip) filling the panel's width, as many to a row as it holds, and any `extra`
        buttons after them; one tool is active across every tab."""
        tiles = []
        for tool, label, tip in tools:
            b = widgets.Tile()
            b.setText(label)
            b.short = TILE_WORDS.get(tool)
            self.paint_tile(b, tool)
            b.setCheckable(True)
            self._tile_size(b)
            b.setToolTip(help_tip(tool) if tool in HINTS else "<b>%s</b><br>%s" % (NAMES.get(tool, label), tip))
            b.setProperty("toolId", tool)
            b.clicked.connect(lambda _=False, t=tool: self.activate(t))
            self.group.addButton(b)
            self.buttons.setdefault(tool, b)
            self.tiles.setdefault(tool, []).append(b)
            tiles.append(b)
        tiles.extend(extra)
        return widgets.ToolGrid(tiles, columns=self.MAX_COLUMNS)

    def check_tiles(self, tool):
        """The active tool's tiles on, every other off."""
        for t, tiles in self.tiles.items():
            for b in tiles:
                b.setChecked(t == tool)

    def show_tab(self, name):
        """Show tab `name`; "guides" is the Tools tab with its guide groups open."""
        if name == "guides":
            self.tabs.setCurrentIndex(TABS.index("tools"))
            self.guides_section.set_expanded(True)
        else:
            self.tabs.setCurrentIndex(TABS.index(TAB_ALIASES.get(name, name)))

    def current_tab(self):
        return TABS[self.tabs.currentIndex()] if self.tabs.currentIndex() >= 0 else TABS[0]

    def card_tool(self):
        """The tool the card shows: the Paint tab's is Paint's (the brush it starts), the Photos
        tab has none; else the active tool."""
        tab = self.current_tab()
        if tab == "paint":
            return "paint"
        if tab == "photos":
            return None
        return self.tool if self.tool != "paint" else None

    def show_sections(self):
        """The tool grid and the sections the tab uses (palette.json's "tabs"): the sculpt
        tools on Tools and Correctives, the Paint tile on Paint; the guide groups on Tools."""
        tab = self.current_tab()
        self.tool_palette.setVisible(tab == "tools")
        self.paint_tiles.setVisible(False)  # (the Paint tab is painting: its card on top, no tile)
        self.show_layer_tools()
        groups = section_shown(self.section_rules["groups"], self.tool, tab)
        self.guides_tab.setVisible(groups)  # (its card with it)
        if tab == "tools":
            self.tabs.widget(TABS.index("tools")).setVisible(groups)  # (no empty card)
        # The card under the tools; on Paint the maps come first (what is painted, then how).
        work = self.work_layout
        want = [self.hint, self.settings, self.tabs]  # the card, then the list (Paint: what it paints first)
        if [work.itemAt(i).widget() for i in range(work.count())] != want:
            for w in want:
                work.removeWidget(w)
            for w in want:
                work.addWidget(w)

    def tab_changed(self, index):
        self.show_sections()
        if TABS[index] == "paint" and self.tool != "paint" and self.groom() and self.paint_tab.current():
            # The Paint tab is painting: it starts Paint on the picked map (the brush before kept).
            self._before_paint = self.tool
            self.activate("paint")
            return
        if TABS[index] != "paint" and self.tool == "paint":
            # Leaving the Paint tab is done painting: the strands and scalp stop showing the map, and
            # the brush picked before Paint comes back.
            before = getattr(self, "_before_paint", None)
            self._before_paint = None
            if before and before != "paint" and before in self.buttons:
                self.activate(before)
            else:
                self.stop_tool()
            return
        self.show_tool_settings()

    # -- Groom and syncing ---------------------------------------------------

    def groom(self):
        return groomlab.active_groom()

    def fill_grooms(self, active):
        """The groom choice: every groom, the current one picked (the Graph Editor's choice too,
        unless it is pinned). The eyes show or hide the current groom's hair and its guides."""
        from maya import cmds
        shapes = sorted(cmds.ls(type=groomlab.GROOM_TYPE) or [])
        rows = [(shape, self._short((cmds.listRelatives(shape, parent=True) or [shape])[0])) for shape in shapes]
        visible = guides = None
        if active and cmds.objExists(active):
            visible, guides = groomlab.hair_visible(active), groomlab.guides_visible(active)
        current = (cmds.ls(active) or [active])[0] if active and cmds.objExists(active) else None
        state = (rows, current, visible, guides)
        if state == getattr(self, "_groom_rows", None):
            return
        self._groom_rows = state
        self.grooms.blockSignals(True)
        self.grooms.clear()
        for key, text in rows:
            self.grooms.addItem(text, key)
        self.grooms.setCurrentIndex(self.grooms.findData(current) if current else -1)
        self.grooms.blockSignals(False)
        for button, on, glyphs in ((self.visible, visible, ("eye", "eyeOff")),
                                   (self.guides_eye, guides, ("guides", "guidesOff"))):
            button.blockSignals(True)
            button.setEnabled(on is not None)
            button.setChecked(bool(on))
            button.glyph = glyphs[0] if on or on is None else glyphs[1]
            button.setIcon(widgets.glyph(button.glyph))
            button.blockSignals(False)
        # Nothing to work on: how to start a groom, in place of the tabs and tools.
        self.empty.setVisible(not active)
        self.work.setVisible(bool(active))  # no tabs or tools with nothing to use them on
        self.empty_text.setText(
            "Pick a groom above, or select a scalp mesh (and any guide curves) to grow a new one." if shapes else
            "Select the scalp mesh, and any curves to start the guides from, then:")

    def show_selection(self):
        """No groom yet: what is selected (a mesh: the scalp; curves: the guides), and Create
        waits for a mesh."""
        from maya import cmds
        try:
            picked = cmds.ls(selection=True, long=False) or []
            meshes = [n for n in picked if cmds.listRelatives(n, shapes=True, type="mesh") or
                      cmds.nodeType(n) == "mesh"]
            curves = [n for n in picked if cmds.listRelatives(n, shapes=True, type="nurbsCurve")]
        except (RuntimeError, ValueError, TypeError):
            meshes, curves = [], []
        if meshes:
            text = "Selected: %s (the scalp)%s" % (meshes[0].split("|")[-1],
                                                   " and %d curves (the guides)" % len(curves) if curves else "")
        else:
            text = "Nothing selected: select the scalp mesh"
        if self.empty_selection.text() != text:
            self.empty_selection.setText(text)
        if self.empty_create is not None:
            self.empty_create.setEnabled(bool(meshes))

    @staticmethod
    def _short(name):
        return name.split("|")[-1]

    def groom_picked(self, index):
        """The picked groom is the current groom: the tools, and the Graph Editor unless it is
        pinned, work on it (until another groom is picked or selected)."""
        key = self.grooms.itemData(index)
        if key:
            groomlab.set_current_groom(key)
        self._groom_rows = None
        QtCore.QTimer.singleShot(0, self.poll)  # after the pick: the list may change under it

    def current_groom_changed(self, _groom):
        """groomlab.on_current_groom: a pick in the Graph Editor, a groom selected or made: the
        choice follows at once (not at the next tick)."""
        if not widgets.alive(self):  # deleted without closing
            groomlab.remove_current_groom_listener(self.current_groom_changed)
            return
        self._groom_rows = None
        QtCore.QTimer.singleShot(0, lambda: widgets.alive(self) and self.poll())

    @staticmethod
    def _every_groom():
        """Shift held on an eye's click: it is for every groom in the scene."""
        return bool(QtWidgets.QApplication.keyboardModifiers() & QtCore.Qt.ShiftModifier)

    def _eye(self, what, on, one):
        from maya import cmds
        from groomlab.ui import menu
        if self._every_groom():
            menu.show_all(what, on)
        else:
            groom = self.groom()
            if not groom or not cmds.objExists(groom):
                return
            one(groom, on)
        self._groom_rows = None
        self.fill_grooms(self.groom())  # the eyes' glyphs now

    def hair_visibility(self, on):
        """The eye: show or hide the current groom's hair, the strands alone (its displayHair;
        Shift+click: every groom's)."""
        self._eye("hair", on, groomlab.set_hair_visible)

    def guides_visibility(self, on):
        """The guides' eye: show or hide every guide group of the current groom (its displayGuides;
        Shift+click: every groom's guides)."""
        self._eye("guides", on, groomlab.set_guides_visible)

    def apply_isolation(self):
        groom = self.groom()
        if groom:
            groomlab.isolate_group(groom, self.current_group() if self.isolate.isChecked() else None)

    def sync(self, from_negative):
        from groomlab.ui import menu
        menu.symmetry_sync(from_negative)

    # -- The target group --------------------------------------------------------

    def current_group(self):
        """The group picked for brushes and new guides ("" for all groups)."""
        return self.level.currentData(QtCore.Qt.UserRole + 1) or ""

    def target_changed(self, _index=None):
        self.show_picked_level()
        self._target = (self.current_group(), self.level.currentData())
        self.push(level=self.level.currentData(), group=self.current_group())
        self.apply_isolation()
        self.guides_tab.show_picked()  # the list's row and Segments follow the pick
        if self.tool:
            self.show_editing()  # the card's header: what the next stroke edits

    def show_picked_level(self):
        """A level of a tube hierarchy picked (the list, Page Up / Down, the Roots card): its guides
        alone (the level view; Mixed view: with the tubes of the levels above it does not continue),
        the eyes as the artist left them; another group or all groups: every group as its eye
        says. The level kept in view (a level row's tube) stays."""
        group = self.current_group()
        groom = self.groom()
        if (groom, group) == getattr(self, "_level_shown", None):
            return
        self._level_shown = (groom, group)
        if not groom:
            return
        from groomlab import root_tubes
        info = self.tube_hierarchy(groom, group) if group else None
        try:
            if info and group in info["levels"]:
                root_tubes.show_level_guides(groom, info["base"], info["levels"].index(group) + 1,
                                             solo=not self.root_card.mixed_view.isChecked())
            else:
                root_tubes.set_mixed_view(groom, "", 0)
        except (RuntimeError, ValueError) as e:
            self.status.setText(str(e).strip())

    def pick_group(self, name):
        """Brushes and new guides use group `name` (from the groups list; "" for all groups)."""
        index = next((i for i in range(self.level.count())
                      if self.level.itemData(i, QtCore.Qt.UserRole + 1) == (name or "")), -1)
        if index >= 0:
            self.level.setCurrentIndex(index)

    def new_group(self):
        """Ask for a name (and a parent group), make the group and work in it."""
        groom = self.groom()
        if not groom:
            self.status.setText("Select a groom first.")
            return
        dialog = QtWidgets.QDialog(self)
        dialog.setWindowTitle("New Guide Group")
        form = QtWidgets.QFormLayout(dialog)
        name = QtWidgets.QLineEdit()
        name.setPlaceholderText("e.g. bangs, beard, flyaways")
        parent = QtWidgets.QComboBox()
        parent.addItem("None: a top-level group", "")
        for g in groomlab.list_guide_groups(groom):
            parent.addItem("child of %s" % g["name"], g["name"])
        buttons = QtWidgets.QDialogButtonBox(QtWidgets.QDialogButtonBox.Ok | QtWidgets.QDialogButtonBox.Cancel)
        buttons.accepted.connect(dialog.accept)
        buttons.rejected.connect(dialog.reject)
        form.addRow("Name", name)
        form.addRow("Parent", parent)
        form.addRow(buttons)
        theme.style(dialog)
        if qt.exec_(dialog) != QtWidgets.QDialog.Accepted or not name.text().strip():
            return
        group = name.text().strip()
        try:
            group = groomlab.add_guide_group(groom, group, parent.currentData() or None)  # one word: spaces as '_'
        except (RuntimeError, ValueError) as e:
            self.status.setText(str(e).strip())
            return
        self.fill_levels(groom, select=group)
        self.guides_tab.refresh()
        self.status.setText("New guides go into %s." % group)

    def add_sample(self, sample):
        import groomlab
        try:
            part = groomlab.add_sample(sample)
        except Exception as e:  # noqa: BLE001 (a button never ends in a bare traceback)
            from groomlab.ui import messages
            self.status.setText("The sample wasn't added: %s" % messages.error("Add Sample Mesh", e))
            return
        self.status.setText("Added %s: now Create Groom from Selection." % part)

    def create_groom(self):
        from groomlab.ui import menu
        if not menu.selected_meshes():
            self.status.setText("Select a scalp mesh (or its faces) first.")
            return
        try:
            menu.create_groom_from_selection()
        except Exception as e:  # noqa: BLE001 (a button never ends in a bare traceback)
            from groomlab.ui import messages
            self.status.setText("The groom wasn't made: %s" % messages.error("Create Groom from Selection", e))
            return
        self.status.setText("")
        self.poll()

    def step_level(self, delta):
        """Move the Guides choice one level (Page Up / Page Down)."""
        index = self.level.currentIndex()
        levels = [self.level.itemData(i) for i in range(self.level.count())]
        current = levels[index] if index >= 0 else 0
        candidates = sorted(l for l in levels if l is not None and l >= 0)
        if not candidates:
            return
        target = min(candidates, key=lambda l: abs(l - (max(current, 0) + delta)))
        self.level.setCurrentIndex(levels.index(target))

    # The tick: quick while a GroomLab tool is on (its radius and strength follow middle-drags in the
    # viewport), slower otherwise (the selection, the grooms, edits made elsewhere).
    POLL_TOOL_MS = 400
    POLL_IDLE_MS = 1000

    def poll(self):
        """Follow the selection and the tool's settings changed in the viewport (middle-drag). The
        scene-wide reads (every groom, the photos) run when the active groom changes and every few
        ticks; the rest only reads what the current tool or tab shows."""
        if not self.isVisible():
            return
        app = QtWidgets.QApplication.instance()
        if app.activePopupWidget() or app.activeModalWidget():  # a menu or dialog is open: wait
            return
        self._polled = True
        from maya import cmds
        groom = groomlab.active_groom()
        # The node, not just its name: a new scene's groom of the same name is another groom.
        node = (groom, (cmds.ls(groom, uuid=True) or [""])[0] if groom else "")
        # Levels and layers: on a groom change, and every few seconds for edits made elsewhere.
        self._ticks = getattr(self, "_ticks", 0) + 1
        if node != getattr(self, "_groom_key", "?") or self._ticks % 3 == 0:
            self._groom_key = node
            self.fill_grooms(groom)
        if node != getattr(self, "_last_groom", "?"):
            self._last_groom = node
            for tab in (self.guides_tab, self.paint_tab, self.correctives, self.reference_tab):
                tab.set_groom(groom)
        # The group list follows the guides through the Guides tab (its callback on guideData); here
        # only a new groom. The first-groom card: now and then until it is done or dismissed.
        if groom != getattr(self, "_levels_groom", "?"):
            self._levels_groom = groom
            self.fill_levels(groom, select=self.current_group())
        if not groom:
            self.show_selection()
        if self.tool == PART:
            self.fill_parts(groom)
        if self.tool == "flow" and (self._ticks % 2 == 0 or groom != getattr(self, "_flow_groom", None)):
            self._flow_groom = groom
            self.fill_flow_count(groom)
        if self.tool == "clone":  # a Ctrl+click captured, or Pick Source's click used
            self.fill_clone()
        if self.length_keys.isVisible():  # the keys clicked and dragged in the viewport
            self.length_keys.refresh()
        if any(b.isVisible() for b in self.tube_panel.blocks):  # (Shape, Select, Place, Draw) a selection or solo
            self.tube_panel.refresh(groom, full=self._ticks % 3 == 0)
        if self.root_card.visible():  # the root tool's state; the hierarchy's levels now and then
            self.root_card.refresh(groom, full=self._ticks % 5 == 0)
        # Weights change with the frame: read them every tick, the stack itself now and then
        # (keys edited in the timeline) and whenever the groom's correctives change (its callback);
        # only while the Correctives box shows.
        self.correctives.tick(full=self._ticks % 6 == 0)
        self.layer_changed()  # (a corrective picked elsewhere)
        if self.reference_tab.isVisible():  # the photos follow the scene and the viewport's camera
            self.reference_tab.poll(full=self._ticks % 5 == 0)
        ctx = groomlab.BRUSH_CONTEXT
        current = cmds.currentCtx()
        wanted = self.POLL_TOOL_MS if current in (ctx, groomlab.SELECT_CONTEXT) else self.POLL_IDLE_MS
        if self.timer.interval() != wanted:
            self.timer.setInterval(wanted)
        mode = self.context_tool()
        if mode is not None:
            if mode != self.tool and mode in self.buttons:
                if self.tool == "paint":
                    self.paint_tab.end_painting()  # the brush left Paint (a hotkey, the tool settings)
                self.tool = mode
                self.buttons[mode].setChecked(True)
                self.leave_paint_tab()
                self.show_tool_settings()
            # (the Paint card while another brush is on shows Paint's own: left alone)
            if not self.radius.busy() and self._brush_shown == self.tool:
                self.radius.setValue(cmds.groomLabBrushCtx(ctx, query=True, radius=True))
            # The group or level picked from the viewport (the level keys) shows here too.
            if self.tool != PART:
                picked = cmds.groomLabBrushCtx(ctx, query=True, group=True)
                if picked:
                    index = next((i for i in range(self.level.count())
                                  if self.level.itemData(i, QtCore.Qt.UserRole + 1) == picked), -1)
                else:
                    index = self.level.findData(cmds.groomLabBrushCtx(ctx, query=True, level=True))
                if index >= 0 and index != self.level.currentIndex():
                    self.level.blockSignals(True)
                    self.level.setCurrentIndex(index)
                    self.level.blockSignals(False)
                    self._target = (self.current_group(), self.level.currentData())
                    self.apply_isolation()
            if not self.strength.busy():
                strength = cmds.groomLabBrushCtx(ctx, query=True, strength=True)
                if self._brush_shown == self.tool:
                    self.strength.setValue(strength)
                if self.tool in self.strengths:
                    self.strengths[self.tool] = strength
        elif current == groomlab.SELECT_CONTEXT and self.tool != "select":
            if self.tool == "paint":
                self.paint_tab.end_painting()  # the Select tool from elsewhere (a shelf button, a script)
            self.tool = "select"
            self.buttons["select"].setChecked(True)
            self.show_tool_settings()
        elif current not in (ctx, groomlab.SELECT_CONTEXT) and self.tool is not None:
            self.clear_tool()  # another Maya tool took over: nothing is active here

    def fill_levels(self, groom, select=None, groups=None):
        """The group choices: All groups, then each group (children under their parents).
        `groups`: groomlab.list_guide_groups(groom) when the caller has read it."""
        role = QtCore.Qt.UserRole + 1
        wanted = select if select is not None else self.current_group()
        first = getattr(self, "_target", None) is None  # a new window starts on the first group
        if groups is None:
            groups = []
            if groom:
                try:
                    groups = groomlab.list_guide_groups(groom)
                except RuntimeError:
                    groups = []
        items = [("All groups", -1, "")]
        ordered = []

        def walk(parent, depth):
            for g in groups:
                if g["parent"] == parent:
                    ordered.append(("    " * depth + g["name"], g["depth"], g["name"]))
                    walk(g["name"], depth + 1)
        walk(None, 0)
        items += ordered or [("primary", 0, "primary")]  # a new groom: Place starts the primary group
        state = [(text, level, group) for text, level, group in items]
        if state == getattr(self, "_level_items", None) and wanted == self.current_group():
            return
        self._level_items = state
        self.level.blockSignals(True)
        self.level.clear()
        for text, level, group in items:
            self.level.addItem(text, level)
            self.level.setItemData(self.level.count() - 1, group, role)
        index = -1 if first else next((i for i in range(self.level.count()) if self.level.itemData(i, role) == wanted), -1)
        if index < 0:
            index = next((i for i in range(self.level.count()) if self.level.itemData(i) == 0), 0)
        self.level.setCurrentIndex(index)
        self.level.blockSignals(False)
        if self.current_group() != getattr(self, "_target", ("?",))[0]:
            self._target = (self.current_group(), self.level.currentData())
            self.push(level=self.level.currentData(), group=self.current_group())

    # -- Tools -----------------------------------------------------------------

    def activate(self, tool):
        from maya import cmds
        if tool == "paint" and self.tool == "paint" and cmds.currentCtx() == groomlab.BRUSH_CONTEXT:
            self.stop_tool()  # the Paint tile again: done painting
            return
        if tool != "paint":
            self.paint_tab.end_painting()
        if tool != "clone":
            groomlab.clone_reset()  # another tool: Clone's source let go (Select's context too)
        if tool != "roots" and self.root_card.tool.active():
            self.root_card.tool.leave()  # another tool: root mode left (an edit pending finished)
        self.tool = tool
        self.check_tiles(tool)
        self.leave_paint_tab()
        self.sync_brush()  # the tool's own radius, falloff and curves, before they go to it
        self.status.setText("")
        if not self.groom():
            self.status.setText("Select a groom to use the tools.")
        if tool == "select":
            groomlab.select_tool(level=max(self.level.currentData() or 0, 0), group=self.current_group(),
                                 symmetry=self.symmetry_name())
            self.show_tool_settings()
            return
        if tool == "freeze":
            try:
                groomlab.freeze_tool(radius=self.radius.value(), strength=self.strengths.get("freeze", 1.0),
                                     level=self.level.currentData() if self.level.currentData() is not None else 0,
                                     group=self.current_group(),
                                     symmetry=self.symmetry_name(),
                                     pressure=self.pressure.isChecked())
            except (RuntimeError, ValueError) as e:
                self.tool_failed(e)
            self.show_tool_settings()
            return
        if tool == PART:
            groom = self.groom()
            if groom:
                try:
                    groomlab.draw_part_tool(groom, name=self.part_name.text() or "part",
                                            symmetry=self.symmetry_name())
                except (RuntimeError, ValueError) as e:
                    self.tool_failed(e)
            self.fill_parts(groom)
            self.show_tool_settings()
            return
        if tool == "roots":
            # The root tube tool (the brush context's "roots" mode), drawing a root to start with.
            self.show_tool_settings()
            self.root_card.start("place")
            self.push_tube_display()  # (the root tubes drawn as the guides display says)
            self.push_help("roots")
            return
        kwargs = dict(strength=self.strengths.get(tool, 1.0),
                      level=self.level.currentData() if self.level.currentData() is not None else 0,
                      # Selected only and Front only are not in the panel: the brushes' defaults.
                      group=self.current_group(), selected_only=False,
                      erase_guides=self.erase_guides.isChecked(), erase_parts=self.erase_parts.isChecked(),
                      erase_flow=self.erase_flow.isChecked(), stretch=self.stretch.isChecked(),
                      straighten=self.straighten.isChecked(),
                      tube_channel=self.tube_channel_name(),
                      flow_sink=self.flow_sink.currentData() or "",
                      symmetry=self.symmetry_name(), pressure=self.pressure.isChecked(),
                      collide=self.collide.isChecked(), mask_by_scalp=True,
                      match_neighbors=self.match.isChecked(), solo_new_guides=self.solo_new.isChecked(),
                      clone_replace=self.clone_replace.isChecked(),
                      length=self.length.value() / self.length_factor, segments=self.segments.value(),
                      spacing=self.spacing.value() / self.length_factor,
                      value=self.paint_tab.value.value(),
                      targets=self.targets_name(),
                      smooth_strength=self.smooth_strength.value(),
                      tube=self.tube_panel.placement())
        kwargs.update(self.brush_flags())
        if tool == "paint":
            self.show_tab("paint")
            self.sync_brush()  # (the Paint card's, now its tab shows)
            kwargs.update(self.brush_flags())
            name = self.paint_tab.current() or self.paint_tab.new_map()
            if not name:
                self.status.setText("Make a map to paint (New...).")
                return
            kwargs["attribute"] = name
            kwargs.update(self.paint_tab.paint_flags())
            kwargs.update(self.alpha_flags())
            if self.groom():
                self.paint_tab.show_while_painting(name)
        try:
            groomlab.brush_tool(tool, **kwargs)
            self.push_tube_display()  # (the context's own: not stroke settings)
            self.push_help(tool)
        except (RuntimeError, ValueError) as e:
            self.tool_failed(e)
        self.show_tool_settings()
        try:
            cmds.setFocus("MayaWindow")  # keyboard shortcuts go back to Maya
        except RuntimeError:
            pass

    def leave_paint_tab(self):
        """Another brush picked (a hotkey, the marking menu) while the Paint or Photos tab shows:
        the Tools tab, where its card is."""
        if self.tool not in (None, "paint") and self.current_tab() in ("paint", "photos"):
            self.show_tab("tools")

    def clear_tool(self):
        """No tool here: every tile off, the card back to how to start."""
        self.check_tiles(None)
        self.tool = None
        self.paint_tab.end_painting()  # the map shown while painting goes back to how it was
        self.show_tool_settings()

    def stop_tool(self):
        """No GroomLab tool: Maya's Select tool, every tile off."""
        from maya import cmds
        self.clear_tool()
        cmds.setToolTo("selectSuperContext")

    def tool_failed(self, error):
        """A tool that couldn't start: say why, and leave no tool active, so a drag
        in the viewport doesn't quietly use the one picked before."""
        self.status.setText("%s: %s" % ((self.tool or "tool").capitalize(), str(error).strip()))
        self.stop_tool()

    def show_tool_settings(self):
        """The card shows what its tool uses (the catalog's settings for it: card_tool()), and
        nothing else; with no tool, one line on how to start. Its blocks, in order: Brush, the
        tool's own, Applies to (Paint: its own block, then Brush, titled)."""
        self.sync_brush()
        tool = self.card_tool()
        self.keep_scroll(tool)
        uses = TOOL_SETTINGS.get(tool, set())
        photos = self.current_tab() == "photos"
        self.settings.setVisible(not photos and tool is not None)
        self.hint.setVisible(not photos and tool is None)
        self.show_card_header(tool)
        brush = "radius" in uses
        self.brush_block.setVisible(brush)
        for slider in (self.radius, self.strength, self.falloff):
            slider.setEnabled(brush)
        # (Clone has a circle but no strength or falloff.)
        self.strength.setVisible(not brush or "strength" in uses)
        self.falloff.setVisible(not brush or "falloff" in uses)
        self.smooth_strength.setVisible("smoothStrength" in uses)
        self.pressure.row.setVisible("pressure" in uses)
        self.pressure.setEnabled("pressure" in uses)
        self.collide.row.setVisible("collide" in uses)
        for chip_, editor, setting in ((self.falloff_curve_box, self.falloff_curve, "falloffProfile"),
                                       (self.strength_curve_box, self.strength_curve, "strengthProfile")):
            chip_.setVisible(setting in uses)
            editor.setVisible(setting in uses and chip_.isChecked())
        self.brush_chips.setVisible("falloffProfile" in uses or "strengthProfile" in uses)
        paints = "map" in uses
        self.paint_block.setVisible(paints)
        self.brush_block.set_titled(False)  # (the card's header names the tool)
        self.sync_alpha()
        self.create_box.setVisible("newGuides" in uses)
        self.spacing.setVisible("spacing" in uses)
        self.solo_new.row.setVisible("newGuides" in uses)
        # The tubes: Place and Draw give new guides one (Tube); Shape and Select edit the selected
        # guides' (Profile, Selected guides).
        new_tube = "tube" in uses and "newGuides" in uses
        edit_tube = "tubeChannel" in uses
        panel = self.tube_panel
        panel.tube_block.setVisible(new_tube)
        panel.profile_block.setVisible(edit_tube)  # (the Tube brush: always, under its brush settings)
        panel.selected_block.setVisible(edit_tube)
        if new_tube or edit_tube:
            panel.set_mode("new" if new_tube else "edit")
            panel.refresh(self.groom())
        self.stretch.row.setVisible("stretch" in uses)
        self.straighten.row.setVisible("straighten" in uses)
        self.tube_channel_row.setVisible("tubeChannel" in uses)
        # The levels' row: Tube's and Roots'; the root block: Roots'.
        self.root_card.show("tubeLevels" in uses, roots="rootTubes" in uses)
        self.select_box.setVisible("selection" in uses)
        self.erase_box.setVisible("erases" in uses)
        self.keys_block.setVisible("lengthKeys" in uses)
        if "lengthKeys" in uses:
            self.length_keys.refresh()
        self.flow_box.setVisible("flow" in uses)
        if "flow" in uses:
            self.fill_flow_count(self.groom())
        self.part_box.setVisible("parts" in uses)
        self.clone_box.setVisible("clone" in uses)
        if "clone" in uses:
            self.fill_clone()
        # The toggles row (Mirror first) and More: what the tool uses.
        tube_card = tool == "shape"
        self.symmetry.setVisible("symmetry" in uses)
        self.symmetry_row.setVisible("symmetry" in uses)
        self.targets_row.setVisible("targets" in uses and not tube_card)
        self.paint_tab.whole_faces.setVisible(paints)
        self.toggles.setVisible(any(b.isVisibleTo(self.toggles) for b in (
            self.symmetry, self.pressure, self.collide, self.stretch, self.straighten,
            self.paint_tab.whole_faces)) and tool is not None)
        self.more_section.setVisible(tool is not None and (brush or "symmetry" in uses or "targets" in uses
                                                           or "newGuides" in uses or "map" in uses))
        self.applies_block.setVisible(False)
        if tool is not None:
            self.show_editing()
        self.show_sections()
        if tool in self.strengths:
            signed = tool in SIGNED
            span = SETTINGS["strength"]  # (a signed tool's: as far the other way)
            self.strength.setRange(-span["soft_max"] if signed else span["soft_min"], span["soft_max"])
            self.strength.setValue(self.strengths[tool])
            self.strength.setLabel("Strength (±)" if signed else "Strength")

    def keep_scroll(self, tool):
        """Each tool's card scrolled where it was left (issue #114). Where each tool's card is
        scrolled is noted as it is scrolled (a wheel, the bar, keys: never the panel's own moves
        as cards change height), and a tool shown again goes back there, held there while its
        card settles (its blocks showing, hiding and refilling over the next moment); a tool not
        shown before starts at the top."""
        bar = self.scroll.verticalScrollBar()
        if not getattr(self, "_scroll_hooked", False):
            self._scroll_hooked = True
            self._scrolls = {}
            self._scroll_hold = None
            bar.actionTriggered.connect(lambda *_: QtCore.QTimer.singleShot(0, self._note_scroll))
            bar.rangeChanged.connect(lambda *_: self._hold_scroll())
        if tool == getattr(self, "_scroll_tool", False):
            return
        self._scroll_tool = tool
        self._scroll_hold = (tool, self._scrolls.get(tool, 0), QtCore.QElapsedTimer())
        self._scroll_hold[2].start()
        self._hold_scroll()
        for wait in (0, 60, 250):
            QtCore.QTimer.singleShot(wait, self._hold_scroll)

    def _note_scroll(self):
        """Scrolled by hand: where the current tool's card is now (and no longer held)."""
        self._scroll_hold = None
        self._scrolls[getattr(self, "_scroll_tool", None)] = self.scroll.verticalScrollBar().value()

    def _hold_scroll(self):
        hold = getattr(self, "_scroll_hold", None)
        if not hold:
            return
        tool, want, since = hold
        if tool != getattr(self, "_scroll_tool", False) or since.elapsed() > 500:
            self._scroll_hold = None
            return
        bar = self.scroll.verticalScrollBar()
        bar.setValue(min(want, bar.maximum()))

    # -- Each brush's own settings ----------------------------------------------------
    # Radius, falloff and the two curves are kept per brush (strength too: self.strengths), so a
    # brush picked again has them as it was left; one never used starts from the defaults.

    def context_tool(self):
        """The brush the viewport's GroomLab brush is on now (Freeze: Paint on the freeze map), or None."""
        from maya import cmds
        ctx = groomlab.BRUSH_CONTEXT
        if cmds.currentCtx() != ctx or not cmds.contextInfo(ctx, exists=True):
            return None
        mode = cmds.groomLabBrushCtx(ctx, query=True, mode=True)
        if mode == "paint" and cmds.groomLabBrushCtx(ctx, query=True, attribute=True) == "freeze":
            mode = "freeze"
        return mode

    def sync_brush(self):
        """The Brush block follows the card's tool: the one it showed keeps what it shows, and the
        card's own come back."""
        tool = self.card_tool()
        if tool == self._brush_shown:
            return
        self.remember_brush()
        self.recall_brush(tool)

    def remember_brush(self):
        """Keep the Brush block's settings for the tool it shows (a radius or strength just
        changed in the viewport too: the brush's own, not yet polled)."""
        tool = self._brush_shown
        if tool is None or "radius" not in TOOL_SETTINGS.get(tool, set()):
            return
        radius = self.radius.value()
        if self.context_tool() == tool:
            from maya import cmds
            ctx = groomlab.BRUSH_CONTEXT
            radius = cmds.groomLabBrushCtx(ctx, query=True, radius=True)
            if tool in self.strengths:
                self.strengths[tool] = cmds.groomLabBrushCtx(ctx, query=True, strength=True)
        self.brushes[tool] = {
            "radius": radius, "falloff": self.falloff.value(),
            "falloffProfile": (self.falloff_curve.check.isChecked(), self.falloff_curve.value()),
            "strengthProfile": (self.strength_curve.check.isChecked(), self.strength_curve.value())}

    def recall_brush(self, tool):
        """Show the tool's own brush settings (the defaults for one not used yet); nothing is
        sent: the tool takes them as it starts (activate)."""
        self._brush_shown = tool
        if tool is None or "radius" not in TOOL_SETTINGS.get(tool, set()):
            return
        kept = self.brushes.get(tool, {})
        self.radius.setValue(kept.get("radius", DEFAULT_RADIUS))
        self.falloff.setValue(kept.get("falloff", DEFAULT_FALLOFF))
        for setting, editor in (("falloffProfile", self.falloff_curve), ("strengthProfile", self.strength_curve)):
            on, keys = kept.get(setting, (False, editor.default_keys))
            editor.check.blockSignals(True)
            editor.check.setChecked(on)
            editor.check.blockSignals(False)
            editor.setValue(keys)

    def brush_flags(self):
        """The Brush block's radius, falloff and curves as brush_tool takes them."""
        return dict(radius=self.radius.value(), hardness=1.0 - self.falloff.value(),
                    falloff_profile=self.curve_of(self.falloff_curve.check, self.falloff_curve),
                    strength_profile=self.curve_of(self.strength_curve.check, self.strength_curve))

    def show_card_header(self, tool):
        """The card's header: the tool's icon (in the accent's ink) and name, then what it does."""
        if tool is None:
            return
        t = theme.current()
        self.card_icon.setPixmap(widgets.tinted_pixmap(tool, t.token("accentInk"), theme.px(18)))
        self.card_title.setText(NAMES.get(tool, tool.title()))
        # One line (the app's): the hint's first clause and the gesture every brush shares; the
        # whole hint on the tool name's hover.
        whole = HINTS.get(tool, "")
        category = next((t["category"] for t in CATALOG if t["id"] == tool), "")
        self.card_hint.setText(short_hint(whole, category) if whole else "")
        self.card_hint.setVisible(False)  # (one header line: the tile's and the title's tooltip have it)
        self.card_title.setToolTip(whole)

    def push(self, **flags):
        """Send changed settings to the active tool (the Paint card on the Paint tab while another
        brush is on: kept for Paint, which reads them when it starts)."""
        from maya import cmds
        ctx = groomlab.BRUSH_CONTEXT
        if self.tool in (None, "select") or not cmds.contextInfo(ctx, exists=True):
            if self.tool == "select" and ("level" in flags or "symmetry" in flags):
                groomlab.select_tool(level=max(self.level.currentData() or 0, 0), group=self.current_group(),
                                     symmetry=self.symmetry_name())
            return
        card = self.card_tool()
        if card is not None and card != self.tool and not set(flags) & {"level", "group", "symmetry"}:
            return
        try:
            cmds.groomLabBrushCtx(ctx, edit=True, **flags)
        except RuntimeError as e:
            self.status.setText(str(e))

    # -- The tubes' display: shown with every tool, and the level whose tubes show -----------

    def tube_hierarchy(self, groom=None, group=None):
        """The tube hierarchy of `group` (else the group picked): {"base", "levels": [each level's
        group, the base first]}, or None when it has one level only (or no groom)."""
        groom = groom or self.groom()
        if not groom:
            return None
        from groomlab import root_tubes
        try:
            info = root_tubes.tube_levels(groom, picked=(group if group is not None else self.current_group()) or "")
        except (RuntimeError, ValueError):
            return None
        return info if info.get("base") and len(info.get("levels") or []) > 1 else None

    def tube_view(self, groom=None):
        """The group whose tubes alone show (the groom's tubeViewGroup), "" when they follow the guides."""
        from maya import cmds
        groom = groom or self.groom()
        try:
            return (cmds.getAttr(groom + ".tubeViewGroup") or "") if groom else ""
        except (RuntimeError, ValueError):
            return ""

    def set_tube_view(self, group):
        """Keep level `group`'s tubes in view whichever level's guides are picked ("": the tubes
        follow their guides). The tubes show (Show tubes) so the pick is seen with any tool."""
        from groomlab import root_tubes
        groom = self.groom()
        if not groom:
            return
        try:
            root_tubes.set_tube_view(groom, group or "")
        except RuntimeError as e:
            self.status.setText(str(e))
            return
        if group and not self.tube_show.isChecked() and self.tool not in ("shape", "roots"):
            self.tube_show.setChecked(True)  # (pinned: seen whichever tool is up)
        self.root_card.sync_lock(groom)
        self.read_tube_display()
        try:
            self.guides_tab.refresh()
        except Exception:  # noqa: BLE001 (the list catches up on its own)
            pass
        if self.tool:
            self.show_editing()
        level = self.tube_level_of(group) if group else 0
        self.status.setText("Tubes of level %d kept in view." % level if level else "Tubes follow the guides.")

    def tube_level_of(self, group, groom=None):
        """Level `group`'s number in its tube hierarchy (1: the base), 0 when it is in none."""
        info = self.tube_hierarchy(groom, group) if group else None
        return info["levels"].index(group) + 1 if info and group in info["levels"] else 0

    def read_tube_display(self):
        """The guides display as the groom stands (a pin from a level row, an undo)."""
        self.display_popup.read()

    def push_tube_display(self):
        """The tubes' look (the guides display: Shaded, Wireframe, Colors, Level colors) to the brush
        context whatever tool is up: it gives it to the groom, drawn at once."""
        from maya import cmds
        ctx = groomlab.BRUSH_CONTEXT
        try:
            groomlab.ensure_plugin()
            if not cmds.contextInfo(ctx, exists=True):
                cmds.groomLabBrushCtx(ctx)
            cmds.groomLabBrushCtx(ctx, edit=True, tubeShaded=self.tube_shaded.isChecked(),
                                  tubeWireframe=self.tube_wireframe.isChecked(), tubeColors=self.tube_colors.isChecked(),
                                  tubeLevelColors=self.tube_level_colors.isChecked())
        except (RuntimeError, AttributeError) as e:
            self.status.setText(str(e))

    def toggle_symmetry(self):
        """Symmetry on or off (Alt+Shift+X): the box, which tells the tool. Returns whether it is on."""
        self.symmetry.setChecked(not self.symmetry.isChecked())
        return self.symmetry.isChecked()

    def symmetry_name(self):
        """The tools' -symmetry: "none", "x" (world X) or "topology"."""
        return self.symmetry_mode.currentData() if self.symmetry.isChecked() else "none"

    def set_strength(self, v):
        tool = self.card_tool() or self.tool
        if tool in self.strengths:
            self.strengths[tool] = v
        self.push(strength=v)

    def tube_channel_name(self):
        """The Shape brush's -tubeChannel: width, flatness or twist."""
        return next((name for name, b in self.tube_channel_buttons.items() if b.isChecked()), "width")

    def targets_name(self):
        """The Brushes checkboxes as the tool's -targets: "guides,parts,flow" (any subset) or "none"."""
        names = [name for box, name in ((self.target_guides, "guides"), (self.target_parts, "parts"),
                                        (self.target_flow, "flow")) if box.isChecked()]
        return ",".join(names) or "none"

    def push_targets(self):
        """The Brushes checkboxes: what the shaping brushes act on, sent to the active tool."""
        self.push(targets=self.targets_name())

    # -- Actions ---------------------------------------------------------------

    def fill_parts(self, groom):
        names = groomlab.list_parts(groom) if groom else []
        if [self.part_list.item(i).text() for i in range(self.part_list.count())] != names:
            self.part_list.clear()
            self.part_list.addItems(names)

    def remove_part(self):
        groom, item = self.groom(), self.part_list.currentItem()
        if groom and item:
            groomlab.remove_part(groom, item.text())
            self.fill_parts(groom)

    def parts_from_curves(self):
        groom = self.groom()
        curves = groomlab.selected_curves(groom)  # (groups of curves too)
        if not groom or not curves:
            self.status.setText("Select the part curves and the groom.")
            return
        try:
            names = groomlab.add_part(groom, groomlab.transforms(curves), delete_curves=True)
            groomlab.add_part_node(groom, if_missing=True)
        except (RuntimeError, ValueError) as e:
            self.status.setText(str(e))
            return
        self.status.setText("Added %s." % ", ".join(names))
        self.fill_parts(groom)

    def flow_sink_changed(self, *_):
        kind = self.flow_sink.currentData() or ""
        self.flow_sink.setToolTip(definitions.get("choices/flowSink")["tip"] + ". " +
                                  definitions.text("flowSinks" if kind else "flowArrows"))
        self.push(flowSink=kind)

    def mirror_symmetry(self):
        """Mirror to Other Side's symmetry: the brushes' Mirror, X when it is off."""
        name = self.symmetry_name()
        return "x" if name in (None, "", "none") else name

    def mirror_flow(self, side):
        groom = self.groom()
        if groom:
            try:
                done = groomlab.mirror_flow(groom, side, self.mirror_symmetry())
            except RuntimeError as e:
                self.status.setText(str(e).strip())
                return
            self.fill_flow_count(groom)
            self.status.setText("Flow mirrored: %s" % done)

    def mirror_parts(self, side):
        groom = self.groom()
        if groom:
            try:
                done = groomlab.mirror_parts(groom, side, self.mirror_symmetry())
            except RuntimeError as e:
                self.status.setText(str(e).strip())
                return
            self.fill_parts(groom)
            self.status.setText("Parts mirrored: %s" % done)

    def clear_flow(self):
        groom = self.groom()
        if groom:
            groomlab.clear_flow(groom)
            self.fill_flow_count(groom)

    def pick_clone_source(self, on):
        """Pick Source: the Clone tool's next click captures (the button stays down until it has)."""
        armed = groomlab.clone_pick_source(on) if self.tool == "clone" else False
        if armed != on:
            self.clone_pick.blockSignals(True)
            self.clone_pick.setChecked(armed)
            self.clone_pick.blockSignals(False)

    def reset_clone_source(self):
        """Reset: the Clone tool's source let go (and Pick Source up)."""
        groomlab.clone_reset()
        self.fill_clone()

    def fill_clone(self):
        """The Clone card: Pick Source down while a capture is armed, and what the source holds."""
        from maya import cmds
        ctx = groomlab.BRUSH_CONTEXT
        armed, size = False, 0
        if self.context_tool() == "clone":
            armed = bool(cmds.groomLabBrushCtx(ctx, query=True, pickSource=True))
            size = groomlab.clone_source_size()
        if self.clone_pick.isChecked() != armed:
            self.clone_pick.blockSignals(True)
            self.clone_pick.setChecked(armed)
            self.clone_pick.blockSignals(False)
        text = ("No source yet: Ctrl+click over guides" if not size else
                "Source: %d guide%s" % (size, "" if size == 1 else "s"))
        if self.clone_count.text() != text:
            self.clone_count.setText(text)
        self.clone_reset.setEnabled(bool(size) or armed)

    def fill_flow_count(self, groom):
        """The Flow card's count: "34 arrows · 2 sinks" kept on the groom (Clear All off with none)."""
        arrows, sinks = groomlab.flow_counts(groom) if groom else (0, 0)
        if (arrows, sinks) == getattr(self, "_flow_counted", None):
            return
        self._flow_counted = (arrows, sinks)
        self.flow_count.setText(flow_count_text(arrows, sinks))
        self.clear_flow_button.setEnabled(bool(arrows or sinks))

    def set_soft_selection(self, radius):
        groom = self.groom()
        if groom:
            groomlab.set_soft_selection(groom, radius)

    def clear_parts(self):
        groom = self.groom()
        if groom:
            groomlab.clear_parts(groom)
            self.fill_parts(groom)

    def open_graph(self):
        from groomlab.ui import graph_editor
        graph_editor.show()  # the current groom (a pinned Graph Editor stays on its own)

_window = None


def current():
    """The open palette, or None."""
    try:
        return _window if _window is not None and _window.isVisible() else None
    except RuntimeError:  # its widget was deleted
        return None


def select_map(name):
    """Pick map `name` in the Paint tab of the open Groom Tools (show() first)."""
    if _window is not None:
        _window.paint_tab.select(name)


# What Maya runs to rebuild the Groom Tools in their dock as it restores its workspace (restore).
UI_SCRIPT = "import groomlab.ui.palette; groomlab.ui.palette.restore()"


def _make():
    from maya.app.general.mayaMixin import MayaQWidgetDockableMixin

    class DockableToolPalette(MayaQWidgetDockableMixin, ToolPalette):
        pass

    window = DockableToolPalette()
    window.setObjectName("groomLabToolPalette")
    widgets.at_exit(_close_at_exit)
    return window


def show(tab=None):
    """Open (or raise) the dockable Groom Tools; `tab` picks "tools", "paint", "correctives" or "photos" (the
    old ids "sculpt" and "reference" too; "guides": the Tools tab with its guide groups open)."""
    global _window
    if _window is not None and not widgets.alive(_window):  # its dock was deleted
        _window = None
    if _window is None:
        _window = _make()
        widgets.into_dock(_window, UI_SCRIPT)  # the dock Maya kept: opens where it was
    _window.show(dockable=True, area="left", floating=False, uiScript=UI_SCRIPT)  # (a new dock: on the left)
    widgets.keep_dock_resizable(_window)
    if tab:
        _window.show_tab(tab)
    groomlab.adopt_scene_groom()  # a scene with grooms: one of them, not "Start a groom"
    _window.poll()
    return _window


def restore():
    """Maya restoring its workspace (a restart, an update's too): the Groom Tools rebuilt in their
    dock, where they were when Maya last quit."""
    global _window
    groomlab.ensure_plugin()
    if _window is not None and widgets.alive(_window):
        widgets.dispose(_window)
    _window = _make()
    widgets.into_dock(_window, UI_SCRIPT, restoring=True)
    widgets.keep_dock_resizable(_window)


def _close_at_exit():
    """Maya quits: the palette stops its poll and callbacks and its widgets go while Qt and Python
    are whole (see widgets.at_exit). Its dock stays as Maya saves it."""
    global _window
    window, _window = _window, None
    if window is not None and widgets.alive(window):
        window.stop_following()
        widgets.dispose(window)
