# SPDX-License-Identifier: GPL-3.0-or-later
"""The agent link: an AI agent grooms the scene's grooms (docs/BLENDER.md "Agent link").

While it is on (Groom Tools > More > Agent Link), Blender listens on 127.0.0.1 (a port of its own)
and the helper says so in a discovery file in the user's GroomLab folder
(Documents/GroomLab/agent_link/blender-<pid>.json, with a fresh token); `groomlab-cli mcp --live`
(the MCP server an agent's client starts) finds the file and forwards the agent's tool calls here.
The socket is polled from a timer on Blender's main thread (never blocking); each request line goes
to the helper (`agent_request`), which answers it with the agent tools on the grooms' sessions, its
edits steps of each groom's own undo stack. Here a groom an answer changed is shown again (its
strands, graph and lists, as after a Groom Tools edit) and gets one Blender undo step named
"Agent: <tool>", so Ctrl+Z takes it back as it takes back a stroke. Off by default.

    agent_link.start()      the link on (returns its discovery file)
    agent_link.stop()
    agent_link.running(), clients()
"""

import os
import socket

import bpy

from . import groom, process
from .helper_client import HelperError

POLL = 0.1         # seconds between polls while no agent is connected
POLL_BUSY = 0.02   # while one is
MAX_LINE = 64 * 1024 * 1024  # (agent_link.h kMaxLinkLine)

_link = None


def _folders():
    """The folder the agent reads and writes in: the .blend's, else Documents/GroomLab."""
    if bpy.data.filepath:
        root = os.path.dirname(os.path.abspath(bpy.data.filepath))
    else:
        root = os.path.join(os.path.expanduser("~"), "Documents", "GroomLab")
    return {"root": root}


def _grooms():
    """The scene's grooms by name (their scalp objects) -> their sessions, and the active one's name."""
    from .ops import active_groom
    by_name, scalps = {}, {}
    for scalp in groom.scalps():
        try:
            rt = groom.ensure(scalp)
        except HelperError as e:
            print("GroomLab: the agent link skips %s: %s" % (scalp.name, e.message))
            continue
        by_name[scalp.name] = rt.session
        scalps[rt.session] = scalp
    active = ""
    try:
        chosen = active_groom(bpy.context)
        active = chosen.name if chosen is not None else ""
    except (AttributeError, RuntimeError):
        pass
    return by_name, scalps, active


def _push_undo(label):
    """One Blender undo step (tools.push_undo_step) from the timer, where no window is current."""
    from . import tools
    if bpy.app.background:
        return
    wm = bpy.context.window_manager
    window = getattr(bpy.context, "window", None) or (wm.windows[0] if wm and wm.windows else None)
    if window is None:
        return
    with bpy.context.temp_override(window=window, screen=window.screen):
        tools.push_undo_step(label)


class _Client(object):
    def __init__(self, sock):
        self.sock = sock
        self.buffer = b""


