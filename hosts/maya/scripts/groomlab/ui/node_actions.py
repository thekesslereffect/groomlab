# SPDX-License-Identifier: Apache-2.0
"""A node's actions in the Graph Editor's parameter panel (ParamPanel.node_actions), pinned over
its params: Maya's own per type (Cards, Simulate, Map, Braid, Instance, HairColor, StrandMesh,
Script), then graph_ui.json's nodeActions for the type. On its own beside param_panel.py (E6)."""

import json
import math
import os
import sys
from groomlab.base import no_undo
from groomlab.ui import qt
from groomlab.ui.qt import QtCore, QtGui, QtWidgets
from groomlab import graph as gm
from groomlab.ui import definitions, theme, widgets
from groomlab.ui.messages import plain as plain_message
from groomlab.ui.shortcuts import keys as shortcut_keys
from groomlab.ui.widgets import shrinkable
from groomlab.ui import graph_editor as _ge  # (the theme it applies: its colors)
from groomlab.ui.graph_editor import (ADVANCED, ATTRIBUTE_TIPS, CATEGORY_COLORS, KEYABLE_KINDS,
    MeshCombo, PLUMBING, SCATTER_LENGTH_TIP, category_of, describe, file_filter, guides_shape,
    key_state, later, library_category, names_editor, paint_key_state, param_sections, pretty,
    readable_on, show, show_value, shown_when, suggest_box, unit_of)


