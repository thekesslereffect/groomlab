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
  Mirror Selection, Reparent, Promote / Demote, Mirror Group, parts, flow, the maps' menu (rename,
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
tool_ops = importlib.import_module(bu.MODULE + ".tool_ops")

# Ids the definitions show for Blender that it does not run yet: hidden in Blender. This is the
# parity checklist; bind one in actions.py (and take it off here) as it lands.
ALLOWED_MISSING = set()
# The Graph Editor's ids (graph_ui.json) Blender does not run yet (the rest are bound in graph_ui.ACTIONS).
ALLOWED_MISSING_GRAPH = {
    "graph.gpuPreview",   # the GPU preview is Maya's viewport (Blender draws what the helper builds)
}
# Ids Blender does its own way (n/a in the parity audit): Blender's theme, keymap and updates.
NOT_IN_BLENDER = {
    "theme.set", "theme.new", "theme.reload", "theme.openFolder",  # Blender's own UI theme
}


MAIN = ["Groom Tools", "Graph Editor", "Create Groom from Selection", "Add Sample Mesh", "Guides from Photos (experimental)...",
        "Groom Files", "Import", "Add", "Simulate", "Display", "Review", "Export", "Utilities", "Help"]


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
        for gone in ("Studio", "Render", "Theme"):
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
                self.assertIn(idname.split(".")[-1], t.bl_rna.properties, action_id)
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
        for name in ("GROOMLAB_MT_main", "GROOMLAB_MT_main_groomfiles", "GROOMLAB_MT_main_add", "GROOMLAB_MT_main_review",
                     "GROOMLAB_MT_main_export", "GROOMLAB_MT_main_help", "GROOMLAB_MT_tools_more", "GROOMLAB_MT_guide_edits",
                     "GROOMLAB_MT_group_context", "GROOMLAB_MT_map_context", "GROOMLAB_MT_maps_mapconvert",
                     "GROOMLAB_MT_mapsbar_mapconvert", "GROOMLAB_MT_corrective_context",
                     "GROOMLAB_MT_sculpt_node_context", "GROOMLAB_MT_output_export", "GROOMLAB_MT_output_import",
                     "GROOMLAB_MT_photo_context", "GROOMLAB_MT_pair_context",
                     "GROOMLAB_MT_empty_import"):
            self.assertTrue(hasattr(bpy.types, name), name)
        log = drawn(bpy.types.GROOMLAB_MT_main)
        self.assertEqual([e[2] for e in log if e[0] in ("op_text", "menu")], MAIN)
        self.assertEqual(texts(drawn(bpy.types.GROOMLAB_MT_main_groomfiles)),
                         ["Import Groom File...", "Reload from Groom File", "Save to Groom File", "Save Groom File As..."])
        self.assertEqual(texts(drawn(bpy.types.GROOMLAB_MT_main_export)),
                         ["Alembic...", "USD...", "V-Ray Scene...", "Unreal Groom...", "Unreal Cards...", "Unity Strands (.abc)...",
                          "Unity Feathers...", "Unity Cards...", "glTF Cards...", "Card Maps...", "Scales Mesh (FBX)...",
                          "Scales Instancer (USD)..."])
        add = drawn(bpy.types.GROOMLAB_MT_main_add)
        self.assertEqual(texts(add), ["Feathers...", "Braid on Curves...", "Collider", "Wind", "Jiggle"])
        self.assertIn(("menu", "GROOMLAB_MT_main_add_cards", "Cards"), add)
        cards = drawn(bpy.types.GROOMLAB_MT_main_add_cards)
        self.assertEqual(texts(cards), ["Hero", "Mid", "Low"])
        self.assertEqual([e[3].lod for e in cards if e[0] == "op_text"], ["hero", "mid", "low"])
        samples = drawn(bpy.types.GROOMLAB_MT_main_samples)
        self.assertEqual(texts(samples), ["Nika (head)", "Manny (head)", "Wolf", "Buddy (rig)"])
        self.assertEqual([e[3].sample for e in samples if e[0] == "op_text"], ["nika", "manny", "wolf", "buddy"])
        sim = drawn(bpy.types.GROOMLAB_MT_main_simulate)
        self.assertEqual(texts(sim), ["Add Simulate Node", "Simulate Range", "Bake Simulation", "Clear Simulation"])
        self.assertEqual([e[3].bake for e in sim if e[0] == "op_text" and e[1] == "groomlab.simulate_range"],
                         [False, True])
        self.assertEqual(actions.action_for("groomlab.simulate_range", {"bake": True}), "sim.bake")
        self.assertEqual(actions.action_for("groomlab.simulate_range", {"bake": False}), "sim.range")
        review = drawn(bpy.types.GROOMLAB_MT_main_review)
        self.assertEqual(texts(review), ["Density Heatmap", "Turntable"])
        self.assertIn(("prop", "show_hud"), review)  # Strand Count HUD: a checkbox (checkable)
        self.assertEqual(texts(drawn(bpy.types.GROOMLAB_MT_main_utilities)),
                         ["Strands to Mesh...", "Bake Node to Guides...", "Bake Scale Maps", "Delete All Flow Arrows",
                          "Transfer Groom to Scalp...",
                          "Copy Graph to Other Grooms...", "Find Missing Files..."])
        help_ = drawn(bpy.types.GROOMLAB_MT_main_help)
        self.assertEqual(texts(help_), ["Docs", "Keyboard Shortcuts", "Check for Updates...", "Discord",
                                        "Support GroomLab", "About GroomLab"])
        donate = next(e for e in help_ if e[0] == "op_text" and e[2] == "Support GroomLab")
        self.assertEqual(donate[3].url, actions.DONATE_URL)
        self.assertEqual(texts(drawn(bpy.types.GROOMLAB_MT_output_export)),
                         ["Alembic...", "USD...", "Unreal Groom...", "Unreal Cards...", "Unity Strands (.abc)...",
                          "Unity Cards...", "glTF Cards...", "Card Maps...", "Save Groom File As..."])
        self.assertEqual(texts(drawn(bpy.types.GROOMLAB_MT_output_import)), ["Alembic Curves...", "Groom File..."])
        self.assertEqual(texts(drawn(bpy.types.GROOMLAB_MT_main_import)), ["Alembic Curves..."])
        # The Photos tab's menus: the photo's (its actions need a photo picked) and the pairs'.
        self.assertEqual(texts(drawn(bpy.types.GROOMLAB_MT_photo_context)), ["Import Photo..."])
        edits = drawn(bpy.types.GROOMLAB_MT_guide_edits)
        self.assertEqual(texts(edits)[:9], ["Children", "Sync +X → -X", "Sync -X → +X", "Mirror Selection",
                                            "Mirror Group...", "Reparent...", "Promote / Demote...", "Clear Selection",
                                            "Lock"])
        self.assertNotIn("From Curves", texts(edits))  # Maya's
        # Groups' right-click menu without a group: New Group only.
        self.assertEqual(texts(drawn(bpy.types.GROOMLAB_MT_group_context)), ["New Group..."])
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
        self.assertEqual([i.identifier for i in rna["tab"].enum_items], ["TOOLS", "PAINT", "CORRECTIVES", "PHOTOS"])
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
        log = drawn(bpy.types.GROOMLAB_MT_group_context)
        self.assertEqual(texts(log), ["New Group...", "Rename", "Color...", "Default Color", "Select Guides", "Reset Segments", "Delete"])
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
        # Mirror Selection: the mirror images of the selected guides.
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
        # Only the blocks the tool uses: Flow draws no tubes and no Mirror (it doesn't mirror); the
        # guide groups over the card; While sculpting with the brushes that stroke.
        self.assertNotIn(("prop", "tube_shape"), log)
        self.assertNotIn(("prop", "symmetry"), log)
        self.assertTrue(palette.VIEW3D_PT_groomlab_tools_sculpting.poll(bpy.context))
        for gone in ("VIEW3D_PT_groomlab_tools_tubes", "VIEW3D_PT_groomlab_tools_output"):
            self.assertFalse(hasattr(palette, gone))  # their items: the cards, the GroomLab menu
        bpy.ops.groomlab.tool(tool="part")
        self.assertFalse(palette.VIEW3D_PT_groomlab_tools_sculpting.poll(bpy.context))
        bpy.ops.groomlab.tool(tool="comb")
        log = drawn(palette.VIEW3D_PT_groomlab_tools)
        self.assertNotIn(("prop", "tube_shape"), log)
        self.assertIn(("prop", "symmetry"), log)  # Applies to: Mirror
        self.assertTrue(palette.VIEW3D_PT_groomlab_tools_sculpting.poll(bpy.context))

    def test_3_maps(self):
        head, t = self.head, self.t
        t.tab = "PAINT"
        self.assertEqual(bpy.ops.groomlab.map_new(name="mask", kind="vertices", value=0.0), {"FINISHED"})
        log = drawn(palette.VIEW3D_PT_groomlab_tools)
        self.assertIn(("list", "maps"), log)
        bar = texts(log, "groomlab.map_new") + texts(log, "groomlab.map_import")
        self.assertEqual(bar, ["", ""])  # icons in the Maps header
        self.assertIn(("menu", "GROOMLAB_MT_mapsbar_mapconvert", ""), log)
        self.assertTrue(any(e[0] == "label" and e[1].startswith("Read by nothing") for e in log))
        self.assertIn(("prop", "paint_mode"), log)  # the Paint card's own block: Mode, Value and Fill
        self.assertIn("Fill", texts(log, "groomlab.map_fill"))
        ctx = drawn(bpy.types.GROOMLAB_MT_map_context)
        self.assertEqual(texts(ctx), ["New Map...", "Import Image as Map...", "Rename...", "Make Color (r g b a)",
                                      "Relink Image...", "Delete"])
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
        bpy.ops.groomlab.tool(tool="select")  # Select's card: Selection, Profile, Selected guides
        log = drawn(palette.VIEW3D_PT_groomlab_tools)
        self.assertIn(("prop", "tube_shape"), log)
        self.assertIn("Edit as Custom", texts(log))
        # No targets: the shape and values go to the guides Place and Draw make (the placement tube).
        t.tube_shape = "ribbon"
        bpy.ops.groomlab.tool(tool="place")
        got = hcall(head, "tool_get")["settings"]
        self.assertEqual(got["tube"]["shape"], "ribbon")
        # The Shape brush paints the channel picked.
        bpy.ops.groomlab.tool(tool="shape")
        self.assertIn(("prop", "tube_channel"), drawn(palette.VIEW3D_PT_groomlab_tools))
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
        self.assertIn(("list", "tube_shapes"), log)
        self.assertIn("Apply", texts(log, "groomlab.tube_apply"))
        self.assertNotIn(("prop", "tube_fill"), log)  # the Interpolate node's settings have it
        tools.selection_set(head, ids=[], mode="replace")
        bpy.ops.groomlab.tool(tool="comb")

    def test_5_correctives_keyed_in_blender(self):
        head, t = self.head, self.t
        scene = bpy.context.scene
        t.tab = "CORRECTIVES"
        log = drawn(palette.VIEW3D_PT_groomlab_tools)
        self.assertEqual(texts(log, "groomlab.corrective_new"), ["", ""])  # Fix Frame and New: header icons
        self.assertIn(("list", "correctives"), log)
        self.assertEqual([r.kind for r in t.correctives], ["base"])
        self.assertEqual(bpy.ops.groomlab.corrective_new(), {"FINISHED"})
        layers = tools.correctives(head)["layers"]
        self.assertEqual(len(layers), 1)
        name = layers[0]["name"]
        self.assertEqual([(r.kind, r.name) for r in t.correctives], [("layer", name), ("base", "Base groom")])
        self.assertEqual(tools.picked_corrective(scene), ("layer", name))  # brushes sculpt into it
        self.assertEqual(tools.correctives(head)["target"]["kind"], "corrective")
        self.assertEqual(texts(drawn(bpy.types.GROOMLAB_MT_corrective_context)),
                         ["Go to Its Frame", "Key Weight Here", "Clear Keys (every frame)", "Rename", "Solo", "Delete"])
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
        made, _err = graph.add_node(head, "Sculpt")
        tools.refresh_info(head)
        self.assertEqual([(r.kind, r.name) for r in t.correctives], [("node", made), ("base", "Base groom")])
        t.corrective_index = 0
        self.assertEqual(tools.correctives(head)["target"], {"kind": "node", "name": made})
        self.assertEqual(menu.correctives_menu(bpy.context), "GROOMLAB_MT_sculpt_node_context")
        self.assertEqual(texts(drawn(bpy.types.GROOMLAB_MT_sculpt_node_context)), ["Clear Edits"])
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
        self.assertEqual(order, ["While sculpting", "Display"])
        for tab in ("TOOLS", "PAINT", "CORRECTIVES", "PHOTOS"):
            t.tab = tab
            drawn(*[c for c in palette.classes if issubclass(c, bpy.types.Panel)])
        t.tab = "TOOLS"
        # The Output strip's steps: in the GroomLab menu (Simulate, Utilities, Export, Import).
        main = drawn(bpy.types.GROOMLAB_MT_main)
        self.assertTrue({"Simulate", "Export", "Import"} <= set(e[2] for e in main if e[0] == "menu"), main)
        everything = []
        for e in main:
            if e[0] == "menu" and e[1].startswith("GROOMLAB_MT_main_"):
                everything += texts(drawn(getattr(bpy.types, e[1])))
        for item in ("Simulate Range", "Transfer Groom to Scalp...", "Alembic...", "Alembic Curves..."):
            self.assertIn(item, everything)
        log = drawn(palette.VIEW3D_PT_groomlab_tools)
        groups = log.index(("list", "groups")) if ("list", "groups") in log else -1
        self.assertGreaterEqual(groups, 0, log)
        brush = [i for i, e in enumerate(log) if e[:2] == ("prop", "radius")]
        if brush:
            self.assertLess(groups, brush[0], "the groups over the card")
        self.assertIn(("menu", "GROOMLAB_MT_guide_edits", "Edit Guides"), log)
        self.assertIn(("menu", "GROOMLAB_MT_group_context", ""), log)
        self.assertNotIn(("prop", "group"), drawn(palette.VIEW3D_PT_groomlab_tools))  # the list picks the group
        self.assertIn(("prop", "resample_segments"), log)
        self.assertNotIn(("prop", "guide_color_mode"), log)  # every group its own color: Display has the mode
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
        self.assertEqual(handlers.active_changed(), head)
        self.assertEqual(bpy.context.scene.groomlab.groom, head)


if __name__ == "__main__":
    bu.run(DefinitionsTest, ToolsTest)