class _Link(object):
    """The listener: a non-blocking socket on 127.0.0.1, its connections' lines answered in turn."""

    def __init__(self, root=None):
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            self.server.bind(("127.0.0.1", 0))
            self.server.listen(4)
            self.server.setblocking(False)
            self.port = self.server.getsockname()[1]
            self.folders = _folders()
            if root:
                self.folders["root"] = root
            self.token, self.file, self.generation = "", "", -1
            self._start_helper()
        except Exception:
            self.server.close()
            raise
        self.root = self.folders["root"]
        self.clients = []

    def _start_helper(self):
        """The helper's side on (again, with the same token, after the helper started again)."""
        _, _, active = _grooms()
        args = {"port": self.port, "pid": os.getpid(), "folders": self.folders, "groom": active}
        if self.token:
            args["token"] = self.token
        r = process.call("agent_start", **args)
        self.token, self.file = r["token"], r["file"]
        self.folders["root"] = r.get("root", self.folders["root"])
        self.generation = process.generation()

    def close(self):
        for c in self.clients:
            try:
                c.sock.close()
            except OSError:
                pass
        self.clients = []
        try:
            self.server.close()
        except OSError:
            pass
        if process.running():
            try:
                process.call("agent_stop")
            except HelperError:
                pass

    def poll(self):
        """New connections taken, each one's lines answered. Returns whether any came or went."""
        changed = False
        while True:
            try:
                sock, _addr = self.server.accept()
            except (BlockingIOError, InterruptedError):
                break
            except OSError:
                break
            sock.setblocking(False)
            self.clients.append(_Client(sock))
            changed = True
        for c in list(self.clients):
            gone = False
            while True:
                try:
                    data = c.sock.recv(1 << 20)
                except (BlockingIOError, InterruptedError):
                    break
                except OSError:
                    gone = True
                    break
                if not data:
                    gone = True
                    break
                c.buffer += data
            while b"\n" in c.buffer and c in self.clients:
                line, c.buffer = c.buffer.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                reply = self.answer(line.decode("utf-8", "replace")) + "\n"
                try:
                    c.sock.setblocking(True)
                    c.sock.sendall(reply.encode("utf-8"))
                    c.sock.setblocking(False)
                except OSError:
                    gone = True
                    break
            if len(c.buffer) > MAX_LINE:
                gone = True
            if gone and c in self.clients:
                try:
                    c.sock.close()
                except OSError:
                    pass
                self.clients.remove(c)
                changed = True
        return changed

    def answer(self, line):
        """One request line answered (the answer line); what it changed shown, one undo step each."""
        import json
        try:
            if not process.running() or self.generation != process.generation():
                process.client()
                self._start_helper()
            by_name, scalps, active = _grooms()
            if _is_render(line):
                return self._render(line, by_name, scalps, active)
            r = process.call("agent_request", line=line, grooms=by_name, groom=active)
        except HelperError as e:
            return json.dumps({"id": None, "error": {"code": -32603, "message": "Blender: %s" % e.message}})
        tool = r.get("tool")
        for ch in r.get("changed") or []:
            scalp = scalps.get(ch.get("session"))
            if scalp is not None:
                self._show(scalp, ch, tool)
        if tool:
            groom._redraw()
        return r["reply"]

    def _render(self, line, by_name, scalps, active):
        """render_frame: checked by the helper, rendered here by Blender's engine, answered by the helper."""
        begun = process.call("agent_render_begin", line=line, grooms=by_name, groom=active)
        if "reply" in begun:
            return begun["reply"]
        request = begun["render"]
        scalp = scalps.get(request.get("session"))
        done = {"png": "", "seconds": 0.0, "info": {}, "error": ""}
        if scalp is None:
            done["error"] = "no groom '%s' in Blender's scene" % request.get("groom")
        else:
            try:
                done.update(render_frame(scalp, request))
            except (RuntimeError, ValueError, OSError) as e:
                done["error"] = str(e)
        return process.call("agent_render_end", begun=begun, **done)["reply"]

    def _show(self, scalp, ch, tool):
        """A groom the agent changed: its graph, photos, strands and lists followed, one undo step."""
        from . import photos, tools
        scene = groom.scene_of(scalp)
        try:
            tools.follow_graph(scalp, ch)
            if ch.get("photosChanged"):
                photos.restore_from_groom(scalp, scene)
            tools.after_edit(scalp, scene, ch.get("undo"), refresh=True)
        except Exception as e:  # noqa: BLE001 - the edit stands; the display catches up on its next refresh
            print("GroomLab: %s did not show the agent's edit: %s" % (scalp.name, e))
        _push_undo("Agent: %s" % (tool or "edit"))


def _is_render(line):
    import json
    try:
        request = json.loads(line)
    except ValueError:
        return False
    params = request.get("params") if isinstance(request, dict) else None
    return (isinstance(params, dict) and request.get("method") == "tools/call" and params.get("name") == "render_frame")


