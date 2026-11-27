# SPDX-License-Identifier: Apache-2.0
"""The add-on's UI from the shared definitions (engine/ui/*.json, docs/UI_DEFINITIONS.md) in a
headless Blender (run_blender_test.py installs the extension in a throwaway profile):

- the loader reads the extension's ui/ folder with no helper running, as Maya's definitions.py;
- every action id the definitions show for Blender has a binding in actions.py (an operator that
  exists), except the ids listed below: ALLOWED_MISSING is what is left to do for parity (Blender
  hides those), NOT_IN_BLENDER what Blender does its own way;
- the GroomLab menu, the Groom Tools (every tab and section), the lists' right-click menus and the
  pie are built from the files, in their order, unbound actions hidden;
- the Groom Tools' operators the helper's B1/B2 commands back: guide group colors, Segments,
  Select Mirror Guides, Reparent, Promote / Demote, Mirror Group, parts, flow, the maps' menu (rename,
  convert, gray / color, fill, import, relink, reload, read by, delete with its file), Tubes (shape,
  library, width, flatness, twist, smooth, Fill Tubes, the Shape brush's channel), correctives
  (new, fix frame, weight keyed with Blender's keyframes, solo, move, rename, clear keys, delete),
  Sculpt nodes, the linked groom file and Add > Wind / Jiggle."""

import importlib
import json
import os
import shutil
import sys
import types
import unittest

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blender_util as bu  # noqa: E402

# The helper (started later) adds alphas to the test's own folder, never the user's.
os.environ["GROOMLAB_USER_ALPHAS"] = os.path.join(bu.TMP, "alphas")
shutil.rmtree(os.environ["GROOMLAB_USER_ALPHAS"], ignore_errors=True)

pkg, m = bu.enable()
groom, process, graph = m["groom"], m["process"], m["graph"]
defs = importlib.import_module(bu.MODULE + ".definitions")
actions = importlib.import_module(bu.MODULE + ".actions")
menu = importlib.import_module(bu.MODULE + ".menu")
palette = importlib.import_module(bu.MODULE + ".palette")
tools = importlib.import_module(bu.MODULE + ".tools")
tool_props = importlib.import_module(bu.MODULE + ".tool_props")
tool_ops = importlib.import_module(bu.MODULE + ".tool_ops")

# Ids the definitions show for Blender that it does not run yet: hidden in Blender. This is the
# parity checklist; bind one in actions.py (and take it off here) as it lands.
ALLOWED_MISSING = set()
# The Graph Editor's ids (graph_ui.json) Blender does not run yet (the rest are bound in graph_ui.ACTIONS).
ALLOWED_MISSING_GRAPH = {
    "graph.gpuPreview",   # the GPU preview is Maya's viewport (Blender draws what the helper builds)
}
# Ids Blender does its own way (n/a in the parity audit). (GroomLab's themes run here too since
# themes.py: GroomLab > Theme restyles the window and the Graph Editor's canvas, as in Maya and the app.)
NOT_IN_BLENDER = set()


MAIN = ["Groom Tools", "Graph Editor", "Find Command...", "Create Groom from Selection", "Start From", "Add", "Groom File",
        "Export", "Render", "Simulate", "Utilities", "Display", "Settings", "Help"]


def blender_ids():
    """{action id: [containers]} of every id the definitions can show on Blender."""
    out = {}
    for path, entries in defs.containers().items():
        for action_id in defs.listed_ids(entries, host="blender"):
            out.setdefault(action_id, []).append(path)
    return out


class Recorder(bu.FakeLayout):
    """A FakeLayout recording what a panel or menu draws (operators with their text and properties,
    menus, which must exist), and taking what the Groom Tools use."""

    scale_x = scale_y = 1.0

    def _sub(self, *args, **kwargs):
        return Recorder(self.log)

    row = column = box = split = grid_flow = menu_pie = _sub

    def operator(self, idname, text="", **kwargs):
        bu.FakeLayout.operator(self, idname, **kwargs)

        class Props(object):
            pass
        props = Props()
        self.log.append(("op_text", idname, text, props))
        return props

    def menu(self, idname, text="", **kwargs):
        if not hasattr(bpy.types, idname):
            raise AttributeError("no menu %s" % idname)
        self.log.append(("menu", idname, text))

    def prop_search(self, data, name, search_data, search_name, **kwargs):
        self.prop(data, name)

    def template_list(self, list_type, list_id, data, prop, active_data, active_prop, **kwargs):
        cls = getattr(bpy.types, list_type)
        for i, item in enumerate(getattr(data, prop)):
            cls.draw_item(cls, bpy.context, Recorder(self.log), data, item, 0, active_data, active_prop, i)
        self.log.append(("list", prop))


def drawn(*classes):
    log = []

    class Holder(object):
        def __init__(self, cls):
            self.cls = cls
            self.layout = Recorder(log)
            self.bl_idname = getattr(cls, "bl_idname", cls.__name__)

        def __getattr__(self, name):
            value = getattr(self.cls, name)
            return types.MethodType(value, self) if isinstance(value, types.FunctionType) else value

    for cls in classes:
        if issubclass(cls, bpy.types.Panel) and hasattr(cls, "poll") and not cls.poll(bpy.context):
            continue
        cls.draw(Holder(cls), bpy.context)
    return log


def texts(log, idname=None):
    return [e[2] for e in log if e[0] == "op_text" and (idname is None or e[1] == idname)]


def hcall(scalp, cmd, **args):
    return process.call(cmd, session=groom.runtime(scalp).session, **args)


