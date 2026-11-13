# SPDX-License-Identifier: Apache-2.0
"""GroomLab updates in Maya: GroomLab > Help > Check for Updates..., and a quiet daily check at startup.

    groomlab.updates.check()          {"ok", "reached", "newer", "current", "latest", "notes", "info",
                                      "message"}: the newest release against this install's
    groomlab.updates.install(info)    downloads check()'s release, verifies its sha256, unpacks it
                                      beside this install and points GroomLab.mod at it: {"ok",
                                      "message", "folder"}; Maya's restart finishes it
    groomlab.updates.off_reason()     why updates are off for this install ("" when they are on)

The release (tools/publish_release.py): https://downloads.getgroomlab.com/maya/latest.json names
{version, file, url, sha256, size, date, notes, maya, mayaVersions, mayaBeta, platform}: one zip holds
a build for every Maya in mayaVersions (mayaBeta: those built but not yet tested in their Maya); maya
is the tested one, what releases before mayaVersions named alone (and what their checks still read).
A check is one plain GET of that file (User-Agent "GroomLab/<version>"): nothing about the machine or
the user is sent.

The layout (tools/package_release.py, install_groomlab.py): a folder GroomLab-<version>/ holds the
module GroomLab/ (with VERSION.txt; Maya 2022's binaries at its root, every other Maya's in
maya<year>/) and the bundled GroomLab.mod (a MAYAVERSION section per Maya); the user's modules folder holds a
GroomLab.mod pointing at the module. An update unpacks the new release as a sibling folder
GroomLab-<new version>/, copies GroomLab.mod to GroomLab.mod.bak and rewrites GroomLab.mod
(atomically) to point at the new module. The running plugin's files are never touched (Windows locks
the .mll). The previous folder stays until the new version has loaded once (the next start), then
goes; until then, renaming GroomLab.mod.bak back to GroomLab.mod goes back to it.

Developer installs are never touched: a module without VERSION.txt (cmake --install, the source or
build tree: only tools/package_release.py writes one), a module inside a source checkout or build
folder (a CMakeLists.txt or CMakeCache.txt above it), or GROOMLAB_NO_UPDATES=1 in the environment.
"""

import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
import threading
import time
import zipfile

import groomlab_core

# The plugin a release zip holds for this platform (the zips are per platform).
PLUGIN_FILE = "GroomLab.bundle" if sys.platform == "darwin" else "GroomLab.mll"
# Each platform's latest release (tools/publish_release.py): Windows' latest.json, macOS' latest-macos.json.
LATEST_URL = "https://downloads.getgroomlab.com/maya/%s" % ("latest-macos.json" if sys.platform == "darwin" else "latest.json")
TIMEOUT = 5            # seconds the check may take
DOWNLOAD_TIMEOUT = 30  # seconds a download may stall
CHECK_INTERVAL = 24 * 3600  # the startup check: at most once a day
STARTUP_VAR = "groomLabCheckForUpdates"  # optionVar: 0 turns the startup check off
DEV_MESSAGE = "Updates are off for this install (developer build)"
ENV_MESSAGE = "Updates are off for this install (GROOMLAB_NO_UPDATES)"
RESTART_MESSAGE = "Restart Maya to finish"

# Tests point these elsewhere: a fake install, its modules folder, a state folder, a local server.
MODULE_DIR = None  # the running module (None: the folder this package was loaded from)
MOD_DIRS = None    # folders holding .mod files (None: MAYA_MODULE_PATH and the user's modules folders)
STATE_DIR = None   # updates.json's folder (None: GROOMLAB_UPDATES_DIR, else the user's GroomLab folder)
STATE_FILE = "updates.json"


class UpdateError(RuntimeError):
    """An update refused or failed: the message says why."""


# ---- versions ------------------------------------------------------------------------------------

