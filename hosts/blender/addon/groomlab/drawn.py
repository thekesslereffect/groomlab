# SPDX-License-Identifier: GPL-3.0-or-later
"""Blender's panel drawing code, drawn in the GroomLab window (window.py) in the app's look.

DrawnLayout stands in for bpy.types.UILayout: a panel's draw(self, context) runs against it as it
does in the sidebar (palette.py's Groom Tools, its sub-panels, the lists' draw_item), and it records
what was asked for: rows, columns, boxes, splits, grids, labels, properties, operators, menus, lists
and folding panels. place() then lays the record out in the window (a uikit.Layout) as uikit
widgets: one source for the sidebar and the window, so they show the same sections.

What has no drawn counterpart opens Blender's own: a menu button calls the menu, a color swatch
Blender's color picker. Blender's icons map to the app's glyphs (ICONS); one without a glyph is left
out."""

import bpy

from . import editors, icons, uikit

# Blender's icons the panels use, as the app's glyphs (icons/ui_*.png) or the tools' own icons.
ICONS = {
    "ADD": "ui_plus", "X": "ui_close", "CANCEL": "ui_close", "TRASH": "ui_glyph_delete",
    "GREASEPENCIL": "ui_glyph_rename", "RESTRICT_SELECT_OFF": "ui_glyph_select", "SELECT_SET": "ui_glyph_select",
    "FILE_REFRESH": "ui_reload", "TRIA_UP": "ui_glyph_up", "TRIA_DOWN": "ui_glyph_down", "SOLO_OFF": "ui_glyph_solo",
    "SOLO_ON": "ui_glyph_solo", "HIDE_OFF": "ui_eye", "HIDE_ON": "ui_eye_off", "DOWNARROW_HLT": "ui_more",
    "TRIA_RIGHT": "ui_chevron_right", "IMPORT": "ui_image_plus", "EXPORT": "ui_export", "FILE_FOLDER": "ui_file",
    "TEXT": "ui_file", "LOOP_BACK": "ui_undo", "LOOP_FORWARDS": "ui_redo", "CHECKMARK": "ui_check_light",
    "DUPLICATE": "ui_copy", "DECORATE_KEYFRAME": "ui_key", "INFO": "ui_help", "ERROR": "ui_shield",
    "LINKED": "ui_link", "URL": "ui_link", "PINNED": "ui_frame", "NODE": "ui_graph", "TEXTURE": "ui_layers",
    "IMAGE_DATA": "ui_layers", "SHAPEKEY_DATA": "ui_layers", "CURVES": "ui_spline", "OUTLINER_OB_CURVES": "ui_spline",
    "OUTLINER_DATA_CURVES": "ui_spline", "CURVE_BEZCURVE": "ui_spline", "IPO_LINEAR": "ui_spline",
    "MESH_CIRCLE": "ui_box", "PREFERENCES": "ui_menu", "TIME": "ui_play", "LOCKED": "ui_shield",
    "SHADING_SOLID": "ui_sun", "VIEW_CAMERA": "ui_frame", "OUTLINER_OB_CAMERA": "ui_frame",
    "SCULPTMODE_HLT": "ui_pen", "BRUSH_DATA": "ui_pen", "VERTEXSEL": "ui_glyph_select", "FUND": "ui_shield",
    "FILE_TICK": "ui_save", "ASSET_MANAGER": "ui_box", "SORT_DESC": "ui_glyph_down", "NODE_INSERT_ON": "ui_graph",
}


def icon_name(icon="NONE", icon_value=0):
    """The window's icon for a Blender icon name or an icon_value of the add-on's previews ("": none)."""
    if icon_value:
        name = icons.name_of(icon_value)
        if name:
            return name
    return ICONS.get(icon or "NONE", "")


class OperatorProperties(object):
    """What `layout.operator(...)` returns: the properties the panel sets, kept for the press."""

    def __init__(self):
        object.__setattr__(self, "_values", {})

    def __setattr__(self, name, value):
        self._values[name] = value

    def __getattr__(self, name):
        try:
            return self._values[name]
        except KeyError:
            raise AttributeError(name)


class Item(object):
    """One thing a panel asked for (a label, a property, an operator...)."""

    def __init__(self, kind, **args):
        self.kind, self.args = kind, args


