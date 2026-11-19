# SPDX-License-Identifier: Apache-2.0
"""GroomLab's keyboard shortcuts in Maya, from the one table every host shares (the core's
shortcuts.h: the app's sheet and Blender's popup read the same rows).

    from groomlab.ui import shortcuts
    shortcuts.keys("graph.frame")            # "F"
    shortcuts.tip("Frame the graph", "graph.frame")   # "Frame the graph (F)"
    print(shortcuts.text())                  # Help > Keyboard Shortcuts
"""


_rows = None


def rows():
    """[{id, context, keys, action, hotkey_set}] for Maya, in the table's order."""
    global _rows
    if _rows is None:
        try:
            import groomlab_core
            _rows = list(groomlab_core.shortcuts("maya"))
        except (ImportError, AttributeError):
            _rows = []
    return _rows


def keys(shortcut_id):
    """The keys of a row ("" when Maya has none for it)."""
    return next((r["keys"] for r in rows() if r["id"] == shortcut_id), "")


def tip(text, shortcut_id):
    """A tooltip with its keys after it: "Frame the graph (F)"; the text alone when there are none."""
    k = keys(shortcut_id)
    return "%s (%s)" % (text, k) if k else text


def text():
    """Help > Keyboard Shortcuts: the rows by context, the ones the GroomLab hotkey set binds marked."""
    lines, context = [], None
    for r in rows():
        if r["context"] != context:
            context = r["context"]
            if lines:
                lines.append("")
            lines.append(context.upper() + (" (GRAPH EDITOR)" if context == "Graph" else ""))
        what = r["action"] + (" *" if r.get("hotkey_set") else "")
        lines.append("  %-30s %s" % (r["keys"], what))
    lines += ["", "* With the GroomLab hotkey set: GroomLab > Install GroomLab Hotkeys."]
    return "\n".join(lines)