def parse_version(text):
    """A semantic version's parts ("1.2.0", "v1.3.0-beta.2", "1.2.0+build.5"), None when it is not
    one (the core's: groomlab_core.parse_version, as the app reads versions)."""
    return groomlab_core.parse_version(text or "")


def compare_versions(a, b):
    """-1, 0 or 1 as version a is older than, the same as or newer than b (ValueError: not versions;
    the core's: groomlab_core.compare_versions). A pre-release sorts before its release, beta.2
    before beta.10; missing parts count as 0 ("1.2" == "1.2.0")."""
    return groomlab_core.compare_versions(a or "", b or "")


# ---- this install --------------------------------------------------------------------------------

def _norm(path):
    return os.path.normpath(os.path.abspath(path)).replace("\\", "/")


def _same(a, b):
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


def module_dir():
    """The running GroomLab module: <module>/scripts/groomlab/updates.py's <module>."""
    if MODULE_DIR:
        return _norm(MODULE_DIR)
    return _norm(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def installed_version(module=None):
    """The packaged install's version (its VERSION.txt: "GroomLab <version> (<commit>, <config>)",
    written by tools/package_release.py), "" without one: a developer build."""
    try:
        with open(os.path.join(module or module_dir(), "VERSION.txt")) as f:
            line = f.readline()
    except (IOError, OSError):
        return ""
    match = re.match(r"^GroomLab (\S+) \(", line)
    return match.group(1) if match and parse_version(match.group(1)) else ""


def current_version(module=None):
    """This install's version: VERSION.txt's, else the plugin's (a developer build)."""
    version = installed_version(module)
    if version:
        return version
    try:
        from maya import cmds
        return cmds.groomLabVersion() or ""
    except Exception:
        return ""


def _in_source_or_build_tree(folder):
    """A module inside a checkout (CMakeLists.txt) or a build folder (CMakeCache.txt) above it."""
    path = _norm(folder)
    while True:
        if any(os.path.exists(os.path.join(path, m)) for m in ("CMakeLists.txt", "CMakeCache.txt")):
            return True
        parent = os.path.dirname(path)
        if parent == path:
            return False
        path = parent


def off_reason(module=None):
    """Why this install never updates itself ("" when it may): GROOMLAB_NO_UPDATES, or a developer
    build (no VERSION.txt from packaging, or inside a source or build tree)."""
    if os.environ.get("GROOMLAB_NO_UPDATES", "").strip() not in ("", "0"):
        return ENV_MESSAGE
    module = module or module_dir()
    if not installed_version(module) or _in_source_or_build_tree(module):
        return DEV_MESSAGE
    return ""


def maya_major():
    """The running Maya's version year (2022), 0 when unknown."""
    try:
        from maya import cmds
        return int(cmds.about(apiVersion=True)) // 10000
    except Exception:
        return 0


def release_mayas(info):
    """The Maya versions (strings) a release has a build for: latest.json's mayaVersions, else its
    maya (a release from before one zip held every Maya); [] when it names none (any Maya)."""
    versions = info.get("mayaVersions")
    if isinstance(versions, (list, tuple)) and versions:
        return [str(v) for v in versions]
    return [str(info["maya"])] if info.get("maya") else []


def beta_for(info, running=None):
    """Whether the release's build for the running Maya is a beta (built, not yet tested in it)."""
    running = maya_major() if running is None else running
    return bool(running) and str(running) in [str(v) for v in (info.get("mayaBeta") or [])]


def mod_dirs():
    """The folders Maya reads .mod files from: MAYA_MODULE_PATH, the user's modules folders."""
    if MOD_DIRS is not None:
        return [_norm(d) for d in MOD_DIRS]
    dirs = [d for d in os.environ.get("MAYA_MODULE_PATH", "").split(os.pathsep) if d.strip()]
    try:
        from maya import cmds
        app = cmds.internalVar(userAppDir=True)
        dirs += [os.path.join(app, "modules"), os.path.join(app, cmds.about(version=True), "modules")]
    except Exception:
        pass
    out = []
    for d in dirs:
        d = _norm(d)
        if os.path.isdir(d) and d not in out:
            out.append(d)
    return out


# "+ [MAYAVERSION:2022] [PLATFORM:win64] GroomLab <version> <path>": the path is the rest of the line.
_MOD_LINE = re.compile(r"^(\+\s+(?:\S+:\S*\s+)*GroomLab\s+(\S+)\s+)(.+?)\s*$")


def _mod_module_paths(mod_file):
    """The module folders a .mod file's GroomLab entries point at (absolute)."""
    try:
        with open(mod_file) as f:
            lines = f.read().splitlines()
    except (IOError, OSError):
        return []
    out = []
    for line in lines:
        match = _MOD_LINE.match(line)
        if match:
            path = match.group(3).strip().strip('"')
            if not os.path.isabs(path):
                path = os.path.join(os.path.dirname(mod_file), path)
            out.append(_norm(path))
    return out


def find_mod_file(module=None):
    """The .mod file whose GroomLab entry points at this module, "" when none does."""
    module = module or module_dir()
    for folder in mod_dirs():
        try:
            names = sorted(os.listdir(folder))
        except OSError:
            continue
        for name in names:
            path = os.path.join(folder, name)
            if name.lower().endswith(".mod") and os.path.isfile(path):
                if any(_same(p, module) for p in _mod_module_paths(path)):
                    return _norm(path)
    return ""


# ---- the state: the last check, an update waiting for its restart ---------------------------------

def state_dir():
    """The user's GroomLab folder : %APPDATA%/GroomLab on Windows."""
    if STATE_DIR:
        return STATE_DIR
    if os.environ.get("GROOMLAB_UPDATES_DIR"):
        return os.environ["GROOMLAB_UPDATES_DIR"]
    if sys.platform == "win32" and os.environ.get("APPDATA"):
        return os.path.join(os.environ["APPDATA"], "GroomLab")
    if sys.platform == "darwin":
        return os.path.expanduser("~/Library/Application Support/GroomLab")
    return os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "GroomLab")