class DrawnLayout(object):
    """bpy.types.UILayout's drawing calls, recorded (see the module's docstring)."""

    def __init__(self, kind="column", align=False, factor=0.0, columns=0):
        self.kind, self.align, self.factor, self.columns = kind, align, factor, columns
        self.children = []  # DrawnLayouts and Items, in order
        self.enabled = True
        self.active = True
        self.alert = False
        self.alignment = "EXPAND"
        self.scale_x = 1.0
        self.scale_y = 1.0
        self.emboss = "NORMAL"
        self.use_property_split = False
        self.use_property_decorate = False
        self.operator_context = "INVOKE_DEFAULT"
        self.ui_units_x = 0.0
        self.ui_units_y = 0.0
        self.active_default = False
        self.activate_init = False

    def _child(self, kind, **kw):
        child = DrawnLayout(kind, **kw)
        self.children.append(child)
        return child

    def _item(self, kind, **args):
        item = Item(kind, **args)
        self.children.append(item)
        return item

    # Sub-layouts
    def row(self, align=False, heading="", heading_ctxt="", translate=True):
        return self._child("row", align=align)

    def column(self, align=False, heading="", heading_ctxt="", translate=True):
        return self._child("column", align=align)

    def box(self):
        return self._child("box")

    def split(self, factor=0.0, align=False):
        return self._child("split", align=align, factor=factor)

    def grid_flow(self, row_major=False, columns=0, even_columns=False, even_rows=False, align=False):
        return self._child("grid", align=align, columns=columns)

    def column_flow(self, columns=0, align=False):
        return self._child("grid", align=align, columns=columns)

    def panel(self, idname, default_closed=False):
        head = self._child("panel")
        head.panel_id, head.default_closed = idname, default_closed
        body = DrawnLayout("column")
        head.body = body
        return head, (body if _fold_open(idname, default_closed) else None)

    def panel_prop(self, data, property):
        return self.panel("prop:" + property)

    # Leaves
    def label(self, text="", text_ctxt="", translate=True, icon="NONE", icon_value=0):
        self._item("label", text=text, icon=icon_name(icon, icon_value))

    def separator(self, factor=1.0, type="AUTO"):
        self._item("separator", factor=factor, line=type == "LINE")

    def separator_spacer(self):
        self._item("separator", factor=1.0, line=False)

    def prop(self, data, property, text=None, text_ctxt="", translate=True, icon="NONE", placeholder="",
             expand=False, slider=False, toggle=-1, icon_only=False, event=False, full_event=False, emboss=True,
             index=-1, icon_value=0, invert_checkbox=False):
        self._item("prop", data=data, prop=property, text=text, icon=icon_name(icon, icon_value),
                   has_icon=icon not in ("NONE", None) or bool(icon_value), expand=expand, toggle=toggle,
                   icon_only=icon_only, emboss=emboss, placeholder=placeholder)

    def prop_enum(self, data, property, value, text=None, icon="NONE", icon_value=0):
        self._item("enum_choice", data=data, prop=property, value=value, text=text, icon=icon_name(icon, icon_value))

    def operator(self, operator, text=None, text_ctxt="", translate=True, icon="NONE", emboss=True, depress=False,
                 icon_value=0, search_weight=0.0):
        props = OperatorProperties()
        self._item("operator", idname=operator, text=text, icon=icon_name(icon, icon_value),
                   has_icon=icon not in ("NONE", None) or bool(icon_value), emboss=emboss, depress=depress,
                   props=props, context=self.operator_context)
        return props

    def operator_menu_enum(self, operator, property, text="", text_ctxt="", translate=True, icon="NONE"):
        self._item("menu", menu="", text=text or property, icon=icon_name(icon), call=None)
        return OperatorProperties()

    def menu(self, menu, text=None, text_ctxt="", translate=True, icon="NONE", icon_value=0):
        cls = getattr(bpy.types, menu, None)
        label = getattr(cls, "bl_label", menu) if text is None else text  # (no text given: the menu's name)
        self._item("menu", menu=menu, text=label, icon=icon_name(icon, icon_value),
                   has_icon=icon not in ("NONE", None) or bool(icon_value))

    def popover(self, panel, text=None, text_ctxt="", translate=True, icon="NONE", icon_value=0):
        cls = getattr(bpy.types, panel, None)
        self._item("popover", panel=panel, text=getattr(cls, "bl_label", panel) if text is None else text,
                   icon=icon_name(icon, icon_value))

    def template_list(self, listtype_name, list_id, dataptr, propname, active_dataptr, active_propname,
                      item_dyntip_propname="", rows=5, maxrows=5, type="DEFAULT", columns=9, sort_reverse=False,
                      sort_lock=False):
        self._item("list", cls=listtype_name, list_id=list_id, data=dataptr, prop=propname,
                   active_data=active_dataptr, active_prop=active_propname, rows=rows)

    def template_icon_view(self, data, property, show_labels=False, scale=6.0, scale_popup=5.0):
        self._item("icon_view", data=data, prop=property)

    def template_curve_mapping(self, data, property, type="NONE", levels=False, brush=False, use_negative_slope=False,
                               show_tone=False):
        self._item("curve", data=data, prop=property)

    def groomlab_profile_editor(self, context, tools_props):
        """The tube cross-section editor (the window's own: palette.draw_profile asks for it here)."""
        self._item("profile", tools=tools_props)

    def context_pointer_set(self, name, data):
        pass

    def context_string_set(self, name, value):
        pass

    def __getattr__(self, name):
        # A UILayout call not drawn here (a template the panels do not use yet): a column that
        # collects anything drawn into it, so the panel still runs.
        if name.startswith("template_") or name in ("prop_search", "prop_with_popover", "progress"):
            return lambda *a, **k: self._child("column")
        raise AttributeError(name)


