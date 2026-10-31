# SPDX-License-Identifier: GPL-3.0-or-later
"""What each action id of the shared UI definitions (engine/ui/actions.json, docs/UI_DEFINITIONS.md)
runs in Blender: BINDINGS {action id: (operator idname, {property: value})}, Maya's menu.ACTIONS
and window ACTIONS tables in one dict.

menu.py and palette.py build the menus, the Groom Tools' bars and the lists' right-click menus
from the definitions and this table. An id the definitions show for Blender without a binding
here is hidden (the policy: Blender shows only what it runs); tests/hosts/blender/
test_addon_ui.py lists those ids (ALLOWED_MISSING: what is left to do for parity).

Forms of a binding:
- ("groomlab.op", {"prop": value}): the operator with those properties.
- An item generated from an option list (a "choices" generator: Convert to's kinds) passes its
  value to the property the binding's "$value" names ("value" when it names none).
- ("prop:groomlab_tools.name", {}): a checkbox of the scene's setting (a checkable action that is
  a setting, as Delete Its File Too).

The Graph Editor's ids (graph_ui.json: node, backdrop, Edit, Add Node, Presets and param menus)
are bound here too, by the Graph Editor's authors; the Groom Tools' and the GroomLab menu's are
below."""

DOCS_URL = "https://getgroomlab.com/docs/blender"
UPDATES_URL = "https://getgroomlab.com/download"  # the Download page (Blender's own updates: the remote repository)
DONATE_URL = "https://ko-fi.com/getgroomlab"  # Support GroomLab: free for any use, funded by donations
DISCORD_URL = "https://discord.gg/sr2rxjRVkm"  # the GroomLab community