def read_state():
    try:
        with open(os.path.join(state_dir(), STATE_FILE)) as f:
            state = json.load(f)
        return state if isinstance(state, dict) else {}
    except (IOError, OSError, ValueError):
        return {}


def _atomic_write(path, text):
    """Writes path through a temporary file beside it and a rename: never half written."""
    folder = os.path.dirname(path)
    fd, tmp = tempfile.mkstemp(prefix=".%s." % os.path.basename(path), suffix=".tmp", dir=folder)
    try:
        with os.fdopen(fd, "w") as f:
            f.write(text)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise


def write_state(state):
    folder = state_dir()
    if not os.path.isdir(folder):
        os.makedirs(folder)
    _atomic_write(os.path.join(folder, STATE_FILE), json.dumps(state, indent=2, sort_keys=True) + "\n")


def _update_state(**changes):
    state = read_state()
    for key, value in changes.items():
        if value is None:
            state.pop(key, None)
        else:
            state[key] = value
    write_state(state)
    return state


def check_due(now=None):
    """The startup check runs at most once a day (the last one's time in updates.json)."""
    now = time.time() if now is None else now
    try:
        last = float(read_state().get("lastCheck", 0))
    except (TypeError, ValueError):
        last = 0
    return now - last >= CHECK_INTERVAL or last > now  # a clock set back: check again


def mark_checked(now=None):
    _update_state(lastCheck=time.time() if now is None else now)


def startup_check_enabled():
    """The startup check's preference (optionVar groomLabCheckForUpdates, on by default)."""
    try:
        from maya import cmds
        if cmds.optionVar(exists=STARTUP_VAR):
            return bool(cmds.optionVar(query=STARTUP_VAR))
    except Exception:
        pass
    return True


def set_startup_check(on):
    from maya import cmds
    cmds.optionVar(intValue=(STARTUP_VAR, 1 if on else 0))