ENGINES = {"cycles": "CYCLES", "eevee": "BLENDER_EEVEE"}
SAMPLES = {"cycles": {"draft": 16, "preview": 64, "final": 256}, "eevee": {"draft": 16, "preview": 32, "final": 64}}


def _framing_camera(scalp, scene, aspect):
    """A camera looking at the groom from the front (-Y), the scalp's bounds filling the frame."""
    from mathutils import Vector
    corners = [scalp.matrix_world @ Vector(c) for c in scalp.bound_box]
    lo = Vector([min(c[i] for c in corners) for i in range(3)])
    hi = Vector([max(c[i] for c in corners) for i in range(3)])
    middle, radius = (lo + hi) * 0.5, max((hi - lo).length * 0.5, 1e-4)
    radius *= 1.6  # (the hair reaches past the scalp)
    data = bpy.data.cameras.new("GroomLabAgentCamera")
    data.lens = 50.0
    data.clip_end = max(1000.0, radius * 100.0)
    data.clip_start = radius * 0.01
    cam = bpy.data.objects.new("GroomLabAgentCamera", data)
    scene.collection.objects.link(cam)
    import math
    fov = 2.0 * math.atan(data.sensor_width * 0.5 / data.lens)
    if aspect < 1.0:
        fov *= aspect
    distance = radius / math.tan(fov * 0.5)
    cam.location = middle + Vector((0.0, -distance, 0.0))
    cam.rotation_euler = (math.pi * 0.5, 0.0, 0.0)
    return cam


def render_frame(scalp, request):
    """A frame of the scene the groom is in, by Cycles or EEVEE, as a PNG: {"png", "seconds", "info"}.
    The camera named, else the scene's, else one framing the groom from the front; with no light in
    the scene a sun for the render; the scene's render settings put back after."""
    import tempfile
    import time
    scene = groom.scene_of(scalp)
    renderer = request.get("renderer")
    if renderer not in ENGINES:
        raise ValueError("Blender renders with %s" % ", ".join(sorted(ENGINES)))
    r = scene.render
    keep = {"engine": r.engine, "resolution_x": r.resolution_x, "resolution_y": r.resolution_y,
            "resolution_percentage": r.resolution_percentage, "filepath": r.filepath}
    image_keep = {"file_format": r.image_settings.file_format, "color_mode": r.image_settings.color_mode}
    camera_keep, frame_keep = scene.camera, scene.frame_current
    samples = int(request.get("samples") or SAMPLES[renderer][request.get("quality") or "draft"])
    samples_keep = None
    made = []
    started = time.time()
    try:
        r.engine = ENGINES[renderer]
        r.resolution_x, r.resolution_y, r.resolution_percentage = int(request["width"]), int(request["height"]), 100
        r.image_settings.file_format = "PNG"
        r.image_settings.color_mode = "RGB"
        if renderer == "cycles":
            samples_keep = scene.cycles.samples
            scene.cycles.samples = samples
        else:
            samples_keep = scene.eevee.taa_render_samples
            scene.eevee.taa_render_samples = samples
        name = request.get("camera") or ""
        if name:
            cam = bpy.data.objects.get(name)
            if cam is None or cam.type != "CAMERA":
                raise ValueError("no camera '%s' in Blender's scene" % name)
            scene.camera = cam
        elif scene.camera is None:
            scene.camera = _framing_camera(scalp, scene, float(request["width"]) / float(request["height"]))
            made.append(scene.camera)
        if not any(o.type == "LIGHT" and not o.hide_render for o in scene.objects):
            data = bpy.data.lights.new("GroomLabAgentLight", "SUN")
            data.energy = 3.0
            light = bpy.data.objects.new("GroomLabAgentLight", data)
            light.rotation_euler = (0.8, 0.0, -0.5)
            scene.collection.objects.link(light)
            made.append(light)
        if request.get("frame") is not None:
            scene.frame_set(int(round(float(request["frame"]))))
            groom.flush()  # (the groom evaluated at that frame)
        # (beside the .blend, as GroomLab's renders go: <folder>/groomlab/<name>/renders; untitled: the temp folder)
        folder = groom.asset_folder("renders") or tempfile.gettempdir()
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, "agent_%s_%s.png" % (renderer, time.strftime("%Y%m%d_%H%M%S")))
        r.filepath = path
        _run_render()
        if not os.path.isfile(path):
            raise RuntimeError("%s wrote no image" % renderer)
        return {"png": path.replace("\\", "/"), "seconds": time.time() - started,
                "info": {"samples": samples, "camera": scene.camera.name if scene.camera else ""}}
    finally:
        for key, value in keep.items():
            setattr(r, key, value)
        for key, value in image_keep.items():
            setattr(r.image_settings, key, value)
        if samples_keep is not None:
            if renderer == "cycles":
                scene.cycles.samples = samples_keep
            else:
                scene.eevee.taa_render_samples = samples_keep
        scene.camera = camera_keep
        if scene.frame_current != frame_keep:
            scene.frame_set(frame_keep)
        for obj in made:
            data = obj.data
            bpy.data.objects.remove(obj)
            if isinstance(data, bpy.types.Camera):
                bpy.data.cameras.remove(data)
            elif isinstance(data, bpy.types.Light):
                bpy.data.lights.remove(data)


