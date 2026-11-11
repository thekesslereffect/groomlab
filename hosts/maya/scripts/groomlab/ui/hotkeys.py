# SPDX-License-Identifier: Apache-2.0
"""GroomLab hotkeys: runtime commands for every tool, a marking menu, level stepping.

Runtime commands (Hotkey Editor > Runtime Command Editor > GroomLab) are created
when the menu installs, so any of them can be bound by hand. install() also
makes a "GroomLab" hotkey set (a copy of the current one) with:

    Y (hold) + left click   the GroomLab marking menu: brushes around, the rest below
    Page Up / Page Down     the brushes' guide level up / down (isolated when isolating)
    B / M (hold) + drag     a GroomLab brush's radius / strength (other tools: Maya's own B / M)
    Alt+Shift+X             the brushes' symmetry on or off (Alt+X is Maya's Walk Tool)
    Alt+Shift+G             every groom's guides on or off (GroomLab > Display)

    import groomlab.ui.hotkeys as hk
    hk.install()             # switches to the GroomLab set
    hk.last_changes          # what it changed: [(keys, what it did before, what it does now)]
"""

from maya import cmds, mel

from groomlab.ui import definitions

CATEGORY = "GroomLab"
MARKING_MENU = "groomLabMarkingMenu"
HOTKEY_SET = "GroomLab"
# What the last install() changed: [(keys, what the key did before, what it does now)]; the set it
# switched from is the first entry when it switched.
last_changes = []
# Key names as people read them, for the list of changes.
_KEY_NAMES = {"y": "Y", "Page_Up": "Page Up", "Page_Down": "Page Down", "b": "B", "m": "M", "X": "Alt+Shift+X",
              "G": "Alt+Shift+G"}

# The marking menu's layout, every host's (engine/ui/marking_menu.json; Blender's pie reads the
# same): the brushes reached for most at radial positions, the rest of the catalog (brush_catalog.h)
# below them in its order, then a divider and the tail (Select, Freeze, Groom Tools).
RADIAL = [(r["position"], r["tool"]) for r in definitions.get("markingMenu/radial")]
# What the tail's action ids run; Freeze has always shown without its icon here.
ACTIONS = {"tools.open": lambda: __import__("groomlab.ui.palette", fromlist=["palette"]).show()}
_NO_ICON = {"freeze"}


def overflow():
    import groomlab_core
    skip = set(mode for _, mode in RADIAL) | set(definitions.get("markingMenu/overflow").get("exclude", []))
    return [t["id"] for t in groomlab_core.brush_catalog() if t["id"] not in skip]


def tool(mode):
    """Start a tool on the active groom, keeping the palette's settings when it is open. Paint
    opens the Groom Tools (its Paint tab picks the map to paint, or makes one, as its tile does).
    What stops a tool is said over the viewport, as the palette says it, never a traceback."""
    import groomlab
    from groomlab.ui import messages, palette
    window = palette.current()
    if window is None and mode == "paint":
        window = palette.show(tab="paint")
    if window is not None:
        window.activate(mode)
        return
    if mode != "select" and not groomlab.active_groom():
        # As the palette: said, and a brush is still picked (it brushes the groom selected next).
        messages.say("select a groom first; tools work on the selected groom")
        if mode == "part":  # (drawing a part needs the groom now)
            return
    try:
        if mode == "select":
            groomlab.select_tool()
        elif mode == "part":
            groomlab.draw_part_tool(groomlab.active_groom())
        elif mode == "freeze":
            groomlab.freeze_tool()
        else:
            groomlab.brush_tool(mode)
    except (RuntimeError, ValueError) as e:
        messages.say(messages.plain(e))


def step_level(delta):
    """Brush one guide level up or down."""
    import groomlab
    from groomlab.ui import palette
    window = palette.current()
    if window is not None:
        window.step_level(delta)
        return
    ctx = groomlab.BRUSH_CONTEXT
    if cmds.contextInfo(ctx, exists=True):
        level = max(0, cmds.groomLabBrushCtx(ctx, query=True, level=True) + delta)
        cmds.groomLabBrushCtx(ctx, edit=True, level=level)
        cmds.inViewMessage(amg="GroomLab: level %d" % level, pos="topCenter", fade=True)


def toggle_symmetry():
    """Alt+Shift+X: the brushes' symmetry on or off (World X when it comes on with no Groom Tools open),
    said in the viewport. Returns the symmetry now: "none", "x", "topology"..."""
    import groomlab
    from groomlab.ui import palette
    window = palette.current()
    ctx = groomlab.BRUSH_CONTEXT
    if window is not None:
        window.toggle_symmetry()
        name = window.symmetry_name()
    elif cmds.contextInfo(ctx, exists=True):
        name = "x" if cmds.groomLabBrushCtx(ctx, query=True, symmetry=True) in ("none", "") else "none"
        cmds.groomLabBrushCtx(ctx, edit=True, symmetry=name)
    else:
        return "none"
    label = {"x": "World X", "topology": "Topology"}.get(name, name)
    cmds.inViewMessage(amg="GroomLab: symmetry " + ("off" if name == "none" else "on (%s)" % label),
                       pos="topCenter", fade=True)
    return name


