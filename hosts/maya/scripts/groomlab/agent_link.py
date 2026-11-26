# SPDX-License-Identifier: Apache-2.0
"""The agent link: an AI agent grooms the scene's grooms (engine/editor agent_link.h, agent_tools.h).

While it is on, Maya listens on 127.0.0.1 (a port of its own) and says so in a discovery file in the
user's GroomLab folder (Documents/GroomLab/agent_link/maya-<pid>.json, with a fresh token);
`groomlab-cli mcp --live` (the MCP server an agent's client starts) finds the file and forwards the
agent's tool calls here. Each request line goes to groomLabAgent -request on Maya's main thread
(this listener is a QTcpServer), inside an undo chunk named "Agent: <tool>", so every tool call is
one step of Maya's undo; the agent's undo, redo and history are Maya's own undo queue. Off by
default (Groom Tools > ... > Agent link).

    groomlab.agent_link.start()      the link on (returns its discovery file)
    groomlab.agent_link.stop()
    groomlab.agent_link.running(), clients()
"""

import json
import os

from maya import cmds

from groomlab.base import ensure_plugin

# What Maya's own undo queue answers (not groomLabAgent): the agent undoes what the artist undoes.
HOST_TOOLS = ("undo", "redo", "history")

_link = None
_listeners = []  # called (no arguments) when the link starts, stops or a client comes or goes


def add_listener(fn):
    """`fn()` runs when the link starts or stops, or an agent connects or leaves (the Groom Tools' badge)."""
    if fn not in _listeners:
        _listeners.append(fn)


def remove_listener(fn):
    if fn in _listeners:
        _listeners.remove(fn)


def _changed():
    for fn in list(_listeners):
        try:
            fn()
        except Exception:
            pass


def _folders():
    """The folders the tools read and write in: the project's (its root), GroomLab's own presets,
    sample meshes and docs (the module's)."""
    root = cmds.workspace(q=True, rootDirectory=True) or os.getcwd()
    out = {"root": os.path.normpath(root)}
    from groomlab.shipped import shipped_dir
    for key in ("presets", "meshes"):
        path = shipped_dir(key)
        if os.path.isdir(path):
            out[key] = path
    # The docs' pages: the module's (installed beside groomlab-cli), else a checkout's.
    here = os.path.dirname(os.path.abspath(__file__))
    for docs in (os.path.join(here, os.pardir, os.pardir, "docs", "pages"),
                 os.path.join(here, os.pardir, os.pardir, os.pardir, os.pardir, "web", "content", "docs")):
        if os.path.isdir(docs):
            out["docs"] = os.path.normpath(docs)
            break
    return out


