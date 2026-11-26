# SPDX-License-Identifier: Apache-2.0
"""GUI test for the agent link in Maya (needs a Maya UI session): Groom Tools' ... > Agent Link turns it
on (the listener, groomlab.agent_link: a QTcpServer on Maya's main thread, and its discovery file);
an agent's client (a thread here, as groomlab-cli mcp --live talks: JSON lines on 127.0.0.1 with the
file's token) lists the tools, places guides and combs them, each call one Maya undo step named
"Agent: <tool>"; the agent's undo is Maya's undo, checked on the groom's stored guides; the header's
badge shows while the agent is connected, and its Stop turns the link off.

A suite of the GUI session (gui_runner.py, ctest maya.gui; harness: gui_support.py). The client's
requests go out from a thread while Maya's idle loop runs (each step returns to Maya), never a
nested event loop.
"""

import json
import os
import socket
import tempfile
import threading
import time

from maya import cmds

from gui_support import check, finish, guarded, settle_undo, soon

state = {}


class Client(object):
    """An agent's client: one connection, one request at a time on a thread."""

    def __init__(self, link):
        self.link = link
        self.sock = socket.create_connection(("127.0.0.1", link["port"]), timeout=60)
        self.buffer = b""
        self.next_id = 1

    def ask(self, method, params=None, token=None):
        """Starts a request; returns a function giving its answer once it came (None until then)."""
        box = {}
        line = json.dumps({"token": self.link["token"] if token is None else token, "id": self.next_id,
                           "method": method, "params": params or {}}) + "\n"
        self.next_id += 1

        def work():
            try:
                self.sock.sendall(line.encode("utf-8"))
                while b"\n" not in self.buffer:
                    data = self.sock.recv(1 << 20)
                    if not data:
                        raise IOError("closed")
                    self.buffer += data
                answer, self.buffer = self.buffer.split(b"\n", 1)
                box["answer"] = json.loads(answer)
            except Exception as e:  # noqa: BLE001
                box["answer"] = {"error": {"message": "client: %s" % e}}
        threading.Thread(target=work, daemon=True).start()
        return lambda: box.get("answer")

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def wait_for(get, then, timeout=60.0):
    """then(answer) once get() gives it, Maya's idle loop running between looks."""
    from groomlab.ui.qt import QtCore
    started = time.time()

    def look():
        answer = get()
        if answer is not None:
            then(answer)
        elif time.time() - started > timeout:
            check("an answer within %d s" % timeout, False)
            finish()
        else:
            QtCore.QTimer.singleShot(20, guarded(look))
    look()


def call(name, arguments, then):
    wait_for(state["client"].ask("tools/call", {"name": name, "arguments": arguments}), then)


def result_json(answer):
    result = answer.get("result") or {}
    texts = [c["text"] for c in result.get("content", []) if c.get("type") == "text"]
    return result, (json.loads(texts[-1]) if texts else None)


def stored_guides():
    text = cmds.getAttr(state["shape"] + ".guideData") or ""
    return text, (len(json.loads(text)["guides"]) if text else 0)


def start():
    state.clear()
    soon(setup)


def setup():
    import groomlab
    from groomlab import agent_link
    from groomlab.ui import palette
    os.environ.setdefault("GROOMLAB_AGENT_LINK_DIR", os.path.join(tempfile.gettempdir(), "groomlab_gui_agent_link"))
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=10, subdivisionsX=40, subdivisionsY=30, name="agentHead")[0]
    state["shape"] = groomlab.create_groom(scalp)
    cmds.select(cmds.listRelatives(state["shape"], parent=True))
    window = palette.show()
    state["window"] = window
    window.groom_picked(window.grooms.findData(state["shape"]))
    check("the link is off by default", not agent_link.running())
    check("no badge while it is off", not window.agent_badge.isVisible())
    action = window.agent_link_action()
    check("Groom Tools' More menu has Agent Link", action is not None and action.isCheckable())
    action.trigger()  # Groom Tools > ... > Agent Link
    check("Agent Link turns it on", agent_link.running())
    window.sync_more_menu()
    check("the menu item shows it on", window.agent_link_action().isChecked())
    path = agent_link.info()["file"]
    with open(path) as f:
        link = json.load(f)
    check("the discovery file: maya, this process, its port and a token",
          link["host"] == "maya" and link["pid"] == os.getpid() and link["port"] == agent_link.info()["port"]
          and len(link["token"]) == 32)
    state["link"], state["file"] = link, path
    state["client"] = Client(link)
    settle_undo()
    wait_for(state["client"].ask("tools/list"), listed)


def listed(answer):
    from groomlab import agent_link
    tools = {t["name"] for t in answer.get("result", {}).get("tools", [])}
    check("tools/list: the agent tools", {"brush_stroke", "render_preview", "undo"} <= tools)
    check("the badge shows while the agent is connected", agent_link.clients() == 1 and state["window"].agent_badge.isVisible())
    wait_for(state["client"].ask("tools/list", token="wrong"), refused)


def refused(answer):
    check("a request without the token is refused", answer.get("error", {}).get("code") == -32001 and "result" not in answer)
    call("brush_stroke", {"mode": "place", "from": "front_hairline", "via": ["top"], "to": "crown", "length": 6}, placed)


def placed(answer):
    result, data = result_json(answer)
    made = (data or {}).get("changed", {}).get("guidesAdded", 0)
    text, count = stored_guides()
    check("placed guides are the groom's stored guides", not result.get("isError") and made > 2 and count == made)
    check("one undo step named for the tool", cmds.undoInfo(q=True, undoName=True) == "Agent: brush_stroke")
    state["placed"] = text
    call("brush_stroke", {"mode": "comb", "from": "top", "to": "back", "offset": 3, "radius": 5}, combed)


def combed(answer):
    result, data = result_json(answer)
    text, _ = stored_guides()
    check("combing moved them", not result.get("isError") and data["changed"]["guidesTouched"] > 0 and text != state["placed"])
    state["combed"] = text
    call("undo", {}, undone)


def undone(answer):
    result, data = result_json(answer)
    text, _ = stored_guides()
    check("the agent's undo is Maya's: the comb taken back", not result.get("isError") and text == state["placed"]
          and data["undone"] == ["Agent: brush_stroke"])
    check("its answer has the groom's summary", "summary" in data and data["summary"].get("guides", 0) > 0)
    cmds.redo()  # (the artist's redo: the same queue)
    text, _ = stored_guides()
    check("Maya's redo puts the comb back", text == state["combed"])
    call("render_preview", {"viewport": True, "size": 256}, viewed)


def viewed(answer):
    result = answer.get("result") or {}
    images = [c for c in result.get("content", []) if c.get("type") == "image"]
    check("render_preview viewport: the view as the artist sees it", not result.get("isError") and len(images) == 1)
    soon(stop)


def stop():
    from groomlab import agent_link
    window = state["window"]
    from groomlab.ui.qt import QtWidgets
    window.agent_badge.findChild(QtWidgets.QToolButton, "agentStop").click()  # the badge's Stop
    check("Stop turns the link off", not agent_link.running())
    check("its file is gone", not os.path.exists(state["file"]))
    check("the badge goes", not window.agent_badge.isVisible())
    state["client"].close()
    window.close()
    finish()
