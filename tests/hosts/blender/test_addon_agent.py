# SPDX-License-Identifier: Apache-2.0
"""The agent link in a headless Blender (agent_link.py; the helper's agent_start / agent_request,
hosts/blender/helper/src/agent.h). Groom Tools > More > Agent Link turns it on: the helper writes the
discovery file (host "blender", Blender's pid, the add-on's port, a token). An agent's client (a
socket here, as groomlab-cli mcp --live talks: JSON lines with the file's token; then the CLI
itself) connects, lists the tools, places and combs guides on the scene's groom, each one step of
the groom's own undo stack, its depth kept on the scalp as every Groom Tools edit keeps it, so
Blender's undo (tools.sync_undo) takes the agent's edits back; the agent's undo and history are the
same stack; a graph edit reaches the groom's graph; the header's badge shows while it is connected
and its Stop turns the link off.

A background Blender runs no timers between a test's lines: the test runs the link's timer
callback itself (agent_link.poll_now), the client in a thread of its own."""

import importlib
import json
import os
import queue
import socket
import subprocess
import sys
import threading
import time
import unittest

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blender_util as bu  # noqa: E402

LINK_DIR = os.path.join(bu.TMP, "agent_link")
os.environ["GROOMLAB_AGENT_LINK_DIR"] = LINK_DIR  # (the helper started below reads it)

pkg, m = bu.enable()
groom, process = m["groom"], m["process"]
agent_link = importlib.import_module(bu.MODULE + ".agent_link")
tools = importlib.import_module(bu.MODULE + ".tools")
palette = importlib.import_module(bu.MODULE + ".palette")


def hcall(obj, cmd, **args):
    return process.call(cmd, session=groom.ensure(obj).session, **args)


def stored_guides(obj):
    return hcall(obj, "get_state", scalp=False, textures=False, texts=True)["texts"]["guideData"]


def guide_count(scalp):
    return hcall(scalp, "guides_info")["guides"]


def text_of(answer):
    for c in reversed(answer["result"]["content"]):
        if c["type"] == "text":
            return json.loads(c["text"])
    return None


