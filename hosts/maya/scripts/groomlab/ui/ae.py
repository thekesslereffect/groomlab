# SPDX-License-Identifier: Apache-2.0
"""Attribute Editor sections for groomLabShape that need more than attribute
controls: the groom's inputs (scalp, rest mesh, guides, colliders, the groom file it is linked
to, with Reload and Save), its controls (the sliders Control nodes add) and the strand colors'
source and range (Auto Range). AEgroomLabShapeTemplate.mel calls these.
"""

import json

from maya import cmds

import groomlab
from groomlab.ui import pick

INPUTS = "groomLabAEInputs"
CONTROLS = "groomLabAEControls"


def _groom(attr):
    return attr.split(".")[0]


def _short(node):
    return node.split("|")[-1] if node else ""


# ---------------------------------------------------------------------------
# Inputs

def inputs_new(attr):
    cmds.setUITemplate("attributeEditorTemplate", pushTemplate=True)
    cmds.columnLayout(INPUTS, adjustableColumn=True, rowSpacing=2)
    cmds.setUITemplate(popTemplate=True)
    _fill_inputs(_groom(attr))
    cmds.setParent("..")


def inputs_replace(attr):
    if cmds.columnLayout(INPUTS, exists=True):
        _fill_inputs(_groom(attr))


def _fill_inputs(groom):
    for child in cmds.columnLayout(INPUTS, query=True, childArray=True) or []:
        cmds.deleteUI(child)
    cmds.setParent(INPUTS)
    s = groomlab.inputs_summary(groom)

    def row(label, value, buttons, tip, kind=None):
        # `kind`: the first button is the eyedropper of that input (_pick_button).
        cmds.rowLayout(numberOfColumns=2 + len(buttons), adjustableColumn=2,
                       columnWidth=[(1, 60)] + [(3 + i, 50) for i in range(len(buttons))],
                       columnAttach=[(1, "right", 6)])
        cmds.text(label=label, annotation=tip)
        field = cmds.textField(text=value, editable=False, annotation=tip)
        made = [cmds.button(label=text, command=command, annotation=button_tip) if command else
                cmds.button(label=text, annotation=button_tip) for text, command, button_tip in buttons]
        cmds.setParent("..")
        if kind:
            _pick_button(groom, kind, field, made[0], value, buttons[0][2])

    row("Scalp", _short(s["scalp"]) or "none", [
        ("Pick", None, "Grow the groom on another mesh: click it in the viewport or the Outliner (Esc cancels); "
                       "guides move onto it"),
        ("Select", lambda *_: s["scalp"] and cmds.select(s["scalp"]), "Select the scalp mesh"),
    ], "The mesh the hair grows on", kind="scalp")
    row("Rest", _short(s["rest"]) or "none", [
        ("Pick", None, "Pick the scalp's undeformed shape (a mesh with its vertices): click it in the viewport "
                       "or the Outliner (Esc cancels)"),
        ("Set", lambda *_: _run(groomlab.set_rest_mesh, groom),
         "Scatter on the undeformed scalp, so the same strands stay put while it deforms"),
        ("Clear", lambda *_: _run(groomlab.clear_rest_mesh, groom), "No rest mesh"),
    ], "The scalp's undeformed shape (set automatically for a skinned or blend-shaped scalp)", kind="rest")
    # Other scalps (several scalps on one groom): one row each (Select, Remove), and a Pick row adding one.
    for other in s.get("scalps", []):
        _scalp_row(groom, other)
    row("Scalps", "none" if not s.get("scalps") else "", [
        ("Pick", None, "Grow hair on another mesh too (separate ears, a body, eyebrow patches): click it in the "
                       "viewport or the Outliner (Esc cancels). A Scalp, Scatter and Interpolate for it merge into "
                       "the groom's strands"),
    ], "Other meshes the hair grows on, joined after the scalp: a Scalp node naming one grows on it alone",
        kind="extraScalp")
    guides = groomlab.guides_text(s) if s["groups"] else "none yet: place or draw them"
    extra = [text for count, text in ((s["curves"], "%d curves" % s["curves"]), (s["sim"], "%d simulated" % s["sim"]),
                                      (s["cache"], "cache")) if count]
    if extra:
        guides += " (+ " + ", ".join(extra) + ")"
    row("Guides", guides, [
        ("Edit", lambda *_: _open_tools(), "Edit the guides in the Groom Tools"),
    ], "The guides the groom interpolates")
    # Colliders: one row each (its friction, Select, Remove), and a Pick row adding a mesh.
    for collider in s["colliders"]:
        _collider_row(groom, collider)
    row("Collider", "none" if not s["colliders"] else "", [
        ("Pick", None, "Make a mesh a collider: click it in the viewport or the Outliner (Esc cancels). A "
                       "ColliderMesh and a Collide node are added before the output"),
    ], "Add a mesh the strands keep off: a body, a collar, another character", kind="collider")
    # The groom file it is linked to (imported from, saved to): the round trip with the standalone app.
    linked = cmds.getAttr(groom + ".groomFile") or ""
    row("File", linked or "none", [
        ("Reload", lambda *_: _reload(groom),
         "Replace the graph, guides, correctives and maps with the groom file's (the app's edits); one undo step"),
        ("Save", lambda *_: _save(groom), "Write the groom to its groom file (Save to Groom File), for the app to open"),
    ], "The groom file this groom is linked to: GroomLab > Groom Files")
    cmds.setParent("..")
    # Keep up with the scene while the Attribute Editor shows it.
    _watch(INPUTS, groom, lambda: inputs_replace(groom + ".inMesh"),
           connections=("inMesh", "restMesh", "guideCurves", "colliderMeshes", "extraScalps"),
           attributes=("guideData", "groomFile"))


