# SPDX-License-Identifier: GPL-3.0-or-later
"""GroomLab updates in Blender (Maya's groomlab.updates, for the extension): GroomLab > Help >
Check for Updates... shows this version and the newest one with what's new, and Update installs it
in place; a quiet daily check at startup puts a line in the Groom Tools when a newer one is out.

    check()          {"ok", "off", "reached", "newer", "installable", "current", "latest", "notes",
                     "info", "message"}: the newest release against this install (never raises)
    install(info)    downloads check()'s release, verifies its size and sha256, stops the helper and
                     installs the zip over this extension (Blender's Install from Disk, into the
                     same repository): {"ok", "message"}; a restart of Blender finishes it
    off_reason()     why updates are off here ("" when they are on)

The release (tools/publish_blender.py): https://downloads.getgroomlab.com/blender/latest.json names
{version, file, url, sha256, size, date, notes, blender, platform}. A check is one plain GET of that
file (User-Agent "GroomLab-Blender/<version>"): nothing about the machine or the user is sent. It
only runs while Blender's Allow Online Access is on (Preferences > System > Network), and never in
background mode or with GROOMLAB_NO_UPDATES=1 in the environment.

Users who prefer Blender's own updates can add https://downloads.getgroomlab.com/blender/index.json
as a remote repository instead (docs/BLENDER.md)."""

import hashlib
import json
import os
import re
import sys
import tempfile
import threading
import time

import bpy

LATEST_URL = "https://downloads.getgroomlab.com/blender/latest.json"
TIMEOUT = 10.0
DOWNLOAD_TIMEOUT = 300.0
CHECK_EVERY = 24 * 3600.0  # the startup check: at most once a day
RESTART_MESSAGE = "Restart Blender to finish"

# The startup check's answer when a newer release is out: (version, notes); None otherwise.
available = None
_installed = None  # the version Update installed this session (it waits for a restart)


class UpdateError(RuntimeError):
    pass


# ---- This install --------------------------------------------------------------------------------

def addon_dir():
    return os.path.dirname(os.path.abspath(__file__))


def repo_module():
    """The extensions repository this add-on was installed into ("user_default"...), from its package
    name (bl_ext.<repo>.groomlab); "" when it was not installed as an extension."""
    parts = (__package__ or "").split(".")
    return parts[1] if len(parts) == 3 and parts[0] == "bl_ext" else ""


def current_version():
    """The manifest's version ("0.35.0"), "" when it cannot be read."""
    try:
        with open(os.path.join(addon_dir(), "blender_manifest.toml"), encoding="utf-8") as f:
            for line in f:
                m = re.match(r'\s*version\s*=\s*"([^"]+)"', line)
                if m:
                    return m.group(1)
    except OSError:
        pass
    return ""


def platform():
    if sys.platform.startswith("win"):
        return "windows-x64"
    if sys.platform == "darwin":
        return "macos-arm64" if os.uname().machine == "arm64" else "macos-x64"
    return "linux-x64"


def off_reason():
    """Why updates are off here ("" when they are on)."""
    if os.environ.get("GROOMLAB_NO_UPDATES", "") not in ("", "0"):
        return "Updates are off (GROOMLAB_NO_UPDATES is set)"
    if not repo_module():
        return "This GroomLab was not installed as a Blender extension: update it by hand"
    if not bpy.app.online_access:
        return ("Blender's online access is off: turn on Allow Online Access in Preferences > System > Network "
                "to check for updates")
    return ""


def parse_version(text):
    m = re.match(r"^\s*(\d+)\.(\d+)(?:\.(\d+))?", text or "")
    return tuple(int(g or 0) for g in m.groups()) if m else None


def compare_versions(a, b):
    va, vb = parse_version(a), parse_version(b)
    if va is None or vb is None:
        raise ValueError("not a version: %r, %r" % (a, b))
    return (va > vb) - (va < vb)


# ---- The release ---------------------------------------------------------------------------------

def user_agent():
    return "GroomLab-Blender/%s" % (current_version() or "unknown")


def _host(url):
    from urllib.parse import urlparse
    return urlparse(url).hostname or ""


def _check_download_url(url, latest_url):
    """The release must come from where latest.json came from, over https (http only on localhost)."""
    from urllib.parse import urlparse
    got, origin = urlparse(url or ""), urlparse(latest_url)
    local = got.hostname in ("localhost", "127.0.0.1", "::1")
    if got.scheme not in ("https",) + (("http",) if local else ()) or got.netloc != origin.netloc:
        raise UpdateError("the release's address (%s) is not on %s" % (url, origin.netloc))