def toggle_all_guides():
    """Alt+Shift+G: every groom's guides off when any show, else on, said in the viewport. Returns
    whether they show now."""
    import groomlab
    on = groomlab.toggle_all_guides()
    cmds.inViewMessage(amg="GroomLab: every groom's guides " + ("shown" if on else "hidden"), pos="topCenter", fade=True)
    return on


def drag_slider(name, fallback):
    """B / M held: the next left drag in a GroomLab brush sets its radius / strength.

    Outside GroomLab brushes the key does what it does in Maya (`fallback`, a
    runtime command: soft selection falloff, Artisan radius...).
    """
    import groomlab
    ctx = groomlab.BRUSH_CONTEXT
    ours = cmds.currentCtx() == ctx and cmds.contextInfo(ctx, exists=True)
    if cmds.contextInfo(ctx, exists=True) and (ours or not name):
        cmds.groomLabBrushCtx(ctx, edit=True, dragSlider=name or "none")
    if not ours and fallback:
        mel.eval(fallback)


def _build(menu, *_):
    cmds.popupMenu(menu, edit=True, deleteAllItems=True)
    from groomlab.ui import widgets
    for position, mode in RADIAL:
        cmds.menuItem(parent=menu, label=mode.capitalize(), radialPosition=position,
                      image=widgets.icon_path(mode), command=lambda _, m=mode: tool(m))
    for mode in overflow():
        cmds.menuItem(parent=menu, label=mode.capitalize(), image=widgets.icon_path(mode),
                      command=lambda _, m=mode: tool(m))
    cmds.menuItem(parent=menu, divider=True)
    for entry in definitions.get("markingMenu/tail"):
        if isinstance(entry, dict):  # a tool
            mode = entry["tool"]
            icon = {} if mode in _NO_ICON else {"image": widgets.icon_path(mode)}
            cmds.menuItem(parent=menu, label=mode.capitalize(), command=lambda _, m=mode: tool(m), **icon)
        else:
            action = definitions.action(entry)
            icon = {"image": widgets.icon_path(action["icon"])} if action.get("icon") else {}
            cmds.menuItem(parent=menu, label=action["label"], command=lambda _, a=entry: ACTIONS[a](), **icon)


def marking_menu_press():
    if cmds.popupMenu(MARKING_MENU, exists=True):
        cmds.deleteUI(MARKING_MENU)
    cmds.popupMenu(MARKING_MENU, markingMenu=True, button=1, allowOptionBoxes=False, parent="viewPanes",
                   postMenuCommandOnce=True, postMenuCommand=lambda *a: _build(MARKING_MENU))


def marking_menu_release():
    if cmds.popupMenu(MARKING_MENU, exists=True):
        cmds.deleteUI(MARKING_MENU)


def _runtime(name, python, annotation):
    if cmds.runTimeCommand(name, exists=True):
        cmds.runTimeCommand(name, edit=True, delete=True)
    cmds.runTimeCommand(name, category=CATEGORY, annotation=annotation, commandLanguage="python", command=python)


def create_runtime_commands():
    """Runtime commands for every tool, the marking menu, the palette and level stepping."""
    import groomlab
    for mode in groomlab.BRUSH_MODES + ("select", "freeze"):
        _runtime("GroomLab" + mode.capitalize(), "import groomlab.ui.hotkeys as h; h.tool(%r)" % mode,
                 "GroomLab %s tool" % mode)
    _runtime("GroomLabUnfreezeAll", "import groomlab; groomlab.unfreeze_all()",
             "GroomLab: unfreeze every guide of the groom the tools work on")
    _runtime("GroomLabMarkingMenuPress", "import groomlab.ui.hotkeys as h; h.marking_menu_press()",
             "GroomLab marking menu (hold, then left click)")
    _runtime("GroomLabMarkingMenuRelease", "import groomlab.ui.hotkeys as h; h.marking_menu_release()",
             "GroomLab marking menu release")
    _runtime("GroomLabLevelUp", "import groomlab.ui.hotkeys as h; h.step_level(-1)", "GroomLab: brush the parents")
    _runtime("GroomLabLevelDown", "import groomlab.ui.hotkeys as h; h.step_level(1)", "GroomLab: brush the children")
    _runtime("GroomLabGroomTools", "import groomlab.ui.palette as p; p.show()", "GroomLab Groom Tools")
    _runtime("GroomLabToggleSymmetry", "import groomlab.ui.hotkeys as h; h.toggle_symmetry()",
             "GroomLab: brush symmetry on or off")
    _runtime("GroomLabToggleAllGuides", "import groomlab.ui.hotkeys as h; h.toggle_all_guides()",
             "GroomLab: every groom's guides on or off")
    for name, slider, key, maya_press in (("Radius", "radius", "B", "ModifyUpperRadius"),
                                          ("Strength", "strength", "M", "ModifyDisplacement")):
        _runtime("GroomLabBrush%sPress" % name,
                 "import groomlab.ui.hotkeys as h; h.drag_slider(%r, %r)" % (slider, maya_press + "Press"),
                 "GroomLab: hold %s and drag for the brush %s" % (key, slider))
        _runtime("GroomLabBrush%sRelease" % name,
                 "import groomlab.ui.hotkeys as h; h.drag_slider(None, %r)" % (maya_press + "Release"),
                 "GroomLab: brush %s drag release" % slider)


