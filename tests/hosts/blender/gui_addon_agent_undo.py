# SPDX-License-Identifier: Apache-2.0
"""The agent link in a Blender with its UI (not --background): the link's own timer answering
`groomlab-cli mcp --live blender`, and the Blender undo step it pushes from that timer. Run by
run_blender_test.py (a gui_ test: Blender with a window, a throwaway profile).

The script runs before Blender's event loop, so it drives itself from a timer, step by step: a
groom on the test head; Groom Tools > More > Agent Link on; the CLI (a thread reads its answers)
places guides and combs them; then Blender's own Edit > Undo (bpy.ops.ed.undo, as Ctrl+Z) must take
the comb back, then the guides, and Redo bring them again, the groom following each step
(tools.sync_undo). Blender quits with exit code 0 when every check passed, 1 otherwise (each check
printed)."""

import importlib
import json
import os
import queue
import subprocess
import sys
import threading
import time
import traceback

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blender_util as bu  # noqa: E402

LINK_DIR = os.path.join(bu.TMP, "agent_link")
os.environ["GROOMLAB_AGENT_LINK_DIR"] = LINK_DIR
TIMEOUT = 240.0

failures = []


def check(ok, what):
    print("%s %s" % ("PASS" if ok else "FAIL", what))
    sys.stdout.flush()
    if not ok:
        failures.append(what)


def window_context():
    wm = bpy.context.window_manager
    window = wm.windows[0]
    area = next((a for a in window.screen.areas if a.type == "VIEW_3D"), window.screen.areas[0])
    region = next((r for r in area.regions if r.type == "WINDOW"), area.regions[0])
    return bpy.context.temp_override(window=window, screen=window.screen, area=area, region=region)


class Agent(object):
    """groomlab-cli mcp --live blender, its answers read by a thread."""

    def __init__(self):
        self.proc = subprocess.Popen([bu.CLI, "mcp", "--live", "blender"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, creationflags=0x08000000 if os.name == "nt" else 0)
        self.lines = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()
        self.next_id = 1

    def _read(self):
        for line in iter(self.proc.stdout.readline, b""):
            self.lines.put(json.loads(line))

    def send(self, method, params):
        rid = self.next_id
        self.next_id += 1
        self.proc.stdin.write((json.dumps({"jsonrpc": "2.0", "id": rid, "method": method, "params": params}) + "\n").encode())
        self.proc.stdin.flush()
        return rid

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=30)
        except Exception:  # noqa: BLE001
            self.proc.kill()


def steps():
    """The probe, one yield per step (the timer comes back between them: the link's timer runs)."""
    pkg, m = bu.enable()
    groom, process = m["groom"], m["process"]
    tools = importlib.import_module(bu.MODULE + ".tools")
    agent_link = importlib.import_module(bu.MODULE + ".agent_link")
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o)
    head = bu.head_object()
    head_name = head.name
    with window_context():
        check(bpy.ops.groomlab.add_groom("INVOKE_DEFAULT") == {"FINISHED"}, "a groom on the head")
        bpy.ops.ed.undo_push(message="probe: the groom")
    yield 0.5

    def scalp():
        return bpy.data.objects[head_name]  # (Blender's undo replaces the objects: looked up each time)

    def guides():
        rt = groom.ensure(scalp())
        return process.call("get_state", session=rt.session, scalp=False, textures=False, texts=True)["texts"]["guideData"]

    def count():
        text = guides()
        return len(json.loads(text)["guides"]) if text else 0

    bpy.context.scene.groomlab_tools.agent_link = True
    check(agent_link.running(), "the link on")
    agent = Agent()
    try:
        rid = agent.send("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "probe", "version": "1"}})

        def answer(rid):
            end = time.monotonic() + 120
            while time.monotonic() < end:
                try:
                    got = agent.lines.get_nowait()
                except queue.Empty:
                    yield 0.05  # (Blender's loop runs the link's timer meanwhile)
                    continue
                if got.get("id") == rid:
                    holder.append(got)
                    return
            holder.append(None)

        holder = []
        yield from answer(rid)
        check(holder[-1] is not None and "blender" in json.dumps(holder[-1]).lower(), "the CLI found Blender's link")
        agent.proc.stdin.write(b'{"jsonrpc": "2.0", "method": "notifications/initialized"}\n')
        agent.proc.stdin.flush()

        def call(name, arguments):
            rid = agent.send("tools/call", {"name": name, "arguments": arguments})
            yield from answer(rid)
            got = holder[-1]
            ok = got is not None and not got.get("result", {}).get("isError", True)
            check(ok, "%s answered: %s" % (name, json.dumps(got)[:300] if not ok else "ok"))

        yield from call("brush_stroke", {"mode": "place", "from": "front_hairline", "via": ["top"], "to": "crown", "length": 12,
                                         "match_neighbors": False})
        placed, placed_count = guides(), count()
        check(placed_count > 2, "guides placed (%d)" % placed_count)
        yield from call("brush_stroke", {"mode": "comb", "from": "crown", "to": "back", "offset": 3, "radius": 6})
        combed = guides()
        check(combed != placed, "the guides combed")
        depth = scalp().groomlab_tools.undo_depth
        yield 0.2
        # Blender's own undo: the comb, then the guides; redo both.
        with window_context():
            bpy.ops.ed.undo()
        yield 0.3
        check(guides() == placed, "Blender's undo took the comb back (the groom at depth %d)" % scalp().groomlab_tools.undo_depth)
        with window_context():
            bpy.ops.ed.undo()
        yield 0.3
        check(count() == 0, "Blender's undo took the guides back")
        with window_context():
            bpy.ops.ed.redo()
        yield 0.3
        check(guides() == placed, "Blender's redo placed them again")
        with window_context():
            bpy.ops.ed.redo()
        yield 0.3
        check(guides() == combed and scalp().groomlab_tools.undo_depth == depth, "Blender's redo combed them again")
    finally:
        agent.close()
        agent_link.stop()


def run():
    gen = steps()

    def tick():
        try:
            wait = next(gen)
            if time.monotonic() - started > TIMEOUT:
                raise RuntimeError("the probe took too long")
            return wait
        except StopIteration:
            finish()
        except Exception:  # noqa: BLE001
            traceback.print_exc()
            failures.append("an exception")
            finish()
        return None

    started = time.monotonic()
    bpy.app.timers.register(tick, first_interval=1.0)


def finish():
    try:
        importlib.import_module(bu.MODULE + ".process").shutdown()
    except Exception:  # noqa: BLE001
        pass
    print("RESULT: %s" % ("OK" if not failures else "FAILED: " + "; ".join(failures)))
    sys.stdout.flush()
    os._exit(1 if failures else 0)


run()