SCALE = "groomLabAEScale"
SCALE_TIP = ("What the groom is scaled by with its scalp: 1.00x at the size it was groomed at. Set Scale Reference "
             "makes the scalp's size now 1x (the groom then keeps the sizes its params say at this size)")


def scale_new(attr):
    """The groom's scale (groomScale, read only, as 1.00x) and Set Scale Reference."""
    groom = _groom(attr)
    cmds.setUITemplate("attributeEditorTemplate", pushTemplate=True)
    cmds.rowLayout(SCALE, numberOfColumns=4, adjustableColumn=4, columnWidth=[(1, 145), (2, 60), (3, 20)],
                   columnAttach=[(1, "right", 6)])
    cmds.text(label="Groom Scale", annotation=SCALE_TIP)
    field = cmds.floatField(SCALE + "Field", editable=False, precision=2, annotation=SCALE_TIP)
    cmds.text(label="x", align="left", annotation=SCALE_TIP)
    # The groom the section shows now (scale_replace moves it): kept on the button.
    cmds.button(SCALE + "Set", label="Set Scale Reference", annotation=SCALE_TIP, docTag=groom,
                command=lambda *_: _run(groomlab.set_scale_reference, cmds.button(SCALE + "Set", query=True, docTag=True)))
    cmds.setParent("..")
    cmds.setUITemplate(popTemplate=True)
    cmds.connectControl(field, groom + ".groomScale")


def scale_replace(attr):
    groom = _groom(attr)
    if cmds.floatField(SCALE + "Field", exists=True):
        cmds.connectControl(SCALE + "Field", groom + ".groomScale")
        cmds.button(SCALE + "Set", edit=True, docTag=groom)


COLLIDER_FRICTION_TIP = ("Friction: how much simulated hair resting on this mesh is kept from sliding, 0-1, in a "
                         "Simulate node it feeds (-1: the Simulate node's friction)")


def _collider_row(groom, collider):
    """A collider's row: its name and mesh, its friction (a field), Select and Remove."""
    tip = "A mesh the strands keep off (a Collide node reads it through a ColliderMesh node)"
    cmds.rowLayout(numberOfColumns=5, adjustableColumn=2, columnWidth=[(1, 60), (3, 50), (4, 50), (5, 50)],
                   columnAttach=[(1, "right", 6)])
    cmds.text(label="Collider", annotation=tip)
    cmds.textField(text="%s (%s)" % (collider["name"], _short(collider["mesh"])), editable=False, annotation=tip)
    cmds.floatField(value=collider.get("friction", -1.0), minValue=-1.0, maxValue=1.0, precision=2, step=0.05,
                    annotation=COLLIDER_FRICTION_TIP,
                    changeCommand=lambda value, n=collider["name"]: _run(groomlab.set_collider_friction, groom, n, value))
    cmds.button(label="Select", command=lambda *_, m=collider["mesh"]: cmds.select(m), annotation="Select the collider mesh")
    cmds.button(label="Remove", command=lambda *_, n=collider["name"]: _run(groomlab.remove_collider, groom, n),
                annotation="Drop this collider: its ColliderMesh node and the Collide node it fed go too")
    cmds.setParent("..")