# The window's folding sections (layout.panel and the sub-panels): open or folded, kept in the
# window's state between draws.
_folds = {}


def _fold_open(fold_id, default_closed):
    return _folds.get(fold_id, not default_closed)


def set_folds(folds):
    global _folds
    _folds = folds


class PanelProxy(object):
    """`self` for a Panel class's draw(self, context): its layout a DrawnLayout, its methods bound to
    this (a Panel cannot be made outside Blender's drawing)."""

    def __init__(self, cls, layout):
        object.__setattr__(self, "_cls", cls)
        object.__setattr__(self, "layout", layout)

    def __getattr__(self, name):
        for klass in self._cls.__mro__:
            if name in klass.__dict__:
                raw = klass.__dict__[name]
                return raw.__get__(self, self._cls) if hasattr(raw, "__get__") else raw
        raise AttributeError(name)


def draw_panel(cls, layout, context):
    """cls.draw(self, context) into layout, as Blender draws the panel."""
    cls.draw(PanelProxy(cls, layout), context)


# ---- Operators and menus --------------------------------------------------------------------------

def run_operator(idname, props, call_context="INVOKE_DEFAULT", override=None):
    """bpy.ops.<idname>(call_context, **props), in override's context when one is given (a node's
    actions run in the Graph Editor showing its graph: they ask the node editor for their tree)."""
    module, name = idname.split(".", 1)
    op = getattr(getattr(bpy.ops, module), name)
    where = override() if callable(override) else override
    if where:
        with bpy.context.temp_override(**where):
            return op(call_context, **props)
    return op(call_context, **props)


def menu_entries(name, context, override=None):
    """A Menu class's entries (its draw run against a DrawnLayout) for editors.PopupMenu, or None
    when it draws something a drawn menu cannot show (Blender's own popup opens instead)."""
    cls = getattr(bpy.types, name, None)
    if cls is None or not hasattr(cls, "draw"):
        return None
    rec = DrawnLayout("column")
    try:
        draw_panel(cls, rec, context)
    except Exception:  # noqa: BLE001 - a menu that cannot draw here opens as Blender's
        return None
    entries = []

    def walk(node, enabled):
        for c in node.children:
            if isinstance(c, DrawnLayout):
                if not walk(c, enabled and c.enabled):
                    return False
                continue
            kind, a = c.kind, c.args
            if kind == "separator":
                if entries and entries[-1]["kind"] != "sep":
                    entries.append({"kind": "sep"})
            elif kind == "label":
                if a.get("text"):
                    entries.append({"kind": "label", "text": a["text"]})
            elif kind == "operator":
                idname, props = a["idname"], dict(a["props"]._values)
                cls_op = _operator_class(idname)
                text = a["text"] if a["text"] is not None else getattr(cls_op, "bl_label", idname)
                entries.append({"kind": "op", "text": text, "icon": a.get("icon"), "enabled": enabled,
                                "checked": bool(a.get("depress")),
                                "run": lambda ctx, i=idname, p=props, cc=a.get("context"): run_operator(
                                    i, p, cc or "INVOKE_DEFAULT", override)})
            elif kind == "prop":
                rna = _rna(a["data"], a["prop"])
                if rna is None:
                    continue
                prop = uikit.Prop(a["data"], a["prop"], rna.name if a.get("text") is None else a["text"])
                if rna.type == "BOOLEAN":
                    entries.append({"kind": "check", "text": prop.label, "enabled": enabled, "checked": bool(prop.get()),
                                    "run": lambda ctx, p=prop: p.set(not bool(p.get()))})
                elif rna.type == "ENUM":
                    for ident, label, _tip in prop.items():
                        entries.append({"kind": "check", "text": label, "enabled": enabled,
                                        "checked": prop.get() == ident,
                                        "run": lambda ctx, p=prop, i=ident: p.set(i)})
                else:
                    return False
            elif kind == "menu":
                entries.append({"kind": "sub", "text": a.get("text") or "", "icon": a.get("icon"), "enabled": enabled,
                                "sub": lambda n=a["menu"]: menu_entries(n, bpy.context, override)})
            elif kind == "enum_choice":
                prop = uikit.Prop(a["data"], a["prop"])
                entries.append({"kind": "check", "text": a.get("text") or a["value"], "enabled": enabled,
                                "checked": prop.get() == a["value"],
                                "run": lambda ctx, p=prop, v=a["value"]: p.set(v)})
            else:
                return False
        return True

    if not walk(rec, True):
        return None
    while entries and entries[0]["kind"] == "sep":
        entries.pop(0)
    while entries and entries[-1]["kind"] == "sep":
        entries.pop()
    return entries or None