def fetch_latest(url=None, timeout=None):
    """latest.json as a dict (UpdateError when it cannot be read or is not a release)."""
    from urllib.error import URLError
    from urllib.request import Request, urlopen
    url = url or LATEST_URL
    request = Request(url, headers={"User-Agent": user_agent(), "Accept": "application/json"})
    try:
        with urlopen(request, timeout=TIMEOUT if timeout is None else timeout) as response:
            body = response.read(1 << 20)
    except (URLError, OSError, ValueError) as e:
        if getattr(e, "code", None):  # an HTTP error: the server answered
            raise UpdateError("%s answered HTTP %s for the release information" % (_host(url), e.code))
        raise UpdateError("Couldn't reach %s (%s)" % (_host(url), getattr(e, "reason", e)))
    try:
        info = json.loads(body.decode("utf-8"))
    except ValueError:
        raise UpdateError("%s sent something that is not GroomLab's release information" % _host(url))
    if not isinstance(info, dict) or parse_version(str(info.get("version", ""))) is None:
        raise UpdateError("%s sent no release version" % _host(url))
    if not re.match(r"^[0-9a-fA-F]{64}$", str(info.get("sha256", ""))) or not info.get("url"):
        raise UpdateError("the release information has no download or checksum")
    return info


def _result(**values):
    out = {"ok": False, "off": False, "reached": False, "newer": False, "installable": False, "restart": False,
           "current": "", "latest": "", "notes": "", "info": None, "message": ""}
    out.update(values)
    return out


def check(url=None, timeout=None):
    """The newest release against this install (never raises; see the module's docstring)."""
    current = current_version()
    why = off_reason()
    if why:
        return _result(off=True, current=current, message=why)
    try:
        info = fetch_latest(url, timeout)
    except UpdateError as e:
        return _result(reached=not str(e).startswith("Couldn't reach"), current=current, message=str(e))
    latest = str(info["version"])
    try:
        newer = compare_versions(latest, current) > 0
    except ValueError:
        newer = False
    notes = str(info.get("notes") or "").strip()
    wanted = str(info.get("platform") or "")
    needs = parse_version(str(info.get("blender") or "")) or (0, 0, 0)
    fits_platform = not wanted or wanted == platform()
    fits_blender = tuple(bpy.app.version) >= needs
    waiting = newer and _installed == latest
    if not newer:
        message = "GroomLab %s is the latest version" % current
    elif waiting:
        message = "GroomLab %s is installed. %s." % (latest, RESTART_MESSAGE)
    elif not fits_platform:
        message = "GroomLab %s is out for %s (this is %s)" % (latest, wanted, platform())
    elif not fits_blender:
        message = "GroomLab %s needs Blender %s or later" % (latest, ".".join(str(v) for v in needs))
    else:
        message = "GroomLab %s is available (you have %s)" % (latest, current)
    return _result(ok=True, reached=True, newer=newer, installable=newer and fits_platform and fits_blender and not waiting,
                   restart=bool(waiting), current=current, latest=latest, notes=notes, info=info, message=message)


def download(info, latest_url=None):
    """The release's zip in a new temporary folder (UpdateError: unreachable, not the size or the
    checksum latest.json names)."""
    from urllib.error import URLError
    from urllib.request import Request, urlopen
    url = str(info.get("url") or "")
    _check_download_url(url, latest_url or LATEST_URL)
    folder = tempfile.mkdtemp(prefix="groomlab_update_")
    path = os.path.join(folder, os.path.basename(url.split("?")[0]) or "GroomLab-Blender.zip")
    digest = hashlib.sha256()
    size = 0
    try:
        with urlopen(Request(url, headers={"User-Agent": user_agent()}), timeout=DOWNLOAD_TIMEOUT) as response, \
                open(path, "wb") as out:
            while True:
                chunk = response.read(1 << 20)
                if not chunk:
                    break
                digest.update(chunk)
                size += len(chunk)
                out.write(chunk)
    except (URLError, OSError, ValueError) as e:
        raise UpdateError("Couldn't download %s (%s)" % (url, getattr(e, "reason", e)))
    if info.get("size") and int(info["size"]) != size:
        raise UpdateError("the download is %d bytes, not the %d the release names" % (size, int(info["size"])))
    if digest.hexdigest().lower() != str(info["sha256"]).lower():
        raise UpdateError("the download's checksum does not match the release's")
    return path