# ---- the network ---------------------------------------------------------------------------------

def _urllib():
    try:
        from urllib.request import Request, urlopen
        from urllib.error import URLError
        from urllib.parse import urlparse
    except ImportError:  # Python 2 (Maya 2020 and older)
        from urllib2 import Request, urlopen, URLError
        from urlparse import urlparse
    return Request, urlopen, URLError, urlparse


# The system's trusted certificates, for a Python that found none of its own: Maya's on macOS (its
# OpenSSL looks where no bundle is, so every https site fails with CERTIFICATE_VERIFY_FAILED), and
# Linux distributions' usual places.
CA_BUNDLES = ("/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
              "/etc/ssl/ca-bundle.pem", "/opt/homebrew/etc/openssl@3/cert.pem", "/usr/local/etc/openssl@3/cert.pem",
              "/usr/local/etc/openssl/cert.pem")


def ssl_context():
    """A context that verifies the server (never one that does not): Python's default, with the
    system's trusted certificates added when it holds none (certifi's when it is there, else the
    first bundle of CA_BUNDLES that loads). None on a Python without ssl."""
    try:
        import ssl
        context = ssl.create_default_context()
    except (ImportError, AttributeError):
        return None
    if context.cert_store_stats().get("x509_ca", 0):
        return context
    # (None loaded yet: a Python whose OpenSSL looks in a folder loads them as it needs them, so
    # adding a bundle costs nothing there; Maya's on macOS has neither.)
    bundles = []
    try:
        import certifi
        bundles.append(certifi.where())
    except Exception:
        pass
    for path in bundles + list(CA_BUNDLES):
        if path and os.path.isfile(path):
            try:
                context.load_verify_locations(cafile=path)
                return context
            except Exception:
                continue
    return context


def _open(request, timeout):
    """urlopen with ssl_context() (the server's certificate verified as ever)."""
    urlopen = _urllib()[1]
    context = ssl_context()
    if context is None:
        return urlopen(request, timeout=timeout)
    return urlopen(request, timeout=timeout, context=context)


def user_agent(module=None):
    return "GroomLab/%s" % (current_version(module) or "unknown")


def _host(url):
    return _urllib()[3](url).hostname or ""


def _check_download_url(url, latest_url):
    """The release must come from where latest.json came from, over https (http only on localhost)."""
    urlparse = _urllib()[3]
    got, origin = urlparse(url or ""), urlparse(latest_url)
    local = got.hostname in ("localhost", "127.0.0.1", "::1")
    if got.scheme not in ("https",) + (("http",) if local else ()) or got.netloc != origin.netloc:
        raise UpdateError("the release's address (%s) is not on %s" % (url, origin.netloc))


def fetch_latest(url=None, module=None, timeout=None):
    """latest.json as a dict (UpdateError when it cannot be read or is not a release)."""
    Request, _, URLError, _ = _urllib()
    url = url or LATEST_URL
    request = Request(url, headers={"User-Agent": user_agent(module), "Accept": "application/json"})
    try:
        response = _open(request, TIMEOUT if timeout is None else timeout)
        try:
            body = response.read(1 << 20)
        finally:
            response.close()
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