class MenuButton(uikit.Button):
    """A menu's button: a press opens it drawn (editors.PopupMenu), else as Blender's own popup."""

    def __init__(self, wid, rect, text, icon, menu_name, override=None):
        super().__init__(wid, rect, text, None, icon, flat=not text, align="left" if text else "center")
        self.menu_name, self.override = menu_name, override

    def press(self, win, context, event):
        entries = menu_entries(self.menu_name, context, self.override)
        if not entries:
            bpy.ops.wm.call_menu(name=self.menu_name)
            return None
        return editors.PopupMenu(win, entries, self.rect)


# ---- Laying out -------------------------------------------------------------------------------

class _State(object):
    """What children take from their layouts: enabled, active, alert, the height scale, emboss."""

    def __init__(self, enabled=True, active=True, alert=False, scale_y=1.0, in_list=False, tiles=False):
        self.enabled, self.active, self.alert, self.scale_y = enabled, active, alert, scale_y
        self.in_list, self.tiles = in_list, tiles

    def under(self, layout):
        return _State(self.enabled and layout.enabled, self.active and layout.active, self.alert or layout.alert,
                      self.scale_y * (layout.scale_y or 1.0), self.in_list,
                      self.tiles or (layout.kind == "grid" and (layout.scale_y or 1.0) >= 1.3))


