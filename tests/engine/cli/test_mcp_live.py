# SPDX-License-Identifier: Apache-2.0
"""`groomlab-cli mcp --live`: the server forwarding to a host's agent link (engine/editor agent_link.h).

    python test_mcp_live.py <groomlab-cli> <work folder>

A fake host (a thread here) listens on 127.0.0.1 and writes its discovery file into a folder of its
own (GROOMLAB_AGENT_LINK_DIR), answering the protocol: tools/list and tools/call, refusing requests
without its token. Checked: --live finds it (named and newest), says so in its instructions, forwards
tools/list and tools/call (images and errors as the host gave them, the token in every request),
says clearly when the host went away and finds it again when its link comes back; a stale file (a
dead process) is ignored and removed; --live with no host works headless and says so; plain mcp
never looks for hosts. Standard library only.
"""

import json
import os
import shutil
import socket
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_mcp import Client  # noqa: E402


class FakeHost:
    """A host's agent link: tools/list and tools/call answered for one made-up tool and one real name."""

    def __init__(self, folder, host="maya", token="0123456789abcdef0123456789abcdef", started=None):
        self.folder, self.host, self.token = folder, host, token
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(4)
        self.port = self.server.getsockname()[1]
        self.requests = []
        self.connections = []
        self.running = True
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()
        self.file = os.path.join(folder, "%s-%d.json" % (host, os.getpid()))
        with open(self.file, "w", encoding="utf-8") as f:
            json.dump({"host": host, "pid": os.getpid(), "port": self.port, "token": token, "groom": "groomShape1",
                       "started": started if started is not None else time.time(), "version": "test"}, f)

    def serve(self):
        while self.running:
            try:
                conn, _ = self.server.accept()
            except OSError:
                return
            self.connections.append(conn)
            threading.Thread(target=self.talk, args=(conn,), daemon=True).start()

    def talk(self, conn):
        buffer = b""
        while True:
            try:
                data = conn.recv(65536)
            except OSError:
                return
            if not data:
                return
            buffer += data
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                request = json.loads(line)
                self.requests.append(request)
                answer = self.answer(request)
                try:
                    conn.sendall((json.dumps(answer) + "\n").encode("utf-8"))
                except OSError:
                    return

    def answer(self, request):
        rid = request.get("id")
        if request.get("token") != self.token:
            return {"id": rid, "error": {"code": -32001, "message": "wrong or missing token"}}
        if request["method"] == "tools/list":
            return {"id": rid, "result": {"tools": [
                {"name": "evaluate", "title": "Evaluate", "description": "the host's", "inputSchema": {"type": "object", "properties": {}}},
                {"name": "host_only", "title": "Host only", "description": "a tool the headless server lacks",
                 "inputSchema": {"type": "object", "properties": {}}}]}}
        if request["method"] == "tools/call":
            name = request["params"]["name"]
            if name == "host_only":
                return {"id": rid, "result": {"isError": False, "content": [
                    {"type": "image", "data": "iVBORw0KGgo=", "mimeType": "image/png"},
                    {"type": "text", "text": json.dumps({"host": self.host, "arguments": request["params"].get("arguments", {})})}]}}
            return {"id": rid, "result": {"isError": True, "content": [{"type": "text", "text": json.dumps({"error": "nothing open"})}]}}
        return {"id": rid, "error": {"code": -32601, "message": "unknown method"}}

    def stop(self):
        self.running = False
        self.server.close()
        for c in self.connections:
            try:
                c.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            c.close()
        if os.path.exists(self.file):
            os.remove(self.file)


class LiveClient(Client):
    def __init__(self, exe, root, args, env):
        self.log = open(os.path.join(root, "server_stderr_%d.txt" % id(self)), "wb")
        self.proc = subprocess.Popen([exe, "mcp", "--root", root] + args, cwd=root, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=self.log, env=env)
        self.next_id = 1


def start(exe, root, args, env):
    c = LiveClient(exe, root, args, env)
    init = c.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}})
    return c, init["result"]["instructions"]


def main():
    exe, root = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    shutil.rmtree(root, ignore_errors=True)
    links = os.path.join(root, "agent_link")
    os.makedirs(links)
    env = dict(os.environ, GROOMLAB_AGENT_LINK_DIR=links)

    # No host: --live works headless and says so (instructions, and the first answer).
    c, instructions = start(exe, root, ["--live"], env)
    try:
        assert "headless" in instructions, instructions
        names = {t["name"] for t in c.request("tools/list")["result"]["tools"]}
        assert "new_groom" in names and "host_only" not in names
        _, result = c.call("list_presets")
        assert any("headless" in x.get("text", "") for x in result["content"]), result
    finally:
        assert c.close() == 0

    # A stale file (a process that is gone) is ignored and removed; two hosts: the newest is taken,
    # or the one named.
    stale = os.path.join(links, "blender-999999.json")
    with open(stale, "w", encoding="utf-8") as f:
        json.dump({"host": "blender", "pid": 999999, "port": 1, "token": "x", "started": time.time() + 100}, f)
    old = FakeHost(links, host="app", token="a" * 32, started=time.time() - 50)
    host = FakeHost(links, host="maya")
    try:
        c, instructions = start(exe, root, ["--live"], env)
        try:
            assert "Live" in instructions and "maya" in instructions, instructions
            assert not os.path.exists(stale), "the stale link file is removed"
            names = [t["name"] for t in c.request("tools/list")["result"]["tools"]]
            assert names == ["evaluate", "host_only"], names
            data, result = c.call("host_only", {"x": 1})
            assert data == {"host": "maya", "arguments": {"x": 1}}, data
            assert result["content"][0]["type"] == "image", result
            data, _ = c.call("evaluate", {}, expect_error=True)
            assert data == {"error": "nothing open"}, data
            assert all(r.get("token") == host.token for r in host.requests), "the token in every request"
            assert c.request("tools/call", {"name": "new_groom", "arguments": {}})["error"]["code"] == -32602  # not the host's
        finally:
            assert c.close() == 0
        c, instructions = start(exe, root, ["--live", "app"], env)
        try:
            assert "app" in instructions, instructions
            data, _ = c.call("host_only")
            assert data["host"] == "app", data
            # The host goes away: said plainly; it comes back (a new link): found again.
            old.stop()
            data, _ = c.call("host_only", expect_error=True)
            assert "went away" in data["error"], data
            old = FakeHost(links, host="app", token="b" * 32)
            data, _ = c.call("host_only")
            assert data["host"] == "app", data
        finally:
            assert c.close() == 0
    finally:
        host.stop()
        old.stop()

    # Plain mcp never looks for a host.
    host = FakeHost(links, host="maya")
    try:
        c, instructions = start(exe, root, [], env)
        try:
            assert "Live" not in instructions and not host.requests
        finally:
            assert c.close() == 0
    finally:
        host.stop()
    print("mcp live: ok")


if __name__ == "__main__":
    main()