def _scalp_row(groom, other):
    """Another scalp's row: its name and mesh, Select and Remove."""
    tip = "Another mesh the hair grows on: the Scalp nodes whose input is its name read it"
    cmds.rowLayout(numberOfColumns=4, adjustableColumn=2, columnWidth=[(1, 60), (3, 50), (4, 50)],
                   columnAttach=[(1, "right", 6)])
    cmds.text(label="Scalp", annotation=tip)
    cmds.textField(text="%s (%s)" % (other["name"], _short(other["mesh"])), editable=False, annotation=tip)
    cmds.button(label="Select", command=lambda *_, m=other["mesh"]: cmds.select(m), annotation="Select the scalp mesh")
    cmds.button(label="Remove", command=lambda *_, n=other["name"]: _run(groomlab.remove_scalp, groom, n),
                annotation="Drop this scalp: the Scalp nodes reading it and the branch they alone fed go too")
    cmds.setParent("..")


def _reload(groom):
    from groomlab.ui import menu
    menu.reload_groom_file(groom)


def _save(groom):
    from groomlab.ui import menu
    menu.save_groom_file(groom)


# Each section watches one groom with one set of scriptJobs, made when it
# starts showing that groom. (Making them on every refresh multiplied them:
# every edit fired them all, and each refresh added more, doubling per edit.)
_watching = {}  # layout -> (groom, [job ids])
_pending = set()  # layouts with a refresh queued


def _watch(layout, groom, refresh, connections=(), attributes=()):
    current = _watching.get(layout)
    if current and current[0] == groom and all(cmds.scriptJob(exists=j) for j in current[1]):
        return
    if current:
        for job in current[1]:
            if cmds.scriptJob(exists=job):
                cmds.scriptJob(kill=job, force=True)

    def queue(*_):  # many changes in one go (a slider drag): one refresh, when Maya is idle
        if layout in _pending:
            return
        _pending.add(layout)

        def run():
            _pending.discard(layout)
            refresh()
        cmds.evalDeferred(run)

    jobs = [cmds.scriptJob(connectionChange=[groom + "." + plug, queue], parent=layout) for plug in connections]
    jobs += [cmds.scriptJob(attributeChange=[groom + "." + attr, queue], parent=layout) for attr in attributes]
    _watching[layout] = (groom, jobs)


def _run(fn, *args):
    try:
        fn(*args)
    except (RuntimeError, ValueError) as e:
        cmds.warning("GroomLab: %s" % e)


def _from_selection(groom, kind, ask=True):
    mesh = pick.usable(lambda m: pick.problem(groom, kind, m))
    if not mesh:
        raise ValueError("select %s first" % pick.WHAT[kind])
    return pick.set_input(groom, kind, mesh, ask=ask)


def pick_scalp(groom, ask=True):
    """Grow `groom` on the selected mesh (not its scalp). A groom with guides asks how they move
    (closest point, or fit the old scalp's bounds first). Returns a message;
    raises ValueError when nothing usable is selected. (The AE's Pick: _pick_button.)"""
    return _from_selection(groom, "scalp", ask)


def pick_extra_scalp(groom):
    """Make the selected mesh (not the scalp, nor a scalp it has) another scalp of `groom`, its
    branch wired (groomlab.add_scalp); returns a message. Raises ValueError when nothing usable is
    selected."""
    return _from_selection(groom, "extraScalp")


def pick_collider(groom):
    """Make the selected mesh (not the scalp, nor a collider it has) a collider of `groom`; returns
    a message. Raises ValueError when nothing usable is selected."""
    return _from_selection(groom, "collider")


# The eyedroppers on show: input -> (text field, button, the field's text). One AE at a time.
_pick_buttons = {}
ARMED_TEXT = "click a mesh in the viewport or the Outliner (Esc cancels)"