def _run_render():
    """bpy.ops.render.render(write_still), from the timer too (a window given where none is current)."""
    wm = bpy.context.window_manager
    window = getattr(bpy.context, "window", None) or (wm.windows[0] if wm and wm.windows else None)
    if window is None or bpy.app.background:
        bpy.ops.render.render(write_still=True)
        return
    with bpy.context.temp_override(window=window, screen=window.screen):
        bpy.ops.render.render(write_still=True)


def _tick():
    link = _link
    if link is None:
        return None
    try:
        if link.poll():
            groom._redraw()
    except Exception as e:  # noqa: BLE001 - the link keeps listening
        print("GroomLab: the agent link: %s" % e)
    return POLL_BUSY if link.clients else POLL


def start(root=None):
    """The link on (a new token and discovery file); its file's path. Raises HelperError or OSError."""
    global _link
    stop()
    _link = _Link(root)
    if not bpy.app.timers.is_registered(_tick):
        bpy.app.timers.register(_tick, first_interval=0.0, persistent=True)
    groom._redraw()
    return _link.file


def stop():
    global _link
    link, _link = _link, None
    if link is not None:
        link.close()
        groom._redraw()
    if bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)


def running():
    return _link is not None


def clients():
    return len(_link.clients) if _link is not None else 0


def link():
    return _link


def poll_now():
    """One poll at once (the tests: a background Blender runs no timers between their steps)."""
    return _tick()


# ---- The setting (Groom Tools > More > Agent Link) and the badge's Stop ----------------------------

def get_on(self):
    return running()


def set_on(self, on):
    if bool(on) == running():
        return
    if not on:
        stop()
        return
    try:
        path = start()
        print("GroomLab: agent link on: an agent's groomlab-cli mcp --live finds this Blender (%s)" % path)
    except (HelperError, OSError) as e:
        print("GroomLab: the agent link didn't start: %s" % (e.message if isinstance(e, HelperError) else e))


class GROOMLAB_OT_agent_link_stop(bpy.types.Operator):
    """Stop the agent link: the agent is disconnected and its link file removed"""
    bl_idname = "groomlab.agent_link_stop"
    bl_label = "Stop the Agent Link"

    def execute(self, context):
        stop()
        return {"FINISHED"}


def draw_badge(row):
    """The header's Agent badge while an agent is connected, and its Stop."""
    if not clients():
        return
    badge = row.row(align=True)
    badge.alert = True
    badge.label(text="Agent", icon="LINKED")
    badge.operator(GROOMLAB_OT_agent_link_stop.bl_idname, text="", icon="X")


def register():
    bpy.utils.register_class(GROOMLAB_OT_agent_link_stop)


def unregister():
    stop()
    bpy.utils.unregister_class(GROOMLAB_OT_agent_link_stop)
