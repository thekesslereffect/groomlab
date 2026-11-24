# SPDX-License-Identifier: Apache-2.0
"""Sculpt nodes: brush the guides the graph made.

A Sculpt node sits on generated guides (Scatter + Interpolate -> ToGuides -> Sculpt -> the next
Interpolate, Clump centers...). Pick it as what brushes shape (select it in the Graph Editor, or its
row in Sculpt > Correctives): the viewport shows its output as the guides, and shaping brushes
(Comb, Smooth, Lift, Length, Twist, Straighten, Attract, Cut) reshape them.
Each stroke is one undo step, kept in the groom's sculptData by node name and guide ID as a
relative form: when what feeds the node changes (the sparse guides combed, density, a Noise), the
guides are made again and the strokes add the same bends and length changes on top.

    import groomlab.sculpts as sculpts
    sculpts.set_target(groom, "sculpt1")   # brushes shape sculpt1's guides
    ...brush...
    sculpts.clear_edits(groom, "sculpt1")  # its guides as fed again (one undo step)
    sculpts.set_target(groom, None)        # back to the base groom
"""

import json

import groomlab_core
from maya import cmds

from groomlab import undoable

TYPE = "Sculpt"


def _text(groom):
    return cmds.getAttr(groom + ".sculptData") or ""


def _set_text(groom, text):
    if text != _text(groom):
        cmds.setAttr(groom + ".sculptData", text, type="string")


def sculpt_nodes(groom):
    """The groom's Sculpt nodes, in graph order (top level)."""
    from groomlab.params import get_graph
    return [n["name"] for n in get_graph(groom).get("nodes", []) if n.get("type") == TYPE]


def sculpted_nodes(groom):
    """The nodes holding edits (a node renamed or deleted outside the Graph Editor keeps them, unused)."""
    return list(groomlab_core.sculpted_nodes(_text(groom)))


def edited_guides(groom, node):
    """How many guides of `node` hold an edit."""
    text = _text(groom)
    data = json.loads(text) if text.strip() else {}
    return len(data.get(node, {}).get("guides", {}))


def target(groom):
    """The Sculpt node brushes shape, or None (the base groom or a corrective)."""
    return cmds.getAttr(groom + ".sculptNode") or None


def set_target(groom, node):
    """Brushes shape Sculpt node `node`'s guides (shot sculpting off), or none (None)."""
    if node:
        if node not in sculpt_nodes(groom):
            raise ValueError("%s has no Sculpt node named %r" % (groom, node))
        if cmds.getAttr(groom + ".shotSculpt"):
            cmds.setAttr(groom + ".shotSculpt", False)
    if (cmds.getAttr(groom + ".sculptNode") or "") != (node or ""):
        cmds.setAttr(groom + ".sculptNode", node or "", type="string")


def leave(groom):
    """Brushes shape the guides again, not a Sculpt node (Esc in the brush tool: Sculpt Sections
    left, the braid's path brushed among the guides); the Graph Editor's panel follows. Returns the
    node left, or None."""
    node = target(groom)
    if not node:
        return None
    braid = sculpted_braid(groom, node)
    from groomlab.base import no_undo
    with no_undo():  # (what brushes shape is a tool setting: no undo step, as the Graph Editor's)
        set_target(groom, None)
    text = "brushing the guides again" + (" (done sculpting %s's sections)" % braid if braid else "")
    from maya.api import OpenMaya as om
    om.MGlobal.displayInfo("GroomLab: " + text)
    try:
        from groomlab.ui import graph_editor
        graph_editor.sculpt_target_changed(groom, text)
    except ImportError:
        pass
    return node


def sculpted_braid(groom, node):
    """The Braid whose sections Sculpt node `node` brushes (Sculpt Sections; graph_inserts.h
    sculptedBraid), or None."""
    from groomlab.params import get_graph
    edit = groomlab_core.GraphEdit(json.dumps(get_graph(groom)))
    return edit.sculpted_braid(node or "") or None


@undoable("clear sculpt edits")
def clear_edits(groom, node):
    """Drop Sculpt node `node`'s edits: its guides as it is fed them. One undo step."""
    _set_text(groom, groomlab_core.clear_sculpt_node(_text(groom), node))


# The edits follow the Graph Editor's edits (renames, copies, removals) in GraphModel.save's undo
# step, as every host's: groomlab_core.follow_graph_edit (the editor's graph_edits.h).