def _maya_binding(key, **modifiers):
    """The name command Maya's own hotkey set binds to `key` (with `modifiers`), or None."""
    current = cmds.hotkeySet(query=True, current=True)
    try:
        cmds.hotkeySet("Maya_Default", edit=True, current=True)
        return cmds.hotkey(key, query=True, name=True, **modifiers) or None
    except RuntimeError:
        return None
    finally:
        cmds.hotkeySet(current, edit=True, current=True)


def _bound(key, modifiers):
    """What `key` (with `modifiers`) runs in the current hotkey set: its name command's annotation
    (or name), "" when unbound."""
    try:
        name = cmds.hotkey(key, query=True, name=True, **modifiers) or ""
    except RuntimeError:
        return ""
    if name:
        try:
            return cmds.nameCommand(name, query=True, annotation=True) or name
        except (RuntimeError, TypeError):
            return name
    return ""


def install(activate=True):
    """A "GroomLab" hotkey set (copied from the current one) with the marking menu and level keys.
    last_changes lists what it changed (the set switched to, each key rebound and what it did)."""
    global last_changes
    changes = []
    create_runtime_commands()
    before = cmds.hotkeySet(query=True, current=True)
    if activate and before != HOTKEY_SET:
        changes.append(("Hotkey set", before, HOTKEY_SET + " (a copy of %s with the keys below)" % before))
    if not cmds.hotkeySet(HOTKEY_SET, exists=True):
        cmds.hotkeySet(HOTKEY_SET, source=cmds.hotkeySet(query=True, current=True))
    if activate:
        cmds.hotkeySet(HOTKEY_SET, edit=True, current=True)
    bindings = [("y", "GroomLabMarkingMenuPress", "GroomLabMarkingMenuRelease"),
                ("Page_Up", "GroomLabLevelUp", None), ("Page_Down", "GroomLabLevelDown", None),
                ("b", "GroomLabBrushRadiusPress", "GroomLabBrushRadiusRelease"),
                ("m", "GroomLabBrushStrengthPress", "GroomLabBrushStrengthRelease"),
                ("X", "GroomLabToggleSymmetry", None, {"altModifier": True}),
                ("G", "GroomLabToggleAllGuides", None, {"altModifier": True})]
    current = cmds.hotkeySet(query=True, current=True)
    walk = _maya_binding("x", altModifier=True)
    cmds.hotkeySet(HOTKEY_SET, edit=True, current=True)
    try:
        # Symmetry was Alt+X once, which is Maya's Walk Tool: a set made then gets Maya's back.
        if cmds.hotkey("x", altModifier=True, query=True, name=True) == "GroomLabToggleSymmetryNameCommand":
            cmds.hotkey(keyShortcut="x", altModifier=True, name=walk or "")
        for binding in bindings:
            key, press, release = binding[:3]
            press_name = press + "NameCommand"
            cmds.nameCommand(press_name, annotation=press, command=press, sourceType="mel")
            kwargs = {"keyShortcut": key, "name": press_name}
            modifiers = binding[3] if len(binding) > 3 else {}
            kwargs.update(modifiers)
            was = _bound(key, modifiers)
            if was != press and not was.startswith("GroomLab"):
                changes.append((_KEY_NAMES.get(key, key), was or "(nothing)", press))
            if release:
                release_name = release + "NameCommand"
                cmds.nameCommand(release_name, annotation=release, command=release, sourceType="mel")
                kwargs["releaseName"] = release_name
            cmds.hotkey(**kwargs)
    finally:
        if not activate:
            cmds.hotkeySet(current, edit=True, current=True)
    last_changes = changes
    return HOTKEY_SET


def changes_text(changes=None):
    """What install() changed, for the note it shows: one line per change."""
    changes = last_changes if changes is None else changes
    if not changes:
        return "Nothing changed: the GroomLab hotkey set was already active with its keys."
    lines = ["%-12s %s  ->  %s" % (k, was, now) for k, was, now in changes]
    return ("The GroomLab hotkey set changed these keys (Windows > Settings/Preferences > Hotkey Editor "
            "switches back to your set):\n\n" + "\n".join(lines))