class _Link(object):
    """The listener: a QTcpServer on 127.0.0.1, each connection's lines answered in turn."""

    def __init__(self, root=None):
        from groomlab.ui import qt
        network = qt.QtNetwork()
        # Owned by the application, not by Python: Qt deletes it (and its connections) when the
        # link closes, after the events in flight (deleteLater), never under a slot that runs.
        self.server = network.QTcpServer(qt.QtCore.QCoreApplication.instance())
        if not self.server.listen(network.QHostAddress(network.QHostAddress.LocalHost), 0):
            raise RuntimeError("the agent link cannot listen on 127.0.0.1: %s" % self.server.errorString())
        self.port = int(self.server.serverPort())
        folders = _folders()
        if root:
            folders["root"] = os.path.normpath(root)
        info = json.loads(cmds.groomLabAgent(start=True, port=self.port, folders=json.dumps(folders)))
        self.token, self.file = info["token"], info["file"]
        self.root = folders["root"]
        self.sockets = []
        self.server.newConnection.connect(self._accept)

    def _accept(self):
        while self.server.hasPendingConnections():
            sock = self.server.nextPendingConnection()
            sock.buffer = b""
            self.sockets.append(sock)
            sock.readyRead.connect(lambda s=sock: self._read(s))
            sock.disconnected.connect(lambda s=sock: self._gone(s))
            _changed()

    def _gone(self, sock):
        if sock in self.sockets:
            self.sockets.remove(sock)  # (the server's child: deleted with it)
            _changed()

    def _read(self, sock):
        sock.buffer += bytes(sock.readAll())
        while b"\n" in sock.buffer:
            line, sock.buffer = sock.buffer.split(b"\n", 1)
            line = line.strip()
            if not line:
                continue
            answer = self.answer(line.decode("utf-8", "replace"))
            sock.write((answer + "\n").encode("utf-8"))
            sock.flush()

    def answer(self, line):
        """One request line answered (the answer line)."""
        try:
            request = json.loads(line)
        except ValueError:
            request = None
        if not isinstance(request, dict):
            return self._command(line)
        params = request.get("params") or {}
        name = params.get("name") if request.get("method") == "tools/call" and isinstance(params, dict) else None
        if name in HOST_TOOLS:
            if request.get("token") != self.token:
                return self._command(line)  # (refused there, as every request without the token)
            arguments = params.get("arguments") or {}
            return json.dumps({"id": request.get("id"), "result": self.host_tool(name, arguments)})
        if name:
            cmds.undoInfo(openChunk=True, chunkName="Agent: %s" % name)
            try:
                return self._command(line, request.get("id"))
            finally:
                cmds.undoInfo(closeChunk=True)
        return self._command(line, request.get("id"))

    def _command(self, line, rid=None):
        try:
            return cmds.groomLabAgent(request=line)
        except RuntimeError as e:
            return json.dumps({"id": rid, "error": {"code": -32603, "message": "Maya: %s" % e}})

    def _call(self, name, arguments):
        """A tool's result through groomLabAgent (no undo chunk: for what changes nothing)."""
        line = json.dumps({"token": self.token, "id": 0, "method": "tools/call", "params": {"name": name, "arguments": arguments}})
        answer = json.loads(self._command(line))
        return answer.get("result") or {"isError": True, "content": [{"type": "text", "text": json.dumps(answer.get("error"))}]}

    def host_tool(self, name, arguments):
        """undo, redo and history on Maya's undo queue: the agent's steps are its "Agent: <tool>"
        chunks among the artist's own."""
        def result(value, error=False):
            return {"isError": error, "content": [{"type": "text", "text": json.dumps(value)}]}

        if name == "history":
            return result({"undo": cmds.undoInfo(q=True, undoName=True) or None, "redo": cmds.undoInfo(q=True, redoName=True) or None,
                           "note": "Maya keeps the undo queue: each agent tool call is one step named \"Agent: <tool>\" among the "
                                   "artist's own; undo and redo step through it (the artist's steps too)."})
        steps = arguments.get("steps", 1)
        if not isinstance(steps, int) or steps < 1 or steps > 64:
            return result({"error": "steps must be 1 to 64"}, True)
        query, step = ("undoName", cmds.undo) if name == "undo" else ("redoName", cmds.redo)
        if not cmds.undoInfo(q=True, state=True):
            return result({"error": "Maya's undo is off (Preferences > Undo)"}, True)
        names = []
        for _ in range(steps):
            label = cmds.undoInfo(q=True, **{query: True})
            if not label:
                break
            names.append(label)
            step()
        if not names:
            return result({"error": "nothing to %s" % name}, True)
        summary = self._call("evaluate", {"groom": arguments["groom"]} if arguments.get("groom") else {})
        out = {"undone" if name == "undo" else "redone": names}
        try:
            out["summary"] = json.loads(summary["content"][-1]["text"])
        except (KeyError, IndexError, ValueError, TypeError):
            pass
        return result(out)

    def close(self):
        sockets, self.sockets = self.sockets, []
        for sock in sockets:
            try:
                sock.disconnected.disconnect()
                sock.readyRead.disconnect()
            except (RuntimeError, TypeError):
                pass
            sock.abort()
        try:
            self.server.newConnection.disconnect()
        except (RuntimeError, TypeError):
            pass
        self.server.close()
        self.server.deleteLater()
        cmds.groomLabAgent(stop=True)


def start(root=None):
    """The agent link on (again: a new token and file); returns its discovery file."""
    global _link
    ensure_plugin()
    if _link is not None:
        stop()
    _link = _Link(root)
    _changed()
    return _link.file


def stop():
    """The link off: its connections closed, its discovery file removed."""
    global _link
    if _link is not None:
        try:
            _link.close()
        finally:
            _link = None
            _changed()


def running():
    return _link is not None


def clients():
    """How many agents are connected now."""
    return len(_link.sockets) if _link is not None else 0


def info():
    """{"running", "port", "file", "root", "clients"} of the link now."""
    if _link is None:
        return {"running": False, "clients": 0}
    return {"running": True, "port": _link.port, "file": _link.file, "root": _link.root, "clients": clients()}


def toggle(on):
    """The Groom Tools' switch: on or off."""
    if on:
        start()
    else:
        stop()