def install(info, latest_url=None):
    """Downloads `info`'s release and installs it over this extension (see the module's docstring)."""
    global _installed, available
    why = off_reason()
    if why:
        return {"ok": False, "message": why}
    try:
        path = download(info, latest_url)
    except UpdateError as e:
        return {"ok": False, "message": str(e)}
    from . import process
    process.shutdown()  # Windows locks the running helper's files: the install replaces them
    try:
        r = bpy.ops.extensions.package_install_files(filepath=path, repo=repo_module(), enable_on_install=True)
    except (RuntimeError, TypeError) as e:
        return {"ok": False, "message": "Blender couldn't install %s: %s" % (os.path.basename(path), e)}
    if "FINISHED" not in r:
        return {"ok": False, "message": "Blender couldn't install %s" % os.path.basename(path)}
    _installed = str(info.get("version") or "")
    available = None
    return {"ok": True, "message": "GroomLab %s is installed. %s." % (_installed, RESTART_MESSAGE)}


# ---- The startup check ---------------------------------------------------------------------------

def _state_path():
    try:
        folder = bpy.utils.extension_path_user(__package__, path="", create=True)
    except (ValueError, RuntimeError, AttributeError):
        return ""
    return os.path.join(folder, "updates.json")


def _read_state():
    path = _state_path()
    if not path:
        return {}
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def _write_state(state):
    path = _state_path()
    if not path:
        return
    try:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(state, f)
    except OSError:
        pass


def startup_check_enabled():
    try:
        return bool(bpy.context.preferences.addons[__package__].preferences.check_updates)
    except (KeyError, AttributeError):
        return True


def check_due(now=None):
    """Whether the startup check runs now: on, updates on here, not in background mode, and a day
    since the last one."""
    if bpy.app.background or off_reason() or not startup_check_enabled():
        return False
    last = float(_read_state().get("checked", 0.0) or 0.0)
    return (time.time() if now is None else now) - last >= CHECK_EVERY


def _show(result):
    global available
    available = (result["latest"], result["notes"]) if result.get("installable") else None
    try:
        for window in bpy.context.window_manager.windows:
            for area in window.screen.areas:
                if area.type == "VIEW_3D":
                    area.tag_redraw()
    except (AttributeError, ReferenceError):
        pass
    return None  # (a timer's: once)


def startup_check():
    """A day since the last check: one in a thread, its answer shown in the Groom Tools (a timer
    hands it back to Blender's thread)."""
    if not check_due():
        return

    def run():
        result = check()
        if result["ok"]:
            state = _read_state()
            state["checked"] = time.time()
            _write_state(state)
        bpy.app.timers.register(lambda: _show(result), first_interval=0.0)

    threading.Thread(target=run, name="GroomLab update check", daemon=True).start()


# ---- The dialog ----------------------------------------------------------------------------------

def _wrap(layout, text, width=78):
    import textwrap
    for paragraph in text.splitlines() or [""]:
        lines = textwrap.wrap(paragraph, width) or [""]
        for line in lines:
            layout.label(text=line)


class GROOMLAB_OT_check_updates(bpy.types.Operator):
    """This version and the newest one with what's new; update in place (restart Blender to finish)"""
    bl_idname = "groomlab.check_updates"
    bl_label = "Check for Updates"

    result = None  # check()'s answer, for draw and execute (class-wide: the dialog's operator is a copy)

    def invoke(self, context, event):
        type(self).result = check()
        r = type(self).result
        return context.window_manager.invoke_props_dialog(self, width=520, title="GroomLab Updates",
                                                          confirm_text="Update" if r["installable"] else "OK")

    def draw(self, context):
        r = type(self).result or _result(message="Not checked yet")
        layout = self.layout
        layout.label(text=r["message"], icon="INFO" if r["ok"] or r["off"] else "ERROR")
        if r["current"]:
            layout.label(text="This version: %s" % r["current"])
        if r["newer"] and r["notes"]:
            box = layout.box()
            box.label(text="What's new in %s" % r["latest"])
            _wrap(box.column(align=True), r["notes"])
        if r["installable"]:
            layout.label(text="Update downloads it and installs it over this one; %s." % RESTART_MESSAGE.lower())

    def execute(self, context):
        r = type(self).result
        if not r or not r["installable"]:
            return {"FINISHED"}
        out = install(r["info"])
        self.report({"INFO"} if out["ok"] else {"ERROR"}, out["message"])
        return {"FINISHED"} if out["ok"] else {"CANCELLED"}


classes = (GROOMLAB_OT_check_updates,)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    if not bpy.app.background:
        bpy.app.timers.register(startup_check, first_interval=5.0)  # after Blender has started


def unregister():
    if bpy.app.timers.is_registered(startup_check):
        bpy.app.timers.unregister(startup_check)
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