class DefinitionsTest(unittest.TestCase):
    """No helper needed: the files and the bindings."""

    def test_1_loader(self):
        self.assertFalse(process.running())
        self.assertEqual(defs.HOST, "blender")
        folder = defs.folder()
        self.assertTrue(os.path.isfile(os.path.join(folder, "menus.json")), folder)
        self.assertEqual(os.path.basename(folder), "ui")  # the extension's own copy
        self.assertEqual(defs.label("export.alembic"), "Alembic...")
        self.assertEqual(defs.choices("flowSink")[0]["label"], "Arrows")
        # Host filtering: Maya-only entries never show; needs and enabled conditions; generators.
        ids = [i["id"] for i in defs.resolve(defs.get("menus/import"), state=())]
        self.assertEqual(ids, ["import.alembicCurves"])
        ctx = [i.get("id") for i in defs.resolve(defs.get("lists/maps/context"), state={"map", "map.texture"})]
        self.assertIn("map.reloadImage", ctx)
        self.assertIn("map.deleteFiles", ctx)
        self.assertNotIn("map.makeGray", ctx)  # needs map.scalp and map.color
        ctx = [i.get("id") for i in defs.resolve(defs.get("lists/maps/context"), state=())]
        self.assertNotIn("map.rename", ctx)
        convert = defs.resolve(defs.get("lists/maps/convert"), state=())
        self.assertEqual([i["value"] for i in convert], ["guides", "vertices", "texture"])
        self.assertEqual(convert[2]["label"], "A texture at the scalp's UVs...")
        # Includes, refs and dividers: the Output strip's Studio is the GroomLab menu's.
        self.assertEqual(defs.listed_ids(defs.get("menus/output.studio"), host="maya"),
                         ["studio.preset", "studio.lookThrough", "studio.remove"])
        self.assertEqual(defs.listed_ids(defs.get("menus/output.studio")), [])
        items = defs.resolve(["-", "help.docs", "-", "-", "help.about", "-"], state=())
        self.assertEqual([i["kind"] for i in items], ["action", "divider", "action"])
        # Unbound actions hidden: a submenu left empty goes.
        main = menu.resolve(defs.get("menus/main"), bpy.context)
        labels = [i["label"] for i in main if i["kind"] != "divider"]
        for gone in ("Studio", "Review", "Import", "Theme", "Guides from Photos (experimental)..."):  # (grouped: Render, Start From, Settings)
            self.assertNotIn(gone, labels)
        self.assertEqual(labels, MAIN)
        self.assertEqual(defs.plain("the <b>Group</b> picked"), "the Group picked")
        self.assertEqual(defs.category_colors()["Output"], (245, 176, 66))
        self.assertIn(("Guides", "input"), defs.plumbing())
        self.assertEqual(defs.evaluation_labels(), {"viewportOnly": "Viewport Only", "renderOnly": "Render Only"})

    def test_2_every_id_bound_or_listed(self):
        listed = blender_ids()
        missing = sorted(i for i in listed if not actions.bound(i))
        unexpected = sorted(set(missing) - ALLOWED_MISSING - ALLOWED_MISSING_GRAPH - NOT_IN_BLENDER)
        self.assertEqual(unexpected, [], "ids the definitions show for Blender with no binding in actions.py "
                                         "(bind them, or list them in ALLOWED_MISSING): %s" %
                         ["%s (%s)" % (i, ", ".join(listed[i])) for i in unexpected])
        allowed = ALLOWED_MISSING | ALLOWED_MISSING_GRAPH | NOT_IN_BLENDER
        stale = sorted(i for i in allowed if actions.bound(i) or i not in listed)
        self.assertEqual(stale, [], "listed as missing but bound now, or not shown for Blender: take them off")
        self.assertFalse(ALLOWED_MISSING & ALLOWED_MISSING_GRAPH or ALLOWED_MISSING & NOT_IN_BLENDER)
        # Every binding: an action of the files, an operator (or a setting) that exists.
        every = defs.get("actions")
        t = bpy.context.scene.groomlab_tools
        for action_id, (idname, props) in actions.BINDINGS.items():
            self.assertIn(action_id, every, action_id)
            if idname.startswith("prop:"):
                owner = (bpy.types.Object.bl_rna.properties["groomlab_tools"].fixed_type if idname.startswith("prop:groom.")
                         else t.bl_rna)
                self.assertIn(idname.split(".")[-1], owner.properties, action_id)
                continue
            module, op = idname.split(".")
            self.assertTrue(hasattr(getattr(bpy.ops, module), op), "%s: no operator %s" % (action_id, idname))
            rna = getattr(getattr(bpy.ops, module), op).get_rna_type()
            value_prop = props.get("$value")
            for k in [k for k in props if not k.startswith("$")] + ([value_prop] if value_prop else []):
                self.assertIn(k, rna.properties, "%s: %s has no %s" % (action_id, idname, k))
        print("parity checklist: %d ids hidden in Blender (%d of them the Graph Editor's), %d not Blender's" %
              (len(ALLOWED_MISSING) + len(ALLOWED_MISSING_GRAPH), len(ALLOWED_MISSING_GRAPH), len(NOT_IN_BLENDER)))

    def test_3_menus_from_the_files(self):
        # Every submenu of the files a Blender menu class.
        for name in ("GROOMLAB_MT_main", "GROOMLAB_MT_main_groomfiles", "GROOMLAB_MT_main_add", "GROOMLAB_MT_main_render_review",
                     "GROOMLAB_MT_main_samples", "GROOMLAB_MT_main_settings",
                     "GROOMLAB_MT_main_export", "GROOMLAB_MT_main_help", "GROOMLAB_MT_tools_more", "GROOMLAB_MT_guide_edits",
                     "GROOMLAB_MT_group_context", "GROOMLAB_MT_map_context", "GROOMLAB_MT_maps_mapconvert",
                     "GROOMLAB_MT_children_counts", "GROOMLAB_MT_corrective_context",
                     "GROOMLAB_MT_sculpt_node_context", "GROOMLAB_MT_output_export", "GROOMLAB_MT_output_import",
                     "GROOMLAB_MT_photo_context", "GROOMLAB_MT_pair_context",
                     "GROOMLAB_MT_empty_import"):
            self.assertTrue(hasattr(bpy.types, name), name)
        log = drawn(bpy.types.GROOMLAB_MT_main)
        self.assertEqual([e[2] for e in log if e[0] in ("op_text", "menu")], MAIN)
        self.assertEqual(texts(drawn(bpy.types.GROOMLAB_MT_main_groomfiles)),
                         ["Save to Groom File", "Save Groom File As...", "Import Groom File...", "Reload from Groom File"])
        self.assertEqual(texts(drawn(bpy.types.GROOMLAB_MT_main_export)),
                         ["Alembic...", "USD...", "V-Ray Scene...", "Unreal Groom...", "Unreal Cards...", "Unity Strands (.abc)...",
                          "Unity Feathers...", "Unity Cards...", "glTF Cards...", "Card Maps...", "Scales Mesh (FBX)...",
                          "Scales Instancer (USD)..."])
        add = drawn(bpy.types.GROOMLAB_MT_main_add)
        self.assertEqual(texts(add), ["Feathers...", "Braid on Curves...", "Bun...", "Collider", "Scalp", "Wind", "Jiggle"])
        self.assertIn(("menu", "GROOMLAB_MT_main_add_cards", "Cards"), add)
        cards = drawn(bpy.types.GROOMLAB_MT_main_add_cards)
        self.assertEqual(texts(cards), ["Hero", "Mid", "Low"])
        self.assertEqual([e[3].lod for e in cards if e[0] == "op_text"], ["hero", "mid", "low"])
        # Start From: the sample meshes, then the imports (Blender's: Alembic curves).
        samples = drawn(bpy.types.GROOMLAB_MT_main_samples)
        self.assertEqual(texts(samples), ["Nika (head)", "Manny (head)", "Wolf", "Buddy (rig)", "Alembic Curves..."])
        self.assertEqual([e[3].sample for e in samples if e[0] == "op_text" and e[1] == "groomlab.add_sample"],
                         ["nika", "manny", "wolf", "buddy"])
        sim = drawn(bpy.types.GROOMLAB_MT_main_simulate)
        self.assertEqual(texts(sim), ["Add Simulate Node", "Simulate Range", "Bake Simulation", "Clear Simulation",
                                      "Export Guides for Simulation...", "Load Guide Cache..."])
        self.assertEqual([e[3].bake for e in sim if e[0] == "op_text" and e[1] == "groomlab.simulate_range"],
                         [False, True])
        self.assertEqual(actions.action_for("groomlab.simulate_range", {"bake": True}), "sim.bake")
        self.assertEqual(actions.action_for("groomlab.simulate_range", {"bake": False}), "sim.range")
        review = drawn(bpy.types.GROOMLAB_MT_main_render_review)  # (Review: under Render)
        self.assertEqual(texts(review), ["Density Heatmap", "Turntable"])
        self.assertIn(("prop", "show_hud"), review)  # Strand Count HUD: a checkbox (checkable)
        self.assertEqual(texts(drawn(bpy.types.GROOMLAB_MT_main_utilities)),
                         ["Guides to Curves", "Curves to Guides...", "Strands to Curves...",  # (Blender's curve objects)
                          "To Mesh...", "Bake Node to Guides...", "Bake Scale Maps", "Bake Follicle Map", "Delete All Flow Arrows",
                          "Transfer Groom to Scalp...",
                          "Copy Graph to Other Grooms...", "Find Missing Files...", "Collect Files"])
        help_ = drawn(bpy.types.GROOMLAB_MT_main_help)
        self.assertEqual(texts(help_), ["Docs", "Keyboard Shortcuts", "Check for Updates...", "Discord",
                                        "Support GroomLab", "About GroomLab"])
        donate = next(e for e in help_ if e[0] == "op_text" and e[2] == "Support GroomLab")
        self.assertEqual(donate[3].url, actions.DONATE_URL)
        self.assertEqual(texts(drawn(bpy.types.GROOMLAB_MT_output_export)),
                         ["Alembic...", "USD...", "Unreal Groom...", "Unreal Cards...", "Unity Strands (.abc)...",
                          "Unity Cards...", "glTF Cards...", "Card Maps...", "Save Groom File As..."])
        self.assertEqual(texts(drawn(bpy.types.GROOMLAB_MT_output_import)), ["Alembic Curves...", "Groom File..."])
        self.assertFalse(hasattr(bpy.types, "GROOMLAB_MT_main_import"))  # (included in Start From)
        # The Photos page's row menu: its actions need a photo picked (Import is the list's header).
        self.assertEqual(texts(drawn(bpy.types.GROOMLAB_MT_photo_context)), [])
        edits = drawn(bpy.types.GROOMLAB_MT_guide_edits)
        # (Mirror and Move to Group are submenus; Paste Guides needs a group picked.)
        self.assertEqual(texts(edits)[:6], ["Children", "Reparent...", "Copy Guides", "From Curves", "Clear Selection",
                                            "Lock"])  # From Curves: Blender's curve objects
        # A group row's menu without a group: Edit Guides' entries (Generate Children a submenu of
        # counts) and Promote / Demote (its dialog picks the group); New Group is the list's header.
        groups = drawn(bpy.types.GROOMLAB_MT_group_context)
        self.assertEqual(texts(groups), ["Reparent...", "Copy Guides", "From Curves", "Clear Selection", "Promote / Demote..."])
        self.assertEqual([e[2] for e in groups if e[0] == "menu"], ["Generate Children", "Mirror", "Move to Group"])
        children = drawn(bpy.types.GROOMLAB_MT_children_counts)
        self.assertEqual(texts(children), ["1 per guide", "2 per guide", "3 per guide", "4 per guide", "6 per guide",
                                           "8 per guide"])
        self.assertEqual([(e[3].action, e[3].count) for e in children if e[0] == "op_text"][-1], ("CHILDREN", 8))
        # Deletes are marked (drawn in the error red, as Maya's and the app's).
        for action_id in ("group.delete", "map.delete", "corrective.delete", "photo.remove"):
            self.assertTrue(defs.action(action_id).get("destructive"), action_id)
        # Every Blender icon the Groom Tools name is one this Blender has (a wrong name fails the
        # whole panel), and the window draws each it maps.
        import re
        known = set(bpy.types.UILayout.bl_rna.functions["operator"].parameters["icon"].enum_items.keys())
        drawn_mod = importlib.import_module(bu.MODULE + ".drawn")
        for module in ("palette", "photos", "menu", "find_command", "ui"):
            path = importlib.import_module(bu.MODULE + "." + module).__file__
            with open(path, encoding="utf-8") as f:
                for icon in set(re.findall(r'icon="([A-Z_0-9]+)"', f.read())) - {"NONE"}:
                    self.assertIn(icon, known, "%s: %s" % (module, icon))
        for icon in drawn_mod.ICONS:
            self.assertIn(icon, known, "drawn.ICONS: %s" % icon)
        # Every operator's tooltip from actions.json.
        self.assertEqual(bpy.types.GROOMLAB_OT_guides_edit.description(bpy.context, types.SimpleNamespace(action="MIRROR_SELECTION")),
                         defs.tip("guides.mirrorSelection"))
        self.assertEqual(actions.tip_for("groomlab.corrective_new", {"fix": True}), defs.tip("corrective.fixFrame"))
        self.assertEqual(actions.action_for("groomlab.group_edit", {"action": "DEFAULT_COLOR"}), "group.defaultColor")
        # The pie: marking_menu.json's radial in Blender's pie order, its tail.
        self.assertEqual(tool_ops.RADIAL[0], ("N", "comb"))
        self.assertEqual(tool_ops.tail(), [("tool", "select"), ("tool", "freeze"), ("action", "tools.open")])
        # Option lists from choices.json.
        t = bpy.context.scene.groomlab_tools
        rna = t.bl_rna.properties
        self.assertEqual([i.identifier for i in rna["flow_sink_item"].enum_items],
                         ["ARROWS", "whorl", "whorlcw", "converge", "part"])
        self.assertEqual([i.identifier for i in rna["symmetry_mode"].enum_items], ["x", "topology"])
        self.assertEqual([i.name for i in rna["tube_channel"].enum_items], ["Width", "Flatness", "Twist"])
        self.assertEqual([i.identifier for i in rna["tab"].enum_items], ["TOOLS", "PAINT", "PHOTOS"])  # (Correctives: a Tools section)
        self.assertEqual(tool_ops.FOR_TIP, defs.get("lists/maps/forTip"))