BINDINGS = {
    # The GroomLab menu (menus.json "main") and the Groom Tools' header, empty state and Output strip.
    "tools.open": ("groomlab.groom_tools", {}),
    "graph.open": ("groomlab.graph_open", {}),
    "groom.create": ("groomlab.add_groom", {}),
    "file.import": ("groomlab.load_groom", {}),
    "file.reload": ("groomlab.reload_groom", {}),
    "file.save": ("groomlab.save_groom_linked", {}),
    "file.saveAs": ("groomlab.save_groom", {}),
    "sample.nika": ("groomlab.add_sample", {"sample": "nika"}),
    "sample.manny": ("groomlab.add_sample", {"sample": "manny"}),
    "sample.wolf": ("groomlab.add_sample", {"sample": "wolf"}),
    "sample.buddy": ("groomlab.add_sample", {"sample": "buddy"}),
    "add.feathers": ("groomlab.add_feathers", {}),
    "add.braid": ("groomlab.add_braid", {}),
    "add.cards.hero": ("groomlab.add_cards", {"lod": "hero"}),
    "add.cards.mid": ("groomlab.add_cards", {"lod": "mid"}),
    "add.cards.low": ("groomlab.add_cards", {"lod": "low"}),
    "add.collider": ("groomlab.add_collider", {}),
    "add.wind": ("groomlab.add_node_before_output", {"type": "Wind"}),
    "add.jiggle": ("groomlab.add_node_before_output", {"type": "Jiggle"}),
    "sim.add": ("groomlab.simulate_add", {}),
    "sim.range": ("groomlab.simulate_range", {"bake": False}),
    "sim.bake": ("groomlab.simulate_range", {"bake": True}),
    "sim.clear": ("groomlab.simulate_clear", {}),
    "display.showAllHair": ("groomlab.eye", {"what": "STRANDS", "mode": "SHOW", "every": True}),
    "display.hideAllHair": ("groomlab.eye", {"what": "STRANDS", "mode": "HIDE", "every": True}),
    "display.showAllGuides": ("groomlab.eye", {"what": "GUIDES", "mode": "SHOW", "every": True}),
    "display.hideAllGuides": ("groomlab.eye", {"what": "GUIDES", "mode": "HIDE", "every": True}),
    "review.heatmap": ("groomlab.review_heatmap", {}),
    "review.hud": ("prop:groomlab_tools.show_hud", {}),
    "review.turntable": ("groomlab.turntable", {}),
    "export.alembic": ("groomlab.export_alembic", {}),
    "export.usd": ("groomlab.export_usd", {}),
    "export.vrscene": ("groomlab.export_vrscene", {}),
    "export.unrealGroom": ("groomlab.export_unreal_groom", {}),
    "export.unrealCards": ("groomlab.export_unreal_cards", {}),
    "export.unityStrands": ("groomlab.export_unity_strands", {}),
    "export.unityFeathers": ("groomlab.export_unity_feathers", {}),
    "export.unityCards": ("groomlab.export_unity_cards", {}),
    "export.gltfCards": ("groomlab.export_gltf_cards", {}),
    "export.cardMaps": ("groomlab.bake_card_maps", {}),
    "export.scalesFbx": ("groomlab.export_scales_fbx", {}),
    "export.scalesUsd": ("groomlab.export_scales_usd", {}),
    # Utilities (pipeline_ops.py)
    "strands.toMesh": ("groomlab.to_mesh", {}),
    "scales.bakeMaps": ("groomlab.bake_scale_maps", {}),
    "flow.deleteAll": ("groomlab.flow_clear", {}),
    "groom.transfer": ("groomlab.transfer_groom", {}),
    "graph.copyToGrooms": ("groomlab.copy_graph_to_grooms", {}),
    "files.findMissing": ("groomlab.find_missing_files", {}),
    "node.bakeToGuides": ("groomlab.bake_to_guides", {}),
    "import.alembicCurves": ("groomlab.import_alembic_curves", {}),
    "photos.guides": ("groomlab.photos_guides", {}),
    "help.docs": ("wm.url_open", {"url": DOCS_URL}),
    "help.shortcuts": ("groomlab.shortcuts", {}),
    "help.updates": ("groomlab.check_updates", {}),
    "help.discord": ("wm.url_open", {"url": DISCORD_URL}),
    "help.donate": ("wm.url_open", {"url": DONATE_URL}),
    "help.about": ("groomlab.about", {}),

    # Guide groups (lists.json "groups"): the bar, the right-click menu and Edit Guides.
    "group.new": ("groomlab.group_new", {}),
    "group.rename": ("groomlab.group_edit", {"action": "RENAME"}),
    "group.color": ("groomlab.group_color", {}),
    "group.defaultColor": ("groomlab.group_edit", {"action": "DEFAULT_COLOR"}),
    "group.select": ("groomlab.group_edit", {"action": "SELECT"}),
    "group.resetSegments": ("groomlab.group_edit", {"action": "RESET_SEGMENTS"}),
    "group.delete": ("groomlab.group_edit", {"action": "DELETE"}),
    "group.promoteDemote": ("groomlab.group_promote_demote", {}),
    "guides.children": ("groomlab.guides_edit", {"action": "CHILDREN"}),
    "guides.syncPos": ("groomlab.guides_edit", {"action": "SYNC_POS"}),
    "guides.syncNeg": ("groomlab.guides_edit", {"action": "SYNC_NEG"}),
    "guides.mirrorSelection": ("groomlab.guides_edit", {"action": "MIRROR_SELECTION"}),
    "guides.mirrorGroup": ("groomlab.guides_mirror_group", {}),
    "guides.reparent": ("groomlab.guides_reparent", {}),
    "guides.clearSelection": ("groomlab.guides_edit", {"action": "CLEAR"}),

    # Parts (the Part tool's card).
    "parts.remove": ("groomlab.part_remove", {}),
    "parts.clear": ("groomlab.parts_clear", {}),

    # Maps (the Paint tab's list).
    "map.new": ("groomlab.map_new", {}),
    "map.import": ("groomlab.map_import", {}),
    "map.rename": ("groomlab.map_rename", {}),
    "map.convert": ("groomlab.map_convert", {"$value": "kind"}),
    "map.makeGray": ("groomlab.map_set_channels", {"channels": 1}),
    "map.makeColor": ("groomlab.map_set_channels", {"channels": 4}),
    "map.relink": ("groomlab.map_relink", {}),
    "map.reloadImage": ("groomlab.map_reload", {}),
    "map.showFile": ("groomlab.map_show_file", {}),
    "map.delete": ("groomlab.map_remove", {}),
    "map.deleteFiles": ("prop:groomlab_tools.map_delete_files", {}),

    # Correctives (the Correctives tab) and Sculpt nodes.
    "corrective.new": ("groomlab.corrective_new", {"fix": False}),
    "corrective.fixFrame": ("groomlab.corrective_new", {"fix": True}),
    "corrective.up": ("groomlab.corrective_move", {"step": 1}),
    "corrective.down": ("groomlab.corrective_move", {"step": -1}),
    "corrective.solo": ("groomlab.corrective_solo", {}),
    "corrective.delete": ("groomlab.corrective_delete", {}),
    "corrective.goTo": ("groomlab.corrective_go_to", {}),
    "corrective.keyWeight": ("groomlab.corrective_key", {}),
    "corrective.clearKeys": ("groomlab.corrective_clear_keys", {}),
    "corrective.rename": ("groomlab.corrective_rename", {}),
    "sculpt.clearEdits": ("groomlab.sculpt_clear", {}),
    # The Graph Editor's node menu: brush a Sculpt node's guides (the operator takes "node").
    "node.brushSculpt": ("groomlab.sculpt_target", {}),

    # Photos (the Photos tab: lists.json "photos", photos.py).
    "photo.import": ("groomlab.photo_import", {}),
    "photo.lookThrough": ("groomlab.photo_look_through", {}),
    "photo.rename": ("groomlab.photo_rename", {}),
    "photo.remove": ("groomlab.photo_remove", {}),
    "pairs.solve": ("groomlab.pairs_solve", {}),
    "pairs.clear": ("groomlab.pairs_clear", {}),
    "pairs.delete": ("groomlab.pairs_delete", {}),
    "pairs.repickPhoto": ("groomlab.pairs_repick", {"kind": "image"}),
    "pairs.repickMesh": ("groomlab.pairs_repick", {"kind": "mesh"}),
}


def binding(action_id):
    """(idname, props) of an action, or None when Blender does not run it (hidden)."""
    return BINDINGS.get(action_id)


def bound(action_id):
    """Whether Blender runs the action: here, or among the Graph Editor's (graph_ui.ACTIONS)."""
    if action_id in BINDINGS:
        return True
    from . import graph_ui
    return action_id in graph_ui.ACTIONS


def action_for(idname, properties=None):
    """The action id an operator call stands for: the binding of `idname` whose properties all
    match `properties` (an operator's, or a dict), the most specific first; None when none does."""
    best, best_n = None, -1
    for action_id, (op, props) in BINDINGS.items():
        if op != idname:
            continue
        fixed = {k: v for k, v in props.items() if not k.startswith("$")}
        try:
            ok = all((properties[k] if isinstance(properties, dict) else getattr(properties, k)) == v
                     for k, v in fixed.items())
        except (AttributeError, KeyError, TypeError):
            ok = False
        if ok and len(fixed) > best_n:
            best, best_n = action_id, len(fixed)
    return best


def tip_for(idname, properties=None, fallback=""):
    """The definitions' tooltip of the action an operator call stands for (an operator's
    description: every tooltip from actions.json)."""
    from . import definitions
    action_id = action_for(idname, properties)
    if action_id is None:
        return fallback
    return definitions.action(action_id).get("tip") or fallback