class LineReader(object):
    """A stream's lines into a queue from a thread of its own (the link answers on this thread)."""

    def __init__(self, read_line):
        self.lines = queue.Queue()
        self.thread = threading.Thread(target=self._run, args=(read_line,), daemon=True)
        self.thread.start()

    def _run(self, read_line):
        while True:
            try:
                line = read_line()
            except (OSError, ValueError):
                line = b""
            self.lines.put(line)
            if not line:
                return

    def next(self, timeout=120.0):
        """The next line, the link's timer run (its callback) until it comes."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            agent_link.poll_now()
            try:
                return self.lines.get(timeout=0.01)
            except queue.Empty:
                pass
        raise AssertionError("no answer")


class AgentLinkTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for o in list(bpy.data.objects):
            bpy.data.objects.remove(o)
        cls.head = bu.head_object()
        assert bpy.ops.groomlab.add_groom("INVOKE_DEFAULT") == {"FINISHED"}
        assert cls.head.groomlab.guide_target == "hierarchy"

    def badge(self):
        log = []
        agent_link.draw_badge(bu.FakeLayout(log))
        return log

    def test_an_agent_grooms_the_scene_through_blenders_undo(self):
        head = self.head
        t = bpy.context.scene.groomlab_tools
        self.assertFalse(t.agent_link)
        self.assertEqual(self.badge(), [])
        # The ... menu's Agent Link (its setting): on, the file written (host "blender", this Blender).
        t.agent_link = True
        self.assertTrue(agent_link.running() and t.agent_link)
        files = [f for f in os.listdir(LINK_DIR) if f.startswith("blender-")]
        self.assertEqual(files, ["blender-%d.json" % os.getpid()])
        with open(os.path.join(LINK_DIR, files[0]), encoding="utf-8") as f:
            info = json.load(f)
        self.assertEqual((info["host"], info["pid"], info["port"]), ("blender", os.getpid(), agent_link.link().port))
        self.assertEqual(info["groom"], head.name)
        token = info["token"]

        # An agent connects: the badge shows.
        sock = socket.create_connection(("127.0.0.1", info["port"]), timeout=30)
        reader = LineReader(sock.makefile("rb").readline)
        ids = iter(range(1, 1000))

        def request(method, params, tok=token):
            sock.sendall((json.dumps({"token": tok, "id": next(ids), "method": method, "params": params}) + "\n").encode())
            return json.loads(reader.next())

        def call(name, arguments):
            answer = request("tools/call", {"name": name, "arguments": arguments})
            self.assertIn("result", answer, answer)
            self.assertFalse(answer["result"]["isError"], json.dumps(answer)[:2000])
            return text_of(answer)

        listed = request("tools/list", {})
        self.assertGreater(len(listed["result"]["tools"]), 40)
        self.assertEqual(agent_link.clients(), 1)
        self.assertIn(("label", "Agent"), self.badge())
        self.assertIn(("operator", "groomlab.agent_link_stop"), self.badge())
        self.assertEqual(request("tools/list", {}, "wrong")["error"]["code"], -32001)
        grooms = call("list_grooms", {})
        self.assertIn(head.name, [g["name"] for g in grooms["grooms"]])

        # Guides placed and combed: steps of the groom's stack, its depth on the scalp (Blender's undo).
        depth = hcall(head, "undo_state")["undoDepth"]
        placed = call("brush_stroke", {"mode": "place", "from": "front_hairline", "via": ["top"], "to": "crown",
                                       "length": 12, "match_neighbors": False})
        made = placed["changed"]["guidesAdded"]
        self.assertGreater(made, 2)
        self.assertEqual(guide_count(head), made)
        state = hcall(head, "undo_state")
        self.assertEqual((state["undoDepth"], state["undoLabel"]), (depth + 1, "place stroke"))
        self.assertEqual(head.groomlab_tools.undo_depth, depth + 1)
        self.assertGreater(len(head.groomlab.hair.data.curves), 0)  # (the strands shown: evaluated, written)
        placed_text = stored_guides(head)
        combed = call("brush_stroke", {"mode": "comb", "from": "crown", "to": "back", "offset": 3, "radius": 6})
        self.assertGreater(combed["changed"]["guidesTouched"], 0)
        combed_text = stored_guides(head)
        self.assertNotEqual(combed_text, placed_text)
        self.assertEqual(head.groomlab_tools.undo_depth, depth + 2)

        # The agent's undo is the groom's: the comb taken back; Blender's undo takes the guides.
        self.assertEqual(call("undo", {})["undone"], ["comb stroke"])
        self.assertEqual(stored_guides(head), placed_text)
        self.assertEqual(head.groomlab_tools.undo_depth, depth + 1)
        history = call("history", {})
        self.assertEqual([(s["title"], s["state"]) for s in history["steps"]][-2:],
                         [("place stroke", "done"), ("comb stroke", "undone")])
        head.groomlab_tools.undo_depth = depth  # (as Blender's undo puts it back)
        self.assertEqual(tools.sync_undo(head), -1)
        self.assertEqual(guide_count(head), 0)
        head.groomlab_tools.undo_depth = depth + 1  # (Blender's redo)
        self.assertEqual(tools.sync_undo(head), 1)
        self.assertEqual(call("redo", {})["redone"], ["comb stroke"])
        self.assertEqual(stored_guides(head), combed_text)

        # A graph edit lands in the groom's graph (what the Graph Editor and the .blend read).
        curl = call("add_node", {"type": "Curl"})["node"]
        nodes = [n["name"] for n in hcall(head, "get_graph")["graph"]["nodes"]]
        self.assertIn(curl, nodes)
        self.assertIn(curl, head.groomlab.graph_json)
        call("undo", {})
        self.assertNotIn(curl, [n["name"] for n in hcall(head, "get_graph")["graph"]["nodes"]])

        # What Blender does its own way: said, nothing changed.
        refused = request("tools/call", {"name": "new_groom", "arguments": {"name": "another", "mesh": "nika"}})
        self.assertTrue(refused["result"]["isError"])
        self.assertIn("Create Groom", json.dumps(refused))

        # groomlab-cli mcp --live blender finds this Blender through the file and forwards to it.
        if bu.CLI and os.path.isfile(bu.CLI):
            proc = subprocess.Popen([bu.CLI, "mcp", "--live", "blender"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=subprocess.DEVNULL)
            try:
                out = LineReader(proc.stdout.readline)

                def mcp(rid, method, params):
                    proc.stdin.write((json.dumps({"jsonrpc": "2.0", "id": rid, "method": method, "params": params}) + "\n").encode())
                    proc.stdin.flush()
                    return json.loads(out.next())

                init = mcp(1, "initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                             "clientInfo": {"name": "test", "version": "1"}})
                self.assertIn("blender", json.dumps(init["result"]).lower())
                proc.stdin.write(b'{"jsonrpc": "2.0", "method": "notifications/initialized"}\n')
                proc.stdin.flush()
                listed = mcp(2, "tools/call", {"name": "list_grooms", "arguments": {}})
                self.assertIn(head.name, json.dumps(listed["result"]))
            finally:
                proc.stdin.close()
                proc.wait(timeout=60)

        # The badge's Stop: the link off, its file gone, the agent disconnected.
        self.assertEqual(bpy.ops.groomlab.agent_link_stop(), {"FINISHED"})
        self.assertFalse(agent_link.running() or t.agent_link)
        self.assertEqual([f for f in os.listdir(LINK_DIR) if f.startswith("blender-")], [])
        self.assertEqual(self.badge(), [])
        self.assertEqual(reader.next(timeout=10), b"")
        sock.close()


if __name__ == "__main__":
    bu.run(AgentLinkTest)