def check(url=None, module=None, timeout=None):
    """The newest release against this install. Never raises: {"ok" (an answer came), "off" (updates
    are off here: "message" says why), "reached", "newer", "installable" (newer and for this Maya),
    "current", "latest", "notes", "info" (latest.json for install()), "message"}."""
    current = current_version(module)
    why = off_reason(module)
    if why:
        return _result(off=True, current=current, message=why)
    try:
        info = fetch_latest(url, module, timeout)
    except UpdateError as e:
        reached = not str(e).startswith("Couldn't reach")
        return _result(reached=reached, current=current, message=str(e))
    latest = str(info["version"])
    try:
        newer = compare_versions(latest, current) > 0
    except ValueError:
        newer = False
    notes = str(info.get("notes") or "").strip()
    wanted = release_mayas(info)
    running = maya_major()
    fits = not wanted or not running or str(running) in wanted
    waiting = newer and waiting_for_restart(latest, module)
    if not newer:
        message = "GroomLab %s is the latest version" % current
    elif waiting:
        fits = False
        message = "GroomLab %s is installed. %s." % (latest, RESTART_MESSAGE)
    elif not fits:
        message = "GroomLab %s is out for Maya %s (this is Maya %s)" % (latest, ", ".join(wanted), running)
    else:
        message = "GroomLab %s is available (you have %s)" % (latest, current)
        if beta_for(info, running):
            message += "; its Maya %s build is a beta (built, not yet tested in Maya %s)" % (running, running)
    return _result(ok=True, reached=True, newer=newer, installable=newer and fits, restart=bool(waiting),
                   current=current, latest=latest, notes=notes, info=info, message=message)


def _download(url, path, sha256, size, progress=None, module=None):
    Request, _, URLError, _ = _urllib()
    digest = hashlib.sha256()
    done = 0
    try:
        response = _open(Request(url, headers={"User-Agent": user_agent(module)}), DOWNLOAD_TIMEOUT)
        try:
            total = int(response.headers.get("Content-Length") or size or 0)
            with open(path, "wb") as f:
                while True:
                    chunk = response.read(1 << 16)
                    if not chunk:
                        break
                    digest.update(chunk)
                    f.write(chunk)
                    done += len(chunk)
                    if progress:
                        progress(done, total)
        finally:
            response.close()
    except (URLError, OSError, ValueError) as e:
        raise UpdateError("the download from %s failed (%s)" % (_host(url), getattr(e, "reason", e)))
    if size and done != int(size):
        raise UpdateError("the download is %d bytes, not the release's %d: refused" % (done, int(size)))
    if digest.hexdigest().lower() != str(sha256).lower():
        raise UpdateError("the download's checksum does not match the release's: refused (nothing was installed)")


def _unpack(zip_path, dest, version, maya=0):
    """The release zip's GroomLab-<version>/ folder unpacked into dest; returns its path. It must hold
    the GroomLab.mod and a plugin build: the running Maya's (`maya`; 0: unknown, any build). Maya
    2022's is the one at the module's root (Windows' zips); the others' are in maya<year>/ (every
    build on macOS, whose zips have no Maya 2022 nor a root plugin)."""
    top = "GroomLab-%s" % version
    try:
        with zipfile.ZipFile(zip_path) as z:
            for name in z.namelist():
                parts = name.replace("\\", "/").split("/")
                if parts[0] != top or ".." in parts or ":" in name or name.startswith(("/", "\\")):
                    raise UpdateError("the release zip holds an unexpected file (%s): refused" % name)
            z.extractall(dest)
    except zipfile.BadZipfile:
        raise UpdateError("the download is not a zip: refused")
    folder = os.path.join(dest, top)
    module = os.path.join(folder, "GroomLab")

    def build(year):  # (a plugin may be a file or, as a .bundle can be, a folder)
        return os.path.exists(os.path.join(module, *(("maya%s" % year,) if str(year) != "2022" else ()) + ("plug-ins", PLUGIN_FILE)))

    years = [d[4:] for d in (os.listdir(module) if os.path.isdir(module) else []) if re.match(r"maya\d{4}$", d)]
    if not (os.path.isfile(os.path.join(folder, "GroomLab.mod")) and (build(2022) or any(build(y) for y in years))):
        raise UpdateError("the release zip has no GroomLab module: refused")
    if maya and not build(maya):
        raise UpdateError("the release zip has no GroomLab for Maya %s: refused" % maya)
    if installed_version(module) != version:
        raise UpdateError("the release zip's VERSION.txt is not %s: refused" % version)
    return folder