def _pick_button(groom, kind, field, button, value, tip):
    """Makes `button` the eyedropper of the groom's mesh input `kind` (groomlab.ui.pick): a mesh
    it takes that is selected with the groom goes there at once; else pressing it arms a pick (the
    button reads Cancel, the field what to do) that the next selection in the viewport or the
    Outliner ends, and the groom is selected again."""
    _pick_buttons[kind] = (field, button, value, tip)
    cmds.button(button, edit=True, command=lambda *_: press_pick(groom, kind))
    if pick.armed(groom, kind):
        _show_pick(kind, True)


def press_pick(groom, kind, ask=True):
    """A Pick button pressed (_pick_button): returns what pick.pick_input returns."""
    result = pick.pick_input(groom, kind, on_end=lambda applied: _show_pick(kind, False), ask=ask)
    _show_pick(kind, result == "armed")
    return result


def _show_pick(kind, armed):
    field, button, value, tip = _pick_buttons.get(kind, (None, None, None, None))
    if not button or not cmds.button(button, exists=True):
        return
    if armed:
        cmds.button(button, edit=True, label="Cancel", backgroundColor=(0.85, 0.55, 0.2),
                    annotation="Picking: click a mesh in the viewport or the Outliner. Cancel (or Esc) stops")
    else:
        cmds.button(button, edit=True, label="Pick", enableBackground=False, annotation=tip)
    if cmds.textField(field, exists=True):
        cmds.textField(field, edit=True, text=ARMED_TEXT if armed else value)


def _open_tools():
    from groomlab.ui import palette
    palette.show()


# ---------------------------------------------------------------------------
# Controls

def controls_new(attr):
    cmds.setUITemplate("attributeEditorTemplate", pushTemplate=True)
    cmds.columnLayout(CONTROLS, adjustableColumn=True)
    cmds.setUITemplate(popTemplate=True)
    _fill_controls(_groom(attr))
    cmds.setParent("..")


def controls_replace(attr):
    if cmds.columnLayout(CONTROLS, exists=True):
        _fill_controls(_groom(attr))


# ---------------------------------------------------------------------------
# Hair Color: the HairColor node making the output, its everyday params in the groom's own
# Attribute Editor (the color is what most grooms change first; the Graph Editor has the rest).

COLOR = "groomLabAEColor"

# (param, label, kind, tip); kind: mode, color, float, heading (a group's title). Rows shown in one
# mode only are in _COLOR_ONLY. The groups and names are HairColor's (PxrHairColor's: Base,
# Randomness, Tips, Highlight).
COLOR_ROWS = (
    ("mode", "Mode", "mode", "Natural (the melanin mode): pigment and redness, and a dye, as real hair. Picked color "
     "(the color mode): the color you pick, as it shows and renders (white hair: white)"),
    ("color", "Color", "color", "The hair's color as it shows and renders (mode Color)"),
    ("melanin", "Melanin", "float", "Pigment: 0 white or blond, 1 black"),
    ("redness", "Redness", "float", "Share of red pigment: 0 brown or black, 1 red"),
    ("dye", "Dye", "color", "A color dyeing the pigment (white: none)"),
    ("result", "Result", "result", "The color the pigment and dye make, before the randomness (what renders, "
     "a hair color as aiStandardHair's base color)"),
    ("greys", "Greys", "float", "Share of grey and white strands"),
    ("-Randomness", "Randomness", "heading", ""),
    ("melaninRandom", "Melanin Random", "float", "Each strand's melanin up to this much more or less"),
    ("valueRandom", "Value Random", "float", "Each strand up to this share lighter or darker"),
    ("hueRandom", "Hue Random", "float", "Each strand's hue turned, up to half a turn either way at 1"),
    ("saturationRandom", "Saturation Random", "float", "Each strand's saturation up to this share more or less"),
    ("clumpShare", "Clump Share", "float", "How much of the randomness the strands of one clump share: 0 each its "
     "own, 1 the whole clump alike (needs a Clump before HairColor)"),
    ("-Tips", "Tips", "heading", ""),
    ("tipColor", "Tip Color", "color", "The color the strands blend to toward the tip (with Tip Strength above 0)"),
    ("tipStrength", "Tip Strength", "float", "How far toward Tip Color the tips go, 0-1 (along the node's tipProfile)"),
    ("tipOffsetRandom", "Tip Offset Random", "float", "Each strand's tip color starts at its own place"),
    ("-Highlight", "Highlight", "heading", ""),
    ("specular", "Specular", "float", "Highlight strength (0.3: aiStandardHair's full specular tint)"),
    ("roughness", "Roughness", "float", "Highlight spread, 0 sharp to 1 broad"),
)
# The two ways as every host names them (docs/APP_PARITY.md, Glossary): HairColor's mode melanin / color.
COLOR_MODES = (("melanin", "Natural"), ("color", "Picked color"))
_COLOR_ONLY = {"color": "color", "valueRandom": "color", "melanin": "melanin", "redness": "melanin", "dye": "melanin",
               "result": "melanin", "greys": "melanin", "melaninRandom": "melanin"}
