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


@undoable("clear sculpt edits")
def clear_edits(groom, node):
    """Drop Sculpt node `node`'s edits: its guides as it is fed them. One undo step."""
    _set_text(groom, groomlab_core.clear_sculpt_node(_text(groom), node))


def follow_graph_edit(groom, renamed=None, copied=None, kept=None):
    """The edits follow the Graph Editor's edits, in its undo step: renamed nodes ({old: new}, paths
    inside a compound too: Group moves "sculpt1" to "compound1/sculpt1", Ungroup back) take theirs
    along, a compound's inner nodes' with it; copies ({copy: source}) get a copy, and nodes no longer
    in the graph (`kept`: the graph's node names, None: not checked) lose theirs. The target follows
    a rename and is cleared with its node (grouped: brushes shape top-level nodes only)."""
    text = groomlab_core.move_sculpt_nodes(_text(groom), list((renamed or {}).items()))
    for copy, source in (copied or {}).items():
        text = groomlab_core.copy_sculpt_node(text, source, copy)
    if kept is not None:
        for name in groomlab_core.sculpted_nodes(text):
            if name.split("/")[0] not in kept:  # (a compound's inner node: its compound's name)
                text = groomlab_core.clear_sculpt_node(text, name)
    _set_text(groom, text)
    current = cmds.getAttr(groom + ".sculptNode") or ""
    wanted = (renamed or {}).get(current, current)
    if kept is not None and wanted not in kept:
        wanted = ""
    if wanted != current:
        cmds.setAttr(groom + ".sculptNode", wanted, type="string")