class ToolsTest(unittest.TestCase):
    """With a groom: the Groom Tools' operators over the helper."""

    @classmethod
    def setUpClass(cls):
        for o in list(bpy.data.objects):
            bpy.data.objects.remove(o)
        cls.head = bu.head_object()
        assert bpy.ops.groomlab.add_groom("EXEC_DEFAULT", preset="hair_basics", guides=40) == {"FINISHED"}
        tools.catalog()
        tools.refresh_info(cls.head)

    def setUp(self):
        self.t = bpy.context.scene.groomlab_tools
        self.t.tab = "TOOLS"

    def guide_ids(self):
        g = hcall(self.head, "get_guides", source="hierarchy")
        return [int(i) for i in process.client().array(g["arrays"]["ids"]).tolist()]

    def test_1_groups_colors_segments_edits(self):
        head, t = self.head, self.t
        self.assertEqual(head.groomlab.guide_target, "hierarchy")
        self.assertIn("primary", [g.name for g in t.groups])
        t.group = "primary"
        self.assertEqual(bpy.ops.groomlab.group_color(group="primary", color=(1.0, 0.0, 0.0)), {"FINISHED"})
        g = next(g for g in t.groups if g.name == "primary")
        self.assertTrue(g.has_color)
        self.assertAlmostEqual(g.color[0], 1.0, places=2)
        self.assertIn("group.color", menu.state(bpy.context))
        # A group row's menu (lists.json's groups context): Rename first, Edit Guides' entries, Delete last.
        log = drawn(bpy.types.GROOMLAB_MT_group_context)
        self.assertEqual(texts(log), ["Rename", "Select Guides", "Reparent...", "Copy Guides", "Paste Guides", "From Curves",
                                      "Clear Selection", "Color...", "Default Color", "Reset Segments",
                                      "Promote / Demote...", "Paste Guides Here", "Delete"])
        self.assertEqual(bpy.ops.groomlab.group_edit(action="DEFAULT_COLOR", group="primary"), {"FINISHED"})
        g = next(g for g in t.groups if g.name == "primary")
        self.assertFalse(g.has_color)
        # The swatch shows the color it draws in: its automatic hue (saturated), not the red it had.
        auto = tools.call(head, "guides_info", guideColorSeed=head.name)["groups"]
        want = tools._rgb(next(x for x in auto if x["name"] == "primary")["autoColor"])
        self.assertEqual([round(c, 3) for c in g.color], [round(c, 3) for c in want])
        self.assertGreater(max(g.color) - min(g.color), 0.2)
        self.assertNotAlmostEqual(g.color[0], 1.0, places=2)
        # Segments, nothing selected: the picked group's guides resampled (headless: committed at
        # once), its new guides getting the count too; one undo step.
        before = set(tools.guide_segments(head)["segments"])
        t.resample_segments = 5
        self.assertEqual(set(tools.guide_segments(head)["segments"]), {5})
        self.assertIn("primary: 40", t.segments_scope)
        info = tools.call(head, "guides_info", guideColorSeed=head.name)["groups"]
        self.assertEqual(next(x for x in info if x["name"] == "primary")["segments"], 5)
        self.assertEqual(bpy.ops.groomlab.tool_undo(), {"FINISHED"})
        self.assertEqual(set(tools.guide_segments(head)["segments"]), before)
        # Segments by Length (issue #98): toggled on, each guide's count its length's share (one
        # undo step), the group keeping the spacing; the switch follows the picked group.
        t.segments_by_length = True
        info = tools.call(head, "guides_info", guideColorSeed=head.name)["groups"]
        self.assertGreater(next(x for x in info if x["name"] == "primary")["segmentLength"], 0.0)
        self.assertTrue(t.segments_by_length)
        self.assertEqual(bpy.ops.groomlab.tool_undo(), {"FINISHED"})
        self.assertEqual(set(tools.guide_segments(head)["segments"]), before)
        # Select Mirror Guides: the mirror images of the selected guides.
        ids = self.guide_ids()
        tools.selection_set(head, ids=ids[:5], mode="replace")
        self.assertEqual(bpy.ops.groomlab.guides_edit(action="MIRROR_SELECTION"), {"FINISHED"})
        self.assertGreater(t.selected_count, 0)
        # Children, then Reparent one child under another primary.
        tools.selection_set(head, ids=ids[:2], mode="replace")
        t.children_count = 2
        self.assertEqual(bpy.ops.groomlab.guides_edit(action="CHILDREN"), {"FINISHED"})
        info = hcall(head, "guides_info")
        self.assertGreater(info["guides"], 40)
        child_group = next(g.name for g in t.groups if g.parent == "primary")
        children = [int(i) for i in tools.guide_segments(head, group=child_group)["ids"]]
        self.assertEqual(len(children), 4)
        t.group = tools.ALL_GROUPS  # every level selectable
        tools.selection_set(head, ids=[], mode="replace")
        with self.assertRaises(RuntimeError):  # nothing selected: said, nothing done
            bpy.ops.groomlab.guides_reparent(parent=ids[3])
        tools.selection_set(head, ids=children[:1], mode="replace")
        self.assertEqual(tools.selected_guides(head), children[:1])
        depth = tools.groom_tools(head).undo.get("undoDepth")
        self.assertEqual(bpy.ops.groomlab.guides_reparent(parent=ids[3]), {"FINISHED"})
        self.assertEqual(tools.groom_tools(head).undo.get("undoDepth"), depth + 1)
        # Issue #94: a child into a new group beside its own, back by Move to Group (the menu
        # lists the groups beside), duplicated, copied and pasted into the picked group.
        tools.selection_set(head, ids=children[:1], mode="replace")
        beside = tools.guides_groups_beside(head, children[:1])
        self.assertIn(child_group, beside)
        self.assertEqual(bpy.ops.groomlab.guides_regroup(action="NEW_GROUP"), {"FINISHED"})
        made = [g.name for g in t.groups if g.parent == "primary" and g.name != child_group]
        self.assertEqual(len(made), 1)
        self.assertEqual(bpy.ops.groomlab.guides_regroup(action="MOVE", group=child_group), {"FINISHED"})
        self.assertIn(children[0], [int(i) for i in tools.guide_segments(head, group=child_group)["ids"]])
        self.assertEqual(bpy.ops.groomlab.guides_regroup(action="DUPLICATE", group=made[0]), {"FINISHED"})
        self.assertEqual(len(tools.guide_segments(head, group=made[0])["ids"]), 1)
        self.assertEqual(bpy.ops.groomlab.guides_regroup(action="COPY"), {"FINISHED"})
        # (A headless Blender keeps no clipboard: the copied text pasted as Paste Guides pastes it.)
        text = tools.guides_copy(head, children[:1])
        self.assertIn("groomlab.guides", text)
        tools.guides_edit(head, "paste_guides", text=text, group=made[0])
        self.assertEqual(len(tools.guide_segments(head, group=made[0])["ids"]), 2)
        t.group = tools.ALL_GROUPS
        for _ in range(4):
            bpy.ops.groomlab.tool_undo()
        bpy.ops.groomlab.tool_undo()
        bpy.ops.groomlab.tool_undo()  # the children
        # Promote / Demote and Mirror Group (dialogs; run here with their values).
        self.assertEqual(bpy.ops.groomlab.group_new(name="extra"), {"FINISHED"})
        self.assertEqual(bpy.ops.groomlab.group_promote_demote(action="DEMOTE", group="extra", parent="primary"),
                         {"FINISHED"})
        self.assertEqual(next(g for g in t.groups if g.name == "extra").parent, "primary")
        self.assertEqual(bpy.ops.groomlab.group_promote_demote(action="PROMOTE", group="extra"), {"FINISHED"})
        self.assertEqual(next(g for g in t.groups if g.name == "extra").parent, "")
        self.assertEqual(bpy.ops.groomlab.guides_mirror_group(group="primary", direction="POS"), {"FINISHED"})
        # Copy Shape to Other Side (issue 74): the picked guide's mirror guide reshaped in place
        # (the IDs kept), one undo step.
        primaries = sorted(int(i) for i in tools.guide_segments(head, group="primary")["ids"])
        tools.selection_set(head, ids=primaries[:1], mode="replace")
        depth = tools.groom_tools(head).undo.get("undoDepth")
        self.assertEqual(bpy.ops.groomlab.guides_edit(action="MIRROR_TO_OTHER"), {"FINISHED"})
        self.assertEqual(tools.groom_tools(head).undo.get("undoDepth"), depth + 1)
        self.assertEqual(sorted(int(i) for i in tools.guide_segments(head, group="primary")["ids"]), primaries)
        tools.selection_set(head, ids=[], mode="replace")
        t.group = "primary"

    def test_2_parts_and_flow(self):
        head, t = self.head, self.t
        bpy.ops.groomlab.tool(tool="part")
        points = bu.th.to_blender(bu.th.crown_points(4)) if hasattr(bu, "th") else None
        import test_helper as th
        points = th.to_blender(th.crown_points(4))
        tools.part_add(head, points, name="part")
        tools.refresh_info(head)
        self.assertEqual([p.name for p in t.parts], ["part"])
        log = drawn(palette.VIEW3D_PT_groomlab_tools)
        self.assertIn(("list", "parts"), log)
        self.assertEqual(texts(log, "groomlab.part_remove") + texts(log, "groomlab.parts_clear"), ["Remove", "Clear All"])
        tools.part_add(head, points, name="part")
        tools.refresh_info(head)
        self.assertEqual(len(t.parts), 2)
        t.part_index = 0
        self.assertEqual(bpy.ops.groomlab.part_remove(), {"FINISHED"})
        self.assertEqual(len(t.parts), 1)
        self.assertEqual(bpy.ops.groomlab.parts_clear(), {"FINISHED"})
        self.assertEqual(len(t.parts), 0)
        bpy.ops.groomlab.tool(tool="flow")
        # The Flow card: how many arrows the groom keeps, beside Clear All (Delete All Flow Arrows).
        hcall(head, "flow_clear")
        tools.refresh_info(head)
        self.assertEqual((t.flow_arrows, t.flow_sinks), (0, 0))
        log = drawn(palette.VIEW3D_PT_groomlab_tools)
        self.assertIn("Clear All", texts(log, "groomlab.flow_clear"))
        self.assertIn(("label", "No flow arrows yet"), [e[:2] for e in log])
        self.assertEqual(palette.flow_count_text(34, 2), "34 arrows \u00b7 2 sinks")
        self.assertEqual(palette.flow_count_text(1, 1), "1 arrow \u00b7 1 sink")
        self.assertEqual(bpy.ops.groomlab.flow_clear(), {"FINISHED"})
        # Mirror to Other Side (issue 105): the Flow block's two buttons.
        self.assertEqual(texts(log, "groomlab.overlay_mirror"), ["Mirror -X to +X", "Mirror +X to -X"])
        # Only the blocks the tool uses: Flow draws no tubes, and Mirror (it mirrors, issue 105); the
        # guide groups over the card; While sculpting with the brushes that stroke.
        self.assertNotIn(("prop", "tube_shape"), log)
        self.assertIn(("prop", "symmetry"), log)
        self.assertIn(("prop", "sculpt_display"), log)  # Display while sculpting: under the Correctives
        for gone in ("VIEW3D_PT_groomlab_tools_tubes", "VIEW3D_PT_groomlab_tools_output",
                     "VIEW3D_PT_groomlab_tools_sculpting"):
            self.assertFalse(hasattr(palette, gone))  # their items: the cards, the GroomLab menu, the Correctives
        bpy.ops.groomlab.tool(tool="comb")
        log = drawn(palette.VIEW3D_PT_groomlab_tools)
        self.assertNotIn(("prop", "tube_shape"), log)
        # The toggles row: Mirror first, Pressure, Keep off scalp, Comb's Stretch; Mirror across in More.
        props = [e[1] for e in log if e[0] == "prop"]
        toggles = [p for p in props if p in ("symmetry", "pressure", "collide", "stretch", "straighten")]
        self.assertEqual(toggles, ["symmetry", "pressure", "collide", "stretch"])
        self.assertLess(props.index("stretch"), props.index("symmetry_mode"))

    def test_3_maps(self):
        head, t = self.head, self.t
        t.tab = "PAINT"
        self.assertEqual(bpy.ops.groomlab.map_new(name="mask", kind="vertices", value=0.0), {"FINISHED"})
        log = drawn(palette.VIEW3D_PT_groomlab_tools)
        self.assertIn(("list", "maps"), log)
        bar = texts(log, "groomlab.map_new") + texts(log, "groomlab.map_import")
        self.assertEqual(bar, ["", ""])  # icons in the Maps header
        # One line per map: its info (a popover) and its menu, the row picked first.
        rows = [e[3] for e in log if e[0] == "op_text" and e[1] == "groomlab.row_popup"]
        self.assertEqual([(r.rows, r.panel, r.menu) for r in rows],
                         [("MAPS", "GROOMLAB_PT_map_info", ""), ("MAPS", "", "GROOMLAB_MT_map_context")])
        info = drawn(palette.GROOMLAB_PT_map_info)
        self.assertTrue(any(e[0] == "label" and e[1].startswith("Read by nothing") for e in info))
        for name in ("show_on_guides", "show_on_strands", "show_on_scalp"):
            self.assertIn(("prop", name), info)
        self.assertIn("groomlab.map_show_reader", [e[1] for e in info if e[0] == "op_text"])
        self.assertIn(("prop", "paint_mode"), log)  # the Paint card's own block: Mode, Value and Fill
        # The Paint card heads the page (what a stroke paints, the brush under it), the maps under it.
        self.assertLess(log.index(("prop", "paint_mode")), log.index(("prop", "radius")))
        self.assertLess(log.index(("prop", "radius")), log.index(("list", "maps")))
        self.assertIn(("prop", "paint_whole_faces"), log)  # (a toggle of the row)
        self.assertIn("Fill", texts(log, "groomlab.map_fill"))
        ctx = drawn(bpy.types.GROOMLAB_MT_map_context)
        self.assertEqual(texts(ctx), ["Rename...", "Make Color (r g b a)", "Relink Image...", "Delete"])
        self.assertIn(("menu", "GROOMLAB_MT_maps_mapconvert", "Convert to"), ctx)
        # Rename (what reads it follows), the read-by line.
        self.assertEqual(bpy.ops.groomlab.map_rename("EXEC_DEFAULT", name="tint"), {"FINISHED"})
        self.assertEqual(t.paint_map, "tint")
        self.assertIn("tint", [mm["name"] for mm in hcall(head, "maps_list")["maps"]])
        # Gray / color, fill (a color map: the color in the channels ticked).
        self.assertEqual(bpy.ops.groomlab.map_set_channels(channels=4), {"FINISHED"})
        self.assertEqual(tools.current_map(bpy.context.scene).channels, 4)
        self.assertIn("map.color", menu.state(bpy.context))
        self.assertIn("Make Gray (its luminance)", texts(drawn(bpy.types.GROOMLAB_MT_map_context)))
        t.paint_color = (1.0, 0.0, 0.0, 1.0)
        self.assertEqual(bpy.ops.groomlab.map_fill(), {"FINISHED"})
        # Convert to a texture (its file in the maps folder), reload, then delete it with its file.
        self.assertEqual(bpy.ops.groomlab.map_convert("EXEC_DEFAULT", kind="texture", resolution="512"), {"FINISHED"})
        m = tools.current_map(bpy.context.scene)
        self.assertEqual(m.kind, "texture")
        self.assertIn("map.texture", menu.state(bpy.context))
        log = drawn(bpy.types.GROOMLAB_MT_map_context)
        for item in ("Reload Image", "Show in Explorer"):
            self.assertIn(item, texts(log))
        self.assertIn(("prop", "map_delete_files"), log)  # Delete Its File Too
        self.assertEqual(bpy.ops.groomlab.map_reload(), {"FINISHED"})
        texture = next(mm for mm in hcall(head, "maps_list")["maps"] if mm["name"] == "tint").get("texture")
        self.assertTrue(texture)
        # Gray again, a guide layer filled on every guide.
        self.assertEqual(bpy.ops.groomlab.map_convert("EXEC_DEFAULT", kind="guides"), {"FINISHED"})
        self.assertEqual(tools.current_map(bpy.context.scene).kind, "guides")
        t.value = 0.5
        self.assertEqual(bpy.ops.groomlab.map_fill(), {"FINISHED"})
        self.assertIn("tint", json.dumps(json.loads(hcall(head, "get_state", texts=True, textures=False)["texts"]
                                                     ["guideData"])))
        self.assertTrue(t.map_delete_files)  # Delete Its File Too: on by default
        self.assertEqual(bpy.ops.groomlab.map_remove(), {"FINISHED"})
        self.assertNotIn("tint", [mm.name for mm in t.maps])
        # Import an image as a map, relink it to another, delete it with its file.
        img = bpy.data.images.new("spot", 8, 8)
        img.pixels = [0.5] * (8 * 8 * 4)
        a = os.path.join(bu.TMP, "spot.png")
        img.filepath_raw = a
        img.file_format = "PNG"
        img.save()
        b = os.path.join(bu.TMP, "spot_b.png")
        img.filepath_raw = b
        img.save()
        self.assertEqual(bpy.ops.groomlab.map_import(filepath=a), {"FINISHED"})
        self.assertEqual(t.paint_map, "spot")
        self.assertEqual(tools.current_map(bpy.context.scene).kind, "texture")
        self.assertEqual(bpy.ops.groomlab.map_relink(filepath=b), {"FINISHED"})
        self.assertTrue(tools.current_map(bpy.context.scene).texture.endswith("spot_b.png"))
        self.assertEqual(bpy.ops.groomlab.map_remove(), {"FINISHED"})
        self.assertTrue(os.path.isfile(b))  # a PNG of yours linked in place stays
        t.tab = "TOOLS"

    def test_3b_paint_alpha(self):
        # Paint on a scalp map shows the Alpha grid (the library: the shipped alphas), not on a
        # guide map; a pick reaches the helper's tool with its rotation and spacing; Add copies an
        # image into the user's folder (GROOMLAB_USER_ALPHAS: the test's own).
        alphas = importlib.import_module(bu.MODULE + ".alphas")
        head, t = self.head, self.t
        t.tab = "PAINT"
        self.assertEqual(bpy.ops.groomlab.map_new(name="speckles", kind="vertices", value=0.0), {"FINISHED"})
        self.assertEqual(bpy.ops.groomlab.tool(tool="paint"), {"FINISHED"})
        log = drawn(palette.VIEW3D_PT_groomlab_tools)
        self.assertIn(("icon_view", "alpha_choice"), log)
        self.assertNotIn(("prop", "alpha_spacing"), log)  # its rows only while one is picked
        library = alphas.library(refresh=True)
        names = [a["name"] for a in library]
        for name in ("Speckle", "Spots", "Streaks"):
            self.assertIn(name, names)
        items = alphas.enum_items(t, bpy.context)
        self.assertEqual(items[0][0], "NONE")
        t.alpha_choice = next(i[0] for i in items if i[1] == "Spots")
        spots = next(a["path"] for a in library if a["name"] == "Spots")
        self.assertEqual(os.path.normcase(t.alpha), os.path.normcase(spots))
        t.alpha_rotation = "STROKE"
        t.alpha_spacing = 60
        tool = hcall(head, "tool_get")["settings"]
        self.assertEqual(os.path.normcase(tool["alpha"]), os.path.normcase(spots))
        self.assertEqual(tool["alphaRotation"], "stroke")
        self.assertAlmostEqual(tool["alphaSpacing"], 0.6, places=5)
        log = drawn(palette.VIEW3D_PT_groomlab_tools)
        for name in ("alpha_rotation", "alpha_angle", "alpha_spacing", "alpha_invert"):
            self.assertIn(("prop", name), log)
        self.assertIn("groomlab.alpha_clear", [e[1] for e in log if e[0] == "op_text"])
        # Add: a copy in the user's folder, picked.
        user = os.environ["GROOMLAB_USER_ALPHAS"]
        outside = os.path.join(bu.TMP, "alpha_src", "Spots.png")
        os.makedirs(os.path.dirname(outside), exist_ok=True)
        shutil.copyfile(spots, outside)
        self.assertEqual(bpy.ops.groomlab.alpha_add(filepath=outside), {"FINISHED"})
        self.assertEqual(os.path.normcase(os.path.normpath(os.path.dirname(t.alpha))), os.path.normcase(os.path.normpath(user)))
        self.assertEqual(os.path.basename(t.alpha), "Spots 2.png")
        self.assertEqual(alphas.library(refresh=True)[0]["name"], "Spots 2")
        # A guide map: no alpha rows.
        self.assertEqual(bpy.ops.groomlab.map_new(name="layer", kind="guides", value=0.0), {"FINISHED"})
        self.assertNotIn(("icon_view", "alpha_choice"), drawn(palette.VIEW3D_PT_groomlab_tools))
        self.assertEqual(bpy.ops.groomlab.alpha_clear(), {"FINISHED"})
        self.assertEqual(hcall(head, "tool_get")["settings"]["alpha"], "")
        bpy.ops.groomlab.tool_exit()
        t.tab = "TOOLS"

    def test_4_tubes(self):
        head, t = self.head, self.t
        bpy.ops.groomlab.tool(tool="select")  # Select's card: the selection only (the tubes: Tube's)
        self.assertNotIn(("prop", "tube_shape"), drawn(palette.VIEW3D_PT_groomlab_tools))
        bpy.ops.groomlab.tool(tool="shape")  # Tube's card (Maya's Simple): no jobs; the Profile always shown
        log = drawn(palette.VIEW3D_PT_groomlab_tools)
        self.assertNotIn("tube_job", t.bl_rna.properties)
        self.assertIn(("prop", "tube_channel"), log)  # what it paints
        self.assertIn(("prop", "tube_profile"), log)  # Root, Mid, Tip
        self.assertIn(("prop", "tube_shape"), log)  # (the root's picker: Root is shown first)
        self.assertNotIn(("prop", "tube_tip_shape"), log)
        self.assertIn(("menu", "GROOMLAB_MT_tube_levels", ""), log)  # the levels' row
        self.assertIn(("menu", "GROOMLAB_MT_tube_library", ""), log)  # the library's ⋯
        self.assertNotIn("Draw root", texts(log))  # (the Roots brush's)
        self.assertLess(log.index(("prop", "tube_channel")), log.index(("prop", "tube_profile")))
        t.tube_profile = "TIP"
        tip = drawn(palette.VIEW3D_PT_groomlab_tools)
        self.assertIn(("prop", "tube_tip_shape"), tip)
        self.assertIn(("prop", "tube_tip_hold"), tip)
        self.assertNotIn(("prop", "tube_shape"), tip)
        t.tube_profile = "ROOT"
        # The Roots brush: its own tile and card (picking it starts Draw root), the levels' row too;
        # another brush leaves root mode.
        self.assertIn("roots", [x["id"] for x in tools.catalog()["tools"]])
        self.assertEqual(bpy.ops.groomlab.tool(tool="roots"), {"FINISHED"})
        self.assertEqual((t.tool, t.root_mode), ("roots", "place"))
        self.assertEqual(hcall(head, "tool_get")["tool"], "roots")
        roots = drawn(palette.VIEW3D_PT_groomlab_tools)
        self.assertEqual(texts(roots, "groomlab.root_tube"), ["Draw root", "Edit roots", "Select root", "Undo CV",
                                                              "Delete", "Duplicate", "Subdivide", "", "Apply"])
        self.assertNotIn(("prop", "tube_shape"), roots)
        self.assertIn(("menu", "GROOMLAB_MT_tube_levels", ""), roots)
        self.assertIn(("prop", "root_snap"), roots)
        self.assertIn(("prop", "root_taper"), roots)
        for name in ("roots", "roots_main", "roots_ink"):  # (its own design, two tones)
            self.assertIn(name, palette.icons.names())
        bpy.ops.groomlab.tool(tool="shape")
        self.assertEqual((t.tool, t.root_mode), ("shape", ""))
        self.assertEqual(hcall(head, "tool_get")["tool"], "shape")
        # No targets: the shape and values go to the guides Place and Draw make (the placement tube).
        t.tube_shape = "ribbon"
        bpy.ops.groomlab.tool(tool="place")
        got = hcall(head, "tool_get")["settings"]
        self.assertEqual(got["tube"]["shape"], "ribbon")
        # The Shape brush paints the channel picked.
        bpy.ops.groomlab.tool(tool="shape")
        t.tube_channel = "twist"
        self.assertEqual(hcall(head, "tool_get")["settings"]["tubeChannel"], "twist")
        t.tube_channel = "width"
        # Targets: the selected guides.
        ids = self.guide_ids()[:3]
        tools.selection_set(head, ids=ids, mode="replace")
        self.assertEqual(t.tube_targets, 3)
        self.assertEqual(t.tube_source, "selection")
        t.tube_shape = "teardrop"
        self.assertEqual(tools.get_tube(head, ids[0])["shape"], "teardrop")
        width = max(tools.get_tube(head, ids[0])["width"])
        t.tube_width = width * 2.0
        self.assertAlmostEqual(max(tools.get_tube(head, ids[0])["width"]), width * 2.0, places=6)
        t.tube_flatness = 0.5
        aspect = tools.get_tube(head, ids[0])["aspect"]
        self.assertAlmostEqual(sum(aspect) / len(aspect), 0.5, places=4)
        t.tube_twist = 0.5
        twist = tools.get_tube(head, ids[1])["twist"]
        self.assertAlmostEqual(sum(twist) / len(twist), 0.5, places=4)
        self.assertEqual(bpy.ops.groomlab.tube_smooth(), {"FINISHED"})
        # Mid and tip profiles (tube.h; Maya's rows): picked, they go to the targets; each Length
        # its share of the tube; unset again; read back with the targets.
        t.tube_tip_shape = "ribbon"
        self.assertEqual(tools.get_tube(head, ids[0]).get("tipShape"), "ribbon")
        t.tube_mid_shape = "blade"
        self.assertEqual(tools.get_tube(head, ids[1]).get("midShape"), "blade")
        tools.set_tube(head, guides=ids, tip_hold=0.25)
        tools.refresh_tubes(head)
        self.assertAlmostEqual(t.tube_tip_hold, 25.0, places=3)
        self.assertEqual(t.tube_tip_choice, "ribbon")
        self.assertEqual(bpy.ops.groomlab.tube_apply(profile="MID"), {"FINISHED"})
        tools.set_tube(head, guides=[ids[2]], tip_shape="teardrop")
        tools.refresh_tubes(head)
        self.assertTrue(t.tube_tip_mixed)
        tools.set_tube(head, guides=ids, mid_shape="", tip_shape="")
        self.assertNotIn("tipShape", tools.get_tube(head, ids[0]))
        tools.refresh_tubes(head)
        self.assertEqual(t.tube_tip_shape, tool_props.UNSET)
        t.tube_profile = "MID"
        log = drawn(palette.VIEW3D_PT_groomlab_tools)
        self.assertIn(("prop", "tube_mid_shape"), log)
        self.assertIn(("prop", "tube_mid_hold"), log)
        t.tube_profile = "ROOT"
        self.assertNotIn(("prop", "tube_shaded"), log)  # the tubes' display: the header's guides popover
        display = drawn(palette.GROOMLAB_PT_guide_display)
        self.assertIn(("prop", "tube_shaded"), display)  # the brush's tubes shaded, and as wires
        self.assertIn(("prop", "tube_wireframe"), display)
        # The library: Edit as Custom, New, Duplicate, Rename, Apply, Delete.
        self.assertEqual(bpy.ops.groomlab.tube_shape_custom(), {"FINISHED"})
        self.assertEqual([s.name for s in t.tube_shapes], ["teardrop 1"])
        self.assertEqual(tools.get_tube(head, ids[0])["shape"], "teardrop 1")
        self.assertEqual(t.tube_shape, "teardrop 1")
        self.assertEqual(bpy.ops.groomlab.tube_shape_new(), {"FINISHED"})
        self.assertEqual(bpy.ops.groomlab.tube_shape_new(duplicate=True), {"FINISHED"})
        self.assertEqual(sorted(s.name for s in t.tube_shapes), ["shape 1", "shape 1 1", "teardrop 1"])
        t.tube_shape_index = [s.name for s in t.tube_shapes].index("teardrop 1")
        self.assertEqual(bpy.ops.groomlab.tube_shape_rename("EXEC_DEFAULT", name="lock"), {"FINISHED"})
        self.assertEqual(tools.get_tube(head, ids[0])["shape"], "lock")
        self.assertEqual(t.tube_choice, "lock")
        t.tube_shape = "shape 1"
        self.assertEqual(tools.get_tube(head, ids[0])["shape"], "shape 1")
        t.tube_shape_index = [s.name for s in t.tube_shapes].index("shape 1")
        self.assertEqual(bpy.ops.groomlab.tube_shape_remove("EXEC_DEFAULT"), {"FINISHED"})
        self.assertEqual(tools.get_tube(head, ids[0])["shape"], "round")
        self.assertNotIn("shape 1", [s.name for s in t.tube_shapes])
        # Fill Tubes: the Interpolate nodes'.
        self.assertTrue(t.tube_has_fill)
        t.tube_fill = True
        node = tools._interpolate_nodes(head)[0]
        self.assertTrue(hcall(head, "get_param", node=node, param="fillTubes")["value"])
        t.tube_fill = False
        log = drawn(palette.VIEW3D_PT_groomlab_tools)
        self.assertIn(("menu", "GROOMLAB_MT_tube_library", ""), log)
        self.assertIn("Apply", texts(log, "groomlab.tube_apply"))
        self.assertIn("Smooth", texts(log, "groomlab.tube_smooth"))  # (on Selected tubes' header)
        self.assertNotIn(("prop", "tube_fill"), log)  # the Interpolate node's settings have it
        # A library shape in the editor: its Mirror and Smooth pills; Smooth goes to the shape.
        t.tube_shape = "lock"
        self.assertEqual(t.tube_shapes[t.tube_shape_index].name, "lock")  # (the library row follows the profile)
        log = drawn(palette.VIEW3D_PT_groomlab_tools)
        self.assertIn(("prop", "tube_shape_smooth"), log)
        t.tube_shape_smooth = False
        self.assertFalse(next(s for s in tools.tube_shapes(head) if s["name"] == "lock")["smooth"])
        t.tube_shape_smooth = True
        tools.selection_set(head, ids=[], mode="replace")
        bpy.ops.groomlab.tool(tool="comb")

    def test_5_correctives_keyed_in_blender(self):
        head, t = self.head, self.t
        scene = bpy.context.scene
        t.tab = "TOOLS"  # (Correctives: a section of the Tools page, under the guide groups)
        log = drawn(palette.VIEW3D_PT_groomlab_tools)
        self.assertEqual(texts(log, "groomlab.corrective_new"), ["", ""])  # Fix Frame and New: header icons
        self.assertIn(("list", "correctives"), log)
        self.assertLess(log.index(("list", "groups")), log.index(("list", "correctives")))
        self.assertEqual([r.kind for r in t.correctives], ["base"])
        bpy.ops.groomlab.tool(tool="place")  # (a tool that can't edit a corrective: it stops on one)
        self.assertFalse(tools.edits_layers("place"))
        self.assertEqual(bpy.ops.groomlab.corrective_new(), {"FINISHED"})
        layers = tools.correctives(head)["layers"]
        self.assertEqual(len(layers), 1)
        name = layers[0]["name"]
        self.assertEqual([(r.kind, r.name) for r in t.correctives], [("layer", name), ("base", "Base groom")])
        self.assertEqual(tools.picked_corrective(scene), ("layer", name))  # brushes sculpt into it
        self.assertEqual(tools.correctives(head)["target"]["kind"], "corrective")
        self.assertEqual(t.tool, "")
        # Its row: the weight (a popover), Solo, the eye, then its menu; the card says what it edits.
        log = drawn(palette.VIEW3D_PT_groomlab_tools)
        rows = [e[3] for e in log if e[0] == "op_text" and e[1] == "groomlab.row_popup" and e[3].rows == "CORRECTIVES"]
        self.assertEqual([(r.panel, r.menu) for r in rows], [("GROOMLAB_PT_corrective_weight", ""),
                                                            ("", "GROOMLAB_MT_corrective_context")])
        self.assertIn("groomlab.corrective_solo", [e[1] for e in log if e[0] == "op_text"])
        self.assertIn(("prop", "weight"), drawn(palette.GROOMLAB_PT_corrective_weight))
        bpy.ops.groomlab.tool(tool="comb")
        log = drawn(palette.VIEW3D_PT_groomlab_tools)
        self.assertIn(("label", "primary on %s" % name), log)
        # Clicked again: the base groom again; once more: the corrective.
        t.corrective_index = t.corrective_index
        self.assertEqual(tools.picked_corrective(scene), ("base", "Base groom"))
        self.assertEqual(tools.correctives(head)["target"]["kind"], "base")
        t.corrective_index = 0
        self.assertEqual(tools.picked_corrective(scene), ("layer", name))
        # The eye: off (kept) and on again.
        self.assertEqual(bpy.ops.groomlab.corrective_enable(name=name), {"FINISHED"})
        self.assertFalse(tools.correctives(head)["layers"][0]["enabled"])
        bpy.ops.groomlab.corrective_enable(name=name)
        self.assertTrue(tools.correctives(head)["layers"][0]["enabled"])
        # Its menu: Rename first, Delete last.
        self.assertEqual(texts(drawn(bpy.types.GROOMLAB_MT_corrective_context)),
                         ["Rename", "Move Up", "Move Down", "Go to Its Frame", "Key Weight Here",
                          "Clear Keys (every frame)", "Delete"])
        # The weight: unkeyed, the corrective's on every frame.
        w = head.groomlab_tools.corrective_weights[name]
        w.weight = 0.4
        self.assertAlmostEqual(tools.correctives(head)["layers"][0]["weight"], 0.4, places=5)
        # Key it with Blender's keyframes: the fcurve sampled into the helper's keys.
        scene.frame_set(1)
        self.assertEqual(bpy.ops.groomlab.corrective_key(), {"FINISHED"})
        scene.frame_set(11)
        w.weight = 1.0  # keyed: keyed here as it changes
        fc = tools.weight_fcurve(head, name)
        self.assertIsNotNone(fc)
        self.assertEqual(len(fc.keyframe_points), 2)
        layer = tools.correctives(head)["layers"][0]
        self.assertTrue(layer["keyed"])
        self.assertEqual([k[0] for k in layer["keys"]], [float(f) for f in range(1, 12)])
        self.assertAlmostEqual(layer["keys"][0][1], 0.4, places=4)
        self.assertAlmostEqual(layer["keys"][-1][1], 1.0, places=4)
        # A key moved in Blender (the timeline): the depsgraph handler sends the fcurve again.
        fc.keyframe_points[1].co = (21.0, 1.0)
        fc.update()
        tools.push_corrective_keys(head, name)
        self.assertEqual(tools.correctives(head)["layers"][0]["keys"][-1][0], 21.0)
        # Rename: the fcurve follows.
        self.assertEqual(bpy.ops.groomlab.corrective_rename("EXEC_DEFAULT", name=name, new_name="fringe_fix"),
                         {"FINISHED"})
        self.assertIsNotNone(tools.weight_fcurve(head, "fringe_fix"))
        self.assertIsNone(tools.weight_fcurve(head, name))
        self.assertIn("fringe_fix", head.groomlab_tools.corrective_weights)
        # Fix Frame: keyed in and out around this frame, as Blender keys.
        scene.frame_set(30)
        self.assertEqual(bpy.ops.groomlab.corrective_new(fix=True), {"FINISHED"})
        fix = next(l for l in tools.correctives(head)["layers"] if l["name"] != "fringe_fix")
        fc = tools.weight_fcurve(head, fix["name"])
        self.assertIsNotNone(fc)
        self.assertEqual(sorted(k.co[0] for k in fc.keyframe_points), [24.0, 30.0, 36.0])
        self.assertTrue(fix["keyed"])
        self.assertEqual(bpy.ops.groomlab.corrective_go_to(name=fix["name"]), {"FINISHED"})
        self.assertEqual(scene.frame_current, 30)
        # Solo, move, clear keys, delete.
        self.assertEqual(bpy.ops.groomlab.corrective_solo(name="fringe_fix"), {"FINISHED"})
        enabled = {l["name"]: l["enabled"] for l in tools.correctives(head)["layers"]}
        self.assertEqual(enabled, {"fringe_fix": True, fix["name"]: False})
        bpy.ops.groomlab.corrective_solo(name="fringe_fix")
        self.assertEqual(bpy.ops.groomlab.corrective_move(name="fringe_fix", step=1), {"FINISHED"})
        self.assertEqual(tools.correctives(head)["layers"][-1]["name"], "fringe_fix")
        self.assertEqual(t.correctives[0].name, "fringe_fix")  # the top of the stack first
        self.assertEqual(bpy.ops.groomlab.corrective_clear_keys(name="fringe_fix"), {"FINISHED"})
        self.assertIsNone(tools.weight_fcurve(head, "fringe_fix"))
        self.assertFalse(next(l for l in tools.correctives(head)["layers"] if l["name"] == "fringe_fix")["keyed"])
        for n in ("fringe_fix", fix["name"]):
            self.assertEqual(bpy.ops.groomlab.corrective_delete(name=n), {"FINISHED"})
        self.assertEqual(tools.correctives(head)["layers"], [])
        self.assertEqual(len(head.groomlab_tools.corrective_weights), 0)
        self.assertEqual([r.kind for r in t.correctives], ["base"])
        # A Sculpt node: brushed (its row picked), its edits cleared.
        made, _err = graph.add_node(head, "SculptGuides")
        tools.refresh_info(head)
        self.assertEqual([(r.kind, r.name) for r in t.correctives], [("node", made), ("base", "Base groom")])
        t.corrective_index = 0
        self.assertEqual(tools.correctives(head)["target"], {"kind": "node", "name": made})
        self.assertEqual(menu.correctives_menu(bpy.context), "GROOMLAB_MT_sculpt_node_context")
        self.assertEqual(texts(drawn(bpy.types.GROOMLAB_MT_sculpt_node_context)), ["Clear Edits"])
        # Its panel (graph_ui.json's nodeActions): Done Sculpting while it is brushed, what Esc in the
        # brush tool does (tool_ops.leave_sculpt_node, no undo step); Sculpt in its place then, which
        # brushes it again.
        ge = importlib.import_module(bu.MODULE + ".graph_editor")
        gt = importlib.import_module(bu.MODULE + ".graph_tree")
        authoring = importlib.import_module(bu.MODULE + ".graph_authoring")
        gt.flush()
        node = gt.node_named(gt.tree_of(head), made)
        pinned = lambda: ge.draw_pinned_actions(Recorder([]), bpy.context, node)  # noqa: E731
        self.assertIn("sculpt.done", pinned())
        self.assertNotIn("sculpt.brush", pinned())
        for op in (authoring.GROOMLAB_OT_graph_sculpt_done, authoring.GROOMLAB_OT_graph_sculpt_brush):
            self.assertNotIn("UNDO", op.bl_options)  # (a tool setting, as Esc)
        self.assertEqual(bpy.ops.groomlab.graph_sculpt_done(node=made), {"FINISHED"})
        self.assertEqual(tools.correctives(head)["target"]["kind"], "base")
        self.assertEqual(bpy.context.scene.groomlab_tools.status, "Brushing the guides again")
        self.assertIn("sculpt.brush", pinned())
        self.assertNotIn("sculpt.done", pinned())
        self.assertEqual(bpy.ops.groomlab.graph_sculpt_brush(node=made), {"FINISHED"})
        self.assertEqual(tools.correctives(head)["target"], {"kind": "node", "name": made})
        self.assertEqual([(r.kind, r.name) for r in t.correctives][t.corrective_index], ("node", made))
        self.assertIn("sculpt.done", pinned())
        self.assertEqual(bpy.ops.groomlab.sculpt_clear(), {"FINISHED"})
        self.assertEqual(bpy.ops.groomlab.sculpt_target(), {"FINISHED"})  # the base groom again
        self.assertEqual(tools.correctives(head)["target"]["kind"], "base")
        graph.remove_node(head, made)
        t.tab = "TOOLS"

    def test_6_files_add_review_and_panels(self):
        head, t = self.head, self.t
        path = os.path.join(bu.TMP, "linked.groom")
        self.assertEqual(bpy.ops.groomlab.save_groom(filepath=path), {"FINISHED"})
        self.assertEqual(os.path.normcase(os.path.normpath(head.groomlab.groom_file)), os.path.normcase(path))
        os.remove(path)
        self.assertEqual(bpy.ops.groomlab.save_groom_linked("EXEC_DEFAULT"), {"FINISHED"})
        self.assertTrue(os.path.isfile(path))
        guides = hcall(head, "guides_info")["guides"]
        self.assertEqual(bpy.ops.groomlab.reload_groom(), {"FINISHED"})
        self.assertEqual(hcall(head, "guides_info")["guides"], guides)
        self.assertEqual(bpy.ops.groomlab.add_node_before_output(type="Wind"), {"FINISHED"})
        self.assertIn("Wind", [n["type"] for n in hcall(head, "get_graph")["graph"]["nodes"]])
        d = head.groomlab_tools
        self.assertEqual(bpy.ops.groomlab.review_heatmap(), {"FINISHED"})
        self.assertEqual(d.strand_color, "density")
        bpy.ops.groomlab.review_heatmap()
        self.assertEqual(d.strand_color, "solid")
        # Every panel of the Groom Tools draws on every tab: the Guide groups (palette.json's one
        # section, the Tools tab's list) right under the grid, then the card; While sculpting and
        # Display under it.
        order = [c.bl_label for c in palette.classes if getattr(c, "bl_parent_id", "") == "VIEW3D_PT_groomlab_tools"]
        self.assertEqual(order, ["Display"])  # (While sculpting: under the Correctives)
        for tab in ("TOOLS", "PAINT", "PHOTOS"):
            t.tab = tab
            drawn(*[c for c in palette.classes if issubclass(c, bpy.types.Panel)])
        t.tab = "TOOLS"
        # The Output strip's steps: in the GroomLab menu (Simulate, Utilities, Export, Import).
        main = drawn(bpy.types.GROOMLAB_MT_main)
        self.assertTrue({"Simulate", "Export", "Start From"} <= set(e[2] for e in main if e[0] == "menu"), main)
        everything = []
        for e in main:
            if e[0] == "menu" and e[1].startswith("GROOMLAB_MT_main_"):
                everything += texts(drawn(getattr(bpy.types, e[1])))
        for item in ("Simulate Range", "Transfer Groom to Scalp...", "Alembic...", "Alembic Curves..."):
            self.assertIn(item, everything)
        bpy.ops.groomlab.tool(tool="comb")
        log = drawn(palette.VIEW3D_PT_groomlab_tools)
        groups = log.index(("list", "groups")) if ("list", "groups") in log else -1
        self.assertGreaterEqual(groups, 0, log)
        brush = [i for i, e in enumerate(log) if e[:2] == ("prop", "radius")]
        self.assertTrue(brush)
        self.assertLess(brush[0], groups, "the card over the lists")
        # A group's row: its segments (a popover: Segments, By length) and its menu (Edit Guides).
        rows = [e[3] for e in log if e[0] == "op_text" and e[1] == "groomlab.row_popup" and e[3].rows == "GROUPS"]
        self.assertEqual([(r.panel, r.menu) for r in rows][:2], [("GROOMLAB_PT_group_segments", ""),
                                                                ("", "GROOMLAB_MT_group_context")])
        self.assertNotIn(("prop", "group"), log)  # the list picks the group
        self.assertNotIn(("prop", "resample_segments"), log)  # (the row's popover)
        segments = drawn(palette.GROOMLAB_PT_group_segments)
        self.assertIn(("prop", "resample_segments"), segments)
        self.assertIn(("prop", "segments_by_length"), segments)
        self.assertNotIn(("prop", "guide_color_mode"), log)  # how the guides draw: the guides' popover
        display = drawn(palette.GROOMLAB_PT_guide_display)
        # One row of what shows, then the guides' look, then the tubes' look; no Level menu.
        for name in ("show_guides", "show_parts", "show_flow", "show_tubes", "guide_width", "guide_points",
                     "guide_color_mode", "guides_smoothed", "tube_shaded", "tube_wireframe", "tube_colors",
                     "tube_level_colors"):
            self.assertIn(("prop", name), display)
        for gone in ("tubes_always", "tube_view_level"):
            self.assertNotIn(("prop", gone), display)
        self.assertIn(("popover", "GROOMLAB_PT_guide_display"), log)  # beside the guides' eye
        # The card's header: the brush's name and what the next stroke edits.
        self.assertIn(("label", "Comb"), log)
        self.assertIn(("label", "primary on Base groom"), log)
        # Find Command: the brushes, the GroomLab menu's commands, the presets; one runs.
        find_command = importlib.import_module(bu.MODULE + ".find_command")
        self.assertIn("groomlab.find_command", [e[1] for e in log if e[0] == "op_text"])
        found = find_command.entries(bpy.context)
        names = [e[1] for e in found]
        self.assertIn("Comb  \u00b7  Brush", names)
        self.assertIn("Alembic  \u00b7  Export", names)
        self.assertFalse([n for n in names if n.startswith("Find Command")])
        next(e for e in found if e[0] == "brush:smooth")[3](bpy.context)
        self.assertEqual(t.tool, "smooth")
        bpy.ops.groomlab.tool(tool="comb")
        # The More menu and the empty state.
        self.assertEqual(texts(drawn(bpy.types.GROOMLAB_MT_tools_more))[:1], ["Create Groom from Selection"])
        # The pie: the tail after the catalog's overflow.
        log = drawn(bpy.types.GROOMLAB_MT_pie)
        self.assertEqual(texts(log)[-8:-5], ["Select", "Freeze", "Groom Tools"])
        # The header's eyes: the strands and the guides (groomlab.eye; Shift+click or right-click:
        # every groom's), and the right-click menu's Show / Hide All from palette.json.
        header = [e for e in drawn(palette.VIEW3D_PT_groomlab_tools) if e[0] == "op_text" and e[1] == "groomlab.eye"]
        self.assertEqual([(e[3].what, e[3].mode) for e in header], [("STRANDS", "TOGGLE"), ("GUIDES", "TOGGLE")])

        class EyeContext(object):
            scene = bpy.context.scene
            active_object = head

            class button_operator(object):
                bl_rna = bpy.types.GROOMLAB_OT_eye.bl_rna
                what, every = "GUIDES", False

        holder = types.SimpleNamespace(layout=Recorder([]))
        menu.draw_eye_context(holder, EyeContext())
        self.assertEqual([e[2] for e in holder.layout.log if e[0] == "op_text"], ["Show All Guides", "Hide All Guides"])
        self.assertEqual([(e[3].mode, e[3].every) for e in holder.layout.log if e[0] == "op_text"],
                         [("SHOW", True), ("HIDE", True)])
        # Selecting a groom (its scalp the active object) makes it the current groom.
        handlers = importlib.import_module(bu.MODULE + ".handlers")
        bpy.context.scene.groomlab.groom = None
        bpy.context.view_layer.objects.active = head
        self.assertEqual(handlers.follow_active(), head)
        self.assertIsNone(handlers.active_changed())  # the message bus's callback returns nothing
        self.assertEqual(bpy.context.scene.groomlab.groom, head)


if __name__ == "__main__":
    bu.run(DefinitionsTest, ToolsTest)