_COLOR_DEFAULTS = {"mode": "melanin", "color": [0.8, 0.8, 0.8], "melanin": 0.35, "redness": 0.2, "dye": [1.0, 1.0, 1.0],
                   "greys": 0.0, "melaninRandom": 0.1, "valueRandom": 0.0, "hueRandom": 0.0, "saturationRandom": 0.0,
                   "clumpShare": 0.5, "tipColor": [0.55, 0.42, 0.28], "tipStrength": 0.0, "tipOffsetRandom": 0.0,
                   "specular": 0.3, "roughness": 0.5}


def _node_note(groom, node):
    """The node's note from its last evaluation (graph stats' notes), "" without one."""
    try:
        return (json.loads(cmds.groomLabGraphStats(groom)).get("notes") or {}).get(node) or ""
    except (RuntimeError, ValueError, TypeError):
        return ""


def color_rows(groom):
    """What the Hair Color section shows: {"node", "where", "note", "rows": [{param, label, kind, tip,
    value, mapped}]}. node "" without a HairColor making the output; `where` says why the section
    cannot edit it (inside a compound: edit it there), else ""; `note` the node's note (a
    clumpShare without a Clump before it...). A "result" row's value is the pigment's color times
    the dye (groomlab_core.melanin_color), "heading" rows a group's title."""
    from groomlab import shading
    import groomlab_core
    look = shading.hair_look_json(groom)
    node = look.get("node") or ""
    if not node:
        return {"node": "", "where": "No HairColor node colors this groom: add one in the Graph Editor "
                                     "(Add Node > Color > HairColor).", "note": "", "rows": []}
    if "/" in node:
        return {"node": node, "where": "The color comes from %s, inside a compound: edit it in the Graph Editor." % node,
                "note": "", "rows": []}
    params = dict(_COLOR_DEFAULTS)
    params.update(look.get("hairColorParams") or {})
    mode = params.get("mode") if params.get("mode") in ("melanin", "color") else "melanin"
    rows = []
    for param, label, kind, tip in COLOR_ROWS:
        only = _COLOR_ONLY.get(param)
        if only and only != mode:
            continue
        if kind == "result":
            melanin, redness, dye = params.get("melanin"), params.get("redness"), params.get("dye")
            if isinstance(melanin, dict) or isinstance(redness, dict):
                continue  # mapped: no one color
            value = list(groomlab_core.melanin_color(float(melanin), float(redness), [float(c) for c in dye]))
        else:
            value = params.get(param)
        rows.append({"param": param, "label": label, "kind": kind, "tip": tip, "value": value,
                     "mapped": isinstance(value, dict)})
    return {"node": node, "where": "", "note": _node_note(groom, node), "rows": rows}


def set_color_param(groom, node, param, value):
    """A Hair Color row's edit: the param as the Graph Editor sets it, then a hair shader Create
    Hair Shader made follows the new look (shading.sync_hair_shader). Picking a tip color while
    Tip Strength is 0 raises it to 1, so the pick shows."""
    import groomlab
    groomlab.set_param_value(groom, node, param, value)
    if param == "tipColor":
        strength = (groomlab.hair_color_params(groom) or {}).get("tipStrength", 0.0)
        if not isinstance(strength, dict) and float(strength) <= 0.0:
            groomlab.set_param_value(groom, node, "tipStrength", 1.0)
    groomlab.sync_hair_shader(groom)


