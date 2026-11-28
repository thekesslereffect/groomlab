# SPDX-License-Identifier: Apache-2.0
"""The Tube tool's root-first mode in Maya's viewport (issue 61).

The tool is the core's (engine/core/include/groomlab/root_tube_tool.h), the same in Blender and the
app: GroomLab's brush context runs it in its "roots" mode (groomLabBrushCtx -rootEdit,
-rootSettings, -rootState) and draws its outlines with the groom. This keeps the names the Tube
card uses.

Draw root: click the root's outline on the scalp one CV at a time (a CV snaps onto a neighbouring
root's CVs or edges, so roots seal with no gap); click the first CV again to close it, then draw the
tube's path from its middle (or, without Draw before extrude, it goes straight off the scalp).
Undoing the tube just made brings its outline back. Edit roots: drag CVs (Ctrl+click an edge adds
one, a CV takes it out of the root edged green), then Finish edit (leaving the tool finishes it
too). Select root: click a root's middle (Shift: several), drag to carry them over the scalp;
Duplicate sets copies down where you click; Delete removes them.
"""

import json

from maya import cmds

import groomlab


class _NoUndo(object):
    """Display edits kept out of the undo queue (the guides hidden while editing roots)."""

    def __enter__(self):
        self.state = cmds.undoInfo(query=True, stateWithoutFlush=True)
        cmds.undoInfo(stateWithoutFlush=False)

    def __exit__(self, *_):
        cmds.undoInfo(stateWithoutFlush=self.state)


class RootTubeTool(object):
    """The card's handle on the context's root tool (one per session: TOOL)."""

    def __init__(self):
        self.groom = None
        self.settings = {"draw": True, "length": 1.0, "segments": 12, "taper": 0.5, "snap": True, "group": None,
                         "density": 1.0}
        self.listeners = []     # called with the tool's message after each change (the Groom Tools card)
        self.state = {}
        self.guides_shown = None
        self._heard = -1

    # -- The context --------------------------------------------------------------------

    def _context(self):
        from groomlab.brushes import BRUSH_CONTEXT
        groomlab.ensure_plugin()
        if not cmds.contextInfo(BRUSH_CONTEXT, exists=True):
            cmds.groomLabBrushCtx(BRUSH_CONTEXT)
        return BRUSH_CONTEXT

    def _edit(self, words):
        cmds.groomLabBrushCtx(self._context(), edit=True, rootEdit=words)
        return self.poll()

    def _push_settings(self):
        s = self.settings
        cmds.groomLabBrushCtx(self._context(), edit=True, rootSettings=json.dumps({
            "drawPath": bool(s["draw"]), "snap": bool(s["snap"]), "length": float(s["length"]),
            "segments": int(s["segments"]), "taper": float(s["taper"]), "density": float(s.get("density", 1.0)),
            "group": s.get("group") or ""}))

    def poll(self):
        """The tool's state as the context has it (read again after an undo: the outlines follow),
        its message to the listeners when it has a new one, the guides hidden while editing."""
        from groomlab.brushes import BRUSH_CONTEXT
        if not cmds.contextInfo(BRUSH_CONTEXT, exists=True):
            self.state = {}
            return self.state
        try:
            self.state = json.loads(cmds.groomLabBrushCtx(BRUSH_CONTEXT, query=True, rootState=True) or "{}")
        except (RuntimeError, ValueError):
            self.state = {}
        # (Edit roots no longer hides the guides and tubes on its own: they show as the Visibility
        # switches say; one hidden by an older build is shown again.)
        self.show_guides(True)
        count = self.state.get("messageCount", 0)
        if count != self._heard:
            self._heard = count
            self.say(self.state.get("message", ""))
        return self.state

    def say(self, text):
        for fn in list(self.listeners):
            try:
                fn(text)
            except Exception:  # noqa: BLE001 (a closed panel)
                pass
        if text:
            print("GroomLab Root tubes: " + text)

    # -- What the card reads --------------------------------------------------------------------

    def active(self):
        """Whether the root tool is the current tool."""
        from groomlab.brushes import BRUSH_CONTEXT
        return cmds.currentCtx() == BRUSH_CONTEXT and bool(self.state.get("active"))

    @property
    def mode(self):
        return self.state.get("mode", "place") if self.state.get("active") else None

    @property
    def selected(self):
        """The last root selected (None: none)."""
        picked = self.state.get("selected") or []
        return picked[-1] if picked else None

    @property
    def picked(self):
        return list(self.state.get("selected") or [])

    # -- Starting and leaving -----------------------------------------------------------------

    def start(self, groom, mode="place"):
        """Make the tool current on `groom` in "place" (draw roots), "edit" (edit roots' CVs) or
        "select" (pick, carry, duplicate and delete roots)."""
        if not groom:
            raise ValueError("select a groom first")
        if mode not in ("place", "edit", "select"):
            raise ValueError("mode must be place, edit or select")
        self.groom = groom
        from groomlab import groom as groom_module
        if groomlab.active_groom() != groom:
            groom_module.set_current_groom(groom)  # (the brush context works on the current groom)
        context = self._context()
        cmds.groomLabBrushCtx(context, edit=True, mode="roots")
        self._push_settings()
        if cmds.currentCtx() != context:
            cmds.setToolTo(context)
        return self._edit("start " + mode)

    def leave(self):
        """The tool put away: an edit not finished given to its roots, its outlines gone."""
        from groomlab.brushes import BRUSH_CONTEXT
        if cmds.contextInfo(BRUSH_CONTEXT, exists=True) and self.groom and cmds.objExists(self.groom):
            try:
                self._edit("leave")
            except RuntimeError:
                pass
        self.show_guides(True)

    def update_settings(self, **settings):
        self.settings.update(settings)
        if self.groom:
            self._push_settings()
            self.poll()

    # -- The card's buttons ----------------------------------------------------------------------

    def undo_cv(self):
        return self._edit("undoCv")

    def finish_edit(self):
        return self._edit("finish")

    def begin_duplicate(self):
        return self._edit("duplicate")

    def end_duplicate(self):
        return self._edit("endDuplicate")

    def delete_selected(self):
        return self._edit("delete")

    def select_shown(self):
        """Every root shown (the level in view) selected: Shift+click on the Select root button."""
        return self._edit("selectShown")

    # -- The guides while editing ----------------------------------------------------------------

    def show_guides(self, on):
        """Edit roots hides the groom's guides (and their tubes), showing only the roots' outlines."""
        if not self.groom or not cmds.objExists(self.groom):
            self.guides_shown = None
            return
        with _NoUndo():
            if not on:
                if self.guides_shown is None:
                    self.guides_shown = groomlab.guides_visible(self.groom)
                    groomlab.set_guides_visible(self.groom, False)
            elif self.guides_shown is not None:
                groomlab.set_guides_visible(self.groom, self.guides_shown)
                self.guides_shown = None


TOOL = RootTubeTool()


def start(groom, mode="place", **settings):
    """The root-first tube tool on `groom` ("place": draw roots; "edit": edit the root tubes' CVs;
    "select": select, move, duplicate and delete roots); `settings`: draw, length, segments,
    taper, snap, group, density."""
    TOOL.settings.update(settings)
    TOOL.start(groom, mode)
    return TOOL