def node_actions(self, name):
    """A node's actions, pinned over its params (set_pinned): Maya's own first (Cards: stats, show,
    LOD, bake; Simulate: presets; Map: what it reads, Feed Param, Paint; Braid, Instance, HairColor,
    StrandMesh, Script), then graph_ui.json's nodeActions for its type (the Cards exports, Simulate's
    range, bake and clear, Bake Node to Guides, Clear Sculpt Edits, a compound's). None when it has
    none (or there is no groom)."""
    import groomlab
    from maya import cmds
    node_type = self.editor.model.node_type(name)
    groom = self.editor.groom
    buttons = []
    run = self.action_runner

    if node_type == "Cards":
        # What this node's cards cost now: cards, triangles, textures (its own share).
        try:
            stats = groomlab.card_stats(groom, node=name) if groom else None
        except RuntimeError:
            stats = None
        # Two lines that are always there (issue #47): what the cards cost, and the maps' status.
        # The pinned bar keeps its height as cards come and go and the maps go stale and fresh,
        # so the params under it never move.
        stats = stats or {"cards": 0, "triangles": 0, "textures": 0}
        if stats["cards"]:
            text = "%d cards · %d triangles · %d textures" % (stats["cards"], stats["triangles"],
                                                                     stats["textures"])
            if stats.get("feathers"):
                text += " · %d feathers" % stats["feathers"]
        else:
            text = "No cards yet (nothing feeds this node, or cardDensity is 0)"
        info = QtWidgets.QLabel(text)
        info.setObjectName("nodeAction_CardStats")
        info.setToolTip("This node's cards as they are now (at cardDensity). Change them with the params, "
                        "or pick a level of detail. All Cards nodes share one atlas and one bake")
        buttons.append(info)
        if stats.get("stale"):
            text, color, tip = "⚠ Maps out of date: rebake", _ge.WARNING, self.editor.CARDS_STALE
        elif stats.get("baked"):
            text, color, tip = "✓ Maps up to date", _ge.MUTED, "The baked maps match these cards"
        else:
            text, color, tip = "Maps not baked yet", _ge.MUTED, "Bake Maps makes the atlas's maps for these cards"
        status = QtWidgets.QLabel(text)
        status.setObjectName("nodeAction_CardsStatus")
        status.setStyleSheet("color: %s;%s" % (color.name(), " font-weight: bold;" if stats.get("stale") else ""))
        status.setToolTip(tip)
        buttons.append(status)
        buttons.append(None)
        show = QtWidgets.QCheckBox("Show cards")
        show.setObjectName("nodeAction_ShowCards")
        show.setToolTip("Cards view (A/B): the cards instead of the strands, with the baked maps once "
                        "baked, and the hair cap")
        show.setChecked(bool(groom) and bool(cmds.getAttr(groom + ".displayCards")))
        show.toggled.connect(lambda on: groom and groomlab.compare_cards(groom, on))
        buttons.append(show)
        textures = QtWidgets.QCheckBox("Show textures")
        textures.setObjectName("nodeAction_ShowCardTextures")
        textures.setToolTip("Draw the cards with their baked maps (color, coverage cut-out, AO); off shows "
                            "the flat card geometry (the groom's displayCardTextures)")
        textures.setChecked(bool(groom) and bool(cmds.getAttr(groom + ".displayCardTextures")))
        textures.toggled.connect(lambda on: groom and groomlab.card_display(groom, textures=on))
        buttons.append(textures)
        lod = QtWidgets.QPushButton("LOD")
        lod.setToolTip("Set this node for a budget: triangles, card count, texture variations, atlas size")
        lod_menu = QtWidgets.QMenu(lod)
        try:  # a node carding only feathers takes the feather budgets
            feathers = bool(groom) and groomlab.card_node_kind(groom, name, self.editor.model) == "feathers"
        except Exception:
            feathers = False
        levels = ((("hero", "Hero (~30k triangles, 8 quads a feather, 4K)"),
                   ("mid", "Mid (~8k triangles, 4 quads a feather, 2K)"),
                   ("low", "Low (~2k triangles, a quad a feather, 1K)")) if feathers else
                  (("hero", "Hero (~50k triangles, 4K)"), ("mid", "Mid (~15k triangles, 2K)"),
                   ("low", "Low (~4k triangles, 1K)")))
        for key, label in levels:
            # Into the editor's graph (it is what gets saved), on this node.
            lod_menu.addAction(label, run(lambda k=key: groomlab.cards_preset(groom, k, node=name,
                                                                              model=self.editor.model),
                                          lambda params, k=key: "%s cards: up to %d cards" % (k, params["maxCards"])))
        lod.setMenu(lod_menu)
        buttons.append(lod)
        buttons.append(None)

        buttons.append(("Bake Maps", "Depth, coverage, tangent, attribute, color, AO and normal maps for the atlas "
                        "(TGA, Unreal's hair card layout) into the scene's groomlab/cards folder; the cards "
                        "view shows them",
                        run(lambda: groomlab.bake_cards(groom),
                            lambda paths: "baked %d card maps into %s" % (len(paths), os.path.dirname(paths[0]) if paths else ""),
                            busy="Baking the card maps (depth, coverage, tangent, color, AO)...")))
        buttons.append(("Bake Scalp", "The groom baked onto its scalp's UVs (hair color and coverage) into the "
                        "scene's groomlab/cards folder, with a hair cap mesh that shows it under the cards: no "
                        "bare skin between low LOD cards",
                        run(lambda: groomlab.bake_scalp(groom),
                            lambda paths: "baked %d scalp maps and a hair cap" % len(paths),
                            busy="Baking the scalp maps and the hair cap...")))
    elif node_type == "Map":
        map_name = self.editor.model.param(name, "name") or "mask"
        if not any(c["from"] == name for c in self.editor.model.connections):
            hint = QtWidgets.QLabel("\u26a0 Feeds nothing yet: pick a param under Feed Param")
            hint.setObjectName("nodeAction_MaskUnconnected")
            hint.setStyleSheet("color: %s;" % _ge.WARNING.name())
            hint.setToolTip("A map changes the groom only through the params it feeds: expose a mappable param "
                            "as an input (Feed Param, or right-click the param) and connect this node to it")
            buttons.append(hint)
        kind = groomlab.map_kind(groom, map_name) if groom else ""
        if groom and not kind:
            missing = QtWidgets.QLabel("\u26a0 No map named %s yet: Groom Tools > Paint > New..." % map_name)
            missing.setObjectName("nodeAction_MapMissing")
            missing.setStyleSheet("color: %s;" % _ge.WARNING.name())
            buttons.append(missing)
        elif kind:
            where = {"guides": "a layer on the guides", "vertices": "a value per scalp vertex",
                     "texture": "a texture at the scalp's UVs"}.get(kind, kind)
            stored = next((m["texture"] for m in groomlab.maps(groom) if m["name"] == map_name), "") \
                if kind == "texture" else ""
            # A zero-width space after each slash: a long path wraps at its folders, not mid-name.
            shown = stored.replace("\\", "/").replace("/", "/\u200b")
            info = QtWidgets.QLabel("%s: %s" % (map_name, where) + ("\n" + shown if shown else ""))
            info.setObjectName("nodeAction_MapInfo")
            info.setWordWrap(True)
            info.setProperty("role", "about")
            info.setToolTip("Where the map lives now (Convert to in Groom Tools > Paint changes it; the node "
                            "follows by name)" + ("\n" + groomlab.map_texture(groom, map_name) if stored else ""))
            buttons.append(info)
            if stored and map_name in groomlab.missing_map_files(groom):
                lost = QtWidgets.QLabel("\u26a0 The image is missing: %s" % groomlab.map_texture(groom, map_name))
                lost.setObjectName("nodeAction_MapImageMissing")
                lost.setStyleSheet("color: %s;" % _ge.WARNING.name())
                lost.setToolTip("Relink it to the file (or GroomLab > Utilities > Find Missing Files... looks "
                                "for it in a folder); until then the map reads nothing")
                buttons.append(lost)
                buttons.append(("Relink Image...", "Point the map at its image: a PNG is used in place, another "
                                "format copied as a PNG into the masks folder",
                                lambda _=False, m=map_name: later(lambda: self.relink_map_image(m))))
                buttons.append(None)
        feed = QtWidgets.QPushButton("Feed Param")
        feed.setObjectName("nodeAction_FeedParam")
        feed.setToolTip("Feed this map into a node's mappable param (exposed as an input and connected; the "
                        "param's own value multiplies the map)")
        feed_menu = QtWidgets.QMenu(feed)
        feed_menu.submenus = []  # explicit parents, kept: PySide would collect addMenu("title") results
        model = self.editor.model
        # A color map feeds from one of the node's outputs (a channel: packed masks); a gray one
        # from its first.
        color = kind in ("vertices", "texture") and groomlab.map_channels(groom, map_name) == 4
        for output in (model.output_names(name) if color else [None]):
            target = feed_menu
            if output is not None:
                target = QtWidgets.QMenu("From %s" % output, feed_menu)
                feed_menu.addMenu(target)
                feed_menu.submenus.append(target)
            for other in model.order:
                if other == name or model.node_type(other) in ("Map", "Expression", "FieldNoise", "FieldRemap",
                                                                 "FieldCombine", "Curve", "Control"):
                    continue
                # rgb (the map's color) feeds color params; the rest feed numbers.
                mappable = [p["name"] for p in model.node_spec(other)["params"]
                            if (p.get("colorField") if output == "rgb" else p.get("mappable"))]
                if not mappable:
                    continue
                sub = QtWidgets.QMenu("%s (%s)" % (other, model.node_type(other)), target)
                target.addMenu(sub)
                feed_menu.submenus.append(sub)
                for param in mappable:
                    sub.addAction(param, run(lambda o=other, q=param, out=output: groomlab.feed_map_node(
                        groom, name, o, q, model=self.editor.model, output=out),
                        lambda fed, out=output: "%s%s feeds %s" % (name, "." + out if out else "", fed)))
        feed.setMenu(feed_menu)
        buttons.append(feed)

        def paint():
            from groomlab.ui import palette
            groomlab.paint_map(groom, map_name)
            palette.show(tab="paint")
            palette.select_map(map_name)
            return map_name
        buttons.append(("Paint...", "Paint this map in Groom Tools > Paint: the Paint brush on the guides, the "
                        "scalp's vertices or its texture, whichever kind the map is (white is 1, Ctrl paints "
                        "black); every param this node feeds follows each stroke",
                        run(paint, lambda c: "painting %s" % c)))
    elif node_type == "Script":
        buttons.append(("Edit Node...", "Edit this script node here: its script, params and header, live",
                        lambda _=False: later(lambda: self.editor.show_script_editor(name))))
        buttons.append(("Save to Library", "Write this script node to your node library (Add Node, Tab search; "
                        "the standalone app and Blender too), one version up when the library has it",
                        lambda _=False: later(lambda: self.editor.save_script_to_library(name))))
    elif node_type in ("SculptGuides", "SculptStrands") and groom:
        from groomlab import sculpts
        edited = sculpts.edited_guides(groom, name)
        brushing = sculpts.target(groom) == name
        what = "strands" if node_type == "SculptStrands" else "guides"
        buttons.append(QtWidgets.QLabel(
            "%s. %s" % ("%d %s sculpted" % (edited, what) if edited else "No strokes yet",
                        "Brushes shape this node's %s (shaping brushes; Esc in the brush tool: done)" % what if brushing else
                        "Select this node (or its row in Sculpt > Correctives) to brush its %s" % what)))
    elif node_type == "Simulate":
        # One click to a preset's settings (the core's, every host's), into the editor's graph.
        from groomlab import sim
        preset = QtWidgets.QPushButton("Preset")
        preset.setObjectName("nodeAction_SimulatePreset")
        preset.setToolTip("Set this node's motion settings to a preset (bend, twist, damping, drag, root "
                          "stiffness and its profile, inertia, iterations); gravity, wind, collisions and "
                          "water stay as they are")
        preset_menu = QtWidgets.QMenu(preset)
        preset_menu.setToolTipsVisible(True)
        for info in sim.simulate_presets():
            action = preset_menu.addAction(info["title"], run(
                lambda k=info["name"]: sim.simulate_preset(groom, k, node=name, model=self.editor.model),
                lambda params, t=info["title"]: "%s: %s settings" % (name, t)))
            action.setToolTip(info["description"])
        preset.setMenu(preset_menu)
        buttons.append(preset)
    elif node_type == "ToMesh":
        show = QtWidgets.QCheckBox("Show mesh")
        show.setObjectName("nodeAction_ShowMesh")
        show.setToolTip("The ToMesh nodes' meshes instead of the strands in the viewport (the groom's displayMesh)")
        show.setChecked(bool(groom) and bool(cmds.getAttr(groom + ".displayMesh")))
        show.toggled.connect(lambda on: groom and cmds.setAttr(groom + ".displayMesh", bool(on)))
        buttons.append(show)

        def mesh_in_scene():
            shape = groomlab.to_mesh_shape(groom)
            cmds.select(shape)
            self.editor.report("%s: the ToMesh nodes' live mesh (export it with Alembic)" % shape.split("|")[-1])
        buttons.append(("Mesh in Scene", "A Maya mesh on the groom's outMesh (selected if there is one): the ToMesh "
                        "nodes' meshes, following the animation, to sculpt, model or export (one undo step)",
                        lambda _=False: later(mesh_in_scene)))

        def export_mesh():
            from groomlab.ui import menu
            path = cmds.fileDialog2(fileFilter="OBJ (*.obj);;FBX (*.fbx);;Alembic, this frame (*.abc)",
                                    dialogStyle=2, fileMode=0, caption="Export Mesh",
                                    startingDirectory=menu.default_file(groom, "exports", "_%s.obj" % name))
            if path:
                written = groomlab.export_mesh(groom, path[0], node=name)
                self.editor.report("%s: wrote %s" % (name, written))
        buttons.append(("Export Mesh...", "This node's mesh at the current frame as an OBJ, FBX or Alembic file "
                        "(for ZBrush, baking or printing)", lambda _=False: later(export_mesh)))
    elif node_type == "HairColor":
        buttons.append(("Create Hair Shader", "An aiStandardHair rendering this node's colors (its melanin, or "
                        "with mode color or a tip color its RGB), assigned to the groom; it follows later "
                        "changes of the look. Without one, Arnold renders the groom's colors anyway",
                        run(lambda: groomlab.create_hair_shader(groom), lambda s: "hair shader %s" % s)))
    elif node_type in ("Instance", "Scales"):
        def prototype():
            picked = groomlab.selected_meshes()  # (a mesh, not the groom selected with it)
            if not picked:
                raise ValueError("select the mesh(es) to instance")
            return groomlab.set_instance_prototype(groom, name, picked, model=self.editor.model)
        buttons.append(("Object(s) from Selection", "Instance the selected meshes (several: variants, one per "
                        "instance; hide them: only their instances render)",
                        run(prototype, lambda m: "instancing %s" % m)))
    # The shared ones (graph_ui.json's nodeActions) for its type, two to a row.
    table = self.node_action_table(name)
    shared = [item for item in definitions.resolve(definitions.get("graph/nodeActions")["items"],
                                                   self.node_action_state(name))
              if item["kind"] == "action"]
    if shared:
        buttons.append(None)
    for item in shared:
        buttons.append((item["label"], item["tip"], lambda _=False, fn=table[item["id"]]: fn()))
    if not buttons or not groom:
        return None
    box = QtWidgets.QFrame()
    box.setObjectName("nodeActions")
    box.setToolTip(definitions.get("graph/nodeActions").get("tip", ""))
    rows = QtWidgets.QVBoxLayout(box)
    rows.setContentsMargins(theme.px(10), theme.px(6), theme.px(10), theme.px(4))
    rows.setSpacing(theme.px(4))
    row, pushed = None, 0
    for b in buttons:
        if b is None:  # a new row
            row = None
            continue
        if isinstance(b, QtWidgets.QLabel):  # text: a row of its own, the panel's full width
            b.setWordWrap(True)
            rows.addWidget(b)
            row = None
            continue
        if row is None or (pushed >= 2 and not isinstance(b, QtWidgets.QCheckBox)):
            row, pushed = QtWidgets.QHBoxLayout(), 0  # two buttons to a row: a narrow panel keeps them whole
            rows.addLayout(row)
        if isinstance(b, QtWidgets.QWidget):
            row.addWidget(b)
            pushed += isinstance(b, QtWidgets.QPushButton)
            continue
        label, tip, fn = b
        button = QtWidgets.QPushButton(label)
        button.setObjectName("nodeAction_" + label.replace(" ", "").replace(".", ""))
        button.setToolTip(tip)
        button.clicked.connect(fn)
        row.addWidget(button)
        pushed += 1
    # A line under them: the params scroll below it.
    line = QtWidgets.QFrame()
    line.setFrameShape(QtWidgets.QFrame.HLine)
    line.setFrameShadow(QtWidgets.QFrame.Sunken)
    rows.addSpacing(theme.px(2))
    rows.addWidget(line)
    return box