def color_new(attr):
    cmds.setUITemplate("attributeEditorTemplate", pushTemplate=True)
    cmds.columnLayout(COLOR, adjustableColumn=True, rowSpacing=2)
    cmds.setUITemplate(popTemplate=True)
    _fill_color(_groom(attr))
    cmds.setParent("..")


def color_replace(attr):
    if cmds.columnLayout(COLOR, exists=True):
        _fill_color(_groom(attr))


def _edit_in_graph_editor(groom, node):
    from groomlab.ui import graph_editor
    window = graph_editor.show(groom)
    item = getattr(window, "items", {}).get(node) if node else None
    if item is not None:
        window.scene.clearSelection()
        item.setSelected(True)


def _fill_color(groom):
    for child in cmds.columnLayout(COLOR, query=True, childArray=True) or []:
        cmds.deleteUI(child)
    cmds.setParent(COLOR)
    shown = color_rows(groom)
    node = shown["node"]
    if shown["where"]:
        cmds.text(label=shown["where"], align="left", font="obliqueLabelFont", wordWrap=True)
    for row in shown["rows"]:
        param, label, tip, value = row["param"], row["label"], row["tip"], row["value"]
        if row["kind"] == "heading":
            cmds.separator(style="in", height=8)
            cmds.text(label=label, align="left", font="boldLabelFont")
        elif row["mapped"]:  # a map or an expression: the Graph Editor edits those
            cmds.textFieldGrp(label=label, text="mapped (Graph Editor)", editable=False, annotation=tip)
        elif row["kind"] == "mode":
            menu = cmds.optionMenuGrp(label=label, annotation=tip)
            for _, text in COLOR_MODES:
                cmds.menuItem(label=text)
            keys = [k for k, _ in COLOR_MODES]
            cmds.optionMenuGrp(menu, edit=True, select=keys.index(value) + 1 if value in keys else 1)
            cmds.optionMenuGrp(menu, edit=True, changeCommand=lambda _v, m=menu, p=param: _run(
                set_color_param, groom, node, p, keys[cmds.optionMenuGrp(m, query=True, select=True) - 1]))
        elif row["kind"] == "result":  # the swatch alone: not editable
            cmds.colorSliderGrp(label=label, rgb=[float(c) for c in value][:3], annotation=tip, enable=False)
        elif row["kind"] == "color":
            # Maya's color slider: a swatch (color managed), a click opens Maya's color editor.
            rgb = [float(c) for c in (value or [1.0, 1.0, 1.0])][:3]
            grp = cmds.colorSliderGrp(label=label, rgb=rgb, annotation=tip)
            cmds.colorSliderGrp(grp, edit=True, changeCommand=lambda *_a, g=grp, p=param: _run(
                set_color_param, groom, node, p, list(cmds.colorSliderGrp(g, query=True, rgb=True))))
        else:
            grp = cmds.floatSliderGrp(label=label, field=True, minValue=0.0, maxValue=1.0, fieldMinValue=0.0,
                                      fieldMaxValue=1.0, precision=3, value=float(value or 0.0), annotation=tip)
            cmds.floatSliderGrp(grp, edit=True, changeCommand=lambda *_a, g=grp, p=param: _run(
                set_color_param, groom, node, p, cmds.floatSliderGrp(g, query=True, value=True)))
    if shown.get("note"):
        cmds.text(label=shown["note"], align="left", font="obliqueLabelFont", wordWrap=True)
    cmds.rowLayout(numberOfColumns=2, adjustableColumn=2)
    cmds.text(label="")
    cmds.button(label="Edit in Graph Editor" if node else "Graph Editor",
                annotation="The HairColor node's every param (a map, tipProfile, the seed, the highlight's randomness...)",
                command=lambda *_a: _run(_edit_in_graph_editor, groom, node))
    cmds.setParent("..")
    cmds.setParent("..")
    _watch(COLOR, groom, lambda: color_replace(groom + ".graph"), attributes=("graph",))


# ---------------------------------------------------------------------------
# Strand Colors: under Strand Color Mode, what the mode reads (a strand channel, a color set) and
# the attribute colors' range with Auto Range. Rows the mode does not use are left out.