def _free_folder(parent, name):
    path = os.path.join(parent, name)
    n = 2
    while os.path.exists(path):
        path = os.path.join(parent, "%s-%d" % (name, n))
        n += 1
    return path


def mod_text(bundled_mod, module):
    """The release's bundled .mod with its GroomLab entries pointing at `module` (absolute)."""
    with open(bundled_mod) as f:
        lines = f.read().splitlines()
    out = []
    for line in lines:
        match = _MOD_LINE.match(line)
        out.append(match.group(1) + _norm(module) if match else line)
    return "\n".join(out) + "\n"


def install(info, progress=None, module=None):
    """Installs check()'s release beside this install (see the module's notes). progress(done, total)
    is called with the bytes downloaded, from the calling thread. Returns {"ok", "message",
    "folder", "previous", "mod"}; never touches the running plugin's files."""
    module = _norm(module or module_dir())
    why = off_reason(module)
    if why:
        return {"ok": False, "message": why}
    try:
        return _install(info or {}, progress, module)
    except UpdateError as e:
        return {"ok": False, "message": str(e)}
    except (IOError, OSError) as e:
        return {"ok": False, "message": "the update failed: %s" % e}


def _install(info, progress, module):
    version = str(info.get("version", ""))
    current = installed_version(module)
    if parse_version(version) is None:
        raise UpdateError("no release to install")
    if compare_versions(version, current) <= 0:
        raise UpdateError("GroomLab %s is installed already (the release is %s)" % (current, version))
    if waiting_for_restart(version, module):
        raise UpdateError("GroomLab %s is installed already. %s." % (version, RESTART_MESSAGE))
    wanted, running = release_mayas(info), maya_major()
    if wanted and running and str(running) not in wanted:
        raise UpdateError("GroomLab %s is for Maya %s; this is Maya %s" % (version, ", ".join(wanted), running))
    if not re.match(r"^[0-9a-fA-F]{64}$", str(info.get("sha256", ""))):
        raise UpdateError("the release has no checksum: refused")
    _check_download_url(info.get("url"), LATEST_URL)
    mod = find_mod_file(module)
    if not mod:
        raise UpdateError("no GroomLab.mod in Maya's modules folders points at this install (%s): download the new "
                          "version from getgroomlab.com and install it with its install.mel" % module)
    # A packaged install's module sits in its GroomLab-<version> folder: the new one goes beside that
    # folder (and that folder goes once the new version has loaded). A module elsewhere stays.
    package = os.path.dirname(module)
    if os.path.basename(package).startswith("GroomLab-"):
        parent, previous = os.path.dirname(package), package
    else:
        parent, previous = package, ""
    try:
        staging = tempfile.mkdtemp(prefix=".GroomLab-update-", dir=parent)
    except OSError as e:
        raise UpdateError("can't write beside this install (%s): %s" % (parent, e))
    target = ""
    try:
        zip_path = os.path.join(staging, "release.zip")
        _download(info["url"], zip_path, info["sha256"], info.get("size"), progress)
        folder = _unpack(zip_path, os.path.join(staging, "unpacked"), version, running)
        target = _free_folder(parent, "GroomLab-%s" % version)
        os.rename(folder, target)
        new_module = os.path.join(target, "GroomLab")
        text = mod_text(os.path.join(target, "GroomLab.mod"), new_module)
        shutil.copy2(mod, mod + ".bak")
        _atomic_write(mod, text)
    except BaseException:
        if target and os.path.isdir(target):
            shutil.rmtree(target, ignore_errors=True)
        raise
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    target = _norm(target)
    _update_state(pending={"version": version, "installed": target, "previous": _norm(previous) if previous else "",
                           "mod": mod, "date": time.strftime("%Y-%m-%dT%H:%M:%S")})
    return {"ok": True, "folder": target, "previous": previous, "mod": mod,
            "message": "GroomLab %s is installed in %s. %s." % (version, target, RESTART_MESSAGE)}