class Placer(object):
    """Lays a DrawnLayout out in a uikit.Layout's column as widgets (see place())."""

    def __init__(self, ui, context, actions):
        self.ui, self.context, self.actions = ui, context, actions
        self.s = ui.s
        self.row_h = ui.row
        self.label_h = 20 * self.s
        self.widgets = ui.widgets
        self.count = 0

    def wid(self, kind, key=""):
        self.count += 1
        return "%s:%s:%d" % (kind, key, self.count)

    # Sizes
    def gap(self, layout, horizontal):
        if layout.align:
            return 1 * self.s if horizontal else 2 * self.s
        return 4 * self.s if horizontal else 5 * self.s

    def item_height(self, item, st):
        kind = item.kind
        if kind == "separator":
            return (9 if item.args.get("line") else 6) * self.s * item.args.get("factor", 1.0)
        if kind == "label":
            return self.label_h * max(1.0, st.scale_y)
        if kind == "operator" and st.tiles:
            return 54 * self.s  # a tool tile: its icon over its name (the app's 56 x 54)
        if kind == "icon_view":
            return 44 * self.s
        if kind == "curve":
            return 150 * self.s
        if kind == "profile":
            return min(200 * self.s, self.ui.w) + self.row_h + 5 * self.s
        if kind == "list":
            return self.list_height(item)
        return self.row_h * st.scale_y

    def natural_width(self, node, st):
        """The width a thing wants (None: it takes a share of what is left)."""
        if isinstance(node, DrawnLayout):
            widths = [self.natural_width(c, st) for c in node.children]
            if not widths or any(w is None for w in widths):
                return None
            if node.kind == "row":
                return sum(widths) + self.gap(node, True) * (len(widths) - 1)
            if node.kind == "column":
                return max(widths)
            return None
        a = node.args
        if node.kind in ("operator", "menu", "popover"):
            text = a.get("text")
            if node.kind == "operator" and text is None:
                text = self.op_label(a["idname"])
            if not text:
                return self.row_h
            return self.text_width(text) + (22 * self.s if a.get("icon") else 0) + 18 * self.s
        if node.kind == "label":
            return self.text_width(a.get("text", "")) + (22 * self.s if a.get("icon") else 0) + 6 * self.s
        if node.kind == "prop":
            rna = _rna(a["data"], a["prop"])
            if rna is not None and rna.type == "BOOLEAN" and (a.get("icon_only") or a.get("text") == "") and a.get("has_icon"):
                return self.row_h
            if rna is not None and rna.type == "BOOLEAN" and a.get("toggle") in (True, 1):
                return self.text_width(a.get("text") if a.get("text") is not None else rna.name) + 18 * self.s
        return None

    def text_width(self, text):
        import blf
        fid = uikit.font()
        blf.size(fid, self.ui.win.theme.font_size * self.s)
        return blf.dimensions(fid, text)[0]

    def op_label(self, idname):
        cls = _operator_class(idname)
        return getattr(cls, "bl_label", idname) if cls else idname

    # Placing
    def place(self, node, x, top, w, st):
        """Places node (a DrawnLayout or Item) in (x, top, w); its height."""
        if isinstance(node, Item):
            h = self.item_height(node, st)
            self.leaf(node, (x, top - h, w, h), st)
            return h
        st = st.under(node)
        kind = node.kind
        if kind == "row":
            return self.place_row(node, x, top, w, st)
        if kind == "split":
            return self.place_split(node, x, top, w, st)
        if kind == "grid":
            return self.place_grid(node, x, top, w, st)
        if kind == "panel":
            return self.place_panel(node, x, top, w, st)
        if kind == "box":
            pad = 6 * self.s
            backdrop = uikit.Backdrop(self.wid("box"), (x, top, w, 0))
            self.widgets.append(backdrop)
            h = self.place_column(node, x + pad, top - pad, w - 2 * pad, st) + 2 * pad
            backdrop.rect = (x, top - h, w, h)
            return h
        return self.place_column(node, x, top, w, st)

    def place_column(self, node, x, top, w, st):
        y = top
        gap = self.gap(node, False)
        for i, child in enumerate(node.children):
            if i:
                y -= gap
            y -= self.place(child, x, y, w, st)
        return top - y

    def place_row(self, node, x, top, w, st):
        children = node.children
        if not children:
            return 0.0
        gap = self.gap(node, True)
        naturals = [self.natural_width(c, st) for c in children]
        compact = node.alignment in ("LEFT", "RIGHT", "CENTER")
        if compact:  # each at the width it wants, packed to one side
            widths = [n if n is not None else 90 * self.s for n in naturals]
            total = sum(widths) + gap * (len(widths) - 1)
            if total > w:  # too wide: shrink the flexible ones
                widths = self.share(children, naturals, w, gap)
                total = w
            start = x + {"LEFT": 0.0, "CENTER": (w - total) * 0.5, "RIGHT": w - total}[node.alignment]
        else:
            widths = self.share(children, naturals, w, gap)
            start = x
        h = 0.0
        cx = start
        for child, cw in zip(children, widths):
            h = max(h, self.place(child, cx, top, cw, st))
            cx += cw + gap
        return h

    def share(self, children, naturals, w, gap):
        """Widths for a row: fixed ones as they want, the rest of the width shared by scale_x."""
        free = w - gap * (len(children) - 1)
        fixed = sum(n for n, c in zip(naturals, children) if n is not None and self.fixed(c))
        weights = [0.0 if (n is not None and self.fixed(c)) else max(0.05, getattr(c, "scale_x", 1.0) or 1.0)
                   for n, c in zip(naturals, children)]
        total = sum(weights) or 1.0
        rest = max(0.0, free - fixed)
        return [n if weight == 0.0 else rest * weight / total for n, c, weight in zip(naturals, children, weights)]

    def fixed(self, child):
        """Whether a row's child keeps the width it wants: icon buttons and short toggles do, rows of
        them do; labels, fields and text buttons share the row."""
        if isinstance(child, DrawnLayout):  # a row or column of icon buttons (a list's menu beside it)
            return child.kind in ("row", "column") and child.alignment == "EXPAND" and all(
                isinstance(c, Item) and self.icon_only(c) for c in child.children) and bool(child.children)
        return self.icon_only(child)

    def icon_only(self, item):
        a = item.args
        if item.kind in ("operator", "menu", "popover"):
            return a.get("text") == "" and bool(a.get("icon") or a.get("has_icon") or item.kind != "operator")
        if item.kind == "prop":
            rna = _rna(a["data"], a["prop"])
            return rna is not None and rna.type == "BOOLEAN" and (a.get("icon_only") or a.get("text") == "") and \
                a.get("has_icon")
        return False

    def place_split(self, node, x, top, w, st):
        children = node.children
        if not children:
            return 0.0
        gap = self.gap(node, True)
        free = w - gap * (len(children) - 1)
        if node.factor and len(children) > 1:
            first = free * node.factor
            widths = [first] + [(free - first) / (len(children) - 1)] * (len(children) - 1)
        else:
            widths = [free / len(children)] * len(children)
        h, cx = 0.0, x
        for child, cw in zip(children, widths):
            h = max(h, self.place(child, cx, top, cw, st))
            cx += cw + gap
        return h

    def place_grid(self, node, x, top, w, st):
        children = node.children
        if not children:
            return 0.0
        gap = self.gap(node, True) if not st.tiles else 6 * self.s
        cols = node.columns or max(1, int((w + gap) // (110 * self.s)))
        if st.tiles:  # tool tiles: the app's 56 px minimum, as many as fit, at most `columns`
            cols = max(1, min(cols, int((w + gap) // (56 * self.s + gap))))
        cw = (w - gap * (cols - 1)) / cols
        y = top
        for start in range(0, len(children), cols):
            row_h = 0.0
            for i, child in enumerate(children[start:start + cols]):
                row_h = max(row_h, self.place(child, x + i * (cw + gap), y, cw, st))
            y -= row_h + gap
        return top - y - gap

    def place_panel(self, node, x, top, w, st):
        """A folding section: its header row (the panel's own header content beside the chevron),
        then its body while open."""
        fold_id = node.panel_id
        open_ = _fold_open(fold_id, node.default_closed)
        h = self.row_h
        title = next((c.args.get("text", "") for c in node.children if isinstance(c, Item) and c.kind == "label"), "")

        def toggle(fold_id=fold_id, open_=open_):
            _folds[fold_id] = not open_

        self.widgets.append(uikit.PanelHeader(self.wid("panel", fold_id), (x, top - h, w, h), title, open_, toggle))
        total = h
        if open_ and node.body is not None:
            total += 4 * self.s
            total += self.place(node.body, x, top - total, w, st)
        return total

    # Leaves
    def leaf(self, item, rect, st):
        widget = None
        a = item.args
        kind = item.kind
        if kind == "label":
            widget = uikit.Label(self.wid("label"), rect, a.get("text", ""), icon=a.get("icon") or None,
                                 bright=st.in_list)
        elif kind == "separator":
            if a.get("line"):
                widget = uikit.Backdrop(self.wid("line"), rect, "line")
        elif kind == "operator":
            widget = self.operator(item, rect, st)
        elif kind in ("menu", "popover"):
            name = a.get("menu") or a.get("panel")
            text = a.get("text") or ""
            icon = a.get("icon") or ("ui_chevron_down" if not text else "")
            if kind == "menu" and name:
                widget = MenuButton(self.wid(kind, name), rect, text, icon or None, name, self.actions.get("op_context"))
            else:
                call = (lambda ctx, n=name: bpy.ops.wm.call_panel(name=n)) if name else (lambda ctx: None)
                widget = uikit.Button(self.wid(kind, name), rect, text, call, icon or None, flat=not text,
                                      align="left" if text else "center")
        elif kind == "prop":
            widget = self.prop(item, rect, st)
        elif kind == "enum_choice":
            prop = uikit.Prop(a["data"], a["prop"])
            name = next((n for i, n, _ in prop.items() if i == a["value"]), a["value"])
            widget = uikit.Segment(self.wid("enum", a["prop"]), rect, prop, a["value"],
                                   a.get("text") if a.get("text") is not None else name)
        elif kind == "list":
            self.list(item, rect, st)
        elif kind == "icon_view":
            prop = uikit.Prop(a["data"], a["prop"])
            widget = editors.IconPicker(self.wid("icons", a["prop"]), rect, prop, prop.items(icons=True),
                                        self.actions["preview"])
        elif kind == "curve":
            widget = editors.CurveEditor(self.wid("curve", a["prop"]), rect, a["data"], a["prop"])
        elif kind == "profile":
            self.profile(item, rect, st)
        if widget is not None:
            widget.disabled = not st.enabled
            widget.dim = not st.active
            widget.alert = st.alert
            self.widgets.append(widget)
        return widget

    def operator(self, item, rect, st):
        a = item.args
        idname, props = a["idname"], a["props"]._values
        text = a["text"] if a["text"] is not None else self.op_label(idname)
        context = a.get("context") or "INVOKE_DEFAULT"

        override = self.actions.get("op_context")

        def run(ctx, idname=idname, props=dict(props), context=context):
            run_operator(idname, props, context, override)

        def tip(idname=idname, props=dict(props)):
            return operator_tooltip(idname, props)

        if st.tiles and a.get("icon") and text:
            return uikit.Tile(self.wid("tile", props.get("tool", idname)), rect, text, run, a["icon"],
                              bool(a.get("depress")), tooltip=tip)
        return uikit.Button(self.wid("op", idname), rect, text, run, a.get("icon") or None, tooltip=tip,
                            flat=not a.get("emboss", True), depress=bool(a.get("depress")),
                            align="left" if (text and st.in_list) else "center")

    def prop(self, item, rect, st):
        a = item.args
        data, name = a["data"], a["prop"]
        rna = _rna(data, name)
        if rna is None:
            return uikit.Label(self.wid("label"), rect, name)
        label = a.get("text")
        prop = uikit.Prop(data, name, rna.name if label is None else label)
        kind = rna.type
        key = "%s.%s" % (type(data).__name__, name)
        if kind == "BOOLEAN" and not getattr(rna, "is_array", False):
            if a.get("toggle") in (True, 1) or a.get("has_icon") or a.get("icon_only"):
                text = "" if a.get("icon_only") or (label == "" and a.get("has_icon")) else prop.label
                return uikit.ToggleButton(self.wid("toggle", key), rect, prop, text, a.get("icon") or None,
                                          flat=not a.get("emboss", True))
            return uikit.Check(self.wid("check", key), rect, prop)
        if kind in ("INT", "FLOAT") and getattr(rna, "is_array", False) and rna.array_length > 1:
            if rna.subtype in ("COLOR", "COLOR_GAMMA"):
                if st.in_list:  # a list row's color: a small square at its left, as the app's dot
                    side = min(rect[2], rect[3] - 8 * self.s)
                    rect = (rect[0], rect[1] + (rect[3] - side) * 0.5, side, side)
                return uikit.ColorSwatch(self.wid("color", key), rect, prop, self.actions.get("color"))
            return uikit.Label(self.wid("label"), rect, "%s: %s" % (prop.label, ", ".join(
                "%.3g" % v for v in getattr(data, name))))
        if kind in ("INT", "FLOAT"):
            return uikit.Slider(self.wid("slider", key), rect, prop)
        if kind == "ENUM":
            items = prop.items()
            if a.get("expand"):
                self.segments(prop, items, rect, st, key)
                return None
            return uikit.Dropdown(self.wid("dropdown", key), rect, prop, show_label=bool(prop.label))
        if kind == "STRING":
            if not a.get("emboss", True):
                return uikit.Label(self.wid("label"), rect, str(getattr(data, name)), bright=True)
            return uikit.TextField(self.wid("text", key), rect, prop, a.get("placeholder", ""))
        if kind == "POINTER":
            choice = self.actions["pointer"](data, name, prop.label, rna)
            if choice is not None:
                return uikit.Dropdown(self.wid("pointer", key), rect, choice, show_label=bool(prop.label))
        return uikit.Label(self.wid("label"), rect, prop.label)

    def profile(self, item, rect, st):
        """The picked library shape's cross-section in a square (at most 200 px), Mirror under it."""
        t = item.args["tools"]
        x, y, w, h = rect
        side = h - self.row_h - 5 * self.s
        shape = self.actions["tube_shape"](t)
        mirror = uikit.Prop(t, "tube_shape_mirror")
        editor = editors.ProfileEditor(self.wid("profile"), (x + (w - side) * 0.5, y + self.row_h + 5 * self.s, side, side),
                                       shape, lambda pts: self.actions["tube_preview"](t, shape, pts),
                                       lambda pts: self.actions["tube_commit"](t, shape, pts), lambda: bool(mirror.get()))
        editor.disabled = not st.enabled
        self.widgets.append(editor)
        toggle = uikit.ToggleButton(self.wid("toggle", "tube_shape_mirror"), (x, y, w, self.row_h), mirror, mirror.label)
        toggle.disabled = not st.enabled or not editor.editable
        self.widgets.append(toggle)

    def segments(self, prop, items, rect, st, key):
        x, y, w, h = rect
        gap = 1 * self.s
        n = max(1, len(items))
        cw = (w - gap * (n - 1)) / n
        for i, (ident, name, tip) in enumerate(items):
            seg = uikit.Segment(self.wid("enum", key + "." + ident), (x + i * (cw + gap), y, cw, h), prop, ident,
                                name, tooltip=tip)
            seg.disabled, seg.dim, seg.alert = not st.enabled, not st.active, st.alert
            self.widgets.append(seg)

    # Lists (template_list): a UIList class's draw_item per row
    def list_menu(self, list_cls, pick):
        """A list row's right-click: the row picked, then its list's menu (the sidebar's right-click
        menus, menu.LIST_MENUS; the Correctives' by the picked row)."""
        from . import menu as menus

        def open_menu(ctx, win, event):
            pick(ctx)
            name = menus.correctives_menu(ctx) if list_cls == "GROOMLAB_UL_correctives" else menus.LIST_MENUS.get(list_cls)
            if not name:
                return None
            entries = menu_entries(name, ctx)
            if not entries:
                bpy.ops.wm.call_menu(name=name)
                return None
            return editors.PopupMenu(win, entries, (event.mouse_region_x, event.mouse_region_y, 0, 0), armed=True)
        return open_menu

    def list_rows(self, item):
        a = item.args
        coll = getattr(a["data"], a["prop"])
        return len(coll), max(a.get("rows", 5), min(len(coll), 8))

    def list_height(self, item):
        count, shown = self.list_rows(item)
        return shown * (self.row_h - 2 * self.s) + 4 * self.s

    def list(self, item, rect, st):
        a = item.args
        cls = getattr(bpy.types, a["cls"], None)
        data, coll = a["data"], getattr(a["data"], a["prop"])
        active_data, active_prop = a["active_data"], a["active_prop"]
        list_id = "%s.%s" % (a["cls"], a["prop"])
        count, shown = self.list_rows(item)
        x, y, w, h = rect
        self.widgets.append(uikit.Backdrop(self.wid("list", list_id), rect, "list"))
        first = max(0, min(self.actions["list_scroll"](list_id), count - shown))
        active = getattr(active_data, active_prop, -1)
        row_h = self.row_h - 2 * self.s
        top = y + h - 2 * self.s
        fake = _UIListSelf()
        for index in range(first, min(count, first + shown)):
            r = (x + 2 * self.s, top - row_h, w - 4 * self.s, row_h)

            def pick(ctx, index=index):
                uikit.Prop(active_data, active_prop).set(index)

            row = uikit.ListRow(self.wid("listrow", "%s.%d" % (list_id, index)), r, index == active, pick)
            row.disabled = not st.enabled
            row.context_menu = self.list_menu(a["cls"], pick)
            self.widgets.append(row)
            if cls is not None:
                rec = DrawnLayout("row")
                try:
                    cls.draw_item(fake, self.context, rec, data, coll[index], 0, active_data, active_prop, index)
                except Exception as e:  # noqa: BLE001 - one row that cannot draw leaves the others
                    rec.label(text=str(e))
                inner = _State(st.enabled, st.active, st.alert, 1.0, in_list=True)
                self.place(rec, r[0] + 6 * self.s, r[1] + r[3], r[2] - 10 * self.s, inner)
            top -= row_h
        if count > shown:  # the scroll it has: a thin bar on the right
            frac = shown / float(count)
            bar_h = max(10 * self.s, (h - 6 * self.s) * frac)
            pos = first / float(count - shown)
            self.widgets.append(_ScrollMark(self.wid("listbar", list_id),
                                            (x + w - 4 * self.s, y + 3 * self.s + (h - 6 * self.s - bar_h) * (1 - pos),
                                             2 * self.s, bar_h)))
        self.actions["list_area"](list_id, rect, count - shown)


class _ScrollMark(uikit.Widget):
    passive = True

    def draw(self, p, win):
        p.rect(self.rect, uikit.with_alpha(p.theme.textMuted, 0.6), 1 * p.scale)


class _UIListSelf(object):
    """`self` for a UIList's draw_item outside Blender's drawing."""
    layout_type = "DEFAULT"
    filter_name = ""
    use_filter_show = False
    use_filter_invert = False
    use_filter_sort_alpha = False
    use_filter_sort_reverse = False
    bitflag_filter_item = 1 << 30


def _rna(data, name):
    try:
        return data.bl_rna.properties[name]
    except (AttributeError, KeyError):
        return None


def _operator_class(idname):
    module, _, name = idname.partition(".")
    return getattr(bpy.types, "%s_OT_%s" % (module.upper(), name), None)


def operator_tooltip(idname, props):
    """An operator's tooltip: its description (with the properties the button sets, for those that
    describe themselves by them), as Blender's button would show."""
    cls = _operator_class(idname)
    if cls is None:
        return ""
    describe = getattr(cls, "description", None)
    if callable(describe):
        try:
            return describe(bpy.context, _Props().fill(props)) or ""
        except Exception:  # noqa: BLE001 - a description that cannot be made: the plain one
            pass
    return getattr(cls, "bl_description", "") or (cls.__doc__ or "").strip() or cls.bl_rna.description


class _Props(OperatorProperties):
    def fill(self, values):
        object.__setattr__(self, "_values", dict(values))
        return self

    def __getattr__(self, name):
        return self._values.get(name, "")


def place(layout, ui, context, actions):
    """Lays the recorded `layout` out in `ui` (a uikit.Layout), from its current y down; actions:
    {"color": open the color picker(context, prop), "pointer": a Choice for a pointer property
    (data, name, label, rna) or None, "list_scroll": a list's first row (id), "list_area": where a
    list is (id, rect, rows it can scroll)}."""
    placer = Placer(ui, context, actions)
    h = placer.place(layout, ui.x, ui.y, ui.w, _State())
    ui.y -= h