STRAND_COLORS = "groomLabAEStrandColors"
SOURCE_TIP = ("What the strands are colored by: a strand channel (clumpId, length, melanin, a painted map...) for "
              "Attribute, the channel grouping them for Clump (clumpId, region), a color set of the scalp for Color "
              "Set; the groom as shown (the viewed node's strands)")
AUTO_RANGE_TIP = "Min and Max set to the lowest and highest value of the source on the strands shown"


def strand_colors_new(attr):
    cmds.setUITemplate("attributeEditorTemplate", pushTemplate=True)
    cmds.columnLayout(STRAND_COLORS, adjustableColumn=True, rowSpacing=2)
    cmds.setUITemplate(popTemplate=True)
    _fill_strand_colors(_groom(attr))
    cmds.setParent("..")


def strand_colors_replace(attr):
    if cmds.columnLayout(STRAND_COLORS, exists=True):
        _fill_strand_colors(_groom(attr))


def _set_source(groom, source):
    cmds.setAttr(groom + ".strandColorSource", source, type="string")


def _fill_strand_colors(groom):
    from groomlab import review
    for child in cmds.columnLayout(STRAND_COLORS, query=True, childArray=True) or []:
        cmds.deleteUI(child)
    cmds.setParent(STRAND_COLORS)
    mode = cmds.getAttr(groom + ".strandColorMode")
    if mode in (review.COLOR_SET_MODE, review.ATTRIBUTE_MODE, review.CLUMP_MODE):
        sources = review.strand_color_sources(groom)
        names = list(sources["colorSets" if mode == review.COLOR_SET_MODE else "channels"])
        current = cmds.getAttr(groom + ".strandColorSource") or ""
        if current and current not in names:  # kept, though the groom as shown lacks it
            names.append(current)
        menu = cmds.optionMenuGrp(label="Source", annotation=SOURCE_TIP)
        for name in names or ["(none)"]:
            cmds.menuItem(label=name)
        if current in names:
            cmds.optionMenuGrp(menu, edit=True, select=names.index(current) + 1)
        cmds.optionMenuGrp(menu, edit=True, enable=bool(names), changeCommand=lambda value: _run(_set_source, groom, value))
    if mode == review.ATTRIBUTE_MODE:
        cmds.attrFieldSliderGrp(attribute=groom + ".strandColorMin", label="Min")
        cmds.attrFieldSliderGrp(attribute=groom + ".strandColorMax", label="Max")
        cmds.rowLayout(numberOfColumns=2, adjustableColumn=2)
        cmds.text(label="")
        cmds.button(label="Auto Range", annotation=AUTO_RANGE_TIP,
                    command=lambda *_a: _run(review.auto_strand_color_range, groom))
        cmds.setParent("..")
    cmds.setParent("..")
    _watch(STRAND_COLORS, groom, lambda: strand_colors_replace(groom + ".strandColorSource"),
           attributes=("strandColorMode", "strandColorSource", "viewNode"))


def _fill_controls(groom):
    for child in cmds.columnLayout(CONTROLS, query=True, childArray=True) or []:
        cmds.deleteUI(child)
    cmds.setParent(CONTROLS)
    # A keyed vector or color shows as one control, not its three channels.
    names = [a for a in cmds.listAttr(groom, userDefined=True) or [] if a.startswith(groomlab.CONTROL_PREFIX)
             and not cmds.attributeQuery(a, node=groom, listParent=True)]
    if not names:
        # Wrapped: one long line would widen the whole Attribute Editor past its panel.
        cmds.text(label="No sliders yet: in the Graph Editor, right-click a param > Set Key or Drive with Slider.",
                  align="left", font="obliqueLabelFont", wordWrap=True)
    for name in names:
        label = cmds.attributeQuery(name, node=groom, niceName=True)
        if cmds.getAttr(groom + "." + name, type=True) in ("double", "long"):
            cmds.attrFieldSliderGrp(attribute=groom + "." + name, label=label)
        else:  # a toggle, a vector, a color
            cmds.attrControlGrp(attribute=groom + "." + name, label=label)
    cmds.setParent("..")
    _watch(CONTROLS, groom, lambda: controls_replace(groom + ".graph"), attributes=("graph",))