def waiting_for_restart(version=None, module=None):
    """An update installed (to `version`, any when None) that waits for Maya's restart: its folder,
    else ""."""
    pending = read_state().get("pending")
    if not isinstance(pending, dict) or (version and pending.get("version") != version):
        return ""
    installed = pending.get("installed") or ""
    package = os.path.dirname(_norm(module or module_dir()))
    if installed and os.path.isdir(installed) and not _same(installed, package):
        return installed
    return ""


def finish_pending(module=None):
    """A start after an update: once the new version runs, the previous version's folder goes (best
    effort: a file still held stays, and is tried again next start). Returns the folder removed."""
    state = read_state()
    pending = state.get("pending")
    if not isinstance(pending, dict):
        return ""
    package = os.path.dirname(_norm(module or module_dir()))
    installed = pending.get("installed") or ""
    if not installed or not _same(package, installed):
        return ""  # not restarted yet (or gone back to the previous version): keep everything
    previous = pending.get("previous") or ""
    if (previous and os.path.isdir(previous) and not _same(previous, package)
            and os.path.basename(previous).startswith("GroomLab-")
            and os.path.isfile(os.path.join(previous, "GroomLab", "VERSION.txt"))):
        shutil.rmtree(previous, ignore_errors=True)
        if os.path.isdir(previous):
            return ""  # something held a file: next start
    _update_state(pending=None)
    return previous


# ---- the startup check and who hears about a newer version -----------------------------------------

_available = None  # check()'s result when a newer, installable release was found this session
_listeners = []
_started = False


def available():
    """The newer release found this session (check()'s result), or None."""
    return _available


def on_available(callback):
    """callback(result or None) when a newer release is found (or installed: None)."""
    if callback not in _listeners:
        _listeners.append(callback)


def remove_listener(callback):
    if callback in _listeners:
        _listeners.remove(callback)


def _announce(result):
    global _available
    _available = result if result and result.get("installable") else None
    for callback in list(_listeners):
        try:
            callback(_available)
        except Exception:  # a closed window's
            remove_listener(callback)


def note_text(result=None):
    """The Groom Tools' one-line note about a newer release ("" without one)."""
    result = result or _available
    if not result:
        return ""
    return "GroomLab %s is available — GroomLab > Help > Check for Updates" % result["latest"]


def check_in_background(url=None, module=None, done=None):
    """check() on a thread; its result goes to done(result) (default: the listeners, when newer) on
    Maya's main thread (in batch, which has no UI and no idle loop to wait for: on the thread).
    Returns the thread."""
    try:
        from maya import cmds
        import maya.utils
        deferred = not cmds.about(batch=True)
    except ImportError:
        deferred = False

    def work():
        result = check(url, module)

        def apply():
            if done:
                done(result)
            elif result.get("installable"):
                _announce(result)
        if deferred:
            maya.utils.executeDeferred(apply)
        else:
            apply()

    thread = threading.Thread(target=work, name="GroomLabUpdateCheck")
    thread.daemon = True
    thread.start()
    return thread


def on_startup(now=None):
    """The plugin's menu install (once a session, never in batch): an updated install drops its
    previous folder; at most once a day, unless turned off, a background check that leaves a quiet
    note in the Groom Tools when a newer release is out. Never a dialog, never fails the plugin's
    load, silent when offline. Returns the thread of the check it started, else None."""
    global _started
    if _started:
        return None
    _started = True
    try:
        from maya import cmds
        if cmds.about(batch=True):
            return None
        return startup(now)
    except Exception:
        return None


def startup(now=None, module=None):
    """on_startup's work, every time it is called (tests call it in batch): see on_startup."""
    if off_reason(module):
        return None
    try:
        removed = finish_pending(module)
        if removed:
            print("GroomLab: updated to %s; removed the previous version's folder %s" % (current_version(module), removed))
    except Exception:
        pass
    if not startup_check_enabled() or not check_due(now):
        return None
    mark_checked(now)
    return check_in_background(module=module)
