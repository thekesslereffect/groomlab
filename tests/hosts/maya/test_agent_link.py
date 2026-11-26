# SPDX-License-Identifier: Apache-2.0
"""The agent link's command in Maya (groomLabAgent: engine/editor agent_link.h, agent_tools.h), no UI:
the link's discovery file comes and goes with -start and -stop; requests need its token; the tools
work on the scene's grooms (the current groom by default, any by name); an edit sets the groom's
stored data through Maya's undo (one undo chunk a call, as groomlab.agent_link opens it), so Maya's
undo and redo take it back and do it again; what Maya does its own way is refused with a hint.

Usage: mayapy test_agent_link.py <GroomLab.mll> <maya/scripts dir>
"""

import json
import os
import sys
import tempfile
import unittest

PLUGIN_PATH = os.path.abspath(sys.argv[1])
SCRIPTS_DIR = os.path.abspath(sys.argv[2])
sys.argv = sys.argv[:1]
sys.path.insert(0, SCRIPTS_DIR)

LINK_DIR = os.path.join(tempfile.mkdtemp(prefix="groomlab_agent_link_"), "agent_link")
os.environ["GROOMLAB_AGENT_LINK_DIR"] = LINK_DIR

import maya.standalone  # noqa: E402

maya.standalone.initialize(name="python")

from maya import cmds  # noqa: E402

import groomlab  # noqa: E402


class AgentLink(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH)

    def setUp(self):
        cmds.file(new=True, force=True)
        cmds.undoInfo(state=True, infinity=True)
        self.scalp = cmds.polySphere(radius=10, subdivisionsX=40, subdivisionsY=30, name="head")[0]
        self.groom = groomlab.create_groom(self.scalp)
        self.shape = cmds.listRelatives(self.groom, shapes=True, fullPath=True)[0] if cmds.nodeType(self.groom) != "groomLabShape" else self.groom
        cmds.select(self.groom)
        info = json.loads(cmds.groomLabAgent(start=True, port=50123, folders=json.dumps({"root": tempfile.gettempdir()})))
        self.token, self.file = info["token"], info["file"]
        self.next_id = 1

    def tearDown(self):
        cmds.groomLabAgent(stop=True)

    def ask(self, method, params=None, token=None, chunk=None):
        line = json.dumps({"token": self.token if token is None else token, "id": self.next_id, "method": method,
                           "params": params or {}})
        self.next_id += 1
        if chunk:
            cmds.undoInfo(openChunk=True, chunkName=chunk)
        try:
            return json.loads(cmds.groomLabAgent(request=line))
        finally:
            if chunk:
                cmds.undoInfo(closeChunk=True)

    def call(self, name, arguments=None, error=False):
        answer = self.ask("tools/call", {"name": name, "arguments": arguments or {}}, chunk="Agent: %s" % name)
        result = answer["result"]
        self.assertEqual(result["isError"], error, json.dumps(result)[:1500])
        texts = [c["text"] for c in result["content"] if c["type"] == "text"]
        return json.loads(texts[-1]) if texts else None

    def guide_count(self):
        text = cmds.getAttr(self.shape + ".guideData") or ""
        return len(json.loads(text)["guides"]) if text else 0

    def test_discovery_file_and_token(self):
        self.assertTrue(os.path.isfile(self.file))
        with open(self.file) as f:
            link = json.load(f)
        self.assertEqual((link["host"], link["port"], link["token"], link["pid"]), ("maya", 50123, self.token, os.getpid()))
        status = json.loads(cmds.groomLabAgent(status=True))
        self.assertTrue(status["running"])
        self.assertNotIn("token", status)
        # Without the token: refused, nothing done.
        self.assertEqual(self.ask("tools/list", token="nope")["error"]["code"], -32001)
        tools = {t["name"] for t in self.ask("tools/list")["result"]["tools"]}
        self.assertTrue({"brush_stroke", "get_graph", "render_preview"} <= tools)
        cmds.groomLabAgent(stop=True)
        self.assertFalse(os.path.exists(self.file))
        with self.assertRaises(RuntimeError):
            cmds.groomLabAgent(request=json.dumps({"token": self.token, "id": 1, "method": "tools/list"}))

    def test_edits_are_maya_undo_steps(self):
        grooms = self.call("list_grooms")["grooms"]
        transform = cmds.listRelatives(self.shape, parent=True)[0]
        self.assertEqual([g["name"] for g in grooms], [transform])
        self.assertEqual(self.guide_count(), 0)
        placed = self.call("brush_stroke", {"mode": "place", "from": "front_hairline", "via": ["top"], "to": "crown", "length": 6})
        made = placed["changed"]["guidesAdded"]
        self.assertGreater(made, 2)
        self.assertEqual(self.guide_count(), made, "the stored guides")
        self.assertEqual(cmds.undoInfo(q=True, undoName=True), "Agent: brush_stroke")
        placed_text = cmds.getAttr(self.shape + ".guideData")
        combed = self.call("brush_stroke", {"mode": "comb", "from": "top", "to": "back", "offset": 3, "radius": 5})
        self.assertGreater(combed["changed"]["guidesTouched"], 0)
        combed_text = cmds.getAttr(self.shape + ".guideData")
        self.assertNotEqual(combed_text, placed_text)
        # Maya's undo and redo take the agent's steps back and do them again.
        cmds.undo()
        self.assertEqual(cmds.getAttr(self.shape + ".guideData"), placed_text)
        cmds.undo()
        self.assertEqual(self.guide_count(), 0)
        cmds.redo()
        cmds.redo()
        self.assertEqual(cmds.getAttr(self.shape + ".guideData"), combed_text)
        # A graph edit the same way; the summary follows what the groom shows.
        graph = self.call("get_graph")
        before = len(graph["nodes"])
        stored = cmds.getAttr(self.shape + ".graph")
        self.call("add_node", {"type": "Curl"})
        self.assertEqual(len(json.loads(cmds.getAttr(self.shape + ".graph"))["nodes"]), before + 1)
        cmds.undo()
        self.assertEqual(cmds.getAttr(self.shape + ".graph"), stored)
        # A tool that changes nothing leaves no undo step.
        top = cmds.undoInfo(q=True, undoName=True)
        self.call("measure_guides")
        self.assertEqual(cmds.undoInfo(q=True, undoName=True), top)

    def test_what_maya_does_its_own_way(self):
        self.assertIn("Create Groom", self.call("new_groom", {"name": "x", "mesh": "nika"}, error=True)["error"])
        self.assertIn("GroomLab > Export", self.call("export", {"path": "x.abc"}, error=True)["error"])
        self.assertIn("viewport", self.call("render_preview", {"viewport": True}, error=True)["error"])  # (no viewport in mayapy)
        shot = self.ask("tools/call", {"name": "render_preview", "arguments": {"size": 96}})["result"]
        self.assertFalse(shot["isError"])
        self.assertEqual(shot["content"][0]["type"], "image")


if __name__ == "__main__":
    result = unittest.main(exit=False, verbosity=2).result
    maya.standalone.uninitialize()
    os._exit(0 if result.wasSuccessful() else 1)
